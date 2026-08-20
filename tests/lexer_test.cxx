#include "test.hxx"
#include "../src/lexer/lexer.hxx"

using namespace kex;
using namespace test;

auto tokenize(const std::string& source) -> std::vector<Token> {
    Lexer lexer(source);
    return lexer.tokenizeAll();
}

auto tokenTypes(const std::string& source) -> std::vector<TokenType> {
    auto tokens = tokenize(source);
    std::vector<TokenType> types;
    for (const auto& t : tokens) {
        if (t.type != TokenType::Newline && t.type != TokenType::Eof) {
            types.push_back(t.type);
        }
    }
    return types;
}

auto firstToken(const std::string& source) -> Token {
    Lexer lexer(source);
    return lexer.nextToken();
}

int main() {
    describe("Lexer — Keywords", []() {
        it("tokenizes all keywords", []() {
            assertEqual(firstToken("let").type, TokenType::Let);
            assertEqual(firstToken("do").type, TokenType::Do);
            assertEqual(firstToken("end").type, TokenType::End);
            assertEqual(firstToken("if").type, TokenType::If);
            assertEqual(firstToken("else").type, TokenType::Else);
            assertEqual(firstToken("elif").type, TokenType::Elif);
            assertEqual(firstToken("match").type, TokenType::Match);
            assertEqual(firstToken("module").type, TokenType::Module);
            assertEqual(firstToken("type").type, TokenType::Type);
            assertEqual(firstToken("record").type, TokenType::Record);
            assertEqual(firstToken("make").type, TokenType::Make);
            assertEqual(firstToken("foul").type, TokenType::Foul);
            assertEqual(firstToken("return").type, TokenType::Return);
            assertEqual(firstToken("spawn").type, TokenType::Spawn);
            assertEqual(firstToken("receive").type, TokenType::Receive);
            assertEqual(firstToken("loop").type, TokenType::Loop);
            assertEqual(firstToken("var").type, TokenType::Var);
            assertEqual(firstToken("using").type, TokenType::Using);
            assertEqual(firstToken("true").type, TokenType::True);
            assertEqual(firstToken("false").type, TokenType::False);
            assertEqual(firstToken("this").type, TokenType::This);
            assertEqual(firstToken("main").type, TokenType::Main);
            assertEqual(firstToken("compiled").type, TokenType::Compiled);
            assertEqual(firstToken("private").type, TokenType::Private);
            assertEqual(firstToken("public").type, TokenType::Public);
        });

        it("does not tokenize keyword prefixes as keywords", []() {
            assertEqual(firstToken("letter").type, TokenType::LowerIdent);
            assertEqual(firstToken("doit").type, TokenType::LowerIdent);
            assertEqual(firstToken("iffy").type, TokenType::LowerIdent);
            assertEqual(firstToken("match_something").type, TokenType::LowerIdent);
        });
    });

    describe("Lexer — Identifiers", []() {
        it("tokenizes lowercase identifiers", []() {
            auto tok = firstToken("hello");
            assertEqual(tok.type, TokenType::LowerIdent);
            assertEqual(tok.value, std::string("hello"));
        });

        it("tokenizes camelCase identifiers", []() {
            auto tok = firstToken("myVariable");
            assertEqual(tok.type, TokenType::LowerIdent);
            assertEqual(tok.value, std::string("myVariable"));
        });

        it("tokenizes predicate identifiers with ?", []() {
            auto tok = firstToken("empty?");
            assertEqual(tok.type, TokenType::LowerIdent);
            assertEqual(tok.value, std::string("empty?"));
        });

        it("tokenizes snake_case identifiers", []() {
            auto tok = firstToken("my_var");
            assertEqual(tok.type, TokenType::LowerIdent);
            assertEqual(tok.value, std::string("my_var"));
        });

        it("tokenizes uppercase identifiers", []() {
            auto tok = firstToken("String");
            assertEqual(tok.type, TokenType::UpperIdent);
            assertEqual(tok.value, std::string("String"));
        });

        it("tokenizes None as its own token", []() {
            assertEqual(firstToken("None").type, TokenType::None);
        });

        it("tokenizes underscore as wildcard", []() {
            assertEqual(firstToken("_").type, TokenType::Underscore);
        });

        it("tokenizes a lowercase letter from another script", []() {
            // Greek is the everyday case — `α` reads as a variable, the same
            // as `a`, and stays one token with its ASCII neighbours.
            auto tok = firstToken("\u03b1");
            assertEqual(tok.type, TokenType::LowerIdent);
            assertEqual(tok.value, std::string("\u03b1"));

            auto mixed = firstToken("\u03b1Rate2");
            assertEqual(mixed.type, TokenType::LowerIdent);
            assertEqual(mixed.value, std::string("\u03b1Rate2"));
        });

        it("tokenizes an uppercase letter from another script as a type", []() {
            // Case picks the kind for every script that HAS case: capital
            // alpha names a type, exactly as `String` does.
            auto tok = firstToken("\u0391");
            assertEqual(tok.type, TokenType::UpperIdent);
            assertEqual(tok.value, std::string("\u0391"));
        });

        it("treats a caseless script as a value name", []() {
            // Han has no uppercase form, and types are the rarer
            // declaration, so a caseless name reads as a value.
            auto tok = firstToken("\u5408\u8a08");
            assertEqual(tok.type, TokenType::LowerIdent);
            assertEqual(tok.value, std::string("\u5408\u8a08"));
        });

        it("does not swallow a non-letter symbol into an identifier", []() {
            // `→` is a symbol, not a letter: it ends the name rather than
            // becoming part of it.
            auto tokens = kex::Lexer("a\u2192b").tokenizeAll();
            assertEqual(tokens[0].type, TokenType::LowerIdent);
            assertEqual(tokens[0].value, std::string("a"));
            assertEqual(tokens[1].type, TokenType::Error);
        });

        it("tokenizes splice identifiers", []() {
            auto tok = firstToken("%name");
            assertEqual(tok.type, TokenType::SpliceIdent);
            assertEqual(tok.value, std::string("name"));
        });
    });

    describe("Lexer — Numbers", []() {
        it("tokenizes integers", []() {
            auto tok = firstToken("42");
            assertEqual(tok.type, TokenType::Integer);
            assertEqual(tok.value, std::string("42"));
        });

        it("tokenizes integers with underscores", []() {
            auto tok = firstToken("1_000_000");
            assertEqual(tok.type, TokenType::Integer);
            assertEqual(tok.value, std::string("1000000"));
        });

        it("tokenizes floats", []() {
            auto tok = firstToken("3.14");
            assertEqual(tok.type, TokenType::Float);
            assertEqual(tok.value, std::string("3.14"));
        });

        it("does not confuse range with float", []() {
            auto types = tokenTypes("1..10");
            assertEqual(types.size(), size_t(3));
            assertEqual(types[0], TokenType::Integer);
            assertEqual(types[1], TokenType::DotDot);
            assertEqual(types[2], TokenType::Integer);
        });

        // Radix literals carry their decimal spelling, so everything
        // downstream (the evaluator, literal patterns, Core Erlang emission)
        // only ever sees base 10.
        it("tokenizes hexadecimal literals", []() {
            assertEqual(firstToken("0xff").value, std::string("255"));
            assertEqual(firstToken("0xff").type, TokenType::Integer);
            assertEqual(firstToken("0xAAF23").value, std::string("700195"));
            assertEqual(firstToken("0XFF").value, std::string("255"));
            assertEqual(firstToken("0xDEAD_BEEF").value, std::string("3735928559"));
        });

        it("tokenizes binary and octal literals", []() {
            assertEqual(firstToken("0b1011").value, std::string("11"));
            assertEqual(firstToken("0B1111_0000").value, std::string("240"));
            assertEqual(firstToken("0o755").value, std::string("493"));
            assertEqual(firstToken("0O10").value, std::string("8"));
        });

        it("lexes radix literals beyond int64", []() {
            assertEqual(firstToken("0xFFFFFFFFFFFFFFFFFF").value,
                        std::string("4722366482869645213695"));
        });

        it("rejects radix literals with no digits or bad digits", []() {
            assertEqual(firstToken("0x").type, TokenType::Error);
            assertEqual(firstToken("0b12").type, TokenType::Error);
            assertEqual(firstToken("0o88").type, TokenType::Error);
        });

        // Normalized to a form both std::stod and Erlang accept: a fraction
        // part is required before the exponent, and `E` is lowercased.
        it("tokenizes float exponents", []() {
            assertEqual(firstToken("3e22").type, TokenType::Float);
            assertEqual(firstToken("3e22").value, std::string("3.0e22"));
            assertEqual(firstToken("345e-22").value, std::string("345.0e-22"));
            assertEqual(firstToken("1.5E3").value, std::string("1.5e3"));
            assertEqual(firstToken("2.5e+2").value, std::string("2.5e+2"));
            assertEqual(firstToken("1_0e1_0").value, std::string("10.0e10"));
        });

        it("only starts an exponent when a well-formed one follows", []() {
            // `2e` is an integer followed by an identifier, not a float.
            auto types = tokenTypes("2e");
            assertEqual(types.size(), size_t(2));
            assertEqual(types[0], TokenType::Integer);
            assertEqual(types[1], TokenType::LowerIdent);

            auto each = tokenTypes("2.each");
            assertEqual(each[0], TokenType::Integer);
            assertEqual(each[1], TokenType::Dot);
        });
    });

    describe("Lexer — Strings", []() {
        it("tokenizes simple strings", []() {
            auto tok = firstToken("\"hello\"");
            assertEqual(tok.type, TokenType::String);
            assertEqual(tok.value, std::string("hello"));
        });

        it("handles escape sequences", []() {
            auto tok = firstToken("\"line\\nbreak\"");
            assertEqual(tok.type, TokenType::String);
            assertEqual(tok.value, std::string("line\nbreak"));
        });

        it("handles string interpolation", []() {
            auto tok = firstToken("\"hello ${name}!\"");
            assertEqual(tok.type, TokenType::String);
            assertEqual(tok.value, std::string("hello ${name}!"));
        });

        it("handles escaped dollar in strings", []() {
            auto tok = firstToken("\"cost: \\$5\"");
            assertEqual(tok.type, TokenType::String);
            assertEqual(tok.value, std::string("cost: $5"));
        });

        it("tokenizes raw backtick strings without backslash escapes", []() {
            auto tok = firstToken(R"kex(`C:\Users\akos\regex\d+`)kex");
            assertEqual(tok.type, TokenType::RawString);
            assertEqual(tok.value, std::string(R"kex(C:\Users\akos\regex\d+)kex"));
        });

        it("keeps interpolation syntax literal in raw strings", []() {
            auto tok = firstToken("`${name}`");
            assertEqual(tok.type, TokenType::RawString);
            assertEqual(tok.value, std::string("${name}"));
        });

        it("uses doubled backticks for a literal backtick", []() {
            auto tok = firstToken("`tick `` here`");
            assertEqual(tok.type, TokenType::RawString);
            assertEqual(tok.value, std::string("tick ` here"));
        });

        it("allows raw strings to end in a backslash", []() {
            auto tok = firstToken("`ends-with-\\`");
            assertEqual(tok.type, TokenType::RawString);
            assertEqual(tok.value, std::string("ends-with-\\"));
        });

        it("dedents multiline raw strings by the closing prefix", []() {
            auto tok = firstToken(
                "`\n"
                "    first\n"
                "      second\n"
                "    `");
            assertEqual(tok.type, TokenType::RawString);
            assertEqual(tok.value, std::string("first\n  second\n"));
        });

        it("preserves indentation when the closing backtick is flush left", []() {
            auto tok = firstToken(
                "`\n"
                "  first\n"
                "`");
            assertEqual(tok.type, TokenType::RawString);
            assertEqual(tok.value, std::string("  first\n"));
        });

        it("rejects content left of the closing margin", []() {
            auto tok = firstToken(
                "`\n"
                "    first\n"
                "  second\n"
                "    `");
            assertEqual(tok.type, TokenType::Error);
        });

        it("records token byte spans", []() {
            auto tokens = tokenize("let value = `raw`");
            auto& raw = tokens[3];
            assertEqual(raw.type, TokenType::RawString);
            assertEqual(raw.startOffset, 12);
            assertEqual(raw.endOffset, 17);
            assertEqual(raw.location.startOffset, 12);
            assertEqual(raw.location.endOffset, 17);
        });

        it("tokenizes interpolating backticks", []() {
            auto tok = firstToken("$`hello ${name}`");
            assertEqual(tok.type, TokenType::InterpolatedRawString);
            assertEqual(tok.value, std::string("hello ${name}"));
        });

        it("leaves doubled-dollar interpolation escapes for the parser", []() {
            auto tok = firstToken("$`literal $${name}`");
            assertEqual(tok.type, TokenType::InterpolatedRawString);
            assertEqual(tok.value, std::string("literal $${name}"));
        });

        it("keeps nested backtick literals inside interpolation holes", []() {
            auto tok = firstToken("$`value: ${`raw`}`");
            assertEqual(tok.type, TokenType::InterpolatedRawString);
            assertEqual(tok.value, std::string("value: ${`raw`}"));
        });

        it("reports an unterminated raw backtick string", []() {
            auto tok = firstToken("`never closed");
            assertEqual(tok.type, TokenType::Error);
            assertTrue(tok.value.find("Unterminated backtick") !=
                       std::string::npos);
        });
    });

    describe("Lexer — Atoms", []() {
        it("tokenizes atoms", []() {
            auto tok = firstToken(":ok");
            assertEqual(tok.type, TokenType::Atom);
            assertEqual(tok.value, std::string("ok"));
        });

        it("tokenizes atoms with underscores", []() {
            auto tok = firstToken(":one_for_one");
            assertEqual(tok.type, TokenType::Atom);
            assertEqual(tok.value, std::string("one_for_one"));
        });
    });

    describe("Lexer — Operators", []() {
        it("tokenizes single-char operators", []() {
            assertEqual(firstToken("+").type, TokenType::Plus);
            assertEqual(firstToken("-").type, TokenType::Minus);
            assertEqual(firstToken("*").type, TokenType::Star);
            assertEqual(firstToken("/").type, TokenType::Slash);
            assertEqual(firstToken("%").type, TokenType::Percent);
            assertEqual(firstToken("^").type, TokenType::Caret);
            assertEqual(firstToken("@").type, TokenType::At);
            assertEqual(firstToken("&").type, TokenType::Amp);
            assertEqual(firstToken("?").type, TokenType::Question);
            assertEqual(firstToken("!").type, TokenType::Bang);
        });

        it("tokenizes multi-char operators", []() {
            assertEqual(firstToken("==").type, TokenType::EqEq);
            assertEqual(firstToken("!=").type, TokenType::NotEq);
            assertEqual(firstToken("<=").type, TokenType::LessEq);
            assertEqual(firstToken(">=").type, TokenType::GreaterEq);
            assertEqual(firstToken("&&").type, TokenType::AmpAmp);
            assertEqual(firstToken("||").type, TokenType::PipePipe);
            assertEqual(firstToken("->").type, TokenType::Arrow);
            // `=>` (arms) must not lex as `=` then `>`, and must not disturb
            // `==` — the two share a first character.
            assertEqual(firstToken("=>").type, TokenType::FatArrow);
            assertEqual(firstToken("==").type, TokenType::EqEq);
            assertEqual(firstToken("=").type, TokenType::Equals);
            assertEqual(firstToken(":>").type, TokenType::TypeAnnotation);
            assertEqual(firstToken("..").type, TokenType::DotDot);
            assertEqual(firstToken("...").type, TokenType::DotDotDot);
        });

        it("tokenizes brackets and delimiters", []() {
            assertEqual(firstToken("(").type, TokenType::LParen);
            assertEqual(firstToken(")").type, TokenType::RParen);
            assertEqual(firstToken("[").type, TokenType::LBracket);
            assertEqual(firstToken("]").type, TokenType::RBracket);
            assertEqual(firstToken("{").type, TokenType::LBrace);
            assertEqual(firstToken("}").type, TokenType::RBrace);
            assertEqual(firstToken(",").type, TokenType::Comma);
            assertEqual(firstToken(".").type, TokenType::Dot);
            assertEqual(firstToken("|").type, TokenType::Pipe);
        });

        it("tokenizes pragma start", []() {
            assertEqual(firstToken("#[").type, TokenType::HashLBracket);
        });
    });

    describe("Lexer — Comments", []() {
        it("skips single-line comments", []() {
            auto types = tokenTypes("x # this is a comment\ny");
            assertEqual(types.size(), size_t(2));
            assertEqual(types[0], TokenType::LowerIdent);
            assertEqual(types[1], TokenType::LowerIdent);
        });

        it("distinguishes comment from pragma", []() {
            auto tok = firstToken("#[");
            assertEqual(tok.type, TokenType::HashLBracket);
        });
    });

    describe("Lexer — Complex Expressions", []() {
        it("tokenizes UFCS call chain", []() {
            auto types = tokenTypes("list.map(&.name).filter(&.empty?)");
            assertTrue(types.size() > 5);
            assertEqual(types[0], TokenType::LowerIdent); // list
            assertEqual(types[1], TokenType::Dot);
            assertEqual(types[2], TokenType::LowerIdent); // map
        });

        it("tokenizes function definition", []() {
            auto types = tokenTypes("let factorial(n: Int) -> Int do");
            assertEqual(types[0], TokenType::Let);
            assertEqual(types[1], TokenType::LowerIdent); // factorial
            assertEqual(types[2], TokenType::LParen);
        });

        it("tokenizes type annotation with :>", []() {
            auto types = tokenTypes("modulo :> This -> This");
            assertEqual(types[0], TokenType::LowerIdent);
            assertEqual(types[1], TokenType::TypeAnnotation);
        });

        it("tokenizes make block header", []() {
            auto types = tokenTypes("make final: Integer do");
            assertEqual(types[0], TokenType::Make);
            assertEqual(types[1], TokenType::Final);
            assertEqual(types[2], TokenType::Colon);
            assertEqual(types[3], TokenType::UpperIdent);
            assertEqual(types[4], TokenType::Do);
        });
    });

    describe("Lexer — Source Locations", []() {
        it("tracks line and column", []() {
            auto tokens = tokenize("let x = 5\nlet y = 10");
            // First 'let' at line 1
            assertEqual(tokens[0].location.line, 1);
            assertEqual(tokens[0].location.column, 1);
            // Second 'let' at line 2
            // Find it after the newline
            int letCount = 0;
            for (const auto& t : tokens) {
                if (t.type == TokenType::Let) {
                    letCount++;
                    if (letCount == 2) {
                        assertEqual(t.location.line, 2);
                        assertEqual(t.location.column, 1);
                        break;
                    }
                }
            }
        });
    });

    describe("Lexer — Error Handling", []() {
        it("reports unterminated string", []() {
            auto tok = firstToken("\"unterminated");
            assertEqual(tok.type, TokenType::Error);
        });
    });

    return runAll();
}
