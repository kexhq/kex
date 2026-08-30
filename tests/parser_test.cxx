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
    describe("Parser — Source Spans", []() {
        it("nests binary expression byte spans", []() {
            auto program = parse("1 + 2 * 3");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(
                program.items[0]);
            auto& addition = *main->body[0];
            auto& additionOp = std::get<ast::BinaryOp>(addition.kind);
            auto& multiplication = *additionOp.right;

            assertEqual(addition.location.startOffset, 0);
            assertEqual(addition.location.endOffset, 9);
            assertEqual(multiplication.location.startOffset, 4);
            assertEqual(multiplication.location.endOffset, 9);
            assertTrue(addition.location.startOffset <=
                       multiplication.location.startOffset);
            assertTrue(multiplication.location.endOffset <=
                       addition.location.endOffset);
        });

        it("includes grouping and postfix syntax in type spans", []() {
            auto program = parse("value : (Int | String)?");
            auto& annotation =
                std::get<std::unique_ptr<ast::TypeAnnotation>>(program.items[0]);
            auto& optional = *annotation->type;
            auto& unionType = *std::get<ast::OptionalType>(optional.kind).inner;

            assertEqual(annotation->location.startOffset, 0);
            assertEqual(annotation->location.endOffset, 23);
            assertEqual(optional.location.startOffset, 8);
            assertEqual(optional.location.endOffset, 23);
            assertEqual(unionType.location.startOffset, 8);
            assertEqual(unionType.location.endOffset, 22);
        });

        it("includes pattern delimiters in parent spans", []() {
            auto program = parse("let pick([head | tail]) = head");
            auto& function = std::get<std::unique_ptr<ast::FunctionDef>>(
                program.items[0]);
            auto& list = **function->clauses[0].params[0].pattern;
            auto& listPattern = std::get<ast::ListPattern>(list.kind);

            assertEqual(list.location.startOffset, 9);
            assertEqual(list.location.endOffset, 22);
            assertTrue(list.location.startOffset <=
                       listPattern.elements[0]->location.startOffset);
            assertTrue(listPattern.elements[0]->location.endOffset <=
                       list.location.endOffset);
            assertTrue((*listPattern.rest)->location.endOffset <=
                       list.location.endOffset);
        });

        it("completes every step of a postfix chain", []() {
            auto program = parse("foo.bar(1)[0]");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(
                program.items[0]);
            auto& indexCall = std::get<ast::MethodCall>(main->body[0]->kind);
            auto& methodCall =
                std::get<ast::MethodCall>(indexCall.receiver->kind);

            assertEqual(indexCall.receiver->location.startOffset, 0);
            assertEqual(indexCall.receiver->location.endOffset, 10);
            assertEqual(main->body[0]->location.startOffset, 0);
            assertEqual(main->body[0]->location.endOffset, 13);
            assertEqual(methodCall.receiver->location.startOffset, 0);
            assertEqual(methodCall.receiver->location.endOffset, 3);
        });

        it("extends spread nodes through their operand", []() {
            auto program = parse("[...items]");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(
                program.items[0]);
            auto& list = std::get<ast::ListExpr>(main->body[0]->kind);

            assertEqual(list.elements[0]->location.startOffset, 1);
            assertEqual(list.elements[0]->location.endOffset, 9);
        });

        it("includes effect markers and closing ends in declaration spans", []() {
            const std::string source =
                "module Demo do\n"
                "  foul run() = 1\n"
                "end";
            auto program = parse(source);
            auto& module = std::get<std::unique_ptr<ast::ModuleDef>>(
                program.items[0]);
            auto& function = std::get<std::unique_ptr<ast::FunctionDef>>(
                module->body[0]);

            assertEqual(module->location.startOffset, 0);
            assertEqual(module->location.endOffset,
                        static_cast<int>(source.size()));
            assertEqual(function->location.startOffset,
                        static_cast<int>(source.find("foul")));
            assertEqual(function->location.endOffset,
                        static_cast<int>(source.find('\n',
                                                     source.find("foul"))));
            assertTrue(module->location.startOffset <=
                       function->location.startOffset);
            assertTrue(function->location.endOffset <=
                       module->location.endOffset);
        });

        it("keeps UTF-8 interpolation spans in outer-source bytes", []() {
            const std::string source = "$`é ${1 + 2}`";
            auto program = parse(source);
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(
                program.items[0]);
            auto& literal = std::get<ast::StringLiteral>(main->body[0]->kind);
            auto& showCall = std::get<ast::MethodCall>(literal.values[0]->kind);
            auto& interpolation = *showCall.receiver;
            const auto start = static_cast<int>(source.find("1 + 2"));

            assertEqual(interpolation.location.startOffset, start);
            assertEqual(interpolation.location.endOffset, start + 5);
            assertEqual(std::get<ast::BinaryOp>(interpolation.kind)
                            .left->location.endOffset,
                        start + 1);
            assertTrue(interpolation.location.endOffset <=
                       main->body[0]->location.endOffset);
        });
    });

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

        it("parses an = body that opens on the next line", []() {
            auto program = parse(
                "let double(n: Int) -> Int\n"
                "  = n * 2\n"
            );
            assertEqual(program.items.size(), size_t(1));
            auto& def = std::get<std::unique_ptr<ast::FunctionDef>>(program.items[0]);
            assertEqual(def->clauses.size(), size_t(1));
            assertEqual(def->clauses[0].body.size(), size_t(1));
        });

        it("parses a return type that opens on the next line", []() {
            auto program = parse(
                "let chooseTag(name: String, git: String)\n"
                "    -> Result<String, String> do\n"
                "  return Ok(name)\n"
                "end\n"
            );
            assertEqual(program.items.size(), size_t(1));
            auto& def = std::get<std::unique_ptr<ast::FunctionDef>>(program.items[0]);
            assertTrue(def->clauses[0].returnAnnotation.has_value());
        });

        it("leaves a signature-only declaration alone", []() {
            // The lookahead past newlines must not consume the NEXT
            // declaration when no `=` follows the signature.
            auto program = parse(
                "let twice(n: Int) = n * 2\n"
                "\n"
                "let thrice(n: Int) = n * 3\n"
            );
            assertEqual(program.items.size(), size_t(2));
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

        it("parses before and after test hooks", []() {
            auto program = parse(
                "describe(\"hooks\") do\n"
                "  before { setup() }\n"
                "  after(:all) { cleanup() }\n"
                "end\n");
            assertEqual(program.items.size(), size_t(1));
        });

        it("allows after as a top-level annotation name", []() {
            assertTrue(!parseFails("after : Block<Void> -> Void\n"));
        });
    });

    describe("Parser — Tagged Literals", []() {
        it("keeps an adjacent raw tag as a dedicated AST node", []() {
            auto program = parse("main do\n  csv`a,b`\nend\n");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(
                program.items[0]);
            assertEqual(main->body.size(), size_t(1));
            assertTrue(std::holds_alternative<ast::TaggedLiteral>(
                main->body[0]->kind));
            auto& tagged = std::get<ast::TaggedLiteral>(main->body[0]->kind);
            assertEqual(tagged.tag, std::string("csv"));
            assertEqual(tagged.parts.size(), size_t(1));
            assertEqual(tagged.parts[0], std::string("a,b"));
            assertEqual(tagged.values.size(), size_t(0));
            assertFalse(tagged.interpolating);
        });

        it("requires byte adjacency between tag and body", []() {
            auto program = parse("main do\n  csv `a,b`\nend\n");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(
                program.items[0]);
            assertTrue(!main->body.empty());
            assertFalse(std::holds_alternative<ast::TaggedLiteral>(
                main->body[0]->kind));
        });

        it("parses interpolating backticks into parts and AST values", []() {
            auto program = parse(
                "main do\n  $`hello ${name}, ${1 + 2}!`\nend\n");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(
                program.items[0]);
            auto& literal =
                std::get<ast::StringLiteral>(main->body[0]->kind);
            assertEqual(literal.parts.size(), size_t(3));
            assertEqual(literal.parts[0], std::string("hello "));
            assertEqual(literal.parts[1], std::string(", "));
            assertEqual(literal.parts[2], std::string("!"));
            assertEqual(literal.values.size(), size_t(2));
            // Each interpolation is desugared to a show-protocol call, so the
            // prelude decides how the value renders. The parsed expression is
            // the receiver.
            auto& first = std::get<ast::MethodCall>(literal.values[0]->kind);
            auto& second = std::get<ast::MethodCall>(literal.values[1]->kind);
            assertEqual(first.method, std::string("showValue"));
            assertEqual(second.method, std::string("showValue"));
            assertTrue(std::holds_alternative<ast::Identifier>(
                first.receiver->kind));
            assertTrue(std::holds_alternative<ast::BinaryOp>(
                second.receiver->kind));
        });

        it("parses interpolating tags into the parts and values ABI", []() {
            auto program = parse(
                "main do\n  html$`<p>${body}</p>`\nend\n");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(
                program.items[0]);
            auto& tagged =
                std::get<ast::TaggedLiteral>(main->body[0]->kind);
            assertTrue(tagged.interpolating);
            assertEqual(tagged.parts.size(), size_t(2));
            assertEqual(tagged.parts[0], std::string("<p>"));
            assertEqual(tagged.parts[1], std::string("</p>"));
            assertEqual(tagged.values.size(), size_t(1));
        });

        it("turns $${ into literal interpolation syntax", []() {
            auto program = parse(
                "main do\n  $`literal $${name}`\nend\n");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(
                program.items[0]);
            auto& literal =
                std::get<ast::StringLiteral>(main->body[0]->kind);
            assertEqual(literal.parts.size(), size_t(1));
            assertEqual(
                literal.parts[0], std::string("literal ${name}"));
            assertTrue(literal.values.empty());
        });

        it("handles delimiters inside interpolation expressions", []() {
            assertFalse(parseFails(
                "main do\n"
                "  $`values: ${\"{\"}, ${`raw`}`\n"
                "end\n"));
        });

        it("rejects an unterminated interpolation hole", []() {
            assertTrue(parseFails(
                "main do\n"
                "  $`value: ${name`\n"
                "end\n"));
        });
    });

    describe("Parser — Exponentiation", []() {
        it("parses right-associative powers with the expected precedence", []() {
            assertTrue(!parseFails("main do\n  let x = -2 ^ 3 ^ 2 * 4\nend\n"));
        });
    });

    describe("Parser — Modules", []() {
        it("parses simple module", []() {
            auto program = parse("module Math do\nend");
            assertTrue(firstItemIs<std::unique_ptr<ast::ModuleDef>>(program));
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            assertEqual(mod->name, std::string("Math"));
        });

        it("rejects foul module", []() {
            // Effects are per-function: a module never carries one, so a `let`
            // inside it is never silently foul (kexhq/kex#130).
            assertTrue(parseFails("foul module IO do\nend"));
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

        it("keeps parsing a standalone module after ended declarations", []() {
            auto program = parse(
                "module Records\n"
                "record Entry do\n"
                "  value : Int\n"
                "end\n"
                "let buildEntry() do\n"
                "  Records.Entry { value: 42 }\n"
                "end\n"
                "let answer() = 42\n"
            );
            assertEqual(program.items.size(), size_t(1));
            auto& mod = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            assertEqual(mod->body.size(), size_t(3));
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

    describe("Parser — Records", []() {
        it("allows timeout as a field name", []() {
            auto program = parse(
                "record HttpOptions do\n"
                "  timeout : Integer = 30000\n"
                "end\n");
            assertTrue(firstItemIs<std::unique_ptr<ast::RecordDef>>(program));
            auto& record =
                std::get<std::unique_ptr<ast::RecordDef>>(program.items[0]);
            assertEqual(record->fields.size(), size_t(1));
            assertEqual(record->fields[0].name, std::string("timeout"));
            assertTrue(record->fields[0].defaultValue.has_value());
        });

        it("allows timeout in record construction", []() {
            assertTrue(!parseFails("let opts = HttpOptions { timeout: 5000 }"));
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
                "  let sum = this.reduce(0, ~(+))\n"
                "end\n"
            );
            assertTrue(firstItemIs<std::unique_ptr<ast::MakeDef>>(program));
        });

        // Regression for #246: `next` is the loop-continue keyword, but the
        // lexer produced that token even in member/name position, where a
        // loop-continue statement can never appear. A method literally named
        // `next` (a natural name for "advance and yield the next element")
        // failed to parse its declaration, and the parser's error recovery
        // silently dropped every declaration after it out of the `make`
        // block with no diagnostic at all.
        it("allows 'next' as a method name declared and called in a make block", []() {
            auto program = parse(
                "make Feed do\n"
                "  next :> Integer?\n"
                "  let next = 1\n"
                "\n"
                "  take :> Integer -> [Integer]\n"
                "  let take(n) = [1]\n"
                "end\n"
                "\n"
                "let useIt(f) = f.next\n"
            );
            assertTrue(!parseFails(
                "make Feed do\n"
                "  next :> Integer?\n"
                "  let next = 1\n"
                "\n"
                "  take :> Integer -> [Integer]\n"
                "  let take(n) = [1]\n"
                "end\n"
                "\n"
                "let useIt(f) = f.next\n"
            ), "declaring and calling a 'next' method must not be a parse error");
            assertEqual(program.items.size(), size_t{2});
            auto& make = std::get<std::unique_ptr<ast::MakeDef>>(program.items[0]);
            // Both 'next' (contract + let) and 'take' (contract + let) must
            // survive — 'take' silently vanishing was the reported bug.
            assertEqual(make->body.size(), size_t{4});
        });

        it("still lexes 'next' as loop-continue inside a loop body", []() {
            assertTrue(!parseFails(
                "main do\n"
                "  loop do\n"
                "    next if true\n"
                "  end\n"
                "end\n"
            ));
        });
    });

    describe("Parser — Serving Blocks", []() {
        it("parses serving with call and cast slots", []() {
            auto program = parse(
                "record Counter do count : Integer = 0 end\n"
                "serving Counter do\n"
                "  increment ::> Integer -> Reply<Integer>\n"
                "  slot increment(by: Integer) = { reply: by }\n"
                "  slot reset = new\n"
                "end\n");
            assertEqual(program.items.size(), size_t(2));
            auto& serving = std::get<std::unique_ptr<ast::MakeDef>>(program.items[1]);
            assertTrue(serving->isServing);
            assertEqual(serving->body.size(), size_t(3));
            auto& ann = std::get<std::unique_ptr<ast::TypeAnnotation>>(serving->body[0]);
            assertTrue(ann->implicitThis);
            assertTrue(ann->implicitFrom);
            auto& call = std::get<std::unique_ptr<ast::FunctionDef>>(serving->body[1]);
            auto& cast = std::get<std::unique_ptr<ast::FunctionDef>>(serving->body[2]);
            assertTrue(call->isSlot);
            assertTrue(cast->isSlot);
        });

        it("desugars the new transition shorthand", []() {
            auto program = parse("main do\n  { new, reply: 1 }\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& map = std::get<ast::MapExpr>(main->body[0]->kind);
            assertEqual(map.entries.size(), size_t(2));
            auto& key = std::get<ast::AtomLiteral>(map.entries[0].key->kind);
            auto& value = std::get<ast::Identifier>(map.entries[0].value->kind);
            assertEqual(key.name, std::string("new"));
            assertEqual(value.name, std::string("new"));

            auto singleton = parse("main do\n  { new }\nend");
            auto& singletonMain = std::get<std::unique_ptr<ast::MainBlock>>(singleton.items[0]);
            auto& singletonMap = std::get<ast::MapExpr>(singletonMain->body[0]->kind);
            assertEqual(singletonMap.entries.size(), size_t(1));
        });
    });

    describe("Parser — Expressions", []() {
        it("allows spawn as a callable member name", []() {
            auto program = parse("main do\n  Process.spawn(1)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& call = std::get<ast::MethodCall>(main->body[0]->kind);
            assertEqual(call.method, std::string("spawn"));
        });

        it("allows spawn as a module function name", []() {
            auto program = parse(
                "module Process do\n"
                "  spawn : X -> X\n"
                "  foul spawn(value) = value\n"
                "end\n");
            auto& module = std::get<std::unique_ptr<ast::ModuleDef>>(program.items[0]);
            assertEqual(module->body.size(), size_t(2));
        });

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
                "    0 => \"zero\"\n"
                "    _ => \"other\"\n"
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
                "    loop do\n"
                "      receive do\n"
                "        :ping => :pong\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end\n"
            );
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses while loop with a required 'do'", []() {
            auto program = parse(
                "main do\n"
                "  var i = 0\n"
                "  while i < 3 do\n"
                "    i = i + 1\n"
                "  end\n"
                "end\n"
            );
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& loop = std::get<ast::WhileExpr>(main->body[1]->kind);
            assertEqual(loop.body.size(), size_t(1));
        });

        it("rejects while without 'do'", []() {
            assertTrue(parseFails(
                "main do\n  var i = 0\n  while i < 3\n    i = i + 1\n  end\nend\n"));
        });

        it("desugars '+=' to 'x = x + value'", []() {
            auto program = parse("main do\n  var x = 1\n  x += 2\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& assign = std::get<ast::AssignExpr>(main->body[1]->kind);
            assertEqual(assign.name, std::string("x"));
            auto& binary = std::get<ast::BinaryOp>(assign.value->kind);
            assertTrue(binary.op == TokenType::Plus);
            assertEqual(std::get<ast::Identifier>(binary.left->kind).name,
                        std::string("x"));
        });

        it("desugars '&&=' and '||=' to their binary op", []() {
            auto program = parse(
                "main do\n"
                "  var a = true\n"
                "  a &&= false\n"
                "  var b = false\n"
                "  b ||= true\n"
                "end\n"
            );
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& andAssign = std::get<ast::AssignExpr>(main->body[1]->kind);
            assertTrue(std::get<ast::BinaryOp>(andAssign.value->kind).op ==
                       TokenType::AmpAmp);
            auto& orAssign = std::get<ast::AssignExpr>(main->body[3]->kind);
            assertTrue(std::get<ast::BinaryOp>(orAssign.value->kind).op ==
                       TokenType::PipePipe);
        });

        it("desugars compound assignment on a record field path", []() {
            auto program = parse("main do\n  var x = box\n  x.value *= 3\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& assign = std::get<ast::AssignExpr>(main->body[1]->kind);
            assertEqual(assign.name, std::string("x"));
            assertEqual(assign.path.size(), size_t(1));
            assertEqual(assign.path[0], std::string("value"));
            auto& binary = std::get<ast::BinaryOp>(assign.value->kind);
            assertTrue(binary.op == TokenType::Star);
            auto& fieldRead = std::get<ast::MethodCall>(binary.left->kind);
            assertEqual(fieldRead.method, std::string("value"));
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

        it("parses sender binding", []() {
            auto prog = parse(
                "foul wait do\n"
                "  receive do |sender|\n"
                "    :ping => sender\n"
                "  end\n"
                "end\n");
            auto& fn = std::get<std::unique_ptr<ast::FunctionDef>>(prog.items[0]);
            auto& recv = std::get<ast::ReceiveExpr>(fn->clauses[0].body[0]->kind);
            assertTrue(recv.senderBinding.has_value());
            assertEqual(*recv.senderBinding, std::string("sender"));
        });

        it("parses inline single-expression clause body", []() {
            auto prog = parse(
                "# kex: no-check\n"
                "foul loop do\n"
                "  receive do\n"
                "    :ping => :pong\n"
                "  end\n"
                "end\n"
            );
            assertFalse(parseFails(
                "# kex: no-check\n"
                "foul loop do\n"
                "  receive do\n"
                "    :ping => :pong\n"
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
                "    :ping =>\n"
                "      IO.printLine(\"pong\")\n"
                "    :stop => :done\n"
                "  end\n"
                "end\n"
            ));
        });

        it("multi-statement arm body with do...end keeps enclosing foul function", []() {
            auto prog = parse(
                "# kex: no-check\n"
                "foul counter(name: String, n: Int) do\n"
                "  receive do\n"
                "    :ping => do\n"
                "      IO.printLine(name)\n"
                "      counter(name, n + 1)\n"
                "    end\n"
                "    :boom => IO.printLine(\"crash\")\n"
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
                "    :ping => do\n"
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
                "    :ping =>\n"
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
                "    :ping => do\n"
                "      IO.printLine(\"ping\")\n"
                "      counter(n + 1)\n"
                "    end\n"
                "    :stop => :done\n"
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
                "    :ping =>\n"
                "      counter(n + 1)\n"
                "    (:get, sender) => sender\n"
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
                "    :ping =>\n"
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
                "    :ping => do\n"
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
                "  receive do\n"
                "    :msg => :got\n"
                "  after timeout: 500\n"
                "    :timeout\n"
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

        it("rejects &.operator — capture is spelled with ~(op)", []() {
            assertTrue(parseFails("main do\n  let x = list.map(&.+ 1)\nend"));
            assertTrue(parseFails("main do\n  let x = list.reduce(0, &.+)\nend"));
        });

        it("rejects &function — capture is spelled with ~", []() {
            assertTrue(parseFails("main do\n  let x = list.sort(&compare)\nend"));
        });

        it("rejects &operator — capture is spelled with ~(op)", []() {
            assertTrue(parseFails("main do\n  let x = list.reduce(0, &+)\nend"));
        });
    });

    describe("Parser — Spread", []() {
        it("accepts a spread in a list literal", []() {
            assertFalse(parseFails("main do\n  let x = [0, ...xs, 5]\nend"));
            assertFalse(parseFails("main do\n  let x = [...xs, ...ys]\nend"));
        });

        it("accepts a spread in a map literal", []() {
            assertFalse(parseFails("main do\n  let x = { ...m, \"k\": 1 }\nend"));
            assertFalse(parseFails("main do\n  let x = { \"k\": 1, ...m }\nend"));
            assertFalse(parseFails("main do\n  let x = { ...m }\nend"));
            assertFalse(parseFails("main do\n  let x = { ...m, ...n }\nend"));
        });

        it("parses a map spread as a map, not a lambda body", []() {
            auto program = parse("main do\n  let x = { ...m }\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& let = std::get<ast::LetExpr>(main->body[0]->kind);
            auto& map = std::get<ast::MapExpr>(let.value->kind);
            assertEqual(map.entries.size(), size_t(1));
            assertTrue(map.entries[0].spread);
            assertTrue(map.entries[0].key == nullptr);
        });

        it("accepts a spread as a do-block statement", []() {
            assertFalse(parseFails(
                "main do\n  let x = div do\n    ...items\n  end\nend"));
        });

        it("rejects a spread outside a collection", []() {
            // `...` used to parse as a general unary expression, so these were
            // accepted and evaluated to None.
            assertTrue(parseFails("main do\n  let y = ...xs\nend"));
            assertTrue(parseFails("main do\n  IO.printLine(...xs)\nend"));
            assertTrue(parseFails("main do\n  let y = 1 + ...xs\nend"));
        });
    });

    describe("Parser — Currying", []() {
        it("parses a bare function capture", []() {
            auto program = parse("main do\n  let x = list.sort(~compare)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            assertEqual(main->body.size(), size_t(1));
        });

        it("parses every binary operator capture", []() {
            for (const auto* op : {"+", "-", "*", "/", "%", "^",
                                   "==", "!=", "<", "<=", ">", ">=",
                                   "&&", "||"}) {
                assertFalse(parseFails(
                    "main do\n  let x = list.reduce(0, ~(" + std::string(op) + "))\nend"));
            }
        });

        it("parses a module-qualified capture", []() {
            auto program = parse("main do\n  let x = list.each(~IO.printLine)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& let = std::get<ast::LetExpr>(main->body[0]->kind);
            auto& each = std::get<ast::MethodCall>(let.value->kind);
            auto& curry = std::get<ast::CurryExpr>(each.args[0]->kind);
            assertEqual(curry.module, std::string("IO"));
            assertEqual(curry.name, std::string("printLine"));
        });

        it("parses a nested-submodule capture", []() {
            auto program = parse(
                "main do\n  let x = list.map(~Outer.Inner.Deep.shout)\nend");
            auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[0]);
            auto& let = std::get<ast::LetExpr>(main->body[0]->kind);
            auto& map = std::get<ast::MethodCall>(let.value->kind);
            auto& curry = std::get<ast::CurryExpr>(map.args[0]->kind);
            assertEqual(curry.module, std::string("Outer.Inner.Deep"));
            assertEqual(curry.name, std::string("shout"));
        });

        it("parses partial application of a qualified name", []() {
            assertFalse(parseFails(
                "main do\n  let x = list.map(~Outer.Inner.add(10))\nend"));
        });

        it("rejects a qualified capture with no function name", []() {
            assertTrue(parseFails("main do\n  let x = list.map(~Outer.Inner)\nend"));
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
                "    Just(x) => x\n"
                "    Nothing => 0\n"
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

        it("parses uppercase atoms in a list", []() {
            assertFalse(parseFails(
                "main do\n"
                "  let modules = [:Math, :List, :String]\n"
                "end\n"));
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

        it("parses intersection types tighter than function and union types", []() {
            auto program = parse("reader : A & B -> C | D");
            auto& annotation =
                std::get<std::unique_ptr<ast::TypeAnnotation>>(program.items[0]);
            auto* unionType =
                std::get_if<ast::UnionType>(&annotation->type->kind);
            assertTrue(unionType != nullptr);
            auto* functionType =
                std::get_if<ast::FunctionType>(&unionType->left->kind);
            assertTrue(functionType != nullptr);
            assertTrue(std::holds_alternative<ast::IntersectionType>(
                functionType->param->kind));
        });

        it("parses an intersection on the right side of a type alias", []() {
            auto program = parse(
                "type ActiveNamed = Named & { active: Bool }");
            auto& definition =
                std::get<std::unique_ptr<ast::TypeDef>>(program.items[0]);
            assertTrue(definition->variants.has_value());
            assertEqual(definition->variants->size(), size_t(1));
            assertTrue(std::holds_alternative<ast::IntersectionType>(
                (*definition->variants)[0]->kind));
        });

        it("disambiguates open record types from map types", []() {
            auto records = parse("labelOf : { label: String, age: Integer } -> String");
            auto& recordAnnotation =
                std::get<std::unique_ptr<ast::TypeAnnotation>>(records.items[0]);
            auto* functionType =
                std::get_if<ast::FunctionType>(&recordAnnotation->type->kind);
            assertTrue(functionType != nullptr);
            auto* recordType =
                std::get_if<ast::RecordType>(&functionType->param->kind);
            assertTrue(recordType != nullptr);
            assertEqual(recordType->fields.size(), size_t(2));

            auto maps = parse("lookup : { String: Integer } -> Integer");
            auto& mapAnnotation =
                std::get<std::unique_ptr<ast::TypeAnnotation>>(maps.items[0]);
            auto* mapFunction =
                std::get_if<ast::FunctionType>(&mapAnnotation->type->kind);
            assertTrue(mapFunction != nullptr);
            assertTrue(std::holds_alternative<ast::MapType>(
                mapFunction->param->kind));
        });

        it("rejects duplicate labels in an open record type", []() {
            assertTrue(parseFails(
                "bad : { name: String, name: Integer } -> String"));
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
