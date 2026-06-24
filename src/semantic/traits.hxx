#pragma once

#include "types.hxx"
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

// A named function/method signature — used both for a trait's required
// methods (TraitDef::requiredMethods) and the stdlib function table (see
// stdlib_signatures.hxx).
struct Signature {
    std::string name;
    std::vector<TypePtr> params;
    TypePtr result;
};

struct TraitDef {
    std::string name;                        // "Number", "Comparable", user-defined names too
    std::vector<Signature> requiredMethods;  // e.g. Comparable requires `compare : This -> Comparison`
};

// Open, name-keyed registry of traits and which types implement them.
// Built-in traits (Number, Integer, Float, Equatable, Comparable,
// Resultable, Optionable, Showable) are registered the same way a future
// user `trait ... end` block would be — no special-casing built-ins vs.
// user traits at the call sites that consult this registry.
class TraitRegistry {
public:
    auto define(TraitDef def) -> void;
    auto get(const std::string& name) const -> const TraitDef*;

    // Is `type` a member of `traitName`? Number/Integer/Float are checked
    // structurally against the type's shape (any SizedIntType/SizedFloatType,
    // or the arbitrary-precision PrimitiveType::Integer); compound types
    // (list/tuple/map/optional) recurse into their component types for
    // Equatable/Showable. Everything else is checked against types
    // registered via registerImplementation, keyed by the type's NamedType
    // name or (for built-in primitives/sized types) its canonical printed
    // name from typeToString.
    auto satisfies(const TypePtr& type, const std::string& traitName) const -> bool;

    // Record that `typeName` implements `traitName`. Coherence: at most
    // one registration per (typeName, traitName) pair — a duplicate is a
    // programmer error (asserts) rather than a silent override, since
    // dispatch needs to resolve to exactly one implementation with no
    // priority/ordering rule to fall back on.
    auto registerImplementation(const std::string& typeName, const std::string& traitName) -> void;

    // A registry with Number/Integer/Float/Equatable/Comparable/Showable/
    // Resultable/Optionable pre-registered, plus implementations for the
    // built-in primitive/sized types and the Result/Option prelude ADTs.
    static auto withBuiltins() -> TraitRegistry;

private:
    std::unordered_map<std::string, TraitDef> m_traits;
    std::unordered_map<std::string, std::set<std::string>> m_implementations;

    auto satisfiesStructurally(const TypePtr& type, const std::string& traitName) const -> bool;
    auto implementorKey(const TypePtr& type) const -> std::string;
};

} // namespace kex::semantic
