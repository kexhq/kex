#include "test.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include "../src/interpreter/evaluator.hxx"
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
                "    n if n >= 90 -> \"A\"\n"
                "    n if n >= 80 -> \"B\"\n"
                "    _ -> \"F\"\n"
                "  end\n"
                "end\n"
            );
            assertEqual(std::get<StringValue>(result->data).value, std::string("B"));
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
                run("main do\n  Math.PI\nend\n");
                assertTrue(false, "expected an exception");
            } catch (const std::exception& e) {
                std::string msg = e.what();
                assertTrue(msg.find("Undefined function") != std::string::npos, msg);
            }
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
