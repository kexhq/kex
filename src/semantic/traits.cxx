#include "traits.hxx"
#include <cassert>
#include <unordered_set>

namespace kex::semantic {

auto TraitRegistry::define(TraitDef def) -> void {
    m_traits[def.name] = std::move(def);
}

auto TraitRegistry::get(const std::string& name) const -> const TraitDef* {
    auto it = m_traits.find(name);
    return it == m_traits.end() ? nullptr : &it->second;
}

auto TraitRegistry::registerImplementation(const std::string& typeName,
                                            const std::string& traitName) -> void {
    m_implementations[typeName].insert(traitName);
}

auto TraitRegistry::implementorKey(const TypePtr& type) const -> std::string {
    if (auto* named = std::get_if<NamedType>(&type->kind)) return named->name;
    return typeToString(type);
}

auto TraitRegistry::satisfiesStructurally(const TypePtr& type, const std::string& traitName) const -> bool {
    bool isInt = std::holds_alternative<SizedIntType>(type->kind) ||
                 (std::holds_alternative<PrimitiveType>(type->kind) &&
                  std::get<PrimitiveType>(type->kind).kind == PrimitiveType::Integer);
    if (!isInt) {
        // NamedType("Integer") and friends — produced by Type::named(m_currentMakeType)
        // inside make blocks or by user type annotations that spell "Integer" as a name.
        if (auto* named = std::get_if<NamedType>(&type->kind)) {
            static const std::unordered_set<std::string> kIntNames = {
                "Int", "Integer", "Byte", "Int8", "Int16", "Int32", "Int64",
                "UInt8", "UInt16", "UInt32", "UInt64"
            };
            isInt = kIntNames.count(named->name) > 0;
        }
    }
    bool isFloat = std::holds_alternative<SizedFloatType>(type->kind);
    if (!isFloat) {
        if (auto* named = std::get_if<NamedType>(&type->kind)) {
            static const std::unordered_set<std::string> kFloatNames = {
                "Float", "Float32", "Float64", "Double"
            };
            isFloat = kFloatNames.count(named->name) > 0;
        }
    }

    if (traitName == "Integer") return isInt;
    if (traitName == "Float") return isFloat;
    if (traitName == "Number") return isInt || isFloat;
    return false;
}

auto TraitRegistry::satisfies(const TypePtr& type, const std::string& traitName) const -> bool {
    if (!type) return false;

    // A value whose type IS the trait (e.g. NamedType("Shape") against trait "Shape")
    // trivially satisfies that trait — it was already widened to the trait type.
    if (auto* named = std::get_if<NamedType>(&type->kind))
        if (named->name == traitName && m_traits.count(traitName)) return true;

    // A ConstrainedType("T", "Integer") satisfies "Integer" and (by extension) "Number";
    // likewise "Float" satisfies "Number". This comes up when a constrained hint type
    // (e.g., from a stdlib sig's integerLike() placeholder) is passed as an arg against
    // another constrained param — both are constrained the same way and are compatible.
    if (auto* ct = std::get_if<ConstrainedType>(&type->kind)) {
        if (ct->traitName == traitName) return true;
        if (traitName == "Number" && (ct->traitName == "Integer" || ct->traitName == "Float")) return true;
        return false;
    }

    if (traitName == "Number" || traitName == "Integer" || traitName == "Float") {
        return satisfiesStructurally(type, traitName);
    }

    // Any Optional type satisfies Optionable — Optional<TypeVar>, Optional<String>,
    // etc. — regardless of inner type, since `.or(default)` only cares that the
    // receiver is optional, not what T is.
    if (traitName == "Optionable") {
        if (std::holds_alternative<OptionalType>(type->kind)) return true;
    }

    // Compound types recurse into their component types for Equatable/
    // Showable — ordering compound types isn't supported structurally
    // here, so Comparable doesn't get this treatment.
    if (traitName == "Equatable" || traitName == "Showable") {
        if (auto* list = std::get_if<ListType>(&type->kind)) {
            return satisfies(list->element, traitName);
        }
        if (auto* tup = std::get_if<TupleType>(&type->kind)) {
            for (const auto& elem : tup->elements) {
                if (!satisfies(elem, traitName)) return false;
            }
            return true;
        }
        if (auto* opt = std::get_if<OptionalType>(&type->kind)) {
            return satisfies(opt->inner, traitName);
        }
        if (auto* map = std::get_if<MapType>(&type->kind)) {
            return satisfies(map->key, traitName) && satisfies(map->value, traitName);
        }
    }

    auto it = m_implementations.find(implementorKey(type));
    if (it == m_implementations.end()) return false;
    return it->second.count(traitName) > 0;
}

auto TraitRegistry::commonTrait(const TypePtr& a, const TypePtr& b) const -> std::string {
    auto keyA = implementorKey(a);
    auto keyB = implementorKey(b);
    auto itA = m_implementations.find(keyA);
    auto itB = m_implementations.find(keyB);
    if (itA == m_implementations.end() || itB == m_implementations.end()) return "";
    // Skip structural/primitive traits — only return user-defined ones.
    static const std::set<std::string> kBuiltin = {
        "Number", "Integer", "Float", "Equatable", "Comparable",
        "Showable", "Resultable", "Optionable"
    };
    for (const auto& t : itA->second) {
        if (kBuiltin.count(t)) continue;
        if (itB->second.count(t)) return t;
    }
    return "";
}

auto TraitRegistry::withBuiltins() -> TraitRegistry {
    TraitRegistry reg;

    reg.define(TraitDef{"Number", {}});
    reg.define(TraitDef{"Integer", {}});
    reg.define(TraitDef{"Float", {}});
    reg.define(TraitDef{"Equatable",
        {Signature{"==", {Type::typeVar(-1)}, Type::boolean()}}});
    reg.define(TraitDef{"Comparable",
        {Signature{"compare", {Type::typeVar(-1)}, Type::named("Comparison")}}});
    reg.define(TraitDef{"Showable",
        {Signature{"to_s", {}, Type::string()}}});
    reg.define(TraitDef{"Resultable", {}});
    reg.define(TraitDef{"Optionable", {}});

    // Primitive/sized types implement Equatable/Showable, keyed by their
    // canonical printed name (see implementorKey) — same registry path a
    // NamedType implementation would use.
    static const char* kEquatableShowable[] = {
        "Int", "Integer", "Byte", "Int8", "Int16", "Int32",
        "UInt16", "UInt32", "UInt64", "Float32", "Float64",
        "Char", "Bool", "Atom", "String", "()",
    };
    for (const char* name : kEquatableShowable) {
        reg.registerImplementation(name, "Equatable");
        reg.registerImplementation(name, "Showable");
    }

    // Bool doesn't order with <, >, <=, >= (matches inferBinaryOp's
    // existing rule), so Comparable is registered for everyone else.
    static const char* kComparable[] = {
        "Int", "Integer", "Byte", "Int8", "Int16", "Int32",
        "UInt16", "UInt32", "UInt64", "Float32", "Float64",
        "Char", "String",
    };
    for (const char* name : kComparable) {
        reg.registerImplementation(name, "Comparable");
    }

    // Result<T,E> = Ok(T) | Error(E); Option<T> = Just(T) | None — the
    // checker doesn't yet track these as real generic NamedTypes (that's
    // phase 5), so the individual constructor names are registered as a
    // pragmatic bridge until then.
    for (const char* name : {"Result", "Ok", "Error"}) {
        reg.registerImplementation(name, "Resultable");
    }
    for (const char* name : {"Option", "Just", "None"}) {
        reg.registerImplementation(name, "Optionable");
    }
    for (const char* name : {"Either", "Left", "Right"}) {
        reg.registerImplementation(name, "Eitherable");
    }

    return reg;
}

} // namespace kex::semantic
