#pragma once
// The pass options of this process, as LLVM's are command-line options every
// pass manager reads: what to time, what to print, and pipelines the user
// wrote instead of the default ones (`kex --passes=...`). Set once by the
// CLI; every pipeline the compiler builds reports to the same callbacks.
#include "instrumentation.hxx"

#include <map>
#include <set>
#include <string>
#include <string_view>

namespace kex::pass {

struct PassOptions {
    // `--print-after=<pass>` (repeatable) and `--print-after-all`.
    std::set<std::string> printAfter;
    bool printAfterAll = false;
    // `--time-passes`, or KEX_TIMINGS: a report on stderr at exit.
    bool timePasses = false;
    // A level's pipeline as the user wrote it (`--passes=unit(...)`), by the
    // level's name; absent: the default.
    std::map<std::string, std::string, std::less<>> pipelines;
};

auto passOptions() -> PassOptions&;

// The pipeline text to build for `level`: the user's, or `fallback`.
auto pipelineText(std::string_view level, std::string_view fallback) -> std::string;

// The callbacks every pipeline reports to, set up from passOptions() the
// first time they are asked for.
auto standardCallbacks() -> const PassInstrumentationCallbacks&;

} // namespace kex::pass
