#include "traits.hxx"
#include "../common/well_known.hxx"
#include <cassert>

namespace kex::semantic {

auto tieTraitResult(
    Signature& signature,
    const std::function<TypePtr(const TypePtr&)>& resolveType,
    const std::function<bool(const Type* occurrence)>& justified) -> void {
    if (!signature.result) {
        return;
    }
    // A `Number` as written: not yet a generic, and not one inference made
    // (that one is named `N`, and already relates the positions it shares).
    const auto writtenNumber = [](const TypePtr& type) {
        const auto* constrained = std::get_if<ConstrainedType>(&type->kind);
        return constrained && constrained->genericId == 0 &&
               constrained->varName == constrained->traitName &&
               constrained->traitName == kex::names::kNumberTrait;
    };
    std::vector<const Type*> inParams;
    std::function<void(const TypePtr&)> collect = [&](const TypePtr& type) {
        if (!type) {
            return;
        }
        const auto resolved = resolveType(type);
        if (writtenNumber(resolved)) {
            inParams.push_back(resolved.get());
        }
        forEachTypeChild(resolved, collect);
    };
    for (const auto& param : signature.params) {
        collect(param);
    }
    bool inResult = false;
    std::function<void(const TypePtr&)> findInResult = [&](const TypePtr& type) {
        if (!type || inResult) {
            return;
        }
        const auto resolved = resolveType(type);
        inResult = writtenNumber(resolved);
        forEachTypeChild(resolved, findInResult);
    };
    findInResult(signature.result);
    if (!inResult || inParams.size() != 1 || !justified(inParams.front())) {
        return;
    }
    // Below every generic the signature already uses, so it collides with none.
    int lowest = 0;
    std::function<void(const TypePtr&)> lowestGeneric = [&](const TypePtr& type) {
        if (!type) {
            return;
        }
        const auto resolved = resolveType(type);
        if (const auto* variable = std::get_if<TypeVar>(&resolved->kind)) {
            lowest = std::min(lowest, variable->id);
        } else if (const auto* constrained = std::get_if<ConstrainedType>(&resolved->kind)) {
            lowest = std::min(lowest, constrained->genericId);
        }
        forEachTypeChild(resolved, lowestGeneric);
    };
    for (const auto& param : signature.params) {
        lowestGeneric(param);
    }
    lowestGeneric(signature.result);
    // Keeps the written name, so an interface still writes `Number` and its
    // reader ties the signature again by this same rule.
    const auto tied =
        Type::constrained(kex::names::kNumberTrait, kex::names::kNumberTrait, lowest - 1);
    std::function<TypePtr(const TypePtr&)> tie = [&](const TypePtr& type) -> TypePtr {
        if (!type) {
            return type;
        }
        const auto resolved = resolveType(type);
        if (writtenNumber(resolved)) {
            return tied;
        }
        return mapTypeChildren(resolved, tie);
    };
    for (auto& param : signature.params) {
        param = tie(param);
    }
    signature.result = tie(signature.result);
}

auto sameParameters(const Signature& left, const Signature& right) -> bool {
    if (left.params.size() != right.params.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.params.size(); ++i) {
        const auto& a = left.params[i];
        const auto& b = right.params[i];
        if (a && b && std::holds_alternative<UnknownType>(a->kind) &&
            std::holds_alternative<UnknownType>(b->kind)) {
            continue;
        }
        if (!typesEqual(a, b)) {
            return false;
        }
    }
    return true;
}

auto TraitRegistry::define(TraitDef def) -> void {
    m_traits[def.name] = std::move(def);
}

auto TraitRegistry::get(const std::string& name) const -> const TraitDef* {
    auto it = m_traits.find(name);
    return it == m_traits.end() ? nullptr : &it->second;
}

auto TraitRegistry::lookup(const std::string& written) const -> const TraitDef* {
    if (const auto* exact = get(written)) {
        return exact;
    }
    const TraitDef* found = nullptr;
    const auto suffix = "." + written;
    for (const auto& [name, def] : m_traits) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            if (found) {
                return nullptr;
            }
            found = &def;
        }
    }
    return found;
}

auto TraitRegistry::registerImplementation(
    const std::string& typeName, const std::string& traitName) -> void {
    m_implementations[typeName].insert(traitName);
}

auto TraitRegistry::registerConditionalImplementation(
    const std::string& typeName, const std::string& traitName, TypeArgBounds bounds) -> void {
    m_conditionalImplementations[typeName][traitName].push_back(std::move(bounds));
}

auto TraitRegistry::boundsHold(const NamedType& type, const TypeArgBounds& bounds) const -> bool {
    for (const auto& [index, bound] : bounds) {
        if (index >= type.typeArgs.size()) {
            continue;
        }
        const auto& argument = type.typeArgs[index];
        if (std::holds_alternative<TypeVar>(argument->kind) ||
            std::holds_alternative<UnknownType>(argument->kind)) {
            continue;
        }
        if (!satisfies(argument, bound)) {
            return false;
        }
    }
    return true;
}

auto TraitRegistry::implementorKey(const TypePtr& type) const -> std::string {
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        return named->name;
    }
    // A float keys by its size, whatever it prints as (`Float`, unless
    // `Float64` was written): conformances are registered by that name.
    if (const auto* sized = std::get_if<SizedFloatType>(&type->kind)) {
        return sized->bits == 32 ? "Float32" : "Float64";
    }
    return typeToString(type);
}

auto TraitRegistry::satisfiesStructurally(const TypePtr& type, const std::string& traitName) const
    -> bool {
    // A built-in type can arrive spelled as a name: Type::named(m_currentMakeType)
    // inside a `make Integer` block, or an annotation that wrote `Int64`. It is
    // the type the one built-in table gives that name, so no second list of
    // integer and float names is kept here. `Float` is the one numeric name the
    // table has no entry for, because it is a bound over the float types
    // (docs/typechecker-hm-plan.md §6.3).
    auto resolved = type;
    bool namedFloat = false;
    if (const auto* named = std::get_if<NamedType>(&type->kind)) {
        namedFloat = named->name == kex::names::kFloatType;
        if (auto builtin = builtinType(named->name)) {
            resolved = builtin;
        }
    }
    const bool isInt = std::holds_alternative<SizedIntType>(resolved->kind) ||
                       (std::holds_alternative<PrimitiveType>(resolved->kind) &&
                        std::get<PrimitiveType>(resolved->kind).kind == PrimitiveType::Integer);
    const bool isFloat = namedFloat || std::holds_alternative<SizedFloatType>(resolved->kind);

    if (traitName == kex::names::kIntegerType) {
        return isInt;
    }
    if (traitName == kex::names::kFloatType) {
        return isFloat;
    }
    if (traitName == kex::names::kNumberTrait) {
        return isInt || isFloat;
    }
    return false;
}

auto TraitRegistry::satisfies(const TypePtr& type, const std::string& traitName) const -> bool {
    if (!type) {
        return false;
    }
    // Every runtime value has structural inspect and plain show fallbacks.
    // Concrete make blocks may still override either presentation.
    if (traitName == kex::names::kInspectableTrait || traitName == kex::names::kShowableTrait) {
        return true;
    }

    // A value whose type IS the trait (e.g. NamedType("Shape") against trait "Shape")
    // trivially satisfies that trait — it was already widened to the trait type.
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        if (named->name == traitName && m_traits.count(traitName)) {
            return true;
        }
    }

    // A ConstrainedType("T", "Integer") satisfies "Integer" and (by extension) "Number";
    // likewise "Float" satisfies "Number". This comes up when a constrained hint type
    // (e.g., from an imported signature's Integer constraint) is passed as an arg against
    // another constrained param — both are constrained the same way and are compatible.
    if (auto* ct = std::get_if<ConstrainedType>(&type->kind)) {
        if (ct->traitName == traitName) {
            return true;
        }
        if (traitName == kex::names::kNumberTrait && (ct->traitName == kex::names::kIntegerType ||
                                                      ct->traitName == kex::names::kFloatType)) {
            return true;
        }
        return false;
    }

    // A union satisfies a trait exactly when every member does — every value
    // it can hold implements the trait. This is what lets a union `make`
    // target be used at the trait: `make Float | Integer` has a `Number`
    // receiver, so a method there may be declared to return `Number`.
    if (auto* uni = std::get_if<UnionType>(&type->kind)) {
        if (uni->members.empty()) {
            return false;
        }
        for (const auto& member : uni->members) {
            if (!satisfies(member, traitName)) {
                return false;
            }
        }
        return true;
    }

    if (traitName == kex::names::kNumberTrait || traitName == kex::names::kIntegerType ||
        traitName == kex::names::kFloatType) {
        return satisfiesStructurally(type, traitName);
    }

    // Compound types recurse into their component types for Equatable/
    // Showable — ordering compound types isn't supported structurally
    // here, so Comparable doesn't get this treatment.
    if (traitName == kex::names::kEquatableTrait || traitName == kex::names::kShowableTrait) {
        if (auto* list = std::get_if<ListType>(&type->kind)) {
            return satisfies(list->element, traitName);
        }
        if (auto* tup = std::get_if<TupleType>(&type->kind)) {
            for (const auto& elem : tup->elements) {
                if (!satisfies(elem, traitName)) {
                    return false;
                }
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
    if (it != m_implementations.end() && it->second.count(traitName) > 0) {
        return true;
    }
    // What a bounded `make` claims (#651): `make A is Discrete => Range<A>,
    // implement: Enumerable` makes a `Range<X>` Enumerable where X is
    // Discrete. An argument not known yet is not judged against.
    if (const auto* named = std::get_if<NamedType>(&type->kind)) {
        if (const auto byType = m_conditionalImplementations.find(named->name);
            byType != m_conditionalImplementations.end()) {
            if (const auto claims = byType->second.find(traitName);
                claims != byType->second.end()) {
                for (const auto& bounds : claims->second) {
                    if (boundsHold(*named, bounds)) {
                        return true;
                    }
                }
            }
        }
    }
    // What one instantiation claims: `make Range<Integer>, implement:
    // Enumerable` makes `Range<Integer>` Enumerable, and not `Range<Float>`.
    if (const auto* named = std::get_if<NamedType>(&type->kind);
        named && !named->typeArgs.empty()) {
        const auto instance = m_implementations.find(typeToString(type));
        if (instance != m_implementations.end() && instance->second.count(traitName) > 0) {
            return true;
        }
    }

    // The family it belongs to claims it too: a list, map or optional what
    // `make [X], implement: Trait` claims, a sized number what `make Float`
    // or `make Integer` does (`1.5.present?`, a Float64, is Blankable).
    const auto family = familyKey(type);
    if (family.empty()) {
        return false;
    }
    const auto claims = m_implementations.find(family);
    return claims != m_implementations.end() && claims->second.count(traitName) > 0;
}

// The name a type's family registers conformances under, or "".
auto TraitRegistry::familyKey(const TypePtr& type) -> std::string {
    if (std::holds_alternative<ListType>(type->kind)) {
        return "List";
    }
    if (std::holds_alternative<MapType>(type->kind)) {
        return "Map";
    }
    if (std::holds_alternative<OptionalType>(type->kind)) {
        return "Optional";
    }
    if (isNumericType(type)) {
        return std::holds_alternative<SizedFloatType>(type->kind) ? "Float" : "Integer";
    }
    return "";
}

auto TraitRegistry::implementsInHome(const std::string& type, const std::string& method) const
    -> bool {
    auto implemented = m_implementations.find(type);
    if (implemented == m_implementations.end()) {
        return false;
    }
    for (const auto& traitName : implemented->second) {
        auto trait = m_traits.find(traitName);
        if (trait == m_traits.end()) {
            continue;
        }
        for (const auto& required : trait->second.requiredMethods) {
            if (required.name == method) {
                return true;
            }
        }
        for (const auto& body : trait->second.defaultBodies) {
            if (body == method) {
                return true;
            }
        }
    }
    return false;
}

auto TraitRegistry::declaresMethod(const std::string& type, const std::string& method) const
    -> bool {
    auto implemented = m_implementations.find(type);
    if (implemented == m_implementations.end()) {
        return false;
    }
    for (const auto& traitName : implemented->second) {
        auto trait = m_traits.find(traitName);
        if (trait == m_traits.end()) {
            continue;
        }
        for (const auto& required : trait->second.requiredMethods) {
            if (required.name == method) {
                return true;
            }
        }
        for (const auto& defaulted : trait->second.defaultMethods) {
            if (defaulted == method) {
                return true;
            }
        }
    }
    return false;
}

auto TraitRegistry::requiredMethod(const TypePtr& type, const std::string& method) const
    -> std::pair<const TraitDef*, const Signature*> {
    auto implemented = m_implementations.find(implementorKey(type));
    if (implemented == m_implementations.end()) {
        return {nullptr, nullptr};
    }
    for (const auto& traitName : implemented->second) {
        auto trait = m_traits.find(traitName);
        if (trait == m_traits.end()) {
            continue;
        }
        for (const auto& required : trait->second.requiredMethods) {
            if (required.name == method) {
                return {&trait->second, &required};
            }
        }
    }
    return {nullptr, nullptr};
}

auto TraitRegistry::requiredOverloads(const TypePtr& type, const std::string& method) const
    -> std::vector<const Signature*> {
    std::vector<const Signature*> overloads;
    auto implemented = m_implementations.find(implementorKey(type));
    if (implemented == m_implementations.end()) {
        return overloads;
    }
    for (const auto& traitName : implemented->second) {
        if (auto trait = m_traits.find(traitName); trait != m_traits.end()) {
            for (const auto& required : trait->second.requiredMethods) {
                if (required.name == method) {
                    overloads.push_back(&required);
                }
            }
        }
    }
    return overloads;
}

auto TraitRegistry::withBuiltins() -> TraitRegistry {
    TraitRegistry reg;

    reg.define(TraitDef{"Number", {}});
    reg.define(TraitDef{"Integer", {}});
    reg.define(TraitDef{"Float", {}});
    reg.define(
        TraitDef{"Equatable", {Signature{"==", {Type::genericLetter(-1)}, Type::boolean()}}});
    // `Ordering` is the type comparable.kex declares and the interpreter
    // builds Less/Equal/Greater from. The result was previously named
    // "Comparison", which no declaration anywhere defines.
    reg.define(
        TraitDef{
            "Comparable",
            {Signature{"compare", {Type::genericLetter(-1)}, Type::named("Ordering")}}});
    reg.define(
        TraitDef{"Inspectable", {Signature{"inspectValue", {Type::boolean()}, Type::string()}}});
    reg.define(TraitDef{"Showable", {Signature{"showValue", {}, Type::string()}}});

    // Primitive/sized types implement Equatable/Showable, keyed by their
    // canonical printed name (see implementorKey) — same registry path a
    // NamedType implementation would use.
    static const char* kEquatableShowable[] = {
        "Int",    "Integer", "Byte",    "Int8", "Int16", "Int32", "UInt16", "UInt32",
        "UInt64", "Float32", "Float64", "Char", "Bool",  "Atom",  "String", "Void",
    };
    for (const char* name : kEquatableShowable) {
        reg.registerImplementation(name, "Equatable");
        reg.registerImplementation(name, "Showable");
    }

    // Bool doesn't order with <, >, <=, >= (matches inferBinaryOp's
    // existing rule), so Comparable is registered for everyone else.
    static const char* kComparable[] = {
        "Int",    "Integer", "Byte",    "Int8",    "Int16", "Int32",  "UInt16",
        "UInt32", "UInt64",  "Float32", "Float64", "Char",  "String",
    };
    for (const char* name : kComparable) {
        reg.registerImplementation(name, "Comparable");
    }

    return reg;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TraitRegistry::isStructural(const std::string& traitName) -> bool {
    return traitName == kex::names::kNumberTrait || traitName == kex::names::kIntegerType ||
           traitName == kex::names::kFloatType || traitName == kex::names::kInspectableTrait ||
           traitName == kex::names::kShowableTrait;
}

} // namespace kex::semantic
