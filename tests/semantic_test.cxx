#include "test.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include "../src/semantic/analyzer.hxx"

using namespace kex;
using namespace test;

auto check(const std::string& source) -> std::vector<semantic::Diagnostic> {
    Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    auto program = parser.parseProgram();

    semantic::Analyzer analyzer;
    analyzer.analyze(program);
    return analyzer.diagnostics();
}

auto hasError(const std::string& source, const std::string& containing = "") -> bool {
    auto diags = check(source);
    for (const auto& d : diags) {
        if (d.level == semantic::Diagnostic::Level::Error) {
            if (containing.empty() || d.message.find(containing) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

auto noErrors(const std::string& source) -> bool {
    auto diags = check(source);
    for (const auto& d : diags) {
        if (d.level == semantic::Diagnostic::Level::Error) return false;
    }
    return true;
}

int main() {
    describe("Semantic — Purity", []() {
        it("allows foul calls in main (implicitly foul)", []() {
            assertTrue(noErrors(
                "foul readFile(path: String) = path\n"
                "main do\n"
                "  readFile(\"test\")\n"
                "end\n"
            ));
        });

        it("rejects foul calls in pure functions", []() {
            assertTrue(hasError(
                "foul readFile(path: String) = path\n"
                "let pure_func(x: Int) do\n"
                "  readFile(\"test\")\n"
                "end\n",
                "foul"
            ));
        });

        it("allows foul calls in foul functions", []() {
            assertTrue(noErrors(
                "foul readFile(path: String) = path\n"
                "foul loadConfig(path: String) do\n"
                "  readFile(path)\n"
                "end\n"
            ));
        });

        it("rejects spawn in pure context", []() {
            assertTrue(hasError(
                "let pure_func(x: Int) do\n"
                "  spawn do\n"
                "    x\n"
                "  end\n"
                "end\n",
                "spawn"
            ));
        });

        it("allows spawn in foul context", []() {
            assertTrue(noErrors(
                "foul start do\n"
                "  spawn do\n"
                "    loop\n"
                "    end\n"
                "  end\n"
                "end\n"
            ));
        });

        it("rejects spawn in main's inner pure function", []() {
            assertTrue(hasError(
                "let pure(x: Int) do\n"
                "  spawn do\n"
                "    x\n"
                "  end\n"
                "end\n",
                "spawn"
            ));
        });
    });

    describe("Semantic — Immutability", []() {
        it("rejects assignment to let binding", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = 5\n"
                "  x = 10\n"
                "end\n",
                "immutable"
            ));
        });

        it("allows assignment to var binding", []() {
            assertTrue(noErrors(
                "main do\n"
                "  var x = 5\n"
                "  x = 10\n"
                "end\n"
            ));
        });

        it("rejects ! on let binding", []() {
            assertTrue(hasError(
                "main do\n"
                "  let list = [1, 2, 3]\n"
                "  list.push!(4)\n"
                "end\n",
                "immutable"
            ));
        });

        it("allows ! on var binding", []() {
            assertTrue(noErrors(
                "main do\n"
                "  var list = [1, 2, 3]\n"
                "  list.push!(4)\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — Type Checking (Literals)", []() {
        it("infers Int for integer literals", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = 5 + 3\n"
                "end\n"
            ));
        });

        it("infers String for string literals", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = \"hello\" + \" world\"\n"
                "end\n"
            ));
        });

        it("infers Float for float literals", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = 3.14 * 2.0\n"
                "end\n"
            ));
        });

        it("infers Bool for boolean ops", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = true && false\n"
                "  let y = 5 > 3\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — Type Checking (Operators)", []() {
        it("rejects Int + String", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = 5 + \"hello\"\n"
                "end\n",
                "Cannot add"
            ));
        });

        it("rejects Bool + Int", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = true + 1\n"
                "end\n",
                "matching types"
            ));
        });

        it("rejects arithmetic on String", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = \"hello\" - \"world\"\n"
                "end\n",
                "arithmetic"
            ));
        });

        it("rejects non-Bool in if condition", []() {
            assertTrue(hasError(
                "main do\n"
                "  if 42\n"
                "    \"oops\"\n"
                "  end\n"
                "end\n",
                "Bool"
            ));
        });

        it("accepts Bool in if condition", []() {
            assertTrue(noErrors(
                "main do\n"
                "  if true\n"
                "    \"ok\"\n"
                "  end\n"
                "end\n"
            ));
        });

        it("rejects non-Bool in logical operators", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = 5 && 3\n"
                "end\n",
                "Logical operator requires Bool"
            ));
        });
    });

    describe("Semantic — Type Checking (Collections)", []() {
        it("rejects heterogeneous list", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = [1, 2, \"three\"]\n"
                "end\n",
                "same type"
            ));
        });

        it("accepts homogeneous list", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = [1, 2, 3]\n"
                "end\n"
            ));
        });

        it("accepts string list", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = [\"a\", \"b\", \"c\"]\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — Type Checking (Assignment)", []() {
        it("rejects type mismatch in var reassignment", []() {
            assertTrue(hasError(
                "main do\n"
                "  var x = 5\n"
                "  x = \"hello\"\n"
                "end\n",
                "Type mismatch"
            ));
        });

        it("accepts same-type var reassignment", []() {
            assertTrue(noErrors(
                "main do\n"
                "  var x = 5\n"
                "  x = 10\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — Modules", []() {
        it("checks purity inside foul module", []() {
            assertTrue(noErrors(
                "foul module IO do\n"
                "  let print(msg: String) = msg\n"
                "end\n"
            ));
        });

        it("modules have isolated scopes", []() {
            // write is defined in IO but not visible in Pure
            // Currently no cross-module resolution — will be added with 'using'
            assertTrue(noErrors(
                "foul module IO do\n"
                "  let write(msg: String) = msg\n"
                "end\n"
                "module Pure do\n"
                "  let process(x: Int) = x\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — Purity (Edge Cases)", []() {
        it("rejects receive in pure context", []() {
            assertTrue(hasError(
                "let handler(x: Int) do\n"
                "  receive do\n"
                "    :msg -> x\n"
                "  end\n"
                "end\n",
                "receive"
            ));
        });

        it("allows receive in foul context", []() {
            assertTrue(noErrors(
                "foul handler(x: Int) do\n"
                "  receive do\n"
                "    :msg -> x\n"
                "  end\n"
                "end\n"
            ));
        });

        it("pure function can call other pure functions", []() {
            assertTrue(noErrors(
                "let double(n: Int) = n * 2\n"
                "let quad(n: Int) = double(double(n))\n"
            ));
        });

        it("foul propagates through call chain", []() {
            assertTrue(hasError(
                "foul read(path: String) = path\n"
                "foul load(path: String) = read(path)\n"
                "let process(path: String) do\n"
                "  load(path)\n"
                "end\n",
                "foul"
            ));
        });
    });

    describe("Semantic — Immutability (Edge Cases)", []() {
        it("rejects reassigning function parameter", []() {
            assertTrue(hasError(
                "let foo(x: Int) do\n"
                "  x = 5\n"
                "end\n",
                "immutable"
            ));
        });

        it("allows var in loop", []() {
            assertTrue(noErrors(
                "main do\n"
                "  var count = 0\n"
                "  count = count + 1\n"
                "  count = count + 1\n"
                "end\n"
            ));
        });

        it("rejects undefined variable assignment", []() {
            assertTrue(hasError(
                "main do\n"
                "  unknown = 5\n"
                "end\n",
                "Undefined variable"
            ));
        });

        it("rejects ! on function parameter", []() {
            assertTrue(hasError(
                "let process(list: [Int]) do\n"
                "  list.push!(4)\n"
                "end\n",
                "immutable"
            ));
        });
    });

    describe("Semantic — Type Checking (Arithmetic)", []() {
        it("accepts Int + Int", []() {
            assertTrue(noErrors("main do\n  let x = 1 + 2\nend\n"));
        });

        it("accepts Float + Float", []() {
            assertTrue(noErrors("main do\n  let x = 1.0 + 2.5\nend\n"));
        });

        it("accepts Int * Int", []() {
            assertTrue(noErrors("main do\n  let x = 3 * 4\nend\n"));
        });

        it("accepts Float / Float", []() {
            assertTrue(noErrors("main do\n  let x = 10.0 / 3.0\nend\n"));
        });

        it("rejects String * String", []() {
            assertTrue(hasError(
                "main do\n  let x = \"a\" * \"b\"\nend\n",
                "arithmetic"
            ));
        });

        it("rejects String - String", []() {
            assertTrue(hasError(
                "main do\n  let x = \"a\" - \"b\"\nend\n",
                "arithmetic"
            ));
        });

        it("accepts String + String", []() {
            assertTrue(noErrors("main do\n  let x = \"a\" + \"b\"\nend\n"));
        });

        it("promotes Int to Float in mixed arithmetic", []() {
            assertTrue(noErrors("main do\n  let x = 1 + 2.0\nend\n"));
        });
    });

    describe("Semantic — Type Checking (Comparison)", []() {
        it("accepts Int == Int", []() {
            assertTrue(noErrors("main do\n  let x = 1 == 2\nend\n"));
        });

        it("accepts String == String", []() {
            assertTrue(noErrors("main do\n  let x = \"a\" == \"b\"\nend\n"));
        });

        it("accepts Int < Int", []() {
            assertTrue(noErrors("main do\n  let x = 1 < 2\nend\n"));
        });

        it("accepts Int >= Int", []() {
            assertTrue(noErrors("main do\n  let x = 5 >= 3\nend\n"));
        });

        it("rejects Bool < Bool", []() {
            assertTrue(hasError(
                "main do\n  let x = true < false\nend\n",
                "Cannot compare Bool"
            ));
        });

        it("comparison returns Bool", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = 5 > 3\n"
                "  if x\n"
                "    1\n"
                "  end\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — Type Checking (Logical)", []() {
        it("accepts Bool && Bool", []() {
            assertTrue(noErrors("main do\n  let x = true && false\nend\n"));
        });

        it("accepts Bool || Bool", []() {
            assertTrue(noErrors("main do\n  let x = true || false\nend\n"));
        });

        it("rejects Int && Int", []() {
            assertTrue(hasError(
                "main do\n  let x = 1 && 2\nend\n",
                "Logical operator requires Bool"
            ));
        });

        it("rejects String || String", []() {
            assertTrue(hasError(
                "main do\n  let x = \"a\" || \"b\"\nend\n",
                "Logical operator requires Bool"
            ));
        });

        it("rejects ! on non-Bool", []() {
            assertTrue(hasError(
                "main do\n  let x = !5\nend\n",
                "Logical not '!' requires Bool"
            ));
        });

        it("accepts ! on Bool", []() {
            assertTrue(noErrors("main do\n  let x = !true\nend\n"));
        });
    });

    describe("Semantic — Type Checking (If/Elif)", []() {
        it("rejects String as condition", []() {
            assertTrue(hasError(
                "main do\n  if \"yes\"\n    1\n  end\nend\n",
                "Bool"
            ));
        });

        it("rejects Float as condition", []() {
            assertTrue(hasError(
                "main do\n  if 3.14\n    1\n  end\nend\n",
                "Bool"
            ));
        });

        it("accepts comparison as condition", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let n = 5\n"
                "  if n > 0\n"
                "    n\n"
                "  end\n"
                "end\n"
            ));
        });

        it("accepts negation as condition", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let done = false\n"
                "  if !done\n"
                "    1\n"
                "  end\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — Type Checking (Collections)", []() {
        it("accepts empty list", []() {
            assertTrue(noErrors("main do\n  let x = []\nend\n"));
        });

        it("rejects [Int, String] mix", []() {
            assertTrue(hasError(
                "main do\n  let x = [1, \"two\", 3]\nend\n",
                "same type"
            ));
        });

        it("accepts nested homogeneous list", []() {
            assertTrue(noErrors(
                "main do\n  let x = [[1, 2], [3, 4]]\nend\n"
            ));
        });

        it("accepts map literal", []() {
            assertTrue(noErrors(
                "main do\n  let x = { \"a\": 1, \"b\": 2 }\nend\n"
            ));
        });

        it("accepts tuple with mixed types", []() {
            assertTrue(noErrors(
                "main do\n  let x = (1, \"hello\", true)\nend\n"
            ));
        });

        it("accepts range", []() {
            assertTrue(noErrors("main do\n  let x = 1..10\nend\n"));
        });
    });

    describe("Semantic — Type Checking (Let Binding)", []() {
        it("propagates type through let", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = 5\n"
                "  let y = x + 3\n"
                "end\n"
            ));
        });

        it("detects type error after let binding", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = 5\n"
                "  let y = x + \"hello\"\n"
                "end\n",
                "Cannot add"
            ));
        });

        it("propagates Bool type through let", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let flag = true\n"
                "  if flag\n"
                "    1\n"
                "  end\n"
                "end\n"
            ));
        });

        it("propagates String type through let", []() {
            assertTrue(hasError(
                "main do\n"
                "  let name = \"Alice\"\n"
                "  let x = name - 1\n"
                "end\n",
                "arithmetic"
            ));
        });
    });

    describe("Semantic — Var Reassignment Types", []() {
        it("rejects Int var assigned String", []() {
            assertTrue(hasError(
                "main do\n"
                "  var x = 0\n"
                "  x = \"oops\"\n"
                "end\n",
                "Type mismatch"
            ));
        });

        it("rejects Bool var assigned Int", []() {
            assertTrue(hasError(
                "main do\n"
                "  var flag = true\n"
                "  flag = 42\n"
                "end\n",
                "Type mismatch"
            ));
        });

        it("accepts Int var assigned Int", []() {
            assertTrue(noErrors(
                "main do\n"
                "  var x = 0\n"
                "  x = 1\n"
                "  x = 2\n"
                "end\n"
            ));
        });

        it("accepts String var assigned String", []() {
            assertTrue(noErrors(
                "main do\n"
                "  var name = \"hello\"\n"
                "  name = \"world\"\n"
                "end\n"
            ));
        });
    });

    return runAll();
}
