#pragma once
// The IR level of the pass infrastructure (docs/pass-restructure-plan.md,
// Stage 7): an ir::Module is the unit, as a Function is in LLVM. Every rewrite
// of a lowered module is a pass here, registered by name, and every module is
// emitted through the same pipeline: `guards,upgrade-points,verify` unless
// `kex --passes='...module(...)'` says otherwise.
#include "ir.hxx"
#include "../pass/pass_registry.hxx"

#include <ostream>
#include <string>
#include <string_view>

namespace kex::pass {
template <>
struct IRUnitLevel<ir::Module> {
    static constexpr std::string_view kName = "module";
};
} // namespace kex::pass

namespace kex::ir {

using ModulePassManager = pass::PassManager<Module>;
using ModuleAnalysisManager = pass::AnalysisManager<Module>;

// For instrumentation: a module's name, and the module as Core Erlang.
auto unitName(const Module& module) -> std::string;
auto printUnit(std::ostream& out, const Module& module) -> void;

// `when` guards out of BEAM guard position, before any pass reads the control
// flow they become.
struct GuardsPass : pass::PassInfoMixin<GuardsPass> {
    static constexpr std::string_view kName = "guards";
    static constexpr std::string_view kDescription = "move `when` guards out of BEAM guard position";
    auto run(Module& module, ModuleAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

// Loop lifting, and the hot-code-upgrade points a reloaded module needs.
struct UpgradePointsPass : pass::PassInfoMixin<UpgradePointsPass> {
    static constexpr std::string_view kName = "upgrade-points";
    static constexpr std::string_view kDescription = "lift loops; insert hot-code-upgrade points";
    auto run(Module& module, ModuleAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

// What every pass left must be a well-formed module: a violation stops the
// build, naming each one, instead of reaching erlc. Runs wherever it is
// placed.
struct VerifierPass : pass::PassInfoMixin<VerifierPass> {
    static constexpr std::string_view kName = "verify";
    static constexpr std::string_view kDescription = "check the module is well formed";
    static constexpr bool kRequired = true;
    auto run(Module& module, ModuleAnalysisManager& analyses) -> pass::PreservedAnalyses;
};

// Every module pass, by name.
auto modulePassRegistry() -> const pass::PassRegistry<Module>&;
inline constexpr std::string_view kDefaultModulePipeline = "guards,upgrade-points,verify";
// The module pipeline to run: the user's (`--passes`), or the default. A
// pipeline that does not parse is a usage error, reported as one.
auto buildModulePipeline() -> ModulePassManager;

} // namespace kex::ir
