#pragma once

// `KEX_TIMINGS=1`: where a compile spends its time, one stderr line per phase
// (kexhq/kex#323). Every timed site asks `enabled()`, so the switch reads the
// same everywhere: on for any value but empty or `0`.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace kex::timings {

using Clock = std::chrono::steady_clock;

inline auto enabled() -> bool {
    static const bool on = [] {
        const char* value = std::getenv("KEX_TIMINGS");
        return value && *value && std::string_view(value) != "0";
    }();
    return on;
}

inline auto milliseconds(Clock::duration duration) -> double {
    return std::chrono::duration<double, std::milli>(duration).count();
}

// Prints, at each `mark`, how long since the previous one — indented to show
// nesting, and with the running total when `total` is set. Silent unless
// timings are enabled.
class PhaseTimer {
public:
    explicit PhaseTimer(int indent = 2, bool total = true)
        : m_indent(indent), m_total(total) {}

    auto mark(const char* phase) -> void {
        if (!enabled()) return;
        const auto now = Clock::now();
        if (m_total)
            std::fprintf(stderr, "%*s%-26s %8.1f ms  (%8.1f ms total)\n",
                         m_indent, "", phase, milliseconds(now - m_last),
                         milliseconds(now - m_start));
        else
            std::fprintf(stderr, "%*s%-26s %8.1f ms\n", m_indent, "", phase,
                         milliseconds(now - m_last));
        m_last = now;
    }

private:
    int m_indent;
    bool m_total;
    Clock::time_point m_start = Clock::now();
    Clock::time_point m_last = m_start;
};

} // namespace kex::timings
