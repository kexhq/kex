#include "common/crash_report.hxx"

#include "common/color.hxx"
#include "common/version.hxx"

#include <array>
#include <csignal>

#ifndef __EMSCRIPTEN__
#include <unistd.h>
#include <cstdlib>
#include <signal.h>
#endif
// glibc and macOS have backtrace(); musl (Alpine) does not, and there
// KEX_BACKTRACE prints the report without one.
#if !defined(__EMSCRIPTEN__) && __has_include(<execinfo.h>)
#include <execinfo.h>
#define KEX_HAS_BACKTRACE 1
#endif

namespace kex::crash {

namespace {

// The command line, as `kex a b c`, for the report.
std::string g_commandLine; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

auto platform() -> std::string {
#if defined(__EMSCRIPTEN__)
    std::string os = "wasm";
#elif defined(__APPLE__)
    std::string os = "macOS";
#elif defined(__linux__)
    std::string os = "Linux";
#elif defined(_WIN32)
    std::string os = "Windows";
#else
    std::string os = "unknown OS";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    return os + " x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return os + " arm64";
#else
    return os;
#endif
}

// The issue's text: what happened, and the facts a maintainer asks for first.
auto issueBody(std::string_view what) -> std::string {
    const std::string fence = "```";
    std::string body;
    body += "The compiler failed with:\n\n" + fence + "\n";
    body += what;
    body += "\n" + fence + "\n\n";
    body += "- Kex: " + kex::versionStringWithBuild() + "\n";
    body += "- Platform: " + platform() + "\n";
    body += "- Command: `" + g_commandLine + "`\n\n";
    body += "### The program\n\n";
    body += "<!-- The smallest program that still fails, if you can share one. -->\n\n";
    body += fence + "kex\n\n" + fence + "\n";
    return body;
}

// The two lines that follow the error: whose fault it is, and where to say so.
// In a terminal that takes hyperlinks the link is a few words; elsewhere it is
// the whole URL, to copy.
auto reportLines(std::string_view title, std::string_view what) -> std::string {
    using namespace kex::color;
    const auto url = issueUrl(title, issueBody(what));
    std::string out = "This is a bug in the Kex compiler, not in your program. Please report it";
    if (enabled && hyperlinks) {
        out += ": " +
               link(url, std::string(apply(underline)) + "open an issue on GitHub" + apply(reset));
        out += "\n";
    } else {
        out += ", with what is filled in here:\n  " + url + "\n";
    }
    return out;
}

#ifndef __EMSCRIPTEN__
auto signalName(int sig) -> const char* {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGBUS: return "SIGBUS (bus error)";
        case SIGILL: return "SIGILL (illegal instruction)";
        case SIGFPE: return "SIGFPE (arithmetic error)";
        case SIGABRT: return "SIGABRT (aborted)";
        default: return "a fatal signal";
    }
}

constexpr std::array kFatalSignals = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

// One message per signal, built before any crash, so the handler only picks
// one and writes it.
std::array<
    std::string,
    kFatalSignals.size()> // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    g_crashMessages;

// Set from KEX_BACKTRACE when the handlers are installed: the native stack
// follows the report, for whoever chases a crash a fuzzer found.
bool g_printBacktrace = false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

extern "C" void onFatalSignal(int sig) {
    for (size_t i = 0; i < kFatalSignals.size(); ++i) {
        if (kFatalSignals[i] == sig) {
            const auto& message = g_crashMessages[i];
            [[maybe_unused]] auto written = ::write(STDERR_FILENO, message.data(), message.size());
            break;
        }
    }
#ifdef KEX_HAS_BACKTRACE
    if (g_printBacktrace) {
        std::array<void*, 64> frames{};
        const int depth = ::backtrace(frames.data(), static_cast<int>(frames.size()));
        ::backtrace_symbols_fd(frames.data(), depth, STDERR_FILENO);
    }
#endif
    // Back to the default action, and again: the process ends as it would
    // have without this handler.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}
#endif

} // namespace

auto percentEncode(std::string_view text) -> std::string {
    static constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                                byte == '_' || byte == '~';
        if (unreserved) {
            out += c;
        } else {
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 0xF];
        }
    }
    return out;
}

auto issueUrl(std::string_view title, std::string_view body) -> std::string {
    return std::string(kIssuesUrl) + "?labels=Bug&title=" + percentEncode(title) +
           "&body=" + percentEncode(body);
}

auto rememberCommandLine(int argc, char** argv) -> void {
    g_commandLine = "kex";
    for (int i = 1; i < argc; ++i) {
        g_commandLine += " ";
        g_commandLine += argv[i];
    }
}

auto internalErrorReport(std::string_view what) -> std::string {
    // A title past GitHub's limit is cut; the body has the rest.
    constexpr size_t kTitleLimit = 120;
    std::string title = "Internal error: " + std::string(what.substr(0, kTitleLimit));
    if (const auto newline = title.find('\n'); newline != std::string::npos) {
        title.resize(newline);
    }
    return reportLines(title, what);
}

auto installCrashHandlers() -> void {
#ifndef __EMSCRIPTEN__
    g_printBacktrace = std::getenv("KEX_BACKTRACE") != nullptr;
    using namespace kex::color;
    for (size_t i = 0; i < kFatalSignals.size(); ++i) {
        const std::string name = signalName(kFatalSignals[i]);
        auto& message = g_crashMessages[i];
        message = "\n";
        message += std::string(apply(bold)) + apply(red) + "kex crashed:" + apply(reset) + " " +
                   name + "\n";
        message += reportLines("Crash: " + name, "kex crashed with " + name);
        // On a stack of its own: a stack overflow left none to report on,
        // and the process died without a word (exit 132).
        struct sigaction action{};
        action.sa_handler = onFatalSignal;
        action.sa_flags = SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(kFatalSignals[i], &action, nullptr);
    }
    static std::array<char, 64 * 1024> alternateStack{};
    stack_t alternate{};
    alternate.ss_sp = alternateStack.data();
    alternate.ss_size = alternateStack.size();
    sigaltstack(&alternate, nullptr);
#endif
}

} // namespace kex::crash
