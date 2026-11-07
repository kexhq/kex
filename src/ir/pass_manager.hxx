#pragma once
// IR passes. Each rewrite of a lowered module is a named pass, run in order by
// a PassManager that times it: with KEX_TIMINGS set, one line per pass, summed
// over every module it ran on. A new IR rewrite registers in `standardPasses`
// rather than being called ad hoc wherever modules are emitted.
#include "ir.hxx"
#include "../common/timings.hxx"

#include <functional>
#include <string>
#include <vector>

namespace kex::ir {

struct Pass {
    std::string name;
    std::function<void(Module&)> run;
};

class PassManager {
public:
    auto add(std::string name, std::function<void(Module&)> run) -> PassManager&;
    // Runs every pass, in the order added, over `module`.
    auto run(Module& module) -> void;
    // With KEX_TIMINGS: what each pass cost, over all the modules run.
    auto report() const -> void;
    auto passes() const -> const std::vector<Pass>& { return m_passes; }

private:
    std::vector<Pass> m_passes;
    std::vector<timings::Clock::duration> m_spent;
    std::size_t m_modules = 0;
};

// The passes every emitted module goes through, in order.
auto standardPasses() -> PassManager;

} // namespace kex::ir
