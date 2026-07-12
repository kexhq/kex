#include "test.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"

using namespace kex;
using namespace test;

auto parse(const std::string& source) -> ast::Program {
    Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    return parser.parseProgram();
}

auto parseFails(const std::string& source) -> bool {
    Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    parser.parseProgram();
    return !parser.diagnostics().empty();
}

auto itemCount(const std::string& source) -> size_t {
    return parse(source).items.size();
}

template<typename T>
auto firstItemIs(const ast::Program& program) -> bool {
    if (program.items.empty()) return false;
    return std::holds_alternative<T>(program.items[0]);
}

int main() {
    describe("Parser — Top Level", []() {
        it("parses empty program", []() {
            auto program = parse("");
            assertEqual(program.items.size(), size_t(0));
        });

        it("parses function definitions", []() {
            auto program = parse("let double(n: Int) = n * 2");
            assertEqual(program.items.size(), size_t(1));
            assertTrue(firstItemIs<std::unique_ptr<ast::FunctionDef>>(program));
        });

        it("parses multiple function clauses", []() {
            auto program = parse(
                "let factorial(0) = 1\n"
                "let factorial(n: Int) = n * factorial(n - 1)\n"
            );
            assertEqual(program.items.size(), size_t(2));
        });

        it("parses main block", []() {
            auto program = parse("main do\n  let x = 5\nend");
            assertEqual(program.items.size(), size_t(1));
            assertTrue(firstItemIs<std::unique_ptr<ast::MainBlock>>(program));
        });

        it("parses foul function", []() {
            auto program = parse("foul readFile(path: String) = BuiltIn.read(path)");
            assertTrue(firstItemIs<std::unique_ptr<ast::FunctionDef>>(program));
            auto& func = std::get<std::unique_ptr<ast::FunctionDef>>(program.items[0]);
            assertTrue(func->isFoul);
        });
    });

    describe("Parser — Modules", []() {
        it("parses simple module", []() {
            auto program = parse("module Math do\nend");
            assertTrue(firstItemIs<std::unique_ptr<ast::ModuleDef>>(program));
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            assertEqual(mod->name, std::string("Math"));
        });

        it("parses foul module", []() {
            auto program = parse("foul module IO do\nend");
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            assertTrue(mod->isFoul);
        });

        it("parses a qualified module name", []() {
            auto program = parse("module Http.Router do\nend");
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            assertEqual(mod->name, std::string("Http.Router"));
        });

        it("desugars a standalone file module", []() {
            auto program = parse(
                "module Math\n"
                "let twice(n: Int) = n * 2\n"
            );
            assertEqual(program.items.size(), size_t(1));
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            assertEqual(mod->name, std::string("Math"));
            assertEqual(mod->body.size(), size_t(1));
        });

        it("keeps main outside a standalone module", []() {
            auto program = parse(
                "module App\n"
                "let answer() = 42\n"
                "main do\n"
                "  App.answer()\n"
                "end\n");
            assertEqual(program.items.size(), size_t(2));
            assertTrue(std::holds_alternative<std::unique_ptr<ast::ModuleDef>>(program.items[0]));
            assertTrue(std::holds_alternative<std::unique_ptr<ast::MainBlock>>(program.items[1]));
        });

        it("rejects a standalone module after another statement", []() {
            assertTrue(parseFails(
                "let before() = 1\n"
                "module App\n"
                "let answer() = 42\n"));
        });

        it("parses module with functions", []() {
            auto program = parse(
                "module Math do\n"
                "  let abs(n: Int) = n\n"
                "end\n"
            );
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            assertEqual(mod->body.size(), size_t(1));
        });

        it("parses nested modules", []() {
            auto program = parse(
                "module A do\n"
                "  module B do\n"
                "  end\n"
                "end\n"
            );
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            assertEqual(mod->body.size(), size_t(1));
            auto& nested = std::get<std::unique_ptr<ast::ModuleDef>>(mod->body[0]);
            assertEqual(nested->name, std::string("A.B"));
        });

        it("parses using alias and selective imports", []() {
            auto program = parse(
                "module App do\n"
                "  using Http.Router, as: Router, only: [get, Request]\n"
                "end\n"
            );
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            auto& usingBlock = std::get<std::unique_ptr<ast::UsingBlock>>(mod->body[0]);
            assertTrue(usingBlock->alias.has_value());
            assertEqual(*usingBlock->alias, std::string("Router"));
            assertEqual(usingBlock->onlyNames.size(), size_t(2));
            assertEqual(usingBlock->onlyNames[0], std::string("get"));
            assertEqual(usingBlock->onlyNames[1], std::string("Request"));
        });

        it("parses using except with operators", []() {
            auto program = parse(
                "module App do\n"
                "  using Math, except: [(+), (==)]\n"
                "end\n"
            );
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            auto& usingBlock = std::get<std::unique_ptr<ast::UsingBlock>>(mod->body[0]);
            assertEqual(usingBlock->exceptNames.size(), size_t(2));
            assertEqual(usingBlock->exceptNames[0], std::string("+"));
            assertEqual(usingBlock->exceptNames[1], std::string("=="));
        });

        it("rejects using only and except together", []() {
            assertTrue(parseFails(
                "module App do\n"
                "  using Math, only: [sqrt], except: [sin]\n"
                "end\n"
            ));
        });

        it("parses export declaration options", []() {
            auto program = parse(
                "module App do\n"
                "  export Http.Methods, as: Methods, only: [get, (+)]\n"
                "end\n"
            );
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            auto& exportDecl = std::get<std::unique_ptr<ast::ExportDecl>>(mod->body[0]);
            assertTrue(exportDecl->alias.has_value());
            assertEqual(*exportDecl->alias, std::string("Methods"));
            assertEqual(exportDecl->onlyNames.size(), size_t(2));
            assertEqual(exportDecl->onlyNames[0], std::string("get"));
            assertEqual(exportDecl->onlyNames[1], std::string("+"));
        });
    });

    describe("Parser — Types", []() {
        it("parses simple type declaration", []() {
            auto program = parse("type Integer");
            assertTrue(firstItemIs<std::unique_ptr<ast::TypeDef>>(program));
        });

        it("parses type with inheritance", []() {
            auto program = parse("type Integer < Number, Comparable");
            auto& td = std::get<std::unique_ptr<ast::TypeDef>>(program.items[0]);
            assertEqual(td->name, std::string("Integer"));
            assertEqual(td->parents.size(), size_t(2));
        });

        it("parses sum type", []() {
            auto program = parse("type Option<A> = Just(A) | Nothing");
            auto& td = std::get<std::unique_ptr<ast::TypeDef>>(program.items[0]);
            assertEqual(td->name, std::string("Option"));
            assertEqual(td->typeParams.size(), size_t(1));
            assertTrue(td->variants.has_value());
            assertEqual(td->variants->size(), size_t(2));
        });

        it("parses multiline sum type", []() {
            auto program = parse(
                "type Shape\n"
                "  = Circle(Float)\n"
                "  | Rectangle(Float, Float)\n"
            );
            auto& td = std::get<std::unique_ptr<ast::TypeDef>>(program.items[0]);
            assertEqual(td->variants->size(), size_t(2));
        });

        it("parses type alias with function type", []() {
            auto program = parse("type Handler = Request -> Response");
            auto& td = std::get<std::unique_ptr<ast::TypeDef>>(program.items[0]);
            assertTrue(td->variants.has_value());
            assertEqual(td->variants->size(), size_t(1));
        });

        it("parses abstract type with required functions", []() {
            auto program = parse(
                "type Comparable do\n"
                "  compare :> This -> This -> Int\n"
                "end\n"
            );
            auto& td = std::get<std::unique_ptr<ast::TypeDef>>(program.items[0]);
            assertTrue(td->abstractFunctions.has_value());
            assertEqual(td->abstractFunctions->size(), size_t(1));
        });
    });

    describe("Parser — Records", []() {
        it("parses simple record", []() {
            auto program = parse(
                "record User do\n"
                "  name : String\n"
                "  age : Int\n"
                "end\n"
            );
            assertTrue(firstItemIs<std::unique_ptr<ast::RecordDef>>(program));
            auto& rec = std::get<std::unique_ptr<ast::RecordDef>>(program.items[0]);
            assertEqual(rec->name, std::string("User"));
            assertEqual(rec->fields.size(), size_t(2));
        });

        it("parses record with defaults", []() {
            auto program = parse(
                "record Config do\n"
                "  port : Int = 8080\n"
                "  host : String = \"localhost\"\n"
                "end\n"
            );
            auto& rec = std::get<std::unique_ptr<ast::RecordDef>>(program.items[0]);
            assertTrue(rec->fields[0].defaultValue.has_value());
            assertTrue(rec->fields[1].defaultValue.has_value());
        });

        it("parses record with type params", []() {
            auto program = parse(
                "record Pair<A, B> do\n"
                "  first : A\n"
                "  second : B\n"
                "end\n"
            );
            auto& rec = std::get<std::unique_ptr<ast::RecordDef>>(program.items[0]);
            assertEqual(rec->typeParams.size(), size_t(2));
        });
    });

    describe("Parser — Make Blocks", []() {
        it("parses make with type target", []() {
            auto program = parse(
                "make Integer do\n"
                "  let double = this * 2\n"
                "end\n"
            );
            assertTrue(firstItemIs<std::unique_ptr<ast::MakeDef>>(program));
        });

        it("parses make with final modifier", []() {
            auto program = parse(
                "make final: Bool do\n"
                "  let negate = !this\n"
                "end\n"
            );
            auto& make = std::get<std::unique_ptr<ast::MakeDef>>(program.items[0]);
            assertTrue(make->isFinal);
        });

        it("parses make with list type", []() {
            auto program = parse(
                "make [A] do\n"
                "  let first(@[x | _]) = Just(x)\n"
                "end\n"
            );
            assertTrue(firstItemIs<std::unique_ptr<ast::MakeDef>>(program));
        });

        it("parses make with specialized type", []() {
            auto program = parse(
                "make [Int] do\n"
                "  let sum = this.reduce(0, &.+)\n"
                "end\n"
            );
            assertTrue(firstItemIs<std::unique_ptr<ast::MakeDef>>(program));
        });
    });

    describe("Parser — Expressions", []() {
        it("parses let binding", []() {
            auto program = parse("main do\n  let x = 5\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses var binding", []() {
            auto program = parse("main do\n  var x = 0\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses binary operations", []() {
            auto program = parse("main do\n  let x = 1 + 2 * 3\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses method calls", []() {
            auto program = parse("main do\n  let x = list.map(&.name)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses mutating calls", []() {
            auto program = parse("main do\n  var x = [1, 2]\n  x.push!(3)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(2));
        });

        it("parses or! result type sugar", []() {
            auto program = parse("let f(x: String) -> Integer or! String do\n  Ok(1)\nend");
            auto& fn = std::get<std::unique_ptr<ast::FunctionDef>>(program.items[0]);
            assertEqual(fn->name, std::string("f"));
        });

        it("parses if expression", []() {
            auto program = parse(
                "main do\n"
                "  if x > 0\n"
                "    x\n"
                "  else\n"
                "    -x\n"
                "  end\n"
                "end\n"
            );
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses match expression", []() {
            auto program = parse(
                "main do\n"
                "  match x do\n"
                "    0 -> \"zero\"\n"
                "    _ -> \"other\"\n"
                "  end\n"
                "end\n"
            );
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses trailing if", []() {
            auto program = parse("main do\n  return x if condition\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses spawn", []() {
            auto program = parse(
                "main do\n"
                "  let pid = spawn do\n"
                "    loop\n"
                "      receive do\n"
                "        :ping -> :pong\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end\n"
            );
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });
    });

    describe("Parser — Receive clause body", []() {
        // Helpers to extract a ReceiveExpr from a foul function's body.
        auto getFoulReceive = [](const std::string& src) -> const ast::ReceiveExpr& {
            Lexer lexer(src);
            auto tokens = lexer.tokenizeAll();
            Parser parser(std::move(tokens));
            auto prog = parser.parseProgram();
            auto& fn = std::get<std::unique_ptr<ast::FunctionDef>>(prog.items[0]);
            auto& body = fn->clauses[0].body;
            return std::get<ast::ReceiveExpr>(body[0]->kind);
        };

        it("parses inline single-expression clause body", []() {
            auto prog = parse(
                "# kex: no-check\n"
                "foul loop do\n"
                "  receive do\n"
                "    :ping -> :pong\n"
                "  end\n"
                "end\n"
            );
            assertFalse(parseFails(
                "# kex: no-check\n"
                "foul loop do\n"
                "  receive do\n"
                "    :ping -> :pong\n"
                "  end\n"
                "end\n"
            ));
            assertEqual(prog.items.size(), size_t(1));
        });

        it("parses multi-line clause body (single expression on next line)", []() {
            // The body starts on the line after '->'; must not be misread as a new clause.
            assertFalse(parseFails(
                "# kex: no-check\n"
                "foul loop do\n"
                "  receive do\n"
                "    :ping ->\n"
                "      IO.printLine(\"pong\")\n"
                "    :stop -> :done\n"
                "  end\n"
                "end\n"
            ));
        });

        it("multi-statement arm body with do...end keeps enclosing foul function", []() {
            auto prog = parse(
                "# kex: no-check\n"
                "foul counter(name: String, n: Int) do\n"
                "  receive do\n"
                "    :ping -> do\n"
                "      IO.printLine(name)\n"
                "      counter(name, n + 1)\n"
                "    end\n"
                "    :boom -> IO.printLine(\"crash\")\n"
                "  end\n"
                "end\n"
                "main do\n"
                "  IO.printLine(\"hi\")\n"
                "end\n"
            );
            // Both the foul function AND main must be present.
            assertEqual(prog.items.size(), size_t(2));
            assertTrue((std::holds_alternative<std::unique_ptr<ast::FunctionDef>>(prog.items[0])));
            assertTrue((std::holds_alternative<std::unique_ptr<ast::MainBlock>>(prog.items[1])));
        });

        it("do...end arm body with two expressions returns a block", []() {
            auto prog = parse(
                "# kex: no-check\n"
                "foul counter(n: Int) do\n"
                "  receive do\n"
                "    :ping -> do\n"
                "      IO.printLine(\"ping\")\n"
                "      counter(n + 1)\n"
                "    end\n"
                "  end\n"
                "end\n"
            );
            auto& fn  = std::get<std::unique_ptr<ast::FunctionDef>>(prog.items[0]);
            auto& recv = std::get<ast::ReceiveExpr>(fn->clauses[0].body[0]->kind);
            assertEqual(recv.clauses.size(), size_t(1));
            // The two-expression body must be wrapped in a BlockExpr.
            assertTrue(std::holds_alternative<ast::BlockExpr>(recv.clauses[0].body->kind));
            auto& block = std::get<ast::BlockExpr>(recv.clauses[0].body->kind);
            assertEqual(block.body.size(), size_t(2));
        });

        it("multi-line body with single expression is not wrapped in a block", []() {
            auto prog = parse(
                "# kex: no-check\n"
                "foul loop do\n"
                "  receive do\n"
                "    :ping ->\n"
                "      counter(1)\n"
                "  end\n"
                "end\n"
            );
            auto& fn   = std::get<std::unique_ptr<ast::FunctionDef>>(prog.items[0]);
            auto& recv = std::get<ast::ReceiveExpr>(fn->clauses[0].body[0]->kind);
            assertEqual(recv.clauses.size(), size_t(1));
            // Single expression: returned as-is, not wrapped.
            assertFalse(std::holds_alternative<ast::BlockExpr>(recv.clauses[0].body->kind));
        });

        it("do...end arm body correctly separates two clauses", []() {
            auto prog = parse(
                "# kex: no-check\n"
                "foul counter(n: Int) do\n"
                "  receive do\n"
                "    :ping -> do\n"
                "      IO.printLine(\"ping\")\n"
                "      counter(n + 1)\n"
                "    end\n"
                "    :stop -> :done\n"
                "  end\n"
                "end\n"
            );
            auto& fn   = std::get<std::unique_ptr<ast::FunctionDef>>(prog.items[0]);
            auto& recv = std::get<ast::ReceiveExpr>(fn->clauses[0].body[0]->kind);
            // Must have exactly 2 clauses: :ping and :stop.
            assertEqual(recv.clauses.size(), size_t(2));
        });

        it("tuple pattern clause after multi-line body is recognised", []() {
            auto prog = parse(
                "# kex: no-check\n"
                "foul counter(n: Int) do\n"
                "  receive do\n"
                "    :ping ->\n"
                "      counter(n + 1)\n"
                "    (:get, sender) -> sender\n"
                "  end\n"
                "end\n"
            );
            auto& fn   = std::get<std::unique_ptr<ast::FunctionDef>>(prog.items[0]);
            auto& recv = std::get<ast::ReceiveExpr>(fn->clauses[0].body[0]->kind);
            assertEqual(recv.clauses.size(), size_t(2));
        });

        it("function call on next line is NOT a clause boundary", []() {
            // counter(name, n+1) must be body expression, not a new clause pattern.
            auto prog = parse(
                "# kex: no-check\n"
                "foul counter(name: String, n: Int) do\n"
                "  receive do\n"
                "    :ping ->\n"
                "      counter(name, n + 1)\n"
                "  end\n"
                "end\n"
            );
            auto& fn   = std::get<std::unique_ptr<ast::FunctionDef>>(prog.items[0]);
            auto& recv = std::get<ast::ReceiveExpr>(fn->clauses[0].body[0]->kind);
            // Only one clause: :ping
            assertEqual(recv.clauses.size(), size_t(1));
            // Its body is counter(name, n+1) — a FunctionCall, not a wildcard/atom
            assertFalse(std::holds_alternative<ast::BlockExpr>(recv.clauses[0].body->kind));
        });

        it("parses receive with explicit do...end clause body", []() {
            assertFalse(parseFails(
                "# kex: no-check\n"
                "foul loop do\n"
                "  receive do\n"
                "    :ping -> do\n"
                "      IO.printLine(\"ping\")\n"
                "      loop()\n"
                "    end\n"
                "  end\n"
                "end\n"
            ));
        });

        it("parses receive with timeout", []() {
            auto prog = parse(
                "# kex: no-check\n"
                "foul wait do\n"
                "  receive timeout: 500 do\n"
                "    :msg -> :got\n"
                "  after -> :timeout\n"
                "  end\n"
                "end\n"
            );
            auto& fn   = std::get<std::unique_ptr<ast::FunctionDef>>(prog.items[0]);
            auto& recv = std::get<ast::ReceiveExpr>(fn->clauses[0].body[0]->kind);
            assertTrue(recv.timeout.has_value());
            assertTrue(recv.afterBody.has_value());
        });
    });

    describe("Parser — Lambdas", []() {
        it("parses inline lambda", []() {
            auto program = parse("main do\n  let f = { |x| x + 1 }\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses zero-arg lambda", []() {
            auto program = parse("main do\n  let f = { 42 }\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses do-end lambda", []() {
            auto program = parse(
                "main do\n"
                "  list.each do |x|\n"
                "    print(x)\n"
                "  end\n"
                "end\n"
            );
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses shorthand method lambda", []() {
            auto program = parse("main do\n  let x = list.map(&.name)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("desugars a chained shorthand method lambda", []() {
            auto program = parse(
                "main do\n"
                "  let x = (1..10).items.map(&.to(String).or(\"\"))\n"
                "end\n");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& let = std::get<ast::LetExpr>(main->body[0]->kind);
            auto& map = std::get<ast::MethodCall>(let.value->kind);
            assertTrue(std::holds_alternative<ast::Lambda>(map.args[0]->kind));
        });

        it("parses shorthand function lambda", []() {
            auto program = parse("main do\n  let x = list.sort(&compare)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses shorthand operator lambda", []() {
            auto program = parse("main do\n  let x = list.map(&.+ 1)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });
    });

    describe("Parser — Patterns", []() {
        it("parses destructuring let", []() {
            auto program = parse("main do\n  let { name, age } = user\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses tuple destructuring", []() {
            auto program = parse("main do\n  let (x, y) = point\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses list destructuring", []() {
            auto program = parse("main do\n  let [first | rest] = list\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses @ pattern in make", []() {
            auto program = parse(
                "make [A] do\n"
                "  let first(@[]) = Nothing\n"
                "  let first(@[x | _]) = Just(x)\n"
                "end\n"
            );
            assertTrue(firstItemIs<std::unique_ptr<ast::MakeDef>>(program));
        });

        it("parses constructor patterns in match", []() {
            auto program = parse(
                "main do\n"
                "  match opt do\n"
                "    Just(x) -> x\n"
                "    Nothing -> 0\n"
                "  end\n"
                "end\n"
            );
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses nested destructuring", []() {
            auto program = parse(
                "make Foo do\n"
                "  let bar({ config: { timeout: t } }) = t\n"
                "end\n"
            );
            assertTrue(firstItemIs<std::unique_ptr<ast::MakeDef>>(program));
        });
    });

    describe("Parser — Collections", []() {
        it("parses list literal", []() {
            auto program = parse("main do\n  let x = [1, 2, 3]\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses map literal", []() {
            auto program = parse("main do\n  let x = { \"a\": 1, \"b\": 2 }\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses tuple", []() {
            auto program = parse("main do\n  let x = (1, \"hello\", true)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses range", []() {
            auto program = parse("main do\n  let x = 1..10\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses bracket access", []() {
            auto program = parse("main do\n  let x = map[\"key\"]\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });
    });

    describe("Parser — Type Expressions", []() {
        it("parses simple type", []() {
            auto program = parse("let foo(x: Int) = x");
            assertTrue(firstItemIs<std::unique_ptr<ast::FunctionDef>>(program));
        });

        it("parses generic type", []() {
            auto program = parse("let foo(x: Map<String, Int>) = x");
            assertTrue(firstItemIs<std::unique_ptr<ast::FunctionDef>>(program));
        });

        it("parses optional type", []() {
            auto program = parse("let foo(x: String?) = x");
            assertTrue(firstItemIs<std::unique_ptr<ast::FunctionDef>>(program));
        });

        it("parses function type", []() {
            auto program = parse("let foo(f: Int -> String) = f(1)");
            assertTrue(firstItemIs<std::unique_ptr<ast::FunctionDef>>(program));
        });

        it("parses list type", []() {
            auto program = parse("let foo(x: [Int]) = x");
            assertTrue(firstItemIs<std::unique_ptr<ast::FunctionDef>>(program));
        });

        it("parses Block type", []() {
            auto program = parse("let foo(block: Block<[Int]>) = block()");
            assertTrue(firstItemIs<std::unique_ptr<ast::FunctionDef>>(program));
        });
    });

    describe("Parser — Using", []() {
        it("parses using with block", []() {
            auto program = parse(
                "using Html.Language do\n"
                "  html do\n"
                "  end\n"
                "end\n"
            );
            assertTrue(firstItemIs<std::unique_ptr<ast::UsingBlock>>(program));
        });

        it("parses bare using", []() {
            auto program = parse("using Test\n");
            assertTrue(firstItemIs<std::unique_ptr<ast::UsingBlock>>(program));
        });
    });

    describe("Parser — Pragma", []() {
        it("parses single requirement", []() {
            auto program = parse("#[IO]\n");
            assertTrue(firstItemIs<std::unique_ptr<ast::Pragma>>(program));
            auto& pragma = std::get<std::unique_ptr<ast::Pragma>>(program.items[0]);
            assertEqual(pragma->requirements.size(), size_t(1));
            assertEqual(pragma->requirements[0], std::string("IO"));
        });

        it("parses multiple requirements", []() {
            auto program = parse("#[Process, IO]\n");
            auto& pragma = std::get<std::unique_ptr<ast::Pragma>>(program.items[0]);
            assertEqual(pragma->requirements.size(), size_t(2));
        });
    });

    describe("Parser — Error Cases", []() {
        it("rejects unclosed do block", []() {
            assertTrue(parseFails("main do\n  let x = 5\n"));
        });

        it("rejects unclosed module", []() {
            assertTrue(parseFails("module Foo do\n"));
        });

        it("rejects invalid token at top level", []() {
            assertTrue(parseFails("+ 5"));
        });
    });

    return runAll();
}
