#include "types.hxx"

namespace kex::semantic {

auto Type::integer() -> TypePtr {
    return std::make_shared<Type>(Type{PrimitiveType{PrimitiveType::Integer}});
}

auto Type::charT() -> TypePtr {
    return std::make_shared<Type>(Type{PrimitiveType{PrimitiveType::Char}});
}

auto Type::string() -> TypePtr {
    return Type::list(Type::charT());
}

auto Type::boolean() -> TypePtr {
    return std::make_shared<Type>(Type{PrimitiveType{PrimitiveType::Bool}});
}

auto Type::atom() -> TypePtr {
    return std::make_shared<Type>(Type{PrimitiveType{PrimitiveType::Atom}});
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

auto Type::constrained(const std::string& varName, const std::string& traitName) -> TypePtr {
    return std::make_shared<Type>(Type{ConstrainedType{varName, traitName}});
}

auto typeToString(const TypePtr& type) -> std::string {
    if (!type) return "?";

    return std::visit([](const auto& t) -> std::string {
        using T = std::decay_t<decltype(t)>;

        if constexpr (std::is_same_v<T, PrimitiveType>) {
            switch (t.kind) {
                case PrimitiveType::Integer: return "Integer";
                case PrimitiveType::Char: return "Char";
                case PrimitiveType::Bool: return "Bool";
                case PrimitiveType::Atom: return "Atom";
                case PrimitiveType::Unit: return "()";
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
            std::string result = "(";
            for (size_t i = 0; i < t.params.size(); i++) {
                if (i > 0) result += ", ";
                result += typeToString(t.params[i]);
            }
            result += ") -> " + typeToString(t.result);
            return result;
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
            if (auto* elemPrim = std::get_if<PrimitiveType>(&t.element->kind);
                elemPrim && elemPrim->kind == PrimitiveType::Char) {
                return "String";
            }
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
            return "unknown";
        }
    }, type->kind);
}

// TupleType{} (empty tuple, from parsing `()` in a type annotation) and
// PrimitiveType{Unit} (from Type::unit() in compiled signatures) represent
// the same concept. Normalize both to Unit before comparing.
static auto isUnit(const TypePtr& t) -> bool {
    if (!t) return false;
    if (auto* p = std::get_if<PrimitiveType>(&t->kind)) return p->kind == PrimitiveType::Unit;
    if (auto* tup = std::get_if<TupleType>(&t->kind)) return tup->elements.empty();
    return false;
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
            if (at.name != bt->name) return false;
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
