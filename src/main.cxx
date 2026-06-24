#include "lexer/lexer.hxx"
#include "parser/parser.hxx"
#include "semantic/analyzer.hxx"
#include "interpreter/evaluator.hxx"
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef HAS_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

auto readLine(const std::string& prompt) -> std::pair<std::string, bool> {
#ifdef HAS_READLINE
    char* input = readline(prompt.c_str());
    if (!input) return {"", false};
    std::string line(input);
    if (!line.empty()) add_history(input);
    free(input);
    return {line, true};
#else
    std::cout << prompt;
    std::string line;
    if (!std::getline(std::cin, line)) return {"", false};
    return {line, true};
#endif
}

auto readFile(const std::string& path) -> std::string {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Error: could not open file: " << path << "\n";
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Convention: `<name>.spec.kex` is a spec for `<name>.kex` and doesn't need
// to redeclare its types/records/functions — running the spec auto-loads
// the base file's declarations (skipping its own `main` block(s), so its
// demo output/side effects don't run) into the same scope first. Looked up
// next to the spec file, and — since this project keeps specs in spec/
// alongside examples in a sibling examples/ — also under examples/ if the
// spec's directory is named "spec".
auto specBaseCandidates(const std::string& filepath) -> std::vector<std::string> {
    static const std::string suffix = ".spec.kex";
    if (filepath.size() <= suffix.size()) return {};
    if (filepath.compare(filepath.size() - suffix.size(), suffix.size(), suffix) != 0) return {};

    std::string stem = filepath.substr(0, filepath.size() - suffix.size());
    auto slash = stem.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : stem.substr(0, slash);
    std::string name = slash == std::string::npos ? stem : stem.substr(slash + 1);

    std::vector<std::string> candidates = {stem + ".kex"};
    const std::string specSuffix = "spec";
    if (dir.size() >= specSuffix.size() &&
        dir.compare(dir.size() - specSuffix.size(), specSuffix.size(), specSuffix) == 0) {
        std::string parent = dir.substr(0, dir.size() - specSuffix.size());
        candidates.push_back(parent + "examples/" + name + ".kex");
    }
    return candidates;
}

auto fileExists(const std::string& path) -> bool {
    std::ifstream probe(path);
    return probe.good();
}

auto printAst(const kex::ast::Program& program) -> void {
    std::cout << "Program (" << program.items.size() << " items)\n";
    for (const auto& item : program.items) {
        std::visit([](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::ModuleDef>>) {
                std::cout << "  Module: " << node->name
                          << (node->isFoul ? " [foul]" : "") << "\n";
            } else if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::TypeDef>>) {
                std::cout << "  Type: " << node->name << "\n";
            } else if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::RecordDef>>) {
                std::cout << "  Record: " << node->name
                          << " (" << node->fields.size() << " fields)\n";
            } else if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::MakeDef>>) {
                std::cout << "  Make" << (node->isFinal ? " [final]" : "") << "\n";
            } else if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::FunctionDef>>) {
                std::cout << "  Function: " << node->name
                          << (node->isFoul ? " [foul]" : "")
                          << (node->isPredicate ? " [?]" : "") << "\n";
            } else if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::CompiledBlock>>) {
                std::cout << "  Compiled block\n";
            } else if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::UsingBlock>>) {
                std::cout << "  Using block\n";
            } else if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::MainBlock>>) {
                std::cout << "  Main (" << node->body.size() << " expressions)\n";
            } else if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::Pragma>>) {
                std::cout << "  Pragma: ";
                for (const auto& r : node->requirements) std::cout << r << " ";
                std::cout << "\n";
            }
        }, item);
    }
}

namespace color {
    constexpr auto reset   = "\033[0m";
    constexpr auto dim     = "\033[90m";
    constexpr auto yellow  = "\033[33m";
    constexpr auto green   = "\033[32m";
    constexpr auto magenta = "\033[35m";
    constexpr auto purple  = "\033[95m";
    constexpr auto cyan    = "\033[36m";
}

auto colorValue(const kex::interpreter::ValuePtr& val) -> std::string {
    using namespace kex::interpreter;
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, IntValue>)
            return std::string(color::yellow) + std::to_string(v.value) + color::reset;
        else if constexpr (std::is_same_v<T, FloatValue>) {
            auto s = std::to_string(v.value);
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                auto last = s.find_last_not_of('0');
                if (last == dot) last++;
                s = s.substr(0, last + 1);
            }
            return std::string(color::yellow) + s + color::reset;
        }
        else if constexpr (std::is_same_v<T, StringValue>)
            return std::string(color::green) + "\"" + v.value + "\"" + color::reset;
        else if constexpr (std::is_same_v<T, CharValue>)
            return std::string(color::green) + "'" + std::string(1, v.value) + "'" + color::reset;
        else if constexpr (std::is_same_v<T, BoolValue>)
            return std::string(color::magenta) + (v.value ? "true" : "false") + color::reset;
        else if constexpr (std::is_same_v<T, NoneValue>)
            return std::string(color::dim) + "None" + color::reset;
        else if constexpr (std::is_same_v<T, AtomValue>)
            return std::string(color::purple) + ":" + v.name + color::reset;
        else if constexpr (std::is_same_v<T, ListValue>) {
            std::string result = "[";
            for (size_t i = 0; i < v.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += colorValue(v.elements[i]);
            }
            return result + "]";
        }
        else if constexpr (std::is_same_v<T, TupleValue>) {
            std::string result = "(";
            for (size_t i = 0; i < v.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += colorValue(v.elements[i]);
            }
            return result + ")";
        }
        else if constexpr (std::is_same_v<T, MapValue>) {
            std::string result = "{ ";
            for (size_t i = 0; i < v.entries.size(); i++) {
                if (i > 0) result += ", ";
                result += colorValue(v.entries[i].first) + ": " + colorValue(v.entries[i].second);
            }
            return result + " }";
        }
        else if constexpr (std::is_same_v<T, RangeValue>)
            return std::string(color::yellow) + std::to_string(v.start) + color::reset
                   + ".." + std::string(color::yellow) + std::to_string(v.end) + color::reset;
        else if constexpr (std::is_same_v<T, StreamValue>)
            return std::string(color::cyan) + "<Stream>" + color::reset;
        else if constexpr (std::is_same_v<T, RecordValue>) {
            // Positional constructor (Just(x), Ok(x), Number(n), ...): fields
            // keyed "0", "1", ... — print as Name(v0, v1, ...), matching
            // Value::toRepr()'s convention, instead of the raw field dump.
            bool positional = !v.fields.empty();
            for (size_t i = 0; positional && i < v.fields.size(); i++) {
                if (v.fields.find(std::to_string(i)) == v.fields.end()) positional = false;
            }
            if (positional) {
                std::string result = std::string(color::cyan) + v.typeName + color::reset + "(";
                for (size_t i = 0; i < v.fields.size(); i++) {
                    if (i > 0) result += ", ";
                    result += colorValue(v.fields.at(std::to_string(i)));
                }
                return result + ")";
            }
            std::string result = std::string(color::cyan) + v.typeName + color::reset + " { ";
            bool first = true;
            for (const auto& [key, val] : v.fields) {
                if (!first) result += ", ";
                result += key + ": " + colorValue(val);
                first = false;
            }
            return result + " }";
        }
        else if constexpr (std::is_same_v<T, FunctionValue>)
            return std::string(color::dim) + "<function:" + v.name + ">" + color::reset;
        else if constexpr (std::is_same_v<T, LambdaValue>)
            return std::string(color::dim) + "<lambda>" + color::reset;
        else
            return "?";
    }, val->data);
}

auto printUsage(const char* progName) -> void {
    std::cerr << "Usage: " << progName << " [options] <file.kex>\n"
              << "\n"
              << "Options:\n"
              << "  -r, --run       Execute the program\n"
              << "  -l, --lex       Print token stream\n"
              << "  -p, --parse     Print AST\n"
              << "  -c, --check     Run semantic analysis\n"
              << "  -h, --help      Show this help\n"
              << "  -v, --version   Show version\n";
}

auto printVersion() -> void {
    std::cout << "kex 0.1.0\n";
}

int main(int argc, char* argv[]) {
    static struct option longOptions[] = {
        {"run",     no_argument, nullptr, 'r'},
        {"lex",     no_argument, nullptr, 'l'},
        {"parse",   no_argument, nullptr, 'p'},
        {"check",   no_argument, nullptr, 'c'},
        {"help",    no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, 'v'},
        {nullptr,   0,           nullptr,  0 }
    };

    std::string mode = "run";
    int opt;

    while ((opt = getopt_long(argc, argv, "rlcphv", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 'r': mode = "run"; break;
            case 'l': mode = "lex"; break;
            case 'p': mode = "parse"; break;
            case 'c': mode = "check"; break;
            case 'h': printUsage(argv[0]); return 0;
            case 'v': printVersion(); return 0;
            default: printUsage(argv[0]); return 1;
        }
    }

    if (optind >= argc && mode != "repl") {
        // No file — enter REPL mode
        mode = "repl";
    }

    if (mode == "repl") {
        std::cout << "kex 0.1.0 — interactive REPL\n";
        std::cout << "Type :help for available commands, exit to quit.\n\n";
        kex::interpreter::Evaluator evaluator;
        evaluator.setReplMode(true);
        std::string line;
        // Keep parsed programs alive so function closures can reference AST nodes
        std::vector<kex::ast::Program*> replPrograms;
        // A line read ahead while chaining function clauses that turned out
        // to belong to the next statement; replayed on the next iteration.
        std::optional<std::string> pendingLine;

        // If `s` looks like the start of a function clause definition
        // (`let name(...` or `foul name(...`), return the function name.
        auto clauseFuncName = [](const std::string& s) -> std::optional<std::string> {
            size_t offset;
            if (s.rfind("foul ", 0) == 0) offset = 5;
            else if (s.rfind("let ", 0) == 0) offset = 4;
            else return std::nullopt;

            size_t i = offset;
            while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
            if (i == offset) return std::nullopt;
            std::string name = s.substr(offset, i - offset);
            if (i < s.size() && s[i] == '?') { name += '?'; i++; }
            if (i < s.size() && s[i] == '(') return name;
            return std::nullopt;
        };

        // REPL settings
        bool showTypes = true;
        bool showAst = false;
        bool showTokens = false;

        auto printReplHelp = [&]() {
            std::cout << "\n  Commands:\n"
                      << "    exit          Exit the REPL\n"
                      << "    :help         Show this help\n"
                      << "    :set <opt>    Enable a feature\n"
                      << "    :unset <opt>  Disable a feature\n"
                      << "\n"
                      << "  Options for :set / :unset:\n"
                      << "    types         Show type of each result\n"
                      << "    ast           Show AST for each input\n"
                      << "    tokens        Show token stream for each input\n"
                      << "\n";
        };

        auto handleSet = [&](const std::string& arg, bool enable) {
            if (arg == "types") {
                showTypes = enable;
                std::cout << "  types: " << (enable ? "on" : "off") << "\n";
            } else if (arg == "ast") {
                showAst = enable;
                std::cout << "  ast: " << (enable ? "on" : "off") << "\n";
            } else if (arg == "tokens") {
                showTokens = enable;
                std::cout << "  tokens: " << (enable ? "on" : "off") << "\n";
            } else {
                std::cerr << "  Unknown option: " << arg << "\n";
                std::cerr << "  Available: types, ast, tokens\n";
            }
        };

        while (true) {
            std::string input;
            bool ok;
            if (pendingLine) {
                input = *pendingLine;
                pendingLine.reset();
                ok = true;
            } else {
                auto result = readLine("kex> ");
                input = result.first;
                ok = result.second;
            }
            if (!ok) break;
            line = input;
            if (line == "exit" || line == ":exit" || line == ":quit" || line == ":q") break;
            if (line.empty()) continue;

            // REPL commands
            if (line == ":help" || line == ":h") {
                printReplHelp();
                continue;
            }
            if (line == ":set") {
                std::cout << "  types:  " << (showTypes ? "on" : "off") << "\n"
                          << "  ast:    " << (showAst ? "on" : "off") << "\n"
                          << "  tokens: " << (showTokens ? "on" : "off") << "\n";
                continue;
            }
            if (line.substr(0, 5) == ":set ") {
                handleSet(line.substr(5), true);
                continue;
            }
            if (line.substr(0, 7) == ":unset ") {
                handleSet(line.substr(7), false);
                continue;
            }
            // If it starts with : but isn't a known command, treat as atom expression
            // (known commands already handled above)

            // Multi-line: accumulate if there are unmatched do/end blocks
            std::string source = line;

            auto countBlocks = [](const std::string& s) -> int {
                int count = 0;
                for (size_t i = 0; i < s.size(); i++) {
                    if (i + 2 <= s.size() && s.substr(i, 2) == "do") {
                        bool wordBefore = (i > 0 && std::isalnum(s[i - 1]));
                        bool wordAfter = (i + 2 < s.size() && std::isalnum(s[i + 2]));
                        if (!wordBefore && !wordAfter) count++;
                    }
                    if (i + 3 <= s.size() && s.substr(i, 3) == "end") {
                        bool wordBefore = (i > 0 && std::isalnum(s[i - 1]));
                        bool wordAfter = (i + 3 < s.size() && std::isalnum(s[i + 3]));
                        if (!wordBefore && !wordAfter) count--;
                    }
                }
                return count;
            };

            int doCount = countBlocks(source);
            while (doCount > 0) {
                auto [contLine, contOk] = readLine("...> ");
                if (!contOk) break;
                line = contLine;
                source += "\n" + line;
                doCount += countBlocks(line);
            }

            // If this line starts a function clause definition, keep reading
            // additional clauses for the *same* function so pattern-matching
            // definitions like `let fact(1) = 1` / `let fact(n) = ...` are
            // combined into one function. The first line that isn't another
            // clause of the same function is replayed on the next iteration.
            if (doCount == 0) {
                if (auto name = clauseFuncName(source)) {
                    while (true) {
                        auto [contLine, contOk] = readLine("...> ");
                        if (!contOk) break;
                        auto nextName = clauseFuncName(contLine);
                        if (nextName && *nextName == *name) {
                            source += "\n" + contLine;
                            int extra = countBlocks(contLine);
                            while (extra > 0) {
                                auto [contLine2, contOk2] = readLine("...> ");
                                if (!contOk2) break;
                                source += "\n" + contLine2;
                                extra += countBlocks(contLine2);
                            }
                        } else {
                            if (!contLine.empty()) pendingLine = contLine;
                            break;
                        }
                    }
                }
            }

            // Show tokens if enabled
            if (showTokens) {
                kex::Lexer debugLexer(source);
                auto debugTokens = debugLexer.tokenizeAll();
                std::cout << "  tokens: ";
                for (const auto& t : debugTokens) {
                    if (t.type == kex::TokenType::Eof || t.type == kex::TokenType::Newline) continue;
                    std::cout << kex::tokenTypeName(t.type);
                    if (!t.value.empty()) std::cout << "[" << t.value << "]";
                    std::cout << " ";
                }
                std::cout << "\n";
            }

            auto execProgram = [&](kex::ast::Program* program) -> kex::interpreter::ValuePtr {
                if (showAst) printAst(*program);
                return evaluator.execute(*program);
            };

            auto showResult = [&](const kex::interpreter::ValuePtr& result) {
                if (result && !std::holds_alternative<kex::interpreter::NoneValue>(result->data)) {
                    std::cout << color::dim << "=> " << color::reset << colorValue(result);
                    if (showTypes) {
                        std::cout << " " << color::dim << ":" << color::reset
                                  << " " << color::cyan << result->typeName() << color::reset;
                    }
                    std::cout << "\n";
                }
            };

            // Detect if this is a top-level definition (not an expression)
            bool isFuncDef = false;
            size_t defOffset = std::string::npos;
            if (source.substr(0, 4) == "let ") defOffset = 4;
            else if (source.substr(0, 5) == "foul ") defOffset = 5;
            if (defOffset != std::string::npos) {
                // It's a function def if: let/foul name( ... )  with parens before =
                auto parenPos = source.find('(', defOffset);
                auto eqPos = source.find('=', defOffset);
                auto doPos = source.find(" do", defOffset);
                isFuncDef = (parenPos != std::string::npos &&
                             (eqPos == std::string::npos || parenPos < eqPos) &&
                             (doPos == std::string::npos || parenPos < doPos));
            }
            if (source.substr(0, 7) == "module " || source.substr(0, 5) == "type " ||
                source.substr(0, 7) == "record " || source.substr(0, 5) == "make " ||
                source.substr(0, 12) == "foul module ") {
                isFuncDef = true;
            }

            try {
                if (isFuncDef) {
                    // Parse as top-level definition
                    kex::Lexer lexer(source);
                    auto tokens = lexer.tokenizeAll();
                    kex::Parser parser(std::move(tokens));
                    auto* program = new kex::ast::Program(parser.parseProgram());
                    replPrograms.push_back(program);
                    execProgram(program);
                } else {
                    // Wrap in main for expression evaluation
                    auto wrapped = "main do\n" + source + "\nend\n";
                    kex::Lexer lexer(wrapped);
                    auto tokens = lexer.tokenizeAll();
                    kex::Parser parser(std::move(tokens));
                    auto* program = new kex::ast::Program(parser.parseProgram());
                    replPrograms.push_back(program);
                    auto result = execProgram(program);
                    showResult(result);
                }
            } catch (const std::exception& e) {
                std::cerr << "  error: " << e.what() << "\n";
            }
        }
        return 0;
    }

    std::string filepath = argv[optind];
    auto source = readFile(filepath);
    if (source.empty()) return 1;

    // Everything after the script path is the script's own argument list,
    // exposed to Kex code via `main(args) do ... end`.
    std::vector<std::string> scriptArgs;
    for (int i = optind + 1; i < argc; i++) {
        scriptArgs.push_back(argv[i]);
    }

    kex::Lexer lexer(std::move(source), filepath);
    auto tokens = lexer.tokenizeAll();

    if (mode == "lex") {
        for (const auto& token : tokens) {
            std::cout << token.location.line << ":" << token.location.column
                      << "  " << kex::tokenTypeName(token.type);
            if (!token.value.empty()) {
                std::cout << "  [" << token.value << "]";
            }
            std::cout << "\n";
        }
        return 0;
    }

    try {
        kex::Parser parser(std::move(tokens), filepath);
        auto program = parser.parseProgram();

        if (mode == "parse") {
            printAst(program);
            return 0;
        }

        if (mode == "run") {
            kex::interpreter::Evaluator evaluator;
            evaluator.setArgs(scriptArgs);

            // Must outlive `evaluator.execute(program)` below: the
            // evaluator keeps raw `const ast::FunctionDef*` pointers into
            // whatever Program owns these nodes (see m_functionDefs), so a
            // Program that goes out of scope before the evaluator is done
            // leaves those pointers dangling — declaring it here, not
            // inside the loop, keeps it alive for the rest of this block.
            kex::ast::Program declarationsOnly;
            for (const auto& candidate : specBaseCandidates(filepath)) {
                if (!fileExists(candidate)) continue;

                auto baseSource = readFile(candidate);
                kex::Lexer baseLexer(std::move(baseSource), candidate);
                auto baseTokens = baseLexer.tokenizeAll();
                kex::Parser baseParser(std::move(baseTokens), candidate);
                auto baseProgram = baseParser.parseProgram();

                declarationsOnly.items.reserve(baseProgram.items.size());
                for (auto& item : baseProgram.items) {
                    if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(item)) {
                        declarationsOnly.items.push_back(std::move(item));
                    }
                }
                evaluator.execute(declarationsOnly);
                break;
            }

            evaluator.execute(program);
            return 0;
        }

        // mode == "check"
        kex::semantic::Analyzer analyzer;
        bool ok = analyzer.analyze(program);

        for (const auto& diag : analyzer.diagnostics()) {
            auto prefix = diag.level == kex::semantic::Diagnostic::Level::Error
                ? "error" : "warning";
            std::cerr << diag.location.file << ":" << diag.location.line << ":"
                      << diag.location.column << ": " << prefix << ": "
                      << diag.message << "\n";
        }

        if (ok) {
            std::cout << "No errors found.\n";
        }

        return ok ? 0 : 1;
    } catch (const std::runtime_error& e) {
        std::cerr << "Parse error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
