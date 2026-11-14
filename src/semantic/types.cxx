#include "types.hxx"
#include "../common/well_known.hxx"
#include <algorithm>
#include <functional>

namespace kex::semantic {

auto Type::integer() -> TypePtr {
    return std::make_shared<Type>(Type{PrimitiveType{PrimitiveType::Integer}});
}

auto Type::charT() -> TypePtr {
    return std::make_shared<Type>(Type{PrimitiveType{PrimitiveType::Char}});
}

auto Type::string() -> TypePtr {
    return std::make_shared<Type>(Type{PrimitiveType{PrimitiveType::String}});
}

auto Type::boolean() -> TypePtr {
    return std::make_shared<Type>(Type{PrimitiveType{PrimitiveType::Bool}});
}

auto Type::atom(std::string atomName) -> TypePtr {
    return std::make_shared<Type>(
        Type{PrimitiveType{PrimitiveType::Atom, std::move(atomName)}});
}

auto Type::unit() -> TypePtr {
    return std::make_shared<Type>(Type{PrimitiveType{PrimitiveType::Unit}});
}

auto Type::unknown() -> TypePtr {
    return std::make_shared<Type>(Type{UnknownType{}});
}

auto Type::voidType(std::string reason) -> TypePtr {
    return std::make_shared<Type>(Type{VoidType{std::move(reason)}});
}

auto Type::named(const std::string& name, std::vector<TypePtr> args) -> TypePtr {
    return std::make_shared<Type>(Type{NamedType{name, std::move(args)}});
}

auto Type::variantOf(const std::string& adt, const std::string& variant) -> TypePtr {
    return std::make_shared<Type>(Type{NamedType{adt, {}, variant}});
}

auto Type::func(std::vector<TypePtr> params, TypePtr result, bool block) -> TypePtr {
    return std::make_shared<Type>(
        Type{FuncType{std::move(params), std::move(result), block}});
}

auto Type::list(TypePtr element) -> TypePtr {
    return std::make_shared<Type>(Type{ListType{std::move(element)}});
}

auto Type::tuple(std::vector<TypePtr> elements) -> TypePtr {
    return std::make_shared<Type>(Type{TupleType{std::move(elements)}});
}

auto Type::map(TypePtr key, TypePtr value) -> TypePtr {
    return std::make_shared<Type>(Type{MapType{std::move(key), std::move(value)}});
}

auto Type::optional(TypePtr inner) -> TypePtr {
    return std::make_shared<Type>(Type{OptionalType{std::move(inner)}});
}

auto Type::intersection(std::vector<TypePtr> members) -> TypePtr {
    std::vector<TypePtr> flattened;
    for (auto& member : members) {
        if (auto* nested = std::get_if<IntersectionType>(&member->kind)) {
            flattened.insert(flattened.end(), nested->members.begin(),
                             nested->members.end());
        } else {
            flattened.push_back(std::move(member));
        }
    }
    // Structural rows form a meet by combining their required fields. When a
    // label occurs on both sides, a value must satisfy both field types too.
    // Keep the first spelling order for stable diagnostics.
    std::vector<std::pair<std::string, TypePtr>> rowFields;
    std::vector<TypePtr> nonRows;
    for (auto& member : flattened) {
        if (auto* row = std::get_if<RecordType>(&member->kind)) {
            for (const auto& [name, fieldType] : row->fields) {
                auto existing = std::find_if(
                    rowFields.begin(), rowFields.end(),
                    [&](const auto& field) { return field.first == name; });
                if (existing == rowFields.end()) {
                    rowFields.emplace_back(name, fieldType);
                } else if (!typesEqual(existing->second, fieldType)) {
                    existing->second = Type::intersection(
                        {existing->second, fieldType});
                }
            }
        } else {
            nonRows.push_back(std::move(member));
        }
    }
    if (!rowFields.empty())
        nonRows.push_back(Type::record(std::move(rowFields)));
    flattened = std::move(nonRows);
    std::vector<TypePtr> normalized;
    for (auto& member : flattened) {
        if (std::holds_alternative<VoidType>(member->kind))
            return Type::voidType();
        if (std::holds_alternative<UnknownType>(member->kind))
            continue; // Unknown is the gradual top/escape hatch.
        for (const auto& existing : normalized) {
            const auto* left = std::get_if<PrimitiveType>(&existing->kind);
            const auto* right = std::get_if<PrimitiveType>(&member->kind);
            if (left && right && left->kind != right->kind)
                return Type::voidType();
            if (left && right && left->kind == PrimitiveType::Atom &&
                !left->atomName.empty() && !right->atomName.empty() &&
                left->atomName != right->atomName)
                return Type::voidType();
        }
        if (std::none_of(normalized.begin(), normalized.end(),
                         [&](const TypePtr& existing) {
                             return typesEqual(existing, member);
                         }))
            normalized.push_back(std::move(member));
    }
    if (normalized.empty()) return Type::unknown();
    if (normalized.size() == 1) return normalized.front();
    return std::make_shared<Type>(
        Type{IntersectionType{std::move(normalized)}});
}

auto Type::record(std::vector<std::pair<std::string, TypePtr>> fields)
    -> TypePtr {
    for (const auto& [_, fieldType] : fields)
        if (fieldType && std::holds_alternative<VoidType>(fieldType->kind))
            return Type::voidType();
    return std::make_shared<Type>(Type{RecordType{std::move(fields)}});
}

auto Type::typeVar(int id) -> TypePtr {
    return std::make_shared<Type>(Type{TypeVar{id}});
}

auto Type::byte() -> TypePtr {
    return std::make_shared<Type>(Type{SizedIntType{8, false}});
}

auto Type::int8() -> TypePtr {
    return std::make_shared<Type>(Type{SizedIntType{8, true}});
}

auto Type::int16() -> TypePtr {
    return std::make_shared<Type>(Type{SizedIntType{16, true}});
}

auto Type::int32() -> TypePtr {
    return std::make_shared<Type>(Type{SizedIntType{32, true}});
}

auto Type::int64() -> TypePtr {
    return std::make_shared<Type>(Type{SizedIntType{64, true}});
}

auto Type::uint8() -> TypePtr {
    return std::make_shared<Type>(Type{SizedIntType{8, false}});
}

auto Type::uint16() -> TypePtr {
    return std::make_shared<Type>(Type{SizedIntType{16, false}});
}

auto Type::uint32() -> TypePtr {
    return std::make_shared<Type>(Type{SizedIntType{32, false}});
}

auto Type::uint64() -> TypePtr {
    return std::make_shared<Type>(Type{SizedIntType{64, false}});
}

auto Type::float32() -> TypePtr {
    return std::make_shared<Type>(Type{SizedFloatType{32}});
}

auto Type::float64() -> TypePtr {
    return std::make_shared<Type>(Type{SizedFloatType{64}});
}

auto Type::constrained(const std::string& varName, const std::string& traitName,
                       int genericId) -> TypePtr {
    return std::make_shared<Type>(
        Type{ConstrainedType{varName, traitName, genericId}});
}

namespace {

// The built-in names and the factory of the type each one denotes.
struct BuiltinTypeEntry {
    std::string_view name;
    TypePtr (*make)();
};

auto atomType() -> TypePtr { return Type::atom(); }

constexpr BuiltinTypeEntry kBuiltinTypes[] = {
    {"Integer", &Type::integer}, {"Int", &Type::integer},
    {"Char", &Type::charT},      {"String", &Type::string},
    {"Bool", &Type::boolean},    {"Atom", &atomType},
    {"Byte", &Type::byte},       {"Int8", &Type::int8},
    {"Int16", &Type::int16},     {"Int32", &Type::int32},
    {"Int64", &Type::int64},     {"UInt8", &Type::uint8},
    {"UInt16", &Type::uint16},   {"UInt32", &Type::uint32},
    {"UInt64", &Type::uint64},   {"Float32", &Type::float32},
    {"Float64", &Type::float64},
};

} // namespace

auto builtinType(std::string_view name) -> TypePtr {
    for (const auto& entry : kBuiltinTypes)
        if (entry.name == name) return entry.make();
    return nullptr;
}

auto builtinTypeNames() -> std::vector<std::string_view> {
    std::vector<std::string_view> names;
    for (const auto& entry : kBuiltinTypes) names.push_back(entry.name);
    return names;
}

auto signatureBuiltinType(std::string_view name) -> TypePtr {
    if (auto type = builtinType(name)) return type;
    if (name == "Number" || name == "Float")
        return Type::constrained(std::string(name), std::string(name));
    if (name == "Void") return Type::unit();
    return nullptr;
}

auto isPrimitiveTypeName(const std::string& name) -> bool {
    // Kept beside typeToString deliberately: these are exactly the names it
    // prints for types that no source file declares. A structural type such
    // as List or Map only appears here if the stdlib does not declare it —
    // `type List<X> = [X]` in list.kex means `List` resolves as a declared
    // type name and needs no entry.
    return signatureBuiltinType(name) != nullptr || name == "Any" ||
           name == "Tuple" || name == "Block";
}

auto typeToString(const TypePtr& type) -> std::string {
    if (!type) return "?";

    return std::visit([](const auto& t) -> std::string {
        using T = std::decay_t<decltype(t)>;

        if constexpr (std::is_same_v<T, PrimitiveType>) {
            switch (t.kind) {
                case PrimitiveType::Integer: return "Integer";
                case PrimitiveType::Char: return "Char";
                case PrimitiveType::String: return "String";
                case PrimitiveType::Bool: return "Bool";
                case PrimitiveType::Atom:
                    return t.atomName.empty() ? "Atom" : ":" + t.atomName;
                case PrimitiveType::Unit: return "Void";
            }
            return "?";
        }
        else if constexpr (std::is_same_v<T, SizedIntType>) {
            if (!t.isSigned) {
                switch (t.bits) {
                    case 8: return "Byte";
                    case 16: return "UInt16";
                    case 32: return "UInt32";
                    case 64: return "UInt64";
                }
            } else {
                switch (t.bits) {
                    case 8: return "Int8";
                    case 16: return "Int16";
                    case 32: return "Int32";
                    case 64: return "Int";  // Int is the canonical name for 64-bit signed
                }
            }
            return "?";
        }
        else if constexpr (std::is_same_v<T, SizedFloatType>) {
            switch (t.bits) {
                case 32: return "Float32";
                case 64: return "Float64";
            }
            return "?";
        }
        else if constexpr (std::is_same_v<T, NamedType>) {
            std::string result = t.name;
            if (!t.variant.empty()) result += "." + t.variant;
            if (!t.typeArgs.empty()) {
                result += "<";
                for (size_t i = 0; i < t.typeArgs.size(); i++) {
                    if (i > 0) result += ", ";
                    result += typeToString(t.typeArgs[i]);
                }
                result += ">";
            }
            return result;
        }
        else if constexpr (std::is_same_v<T, FuncType>) {
            if (t.block) {
                std::string result = "Block<";
                for (const auto& param : t.params)
                    result += typeToString(param) + ", ";
                return result + typeToString(t.result) + ">";
            }
            // Kex function types are curried, so they print as
            // `A -> B -> R`, matching how they are written in source. A
            // parameter that is itself a function has to be parenthesised or
            // the arrows associate wrongly: `(A -> Bool) -> [A]` says
            // something quite different from `A -> Bool -> [A]`. The RESULT
            // needs the same treatment: a 1-parameter function returning
            // ANOTHER function (`A -> (B -> C)`, e.g. a def whose own
            // `-> (B -> C)` return annotation is itself an arrow type) must
            // not print identically to a flat 2-parameter function
            // (`A -> B -> C`) — those are different arities at the call
            // site, and printing them the same way made mismatch diagnostics
            // report the wrong arity entirely (#350).
            auto resultText = typeToString(t.result);
            if (t.result && std::holds_alternative<FuncType>(t.result->kind))
                resultText = "(" + resultText + ")";
            if (t.params.empty()) return "() -> " + resultText;
            std::string result;
            for (const auto& param : t.params) {
                auto text = typeToString(param);
                if (param && std::holds_alternative<FuncType>(param->kind))
                    text = "(" + text + ")";
                result += text + " -> ";
            }
            return result + resultText;
        }
        else if constexpr (std::is_same_v<T, TupleType>) {
            std::string result = "(";
            for (size_t i = 0; i < t.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += typeToString(t.elements[i]);
            }
            result += ")";
            return result;
        }
        else if constexpr (std::is_same_v<T, ListType>) {
            return "[" + typeToString(t.element) + "]";
        }
        else if constexpr (std::is_same_v<T, MapType>) {
            return "{" + typeToString(t.key) + ": " + typeToString(t.value) + "}";
        }
        else if constexpr (std::is_same_v<T, OptionalType>) {
            return typeToString(t.inner) + "?";
        }
        else if constexpr (std::is_same_v<T, UnionType>) {
            std::string result;
            for (size_t i = 0; i < t.members.size(); i++) {
                if (i > 0) result += " | ";
                result += typeToString(t.members[i]);
            }
            return result;
        }
        else if constexpr (std::is_same_v<T, IntersectionType>) {
            std::string result;
            for (size_t i = 0; i < t.members.size(); i++) {
                if (i > 0) result += " & ";
                result += typeToString(t.members[i]);
            }
            return result;
        }
        else if constexpr (std::is_same_v<T, RecordType>) {
            std::string result = "{";
            for (size_t i = 0; i < t.fields.size(); i++) {
                if (i > 0) result += ", ";
                result += t.fields[i].first + ": " +
                          typeToString(t.fields[i].second);
            }
            return result + "}";
        }
        else if constexpr (std::is_same_v<T, TypeVar>) {
            if (t.id < 0) {
                // Table-level generic placeholder: -1 -> 'A', -2 -> 'B', ...
                int idx = (-t.id - 1) % 26;
                return std::string(1, static_cast<char>('A' + idx));
            }
            return "T" + std::to_string(t.id);
        }
        else if constexpr (std::is_same_v<T, VoidType>) {
            return "Never";
        }
        else if constexpr (std::is_same_v<T, ConstrainedType>) {
            return t.varName;
        }
        else {
            // Capitalised like every other type name: it appears in hovers and
            // type dumps beside `Integer`/`String`, where a lowercase word read
            // as prose rather than as the type it is.
            return "Unknown";
        }
    }, type->kind);
}

auto structuredTypeOf(const TypePtr& type) -> std::optional<StructuredType> {
    if (!type) return std::nullopt;
    return std::visit([](const auto& t) -> std::optional<StructuredType> {
        using T = std::decay_t<decltype(t)>;
        auto argsOf = [](const std::vector<TypePtr>& types)
            -> std::optional<std::vector<StructuredType>> {
            std::vector<StructuredType> out;
            for (const auto& element : types) {
                auto structured = structuredTypeOf(element);
                if (!structured) return std::nullopt;
                out.push_back(std::move(*structured));
            }
            return out;
        };
        auto compound = [&](const std::string& name,
                            const std::vector<TypePtr>& types)
            -> std::optional<StructuredType> {
            auto args = argsOf(types);
            if (!args) return std::nullopt;
            return StructuredType{name, std::move(*args)};
        };
        // Unknown, type variables and trait bounds are exactly the cases the
        // value answers better than the checker.
        if constexpr (std::is_same_v<T, UnknownType> ||
                      std::is_same_v<T, TypeVar> ||
                      std::is_same_v<T, ConstrainedType> ||
                      std::is_same_v<T, UnionType> ||
                      std::is_same_v<T, IntersectionType> ||
                      std::is_same_v<T, RecordType>) {
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, FuncType>) {
            // Parameters then the result, matching how a signature is written.
            std::vector<TypePtr> parts = t.params;
            parts.push_back(t.result);
            return compound("Function", parts);
        } else if constexpr (std::is_same_v<T, NamedType>) {
            return compound(t.name, t.typeArgs);
        } else if constexpr (std::is_same_v<T, ListType>) {
            return compound("List", {t.element});
        } else if constexpr (std::is_same_v<T, OptionalType>) {
            return compound("Option", {t.inner});
        } else if constexpr (std::is_same_v<T, MapType>) {
            return compound("Map", {t.key, t.value});
        } else if constexpr (std::is_same_v<T, TupleType>) {
            return compound("Tuple", t.elements);
        } else if constexpr (std::is_same_v<T, SizedIntType>) {
            // Width is not observable at runtime (IntValue is one int64_t,
            // and the fallback answers "Integer"), so a static answer must
            // not claim more than the value can confirm.
            return StructuredType{"Integer", {}};
        } else if constexpr (std::is_same_v<T, SizedFloatType>) {
            return StructuredType{"Float", {}};
        } else if constexpr (std::is_same_v<T, PrimitiveType>) {
            if (t.kind == PrimitiveType::Integer)
                return StructuredType{"Integer", {}};
            // Reflection reports the RUNTIME type, so an atom answers "Atom"
            // whatever literal its static type was written as.
            return StructuredType{
                typeToString(std::make_shared<Type>(Type{PrimitiveType{t.kind}})), {}};
        } else {
            return StructuredType{typeToString(std::make_shared<Type>(Type{t})), {}};
        }
    }, type->kind);
}

auto isFullyConcrete(const TypePtr& type) -> bool {
    return !containsOpenType(type);
}

auto isConcreteValue(const TypePtr& type) -> bool {
    return !std::holds_alternative<VoidType>(type->kind) && isFullyConcrete(type);
}

auto isConcreteOrBound(const TypePtr& type) -> bool {
    return isConcreteValue(type) || std::holds_alternative<ConstrainedType>(type->kind);
}

auto typeVariableId(const TypePtr& type) -> std::optional<int> {
    if (const auto* variable = type ? std::get_if<TypeVar>(&type->kind) : nullptr) return variable->id;
    return std::nullopt;
}

auto containsTypeVariable(const TypePtr& type) -> bool {
    return anyType(
        type,
        [](const TypePtr& t) {
            return std::holds_alternative<TypeVar>(t->kind) ||
                   std::holds_alternative<ConstrainedType>(t->kind);
        },
        NullType::Unknown);
}

auto containsOpenType(const TypePtr& type) -> bool {
    return anyType(
        type,
        [](const TypePtr& t) {
            return std::holds_alternative<TypeVar>(t->kind) ||
                   std::holds_alternative<UnknownType>(t->kind) ||
                   std::holds_alternative<ConstrainedType>(t->kind);
        },
        NullType::Unknown);
}

// TupleType{} (empty tuple, from parsing `()` in a type annotation) and
// PrimitiveType{Unit} (from Type::unit() in compiled signatures) represents
// the same concept. Normalize both representations before comparing.
static auto isUnit(const TypePtr& t) -> bool {
    if (!t) return false;
    if (auto* p = std::get_if<PrimitiveType>(&t->kind)) return p->kind == PrimitiveType::Unit;
    if (auto* tup = std::get_if<TupleType>(&t->kind)) return tup->elements.empty();
    return false;
}

auto namedTypesMatch(const std::string& a, const std::string& b) -> bool {
    if (a == b) return true;
    const auto aQualified = a.find('.') != std::string::npos;
    const auto bQualified = b.find('.') != std::string::npos;
    // Two qualified identities are compared in full: that is what keeps
    // Tey.Manifest.Dependency and Tey.Lockfile.Dependency distinct.
    if (aQualified == bQualified) return false;
    const auto& qualified = aQualified ? a : b;
    const auto& bare = aQualified ? b : a;
    return qualified.size() > bare.size() + 1 &&
           qualified.compare(qualified.size() - bare.size(), bare.size(),
                             bare) == 0 &&
           qualified[qualified.size() - bare.size() - 1] == '.';
}

auto typesEqual(const TypePtr& a, const TypePtr& b) -> bool {
    if (!a || !b) return false;
    if (a.get() == b.get()) return true;
    if (isUnit(a) && isUnit(b)) return true;


    return std::visit([&b](const auto& at) -> bool {
        using AT = std::decay_t<decltype(at)>;
        auto* bt = std::get_if<AT>(&b->kind);
        if (!bt) return false;

        if constexpr (std::is_same_v<AT, PrimitiveType>) {
            return at.kind == bt->kind;
        }
        else if constexpr (std::is_same_v<AT, SizedIntType>) {
            return at.bits == bt->bits && at.isSigned == bt->isSigned;
        }
        else if constexpr (std::is_same_v<AT, SizedFloatType>) {
            return at.bits == bt->bits;
        }
        else if constexpr (std::is_same_v<AT, NamedType>) {
            if (!namedTypesMatch(at.name, bt->name)) return false;
            if (at.variant != bt->variant) return false;
            if (at.typeArgs.size() != bt->typeArgs.size()) return false;
            for (size_t i = 0; i < at.typeArgs.size(); i++) {
                if (!typesEqual(at.typeArgs[i], bt->typeArgs[i])) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<AT, FuncType>) {
            if (at.block != bt->block) return false;
            if (at.params.size() != bt->params.size()) return false;
            for (size_t i = 0; i < at.params.size(); i++) {
                if (!typesEqual(at.params[i], bt->params[i])) return false;
            }
            return typesEqual(at.result, bt->result);
        }
        else if constexpr (std::is_same_v<AT, ListType>) {
            return typesEqual(at.element, bt->element);
        }
        else if constexpr (std::is_same_v<AT, TupleType>) {
            if (at.elements.size() != bt->elements.size()) return false;
            for (size_t i = 0; i < at.elements.size(); i++) {
                if (!typesEqual(at.elements[i], bt->elements[i])) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<AT, MapType>) {
            return typesEqual(at.key, bt->key) && typesEqual(at.value, bt->value);
        }
        else if constexpr (std::is_same_v<AT, OptionalType>) {
            return typesEqual(at.inner, bt->inner);
        }
        else if constexpr (std::is_same_v<AT, TypeVar>) {
            return at.id == bt->id;
        }
        else if constexpr (std::is_same_v<AT, VoidType>) {
            return true;  // Never == Never
        }
        else if constexpr (std::is_same_v<AT, ConstrainedType>) {
            return at.varName == bt->varName && at.traitName == bt->traitName;
        }
        else if constexpr (std::is_same_v<AT, UnionType>) {
            // Compared as a SET: `Atom | Integer` and `Integer | Atom` denote
            // the same type, and nothing guarantees two spellings of one union
            // reach here in the same order.
            //
            // Without this case a union fell through to `return false`, so a
            // union-typed parameter was never equal to itself. Every dedupe
            // built on typesEqual then failed, and overload resolution
            // reported a call ambiguous against its OWN signature, printed
            // twice — `m.get(1, "")` on a `Regex.Match`, whose parameter is
            // `Atom | Integer`, and which is the documented example for that
            // method.
            if (at.members.size() != bt->members.size()) return false;
            std::vector<bool> paired(bt->members.size(), false);
            for (const auto& mine : at.members) {
                bool found = false;
                for (size_t i = 0; i < bt->members.size() && !found; i++) {
                    if (paired[i]) continue;
                    if (typesEqual(mine, bt->members[i])) {
                        paired[i] = true;
                        found = true;
                    }
                }
                if (!found) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<AT, IntersectionType>) {
            if (at.members.size() != bt->members.size()) return false;
            std::vector<bool> paired(bt->members.size(), false);
            for (const auto& mine : at.members) {
                bool found = false;
                for (size_t i = 0; i < bt->members.size() && !found; i++) {
                    if (!paired[i] && typesEqual(mine, bt->members[i])) {
                        paired[i] = true;
                        found = true;
                    }
                }
                if (!found) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<AT, RecordType>) {
            if (at.fields.size() != bt->fields.size()) return false;
            for (const auto& [name, mine] : at.fields) {
                auto other = std::find_if(
                    bt->fields.begin(), bt->fields.end(),
                    [&](const auto& field) { return field.first == name; });
                if (other == bt->fields.end() ||
                    !typesEqual(mine, other->second))
                    return false;
            }
            return true;
        }
        else {
            return false;
        }
    }, a->kind);
}

auto TypeEnv::set(const std::string& name, TypePtr type, Binding binding) -> void {
    m_entries[name] = Entry{std::move(type), binding};
}

auto TypeEnv::get(const std::string& name) const -> TypePtr {
    auto it = m_entries.find(name);
    return it == m_entries.end() ? nullptr : it->second.type;
}

auto TypeEnv::binding(const std::string& name) const -> const Binding* {
    auto it = m_entries.find(name);
    return it == m_entries.end() ? nullptr : &it->second.binding;
}

// ── Structural traversal ─────────────────────────────────────────────────────
//
// The one place that knows which Type alternatives nest other types. A walk or
// rewrite over types spells out only the kinds it treats specially and hands
// the rest to these, so adding an alternative is one edit, not twenty.

auto forEachTypeChild(const TypePtr& type,
                      const std::function<void(const TypePtr&)>& visit) -> void {
    if (!type) return;
    std::visit([&](const auto& part) {
        using Part = std::decay_t<decltype(part)>;
        if constexpr (std::is_same_v<Part, ListType>) {
            visit(part.element);
        } else if constexpr (std::is_same_v<Part, MapType>) {
            visit(part.key);
            visit(part.value);
        } else if constexpr (std::is_same_v<Part, OptionalType>) {
            visit(part.inner);
        } else if constexpr (std::is_same_v<Part, TupleType>) {
            for (const auto& element : part.elements) visit(element);
        } else if constexpr (std::is_same_v<Part, FuncType>) {
            for (const auto& param : part.params) visit(param);
            visit(part.result);
        } else if constexpr (std::is_same_v<Part, NamedType>) {
            for (const auto& arg : part.typeArgs) visit(arg);
        } else if constexpr (std::is_same_v<Part, UnionType> ||
                             std::is_same_v<Part, IntersectionType>) {
            for (const auto& member : part.members) visit(member);
        } else if constexpr (std::is_same_v<Part, RecordType>) {
            for (const auto& field : part.fields) visit(field.second);
        }
    }, type->kind);
}

// Rebuild `type` with each directly-nested type replaced by `map(child)`.
// Returns `type` unchanged for the leaf kinds, which have no children.
auto mapTypeChildren(const TypePtr& type,
                     const std::function<TypePtr(const TypePtr&)>& map)
    -> TypePtr {
    if (!type) return type;
    // Each child mapped; `same` stays true while every one comes back as the
    // very object it was, and then `type` itself is returned: an unchanged
    // type keeps its identity and costs no allocation.
    bool same = true;
    auto mapped = [&](const TypePtr& child) {
        auto result = map(child);
        same = same && result == child;
        return result;
    };
    auto mappedAll = [&](const std::vector<TypePtr>& children) {
        std::vector<TypePtr> results;
        results.reserve(children.size());
        for (const auto& child : children) results.push_back(mapped(child));
        return results;
    };
    return std::visit([&](const auto& part) -> TypePtr {
        using Part = std::decay_t<decltype(part)>;
        if constexpr (std::is_same_v<Part, ListType>) {
            auto element = mapped(part.element);
            return same ? type : Type::list(std::move(element));
        } else if constexpr (std::is_same_v<Part, MapType>) {
            auto key = mapped(part.key);
            auto value = mapped(part.value);
            return same ? type : Type::map(std::move(key), std::move(value));
        } else if constexpr (std::is_same_v<Part, OptionalType>) {
            auto inner = mapped(part.inner);
            return same ? type : Type::optional(std::move(inner));
        } else if constexpr (std::is_same_v<Part, TupleType>) {
            auto elements = mappedAll(part.elements);
            return same ? type : Type::tuple(std::move(elements));
        } else if constexpr (std::is_same_v<Part, FuncType>) {
            auto params = mappedAll(part.params);
            auto result = mapped(part.result);
            return same ? type
                        : Type::func(std::move(params), std::move(result),
                                     part.block);
        } else if constexpr (std::is_same_v<Part, NamedType>) {
            auto args = mappedAll(part.typeArgs);
            if (same) return type;
            return std::make_shared<Type>(Type{NamedType{part.name, std::move(args), part.variant}});
        } else if constexpr (std::is_same_v<Part, IntersectionType>) {
            auto members = mappedAll(part.members);
            return same ? type : Type::intersection(std::move(members));
        } else if constexpr (std::is_same_v<Part, UnionType>) {
            auto members = mappedAll(part.members);
            return same ? type
                        : std::make_shared<Type>(Type{UnionType{std::move(members)}});
        } else if constexpr (std::is_same_v<Part, RecordType>) {
            std::vector<std::pair<std::string, TypePtr>> fields;
            fields.reserve(part.fields.size());
            for (const auto& [name, fieldType] : part.fields)
                fields.emplace_back(name, mapped(fieldType));
            return same ? type : Type::record(std::move(fields));
        } else {
            return type;
        }
    }, type->kind);
}

auto anyType(const TypePtr& type,
             const std::function<bool(const TypePtr&)>& predicate,
             NullType nullType) -> bool {
    if (!type) return nullType == NullType::Unknown;
    if (predicate(type)) return true;
    bool found = false;
    forEachTypeChild(type, [&](const TypePtr& child) {
        found = found || anyType(child, predicate, nullType);
    });
    return found;
}


auto mentionsUnknown(const TypePtr& type, NullType nullType) -> bool {
    return anyType(
        type,
        [](const TypePtr& t) { return std::holds_alternative<UnknownType>(t->kind); },
        nullType);
}

} // namespace kex::semantic

namespace kex::semantic {

auto isUndetermined(const TypePtr& type) -> bool {
    return std::holds_alternative<UnknownType>(type->kind) ||
           std::holds_alternative<TypeVar>(type->kind);
}

auto holdsFailure(const TypePtr& type) -> bool {
    if (isUndetermined(type) || std::holds_alternative<OptionalType>(type->kind)) return true;
    const auto* named = std::get_if<NamedType>(&type->kind);
    return named && (named->name == names::kResultType || named->name == names::kOptionalType);
}

auto isThisPlaceholder(const TypePtr& type) -> bool {
    const auto* named = type ? std::get_if<NamedType>(&type->kind) : nullptr;
    return named && named->name == names::kThisType && named->typeArgs.empty();
}

auto isNumericType(const TypePtr& type) -> bool {
    if (!type) return false;
    if (const auto* primitive = std::get_if<PrimitiveType>(&type->kind))
        return primitive->kind == PrimitiveType::Integer;
    return std::holds_alternative<SizedIntType>(type->kind) ||
           std::holds_alternative<SizedFloatType>(type->kind);
}

} // namespace kex::semantic
