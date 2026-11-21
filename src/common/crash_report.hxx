#pragma once

// What `kex` says when it fails in a way that is its own fault, not the
// program's: an internal error (an exception nothing else caught) or a crash
// (a fatal signal). Either way the user is told it is a bug in the compiler
// and handed a link to a new kexhq/kex issue with what we know filled in:
// the version, the platform, the command line, and the error.

#include <string>
#include <string_view>

namespace kex::crash {

// Where the report goes.
inline constexpr std::string_view kIssuesUrl = "https://github.com/kexhq/kex/issues/new";

// +text+ percent-encoded for a URL's query: everything but RFC 3986's
// unreserved characters.
auto percentEncode(std::string_view text) -> std::string;

// A link that opens a new issue titled +title+, with +body+ as its text.
auto issueUrl(std::string_view title, std::string_view body) -> std::string;

// Remembers the command line for a report; call once, early in main().
auto rememberCommandLine(int argc, char** argv) -> void;

// The lines after an internal error's message: that it is a compiler bug,
// and the link to report it. +what+ is the error, for the issue.
auto internalErrorReport(std::string_view what) -> std::string;

// Prints a report and re-raises on SIGSEGV, SIGBUS, SIGILL, SIGFPE and
// SIGABRT, so the exit status and a core dump stay what they were. The
// message is built here, once, since a signal handler may only write it.
// Call after colors and hyperlinks are decided.
auto installCrashHandlers() -> void;

} // namespace kex::crash
