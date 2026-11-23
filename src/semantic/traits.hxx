#pragma once

#include "types.hxx"

namespace kex::ast {
struct Expr;
struct FunctionClause;
} // namespace kex::ast
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

// A named function/method signature — used for trait required methods
// (TraitDef::requiredMethods) and imported interface signatures.
// Bounds on a generic type's arguments, by position: `make A is Discrete =>
// Range<A>` (#651) is {{0, "Discrete"}}.
using TypeArgBounds = std::vector<std::pair<std::size_t, std::string>>;

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
    // module member (spec/module_scoped_make.kex), so a call site
    // may only see it with a matching `using` in scope.
    std::string makeModule;
    // The module whose body declares this free function, "" for a top-level
    // one or one known only through an imported interface. The bare-name
    // table holds every module's functions, so a plain call keeps only the
    // ones its module may see (kexhq/kex#496, #494).
    std::string declaringModule;
    // The capabilities a call to it needs, for one known only through an
    // imported interface (KexI). A function of this program has its
    // requirements computed from its body instead
    // (Analyzer::computeCapabilityRequirements).
    std::vector<std::string> requiredCapabilities;
    // For a function of a bounded `make` (`make A is Discrete => Range<A>`,
    // #651): each receiver type argument, by position, and the trait it must
    // satisfy for the function to apply.
    TypeArgBounds receiverBounds;
    // The letters a call must pass the types of (§12 step 7: `A` of `total(A
    // is Monoid => …)` when its body says `A.identity`), for a function known
    // only through its interface. Passed after its other arguments.
    std::vector<std::string> typeParameters;
    // The clause it was made from, for a signature of a FunctionDef: lowering
    // places a call's arguments into it (hir::Program::selectedSignatureOf).
    const ast::FunctionClause* clause = nullptr;
};

// Decision 4 of docs/typechecker-hm-plan.md: a bare `Number` written as the
// result is the type the one parameter written as `Number` received.
// `twice(x: Number) -> Number` on an Integer answers an Integer, and on
// `make [Number]`, `sum :> Number` answers the element's type. Ties the
// parameter's and the result's `Number` into one per-call generic (below
// every generic the signature already uses), which call sites bind as they
// bind any. With no such parameter, or several, it leaves the signature as it
// is: with several, what the result is remains open (the plan's §14).
//
// `resolveType` sees through inference variables (identity for a signature
// read from an interface). `justified` is asked with the parameter's
// occurrence before tying: a checked body must answer that type, while a
// declared or imported signature is tied unconditionally, its body being
// checked against the tie. The single rule, for the checker's own signatures
// and for those an interface brings in, so the two agree.
auto tieTraitResult(
    Signature& signature,
    const std::function<TypePtr(const TypePtr&)>& resolveType,
    const std::function<bool(const Type* occurrence)>& justified) -> void;

// By call: each argument position given for a `Showable` parameter with a
// value of a distinct type, and that type (TypeChecker::shownThroughDistinct).
using ShownThrough =
    std::unordered_map<const ast::Expr*, std::vector<std::pair<std::size_t, std::string>>>;

// Do two signatures take the same parameters? One declaration registered
// twice must read as one: a parameter declared `Any` is Unknown, which
// `typesEqual` never equates, so it counts as equal to another Unknown here.
auto sameParameters(const Signature& left, const Signature& right) -> bool;

struct TraitDef {
    std::string name; // "Number", "Comparable", user-defined names too
    std::vector<Signature>
        requiredMethods; // e.g. Comparable requires `compare : This -> Comparison`
    // Names of methods the trait DEFINES a default body for. They have no
    // signature of their own and are not registered per implementing type, so
    // without this list `bot.shout` (an inherited default) is indistinguishable
    // from a call to a method that does not exist.
    std::vector<std::string> defaultMethods;
    // The defaults the trait's BODY defines, a subset of `defaultMethods`:
    // emitted with each implementor (KexiTraitDef::defaultBodies). The rest
    // are functions over the trait, implemented once where they are declared.
    std::vector<std::string> defaultBodies;
    // How many holes each type parameter writes: `Functor<F<_>>` is {1}, the
    // kind of what implements it (docs/typechecker-hm-plan.md §8.2). Last, so
    // the aggregate initializers above keep their meaning.
    std::vector<std::size_t> paramKinds;
};

// Open, name-keyed registry of traits and which types implement them.
// Irreducible structural traits (Number, Integer, Float, Equatable,
// Comparable, Showable) are registered here. Package-defined traits arrive
// through ImportedInterfaces or local `trait ... end` declarations.
class TraitRegistry {
public:
    auto define(TraitDef def) -> void;
    auto get(const std::string& name) const -> const TraitDef*;
    // A trait as an `implement:` list writes it, without the scope to
    // resolve it properly (see kex::resolveTraitName): its identity, or the
    // end of exactly one (`Pricing` for `Shop.Rules.Pricing`).
    auto lookup(const std::string& written) const -> const TraitDef*;

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
    // `typeName` implements `traitName` where its arguments satisfy `bounds`.
    auto registerConditionalImplementation(
        const std::string& typeName, const std::string& traitName, TypeArgBounds bounds) -> void;

    // Does any trait `type` implements declare `method` — as a requirement or
    // as a default body? An inherited default has no per-type signature, so
    // this is the only way to know the call is legitimate.
    // Whether `type`'s own module implements `method`: a method a trait it
    // claims requires, or a default from that trait's body.
    auto implementsInHome(const std::string& type, const std::string& method) const -> bool;
    auto declaresMethod(const std::string& type, const std::string& method) const -> bool;
    // The method a trait `type` claims requires under `method`'s name, and
    // that trait; nulls when no claimed trait requires one.
    auto requiredMethod(const TypePtr& type, const std::string& method) const
        -> std::pair<const TraitDef*, const Signature*>;
    // Every method the traits `type` claims require under `method`'s name:
    // a capability can require same-arity overloads (`take(text: String)`
    // beside `take(count: Integer)`), and each implementation answers one.
    auto requiredOverloads(const TypePtr& type, const std::string& method) const
        -> std::vector<const Signature*>;

    // A registry with Number/Integer/Float/Equatable/Comparable/Showable
    // pre-registered, plus implementations for built-in primitive/sized types.
    static auto withBuiltins() -> TraitRegistry;

    // Every registered trait, for whole-registry questions such as "does any
    // trait declare a method by this name?" (see reportUnknownMethods).
    auto all() const -> const std::unordered_map<std::string, TraitDef>& {
        return m_traits;
    }

    auto implementorKey(const TypePtr& type) const -> std::string;
    // The name a type's family registers conformances under (`List`, `Float`
    // for a sized float, ...), or "".
    static auto familyKey(const TypePtr& type) -> std::string;
    auto hasConformances(const std::string& key) const -> bool {
        return m_implementations.count(key) > 0;
    }

private:
    std::unordered_map<std::string, TraitDef> m_traits;
    std::unordered_map<std::string, std::set<std::string>> m_implementations;
    // typeName -> trait -> each conditional claim's bounds.
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<TypeArgBounds>>>
        m_conditionalImplementations;

    auto satisfiesStructurally(const TypePtr& type, const std::string& traitName) const -> bool;
    // Whether each argument `bounds` names satisfies its trait; an argument
    // not known yet is not judged against.
    auto boundsHold(const NamedType& type, const TypeArgBounds& bounds) const -> bool;
};

} // namespace kex::semantic
