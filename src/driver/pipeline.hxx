#pragma once
// A compiler pipeline: an ordered list of named passes over one unit of work.
// The driver commands (`kex -c`, `--emit-core`, ...) are built from these, so
// what a build does is a list that can be read (`kex --print-passes`) and
// timed per pass (KEX_TIMINGS), rather than one long function whose phases are
// only told apart by comments.
#include "../common/timings.hxx"

#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace kex::driver {

// What a pass tells the pipeline: carry on with the next pass, or stop now
// with this process exit status (an error, or the work finishing early, as
// `--emit-core` does after writing the Core Erlang).
using PassOutcome = std::optional<int>;
inline constexpr PassOutcome kContinue = std::nullopt;

template <typename Unit>
struct Pass {
    std::string name;
    // One line on what the pass does, for `--print-passes`.
    std::string summary;
    std::function<PassOutcome(Unit&)> run;
};

template <typename Unit>
class Pipeline {
public:
    explicit Pipeline(std::string name) : m_name(std::move(name)) {}

    auto add(std::string name, std::string summary,
             std::function<PassOutcome(Unit&)> run) -> Pipeline& {
        m_passes.push_back({std::move(name), std::move(summary), std::move(run)});
        return *this;
    }

    // Runs the passes in order; the exit status of the first one that stops,
    // or `finished` when every pass carried on.
    auto run(Unit& unit, int finished = 0) const -> int {
        kex::timings::PhaseTimer timer;
        for (const auto& pass : m_passes) {
            const auto outcome = pass.run(unit);
            timer.mark(pass.name.c_str());
            if (outcome) return *outcome;
        }
        return finished;
    }

    auto print(std::ostream& out) const -> void {
        out << m_name << ":\n";
        for (const auto& pass : m_passes) {
            out << "  ";
            out.width(20);
            out << std::left << pass.name << pass.summary << "\n";
        }
    }

private:
    std::string m_name;
    std::vector<Pass<Unit>> m_passes;
};

} // namespace kex::driver
