#include "test.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include "../src/semantic/analyzer.hxx"
#include "../src/semantic/db.hxx"
#include "../src/semantic/types.hxx"
#include "../src/semantic/traits.hxx"
#include "../src/common/prelude_interfaces.hxx"
#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace kex;
using namespace test;

namespace {
#ifdef KEX_RUNTIME_BEAM_DIR
auto preludeInterfaces() -> const semantic::ImportedInterfaces& {
    return preludeSemanticInterfaces(KEX_RUNTIME_BEAM_DIR);
}
#else
auto preludeInterfaces() -> semantic::ImportedInterfaces {
    return {};
}
#endif
}

auto check(const std::string& source) -> std::vector<semantic::Diagnostic> {
    Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    auto program = parser.parseProgram();

    static const auto interfaces = preludeInterfaces();
    semantic::Analyzer analyzer(&interfaces);
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

auto hasErrorWithInterfaces(
    const std::string& source,
    const semantic::ImportedInterfaces& interfaces,
    const std::string& containing) -> bool {
    Lexer lexer(source);
    Parser parser(lexer.tokenizeAll());
    auto program = parser.parseProgram();
    semantic::Analyzer analyzer(&interfaces);
    analyzer.analyze(program);
    for (const auto& diagnostic : analyzer.diagnostics())
        if (diagnostic.level == semantic::Diagnostic::Level::Error &&
            diagnostic.message.find(containing) != std::string::npos)
            return true;
    return false;
}

auto noErrorsWithInterfaces(
    const std::string& source,
    const semantic::ImportedInterfaces& interfaces) -> bool {
    Lexer lexer(source);
    Parser parser(lexer.tokenizeAll());
    auto program = parser.parseProgram();
    semantic::Analyzer analyzer(&interfaces);
    analyzer.analyze(program);
    return std::none_of(
        analyzer.diagnostics().begin(), analyzer.diagnostics().end(),
        [](const auto& diagnostic) {
            return diagnostic.level == semantic::Diagnostic::Level::Error;
        });
}

int main() {
    describe("SemanticDB — Modules", []() {
        it("resolves selectively imported names across indexed files", []() {
            semantic::SemanticDB db;
            db.updateFile("util.kex",
                "module Util\n"
                "let twice(n) = n * 2\n");
            db.updateFile("main.kex",
                "using Util, only: [twice]\n"
                "main do\n"
                "  twice(21)\n"
                "end\n");
            bool hasError = false;
            for (const auto& diagnostic : db.diagnosticsFor("main.kex"))
                if (diagnostic.level == semantic::Diagnostic::Level::Error) hasError = true;
            assertFalse(hasError);
            auto* symbol = db.symbolInModule("Util", "twice");
            assertTrue(symbol != nullptr);
            assertTrue(!symbol->references.empty());
        });

        it("supports go-to-definition for an imported name across files", []() {
            semantic::SemanticDB db;
            db.updateFile("util.kex",
                "module Util\n"
                "let twice(n) = n * 2\n");
            db.updateFile("main.kex",
                "using Util, only: [twice]\n"
                "main do\n"
                "  twice(21)\n"
                "end\n");
            auto* definition = db.symbolInModule("Util", "twice");
            assertTrue(definition != nullptr);
            assertTrue(!definition->references.empty());
            const auto& reference = definition->references.back();
            const auto* atReference = db.symbolAt(
                std::string(reference.file), reference.line, reference.column);
            assertTrue(atReference == definition);
            assertEqual(std::string(atReference->definition.file), std::string("util.kex"));
        });

        it("completes public module members but hides private ones", []() {
            semantic::SemanticDB db;
            db.updateFile("util.kex",
                "module Util\n"
                "let visible() = 1\n"
                "private do\n"
                "  let hidden() = 2\n"
                "end\n");
            const auto completions = db.completionsFor("Util.");
            assertTrue(std::find(completions.begin(), completions.end(), "Util.visible")
                       != completions.end());
            assertTrue(std::find(completions.begin(), completions.end(), "Util.hidden")
                       == completions.end());
        });

        it("rejects importing a private module symbol", []() {
            semantic::SemanticDB db;
            db.updateFile("secrets.kex",
                "module Secrets\n"
                "private do\n"
                "  let hidden() = 42\n"
                "end\n");
            db.updateFile("main.kex",
                "using Secrets, only: [hidden]\n");
            bool found = false;
            for (const auto& diagnostic : db.diagnosticsFor("main.kex"))
                if (diagnostic.message.find("private name `hidden`") != std::string::npos)
                    found = true;
            assertTrue(found);
        });

        it("does not leak module members into the enclosing file", []() {
            semantic::SemanticDB db;
            db.updateFile("main.kex",
                "module Helpers do\n"
                "  let answer() = 42\n"
                "end\n"
                "main do\n"
                "  answer()\n"
                "end\n");
            bool found = false;
            for (const auto& diagnostic : db.diagnosticsFor("main.kex"))
                if (diagnostic.message.find("Undefined function: `answer`") != std::string::npos)
                    found = true;
            assertTrue(found);
        });

        it("keeps top-level definitions file-local", []() {
            semantic::SemanticDB db;
            db.updateFile("one.kex", "let helper() = 42\n");
            db.updateFile("two.kex", "main do\n  helper()\nend\n");
            bool found = false;
            for (const auto& diagnostic : db.diagnosticsFor("two.kex"))
                if (diagnostic.message.find("Undefined function: `helper`") != std::string::npos)
                    found = true;
            assertTrue(found);
        });

        it("discovers an unindexed module from configured source roots", []() {
            namespace fs = std::filesystem;
            const auto root = fs::temp_directory_path() / "kex-semantic-module-test";
            fs::remove_all(root);
            fs::create_directories(root / "http");
            {
                std::ofstream module(root / "http/router.kex");
                module << "module Http.Router\n"
                          "let get(path) = path\n";
            }
            semantic::SemanticDB db;
            db.setModuleRoots({root.string()});
            db.updateFile("main.kex",
                "using Http.Router, only: [get]\n"
                "main do\n"
                "  get(\"/\")\n"
                "end\n");
            fs::remove_all(root);
            assertTrue(db.hasModule("Http.Router"));
            bool hasError = false;
            for (const auto& diagnostic : db.diagnosticsFor("main.kex"))
                if (diagnostic.level == semantic::Diagnostic::Level::Error) hasError = true;
            assertFalse(hasError);
        });

        it("warns when a later source root shadows a module", []() {
            namespace fs = std::filesystem;
            const auto root = fs::temp_directory_path() / "kex-semantic-shadow-test";
            fs::remove_all(root);
            fs::create_directories(root / "lib");
            fs::create_directories(root / "src");
            {
                std::ofstream first(root / "lib/util.kex");
                first << "module Util\nlet value() = 1\n";
                std::ofstream second(root / "src/util.kex");
                second << "module Util\nlet value() = 2\n";
            }
            semantic::SemanticDB db;
            db.setModuleRoots({(root / "lib").string(), (root / "src").string()});
            db.updateFile("main.kex", "using Util, only: [value]\n");
            bool found = false;
            for (const auto& diagnostic : db.diagnosticsFor("main.kex"))
                if (diagnostic.level == semantic::Diagnostic::Level::Warning
                    && diagnostic.message.find("shadowed module definition for Util")
                        != std::string::npos)
                    found = true;
            fs::remove_all(root);
            assertTrue(found);
        });

        it("rejects only: and except: together", []() {
            semantic::SemanticDB db;
            db.updateFile("util.kex",
                "module Util\n"
                "let a() = 1\n"
                "let b() = 2\n");
            db.updateFile("main.kex",
                "using Util, only: [a], except: [b]\n"
                "main do\n  a()\nend\n");
            bool found = false;
            for (const auto& diagnostic : db.diagnosticsFor("main.kex"))
                if (diagnostic.message.find("mutually exclusive") != std::string::npos)
                    found = true;
            assertTrue(found);
        });

        it("validates export target module exists", []() {
            semantic::SemanticDB db;
            db.updateFile("app.kex",
                "module App\n"
                "export Nonexistent\n");
            bool found = false;
            for (const auto& diagnostic : db.diagnosticsFor("app.kex"))
                if (diagnostic.message.find("exported module not found") != std::string::npos)
                    found = true;
            assertTrue(found);
        });

        it("rejects exporting private names", []() {
            semantic::SemanticDB db;
            db.updateFile("secrets.kex",
                "module Secrets\n"
                "private do\n"
                "  let hidden() = 42\n"
                "end\n"
                "let visible() = 1\n");
            db.updateFile("app.kex",
                "module App\n"
                "export Secrets, only: [hidden]\n");
            bool found = false;
            for (const auto& diagnostic : db.diagnosticsFor("app.kex"))
                if (diagnostic.message.find("cannot export private name `hidden`") != std::string::npos)
                    found = true;
            assertTrue(found);
        });

        it("rejects self-export", []() {
            semantic::SemanticDB db;
            db.updateFile("app.kex",
                "module App\n"
                "export App\n");
            bool found = false;
            for (const auto& diagnostic : db.diagnosticsFor("app.kex"))
                if (diagnostic.message.find("cannot export itself") != std::string::npos)
                    found = true;
            assertTrue(found);
        });

        it("allows circular dependencies between lazy module functions", []() {
            namespace fs = std::filesystem;
            const auto root = fs::temp_directory_path() / "kex-cycle-test";
            fs::remove_all(root);
            fs::create_directories(root);
            {
                std::ofstream b(root / "b.kex");
                b << "module B\nusing A\nlet fb() = 2\n";
            }
            semantic::SemanticDB db;
            db.setModuleRoots({root.string()});
            // Index A first (it uses B which will be discovered on disk)
            db.updateFile("a.kex",
                "module A\nusing B\nlet fa() = 1\n");
            fs::remove_all(root);
            bool foundError = false;
            for (const auto& diagnostic : db.diagnosticsFor((root / "b.kex").string()))
                if (diagnostic.level == semantic::Diagnostic::Level::Error)
                    foundError = true;
            assertFalse(foundError);
        });
    });

    describe("Semantic — type declarations", []() {
        it("binds the declared type, not the inferred one", []() {
            // The whole point of writing `[Integer]` is that `[]` has no
            // element type to infer.
            assertTrue(noErrors(
                "main do\n"
                "  let empty: [Integer] = []\n"
                "  let total: Integer = empty.count\n"
                "end\n"));
        });

        it("checks an initializer against a let annotation", []() {
            assertTrue(hasError(
                "main do\n"
                "  let label: String = 42\n"
                "end\n",
                "expected String, got Integer"));
        });

        it("checks an initializer against a var annotation", []() {
            // `var x: T = v` parsed before this and threw the annotation away.
            assertTrue(hasError(
                "main do\n"
                "  var total: Integer = \"zero\"\n"
                "end\n",
                "expected Integer, got String"));
            assertTrue(noErrors(
                "main do\n"
                "  var total: Integer = 0\n"
                "  total = total + 1\n"
                "end\n"));
        });

        it("checks a top-level binding against its standalone declaration", []() {
            // A top-level `let x = v` is a synthetic-main binding rather than
            // a zero-argument function, so its `x : T` line never reached it.
            assertTrue(hasError(
                "count : Integer\n"
                "let count = \"not a number\"\n"
                "main do\n"
                "  IO.printLine(\"x\")\n"
                "end\n",
                "expected Integer, got String"));
        });

        it("does not let a global declaration govern a local binding", []() {
            // Same name, different scope: the annotation belongs to the
            // top-level constant only.
            assertTrue(noErrors(
                "count : Integer\n"
                "let count = 1\n"
                "let describe() -> String do\n"
                "  let count = \"three\"\n"
                "  return count\n"
                "end\n"
                "main do\n"
                "  IO.printLine(describe())\n"
                "end\n"));
        });

        it("resolves a computed alias to the function's return type", []() {
            assertTrue(noErrors(
                "let helloWorld(name: String) -> String = name\n"
                "type Greeting = Type.returnedBy(helloWorld)\n"
                "let shout(text: Greeting) -> Greeting = text.upperCase\n"
                "main do\n"
                "  IO.printLine(shout(\"a\"))\n"
                "end\n"));
        });

        it("enforces a computed alias like any other type", []() {
            assertTrue(hasError(
                "let helloWorld(name: String) -> String = name\n"
                "type Greeting = Type.returnedBy(helloWorld)\n"
                "let shout(text: Greeting) -> Greeting = text.upperCase\n"
                "main do\n"
                "  IO.printLine(shout(42))\n"
                "end\n",
                "to be String, but got Integer"));
        });

        it("computes an alias from a value's type too", []() {
            assertTrue(noErrors(
                "type Answer = Type.of(42)\n"
                "main do\n"
                "  let answer: Answer = 7\n"
                "  IO.printLine(\"${answer}\")\n"
                "end\n"));
            assertTrue(hasError(
                "type Answer = Type.of(42)\n"
                "main do\n"
                "  let answer: Answer = \"seven\"\n"
                "end\n",
                "expected Integer, got String"));
        });

        it("reads a bare type name as that type", []() {
            // `Type.of(Hello)` names a TYPE. It is not a runtime value at
            // all — the walker died on "Undefined identifier: Hello" and BEAM
            // answered `Atom`.
            assertTrue(noErrors(
                "type Hello = You(String) | World\n"
                "main do\n"
                "  let named = Type.of(Hello)\n"
                "  let sameByName = named == Type.named(\"Hello\")\n"
                "  IO.printLine(\"${sameByName}\")\n"
                "end\n"));
        });

        it("clones a type through an alias of a type name", []() {
            // `type HelloClone = Type.of(Hello)` is `Hello`, so it enforces
            // like `Hello` — the alias path used to infer `Hello` as an
            // expression and get nothing.
            assertTrue(noErrors(
                "type Hello = You(String) | World\n"
                "type HelloClone = Type.of(Hello)\n"
                "let greet(h: HelloClone) -> String = \"ok\"\n"
                "main do\n"
                "  IO.printLine(greet(World))\n"
                "end\n"));
            assertTrue(hasError(
                "type Hello = You(String) | World\n"
                "type HelloClone = Type.of(Hello)\n"
                "let greet(h: HelloClone) -> String = \"ok\"\n"
                "main do\n"
                "  IO.printLine(greet(42))\n"
                "end\n",
                "to be Hello, but got Integer"));
        });

        it("rejects Type.returnedBy without a function name", []() {
            assertTrue(hasError(
                "type FromLambda = Type.returnedBy(&.count)\n"
                "main do\n"
                "  IO.printLine(\"x\")\n"
                "end\n",
                "needs the NAME of a function"));
        });

        it("rejects Type.returnedBy on an overloaded name", []() {
            assertTrue(hasError(
                "let render(n: Integer) -> Integer = n\n"
                "let render(s: String) -> String = s\n"
                "type Rendered = Type.returnedBy(render)\n"
                "main do\n"
                "  IO.printLine(\"x\")\n"
                "end\n",
                "cannot choose between the overloads"));
        });
    });

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

        it("rejects foul namespace call in guard", []() {
            assertTrue(hasError(
                "main do\n"
                "  match \"test\" do\n"
                "    s when IO.printLine(s) => s\n"
                "    _ => \"\"\n"
                "  end\n"
                "end\n",
                "guard"
            ));
        });

        it("allows pure receiver call in guard", []() {
            assertTrue(noErrors(
                "using FS\n"
                "main do\n"
                "  match \"hello\" do\n"
                "    s when s.count > 0 => s\n"
                "    _ => \"\"\n"
                "  end\n"
                "end\n"
            ));
        });

        it("rejects transitively foul function in guard", []() {
            assertTrue(hasError(
                "foul log(msg: String) = msg\n"
                "let check(x: String) = log(x)\n"
                "main do\n"
                "  match \"test\" do\n"
                "    s when check(s) => s\n"
                "    _ => \"\"\n"
                "  end\n"
                "end\n",
                "transitively"
            ));
        });

        it("rejects multi-hop transitively foul in guard", []() {
            assertTrue(hasError(
                "foul write(msg: String) = msg\n"
                "let step1(x: String) = write(x)\n"
                "let step2(x: String) = step1(x)\n"
                "let step3(x: String) = step2(x)\n"
                "main do\n"
                "  match \"test\" do\n"
                "    s when step3(s) => s\n"
                "    _ => \"\"\n"
                "  end\n"
                "end\n",
                "transitively"
            ));
        });

        it("allows pure function chain in guard", []() {
            assertTrue(noErrors(
                "let double(x: Int) = x * 2\n"
                "let quadruple(x: Int) = double(double(x))\n"
                "main do\n"
                "  match 5 do\n"
                "    x when quadruple(x) > 10 => x\n"
                "    _ => 0\n"
                "  end\n"
                "end\n"
            ));
        });

        it("rejects call to an imported foul export from pure context", []() {
            // The export's own flag decides: its module carries none.
            semantic::ImportedInterfaces interfaces;
            semantic::ImportedModuleInterface db;
            db.sourceModule = "Database";
            db.backendModule = "kex_database";
            semantic::ImportedFunction query;
            query.sourceName = "query";
            query.signature = {"query", {semantic::Type::string()},
                               semantic::Type::string(), true};
            db.exports["query"].push_back(std::move(query));
            interfaces.modules["Database"] = std::move(db);
            assertTrue(hasErrorWithInterfaces(
                "let check(sql: String) = Database.query(sql)\n",
                interfaces, "foul"));
        });

        it("rejects ambient ENV access from a pure function", []() {
            assertTrue(hasError(
                "let home() = ENV.get(\"HOME\")\n",
                "foul"));
        });

        it("allows pure access to an explicit environment map", []() {
            assertTrue(noErrors(
                "let home(env: {String: String}) = env.get(\"HOME\")\n"));
        });

        it("allows Kex.Intrinsic calls from pure context", []() {
            assertTrue(noErrors(
                "make Map<K, V> do\n"
                "  let delete(key) = Kex.Intrinsic.Map.delete(this, key)\n"
                "end\n"
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

    describe("Semantic — Tagged Literals", []() {
        it("accepts the [String] and [Any] tag ABI", []() {
            assertTrue(noErrors(
                "let rawTag(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "main do rawTag`hello` end\n"));
        });

        it("rejects a tag function with the wrong arity", []() {
            assertTrue(hasError(
                "let rawTag(source: String) -> String = source\n"
                "main do rawTag`hello` end\n",
                "expects"));
        });

        it("rejects a foul tag from a pure function", []() {
            assertTrue(hasError(
                "foul rawTag(parts, values) = parts\n"
                "let build() = rawTag`hello`\n",
                "foul tag"));
        });

        it("resolves names inside interpolating backticks", []() {
            assertTrue(hasError(
                "let render() = $`hello ${missing}`\n",
                "Undefined"));
        });

        it("rejects foul calls inside interpolating backticks", []() {
            assertTrue(hasError(
                "foul readValue() = \"value\"\n"
                "let render() = $`hello ${readValue()}`\n",
                "foul function"));
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

        it("accepts numeric exponentiation", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let x = 2 ^ 10\n"
                "end\n"
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

        it("does not treat capitalized module values as namespaces", []() {
            assertTrue(noErrors(
                "record Temperature do\n"
                "  celsius : Float\n"
                "end\n"
                "module Temperature do\n"
                "  let Fahrenheit(value: Float) -> Temperature = Temperature { celsius: value }\n"
                "  let Freezing = Temperature { celsius: 0.0 }\n"
                "end\n"
                "make Temperature do\n"
                "  let to(String) -> String? = Just(\"temperature\")\n"
                "end\n"
                "main do\n"
                "  Temperature.Fahrenheit(212.0).to(String)\n"
                "  Temperature.Freezing.to(String)\n"
                "end\n"
            ));
        });

        it("does not resolve private intrinsic calls as public overloads", []() {
            assertTrue(noErrors(
                "module Routes do\n"
                "  let delete(server: Any, path: String, handler: Any) -> Any = server\n"
                "end\n"
                "make Map<K, V> do\n"
                "  let delete(key) = Kex.Intrinsic.Map.delete(this, key)\n"
                "  let count = Kex.Intrinsic.Map.count(this)\n"
                "end\n"
                "make List<A> do\n"
                "  let join(sep) = Kex.Intrinsic.List.join(this, sep)\n"
                "end\n"
            ));
        });

        it("checks qualified exports from imported interfaces", []() {
            semantic::ImportedInterfaces interfaces;
            semantic::ImportedModuleInterface numbers;
            numbers.sourceModule = "Numbers";
            numbers.backendModule = "kex_numbers";
            semantic::ImportedFunction doubled;
            doubled.sourceName = "doubled";
            doubled.signature = {"doubled", {semantic::Type::integer()},
                                 semantic::Type::integer()};
            numbers.exports["doubled"].push_back(std::move(doubled));
            interfaces.modules["Numbers"] = std::move(numbers);
            assertTrue(hasErrorWithInterfaces(
                "main do Numbers.doubled(\"wrong\") end\n", interfaces,
                "doubled"));
        });

        it("uses imported ADTs for exhaustiveness checking", []() {
            semantic::ImportedInterfaces interfaces;
            interfaces.adts.push_back({"Choice", {"Yes", "No"}});
            assertTrue(hasErrorWithInterfaces(
                "let choose(value: Choice) do\n"
                "  match value do\n"
                "    Yes => 1\n"
                "  end\n"
                "end\n",
                interfaces, "Non-exhaustive match"));
        });

        it("validates implementations against imported trait definitions", []() {
            semantic::ImportedInterfaces interfaces;
            semantic::TraitDef readable;
            readable.name = "Readable";
            readable.requiredMethods.push_back(
                {"read", {}, semantic::Type::string(), true});
            interfaces.traits.push_back(std::move(readable));
            assertTrue(hasErrorWithInterfaces(
                "record Document do title : String end\n"
                "make Document, implement: Readable do\n"
                "  let read = @title\n"
                "end\n",
                interfaces, "must be declared foul"));
        });

        it("lets a local module shadow an imported module target", []() {
            semantic::ImportedInterfaces interfaces;
            semantic::ImportedModuleInterface numbers;
            numbers.sourceModule = "Numbers";
            numbers.backendModule = "Kex.Stdlib.Numbers";
            semantic::ImportedFunction doubled;
            doubled.sourceName = "doubled";
            doubled.backendModule = "Kex.Stdlib.Numbers";
            doubled.backendFunction = "stdlib_doubled";
            doubled.backendArity = 1;
            doubled.signature = {
                "doubled", {semantic::Type::integer()}, semantic::Type::integer()};
            numbers.exports["doubled"].push_back(std::move(doubled));
            interfaces.modules["Numbers"] = std::move(numbers);

            Lexer lexer(
                "module Numbers do\n"
                "  let doubled(n: Integer) = n * 2\n"
                "end\n"
                "main do Numbers.doubled(21) end\n");
            Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            semantic::Analyzer analyzer(&interfaces);

            assertTrue(analyzer.analyze(program));
            assertTrue(analyzer.resolvedCalls().empty(),
                       "local module call must not retain imported ownership");
        });

        it("checks only package-approved imported receiver functions", []() {
            semantic::ImportedInterfaces interfaces;
            semantic::ImportedFunction doubled;
            doubled.sourceName = "doubled";
            doubled.signature = {"doubled", {semantic::Type::integer()},
                                 semantic::Type::integer()};
            interfaces.receiverFunctions["doubled"].push_back(std::move(doubled));
            assertTrue(hasErrorWithInterfaces(
                "main do \"wrong\".doubled end\n", interfaces, "doubled"));
        });

        it("keeps opt-in receiver functions behind lexical using", []() {
            semantic::ImportedInterfaces interfaces;
            semantic::ImportedModuleInterface opt;
            opt.sourceModule = "Opt";
            opt.automaticImport = false;
            interfaces.modules["Opt"] = std::move(opt);

            semantic::ImportedFunction doubled;
            doubled.sourceName = "doubled";
            doubled.sourceModule = "Opt";
            doubled.signature = {"doubled", {semantic::Type::integer()},
                                 semantic::Type::integer()};
            interfaces.receiverFunctions["doubled"].push_back(
                std::move(doubled));

            assertFalse(hasErrorWithInterfaces(
                "main do \"wrong\".doubled end\n", interfaces, "doubled"));
            assertTrue(hasErrorWithInterfaces(
                "using Opt\nmain do \"wrong\".doubled end\n",
                interfaces, "doubled"));
            assertTrue(hasErrorWithInterfaces(
                "using Opt, only: [doubled]\n"
                "let bad() = \"wrong\".doubled\n",
                interfaces, "doubled"));
            assertFalse(hasErrorWithInterfaces(
                "using Opt, except: [doubled]\n"
                "main do \"wrong\".doubled end\n",
                interfaces, "doubled"));
        });

        it("registers Regex as an opt-in typed stdlib module", []() {
            const auto& interfaces = preludeInterfaces();
            auto regex = interfaces.modules.find("Regex");
            assertTrue(regex != interfaces.modules.end());
            assertFalse(regex->second.automaticImport);
            assertTrue(regex->second.exports.count("regex") > 0);
            assertTrue(regex->second.exports.count("replace") > 0);

            auto replace = interfaces.receiverFunctions.find("replace");
            assertTrue(replace != interfaces.receiverFunctions.end());
            assertTrue(std::any_of(
                replace->second.begin(), replace->second.end(),
                [](const auto& function) {
                    return function.sourceModule == "Regex";
                }));
        });

        it("registers FS constructors as opt-in module exports", []() {
            const auto& interfaces = preludeInterfaces();
            const auto fs = interfaces.modules.find("FS");
            assertTrue(fs != interfaces.modules.end());
            assertFalse(fs->second.automaticImport);
            const auto read = fs->second.exports.find("Read");
            assertTrue(read != fs->second.exports.end());
            assertTrue(std::any_of(
                read->second.begin(), read->second.end(),
                [](const auto& function) {
                    return function.isConstructor &&
                           function.signature.params.empty() &&
                           semantic::typeToString(
                               function.signature.result) == "Read";
                }));

            assertTrue(noErrors(
                "main do FS.Read end\n"));
            assertTrue(noErrors(
                "using FS\nmain do Read end\n"));
        });

        it("keeps qualified uppercase constants typed as constants", []() {
            assertTrue(noErrors(
                "let needsFloat(value: Float) = value\n"
                "let needsString(value: String) = value\n"
                "main do\n"
                "  needsFloat(Math.PI)\n"
                "  needsString(Console.GREEN)\n"
                "end\n"));
        });

        it("source fallback preserves overloaded prelude receiver methods", []() {
            const auto interfaces = sourcePreludeSemanticInterfaces();
            const auto maps = interfaces.receiverFunctions.find("map");
            const auto filters = interfaces.receiverFunctions.find("filter");
            assertTrue(maps != interfaces.receiverFunctions.end());
            assertTrue(filters != interfaces.receiverFunctions.end());
            assertTrue(maps->second.size() >= size_t(3));
            assertTrue(filters->second.size() >= size_t(3));
            assertTrue(interfaces.modules.at("List").automaticImport);
            assertTrue(interfaces.modules.at("String").automaticImport);
            assertTrue(interfaces.modules.at("Stream").automaticImport);
        });

        it("infers the typed result of a using-imported regex tag", []() {
            assertTrue(hasError(
                "using Regex\n"
                "let needsString(value: String) = value\n"
                "main do needsString(regex`\\\\d+`) end\n",
                "needsString"));
        });

        it("selects String.replace over Regex.replace for literal patterns", []() {
            Lexer lexer(
                "using Regex\n"
                "main do \"a-b\".replace(\"-\", \"+\") end\n");
            Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            semantic::Analyzer analyzer(&preludeInterfaces());
            assertTrue(analyzer.analyze(program));
            assertEqual(analyzer.resolvedCalls().size(), size_t(1));
            const auto& target = analyzer.resolvedCalls().begin()->second;
            assertEqual(target.backendModule, std::string("kex_prelude"));
            assertEqual(target.backendFunction, std::string("replace"));
        });

        it("preserves a generic Result payload for overload selection", []() {
            Lexer lexer(
                "using Regex\n"
                "main do \"a-b\".replace(regex(\"-\").or(0), \"+\") end\n");
            Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            semantic::Analyzer analyzer(&preludeInterfaces());
            assertTrue(analyzer.analyze(program));
            assertTrue(analyzer.resolvedCalls().size() >= size_t(2));
            bool foundRegexReplace = false;
            for (const auto& [_, target] : analyzer.resolvedCalls())
                if (target.backendModule == "Kex.Regex" &&
                    target.backendFunction == "replace")
                    foundRegexReplace = true;
            assertTrue(foundRegexReplace);
        });

        it("retains the exact imported receiver target selected by type", []() {
            semantic::ImportedInterfaces interfaces;
            semantic::ImportedFunction integerTarget;
            integerTarget.sourceName = "describe";
            integerTarget.backendModule = "Kex.Numbers";
            integerTarget.backendFunction = "describe_integer";
            integerTarget.backendArity = 1;
            integerTarget.paramNames = {};
            integerTarget.signature = {
                "describe", {semantic::Type::integer()}, semantic::Type::string()};
            interfaces.receiverFunctions["describe"].push_back(
                std::move(integerTarget));

            semantic::ImportedFunction stringTarget;
            stringTarget.sourceName = "describe";
            stringTarget.backendModule = "Kex.Strings";
            stringTarget.backendFunction = "describe_string";
            stringTarget.backendArity = 1;
            stringTarget.signature = {
                "describe", {semantic::Type::string()}, semantic::Type::string()};
            interfaces.receiverFunctions["describe"].push_back(
                std::move(stringTarget));

            Lexer lexer("main do 42.describe end\n");
            Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            semantic::Analyzer analyzer(&interfaces);

            assertTrue(analyzer.analyze(program));
            assertEqual(analyzer.resolvedCalls().size(), size_t(1));
            const auto& target = analyzer.resolvedCalls().begin()->second;
            assertEqual(target.backendModule, std::string("Kex.Numbers"));
            assertEqual(target.backendFunction,
                        std::string("describe_integer"));
            assertEqual(target.backendArity, 1);
            assertTrue(target.passesReceiver);
            assertTrue(target.paramNames.empty());
        });

        it("rejects ambiguous imported receiver ownership", []() {
            semantic::ImportedInterfaces interfaces;
            for (const auto& backend : {"kex_first", "kex_second"}) {
                semantic::ImportedFunction doubled;
                doubled.sourceName = "doubled";
                doubled.backendModule = backend;
                doubled.signature = {"doubled", {semantic::Type::integer()},
                                     semantic::Type::integer()};
                interfaces.receiverFunctions["doubled"].push_back(
                    std::move(doubled));
            }
            assertTrue(hasErrorWithInterfaces(
                "main do 2.doubled end\n", interfaces,
                "ambiguous imported receiver function"));
        });
    });

    describe("Semantic — Purity (Edge Cases)", []() {
        it("rejects receive in pure context", []() {
            assertTrue(hasError(
                "let handler(x: Int) do\n"
                "  receive do\n"
                "    :msg => x\n"
                "  end\n"
                "end\n",
                "receive"
            ));
        });

        it("allows receive in foul context", []() {
            assertTrue(noErrors(
                "foul handler(x: Int) do\n"
                "  receive do\n"
                "    :msg => x\n"
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

        it("keeps primitive make receivers canonical", []() {
            assertTrue(noErrors(
                "make Bool do\n"
                "  let inverted = !this\n"
                "end\n"
            ));
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

    describe("Types — atom literals", []() {
        using namespace kex::semantic;

        it("prints an atom literal type as the literal", []() {
            assertEqual(typeToString(Type::atom("macos")), std::string(":macos"));
            assertEqual(typeToString(Type::atom()), std::string("Atom"));
        });

        it("prints a union of atom literals member by member", []() {
            auto union_ = std::make_shared<Type>(Type{UnionType{
                {Type::atom("macos"), Type::atom("linux"), Type::atom("windows")}}});
            assertEqual(typeToString(union_),
                        std::string(":macos | :linux | :windows"));
        });

        it("keeps every atom literal interchangeable with Atom", []() {
            assertTrue(typesEqual(Type::atom("macos"), Type::atom()));
            assertTrue(typesEqual(Type::atom("macos"), Type::atom("linux")));
        });

        it("reflects an atom literal type as Atom", []() {
            auto structured = structuredTypeOf(Type::atom("macos"));
            assertTrue(structured.has_value());
            assertEqual(structured->name, std::string("Atom"));
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

        it("prints String and [Char] as the distinct types they are", []() {
            assertEqual(typeToString(Type::string()), std::string("String"));
            assertEqual(typeToString(Type::list(Type::charT())),
                        std::string("[Char]"));
        });

        it("prints [Char] verbatim for lists of non-Char types as [T]", []() {
            assertEqual(typeToString(Type::list(Type::integer())), std::string("[Integer]"));
        });

        it("does not equate String with [Char]", []() {
            assertFalse(typesEqual(Type::string(), Type::list(Type::charT())));
            assertTrue(typesEqual(Type::string(), Type::string()));
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

    describe("Types — intersections", []() {
        it("normalizes idempotence, rows, and impossible field meets", []() {
            auto integer = semantic::Type::integer();
            auto same = semantic::Type::intersection({integer, integer});
            assertTrue(semantic::typesEqual(same, integer));

            auto merged = semantic::Type::intersection({
                semantic::Type::record({{"name", semantic::Type::string()}}),
                semantic::Type::record({{"age", semantic::Type::integer()}}),
            });
            assertEqual(semantic::typeToString(merged),
                        std::string("{name: String, age: Integer}"));

            auto impossible = semantic::Type::intersection({
                semantic::Type::record({{"value", semantic::Type::string()}}),
                semantic::Type::record({{"value", semantic::Type::integer()}}),
            });
            assertTrue(std::holds_alternative<semantic::VoidType>(
                impossible->kind));
        });

        it("merges row members before checking field access", []() {
            assertTrue(noErrors(
                "record Entry do\n"
                "  name : String\n"
                "  active : Bool\n"
                "end\n"
                "describe : { name: String } & { active: Bool } -> String\n"
                "let describe(value) = if value.active then value.name else \"off\" end\n"
                "main do describe(Entry { name: \"job\", active: true }) end\n"));
        });

        it("reduces incompatible closed record intersections to Never", []() {
            assertTrue(hasError(
                "record User do name : String end\n"
                "record Pet do name : String end\n"
                "impossible : User & Pet\n"
                "let impossible = User { name: \"Ada\" }\n",
                "incompatible nominal record tags `User` and `Pet`"));
        });

        it("supports intersection and open-row aliases", []() {
            assertTrue(noErrors(
                "record Entry do\n"
                "  name : String\n"
                "  active : Bool\n"
                "end\n"
                "type Named = { name: String }\n"
                "type ActiveNamed = Named & { active: Bool }\n"
                "describe : ActiveNamed -> String\n"
                "let describe(value) = value.name\n"
                "main do describe(Entry { name: \"job\", active: true }) end\n"));
        });

        it("forgets producer intersections to either component", []() {
            assertTrue(noErrors(
                "record Entry do name : String end\n"
                "preserve : R & { name: String } -> R & { name: String }\n"
                "let preserve(value) = value\n"
                "nameOf : { name: String } -> String\n"
                "let nameOf(value) = value.name\n"
                "main do nameOf(preserve(Entry { name: \"job\" })) end\n"));
        });

        it("applies row assignability inside record fields and lists", []() {
            assertTrue(noErrors(
                "record Entry do name : String end\n"
                "record Envelope do item : { name: String } end\n"
                "main do\n"
                "  let entry = Entry { name: \"job\" }\n"
                "  let envelope = Envelope { item: entry }\n"
                "  let entries: [{ name: String }] = [entry]\n"
                "  envelope.item.name + entries.at(0).or(entry).name\n"
                "end\n"));
        });

        it("rejects maps and mutation through an open row", []() {
            assertTrue(hasError(
                "nameOf : { name: String } -> String\n"
                "let nameOf(value) = value.name\n"
                "main do nameOf({ \"name\": \"Ada\" }) end\n",
                "nameOf"));
            assertTrue(hasError(
                "let rename(value: { name: String }) do\n"
                "  value.name = \"changed\"\n"
                "end\n",
                "field assignment requires a record binding"));
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

        it("uses the universal display fallback for a NamedType", [traits]() {
            assertTrue(traits.satisfies(Type::named("Ok"), "Showable"));
            assertTrue(traits.satisfies(Type::named("Ok"), "Inspectable"));
        });

        it("exposes built-in trait definitions by name", [traits]() {
            assertTrue(traits.get("Comparable") != nullptr);
            assertTrue(traits.get("Optionable") == nullptr);
            assertTrue(traits.get("NoSuchTrait") == nullptr);
        });
    });

    describe("Semantic — Stdlib call checking", []() {
        it("accepts min/max/sum lambdas for lists of non-numeric values", []() {
            assertTrue(noErrors(
                "record Shape do\n"
                "  area : Float\n"
                "end\n"
                "main do\n"
                "  let shapes = [Shape { area: 4.0 }, Shape { area: 12.5 }]\n"
                "  let smallest = shapes.min { |shape| shape.area }\n"
                "  let largest = shapes.max { |shape| shape.area }\n"
                "  let total = shapes.sum { |shape| shape.area }\n"
                "end\n"
            ));
        });

        it("types Monoid repeat for strings, lists, and integers", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let text: String = \"ha\".repeat(3)\n"
                "  let items: [Integer] = [1, 2].repeat(2)\n"
                "  let total: Integer = 3.repeat(4)\n"
                "end\n"
            ));
        });

        it("loads Foldable conformances for collection types", []() {
            assertTrue(noErrors(
                "let accept(value: Foldable) = value\n"
                "main do\n"
                "  accept([1, 2])\n"
                "  accept(\"text\")\n"
                "  accept({ a: 1 })\n"
                "  accept(1..3)\n"
                "end\n"
            ));
        });

        it("loads algebraic conformances", []() {
            assertTrue(noErrors(
                "let acceptMonoid(value: Monoid) = value\n"
                "let acceptGroup(value: Group) = value\n"
                "main do\n"
                "  acceptMonoid(1)\n"
                "  acceptGroup(1)\n"
                "  let left = { a: 1 }\n"
                "  let right = { b: 2 }\n"
                "  acceptMonoid(left)\n"
                "  let combined = left.combine(right)\n"
                "  let repeated = combined.repeat(2)\n"
                "end\n"
            ));
        });

        it("loads measures, durations, and numeric time constructors", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let measure: Measure = 2.5.sec\n"
                "  let duration: Duration = Duration { seconds: measure.canonical }\n"
                "  let converted: Result<Measure, String> = measure.convert(Millisecond)\n"
                "end\n"
            ));
        });

        it("loads Optional marker traits and conformances from the prelude", []() {
            assertTrue(noErrors(
                "let accept(value: Optionable) = value\n"
                "let forward(value: Optional<Integer>) = accept(value)\n"));
            assertTrue(hasError(
                "let accept(value: Optionable) = value\n"
                "let reject(value: Result<Integer, String>) = accept(value)\n",
                "expects argument 1 to be Optionable"));
        });

        it("loads Either and its constructors from the prelude interface", []() {
            assertTrue(hasError(
                "let inspect(value: Either<Integer, String>) do\n"
                "  match value do\n"
                "    Left(number) => number\n"
                "  end\n"
                "end\n",
                "Non-exhaustive match on Either: missing case(s) Right"));
        });

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
                "    Circle(r) => r\n"
                "    Rectangle(w, h) => w * h\n"
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
                "    Circle(r) => r\n"
                "    Rectangle(w, h) => w * h\n"
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
                "    Circle(r) => r\n"
                "    _ => 0.0\n"
                "  end\n"
                "end\n"
            ));
        });

        it("rejects an Option match missing None", []() {
            assertTrue(hasError(
                "main do\n"
                "  let x = Just(1)\n"
                "  match x do\n"
                "    Just(v) => v\n"
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
                "    Just(v) => v\n"
                "    None => 0\n"
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
                "    Circle(r) when r > 0.0 => r\n"
                "    Rectangle(w, h) => w * h\n"
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
                "    Pair(a, b) => a + b\n"
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

        it("accepts unrelated nominal records satisfying one open record", []() {
            assertTrue(noErrors(
                "record User do\n"
                "  name : String\n"
                "  age : Integer\n"
                "end\n"
                "record Service do\n"
                "  name : String\n"
                "  port : Integer\n"
                "end\n"
                "nameOf : { name: String } -> String\n"
                "let nameOf(value) = value.name\n"
                "main do\n"
                "  nameOf(User { name: \"Ada\", age: 30 })\n"
                "  nameOf(Service { name: \"search\", port: 9200 })\n"
                "end\n"));
        });

        it("rejects records missing or mistyping an open-record field", []() {
            assertTrue(hasError(
                "record User do\n"
                "  name : Integer\n"
                "end\n"
                "nameOf : { name: String } -> String\n"
                "let nameOf(value) = value.name\n"
                "main do nameOf(User { name: 1 }) end\n",
                "nameOf"));
            assertTrue(hasError(
                "record User do\n"
                "  age : Integer\n"
                "end\n"
                "nameOf : { name: String } -> String\n"
                "let nameOf(value) = value.name\n"
                "main do nameOf(User { age: 1 }) end\n",
                "nameOf"));
        });

        it("requires every member of a trait intersection", []() {
            const std::string declarations =
                "trait Left do left :> String end\n"
                "trait Right do right :> String end\n"
                "record Both do value : String end\n"
                "make Both, implement: Left, Right do\n"
                "  let left = @value\n"
                "  let right = @value\n"
                "end\n"
                "record Half do value : String end\n"
                "make Half, implement: Left do let left = @value end\n"
                "accept : Left & Right -> String\n"
                "let accept(value) = value.left + value.right\n";
            assertTrue(noErrors(
                declarations +
                "main do accept(Both { value: \"ok\" }) end\n"));
            assertTrue(hasError(
                declarations +
                "main do accept(Half { value: \"no\" }) end\n",
                "accept"));
        });

        it("checks open rows against imported generic record fields", []() {
            semantic::ImportedInterfaces interfaces;
            interfaces.typeNames.insert("External.Box");
            interfaces.recordArities["External.Box"] = 1;
            interfaces.recordFieldNames["External.Box"] = {"value"};
            interfaces.recordFields["External.Box"] = {
                {"value", semantic::Type::typeVar(-1)},
            };

            assertTrue(noErrorsWithInterfaces(
                "stringValue : { value: String } -> String\n"
                "let stringValue(box) = box.value\n"
                "let use(box: External.Box<String>) = stringValue(box)\n",
                interfaces));
            assertTrue(hasErrorWithInterfaces(
                "stringValue : { value: String } -> String\n"
                "let stringValue(box) = box.value\n"
                "let use(box: External.Box<Integer>) = stringValue(box)\n",
                interfaces, "stringValue"));
        });

        it("preserves R through an imported intersection signature", []() {
            semantic::ImportedInterfaces interfaces;
            semantic::ImportedModuleInterface external;
            external.sourceModule = "External";
            external.backendModule = "external";
            semantic::ImportedFunction audit;
            audit.sourceName = "audit";
            audit.backendFunction = "audit";
            audit.backendModule = "external";
            audit.backendArity = 1;
            auto row = semantic::Type::record(
                {{"enabled", semantic::Type::boolean()}});
            auto shape = semantic::Type::intersection(
                {semantic::Type::typeVar(-1), row});
            audit.signature = {"audit", {shape}, shape, true};
            external.exports["audit"].push_back(std::move(audit));
            interfaces.modules["External"] = std::move(external);

            assertTrue(noErrorsWithInterfaces(
                "record Account do\n"
                "  enabled : Bool\n"
                "  email : String\n"
                "end\n"
                "foul use(account: Account) -> String = "
                "External.audit(account).email\n",
                interfaces));
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

        it("rejects a make-block method on the wrong receiver refinement", []() {
            assertTrue(hasError(
                "type HandleState = Open | Closed\n"
                "type Handle<S>\n"
                "make Handle<Open> do\n"
                "  read :> String\n"
                "  let read = \"ok\"\n"
                "end\n"
                "let misuse(handle: Handle<Closed>) = handle.read()\n",
                "read"
            ));
        });

        it("accepts a make-block method on its declared receiver refinement", []() {
            assertTrue(noErrors(
                "type HandleState = Open | Closed\n"
                "type Handle<S>\n"
                "make Handle<Open> do\n"
                "  read :> String\n"
                "  let read = \"ok\"\n"
                "end\n"
                "let use(handle: Handle<Open>) = handle.read()\n"
            ));
        });

        it("checks make-block method arguments outside the implementation", []() {
            assertTrue(hasError(
                "make Int do\n"
                "  describe :> String -> String\n"
                "  let describe(label) = label\n"
                "end\n"
                "main do\n"
                "  5.describe(42)\n"
                "end\n",
                "String"
            ));
        });

        it("selects the FileHandle refinement from the exact open mode", []() {
            assertTrue(noErrors(
                "using FS\n"
                "main do\n"
                "  let reader = FS.File.open(\"input.txt\", FS.Read).try\n"
                "  reader.read()\n"
                "  let writer = FS.File.open(\"output.txt\", FS.Write).try\n"
                "  writer.write(\"ok\")\n"
                "  let both = FS.File.open(\"both.txt\", FS.ReadWrite).try\n"
                "  both.read()\n"
                "  both.write(\"ok\")\n"
                "end\n"
            ));
        });

        it("types open as a Result containing the refined handle", []() {
            assertTrue(noErrors(
                "let accepts(value: Result<FileHandle<CannotRead, CanWrite>, "
                "FileError>) = true\n"
                "using FS\n"
                "main do\n"
                "  accepts(FS.File.open(\"output.txt\", FS.Write))\n"
                "end\n"
            ));
        });

        it("rejects writing through a read-only FileHandle", []() {
            assertTrue(hasError(
                "using FS\n"
                "main do\n"
                "  let reader = FS.File.open(\"input.txt\", FS.Read).try\n"
                "  reader.write(\"no\")\n"
                "end\n",
                "FileHandle"
            ));
        });

        it("rejects reading through a write-only FileHandle", []() {
            assertTrue(hasError(
                "using FS\n"
                "main do\n"
                "  let writer = FS.File.open(\"output.txt\", FS.Write).try\n"
                "  writer.readLine()\n"
                "end\n",
                "FileHandle"
            ));
        });

        it("allows an unparameterized receiver method on an inferred generic value", []() {
            assertTrue(noErrors(
                "main do\n"
                "  (1..3).items\n"
                "end\n"
            ));
        });

        it("preserves the inner Optional type through .try", []() {
            assertTrue(hasError(
                "let requireInteger(value: Integer) = value\n"
                "using FS\n"
                "main do\n"
                "  trying do\n"
                "    requireInteger(FS.File.read(\"input.txt\").try)\n"
                "  rescue\n"
                "    _ => 0\n"
                "  end\n"
                "end\n",
                "String"
            ));
        });

        it("accepts .try on an explicitly annotated Optional", []() {
            assertTrue(noErrors(
                "let unwrap(value: Optional<Integer>) -> Integer do\n"
                "  value.try\n"
                "rescue return 0\n"
                "end\n"
            ));
        });

        it("rejects .try on a non-Tryable concrete value", []() {
            assertTrue(hasError(
                "main do\n"
                "  trying do\n"
                "    42.try\n"
                "  rescue\n"
                "    _ => 0\n"
                "  end\n"
                "end\n",
                "Result or Optional"
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

    describe("Semantic — reserved next-state binding", []() {
        it("rejects pure functions named new and recommends build", []() {
            assertTrue(hasError(
                "let new(value: Integer) = value\n",
                "`new` is reserved for the next-state binding; use a name "
                "such as `build` for constructor functions"));
        });

        it("does not reserve new as an effectful command name", []() {
            assertTrue(noErrors(
                "foul new(value: Integer) -> Integer = value\n"));
        });
    });

    describe("Semantic — standalone type annotations", []() {
        it("rejects unknown types in bare declarations in every declaration scope", []() {
            assertTrue(hasError("value : MissingType\nlet value = 1\n",
                                "Unknown type `MissingType`"));
            assertTrue(hasError("convert : MissingType -> Integer\nlet convert(x) = 1\n",
                                "Unknown type `MissingType`"));
            assertTrue(hasError(
                "module Example do\n"
                "  value : MissingType\n"
                "  let value = 1\n"
                "end\n",
                "Unknown type `MissingType`"));
            assertTrue(hasError(
                "trait Example do\n"
                "  value : MissingType\n"
                "end\n",
                "Unknown type `MissingType`"));
            assertTrue(hasError(
                "type Box\n"
                "make Box do\n"
                "  value :> MissingType\n"
                "  let value = 1\n"
                "end\n",
                "Unknown type `MissingType`"));
        });

        it("accepts declared, generic, and forward-referenced declaration types", []() {
            assertTrue(noErrors(
                "identity : A -> A\n"
                "let identity(value) = value\n"
                "later : Later -> Later\n"
                "let later(value) = value\n"
                "type Later\n"
            ));
        });

        it("checks let and var annotations inside main and other functions", []() {
            assertTrue(hasError(
                "main do\n"
                "  let value : String = 1\n"
                "end\n",
                "Type mismatch"));
            assertTrue(hasError(
                "let run do\n"
                "  var value : String = 1\n"
                "end\n",
                "Type mismatch"));
        });

        it("checks defaults against record fields and function parameters", []() {
            assertTrue(hasError(
                "record Settings do\n"
                "  retries : Integer = \"many\"\n"
                "end\n",
                "Type mismatch"));
            assertTrue(hasError(
                "let retry(count: Integer = \"many\") = count\n",
                "Type mismatch"));
        });

        it("applies declarations inside module visibility blocks", []() {
            assertTrue(noErrors(
                "module Text do\n"
                "  private do\n"
                "    repeat : String -> Integer -> String\n"
                "    let repeat(text, n) = "
                    "if n == 0 then \"\" else text + repeat(text, n - 1) end\n"
                "  end\n"
                "  let pad(text) = repeat(\" \", 2) + text\n"
                "end\n"
            ));
        });

        it("propagates annotated list element types into clause patterns", []() {
            assertTrue(noErrors(
                "sort : [Integer] -> [Integer]\n"
                "let sort([]) = []\n"
                "let sort([pivot | rest]) = "
                    "sort(rest.filter { |value| value < pivot })\n"
            ));
        });

        it("prefers a known record field over an imported method collision", []() {
            assertTrue(noErrors(
                "using HTTP\n"
                "record Version do\n"
                "  patch : Integer\n"
                "end\n"
                "main do\n"
                "  let version = Version { patch: 1 }\n"
                "  IO.printLine(version.patch)\n"
                "end\n"
            ));
        });

        it("applies module-owned annotations and contextual callback types", []() {
            assertTrue(noErrors(
                "module Trees do\n"
                "  type Tree = Empty | Node(Integer, Tree, Tree)\n"
                "  make Tree do\n"
                "    insert :> Integer -> Tree\n"
                "    let insert(@Empty, value) = Node(value, Empty, Empty)\n"
                "    let insert(@Node(current, left, right), value) = "
                    "Node(current, left, right)\n"
                "    transform :> (Integer -> Integer) -> Tree\n"
                "    let transform(@Empty, _) = Empty\n"
                "    let transform(@Node(value, left, right), f) = "
                    "Node(f(value), left, right)\n"
                "    fold :> X -> (Integer -> X -> X -> X) -> X\n"
                "    let fold(@Empty, emptyValue, _) = emptyValue\n"
                "    let fold(@Node(value, _, _), emptyValue, combine) = "
                    "combine(value, emptyValue, emptyValue)\n"
                "  end\n"
                "  fromList : [Integer] -> Tree\n"
                "  let fromList(values) = values.reduce(Empty) { |tree, value| "
                    "tree.insert(value) }\n"
                "end\n"
                "main do\n"
                "  let tree = Trees.fromList([1, 2, 3])\n"
                "  tree.transform({ |n| n * 2 })\n"
                "  tree.fold(0, { |value, left, right| value + left + right })\n"
                "end\n"
            ));
        });

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
        it("preserves typed local function params and returns through ~capture", []() {
            auto diagnostics = check(
                "type Handler = Integer -> Integer\n"
                "main do\n"
                "  let wrap(handler: Handler) -> Handler do\n"
                "    return { |n| handler(n) }\n"
                "  end\n"
                "  let middleware = ~wrap\n"
                "  let wrapped = middleware({ |n| n })\n"
                "  wrapped(1)\n"
                "end\n"
            );
            std::string errors;
            for (const auto& diagnostic : diagnostics)
                if (diagnostic.level == semantic::Diagnostic::Level::Error)
                    errors += diagnostic.message + "\n";
            assertTrue(errors.empty(), errors);
        });

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

        it("~function on correct element type passes", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let nums = [1, 2, 3, 4]\n"
                "  let evens = nums.filter(~even?)\n"
                "  IO.printLine(evens.join(\", \"))\n"
                "end\n"
            ));
        });

        it("~function on wrong element type is an error", []() {
            assertTrue(hasError(
                "main do\n"
                "  let words = [\"a\", \"b\"]\n"
                "  let evens = words.filter(~even?)\n"
                "  IO.printLine(evens.join(\", \"))\n"
                "end\n"
            , "expects argument 1 to be Integer, but got String"));
        });
    });

    describe("Never type (bottom type)", []() {
        it("loop returns Never and is compatible with any branch", []() {
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

        it("Never return annotation is accepted for a diverging function", []() {
            // `die` is typed `String -> Never` (bottom); a function annotated
            // `-> Never` whose body diverges typechecks.
            assertTrue(noErrors(
                "let crash(msg: String) -> Never do\n"
                "  die(msg)\n"
                "end\n"
                "main do IO.printLine(\"before\") end\n"
            ));
        });

        it("if-else where one branch is die (Never) takes the other branch type", []() {
            assertTrue(noErrors(
                "let safeDivide(a: Integer, b: Integer) do\n"
                "  if b == 0 then die(\"div by zero\") else a end\n"
                "end\n"
                "main do IO.printLine(safeDivide(10, 2)) end\n"
            ));
        });

        it("Void is the unit type ()", []() {
            // `Void` is `()`, not the bottom type. A function
            // returning `Void` may return `()`.
            assertTrue(noErrors(
                "let noop() -> Void do\n"
                "  return ()\n"
                "end\n"
                "main do noop() end\n"
            ));
        });
    });

    describe("let constructor pattern vs type mismatch", []() {
        it("widens sibling nullary constructors to their parent ADT in lists", []() {
            assertTrue(noErrors(
                "type Comparison = Less | Equal | Greater\n"
                "let render(value: Comparison) = value\n"
                "main do\n"
                "  [Less, Equal, Greater].each(~render)\n"
                "end\n"
            ));
        });

        it("Ok pattern on Optional value errors", []() {
            assertTrue(hasError(
                "main do\n"
                "  let Ok((v, r)) = Integer.parsePrefix(\"42\")\n"
                "  IO.printLine(v)\n"
                "end\n",
                "cannot match `Ok`"
            ));
        });

        it("Just pattern on Result value errors", []() {
            assertTrue(hasError(
                "main do\n"
                "  let Just(v) = Integer.parse(\"42\")\n"
                "  IO.printLine(v)\n"
                "end\n",
                "cannot match `Just`"
            ));
        });

        it("Ok pattern on Result value is ok", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let Ok(n) = Integer.parse(\"42\")\n"
                "  IO.printLine(n)\n"
                "end\n"
            ));
        });

        it("Just pattern on Optional value is ok", []() {
            assertTrue(noErrors(
                "main do\n"
                "  let Just((v, r)) = Integer.parsePrefix(\"42\")\n"
                "  IO.printLine(v)\n"
                "end\n"
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

        it("rejects distinct equally-specific overloads", []() {
            assertTrue(hasError(
                "let choose(value: Integer | String) = 1\n"
                "let choose(value: Integer | Bool) = 2\n"
                "main do IO.printLine(choose(1)) end\n",
                "Ambiguous overload for `choose`"
            ));
        });

        it("rejects a bare first-class reference to an overload set", []() {
            assertTrue(hasError(
                "let render(value: Integer) = \"${value}\"\n"
                "let render(value: String) = value\n"
                "main do\n"
                "  let renderer = ~render\n"
                "  IO.printLine(renderer(1))\n"
                "end\n",
                "Cannot reference overloaded function `render`"
            ));
        });

        it("requires every overload to share purity", []() {
            assertTrue(hasError(
                "let process(value: String) = value\n"
                "foul process(value: Integer) = IO.printLine(value)\n"
                "main do process(\"ok\") end\n",
                "Overloads of `process` must all have the same purity"
            ));
        });

        it("records exact local make-method overload signatures", []() {
            Lexer lexer(
                "record Box do value : Integer end\n"
                "make Box do\n"
                "  let render(s: String) = s\n"
                "  let render(n: Integer) = \"${n}\"\n"
                "end\n"
                "main do\n"
                "  let b = Box { value: 1 }\n"
                "  b.render(\"!\")\n"
                "  b.render(2)\n"
                "end\n");
            Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            semantic::Analyzer analyzer(&preludeInterfaces());
            assertTrue(analyzer.analyze(program));
            bool stringTarget = false;
            bool integerTarget = false;
            for (const auto& [_, target] : analyzer.resolvedCalls()) {
                if (target.backendFunction != "render" ||
                    target.localDispatchTypes.size() != size_t(2))
                    continue;
                stringTarget |= target.localDispatchTypes[1] == "String";
                integerTarget |= target.localDispatchTypes[1] == "Integer";
            }
            assertTrue(stringTarget);
            assertTrue(integerTarget);
        });
    });

    describe("Process<T> send-site typing", []() {
        it("typed process rejects wrong message type (method call)", []() {
            assertTrue(hasError(
                "foul startTyped -> Process<String> do\n"
                "  return spawn do\n"
                "    receive do msg => IO.printLine(msg) end\n"
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
                "    receive do msg => IO.printLine(msg) end\n"
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
                "    receive do msg => IO.printLine(msg.to(String).or(\"\")) end\n"
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
                "    receive do msg => IO.printLine(msg) end\n"
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
                "    receive do msg => IO.printLine(msg) end\n"
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
                "main do IO.printLine(double.to(String).or(\"\")) end\n"
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
