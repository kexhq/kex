#include "options.hxx"

#include "standard_instrumentation.hxx"
#include "../common/timings.hxx"

#include <cstdlib>
#include <iostream>

namespace kex::pass {

auto passOptions() -> PassOptions& {
    static PassOptions options;
    return options;
}

auto pipelineText(std::string_view level, std::string_view fallback) -> std::string {
    const auto& pipelines = passOptions().pipelines;
    const auto found = pipelines.find(level);
    return std::string(found == pipelines.end() ? fallback : found->second);
}

auto standardCallbacks() -> const PassInstrumentationCallbacks& {
    static const auto callbacks = [] {
        PassInstrumentationCallbacks registered;
        const auto& options = passOptions();
        if (options.timePasses || timings::enabled()) {
            // Constructed before the handler is registered, so it outlives it.
            static TimePasses timer;
            timer.registerCallbacks(registered);
            std::atexit([] {
                std::cerr << "pass timings:\n";
                timer.report(std::cerr);
            });
        }
        if (options.printAfterAll || !options.printAfter.empty()) {
            static PrintAfter print(std::cerr, options.printAfter, options.printAfterAll);
            print.registerCallbacks(registered);
        }
        return registered;
    }();
    return callbacks;
}

} // namespace kex::pass
