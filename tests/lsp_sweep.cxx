// lsp_sweep — finds language-server bugs by holding the server's answers to
// the checker's.
//
// The server answers hover, completion, definition and diagnostics from the
// same analysis `kex -C` runs, so on any program the two must agree. Every
// corpus file is a free test case: for each identifier in it the checker
// already knows the type (`kex -C -t`), the name the author actually used after
// a dot, and which names are in scope. The sweep asks the server about every
// such position in one `kex --lsp` session per file and reports where its
// answer contradicts the checker:
//
//   hover       the type shown for an identifier is not one the checker
//               inferred at that position
//   recall      at `recv.na|`, completion does not offer `name` — the name the
//               program really calls there
//   soundness   a sample of what completion offers at `recv.|` (or at the start
//               of a bare name) is substituted into the program and checked:
//               a name the checker then calls unknown, out of scope, or not
//               applicable to the receiver should never have been offered
//   diagnostics the errors published for the file are not the ones `kex -C`
//               reports
//   definition  go-to-definition lands somewhere that does not spell the name
//   crash       the session died, hung, or answered a request with an error
//
//   lsp_sweep --kex build/kex --out /tmp/sweep [--checks hover,recall,...]
//             [--sample N] [--seed S] [--timeout SECONDS] FILE...
//
// Every finding is one line, `check file:line:col: detail`, so the output
// sorts and diffs; the exit status is 1 when anything was found. Positions in
// the report are 1-based, as the compiler prints them.

#include <lsp/json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace json = lsp::json;

namespace {

struct Options {
    std::string kex = "./build/kex";
    std::string outDir = "/tmp/kex-lsp-sweep";
    std::set<std::string> checks{"hover", "recall", "soundness", "diagnostics",
                                 "definition", "crash"};
    std::size_t sample = 3;
    std::size_t maxSitesPerFile = 40;
    unsigned seed = 1;
    int timeoutSeconds = 120;
    std::vector<std::string> files;
};

// One identifier occurrence in code (not in a comment or string), 0-based.
struct Token {
    int line = 0;
    int column = 0;
    std::string text;
    bool afterDot = false;  // `recv.text`, where recv is an expression
};

struct Finding {
    std::string check;
    std::string file;
    int line = 0;    // 1-based
    int column = 0;  // 1-based
    std::string detail;
};

std::vector<Finding> findings;

auto report(const std::string& check, const std::string& file, int line,
            int column, const std::string& detail) -> void {
    findings.push_back({check, file, line + 1, column + 1, detail});
}

auto readFile(const std::string& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

auto writeFile(const std::string& path, const std::string& text) -> void {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

auto shellQuote(const std::string& text) -> std::string {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

// Runs a shell command and answers its stdout (stderr merged when asked).
auto capture(const std::string& command) -> std::string {
    std::string out;
    if (FILE* pipe = popen(command.c_str(), "r")) {
        char buffer[65536];
        std::size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0)
            out.append(buffer, n);
        pclose(pipe);
    }
    return out;
}

auto splitLines(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    lines.push_back(current);
    return lines;
}

auto isIdentStart(char c) -> bool {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

auto isIdentChar(char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

auto asciiOnly(const std::string& line) -> bool {
    return std::all_of(line.begin(), line.end(),
                       [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// The identifiers of `text` outside comments and string literals. Crude on
// purpose — interpolations are skipped with their string — but it never
// reports a position inside a literal, which is what matters: every probe
// lands on real code. Lines with non-ASCII bytes are left out, since the
// server counts UTF-16 units and this counts bytes.
auto identifiers(const std::string& text) -> std::vector<Token> {
    std::vector<Token> tokens;
    const auto lines = splitLines(text);
    bool inHeredoc = false;
    for (int ln = 0; ln < static_cast<int>(lines.size()); ++ln) {
        const auto& line = lines[ln];
        if (line.find("\"\"\"") != std::string::npos) {
            inHeredoc = !inHeredoc;
            continue;
        }
        if (inHeredoc || !asciiOnly(line)) continue;
        std::size_t i = 0;
        while (i < line.size()) {
            const char c = line[i];
            if (c == '#') break;
            if (c == '"' || c == '`' || c == '\'') {
                const char quote = c;
                ++i;
                while (i < line.size() && line[i] != quote) {
                    if (line[i] == '\\') ++i;
                    ++i;
                }
                ++i;
                continue;
            }
            if (c == ':' && i + 1 < line.size() && isIdentStart(line[i + 1]) &&
                (i == 0 || !isIdentChar(line[i - 1]))) {
                // An atom, `:name`: not an identifier.
                i += 2;
                while (i < line.size() && isIdentChar(line[i])) ++i;
                continue;
            }
            if (isIdentStart(c) && (i == 0 || !isIdentChar(line[i - 1]))) {
                std::size_t end = i;
                while (end < line.size() && isIdentChar(line[end])) ++end;
                if (end < line.size() && (line[end] == '?' || line[end] == '!'))
                    ++end;
                Token token{ln, static_cast<int>(i), line.substr(i, end - i)};
                if (i >= 2 && line[i - 1] == '.' &&
                    (isIdentChar(line[i - 2]) || line[i - 2] == ')' ||
                     line[i - 2] == ']' || line[i - 2] == '}'))
                    token.afterDot = true;
                tokens.push_back(std::move(token));
                i = end;
                continue;
            }
            ++i;
        }
    }
    return tokens;
}

auto isKeyword(const std::string& word) -> bool {
    static const std::set<std::string> keywords{
        "let", "var", "do", "end", "if", "else", "elsif", "unless", "then",
        "match", "when", "case", "for", "in", "while", "return", "using",
        "make", "trait", "type", "record", "true", "false", "and", "or",
        "not", "foul", "main", "receive", "after", "compiled", "rescue",
        "serving", "slot", "break", "next", "this", "module", "is", "as",
        "where", "with", "test", "describe", "it", "expect", "spec", "self"};
    return keywords.count(word) > 0;
}

// A type with its variables renamed by order of appearance: `-t` prints an
// inference variable as `T4`, a hover as `A`, and `Process<T3>` is
// `Process<A>`. A variable is `T` and digits, or a lone capital letter.
auto withVariablesRenamed(const std::string& type) -> std::string {
    std::map<std::string, std::string> names;
    std::string out;
    std::size_t i = 0;
    while (i < type.size()) {
        if (!isIdentStart(type[i]) || (i > 0 && (isIdentChar(type[i - 1]) || type[i - 1] == '.'))) {
            out += type[i++];
            continue;
        }
        std::size_t end = i;
        while (end < type.size() && isIdentChar(type[end])) ++end;
        const auto word = type.substr(i, end - i);
        const bool variable =
            (word.size() == 1 && std::isupper(static_cast<unsigned char>(word[0]))) ||
            (word.size() > 1 && word[0] == 'T' &&
             std::all_of(word.begin() + 1, word.end(),
                         [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }));
        if (variable) {
            auto [found, added] = names.try_emplace(word, "");
            if (added) found->second = "#" + std::to_string(names.size());
            out += found->second;
        } else {
            // The checker's `Unknown` is displayed as `Any`.
            out += word == "Unknown" ? "Any" : word;
        }
        i = end;
    }
    return out;
}

// `kex -C -t`: the types the checker inferred, by 1-based line and column.
struct CheckerView {
    std::map<std::pair<int, int>, std::vector<std::string>> types;
    std::set<std::string> errors;  // "line:col: message"
};

auto checkerView(const Options& options, const std::string& path,
                 const std::string& sourceRoot) -> CheckerView {
    CheckerView view;
    const auto output = capture(
        "timeout " + std::to_string(options.timeoutSeconds) + " " +
        shellQuote(options.kex) + " -C -t --no-colors --source-root " +
        shellQuote(sourceRoot) + " " + shellQuote(path) + " 2>&1");
    for (const auto& line : splitLines(output)) {
        int ln = 0, col = 0, used = 0;
        if (std::sscanf(line.c_str(), "%d:%d %n", &ln, &col, &used) == 2 &&
            used > 0 && line.find(": error") == std::string::npos) {
            auto type = line.substr(used);
            while (!type.empty() && type.front() == ' ') type.erase(type.begin());
            view.types[{ln, col}].push_back(type);
            continue;
        }
        const auto marker = path + ":";
        if (line.rfind(marker, 0) == 0) {
            const auto rest = line.substr(marker.size());
            if (const auto at = rest.find(": error: "); at != std::string::npos)
                view.errors.insert(rest.substr(0, at) + ": " + rest.substr(at + 9));
        }
    }
    return view;
}

auto frame(const std::string& body) -> std::string {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

auto jsonString(const std::string& text) -> std::string {
    return json::stringify(json::Value(std::string(text)));
}

// One `kex --lsp` session: the requests, then every message it wrote back.
struct Session {
    std::string uri;
    std::string requests;
    int nextId = 100;
    std::map<int, std::string> methodOf;
    std::map<int, Token> tokenOf;
    std::map<int, std::string> purposeOf;

    auto ask(const std::string& method, int line, int character,
             const Token& token, const std::string& purpose) -> int {
        const int id = nextId++;
        requests += frame(R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                          R"(,"method":"textDocument/)" + method +
                          R"(","params":{"textDocument":{"uri":)" + jsonString(uri) +
                          R"(},"position":{"line":)" + std::to_string(line) +
                          R"(,"character":)" + std::to_string(character) + "}}}");
        methodOf[id] = method;
        tokenOf[id] = token;
        purposeOf[id] = purpose;
        return id;
    }
};

struct SessionResult {
    bool completed = false;
    std::map<int, json::Value> results;
    std::map<int, std::string> errors;
    std::vector<json::Value> diagnostics;  // the last publish for the document
    std::string stderrTail;
};

auto runSession(const Options& options, Session& session,
                const std::string& text, const std::string& workDir)
    -> SessionResult {
    std::string input;
    input += frame(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
    input += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    input += frame(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":)" +
                   jsonString(session.uri) +
                   R"(,"languageId":"kex","version":1,"text":)" + jsonString(text) + "}}}");
    input += session.requests;
    input += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
    input += frame(R"({"jsonrpc":"2.0","method":"exit"})");
    const auto requestPath = workDir + "/requests.lsp";
    const auto errorPath = workDir + "/stderr.txt";
    writeFile(requestPath, input);
    const auto output = capture(
        "timeout " + std::to_string(options.timeoutSeconds) + " " +
        shellQuote(options.kex) + " --lsp < " + shellQuote(requestPath) +
        " 2> " + shellQuote(errorPath));

    SessionResult result;
    const auto errors = readFile(errorPath);
    result.stderrTail = errors.size() > 600 ? errors.substr(errors.size() - 600) : errors;
    std::size_t cursor = 0;
    while (true) {
        const auto header = output.find("Content-Length:", cursor);
        if (header == std::string::npos) break;
        const auto length = std::strtoul(output.c_str() + header + 15, nullptr, 10);
        const auto body = output.find("\r\n\r\n", header);
        if (body == std::string::npos) break;
        const auto message = output.substr(body + 4, length);
        cursor = body + 4 + length;
        json::Value value;
        try {
            value = json::parse(message);
        } catch (const std::exception&) {
            continue;
        }
        if (!value.isObject()) continue;
        const auto& object = value.object();
        if (const auto* method = object.find("method");
            method && method->isString() &&
            method->string() == "textDocument/publishDiagnostics") {
            const auto& params = object.get("params").object();
            if (params.get("uri").string() == session.uri)
                result.diagnostics = params.get("diagnostics").array();
            continue;
        }
        const auto* id = object.find("id");
        if (!id || !id->isInteger()) continue;
        if (id->integer() == 2) result.completed = true;
        if (const auto* error = object.find("error"))
            result.errors[id->integer()] = json::stringify(*error);
        else if (const auto* answer = object.find("result"))
            result.results[id->integer()] = *answer;
    }
    return result;
}

// The text a hover shows, with markdown fences removed.
auto hoverText(const json::Value& hover) -> std::string {
    if (!hover.isObject()) return {};
    const auto* contents = hover.object().find("contents");
    if (!contents) return {};
    std::string text;
    if (contents->isString()) text = contents->string();
    else if (contents->isObject())
        if (const auto* value = contents->object().find("value");
            value && value->isString())
            text = value->string();
    return text;
}

// The type a hover gives a name: the text after `name: ` on its first code
// line, or empty when the hover is not of that form.
auto hoverType(const std::string& hover, const std::string& name) -> std::optional<std::string> {
    for (const auto& line : splitLines(hover)) {
        if (line.rfind("```", 0) == 0 || line.empty()) continue;
        for (const std::string& prefix : {"let " + name + ": ", "var " + name + ": ",
                                         name + ": ", name + " : "})
            if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
        return std::nullopt;
    }
    return std::nullopt;
}

auto completionLabels(const json::Value& answer) -> std::vector<std::string> {
    const json::Array* items = nullptr;
    if (answer.isArray()) items = &answer.array();
    else if (answer.isObject())
        if (const auto* list = answer.object().find("items"); list && list->isArray())
            items = &list->array();
    std::vector<std::string> labels;
    if (!items) return labels;
    for (const auto& item : *items)
        if (item.isObject()) {
            // What the item inserts, which is the name: a label may carry
            // decoration (`foul printLine`).
            const auto& object = item.object();
            for (const char* key : {"filterText", "insertText", "label"})
                if (const auto* text = object.find(key); text && text->isString()) {
                    labels.push_back(text->string());
                    break;
                }
        }
    return labels;
}

// Whether a checker error on a substituted name says the name should never
// have been offered: unknown, not in scope, or — after a dot — not for this
// receiver (its first argument refused). Arity errors and later arguments'
// types are expected: the substitution keeps the original call's arguments.
auto rejectsOffer(const std::string& message, const std::string& label, bool afterDot)
    -> bool {
    if (afterDot && message.rfind("`" + label + "` expects argument 1 to be", 0) == 0)
        return true;
    if (message.find("`" + label + "`") == std::string::npos &&
        message.find("'" + label + "'") == std::string::npos &&
        message.find(" " + label) == std::string::npos)
        return false;
    // About the name itself: `UInt32` offered in place of a receiver draws
    // "`UInt32` has no function `biases`", which is about the member.
    for (const char* marker : {"Undefined", "undefined", "not in scope", "not visible",
                               "not defined", "not imported", "is private",
                               "without `using"})
        if (message.find(marker) != std::string::npos) return true;
    return false;
}

auto sweepFile(const Options& options, const std::string& path, std::mt19937& rng)
    -> void {
    const auto absolute = fs::absolute(path).lexically_normal().string();
    const auto text = readFile(absolute);
    const auto lines = splitLines(text);
    const auto sourceRoot = fs::path(absolute).parent_path().string();
    const auto workDir = options.outDir + "/work";
    fs::create_directories(workDir);

    const auto checker = checkerView(options, absolute, sourceRoot);
    auto tokens = identifiers(text);

    Session session;
    session.uri = "file://" + absolute;

    // Hover on every name the checker typed at its own position.
    std::vector<Token> hoverSites;
    std::vector<Token> dotSites;
    std::vector<Token> bareSites;
    for (const auto& token : tokens) {
        if (isKeyword(token.text)) continue;
        const bool lower = std::islower(static_cast<unsigned char>(token.text[0]));
        if (lower && checker.types.count({token.line + 1, token.column + 1}))
            hoverSites.push_back(token);
        if (token.afterDot && lower) dotSites.push_back(token);
        else if (lower) bareSites.push_back(token);
    }
    auto sampleOf = [&](std::vector<Token> sites) {
        std::shuffle(sites.begin(), sites.end(), rng);
        if (sites.size() > options.maxSitesPerFile) sites.resize(options.maxSitesPerFile);
        return sites;
    };
    if (options.checks.count("hover"))
        for (const auto& token : sampleOf(hoverSites))
            session.ask("hover", token.line, token.column, token, "hover");
    if (options.checks.count("definition"))
        for (const auto& token : sampleOf(bareSites))
            session.ask("definition", token.line, token.column, token, "definition");
    const auto dotSample = sampleOf(dotSites);
    if (options.checks.count("recall"))
        for (const auto& token : dotSample)
            session.ask("completion", token.line,
                        token.column + static_cast<int>(token.text.size()), token, "recall");
    if (options.checks.count("soundness")) {
        for (const auto& token : dotSample)
            session.ask("completion", token.line, token.column, token, "sound-dot");
        for (const auto& token : sampleOf(bareSites))
            session.ask("completion", token.line, token.column, token, "sound-bare");
    }

    const auto result = runSession(options, session, text, workDir);
    if (!result.completed) {
        if (options.checks.count("crash"))
            report("crash", path, 0, 0,
                   "session did not reach shutdown: " + result.stderrTail);
        return;
    }
    for (const auto& [id, error] : result.errors)
        if (options.checks.count("crash"))
            report("crash", path, session.tokenOf[id].line, session.tokenOf[id].column,
                   session.methodOf[id] + " answered an error: " + error);

    // Substitutions are checked after the session, in one batch per file.
    struct Substitution {
        Token token;
        std::string label;
        std::string purpose;
    };
    std::vector<Substitution> substitutions;

    if (std::getenv("LSP_SWEEP_DUMP")) {
        std::ofstream dump(options.outDir + "/answers.txt", std::ios::app);
        for (const auto& [id, answer] : result.results)
            dump << path << ":" << session.tokenOf[id].line + 1 << ":" << session.tokenOf[id].column + 1
                 << " " << session.purposeOf[id] << " `" << session.tokenOf[id].text << "` => "
                 << json::stringify(answer).substr(0, 1500) << "\n";
    }
    for (const auto& [id, answer] : result.results) {
        const auto& token = session.tokenOf[id];
        const auto& purpose = session.purposeOf[id];
        if (purpose == "hover") {
            const auto shown = hoverText(answer);
            const auto& inferred = checker.types.at({token.line + 1, token.column + 1});
            if (shown.empty()) {
                report("hover", path, token.line, token.column,
                       "no hover for `" + token.text + "` (checker: " + inferred.front() + ")");
                continue;
            }
            const auto type = hoverType(shown, token.text);
            if (!type) continue;
            const auto same = [&](const std::string& checked) {
                return withVariablesRenamed(checked) == withVariablesRenamed(*type);
            };
            if (std::none_of(inferred.begin(), inferred.end(), same)) {
                std::string known;
                for (const auto& each : inferred) known += (known.empty() ? "" : " | ") + each;
                report("hover", path, token.line, token.column,
                       "`" + token.text + "` shown as " + *type + ", checker inferred " + known);
            }
        } else if (purpose == "definition") {
            if (answer.isNull()) continue;
            const json::Value* location = &answer;
            if (answer.isArray()) {
                if (answer.array().empty()) continue;
                location = &answer.array().front();
            }
            if (!location->isObject()) continue;
            const auto& object = location->object();
            const auto* uri = object.find("uri");
            const auto* range = object.find("range");
            if (!uri || !range) uri = object.find("targetUri"), range = object.find("targetSelectionRange");
            if (!uri || !range || !uri->isString()) continue;
            auto targetPath = uri->string();
            if (targetPath.rfind("file://", 0) == 0) targetPath = targetPath.substr(7);
            const auto& start = range->object().get("start").object();
            const int line = start.get("line").integer();
            const int character = start.get("character").integer();
            const auto targetLines =
                targetPath == absolute ? lines : splitLines(readFile(targetPath));
            const auto at = line < static_cast<int>(targetLines.size()) &&
                                    character <= static_cast<int>(targetLines[line].size())
                ? targetLines[line].substr(character, token.text.size()) : "";
            if (at != token.text)
                report("definition", path, token.line, token.column,
                       "`" + token.text + "` goes to " + targetPath + ":" +
                           std::to_string(line + 1) + ":" + std::to_string(character + 1) +
                           ", which reads `" + at + "`");
        } else if (purpose == "recall") {
            const auto labels = completionLabels(answer);
            // `push!` is `push` with the rebinding bang typed after it.
            auto offered = token.text;
            if (offered.size() > 1 && offered.back() == '!') offered.pop_back();
            if (std::find(labels.begin(), labels.end(), offered) == labels.end())
                report("recall", path, token.line, token.column,
                       "`" + token.text + "` not offered at `." + token.text + "|` (" +
                           std::to_string(labels.size()) + " offered)");
        } else if (purpose == "sound-dot" || purpose == "sound-bare") {
            auto labels = completionLabels(answer);
            labels.erase(std::remove_if(labels.begin(), labels.end(),
                                        [&](const std::string& label) {
                                            return label == token.text || label.empty() ||
                                                   !isIdentStart(label[0]) ||
                                                   label.find_first_of(" (<") != std::string::npos;
                                        }),
                         labels.end());
            std::shuffle(labels.begin(), labels.end(), rng);
            if (labels.size() > options.sample) labels.resize(options.sample);
            for (const auto& label : labels)
                substitutions.push_back({token, label, purpose});
        }
    }

    if (options.checks.count("diagnostics")) {
        std::set<std::string> published;
        for (const auto& diagnostic : result.diagnostics) {
            const auto& object = diagnostic.object();
            if (const auto* severity = object.find("severity");
                severity && severity->isInteger() && severity->integer() != 1)
                continue;
            const auto& start = object.get("range").object().get("start").object();
            published.insert(std::to_string(start.get("line").integer() + 1) + ":" +
                             std::to_string(start.get("character").integer() + 1) + ": " +
                             object.get("message").string());
        }
        for (const auto& error : checker.errors)
            if (!published.count(error))
                report("diagnostics", path, 0, 0, "kex -C reports, editor does not: " + error);
        for (const auto& error : published)
            if (!checker.errors.count(error))
                report("diagnostics", path, 0, 0, "editor reports, kex -C does not: " + error);
    }

    // Soundness: what completion offered must at least resolve where it was
    // offered. The case keeps the original's directory as its source root so
    // its `using`s resolve the same way.
    int caseNumber = 0;
    for (const auto& substitution : substitutions) {
        const auto& token = substitution.token;
        auto mutated = lines;
        // A bare function offered where the program names a value is checked
        // as the call it would be written as: a reference to one draws
        // "Undefined name" even where a call reaches it. Arity is not the
        // question, and its error is not counted.
        const auto after = mutated[token.line].substr(token.column + token.text.size());
        const bool asCall = substitution.purpose == "sound-bare" &&
                            std::islower(static_cast<unsigned char>(substitution.label[0])) &&
                            !after.starts_with("(");
        mutated[token.line] = mutated[token.line].substr(0, token.column) + substitution.label +
                              (asCall ? "()" : "") + after;
        std::string mutatedText;
        for (std::size_t i = 0; i < mutated.size(); ++i)
            mutatedText += mutated[i] + (i + 1 < mutated.size() ? "\n" : "");
        const auto casePath = workDir + "/" + fs::path(path).stem().string() + "_" +
                              std::to_string(caseNumber++) + ".kex";
        writeFile(casePath, mutatedText);
        const auto view = checkerView(options, casePath, sourceRoot);
        for (const auto& error : view.errors) {
            int ln = 0;
            if (std::sscanf(error.c_str(), "%d:", &ln) != 1 || ln != token.line + 1) continue;
            if (checker.errors.count(error)) continue;
            const auto message = error.substr(error.find(": ") + 2);
            if (!rejectsOffer(message, substitution.label,
                              substitution.purpose == "sound-dot"))
                continue;
            report("soundness", path, token.line, token.column,
                   (substitution.purpose == "sound-dot" ? "after `.`" : "bare") +
                       std::string(" offered `") + substitution.label + "` in place of `" +
                       token.text + "`, checker: " + message);
            break;
        }
        fs::remove(casePath);
    }
}

auto parseOptions(int argc, char** argv) -> std::optional<Options> {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--kex") options.kex = value();
        else if (arg == "--out") options.outDir = value();
        else if (arg == "--sample") options.sample = std::stoul(value());
        else if (arg == "--sites") options.maxSitesPerFile = std::stoul(value());
        else if (arg == "--seed") options.seed = static_cast<unsigned>(std::stoul(value()));
        else if (arg == "--timeout") options.timeoutSeconds = std::stoi(value());
        else if (arg == "--checks") {
            options.checks.clear();
            std::stringstream list(value());
            std::string check;
            while (std::getline(list, check, ',')) options.checks.insert(check);
        } else if (arg.rfind("--", 0) == 0) {
            return std::nullopt;
        } else {
            options.files.push_back(arg);
        }
    }
    if (options.files.empty()) return std::nullopt;
    return options;
}

} // namespace

auto main(int argc, char** argv) -> int {
    const auto options = parseOptions(argc, argv);
    if (!options) {
        std::cerr << "usage: lsp_sweep [--kex PATH] [--out DIR] [--checks hover,recall,"
                     "soundness,diagnostics,definition,crash] [--sample N] [--sites N] "
                     "[--seed S] [--timeout SECONDS] FILE...\n";
        return 2;
    }
    std::mt19937 rng(options->seed);
    fs::create_directories(options->outDir);
    for (const auto& file : options->files) {
        const auto before = findings.size();
        sweepFile(*options, file, rng);
        std::cerr << file << ": " << findings.size() - before << " finding(s)\n";
    }
    std::sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) {
        return std::tie(a.check, a.file, a.line, a.column) <
               std::tie(b.check, b.file, b.line, b.column);
    });
    std::ofstream log(options->outDir + "/findings.txt");
    for (const auto& finding : findings) {
        const auto line = finding.check + " " + finding.file + ":" +
                          std::to_string(finding.line) + ":" + std::to_string(finding.column) +
                          ": " + finding.detail;
        std::cout << line << "\n";
        log << line << "\n";
    }
    std::cerr << findings.size() << " finding(s); seed " << options->seed << "\n";
    return findings.empty() ? 0 : 1;
}
