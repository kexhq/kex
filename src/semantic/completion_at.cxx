#include "completion_at.hxx"


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
            return value.typeArgs.empty() ? value.name
                                          : typeToString(type);
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

    size_t callOpen = 0;
    i = skipSpaceBack(dot);
    while (i > 0) {
        const char c = source[i - 1];
        // A group is never the whole receiver: `makeBox(3)` continues into the
        // name in front of it, and `[c][0]` into the list before the index.
        if (c == ')' || c == ']') {
            const size_t open = matchOpen(i, c, c == ')' ? '(' : '[');
            if (open == std::string::npos) return {};
            if (c == ')' && callOpen == 0) callOpen = open;
            i = open;
            continue;
        }
        // A block continues into the call it is passed to — `xs.map { … }`,
        // recorded at its `{` — while a map literal standing alone is the
        // whole receiver.
        if (c == '}') {
            const size_t open = matchOpen(i, '}', '{');
            if (open == std::string::npos) return {};
            if (callOpen == 0) callOpen = open;
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
        // `@size` is one expression and the AST records it starting at the
        // `@`. Stopping at the sigil pointed one byte too far right, so the
        // recorded type could not be found and every `@field.method` fell back
        // to re-analyzing the whole buffer.
        if (i > 0 && source[i - 1] == '@') --i;
        // A dot here means the chain continues leftwards: `Web.Server.new(0)`.
        const size_t before = skipSpaceBack(i);
        if (before == 0 || source[before - 1] != '.') break;
        i = skipSpaceBack(before - 1);
    }
    if (i >= dot) return {};
    return {.start = i, .dot = dot, .callOpen = callOpen, .valid = true};
}

auto recoveredReceiverQualifier(SemanticDB& analysisDb,
                                const std::string& source,
                                const std::string& path,
                                const DotReceiver& receiver, size_t cursor,
                                const ImportedInterfaces* interfaces)
    -> std::string {
    if (!receiver.valid || cursor < receiver.dot) return {};
    // Deleting through the cursor rather than just the dot is what makes
    // `makeBox(3).g` parse again.
    auto recovered = source;
    recovered.erase(receiver.dot, cursor - receiver.dot);
    // Through a SemanticDB with the file's own module roots, not a bare
    // parse: `using Web` is resolved by loading that module's source, and
    // without it every type from an opt-in module came back `unknown` — so a
    // builder chain on `Web.Server` completed to nothing while the same shape
    // on a local record worked.
    analysisDb.updateFile(path, recovered);
    auto* state = analysisDb.fileState(path);
    if (!state) return {};
    Analyzer analyzer(interfaces);
    analyzer.analyze(state->ast);

    // Everything before the deletion keeps its position, so the receiver's
    // line/column are the ones it had in the original buffer. A CALL is
    // recorded at its argument list's `(`, so a builder chain
    // (`Web.Server.new(0).get(…).post(…)`) has nothing at all at the start of
    // the receiver — that position is where `Web` is, and the chain's own type
    // lives at the last call's paren.
    const auto positionOf = [&](size_t offset) {
        int line = 1;
        size_t lineStart = 0;
        for (size_t i = 0; i < offset; ++i)
            if (source[i] == '\n') { ++line; lineStart = i + 1; }
        return std::pair<int, int>{
            line, static_cast<int>(offset - lineStart) + 1};
    };
    std::vector<std::pair<int, int>> candidates;
    if (receiver.callOpen) candidates.push_back(positionOf(receiver.callOpen));
    candidates.push_back(positionOf(receiver.start));

    // Several expressions can start at one column — `makeBox` the identifier
    // and `makeBox(3)` the call both start at `m`. Only the call has a type
    // worth completing against, so take the first that yields a qualifier.
    for (const auto& [candidateLine, candidateColumn] : candidates)
        for (const auto& [expression, _] : analyzer.typeMap()) {
            if (!expression || expression->location.line != candidateLine ||
                expression->location.column != candidateColumn)
                continue;
            if (auto qualifier =
                    completionQualifierForType(analyzer.displayTypeOf(expression));
                !qualifier.empty())
                return qualifier;
        }
    return {};
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

auto completeAt(const SemanticDB& db, SemanticDB& analysisDb,
                const std::string& path, const std::string& source,
                size_t cursor, const ImportedInterfaces* interfaces)
    -> CompletionAt {
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
            analysisDb, source, path, receiver, cursor, interfaces);
        if (!result.qualifier.empty()) {
            const auto qualified = result.qualifier + ".";
            for (auto& match : db.completionsFor(qualified + word))
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
        for (auto& match : db.completionsFor(qualified + word))
            if (match.rfind(qualified, 0) == 0)
                result.matches.push_back(match.substr(qualified.size()));
        return result;
    }

    // A bare name: the index's global names plus the bindings in scope at the
    // cursor, which only an analysis of the surrounding program knows.
    result.matches = db.completionsFor(word);
    analysisDb.updateFile(path, source);
    int line = 1;
    size_t lineStart = 0;
    for (size_t i = 0; i < wordStart; ++i)
        if (source[i] == '\n') { ++line; lineStart = i + 1; }
    for (auto& name : analysisDb.completionsAt(
             path, static_cast<uint32_t>(line),
             static_cast<uint32_t>(wordStart - lineStart + 1), word))
        result.matches.push_back(std::move(name));
    std::sort(result.matches.begin(), result.matches.end());
    result.matches.erase(
        std::unique(result.matches.begin(), result.matches.end()),
        result.matches.end());
    return result;
}

} // namespace kex::semantic
