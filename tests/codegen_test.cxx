#include "test.hxx"
#include "../src/codegen/core_erlang.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace test;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Parse Kex source and emit Core Erlang text.
auto emit(const std::string& source, const std::string& stem = "test") -> std::string {
    kex::Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    kex::Parser parser(std::move(tokens));
    auto program = parser.parseProgram();
    kex::codegen::CoreErlangEmitter emitter;
    return emitter.emitProgram(program, stem).source;
}

// Returns true if `haystack` contains `needle` as a substring.
auto contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

int main() {
    describe("CoreErlangEmitter — module structure", []() {
        it("emits module header with correct name", []() {
            auto out = emit("main do\n  42\nend\n", "hello");
            assertTrue(contains(out, "module 'kex_hello'"), out);
        });

        it("exports main/0 for a no-arg main block", []() {
            auto out = emit("main do\n  42\nend\n");
            assertTrue(contains(out, "'main'/0"), out);
        });

        it("exports main/1 for a main(args) block", []() {
            auto out = emit("main(args) do\n  42\nend\n");
            assertTrue(contains(out, "'main'/1"), out);
        });

        it("closes the module with 'end'", []() {
            auto out = emit("main do\n  42\nend\n");
            // Last non-whitespace content should be "end"
            auto trimmed = out;
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())))
                trimmed.pop_back();
            assertEqual(trimmed.substr(trimmed.size() - 3), std::string("end"));
        });

        it("exports top-level functions", []() {
            auto out = emit("let greet(name: String) = name\nmain do\n  greet(\"x\")\nend\n");
            assertTrue(contains(out, "'greet'/1"), out);
        });
    });

    describe("CoreErlangEmitter — literals", []() {
        it("emits integer literal", []() {
            auto out = emit("main do\n  42\nend\n");
            assertTrue(contains(out, "42"), out);
        });

        it("emits float literal", []() {
            auto out = emit("main do\n  3.14\nend\n");
            assertTrue(contains(out, "3.14"), out);
        });

        it("emits string literal as quoted charlist", []() {
            auto out = emit("main do\n  \"hello\"\nend\n");
            assertTrue(contains(out, "\"hello\""), out);
        });

        it("emits true as atom 'true'", []() {
            auto out = emit("main do\n  true\nend\n");
            assertTrue(contains(out, "'true'"), out);
        });

        it("emits false as atom 'false'", []() {
            auto out = emit("main do\n  false\nend\n");
            assertTrue(contains(out, "'false'"), out);
        });

        it("emits none as atom 'none'", []() {
            auto out = emit("main do\n  None\nend\n");
            assertTrue(contains(out, "'none'"), out);
        });

        it("emits atom literal", []() {
            auto out = emit("main do\n  :ok\nend\n");
            assertTrue(contains(out, "ok"), out);
        });
    });

    describe("CoreErlangEmitter — binary operators", []() {
        it("emits addition via erlang:'+'", []() {
            auto out = emit("main do\n  1 + 2\nend\n");
            assertTrue(contains(out, "call 'erlang':'+'"), out);
        });

        it("emits subtraction via erlang:'-'", []() {
            auto out = emit("main do\n  5 - 3\nend\n");
            assertTrue(contains(out, "call 'erlang':'-'"), out);
        });

        it("emits multiplication via erlang:'*'", []() {
            auto out = emit("main do\n  2 * 3\nend\n");
            assertTrue(contains(out, "call 'erlang':'*'"), out);
        });

        it("emits division via erlang:'/'", []() {
            auto out = emit("main do\n  10 / 2\nend\n");
            assertTrue(contains(out, "call 'erlang':'/'"), out);
        });

        it("emits equality via erlang:'=:='", []() {
            auto out = emit("main do\n  1 == 1\nend\n");
            assertTrue(contains(out, "call 'erlang':'=:='"), out);
        });

        it("emits less-than via erlang:'<'", []() {
            auto out = emit("main do\n  1 < 2\nend\n");
            assertTrue(contains(out, "call 'erlang':'<'"), out);
        });

        it("emits greater-than via erlang:'>'", []() {
            auto out = emit("main do\n  2 > 1\nend\n");
            assertTrue(contains(out, "call 'erlang':'>'"), out);
        });

        it("emits less-eq via erlang:'=<'", []() {
            auto out = emit("main do\n  1 <= 2\nend\n");
            assertTrue(contains(out, "call 'erlang':'=<'"), out);
        });
    });

    describe("CoreErlangEmitter — stdlib dispatch", []() {
        it("IO.printLine dispatches to kex_io:print_line", []() {
            auto out = emit("main do\n  IO.printLine(\"hi\")\nend\n");
            assertTrue(contains(out, "call 'kex_io':'print_line'"), out);
        });

        it("IO.print dispatches to kex_io:print", []() {
            auto out = emit("main do\n  IO.print(\"hi\")\nend\n");
            assertTrue(contains(out, "call 'kex_io':'print'"), out);
        });

        it("IO.printError dispatches to kex_io:print_error", []() {
            auto out = emit("main do\n  IO.printError(\"bad\")\nend\n");
            assertTrue(contains(out, "call 'kex_io':'print_error'"), out);
        });
    });

    describe("CoreErlangEmitter — let bindings", []() {
        it("let binding becomes Core Erlang let <X> = ... in", []() {
            auto out = emit("main do\n  let x = 1\n  x\nend\n");
            assertTrue(contains(out, "let <X>"), out);
            assertTrue(contains(out, "in"), out);
        });

        it("multiple lets form a chain", []() {
            auto out = emit("main do\n  let x = 1\n  let y = 2\n  x\nend\n");
            auto first  = out.find("let <X>");
            auto second = out.find("let <Y>");
            assertTrue(first  != std::string::npos, "missing let <X>");
            assertTrue(second != std::string::npos, "missing let <Y>");
            assertTrue(first < second, "X should appear before Y");
        });
    });

    describe("CoreErlangEmitter — if expressions", []() {
        it("if/else becomes a case on true/false atoms", []() {
            auto out = emit("main do\n  if true\n    1\n  else\n    2\n  end\nend\n");
            assertTrue(contains(out, "case"), out);
            assertTrue(contains(out, "'true'"), out);
            assertTrue(contains(out, "'false'"), out);
        });
    });

    describe("CoreErlangEmitter — function definitions", []() {
        it("single-clause function emits fun with correct arity", []() {
            auto out = emit("let double(n: Integer) -> Integer = n * 2\nmain do\n  double(3)\nend\n");
            assertTrue(contains(out, "'double'/1"), out);
            assertTrue(contains(out, "fun (_Arg0)"), out);
        });

        it("function body emits param binding then expression", []() {
            auto out = emit("let double(n: Integer) -> Integer = n * 2\nmain do\n  double(3)\nend\n");
            assertTrue(contains(out, "let <N> = _Arg0"), out);
        });

        it("zero-arity function emits fun ()", []() {
            auto out = emit("let answer -> Integer = 42\nmain do\n  answer\nend\n");
            assertTrue(contains(out, "'answer'/0"), out);
        });
    });

    describe("CoreErlangEmitter — tuples and lists", []() {
        it("tuple literal emits {...}", []() {
            auto out = emit("main do\n  (1, 2, 3)\nend\n");
            assertTrue(contains(out, "{1, 2, 3}"), out);
        });

        it("empty list emits []", []() {
            auto out = emit("main do\n  []\nend\n");
            assertTrue(contains(out, "[]"), out);
        });

        it("list literal emits [...]", []() {
            auto out = emit("main do\n  [1, 2, 3]\nend\n");
            assertTrue(contains(out, "[1, 2, 3]"), out);
        });
    });

    describe("CoreErlangEmitter — match expressions", []() {
        it("match emits case with when 'true' guards", []() {
            auto out = emit("main do\n  match 1 do\n    1 -> \"one\"\n    _ -> \"other\"\n  end\nend\n");
            assertTrue(contains(out, "case"), out);
            assertTrue(contains(out, "when 'true'"), out);
        });

        it("tuple subject becomes multi-value case with angle brackets", []() {
            auto out = emit("main do\n  match (1, 2) do\n    (1, 2) -> true\n    (_, _) -> false\n  end\nend\n");
            assertTrue(contains(out, "case <"), out);
            assertTrue(contains(out, "<1, 2>"), out);
        });

        it("no semicolons between match clauses", []() {
            auto out = emit("main do\n  match 1 do\n    1 -> \"one\"\n    _ -> \"other\"\n  end\nend\n");
            assertTrue(contains(out, "->"), "output should contain arrows");
            // Extract the case body and assert no semicolons between clauses
            auto caseStart = out.find("case ");
            auto caseEnd   = out.find("end", caseStart);
            assertTrue(caseStart != std::string::npos, "should have case");
            auto caseText = out.substr(caseStart, caseEnd - caseStart);
            assertFalse(contains(caseText, ";"), "no semicolons between case clauses");
        });
    });

    describe("CoreErlangEmitter — string interpolation", []() {
        it("plain string emits as quoted charlist", []() {
            auto out = emit("main do\n  \"hello\"\nend\n");
            assertTrue(contains(out, "\"hello\""), out);
        });

        it("interpolated string uses kex_io:to_string for embedded expr", []() {
            auto out = emit("main do\n  let n = 42\n  \"value: ${n}\"\nend\n");
            assertTrue(contains(out, "kex_io':'to_string'"), out);
            assertTrue(contains(out, "\"value: \""), out);
        });

        it("interpolated string concatenates with erlang:'++'", []() {
            auto out = emit("main do\n  let n = 42\n  \"${n}!\"\nend\n");
            assertTrue(contains(out, "erlang':'++"), out);
        });
    });

    describe("CoreErlangEmitter — UFCS method dispatch", []() {
        it("modulo maps to erlang:rem", []() {
            auto out = emit("main do\n  let x = 5\n  x.modulo(3)\nend\n");
            assertTrue(contains(out, "call 'erlang':'rem'"), out);
        });

        it("even? maps to erlang:rem =:= 0", []() {
            auto out = emit("main do\n  let x = 4\n  x.even?\nend\n");
            assertTrue(contains(out, "call 'erlang':'rem'"), out);
            assertTrue(contains(out, "'=:='"), out);
        });

        it("each binds lambda and calls lists:foreach", []() {
            auto out = emit("main do\n  [1,2,3].each { |x| IO.printLine(x) }\nend\n");
            assertTrue(contains(out, "call 'lists':'foreach'"), out);
            assertTrue(contains(out, "fun ("), out);
        });

        it("map binds lambda and calls lists:map", []() {
            auto out = emit("main do\n  [1,2,3].map { |x| x }\nend\n");
            assertTrue(contains(out, "call 'lists':'map'"), out);
        });

        it("filter binds lambda and calls lists:filter", []() {
            auto out = emit("main do\n  [1,2,3].filter { |x| true }\nend\n");
            assertTrue(contains(out, "call 'lists':'filter'"), out);
        });

        it("push appends via erlang:'++'", []() {
            auto out = emit("main do\n  let xs = [1]\n  xs.push(2)\nend\n");
            assertTrue(contains(out, "call 'erlang':'++'"), out);
        });
    });

    describe("CoreErlangEmitter — end-to-end compilation", []() {
        // These tests require erlc on PATH. Skip gracefully if not available.
        auto erlcAvailable = []() -> bool {
            return std::system("erlc -help > /dev/null 2>&1") == 0 ||
                   std::system("which erlc > /dev/null 2>&1") == 0;
        };

        it("hello world compiles and runs on BEAM", [&erlcAvailable]() {
            if (!erlcAvailable()) return; // skip
            // Emit .core
            auto out = emit("main do\n  IO.printLine(\"hello from beam\")\nend\n", "e2e_hello");
            // Write to temp file
            std::ofstream f("/tmp/kex_e2e_hello.core");
            f << out;
            f.close();
            // Compile runtime + module
            int r1 = std::system("erlc -o /tmp runtime/src/kex_io.erl > /dev/null 2>&1");
            int r2 = std::system("erlc +from_core -pa /tmp -o /tmp /tmp/kex_e2e_hello.core > /dev/null 2>&1");
            assertEqual(r2, 0, "erlc should succeed");
            // Run it
            int r3 = std::system("erl -noshell -pa /tmp -eval 'kex_e2e_hello:main(), halt()' > /dev/null 2>&1");
            assertEqual(r3, 0, "erl should exit 0");
            (void)r1;
        });

        it("function call compiles and produces correct output", [&erlcAvailable]() {
            if (!erlcAvailable()) return;
            const char* src =
                "let double(n: Integer) -> Integer = n * 2\n"
                "main do\n"
                "  IO.printLine(double(21))\n"
                "end\n";
            auto out = emit(src, "e2e_double");
            std::ofstream f("/tmp/kex_e2e_double.core");
            f << out;
            f.close();
            int r = std::system("erlc +from_core -pa /tmp -o /tmp /tmp/kex_e2e_double.core > /dev/null 2>&1");
            assertEqual(r, 0, "erlc should succeed on double");
            // Capture output
            FILE* pipe = popen("erl -noshell -pa /tmp -eval 'kex_e2e_double:main(), halt()'", "r");
            assertTrue(pipe != nullptr, "popen succeeded");
            char buf[256] = {};
            fgets(buf, sizeof(buf), pipe);
            pclose(pipe);
            std::string actual(buf);
            // trim trailing newline
            while (!actual.empty() && (actual.back() == '\n' || actual.back() == '\r'))
                actual.pop_back();
            assertEqual(actual, std::string("42"), "double(21) should print 42");
        });
    });

    return runAll();
}
