#pragma once
// Pass instrumentation, after LLVM's: callbacks the pass managers call around
// every pass, so timing, printing and skipping are not written into passes.
//
// A callback sees the pass's name and the unit it runs on through `UnitRef`:
// which level it is (`program`, `unit`, `module`), its name, and how to print
// it. An IR level says how through two functions found by argument-dependent
// lookup beside its unit type:
//
//   auto unitName(const Unit& unit) -> std::string;
//   auto printUnit(std::ostream& out, const Unit& unit) -> void;
//
// A level whose units a cache can answer also says which were:
//
//   auto isAnswered(const Unit& unit) -> bool;
//
// An answered unit runs only its required passes, as LLVM skips an
// `optnone` function's optimisations.
#include <functional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kex::pass {

// The level an IR unit is: its name in a textual pipeline.
template <typename IRUnit>
struct IRUnitLevel;

struct UnitRef {
    std::string_view level;
    std::string name;
    bool answered = false;
    std::function<void(std::ostream&)> print;
};

namespace detail {
template <typename IRUnit, typename = void>
struct CanBeAnswered : std::false_type {};
template <typename IRUnit>
struct CanBeAnswered<IRUnit, std::void_t<decltype(isAnswered(std::declval<const IRUnit&>()))>>
    : std::true_type {};
} // namespace detail

template <typename IRUnit>
auto unitRef(const IRUnit& unit) -> UnitRef {
    bool answered = false;
    if constexpr (detail::CanBeAnswered<IRUnit>::value) answered = isAnswered(unit);
    return {IRUnitLevel<IRUnit>::kName, unitName(unit), answered,
            [&unit](std::ostream& out) { printUnit(out, unit); }};
}

class PassInstrumentationCallbacks {
public:
    // Whether the pass runs at all on this unit. A required pass runs anyway.
    using ShouldRun = std::function<bool(std::string_view pass, const UnitRef& unit)>;
    using BeforePass = std::function<void(std::string_view pass, const UnitRef& unit)>;
    using AfterPass = std::function<void(std::string_view pass, const UnitRef& unit)>;

    auto registerShouldRun(ShouldRun callback) -> void {
        m_shouldRun.push_back(std::move(callback));
    }
    auto registerBeforePass(BeforePass callback) -> void {
        m_before.push_back(std::move(callback));
    }
    auto registerAfterPass(AfterPass callback) -> void { m_after.push_back(std::move(callback)); }

    auto shouldRun(std::string_view pass, const UnitRef& unit) const -> bool {
        for (const auto& callback : m_shouldRun)
            if (!callback(pass, unit)) return false;
        return true;
    }
    auto beforePass(std::string_view pass, const UnitRef& unit) const -> void {
        for (const auto& callback : m_before) callback(pass, unit);
    }
    auto afterPass(std::string_view pass, const UnitRef& unit) const -> void {
        for (const auto& callback : m_after) callback(pass, unit);
    }

private:
    std::vector<ShouldRun> m_shouldRun;
    std::vector<BeforePass> m_before;
    std::vector<AfterPass> m_after;
};

// What a pass manager calls around one pass, through its analysis manager's
// callbacks; nothing when there are none.
class PassInstrumentation {
public:
    explicit PassInstrumentation(const PassInstrumentationCallbacks* callbacks)
        : m_callbacks(callbacks) {}

    template <typename IRUnit>
    auto runBeforePass(std::string_view pass, bool required, const IRUnit& unit) const -> bool {
        const auto ref = unitRef(unit);
        if (!required && ref.answered) return false;
        if (!m_callbacks) return true;
        if (!required && !m_callbacks->shouldRun(pass, ref)) return false;
        m_callbacks->beforePass(pass, ref);
        return true;
    }
    template <typename IRUnit>
    auto runAfterPass(std::string_view pass, const IRUnit& unit) const -> void {
        if (m_callbacks) m_callbacks->afterPass(pass, unitRef(unit));
    }

private:
    const PassInstrumentationCallbacks* m_callbacks;
};

} // namespace kex::pass
