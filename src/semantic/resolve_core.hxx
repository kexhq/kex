#pragma once

#include "db.hxx"
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

// Resolve a `using` module path against collected source modules: as
// written, then `SemanticDB::ensureModule` discovery, then the
// enclosing-module-relative walk. Unknown paths come back unchanged;
// the caller decides whether that is an error or a warning.
auto resolveSourceModulePath(SemanticDB* db,
                             const std::string& currentModule,
                             const std::string& requested) -> std::string;

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
enum class SuffixMatchKind { none, unique, ambiguous };
struct SuffixMatch {
    SuffixMatchKind kind = SuffixMatchKind::none;
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
        if (match.kind == SuffixMatchKind::unique) {
            match.kind = SuffixMatchKind::ambiguous;
            match.name.clear();
            break;
        }
        match.kind = SuffixMatchKind::unique;
        match.name = candidate;
    }
    return match;
}

} // namespace resolve_core

} // namespace kex::semantic
