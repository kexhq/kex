#include "common/color.hxx"
#include "lexer/lexer.hxx"
#include "parser/parser.hxx"
#include "codegen/core_erlang.hxx"
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
#include "common/completion.hxx"

// Completion state — populated before the REPL loop, read by the C callback.
static kex::semantic::SemanticDB *g_replDb = nullptr;
// Set to the type name while the user is typing inside a `make X do` block,
// so the completer can infer parameter types from pattern signatures.
static std::string g_currentMakeTarget;
static std::vector<std::string> g_completionMatches;
static std::string g_completionWord;
static std::string g_completionStripPrefix;
static std::string g_completionRewriteTo;

extern "C"
{
    // Display hook: strip the shared "Qualifier." prefix from every entry so the
    // list shows just member names like `map` instead of `[123,1,123,123].map`.
    static void kexDisplayMatches(char **matches, int num_matches, int /*max_length*/)
    {
        // matches[0] is readline's longest-common-prefix; find the last '.' in it
        // to determine how many characters to strip from every completion.
        size_t stripLen = 0;
        if (matches[0])
        {
            std::string common(matches[0]);
            auto dot = common.rfind('.');
            if (dot != std::string::npos)
                stripLen = dot + 1;
        }

        if (stripLen == 0)
        {
            // No dot — let readline display normally via its own list formatter.
            rl_display_match_list(matches, num_matches, 0);
            rl_on_new_line();
            return;
        }

        // Build a temporary array of stripped names and call rl_display_match_list.
        std::vector<char *> stripped(num_matches + 2);
        stripped[0] = strdup(matches[0] + stripLen); // stripped common prefix
        int newMax = 0;
        for (int i = 1; i <= num_matches; i++)
        {
            const char *src = matches[i];
            size_t srcLen = std::strlen(src);
            const char *member = (srcLen > stripLen) ? src + stripLen : src;
            stripped[i] = strdup(member);
            int len = static_cast<int>(std::strlen(stripped[i]));
            if (len > newMax)
                newMax = len;
        }
        stripped[num_matches + 1] = nullptr;

        rl_display_match_list(stripped.data(), num_matches, newMax);

        for (int i = 0; i <= num_matches; i++)
            free(stripped[i]);
        rl_on_new_line();
    }

    static char *kexCompletionEntry(const char * /*text*/, int state)
    {
        if (state == 0)
        {
            g_completionMatches.clear();
            if (g_replDb)
            {
                auto raw = g_replDb->completionsFor(g_completionWord);
                g_completionMatches = kex::rewriteCompletions(std::move(raw),
                                                              g_completionStripPrefix,
                                                              g_completionRewriteTo);
            }
        }
        if (state < static_cast<int>(g_completionMatches.size()))
            return strdup(g_completionMatches[state].c_str());
        return nullptr;
    }
    static char **kexCompletion(const char *text, int start, int end)
    {
        rl_attempted_completion_over = 1;
        rl_completion_suppress_append = 1; // no trailing space/quote after completion
        auto cq = kex::resolveCompletionQuery(rl_line_buffer, start, text);

        // If the DB query still has an unresolved lowercase qualifier (e.g. "x.")
        // and we're inside a `make X` block, try to resolve it via parameter
        // pattern inference (handles `@[x|xs]` head/tail and simple named params).
        if (!g_currentMakeTarget.empty())
        {
            auto dotPos = cq.dbQuery.rfind('.');
            if (dotPos != std::string::npos)
            {
                std::string qualifier = cq.dbQuery.substr(0, dotPos);
                bool looksUnresolved = !qualifier.empty() && std::islower((unsigned char)qualifier[0]) && std::all_of(qualifier.begin(), qualifier.end(), [](char c)
                                                                                                                      { return std::isalnum((unsigned char)c) || c == '_'; });
                if (looksUnresolved)
                {
                    std::string inferred = kex::inferPatternParamType(
                        rl_line_buffer, qualifier, g_currentMakeTarget);
                    if (!inferred.empty())
                    {
                        std::string memberPart = cq.dbQuery.substr(dotPos + 1);
                        cq.dbQuery = inferred + "." + memberPart;
                        cq.rewriteFrom = inferred + ".";
                        // cq.rewriteTo keeps the original "x." so readline inserts correctly
                    }
                }
            }
        }

        g_completionWord = cq.dbQuery;
        g_completionStripPrefix = cq.rewriteFrom;
        g_completionRewriteTo = cq.rewriteTo;

        static bool debugCompl = (std::getenv("KEX_DEBUG_COMPLETE") != nullptr);
        if (debugCompl)
        {
            fprintf(stderr, "\n[complete] linebuf=%s start=%d end=%d text=%s"
                            " -> query=%s rewriteFrom=%s rewriteTo=%s\n",
                    rl_line_buffer, start, end, text,
                    cq.dbQuery.c_str(), cq.rewriteFrom.c_str(), cq.rewriteTo.c_str());
            auto preview = g_replDb ? g_replDb->completionsFor(cq.dbQuery)
                                    : std::vector<std::string>{};
            auto rewritten = kex::rewriteCompletions(preview, cq.rewriteFrom, cq.rewriteTo);
            for (const auto &c : rewritten)
                fprintf(stderr, "  [match] %s\n", c.c_str());
        }

        return rl_completion_matches(text, kexCompletionEntry);
    }
} // extern "C"
#endif

auto readLine(const std::string &prompt) -> std::pair<std::string, bool>
{
#ifdef HAS_READLINE
    char *input = readline(prompt.c_str());
    if (!input)
        return {"", false};
    std::string line(input);
    if (!line.empty())
        add_history(input);
    free(input);
    return {line, true};
#else
    std::cout << prompt;
    std::string line;
    if (!std::getline(std::cin, line))
        return {"", false};
    return {line, true};
#endif
}

auto readFile(const std::string &path) -> std::string
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::cerr << "Error: could not open file: " << path << "\n";
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

namespace
{

    auto isIdentChar(char c) -> bool
    {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    // ===== TypeExpr pretty-printer =====

    auto typeExprToString(const kex::ast::TypeExpr &te) -> std::string;

    auto typeNameToString(const kex::ast::TypeName &tn) -> std::string
    {
        std::string r;
        for (size_t i = 0; i < tn.parts.size(); i++)
        {
            if (i)
                r += ".";
            r += tn.parts[i];
        }
        return r;
    }

    auto typeExprToString(const kex::ast::TypeExpr &te) -> std::string
    {
        return std::visit([](const auto &node) -> std::string
                          {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, kex::ast::TypeName>) {
            return typeNameToString(node);
        } else if constexpr (std::is_same_v<T, kex::ast::GenericType>) {
            std::string r = typeNameToString(node.name) + "<";
            for (size_t i = 0; i < node.args.size(); i++) {
                if (i) r += ", ";
                if (node.args[i]) r += typeExprToString(*node.args[i]);
            }
            return r + ">";
        } else if constexpr (std::is_same_v<T, kex::ast::FunctionType>) {
            std::string l = node.param ? typeExprToString(*node.param) : "?";
            std::string r = node.result ? typeExprToString(*node.result) : "?";
            return l + " -> " + r;
        } else if constexpr (std::is_same_v<T, kex::ast::TupleType>) {
            std::string r = "(";
            for (size_t i = 0; i < node.elements.size(); i++) {
                if (i) r += ", ";
                if (node.elements[i]) r += typeExprToString(*node.elements[i]);
            }
            return r + ")";
        } else if constexpr (std::is_same_v<T, kex::ast::ListType>) {
            return "[" + (node.element ? typeExprToString(*node.element) : "?") + "]";
        } else if constexpr (std::is_same_v<T, kex::ast::MapType>) {
            std::string k = node.key ? typeExprToString(*node.key) : "?";
            std::string v = node.value ? typeExprToString(*node.value) : "?";
            return "Map<" + k + ", " + v + ">";
        } else if constexpr (std::is_same_v<T, kex::ast::UnionType>) {
            std::string l = node.left ? typeExprToString(*node.left) : "?";
            std::string r = node.right ? typeExprToString(*node.right) : "?";
            return l + " | " + r;
        } else if constexpr (std::is_same_v<T, kex::ast::OptionalType>) {
            return (node.inner ? typeExprToString(*node.inner) : "?") + "?";
        } else if constexpr (std::is_same_v<T, kex::ast::BlockType>) {
            return "Block<" + (node.inner ? typeExprToString(*node.inner) : "?") + ">";
        } else if constexpr (std::is_same_v<T, kex::ast::AtomType>) {
            return ":" + node.name;
        } else if constexpr (std::is_same_v<T, kex::ast::GenericVar>) {
            return node.name;
        }
        return "?"; }, te.kind);
    }

    // ===== JSON helpers (no external dep — hand-rolled) =====

    auto jsonEscape(const std::string &s) -> std::string
    {
        std::string out;
        out.reserve(s.size() + 4);
        for (char c : s)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
            }
        }
        return out;
    }

    // Extract the "did you mean `X`?" portion from a diagnostic message, if any.
    auto extractHint(const std::string &msg) -> std::string
    {
        const std::string needle = "did you mean `";
        auto pos = msg.find(needle);
        if (pos == std::string::npos)
            return "";
        pos += needle.size();
        auto end = msg.find('`', pos);
        if (end == std::string::npos)
            return "";
        return msg.substr(pos, end - pos);
    }

    // Syntax-highlights a diagnostic message body using a palette kept distinct
    // from Value::inspect's literal coloring: type names / type vars become cyan
    // (matching the REPL/IO.inspect type suffix), function names become bold
    // (backtick spans and signature heads), arrows (->) become magenta, and
    // backtick delimiters become gray. Cyan-vs-bold keeps functions and types
    // distinguishable. Honors kex::color::enabled.
    auto colorizeMessage(const std::string &msg) -> std::string
    {
        using namespace kex::color;
        std::string out;
        out.reserve(msg.size() * 2);
        const auto n = msg.size();
        bool atLineStart = true;
        for (size_t i = 0; i < n;)
        {
            char c = msg[i];

            if (c == '`')
            {
                size_t end = msg.find('`', i + 1);
                if (end == std::string::npos)
                {
                    out += c;
                    i++;
                    continue;
                }
                out += apply(gray);
                out += '`';
                out += apply(reset);
                out += apply(bold);
                out.append(msg, i + 1, end - i - 1);
                out += apply(reset);
                out += apply(gray);
                out += '`';
                out += apply(reset);
                i = end + 1;
                atLineStart = false;
                continue;
            }

            if (atLineStart)
            {
                bool startsLower = std::isalpha(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(c));
                bool identStart = std::isalpha(static_cast<unsigned char>(c)) || c == '_';
                if (startsLower || (c == '_' && identStart))
                {
                    size_t j = i + 1;
                    while (j < n && isIdentChar(msg[j]))
                        j++;
                    if (j < n && (msg[j] == '?' || msg[j] == '!'))
                        j++;
                    size_t k = j;
                    while (k < n && msg[k] == ' ')
                        k++;
                    if (k < n && msg[k] == ':')
                    {
                        out += apply(bold);
                        out.append(msg, i, j - i);
                        out += apply(reset);
                        i = j;
                        atLineStart = false;
                        continue;
                    }
                }
            }

            if (c == '-' && i + 1 < n && msg[i + 1] == '>')
            {
                out += apply(magenta);
                out += "->";
                out += apply(reset);
                i += 2;
                atLineStart = false;
                continue;
            }

            if (std::isupper(static_cast<unsigned char>(c)) && (i == 0 || !isIdentChar(msg[i - 1])))
            {
                size_t j = i + 1;
                while (j < n && isIdentChar(msg[j]))
                    j++;
                out += apply(cyan);
                out.append(msg, i, j - i);
                out += apply(reset);
                i = j;
                atLineStart = false;
                continue;
            }

            if (c == '\n')
            {
                out += c;
                i++;
                atLineStart = true;
                continue;
            }
            out += c;
            i++;
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
auto specBaseCandidates(const std::string &filepath) -> std::vector<std::string>
{
    static const std::string suffix = ".spec.kex";
    if (filepath.size() <= suffix.size())
        return {};
    if (filepath.compare(filepath.size() - suffix.size(), suffix.size(), suffix) != 0)
        return {};

    std::string stem = filepath.substr(0, filepath.size() - suffix.size());
    auto slash = stem.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : stem.substr(0, slash);
    std::string name = slash == std::string::npos ? stem : stem.substr(slash + 1);

    std::vector<std::string> candidates = {stem + ".kex"};
    const std::string specSuffix = "spec";
    if (dir.size() >= specSuffix.size() &&
        dir.compare(dir.size() - specSuffix.size(), specSuffix.size(), specSuffix) == 0)
    {
        std::string parent = dir.substr(0, dir.size() - specSuffix.size());
        candidates.push_back(parent + "examples/" + name + ".kex");
    }
    return candidates;
}

auto fileExists(const std::string &path) -> bool
{
    std::ifstream probe(path);
    return probe.good();
}

auto loadPrelude(kex::semantic::SemanticDB &db) -> void
{
#ifdef KEX_PRELUDE_DIR
    std::filesystem::path preludeDir(KEX_PRELUDE_DIR);
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(preludeDir, ec))
    {
        if (entry.path().extension() == ".kex")
            db.updateFile(entry.path().string(), readFile(entry.path().string()));
    }
#endif
}

// Runs semantic analysis (undefined-name detection + type checking) and
// prints any diagnostics, same as plain `run` mode's pre-execution check.
// Shared by `run` and `compile`/`-R` (BEAM) so both backends catch the same
// errors before doing anything backend-specific — previously `-R` skipped
// this entirely and fell straight through to erlc, which reports things
// like an undefined function as a raw, un-Kex-like Core Erlang compile
// error ("unbound variable 'UndefinedFunctionCall' in main/0") instead of
// this backend-agnostic diagnostic. Returns false if there were any errors.
auto runSemanticCheck(const kex::ast::Program &program, const std::string &filepath) -> bool
{
    auto printDiag = [&](const kex::semantic::Diagnostic &diag)
    {
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

    // Pass 1+2: SemanticDB undefined-name detection
    kex::semantic::SemanticDB runDb;
    loadPrelude(runDb);
    runDb.updateFile(filepath, readFile(filepath));
    bool dbOk = true;
    for (const auto &diag : runDb.diagnosticsFor(filepath))
    {
        if (diag.level == kex::semantic::Diagnostic::Level::Error)
            dbOk = false;
        printDiag(diag);
    }

    // Pass 3+: existing Analyzer (purity, type checking)
    kex::semantic::Analyzer analyzer;
    bool ok = analyzer.analyze(program);
    for (const auto &diag : analyzer.diagnostics())
        printDiag(diag);

    return ok && dbOk;
}

auto printAst(const kex::ast::Program &program) -> void
{
    std::cout << "Program (" << program.items.size() << " items)\n";
    for (const auto &item : program.items)
    {
        std::visit([](const auto &node)
                   {
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
            } }, item);
    }
}

// Absolute path to cmake-pre-built Kex runtime beams (kex_io.beam, ...),
// or empty if none is available. Using these avoids an erlc spawn (~0.15s
// per module) on every BEAM invocation.
auto prebuiltRuntimeBeamDir() -> std::string
{
#ifdef KEX_RUNTIME_BEAM_DIR
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(fs::path{KEX_RUNTIME_BEAM_DIR} / "kex_io.beam", ec))
        return KEX_RUNTIME_BEAM_DIR;
#else
    (void)0;
#endif
    return {};
}

// Fallback: walk up from argv[0] to find runtime/src/*.erl and compile them
// into a fresh temp dir. Returns the dir (empty on failure); caller owns
// cleanup via fs::remove_all.
auto compileRuntimeBeamsToTemp(const char* argv0) -> std::string
{
    namespace fs = std::filesystem;
    fs::path bin = fs::weakly_canonical(argv0);
    for (auto dir = bin.parent_path(); dir != dir.root_path(); dir = dir.parent_path())
    {
        auto rtSrc = dir / "runtime" / "src";
        if (!fs::exists(rtSrc)) continue;
        char tmp[] = "/tmp/kex_rt_XXXXXX";
        if (!mkdtemp(tmp)) return {};
        std::string out = tmp;
        for (const auto& e : fs::directory_iterator(rtSrc))
        {
            if (e.path().extension() == ".erl")
            {
                std::string cmd = "erlc -o " + out + " "
                                + e.path().string() + " > /dev/null 2>&1";
                std::system(cmd.c_str());
            }
        }
        return out;
    }
    return {};
}

auto printUsage(const char *progName) -> void
{
    std::cerr << "Usage: " << progName << " [options] <file.kex>\n"
              << "\n"
              << "Options:\n"
              << "  -r, --run         Interpret the program (default)\n"
              << "  -c, --compile     Compile to BEAM via Core Erlang\n"
              << "  -R, --run-beam    Run on BEAM (.kex or existing .beam; temp dir, auto-clean)\n"
              << "  -i, --interactive Interactive REPL on BEAM (also: kex -R with no file)\n"
              << "  -C, --check       Run semantic analysis only\n"
              << "  -n, --no-check    Skip semantic check when running\n"
              << "  -l, --lex         Print token stream\n"
              << "  -p, --parse       Print AST\n"
              << "  -j, --json        With --check: output diagnostics as JSON\n"
              << "  -s, --summary     Print public API signatures (Kex syntax)\n"
              << "  -t, --types       With --check: dump inferred expression types\n"
              << "  -e, --emit-core   Emit Core Erlang (.core) — does not invoke erlc\n"
              << "  -o <dir>          Output directory for -c / --emit-core (default: .)\n"
              << "  -h, --help        Show this help\n"
              << "  -v, --version     Show version\n"
              << "  --no-colors       Disable ANSI color output\n";
}

auto printVersion() -> void
{
    std::cout << "kex 0.1.0\n";
}

int main(int argc, char *argv[])
{
    static struct option longOptions[] = {
        {"run", no_argument, nullptr, 'r'},
        {"no-check", no_argument, nullptr, 'n'},
        {"lex", no_argument, nullptr, 'l'},
        {"parse", no_argument, nullptr, 'p'},
        {"check", no_argument, nullptr, 'C'},
        {"compile", no_argument, nullptr, 'c'},
        {"run-beam", no_argument, nullptr, 'R'},
        {"interactive", no_argument, nullptr, 'i'},
        {"json", no_argument, nullptr, 'j'},
        {"summary", no_argument, nullptr, 's'},
        {"types", no_argument, nullptr, 't'},
        {"emit-core", no_argument, nullptr, 'e'},
        {"complete", required_argument, nullptr, 'K'},
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, 'v'},
        {"no-colors", no_argument, nullptr, 'N'},
        {nullptr, 0, nullptr, 0}};

    std::string mode = "run";
    bool skipCheck = false;
    bool dumpTypes = false;
    bool jsonOutput = false;
    bool summaryMode = false;
    std::string completePrefix;
    std::string outputDir = ".";
    bool outputDirExplicit = false;
    int opt;

    bool compileRun = false;
    while ((opt = getopt_long(argc, argv, "rnlcCiRjspethvK:o:", longOptions, nullptr)) != -1)
    {
        switch (opt)
        {
        case 'r':
            mode = "run";
            break;
        case 'n':
            skipCheck = true;
            break;
        case 'l':
            mode = "lex";
            break;
        case 'p':
            mode = "parse";
            break;
        case 'C':
            mode = "check";
            break;
        case 'c':
            mode = "compile";
            break;
        case 'R':
            mode = "compile";
            compileRun = true;
            break;
        case 'i':
            mode = "beam-repl";
            break;
        case 'j':
            jsonOutput = true;
            mode = "check";
            break;
        case 's':
            summaryMode = true;
            mode = "check";
            break;
        case 't':
            dumpTypes = true;
            break;
        case 'e':
            mode = "emit-core";
            break;
        case 'o':
            outputDir = optarg;
            outputDirExplicit = true;
            break;
        case 'K':
            completePrefix = optarg;
            mode = "complete";
            break;
        case 'h':
            printUsage(argv[0]);
            return 0;
        case 'v':
            printVersion();
            return 0;
        case 'N':
            kex::color::enabled = false;
            break;
        default:
            printUsage(argv[0]);
            return 1;
        }
    }

    if (mode == "complete")
    {
        kex::semantic::SemanticDB db;
        loadPrelude(db);
        if (optind < argc)
            db.updateFile(argv[optind], readFile(argv[optind]));
        // Simulate readline not splitting: start=0, text=completePrefix
        auto cq = kex::resolveCompletionQuery(completePrefix.c_str(), 0,
                                              completePrefix.c_str());
        auto raw = db.completionsFor(cq.dbQuery);
        auto completions = kex::rewriteCompletions(std::move(raw),
                                                   cq.rewriteFrom, cq.rewriteTo);
        for (const auto &c : completions)
            std::cout << c << "\n";
        return 0;
    }

    if (optind >= argc && mode != "repl")
    {
        // No file — enter REPL mode (BEAM REPL if -R was given, tree-walker otherwise)
        if (mode == "compile" && compileRun)
            mode = "beam-repl";
        else if (mode != "beam-repl")
            mode = "repl";
    }

    if (mode == "beam-repl")
    {
        // ── Writable temp dir for per-eval user beams ─────────────────────
        char rtmpl[] = "/tmp/kex_irepl_XXXXXX";
        char* rtd = mkdtemp(rtmpl);
        if (!rtd) { std::cerr << "error: mkdtemp failed\n"; return 1; }
        std::string beamDir = rtd;

        // Put runtime beams (kex_io, ...) on the path. Prefer the cmake
        // pre-built dir (added as an extra -pa at run time); fall back to
        // compiling them into beamDir so a single -pa suffices.
        std::string rtPaDir = prebuiltRuntimeBeamDir();
        if (rtPaDir.empty())
        {
            namespace fs = std::filesystem;
            fs::path bin = fs::weakly_canonical(argv[0]);
            for (auto dir = bin.parent_path(); dir != dir.root_path(); dir = dir.parent_path())
            {
                auto rtDir = dir / "runtime" / "src";
                if (fs::exists(rtDir)) {
                    for (const auto& entry : fs::directory_iterator(rtDir))
                        if (entry.path().extension() == ".erl") {
                            std::string cmd = "erlc -o " + beamDir + " "
                                            + entry.path().string() + " > /dev/null 2>&1";
                            std::system(cmd.c_str());
                        }
                    break;
                }
            }
        }

        std::cout << "kex 0.1.0 — interactive REPL (BEAM)\n";
        std::cout << "Type :help for available commands, exit to quit.\n\n";

        std::string topDefs;    // function defs — emitted above main do
        std::string localBinds; // let x = ... — emitted inside main do
        int iteration = 0;

        // Shared countBlocks lambda for multi-line detection
        auto countBlocksBeam = [](const std::string &s) -> int {
            int count = 0;
            for (size_t i = 0; i < s.size(); i++) {
                if (i + 2 <= s.size() && s.substr(i, 2) == "do") {
                    bool wb = (i > 0 && std::isalnum(s[i - 1]));
                    bool wa = (i + 2 < s.size() && std::isalnum(s[i + 2]));
                    if (!wb && !wa) count++;
                }
                if (i + 3 <= s.size() && s.substr(i, 3) == "end") {
                    bool wb = (i > 0 && std::isalnum(s[i - 1]));
                    bool wa = (i + 3 < s.size() && std::isalnum(s[i + 3]));
                    if (!wb && !wa) count--;
                }
            }
            return count;
        };

        while (true) {
            auto [input, ok] = readLine("kex(beam)> ");
            if (!ok) break;
            if (input.empty()) continue;
            if (input == "exit" || input == ":exit" || input == ":quit" || input == ":q") break;
            if (input == ":help" || input == ":h") {
                std::cout << "\n  Commands:\n"
                          << "    exit          Exit the REPL\n"
                          << "    :help         Show this help\n"
                          << "    :reset        Clear all accumulated bindings\n"
                          << "\n";
                continue;
            }
            if (input == ":reset") {
                topDefs.clear(); localBinds.clear(); iteration = 0;
                std::cout << "  (bindings cleared)\n";
                continue;
            }

            // Accumulate multi-line blocks
            std::string source = input;
            int dc = countBlocksBeam(source);
            while (dc > 0) {
                auto [cont, contOk] = readLine("  ...> ");
                if (!contOk) break;
                source += "\n" + cont;
                dc += countBlocksBeam(cont);
            }

            // Classify: function def vs local let vs expression
            bool isFuncDef = false;
            bool isLocalLet = false;
            std::string letVarName;
            {
                size_t off = std::string::npos;
                if (source.rfind("let ", 0) == 0) off = 4;
                else if (source.rfind("foul ", 0) == 0) off = 5;
                if (off != std::string::npos) {
                    auto parenPos = source.find('(', off);
                    auto eqPos    = source.find('=', off);
                    bool hasParenBeforeEq = parenPos != std::string::npos &&
                                           (eqPos == std::string::npos || parenPos < eqPos);
                    if (hasParenBeforeEq) {
                        isFuncDef = true;
                    } else if (eqPos != std::string::npos) {
                        isLocalLet = true;
                        // Extract variable name: word after let/foul keyword
                        size_t i = off;
                        while (i < source.size() && (std::isalnum((unsigned char)source[i]) || source[i] == '_'))
                            i++;
                        letVarName = source.substr(off, i - off);
                    } else {
                        isFuncDef = true; // 0-arity function def
                    }
                }
            }
            // Also treat module/type/record/make as top-level
            if (source.rfind("module ", 0) == 0 || source.rfind("type ", 0) == 0 ||
                source.rfind("record ", 0) == 0 || source.rfind("make ", 0) == 0)
                isFuncDef = true;

            try {
                if (isFuncDef) {
                    // Validate by parsing; no need to run on BEAM for a definition.
                    std::string check = topDefs + source + "\n";
                    kex::Lexer lexer(check);
                    auto tokens = lexer.tokenizeAll();
                    kex::Parser parser(std::move(tokens));
                    parser.parseProgram(); // throws on syntax error

                    // Extract function name for the "=> defined" message
                    size_t off = source.rfind("foul ", 0) == 0 ? 5 : 4;
                    size_t i = off;
                    while (i < source.size() && (std::isalnum((unsigned char)source[i]) || source[i] == '_'))
                        i++;
                    std::string fname = source.substr(off, i - off);
                    std::cout << kex::color::apply(kex::color::gray) << "=> "
                              << kex::color::apply(kex::color::reset)
                              << "defined " << fname << "\n";
                    topDefs += source + "\n";
                } else {
                    // Expression or local let — compile and run on BEAM.
                    std::string kexSource;
                    if (isLocalLet) {
                        kexSource = topDefs + "main do\n" + localBinds
                                  + "  " + source + "\n"
                                  + "  IO.inspect(" + letVarName + ")\nend\n";
                    } else {
                        kexSource = topDefs + "main do\n" + localBinds
                                  + "  IO.inspect(" + source + ")\nend\n";
                    }

                    kex::Lexer lexer(kexSource);
                    auto tokens = lexer.tokenizeAll();
                    kex::Parser parser(std::move(tokens));
                    auto program = parser.parseProgram();
                    kex::codegen::CoreErlangEmitter emitter;
                    auto result = emitter.emitProgram(program, "irepl_" + std::to_string(iteration));

                    std::string corePath = beamDir + "/" + result.moduleName + ".core";
                    std::ofstream cf(corePath);
                    if (!cf) { std::cerr << "  error: cannot write " << corePath << "\n"; continue; }
                    cf << result.source;
                    cf.close();

                    std::string erlCmd = "erlc +from_core -pa " + beamDir +
                                         " -o " + beamDir + " " + corePath + " 2>&1";
                    int erlcRet = std::system(erlCmd.c_str());
                    std::filesystem::remove(corePath);
                    if (erlcRet != 0) { std::cerr << "  error: compilation failed\n"; continue; }

                    std::string runCmd = "erl -noshell -pa " + beamDir +
                                         (rtPaDir.empty() ? "" : " -pa " + rtPaDir) +
                                         " -eval '" + result.moduleName + ":main(), halt()'"
                                         " < /dev/null";
                    std::system(runCmd.c_str());
                    std::filesystem::remove(beamDir + "/" + result.moduleName + ".beam");
                    ++iteration;

                    if (isLocalLet)
                        localBinds += "  " + source + "\n";
                }
            } catch (const std::exception &e) {
                std::cerr << "  " << kex::color::apply(kex::color::red) << "error:"
                          << kex::color::apply(kex::color::reset) << " " << e.what() << "\n";
            }
        }

        std::filesystem::remove_all(beamDir);
        return 0;
    }

    if (mode == "repl")
    {
        std::cout << "kex 0.1.0 — interactive REPL\n";
        std::cout << "Type :help for available commands, exit to quit.\n\n";

#ifdef HAS_READLINE
        std::string historyFile;
        if (const char *home = std::getenv("HOME"))
        {
            std::filesystem::path histDir = std::filesystem::path(home) / ".config" / "kex";
            std::error_code ec;
            std::filesystem::create_directories(histDir, ec);
            historyFile = (histDir / "history").string();
            read_history(historyFile.c_str());
        }
#endif

        // SemanticDB for REPL: prelude loaded once, updated on each input.
        kex::semantic::SemanticDB replDb;
        loadPrelude(replDb);
#ifdef HAS_READLINE
        g_replDb = &replDb;
        rl_attempted_completion_function = kexCompletion;
        rl_completion_display_matches_hook = kexDisplayMatches;
        // Exclude '.' so "IO.pr<TAB>" is one token; exclude '"' so readline
        // never treats string literals as quoted words (which would cause it to
        // call a NULL dequoting function → segfault and add a stray close-quote).
        rl_completer_word_break_characters = (char *)" \t\n\\@$><=;|&{(";
        // No quote characters: our completions can contain '"' and we don't want
        // readline's quoting machinery to touch them.
        rl_completer_quote_characters = (char *)"";
#endif

        kex::interpreter::Evaluator evaluator;
        evaluator.setReplMode(true);
        std::string line;
        // Accumulated source of all top-level definitions typed in the REPL so
        // far, used to keep the SemanticDB index complete across multiple inputs.
        std::string replAccumSource;
        // Keep parsed programs alive so function closures can reference AST nodes
        std::vector<kex::ast::Program *> replPrograms;
        // A line read ahead while chaining function clauses that turned out
        // to belong to the next statement; replayed on the next iteration.
        std::optional<std::string> pendingLine;

        // If `s` looks like the start of a function clause definition
        // (`let name(...` or `foul name(...`), return the function name.
        auto clauseFuncName = [](const std::string &s) -> std::optional<std::string>
        {
            size_t offset;
            if (s.rfind("foul ", 0) == 0)
                offset = 5;
            else if (s.rfind("let ", 0) == 0)
                offset = 4;
            else
                return std::nullopt;

            size_t i = offset;
            while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_'))
                i++;
            if (i == offset)
                return std::nullopt;
            std::string name = s.substr(offset, i - offset);
            if (i < s.size() && s[i] == '?')
            {
                name += '?';
                i++;
            }
            if (i < s.size() && s[i] == '(')
                return name;
            return std::nullopt;
        };

        // REPL settings
        bool showTypes = true;
        bool showAst = false;
        bool showTokens = false;

        auto printReplHelp = [&]()
        {
            std::cout << "\n  Commands:\n"
                      << "    exit          Exit the REPL\n"
                      << "    :help         Show this help\n"
                      << "    :set <opt>    Enable a feature\n"
                      << "    :unset <opt>  Disable a feature\n"
                      << "    :complete <p> Show completions for prefix p\n"
                      << "\n"
                      << "  Options for :set / :unset:\n"
                      << "    types         Show type of each result\n"
                      << "    ast           Show AST for each input\n"
                      << "    tokens        Show token stream for each input\n"
                      << "\n";
        };

        auto handleSet = [&](const std::string &arg, bool enable)
        {
            if (arg == "types")
            {
                showTypes = enable;
                std::cout << "  types: " << (enable ? "on" : "off") << "\n";
            }
            else if (arg == "ast")
            {
                showAst = enable;
                std::cout << "  ast: " << (enable ? "on" : "off") << "\n";
            }
            else if (arg == "tokens")
            {
                showTokens = enable;
                std::cout << "  tokens: " << (enable ? "on" : "off") << "\n";
            }
            else
            {
                std::cerr << "  Unknown option: " << arg << "\n";
                std::cerr << "  Available: types, ast, tokens\n";
            }
        };

        while (true)
        {
            std::string input;
            bool ok;
            if (pendingLine)
            {
                input = *pendingLine;
                pendingLine.reset();
                ok = true;
            }
            else
            {
                auto result = readLine("kex> ");
                input = result.first;
                ok = result.second;
            }
            if (!ok)
                break;
            line = input;
            if (line == "exit" || line == ":exit" || line == ":quit" || line == ":q")
                break;
            if (line.empty())
                continue;

            // REPL commands
            if (line == ":help" || line == ":h")
            {
                printReplHelp();
                continue;
            }
            if (line == ":set")
            {
                std::cout << "  types:  " << (showTypes ? "on" : "off") << "\n"
                          << "  ast:    " << (showAst ? "on" : "off") << "\n"
                          << "  tokens: " << (showTokens ? "on" : "off") << "\n";
                continue;
            }
            if (line.substr(0, 5) == ":set ")
            {
                handleSet(line.substr(5), true);
                continue;
            }
            if (line.substr(0, 7) == ":unset ")
            {
                handleSet(line.substr(7), false);
                continue;
            }
            if (line.substr(0, 10) == ":complete ")
            {
                auto prefix = line.substr(10);
                auto results = replDb.completionsFor(prefix);
                if (results.empty())
                {
                    std::cout << "  (no completions for \"" << prefix << "\")\n";
                }
                else
                {
                    for (const auto &c : results)
                        std::cout << "  " << c << "\n";
                }
                continue;
            }
            // If it starts with : but isn't a known command, treat as atom expression
            // (known commands already handled above)

            // Multi-line: accumulate if there are unmatched do/end blocks
            std::string source = line;

            // Returns true if `s` starts with `make <TypeName>` but has no
            // `do` keyword on the same line (implicit-do block opener).
            auto isMakeWithoutDo = [](const std::string &s) -> bool
            {
                if (s.rfind("make ", 0) != 0)
                    return false;
                // Check that the rest is a type name (no explicit `do`)
                auto doPos = s.find(" do");
                return doPos == std::string::npos;
            };

            auto countBlocks = [](const std::string &s) -> int
            {
                int count = 0;
                for (size_t i = 0; i < s.size(); i++)
                {
                    if (i + 2 <= s.size() && s.substr(i, 2) == "do")
                    {
                        bool wordBefore = (i > 0 && std::isalnum(s[i - 1]));
                        bool wordAfter = (i + 2 < s.size() && std::isalnum(s[i + 2]));
                        if (!wordBefore && !wordAfter)
                            count++;
                    }
                    if (i + 3 <= s.size() && s.substr(i, 3) == "end")
                    {
                        bool wordBefore = (i > 0 && std::isalnum(s[i - 1]));
                        bool wordAfter = (i + 3 < s.size() && std::isalnum(s[i + 3]));
                        if (!wordBefore && !wordAfter)
                            count--;
                    }
                }
                return count;
            };

            // `make TypeName` (no explicit `do`) implicitly opens a block.
            bool implicitDo = isMakeWithoutDo(source);
            int doCount = implicitDo ? 1 : countBlocks(source);

            // Track which make block we're inside so the completer can infer
            // parameter types from `@[x|xs]` patterns, etc.
            if (doCount > 0 && source.rfind("make ", 0) == 0)
            {
                std::string rest = source.substr(5);
                auto sp = rest.find_first_of(" \n\t");
                g_currentMakeTarget = (sp != std::string::npos) ? rest.substr(0, sp) : rest;
            }
            while (doCount > 0)
            {
                auto [contLine, contOk] = readLine("...> ");
                if (!contOk)
                    break;
                line = contLine;
                source += "\n" + line;
                doCount += countBlocks(line);
            }

            // If this line starts a function clause definition, keep reading
            // additional clauses for the *same* function so pattern-matching
            // definitions like `let fact(1) = 1` / `let fact(n) = ...` are
            // combined into one function. The first line that isn't another
            // clause of the same function is replayed on the next iteration.
            if (doCount == 0)
            {
                if (auto name = clauseFuncName(source))
                {
                    while (true)
                    {
                        auto [contLine, contOk] = readLine("...> ");
                        if (!contOk)
                            break;
                        auto nextName = clauseFuncName(contLine);
                        if (nextName && *nextName == *name)
                        {
                            source += "\n" + contLine;
                            int extra = countBlocks(contLine);
                            while (extra > 0)
                            {
                                auto [contLine2, contOk2] = readLine("...> ");
                                if (!contOk2)
                                    break;
                                source += "\n" + contLine2;
                                extra += countBlocks(contLine2);
                            }
                        }
                        else
                        {
                            if (!contLine.empty())
                                pendingLine = contLine;
                            break;
                        }
                    }
                }
            }

            // Show tokens if enabled
            if (showTokens)
            {
                kex::Lexer debugLexer(source);
                auto debugTokens = debugLexer.tokenizeAll();
                std::cout << "  tokens: ";
                for (const auto &t : debugTokens)
                {
                    if (t.type == kex::TokenType::Eof || t.type == kex::TokenType::Newline)
                        continue;
                    std::cout << kex::tokenTypeName(t.type);
                    if (!t.value.empty())
                        std::cout << "[" << t.value << "]";
                    std::cout << " ";
                }
                std::cout << "\n";
            }

            auto execProgram = [&](kex::ast::Program *program) -> kex::interpreter::ValuePtr
            {
                if (showAst)
                    printAst(*program);
                return evaluator.execute(*program);
            };

            auto showResult = [&](const kex::interpreter::ValuePtr &result)
            {
                if (result && !std::holds_alternative<kex::interpreter::UnitValue>(result->data))
                {
                    std::cout << kex::color::apply(kex::color::gray) << "=> "
                              << kex::color::apply(kex::color::reset) << result->inspect();
                    if (showTypes)
                    {
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
            if (source.substr(0, 4) == "let ")
                defOffset = 4;
            else if (source.substr(0, 5) == "foul ")
                defOffset = 5;
            if (defOffset != std::string::npos)
            {
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
                source.substr(0, 12) == "foul module ")
            {
                isFuncDef = true;
            }

            try
            {
                if (isFuncDef)
                {
                    // If the user wrote `make TypeName` without `do`, insert it
                    // before parsing so the grammar's `expect(Do)` is satisfied.
                    if (implicitDo)
                    {
                        auto nl = source.find('\n');
                        if (nl != std::string::npos)
                            source.insert(nl, " do");
                        else
                            source += " do";
                    }
                    // Parse as top-level definition
                    kex::Lexer lexer(source);
                    auto tokens = lexer.tokenizeAll();
                    kex::Parser parser(std::move(tokens));
                    auto *program = new kex::ast::Program(parser.parseProgram());
                    replPrograms.push_back(program);
                    execProgram(program);
                    // Accumulate and re-index so all prior definitions stay
                    // visible for tab completion, not just the latest one.
                    replAccumSource += source + "\n";
                    replDb.updateFile("<repl>", replAccumSource);
                    g_currentMakeTarget.clear(); // block is complete
                }
                else
                {
                    // Wrap in main for expression evaluation
                    auto wrapped = "main do\n" + source + "\nend\n";
                    kex::Lexer lexer(wrapped);
                    auto tokens = lexer.tokenizeAll();
                    kex::Parser parser(std::move(tokens));
                    auto *program = new kex::ast::Program(parser.parseProgram());
                    replPrograms.push_back(program);
                    auto result = execProgram(program);
                    showResult(result);
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "  " << kex::color::apply(kex::color::red) << "error:"
                          << kex::color::apply(kex::color::reset) << " " << e.what() << "\n";
            }
        }
#ifdef HAS_READLINE
        if (!historyFile.empty())
            write_history(historyFile.c_str());
#endif
        return 0;
    }

    std::string filepath = argv[optind];

    // `kex file.kx.beam [args]` or `kex file.beam [args]` — run a compiled BEAM module.
    if (filepath.size() > 5 &&
        filepath.compare(filepath.size() - 5, 5, ".beam") == 0)
    {
        std::vector<std::string> beamArgs;
        for (int i = optind + 1; i < argc; i++)
            beamArgs.push_back(argv[i]);

        namespace fs = std::filesystem;
        std::string absBeamPath = fs::weakly_canonical(filepath).string();
        std::string absBeamDir  = fs::path(absBeamPath).parent_path().string();
        std::string modFile     = fs::path(absBeamPath).filename().string();

        // Derive module name from filename convention:
        //   <stem>.kx.beam → kex_<stem>   (our compiled output)
        //   kex_<x>.beam   → kex_<x>      (backward compat)
        //   anything.beam  → anything      (external module)
        std::string moduleName;
        if (modFile.size() > 8 &&
            modFile.compare(modFile.size() - 8, 8, ".kx.beam") == 0) {
            moduleName = "kex_" + modFile.substr(0, modFile.size() - 8);
        } else {
            moduleName = modFile.substr(0, modFile.size() - 5);
        }

        // Put Kex runtime beams (kex_io, kex_file, ...) on the code path.
        // Prefer cmake-pre-built beams (zero compile cost); fall back to
        // compiling them into a temp dir we clean up afterwards.
        std::string rtBeamDir = prebuiltRuntimeBeamDir();
        bool rtTemp = false;
        if (rtBeamDir.empty())
        {
            rtBeamDir = compileRuntimeBeamsToTemp(argv[0]);
            rtTemp = !rtBeamDir.empty();
        }

        // Load explicitly — code:load_abs rejects when filename != module name,
        // so use code:load_binary which skips that check.
        std::string mainCall =
            "{ok,_Bin}=file:read_file(\"" + absBeamPath + "\"), "
            "code:load_binary('" + moduleName + "',\"" + absBeamPath + "\",_Bin), "
            "case lists:member({main,1},erlang:get_module_info('" + moduleName + "',exports)) of "
            "true -> " + moduleName + ":main(init:get_plain_arguments()); "
            "false -> " + moduleName + ":main() end, halt()";
        std::string runCmd = "erl -noshell -pa " + absBeamDir;
        if (!rtBeamDir.empty()) runCmd += " -pa " + rtBeamDir;
        runCmd += " -eval '" + mainCall + "'";
        if (!beamArgs.empty()) {
            runCmd += " -extra";
            for (const auto& a : beamArgs) runCmd += " " + a;
        }
        int rc = std::system(runCmd.c_str());
        if (rtTemp) fs::remove_all(rtBeamDir);
        return rc;
    }

    // Reject non-.kex files before trying to parse them.
    if (filepath.size() < 4 ||
        filepath.compare(filepath.size() - 4, 4, ".kex") != 0)
    {
        std::cerr << "error: " << filepath
                  << ": expected a .kex source file\n";
        return 1;
    }

    auto source = readFile(filepath);
    if (source.empty())
        return 1;

    // Honour `# kex: no-check` pragma in the first few lines — any file that
    // contains it is treated as if --no-check was passed on the command line.
    {
        std::istringstream ss(source);
        std::string line;
        for (int ln = 0; ln < 10 && std::getline(ss, line); ++ln)
        {
            if (line.find("# kex: no-check") != std::string::npos)
            {
                skipCheck = true;
                break;
            }
        }
    }

    // Everything after the script path is the script's own argument list,
    // exposed to Kex code via `main(args) do ... end`.
    std::vector<std::string> scriptArgs;
    for (int i = optind + 1; i < argc; i++)
    {
        scriptArgs.push_back(argv[i]);
    }

    kex::Lexer lexer(std::move(source), filepath);
    auto tokens = lexer.tokenizeAll();

    if (mode == "lex")
    {
        for (const auto &token : tokens)
        {
            std::cout << token.location.line << ":" << token.location.column
                      << "  " << kex::tokenTypeName(token.type);
            if (!token.value.empty())
            {
                std::cout << "  [" << token.value << "]";
            }
            std::cout << "\n";
        }
        return 0;
    }

    try
    {
        kex::Parser parser(std::move(tokens), filepath);
        auto program = parser.parseProgram();

        // In --run mode, print parse errors and abort — SemanticDB is not
        // invoked there so we're the only place that can report them.
        // In --check mode the SemanticDB re-parses and reports them itself;
        // printing here would duplicate every message.
        if (!parser.diagnostics().empty() && mode == "run")
        {
            for (const auto &pd : parser.diagnostics())
            {
                std::cerr << kex::color::apply(kex::color::gray)
                          << pd.location.file << ":" << pd.location.line << ":"
                          << pd.location.column << ":" << kex::color::apply(kex::color::reset)
                          << " " << kex::color::apply(kex::color::bold)
                          << kex::color::apply(kex::color::red) << "error:"
                          << kex::color::apply(kex::color::reset) << " "
                          << colorizeMessage(pd.message) << "\n";
            }
            std::cerr << kex::color::apply(kex::color::bold)
                      << kex::color::apply(kex::color::magenta)
                      << "Aborted:" << kex::color::apply(kex::color::reset)
                      << " fix syntax errors before running.\n";
            return 1;
        }

        if (mode == "parse")
        {
            printAst(program);
            return 0;
        }

        if (mode == "compile" && !skipCheck)
        {
            // Same pre-execution check `run` mode does — see
            // runSemanticCheck's doc comment for why this matters: without
            // it, an error here only ever surfaced as a raw erlc failure.
            // Not applied to emit-core (a debug/inspection dump — you may
            // want to see the emitted Core Erlang for code that doesn't
            // type-check yet).
            if (!runSemanticCheck(program, filepath))
            {
                std::cerr << kex::color::apply(kex::color::bold)
                          << kex::color::apply(kex::color::magenta)
                          << "Aborted:" << kex::color::apply(kex::color::reset)
                          << " fix type errors before compiling (use --no-check to skip).\n";
                return 1;
            }
        }

        if (mode == "compile" || mode == "emit-core")
        {
            // For `-R file.kex` without explicit `-o`, use a temp dir and clean up after.
            std::string tempDir;
            if (compileRun && !outputDirExplicit)
            {
                char tmpl[] = "/tmp/kex_XXXXXX";
                char *td = mkdtemp(tmpl);
                if (!td)
                {
                    std::cerr << "error: mkdtemp failed\n";
                    return 1;
                }
                tempDir = td;
                outputDir = tempDir;
            }

            // Derive module stem from filename (e.g. "hello" from "hello.kex")
            std::string stem = filepath;
            auto slash = stem.rfind('/');
            if (slash != std::string::npos)
                stem = stem.substr(slash + 1);
            auto dot = stem.rfind('.');
            if (dot != std::string::npos)
                stem = stem.substr(0, dot);

            // Require an explicit `main do ... end` block for codegen.
            bool hasMain = false;
            for (const auto &item : program.items)
            {
                std::visit([&](const auto &node)
                           {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<kex::ast::MainBlock>>) {
                        if (node && node->isExplicitMain) hasMain = true;
                    } }, item);
            }
            if (!hasMain)
            {
                std::cerr << "error: " << filepath
                          << ": no 'main do ... end' block found.\n"
                          << "  Bare top-level expressions are not allowed in --emit-core mode.\n"
                          << "  Wrap your entry point in: main do\n"
                          << "                              ...\n"
                          << "                            end\n";
                return 1;
            }

            kex::codegen::CoreErlangEmitter emitter;
            auto result = emitter.emitProgram(program, stem);

            std::string outPath = outputDir + "/" + result.moduleName + ".core";
            std::ofstream outFile(outPath);
            if (!outFile)
            {
                std::cerr << "error: cannot write " << outPath << "\n";
                return 1;
            }
            outFile << result.source;
            outFile.close();
            if (mode == "emit-core")
            {
                std::cerr << "wrote " << outPath << "\n";
                return 0;
            }

            // --compile: also invoke erlc to produce a .beam file.
            // Place Kex runtime beams (kex_io, kex_file, ...) into the output
            // dir so the compiled .beam is self-contained. Prefer copying the
            // cmake-pre-built beams; fall back to compiling from source.
            {
                namespace fs = std::filesystem;
                std::string prebuilt = prebuiltRuntimeBeamDir();
                if (!prebuilt.empty())
                {
                    std::error_code ec;
                    for (const auto& e : fs::directory_iterator(prebuilt))
                        if (e.path().extension() == ".beam")
                            fs::copy_file(e.path(),
                                          fs::path{outputDir} / e.path().filename(),
                                          fs::copy_options::overwrite_existing, ec);
                }
                else
                {
                    fs::path bin = fs::weakly_canonical(argv[0]);
                    for (auto dir = bin.parent_path(); dir != dir.root_path(); dir = dir.parent_path())
                    {
                        auto rtDir = dir / "runtime" / "src";
                        if (fs::exists(rtDir))
                        {
                            for (const auto& entry : fs::directory_iterator(rtDir))
                            {
                                if (entry.path().extension() == ".erl")
                                {
                                    std::string rtCmd = "erlc -o " + outputDir + " "
                                                      + entry.path().string() + " > /dev/null 2>&1";
                                    std::system(rtCmd.c_str());
                                }
                            }
                            break;
                        }
                    }
                }
            }

            std::string coreCmd = "erlc +from_core -pa " + outputDir +
                                  " -o " + outputDir + " " + outPath;
            if (!tempDir.empty())
            {
                // Suppress erlc noise in temp-dir (interpreter) mode; redirect stderr.
                coreCmd += " 2>&1";
            }
            else
            {
                std::cerr << "  erlc  " << result.moduleName << "\n";
            }
            int erlcRet = std::system(coreCmd.c_str());
            if (erlcRet != 0)
            {
                std::cerr << "error: erlc failed\n";
                if (!tempDir.empty())
                    std::filesystem::remove_all(tempDir);
                return 1;
            }
            // Rename kex_<stem>.beam → <stem>.kx.beam (user-facing name).
            // The internal Erlang module name stays kex_<stem> inside the file.
            std::string internalBeam = outputDir + "/" + result.moduleName + ".beam";
            std::string kxBeam = outputDir + "/" + stem + ".kx.beam";
            if (tempDir.empty()) {
                std::filesystem::rename(internalBeam, kxBeam);
                std::cerr << "  done  " << kxBeam << "\n";
            } else {
                // In temp-dir (interpreter/REPL) mode keep the internal name so
                // -pa <tempDir> auto-loads it by module name.
                kxBeam = internalBeam;
            }

            if (compileRun)
            {
                // Use code:load_binary for explicit load (filename != module name).
                namespace fs = std::filesystem;
                std::string absBeam = fs::weakly_canonical(kxBeam).string();
                std::string loadExpr =
                    "{ok,_B}=file:read_file(\"" + absBeam + "\"), "
                    "code:load_binary('" + result.moduleName + "',\"" + absBeam + "\",_B), ";
                std::string mainCall = result.mainArity == 1
                    ? loadExpr + result.moduleName + ":main(init:get_plain_arguments()), halt()"
                    : loadExpr + result.moduleName + ":main(), halt()";
                std::string runCmd = "erl -noshell -pa " + outputDir +
                                     " -eval '" + mainCall + "'";
                if (result.mainArity == 1 && !scriptArgs.empty()) {
                    runCmd += " -extra";
                    for (const auto& a : scriptArgs)
                        runCmd += " " + a;
                }
                int ret = std::system(runCmd.c_str());
                if (!tempDir.empty())
                    std::filesystem::remove_all(tempDir);
                return ret;
            }
            return 0;
        }

        if (mode == "run" && !skipCheck)
        {
            if (!runSemanticCheck(program, filepath))
            {
                std::cerr << kex::color::apply(kex::color::bold)
                          << kex::color::apply(kex::color::magenta)
                          << "Aborted:" << kex::color::apply(kex::color::reset)
                          << " fix type errors before running (use --no-check to skip).\n";
                return 1;
            }
        }

        if (mode == "run")
        {
            kex::interpreter::Evaluator evaluator;
            evaluator.setArgs(scriptArgs);

            // Must outlive `evaluator.execute(program)` below: the
            // evaluator keeps raw `const ast::FunctionDef*` pointers into
            // whatever Program owns these nodes (see m_functionDefs), so a
            // Program that goes out of scope before the evaluator is done
            // leaves those pointers dangling — declaring it here, not
            // inside the loop, keeps it alive for the rest of this block.
            kex::ast::Program declarationsOnly;
            for (const auto &candidate : specBaseCandidates(filepath))
            {
                if (!fileExists(candidate))
                    continue;

                auto baseSource = readFile(candidate);
                kex::Lexer baseLexer(std::move(baseSource), candidate);
                auto baseTokens = baseLexer.tokenizeAll();
                kex::Parser baseParser(std::move(baseTokens), candidate);
                auto baseProgram = baseParser.parseProgram();

                declarationsOnly.items.reserve(baseProgram.items.size());
                for (auto &item : baseProgram.items)
                {
                    if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(item))
                    {
                        declarationsOnly.items.push_back(std::move(item));
                    }
                }
                evaluator.execute(declarationsOnly);
                break;
            }

            auto result = evaluator.execute(program);
            if (auto *i = std::get_if<kex::interpreter::IntValue>(&result->data))
                return static_cast<int>(i->value);
            return 0;
        }

        // mode == "check"
        // Pass 1+2: collect symbols and resolve names via SemanticDB
        kex::semantic::SemanticDB db;
        loadPrelude(db);
        db.updateFile(filepath, readFile(filepath));

        // Pass 3+: existing Analyzer (purity, type checking)
        kex::semantic::Analyzer analyzer;
        bool ok = analyzer.analyze(program);

        // Collect all diagnostics
        std::vector<kex::semantic::Diagnostic> allDiags;
        bool dbOk = true;
        for (const auto &d : db.diagnosticsFor(filepath))
        {
            if (d.level == kex::semantic::Diagnostic::Level::Error)
                dbOk = false;
            allDiags.push_back(d);
        }
        for (const auto &d : analyzer.diagnostics())
            allDiags.push_back(d);

        bool allOk = ok && dbOk;

        if (jsonOutput)
        {
            // Machine-readable JSON — one object per diagnostic
            std::cout << "[\n";
            for (size_t i = 0; i < allDiags.size(); i++)
            {
                const auto &d = allDiags[i];
                bool isErr = d.level == kex::semantic::Diagnostic::Level::Error;
                std::string hint = extractHint(d.message);
                std::cout << "  {\n"
                          << "    \"file\": \"" << jsonEscape(std::string(d.location.file)) << "\",\n"
                          << "    \"line\": " << d.location.line << ",\n"
                          << "    \"column\": " << d.location.column << ",\n"
                          << "    \"severity\": \"" << (isErr ? "error" : "warning") << "\",\n"
                          << "    \"message\": \"" << jsonEscape(d.message) << "\"";
                if (!hint.empty())
                    std::cout << ",\n    \"hint\": \"" << jsonEscape(hint) << "\"";
                std::cout << "\n  }" << (i + 1 < allDiags.size() ? "," : "") << "\n";
            }
            std::cout << "]\n";
            return allOk ? 0 : 1;
        }

        if (summaryMode)
        {
            // Output the public API signatures of the file in Kex syntax.
            // Walk the AST for TypeAnnotation + FunctionDef nodes; group by module.
            std::string currentModule;
            bool inModule = false;
            auto closeModule = [&]()
            {
                if (inModule)
                {
                    std::cout << "end\n";
                    inModule = false;
                }
            };
            auto openModule = [&](const std::string &name, bool isFoul)
            {
                closeModule();
                std::cout << (isFoul ? "foul " : "") << "module " << name << " do\n";
                inModule = true;
                currentModule = name;
            };

            for (const auto &item : program.items)
            {
                std::visit([&](const auto &ptr)
                           {
                    using T = std::decay_t<decltype(*ptr)>;
                    if constexpr (std::is_same_v<T, kex::ast::TypeAnnotation>) {
                        if (inModule) std::cout << "  ";
                        std::cout << (ptr->isFoul ? "foul " : "")
                                  << ptr->name << " : ";
                        if (ptr->type) std::cout << typeExprToString(*ptr->type);
                        std::cout << "\n";
                    } else if constexpr (std::is_same_v<T, kex::ast::ModuleDef>) {
                        openModule(ptr->name, ptr->isFoul);
                        for (const auto& mitem : ptr->body) {
                            std::visit([&](const auto& mptr) {
                                using MT = std::decay_t<decltype(*mptr)>;
                                if constexpr (std::is_same_v<MT, kex::ast::TypeAnnotation>) {
                                    std::cout << "  " << (mptr->isFoul ? "foul " : "")
                                              << mptr->name << " : ";
                                    if (mptr->type) std::cout << typeExprToString(*mptr->type);
                                    std::cout << "\n";
                                }
                            }, mitem);
                        }
                        closeModule();
                    } }, item);
            }
            closeModule();
            return allOk ? 0 : 1;
        }

        // Normal colored output
        auto printDiag = [&](const kex::semantic::Diagnostic &diag)
        {
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
        for (const auto &d : allDiags)
            printDiag(d);

        if (dumpTypes)
        {
            // Collect and sort by source location so output is readable top-to-bottom.
            const auto &tmap = analyzer.typeMap();
            std::vector<std::pair<const kex::ast::Expr *, kex::semantic::TypePtr>> entries(
                tmap.begin(), tmap.end());
            std::sort(entries.begin(), entries.end(),
                      [](const auto &a, const auto &b)
                      {
                          const auto &la = a.first->location;
                          const auto &lb = b.first->location;
                          if (la.line != lb.line)
                              return la.line < lb.line;
                          return la.column < lb.column;
                      });
            for (const auto &[expr, type] : entries)
            {
                if (!type)
                    continue;
                std::cout << expr->location.line << ":" << expr->location.column
                          << "  " << kex::semantic::typeToString(type) << "\n";
            }
        }

        if (allOk && !dumpTypes)
        {
            std::cout << "No errors found.\n";
        }
        else if (allOk)
        {
            std::cerr << "No errors found.\n";
        }

        return allOk ? 0 : 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << kex::color::apply(kex::color::bold) << kex::color::apply(kex::color::red)
                  << "Internal error:" << kex::color::apply(kex::color::reset)
                  << " " << e.what() << "\n";
        return 1;
    }

    return 0;
}
