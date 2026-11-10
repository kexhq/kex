#pragma once
// Passes and pass managers, after LLVM's new pass manager
// (docs/pass-restructure-plan.md, Stage 7).
//
// A pass is a class over one IR level:
//
//   struct GuardsPass : pass::PassInfoMixin<GuardsPass> {
//       static constexpr std::string_view kName = "guards";
//       auto run(ir::Module& module, pass::AnalysisManager<ir::Module>& am)
//           -> pass::PreservedAnalyses;
//   };
//
// A PassManager runs passes of its level in order, invalidating what each did
// not preserve; it is a pass itself, so pipelines nest. A pass that must run
// even where instrumentation would skip it (a cache lookup, a verifier)
// declares `static constexpr bool kRequired = true`.
#include "analysis.hxx"
#include "instrumentation.hxx"

#include <memory>
#include <ostream>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kex::pass {

template <typename Derived>
struct PassInfoMixin {
    static auto name() -> std::string_view { return Derived::kName; }
};

namespace detail {

template <typename Pass, typename = void>
struct IsRequired : std::false_type {};
template <typename Pass>
struct IsRequired<Pass, std::void_t<decltype(Pass::kRequired)>>
    : std::bool_constant<Pass::kRequired> {};

template <typename Pass, typename = void>
struct HasPrintPipeline : std::false_type {};
template <typename Pass>
struct HasPrintPipeline<
    Pass, std::void_t<decltype(std::declval<const Pass&>().printPipeline(std::declval<std::ostream&>()))>>
    : std::true_type {};

} // namespace detail

// Any pass of one level, behind one interface.
template <typename IRUnit>
struct PassConcept {
    virtual ~PassConcept() = default;
    virtual auto run(IRUnit& unit, AnalysisManager<IRUnit>& analyses) -> PreservedAnalyses = 0;
    virtual auto name() const -> std::string_view = 0;
    virtual auto required() const -> bool = 0;
    // The pass as a textual pipeline names it: `guards`, `module(guards,verify)`.
    virtual auto printPipeline(std::ostream& out) const -> void = 0;
};

template <typename IRUnit, typename Pass>
struct PassModel final : PassConcept<IRUnit> {
    explicit PassModel(Pass pass) : m_pass(std::move(pass)) {}
    auto run(IRUnit& unit, AnalysisManager<IRUnit>& analyses) -> PreservedAnalyses override {
        return m_pass.run(unit, analyses);
    }
    auto name() const -> std::string_view override { return Pass::name(); }
    auto required() const -> bool override { return detail::IsRequired<Pass>::value; }
    auto printPipeline(std::ostream& out) const -> void override {
        if constexpr (detail::HasPrintPipeline<Pass>::value) m_pass.printPipeline(out);
        else out << Pass::name();
    }
    Pass m_pass;
};

template <typename IRUnit>
class PassManager : public PassInfoMixin<PassManager<IRUnit>> {
public:
    static constexpr std::string_view kName = "pass-manager";
    // A nested pipeline always runs; the passes in it decide for themselves.
    static constexpr bool kRequired = true;

    PassManager() = default;
    PassManager(PassManager&&) noexcept = default;
    auto operator=(PassManager&&) noexcept -> PassManager& = default;

    template <typename Pass>
    auto addPass(Pass pass) -> PassManager& {
        m_passes.push_back(std::make_unique<PassModel<IRUnit, Pass>>(std::move(pass)));
        return *this;
    }
    auto addPass(std::unique_ptr<PassConcept<IRUnit>> pass) -> PassManager& {
        m_passes.push_back(std::move(pass));
        return *this;
    }

    auto run(IRUnit& unit, AnalysisManager<IRUnit>& analyses) -> PreservedAnalyses {
        const PassInstrumentation instrumentation(analyses.callbacks());
        auto preserved = PreservedAnalyses::all();
        for (const auto& pass : m_passes) {
            if (!instrumentation.runBeforePass(pass->name(), pass->required(), unit)) continue;
            const auto passPreserved = pass->run(unit, analyses);
            analyses.invalidate(unit, passPreserved);
            instrumentation.runAfterPass(pass->name(), unit);
            preserved.intersect(passPreserved);
        }
        return preserved;
    }

    auto empty() const -> bool { return m_passes.empty(); }
    auto size() const -> std::size_t { return m_passes.size(); }
    auto printPipeline(std::ostream& out) const -> void {
        for (std::size_t i = 0; i < m_passes.size(); ++i) {
            if (i > 0) out << ",";
            m_passes[i]->printPipeline(out);
        }
    }

private:
    std::vector<std::unique_ptr<PassConcept<IRUnit>>> m_passes;
};

} // namespace kex::pass
