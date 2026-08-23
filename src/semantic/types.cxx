#include "types.hxx"
#include <unordered_set>

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

auto Type::voidType() -> TypePtr {
    return std::make_shared<Type>(Type{VoidType{}});
}

auto Type::named(const std::string& name, std::vector<TypePtr> args) -> TypePtr {
    return std::make_shared<Type>(Type{NamedType{name, std::move(args)}});
}

auto Type::func(std::vector<TypePtr> params, TypePtr result) -> TypePtr {
    return std::make_shared<Type>(Type{FuncType{std::move(params), std::move(result)}});
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

auto isPrimitiveTypeName(const std::string& name) -> bool {
    // Kept beside typeToString deliberately: these are exactly the names it
    // prints for types that no source file declares. A structural type such
    // as List or Map only appears here if the stdlib does not declare it —
    // `type List<X> = [X]` in list.kex means `List` resolves as a declared
    // type name and needs no entry.
    static const std::unordered_set<std::string> names = {
        "Integer", "Int", "Float", "Number", "String", "Char", "Bool",
        "Atom", "Void", "Any", "Byte", "Int8", "Int16", "Int32", "Int64",
        "UInt16", "UInt32", "UInt64", "Float32", "Float64",
        "Tuple", "Block",
    };
    return names.count(name) != 0;
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
            // Kex function types are curried, so they print as
            // `A -> B -> R`, matching how they are written in source. A
            // parameter that is itself a function has to be parenthesised or
            // the arrows associate wrongly: `(A -> Bool) -> [A]` says
            // something quite different from `A -> Bool -> [A]`.
            if (t.params.empty()) return "() -> " + typeToString(t.result);
            std::string result;
            for (const auto& param : t.params) {
                auto text = typeToString(param);
                if (param && std::holds_alternative<FuncType>(param->kind))
                    text = "(" + text + ")";
                result += text + " -> ";
            }
            return result + typeToString(t.result);
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
                      std::is_same_v<T, UnionType>) {
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
    if (!type) return false;
    return std::visit([](const auto& t) -> bool {
        using T = std::decay_t<decltype(t)>;
        auto all = [](const std::vector<TypePtr>& types) {
            for (const auto& element : types)
                if (!isFullyConcrete(element)) return false;
            return true;
        };
        if constexpr (std::is_same_v<T, UnknownType> ||
                      std::is_same_v<T, TypeVar> ||
                      std::is_same_v<T, ConstrainedType>) {
            return false;
        } else if constexpr (std::is_same_v<T, NamedType>) {
            return all(t.typeArgs);
        } else if constexpr (std::is_same_v<T, ListType>) {
            return isFullyConcrete(t.element);
        } else if constexpr (std::is_same_v<T, OptionalType>) {
            return isFullyConcrete(t.inner);
        } else if constexpr (std::is_same_v<T, MapType>) {
            return isFullyConcrete(t.key) && isFullyConcrete(t.value);
        } else if constexpr (std::is_same_v<T, TupleType>) {
            return all(t.elements);
        } else if constexpr (std::is_same_v<T, UnionType>) {
            return all(t.members);
        } else if constexpr (std::is_same_v<T, FuncType>) {
            return all(t.params) && isFullyConcrete(t.result);
        } else {
            return true;
        }
    }, type->kind);
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
            if (at.typeArgs.size() != bt->typeArgs.size()) return false;
            for (size_t i = 0; i < at.typeArgs.size(); i++) {
                if (!typesEqual(at.typeArgs[i], bt->typeArgs[i])) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<AT, FuncType>) {
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
        else {
            return false;
        }
    }, a->kind);
}

auto TypeEnv::set(const std::string& name, TypePtr type) -> void {
    m_types[name] = std::move(type);
}

auto TypeEnv::get(const std::string& name) const -> TypePtr {
    auto it = m_types.find(name);
    if (it != m_types.end()) return it->second;
    return nullptr;
}

auto TypeEnv::has(const std::string& name) const -> bool {
    return m_types.count(name) > 0;
}

} // namespace kex::semantic
