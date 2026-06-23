#include "test.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include "../src/interpreter/evaluator.hxx"
#include <fstream>
#include <sstream>

using namespace kex;
using namespace kex::interpreter;
using namespace test;

auto readFile(const std::string& path) -> std::string {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::stringstream buf;
    buf << file.rdbuf();
    return buf.str();
}

auto parseFile(const std::string& path) -> bool {
    auto source = readFile(path);
    if (source.empty()) return false;
    try {
        Lexer lexer(source, path);
        auto tokens = lexer.tokenizeAll();
        Parser parser(std::move(tokens), path);
        parser.parseProgram();
        return true;
    } catch (...) {
        return false;
    }
}

auto runFile(const std::string& path) -> std::string {
    auto source = readFile(path);
    Lexer lexer(source, path);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens), path);
    auto program = parser.parseProgram();
    Evaluator evaluator;
    evaluator.execute(program);
    return evaluator.output();
}

// Like runFile, but reports success/failure instead of throwing — used to
// catch examples that parse fine but blow up at runtime (e.g. calling a
// stdlib function that was never registered).
auto runFileOk(const std::string& path) -> bool {
    try {
        runFile(path);
        return true;
    } catch (...) {
        return false;
    }
}

int main() {
    describe("Examples — All Parse", []() {
        it("basics.kex", []() { assertTrue(parseFile("examples/basics.kex")); });
        it("closures.kex", []() { assertTrue(parseFile("examples/closures.kex")); });
        it("compiled.kex", []() { assertTrue(parseFile("examples/compiled.kex")); });
        it("error_handling.kex", []() { assertTrue(parseFile("examples/error_handling.kex")); });
        it("generics.kex", []() { assertTrue(parseFile("examples/generics.kex")); });
        it("html_dsl.kex", []() { assertTrue(parseFile("examples/html_dsl.kex")); });
        it("json_parser.kex", []() { assertTrue(parseFile("examples/json_parser.kex")); });
        it("maps.kex", []() { assertTrue(parseFile("examples/maps.kex")); });
        it("modules.kex", []() { assertTrue(parseFile("examples/modules.kex")); });
        it("mutating.kex", []() { assertTrue(parseFile("examples/mutating.kex")); });
        it("pattern_matching.kex", []() { assertTrue(parseFile("examples/pattern_matching.kex")); });
        it("processes.kex", []() { assertTrue(parseFile("examples/processes.kex")); });
        it("real_world.kex", []() { assertTrue(parseFile("examples/real_world.kex")); });
        it("records.kex", []() { assertTrue(parseFile("examples/records.kex")); });
        it("streams.kex", []() { assertTrue(parseFile("examples/streams.kex")); });
        it("testing.kex", []() { assertTrue(parseFile("examples/testing.kex")); });
        it("types.kex", []() { assertTrue(parseFile("examples/types.kex")); });
        it("vectors.kex", []() { assertTrue(parseFile("examples/vectors.kex")); });
        it("vectors_advanced.kex", []() { assertTrue(parseFile("examples/vectors_advanced.kex")); });
        it("env.kex", []() { assertTrue(parseFile("examples/env.kex")); });
        it("fizzbuzz.kex", []() { assertTrue(parseFile("examples/fizzbuzz.kex")); });
        it("fizzbuzz_abstraction.kex", []() { assertTrue(parseFile("examples/fizzbuzz_abstraction.kex")); });
        it("fizzbuzz_functional.kex", []() { assertTrue(parseFile("examples/fizzbuzz_functional.kex")); });
        it("fizzbuzz_pattern_matching.kex", []() { assertTrue(parseFile("examples/fizzbuzz_pattern_matching.kex")); });
        it("fizzbuzz_recursive.kex", []() { assertTrue(parseFile("examples/fizzbuzz_recursive.kex")); });
        it("fizzbuzz_simple.kex", []() { assertTrue(parseFile("examples/fizzbuzz_simple.kex")); });
        it("hello.kex", []() { assertTrue(parseFile("examples/hello.kex")); });
        it("stdlib_demo.kex", []() { assertTrue(parseFile("examples/stdlib_demo.kex")); });
        it("test_union.kex", []() { assertTrue(parseFile("examples/test_union.kex")); });
    });

    // Parsing is not enough to catch regressions like a stdlib function
    // that's referenced but never registered — only running the example
    // surfaces that. Every example is expected to run without throwing.
    describe("Examples — All Run", []() {
        it("basics.kex", []() { assertTrue(runFileOk("examples/basics.kex")); });
        it("closures.kex", []() { assertTrue(runFileOk("examples/closures.kex")); });
        it("compiled.kex", []() { assertTrue(runFileOk("examples/compiled.kex")); });
        it("env.kex", []() { assertTrue(runFileOk("examples/env.kex")); });
        it("error_handling.kex", []() { assertTrue(runFileOk("examples/error_handling.kex")); });
        it("fizzbuzz.kex", []() { assertTrue(runFileOk("examples/fizzbuzz.kex")); });
        it("fizzbuzz_abstraction.kex", []() { assertTrue(runFileOk("examples/fizzbuzz_abstraction.kex")); });
        it("fizzbuzz_functional.kex", []() { assertTrue(runFileOk("examples/fizzbuzz_functional.kex")); });
        it("fizzbuzz_pattern_matching.kex", []() { assertTrue(runFileOk("examples/fizzbuzz_pattern_matching.kex")); });
        it("fizzbuzz_recursive.kex", []() { assertTrue(runFileOk("examples/fizzbuzz_recursive.kex")); });
        it("fizzbuzz_simple.kex", []() { assertTrue(runFileOk("examples/fizzbuzz_simple.kex")); });
        it("generics.kex", []() { assertTrue(runFileOk("examples/generics.kex")); });
        it("hello.kex", []() { assertTrue(runFileOk("examples/hello.kex")); });
        it("html_dsl.kex", []() { assertTrue(runFileOk("examples/html_dsl.kex")); });
        it("maps.kex", []() { assertTrue(runFileOk("examples/maps.kex")); });
        it("modules.kex", []() { assertTrue(runFileOk("examples/modules.kex")); });
        it("mutating.kex", []() { assertTrue(runFileOk("examples/mutating.kex")); });
        it("pattern_matching.kex", []() { assertTrue(runFileOk("examples/pattern_matching.kex")); });
        it("processes.kex", []() { assertTrue(runFileOk("examples/processes.kex")); });
        it("real_world.kex", []() { assertTrue(runFileOk("examples/real_world.kex")); });
        it("records.kex", []() { assertTrue(runFileOk("examples/records.kex")); });
        it("stdlib_demo.kex", []() { assertTrue(runFileOk("examples/stdlib_demo.kex")); });
        it("streams.kex", []() { assertTrue(runFileOk("examples/streams.kex")); });
        it("test_union.kex", []() { assertTrue(runFileOk("examples/test_union.kex")); });
        it("types.kex", []() { assertTrue(runFileOk("examples/types.kex")); });
        it("vectors.kex", []() { assertTrue(runFileOk("examples/vectors.kex")); });
        it("vectors_advanced.kex", []() { assertTrue(runFileOk("examples/vectors_advanced.kex")); });

        it("json_parser.kex", []() {
            // Used to fail with "Undefined function: parse" — Float had no
            // static `parse` (see registerIntegerBuiltins's Float::parse).
            auto output = runFile("examples/json_parser.kex");
            assertTrue(output.find("Parsed: JsonObject(") != std::string::npos, output);
        });
        it("testing.kex", []() {
            // Rewritten to use real, parenthesized describe/it/assert
            // calls (see registerTestBuiltins) against a User record and
            // Integer.even?/odd? defined right in the example — the
            // original relied on bare no-parens calls (unsupported) and on
            // Mock/before/MyServer/Config/dividesBy?, none of which exist.
            auto output = runFile("examples/testing.kex");
            assertTrue(output.find("5 passed, 0 failed") != std::string::npos, output);
        });
    });

    describe("Examples — Run (vectors.kex)", []() {
        it("computes vector operations correctly", []() {
            auto output = runFile("examples/vectors.kex");
            assertTrue(output.find("a + b = (4.0, 6.0)") != std::string::npos);
            assertTrue(output.find("a * 2 = (6.0, 8.0)") != std::string::npos);
            assertTrue(output.find("a . b = 11.0") != std::string::npos);
            assertTrue(output.find("|a|^2 = 25.0") != std::string::npos);
            assertTrue(output.find("v1 x v2 = (0.0, 0.0, 1.0)") != std::string::npos);
            assertTrue(output.find("v3 + v4 = (5.0, 7.0, 9.0)") != std::string::npos);
            assertTrue(output.find("v3 . v4 = 32.0") != std::string::npos);
        });
    });

    describe("Examples — Run (vectors_advanced.kex)", []() {
        it("runs and produces output", []() {
            auto output = runFile("examples/vectors_advanced.kex");
            assertTrue(output.find("Static Constructors") != std::string::npos);
            assertTrue(output.find("Instance Methods") != std::string::npos);
            assertTrue(output.find("Interpolation") != std::string::npos);
        });
    });

    describe("Examples — Run (stdlib_demo.kex)", []() {
        it("list operations work", []() {
            auto output = runFile("examples/stdlib_demo.kex");
            assertTrue(output.find("sorted: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]") != std::string::npos);
            assertTrue(output.find("sum: 55") != std::string::npos);
            assertTrue(output.find("min: 1") != std::string::npos);
            assertTrue(output.find("max: 10") != std::string::npos);
        });

        it("higher-order functions work", []() {
            auto output = runFile("examples/stdlib_demo.kex");
            assertTrue(output.find("doubled: [20, 6, 14, 2, 18, 8, 12, 4, 16, 10]") != std::string::npos);
            assertTrue(output.find("evens: [10, 4, 6, 2, 8]") != std::string::npos);
            assertTrue(output.find("reduce product: 120") != std::string::npos);
        });

        it("string operations work", []() {
            auto output = runFile("examples/stdlib_demo.kex");
            assertTrue(output.find("trimmed: 'Hello, Kex!'") != std::string::npos);
            assertTrue(output.find("upcase: 'HELLO, KEX!'") != std::string::npos);
            assertTrue(output.find("reversed: 'olleh'") != std::string::npos);
            assertTrue(output.find("joined: one | two | three | four") != std::string::npos);
        });

        it("recursion works", []() {
            auto output = runFile("examples/stdlib_demo.kex");
            assertTrue(output.find("factorial(10) = 3628800") != std::string::npos);
            assertTrue(output.find("fibonacci: [0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55]") != std::string::npos);
        });

        it("records and pattern matching work", []() {
            auto output = runFile("examples/stdlib_demo.kex");
            assertTrue(output.find("grade for 87: B") != std::string::npos);
            assertTrue(output.find("user: Sam, age 32") != std::string::npos);
            assertTrue(output.find("adults: 2") != std::string::npos);
            assertTrue(output.find("names: [Alice, Bob, Charlie]") != std::string::npos);
        });

        it("shorthand lambdas work", []() {
            auto output = runFile("examples/stdlib_demo.kex");
            assertTrue(output.find("count evens: 5") != std::string::npos);
            assertTrue(output.find("count odds: 5") != std::string::npos);
        });
    });

    return runAll();
}
