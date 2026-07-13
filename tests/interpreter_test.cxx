#include "test.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include "../src/interpreter/evaluator.hxx"
#include <cstdlib>
#include <filesystem>
#include <sstream>

using namespace kex;
using namespace kex::interpreter;
using namespace test;

auto run(const std::string& source) -> ValuePtr {
    Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    auto program = parser.parseProgram();
    Evaluator evaluator;
    return evaluator.execute(program);
}

auto runOutput(const std::string& source) -> std::string {
    Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    auto program = parser.parseProgram();
    Evaluator evaluator;
    evaluator.execute(program);
    return evaluator.output();
}

auto runWithArgs(const std::string& source, std::vector<std::string> args) -> ValuePtr {
    Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    auto program = parser.parseProgram();
    Evaluator evaluator;
    evaluator.setArgs(std::move(args));
    return evaluator.execute(program);
}

auto runWithModuleRoots(const std::string& source, std::vector<std::string> roots) -> ValuePtr {
    Lexer lexer(source);
    Parser parser(lexer.tokenizeAll());
    auto program = parser.parseProgram();
    Evaluator evaluator;
    evaluator.setModuleRoots(std::move(roots));
    return evaluator.execute(program);
}

int main() {
    describe("Interpreter — Modules", []() {
        it("imports selected module exports with using", []() {
            auto result = run(
                "module Util do\n"
                "  let twice(n) = n * 2\n"
                "end\n"
                "main do\n"
                "  using Util, only: [twice]\n"
                "  twice(21)\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("calls a module function through its qualified name", []() {
            auto result = run(
                "module Util do\n"
                "  let twice(n) = n * 2\n"
                "end\n"
                "main do\n"
                "  Util.twice(21)\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("discovers and loads an unresolved module from a source root", []() {
            namespace fs = std::filesystem;
            const auto root = fs::temp_directory_path() / "kex-module-load-test";
            fs::remove_all(root);
            fs::create_directories(root / "http");
            {
                std::ofstream module(root / "http/router.kex");
                module << "module Http.Router\n"
                          "let twice(n) = n * 2\n";
            }
            auto result = runWithModuleRoots(
                "main do\n"
                "  using Http.Router, only: [twice]\n"
                "  twice(21)\n"
                "end\n",
                {root.string()});
            fs::remove_all(root);
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("resolves a relative using from the enclosing module", []() {
            namespace fs = std::filesystem;
            const auto root = fs::temp_directory_path() / "kex-relative-module-load-test";
            fs::remove_all(root);
            fs::create_directories(root / "http");
            {
                std::ofstream module(root / "http/router.kex");
                module << "module Http.Router\n"
                          "let twice(n) = n * 2\n";
            }
            auto result = runWithModuleRoots(
                "module Http do\n"
                "  using Router, only: [twice]\n"
                "  let answer() = twice(21)\n"
                "end\n"
                "main do\n"
                "  Http.answer()\n"
                "end\n",
                {root.string()});
            fs::remove_all(root);
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("loads a nested module from its containing module file", []() {
            namespace fs = std::filesystem;
            const auto root = fs::temp_directory_path() / "kex-container-module-load-test";
            fs::remove_all(root);
            fs::create_directories(root);
            {
                std::ofstream module(root / "shop.kex");
                module << "module Shop do\n"
                          "  module Cart do\n"
                          "    let total() = 42\n"
                          "  end\n"
                          "end\n";
            }
            auto result = runWithModuleRoots(
                "main do\n"
                "  using Shop.Cart, only: [total]\n"
                "  total()\n"
                "end\n",
                {root.string()});
            fs::remove_all(root);
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("allows private module functions only from within their module", []() {
            auto result = run(
                "module Secrets do\n"
                "  private do\n"
                "    let hidden() = 42\n"
                "  end\n"
                "  public do\n"
                "    let reveal() = hidden()\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  Secrets.reveal()\n"
                "end\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));

            bool rejected = false;
            try {
                run(
                    "module Secrets do\n"
                    "  private do\n"
                    "    let hidden() = 42\n"
                    "  end\n"
                    "end\n"
                    "main do\n"
                    "  Secrets.hidden()\n"
                    "end\n");
            } catch (const RuntimeError& error) {
                rejected = std::string(error.what()).find("private name `hidden`") != std::string::npos;
            }
            assertTrue(rejected);
        });

        it("rejects importing a private name explicitly", []() {
            bool rejected = false;
            try {
                run(
                    "module Secrets do\n"
                    "  private do\n"
                    "    let hidden() = 42\n"
                    "  end\n"
                    "end\n"
                    "main do\n"
                    "  using Secrets, only: [hidden]\n"
                    "end\n");
            } catch (const RuntimeError& error) {
                rejected = std::string(error.what()).find("private name `hidden`") != std::string::npos;
            }
            assertTrue(rejected);
        });

        it("resolves a filtered forward re-export as a nested namespace", []() {
            auto result = run(
                "module App do\n"
                "  export Http.Methods, as: Methods, only: [get]\n"
                "end\n"
                "module Http.Methods do\n"
                "  let get() = 42\n"
                "  let post() = 99\n"
                "end\n"
                "main do\n"
                "  App.Methods.get()\n"
                "end\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("rejects exporting a module's private name", []() {
            bool rejected = false;
            try {
                run(
                    "module App do\n"
                    "  export Secrets, only: [hidden]\n"
                    "end\n"
                    "module Secrets do\n"
                    "  private do\n"
                    "    let hidden() = 42\n"
                    "  end\n"
                    "end\n");
            } catch (const RuntimeError& error) {
                rejected = std::string(error.what()).find("cannot export private name `hidden`")
                    != std::string::npos;
            }
            assertTrue(rejected);
        });

        it("rejects a module exporting itself", []() {
            bool rejected = false;
            try {
                run(
                    "module App do\n"
                    "  export App\n"
                    "end\n");
            } catch (const RuntimeError& error) {
                rejected = std::string(error.what()).find("module cannot export itself")
                    != std::string::npos;
            }
            assertTrue(rejected);
        });

        it("uses an import alias for qualified access", []() {
            auto result = run(
                "module Util do\n"
                "  let twice(n) = n * 2\n"
                "end\n"
                "main do\n"
                "  using Util, as: U\n"
                "  U.twice(21)\n"
                "end\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("does not leak names from a scoped using block", []() {
            bool rejected = false;
            try {
                run(
                    "module Util do\n"
                    "  let twice(n) = n * 2\n"
                    "end\n"
                    "main do\n"
                    "  using Util do\n"
                    "    twice(10)\n"
                    "  end\n"
                    "  twice(21)\n"
                    "end\n");
            } catch (const RuntimeError& error) {
                rejected = std::string(error.what()).find("Undefined function: twice")
                    != std::string::npos;
            }
            assertTrue(rejected);
        });

        it("lets an explicit import override a wildcard import", []() {
            auto result = run(
                "module A do\n"
                "  let value() = 1\n"
                "end\n"
                "module B do\n"
                "  let value() = 2\n"
                "end\n"
                "main do\n"
                "  using A\n"
                "  using B, only: [value]\n"
                "  value()\n"
                "end\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(2));
        });

        it("rejects ambiguous wildcard imports", []() {
            bool rejected = false;
            try {
                run(
                    "module A do\n"
                    "  let value() = 1\n"
                    "end\n"
                    "module B do\n"
                    "  let value() = 2\n"
                    "end\n"
                    "main do\n"
                    "  using A\n"
                    "  using B\n"
                    "end\n");
            } catch (const RuntimeError& error) {
                rejected = std::string(error.what()).find("ambiguous name `value`")
                    != std::string::npos;
            }
            assertTrue(rejected);
        });

        it("keeps imports inside a visibility block positional and scoped", []() {
            auto inside = run(
                "module Util do\n"
                "  let value() = 42\n"
                "end\n"
                "module App do\n"
                "  private do\n"
                "    using Util, only: [value]\n"
                "    let inside() = value()\n"
                "  end\n"
                "  let reveal() = inside()\n"
                "end\n"
                "main do\n"
                "  App.reveal()\n"
                "end\n");
            assertEqual(std::get<IntValue>(inside->data).value, int64_t(42));

            bool outsideRejected = false;
            try {
                run(
                    "module Util do\n"
                    "  let value() = 42\n"
                    "end\n"
                    "module App do\n"
                    "  private do\n"
                    "    using Util, only: [value]\n"
                    "  end\n"
                    "  let outside() = value()\n"
                    "end\n"
                    "main do\n"
                    "  App.outside()\n"
                    "end\n");
            } catch (const RuntimeError& error) {
                outsideRejected = std::string(error.what()).find("Undefined function: value")
                    != std::string::npos;
            }
            assertTrue(outsideRejected);
        });
    });

    describe("Interpreter — Literals", []() {
        it("evaluates integers", []() {
            auto result = run("main do\n  42\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("evaluates floats", []() {
            auto result = run("main do\n  3.14\nend\n");
            assertTrue(std::get<FloatValue>(result->data).value > 3.13);
        });

        it("evaluates strings", []() {
            auto result = run("main do\n  \"hello\"\nend\n");
            assertEqual(std::get<StringValue>(result->data).value, std::string("hello"));
        });

        it("evaluates booleans", []() {
            auto result = run("main do\n  true\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
        });

        it("evaluates atoms", []() {
            auto result = run("main do\n  :ok\nend\n");
            assertEqual(std::get<AtomValue>(result->data).name, std::string("ok"));
        });

        it("evaluates None", []() {
            auto result = run("main do\n  None\nend\n");
            assertTrue(result->isNone());
        });
    });

    describe("Interpreter — Arithmetic", []() {
        it("adds integers", []() {
            auto result = run("main do\n  5 + 3\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(8));
        });

        it("subtracts integers", []() {
            auto result = run("main do\n  10 - 4\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(6));
        });

        it("multiplies integers", []() {
            auto result = run("main do\n  6 * 7\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("divides integers", []() {
            auto result = run("main do\n  10 / 3\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(3));
        });

        it("modulo", []() {
            auto result = run("main do\n  10 % 3\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(1));
        });

        it("concatenates strings with +", []() {
            auto result = run("main do\n  \"hello\" + \" world\"\nend\n");
            assertEqual(std::get<StringValue>(result->data).value, std::string("hello world"));
        });

        it("handles mixed int/float arithmetic", []() {
            auto result = run("main do\n  1 + 2.5\nend\n");
            assertTrue(std::get<FloatValue>(result->data).value > 3.4);
        });

        it("negates integers", []() {
            auto result = run("main do\n  -5\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(-5));
        });
    });

    describe("Interpreter — Arbitrary precision Integer", []() {
        it("stays a plain Int for values that fit in int64_t", []() {
            auto result = run("main do\n  5 + 3\nend\n");
            assertTrue(std::holds_alternative<IntValue>(result->data));
        });

        it("promotes + to BigIntValue on int64_t overflow", []() {
            auto result = run("main do\n  9223372036854775807 + 1\nend\n");
            assertTrue(std::holds_alternative<BigIntValue>(result->data));
            assertEqual(result->toString(), std::string("9223372036854775808"));
        });

        it("promotes - on underflow (negating beyond INT64_MIN)", []() {
            auto result = run("main do\n  -9223372036854775807 - 2\nend\n");
            assertTrue(std::holds_alternative<BigIntValue>(result->data));
            assertEqual(result->toString(), std::string("-9223372036854775809"));
        });

        it("promotes * on overflow", []() {
            auto result = run("main do\n  9223372036854775807 * 2\nend\n");
            assertTrue(std::holds_alternative<BigIntValue>(result->data));
        });

        it("parses a literal too large for int64_t directly as a bignum", []() {
            auto result = run("main do\n  100000000000000000000\nend\n");
            assertTrue(std::holds_alternative<BigIntValue>(result->data));
            assertEqual(result->toString(), std::string("100000000000000000000"));
        });

        it("computes factorial(25), far beyond int64_t range", []() {
            auto result = run(
                "let factorial(n) do\n"
                "  if n <= 1\n"
                "    1\n"
                "  else\n"
                "    n * factorial(n - 1)\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  factorial(25)\n"
                "end\n"
            );
            assertEqual(result->toString(), std::string("15511210043330985984000000"));
        });

        it("demotes a bignum result back to Int once it fits again", []() {
            auto result = run("main do\n  let big = 100000000000000000000\n  big - big\nend\n");
            assertTrue(std::holds_alternative<IntValue>(result->data));
            assertEqual(std::get<IntValue>(result->data).value, int64_t(0));
        });

        it("negates a bignum", []() {
            auto result = run("main do\n  let big = 100000000000000000000\n  -big\nend\n");
            assertEqual(result->toString(), std::string("-100000000000000000000"));
        });

        it("compares Int against BigIntValue correctly", []() {
            auto result = run("main do\n  let big = 100000000000000000000\n  5 < big\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
        });

        it("equates a bignum with an equal-valued Int-shaped literal", []() {
            auto result = run(
                "main do\n  let big = 100000000000000000000\n  big == 100000000000000000000\nend\n"
            );
            assertTrue(std::get<BoolValue>(result->data).value);
        });

        it("divides and computes modulo on a bignum", []() {
            assertEqual(
                run("main do\n  100000000000000000000 / 3\nend\n")->toString(),
                std::string("33333333333333333333")
            );
            assertEqual(
                run("main do\n  100000000000000000000 % 7\nend\n")->toString(),
                std::string("2")
            );
        });

        it("matches a match clause against a big integer literal pattern", []() {
            auto result = run(
                "main do\n"
                "  let big = 100000000000000000000\n"
                "  match big do\n"
                "    100000000000000000000 -> \"matched\"\n"
                "    _ -> \"no match\"\n"
                "  end\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("matched"));
        });

        it("even?/odd? work on a bignum", []() {
            assertTrue(std::get<BoolValue>(
                run("main do\n  100000000000000000000.even?\nend\n")->data).value);
            assertTrue(std::get<BoolValue>(
                run("main do\n  (100000000000000000000 + 1).odd?\nend\n")->data).value);
        });

        it("abs works on a negative bignum", []() {
            auto result = run("main do\n  (-100000000000000000000).abs\nend\n");
            assertEqual(result->toString(), std::string("100000000000000000000"));
        });

        it("Integer.parse handles a string too large for int64_t", []() {
            auto result = run(
                "main do\n"
                "  match Integer.parse(\"999999999999999999999999\") do\n"
                "    Ok(n) -> n\n"
                "    Error(e) -> e\n"
                "  end\n"
                "end\n"
            );
            assertEqual(result->toString(), std::string("999999999999999999999999"));
        });

        it("Integer.parse returns a typed ParseError on invalid input", []() {
            auto result = run(
                "main do\n"
                "  match Integer.parse(\"abc\") do\n"
                "    Ok(_) -> -1\n"
                "    Error(e) -> e.position\n"
                "  end\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(0));
        });

        it("Integer.parse returns value and rest on trailing", []() {
            auto result = run(
                "main do\n"
                "  match Integer.parse(\"12ab\") do\n"
                "    Ok(_) -> -1\n"
                "    Error(e) -> e.value\n"
                "  end\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(12));
        });

        it("Integer.parsePrefix returns Just((value, rest))", []() {
            auto result = run(
                "main do\n"
                "  let Just(pair) = Integer.parsePrefix(\"123xyz\")\n"
                "  pair\n"
                "end\n"
            );
            auto& tup = std::get<TupleValue>(result->data);
            assertEqual(tup.elements.size(), size_t(2));
            assertEqual(std::get<IntValue>(tup.elements[0]->data).value, int64_t(123));
            assertEqual(std::get<StringValue>(tup.elements[1]->data).value, std::string("xyz"));
        });

        it("Float.parse returns a typed ParseError on invalid input", []() {
            auto result = run(
                "main do\n"
                "  match Float.parse(\"abc\") do\n"
                "    Ok(f) -> \"\"\n"
                "    Error(e) -> e.message\n"
                "  end\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("invalid float"));
        });

        it("Number.parse tries integer then falls back to float", []() {
            auto ri = run(
                "main do\n"
                "  match Number.parse(\"42\") do\n"
                "    Ok(n) -> n\n"
                "    Error(_) -> -1\n"
                "  end\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(ri->data).value, int64_t(42));
            auto rf = run(
                "main do\n"
                "  match Number.parse(\"3.14\") do\n"
                "    Ok(n) -> n\n"
                "    Error(_) -> 0.0\n"
                "  end\n"
                "end\n"
            );
            assertTrue(std::holds_alternative<FloatValue>(rf->data));
        });
    });

    describe("Interpreter — Comparison", []() {
        it("== on equal values", []() {
            auto result = run("main do\n  5 == 5\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
        });

        it("== on unequal values", []() {
            auto result = run("main do\n  5 == 3\nend\n");
            assertFalse(std::get<BoolValue>(result->data).value);
        });

        it("!= works", []() {
            auto result = run("main do\n  5 != 3\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
        });

        it("< works", []() {
            auto result = run("main do\n  3 < 5\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
        });

        it("> works", []() {
            auto result = run("main do\n  5 > 3\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
        });

        it("<= and >= work on strings (regression)", []() {
            // Regression test: <= and >= only handled Int/Float, unlike <
            // and > which already supported String — "9" <= "9" used to
            // throw "Cannot compare String and String".
            auto le = run("main do\n  \"5\" <= \"9\"\nend\n");
            assertTrue(std::get<BoolValue>(le->data).value);
            auto ge = run("main do\n  \"9\" >= \"5\"\nend\n");
            assertTrue(std::get<BoolValue>(ge->data).value);
        });

        it("logical and", []() {
            auto result = run("main do\n  true && false\nend\n");
            assertFalse(std::get<BoolValue>(result->data).value);
        });

        it("logical or", []() {
            auto result = run("main do\n  true || false\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
        });

        it("logical not", []() {
            auto result = run("main do\n  !true\nend\n");
            assertFalse(std::get<BoolValue>(result->data).value);
        });

    });

    describe("Interpreter — Variables", []() {
        it("let binding", []() {
            auto result = run("main do\n  let x = 42\n  x\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("var binding and reassignment", []() {
            auto result = run(
                "main do\n"
                "  var x = 1\n"
                "  x = 2\n"
                "  x = 3\n"
                "  x\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(3));
        });

        it("let propagates through expressions", []() {
            auto result = run(
                "main do\n"
                "  let x = 5\n"
                "  let y = x + 3\n"
                "  y\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(8));
        });
    });

    describe("Interpreter — Functions", []() {
        it("calls simple function", []() {
            auto result = run(
                "let double(n: Int) = n * 2\n"
                "main do\n  double(21)\nend\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("calls function with multiple args", []() {
            auto result = run(
                "let add(a: Int, b: Int) = a + b\n"
                "main do\n  add(3, 4)\nend\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(7));
        });

        it("recursive function with pattern matching", []() {
            auto result = run(
                "let factorial(0) = 1\n"
                "let factorial(n: Int) = n * factorial(n - 1)\n"
                "main do\n  factorial(5)\nend\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(120));
        });

        it("recursive fibonacci", []() {
            auto result = run(
                "let fib(0) = 0\n"
                "let fib(1) = 1\n"
                "let fib(n: Int) = fib(n - 1) + fib(n - 2)\n"
                "main do\n  fib(10)\nend\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(55));
        });

        it("function with do block body", []() {
            auto output = runOutput(
                "let greet(name, birthYear) do\n"
                "  let age = 2026 - birthYear\n"
                "  IO.printLine(\"Hello, ${name} (${age})\")\n"
                "end\n"
                "main do\n  greet(\"Alice\", 1994)\nend\n"
            );
            assertEqual(output, std::string("Hello, Alice (32)\n"));
        });

        it("nested local function (let name(params) do...end inside a body) keeps its params", []() {
            // Parser::parseLetExpr desugars this shape to a Lambda — its
            // params were previously dropped entirely (hardcoded `{}`),
            // so the body's references to them resolved to whatever the
            // *enclosing* scope happened to have, or nothing at all.
            auto result = run(
                "let countTo(n: Int) do\n"
                "  let loop(state: Int) do\n"
                "    if state >= n\n"
                "      state\n"
                "    else\n"
                "      loop(state + 1)\n"
                "    end\n"
                "  end\n"
                "  loop(0)\n"
                "end\n"
                "main do\n  countTo(5)\nend\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(5));
        });
    });

    describe("Interpreter — Control Flow", []() {
        it("if true branch", []() {
            auto result = run(
                "main do\n"
                "  if true\n    1\n  else\n    2\n  end\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(1));
        });

        it("if false branch", []() {
            auto result = run(
                "main do\n"
                "  if false\n    1\n  else\n    2\n  end\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(2));
        });

        it("if with comparison", []() {
            auto result = run(
                "main do\n"
                "  let x = 10\n"
                "  if x > 5\n    \"big\"\n  else\n    \"small\"\n  end\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("big"));
        });

        it("match expression", []() {
            auto result = run(
                "main do\n"
                "  match 42 do\n"
                "    0 -> \"zero\"\n"
                "    42 -> \"the answer\"\n"
                "    _ -> \"other\"\n"
                "  end\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("the answer"));
        });

        it("match with guard", []() {
            auto result = run(
                "main do\n"
                "  match 85 do\n"
                "    n when n >= 90 -> \"A\"\n"
                "    n when n >= 80 -> \"B\"\n"
                "    _ -> \"F\"\n"
                "  end\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("B"));
        });

        it("loop runs until a return escapes it (regression)", []() {
            // Regression test: `loop do ... end` was a parsed-but-never-
            // evaluated no-op (silently returned None without running the
            // body even once). It must actually run, repeatedly, until a
            // `return` inside it throws (Kex has no break/continue).
            auto result = run(
                "let countTo(limit: Int) -> Int do\n"
                "  var i = 0\n"
                "  loop\n"
                "    if i >= limit\n"
                "      return i\n"
                "    end\n"
                "    i = i + 1\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  countTo(5)\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(5));
        });

        it("return if COND (value-less guard) skips the body when true (regression)", []() {
            // Regression test: `return if COND` (no value before `if`) used
            // to fail to parse — `if` immediately after `return` was parsed
            // as a primary if-expression requiring its own `do` block, not
            // as a trailing guard. `return if COND` should short-circuit
            // with None when COND is true, and otherwise fall through.
            auto guarded = run(
                "let f(skip: Bool) -> String do\n"
                "  return if skip\n"
                "  return \"ran\"\n"
                "end\n"
                "main do\n"
                "  f(true)\n"
                "end\n"
            );
            assertTrue(guarded->isNone());

            auto notGuarded = run(
                "let f(skip: Bool) -> String do\n"
                "  return if skip\n"
                "  return \"ran\"\n"
                "end\n"
                "main do\n"
                "  f(false)\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(notGuarded->data).value, std::string("ran"));
        });
    });

    describe("Interpreter — Collections", []() {
        it("list literal", []() {
            auto result = run("main do\n  [1, 2, 3]\nend\n");
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(3));
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(1));
        });

        it("tuple literal", []() {
            auto result = run("main do\n  (1, \"hello\", true)\nend\n");
            auto& tuple = std::get<TupleValue>(result->data);
            assertEqual(tuple.elements.size(), size_t(3));
        });

        it("tuple destructuring", []() {
            auto result = run(
                "main do\n"
                "  let (a, b) = (10, 20)\n"
                "  a + b\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(30));
        });

        it("tuple destructuring binds nested constructor patterns (regression)", []() {
            // Regression test: `let (Tag(x), y) = ...` used to only bind
            // bare-variable tuple elements; a ConstructorPattern element
            // like Tag(x) was silently skipped, leaving `x` undefined.
            auto result = run(
                "type Wrapped = Tag(Int)\n"
                "main do\n"
                "  let (Tag(x), y) = (Tag(7), 3)\n"
                "  x + y\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(10));
        });

        it("range literal", []() {
            auto result = run("main do\n  1..10\nend\n");
            auto& range = std::get<RangeValue>(result->data);
            assertEqual(range.start, int64_t(1));
            assertEqual(range.end, int64_t(10));
        });
    });

    describe("Interpreter — String Interpolation", []() {
        it("interpolates variables", []() {
            auto result = run(
                "main do\n"
                "  let name = \"World\"\n"
                "  \"Hello, ${name}!\"\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("Hello, World!"));
        });

        it("interpolates expressions", []() {
            auto result = run(
                "main do\n"
                "  let x = 5\n"
                "  \"x + 3 = ${x + 3}\"\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("x + 3 = 8"));
        });

        it("interpolates function calls", []() {
            auto result = run(
                "let double(n: Int) = n * 2\n"
                "main do\n"
                "  \"double(21) = ${double(21)}\"\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("double(21) = 42"));
        });
    });

    describe("Interpreter — IO", []() {
        it("IO.printLine prints with trailing newline", []() {
            auto output = runOutput("main do\n  IO.printLine(\"hello\")\nend\n");
            assertEqual(output, std::string("hello\n"));
        });

        it("IO.print prints without trailing newline", []() {
            auto output = runOutput("main do\n  IO.print(\"hello\")\nend\n");
            assertEqual(output, std::string("hello"));
        });

        it("IO.printLine concatenates multiple args with no separator", []() {
            auto output = runOutput("main do\n  IO.printLine(\"a\", \"b\")\nend\n");
            assertEqual(output, std::string("ab\n"));
        });

        it("IO.printLine prints integers", []() {
            auto output = runOutput("main do\n  IO.printLine(42)\nend\n");
            assertEqual(output, std::string("42\n"));
        });

        it("IO.put/IO.putLine are aliases of IO.print/IO.printLine", []() {
            auto output = runOutput("main do\n  IO.put(\"a\")\n  IO.putLine(\"b\")\nend\n");
            assertEqual(output, std::string("ab\n"));
        });

        it("IO.getLine reads a line from stdin", []() {
            std::istringstream input("hello\n");
            auto* prevCin = std::cin.rdbuf(input.rdbuf());
            auto result = run("main do\n  IO.getLine()\nend\n");
            std::cin.rdbuf(prevCin);
            assertEqual(std::get<StringValue>(result->data).value, std::string("hello"));
        });

        it("IO.getLine returns None at EOF", []() {
            std::istringstream input("");
            auto* prevCin = std::cin.rdbuf(input.rdbuf());
            auto result = run("main do\n  IO.getLine()\nend\n");
            std::cin.rdbuf(prevCin);
            assertTrue(result->isNone());
        });

        it("IO.get reads a single character from stdin", []() {
            std::istringstream input("ab");
            auto* prevCin = std::cin.rdbuf(input.rdbuf());
            auto result = run("main do\n  IO.get()\nend\n");
            std::cin.rdbuf(prevCin);
            assertEqual(std::get<StringValue>(result->data).value, std::string("a"));
        });

        it("IO.get returns None at EOF", []() {
            std::istringstream input("");
            auto* prevCin = std::cin.rdbuf(input.rdbuf());
            auto result = run("main do\n  IO.get()\nend\n");
            std::cin.rdbuf(prevCin);
            assertTrue(result->isNone());
        });
    });

    describe("Interpreter — File IO", []() {
        auto scratchPath = []() -> std::string {
            return (std::filesystem::temp_directory_path() / "kex_interpreter_test_file_io.txt").string();
        };

        it("writes and reads a file", [scratchPath]() {
            auto path = scratchPath();
            std::filesystem::remove(path);
            auto result = run(
                "main do\n"
                "  File.write(\"" + path + "\", \"hello\")\n"
                "  File.read(\"" + path + "\")\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("hello"));
            std::filesystem::remove(path);
        });

        it("exists? reflects write and delete", [scratchPath]() {
            auto path = scratchPath();
            std::filesystem::remove(path);
            auto before = run("main do\n  File.exists?(\"" + path + "\")\nend\n");
            assertFalse(std::get<BoolValue>(before->data).value);

            run("main do\n  File.write(\"" + path + "\", \"x\")\nend\n");
            auto afterWrite = run("main do\n  File.exists?(\"" + path + "\")\nend\n");
            assertTrue(std::get<BoolValue>(afterWrite->data).value);

            auto deleted = run("main do\n  File.delete(\"" + path + "\")\nend\n");
            assertTrue(std::get<BoolValue>(deleted->data).value);
            auto afterDelete = run("main do\n  File.exists?(\"" + path + "\")\nend\n");
            assertFalse(std::get<BoolValue>(afterDelete->data).value);
        });

        it("appends to a file", [scratchPath]() {
            auto path = scratchPath();
            std::filesystem::remove(path);
            auto result = run(
                "main do\n"
                "  File.write(\"" + path + "\", \"a\")\n"
                "  File.append(\"" + path + "\", \"b\")\n"
                "  File.read(\"" + path + "\")\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("ab"));
            std::filesystem::remove(path);
        });

        it("reading a nonexistent file returns None", []() {
            auto result = run("main do\n  File.read(\"/nonexistent/kex/path/xyz\")\nend\n");
            assertTrue(result->isNone());
        });

        it("deleting a nonexistent file returns false", []() {
            auto result = run("main do\n  File.delete(\"/nonexistent/kex/path/xyz\")\nend\n");
            assertFalse(std::get<BoolValue>(result->data).value);
        });

        it("lines materializes a list of lines", [scratchPath]() {
            auto path = scratchPath();
            std::filesystem::remove(path);
            auto result = run(
                "main do\n"
                "  File.write(\"" + path + "\", \"a\\nb\\nc\")\n"
                "  File.lines(\"" + path + "\")\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(3));
            assertEqual(std::get<StringValue>(list.elements[0]->data).value, std::string("a"));
            assertEqual(std::get<StringValue>(list.elements[2]->data).value, std::string("c"));
            std::filesystem::remove(path);
        });

        it("feed wraps file lines in a stream", [scratchPath]() {
            auto path = scratchPath();
            std::filesystem::remove(path);
            auto result = run(
                "main do\n"
                "  File.write(\"" + path + "\", \"a\\nb\")\n"
                "  File.feed(\"" + path + "\").take(2)\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(2));
            assertEqual(std::get<StringValue>(list.elements[0]->data).value, std::string("a"));
            assertEqual(std::get<StringValue>(list.elements[1]->data).value, std::string("b"));
            std::filesystem::remove(path);
        });

        it("feed on a nonexistent file returns None", []() {
            auto result = run("main do\n  File.feed(\"/nonexistent/kex/path/xyz\")\nend\n");
            assertTrue(result->isNone());
        });
    });

    describe("Interpreter — Namespace dispatch", []() {
        it("Stream.Sequence works without corrupting the call (regression)", []() {
            // Regression test: Stream isn't pre-registered as a namespace, so
            // the receiver used to fall back to an Atom that got silently
            // smuggled in as the step function's first argument, corrupting
            // the stream ("Cannot add Atom and Int" on .take()).
            auto result = run(
                "main do\n"
                "  let naturals = Stream.Sequence(from: 0) { |n| n + 1 }\n"
                "  naturals.take(3)\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(3));
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(0));
            assertEqual(std::get<IntValue>(list.elements[1]->data).value, int64_t(1));
            assertEqual(std::get<IntValue>(list.elements[2]->data).value, int64_t(2));
        });

        it("unresolved namespace call to a missing function errors cleanly", []() {
            try {
                run("main do\n  NotANamespace.bogus\nend\n");
                assertTrue(false, "expected an exception");
            } catch (const std::exception& e) {
                std::string msg = e.what();
                assertTrue(msg.find("Undefined function") != std::string::npos, msg);
            }
        });

        it("make TypeName dispatches on an arg-carrying variant (regression)", []() {
            // Regression test: `make Option<A> do let map(@Just(x), f) = ... end`
            // registers under "Option::map", but a Just(5) value is tagged
            // "Just" — dispatch must map the variant back to its declaring
            // type, otherwise this silently falls back to the unrelated
            // list `map` builtin.
            auto result = run(
                "type Option<A> = Just(A) | Nothing\n"
                "make Option<A> do\n"
                "  let map(@Just(x), f) = Just(f(x))\n"
                "  let map(@Nothing, _) = Nothing\n"
                "end\n"
                "main do\n"
                "  Just(5).map { |x| x * 2 }\n"
                "end\n"
            );
            auto& var = std::get<VariantValue>(result->data);
            assertEqual(var.tag, std::string("Just"));
            assertEqual(std::get<IntValue>(var.args.at(0)->data).value, int64_t(10));
        });

        it("make TypeName dispatches on a zero-arg variant (regression)", []() {
            // Zero-arg variant (Nothing) is now a VariantValue, dispatched
            // via the VariantValue receiverType case in the dispatch chain.
            auto result = run(
                "type Option<A> = Just(A) | Nothing\n"
                "make Option<A> do\n"
                "  let map(@Just(x), f) = Just(f(x))\n"
                "  let map(@Nothing, _) = Nothing\n"
                "end\n"
                "main do\n"
                "  Nothing.map { |x| x * 2 }\n"
                "end\n"
            );
            auto& var = std::get<VariantValue>(result->data);
            assertTrue(var.tag == "Nothing");
        });
    });

    describe("Interpreter — Records", []() {
        it("applies declared field defaults on construction (regression)", []() {
            // Regression test: `record X do f : Int = 0 end` then `X { }`
            // used to leave `f` entirely absent from the RecordValue rather
            // than applying the declared default, breaking anything that
            // destructures/reads that field without setting it explicitly.
            auto result = run(
                "record Counter do\n"
                "  value : Int = 0\n"
                "end\n"
                "main do\n"
                "  Counter { }.value\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(0));
        });

        it("explicit fields override declared defaults", []() {
            auto result = run(
                "record Counter do\n"
                "  value : Int = 0\n"
                "end\n"
                "main do\n"
                "  Counter { value: 99 }.value\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(99));
        });

        it("functions defined in a private block inside make are callable (regression)", []() {
            // Regression test: `make X do private do let f(...) = ... end end`
            // used to silently skip the private block entirely — execMakeDef
            // only handled bare FunctionDef items, not VisibilityBlock ones —
            // so `f` was never registered at all.
            auto result = run(
                "record Box do\n"
                "  value : Int\n"
                "end\n"
                "make Box do\n"
                "  let pub(b) = b.priv\n"
                "  private do\n"
                "    let priv(b) = b.value * 2\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  Box { value: 5 }.pub\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(10));
        });
    });

    describe("Interpreter — Args and Env", []() {
        it("main(args) binds the script's command-line arguments", []() {
            auto result = runWithArgs(
                "main(args) do\n"
                "  args\n"
                "end\n",
                {"foo", "bar"}
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(2));
            assertEqual(std::get<StringValue>(list.elements[0]->data).value, std::string("foo"));
            assertEqual(std::get<StringValue>(list.elements[1]->data).value, std::string("bar"));
        });

        it("main without params still works (no args bound)", []() {
            auto result = runWithArgs("main do\n  42\nend\n", {"ignored"});
            assertEqual(std::get<IntValue>(result->data).value, int64_t(42));
        });

        it("main(args) is empty when no script args are set", []() {
            auto result = run("main(args) do\n  args\nend\n");
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(0));
        });

        it("ENV.get returns Just(value) for a set variable", []() {
            setenv("KEX_TEST_ENV_VAR", "hello", 1);
            auto result = run("main do\n  ENV.get(\"KEX_TEST_ENV_VAR\")\nend\n");
            auto& var = std::get<VariantValue>(result->data);
            assertEqual(var.tag, std::string("Just"));
            assertEqual(std::get<StringValue>(var.args.at(0)->data).value, std::string("hello"));
            unsetenv("KEX_TEST_ENV_VAR");
        });

        it("ENV.get returns None for an unset variable", []() {
            unsetenv("KEX_TEST_ENV_VAR_UNSET");
            auto result = run("main do\n  ENV.get(\"KEX_TEST_ENV_VAR_UNSET\")\nend\n");
            assertTrue(result->isNone());
        });

        it("ENV.get with a default returns the default when unset", []() {
            unsetenv("KEX_TEST_ENV_VAR_UNSET");
            auto result = run("main do\n  ENV.get(\"KEX_TEST_ENV_VAR_UNSET\", \"fallback\")\nend\n");
            assertEqual(std::get<StringValue>(result->data).value, std::string("fallback"));
        });

        it("ENV.get with a default still returns the real value when set", []() {
            setenv("KEX_TEST_ENV_VAR", "hello", 1);
            auto result = run("main do\n  ENV.get(\"KEX_TEST_ENV_VAR\", \"fallback\")\nend\n");
            assertEqual(std::get<StringValue>(result->data).value, std::string("hello"));
            unsetenv("KEX_TEST_ENV_VAR");
        });

        it("ENV is a real Map, usable with generic Map ops", []() {
            setenv("KEX_TEST_ENV_VAR", "hello", 1);
            auto result = run("main do\n  ENV.has?(\"KEX_TEST_ENV_VAR\")\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
            unsetenv("KEX_TEST_ENV_VAR");
        });
    });

    describe("Interpreter — Map", []() {
        it("get returns Just(value) when present", []() {
            auto result = run("main do\n  { \"a\": 1 }.get(\"a\")\nend\n");
            auto& var = std::get<VariantValue>(result->data);
            assertEqual(var.tag, std::string("Just"));
            assertEqual(std::get<IntValue>(var.args.at(0)->data).value, int64_t(1));
        });

        it("get returns None when missing and no default given", []() {
            auto result = run("main do\n  { \"a\": 1 }.get(\"b\")\nend\n");
            assertTrue(result->isNone());
        });

        it("get(name, default) returns the default when missing (regression)", []() {
            auto result = run("main do\n  { \"a\": 1 }.get(\"b\", 99)\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(99));
        });

        it("get(name, default) returns the real value when present", []() {
            auto result = run("main do\n  { \"a\": 1 }.get(\"a\", 99)\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(1));
        });

        it("put adds a new key", []() {
            auto result = run("main do\n  { \"a\": 1 }.put(\"b\", 2).get(\"b\", 0)\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(2));
        });

        it("put replaces an existing key", []() {
            auto result = run("main do\n  { \"a\": 1 }.put(\"a\", 5).get(\"a\", 0)\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(5));
        });

        it("delete removes a key", []() {
            auto result = run("main do\n  { \"a\": 1, \"b\": 2 }.delete(\"a\").has?(\"a\")\nend\n");
            assertFalse(std::get<BoolValue>(result->data).value);
        });

        it("has? reflects presence", []() {
            auto result = run("main do\n  { \"a\": 1 }.has?(\"a\")\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
        });

        it("keys and values", []() {
            auto keysResult = run("main do\n  { \"a\": 1, \"b\": 2 }.keys\nend\n");
            auto& keys = std::get<ListValue>(keysResult->data);
            assertEqual(keys.elements.size(), size_t(2));

            auto valuesResult = run("main do\n  { \"a\": 1, \"b\": 2 }.values\nend\n");
            auto& values = std::get<ListValue>(valuesResult->data);
            assertEqual(values.elements.size(), size_t(2));
        });

        it("each yields (key, value) pairs (regression)", []() {
            // Regression test: Map.each used to be a silent no-op — `each`
            // only handled List/Range via getElements(), never Map.
            auto output = runOutput(
                "main do\n"
                "  { \"a\": 1 }.each do |key, value|\n"
                "    IO.printLine(\"${key}=${value}\")\n"
                "  end\n"
                "end\n"
            );
            assertEqual(output, std::string("a=1\n"));
        });

        it("let { \"key\": renamed } = map binds the rename target (regression)", []() {
            // Regression test: destructuring with a rename (`{ "key": x }`)
            // used to bind a variable literally named after the map KEY
            // ("key") instead of the rename target ("x") — field.pattern
            // (the rename) was parsed but ignored at eval time.
            auto result = run(
                "main do\n"
                "  let ages = { \"alice\": 30, \"bob\": 25 }\n"
                "  let { \"alice\": a, \"bob\": b } = ages\n"
                "  a + b\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(55));
        });
    });

    describe("Interpreter — String at", []() {
        it("returns the Char at an index", []() {
            auto result = run("main do\n  \"hello\".at(1)\nend\n");
            assertEqual(std::get<CharValue>(result->data).value, 'e');
        });

        it("returns None out of range", []() {
            auto result = run("main do\n  \"hi\".at(5)\nend\n");
            assertTrue(result->isNone());
        });

        it("Char is its own type, not a 1-char String", []() {
            auto result = run("main do\n  \"hello\".at(1) == \"e\"\nend\n");
            assertFalse(std::get<BoolValue>(result->data).value);
        });

        it("but [Char] IS String", []() {
            auto result = run("main do\n  ['h', 'i'] == \"hi\"\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
        });
    });

    describe("Interpreter — Streams", []() {
        it("creates sequence and takes elements", []() {
            auto result = run(
                "main do\n"
                "  let naturals = Sequence(from: 0) { |n| n + 1 }\n"
                "  naturals.take(5)\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(5));
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(0));
            assertEqual(std::get<IntValue>(list.elements[4]->data).value, int64_t(4));
        });

        it("drops elements from stream", []() {
            auto result = run(
                "main do\n"
                "  let naturals = Sequence(from: 0) { |n| n + 1 }\n"
                "  naturals.drop(5).take(3)\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(3));
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(5));
            assertEqual(std::get<IntValue>(list.elements[2]->data).value, int64_t(7));
        });

        it("lazily maps a stream", []() {
            auto result = run(
                "main do\n"
                "  let squares = Sequence(from: 1) { |n| n + 1 }\n"
                "    .map { |n| n * n }\n"
                "  squares.take(5)\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(1));
            assertEqual(std::get<IntValue>(list.elements[1]->data).value, int64_t(4));
            assertEqual(std::get<IntValue>(list.elements[2]->data).value, int64_t(9));
            assertEqual(std::get<IntValue>(list.elements[3]->data).value, int64_t(16));
            assertEqual(std::get<IntValue>(list.elements[4]->data).value, int64_t(25));
        });

        it("lazily filters a stream", []() {
            auto result = run(
                "let even?(n: Int) = n.modulo(2) == 0\n"
                "main do\n"
                "  let evens = Sequence(from: 1) { |n| n + 1 }\n"
                "    .filter(&even?)\n"
                "  evens.take(5)\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(2));
            assertEqual(std::get<IntValue>(list.elements[1]->data).value, int64_t(4));
            assertEqual(std::get<IntValue>(list.elements[2]->data).value, int64_t(6));
            assertEqual(std::get<IntValue>(list.elements[3]->data).value, int64_t(8));
            assertEqual(std::get<IntValue>(list.elements[4]->data).value, int64_t(10));
        });

        it("chains map and filter lazily", []() {
            auto result = run(
                "let even?(n: Int) = n.modulo(2) == 0\n"
                "main do\n"
                "  let evenSquares = Sequence(from: 1) { |n| n + 1 }\n"
                "    .map { |n| n * n }\n"
                "    .filter(&even?)\n"
                "  evenSquares.take(4)\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(4));
            assertEqual(std::get<IntValue>(list.elements[1]->data).value, int64_t(16));
            assertEqual(std::get<IntValue>(list.elements[2]->data).value, int64_t(36));
            assertEqual(std::get<IntValue>(list.elements[3]->data).value, int64_t(64));
        });

        it("computes primes via lazy filter", []() {
            auto result = run(
                "let isPrime?(n: Int) do\n"
                "  if n < 2\n"
                "    return false\n"
                "  end\n"
                "  return (2..n - 1).all? { |d| n.modulo(d) != 0 }\n"
                "end\n"
                "main do\n"
                "  let primes = Sequence(from: 2) { |n| n + 1 }\n"
                "    .filter(&isPrime?)\n"
                "  primes.take(10)\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(10));
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(2));
            assertEqual(std::get<IntValue>(list.elements[1]->data).value, int64_t(3));
            assertEqual(std::get<IntValue>(list.elements[2]->data).value, int64_t(5));
            assertEqual(std::get<IntValue>(list.elements[3]->data).value, int64_t(7));
            assertEqual(std::get<IntValue>(list.elements[4]->data).value, int64_t(11));
            assertEqual(std::get<IntValue>(list.elements[9]->data).value, int64_t(29));
        });
    });

    describe("Interpreter — Ranges", []() {
        it("maps over range", []() {
            auto result = run(
                "main do\n"
                "  (1..5).map { |x| x * x }\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(5));
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(1));
            assertEqual(std::get<IntValue>(list.elements[4]->data).value, int64_t(25));
        });

        it("filters range", []() {
            auto result = run(
                "main do\n"
                "  (1..10).filter { |x| x.modulo(2) == 0 }\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(5));
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(2));
            assertEqual(std::get<IntValue>(list.elements[4]->data).value, int64_t(10));
        });

        it("reduces range", []() {
            auto result = run(
                "main do\n"
                "  (1..100).reduce(0) { |acc, x| acc + x }\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(5050));
        });

        it("sums range", []() {
            auto result = run("main do\n  (1..10).sum\nend\n");
            assertEqual(std::get<IntValue>(result->data).value, int64_t(55));
        });

        it("converts range to list", []() {
            auto result = run("main do\n  (1..5).to(List)\nend\n");
            auto& converted = std::get<VariantValue>(result->data);
            assertEqual(converted.tag, std::string("Just"));
            auto& list = std::get<ListValue>(converted.args[0]->data);
            assertEqual(list.elements.size(), size_t(5));
        });

        it("returns Optional from universal conversions", []() {
            auto success = run("main do\n  \"42\".to(Integer)\nend\n");
            auto& just = std::get<VariantValue>(success->data);
            assertEqual(just.tag, std::string("Just"));
            assertEqual(std::get<IntValue>(just.args[0]->data).value, int64_t(42));

            auto failure = run("main do\n  \"42x\".to(Integer)\nend\n");
            assertTrue(failure->isNone());
        });

        it("collects Just results, dropping None", []() {
            auto result = run(
                "main do\n"
                "  [1, 2, 3, 4].collect { |n| n.even? then Just(n * 10) else None }\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(2));
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(20));
            assertEqual(std::get<IntValue>(list.elements[1]->data).value, int64_t(40));
        });

        it("supports method chains inside shorthand lambdas", []() {
            auto result = run(
                "main do\n"
                "  (1..3).items.map(&.to(String).or(\"\"))\n"
                "end\n");
            auto& list = std::get<ListValue>(result->data);
            assertEqual(std::get<StringValue>(list.elements[0]->data).value,
                        std::string("1"));
            assertEqual(std::get<StringValue>(list.elements[2]->data).value,
                        std::string("3"));
        });

        it("collects over a range via Enumerable", []() {
            auto result = run(
                "main do\n"
                "  (1..5).collect { |n| n.modulo(2) == 0 then Just(n * n) else None }\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(2));
            assertEqual(std::get<IntValue>(list.elements[0]->data).value, int64_t(4));
            assertEqual(std::get<IntValue>(list.elements[1]->data).value, int64_t(16));
        });
    });

    describe("Interpreter — top-level value binding scope", []() {
        it("top-level `let x = expr` is visible in subsequent top-level bindings", []() {
            // Regression: before the parser+evaluator fix each top-level `let`
            // wrapped in a scoped MainBlock, so `y` couldn't see `x`.
            auto result = run(
                "let x = 10\n"
                "let y = x * 3\n"
                "main do\n"
                "  y\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(30));
        });

        it("top-level `let` binding a list is accessible by name", []() {
            auto result = run(
                "let items = [1, 2, 3]\n"
                "main do\n"
                "  items.count\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(3));
        });
    });

    describe("Interpreter — Traits and Comparable", []() {
        it("trait default method is callable", []() {
            auto result = run(
                "trait Describable do\n"
                "  describe : () -> String\n"
                "  let shout = this.describe.upperCase\n"
                "end\n"
                "record Dog do name : String end\n"
                "make Dog, implement: Describable do\n"
                "  let describe = \"Dog: \" + @name\n"
                "end\n"
                "main do Dog { name: \"rex\" }.shout end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("DOG: REX"));
        });

        it("Less/Equal/Greater are stdlib builtins", []() {
            auto result = run("main do Less end\n");
            assertEqual(std::get<VariantValue>(result->data).tag, std::string("Less"));
        });

        it("sort uses Comparable.compare for user records", []() {
            auto result = run(
                "trait Comparable do\n"
                "  compare : This -> Comparison\n"
                "end\n"
                "record Box do n : Integer end\n"
                "make Box, implement: Comparable do\n"
                "  let compare(other) do\n"
                "    if @n < other.n then Less\n"
                "    elif @n > other.n then Greater\n"
                "    else Equal\n"
                "    end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let sorted = [Box { n: 3 }, Box { n: 1 }, Box { n: 2 }].sort\n"
                "  sorted.map { |b| b.n }\n"
                "end\n"
            );
            auto& list = std::get<ListValue>(result->data).elements;
            assertEqual(std::get<IntValue>(list[0]->data).value, int64_t(1));
            assertEqual(std::get<IntValue>(list[1]->data).value, int64_t(2));
            assertEqual(std::get<IntValue>(list[2]->data).value, int64_t(3));
        });

        it("sort with custom comparator (2-arg form)", []() {
            auto result = run(
                "main do [3, 1, 4, 1, 5].sort { |a, b| a > b } end\n"
            );
            auto& list = std::get<ListValue>(result->data).elements;
            assertEqual(std::get<IntValue>(list[0]->data).value, int64_t(5));
            assertEqual(std::get<IntValue>(list[1]->data).value, int64_t(4));
        });
    });

    describe("Interpreter — Processes (fiber-based scheduler, phase 1)", []() {
        it("spawn/send/receive round trip", []() {
            auto out = runOutput(
                "foul pingServer do\n"
                "  spawn do\n"
                "    loop\n"
                "      receive do |sender|\n"
                "        :ping -> sender.send(:pong)\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let server = pingServer()\n"
                "  server.send(:ping)\n"
                "  receive do\n"
                "    :pong -> IO.printLine(\"pong\")\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("pong\n"));
        });

        it("selective receive skips non-matching messages already in the mailbox", []() {
            // Sends (:a, ...) then (:b, ...) then (:c, ...); the receiver
            // only has a clause for :b — proves it scans past the
            // already-arrived :a rather than only ever checking the first
            // queued message.
            auto out = runOutput(
                "foul echoer do\n"
                "  spawn do\n"
                "    receive do\n"
                "      (:b, sender) -> sender.send(:got_b)\n"
                "    end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let p = echoer()\n"
                "  p.send((:a, Process.self))\n"
                "  p.send((:b, Process.self))\n"
                "  p.send((:c, Process.self))\n"
                "  receive do\n"
                "    :got_b -> IO.printLine(\"selective\")\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("selective\n"));
        });

        it("Process.self differs between spawner and spawned process", []() {
            auto out = runOutput(
                "main do\n"
                "  let parent = Process.self\n"
                "  let child = spawn do\n"
                "    parent.send((:ready, Process.self))\n"
                "  end\n"
                "  receive do\n"
                "    (:ready, childSelf) -> IO.printLine((parent == childSelf).to(String).or(""))\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("false\n"));
        });

        it("two processes with same-named top-level lets in their spawn bodies don't cross-contaminate", []() {
            auto out = runOutput(
                "main do\n"
                "  let parent = Process.self\n"
                "  spawn do\n"
                "    let n = 1\n"
                "    parent.send(n)\n"
                "  end\n"
                "  spawn do\n"
                "    let n = 2\n"
                "    parent.send(n)\n"
                "  end\n"
                "  receive do\n"
                "    n -> receive do\n"
                "      m -> IO.printLine((n + m).to(String).or(""))\n"
                "    end\n"
                "  end\n"
                "end\n"
            );
            // Order between the two spawned processes isn't guaranteed, but
            // if either process's `n` leaked into the other's environment
            // (the m_env-swap bug this test exists to catch), the sum would
            // come out wrong (e.g. 2+2 or 1+1 instead of 1+2).
            assertEqual(out, std::string("3\n"));
        });

        it("a process spawned on one execute() call is reachable from a later call on the same Evaluator", []() {
            // Mirrors REPL usage: `let pid = spawn do ... end` on one line,
            // then `pid.send(...)` on a later line — the Scheduler must
            // persist across execute() calls, not be torn down between
            // them, so a still-alive (blocked-in-receive) process from an
            // earlier call keeps working on a later one.
            Evaluator evaluator;
            // Kept alive for the whole test, matching how main.cxx's REPL
            // mode keeps every line's Program alive forever (`new`, never
            // deleted) — spawn captures a raw pointer into the AST (same
            // convention LambdaValue::body already uses), so a process
            // still blocked in `receive` after execute() returns needs its
            // originating Program to outlive it.
            std::vector<ast::Program> keepAlive;

            auto runOnEvaluator = [&](const std::string& source) {
                Lexer lexer(source);
                auto tokens = lexer.tokenizeAll();
                Parser parser(std::move(tokens));
                keepAlive.push_back(parser.parseProgram());
                evaluator.execute(keepAlive.back());
            };

            // Top-level `let` (no `main` wrapper), matching the REPL's
            // actual line-by-line shape — top-level `var` isn't a
            // supported construct today, unrelated to this test's concern.
            runOnEvaluator(
                "let storedPid = spawn do\n"
                "  receive do\n"
                "    (:ping, sender) -> sender.send(:pong)\n"
                "  end\n"
                "end\n"
            );
            runOnEvaluator(
                "main do\n"
                "  storedPid.send((:ping, Process.self))\n"
                "  receive do\n"
                "    :pong -> IO.printLine(\"cross-call\")\n"
                "  end\n"
                "end\n"
            );
            assertEqual(evaluator.output(), std::string("cross-call\n"));
        });
    });

    describe("Interpreter — Processes (phase 2: receive timeout/after)", []() {
        it("fires `after` when nothing arrives before the deadline", []() {
            auto out = runOutput(
                "main do\n"
                "  receive timeout: 20 do\n"
                "    _ -> IO.printLine(\"unexpected\")\n"
                "  after -> IO.printLine(\"timed out\")\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("timed out\n"));
        });

        it("does not fire `after` when a matching message arrives first", []() {
            auto out = runOutput(
                "main do\n"
                "  let parent = Process.self\n"
                "  let child = spawn do\n"
                "    receive do\n"
                "      :go -> parent.send(:early)\n"
                "    end\n"
                "  end\n"
                "  child.send(:go)\n"
                "  receive timeout: 5000 do\n"
                "    :early -> IO.printLine(\"got it early\")\n"
                "  after -> IO.printLine(\"should not time out\")\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("got it early\n"));
        });

        it("a stale timeout from an earlier receive doesn't leak into a later one", []() {
            // The first receive gets its message almost immediately, well
            // before its own long deadline. That deadline is still sitting
            // in the scheduler's timeout table, unfired, when the second
            // (short-timeout) receive starts — it must not be mistaken for
            // the second receive's own deadline (the waitGeneration guard
            // in blockingReceive exists specifically for this).
            auto out = runOutput(
                "main do\n"
                "  let parent = Process.self\n"
                "  let child = spawn do\n"
                "    receive do\n"
                "      :go -> parent.send(:early)\n"
                "    end\n"
                "  end\n"
                "  child.send(:go)\n"
                "  receive timeout: 5000 do\n"
                "    :early -> IO.printLine(\"first: got it\")\n"
                "  after -> IO.printLine(\"first: should not happen\")\n"
                "  end\n"
                "  receive timeout: 20 do\n"
                "    _ -> IO.printLine(\"second: unexpected message\")\n"
                "  after -> IO.printLine(\"second: correctly timed out\")\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("first: got it\nsecond: correctly timed out\n"));
        });
    });

    describe("Interpreter — Processes (phase 3: link/unlink/alive?)", []() {
        it("alive?() is true while blocked in receive, false once the process finishes", []() {
            auto out = runOutput(
                "main do\n"
                "  let parent = Process.self\n"
                "  let child = spawn do\n"
                "    receive do\n"
                "      :go -> parent.send(:done)\n"
                "    end\n"
                "  end\n"
                "  IO.printLine(\"before: \" + child.alive?().to(String).or(""))\n"
                "  child.send(:go)\n"
                "  receive do\n"
                "    :done -> IO.printLine(\"got done\")\n"
                "  end\n"
                "  # Give the child's own fiber a turn to actually finish —\n"
                "  # it sent :done and returned in the same reduction, so by\n"
                "  # the time we've been woken and resumed, it's already run\n"
                "  # to completion.\n"
                "  IO.printLine(\"after: \" + child.alive?().to(String).or(""))\n"
                "end\n"
            );
            assertEqual(out, std::string("before: true\ngot done\nafter: false\n"));
        });

        it("link() is passive bookkeeping — a linked partner exiting does not kill the other process", []() {
            // Deliberately NOT BEAM's link model: this asserts the
            // absence of cascading kill, since it'd be
            // an easy regression to accidentally reintroduce later.
            auto out = runOutput(
                "main do\n"
                "  let parent = Process.self\n"
                "  let child = spawn do\n"
                "    receive do\n"
                "      :go -> IO.printLine(\"child exiting\")\n"
                "    end\n"
                "  end\n"
                "  child.link()\n"
                "  child.send(:go)\n"
                "  receive timeout: 20 do\n"
                "    _ -> IO.printLine(\"unexpected\")\n"
                "  after -> IO.printLine(\"parent still alive: \" + Process.self.alive?().to(String).or(""))\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("child exiting\nparent still alive: true\n"));
        });
    });

    describe("Interpreter — Processes (phase 4: Task)", []() {
        it("Task.start/.await(timeout:) returns Ok(result) on success", []() {
            auto out = runOutput(
                "main do\n"
                "  let t = Task.start { 10 + 20 }\n"
                "  match t.await(timeout: 2000) do\n"
                "    Ok(v) -> IO.printLine(\"got \" + v.to(String).or(""))\n"
                "    Error(reason) -> IO.printLine(\"error \" + reason.to(String).or(""))\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("got 30\n"));
        });

        it("await(timeout:) returns Error(:timeout) if the task never replies in time", []() {
            auto out = runOutput(
                "main do\n"
                "  let t = Task.start {\n"
                "    receive do\n"
                "      :never -> 1\n"
                "    end\n"
                "  }\n"
                "  match t.await(timeout: 20) do\n"
                "    Ok(v) -> IO.printLine(\"got \" + v.to(String).or(""))\n"
                "    Error(reason) -> IO.printLine(\"error \" + reason.to(String).or(""))\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("error :timeout\n"));
        });

        it("Task.awaitAll awaits multiple tasks in order, each as Ok(result)", []() {
            auto out = runOutput(
                "main do\n"
                "  let t1 = Task.start { 1 + 1 }\n"
                "  let t2 = Task.start { 2 + 2 }\n"
                "  let results = Task.awaitAll([t1, t2])\n"
                "  results.each { |r|\n"
                "    match r do\n"
                "      Ok(v) -> IO.printLine(\"got \" + v.to(String).or(""))\n"
                "      Error(reason) -> IO.printLine(\"error \" + reason.to(String).or(""))\n"
                "    end\n"
                "  }\n"
                "end\n"
            );
            assertEqual(out, std::string("got 2\ngot 4\n"));
        });
    });

    describe("Interpreter — Processes (regressions: bugs found while implementing the scheduler)", []() {
        it("a runtime error at top level still propagates, not silently swallowed by the process wrapper", []() {
            // Wrapping the top-level program in a process's fiber (see
            // Scheduler::runToCompletion) needs a catch(...) to turn an
            // uncaught Kex-level error into that process's exit value — an
            // earlier version of this let the error vanish entirely
            // instead of reaching Evaluator::execute()'s caller, because
            // the fiber's outer catch(...) discarded it rather than
            // propagating it back out. Would have silently swallowed
            // every runtime error in top-level code, not just
            // process-related code.
            try {
                run("main do\n  undefinedFunctionCall()\nend\n");
                assertTrue(false, "expected an exception");
            } catch (const std::exception& e) {
                std::string msg = e.what();
                assertTrue(msg.find("Undefined function") != std::string::npos, msg);
            }
        });

        it("destroying a still-suspended process doesn't corrupt later, unrelated process operations", []() {
            // Boost.Context resumes a still-suspended fiber one last time
            // (to throw forced_unwind) when its Evaluator/Scheduler is
            // destroyed — a generic catch(...) around a spawned process's
            // entry function swallows that internal exception instead of
            // letting it propagate, corrupting Boost.Context's state for
            // every subsequent, unrelated fiber operation. First block
            // spawns a process that never finishes (blocked in receive
            // forever, same shape as examples/beam/proc_ping.kex's server
            // loop) and lets its Evaluator be destroyed while still
            // suspended; the second block proves a completely fresh,
            // unrelated process round trip still works afterward.
            {
                auto out = runOutput(
                    "main do\n"
                    "  spawn do\n"
                    "    receive do\n"
                    "      :never -> IO.printLine(\"unreachable\")\n"
                    "    end\n"
                    "  end\n"
                    "  IO.printLine(\"main done, child left suspended\")\n"
                    "end\n"
                );
                assertEqual(out, std::string("main done, child left suspended\n"));
            }
            auto out2 = runOutput(
                "main do\n"
                "  let parent = Process.self\n"
                "  spawn do\n"
                "    parent.send(:ok)\n"
                "  end\n"
                "  receive do\n"
                "    :ok -> IO.printLine(\"still works\")\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out2, std::string("still works\n"));
        });

        it("a server process handling multiple round trips, interleaved with the caller, doesn't corrupt fiber state", []() {
            // A single thread_local "which continuation does
            // yieldToScheduler() resume" slot was only ever set once, when
            // a fiber first started — resuming the SAME fiber a second
            // time (after some OTHER fiber had run in between and
            // repointed that global slot at itself) read a stale pointer
            // into a different, unrelated, currently-suspended fiber's own
            // stack. Boost.Context caught it as an assertion failure
            // rather than silently corrupting memory, which is how this
            // was found. Two fibers each yielding more than once,
            // interleaved, is the minimal repro — a ping server handling
            // more than one round trip.
            auto out = runOutput(
                "foul pingServer do\n"
                "  spawn do\n"
                "    loop\n"
                "      receive do |sender|\n"
                "        :ping -> sender.send(:pong)\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let server = pingServer()\n"
                "  server.send(:ping)\n"
                "  receive do :pong -> IO.printLine(\"pong1\") end\n"
                "  server.send(:ping)\n"
                "  receive do :pong -> IO.printLine(\"pong2\") end\n"
                "  server.send(:ping)\n"
                "  receive do :pong -> IO.printLine(\"pong3\") end\n"
                "end\n"
            );
            assertEqual(out, std::string("pong1\npong2\npong3\n"));
        });
    });

    describe("Interpreter — Processes (phase 5: Supervisor, :only_crashed only)", []() {
        it("Supervisor.start(restart: :only_crashed) starts each worker and returns Ok(supervisorPid)", []() {
            auto out = runOutput(
                "foul startWorker do\n"
                "  spawn do\n"
                "    receive do\n"
                "      :never -> 1\n"
                "    end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let result = Supervisor.start(restart: :only_crashed) do\n"
                "    [worker { startWorker() }]\n"
                "  end\n"
                "  match result do\n"
                "    Ok(pid) -> IO.printLine(\"started\")\n"
                "    Error(reason) -> IO.printLine(\"error: \" + reason.to(String).or(""))\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("started\n"));
        });

        it("respawns a worker that finishes, with a different pid, matching :only_crashed (one_for_one)", []() {
            auto out = runOutput(
                "foul startWorker(parent) do\n"
                "  spawn do\n"
                "    parent.send((:worker_started, Process.self))\n"
                "    receive do\n"
                "      :die -> IO.printLine(\"worker dying\")\n"
                "    end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let parent = Process.self\n"
                "  Supervisor.start(restart: :only_crashed) do\n"
                "    [worker { startWorker(parent) }]\n"
                "  end\n"
                "  receive do\n"
                "    (:worker_started, pid1) -> do\n"
                "      pid1.send(:die)\n"
                "      receive timeout: 500 do\n"
                "        (:worker_started, pid2) -> IO.printLine((pid1 == pid2).to(String).or(""))\n"
                "      after -> IO.printLine(\"no restart\")\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("worker dying\nfalse\n"));
        });

        it("restart strategies other than :only_crashed return a clear Error, not silently wrong behavior", []() {
            auto out = runOutput(
                "main do\n"
                "  let result = Supervisor.start(restart: :all) do [] end\n"
                "  match result do\n"
                "    Ok(_) -> IO.printLine(\"unexpected ok\")\n"
                "    Error(reason) -> IO.printLine(\"rejected: \" + reason.to(String).or("").contains?(\"only_crashed\").to(String).or(""))\n"
                "  end\n"
                "end\n"
            );
            assertEqual(out, std::string("rejected: true\n"));
        });
    });

    return runAll();
}
