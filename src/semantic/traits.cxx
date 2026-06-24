#include "traits.hxx"
#include <cassert>

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
    auto& implemented = m_implementations[typeName];
    assert(implemented.count(traitName) == 0 &&
           "duplicate (typeName, traitName) registration — coherence violation");
    implemented.insert(traitName);
}

auto TraitRegistry::implementorKey(const TypePtr& type) const -> std::string {
    if (auto* named = std::get_if<NamedType>(&type->kind)) return named->name;
    return typeToString(type);
}

auto TraitRegistry::satisfiesStructurally(const TypePtr& type, const std::string& traitName) const -> bool {
    bool isInt = std::holds_alternative<SizedIntType>(type->kind) ||
                 (std::holds_alternative<PrimitiveType>(type->kind) &&
                  std::get<PrimitiveType>(type->kind).kind == PrimitiveType::Integer);
    bool isFloat = std::holds_alternative<SizedFloatType>(type->kind);

    if (traitName == "Integer") return isInt;
    if (traitName == "Float") return isFloat;
    if (traitName == "Number") return isInt || isFloat;
    return false;
}

auto TraitRegistry::satisfies(const TypePtr& type, const std::string& traitName) const -> bool {
    if (!type) return false;

    if (traitName == "Number" || traitName == "Integer" || traitName == "Float") {
        return satisfiesStructurally(type, traitName);
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

    return reg;
}

} // namespace kex::semantic
