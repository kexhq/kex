#include "test.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include "../src/interpreter/evaluator.hxx"
#include <sstream>

using namespace kex;
using namespace kex::interpreter;
using namespace test;

class ReplSession {
public:
    ReplSession() {
        m_evaluator.setReplMode(true);
    }

    auto eval(const std::string& input) -> std::string {
        // Detect if this is a function def
        bool isFuncDef = false;
        size_t defOffset = std::string::npos;
        if (input.substr(0, 4) == "let ") defOffset = 4;
        else if (input.substr(0, 5) == "foul ") defOffset = 5;
        if (defOffset != std::string::npos) {
            auto parenPos = input.find('(', defOffset);
            auto eqPos = input.find('=', defOffset);
            auto doPos = input.find(" do", defOffset);
            isFuncDef = (parenPos != std::string::npos &&
                         (eqPos == std::string::npos || parenPos < eqPos) &&
                         (doPos == std::string::npos || parenPos < doPos));
        }
        if (input.substr(0, 7) == "module " || input.substr(0, 5) == "type " ||
            input.substr(0, 7) == "record " || input.substr(0, 5) == "make " ||
            input.substr(0, 12) == "foul module ") {
            isFuncDef = true;
        }

        try {
            if (isFuncDef) {
                Lexer lexer(input);
                auto tokens = lexer.tokenizeAll();
                Parser parser(std::move(tokens));
                auto* program = new kex::ast::Program(parser.parseProgram());
                m_programs.push_back(program);
                m_evaluator.execute(*program);
                return "";
            } else {
                auto wrapped = "main do\n" + input + "\nend\n";
                Lexer lexer(wrapped);
                auto tokens = lexer.tokenizeAll();
                Parser parser(std::move(tokens));
                auto* program = new kex::ast::Program(parser.parseProgram());
                m_programs.push_back(program);
                auto result = m_evaluator.execute(*program);
                if (result && !result->isNone()) {
                    return result->toRepr();
                }
                return "";
            }
        } catch (const std::exception& e) {
            return std::string("ERROR: ") + e.what();
        }
    }

    auto output() -> std::string {
        auto out = m_evaluator.output();
        return out;
    }

    ~ReplSession() {
        for (auto* p : m_programs) delete p;
    }

private:
    Evaluator m_evaluator;
    std::vector<kex::ast::Program*> m_programs;
};

int main() {
    describe("REPL — Basic Expressions", []() {
        it("evaluates literals", []() {
            ReplSession repl;
            assertEqual(repl.eval("42"), std::string("42"));
            assertEqual(repl.eval("\"hello\""), std::string("\"hello\""));
            assertEqual(repl.eval("true"), std::string("true"));
            assertEqual(repl.eval(":ok"), std::string(":ok"));
            // Regression: the actual CLI's REPL value printer (colorValue
            // in main.cxx, not exercised by this ReplSession harness) had
            // no CharValue case and printed every Char as a bare "?" —
            // this only catches Value::toRepr() missing a case the same
            // way; see spec/char_type.kex for the rest of Char's behavior.
            assertEqual(repl.eval("'a'"), std::string("'a'"));
        });

        it("evaluates arithmetic", []() {
            ReplSession repl;
            assertEqual(repl.eval("1 + 2"), std::string("3"));
            assertEqual(repl.eval("10 * 5"), std::string("50"));
            assertEqual(repl.eval("\"hello\" + \" world\""), std::string("\"hello world\""));
        });

        it("persists let bindings", []() {
            ReplSession repl;
            repl.eval("let x = 5");
            assertEqual(repl.eval("x + 3"), std::string("8"));
        });

        it("persists var bindings and assignment", []() {
            ReplSession repl;
            repl.eval("var count = 0");
            repl.eval("count = count + 1");
            repl.eval("count = count + 1");
            assertEqual(repl.eval("count"), std::string("2"));
        });
    });

    describe("REPL — Function Definitions", []() {
        it("defines and calls functions", []() {
            ReplSession repl;
            repl.eval("let double(n: Int) = n * 2");
            assertEqual(repl.eval("double(21)"), std::string("42"));
        });

        it("defines recursive functions", []() {
            ReplSession repl;
            repl.eval("let factorial(0) = 1");
            repl.eval("let factorial(n: Int) = n * factorial(n - 1)");
            assertEqual(repl.eval("factorial(5)"), std::string("120"));
        });

        it("defines multi-line functions", []() {
            ReplSession repl;
            repl.eval("let greet(name: String) do\n  \"Hello, \" + name + \"!\"\nend");
            assertEqual(repl.eval("greet(\"Kex\")"), std::string("\"Hello, Kex!\""));
        });
    });

    describe("REPL — Collections", []() {
        it("creates and uses lists", []() {
            ReplSession repl;
            repl.eval("let nums = [1, 2, 3, 4, 5]");
            assertEqual(repl.eval("nums.count"), std::string("5"));
            assertEqual(repl.eval("nums.sum"), std::string("15"));
            assertEqual(repl.eval("nums.first"), std::string("Just(1)"));
            assertEqual(repl.eval("nums.last"), std::string("Just(5)"));
        });

        it("maps and filters", []() {
            ReplSession repl;
            assertEqual(repl.eval("[1, 2, 3].map { |x| x * 2 }"), std::string("[2, 4, 6]"));
            assertEqual(repl.eval("[1, 2, 3, 4].filter { |x| x > 2 }"), std::string("[3, 4]"));
        });

        it("uses ranges", []() {
            ReplSession repl;
            assertEqual(repl.eval("(1..5).sum"), std::string("15"));
            assertEqual(repl.eval("(1..5).to(List)"), std::string("Just([1, 2, 3, 4, 5])"));
        });
    });

    describe("REPL — Streams", []() {
        it("creates and takes from streams", []() {
            ReplSession repl;
            repl.eval("let naturals = Sequence(from: 0) { |n| n + 1 }");
            assertEqual(repl.eval("naturals.take(5)"), std::string("[0, 1, 2, 3, 4]"));
        });

        it("maps streams lazily", []() {
            ReplSession repl;
            repl.eval("let squares = Sequence(from: 1) { |n| n + 1 }\n  .map { |n| n * n }");
            assertEqual(repl.eval("squares.take(4)"), std::string("[1, 4, 9, 16]"));
        });

        it("filters streams lazily", []() {
            ReplSession repl;
            repl.eval("let even?(n: Int) = n.modulo(2) == 0");
            repl.eval("let evens = Sequence(from: 1) { |n| n + 1 }\n  .filter(&even?)");
            assertEqual(repl.eval("evens.take(5)"), std::string("[2, 4, 6, 8, 10]"));
        });
    });

    describe("REPL — Records", []() {
        it("defines and uses records", []() {
            ReplSession repl;
            repl.eval("record User do\n  name : String\n  age : Int\nend");
            repl.eval("let user = User { name: \"Alice\", age: 32 }");
            assertEqual(repl.eval("user.name"), std::string("\"Alice\""));
            assertEqual(repl.eval("user.age"), std::string("32"));
        });
    });

    describe("REPL — Pattern Matching", []() {
        it("matches in expressions", []() {
            ReplSession repl;
            auto result = repl.eval(
                "match 42 do\n"
                "  0 -> \"zero\"\n"
                "  42 -> \"the answer\"\n"
                "  _ -> \"other\"\n"
                "end"
            );
            assertEqual(result, std::string("\"the answer\""));
        });
    });

    describe("REPL — String Operations", []() {
        it("trim, upperCase, lowerCase", []() {
            ReplSession repl;
            assertEqual(repl.eval("\"  hi  \".trim"), std::string("\"hi\""));
            assertEqual(repl.eval("\"hello\".upperCase"), std::string("\"HELLO\""));
            assertEqual(repl.eval("\"HELLO\".lowerCase"), std::string("\"hello\""));
        });

        it("split and join", []() {
            ReplSession repl;
            assertEqual(repl.eval("\"a,b,c\".split(\",\").join(\" \")"), std::string("\"a b c\""));
        });

        it("contains?", []() {
            ReplSession repl;
            assertEqual(repl.eval("\"hello world\".contains?(\"world\")"), std::string("true"));
            assertEqual(repl.eval("\"hello world\".contains?(\"xyz\")"), std::string("false"));
        });

        it("reverse", []() {
            ReplSession repl;
            assertEqual(repl.eval("\"hello\".reverse"), std::string("\"olleh\""));
        });
    });

    return runAll();
}
