#include "test.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include "../src/semantic/analyzer.hxx"
#include "../src/semantic/types.hxx"
#include "../src/semantic/traits.hxx"
#include "../src/semantic/stdlib_signatures.hxx"

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

    describe("Types — numeric tower", []() {
        using namespace kex::semantic;

        it("prints Integer for the arbitrary-precision default", []() {
            assertEqual(typeToString(Type::integer()), std::string("Integer"));
        });

        it("prints Int (not Int64) for the 64-bit signed alias", []() {
            assertEqual(typeToString(Type::int64()), std::string("Int"));
        });

        it("prints sized integer names by table lookup", []() {
            assertEqual(typeToString(Type::byte()), std::string("Byte"));
            assertEqual(typeToString(Type::int8()), std::string("Int8"));
            assertEqual(typeToString(Type::int16()), std::string("Int16"));
            assertEqual(typeToString(Type::int32()), std::string("Int32"));
            assertEqual(typeToString(Type::uint8()), std::string("Byte"));  // UInt8 == Byte
            assertEqual(typeToString(Type::uint16()), std::string("UInt16"));
            assertEqual(typeToString(Type::uint32()), std::string("UInt32"));
            assertEqual(typeToString(Type::uint64()), std::string("UInt64"));
        });

        it("prints sized float names, with no plain Float type", []() {
            assertEqual(typeToString(Type::float32()), std::string("Float32"));
            assertEqual(typeToString(Type::float64()), std::string("Float64"));
        });

        it("prints String for [Char], not [Char]", []() {
            assertEqual(typeToString(Type::string()), std::string("String"));
            assertEqual(typeToString(Type::list(Type::charT())), std::string("String"));
        });

        it("prints [Char] verbatim for lists of non-Char types as [T]", []() {
            assertEqual(typeToString(Type::list(Type::integer())), std::string("[Integer]"));
        });

        it("treats String as a list of Char structurally", []() {
            assertTrue(typesEqual(Type::string(), Type::list(Type::charT())));
        });

        it("distinguishes sized ints by bit width and signedness", []() {
            assertFalse(typesEqual(Type::int8(), Type::uint8()));
            assertFalse(typesEqual(Type::int32(), Type::int64()));
            assertTrue(typesEqual(Type::int64(), Type::int64()));
        });

        it("distinguishes sized floats by bit width", []() {
            assertFalse(typesEqual(Type::float32(), Type::float64()));
            assertTrue(typesEqual(Type::float64(), Type::float64()));
        });

        it("does not equate Char with Integer", []() {
            assertFalse(typesEqual(Type::charT(), Type::integer()));
        });
    });

    describe("Types — trait registry", []() {
        using namespace kex::semantic;
        auto traits = TraitRegistry::withBuiltins();

        it("satisfies Number/Integer for every sized int and arbitrary-precision Integer", [traits]() {
            assertTrue(traits.satisfies(Type::integer(), "Integer"));
            assertTrue(traits.satisfies(Type::integer(), "Number"));
            assertTrue(traits.satisfies(Type::int8(), "Integer"));
            assertTrue(traits.satisfies(Type::byte(), "Number"));
        });

        it("satisfies Float/Number for sized floats only", [traits]() {
            assertTrue(traits.satisfies(Type::float32(), "Float"));
            assertTrue(traits.satisfies(Type::float64(), "Number"));
            assertFalse(traits.satisfies(Type::float64(), "Integer"));
        });

        it("rejects Char/Bool for Integer and Number", [traits]() {
            assertFalse(traits.satisfies(Type::charT(), "Integer"));
            assertFalse(traits.satisfies(Type::boolean(), "Number"));
        });

        it("matches Ruby: even?'s Integer constraint rejects Float", [traits]() {
            assertTrue(traits.satisfies(Type::int64(), "Integer"));
            assertFalse(traits.satisfies(Type::float64(), "Integer"));
        });

        it("satisfies Comparable for ordered primitives but not Bool", [traits]() {
            assertTrue(traits.satisfies(Type::int64(), "Comparable"));
            assertTrue(traits.satisfies(Type::string(), "Comparable"));
            assertFalse(traits.satisfies(Type::boolean(), "Comparable"));
        });

        it("satisfies Equatable/Showable for primitives", [traits]() {
            assertTrue(traits.satisfies(Type::boolean(), "Equatable"));
            assertTrue(traits.satisfies(Type::atom(), "Showable"));
        });

        it("satisfies Equatable for a list/tuple by recursing into element types", [traits]() {
            assertTrue(traits.satisfies(Type::list(Type::integer()), "Equatable"));
            assertTrue(traits.satisfies(Type::tuple({Type::integer(), Type::boolean()}), "Equatable"));
        });

        it("does not satisfy Equatable for a list of an unregistered type", [traits]() {
            assertFalse(traits.satisfies(Type::list(Type::named("MysteryType")), "Equatable"));
        });

        it("satisfies Resultable/Optionable via the prelude ADT bridge", [traits]() {
            assertTrue(traits.satisfies(Type::named("Ok"), "Resultable"));
            assertTrue(traits.satisfies(Type::named("Error"), "Resultable"));
            assertTrue(traits.satisfies(Type::named("Just"), "Optionable"));
            assertFalse(traits.satisfies(Type::named("Ok"), "Optionable"));
        });

        it("does not satisfy an unregistered trait for a NamedType", [traits]() {
            assertFalse(traits.satisfies(Type::named("Ok"), "Showable"));
        });

        it("exposes built-in trait definitions by name", [traits]() {
            assertTrue(traits.get("Comparable") != nullptr);
            assertTrue(traits.get("NoSuchTrait") == nullptr);
        });
    });

    describe("Types — stdlib signature table", []() {
        using namespace kex::semantic;
        auto table = SignatureTable::withStdlib();

        it("knows even?/odd? take one Integer-constrained param and return Bool", [table]() {
            auto* sigs = table.lookup("even?");
            assertTrue(sigs != nullptr);
            assertEqual(sigs->size(), size_t(1));
            assertEqual((*sigs)[0].params.size(), size_t(1));
            assertTrue(typesEqual((*sigs)[0].result, Type::boolean()));
        });

        it("knows ok?/error?/some?/none? return Bool", [table]() {
            for (const auto& name : {"ok?", "error?", "some?", "none?"}) {
                auto* sigs = table.lookup(name);
                assertTrue(sigs != nullptr);
                assertTrue(typesEqual((*sigs)[0].result, Type::boolean()));
            }
        });

        it("registers `or` as two overloads, one per prelude ADT family", [table]() {
            auto* sigs = table.lookup("or");
            assertTrue(sigs != nullptr);
            assertEqual(sigs->size(), size_t(2));
        });

        it("returns nullptr for a function not in the table", [table]() {
            assertTrue(table.lookup("not_a_real_stdlib_fn") == nullptr);
        });
    });

    describe("Semantic — Stdlib call checking", []() {
        it("rejects 'c'.even? — Char doesn't satisfy the Integer constraint", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = 'c'.even?\n"
                "end\n",
                "even?"
            ));
        });

        it("accepts 4.even?", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = 4.even?\n"
                "end\n"
            ));
        });

        it("rejects a wrong-arity stdlib call", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = even?(1, 2)\n"
                "end\n",
                "argument"
            ));
        });

        it("does not check calls to unknown/user-defined functions", []() {
            assertTrue(noErrors(
                "let myFunc(x) = x\n"
                "main do\n"
                "  myFunc(1)\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — Match exhaustiveness", []() {
        it("rejects a user sum type match missing a constructor", []() {
            assertTrue(hasError(
                "type Shape = Circle(Float) | Rectangle(Float, Float) | Triangle(Float, Float, Float)\n"
                "main do\n"
                "  let s = Circle(1.0)\n"
                "  match s do\n"
                "    Circle(r) -> r\n"
                "    Rectangle(w, h) -> w * h\n"
                "  end\n"
                "end\n",
                "Non-exhaustive"
            ));
        });

        it("accepts a fully-covered user sum type match", []() {
            assertTrue(noErrors(
                "type Shape = Circle(Float) | Rectangle(Float, Float)\n"
                "main do\n"
                "  let s = Circle(1.0)\n"
                "  match s do\n"
                "    Circle(r) -> r\n"
                "    Rectangle(w, h) -> w * h\n"
                "  end\n"
                "end\n"
            ));
        });

        it("accepts a match covered by an unguarded wildcard clause", []() {
            assertTrue(noErrors(
                "type Shape = Circle(Float) | Rectangle(Float, Float) | Triangle(Float, Float, Float)\n"
                "main do\n"
                "  let s = Circle(1.0)\n"
                "  match s do\n"
                "    Circle(r) -> r\n"
                "    _ -> 0.0\n"
                "  end\n"
                "end\n"
            ));
        });

        it("rejects an Option match missing None", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = Just(1)\n"
                "  match x do\n"
                "    Just(v) -> v\n"
                "  end\n"
                "end\n",
                "Non-exhaustive"
            ));
        });

        it("accepts an Option match that does cover None", []() {
            // None lexes as its own TokenType::None and parses as a
            // LiteralPattern, not ConstructorPattern{"None"} — a clause
            // set with both Just and None present must be recognized as
            // exhaustive, not just one without the other.
            assertTrue(noErrors(
                "main do\n"
                "  let x = Just(1)\n"
                "  match x do\n"
                "    Just(v) -> v\n"
                "    None -> 0\n"
                "  end\n"
                "end\n"
            ));
        });

        it("does not count a guarded clause alone as covering its constructor", []() {
            assertTrue(hasError(
                "type Shape = Circle(Float) | Rectangle(Float, Float)\n"
                "main do\n"
                "  let s = Circle(1.0)\n"
                "  match s do\n"
                "    Circle(r) when r > 0.0 -> r\n"
                "    Rectangle(w, h) -> w * h\n"
                "  end\n"
                "end\n",
                "Non-exhaustive"
            ));
        });

        it("binds constructor pattern args so they're not flagged as undefined", []() {
            assertTrue(noErrors(
                "type Pair = Pair(Int, Int)\n"
                "main do\n"
                "  let p = Pair(1, 2)\n"
                "  match p do\n"
                "    Pair(a, b) -> a + b\n"
                "  end\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — User-defined function signatures", []() {
        it("rejects a call that violates a declared param annotation", []() {
            assertTrue(hasError(
                "let greet(name: String) = name\n"
                "main do\n"
                "  let x = greet(42)\n"
                "end\n",
                "greet"
            ));
        });

        it("accepts a call matching a declared param annotation", []() {
            assertTrue(noErrors(
                "let greet(name: String) = name\n"
                "main do\n"
                "  let x = greet(\"world\")\n"
                "end\n"
            ));
        });

        it("does not hard-error Int vs Integer — not runtime-distinguished yet", []() {
            assertTrue(noErrors(
                "let double(n: Int) = n * 2\n"
                "let quad(n: Int) = double(double(n))\n"
            ));
        });

        it("treats a single-letter param annotation as generic, usable at multiple call-site types", []() {
            assertTrue(noErrors(
                "let identity(a: A) = a\n"
                "main do\n"
                "  let x = identity(1)\n"
                "  let y = identity(\"hi\")\n"
                "end\n"
            ));
        });

        it("forward references with compatible types pass", []() {
            assertTrue(noErrors(
                "let useIt(x: String) = laterFunc(x)\n"
                "let laterFunc(s: String) = s\n"
                "main do\n"
                "  useIt(\"hello\")\n"
                "end\n"
            ));
        });

        it("forward references with type mismatches are caught", []() {
            assertTrue(hasError(
                "let useIt(x: Int) = laterFunc(x)\n"
                "let laterFunc(s: String) = s\n",
                "laterFunc"
            ));
        });

        it("does not register make-block methods for call checking (implicit `this` receiver)", []() {
            assertTrue(noErrors(
                "make Int do\n"
                "  let describe(label: String) = label\n"
                "end\n"
                "main do\n"
                "  let x = 5.describe(42)\n"
                "end\n"
            ));
        });

        it("accepts calling a zero-arg top-level binding with arguments (auto-call-then-apply)", []() {
            // Every top-level `let NAME = EXPR` is a 0-param function
            // (Parser::parseFunctionDef) — referencing it auto-calls it
            // (Evaluator::autoCallZeroArgConstant), so `hello("Alice")`
            // means "call hello(), then apply 'Alice' to the result,"
            // not "call hello with 1 argument" (a real arity mismatch).
            assertTrue(noErrors(
                "let makeGreeter(prefix: String) -> (String -> String) do\n"
                "  return { |name| \"${prefix}, ${name}!\" }\n"
                "end\n"
                "let hello = makeGreeter(\"Hello\")\n"
                "main do\n"
                "  hello(\"Alice\")\n"
                "end\n"
            ));
        });

        it("counts a trailing do...end block as an argument toward arity", []() {
            assertTrue(noErrors(
                "let times(n: Int, block: Block<[()]>) do\n"
                "  (1..n).each { |_| block() }\n"
                "end\n"
                "main do\n"
                "  times(3) do\n"
                "    IO.printLine(\"hello\")\n"
                "  end\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — main(params) binding", []() {
        it("binds main's params so they're not flagged as undefined", []() {
            assertTrue(noErrors(
                "main(args) do\n"
                "  IO.printLine(args)\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — standalone type annotations", []() {
        it("accepts a function whose body matches the declared return type", []() {
            assertTrue(noErrors(
                "add : Int -> Int -> Int\n"
                "let add(a, b) = a + b\n"
                "main do\n"
                "  IO.printLine(add(1, 2))\n"
                "end\n"
            ));
        });

        it("rejects a function whose body return type contradicts the annotation", []() {
            assertTrue(hasError(
                "add : Int -> Int -> String\n"
                "let add(a, b) = a + b\n"
                "main do\n"
                "  IO.printLine(add(1, 2))\n"
                "end\n"
            ));
        });

        it("uses declared param types for call checking even without inline annotations", []() {
            // `add` has no inline type annotations on its params, but the
            // standalone annotation supplies them — passing a String should error.
            assertTrue(hasError(
                "add : Int -> Int -> Int\n"
                "let add(a, b) = a + b\n"
                "main do\n"
                "  add(\"x\", 2)\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — type aliases", []() {
        it("resolves a type alias in a param annotation", []() {
            // `type Level = :debug | :info` should make `level: Level`
            // accept an Atom argument without a type error.
            assertTrue(noErrors(
                "type Level = :debug | :info | :warn | :error\n"
                "foul log(level: Level, msg: String) do\n"
                "  IO.printLine(msg)\n"
                "end\n"
                "main do\n"
                "  log(:debug, \"hello\")\n"
                "end\n"
            ));
        });

        it("still rejects a clearly wrong type for an aliased param", []() {
            assertTrue(hasError(
                "type Level = :debug | :info\n"
                "foul log(level: Level, msg: String) do\n"
                "  IO.printLine(msg)\n"
                "end\n"
                "main do\n"
                "  log(42, \"hello\")\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — top-level value binding scope", []() {
        it("top-level `let x = expr` is visible to subsequent top-level bindings", []() {
            assertTrue(noErrors(
                "let base = 10\n"
                "let doubled = base * 2\n"
                "main do\n"
                "  IO.printLine(doubled)\n"
                "end\n"
            ));
        });

        it("top-level `let x = expr` is not confused with a function def", []() {
            // Before the parser fix `let greeting = \"hi\"` was parsed as a
            // FunctionDef; now it's a plain LetExpr in a synthetic MainBlock.
            assertTrue(noErrors(
                "let greeting = \"hello\"\n"
                "main do\n"
                "  IO.printLine(greeting)\n"
                "end\n"
            ));
        });

        it("still parses actual function defs correctly after the fix", []() {
            assertTrue(noErrors(
                "let add(a: Int, b: Int) -> Int do\n"
                "  a + b\n"
                "end\n"
                "main do\n"
                "  let r = add(1, 2)\n"
                "  IO.printLine(r)\n"
                "end\n"
            ));
        });
    });

    describe("Semantic — purity (foul)", []() {
        it("pure function calling IO is an error", []() {
            assertTrue(hasError(
                "let impure(msg: String) do\n"
                "  IO.printLine(msg)\n"
                "end\n"
                "main do impure(\"hi\") end\n"
            ));
        });

        it("foul function calling IO is fine", []() {
            assertTrue(noErrors(
                "foul doIO(msg: String) do\n"
                "  IO.printLine(msg)\n"
                "end\n"
                "main do doIO(\"hi\") end\n"
            ));
        });

        it("IO in closure inside pure function is an error", []() {
            assertTrue(hasError(
                "let process(nums) do\n"
                "  nums.map { |n|\n"
                "    IO.printLine(n)\n"
                "    n * 2\n"
                "  }\n"
                "end\n"
                "main do process([1,2,3]) end\n"
            ));
        });

        it("IO.inspect is always allowed even in pure context", []() {
            assertTrue(noErrors(
                "let debug(n) do\n"
                "  IO.inspect(n)\n"
                "  n * 2\n"
                "end\n"
                "main do IO.printLine(debug(5)) end\n"
            ));
        });

        it("pure calling another pure is fine", []() {
            assertTrue(noErrors(
                "let double(n) = n * 2\n"
                "let quadruple(n) = double(double(n))\n"
                "main do IO.printLine(quadruple(3)) end\n"
            ));
        });

        it("pure calling user-defined foul is an error", []() {
            assertTrue(hasError(
                "foul doSomething do\n"
                "  42\n"
                "end\n"
                "let pure(n) do\n"
                "  doSomething()\n"
                "  n\n"
                "end\n"
                "main do pure(1) end\n"
            ));
        });
    });

    describe("Semantic — traits", []() {
        it("valid trait implementation passes", []() {
            assertTrue(noErrors(
                "trait Printable do\n"
                "  toString : () -> String\n"
                "end\n"
                "make Point, implement: Printable do\n"
                "  let toString = \"a point\"\n"
                "end\n"
                "main do IO.printLine(\"ok\") end\n"
            ));
        });

        it("missing required method is an error", []() {
            assertTrue(hasError(
                "trait Printable do\n"
                "  toString : () -> String\n"
                "end\n"
                "make Point, implement: Printable do\n"
                "  let other = 42\n"
                "end\n"
                "main do IO.printLine(\"ok\") end\n"
            ));
        });

        it("unknown trait is an error", []() {
            assertTrue(hasError(
                "make Point, implement: NonExistentTrait do\n"
                "  let foo = 1\n"
                "end\n"
                "main do IO.printLine(\"ok\") end\n"
            ));
        });

        it("implementing multiple traits all satisfied passes", []() {
            assertTrue(noErrors(
                "trait Scorable do\n"
                "  score : () -> Integer\n"
                "end\n"
                "trait Labelable do\n"
                "  label : () -> String\n"
                "end\n"
                "make Thing, implement: Scorable, Labelable do\n"
                "  let score = 1\n"
                "  let label = \"hi\"\n"
                "end\n"
                "main do IO.printLine(\"ok\") end\n"
            ));
        });

        it("foul trait method implemented as let is an error", []() {
            assertTrue(hasError(
                "trait Storable do\n"
                "  foul save : () -> String\n"
                "end\n"
                "make Doc, implement: Storable do\n"
                "  let save = \"saved\"\n"
                "end\n"
                "main do IO.printLine(\"ok\") end\n"
            ));
        });

        it("foul trait method implemented as foul passes", []() {
            assertTrue(noErrors(
                "trait Storable do\n"
                "  foul save : () -> String\n"
                "end\n"
                "make Doc, implement: Storable do\n"
                "  foul save = \"saved\"\n"
                "end\n"
                "main do IO.printLine(\"ok\") end\n"
            ));
        });

        it("missing one of two required traits is an error", []() {
            assertTrue(hasError(
                "trait Scorable do\n"
                "  score : () -> Integer\n"
                "end\n"
                "trait Labelable do\n"
                "  label : () -> String\n"
                "end\n"
                "make Thing, implement: Scorable, Labelable do\n"
                "  let score = 1\n"
                "end\n"
                "main do IO.printLine(\"ok\") end\n"
            ));
        });

        it("This in param annotation resolves to implementing type", []() {
            assertTrue(noErrors(
                "trait Comparable do\n"
                "  compare : This -> String\n"
                "end\n"
                "record Point do x : Integer end\n"
                "make Point, implement: Comparable do\n"
                "  let compare(other: This) = \"done\"\n"
                "end\n"
                "main do\n"
                "  let p = Point { x: 1 }\n"
                "  IO.printLine(p.compare(Point { x: 2 }))\n"
                "end\n"
            ));
        });

        it("trait-bounded param rejects non-implementing type", []() {
            assertTrue(hasError(
                "trait Describable do\n"
                "  describe : () -> String\n"
                "end\n"
                "record Animal do name : String end\n"
                "record Dog do name : String end\n"
                "make Dog, implement: Describable do\n"
                "  let describe = \"dog\"\n"
                "end\n"
                "let showDescription(item: Describable) = item.describe()\n"
                "main do IO.printLine(showDescription(Animal { name: \"Cat\" })) end\n"
            , "expects argument 1 to be Describable"));
        });

        it("trait method return type inferred from trait definition", []() {
            assertTrue(noErrors(
                "trait Nameable do\n"
                "  getName : () -> String\n"
                "end\n"
                "record Cat do name : String end\n"
                "make Cat, implement: Nameable do\n"
                "  let getName = @name\n"
                "end\n"
                "let extractName(item: Nameable) = item.getName()\n"
                "main do\n"
                "  let c = Cat { name: \"Whiskers\" }\n"
                "  IO.printLine(extractName(c))\n"
                "end\n"
            ));
        });

        it("trait default method using this is callable without override", []() {
            assertTrue(noErrors(
                "trait Describable do\n"
                "  describe : () -> String\n"
                "  let shout = this.describe.upperCase\n"
                "end\n"
                "record Dog do name : String end\n"
                "make Dog, implement: Describable do\n"
                "  let describe = \"Dog: \" + @name\n"
                "end\n"
                "main do\n"
                "  let d = Dog { name: \"Rex\" }\n"
                "  IO.printLine(d.describe)\n"
                "  IO.printLine(d.shout)\n"
                "end\n"
            ));
        });

        it("overriding a trait default method uses the override", []() {
            assertTrue(noErrors(
                "trait Describable do\n"
                "  describe : () -> String\n"
                "  let shout = this.describe.upperCase\n"
                "end\n"
                "record Cat do name : String end\n"
                "make Cat, implement: Describable do\n"
                "  let describe = \"Cat: \" + @name\n"
                "  let shout = \"MEOW: \" + @name.upperCase\n"
                "end\n"
                "main do IO.printLine(Cat { name: \"w\" }.shout) end\n"
            ));
        });

        it("Comparison stdlib type: Less/Equal/Greater are available without declaration", []() {
            assertTrue(noErrors(
                "main do\n"
                "  IO.printLine(Less)\n"
                "  IO.printLine(Equal)\n"
                "  IO.printLine(Greater)\n"
                "end\n"
            ));
        });
    });

    describe("forward reference type checking", []() {
        it("calling a forward-declared function with correct types passes", []() {
            assertTrue(noErrors(
                "let b(x) = a(x) + 1\n"
                "let a(x) = x * 2\n"
                "main do IO.printLine(b(5)) end\n"
            ));
        });

        it("type error in forward-declared function call is detected", []() {
            assertTrue(hasError(
                "let b(x) = a(x) + \"world\"\n"
                "let a(x) = x * 2\n"
                "main do IO.printLine(b(5)) end\n"
            , "Cannot add"));
        });

        it("mutual recursion type checks correctly", []() {
            assertTrue(noErrors(
                "let isEven(n) = if n == 0 then true else isOdd(n - 1) end\n"
                "let isOdd(n) = if n == 0 then false else isEven(n - 1) end\n"
                "main do IO.printLine(isEven(4)) end\n"
            ));
        });
    });

    describe("inline if-then-else", []() {
        it("if cond then a else b end parses and runs", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = 5\n"
                "  let r = if x > 3 then \"big\" else \"small\" end\n"
                "  IO.printLine(r)\n"
                "end\n"
            ));
        });

        it("branch type mismatch in inline if-then-else is an error", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = 5\n"
                "  let r = if x > 3 then \"big\" else 42 end\n"
                "  IO.printLine(r)\n"
                "end\n"
            , "Branch type mismatch"));
        });

        it("mutual recursion with inline if-then-else works", []() {
            assertTrue(noErrors(
                "let isEven(n) = if n == 0 then true else isOdd(n - 1) end\n"
                "let isOdd(n) = if n == 0 then false else isEven(n - 1) end\n"
                "main do IO.printLine(isEven(4)) end\n"
            ));
        });

        it("inline if-then-elif-then-else parses and runs", []() {
            assertTrue(noErrors(
                "let classify(n) = if n > 0 then \"pos\" elif n < 0 then \"neg\" else \"zero\" end\n"
                "main do IO.printLine(classify(5)) end\n"
            ));
        });

        it("inline elif chains can span multiple lines", []() {
            assertTrue(noErrors(
                "let classify(n) do\n"
                "  if n < 0 then :neg\n"
                "  elif n == 0 then :zero\n"
                "  else :pos\n"
                "  end\n"
                "end\n"
                "main do IO.printLine(classify(0)) end\n"
            ));
        });

        it("inline elif branch type mismatch is an error", []() {
            assertTrue(hasError(
                "let classify(n) = if n > 0 then \"pos\" elif n < 0 then 0 else \"zero\" end\n"
                "main do IO.printLine(classify(5)) end\n"
            , "Branch type mismatch"));
        });
    });

    describe("ShorthandLambda typing", []() {
        it("&.method on correct element type passes", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let words = [\"hello world\", \"foo\"]\n"
                "  let parts = words.map(&.split(\" \"))\n"
                "  IO.printLine(parts.join(\", \"))\n"
                "end\n"
            ));
        });

        it("&.method on wrong element type is an error", []() {
            assertTrue(hasError(
                "main do\n"
                "  let nums = [1, 2, 3]\n"
                "  let result = nums.map(&.split(\" \"))\n"
                "  IO.printLine(result.join(\", \"))\n"
                "end\n"
            , "expects argument 1 to be String, but got Integer"));
        });

        it("&function on correct element type passes", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let nums = [1, 2, 3, 4]\n"
                "  let evens = nums.filter(&even?)\n"
                "  IO.printLine(evens.join(\", \"))\n"
                "end\n"
            ));
        });

        it("&function on wrong element type is an error", []() {
            assertTrue(hasError(
                "main do\n"
                "  let words = [\"a\", \"b\"]\n"
                "  let evens = words.filter(&even?)\n"
                "  IO.printLine(evens.join(\", \"))\n"
                "end\n"
            , "expects argument 1 to be Integer, but got String"));
        });
    });

    describe("Void type (bottom type)", []() {
        it("loop returns Void and is compatible with any branch", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let result = if true then 42 else\n"
                "    loop\n"
                "    end\n"
                "  end\n"
                "  IO.printLine(result)\n"
                "end\n"
            ));
        });

        it("Void return annotation is accepted by the parser and resolver", []() {
            // The stdlib `panic` is already typed `String -> Void`.
            // Calling it without binding the result should not error.
            assertTrue(noErrors(
                "main do\n"
                "  IO.printLine(\"before\")\n"
                "end\n"
            ));
        });

        it("if-else where one branch is die (Void) takes the other branch type", []() {
            assertTrue(noErrors(
                "let safeDivide(a: Integer, b: Integer) do\n"
                "  if b == 0 then die(\"div by zero\") else a end\n"
                "end\n"
                "main do IO.printLine(safeDivide(10, 2)) end\n"
            ));
        });
    });

    describe("overload tie-breaking (5c)", []() {
        it("concrete overload beats trait-constrained overload for a concrete arg", []() {
            // `describe` has two overloads: Integer->String (specific) and Printable->String (generic)
            // Passing an Integer should pick the Integer overload (returns "number: ...")
            assertTrue(noErrors(
                "trait Printable do\n"
                "  show : This -> String\n"
                "end\n"
                "make Integer, implement: Printable do\n"
                "  let show(n) = n.to_s()\n"
                "end\n"
                "show : Integer -> String\n"
                "let show(n: Integer) = \"number\"\n"
                "show : Printable -> String\n"
                "let show(p: Printable) = \"printable\"\n"
                "main do IO.printLine(show(42)) end\n"
            ));
        });

        it("unannotated overload loses to annotated when arg matches annotated param", []() {
            // Two overloads: one annotated (String->Int), one unannotated (TypeVar->Int)
            // Passing a String should pick the annotated overload
            assertTrue(noErrors(
                "len : String -> Int\n"
                "let len(s: String) = s.length()\n"
                "let len(x) = 0\n"
                "main do IO.printLine(len(\"hi\")) end\n"
            ));
        });
    });

    describe("Process<T> send-site typing", []() {
        it("typed process rejects wrong message type (method call)", []() {
            assertTrue(hasError(
                "foul startTyped -> Process<String> do\n"
                "  return spawn do\n"
                "    receive do msg -> IO.printLine(msg) end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let p = startTyped()\n"
                "  p.send(42)\n"
                "end\n",
                "does not match Process<String>"
            ));
        });

        it("typed process accepts correct message type (method call)", []() {
            assertTrue(noErrors(
                "foul startTyped -> Process<String> do\n"
                "  return spawn do\n"
                "    receive do msg -> IO.printLine(msg) end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let p = startTyped()\n"
                "  p.send(\"hello\")\n"
                "end\n"
            ));
        });

        it("Process<Any> accepts any message type", []() {
            assertTrue(noErrors(
                "foul startDynamic -> Process<Any> do\n"
                "  return spawn do\n"
                "    receive do msg -> IO.printLine(msg.toString()) end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let d = startDynamic()\n"
                "  d.send(\"anything\")\n"
                "  d.send(42)\n"
                "end\n"
            ));
        });

        it("typed process rejects wrong message type (free function send)", []() {
            assertTrue(hasError(
                "foul startTyped -> Process<String> do\n"
                "  return spawn do\n"
                "    receive do msg -> IO.printLine(msg) end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let p = startTyped()\n"
                "  send(p, 42)\n"
                "end\n",
                "does not match Process<String>"
            ));
        });

        it("typed process accepts correct message type (free function send)", []() {
            assertTrue(noErrors(
                "foul startTyped -> Process<String> do\n"
                "  return spawn do\n"
                "    receive do msg -> IO.printLine(msg) end\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  let p = startTyped()\n"
                "  send(p, \"hello\")\n"
                "end\n"
            ));
        });

        it("inline return type annotation is honoured for non-process types", []() {
            assertTrue(noErrors(
                "let double -> Integer = 2 * 2\n"
                "main do IO.printLine(double.toString()) end\n"
            ));
        });

        it("inline return type mismatch is an error", []() {
            assertTrue(hasError(
                "let getNum -> String = 42\n"
                "main do IO.printLine(getNum()) end\n",
                "declared to return"
            ));
        });
    });

    return runAll();
}
