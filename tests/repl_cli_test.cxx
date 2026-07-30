// Runs the *actual* `kex` binary as a subprocess and feeds it stdin like a
// real terminal session would. tests/repl_test.cxx's ReplSession calls
// Evaluator::execute() directly and only ever renders with Value::toRepr(),
// so it never exercises main.cxx's REPL loop (prompt handling, multi-line
// continuation, :set/:unset, or colorValue()) — that gap is exactly how the
// colorValue() positional-ADT-record bug (Ok(x) printing as "Ok { 0: x }"
// instead of "Ok(x)") shipped unnoticed. This suite pipes input into the
// real binary and asserts on its real stdout.
#include "test.hxx"
#include "../src/common/prelude_loader.hxx"
#include <array>
#include <cstdio>
#include <filesystem>
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

auto runCommand(const std::string& command) -> std::string {
    std::string result;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe) {
        std::array<char, 4096> buf;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
            result.append(buf.data(), n);
        pclose(pipe);
    }
    return stripAnsi(result);
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

auto runBeamFile(const std::string& source, const std::string& argument,
                 bool noCheck = false) -> std::string {
    char sourcePath[] = "/tmp/kex_beam_cli_test_XXXXXX.kex";
    int fd = mkstemps(sourcePath, 4);
    assertTrue(fd >= 0, "mkstemps should create a Kex source file");
    {
        std::ofstream f(sourcePath);
        f << source;
    }
    close(fd);

    std::string cmd = std::string(KEX_BINARY_PATH) + " -R " +
        (noCheck ? "--no-check " : "") + sourcePath + " " + argument + " 2>&1";
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
    describe("CLI — installed stdlib discovery", []() {
        it("loads interpreter and BEAM stdlib from executable-relative share directories", []() {
            namespace fs = std::filesystem;
            char rootTemplate[] = "/tmp/kex_installed_cli_test_XXXXXX";
            const auto root = fs::path(mkdtemp(rootTemplate));
            const auto binDir = root / "bin";
            const auto stdlibDir = root / "share/kex/stdlib";
            const auto runtimeDir = root / "share/kex/runtime";
            fs::create_directories(binDir);
            fs::create_directories(stdlibDir);
            fs::create_directories(runtimeDir);
            fs::copy_file(KEX_BINARY_PATH, binDir / "kex");
            for (const auto& source :
                 fs::recursive_directory_iterator("src/stdlib"))
                if (source.is_regular_file() &&
                    source.path().extension() == ".kex") {
                    const auto relative =
                        fs::relative(source.path(), "src/stdlib");
                    fs::create_directories(
                        (stdlibDir / relative).parent_path());
                    fs::copy_file(source.path(),
                                  stdlibDir / relative);
                }
            const auto buildRuntime = fs::path(KEX_BINARY_PATH).parent_path() / "runtime/beam";
            for (const auto& artifact : fs::directory_iterator(buildRuntime))
                if (artifact.path().extension() == ".beam")
                    fs::copy_file(artifact.path(), runtimeDir / artifact.path().filename());

            const auto program = root / "installed.kex";
            std::ofstream(program)
                << "main do\n"
                   "  IO.printLine(Stream.Sequence(from: 1) { |n| n + 1 }.take(2))\n"
                   "end\n";

            const auto interpreterOutput = runCommand(
                "env -u KEX_STDLIB_DIR " + (binDir / "kex").string() +
                " --no-check --no-colors " + program.string() + " 2>&1");
            const auto beamOutput = runCommand(
                "env -u KEX_STDLIB_DIR -u KEX_RUNTIME_DIR " +
                (binDir / "kex").string() + " -R --no-check --no-colors " +
                program.string() + " 2>&1");
            fs::remove_all(root);
            assertEqual(interpreterOutput, std::string("[1, 2]\n"));
            assertEqual(beamOutput, std::string("[1, 2]\n"));
        });
    });

    describe("REPL CLI — Basic Output", []() {
        it("prints value and type for an expression", []() {
            auto out = runRepl("1 + 2\n");
            assertTrue(out.find("=> 3 : Int") != std::string::npos, out);
        });

        it("prints persisted let bindings", []() {
            auto out = runRepl("let x = 5\nx + 3\n");
            assertTrue(out.find("=> 8 : Int") != std::string::npos, out);
        });

        it("persists var bindings, reassignment, and ! mutation", []() {
            const std::string input =
                "var s = \"hello\"\n"
                "s.replace!(\"lo\", \"a\")\n"
                "s\n"
                "var n = 1\n"
                "n = n + 5\n"
                "n\n";
            for (const auto& out : {runRepl(input), runBeamRepl(input)}) {
                assertTrue(out.find("Undefined identifier: s")
                           == std::string::npos, out);
                assertTrue(out.find("unimplemented expr node")
                           == std::string::npos, out);
                assertTrue(out.find("=> \"hela\" : String")
                           != std::string::npos, out);
                assertTrue(out.find("=> 6 : Int") != std::string::npos, out);
            }
        });

        it("accepts indentation before persisted let bindings", []() {
            const std::string input =
                "  let source = \"{ /* profile */ \\\"name\\\": \\\"Kex\\\" }\"\n"
                "source\n";
            for (const auto& out : {runRepl(input), runBeamRepl(input)}) {
                assertTrue(
                    out.find("unimplemented expr node") == std::string::npos,
                    out);
                assertTrue(out.find("\"name\"") != std::string::npos, out);
            }
        });

        it("prints the source-level capability type of an open file", []() {
            auto out = runRepl(
                "using FS\n"
                "Mock.FS.File(\"typed.txt\", \"\")\n"
                "let f = FS.File.open(\"typed.txt\", FS.Write)\n"
                "f.try\n"
                "let f = FS.File.open(\"typed.txt\", FS.Write).try\n"
                "f\n");
            assertTrue(
                out.find("FileHandle<CannotRead, CanWrite>") !=
                    std::string::npos,
                out);
            assertTrue(out.find(": FileHandle\n") == std::string::npos, out);
        });

        it("does not execute readable methods on a write-only handle", []() {
            auto out = runRepl(
                "using FS\n"
                "Mock.FS.File(\"typed.txt\", \"contents\")\n"
                "let writer = FS.File.open(\"typed.txt\", FS.Write).try\n"
                "writer.readLine\n"
                "writer.getLine\n"
                "writer.read\n");
            assertTrue(out.find("FileHandle<CannotRead, CanWrite>") !=
                           std::string::npos,
                       out);
            assertTrue(out.find("=> None : Optional") == std::string::npos,
                       out);
            assertTrue(out.find("=> \"\" : String") == std::string::npos,
                       out);
            assertTrue(out.find("error:") != std::string::npos, out);
        });

        it("renders filesystem Optional values with Just and None", []() {
            auto out = runRepl(
                "Mock.FS.File(\"optional.txt\", \"contents\")\n"
                "FS.File.read(\"optional.txt\")\n"
                "let reader = FS.File.open(\"optional.txt\", FS.Read).try\n"
                "reader.readLine\n"
                "reader.readLine\n");
            assertTrue(out.find("Just(\"contents\") : Option<String>") !=
                           std::string::npos,
                       out);
            assertTrue(out.find("None : Option") != std::string::npos, out);
        });

        it("acknowledges native definitions", []() {
            auto out = runRepl(
                "let double(n) = n * 2\n"
                "type Traffic = Red | Go(Integer)\n");
            assertTrue(out.find("=> defined double") != std::string::npos, out);
            assertTrue(out.find("=> defined Traffic") != std::string::npos, out);
        });

        it("continues multiline raw backtick literals", []() {
            auto out = runRepl(
                "`\n"
                "    hello\n"
                "    `\n");
            assertTrue(out.find("=> \"hello\n\" : String") !=
                           std::string::npos,
                       out);
        });

        it("continues interpolating backticks with an open hole", []() {
            auto out = runRepl(
                "$`answer: ${\n"
                "  40 + 2\n"
                "}`\n");
            assertTrue(out.find("=> \"answer: 42\" : String") !=
                           std::string::npos,
                       out);
        });

        it("continues expressions with open delimiters", []() {
            auto out = runRepl(
                "(\n"
                "  1 + 2\n"
                ")\n");
            assertTrue(out.find("=> 3 : Int") != std::string::npos, out);
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
                "using FS\n"
                "main(args) do\n"
                "  IO.printLine(FS.File.exists?(args.first.or(\"\")))\n"
                "end\n",
                inputPath);
            std::remove(inputPath);
            assertTrue(out.find("true") != std::string::npos, out);
        });
    });

    describe("BEAM REPL — Kex Value Display", []() {
        it("renders file results and typestates like the tree REPL", []() {
            auto out = runBeamRepl(
                "using FS\n"
                "Mock.FS.File(\"typed.txt\", \"contents\")\n"
                "let f = FS.File.open(\"typed.txt\", FS.Write)\n"
                "let opened = f.try\n"
                "opened\n"
                "opened.close\n");
            assertTrue(
                out.find("Ok(<FileHandle: \"typed.txt\">) : "
                         "Result<FileHandle<CannotRead, CanWrite>, FileError>")
                    != std::string::npos,
                out);
            assertTrue(
                out.find("<FileHandle: \"typed.txt\"> : "
                         "FileHandle<CannotRead, CanWrite>")
                    != std::string::npos,
                out);
            assertTrue(out.find(": Tuple") == std::string::npos, out);
            assertTrue(out.find(":MockFileHandle") == std::string::npos, out);
        });

        it("renders filesystem Optional values with Just and None", []() {
            auto out = runBeamRepl(
                "Mock.FS.File(\"optional.txt\", \"contents\")\n"
                "FS.File.read(\"optional.txt\")\n"
                "let reader = FS.File.open(\"optional.txt\", FS.Read).try\n"
                "reader.readLine\n"
                "reader.readLine\n");
            assertTrue(out.find("Just(\"contents\") : Option<String>") !=
                           std::string::npos,
                       out);
            assertTrue(out.find("None : Option") != std::string::npos, out);
        });

        it("does not execute readable methods on a write-only handle", []() {
            auto out = runBeamRepl(
                "using FS\n"
                "Mock.FS.File(\"typed.txt\", \"contents\")\n"
                "let writer = FS.File.open(\"typed.txt\", FS.Write).try\n"
                "writer.readLine\n"
                "writer.write(\"ok\")\n");
            assertTrue(out.find("error:") != std::string::npos, out);
            assertTrue(out.find("=> None : Option") == std::string::npos, out);
            assertTrue(out.find("=> true : Bool") != std::string::npos, out);
        });

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

        it("keeps imported unit modules and lambda bindings live across reloads", []() {
            auto out = runBeamRepl(
                "Units.Data.B\n"
                "using Units.SI\n"
                "3.kilo.watt.to(String)\n"
                "let kilowatts = &.kilo.watt.to(String)\n"
                "kilowatts(54)\n"
                "kilowatts(99)\n");
            assertTrue(out.find("=> B : DataUnit") !=
                           std::string::npos,
                       out);
            assertTrue(out.find("\"3000.0 W\"") != std::string::npos, out);
            assertTrue(out.find("\"54000.0 W\"") != std::string::npos, out);
            assertTrue(out.find("\"99000.0 W\"") != std::string::npos, out);
            assertTrue(out.find("badfun") == std::string::npos, out);
        });

        it("renders String lists as Kex strings and suppresses IO Void", []() {
            auto out = runBeamRepl(
                "(1..3).items.map(&.to(String).or(\"\"))\n"
                "IO.printLine({ \"kex\": 3 })\n");
            assertTrue(out.find("=> [\"1\", \"2\", \"3\"] : [String]")
                       != std::string::npos, out);
            assertTrue(out.find("<<\"1\">>") == std::string::npos, out);
            assertTrue(out.find("=> :ok : Atom") == std::string::npos, out);
        });

        it("renders prelude ADT variants by constructor, not as raw data", []() {
            // The prelude's own display info was registered by nobody, so
            // BEAM printed `(:InvalidFormat, "x")` where the walker printed
            // `InvalidFormat("x")`.
            const std::string input = "Date.of(2026, 2, 30)\n";
            for (const auto& out : {runRepl(input), runBeamRepl(input)}) {
                assertTrue(out.find("Error(InvalidDate(2026, 2, 30))")
                           != std::string::npos, out);
                assertTrue(out.find(":InvalidDate") == std::string::npos, out);
            }
        });

        it("types a nullary variant by its ADT, not as an atom", []() {
            const std::string input =
                "type Colour = Red | Blue(Integer)\n"
                "[Red, Red]\n"
                "Just(Red)\n";
            for (const auto& out : {runRepl(input), runBeamRepl(input)}) {
                assertTrue(out.find("[Red, Red] : [Colour]")
                           != std::string::npos, out);
                assertTrue(out.find("Just(Red) : Option<Colour>")
                           != std::string::npos, out);
                assertTrue(out.find("Atom") == std::string::npos, out);
            }
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

    describe("CLI — opt-in JSON and Parsing stdlib", []() {
        it("parses commented JSON and serializes it on BEAM", []() {
            const std::string source =
                "using JSON\n"
                "main do\n"
                "  let Ok(value) = JSON.parse(\"{/*c*/\\\"x\\\":1}\", "
                "options: { allowComments: true })\n"
                "  IO.printLine(JSON.stringify(value))\n"
                "end\n";
            assertEqual(runBeamFile(source, ""), std::string("{\"x\":1}\n"));
        });

        it("accesses parsed JSON values in the BEAM REPL", []() {
            auto out = runBeamRepl(
                "let source = `{ \"active\": true }`\n"
                "let config = JSON.parse(source)\n"
                "config.try[\"active\"]\n"
                "config.try[\"missing\"]\n");
            assertTrue(out.find("if_clause") == std::string::npos, out);
            assertTrue(out.find("Just(true)") != std::string::npos, out);
            assertTrue(out.find("None : Option") != std::string::npos, out);
        });

        it("exposes parser combinators on BEAM", []() {
            const std::string source =
                "using Parsing\n"
                "main do\n"
                "  let input = Input { input: \"12x\" }\n"
                "  let Ok((digits, rest)) = "
                "input.some { |p| p.charWhen(&.digit?) }\n"
                "  IO.printLine(\"${digits.join(\"\")}|${rest.remaining}\")\n"
                "end\n";
            assertEqual(runBeamFile(source, ""), std::string("12|x\n"));
        });
    });

    // Every case below is a bug that reached a user's terminal because the
    // REPL paths had no coverage: module resolution, pattern-let bindings,
    // record rendering, and `using` being announced as a definition.
    describe("REPL CLI — opt-in modules and pattern lets", []() {
        it("resolves an opt-in stdlib module in the tree REPL", []() {
            // The tree REPL kept its default RELATIVE {"lib","src"} module
            // roots, so `using Regex` failed with "Unknown module" while the
            // same input worked in a script and in the BEAM REPL.
            auto out = runRepl("using Regex\nregex(\"\\\\d+\")\n");
            assertTrue(out.find("Unknown module") == std::string::npos, out);
            assertTrue(out.find("Ok(Regex { source:") != std::string::npos, out);
        });

        it("announces `using` as an import, not a definition (BEAM)", []() {
            auto out = runBeamRepl("using Regex\n");
            assertTrue(out.find("using Regex") != std::string::npos, out);
            assertTrue(out.find("defined Regex") == std::string::npos, out);
        });

        it("retains argument-type dispatch under BEAM --no-check", []() {
            auto out = runBeamFile(
                "using Regex\n"
                "main do\n"
                "  IO.printLine(\"a-b-c\".replace(\"-\", \"+\"))\n"
                "  IO.printLine(\"a1b2\".replace(regex`\\d`, \"#\"))\n"
                "end\n",
                "", true);
            assertEqual(out, std::string("a+b+c\na#b#\n"));
        });

        it("binds variables from a destructuring let in the tree REPL", []() {
            // `let Just(x) = ...` was read as DEFINING a function named Just:
            // the REPL echoed "defined Just", waited for more clauses, and the
            // right-hand side's locals were gone by the time it re-ran.
            auto out = runRepl(
                "let opt = Just(42)\n"
                "let Just(x) = opt\n"
                "x\n");
            assertTrue(out.find("defined Just") == std::string::npos, out);
            assertTrue(out.find("Undefined identifier") == std::string::npos, out);
            assertTrue(out.find("42") != std::string::npos, out);
        });

        it("binds variables from a destructuring let in the BEAM REPL", []() {
            auto out = runBeamRepl(
                "let opt = Just(42)\n"
                "let Just(x) = opt\n"
                "x\n");
            assertTrue(out.find("defined Just") == std::string::npos, out);
            assertTrue(out.find("Undefined identifier") == std::string::npos, out);
            assertTrue(out.find("42") != std::string::npos, out);
        });

        it("keeps pattern-let bindings usable on later BEAM inputs", []() {
            // The value was stored in the process dictionary but never
            // replayed into the next input's scope, so `x` came back empty.
            auto out = runBeamRepl(
                "let Just(x) = Just(42)\n"
                "x\n"
                "x + 1\n");
            assertTrue(out.find("43") != std::string::npos, out);
        });

        it("binds every name of a tuple pattern let", []() {
            auto out = runRepl("let (a, b) = (1, 2)\na\nb\n");
            assertTrue(out.find("Undefined identifier") == std::string::npos, out);
        });
    });

    describe("BEAM REPL — record rendering", []() {
        it("renders records by name, not as raw tagged tuples", []() {
            // inspect_string/1 consulted only the VARIANT table, so every
            // record (prelude ones included) printed as "(:ParseError, ...)".
            auto out = runBeamRepl("Integer.parse(\"12x\")\n");
            assertTrue(out.find("ParseError {") != std::string::npos, out);
            assertTrue(out.find("(:ParseError") == std::string::npos, out);
        });

        it("reports a record's type by name, not Tuple", []() {
            auto out = runBeamRepl("Integer.parse(\"12x\")\n");
            assertTrue(out.find("Result<?, ParseError>") != std::string::npos, out);
            assertTrue(out.find("Result<?, Tuple>") == std::string::npos, out);
        });

        it("still reports a genuine tuple as Tuple", []() {
            auto out = runBeamRepl("(1, \"a\")\n");
            assertTrue(out.find(": Tuple") != std::string::npos, out);
        });
    });

    describe("CLI — inspect on both backends", []() {
        it("supports postfix .inspect on BEAM", []() {
            // A top-level prelude function was only registered for UFCS with
            // >= 2 params, so the no-argument postfix form `x.inspect` failed
            // with "Undefined method: inspect" on BEAM only.
            auto beam = runBeamRepl("[1, 2].inspect\n");
            assertTrue(beam.find("Undefined method") == std::string::npos, beam);
            assertTrue(beam.find("[1, 2]") != std::string::npos, beam);
        });

        it("renders identically on both backends", []() {
            auto tree = runRepl("{ \"a\": 1 }.inspect\n");
            auto beam = runBeamRepl("{ \"a\": 1 }.inspect\n");
            assertTrue(tree.find("{ \"a\": 1 }") != std::string::npos, tree);
            assertTrue(beam.find("{ \"a\": 1 }") != std::string::npos, beam);
        });
    });

    return runAll();
}
