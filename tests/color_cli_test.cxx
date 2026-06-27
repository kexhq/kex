// Drives the real `kex` binary to verify ANSI coloring is consistent across
// the three places values/types are rendered: the REPL result printer, the
// IO.inspect builtin, and diagnostic error messages. main.cxx's
// colorizeMessage() and the diagnostic print loops aren't reachable from the
// in-process test harness (which calls the evaluator/typechecker directly),
// so this pipes source through the actual binary and asserts on raw bytes.
#include "test.hxx"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace test;

namespace {

auto runKex(const std::vector<std::string>& args, const std::string& standardIn) -> std::string {
    std::string cmd = std::string(KEX_BINARY_PATH);
    for (const auto& a : args) { cmd += " "; cmd += a; }

    std::string tmpPath;
    if (!standardIn.empty()) {
        char tmp[] = "/tmp/kex_color_cli_test_XXXXXX";
        int fd = mkstemp(tmp);
        { std::ofstream f(tmp); f << standardIn; }
        close(fd);
        tmpPath = tmp;
        cmd += " < " + tmpPath;
    }
    cmd += " 2>&1";

    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        std::array<char, 4096> buf;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) {
            result.append(buf.data(), n);
        }
        pclose(pipe);
    }
    if (!tmpPath.empty()) std::remove(tmpPath.c_str());
    return result;
}

auto writeTempSource(const std::string& source) -> std::string {
    char tmp[] = "/tmp/kex_color_src_XXXXXX";
    int fd = mkstemp(tmp);
    close(fd);
    { std::ofstream f(tmp); f << source; }
    return tmp;
}

auto hasAnsi(const std::string& s) -> bool {
    return s.find("\x1b[") != std::string::npos;
}

auto contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

const std::string TYPE_ERROR_SRC =
    "main do\n"
    "  let nums = [1, 2, 3]\n"
    "  nums.filter { |x| x + 1 }\n"
    "end\n";

} // namespace

int main() {
    describe("Color CLI — value rendering palette (REPL)", []() {
        it("colors an integer yellow and its type cyan", []() {
            auto out = runKex({}, "42\n");
            assertTrue(contains(out, "\x1b[33m42"), "integer value not yellow: " + out);
            assertTrue(contains(out, "\x1b[36mInt"), "type name not cyan: " + out);
            assertTrue(contains(out, "\x1b[90m=> "), "prompt not gray: " + out);
        });

        it("colors a string green and its type cyan", []() {
            auto out = runKex({}, "\"hi\"\n");
            assertTrue(contains(out, "\x1b[32m\"hi\""), "string value not green: " + out);
            assertTrue(contains(out, "\x1b[36mString"), "type name not cyan: " + out);
        });

        it("colors a positional constructor name cyan", []() {
            auto out = runKex({}, "Just(42)\n");
            assertTrue(contains(out, "\x1b[36mJust"), "constructor name not cyan: " + out);
        });
    });

    describe("Color CLI — IO.inspect matches the REPL palette", []() {
        it("renders values and type names with the same colors as the REPL", []() {
            auto path = writeTempSource(
                "main do\n"
                "  IO.inspect(42)\n"
                "  IO.inspect(\"hi\")\n"
                "end\n");
            auto out = runKex({path}, "");
            std::remove(path.c_str());
            // Same palette as the REPL tests above — this is the consistency guarantee.
            assertTrue(contains(out, "\x1b[33m42"), "integer value not yellow: " + out);
            assertTrue(contains(out, "\x1b[36mInt"), "type name not cyan: " + out);
            assertTrue(contains(out, "\x1b[32m\"hi\""), "string value not green: " + out);
            assertTrue(contains(out, "\x1b[36mString"), "type name not cyan: " + out);
            assertTrue(contains(out, "\x1b[90m:"), "':' separator not gray: " + out);
        });
    });

    describe("Color CLI — error diagnostics", []() {
        it("colors a type error: gray location, red label, bold functions, cyan types, magenta arrows", []() {
            auto path = writeTempSource(TYPE_ERROR_SRC);
            auto out = runKex({"--check", path}, "");
            std::remove(path.c_str());
            assertTrue(hasAnsi(out), "expected ANSI escapes in: " + out);
            assertTrue(contains(out, "\x1b[90m"), "missing gray location prefix: " + out);
            assertTrue(contains(out, "\x1b[31m"), "missing red error label: " + out);
            assertTrue(contains(out, "\x1b[35m->"), "missing magenta arrow: " + out);
        });

        it("renders function names bold and types cyan (distinct, not the same)", []() {
            auto path = writeTempSource(TYPE_ERROR_SRC);
            auto out = runKex({"--check", path}, "");
            std::remove(path.c_str());
            // Function name `filter` is bold ...
            assertTrue(contains(out, "\x1b[1mfilter"), "function name not bold: " + out);
            // ... and NOT cyan (functions and types must stay distinguishable).
            assertFalse(contains(out, "\x1b[36mfilter"), "function name wrongly cyan: " + out);
            // Type name Integer is cyan, matching the REPL/IO.inspect type suffix.
            assertTrue(contains(out, "\x1b[36mInteger"), "type name not cyan in error: " + out);
            assertFalse(contains(out, "\x1b[33mInteger"), "type name wrongly yellow in error: " + out);
        });

        it("--no-colors strips every ANSI escape from errors", []() {
            auto path = writeTempSource(TYPE_ERROR_SRC);
            auto out = runKex({"--no-colors", "--check", path}, "");
            std::remove(path.c_str());
            assertFalse(hasAnsi(out), "unexpected ANSI escapes in: " + out);
            assertTrue(contains(out, "error:"), "missing plain 'error:' label: " + out);
            assertTrue(contains(out, "filter"), "missing function name: " + out);
            assertTrue(contains(out, "Integer"), "missing type name: " + out);
            assertTrue(contains(out, "->"), "missing arrow: " + out);
        });
    });

    describe("Color CLI --no-colors flag", []() {
        it("renders the REPL result with no ANSI escapes", []() {
            auto out = runKex({"--no-colors"}, "42\n");
            assertFalse(hasAnsi(out), "unexpected ANSI escapes in: " + out);
            assertTrue(contains(out, "=> 42 : Int"), "missing plain result line: " + out);
        });
    });

    return runAll();
}
