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

int main() {
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
            assertTrue(std::holds_alternative<NoneValue>(result->data));
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

        it("'and'/'or' are word-form aliases for &&/||", []() {
            auto andResult = run("main do\n  true and false\nend\n");
            assertFalse(std::get<BoolValue>(andResult->data).value);
            auto orResult = run("main do\n  true or false\nend\n");
            assertTrue(std::get<BoolValue>(orResult->data).value);
        });

        it("'not' is a word-form alias for !", []() {
            auto result = run("main do\n  not true\nend\n");
            assertFalse(std::get<BoolValue>(result->data).value);
        });

        it("'not' binds looser than == (regression)", []() {
            // Regression test: `not` was initially given the same tight
            // precedence as `!` (unary level), so `not x == y` parsed as
            // `(not x) == y`. It must bind looser, Python-style, so
            // `not x == y` reads as `not (x == y)` — this is what makes
            // `return if not ENV.get(key) == "true"`-style guards work
            // without needing extra parens.
            auto result = run("main do\n  not 1 == 2\nend\n");
            assertTrue(std::get<BoolValue>(result->data).value);
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
    });

    describe("Interpreter — Control Flow", []() {
        it("if true branch", []() {
            auto result = run(
                "main do\n"
                "  if true do\n    1\n  else do\n    2\n  end\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(1));
        });

        it("if false branch", []() {
            auto result = run(
                "main do\n"
                "  if false do\n    1\n  else do\n    2\n  end\n"
                "end\n"
            );
            assertEqual(std::get<IntValue>(result->data).value, int64_t(2));
        });

        it("if with comparison", []() {
            auto result = run(
                "main do\n"
                "  let x = 10\n"
                "  if x > 5 do\n    \"big\"\n  else do\n    \"small\"\n  end\n"
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
                "  loop do\n"
                "    if i >= limit do\n"
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
            assertTrue(std::holds_alternative<NoneValue>(guarded->data));

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
            assertTrue(std::holds_alternative<NoneValue>(result->data));
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
            assertTrue(std::holds_alternative<NoneValue>(result->data));
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
                "  IO.write(\"" + path + "\", \"hello\")\n"
                "  IO.read(\"" + path + "\")\n"
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

            run("main do\n  IO.write(\"" + path + "\", \"x\")\nend\n");
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
                "  IO.write(\"" + path + "\", \"a\")\n"
                "  File.append(\"" + path + "\", \"b\")\n"
                "  IO.read(\"" + path + "\")\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("ab"));
            std::filesystem::remove(path);
        });

        it("reading a nonexistent file returns None", []() {
            auto result = run("main do\n  IO.read(\"/nonexistent/kex/path/xyz\")\nend\n");
            assertTrue(std::holds_alternative<NoneValue>(result->data));
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
                "  IO.write(\"" + path + "\", \"a\\nb\\nc\")\n"
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
                "  IO.write(\"" + path + "\", \"a\\nb\")\n"
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
            assertTrue(std::holds_alternative<NoneValue>(result->data));
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
            auto& rec = std::get<RecordValue>(result->data);
            assertEqual(rec.typeName, std::string("Just"));
            assertEqual(std::get<IntValue>(rec.fields.at("0")->data).value, int64_t(10));
        });

        it("make TypeName dispatches on a zero-arg variant (regression)", []() {
            // Same as above, but for the zero-arg side (Nothing is an Atom,
            // not a RecordValue) — needs the AtomValue receiverType case.
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
            assertTrue(std::get<AtomValue>(result->data).name == "Nothing");
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
            auto& rec = std::get<RecordValue>(result->data);
            assertEqual(rec.typeName, std::string("Just"));
            assertEqual(std::get<StringValue>(rec.fields.at("0")->data).value, std::string("hello"));
            unsetenv("KEX_TEST_ENV_VAR");
        });

        it("ENV.get returns None for an unset variable", []() {
            unsetenv("KEX_TEST_ENV_VAR_UNSET");
            auto result = run("main do\n  ENV.get(\"KEX_TEST_ENV_VAR_UNSET\")\nend\n");
            assertTrue(std::holds_alternative<NoneValue>(result->data));
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
            auto& rec = std::get<RecordValue>(result->data);
            assertEqual(rec.typeName, std::string("Just"));
            assertEqual(std::get<IntValue>(rec.fields.at("0")->data).value, int64_t(1));
        });

        it("get returns None when missing and no default given", []() {
            auto result = run("main do\n  { \"a\": 1 }.get(\"b\")\nend\n");
            assertTrue(std::holds_alternative<NoneValue>(result->data));
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
            assertTrue(std::holds_alternative<NoneValue>(result->data));
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
                "  if n < 2 do\n"
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
            auto& list = std::get<ListValue>(result->data);
            assertEqual(list.elements.size(), size_t(5));
        });
    });

    return runAll();
}
