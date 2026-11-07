#include "pass_manager.hxx"
#include "passes/upgrade.hxx"
#include "passes/guards.hxx"
#include "passes/verify.hxx"

#include <stdexcept>

#include <cstdio>

namespace kex::ir {

auto PassManager::add(std::string name, std::function<void(Module&)> run)
    -> PassManager& {
    m_passes.push_back({std::move(name), std::move(run)});
    m_spent.emplace_back();
    return *this;
}

auto PassManager::run(Module& module) -> void {
    ++m_modules;
    for (std::size_t index = 0; index < m_passes.size(); ++index) {
        const auto start = timings::Clock::now();
        m_passes[index].run(module);
        m_spent[index] += timings::Clock::now() - start;
    }
}

auto PassManager::report() const -> void {
    if (!timings::enabled()) return;
    for (std::size_t index = 0; index < m_passes.size(); ++index)
        std::fprintf(stderr, "    pass %-22s %8.1f ms  (%zu modules)\n",
                     m_passes[index].name.c_str(),
                     timings::milliseconds(m_spent[index]), m_modules);
}

auto standardPasses() -> PassManager {
    PassManager passes;
    // First: `when` guards out of BEAM guard position, before any pass reads
    // the control flow they become.
    passes.add("guards", expandGuards);
    // Loop lifting and the hot-code-upgrade points a reloaded module needs.
    passes.add("upgrade-points", insertUpgradePoints);
    // Last: what every pass left must still be a well-formed module. A
    // violation stops the build, naming each one, instead of reaching erlc.
    passes.add("verify", [](Module& module) {
        const auto problems = verifyModule(module);
        if (problems.empty()) return;
        std::string message = "IR verification failed:";
        for (const auto& problem : problems) message += "\n  " + problem;
        throw std::runtime_error(message);
    });
    return passes;
}

} // namespace kex::ir
