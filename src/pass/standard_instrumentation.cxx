#include "standard_instrumentation.hxx"

#include <algorithm>
#include <iomanip>

namespace kex::pass {

auto TimePasses::registerCallbacks(PassInstrumentationCallbacks& callbacks) -> void {
    callbacks.registerBeforePass([this](std::string_view, const UnitRef&) {
        m_started.push_back(Clock::now());
    });
    callbacks.registerAfterPass([this](std::string_view pass, const UnitRef& unit) {
        if (m_started.empty()) return;
        const auto spent = Clock::now() - m_started.back();
        m_started.pop_back();
        const auto key = std::string(unit.level) + " " + std::string(pass);
        auto found = std::find_if(m_spent.begin(), m_spent.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found == m_spent.end()) {
            m_spent.emplace_back(key, Spent{});
            found = std::prev(m_spent.end());
        }
        found->second.total += spent;
        ++found->second.runs;
    });
}

auto TimePasses::report(std::ostream& out) const -> void {
    for (const auto& [key, spent] : m_spent) {
        const auto ms = std::chrono::duration<double, std::milli>(spent.total).count();
        out << "    pass " << std::left << std::setw(30) << key << std::right << std::setw(9)
            << std::fixed << std::setprecision(1) << ms << " ms  (" << spent.runs << ")\n";
    }
}

auto PrintAfter::registerCallbacks(PassInstrumentationCallbacks& callbacks) -> void {
    callbacks.registerAfterPass([this](std::string_view pass, const UnitRef& unit) {
        if (!m_all && !m_passes.contains(std::string(pass))) return;
        *m_out << "; *** " << unit.level << " " << unit.name << " after " << pass << " ***\n";
        unit.print(*m_out);
        *m_out << "\n";
    });
}

} // namespace kex::pass
