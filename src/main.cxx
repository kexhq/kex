#include "common/color.hxx"
#include "lexer/lexer.hxx"
#include "parser/parser.hxx"
#include "semantic/analyzer.hxx"
#include "semantic/db.hxx"
#include "interpreter/evaluator.hxx"
#include <cctype>
#include <filesystem>
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

namespace {

auto isIdentChar(char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Syntax-highlights a diagnostic message body using a palette kept distinct
// from Value::inspect's literal coloring: type names / type vars become cyan
// (matching the REPL/IO.inspect type suffix), function names become bold
// (backtick spans and signature heads), arrows (->) become magenta, and
// backtick delimiters become gray. Cyan-vs-bold keeps functions and types
// distinguishable. Honors kex::color::enabled.
auto colorizeMessage(const std::string& msg) -> std::string {
    using namespace kex::color;
    std::string out;
    out.reserve(msg.size() * 2);
    const auto n = msg.size();
    bool atLineStart = true;
    for (size_t i = 0; i < n; ) {
        char c = msg[i];

        if (c == '`') {
            size_t end = msg.find('`', i + 1);
            if (end == std::string::npos) { out += c; i++; continue; }
            out += apply(gray); out += '`'; out += apply(reset);
            out += apply(bold);
            out.append(msg, i + 1, end - i - 1);
            out += apply(reset);
            out += apply(gray); out += '`'; out += apply(reset);
            i = end + 1;
            atLineStart = false;
            continue;
        }

        if (atLineStart) {
            bool startsLower = std::isalpha(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(c));
            bool identStart = std::isalpha(static_cast<unsigned char>(c)) || c == '_';
            if (startsLower || (c == '_' && identStart)) {
                size_t j = i + 1;
                while (j < n && isIdentChar(msg[j])) j++;
                if (j < n && (msg[j] == '?' || msg[j] == '!')) j++;
                size_t k = j;
                while (k < n && msg[k] == ' ') k++;
                if (k < n && msg[k] == ':') {
                    out += apply(bold);
                    out.append(msg, i, j - i);
                    out += apply(reset);
                    i = j;
                    atLineStart = false;
                    continue;
                }
            }
        }

        if (c == '-' && i + 1 < n && msg[i + 1] == '>') {
            out += apply(magenta);
            out += "->";
            out += apply(reset);
            i += 2;
            atLineStart = false;
            continue;
        }

        if (std::isupper(static_cast<unsigned char>(c)) && (i == 0 || !isIdentChar(msg[i - 1]))) {
            size_t j = i + 1;
            while (j < n && isIdentChar(msg[j])) j++;
            out += apply(cyan);
            out.append(msg, i, j - i);
            out += apply(reset);
            i = j;
            atLineStart = false;
            continue;
        }

        if (c == '\n') { out += c; i++; atLineStart = true; continue; }
        out += c; i++;
        atLineStart = false;
    }
    return out;
}

} // namespace

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

auto printUsage(const char* progName) -> void {
    std::cerr << "Usage: " << progName << " [options] <file.kex>\n"
              << "\n"
              << "Options:\n"
              << "  -r, --run         Execute the program (default); aborts on type errors\n"
              << "  -n, --no-check    Skip semantic check when running\n"
              << "  -l, --lex         Print token stream\n"
              << "  -p, --parse       Print AST\n"
              << "  -c, --check       Run semantic analysis only\n"
              << "  -t, --types       With --check: dump inferred expression types\n"
              << "  -h, --help        Show this help\n"
              << "  -v, --version     Show version\n"
              << "  --no-colors       Disable ANSI color output\n";
}

auto printVersion() -> void {
    std::cout << "kex 0.1.0\n";
}

int main(int argc, char* argv[]) {
    static struct option longOptions[] = {
        {"run",      no_argument, nullptr, 'r'},
        {"no-check", no_argument, nullptr, 'n'},
        {"lex",      no_argument, nullptr, 'l'},
        {"parse",    no_argument, nullptr, 'p'},
        {"check",    no_argument, nullptr, 'c'},
        {"types",    no_argument, nullptr, 't'},
        {"help",     no_argument, nullptr, 'h'},
        {"version",  no_argument, nullptr, 'v'},
        {"no-colors",no_argument, nullptr, 'N'},
        {nullptr,    0,           nullptr,  0 }
    };

    std::string mode = "run";
    bool skipCheck = false;
    bool dumpTypes = false;
    int opt;

    while ((opt = getopt_long(argc, argv, "rnlcpthv", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 'r': mode = "run"; break;
            case 'n': skipCheck = true; break;
            case 'l': mode = "lex"; break;
            case 'p': mode = "parse"; break;
            case 'c': mode = "check"; break;
            case 't': dumpTypes = true; break;
            case 'h': printUsage(argv[0]); return 0;
            case 'v': printVersion(); return 0;
            case 'N': kex::color::enabled = false; break;
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
                if (result && !std::holds_alternative<kex::interpreter::UnitValue>(result->data)) {
                    std::cout << kex::color::apply(kex::color::gray) << "=> "
                              << kex::color::apply(kex::color::reset) << result->inspect();
                    if (showTypes) {
                        std::cout << " " << kex::color::apply(kex::color::gray) << ":"
                                  << kex::color::apply(kex::color::reset)
                                  << " " << kex::color::apply(kex::color::cyan)
                                  << result->typeName() << kex::color::apply(kex::color::reset);
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
                std::cerr << "  " << kex::color::apply(kex::color::red) << "error:"
                          << kex::color::apply(kex::color::reset) << " " << e.what() << "\n";
            }
        }
        return 0;
    }

    std::string filepath = argv[optind];
    auto source = readFile(filepath);
    if (source.empty()) return 1;

    // Honour `# kex: no-check` pragma in the first few lines — any file that
    // contains it is treated as if --no-check was passed on the command line.
    {
        std::istringstream ss(source);
        std::string line;
        for (int ln = 0; ln < 10 && std::getline(ss, line); ++ln) {
            if (line.find("# kex: no-check") != std::string::npos) {
                skipCheck = true;
                break;
            }
        }
    }

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

        if (mode == "run" && !skipCheck) {
            kex::semantic::Analyzer analyzer;
            bool ok = analyzer.analyze(program);
            for (const auto& diag : analyzer.diagnostics()) {
                bool isError = diag.level == kex::semantic::Diagnostic::Level::Error;
                std::cerr << kex::color::apply(kex::color::gray)
                          << diag.location.file << ":" << diag.location.line << ":"
                          << diag.location.column << ":" << kex::color::apply(kex::color::reset)
                          << " " << kex::color::apply(kex::color::bold)
                          << (isError ? kex::color::apply(kex::color::red) : kex::color::apply(kex::color::magenta))
                          << (isError ? "error" : "warning") << ":"
                          << kex::color::apply(kex::color::reset) << " "
                          << colorizeMessage(diag.message) << "\n";
            }
            if (!ok) {
                std::cerr << kex::color::apply(kex::color::bold)
                          << kex::color::apply(kex::color::magenta)
                          << "Aborted:" << kex::color::apply(kex::color::reset)
                          << " fix type errors before running (use --no-check to skip).\n";
                return 1;
            }
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
        // Pass 1+2: collect symbols and resolve names via SemanticDB
        kex::semantic::SemanticDB db;

        // Index all prelude files first so their TypeAnnotation declarations
        // (describe, it, assert, etc.) are visible to ResolvePass when checking
        // any user file.
#ifdef KEX_PRELUDE_DIR
        {
            std::filesystem::path preludeDir(KEX_PRELUDE_DIR);
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(preludeDir, ec)) {
                if (entry.path().extension() == ".kex") {
                    db.updateFile(entry.path().string(), readFile(entry.path().string()));
                }
            }
        }
#endif

        db.updateFile(filepath, readFile(filepath));

        // Pass 3+: existing Analyzer (purity, type checking)
        kex::semantic::Analyzer analyzer;
        bool ok = analyzer.analyze(program);

        // Merge diagnostics: SemanticDB first, then Analyzer
        auto printDiag = [&](const kex::semantic::Diagnostic& diag) {
            bool isError = diag.level == kex::semantic::Diagnostic::Level::Error;
            std::cerr << kex::color::apply(kex::color::gray)
                      << diag.location.file << ":" << diag.location.line << ":"
                      << diag.location.column << ":" << kex::color::apply(kex::color::reset)
                      << " " << kex::color::apply(kex::color::bold)
                      << (isError ? kex::color::apply(kex::color::red) : kex::color::apply(kex::color::magenta))
                      << (isError ? "error" : "warning") << ":"
                      << kex::color::apply(kex::color::reset) << " "
                      << colorizeMessage(diag.message) << "\n";
        };
        bool dbOk = true;
        for (const auto& diag : db.diagnosticsFor(filepath)) {
            if (diag.level == kex::semantic::Diagnostic::Level::Error) dbOk = false;
            printDiag(diag);
        }
        for (const auto& diag : analyzer.diagnostics()) {
            printDiag(diag);
        }

        if (dumpTypes) {
            // Collect and sort by source location so output is readable top-to-bottom.
            const auto& tmap = analyzer.typeMap();
            std::vector<std::pair<const kex::ast::Expr*, kex::semantic::TypePtr>> entries(
                tmap.begin(), tmap.end());
            std::sort(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) {
                    const auto& la = a.first->location;
                    const auto& lb = b.first->location;
                    if (la.line != lb.line) return la.line < lb.line;
                    return la.column < lb.column;
                });
            for (const auto& [expr, type] : entries) {
                if (!type) continue;
                std::cout << expr->location.line << ":" << expr->location.column
                          << "  " << kex::semantic::typeToString(type) << "\n";
            }
        }

        bool allOk = ok && dbOk;
        if (allOk && !dumpTypes) {
            std::cout << "No errors found.\n";
        } else if (allOk) {
            std::cerr << "No errors found.\n";
        }

        return allOk ? 0 : 1;
    } catch (const std::runtime_error& e) {
        std::cerr << kex::color::apply(kex::color::bold) << kex::color::apply(kex::color::red)
                  << "Parse error:" << kex::color::apply(kex::color::reset)
                  << " " << e.what() << "\n";
        return 1;
    }

    return 0;
}
