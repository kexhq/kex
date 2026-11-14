#include "completion_at.hxx"
#include "../common/well_known.hxx"
#include "symbol_queries.hxx"
#include "collect_pass.hxx"
#include "../compiled/expand.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <fstream>
#include <iterator>


#include <algorithm>
#include <cctype>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace kex::semantic {

auto completionQualifierForType(const TypePtr& type) -> std::string {
    if (!type) return {};
    return std::visit([&type](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ListType>) return "List";
        else if constexpr (std::is_same_v<T, MapType>) return "Map";
        else if constexpr (std::is_same_v<T, OptionalType>)
            return "Optional";
        else if constexpr (std::is_same_v<T, NamedType>)
        {
            // A type named before a dot (`List.ma`) is a namespace there:
            // complete what it holds, not its `Type` record's fields.
            if (value.name == kex::names::kTypeRecord && value.typeArgs.size() == 1)
                return completionQualifierForType(value.typeArgs.front());
            return value.typeArgs.empty() ? value.name : typeToString(type);
        }
        else if constexpr (std::is_same_v<T, PrimitiveType> ||
                           std::is_same_v<T, SizedIntType> ||
                           std::is_same_v<T, SizedFloatType>)
            return typeToString(type);
        return {};
    }, type->kind);
}


auto scanDotReceiver(const std::string& source, size_t cursor) -> DotReceiver {
    const auto isWord = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '?' || c == '!';
    };
    const auto skipSpaceBack = [&](size_t i) {
        while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
            --i;
        return i;
    };
    // Offset of the delimiter opening the group that ends just before `end`.
    const auto matchOpen = [&](size_t end, char close, char open) -> size_t {
        int depth = 0;
        for (size_t j = end; j > 0; --j) {
            const char c = source[j - 1];
            if (c == close) {
                ++depth;
            } else if (c == open) {
                --depth;
                if (depth == 0) return j - 1;
            }
        }
        return std::string::npos;
    };

    size_t i = cursor;
    while (i > 0 && isWord(static_cast<unsigned char>(source[i - 1]))) --i;
    i = skipSpaceBack(i);
    if (i == 0 || source[i - 1] != '.') return {};
    const size_t dot = i - 1;

    // `&.text` in `items.map(&.text)`: the shorthand block's parameter is the
    // receiver (recoveredReceiverQualifier types it).
    if (dot > 0 && source[dot - 1] == '&')
        return {.start = dot - 1, .dot = dot, .callOpen = 0, .valid = true};
    // Where the outermost expression is recorded is decided by how the
    // receiver ENDS: a call or a block at its `(` / `{`, a member of
    // something by its name (MethodCall::methodLocation). Only the last link says
    // this — `a.f(x).g` is the member `g`, not the call `f(x)` met first
    // going back; taking that call, or the chain's start, completed against
    // the wrong link (`layer.biases.` offered Layer's fields).
    size_t callOpen = 0;
    i = skipSpaceBack(dot);
    size_t member = 0;
    bool atEnd = true;
    while (i > 0) {
        const char c = source[i - 1];
        // A group is never the whole receiver: `makeBox(3)` continues into the
        // name in front of it, and `[c][0]` into the list before the index.
        if (c == ')' || c == ']') {
            const size_t open = matchOpen(i, c, c == ')' ? '(' : '[');
            if (open == std::string::npos) return {};
            if (c == ')' && atEnd) callOpen = open;
            atEnd = false;
            i = open;
            continue;
        }
        // A block continues into the call it is passed to — `xs.map { … }`,
        // recorded at its `{` — while a map literal standing alone is the
        // whole receiver.
        if (c == '}') {
            const size_t open = matchOpen(i, '}', '{');
            if (open == std::string::npos) return {};
            if (atEnd) callOpen = open;
            atEnd = false;
            const size_t before = skipSpaceBack(open);
            if (before > 0 &&
                (isWord(static_cast<unsigned char>(source[before - 1])) ||
                 source[before - 1] == ')')) {
                i = before;
                continue;
            }
            i = open;
            break;
        }
        if (c == '"' || c == '\'' || c == '`') {
            size_t j = i - 1;
            while (j > 0) {
                --j;
                if (source[j] == c && (j == 0 || source[j - 1] != '\\')) break;
            }
            i = j;
            continue;
        }
        if (!isWord(static_cast<unsigned char>(c))) break;
        while (i > 0 && isWord(static_cast<unsigned char>(source[i - 1]))) --i;
        // A leading `!` is a negation in front of the receiver, not part of
        // its name: `!control.hint` reads `hint` on `control` (#456).
        while (i < dot && (source[i] == '!' || source[i] == '?')) ++i;
        // `@size` is one expression and the AST records it starting at the
        // `@`. Stopping at the sigil pointed one byte too far right, so the
        // recorded type could not be found and every `@field.method` fell back
        // to re-analyzing the whole buffer.
        if (i > 0 && source[i - 1] == '@') --i;
        // A dot here means the chain continues leftwards: `Web.Server.new(0)`.
        const size_t before = skipSpaceBack(i);
        if (before == 0 || source[before - 1] != '.') break;
        // Lowercase: a capitalized name after a dot is a namespace or a
        // constructor (`FS.File`), not a member call.
        if (atEnd && std::islower(static_cast<unsigned char>(source[i]))) member = i;
        atEnd = false;
        i = skipSpaceBack(before - 1);
    }
    if (i >= dot) return {};
    return {.start = i, .dot = dot, .callOpen = callOpen, .member = member, .valid = true};
}

auto bareCallReceiver(const std::string& source, const DotReceiver& receiver) -> bool {
    if (!receiver.valid || !receiver.callOpen || receiver.callOpen <= receiver.start) return false;
    return std::all_of(source.begin() + static_cast<std::ptrdiff_t>(receiver.start),
                       source.begin() + static_cast<std::ptrdiff_t>(receiver.callOpen),
                       [](char c) {
                           return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
                                  c == '?' || c == '!';
                       });
}

auto receiverTypeOf(const Analyzer& analyzer, const ast::Expr* expression) -> TypePtr {
    if (!expression) return nullptr;
    // `new.age = ...`: the value written, not the assignment's own Unit.
    if (const auto* assignment = std::get_if<ast::AssignExpr>(&expression->kind);
        assignment && !assignment->path.empty())
        return analyzer.assignmentTargetTypeOf(expression);
    return analyzer.displayTypeOf(expression);
}

auto checkedReceiver(const CompletionChecker& check, const std::string& source,
                     const std::string& path, const DotReceiver& receiver,
                     size_t cursor) -> std::optional<CheckedReceiver> {
    if (!receiver.valid || cursor < receiver.dot) return std::nullopt;
    // The 1-based line and column of `offset` in `text`.
    const auto positionOf = [](const std::string& text, size_t offset) {
        int line = 1;
        size_t lineStart = 0;
        for (size_t i = 0; i < offset; ++i)
            if (text[i] == '\n') { ++line; lineStart = i + 1; }
        return std::pair<int, int>{line, static_cast<int>(offset - lineStart) + 1};
    };
    // The typed expression the analysis records at a position. Several can
    // share one — `makeBox` the identifier and `makeBox(3)` the call both
    // start at `m` — and only one has a type to complete against.
    const auto typedAt = [](const Analyzer& analyzer, std::pair<int, int> position)
        -> const ast::Expr* {
        // One that names a type to complete members of first (a namespace
        // path's `FS.File`), then any typed one (a `Number`).
        const ast::Expr* qualified = nullptr;
        const ast::Expr* typed = nullptr;
        const auto better = [](const ast::Expr* held, const ast::Expr& candidate) {
            // A call over the identifier it starts with.
            return !held || std::holds_alternative<ast::MethodCall>(candidate.kind) ||
                   std::holds_alternative<ast::FunctionCall>(candidate.kind);
        };
        for (const auto& [expression, _] : analyzer.typeMap()) {
            if (!expression || expression->location.line != position.first ||
                expression->location.column != position.second)
                continue;
            const auto type = receiverTypeOf(analyzer, expression);
            if (!type || std::holds_alternative<UnknownType>(type->kind)) continue;
            if (better(typed, *expression)) typed = expression;
            if (!completionQualifierForType(type).empty() && better(qualified, *expression))
                qualified = expression;
        }
        return qualified ? qualified : typed;
    };
    // The shorthand `&.`: its parameter is a block's, typed by the call the
    // block is passed to. Written out as `{ |kexItem| kexItem }`, the
    // checker types it, and the body's `kexItem` is the receiver.
    if (source[receiver.start] == '&') {
        const std::string opening = "{ |kexItem| ";
        auto rewritten = source.substr(0, receiver.start) + opening + "kexItem }" +
                         source.substr(cursor);
        auto checked = check(path, rewritten);
        if (!checked) return std::nullopt;
        const auto* expression =
            typedAt(*checked->analyzer, positionOf(rewritten, receiver.start + opening.size()));
        return CheckedReceiver{std::move(*checked), expression};
    }
    // Deleting through the cursor rather than just the dot is what makes
    // `makeBox(3).g` parse again. Checked as a build checks it, not a bare
    // parse: `using Web` is resolved by loading that module's source, and
    // without it every type from an opt-in module came back `unknown`.
    // A name already after the cursor (asking at `c.|area()`) is a member the
    // buffer has: it parses as it is, and deleting only the dot would fuse
    // the two (`carea()`).
    // So does a name already typed (`scores.fi|nd { ... }`, `"s".sh|`): an
    // unknown one is a type error, not a parse error, and deleting it would
    // hand a following block to the receiver (`scores { ... }`). Only a bare
    // trailing dot is removed.
    auto recovered = source;
    const bool memberFollows =
        cursor < source.size() &&
        (std::isalnum(static_cast<unsigned char>(source[cursor])) || source[cursor] == '_');
    const bool memberTyped = std::any_of(
        source.begin() + static_cast<std::ptrdiff_t>(receiver.dot) + 1,
        source.begin() + static_cast<std::ptrdiff_t>(cursor),
        [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });
    if (!memberFollows && !memberTyped) recovered.erase(receiver.dot, cursor - receiver.dot);
    auto checked = check(path, recovered);
    if (!checked) return std::nullopt;
    // Everything before the deletion keeps its position. The receiver is
    // recorded where its outermost expression is: a member by its name, a
    // call at its `(`. A chain's start is its root's (`layer` in
    // `layer.biases`), never the chain's own, so only a receiver that is one
    // expression is asked there.
    if (receiver.member) {
        const auto [line, column] = positionOf(source, receiver.member);
        for (const auto& [expression, _] : checked->analyzer->typeMap())
            if (const auto* call = expression
                    ? std::get_if<ast::MethodCall>(&expression->kind) : nullptr;
                call && call->methodLocation.line == line &&
                call->methodLocation.column == column &&
                expression->location.file == path)
                return CheckedReceiver{std::move(*checked), expression};
        return CheckedReceiver{std::move(*checked), nullptr};
    }
    const auto* expression = typedAt(
        *checked->analyzer,
        positionOf(source, receiver.callOpen ? receiver.callOpen : receiver.start));
    // A bare call (`words()`) is recorded where its name is, not at its `(`:
    // there the receiver's start is the call itself.
    if (!expression && receiver.callOpen && bareCallReceiver(source, receiver))
        expression = typedAt(*checked->analyzer, positionOf(source, receiver.start));
    return CheckedReceiver{std::move(*checked), expression};
}

auto recoveredReceiverQualifier(const CompletionChecker& check,
                                const std::string& source,
                                const std::string& path,
                                const DotReceiver& receiver, size_t cursor,
                                const ImportedInterfaces*)
    -> std::string {
    const auto found = checkedReceiver(check, source, path, receiver, cursor);
    if (!found || !found->expression) return {};
    return completionQualifierForType(receiverTypeOf(*found->checked.analyzer, found->expression));
}

auto completionClosers(const std::string& text) -> std::string {
    const auto isWord = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '?' || c == '!';
    };
    // `(`, `[`, `{`, or 'd' for a `do` block.
    std::vector<char> open;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '#') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (c == '"' || c == '\'' || c == '`') {
            for (++i; i < text.size() && text[i] != c; ++i)
                if (text[i] == '\\' && c != '`') ++i;
        } else if (c == '(' || c == '[' || c == '{') {
            open.push_back(c);
        } else if (c == ')' || c == ']' || c == '}') {
            if (!open.empty() && open.back() != 'd') open.pop_back();
        } else if (isWord(static_cast<unsigned char>(c)) &&
                   (i == 0 || !isWord(static_cast<unsigned char>(text[i - 1])))) {
            size_t end = i;
            while (end < text.size() &&
                   isWord(static_cast<unsigned char>(text[end])))
                ++end;
            const auto word = std::string_view(text).substr(i, end - i);
            if (word == "do") open.push_back('d');
            else if (word == "end" && !open.empty() && open.back() == 'd')
                open.pop_back();
            i = end - 1;
        }
    }
    std::string closers;
    for (auto it = open.rbegin(); it != open.rend(); ++it) {
        switch (*it) {
            case '(': closers += ')'; break;
            case '[': closers += ']'; break;
            case '{': closers += '}'; break;
            default: closers += "\nend"; break;
        }
    }
    return closers;
}

auto completeAt(const std::vector<SymbolInfo>& base, const CompletionChecker& check,
                const std::string& path, const std::string& source,
                size_t cursor, const ImportedInterfaces* interfaces)
    -> CompletionAt {
    // The names completion reads: `base`'s, and those a checked program
    // declares with the names in scope at each point of it.
    auto baseSymbols = base;
    const auto namesIn = [&](const std::optional<CheckedSource>& checked) {
        IndexedFiles files{{"<base>", &baseSymbols}};
        if (checked)
            files.push_back({path, const_cast<std::vector<SymbolInfo>*>(
                                        &checked->analyzer->nameIndex().symbols),
                             &checked->analyzer->nameIndex().completionScopes});
        return files;
    };
    const auto globalCompletions = [&](const std::string& prefix,
                                       const std::optional<CheckedSource>& checked) {
        return completionsFor(namesIn(checked), prefix, interfaces);
    };
    const auto isWord = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '?' || c == '!';
    };
    cursor = std::min(cursor, source.size());
    CompletionAt result;
    size_t wordStart = cursor;
    while (wordStart > 0 &&
           isWord(static_cast<unsigned char>(source[wordStart - 1])))
        --wordStart;
    result.replaceFrom = wordStart;
    const auto word = source.substr(wordStart, cursor - wordStart);

    // A member of an expression the analyzer can type.
    if (const auto receiver = scanDotReceiver(source, cursor); receiver.valid) {
        result.qualifier = recoveredReceiverQualifier(
            check, source, path, receiver, cursor, interfaces);
        if (!result.qualifier.empty()) {
            const auto qualified = result.qualifier + ".";
            for (auto& match : globalCompletions(qualified + word, check(path, source)))
                if (match.rfind(qualified, 0) == 0)
                    result.matches.push_back(match.substr(qualified.size()));
            return result;
        }
    }

    // A dotted name that is not an expression — `IO.pr`, `Web.Server.` — is a
    // module path, looked up as written.
    size_t pathStart = wordStart;
    while (pathStart > 0 &&
           (isWord(static_cast<unsigned char>(source[pathStart - 1])) ||
            source[pathStart - 1] == '.'))
        --pathStart;
    if (pathStart < wordStart) {
        const auto qualified = source.substr(pathStart, wordStart - pathStart);
        for (auto& match : globalCompletions(qualified + word, check(path, source)))
            if (match.rfind(qualified, 0) == 0)
                result.matches.push_back(match.substr(qualified.size()));
        return result;
    }

    // A bare name: the index's global names plus the bindings in scope at the
    // cursor, which only an analysis of the surrounding program knows.
    const auto checked = check(path, source);
    int line = 1;
    size_t lineStart = 0;
    for (size_t i = 0; i < wordStart; ++i)
        if (source[i] == '\n') { ++line; lineStart = i + 1; }
    result.matches = completionsAt(namesIn(checked), path, static_cast<uint32_t>(line),
                                   static_cast<uint32_t>(wordStart - lineStart + 1), word,
                                   interfaces);
    std::sort(result.matches.begin(), result.matches.end());
    result.matches.erase(
        std::unique(result.matches.begin(), result.matches.end()),
        result.matches.end());
    return result;
}

auto completeReplInput(const std::vector<SymbolInfo>& base, const CompletionChecker& check,
                       const std::string& definitions,
                       const std::string& bindings,
                       const std::string& input, const std::string& line,
                       size_t point, bool declaration,
                       const ImportedInterfaces* interfaces) -> CompletionAt {
    const auto prefix =
        definitions + (declaration ? "" : "main do\n" + bindings);
    const auto program = prefix + input + completionClosers(input) +
                         (declaration ? "\n" : "\nend\n");
    const size_t lineStart = prefix.size() + (input.size() - line.size());
    auto completion = completeAt(base, check, "<repl-completion>", program,
                                 lineStart + std::min(point, line.size()),
                                 interfaces);
    completion.replaceFrom -= lineStart;
    return completion;
}

} // namespace kex::semantic

namespace kex::semantic {

auto symbolsOfSource(const std::string& path, std::string source,
                     const std::vector<std::string>& roots) -> std::vector<SymbolInfo> {
    Lexer lexer(std::move(source), path);
    Parser parser(lexer.tokenizeAll(), path);
    auto program = parser.parseProgram();
    std::vector<Diagnostic> diagnostics;
    compiled::ExpandOptions options;
    options.sourcePath = path;
    options.moduleRoots = roots;
    compiled::expand(program, diagnostics, options);
    return collectSymbols(program, path);
}

auto symbolsOfFiles(const std::vector<std::string>& paths,
                    const std::vector<std::string>& roots) -> std::vector<SymbolInfo> {
    std::vector<SymbolInfo> symbols;
    for (const auto& path : paths) {
        std::ifstream input(path, std::ios::binary);
        if (!input) continue;
        std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
        for (auto& symbol : symbolsOfSource(path, std::move(source), roots))
            symbols.push_back(std::move(symbol));
    }
    return symbols;
}

} // namespace kex::semantic
