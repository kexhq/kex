// Runs the *actual* `kex` binary as a subprocess and feeds it stdin like a
// real terminal session would. tests/repl_test.cxx's ReplSession calls
// Evaluator::execute() directly and only ever renders with Value::toRepr(),
// so it never exercises main.cxx's REPL loop (prompt handling, multi-line
// continuation, :set/:unset, or colorValue()) — that gap is exactly how the
// colorValue() positional-ADT-record bug (Ok(x) printing as "Ok { 0: x }"
// instead of "Ok(x)") shipped unnoticed. This suite pipes input into the
// real binary and asserts on its real stdout.
#include "test.hxx"
#include <array>
#include <cstdio>
#include <fstream>
#include <unistd.h>

using namespace test;

namespace {

auto stripAnsi(const std::string& s) -> std::string {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && s[j] != 'm') j++;
            i = (j < s.size()) ? j + 1 : j;
        } else {
            out += s[i];
            i++;
        }
    }
    return out;
}

auto runRepl(const std::string& input) -> std::string {
    char tmpPath[] = "/tmp/kex_repl_cli_test_XXXXXX";
    int fd = mkstemp(tmpPath);
    {
        std::ofstream f(tmpPath);
        f << input;
    }
    close(fd);

    std::string cmd = std::string(KEX_BINARY_PATH) + " --no-colors < " + tmpPath + " 2>&1";
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
    std::remove(tmpPath);
    return stripAnsi(result);
}

} // namespace

int main() {
    describe("REPL CLI — Basic Output", []() {
        it("prints value and type for an expression", []() {
            auto out = runRepl("1 + 2\n");
            assertTrue(out.find("=> 3 : Int") != std::string::npos, out);
        });

        it("prints persisted let bindings", []() {
            auto out = runRepl("let x = 5\nx + 3\n");
            assertTrue(out.find("=> 8 : Int") != std::string::npos, out);
        });
    });

    describe("REPL CLI — Immutability", []() {
        it("rejects plain assignment to a let binding with a runtime error", []() {
            auto out = runRepl("let kx = \"a\"\nkx = \"b\"\nkx\n");
            assertTrue(out.find("Cannot assign to immutable binding: kx") != std::string::npos, out);
            assertTrue(out.find("=> \"a\" : String") != std::string::npos, out);
        });
    });

    describe("REPL CLI — ADT Display", []() {
        it("renders positional records as Name(args), not a field dump", []() {
            auto out = runRepl("Ok(\"hi\")\n");
            assertTrue(out.find("=> Ok(\"hi\") : Ok") != std::string::npos, out);
            assertTrue(out.find("{ 0:") == std::string::npos, out);
        });

        it("renders Error(...) the same way", []() {
            auto out = runRepl("Error(\"bad\")\n");
            assertTrue(out.find("=> Error(\"bad\") : Error") != std::string::npos, out);
        });

        it("renders Just(...) the same way", []() {
            auto out = runRepl("Just(42)\n");
            assertTrue(out.find("=> Just(42) : Just") != std::string::npos, out);
        });
    });

    describe("REPL CLI — Char Display", []() {
        it("renders a Char literal with quotes, not a bare '?'", []() {
            auto out = runRepl("'a'\n");
            assertTrue(out.find("=> 'a' : Char") != std::string::npos, out);
        });
    });

    return runAll();
}
