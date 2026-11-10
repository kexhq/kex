#pragma once
// The instrumentation every pipeline offers, after LLVM's
// StandardInstrumentations: what each pass cost (`--time-passes`, or
// KEX_TIMINGS), and the unit as a pass left it (`--print-after=<pass>`,
// `--print-after-all`).
#include "instrumentation.hxx"

#include <chrono>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kex::pass {

// What each pass cost, summed over every unit it ran on, by level.
class TimePasses {
public:
    auto registerCallbacks(PassInstrumentationCallbacks& callbacks) -> void;
    auto report(std::ostream& out) const -> void;

private:
    using Clock = std::chrono::steady_clock;
    struct Spent {
        Clock::duration total{};
        std::size_t runs = 0;
    };
    // Passes nest (an adaptor runs its pipeline inside its own run), so the
    // start times are a stack.
    std::vector<Clock::time_point> m_started;
    // In first-run order, so the report reads as the pipeline does.
    std::vector<std::pair<std::string, Spent>> m_spent;
};

// Prints the unit each named pass (or every pass) ran on, after it ran.
class PrintAfter {
public:
    PrintAfter(std::ostream& out, std::set<std::string> passes, bool all)
        : m_out(&out), m_passes(std::move(passes)), m_all(all) {}
    auto registerCallbacks(PassInstrumentationCallbacks& callbacks) -> void;

private:
    std::ostream* m_out;
    std::set<std::string> m_passes;
    bool m_all;
};

} // namespace kex::pass
