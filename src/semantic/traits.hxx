#pragma once

#include "types.hxx"
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

// A named function/method signature — used for trait required methods
// (TraitDef::requiredMethods) and imported interface signatures.
struct Signature {
    std::string name;
    std::vector<TypePtr> params;
    TypePtr result;
    bool isFoul = false;
    // How many of `params` the caller must actually pass. Trailing parameters
    // with a default are optional, so `let greet(name, punct = "!")` accepts
    // one argument or two. `nullopt` means "not recorded" and is read as
    // `params.size()` — every signature that does not come from a FunctionDef
    // (trait requirements, imported interfaces) leaves it that way. A real
    // zero must remain representable for functions whose every parameter has
    // a default.
    std::optional<std::size_t> requiredParams;
    // Source parameter names, in order, for a signature that came from a
    // FunctionDef; empty when the signature came from a `:>` annotation, a
    // trait requirement or an imported interface, none of which name their
    // parameters. A receiver prepended to `params` gets no entry here, so a
    // shorter list than `params` means the first parameter is the receiver.
    // Named arguments at a call site need these to place their values.
    std::vector<std::string> paramNames;
    // The module whose `make` block defines this method, "" for a top-level
    // one. A module-scoped `make` is import-gated exactly like every other
    // module member (docs/ufcs-dispatch-plan.md "Follow-up"), so a call site
    // may only see it with a matching `using` in scope.
    std::string makeModule;
};

struct TraitDef {
    std::string name;                        // "Number", "Comparable", user-defined names too
    std::vector<Signature> requiredMethods;  // e.g. Comparable requires `compare : This -> Comparison`
    // Names of methods the trait DEFINES a default body for. They have no
    // signature of their own and are not registered per implementing type, so
    // without this list `bot.shout` (an inherited default) is indistinguishable
    // from a call to a method that does not exist.
    std::vector<std::string> defaultMethods;
};

// Open, name-keyed registry of traits and which types implement them.
// Irreducible structural traits (Number, Integer, Float, Equatable,
// Comparable, Showable) are registered here. Package-defined traits arrive
// through ImportedInterfaces or local `trait ... end` declarations.
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

    // Returns the name of the first non-structural trait both types share,
    // or "" if they have no common user-defined trait. Used to widen
    // heterogeneous list elements to their common trait type.
    auto commonTrait(const TypePtr& a, const TypePtr& b) const -> std::string;

    // Does any trait `type` implements declare `method` — as a requirement or
    // as a default body? An inherited default has no per-type signature, so
    // this is the only way to know the call is legitimate.
    auto declaresMethod(const std::string& type, const std::string& method) const
        -> bool;

    // A registry with Number/Integer/Float/Equatable/Comparable/Showable
    // pre-registered, plus implementations for built-in primitive/sized types.
    static auto withBuiltins() -> TraitRegistry;

    // Every registered trait, for whole-registry questions such as "does any
    // trait declare a method by this name?" (see reportUnknownMethods).
    auto all() const -> const std::unordered_map<std::string, TraitDef>& {
        return m_traits;
    }

    auto implementorKey(const TypePtr& type) const -> std::string;
    auto hasConformances(const std::string& key) const -> bool {
        return m_implementations.count(key) > 0;
    }

private:
    std::unordered_map<std::string, TraitDef> m_traits;
    std::unordered_map<std::string, std::set<std::string>> m_implementations;

    auto satisfiesStructurally(const TypePtr& type, const std::string& traitName) const -> bool;
};

} // namespace kex::semantic
