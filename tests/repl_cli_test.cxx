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

auto runBeamRepl(const std::string& input) -> std::string {
    char tmpPath[] = "/tmp/kex_beam_repl_cli_test_XXXXXX";
    int fd = mkstemp(tmpPath);
    {
        std::ofstream f(tmpPath);
        f << input;
    }
    close(fd);

    std::string cmd = std::string(KEX_BINARY_PATH) +
        " -i --no-colors < " + tmpPath + " 2>&1";
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        std::array<char, 4096> buf;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
            result.append(buf.data(), n);
        pclose(pipe);
    }
    std::remove(tmpPath);
    return stripAnsi(result);
}

auto runBeamFile(const std::string& source, const std::string& argument) -> std::string {
    char sourcePath[] = "/tmp/kex_beam_cli_test_XXXXXX.kex";
    int fd = mkstemps(sourcePath, 4);
    assertTrue(fd >= 0, "mkstemps should create a Kex source file");
    {
        std::ofstream f(sourcePath);
        f << source;
    }
    close(fd);

    std::string cmd = std::string(KEX_BINARY_PATH) + " -R " + sourcePath + " " + argument + " 2>&1";
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        std::array<char, 4096> buf;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
            result.append(buf.data(), n);
        pclose(pipe);
    }
    std::remove(sourcePath);
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

        it("acknowledges native definitions", []() {
            auto out = runRepl(
                "let double(n) = n * 2\n"
                "type Traffic = Red | Go(Integer)\n");
            assertTrue(out.find("=> defined double") != std::string::npos, out);
            assertTrue(out.find("=> defined Traffic") != std::string::npos, out);
        });
    });

    describe("REPL CLI — Immutability", []() {
        it("rejects plain assignment to a let binding with a runtime error", []() {
            auto out = runRepl("let kx = \"a\"\nkx = \"b\"\nkx\n");
            assertTrue(out.find("Cannot assign to immutable binding: kx") != std::string::npos, out);
            assertTrue(out.find("=> \"a\" : String") != std::string::npos, out);
        });
    });

    describe("REPL CLI — loaded definitions", []() {
        it("keeps a loaded module's AST alive for later bindings", []() {
            char sourcePath[] = "/tmp/kex_repl_load_test_XXXXXX.kex";
            int fd = mkstemps(sourcePath, 4);
            assertTrue(fd >= 0, "mkstemps should create a Kex source file");
            {
                std::ofstream f(sourcePath);
                f << "module Loaded do\n"
                     "  let values() = [5, 6, 7]\n"
                     "end\n";
            }
            close(fd);

            auto out = runRepl(std::string("/load ") + sourcePath +
                               "\nlet t = Loaded.values()\nt\n");
            std::remove(sourcePath);
            assertTrue(out.find("loaded ") != std::string::npos, out);
            assertTrue(out.find("=> [5, 6, 7] : [Int]") != std::string::npos, out);
        });
    });

    describe("REPL CLI — ADT Display", []() {
        it("renders positional records as Name(args), not a field dump", []() {
            auto out = runRepl("Ok(\"hi\")\n");
            // : Result<String, ?> — the parent generic type with its
            // resolved type param, not the bare constructor tag "Ok" (see
            // Value::ok/just/error in value.cxx and VariantValue's
            // typeParams/argParamIndex).
            assertTrue(out.find("=> Ok(\"hi\") : Result<String, ?>") != std::string::npos, out);
            assertTrue(out.find("{ 0:") == std::string::npos, out);
        });

        it("renders Error(...) the same way", []() {
            auto out = runRepl("Error(\"bad\")\n");
            assertTrue(out.find("=> Error(\"bad\") : Result<?, String>") != std::string::npos, out);
        });

        it("renders Just(...) the same way", []() {
            auto out = runRepl("Just(42)\n");
            assertTrue(out.find("=> Just(42) : Option<Int>") != std::string::npos, out);
        });
    });

    describe("REPL CLI — Char Display", []() {
        it("renders a Char literal with quotes, not a bare '?'", []() {
            auto out = runRepl("'a'\n");
            assertTrue(out.find("=> 'a' : Char") != std::string::npos, out);
        });
    });

    describe("BEAM CLI — Script Arguments", []() {
        it("passes main(args) path values as Kex strings", []() {
            char inputPath[] = "/tmp/kex_beam_arg_test_XXXXXX";
            int fd = mkstemp(inputPath);
            assertTrue(fd >= 0, "mkstemp should create an input file");
            close(fd);

            auto out = runBeamFile(
                "main(args) do\n"
                "  IO.printLine(File.exists?(args.first.or(\"\")))\n"
                "end\n",
                inputPath);
            std::remove(inputPath);
            assertTrue(out.find("true") != std::string::npos, out);
        });
    });

    describe("BEAM REPL — Kex Value Display", []() {
        it("keeps loaded modules visible to later bindings", []() {
            char sourcePath[] = "/tmp/kex_beam_repl_load_test_XXXXXX.kex";
            int fd = mkstemps(sourcePath, 4);
            assertTrue(fd >= 0, "mkstemps should create a Kex source file");
            {
                std::ofstream f(sourcePath);
                f << "module Loaded do\n"
                     "  let values() = [5, 6, 7]\n"
                     "end\n";
            }
            close(fd);

            auto out = runBeamRepl(std::string("/load ") + sourcePath +
                                   "\nlet t = Loaded.values()\nt\n");
            std::remove(sourcePath);
            assertTrue(out.find("loaded ") != std::string::npos, out);
            assertTrue(out.find("=> [5, 6, 7] : [Int]") != std::string::npos, out);
        });

        it("renders String lists as Kex strings and suppresses IO Unit", []() {
            auto out = runBeamRepl(
                "(1..3).items.map(&.to(String).or(\"\"))\n"
                "IO.printLine({ \"kex\": 3 })\n");
            assertTrue(out.find("=> [\"1\", \"2\", \"3\"] : [String]")
                       != std::string::npos, out);
            assertTrue(out.find("<<\"1\">>") == std::string::npos, out);
            assertTrue(out.find("=> :ok : Atom") == std::string::npos, out);
        });

        it("renders Optional and Result values as Kex ADTs", []() {
            auto out = runBeamRepl(
                "Just(42)\n"
                "Ok(\"ready\")\n"
                "Error(\"bad\")\n");
            assertTrue(out.find("=> Just(42) : Option<Int>")
                       != std::string::npos, out);
            assertTrue(out.find("=> Ok(\"ready\") : Result<String, ?>")
                       != std::string::npos, out);
            assertTrue(out.find("=> Error(\"bad\") : Result<?, String>")
                       != std::string::npos, out);
            assertTrue(out.find("{'Just',42}") == std::string::npos, out);
        });

        it("renders tuples, maps, and nullary variants without Erlang syntax", []() {
            auto out = runBeamRepl(
                "(1, \"x\")\n"
                "{ \"x\": 1 }\n"
                "Less\n");
            assertTrue(out.find("=> (1, \"x\") : Tuple")
                       != std::string::npos, out);
            assertTrue(out.find("=> { \"x\": 1 } : Map")
                       != std::string::npos, out);
            assertTrue(out.find("=> Less : Ordering")
                       != std::string::npos, out);
            assertTrue(out.find("#{") == std::string::npos, out);
            assertTrue(out.find("<<\"x\">>") == std::string::npos, out);
        });

        it("renders custom nullary and payload ADTs with their declared type", []() {
            auto out = runBeamRepl(
                "type Traffic = Red | Go(Integer)\n"
                "Red\n"
                "Go(3)\n");
            assertTrue(out.find("=> defined Traffic") != std::string::npos, out);
            assertTrue(out.find("=> Red : Traffic") != std::string::npos, out);
            assertTrue(out.find("=> Go(3) : Traffic") != std::string::npos, out);
            assertTrue(out.find("{'Go',3}") == std::string::npos, out);
        });
    });

    describe("CLI — Map Display", []() {
        it("uses canonical key order on both backends", []() {
            const std::string source =
                "main do\n"
                "  IO.printLine({ \"kex\": 3, \"fast\": 4, \"is\": 2 })\n"
                "end\n";
            const std::string expected = "{ fast: 4, is: 2, kex: 3 }";
            auto interpreter = runRepl("IO.printLine({ \"kex\": 3, \"fast\": 4, \"is\": 2 })\n");
            auto beam = runBeamFile(source, "");
            assertTrue(interpreter.find(expected) != std::string::npos, interpreter);
            assertTrue(beam.find(expected) != std::string::npos, beam);
        });
    });

    return runAll();
}
