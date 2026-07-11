#include "test.hxx"
#include "../src/codegen/core_erlang.hxx"
#include "../src/ir/emit_core.hxx"
#include "../src/ir/lower.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>

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

// Exercise the current AST -> IR -> Core Erlang pipeline used by `kex -R`.
auto emitIr(const std::string& source, const std::string& stem = "test") -> std::string {
    kex::Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    kex::Parser parser(std::move(tokens));
    auto program = parser.parseProgram();
    return kex::ir::emitCore(kex::ir::lowerProgram(program, stem)).source;
}

// Compile an IR-pipeline Core Erlang module and return main/0's value. The
// sources below deliberately avoid prelude calls, so this unit path stays
// independent of main.cxx's prelude-loading bootstrap.
auto runIrOnBeam(const std::string& source, const std::string& stem) -> std::string {
    auto corePath = "/tmp/kex_" + stem + ".core";
    std::ofstream core(corePath);
    core << emitIr(source, stem);
    core.close();

    auto compile = "erlc +from_core -pa runtime/beam -o /tmp " + corePath +
                   " > /dev/null 2>&1";
    assertEqual(std::system(compile.c_str()), 0, "erlc should compile IR output");

    auto run = "erl -noshell -pa runtime/beam -pa /tmp -eval 'io:format(\"~p~n\", [kex_" + stem +
               ":main()]), halt()'";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(run.c_str(), "r"), pclose);
    assertTrue(pipe != nullptr, "popen should run the BEAM module");
    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe.get())) output += buf;
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return output;
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

        it("zero-arg variant tag emits as bare atom", []() {
            // Less (zero-arg variant) → 'Less' in Core Erlang
            auto out = emit("main do\n  Less\nend\n");
            assertTrue(contains(out, "'Less'"), out);
        });

        it("constructor pattern with args emits as tagged tuple", []() {
            // @Ok(x) in match → {'Ok', X} in Core Erlang
            auto out = emit(
                "let f(v) = match v do\n"
                "  @Ok(x) -> x\n"
                "  @Error(e) -> e\n"
                "end\n"
                "main do f(42) end\n"
            );
            assertTrue(contains(out, "{'Ok'"), out);
            assertTrue(contains(out, "{'Error'"), out);
        });

        it("zero-arg constructor pattern emits as bare atom", []() {
            // @Less in match → 'Less' in Core Erlang
            auto out = emit(
                "let f(v) = match v do\n"
                "  @Less -> -1\n"
                "  @Equal -> 0\n"
                "  @Greater -> 1\n"
                "end\n"
                "main do f(Less) end\n"
            );
            assertTrue(contains(out, "'Less'"), out);
            assertTrue(contains(out, "'Equal'"), out);
            assertTrue(contains(out, "'Greater'"), out);
        });
    });

    describe("CoreErlangEmitter — binary operators", []() {
         it("emits addition via kex_intrinsic_number:add (polymorphic + for numbers and strings)", []() {
            auto out = emit("main do\n  1 + 2\nend\n");
            // + dispatches through kex_intrinsic_number:add/2 which handles both number
            // addition (erlang:'+') and string concatenation (erlang:'++')
            // at runtime based on the operand types.
            assertTrue(contains(out, "call 'kex_intrinsic_number':'add'"), out);
        });

        it("emits subtraction via erlang:'-'", []() {
            auto out = emit("main do\n  5 - 3\nend\n");
            assertTrue(contains(out, "call 'erlang':'-'"), out);
        });

        it("emits multiplication via erlang:'*'", []() {
            auto out = emit("main do\n  2 * 3\nend\n");
            assertTrue(contains(out, "call 'erlang':'*'"), out);
        });

        it("emits division via kex_intrinsic_number:divide", []() {
            auto out = emit("main do\n  4 / 2\nend\n");
            assertTrue(contains(out, "call 'kex_intrinsic_number':'divide'"), out);
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

    describe("IR Core Erlang lowering — branch state", []() {
        it("preserves both reassigned variables after if and elif branches", []() {
            auto output = runIrOnBeam(
                "main do\n"
                "  var left = 3\n"
                "  var right = 5\n"
                "  if false\n"
                "    left = 9\n"
                "    right = 7\n"
                "  elif true\n"
                "    left = left + 2\n"
                "    right = right + 4\n"
                "  else\n"
                "    left = 1\n"
                "    right = 2\n"
                "  end\n"
                "  left * right\n"
                "end\n",
                "branch_state");
            assertEqual(output, std::string("45"));
        });

        it("preserves reassigned state after a match clause", []() {
            auto output = runIrOnBeam(
                "main do\n"
                "  var value = 10\n"
                "  match :two do\n"
                "    :one -> value = 1\n"
                "    :two -> value = value + 2\n"
                "  end\n"
                "  value\n"
                "end\n",
                "match_state");
            assertEqual(output, std::string("12"));
        });

        it("threads captured mutable state through each callbacks", []() {
            auto output = runIrOnBeam(
                "main do\n"
                "  var total = 0\n"
                "  [1, 2, 3].each do |n|\n"
                "    total = total + n\n"
                "  end\n"
                "  total\n"
                "end\n",
                "each_state");
            assertEqual(output, std::string("6"));
        });

        it("matches literal fields in record-pattern function heads", []() {
            auto output = runIrOnBeam(
                "record Point do\n"
                "  x : Float\n"
                "  y : Float\n"
                "end\n"
                "make Point do\n"
                "  let origin?({ x: 0.0, y: 0.0 }) = true\n"
                "  let origin?({ }) = false\n"
                "end\n"
                "main do\n"
                "  origin?(Point { x: 0.0, y: 0.0 })\n"
                "end\n",
                "record_pattern");
            assertEqual(output, std::string("true"));
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
        it("modulo routes to kex_prelude", []() {
            auto out = emit("main do\n  let x = 5\n  x.modulo(3)\nend\n");
            assertTrue(contains(out, "call 'kex_prelude':'modulo'"), out);
        });

        it("even? routes to kex_prelude", []() {
            auto out = emit("main do\n  let x = 4\n  x.even?\nend\n");
            assertTrue(contains(out, "call 'kex_prelude':'even?'"), out);
        });

        it("each binds lambda and calls lists:foreach", []() {
            auto out = emit("main do\n  [1,2,3].each { |x| IO.printLine(x) }\nend\n");
            assertTrue(contains(out, "call 'kex_prelude':'each'"), out);
        });

        it("map routes to kex_prelude", []() {
            auto out = emit("main do\n  [1,2,3].map { |x| x }\nend\n");
            assertTrue(contains(out, "call 'kex_prelude':'map'"), out);
        });

        it("filter routes to kex_prelude", []() {
            auto out = emit("main do\n  [1,2,3].filter { |x| true }\nend\n");
            assertTrue(contains(out, "call 'kex_prelude':'filter'"), out);
        });

        it("push routes to kex_prelude", []() {
            auto out = emit("main do\n  let xs = [1]\n  xs.push(2)\nend\n");
            assertTrue(contains(out, "call 'kex_prelude':'push'"), out);
        });
    });

    describe("CoreErlangEmitter — process primitives", []() {
        it("spawn emits erlang:spawn with a 0-arity fun", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul go do\n"
                "  spawn do IO.printLine(\"child\") end\n"
                "end\n"
                "main do go() end\n"
            );
            assertTrue(contains(out, "call 'erlang':'spawn'"), out);
        });

        it("receive emits native Core Erlang receive with after infinity", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul loop do\n"
                "  receive do\n"
                "    :stop -> IO.printLine(\"done\")\n"
                "  end\n"
                "end\n"
                "main do spawn do loop() end end\n"
            );
            assertTrue(contains(out, "receive"), out);
            assertTrue(contains(out, "after 'infinity'"), out);
        });

        it("receive with timeout emits after clause with the given timeout", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul waitOne do\n"
                "  receive timeout: 1000 do\n"
                "    :ok -> IO.printLine(\"got it\")\n"
                "  after -> IO.printLine(\"timeout\")\n"
                "  end\n"
                "end\n"
                "main do waitOne() end\n"
            );
            assertTrue(contains(out, "receive"), out);
            assertTrue(contains(out, "after 1000"), out);
        });

        it("pid.send(msg) routes to kex_prelude", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul go(pid) do\n"
                "  pid.send(:ping)\n"
                "end\n"
                "main do go(self()) end\n"
            );
            assertTrue(contains(out, "call 'kex_prelude':'send'"), out);
        });

        it("send(pid, msg) free function emits erlang:send", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul go(pid) do\n"
                "  send(pid, :ping)\n"
                "end\n"
                "main do go(self()) end\n"
            );
            assertTrue(contains(out, "call 'erlang':'send'"), out);
        });

        it("self() emits erlang:self()", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul myPid do self() end\n"
                "main do myPid() end\n"
            );
            assertTrue(contains(out, "call 'erlang':'self'()"), out);
        });

        it("pid.link() routes to kex_prelude", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul go(pid) do pid.link() end\n"
                "main do go(self()) end\n"
            );
            assertTrue(contains(out, "call 'kex_prelude':'link'"), out);
        });

        it("pid.alive?() routes to kex_prelude", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul check(pid) do pid.alive?() end\n"
                "main do check(self()) end\n"
            );
            assertTrue(contains(out, "call 'kex_prelude':'alive?'"), out);
        });

        it("Task.start { block } emits kex_task:start/1", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul go do\n"
                "  Task.start { IO.printLine(\"async\") }\n"
                "end\n"
                "main do go() end\n"
            );
            assertTrue(contains(out, "call 'kex_task':'start'"), out);
        });

        it("task.await() routes to kex_prelude with infinity", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul go do\n"
                "  let t = Task.start { 42 }\n"
                "  t.await()\n"
                "end\n"
                "main do go() end\n"
            );
            assertTrue(contains(out, "call 'kex_prelude':'await'"), out);
        });

        it("task.await(timeout: N) routes to kex_prelude with timeout", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul go do\n"
                "  let t = Task.start { 42 }\n"
                "  t.await(timeout: 5000)\n"
                "end\n"
                "main do go() end\n"
            );
            assertTrue(contains(out, "call 'kex_prelude':'await'"), out);
        });

        it("Supervisor.start(strategy:) do block end emits kex_supervisor:start_link", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul startWorker do\n"
                "  spawn do receive do :stop -> \"done\" end end\n"
                "end\n"
                "main do\n"
                "  Supervisor.start(strategy: :only_crashed) do\n"
                "    [worker { startWorker() }]\n"
                "  end\n"
                "end\n"
            );
            assertTrue(contains(out, "call 'kex_supervisor':'start_link'"), out);
            assertTrue(contains(out, "'strategy'"), out);
        });

        it("worker { block } emits kex_supervisor:worker/1", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul w do\n"
                "  worker { spawn do receive do :stop -> \"done\" end end }\n"
                "end\n"
                "main do w() end\n"
            );
            assertTrue(contains(out, "call 'kex_supervisor':'worker'"), out);
        });

        it("worker(Module) MPA form emits kex_MODULE:start with no args", []() {
            auto out = emit(
                "# kex: no-check\n"
                "main do\n"
                "  Supervisor.start(strategy: :only_crashed) do\n"
                "    [worker(Cache)]\n"
                "  end\n"
                "end\n"
            );
            assertTrue(contains(out, "call 'kex_cache':'start'()"), out);
            assertTrue(contains(out, "call 'kex_supervisor':'worker'"), out);
        });

        it("worker(Module, args: [...]) MPA form passes args to kex_MODULE:start", []() {
            auto out = emit(
                "# kex: no-check\n"
                "main do\n"
                "  Supervisor.start(strategy: :only_crashed) do\n"
                "    [worker(Database, args: [\"postgres://localhost\"])]\n"
                "  end\n"
                "end\n"
            );
            assertTrue(contains(out, "call 'kex_database':'start'"), out);
            assertTrue(contains(out, "\"postgres://localhost\""), out);
        });

        it("supervisor(strategy:) do block end as free fn emits nested start_link", []() {
            auto out = emit(
                "# kex: no-check\n"
                "main do\n"
                "  Supervisor.start(strategy: :only_crashed) do\n"
                "    [supervisor(strategy: :all) do\n"
                "      [worker(WebServer)]\n"
                "    end]\n"
                "  end\n"
                "end\n"
            );
            // The nested supervisor should also call start_link
            auto count = 0;
            std::string::size_type pos = 0;
            while ((pos = out.find("kex_supervisor':'start_link'", pos)) != std::string::npos) {
                count++;
                pos++;
            }
            assertTrue(count >= 2, "expected at least 2 start_link calls (outer + nested)");
        });

        it("Process.self without parens emits erlang:self()", []() {
            auto out = emit(
                "# kex: no-check\n"
                "foul go do\n"
                "  let me = Process.self\n"
                "  me\n"
                "end\n"
                "main do go() end\n"
            );
            assertTrue(contains(out, "call 'erlang':'self'()"), out);
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
            // Emit .core (ctest runs from build dir, so runtime/beam is a valid relative path)
            auto out = emit("main do\n  IO.printLine(\"hello from beam\")\nend\n", "e2e_hello");
            std::ofstream f("/tmp/kex_e2e_hello.core");
            f << out;
            f.close();
            int r2 = std::system("erlc +from_core -pa runtime/beam -o /tmp /tmp/kex_e2e_hello.core > /dev/null 2>&1");
            assertEqual(r2, 0, "erlc should succeed");
            int r3 = std::system("erl -noshell -pa runtime/beam -pa /tmp -eval 'kex_e2e_hello:main(), halt()' > /dev/null 2>&1");
            assertEqual(r3, 0, "erl should exit 0");
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
            int r = std::system("erlc +from_core -pa runtime/beam -o /tmp /tmp/kex_e2e_double.core > /dev/null 2>&1");
            assertEqual(r, 0, "erlc should succeed on double");
            FILE* pipe = popen("erl -noshell -pa runtime/beam -pa /tmp -eval 'kex_e2e_double:main(), halt()'", "r");
            assertTrue(pipe != nullptr, "popen succeeded");
            char buf[256] = {};
            fgets(buf, sizeof(buf), pipe);
            pclose(pipe);
            std::string actual(buf);
            while (!actual.empty() && (actual.back() == '\n' || actual.back() == '\r'))
                actual.pop_back();
            assertEqual(actual, std::string("42"), "double(21) should print 42");
        });
    });

    return runAll();
}
