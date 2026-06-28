#include "test.hxx"
#include "../src/codegen/core_erlang.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"

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
        it("let binding becomes Core Erlang let...in", []() {
            auto out = emit("main do\n  let x = 1\n  x\nend\n");
            assertTrue(contains(out, "let X ="), out);
            assertTrue(contains(out, "in"), out);
        });

        it("multiple lets form a chain", []() {
            auto out = emit("main do\n  let x = 1\n  let y = 2\n  x\nend\n");
            // Should have at least two let bindings
            auto first  = out.find("let X =");
            auto second = out.find("let Y =");
            assertTrue(first  != std::string::npos, "missing let X");
            assertTrue(second != std::string::npos, "missing let Y");
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
            // Param 'n' should be bound from _Arg0
            assertTrue(contains(out, "let N = _Arg0"), out);
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

    return runAll();
}
