// What `kex` says when it fails by its own fault: an internal error or a
// crash tells the user it is a compiler bug and links a new kexhq/kex issue
// with the facts filled in. KEX_DEBUG_CRASH makes the real binary fail on
// purpose; the URL itself is checked in-process.
#include "common/crash_report.hxx"
#include "test.hxx"
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace test;

namespace {

struct Run {
    std::string output;
    int status = 0;
};

// Runs kex with +env+ in front, stderr into the captured output.
auto runKex(const std::string& env, const std::string& args) -> Run {
    const std::string cmd = env + " " + std::string(KEX_BINARY_PATH) + " " + args + " 2>&1";
    Run run;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return run;
    }
    std::array<char, 4096> buf{};
    size_t n = 0;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        run.output.append(buf.data(), n);
    }
    run.status = pclose(pipe);
    return run;
}

// A program with nothing wrong in it, for kex to be given.
auto sourceFile() -> std::string {
    char tmp[] = "/tmp/kex_crash_src_XXXXXX";
    close(mkstemp(tmp));
    const std::string path = std::string(tmp) + ".kex";
    std::rename(tmp, path.c_str());
    std::ofstream(path) << "main do\n  IO.printLine(1)\nend\n";
    return path;
}

auto contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main() {
    describe("Crash report — the issue link", []() {
        it("percent-encodes everything but the unreserved characters", []() {
            assertEqual(kex::crash::percentEncode("a-b_c.d~e"), std::string("a-b_c.d~e"));
            assertEqual(
                kex::crash::percentEncode("x y&z=1\n`é`"),
                std::string("x%20y%26z%3D1%0A%60%C3%A9%60"));
        });

        it("opens a new kexhq/kex issue with its title and body", []() {
            assertEqual(
                kex::crash::issueUrl("Crash: SIGSEGV", "body text"),
                std::string(
                    "https://github.com/kexhq/kex/issues/new?labels=Bug"
                    "&title=Crash%3A%20SIGSEGV&body=body%20text"));
        });
    });

    describe("Crash report — the real binary", []() {
        it("follows an internal error with the report and the full link", []() {
            const auto path = sourceFile();
            auto run = runKex("KEX_DEBUG_CRASH=internal", "--no-colors -C " + path);
            std::remove(path.c_str());
            assertTrue(
                contains(run.output, "Internal error: KEX_DEBUG_CRASH asked for an internal error"),
                "missing the error: " + run.output);
            assertTrue(
                contains(run.output, "This is a bug in the Kex compiler"),
                "missing the report: " + run.output);
            assertTrue(
                contains(
                    run.output, "https://github.com/kexhq/kex/issues/new?labels=Bug&title="
                                "Internal%20error%3A%20KEX_DEBUG_CRASH"),
                "missing the issue link: " + run.output);
            assertTrue(contains(run.output, "Platform%3A%20"), "body lacks the platform");
            assertTrue(contains(run.output, "Command%3A%20%60kex%20"), "body lacks the command");
            assertTrue(WIFEXITED(run.status) && WEXITSTATUS(run.status) == 1, "exit status not 1");
        });

        it("does not blame the compiler for the program's own failure", []() {
            char tmp[] = "/tmp/kex_crash_src_XXXXXX";
            close(mkstemp(tmp));
            const std::string path = std::string(tmp) + ".kex";
            std::rename(tmp, path.c_str());
            std::ofstream(path) << "main do\n  let zero = 0\n  IO.printLine(1 / zero)\nend\n";
            auto run = runKex("", "--no-colors --run-walker " + path);
            std::remove(path.c_str());
            assertTrue(contains(run.output, "division by zero"), "no runtime error: " + run.output);
            assertFalse(
                contains(run.output, "bug in the Kex compiler"),
                "a runtime error blamed on the compiler: " + run.output);
        });

        it("reports a crash, then dies of the same signal", []() {
            const auto path = sourceFile();
            auto run = runKex("KEX_DEBUG_CRASH=abort", "--no-colors -C " + path);
            std::remove(path.c_str());
            assertTrue(
                contains(run.output, "kex crashed: SIGABRT (aborted)"),
                "missing the crash line: " + run.output);
            assertTrue(
                contains(run.output, "issues/new?labels=Bug&title=Crash%3A%20SIGABRT"),
                "missing the issue link: " + run.output);
            // popen's shell reports a child killed by a signal as 128 + signal.
            const bool killed = WIFSIGNALED(run.status) ||
                                (WIFEXITED(run.status) && WEXITSTATUS(run.status) == 128 + 6);
            assertTrue(killed, "not killed by SIGABRT: " + std::to_string(run.status));
        });

        it("makes the link a few words in a terminal that takes hyperlinks", []() {
            const auto path = sourceFile();
            auto run = runKex("KEX_DEBUG_CRASH=internal KEX_HYPERLINKS=1", "--colors -C " + path);
            std::remove(path.c_str());
            assertTrue(
                contains(run.output, "\x1b]8;;https://github.com/kexhq/kex/issues/new?"),
                "link not an OSC 8 hyperlink: " + run.output);
            assertTrue(
                contains(run.output, "open an issue on GitHub"),
                "missing link text: " + run.output);
        });
    });

    return runAll();
}
