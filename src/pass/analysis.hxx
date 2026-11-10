#pragma once
// Analyses and what a pass preserves of them, after LLVM's new pass manager
// (docs/pass-restructure-plan.md, Stage 7).
//
// An analysis computes a fact about one IR unit (a Program, a Unit, an
// ir::Module) and is asked for by its TYPE:
//
//   struct ReachAnalysis : pass::AnalysisInfoMixin<ReachAnalysis> {
//       static constexpr std::string_view kName = "reach";
//       using Result = std::set<std::string>;
//       auto run(Unit& unit, pass::AnalysisManager<Unit>& am) -> Result;
//   };
//
//   const auto& reached = am.result<ReachAnalysis>(unit);
//
// The manager computes it once per unit and keeps it until a pass reports
// that it did not preserve it. An analysis that reads others names them in
// `Dependencies`, so dropping one drops everything computed from it.
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace kex::pass {

class PassInstrumentationCallbacks;

// An analysis's identity: the address of its own key.
struct AnalysisKey {};

// What a pass left valid. A pass that changes nothing returns `all()`; one
// that rewrites its unit returns `none()`, or names what still holds.
class PreservedAnalyses {
public:
    static auto all() -> PreservedAnalyses {
        PreservedAnalyses preserved;
        preserved.m_all = true;
        return preserved;
    }
    static auto none() -> PreservedAnalyses { return {}; }

    template <typename Analysis>
    auto preserve() -> PreservedAnalyses& {
        m_preserved.insert(Analysis::key());
        return *this;
    }
    auto preserve(const AnalysisKey* key) -> PreservedAnalyses& {
        m_preserved.insert(key);
        return *this;
    }
    template <typename Analysis>
    auto preserved() const -> bool { return preserved(Analysis::key()); }
    auto preserved(const AnalysisKey* key) const -> bool {
        return m_all || m_preserved.contains(key);
    }
    auto allPreserved() const -> bool { return m_all; }

    // What both left valid: a pipeline preserves what every pass in it did.
    auto intersect(const PreservedAnalyses& other) -> void {
        if (other.m_all) return;
        if (m_all) {
            *this = other;
            return;
        }
        std::erase_if(m_preserved, [&](const AnalysisKey* key) { return !other.preserved(key); });
    }

private:
    bool m_all = false;
    std::set<const AnalysisKey*> m_preserved;
};

// The base every analysis derives from (CRTP): its key, and its name.
// `Derived` declares `kName`, `Result` and `run`, and may declare
// `using Dependencies = std::tuple<OtherAnalysis...>`.
template <typename Derived>
struct AnalysisInfoMixin {
    static auto key() -> const AnalysisKey* {
        static const AnalysisKey identity{};
        return &identity;
    }
    static auto name() -> std::string_view { return Derived::kName; }
};

namespace detail {

template <typename Analysis, typename = void>
struct DependenciesOf {
    using Type = std::tuple<>;
};
template <typename Analysis>
struct DependenciesOf<Analysis, std::void_t<typename Analysis::Dependencies>> {
    using Type = typename Analysis::Dependencies;
};

template <typename Tuple>
struct KeysOf;
template <typename... Analyses>
struct KeysOf<std::tuple<Analyses...>> {
    static auto keys() -> std::vector<const AnalysisKey*> { return {Analyses::key()...}; }
};

} // namespace detail

// The analyses of one IR level, and their results per unit.
template <typename IRUnit>
class AnalysisManager {
public:
    explicit AnalysisManager(const PassInstrumentationCallbacks* callbacks = nullptr)
        : m_callbacks(callbacks) {}
    AnalysisManager(const AnalysisManager&) = delete;
    auto operator=(const AnalysisManager&) -> AnalysisManager& = delete;

    // Makes `Analysis` available, built by `make` whenever a result is asked
    // for. Registering one twice keeps the first, as LLVM does.
    template <typename Analysis>
    auto registerAnalysis(std::function<Analysis()> make) -> bool {
        const auto [_, added] = m_analyses.try_emplace(
            Analysis::key(), std::make_unique<AnalysisModel<Analysis>>(std::move(make)));
        return added;
    }
    template <typename Analysis>
    auto registerAnalysis() -> bool {
        return registerAnalysis<Analysis>([] { return Analysis{}; });
    }
    template <typename Analysis>
    auto registered() const -> bool { return m_analyses.contains(Analysis::key()); }

    // `Analysis`'s result for `unit`, computed now unless it is still valid.
    template <typename Analysis>
    auto result(IRUnit& unit) -> typename Analysis::Result& {
        const auto entry = std::pair{Analysis::key(), &unit};
        if (const auto found = m_results.find(entry); found != m_results.end())
            return static_cast<ResultModel<Analysis>&>(*found->second).value;
        const auto registered = m_analyses.find(Analysis::key());
        if (registered == m_analyses.end())
            throw std::logic_error("analysis not registered: " + std::string(Analysis::name()));
        auto computed = registered->second->run(unit, *this);
        auto& value = static_cast<ResultModel<Analysis>&>(*computed).value;
        m_results.emplace(entry, std::move(computed));
        return value;
    }

    // `Analysis`'s result for `unit` if one is still valid; never computes.
    template <typename Analysis>
    auto cachedResult(const IRUnit& unit) const -> const typename Analysis::Result* {
        const auto found = m_results.find(std::pair{Analysis::key(), &unit});
        if (found == m_results.end()) return nullptr;
        return &static_cast<const ResultModel<Analysis>&>(*found->second).value;
    }

    // Drops every result for `unit` that `preserved` does not keep, and every
    // result computed from one it drops.
    auto invalidate(const IRUnit& unit, const PreservedAnalyses& preserved) -> void {
        if (preserved.allPreserved()) return;
        std::set<const AnalysisKey*> dropped;
        for (const auto& [entry, _] : m_results)
            if (entry.second == &unit && !preserved.preserved(entry.first))
                dropped.insert(entry.first);
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& [entry, _] : m_results) {
                if (entry.second != &unit || dropped.contains(entry.first)) continue;
                for (const auto* dependency : m_analyses.at(entry.first)->dependencies())
                    if (dropped.contains(dependency)) {
                        dropped.insert(entry.first);
                        grew = true;
                        break;
                    }
            }
        }
        std::erase_if(m_results, [&](const auto& result) {
            return result.first.second == &unit && dropped.contains(result.first.first);
        });
    }
    // Forgets `unit`: it is gone, or done with.
    auto clear(const IRUnit& unit) -> void {
        std::erase_if(m_results, [&](const auto& result) { return result.first.second == &unit; });
    }
    auto clear() -> void { m_results.clear(); }

    auto callbacks() const -> const PassInstrumentationCallbacks* { return m_callbacks; }

private:
    struct ResultConcept {
        virtual ~ResultConcept() = default;
    };
    template <typename Analysis>
    struct ResultModel : ResultConcept {
        explicit ResultModel(typename Analysis::Result result) : value(std::move(result)) {}
        typename Analysis::Result value;
    };
    struct AnalysisConcept {
        virtual ~AnalysisConcept() = default;
        virtual auto run(IRUnit& unit, AnalysisManager& manager)
            -> std::unique_ptr<ResultConcept> = 0;
        virtual auto dependencies() const -> const std::vector<const AnalysisKey*>& = 0;
    };
    template <typename Analysis>
    struct AnalysisModel : AnalysisConcept {
        explicit AnalysisModel(std::function<Analysis()> make) : m_make(std::move(make)) {}
        auto run(IRUnit& unit, AnalysisManager& manager)
            -> std::unique_ptr<ResultConcept> override {
            auto analysis = m_make();
            return std::make_unique<ResultModel<Analysis>>(analysis.run(unit, manager));
        }
        auto dependencies() const -> const std::vector<const AnalysisKey*>& override {
            static const auto keys =
                detail::KeysOf<typename detail::DependenciesOf<Analysis>::Type>::keys();
            return keys;
        }
        std::function<Analysis()> m_make;
    };

    const PassInstrumentationCallbacks* m_callbacks;
    std::map<const AnalysisKey*, std::unique_ptr<AnalysisConcept>> m_analyses;
    std::map<std::pair<const AnalysisKey*, const IRUnit*>, std::unique_ptr<ResultConcept>>
        m_results;
};

} // namespace kex::pass
