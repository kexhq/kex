#include "snippets.hxx"

#include "../lexer/lexer.hxx"

#include <algorithm>
#include <cctype>
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

// The indentation one level adds in this file: the first step inward any
// line takes from the one before it. Two spaces when nothing is indented.
auto indentUnit(const std::string& text) -> std::string {
    size_t previous = 0;
    bool havePrevious = false;
    size_t at = 0;
    while (at < text.size()) {
        const auto end = std::min(text.find('\n', at), text.size());
        size_t indent = at;
        while (indent < end && (text[indent] == ' ' || text[indent] == '\t')) {
            ++indent;
        }
        if (indent < end && text[indent] != '#') {
            const auto width = indent - at;
            if (havePrevious && width > previous) {
                if (text[at] == '\t') {
                    return "\t";
                }
                return std::string(width - previous, ' ');
            }
            previous = width;
            havePrevious = true;
        }
        at = end + 1;
    }
    return "  ";
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
struct Walk {
    SnippetContext context = SnippetContext::None;
    std::optional<MatchSite> match;
};

auto walk(const std::string& text, size_t cursor) -> Walk {
    const auto wordStart = wordStartBefore(text, cursor);
    if (wordStart > 0 && (text[wordStart - 1] == '.' || text[wordStart - 1] == '@' ||
                          text[wordStart - 1] == ':' || text[wordStart - 1] == '%')) {
        return {};
    }

    Lexer lexer(text);
    const auto tokens = lexer.tokenizeAll();

    // An open block, the keyword that says what kind it is, and where that
    // keyword is.
    struct Open {
        Block block;
        TokenType opener = TokenType::Eof;
        size_t offset = 0;
    };
    std::vector<Open> stack;
    const auto current = [&] { return stack.empty() ? Block::TopLevel : stack.back().block; };
    const Token* previous = nullptr; // the last token before the word on its line
    TokenType classifier = TokenType::Eof;
    size_t classifierOffset = 0;
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
                continue;
            case TokenType::Do:
                stack.push_back(
                    {blockOpenedBy(classifier, current()), classifier, classifierOffset});
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
}

// The cursor line's indentation, and the file's step inward.
struct Indentation {
    std::string base;
    std::string unit;
};

auto indentationAt(const std::string& text, size_t wordStart) -> Indentation {
    const auto lineStart = lineStartOf(text, wordStart);
    auto indentEnd = lineStart;
    while (indentEnd < text.size() && (text[indentEnd] == ' ' || text[indentEnd] == '\t')) {
        ++indentEnd;
    }
    return {text.substr(lineStart, indentEnd - lineStart), indentUnit(text)};
}

// `body` with each `\t` one indentation step and each line after the first
// at the cursor line's indentation.
auto indented(std::string_view body, const Indentation& indentation) -> std::string {
    std::string out;
    for (const char c : body) {
        if (c == '\t') {
            out += indentation.unit;
        } else if (c == '\n') {
            out += '\n';
            out += indentation.base;
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

auto snippetCompletions(const std::string& text, size_t cursor) -> std::vector<Snippet> {
    cursor = std::min(cursor, text.size());
    const auto wordStart = wordStartBefore(text, cursor);
    const std::string_view word(text.data() + wordStart, cursor - wordStart);
    if (word.empty()) {
        return {};
    }
    const auto bit = contextBit(snippetContextAt(text, cursor));
    if (!bit) {
        return {};
    }
    const auto indentation = indentationAt(text, wordStart);

    std::vector<Snippet> snippets;
    for (const auto& entry : Entries) {
        if (!(entry.contexts & bit) || !std::string_view(entry.filterText).starts_with(word)) {
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

} // namespace kex::lsp
