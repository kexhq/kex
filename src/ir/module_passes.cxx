#include "module_passes.hxx"

#include "emit_core.hxx"
#include "passes/guards.hxx"
#include "passes/upgrade.hxx"
#include "passes/verify.hxx"
#include "../pass/options.hxx"

#include <stdexcept>

namespace kex::ir {

auto unitName(const Module& module) -> std::string { return module.name; }

auto printUnit(std::ostream& out, const Module& module) -> void {
    out << emitCore(module).source;
}

auto GuardsPass::run(Module& module, ModuleAnalysisManager&) -> pass::PreservedAnalyses {
    expandGuards(module);
    return pass::PreservedAnalyses::none();
}

auto UpgradePointsPass::run(Module& module, ModuleAnalysisManager&) -> pass::PreservedAnalyses {
    insertUpgradePoints(module);
    return pass::PreservedAnalyses::none();
}

auto VerifierPass::run(Module& module, ModuleAnalysisManager&) -> pass::PreservedAnalyses {
    const auto problems = verifyModule(module);
    if (!problems.empty()) {
        std::string message = "IR verification failed:";
        for (const auto& problem : problems) message += "\n  " + problem;
        throw std::runtime_error(message);
    }
    return pass::PreservedAnalyses::all();
}

auto modulePassRegistry() -> const pass::PassRegistry<Module>& {
    static const auto registry = [] {
        pass::PassRegistry<Module> passes;
        passes.registerPass<GuardsPass>();
        passes.registerPass<UpgradePointsPass>();
        passes.registerPass<VerifierPass>();
        return passes;
    }();
    return registry;
}

auto buildModulePipeline() -> ModulePassManager {
    ModulePassManager pipeline;
    const auto text = pass::pipelineText(pass::IRUnitLevel<Module>::kName, kDefaultModulePipeline);
    if (auto problem = modulePassRegistry().build(pipeline, text))
        throw std::invalid_argument(*problem);
    return pipeline;
}

} // namespace kex::ir
