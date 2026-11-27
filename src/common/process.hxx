#pragma once

#include "../interpreter/evaluator.hxx"

#include <functional>
#include <string>
#include <sys/types.h> // IWYU pragma: keep (POSIX declares pid_t here)
#include <vector>

#ifndef __EMSCRIPTEN__
#endif

namespace kex::process {

// Wrap `s` as a single POSIX-shell single-quoted argument, escaping any
// embedded `'` via the standard close-quote/escaped-literal-quote/reopen
// idiom (`'\''`).
auto shellSingleQuote(const std::string& s) -> std::string;

// One program to run: `argv[0]` is looked up on PATH the way `execvp` does,
// and every element reaches the program as exactly one argument. No shell is
// involved, so nothing in an argument — a space, a quote, `$(...)`, a
// backtick, an empty string — is reinterpreted on the way.
using Command = std::vector<std::string>;

// Run `command` and wait for it. Unlike `std::system`, which POSIX requires to
// IGNORE SIGINT and SIGQUIT in the calling process for the whole call and
// which gives back no pid, signals the launcher receives are forwarded to the
// child. Returns a wait status, as `std::system` does — pass it to
// `exitStatusOf`. A program that cannot be started exits 127, as a shell
// reports it.
auto runCommand(const Command& command) -> int;

// Decode a wait status (from `runCommand`) into a process exit code.
auto exitStatusOf(int waitStatus) -> int;

// Run `commands` side by side and call `finished(index, status)` for each as
// it ends, in the order they end. `discardOutput` sends each one's stdout and
// stderr to /dev/null.
auto runCommandsTogether(
    const std::vector<Command>& commands,
    bool discardOutput,
    const std::function<void(std::size_t, int)>& finished) -> void;

// `kex_test:configure(Mode, Filters), ` for the `-eval` the BEAM runner
// builds, or "" for an ordinary run.
auto beamTestGate(
    kex::interpreter::Evaluator::TestReportMode mode, const std::vector<std::string>& filters)
    -> std::string;

#ifndef __EMSCRIPTEN__
// Persistent-VM driver for the BEAM REPL. Keeps one `erl -noshell` process
// alive for the whole REPL session, talking to runtime/src/kex_repl_driver.erl
// over stdin/stdout with a nonce-delimited protocol.
struct BeamVm {
    pid_t pid = -1;
    int inFd = -1;  // write end -> child stdin
    int outFd = -1; // read end <- child stdout

    auto start(const std::vector<std::string>& argv) -> bool;
    void writeLine(const std::string& s);
    auto readUntilSentinel(const std::string& sentinelPrefix, std::string& status) -> std::string;
    void close();
};
#else
// Emscripten has no fork/exec/pipe, and the BEAM REPL is meaningless under
// wasm anyway. Stub so callers compile unchanged — start() fails and the
// beam-repl path reports unavailable.
struct BeamVm {
    int pid = -1;
    auto start(const std::vector<std::string>&) -> bool {
        return false;
    }
    void writeLine(const std::string&) {}
    auto readUntilSentinel(const std::string&, std::string& s) -> std::string {
        s = "eof";
        return {};
    }
    void close() {}
};
#endif

// One request of the REPL driver protocol (runtime/src/kex_repl_driver.erl):
// sends `<verb> <id> [<payload>]` and collects what the driver printed before
// its `KEX_REPL_DONE <id> <status>` line. Ids come from one counter, so no
// two requests of a process share one.
struct VmReply {
    std::string status;
    std::string output;
    auto ok() const -> bool {
        return status == "ok";
    }
};

inline auto vmRequest(BeamVm& vm, const std::string& verb, const std::string& payload = "")
    -> VmReply {
    static unsigned long long nextId = 0;
    const auto id = std::to_string(++nextId);
    vm.writeLine(payload.empty() ? verb + " " + id : verb + " " + id + " " + payload);
    VmReply reply;
    reply.output = vm.readUntilSentinel("KEX_REPL_DONE " + id + " ", reply.status);
    return reply;
}

} // namespace kex::process
