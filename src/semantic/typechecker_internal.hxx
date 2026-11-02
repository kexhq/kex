#pragma once

// File-local helpers shared by the TypeChecker's implementation translation
// units. These were anonymous-namespace functions in typechecker.cxx; they
// live here (in kex::semantic::detail) so a split of the checker's methods
// across several .cxx files does not copy them.

#include "../ast/ast.hxx"
#include "imported_interfaces.hxx"
#include "types.hxx"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kex::semantic::detail {

// The name in `candidates` a misspelt `name` most likely meant: one differing
// only in case (a renamed `whereis` is now `whereIs`) first, then the closest
// within two edits. Empty when nothing is close.
inline auto closestName(const std::string& name,
                        const std::vector<std::string>& candidates)
    -> std::string {
    auto lower = [](std::string text) {
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    for (const auto& candidate : candidates)
        if (lower(candidate) == lower(name)) return candidate;
    auto distance = [](const std::string& a, const std::string& b) {
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
        for (size_t i = 1; i <= a.size(); ++i) {
            size_t diagonal = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); ++j) {
                const size_t above = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1,
                                   diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
                diagonal = above;
            }
        }
        return row[b.size()];
    };
    std::string best;
    size_t bestDistance = 3;
    for (const auto& candidate : candidates)
        if (const auto d = distance(name, candidate); d < bestDistance) {
            bestDistance = d;
            best = candidate;
        }
    return best;
}

// Records `A.B.Name` — a type, trait or constant declared in `module A.B` —
// as a module reference for discovery, together with `A.B` itself. Nothing
// spells `a/b/name.kex`, and the resolver's container fallback reaches only
// the first segment (`a.kex`), so without the owner a file that names the
// declaration only this way never gets `a/b.kex` loaded. Whichever of the
// two is not a module, the resolver drops.
inline auto recordWithOwner(std::unordered_set<std::string>& references,
                            const std::string& qualified) -> void {
    const auto dot = qualified.rfind('.');
    if (dot != std::string::npos && qualified.find('.') < dot)
        references.insert(qualified.substr(0, dot));
    references.insert(qualified);
}

// A sum-type variant is constructor-shaped (TypeName or GenericType with a
// single-part name) if it's a bare `Name` or `Name(Args...)` — anything
// else (tuple/list/function/union/atom type exprs) means the TypeDef is a
// type alias, not a real ADT, and the whole def is skipped for exhaustiveness.
inline auto extractConstructorName(const ast::TypeExprPtr& variant)
    -> std::optional<std::string> {
    if (!variant) return std::nullopt;
    if (auto* tn = std::get_if<ast::TypeName>(&variant->kind)) {
        if (tn->parts.size() == 1) return tn->parts[0];
    }
    if (auto* gt = std::get_if<ast::GenericType>(&variant->kind)) {
        if (gt->name.parts.size() == 1) return gt->name.parts[0];
    }
    return std::nullopt;
}

// Does a type contain an Unknown anywhere? On a signature PARAMETER this
// marks a contract the checker never learned — the shape-only entries the
// source-derived interface path records for un-annotated `make` methods,
// where compiled artifacts would supply real types. Type variables are not
// this: `[A]` is a known, genuinely generic contract.
inline auto mentionsUnknownType(const TypePtr& type) -> bool {
    if (!type) return false;
    if (std::holds_alternative<UnknownType>(type->kind)) return true;
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        for (const auto& arg : named->typeArgs)
            if (mentionsUnknownType(arg)) return true;
        return false;
    }
    if (auto* list = std::get_if<ListType>(&type->kind))
        return mentionsUnknownType(list->element);
    if (auto* optional = std::get_if<OptionalType>(&type->kind))
        return mentionsUnknownType(optional->inner);
    if (auto* map = std::get_if<MapType>(&type->kind))
        return mentionsUnknownType(map->key) || mentionsUnknownType(map->value);
    if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
        for (const auto& element : tuple->elements)
            if (mentionsUnknownType(element)) return true;
    }
    if (auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
        for (const auto& member : intersection->members)
            if (mentionsUnknownType(member)) return true;
    }
    if (auto* record = std::get_if<RecordType>(&type->kind)) {
        for (const auto& [_, fieldType] : record->fields)
            if (mentionsUnknownType(fieldType)) return true;
    }
    return false;
}

inline auto headShapeOf(const TypePtr& type) -> TypePtr {
    if (!type) return type;
    if (auto* named = std::get_if<NamedType>(&type->kind))
        return Type::named(named->name);
    return type;
}

inline auto substituteInterfaceGenerics(const TypePtr& type,
                                        const std::vector<TypePtr>& args)
    -> TypePtr {
    if (!type) return type;
    if (auto* var = std::get_if<TypeVar>(&type->kind)) {
        if (var->id < 0) {
            auto index = static_cast<size_t>(-var->id - 1);
            if (index < args.size()) return args[index];
        }
        return type;
    }
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        std::vector<TypePtr> nested;
        for (const auto& arg : named->typeArgs)
            nested.push_back(substituteInterfaceGenerics(arg, args));
        return Type::named(named->name, std::move(nested));
    }
    if (auto* list = std::get_if<ListType>(&type->kind))
        return Type::list(substituteInterfaceGenerics(list->element, args));
    if (auto* optional = std::get_if<OptionalType>(&type->kind))
        return Type::optional(
            substituteInterfaceGenerics(optional->inner, args));
    if (auto* map = std::get_if<MapType>(&type->kind))
        return Type::map(substituteInterfaceGenerics(map->key, args),
                         substituteInterfaceGenerics(map->value, args));
    if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
        std::vector<TypePtr> elements;
        for (const auto& element : tuple->elements)
            elements.push_back(substituteInterfaceGenerics(element, args));
        return Type::tuple(std::move(elements));
    }
    if (auto* fn = std::get_if<FuncType>(&type->kind)) {
        std::vector<TypePtr> params;
        for (const auto& param : fn->params)
            params.push_back(substituteInterfaceGenerics(param, args));
        return Type::func(std::move(params),
                          substituteInterfaceGenerics(fn->result, args));
    }
    if (auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
        std::vector<TypePtr> members;
        for (const auto& member : intersection->members)
            members.push_back(substituteInterfaceGenerics(member, args));
        return Type::intersection(std::move(members));
    }
    if (auto* record = std::get_if<RecordType>(&type->kind)) {
        std::vector<std::pair<std::string, TypePtr>> fields;
        for (const auto& [name, fieldType] : record->fields)
            fields.emplace_back(
                name, substituteInterfaceGenerics(fieldType, args));
        return Type::record(std::move(fields));
    }
    return type;
}

// How many type parameters a record has, read back off its registered field
// types. A record's parameters occupy the negative TypeVar slots -1, -2, …
// (the convention `substituteInterfaceGenerics` consumes), so the highest
// slot any field mentions is the arity. Deriving it this way keeps LOCAL and
// IMPORTED records on one code path: the KexI reader assigns the same
// negative ids when it rebuilds an imported record's fields, and neither
// KexiRecord nor ImportedInterfaces carries a parameter LIST to consult.
inline auto recordTypeParamCount(
    const std::unordered_map<std::string, TypePtr>& fields) -> size_t {
    size_t count = 0;
    std::function<void(const TypePtr&)> scan = [&](const TypePtr& type) {
        if (!type) return;
        if (auto* var = std::get_if<TypeVar>(&type->kind)) {
            if (var->id < 0)
                count = std::max(count, static_cast<size_t>(-var->id));
            return;
        }
        if (auto* named = std::get_if<NamedType>(&type->kind)) {
            for (const auto& arg : named->typeArgs) scan(arg);
            return;
        }
        if (auto* list = std::get_if<ListType>(&type->kind)) return scan(list->element);
        if (auto* optional = std::get_if<OptionalType>(&type->kind))
            return scan(optional->inner);
        if (auto* map = std::get_if<MapType>(&type->kind)) {
            scan(map->key);
            return scan(map->value);
        }
        if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
            for (const auto& element : tuple->elements) scan(element);
            return;
        }
        if (auto* fn = std::get_if<FuncType>(&type->kind)) {
            for (const auto& param : fn->params) scan(param);
            return scan(fn->result);
        }
        if (auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
            for (const auto& member : intersection->members) scan(member);
            return;
        }
        if (auto* record = std::get_if<RecordType>(&type->kind))
            for (const auto& [_, fieldType] : record->fields) scan(fieldType);
    };
    for (const auto& [_, fieldType] : fields) scan(fieldType);
    return count;
}

// Match a record field's DECLARED type against the type of the value supplied
// for it, recording what each parameter slot was filled with. First binding
// wins, so two fields disagreeing about `A` leave the first one's answer
// rather than silently taking the last.
inline auto bindRecordTypeParams(const TypePtr& declared, const TypePtr& actual,
                                 std::unordered_map<int, TypePtr>& slots)
    -> void {
    if (!declared || !actual) return;
    if (auto* var = std::get_if<TypeVar>(&declared->kind)) {
        if (var->id >= 0) return;
        // An open actual says nothing about the slot — `Box { items: [] }`
        // must stay `Box<Any>` rather than pinning A to the empty list's
        // placeholder variable.
        if (std::holds_alternative<UnknownType>(actual->kind) ||
            std::holds_alternative<TypeVar>(actual->kind))
            return;
        slots.try_emplace(var->id, actual);
        return;
    }
    if (auto* named = std::get_if<NamedType>(&declared->kind)) {
        auto* other = std::get_if<NamedType>(&actual->kind);
        if (!other || other->typeArgs.size() != named->typeArgs.size()) return;
        for (size_t i = 0; i < named->typeArgs.size(); ++i)
            bindRecordTypeParams(named->typeArgs[i], other->typeArgs[i], slots);
        return;
    }
    if (auto* list = std::get_if<ListType>(&declared->kind)) {
        if (auto* other = std::get_if<ListType>(&actual->kind))
            bindRecordTypeParams(list->element, other->element, slots);
        return;
    }
    if (auto* optional = std::get_if<OptionalType>(&declared->kind)) {
        if (auto* other = std::get_if<OptionalType>(&actual->kind))
            bindRecordTypeParams(optional->inner, other->inner, slots);
        return;
    }
    if (auto* map = std::get_if<MapType>(&declared->kind)) {
        if (auto* other = std::get_if<MapType>(&actual->kind)) {
            bindRecordTypeParams(map->key, other->key, slots);
            bindRecordTypeParams(map->value, other->value, slots);
        }
        return;
    }
    if (auto* tuple = std::get_if<TupleType>(&declared->kind)) {
        auto* other = std::get_if<TupleType>(&actual->kind);
        if (!other || other->elements.size() != tuple->elements.size()) return;
        for (size_t i = 0; i < tuple->elements.size(); ++i)
            bindRecordTypeParams(tuple->elements[i], other->elements[i], slots);
        return;
    }
    if (auto* fn = std::get_if<FuncType>(&declared->kind)) {
        auto* other = std::get_if<FuncType>(&actual->kind);
        if (!other || other->params.size() != fn->params.size()) return;
        for (size_t i = 0; i < fn->params.size(); ++i)
            bindRecordTypeParams(fn->params[i], other->params[i], slots);
        bindRecordTypeParams(fn->result, other->result, slots);
        return;
    }
}

// Whether a type has any component the checker has not pinned down — a type
// variable, an unknown, or a trait constraint. Checks that would otherwise
// report a mismatch use this to stay quiet rather than invent an error from
// incomplete information.
inline auto containsOpenType(const TypePtr& type) -> bool {
    if (!type) return true;
    return std::visit(
        [](const auto& node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeVar> ||
                          std::is_same_v<T, UnknownType> ||
                          std::is_same_v<T, ConstrainedType>) {
                return true;
            } else if constexpr (std::is_same_v<T, NamedType>) {
                for (const auto& arg : node.typeArgs)
                    if (containsOpenType(arg)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, FuncType>) {
                for (const auto& param : node.params)
                    if (containsOpenType(param)) return true;
                return containsOpenType(node.result);
            } else if constexpr (std::is_same_v<T, TupleType>) {
                for (const auto& element : node.elements)
                    if (containsOpenType(element)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, ListType>) {
                return containsOpenType(node.element);
            } else if constexpr (std::is_same_v<T, MapType>) {
                return containsOpenType(node.key) || containsOpenType(node.value);
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                return containsOpenType(node.inner);
            } else if constexpr (std::is_same_v<T, UnionType>) {
                for (const auto& member : node.members)
                    if (containsOpenType(member)) return true;
                return false;
            } else {
                return false;
            }
        },
        type->kind);
}

// The primitive numbers, as a constraint: `N: Number` is what `n - 1` leaves
// on an unannotated `n`, and the checker decides it by the type's shape
// (TraitRegistry::satisfiesStructurally), not from any declaration. A value
// known only by one of these is a primitive number, so it has the built-in
// operators. Every other trait gives a value exactly the operators the trait
// itself declares, and nothing more by being named here.
inline auto primitiveNumberTrait(const std::string& trait) -> bool {
    return trait == "Number" || trait == "Integer" || trait == "Float";
}

// Whether `known` already offers `function`'s parameters: the prelude serves
// a module's functions as receiver functions too (`Parsing.Input.peekAt` from
// `kex_prelude`), and the same function from both was "ambiguous".
inline auto offeredAlready(const std::vector<const ImportedFunction*>& known,
                           const ImportedFunction& function) -> bool {
    return std::any_of(known.begin(), known.end(), [&](const ImportedFunction* other) {
        const auto& left = other->signature.params;
        const auto& right = function.signature.params;
        if (left.size() != right.size()) return false;
        for (size_t i = 0; i < left.size(); ++i)
            if (typeToString(left[i]) != typeToString(right[i])) return false;
        return true;
    });
}

// A trailing block that reaches its parameter only by skipping defaulted ones
// (`f(attributes = {}, children)` called as `f do … end`) is passed by that
// parameter's label instead. Both backends already fill the skipped defaults
// for a labelled argument; positionally the block would land in `attributes`.
// A call that already labels arguments keeps the block where it is, since
// the hint was matched against those as if they were positional.
//
// The checker walks the AST as const, but annotating the tree is part of its
// job here exactly as setting `Lambda::collection` is, so the cast is safe:
// every node is owned by a Program the caller holds mutably.
template <typename Call>
auto passBlockByLabel(const Call& node, const std::string& label) -> void {
    auto& call = const_cast<Call&>(node);
    if (label.empty() || !call.block || !call.namedArgs.empty()) return;
    call.namedArgs.emplace_back(label, std::move(*call.block));
    call.block.reset();
}

// Binds the type variables of a signature parameter to the matching parts of
// the argument's type, wherever they sit: bare, a list's element, or inside
// `A?`, `Result<A, E>`, a map or a tuple. Only a bare `A` and `[A]` used to
// bind, so a block over an Optional or a Result kept an open hint, and
// `found.map { |x, y| … }` over a record was accepted as if it destructured a
// tuple: BEAM handed back a partial application where the walker ran it.
template <typename Resolve>
auto bindHintVars(const TypePtr& sigP, const TypePtr& argRaw,
                  std::unordered_map<int, TypePtr>& sub,
                  const Resolve& resolve) -> void {
    const auto argT = resolve(argRaw);
    if (auto* tv = std::get_if<TypeVar>(&sigP->kind)) {
        sub.emplace(tv->id, argT);
    } else if (auto* list = std::get_if<ListType>(&sigP->kind)) {
        if (auto* argList = std::get_if<ListType>(&argT->kind))
            bindHintVars(list->element, argList->element, sub, resolve);
    } else if (auto* opt = std::get_if<OptionalType>(&sigP->kind)) {
        if (auto* argOpt = std::get_if<OptionalType>(&argT->kind))
            bindHintVars(opt->inner, argOpt->inner, sub, resolve);
    } else if (auto* map = std::get_if<MapType>(&sigP->kind)) {
        if (auto* argMap = std::get_if<MapType>(&argT->kind)) {
            bindHintVars(map->key, argMap->key, sub, resolve);
            bindHintVars(map->value, argMap->value, sub, resolve);
        }
    } else if (auto* tuple = std::get_if<TupleType>(&sigP->kind)) {
        auto* argTuple = std::get_if<TupleType>(&argT->kind);
        if (argTuple && argTuple->elements.size() == tuple->elements.size())
            for (size_t i = 0; i < tuple->elements.size(); i++)
                bindHintVars(tuple->elements[i], argTuple->elements[i], sub,
                             resolve);
    } else if (auto* named = std::get_if<NamedType>(&sigP->kind)) {
        auto* argNamed = std::get_if<NamedType>(&argT->kind);
        if (argNamed && argNamed->name == named->name &&
            argNamed->typeArgs.size() == named->typeArgs.size())
            for (size_t i = 0; i < named->typeArgs.size(); i++)
                bindHintVars(named->typeArgs[i], argNamed->typeArgs[i], sub,
                             resolve);
    }
}

} // namespace kex::semantic::detail
