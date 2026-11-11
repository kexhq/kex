#pragma once
// The build as IR levels of the pass infrastructure
// (docs/architecture.md):
//
//   build::Program   the whole build: the entry and every file it loads
//                    (LLVM's Module)
//   build::Unit      one strongly connected group of those files, checked,
//                    lowered and described on its own, in build order,
//                    against the units below it (LLVM's CGSCC)
//   ir::Module       one emitted BEAM module (LLVM's Function)
//
// What a build does is the program pipeline (`kex --print-passes`):
//
//   spec-base,load-dependencies,run-cache,
//   unit(unit-cache,assemble,check,lower,load-order,companion-forwards,
//        receiver-forwards,module(guards,upgrade-points,verify),emit-core,
//        describe,store-unit,return-items),
//   merge-dependencies
//
// The driver around it (compile.cxx) chooses the output, copies the runtime,
// writes `.core`, runs erlc and the program; caches answer through the
// `run-cache` and `unit-cache` passes, which mark what they answered.
#include "dependencies.hxx"
#include "../ast/ast.hxx"
#include "../beam/kexi.hxx"
#include "../beam/kexi_registry.hxx"
#include "../cache/separate_unit.hxx"
#include "../hir/hir.hxx"
#include "../ir/emit_core.hxx"
#include "../ir/lower.hxx"
#include "../pass/pass_registry.hxx"
#include "../semantic/analyzer.hxx"
#include "../semantic/imported_interfaces.hxx"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kex::build {

class Program;

// What a unit compiled says of itself to the units above it.
struct DescribedUnit {
    std::set<std::string> files;
    std::vector<std::string> names;
    beam::LoadedUnit unit;
    // SeparateUnitCache::interfaceDigest of `unit`.
    std::string digest;
};

class Unit {
public:
    Unit(Program& program, std::vector<std::string> files)
        : m_program(&program), m_files(std::move(files)) {}

    auto program() const -> Program& { return *m_program; }
    // The unit's files, in build order; the entry is the one empty path.
    auto files() const -> const std::vector<std::string>& { return m_files; }
    auto isEntry() const -> bool { return m_files.size() == 1 && m_files.front().empty(); }
    // Where its runtime errors point, and what its file module is named after.
    auto sourcePath() const -> const std::string&;

    // The unit's items, taken from their files for the length of its build
    // (`assemble`) and given back after (`return-items`); the prelude trait
    // defaults it claims follow its own items.
    ast::Program ast;
    std::vector<std::pair<driver::LoadedDep*, std::size_t>> taken;
    std::size_t ownItems = 0;
    bool assembled = false;

    // Set by `unit-cache` on a hit: everything below came from the cache.
    bool answered = false;
    // What `check` found, and whether none of it is an error.
    std::vector<semantic::Diagnostic> diagnostics;
    bool checked = false;
    // The modules lowering made (`lower`), the IR passes rewrite, and
    // `emit-core` emits.
    std::vector<ir::Module> modules;
    std::string fileModule;
    bool lowered = false;
    std::vector<ir::EmitResult> emitted;
    // What `describe` (or the cache) said of it, and each emitted module's
    // KexI chunk, in `emitted`'s order.
    std::optional<DescribedUnit> described;
    std::vector<beam::KexiChunk> attachChunks;

private:
    Program* m_program;
    std::vector<std::string> m_files;
};

// What the driver asks of a build.
struct BuildSettings {
    std::string entryPath;
    std::vector<std::string> roots;
    const semantic::ImportedInterfaces* interfaces = nullptr;
    // Whether a type error stops the build: not under `--no-check` or
    // `--emit-core`, which lower every unit whatever the check found.
    bool checkGates = true;
};

class Program {
public:
    Program(ast::Program& entry, BuildSettings settings)
        : m_entry(&entry), m_settings(std::move(settings)) {}
    Program(const Program&) = delete;
    auto operator=(const Program&) -> Program& = delete;

    auto entry() const -> ast::Program& { return *m_entry; }
    auto settings() const -> const BuildSettings& { return m_settings; }

    // `spec-base`: a spec's base file merged into the entry, and its text.
    std::string specBaseFile;
    std::string specBaseRawSource;
    // Whose names the check reports: the entry, and a spec's base.
    auto nameCheckFiles() const -> std::vector<std::string>;

    // `load-dependencies`: every file the entry reaches, and its units in
    // build order (each owned apart: a Unit's address keys its analyses).
    driver::Dependencies dependencies;
    std::vector<std::unique_ptr<Unit>> units;
    auto dependencyFile(const std::string& path) const -> driver::LoadedDep*;

    // The driver's run cache, asked by `run-cache`: whether it answered the
    // whole build.
    std::function<bool(Program&)> answerFromRunCache;
    bool answered = false;

    // The unit cache, when caching is on (`load-dependencies` opens it).
    std::optional<cache::SeparateUnitCache> unitCache;
    // What each unit built so far says of itself, in build order: what the
    // units above it are checked and lowered against.
    std::vector<DescribedUnit> described;

    // What the units produced, gathered in build order once the pipeline ran.
    auto diagnostics() const -> std::vector<semantic::Diagnostic>;
    // Whether every unit lowered: a build stands only if each did.
    auto allLowered() const -> bool;
    // Every emitted module, moved out of its unit, and each one's KexI chunk.
    auto takeModules() -> std::vector<ir::EmitResult>;
    auto chunks() const -> std::map<std::string, beam::KexiChunk>;

private:
    ast::Program* m_entry;
    BuildSettings m_settings;
};

auto unitName(const Program& program) -> std::string;
auto printUnit(std::ostream& out, const Program& program) -> void;
auto isAnswered(const Program& program) -> bool;
auto unitName(const Unit& unit) -> std::string;
auto printUnit(std::ostream& out, const Unit& unit) -> void;
auto isAnswered(const Unit& unit) -> bool;

using ProgramAnalysisManager = pass::AnalysisManager<Program>;
using UnitAnalysisManager = pass::AnalysisManager<Unit>;
using ProgramPassManager = pass::PassManager<Program>;
using UnitPassManager = pass::PassManager<Unit>;

} // namespace kex::build

namespace kex::pass {
template <>
struct IRUnitLevel<build::Program> {
    static constexpr std::string_view kName = "program";
};
template <>
struct IRUnitLevel<build::Unit> {
    static constexpr std::string_view kName = "unit";
};
template <>
struct IRUnitChildren<build::Program, build::Unit> {
    template <typename Visit>
    static auto forEach(build::Program& program, Visit&& visit) -> void {
        for (auto& unit : program.units) visit(*unit);
    }
};
template <>
struct IRUnitChildren<build::Unit, ir::Module> {
    // The unit's analyses are of its source program; its modules are its
    // output, read by none of them.
    static constexpr bool kOuterAnalysesReadChildren = false;
    template <typename Visit>
    static auto forEach(build::Unit& unit, Visit&& visit) -> void {
        for (auto& module : unit.modules) visit(module);
    }
};
} // namespace kex::pass

namespace kex::build {

// ── Unit analyses ───────────────────────────────────────────────────────────

// The files a unit's files use, transitively: what it is checked and lowered
// against, and nothing else.
struct ReachAnalysis : pass::AnalysisInfoMixin<ReachAnalysis> {
    static constexpr std::string_view kName = "reach";
    using Result = std::set<std::string>;
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> Result;
};

// The units below this one that it reaches, as a KexI registry (their
// modules one package of receiver providers), with their interface digests.
struct BelowAnalysis : pass::AnalysisInfoMixin<BelowAnalysis> {
    static constexpr std::string_view kName = "below";
    using Dependencies = std::tuple<ReachAnalysis>;
    struct Result {
        beam::KexiRegistry registry;
        std::vector<std::string> digests;
    };
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> Result;
};

// The interfaces a unit is checked against: the prelude's, less what this
// build compiles itself, merged with the units' below it.
struct AvailableInterfacesAnalysis : pass::AnalysisInfoMixin<AvailableInterfacesAnalysis> {
    static constexpr std::string_view kName = "available-interfaces";
    using Dependencies = std::tuple<BelowAnalysis>;
    using Result = semantic::ImportedInterfaces;
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> Result;
};

// What lowering knows of the units below: their functions, record layouts
// and variant tags, beside the prelude's.
struct LinkAnalysis : pass::AnalysisInfoMixin<LinkAnalysis> {
    static constexpr std::string_view kName = "link";
    using Dependencies = std::tuple<BelowAnalysis>;
    struct Result {
        ir::ExternalModules externals;
        std::vector<ir::ExternalRecordLayout> layouts;
        std::vector<ir::ExternalVariantTag> variants;
    };
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> Result;
};

// The unit cache's key for a unit, if it may be cached: its sources, the
// toolchain, the runtime, and the interfaces below it.
struct UnitCacheKeyAnalysis : pass::AnalysisInfoMixin<UnitCacheKeyAnalysis> {
    static constexpr std::string_view kName = "unit-cache-key";
    using Dependencies = std::tuple<BelowAnalysis>;
    using Result = std::optional<std::string>;
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> Result;
};

// The checker over the assembled unit: its diagnostics, and the HIR, the
// checker's answers, that lowering and the description read.
struct HirAnalysis : pass::AnalysisInfoMixin<HirAnalysis> {
    static constexpr std::string_view kName = "hir";
    using Dependencies = std::tuple<AvailableInterfacesAnalysis, BelowAnalysis>;
    struct Result {
        std::unique_ptr<semantic::Analyzer> analyzer;
        std::vector<semantic::Diagnostic> diagnostics;
        bool clean = false;
        std::unique_ptr<hir::Program> hir;
    };
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> Result;
};

// ── Unit passes ─────────────────────────────────────────────────────────────
// A required pass runs even on a unit a cache answered.

struct UnitCachePass : pass::PassInfoMixin<UnitCachePass> {
    static constexpr std::string_view kName = "unit-cache";
    static constexpr std::string_view kDescription = "let the unit cache answer this unit";
    static constexpr bool kRequired = true;
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct AssemblePass : pass::PassInfoMixin<AssemblePass> {
    static constexpr std::string_view kName = "assemble";
    static constexpr std::string_view kDescription =
        "take the unit's items from its files; add the trait defaults it claims";
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct CheckPass : pass::PassInfoMixin<CheckPass> {
    static constexpr std::string_view kName = "check";
    static constexpr std::string_view kDescription =
        "type-check the unit (hir); record its diagnostics";
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct LowerPass : pass::PassInfoMixin<LowerPass> {
    static constexpr std::string_view kName = "lower";
    static constexpr std::string_view kDescription = "lower the checked unit to IR modules";
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct LoadOrderPass : pass::PassInfoMixin<LoadOrderPass> {
    static constexpr std::string_view kName = "load-order";
    static constexpr std::string_view kDescription =
        "load the file module first, where it registers record layouts";
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct CompanionForwardsPass : pass::PassInfoMixin<CompanionForwardsPass> {
    static constexpr std::string_view kName = "companion-forwards";
    static constexpr std::string_view kDescription =
        "forward what a companion module advertises to the file module";
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct ReceiverForwardsPass : pass::PassInfoMixin<ReceiverForwardsPass> {
    static constexpr std::string_view kName = "receiver-forwards";
    static constexpr std::string_view kDescription =
        "give a receiver method its advertised plain entry point";
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct EmitCorePass : pass::PassInfoMixin<EmitCorePass> {
    static constexpr std::string_view kName = "emit-core";
    static constexpr std::string_view kDescription = "emit each module as Core Erlang";
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct DescribePass : pass::PassInfoMixin<DescribePass> {
    static constexpr std::string_view kName = "describe";
    static constexpr std::string_view kDescription =
        "describe the unit's modules (KexI) for the units above and for .beam";
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct StoreUnitPass : pass::PassInfoMixin<StoreUnitPass> {
    static constexpr std::string_view kName = "store-unit";
    static constexpr std::string_view kDescription = "store a clean unit in the unit cache";
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct ReturnItemsPass : pass::PassInfoMixin<ReturnItemsPass> {
    static constexpr std::string_view kName = "return-items";
    static constexpr std::string_view kDescription = "give the unit's items back to their files";
    static constexpr bool kRequired = true;
    auto run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

// ── Program passes ──────────────────────────────────────────────────────────

struct SpecBasePass : pass::PassInfoMixin<SpecBasePass> {
    static constexpr std::string_view kName = "spec-base";
    static constexpr std::string_view kDescription = "merge a spec's base file into the entry";
    auto run(Program& program, ProgramAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct LoadDependenciesPass : pass::PassInfoMixin<LoadDependenciesPass> {
    static constexpr std::string_view kName = "load-dependencies";
    static constexpr std::string_view kDescription =
        "load every module the entry reaches; group the files into units";
    auto run(Program& program, ProgramAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct RunCachePass : pass::PassInfoMixin<RunCachePass> {
    static constexpr std::string_view kName = "run-cache";
    static constexpr std::string_view kDescription = "let the run cache answer the whole build";
    static constexpr bool kRequired = true;
    auto run(Program& program, ProgramAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

struct MergeDependenciesPass : pass::PassInfoMixin<MergeDependenciesPass> {
    static constexpr std::string_view kName = "merge-dependencies";
    static constexpr std::string_view kDescription =
        "merge the loaded modules into the entry; refuse a misplaced module";
    auto run(Program& program, ProgramAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

// Every pass of each level, by name; the unit registry nests the module
// level and the program registry the unit level.
auto unitPassRegistry() -> const pass::PassRegistry<Unit>&;
auto programPassRegistry() -> const pass::PassRegistry<Program>&;
auto defaultProgramPipeline() -> std::string;
// The program pipeline to run: the user's (`--passes`), or the default.
auto buildProgramPipeline() -> ProgramPassManager;
// Every pass of every level, with what it does, and the default pipeline.
auto printPasses(std::ostream& out) -> void;

// Runs `pipeline` over `program` with every analysis registered, reporting
// to `callbacks`. False when a pass failed, which it has reported.
auto runBuild(Program& program, ProgramPassManager& pipeline,
              const pass::PassInstrumentationCallbacks& callbacks) -> bool;

// The providers a unit of this build lowers against: the prebuilt prelude's
// and those of the units below it.
auto unitExternalModules(const beam::KexiRegistry& registry) -> ir::ExternalModules;

} // namespace kex::build
