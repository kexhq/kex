#include "standard_instrumentation.hxx"

#include <algorithm>
#include <iomanip>

namespace kex::pass {

auto TimePasses::add(const std::string& key, Clock::duration spent) -> void {
    auto found = std::find_if(
        m_spent.begin(), m_spent.end(), [&](const auto& entry) { return entry.first == key; });
    if (found == m_spent.end()) {
        m_spent.emplace_back(key, Spent{});
        found = std::prev(m_spent.end());
    }
    found->second.total += spent;
    ++found->second.runs;
}

// Ends the innermost running pass or analysis: its whole time, and the part
// spent in analyses inside it.
auto TimePasses::finish() -> std::pair<Clock::duration, Clock::duration> {
    const auto frame = m_running.back();
    m_running.pop_back();
    return {Clock::now() - frame.started, frame.nested};
}

auto TimePasses::registerCallbacks(PassInstrumentationCallbacks& callbacks) -> void {
    callbacks.registerBeforePass(
        [this](std::string_view, const UnitRef&) { m_running.push_back({Clock::now()}); });
    callbacks.registerAfterPass([this](std::string_view pass, const UnitRef& unit) {
        if (m_running.empty()) {
            return;
        }
        const auto [spent, _] = finish();
        add(std::string(unit.level) + " " + std::string(pass), spent);
        if (m_perUnit && unit.level == "unit") {
            auto found = std::find_if(m_units.begin(), m_units.end(), [&](const auto& entry) {
                return entry.first == unit.name;
            });
            if (found == m_units.end()) {
                m_units.emplace_back(unit.name, Clock::duration{});
                found = std::prev(m_units.end());
            }
            found->second += spent;
        }
    });
    callbacks.registerBeforeAnalysis(
        [this](std::string_view, const UnitRef&) { m_running.push_back({Clock::now()}); });
    callbacks.registerAfterAnalysis([this](std::string_view analysis, const UnitRef& unit) {
        if (m_running.empty()) {
            return;
        }
        const auto [spent, nested] = finish();
        add(std::string(unit.level) + " analysis " + std::string(analysis), spent - nested);
        if (!m_running.empty()) {
            m_running.back().nested += spent;
        }
    });
}

auto TimePasses::report(std::ostream& out) const -> void {
    const auto ms = [](Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    for (const auto& [key, spent] : m_spent) {
        out << "    pass " << std::left << std::setw(40) << key << std::right << std::setw(9)
            << std::fixed << std::setprecision(1) << ms(spent.total) << " ms  (" << spent.runs
            << ")\n";
    }
    if (m_units.empty()) {
        return;
    }
    auto units = m_units;
    std::sort(units.begin(), units.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    out << "  costliest units (" << units.size() << " in all):\n";
    for (std::size_t i = 0; i < units.size() && i < 20; ++i) {
        out << "    " << std::right << std::setw(9) << std::fixed << std::setprecision(1)
            << ms(units[i].second) << " ms  " << units[i].first << "\n";
    }
}

auto PrintAfter::registerCallbacks(PassInstrumentationCallbacks& callbacks) -> void {
    callbacks.registerAfterPass([this](std::string_view pass, const UnitRef& unit) {
        if (!m_all && !m_passes.contains(std::string(pass))) {
            return;
        }
        *m_out << "; *** " << unit.level << " " << unit.name << " after " << pass << " ***\n";
        unit.print(*m_out);
        *m_out << "\n";
    });
}

} // namespace kex::pass
