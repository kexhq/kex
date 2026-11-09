#pragma once

// The modules a program uses, found, loaded and merged into it: what every
// build and every walker run does before checking (docs/pass-restructure-plan.md,
// Stage 4). One implementation, for `kex -c`, `kex -R`, the REPLs and anything
// embedding the walker.

#include "../ast/ast.hxx"
#include "../compiled/expand.hxx"
#include "../hir/compile_time.hxx"
#include "../semantic/imported_interfaces.hxx"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace kex::driver {

// A dependency file, parsed and expanded on its own, before its declarations
// are merged into the program.
struct LoadedDep {
    std::unique_ptr<std::string> source;
    std::unique_ptr<std::string> path;
    std::unique_ptr<ast::Program> program;
};

// Whether a dependency that cannot be loaded ends the process (a build) or is
// thrown as an error (a REPL or an embedder, which catch it). An interactive
// session used to die with the build's `exit(1)` the moment a `/load`-ed file
// used a module with a syntax error.
extern bool g_dependencyFailureThrows;
[[noreturn]] auto dependencyFailure(const std::string& what) -> void;
// An error about a module's file, at its header.
auto reportModuleFileError(const std::filesystem::path& file, int line,
                           const std::string& message) -> void;

// Where an expansion keeps template scans between builds, and for which
// toolchain (kexhq/kex#323); left empty when every cache is off.
auto setScanCache(compiled::ExpandOptions& options) -> void;
// The same for every expansion that builds its own options: a module the
// semantic pass or a compile-time sandbox expands.
auto setDefaultScanCache() -> void;

// What a long build is doing, as it does it (kexhq/kex#586): one stderr line
// per step, written before the step starts.
struct BuildProgress {
    using Clock = std::chrono::steady_clock;
    bool enabled = false;
    Clock::time_point start = Clock::now();
    // A dependency is resolved by the gating pre-check and again for the real
    // build; the second pass is quick and would only repeat the first's lines.
    std::unordered_set<std::string> expanded;

    auto elapsed() const -> double;
    auto step(const char* verb, const char* colour, const std::string& what) const -> void;
    // Once per dependency file, however many passes resolve it.
    auto expanding(const std::string& path) -> void;
    auto finished(const std::string& what) const -> void;
};
extern BuildProgress buildProgress;
// "1 module", "160 modules": what BuildProgress counts.
auto moduleCount(std::size_t count, const char* kind = "") -> std::string;

// The program as units (docs/plan_separate_compilation.md): its files grouped
// into the strongly connected components of what each one uses, so that a
// cycle of modules is one unit and every other module one of its own. In build
// order, a unit after every unit it uses, with the entry program, written as
// the empty path, last.
struct UnitGraph {
    std::map<std::string, std::set<std::string>> uses;
    std::vector<std::vector<std::string>> units;
};
auto unitsOf(const std::map<std::string, std::set<std::string>>& uses)
    -> std::vector<std::vector<std::string>>;

// Expanded dependency files are kept for one build (`kex -c` / `--run`), see
// the definition.
extern bool g_memoizeDependencyExpansion;

// Whether the prelude imports `module` into every program: its interface is
// already known, so its source is never merged in.
auto isAutomaticImport(const semantic::ImportedInterfaces* interfaces,
                       const std::string& module) -> bool;
// The modules `program` names, through `using` or a qualified reference,
// whose source has to be merged in: all but the automatic imports.
auto sourceModulesNamedBy(const ast::Program& program) -> std::unordered_set<std::string>;

// Puts `items` ahead of `program`'s own, in order: where every merged-in
// declaration goes, so the entry's own items stay last.
auto prependItems(ast::Program& program, std::vector<ast::TopLevelItem> items) -> void;

// Every module `program` uses, transitively, each file parsed and expanded on
// its own, with what uses what and the units that makes.
struct Dependencies {
    std::vector<LoadedDep> files;
    std::map<std::string, std::set<std::string>> uses;
    std::vector<std::vector<std::string>> units;
    // Names a `using` or a qualified reference asked for that no file under
    // the source roots answers to, and the files loaded.
    std::set<std::string> unresolved;
    std::unordered_set<std::string> loaded;
    // KEX_TIMINGS=1 splits dependency loading into its costs (kexhq/kex#323).
    std::chrono::steady_clock::duration parseTime{};
    std::chrono::steady_clock::duration expandTime{};
    std::chrono::steady_clock::duration analyzeTime{};
    std::size_t discoveryHits = 0;
    compiled::SandboxTimings sandboxesBefore{};
};

// `qualifiedModules` is the caller's own fresh discovery result for `program`
// (referencedModules, automatic-import filtered), or nullopt when the caller
// did not analyze: the entry program is then analyzed here instead. A
// PROVIDED set, even an empty one, is authoritative and skips that re-analyze;
// the recursion into dependency files always passes nullopt and uses the
// discovery cache.
auto loadDependencies(const ast::Program& program, const std::vector<std::string>& roots,
                      const semantic::ImportedInterfaces* interfaces,
                      const std::optional<std::unordered_set<std::string>>& qualifiedModules =
                          std::nullopt) -> Dependencies;
// Merges the loaded files' declarations into `program`, ahead of its own, and
// refuses a module declared in a file named for another.
auto mergeDependencies(ast::Program& program, Dependencies& dependencies,
                       const std::vector<std::string>& roots,
                       const semantic::ImportedInterfaces* interfaces) -> void;

// A program the walker runs, checked first, as `kex -R` runs one: the project
// modules it uses merged in, expanded, and checked silently against
// `interfaces`. For a harness or an embedder; a dependency that cannot be
// loaded throws.
auto prepareForWalker(ast::Program& program, const std::vector<std::string>& roots,
                      const semantic::ImportedInterfaces* interfaces,
                      const std::string& sourcePath = {})
    -> std::unique_ptr<hir::CheckedProgram>;

} // namespace kex::driver
