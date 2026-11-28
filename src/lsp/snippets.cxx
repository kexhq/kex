#include "snippets.hxx"

#include "../common/well_known.hxx"
#include "../lexer/lexer.hxx"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <string_view>

namespace kex::lsp {

namespace {

// What an open block (or bracket) makes of the lines inside it.
enum class Block {
    TopLevel,
    Declarations,
    Functions,
    Statements,
    Closed,  // `record`, `trait`, `type`, `match`/`receive` arms: no construct starts a line
    Bracket, // `(`, `[`, `{`: an expression continues
};

auto isWordChar(char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// A token a value can end with: one directly before `if` makes the `if` a
// trailing guard (`return x if done?`), not a block.
auto endsValue(TokenType type) -> bool {
    switch (type) {
        case TokenType::LowerIdent:
        case TokenType::UpperIdent:
        case TokenType::SpliceIdent:
        case TokenType::Integer:
        case TokenType::Float:
        case TokenType::String:
        case TokenType::RawString:
        case TokenType::InterpolatedRawString:
        case TokenType::Char:
        case TokenType::Atom:
        case TokenType::RParen:
        case TokenType::RBracket:
        case TokenType::RBrace:
        case TokenType::True:
        case TokenType::False:
        case TokenType::None:
        case TokenType::This:
        case TokenType::End:
        case TokenType::Question:
        case TokenType::Bang:
        case TokenType::Break:
        case TokenType::Next: return true;
        default: return false;
    }
}

// The block a `do` opens, from the last keyword before it on its line that
// says what kind of block it is. `parent` is the block it is written in.
auto blockOpenedBy(TokenType keyword, Block parent) -> Block {
    switch (keyword) {
        case TokenType::Module:
        case TokenType::Capability:
        case TokenType::Compiled: return Block::Declarations;
        case TokenType::Make:
        case TokenType::Serving: return Block::Functions;
        case TokenType::Record:
        case TokenType::Trait:
        case TokenType::Type:
        case TokenType::Match:
        case TokenType::Receive: return Block::Closed;
        case TokenType::Private:
        case TokenType::Public:
        case TokenType::Using: return parent == Block::Bracket ? Block::Statements : parent;
        default: return Block::Statements;
    }
}

auto classifiesBlock(TokenType type) -> bool {
    switch (type) {
        case TokenType::Module:
        case TokenType::Capability:
        case TokenType::Compiled:
        case TokenType::Make:
        case TokenType::Serving:
        case TokenType::Record:
        case TokenType::Trait:
        case TokenType::Type:
        case TokenType::Match:
        case TokenType::Receive:
        case TokenType::Private:
        case TokenType::Public:
        case TokenType::Using:
        case TokenType::Let:
        case TokenType::Foul:
        case TokenType::Main:
        case TokenType::Loop:
        case TokenType::While:
        case TokenType::Trying:
        case TokenType::Spawn: return true;
        default: return false;
    }
}

auto wordStartBefore(const std::string& text, size_t cursor) -> size_t {
    auto start = std::min(cursor, text.size());
    while (start > 0 && isWordChar(text[start - 1])) {
        --start;
    }
    return start;
}

auto lineStartOf(const std::string& text, size_t offset) -> size_t {
    const auto newline = offset == 0 ? std::string::npos : text.rfind('\n', offset - 1);
    return newline == std::string::npos ? 0 : newline + 1;
}

constexpr unsigned TopLevelOnly = 1U << 0;
constexpr unsigned InDeclarations = 1U << 1;
constexpr unsigned InFunctions = 1U << 2;
constexpr unsigned InStatements = 1U << 3;
constexpr unsigned InExpression = 1U << 4;
constexpr unsigned InBlockArgument = 1U << 5;
constexpr unsigned Declaration = TopLevelOnly | InDeclarations;

struct Entry {
    unsigned contexts;
    const char* label;
    const char* filterText;
    const char* detail;
    // `\t` is one indentation level, `\n` a new line at the cursor line's
    // indentation.
    const char* body;
};

// Every snippet, in the order a client shows them when it does not sort.
constexpr Entry Entries[] = {
    {InStatements | InExpression, "if", "if", "if … end", "if ${1:condition}\n\t$0\nend"},
    {InStatements | InExpression, "if else", "ifelse", "if … else … end",
     "if ${1:condition}\n\t$2\nelse\n\t$0\nend"},
    {InStatements | InExpression, "match", "match", "match … do … end",
     "match ${1:value} do\n\t${2:pattern} => $0\nend"},
    {InStatements, "loop", "loop", "loop do … end", "loop do\n\t$0\nend"},
    {InStatements, "loop |i|", "loop", "loop do |i| … end", "loop do |${1:i}|\n\t$0\nend"},
    {InStatements, "while", "while", "while … do … end", "while ${1:condition} do\n\t$0\nend"},
    {InStatements | InExpression, "trying", "trying", "trying do … rescue … end",
     "trying do\n\t$1\nrescue\n\t$0\nend"},
    {InStatements | InExpression, "receive", "receive", "receive do … end",
     "receive do\n\t${1:pattern} => $0\nend"},
    {InStatements | InExpression, "receive after", "receive", "receive do … after … end",
     "receive do\n\t${1:pattern} => $2\nafter timeout: ${3:1000}\n\t$0\nend"},
    {InBlockArgument, "do", "do", "do … end", "do\n\t$0\nend"},
    {InBlockArgument, "do |x|", "do", "do |x| … end", "do |${1:x}|\n\t$0\nend"},
    {Declaration | InFunctions, "let", "let", "let name(…) -> Type do … end",
     "let ${1:name}(${2:x}: ${3:Integer}) -> ${4:Integer} do\n\t$0\nend"},
    {Declaration | InFunctions, "foul", "foul", "foul name(…) do … end",
     "foul ${1:name}($2) do\n\t$0\nend"},
    {TopLevelOnly, "main", "main", "main do … end", "main do\n\t$0\nend"},
    {Declaration, "record", "record", "record Name do … end",
     "record ${1:Name} do\n\t${2:field}: ${3:Type}\nend"},
    {Declaration, "type", "type", "type Name = Variant | …",
     "type ${1:Name} = ${2:Variant} | ${0:Other}"},
    {Declaration, "trait", "trait", "trait Name do … end",
     "trait ${1:Name} do\n\t${2:name} :> ${3:Type}\nend"},
    {Declaration, "make", "make", "make Type do … end", "make ${1:Type} do\n\t$0\nend"},
    {Declaration, "make implement", "make", "make Type, implement: Trait do … end",
     "make ${1:Type}, implement: ${2:Trait} do\n\t$0\nend"},
    {Declaration, "module", "module", "module Name do … end", "module ${1:Name} do\n\t$0\nend"},
    {Declaration, "compiled", "compiled", "compiled do … end", "compiled do\n\t$0\nend"},
};

// A block parameter's name for a value of `type`: its initial (`s` for a
// String), `x` for a type variable, a compound type or one not known.
auto blockParameterName(const std::string& type) -> std::string {
    const auto dot = type.rfind('.');
    const auto leaf = dot == std::string::npos ? type : type.substr(dot + 1);
    if (leaf.size() < 2 || !std::isupper(static_cast<unsigned char>(leaf.front())) ||
        !std::all_of(leaf.begin(), leaf.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c));
        })) {
        return "x";
    }
    return std::string(
        1, static_cast<char>(std::tolower(static_cast<unsigned char>(leaf.front()))));
}

auto contextBit(SnippetContext context) -> unsigned {
    switch (context) {
        case SnippetContext::TopLevel: return TopLevelOnly;
        case SnippetContext::Declarations: return InDeclarations;
        case SnippetContext::Functions: return InFunctions;
        case SnippetContext::Statements: return InStatements;
        case SnippetContext::Expression: return InExpression;
        case SnippetContext::BlockArgument: return InBlockArgument;
        case SnippetContext::None: return 0;
    }
    return 0;
}

} // namespace

namespace {

// What the tokens before a word say about it.
// An open block, the keyword that says what kind it is, and where that
// keyword is (the `do` itself when nothing before it on its line says).
struct Open {
    Block block;
    TokenType opener = TokenType::Eof;
    size_t offset = 0;
};

struct Walk {
    SnippetContext context = SnippetContext::None;
    std::optional<MatchSite> match;
    std::optional<MakeSite> make;
    // The blocks open where the word starts, innermost last.
    std::vector<Open> open;
};

// `onClose`, when given, is told of each block an `end` before the cursor
// closes, and where that `end` starts.
auto walk(
    const std::string& text,
    size_t cursor,
    const std::function<void(const Open&, size_t)>& onClose = {}) -> Walk {
    const auto wordStart = wordStartBefore(text, cursor);
    if (wordStart > 0 && (text[wordStart - 1] == '.' || text[wordStart - 1] == '@' ||
                          text[wordStart - 1] == ':' || text[wordStart - 1] == '%')) {
        return {};
    }

    Lexer lexer(text);
    const auto tokens = lexer.tokenizeAll();

    std::vector<Open> stack;
    const auto current = [&] { return stack.empty() ? Block::TopLevel : stack.back().block; };
    const Token* previous = nullptr; // the last token before the word on its line
    TokenType classifier = TokenType::Eof;
    size_t classifierOffset = 0;
    // `implement` written after `make` on this line: the block it opens
    // claims a trait.
    bool claims = false;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        if (token.type == TokenType::Eof || token.startOffset < 0) {
            break;
        }
        const auto start = static_cast<size_t>(token.startOffset);
        const auto end = static_cast<size_t>(token.endOffset);
        // A word inside a string, character or number literal, or inside an
        // unterminated one the lexer reported as an error, is not code.
        if (start < wordStart &&
            (wordStart < end || (token.type == TokenType::Error && wordStart <= end))) {
            return {};
        }
        if (start >= wordStart) {
            break;
        }
        switch (token.type) {
            case TokenType::Newline:
                previous = nullptr;
                classifier = TokenType::Eof;
                claims = false;
                continue;
            case TokenType::Do:
                stack.push_back(
                    {blockOpenedBy(classifier, current()), classifier,
                     classifier == TokenType::Eof ? start : classifierOffset});
                classifier = TokenType::Eof;
                break;
            case TokenType::FatArrow: classifier = TokenType::Eof; break;
            case TokenType::If: {
                bool block = !previous || !endsValue(previous->type);
                if (previous && previous->type == TokenType::Return) {
                    // `return if c then a else b end` is a value; `return if c`
                    // alone is a guard.
                    block = false;
                    for (size_t j = i + 1;
                         j < tokens.size() && tokens[j].type != TokenType::Newline &&
                         tokens[j].type != TokenType::Eof;
                         ++j) {
                        if (tokens[j].type == TokenType::Then) {
                            block = true;
                            break;
                        }
                    }
                }
                if (block) {
                    const auto parent = current();
                    stack.push_back(
                        {parent == Block::Bracket ? Block::Statements : parent, TokenType::If,
                         start});
                }
                break;
            }
            case TokenType::End:
                if (!stack.empty() && stack.back().block != Block::Bracket) {
                    if (onClose) {
                        onClose(stack.back(), start);
                    }
                    stack.pop_back();
                }
                break;
            case TokenType::LParen:
            case TokenType::LBracket:
            case TokenType::LBrace:
            case TokenType::HashLBracket:
                stack.push_back({Block::Bracket, token.type, start});
                break;
            case TokenType::RParen:
            case TokenType::RBracket:
            case TokenType::RBrace:
                while (!stack.empty()) {
                    const auto popped = stack.back().block;
                    stack.pop_back();
                    if (popped == Block::Bracket) {
                        break;
                    }
                }
                break;
            default:
                if (classifiesBlock(token.type)) {
                    classifier = token.type;
                    classifierOffset = start;
                }
                if (classifier == TokenType::Make && token.type == TokenType::LowerIdent &&
                    token.value == "implement") {
                    claims = true;
                }
                break;
        }
        previous = &token;
    }

    // Comments are skipped by the lexer: a `#` between the last token and
    // the word, on the word's line, starts one.
    const auto lineStart = lineStartOf(text, wordStart);
    {
        size_t gapStart = lineStart;
        if (previous && static_cast<size_t>(previous->endOffset) > gapStart) {
            gapStart = static_cast<size_t>(previous->endOffset);
        }
        for (auto at = gapStart; at < wordStart; ++at) {
            if (text[at] == '#' && (at + 1 >= text.size() || text[at + 1] != '[')) {
                return {};
            }
        }
    }

    const auto block = current();
    const auto decide = [&]() -> Walk {
        const bool lineStartsHere = std::all_of(
            text.begin() + static_cast<std::ptrdiff_t>(lineStart),
            text.begin() + static_cast<std::ptrdiff_t>(wordStart),
            [](char c) { return c == ' ' || c == '\t'; });
        if (block == Block::Closed || block == Block::Bracket) {
            // A line of its own in a `match` block starts an arm.
            if (lineStartsHere && !stack.empty() && stack.back().opener == TokenType::Match) {
                return {SnippetContext::None, MatchSite{stack.back().offset, false}};
            }
            return {};
        }
        if (lineStartsHere && block == Block::Functions && !stack.empty() &&
            stack.back().opener == TokenType::Make) {
            // A line of its own in a `make` block: where a trait's functions go.
            return {SnippetContext::Functions, std::nullopt, MakeSite{stack.back().offset, false}};
        }
        if (lineStartsHere) {
            switch (block) {
                case Block::TopLevel: return {SnippetContext::TopLevel};
                case Block::Declarations: return {SnippetContext::Declarations};
                case Block::Functions: return {SnippetContext::Functions};
                default: return {SnippetContext::Statements};
            }
        }
        if (!previous || wordStart == 0 ||
            !std::isspace(static_cast<unsigned char>(text[wordStart - 1]))) {
            return {};
        }
        if (block == Block::Statements || block == Block::TopLevel) {
            if (previous->type == TokenType::Equals || previous->type == TokenType::FatArrow) {
                return {SnippetContext::Expression};
            }
        }
        if (endsValue(previous->type) && previous->type != TokenType::End) {
            // `match shape d`: the `do` that opens the match's arms.
            if (classifier == TokenType::Match) {
                return {SnippetContext::BlockArgument, MatchSite{classifierOffset, true}};
            }
            // `make Circle, implement: Shape d`: the `do` of a block that owes the
            // trait its functions.
            if (classifier == TokenType::Make && claims) {
                return {
                    SnippetContext::BlockArgument, std::nullopt, MakeSite{classifierOffset, true}};
            }
            return {SnippetContext::BlockArgument};
        }
        switch (previous->type) {
            case TokenType::Loop:
            case TokenType::Trying:
            case TokenType::Receive:
            case TokenType::Main:
            case TokenType::Spawn:
            case TokenType::Compiled: return {SnippetContext::BlockArgument};
            default: return {};
        }
    };
    auto result = decide();
    result.open = std::move(stack);
    return result;
}

// The cursor line's indentation, and one step inward: a tab, which the
// client turns into its own indentation setting as it inserts the snippet.
struct Indentation {
    std::string base;
    std::string unit = "\t";
};

auto indentationAt(const std::string& text, size_t wordStart) -> Indentation {
    const auto lineStart = lineStartOf(text, wordStart);
    auto indentEnd = lineStart;
    while (indentEnd < text.size() && (text[indentEnd] == ' ' || text[indentEnd] == '\t')) {
        ++indentEnd;
    }
    return {text.substr(lineStart, indentEnd - lineStart)};
}

// `body` with each `\t` one indentation step and each line after the first
// at the cursor line's indentation.
auto indented(std::string_view body, const Indentation& indentation) -> std::string {
    std::string out;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '\t') {
            out += indentation.unit;
        } else if (c == '\n') {
            out += '\n';
            // A blank line stays empty: no trailing whitespace.
            if (i + 1 < body.size() && body[i + 1] != '\n') {
                out += indentation.base;
            }
        } else {
            out += c;
        }
    }
    return out;
}

// One arm as a pattern with tab stops from `*stop` on, and `=> ` before its
// body's stop.
auto armPattern(const Arm& arm, int* stop) -> std::string {
    const auto next = [&] { return std::to_string((*stop)++); };
    if (arm.type) {
        std::string binder(
            1, static_cast<char>(std::tolower(
                   static_cast<unsigned char>(arm.name.empty() ? 'x' : arm.name.front()))));
        if (const auto dot = arm.name.rfind('.');
            dot != std::string::npos && dot + 1 < arm.name.size()) {
            binder = std::string(
                1, static_cast<char>(std::tolower(static_cast<unsigned char>(arm.name[dot + 1]))));
        }
        return "${" + next() + ":" + binder + "}: " + arm.name;
    }
    if (arm.arity == 0) {
        return arm.name;
    }
    std::string pattern = arm.name + "(";
    for (size_t i = 0; i < arm.arity; ++i) {
        pattern += (i ? ", ${" : "${") + next() + ":_}";
    }
    return pattern + ")";
}

auto armLabel(const Arm& arm) -> std::string {
    if (arm.type) {
        return "_: " + arm.name + " =>";
    }
    std::string label = arm.name;
    if (arm.arity) {
        label += "(";
        for (size_t i = 0; i < arm.arity; ++i) {
            label += i ? ", _" : "_";
        }
        label += ")";
    }
    return label + " =>";
}

// The arms one after another, a line each, the last body the final stop.
auto armLines(const std::vector<Arm>& arms, const std::string& lineStart) -> std::string {
    std::string out;
    int stop = 1;
    for (size_t i = 0; i < arms.size(); ++i) {
        out += lineStart + armPattern(arms[i], &stop) + " => ";
        out += i + 1 == arms.size() ? std::string("$0") : "$" + std::to_string(stop++);
        if (i + 1 < arms.size()) {
            out += "\n";
        }
    }
    return out;
}

} // namespace

auto snippetContextAt(const std::string& text, size_t cursor) -> SnippetContext {
    return walk(text, std::min(cursor, text.size())).context;
}

auto matchSiteAt(const std::string& text, size_t cursor) -> std::optional<MatchSite> {
    return walk(text, std::min(cursor, text.size())).match;
}

auto snippetCompletions(
    const std::string& text, size_t cursor, const std::vector<std::string>& blockParameters)
    -> std::vector<Snippet> {
    cursor = std::min(cursor, text.size());
    const auto wordStart = wordStartBefore(text, cursor);
    const std::string_view word(text.data() + wordStart, cursor - wordStart);
    if (word.empty()) {
        return {};
    }
    const auto walked = walk(text, cursor);
    const auto bit = contextBit(walked.context);
    if (!bit) {
        return {};
    }
    const auto indentation = indentationAt(text, wordStart);

    std::vector<Snippet> snippets;
    // On a line of its own in an `if`: its next branch, the line moved left
    // to the `if`'s own indentation.
    if (walked.context == SnippetContext::Statements && !walked.open.empty() &&
        walked.open.back().opener == TokenType::If) {
        const auto branch = indentationAt(text, walked.open.back().offset);
        for (const auto& [keyword, body] :
             {std::pair{"elif", "elif ${1:condition}\n\t$0"}, std::pair{"else", "else\n\t$0"}}) {
            if (!std::string_view(keyword).starts_with(word)) {
                continue;
            }
            Snippet snippet{
                keyword, indentation.base + keyword,
                std::string(keyword) + " branch of the `if` above",
                branch.base + indented(body, branch), true};
            snippets.push_back(std::move(snippet));
        }
    }
    for (const auto& entry : Entries) {
        if (!(entry.contexts & bit) || !std::string_view(entry.filterText).starts_with(word)) {
            continue;
        }
        // The block the call expects, its parameters named for their types.
        if (!blockParameters.empty() && std::string_view(entry.label) == "do |x|") {
            std::vector<std::string> names;
            std::string label = "do |";
            std::string body = "do |";
            for (size_t i = 0; i < blockParameters.size(); ++i) {
                auto name = blockParameterName(blockParameters[i]);
                const auto base = name;
                for (int n = 2; std::find(names.begin(), names.end(), name) != names.end(); ++n) {
                    name = base + std::to_string(n);
                }
                names.push_back(name);
                label += (i ? ", " : "") + name;
                body += (i ? ", ${" : "${") + std::to_string(i + 1) + ":" + name + "}";
            }
            label += "|";
            body += "|\n\t$0\nend";
            snippets.push_back(
                {label, entry.filterText, "a block taking what the call passes",
                 indented(body, indentation)});
            continue;
        }
        snippets.push_back(
            {entry.label, entry.filterText, entry.detail, indented(entry.body, indentation)});
    }
    return snippets;
}

auto armSnippets(
    const std::string& text, size_t cursor, const MatchSite& site, const std::vector<Arm>& arms)
    -> std::vector<Snippet> {
    if (arms.empty()) {
        return {};
    }
    cursor = std::min(cursor, text.size());
    const auto wordStart = wordStartBefore(text, cursor);
    const std::string_view word(text.data() + wordStart, cursor - wordStart);
    const auto indentation = indentationAt(text, wordStart);
    std::vector<Snippet> snippets;
    if (site.header) {
        if (!std::string_view("do").starts_with(word)) {
            return {};
        }
        // The arms one step in from the line `match` is on, `end` level with it.
        std::string body = "do\n";
        body += armLines(arms, indentation.base + indentation.unit);
        body += "\n" + indentation.base + "end";
        snippets.push_back(
            {"do with every arm", "do", "match arms for every case", std::move(body)});
        return snippets;
    }
    // Written where the cursor is: the first line at the cursor, the others
    // at its line's indentation.
    for (const auto& arm : arms) {
        if (!arm.name.starts_with(word) && !arm.type) {
            continue;
        }
        int stop = 1;
        snippets.push_back(
            {armLabel(arm), arm.type ? std::string(word) : arm.name, "missing match arm",
             armPattern(arm, &stop) + " => $0"});
    }
    if (arms.size() > 1) {
        auto body = armLines(arms, indentation.base);
        body.erase(0, indentation.base.size());
        snippets.push_back(
            {"every missing arm", arms.front().type ? std::string(word) : arms.front().name,
             "match arms for every missing case", std::move(body)});
    }
    return snippets;
}

namespace {

// A parameter name for a value of type `type`: `other` for the implementor's
// own type, else the type's initial.
auto parameterName(const std::string& type) -> std::string {
    if (type == names::kThisType) {
        return "other";
    }
    const auto dot = type.rfind('.');
    const auto leaf = dot == std::string::npos ? type : type.substr(dot + 1);
    if (!leaf.empty() && std::isalpha(static_cast<unsigned char>(leaf.front()))) {
        return std::string(
            1, static_cast<char>(std::tolower(static_cast<unsigned char>(leaf.front()))));
    }
    return "x";
}

// One stub, its tab stops from `*stop` on: `let area = $1` for a requirement
// taking nothing, otherwise a `do … end` body.
auto stubBody(const Stub& stub, int* stop) -> std::string {
    const auto next = [&] { return std::to_string((*stop)++); };
    std::string out = (stub.foul ? "foul " : "let ") + stub.name;
    if (stub.params.empty() && !stub.foul) {
        return out + " = $" + next();
    }
    if (!stub.params.empty()) {
        std::vector<std::string> names;
        out += "(";
        for (size_t i = 0; i < stub.params.size(); ++i) {
            auto name = parameterName(stub.params[i]);
            const auto base = name;
            for (int n = 2; std::find(names.begin(), names.end(), name) != names.end(); ++n) {
                name = base + std::to_string(n);
            }
            names.push_back(name);
            out += (i ? ", ${" : "${") + next() + ":" + name + "}";
        }
        out += ")";
    }
    if (!stub.result.empty()) {
        out += " -> " + stub.result;
    }
    return out + " do\n\t$" + next() + "\nend";
}

auto stubLabel(const Stub& stub) -> std::string {
    std::string label = (stub.foul ? "foul " : "let ") + stub.name;
    if (!stub.params.empty()) {
        label += "(";
        for (size_t i = 0; i < stub.params.size(); ++i) {
            label += (i ? ", " : "") + stub.params[i];
        }
        label += ")";
    }
    if (!stub.result.empty()) {
        label += " -> " + stub.result;
    }
    return label;
}

// Every stub, a blank line between those with a body, the last stop final.
auto stubLines(const std::vector<Stub>& stubs) -> std::string {
    std::string out;
    int stop = 1;
    for (size_t i = 0; i < stubs.size(); ++i) {
        if (i) {
            out += stubs[i].params.empty() && !stubs[i].foul && stubs[i - 1].params.empty() &&
                           !stubs[i - 1].foul
                       ? "\n"
                       : "\n\n";
        }
        out += stubBody(stubs[i], &stop);
    }
    const auto last = out.rfind("$" + std::to_string(stop - 1));
    if (last != std::string::npos) {
        out.replace(last, 1 + std::to_string(stop - 1).size(), "$0");
    }
    return out;
}

} // namespace

auto makeSiteAt(const std::string& text, size_t cursor) -> std::optional<MakeSite> {
    return walk(text, std::min(cursor, text.size())).make;
}

auto stubSnippets(
    const std::string& text, size_t cursor, const MakeSite& site, const std::vector<Stub>& stubs)
    -> std::vector<Snippet> {
    if (stubs.empty()) {
        return {};
    }
    cursor = std::min(cursor, text.size());
    const auto wordStart = wordStartBefore(text, cursor);
    const std::string_view word(text.data() + wordStart, cursor - wordStart);
    const auto indentation = indentationAt(text, wordStart);
    std::vector<Snippet> snippets;
    if (site.header) {
        if (!std::string_view("do").starts_with(word)) {
            return {};
        }
        // The stubs one step in from the `make` line, `end` level with it.
        auto inner = indentation;
        inner.base += inner.unit;
        snippets.push_back(
            {"do with every required function", "do", "the functions the trait requires",
             "do\n" + inner.base + indented(stubLines(stubs), inner) + "\n" + indentation.base +
                 "end"});
        return snippets;
    }
    // A stub is written from `let` or `foul`, or from its own name.
    for (const auto& stub : stubs) {
        const std::string keyword = stub.foul ? "foul" : "let";
        const auto filter = keyword.starts_with(word) ? keyword : stub.name;
        if (!filter.starts_with(word)) {
            continue;
        }
        int stop = 1;
        auto body = stubBody(stub, &stop);
        const auto last = body.rfind("$" + std::to_string(stop - 1));
        body.replace(last, 1 + std::to_string(stop - 1).size(), "$0");
        snippets.push_back(
            {stubLabel(stub), filter, "required by its trait", indented(body, indentation)});
    }
    if (stubs.size() > 1 && !snippets.empty()) {
        snippets.push_back(
            {"every required function", snippets.front().filterText,
             "the functions the trait requires", indented(stubLines(stubs), indentation)});
    }
    return snippets;
}

auto endOnEnter(const std::string& text, size_t cursor, const std::string& unit)
    -> std::optional<EnterEdit> {
    cursor = std::min(cursor, text.size());
    const auto lineStart = lineStartOf(text, cursor);
    const auto lineEnd = std::min(text.find('\n', lineStart), text.size());
    const auto blank = [&](size_t from, size_t to) {
        return std::all_of(
            text.begin() + static_cast<std::ptrdiff_t>(from),
            text.begin() + static_cast<std::ptrdiff_t>(to),
            [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
    };
    if (lineStart == 0 || !blank(lineStart, lineEnd)) {
        return std::nullopt;
    }
    const auto previousStart = lineStartOf(text, lineStart - 1);
    if (blank(previousStart, lineStart - 1)) {
        return std::nullopt;
    }
    // The innermost block open here must be one the line above opened.
    const auto here = walk(text, cursor);
    if (here.open.empty()) {
        return std::nullopt;
    }
    const auto& opened = here.open.back();
    if (opened.block == Block::Bracket || opened.offset < previousStart ||
        opened.offset >= lineStart) {
        return std::nullopt;
    }
    // And some block in the file must still lack its `end`. An unclosed
    // block takes the `end` after it, so it is an enclosing one that shows
    // up open at the end of the file, not necessarily this one.
    const auto whole = walk(text + "\n", text.size() + 1);
    if (std::none_of(whole.open.begin(), whole.open.end(), [](const Open& block) {
            return block.block != Block::Bracket;
        })) {
        return std::nullopt;
    }
    const auto opener = indentationAt(text, opened.offset);
    EnterEdit edit;
    edit.lineStart = lineStart;
    edit.lineEnd = lineEnd;
    edit.indent = opener.base + unit;
    if (lineEnd < text.size()) {
        edit.closingAt = lineEnd + 1;
        edit.closing = opener.base + "end\n";
    } else {
        edit.closingAt = lineEnd;
        edit.closing = "\n" + opener.base + "end";
    }
    return edit;
}

auto closingEndOf(const std::string& text, size_t openerOffset) -> std::optional<size_t> {
    std::optional<size_t> closing;
    walk(text + "\n", text.size() + 1, [&](const Open& block, size_t end) {
        if (!closing && block.offset == openerOffset) {
            closing = end;
        }
    });
    if (!closing) {
        return std::nullopt;
    }
    const auto start = lineStartOf(text, *closing);
    if (!std::all_of(
            text.begin() + static_cast<std::ptrdiff_t>(start),
            text.begin() + static_cast<std::ptrdiff_t>(*closing),
            [](char c) { return c == ' ' || c == '\t'; })) {
        return std::nullopt;
    }
    return closing;
}

auto armSource(const std::vector<Arm>& arms, const std::string& indent) -> std::string {
    std::string out;
    for (const auto& arm : arms) {
        out += indent;
        if (arm.type) {
            out += "_: " + arm.name;
        } else {
            out += arm.name;
            if (arm.arity) {
                out += "(";
                for (size_t i = 0; i < arm.arity; ++i) {
                    out += i ? ", _" : "_";
                }
                out += ")";
            }
        }
        out += " => die(\"" + arm.name + " is not handled yet\")\n";
    }
    return out;
}

auto lineIndentation(const std::string& text, size_t offset) -> std::string {
    return indentationAt(text, std::min(offset, text.size())).base;
}

auto indentStep(const std::string& text) -> std::string {
    // How far the line after each block opener steps in from the opener's
    // line. Only those lines: a continuation (a union's `| Variant` aligned
    // under the first, a multi-line string) is not a step.
    const auto width = [&](size_t lineStart) {
        size_t at = lineStart;
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) {
            ++at;
        }
        return at;
    };
    std::map<std::string, int> steps;
    const auto measure = [&](const Open& block) {
        if (block.block == Block::Bracket) {
            return;
        }
        const auto openerStart = lineStartOf(text, block.offset);
        const auto openerIndent = width(openerStart);
        // The first line below the opener holding something.
        auto next = text.find('\n', block.offset);
        while (next != std::string::npos) {
            const auto lineStart = next + 1;
            const auto indent = width(lineStart);
            if (indent < text.size() && text[indent] != '\n' && text[indent] != '\r') {
                if (text[indent] == '#') {
                    next = text.find('\n', lineStart);
                    continue;
                }
                const auto inner = indent - lineStart;
                const auto outer = openerIndent - openerStart;
                if (inner > outer &&
                    text.compare(lineStart, outer, text, openerStart, outer) == 0) {
                    steps[text.substr(lineStart + outer, inner - outer)]++;
                }
                return;
            }
            next = text.find('\n', lineStart);
        }
    };
    const auto walked =
        walk(text + "\n", text.size() + 1, [&](const Open& block, size_t) { measure(block); });
    for (const auto& block : walked.open) {
        measure(block);
    }
    std::string best = "  ";
    int count = 0;
    for (const auto& [step, n] : steps) {
        if (n > count) {
            best = step;
            count = n;
        }
    }
    return best;
}

} // namespace kex::lsp
