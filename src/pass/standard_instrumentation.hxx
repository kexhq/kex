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

// What each pass cost, summed over every unit it ran on, by level, and what
// each analysis cost on its own: an analysis's time leaves out the analyses
// it asked for, which are reported under their own names, so the analysis
// lines add up. A pass's time includes everything it ran, analyses too.
//
// With `perUnit`, the report also lists the units that cost most, summing
// the unit-level passes run on each.
class TimePasses {
public:
    explicit TimePasses(bool perUnit = false) : m_perUnit(perUnit) {}
    auto registerCallbacks(PassInstrumentationCallbacks& callbacks) -> void;
    auto report(std::ostream& out) const -> void;

private:
    using Clock = std::chrono::steady_clock;
    struct Spent {
        Clock::duration total{};
        std::size_t runs = 0;
    };
    // Passes nest (an adaptor runs its pipeline inside its own run), and so do
    // analyses, so the running ones are a stack. `nested` is the time spent in
    // analyses computed inside this one.
    struct Frame {
        Clock::time_point started;
        Clock::duration nested{};
    };
    auto add(const std::string& key, Clock::duration spent) -> void;
    auto finish() -> std::pair<Clock::duration, Clock::duration>;

    bool m_perUnit;
    std::vector<Frame> m_running;
    // In first-run order, so the report reads as the pipeline does.
    std::vector<std::pair<std::string, Spent>> m_spent;
    std::vector<std::pair<std::string, Clock::duration>> m_units;
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
