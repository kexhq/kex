#pragma once
// Running one level's passes over the units inside another, after LLVM's
// ModuleToFunctionPassAdaptor and its analysis manager proxies.
//
// A level says what units it holds by specialising IRUnitChildren:
//
//   template <> struct pass::IRUnitChildren<Program, Unit> {
//       template <typename Visit>
//       static auto forEach(Program& program, Visit&& visit) -> void;
//       // Optional: no outer analysis reads the children, so changing
//       // them invalidates none (default: every one, as LLVM assumes).
//       static constexpr bool kOuterAnalysesReadChildren = false;
//   };
//
// The outer analysis manager reaches the inner one through
// InnerAnalysisManagerProxy; an inner pass reads the outer level's results
// (already computed, never computed from inside) through
// OuterAnalysisManagerProxy.
#include "pass_manager.hxx"

#include <ostream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace kex::pass {

template <typename Outer, typename Inner>
struct IRUnitChildren;

namespace detail {
template <typename Children, typename = void>
struct OuterAnalysesReadChildren : std::true_type {};
template <typename Children>
struct OuterAnalysesReadChildren<Children, std::void_t<decltype(Children::kOuterAnalysesReadChildren)>>
    : std::bool_constant<Children::kOuterAnalysesReadChildren> {};
} // namespace detail

// An outer analysis whose result is the inner level's analysis manager.
template <typename Inner, typename Outer>
struct InnerAnalysisManagerProxy
    : AnalysisInfoMixin<InnerAnalysisManagerProxy<Inner, Outer>> {
    static constexpr std::string_view kName = "inner-analysis-manager-proxy";
    struct Result {
        AnalysisManager<Inner>* manager;
    };
    explicit InnerAnalysisManagerProxy(AnalysisManager<Inner>& manager) : m_manager(&manager) {}
    auto run(Outer&, AnalysisManager<Outer>&) -> Result { return {m_manager}; }
    AnalysisManager<Inner>* m_manager;
};

// An inner analysis whose result reads the outer level's cached results.
template <typename Outer, typename Inner>
struct OuterAnalysisManagerProxy
    : AnalysisInfoMixin<OuterAnalysisManagerProxy<Outer, Inner>> {
    static constexpr std::string_view kName = "outer-analysis-manager-proxy";
    class Result {
    public:
        explicit Result(const AnalysisManager<Outer>* manager) : m_manager(manager) {}
        template <typename Analysis>
        auto cachedResult(const Outer& unit) const -> const typename Analysis::Result* {
            return m_manager->template cachedResult<Analysis>(unit);
        }

    private:
        const AnalysisManager<Outer>* m_manager;
    };
    explicit OuterAnalysisManagerProxy(const AnalysisManager<Outer>& manager)
        : m_manager(&manager) {}
    auto run(Inner&, AnalysisManager<Inner>&) -> Result { return Result(m_manager); }
    const AnalysisManager<Outer>* m_manager;
};

// Registers the proxies both ways between two levels' analysis managers.
template <typename Outer, typename Inner>
auto crossRegisterProxies(AnalysisManager<Outer>& outer, AnalysisManager<Inner>& inner) -> void {
    outer.template registerAnalysis<InnerAnalysisManagerProxy<Inner, Outer>>(
        [&inner] { return InnerAnalysisManagerProxy<Inner, Outer>(inner); });
    inner.template registerAnalysis<OuterAnalysisManagerProxy<Outer, Inner>>(
        [&outer] { return OuterAnalysisManagerProxy<Outer, Inner>(outer); });
}

// Runs an inner pipeline over each inner unit of an outer one, in the order
// the outer level gives them. An inner unit's analyses are forgotten once its
// pipeline is done: what later passes need of it is in the unit itself.
template <typename Outer, typename Inner>
class InnerPassAdaptor : public PassInfoMixin<InnerPassAdaptor<Outer, Inner>> {
public:
    static constexpr std::string_view kName = IRUnitLevel<Inner>::kName;
    static constexpr bool kRequired = true;

    explicit InnerPassAdaptor(PassManager<Inner> pipeline) : m_pipeline(std::move(pipeline)) {}

    auto run(Outer& outer, AnalysisManager<Outer>& analyses) -> PreservedAnalyses {
        auto& inner = *analyses.template result<InnerAnalysisManagerProxy<Inner, Outer>>(outer)
                           .manager;
        // A child changed is its outer unit changed: inner keys say nothing
        // of the outer level's analyses.
        bool unchanged = true;
        IRUnitChildren<Outer, Inner>::forEach(outer, [&](Inner& child) {
            unchanged = m_pipeline.run(child, inner).allPreserved() && unchanged;
            inner.clear(child);
        });
        if (unchanged || !detail::OuterAnalysesReadChildren<IRUnitChildren<Outer, Inner>>::value)
            return PreservedAnalyses::all();
        // The inner manager is still the inner manager.
        return PreservedAnalyses::none().template preserve<InnerAnalysisManagerProxy<Inner, Outer>>();
    }

    auto printPipeline(std::ostream& out) const -> void {
        out << IRUnitLevel<Inner>::kName << "(";
        m_pipeline.printPipeline(out);
        out << ")";
    }

private:
    PassManager<Inner> m_pipeline;
};

} // namespace kex::pass
