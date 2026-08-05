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

struct CapturedStreams {
    std::string out;
    std::string err;
};

auto runKexStreams(const std::vector<std::string>& args) -> CapturedStreams {
    char outPath[] = "/tmp/kex_color_stdout_XXXXXX";
    char errPath[] = "/tmp/kex_color_stderr_XXXXXX";
    int outFd = mkstemp(outPath);
    int errFd = mkstemp(errPath);
    close(outFd);
    close(errFd);

    std::string cmd = std::string(KEX_BINARY_PATH);
    for (const auto& arg : args) cmd += " " + arg;
    cmd += " > " + std::string(outPath) + " 2> " + errPath;
    std::system(cmd.c_str());

    auto read = [](const char* path) {
        std::ifstream file(path);
        return std::string(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
    };
    CapturedStreams captured{read(outPath), read(errPath)};
    std::remove(outPath);
    std::remove(errPath);
    return captured;
}

auto writeTempSource(const std::string& source) -> std::string {
    char tmp[] = "/tmp/kex_color_src_XXXXXX";
    int fd = mkstemp(tmp);
    close(fd);
    // Rename with .kex extension so the compiler accepts it
    std::string path = std::string(tmp) + ".kex";
    std::rename(tmp, path.c_str());
    { std::ofstream f(path); f << source; }
    return path;
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

const std::string RANGED_VALIDATION_SRC =
    "let demo(parts: [String], values: [Any]) -> String = "
    "parts.first.or(\"\")\n"
    "let validateDemo(source: String) -> "
    "[TaggedValidation.Issue] = "
    "[TaggedValidation.warnBetween(1, 3, \"ranged warning\")]\n"
    "main do demo`abcde` end\n";

const std::string CRASHING_VALIDATION_SRC =
    "let demo(parts: [String], values: [Any]) -> String = "
    "parts.first.or(\"\")\n"
    "let validateDemo(source: String) -> [TaggedValidation.Issue] do\n"
    "  match source do\n"
    "    \"never\" -> []\n"
    "  end\n"
    "end\n"
    "main do demo`abcde` end\n";

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

        // The substring assertions above pass on both backends even when the
        // two disagree about WHERE the spaces sit relative to the escape
        // sequences — BEAM used to emit the separator as one gray " : " span
        // while the walker put the spaces outside it. Visually identical, so
        // only a byte comparison catches it.
        it("renders the colored inspect line identically on both backends", []() {
            auto path = writeTempSource(
                "main do\n"
                "  IO.inspect(42)\n"
                "  IO.inspect(\"hi\")\n"
                "  IO.inspect([1, 2, 3])\n"
                "end\n");
            auto walker = runKex({path}, "");
            auto beam = runKex({"-R", path}, "");
            std::remove(path.c_str());
            assertTrue(hasAnsi(walker), "expected colored output: " + walker);
            assertEqual(beam, walker);
        });

        it("keeps program inspection on stderr on both backends", []() {
            auto path = writeTempSource(
                "main do\n"
                "  IO.printLine(\"printed\")\n"
                "  IO.inspect(42)\n"
                "end\n");
            for (const auto& backend : {std::vector<std::string>{"--no-colors", path},
                                        std::vector<std::string>{"-R", "--no-colors", path}}) {
                auto captured = runKexStreams(backend);
                assertEqual(captured.out, std::string("printed\n"));
                assertEqual(captured.err, std::string("42 : Int\n"));
            }
            std::remove(path.c_str());
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

        it("underlines compile-time validator ranges", []() {
            auto path = writeTempSource(RANGED_VALIDATION_SRC);
            auto out =
                runKex({"--no-colors", "--check", path}, "");
            std::remove(path.c_str());
            assertTrue(
                contains(out, ":3:15-3:17: warning: ranged warning"),
                "missing exclusive range in diagnostic header: " + out);
            assertTrue(
                contains(out, "3 | main do demo`abcde` end"),
                "missing ranged diagnostic source line: " + out);
            assertTrue(contains(out, "^~"),
                       "missing ranged diagnostic underline: " + out);
        });

        it("emits validator range endpoints in JSON", []() {
            auto path = writeTempSource(RANGED_VALIDATION_SRC);
            auto out = runKex({"--json", "--check", path}, "");
            std::remove(path.c_str());
            assertTrue(contains(out, "\"line\": 3"),
                       "missing JSON start line: " + out);
            assertTrue(contains(out, "\"column\": 15"),
                       "missing JSON start column: " + out);
            assertTrue(contains(out, "\"end_line\": 3"),
                       "missing JSON end line: " + out);
            assertTrue(contains(out, "\"end_column\": 17"),
                       "missing JSON end column: " + out);
        });

        it("points validator crashes back to the triggering literal", []() {
            auto path = writeTempSource(CRASHING_VALIDATION_SRC);
            auto out =
                runKex({"--no-colors", "--check", path}, "");
            std::remove(path.c_str());
            assertTrue(contains(out, ":2:1: error: Compile-time validator "
                                     "`validateDemo` crashed:"),
                       "missing primary validator diagnostic: " + out);
            assertTrue(contains(out, ":7:9: note: triggered by tagged literal "
                                     "`demo`"),
                       "missing triggering-literal note: " + out);
        });

        it("includes validator trigger notes in JSON", []() {
            auto path = writeTempSource(CRASHING_VALIDATION_SRC);
            auto out = runKex({"--json", "--check", path}, "");
            std::remove(path.c_str());
            assertTrue(contains(out, "\"notes\": ["),
                       "missing JSON notes array: " + out);
            assertTrue(contains(out, "\"line\": 7"),
                       "missing JSON note line: " + out);
            assertTrue(
                contains(out, "\"message\": \"triggered by tagged literal "
                              "`demo`\""),
                "missing JSON note message: " + out);
        });
    });

    describe("Color CLI --no-colors flag", []() {
        it("renders the REPL result with no ANSI escapes", []() {
            auto out = runKex({"--no-colors"}, "42\n");
            assertFalse(hasAnsi(out), "unexpected ANSI escapes in: " + out);
            assertTrue(contains(out, "=> 42 : Int"), "missing plain result line: " + out);
        });

        it("renders BEAM IO.inspect like the walker, without a REPL prefix", []() {
            auto path = writeTempSource(
                "main do\n"
                "  IO.inspect(42)\n"
                "end\n");
            auto walker = runKex({"--no-colors", path}, "");
            auto beam = runKex({"-R", "--no-colors", path}, "");
            std::remove(path.c_str());
            assertEqual(walker, std::string("42 : Int\n"));
            assertEqual(beam, walker);
            assertFalse(hasAnsi(beam), "unexpected ANSI escapes in: " + beam);
            assertFalse(contains(beam, "=> "), "unexpected REPL prefix in: " + beam);
        });

        it("passes the runtime color decision to Inspectable on both backends", []() {
            auto path = writeTempSource(
                "record ColorProbe do\n"
                "  value : Integer\n"
                "end\n"
                "make ColorProbe, implement: Inspectable do\n"
                "  let inspectValue(colors: Bool) -> String = "
                    "if colors then \"colors-on\" else \"colors-off\" end\n"
                "end\n"
                "main do\n"
                "  IO.inspect(ColorProbe { value: 1 })\n"
                "end\n");
            for (const auto& backend : {std::vector<std::string>{path},
                                        std::vector<std::string>{"-R", path}}) {
                auto out = runKex(backend, "");
                assertTrue(contains(out, "colors-on"), out);
            }
            for (const auto& backend : {
                     std::vector<std::string>{"--no-colors", path},
                     std::vector<std::string>{"-R", "--no-colors", path}}) {
                auto out = runKex(backend, "");
                assertTrue(contains(out, "colors-off"), out);
                assertFalse(hasAnsi(out), "unexpected ANSI escapes in: " + out);
            }
            std::remove(path.c_str());
        });
    });

    describe("Color CLI — spec reporter", []() {
        const std::string source =
            "it(\"passes\") do\n"
            "  assert(true)\n"
            "end\n"
            "it(\"fails\") do\n"
            "  assert(false)\n"
            "end\n";

        it("renders passing ticks green and failing crosses red", [source]() {
            auto path = writeTempSource(source);
            auto out = runKex({path}, "");
            std::remove(path.c_str());
            assertTrue(contains(out, "\x1b[32m\xE2\x9C\x93\x1b[0m passes"),
                       "passing tick not green: " + out);
            assertTrue(contains(out, "\x1b[31m\xE2\x9C\x97\x1b[0m fails"),
                       "failing cross not red: " + out);
        });

        it("keeps spec output plain with --no-colors", [source]() {
            auto path = writeTempSource(source);
            auto out = runKex({"--no-colors", path}, "");
            std::remove(path.c_str());
            assertFalse(hasAnsi(out), "unexpected ANSI escapes in: " + out);
            assertTrue(contains(out, "✓ passes"), "missing passing tick: " + out);
            assertTrue(contains(out, "✗ fails"), "missing failing cross: " + out);
        });
    });

    return runAll();
}
