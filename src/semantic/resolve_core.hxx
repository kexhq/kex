#pragma once

#include "imported_interfaces.hxx"
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

// Shared name-resolution rules (Phase 2). ResolvePass consumes these
// first; the typechecker and lowerer migrate onto them in later steps.
// Each rule is pure over caller-provided tables: enumeration and
// diagnostics stay with the consumer, whose sinks differ too much to
// share (the checker defers `using` selections without enumerating,
// the lowerer fills module tables). What is shared is the DECISION:
// which module a path names, which import origin wins a name.
namespace resolve_core {

// `receiverMethod` marks an origin that came from a `make T do` block.
// Such a name is reached through a typed receiver (`bag.get(k)`), never
// as a bare call, so it does not compete for the bare name and two
// modules that each spell it differently are not ambiguous.
struct ImportOrigin {
    std::string module;
    bool explicitImport = false;
    bool receiverMethod = false;
};
using ImportScope = std::unordered_map<std::string, ImportOrigin>;

// The scope rule under it: `requested` as written when `exists` says so,
// else relative to the innermost enclosing module that has it, else
// unchanged.
auto resolveModulePathIn(const std::string& currentModule,
                         const std::string& requested,
                         const std::function<bool(const std::string&)>& exists)
    -> std::string;

// Merge one imported name into the scope stack per the `using` rules,
// creating the current scope when the stack is empty. A same-module
// re-import changes nothing; across modules, a bare name beats a
// receiver method, an explicit (`only:`) import beats a whole-module
// one, and two bare claims are ambiguous. `proceed` false means the
// caller binds nothing locally (a standing explicit import keeps the
// name, or the ambiguity error below fires instead).
struct MergeOutcome {
    bool proceed = true;
    std::optional<std::string> ambiguity;
};
auto mergeImportOrigin(std::vector<ImportScope>& scopes,
                       const std::string& name, ImportOrigin origin)
    -> MergeOutcome;

// `using M[, as: A][, only: ...]` imports M's names — except when it
// only binds an alias. `as:` alone imports nothing; an `only:` list
// beside it still asks (ResolvePass, the checker, and the lowerer
// agree on this shape; each enumerates its own tables).
auto usingImportsNames(const std::optional<std::string>& alias,
                       const std::vector<std::string>& only) -> bool;

// Per-name half of the same rule: an `only:` list picks, an `except:`
// list removes. Both lists naming one name is rejected upstream as
// mutually exclusive; here `except:` is checked last, so it wins ties.
auto usingSelectsName(const std::string& name,
                      const std::vector<std::string>& only,
                      const std::vector<std::string>& except) -> bool;

// Innermost-first `scope.name` walk over the enclosing modules of
// `currentModule`: the full path first, then stripping one segment at a
// time. Returns the first candidate `exists` accepts, or nullopt. The
// record-name chains (table E) and the checker's nested-module walk
// share this; what "exists" means stays per consumer.
template <typename Exists>
auto resolveInScopes(const std::string& currentModule,
                     const std::string& name, Exists exists)
    -> std::optional<std::string> {
    for (auto scope = currentModule; !scope.empty();) {
        const auto candidate = scope + "." + name;
        if (exists(candidate)) return candidate;
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) break;
        scope.resize(dot);
    }
    return std::nullopt;
}

// Suffix-match outcome over a candidate table: exactly one longer name
// ending in the suffix wins; zero matches or several settle nothing —
// the caller decides what each means (fall through, answer the bare
// name, report ambiguity).
enum class SuffixMatchKind { None, Unique, Ambiguous };
struct SuffixMatch {
    SuffixMatchKind kind = SuffixMatchKind::None;
    std::string name;
};
template <typename Map>
auto matchSuffix(const Map& candidates, const std::string& suffix)
    -> SuffixMatch {
    SuffixMatch match;
    for (const auto& [candidate, _] : candidates) {
        if (candidate.size() <= suffix.size() ||
            candidate.compare(candidate.size() - suffix.size(),
                              suffix.size(), suffix) != 0)
            continue;
        if (match.kind == SuffixMatchKind::Unique) {
            match.kind = SuffixMatchKind::Ambiguous;
            match.name.clear();
            break;
        }
        match.kind = SuffixMatchKind::Unique;
        match.name = candidate;
    }
    return match;
}

// Devirtualization spelling (Phase 4). The BEAM emitter mangles a
// colliding make-block method as `name/Type`, but the interface only
// carries the plain dispatcher name. The consumer re-derives the
// mangled spelling when it can PROVE the definition exists from
// interface-visible facts; anything unprovable keeps the dispatcher.
// The interface's receiver types are WRITTEN-derived (`convertTypeExpr`
// in collect_metadata.cxx), so in the common cases the head IS the
// spelling — the predicate's job is excluding every case where the
// written spelling diverges from the emitted one (aliases, opaque
// types, undeclared targets, `''`-emitting forms) or the dispatcher
// would route differently (trait peers, shadowed guards, generic
// fallbacks). Each clause mirrors a documented emitter rule
// (`makeTargetNames`, lowering_make.cxx's `name/Type`-vs-bare-vs-
// signature choice, `owners.size() > 1`, `makeDispatcher` order),
// erring to the dispatcher (`nullopt`).

// An emitted `name/Head` head with its provenance: nominal heads
// (written type names) must still prove they name a declared
// record/ADT/distinct type; kind heads (primitives, List, Map) have
// unanimous written-to-emitted spellings and skip that check.
struct TargetHead {
    std::string spelling;
    bool nominal = false;
};

// The head the emitter used for an interface receiver type, or
// nullopt when the type's spelling is unprovable or the head is cut
// for v1: sized numerics (no BIF guard — devirt would change baseline
// behavior), unit, `Block`/`Optional` nominals (the former emits `''`,
// the latter's generic form decodes indistinguishably from `T?`),
// and every `''`-emitting or non-dispatchable kind (optionals,
// unions, records-structural, tuples, funcs, vars, unknown, void,
// constrained).
auto writtenTargetHead(const TypePtr& type) -> std::optional<TargetHead>;

// Transparent-alias expansion callback: the caller's scope tables own
// aliases (`TypeChecker::expandTypeAliases`), resolve_core stays pure.
using AliasExpander = std::function<TypePtr(const TypePtr&)>;

// Whether a (resolved, alias-expanded) call receiver reaches the
// `name/Head` implementation: exact or last-segment spelling for
// nominals (uniqueness below vetoes suffix collisions), head-only
// for List/Map, record-table membership for structural receivers
// (resolution already picked the winner), nothing else.
auto receiverCoversHead(const TypePtr& receiver, const std::string& head,
                        const ImportedInterfaces& ifaces) -> bool;

// The receiver type (`Head`) whose own implementation an imported receiver
// call is proven to run, or nullopt to keep the dispatcher; the backend names
// that implementation (`name/Head` on BEAM). `winner` is the resolved candidate, `receiverType` the
// resolved static receiver. Clauses: (0) a pure make-block winner with a
// receiver (foul calls take their context through a registry keyed on the
// bare name); (1) a provable head; (1b) a nominal head is a declared
// record/ADT/distinct type (aliases and opaques keep the dispatcher); (0b) no
// gradual receiver; the receiver is of that head; and a value of that type
// reaches its own implementation through the dispatcher, as the winner's
// module recorded when it emitted it (`implementationHead`: the routes
// lowering laid out). (10), no trait dictionary for the receiver, is checked by lowering,
// which owns the computed dictionaries.
auto staticReceiverHead(
    const ImportedFunction& winner, const TypePtr& receiverType,
    const ImportedInterfaces& ifaces, AliasExpander expandAlias)
    -> std::optional<std::string>;

} // namespace resolve_core

} // namespace kex::semantic
