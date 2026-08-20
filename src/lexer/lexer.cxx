#include "lexer.hxx"
#include "../common/unicode_category.hxx"
#include "../common/utf8.hxx"
#include <algorithm>
#include <cctype>
#include <gmpxx.h>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace kex {

static const std::unordered_map<std::string, TokenType> keywords = {
    {"after", TokenType::After},
    {"break", TokenType::Break},
    {"compiled", TokenType::Compiled},
    {"do", TokenType::Do},
    {"elif", TokenType::Elif},
    {"else", TokenType::Else},
    {"end", TokenType::End},
    {"export", TokenType::Export},
    {"false", TokenType::False},
    {"final", TokenType::Final},
    {"foul", TokenType::Foul},
    {"if", TokenType::If},
    {"let", TokenType::Let},
    {"loop", TokenType::Loop},
    {"main", TokenType::Main},
    {"make", TokenType::Make},
    {"match", TokenType::Match},
    {"module", TokenType::Module},
    {"next", TokenType::Next},
    {"private", TokenType::Private},
    {"public", TokenType::Public},
    {"receive", TokenType::Receive},
    {"record", TokenType::Record},
    {"rescue", TokenType::Rescue},
    {"return", TokenType::Return},
    {"spawn", TokenType::Spawn},
    {"then", TokenType::Then},
    {"this", TokenType::This},
    {"timeout", TokenType::Timeout},
    {"trait", TokenType::Trait},
    {"true", TokenType::True},
    {"try", TokenType::Try},
    {"trying", TokenType::Trying},
    {"type", TokenType::Type},
    {"using", TokenType::Using},
    {"var", TokenType::Var},
    {"when", TokenType::When},
    {"while", TokenType::While},
};

// Every SourceLocation keeps a `string_view` of the file name, and an AST
// routinely outlives the string its caller passed in — the prelude's most of
// all, parsed from a local `std::string` that is gone long before a prelude
// location is ever printed. Reading one then produced garbage instead of a
// path: `Internal error: !\xef\xbf\xbd.$\xef\xbf\xbdh1` for
// `(1..5).count`, whose error is raised inside `enumerable.kex`.
//
// Interning the name for the process lifetime makes every location printable.
// The set is node-based, so the views stay valid as it grows.
static auto internFilename(std::string_view filename) -> std::string_view {
    static std::unordered_set<std::string> pool;
    return *pool.emplace(filename).first;
}

Lexer::Lexer(std::string source, std::string_view filename)
    : m_source(std::move(source)), m_filename(internFilename(filename)) {}

// Can a token of this type END an expression? If so, a following `%` is the
// modulo operator; otherwise `%name` is a splice. Without this, `x %y` lexes
// the operand as a SpliceIdent and unspaced modulo fails to parse.
static auto canEndExpression(TokenType type) -> bool {
    switch (type) {
        case TokenType::LowerIdent:
        case TokenType::UpperIdent:
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
        case TokenType::End:
        case TokenType::True:
        case TokenType::False:
        case TokenType::None:
        case TokenType::This:
            return true;
        default:
            return false;
    }
}

auto Lexer::nextToken() -> Token {
    Token token = scanToken();
    m_prevType = token.type;
    return token;
}

auto Lexer::scanToken() -> Token {
    skipWhitespace();

    if (atEnd()) {
        return makeToken(TokenType::Eof);
    }

    m_tokenStartLine = m_line;
    m_tokenStartColumn = m_column;
    m_tokenStartOffset = m_pos;

    char c = advance();

    if (isLowerAlpha(c) || c == '_') return lexIdentifier();
    if (isUpperAlpha(c)) {
        std::string ident(1, c);
        consumeIdentRest(ident);
        if (ident == "None") return makeToken(TokenType::None, ident);
        return makeToken(TokenType::UpperIdent, ident);
    }
    // A name written in another script. Kex tells values from types by the
    // case of the first letter, and Unicode has that for the scripts that
    // make the distinction: `α` is a value, `Α` a type. A script without
    // case (Chinese, Arabic, …) has no uppercase form, so its names read as
    // values — the useful default, since types are the rarer declaration.
    if (static_cast<unsigned char>(c) >= 0x80) {
        int width = 1;
        const auto codepoint = identCodepointAt(m_pos - 1, width);
        if (utf8::categories::inRanges(utf8::categories::letterRanges,
                                       codepoint)) {
            std::string ident(1, c);
            for (int i = 1; i < width && !atEnd(); ++i) ident += advance();
            consumeIdentRest(ident);
            const bool upper = utf8::categories::inRanges(
                utf8::categories::upperRanges, codepoint);
            return makeToken(
                upper ? TokenType::UpperIdent : TokenType::LowerIdent, ident);
        }
    }
    if (isDigit(c)) return lexNumber();

    switch (c) {
        // Inside parens, newlines are insignificant whitespace — this is
        // what lets multiline conditions like `if (a\n && b)` work without
        // a continuation marker.
        case '\n':
            if (m_parenDepth > 0) return nextToken();
            return makeToken(TokenType::Newline);

        case '(': m_parenDepth++; return makeToken(TokenType::LParen);
        case ')':
            if (m_parenDepth > 0) m_parenDepth--;
            return makeToken(TokenType::RParen);
        case '{': return makeToken(TokenType::LBrace);
        case '}': return makeToken(TokenType::RBrace);
        case '[': return makeToken(TokenType::LBracket);
        case ']': return makeToken(TokenType::RBracket);
        case ',': return makeToken(TokenType::Comma);
        case '@': return makeToken(TokenType::At);
        case '`': return lexRawString();
        case '?':
            return makeToken(TokenType::Question);
        case '/': return makeToken(TokenType::Slash);
        case '*': return makeToken(TokenType::Star);
        case '+':
            return makeToken(TokenType::Plus);

        case '-':
            if (match('>')) return makeToken(TokenType::Arrow);
            return makeToken(TokenType::Minus);

        case '.':
            if (match('.')) {
                if (match('.')) return makeToken(TokenType::DotDotDot);
                return makeToken(TokenType::DotDot);
            }
            return makeToken(TokenType::Dot);

        case '=':
            if (match('=')) return makeToken(TokenType::EqEq);
            if (match('>')) return makeToken(TokenType::FatArrow);
            return makeToken(TokenType::Equals);

        case '!':
            if (match('=')) return makeToken(TokenType::NotEq);
            return makeToken(TokenType::Bang);

        case '<':
            if (match('=')) return makeToken(TokenType::LessEq);
            return makeToken(TokenType::LessThan);

        case '>':
            if (match('=')) return makeToken(TokenType::GreaterEq);
            return makeToken(TokenType::GreaterThan);

        case '|':
            if (match('|')) return makeToken(TokenType::PipePipe);
            return makeToken(TokenType::Pipe);

        case '&':
            if (match('&')) return makeToken(TokenType::AmpAmp);
            return makeToken(TokenType::Amp);

        case ':':
            if (match('>')) return makeToken(TokenType::TypeAnnotation);
            if (isLowerAlpha(peek())) return lexAtom();
            return makeToken(TokenType::Colon);

        case '^': return makeToken(TokenType::Caret);

        case ';': return makeToken(TokenType::Newline);

        case '#':
            if (match('[')) return makeToken(TokenType::HashLBracket);
            skipComment();
            return nextToken();

        case '~': return makeToken(TokenType::Tilde);

        case '%':
            // `%name` is a splice only where an expression could START. After
            // something that can end an expression, `%` is modulo — so `x %y`
            // is `x % y`, not `x` followed by a splice.
            if (isLowerAlpha(peek()) && !canEndExpression(m_prevType))
                return lexSpliceIdent();
            return makeToken(TokenType::Percent);

        case '"': return lexString();
        case '\'': return lexChar();
        case '$':
            if (peek() == '`') {
                advance();
                return lexRawString(true);
            }
            return errorToken("Unexpected character: $");

        default:
            return errorToken(std::string("Unexpected character: ") + c);
    }
}

auto Lexer::tokenizeAll() -> std::vector<Token> {
    std::vector<Token> tokens;
    while (true) {
        auto token = nextToken();
        bool stop = token.type == TokenType::Eof || token.type == TokenType::Error;
        tokens.push_back(token);
        if (stop) {
            break;
        }
    }
    if (tokens.back().type != TokenType::Eof) {
        tokens.push_back(makeToken(TokenType::Eof));
    }
    return tokens;
}

auto Lexer::peek() const -> char {
    if (atEnd()) return '\0';
    return m_source[m_pos];
}

auto Lexer::peekNext() const -> char {
    if (m_pos + 1 >= static_cast<int>(m_source.size())) return '\0';
    return m_source[m_pos + 1];
}

auto Lexer::peekAt(int offset) const -> char {
    if (m_pos + offset >= static_cast<int>(m_source.size())) return '\0';
    return m_source[m_pos + offset];
}

auto Lexer::advance() -> char {
    char c = m_source[m_pos++];
    if (c == '\n') {
        m_line++;
        m_column = 1;
    } else {
        m_column++;
    }
    return c;
}

auto Lexer::atEnd() const -> bool {
    return m_pos >= static_cast<int>(m_source.size());
}

auto Lexer::match(char expected) -> bool {
    if (atEnd() || m_source[m_pos] != expected) return false;
    advance();
    return true;
}

auto Lexer::currentLocation() const -> SourceLocation {
    return SourceLocation{m_filename, m_tokenStartLine, m_tokenStartColumn};
}

auto Lexer::skipWhitespace() -> void {
    while (!atEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else {
            break;
        }
    }
}

auto Lexer::skipComment() -> void {
    while (!atEnd() && peek() != '\n') {
        advance();
    }
}

auto Lexer::lexIdentifier() -> Token {
    std::string ident(1, m_source[m_pos - 1]);

    consumeIdentRest(ident);

    if (!atEnd() && peek() == '?') {
        ident += advance();
    }

    if (ident == "_") return makeToken(TokenType::Underscore, ident);

    auto it = keywords.find(ident);
    if (it != keywords.end()) {
        return makeToken(it->second, ident);
    }

    return makeToken(TokenType::LowerIdent, ident);
}

auto Lexer::lexNumber() -> Token {
    char first = m_source[m_pos - 1];

    // Radix literals are normalized to their decimal spelling here rather than
    // carried through as `0x…`: every consumer downstream (stoll/mpz_class in
    // the evaluator, literal patterns, Core Erlang literal emission) already
    // understands base 10 and nothing else. The original text stays
    // recoverable from the token's source offsets.
    if (first == '0' && (peek() == 'x' || peek() == 'X' ||
                         peek() == 'b' || peek() == 'B' ||
                         peek() == 'o' || peek() == 'O')) {
        char marker = static_cast<char>(std::tolower(static_cast<unsigned char>(peek())));
        int base = marker == 'x' ? 16 : marker == 'o' ? 8 : 2;
        const char* what = marker == 'x' ? "hexadecimal"
                         : marker == 'o' ? "octal" : "binary";
        advance();
        std::string digits;
        while (!atEnd() && (isHexDigit(peek()) || peek() == '_')) {
            if (peek() == '_') {
                advance();
                continue;
            }
            digits += advance();
        }
        if (digits.empty()) {
            return errorToken(std::string("Expected ") + what + " digits after '0" + marker + "'");
        }
        // mpz_class rejects out-of-range digits (`0b12`, `0o88`) by throwing.
        try {
            return makeToken(TokenType::Integer, mpz_class(digits, base).get_str());
        } catch (const std::invalid_argument&) {
            return errorToken("'" + digits + "' is not a valid " + what + " literal");
        }
    }

    std::string num(1, first);
    bool isFloat = false;

    while (!atEnd() && (isDigit(peek()) || peek() == '_')) {
        if (peek() == '_') {
            advance();
            continue;
        }
        num += advance();
    }

    if (!atEnd() && peek() == '.' && isDigit(peekNext())) {
        isFloat = true;
        num += advance();
        while (!atEnd() && (isDigit(peek()) || peek() == '_')) {
            if (peek() == '_') {
                advance();
                continue;
            }
            num += advance();
        }
    }

    // Exponent (3e22, 1.5E-4). Only consumed when a well-formed exponent
    // actually follows, so `2.each` and a bare `2e` still lex as a number
    // followed by an identifier. An exponent makes the literal a Float even
    // without a fraction part.
    bool hasExponent = (peek() == 'e' || peek() == 'E') &&
        (isDigit(peekNext()) ||
         ((peekNext() == '+' || peekNext() == '-') && isDigit(peekAt(2))));
    if (hasExponent) {
        isFloat = true;
        // Core Erlang (and Erlang) require a fraction part before the
        // exponent, so `3e22` has to become `3.0e22`. Lowercased for the same
        // reason: `3.0E22` is not a valid Erlang float literal.
        if (num.find('.') == std::string::npos) num += ".0";
        num += 'e';
        advance();
        if (peek() == '+' || peek() == '-') num += advance();
        while (!atEnd() && (isDigit(peek()) || peek() == '_')) {
            if (peek() == '_') {
                advance();
                continue;
            }
            num += advance();
        }
    }

    return makeToken(isFloat ? TokenType::Float : TokenType::Integer, num);
}

auto Lexer::lexString() -> Token {
    std::string str;
    while (!atEnd() && peek() != '"') {
        if (peek() == '\\') {
            advance();
            if (atEnd()) return errorToken("Unterminated string escape");
            char escaped = advance();
            switch (escaped) {
                case 'n': str += '\n'; break;
                case 'r': str += '\r'; break;
                case 't': str += '\t'; break;
                case '\\': str += '\\'; break;
                case '"': str += '"'; break;
                case '$': str += '$'; break;
                default: str += escaped; break;
            }
        } else if (peek() == '$' && peekNext() == '{') {
            str += advance(); // $
            str += advance(); // {
            int depth = 1;
            while (!atEnd() && depth > 0) {
                if (peek() == '{') depth++;
                else if (peek() == '}') depth--;
                if (depth > 0) {
                    str += advance();
                } else {
                    str += advance(); // closing }
                }
            }
        } else {
            str += advance();
        }
    }

    if (atEnd()) return errorToken("Unterminated string");
    advance(); // closing "

    return makeToken(TokenType::String, str);
}

auto Lexer::lexRawString(bool interpolating) -> Token {
    std::string raw;
    enum class HoleMode { Normal, DoubleString, Char, RawString, Comment };
    HoleMode holeMode = HoleMode::Normal;
    int holeDepth = 0;
    while (!atEnd()) {
        if (interpolating && holeDepth > 0) {
            char c = peek();
            if (holeMode == HoleMode::DoubleString) {
                raw += advance();
                if (c == '\\' && !atEnd()) raw += advance();
                else if (c == '"') holeMode = HoleMode::Normal;
                continue;
            }
            if (holeMode == HoleMode::Char) {
                raw += advance();
                if (c == '\\' && !atEnd()) raw += advance();
                else if (c == '\'') holeMode = HoleMode::Normal;
                continue;
            }
            if (holeMode == HoleMode::RawString) {
                raw += advance();
                if (c == '`') {
                    if (!atEnd() && peek() == '`')
                        raw += advance();
                    else
                        holeMode = HoleMode::Normal;
                }
                continue;
            }
            if (holeMode == HoleMode::Comment) {
                raw += advance();
                if (c == '\n') holeMode = HoleMode::Normal;
                continue;
            }

            raw += advance();
            if (c == '"') holeMode = HoleMode::DoubleString;
            else if (c == '\'') holeMode = HoleMode::Char;
            else if (c == '`') holeMode = HoleMode::RawString;
            else if (c == '#') holeMode = HoleMode::Comment;
            else if (c == '{') holeDepth++;
            else if (c == '}') holeDepth--;
            continue;
        }

        if (interpolating && peek() == '$' &&
            m_pos + 2 < static_cast<int>(m_source.size()) &&
            m_source[m_pos + 1] == '$' && m_source[m_pos + 2] == '{') {
            raw += advance();
            raw += advance();
            raw += advance();
            continue;
        }
        if (interpolating && peek() == '$' && peekNext() == '{') {
            raw += advance();
            raw += advance();
            holeDepth = 1;
            holeMode = HoleMode::Normal;
            continue;
        }
        if (peek() == '`') {
            // A doubled delimiter is one literal backtick. Backslashes never
            // participate in raw-literal escaping.
            if (peekNext() == '`') {
                advance();
                advance();
                raw += '`';
                continue;
            }
            advance(); // closing backtick

            bool opensWithNewline = raw.starts_with("\n") ||
                                    raw.starts_with("\r\n");
            if (opensWithNewline) {
                raw.erase(0, raw.starts_with("\r\n") ? 2 : 1);
            }

            // A closing delimiter on an otherwise-empty line contributes its
            // exact leading whitespace as the dedent prefix. The prefix is
            // syntax, not part of the resulting string; the preceding newline
            // remains, so block-shaped literals end in '\n'.
            auto lastNewline = raw.rfind('\n');
            if (lastNewline != std::string::npos) {
                std::string margin = raw.substr(lastNewline + 1);
                bool closingOnOwnLine = std::all_of(
                    margin.begin(), margin.end(),
                    [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
                if (closingOnOwnLine) {
                    if (!margin.empty() && margin.back() == '\r')
                        margin.pop_back();
                    bool hasSpace = margin.find(' ') != std::string::npos;
                    bool hasTab = margin.find('\t') != std::string::npos;
                    if (hasSpace && hasTab)
                        return errorToken(
                            "Backtick literal closing margin cannot mix spaces and tabs");

                    raw.erase(lastNewline + 1);
                    if (!margin.empty()) {
                        std::string dedented;
                        size_t lineStart = 0;
                        while (lineStart < raw.size()) {
                            auto lineEnd = raw.find('\n', lineStart);
                            bool hasNewline = lineEnd != std::string::npos;
                            if (!hasNewline) lineEnd = raw.size();
                            auto line = raw.substr(lineStart, lineEnd - lineStart);

                            auto contentEnd = line.size();
                            if (contentEnd > 0 && line[contentEnd - 1] == '\r')
                                contentEnd--;
                            bool blank = std::all_of(
                                line.begin(), line.begin() + contentEnd,
                                [](char c) { return c == ' ' || c == '\t'; });
                            if (blank) {
                                if (contentEnd < line.size()) dedented += '\r';
                            } else if (line.starts_with(margin)) {
                                dedented += line.substr(margin.size());
                            } else {
                                return errorToken(
                                    "Backtick literal line is less indented than its closing margin");
                            }

                            if (hasNewline) dedented += '\n';
                            lineStart = lineEnd + (hasNewline ? 1 : 0);
                        }
                        raw = std::move(dedented);
                    }
                }
            }

            return makeToken(
                interpolating ? TokenType::InterpolatedRawString
                              : TokenType::RawString,
                std::move(raw));
        }
        raw += advance();
    }

    return errorToken("Unterminated backtick literal");
}

auto Lexer::lexChar() -> Token {
    if (atEnd()) return errorToken("Unterminated char literal");

    // The token carries the character's UTF-8 BYTES; a Char is one codepoint,
    // so a non-ASCII one is several of them. Taking a single byte here made
    // `'α'` an unterminated literal — the closing quote was still two bytes
    // away — so every char literal outside ASCII was a syntax error.
    std::string c;
    if (peek() == '\\') {
        advance();
        if (atEnd()) return errorToken("Unterminated char literal escape");
        char escaped = advance();
        switch (escaped) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '0': c = '\0'; break;
            default: c = escaped; break;
        }
    } else if (peek() == '\'') {
        return errorToken("Empty char literal");
    } else {
        c += advance();
        // Continuation bytes (10xxxxxx) belong to the same character.
        while (!atEnd() && (static_cast<unsigned char>(peek()) & 0xC0) == 0x80)
            c += advance();
    }

    if (atEnd() || peek() != '\'') return errorToken("Unterminated char literal");
    advance(); // closing '

    return makeToken(TokenType::Char, std::move(c));
}

auto Lexer::lexAtom() -> Token {
    std::string atom;
    while (!atEnd() && (isLowerAlpha(peek()) || isDigit(peek()) || peek() == '_')) {
        atom += advance();
    }
    return makeToken(TokenType::Atom, atom);
}

auto Lexer::lexSpliceIdent() -> Token {
    std::string ident;
    // isIdentChar, not lowercase-only: a splice names a compile-time variable,
    // and those are camelCase like any other. `%vecName` used to stop at `vec`.
    while (!atEnd() && isIdentChar(peek())) {
        ident += advance();
    }
    return makeToken(TokenType::SpliceIdent, ident);
}

auto Lexer::makeToken(TokenType type) -> Token {
    auto location = currentLocation();
    location.startOffset = m_tokenStartOffset;
    location.endOffset = m_pos;
    return Token{type, "", location, m_tokenStartOffset, m_pos};
}

auto Lexer::makeToken(TokenType type, std::string value) -> Token {
    auto location = currentLocation();
    location.startOffset = m_tokenStartOffset;
    location.endOffset = m_pos;
    return Token{type, std::move(value), location, m_tokenStartOffset, m_pos};
}

auto Lexer::errorToken(std::string message) -> Token {
    auto location = currentLocation();
    location.startOffset = m_tokenStartOffset;
    location.endOffset = m_pos;
    return Token{TokenType::Error, std::move(message), location,
                 m_tokenStartOffset, m_pos};
}

auto Lexer::isLowerAlpha(char c) -> bool {
    return c >= 'a' && c <= 'z';
}

auto Lexer::isUpperAlpha(char c) -> bool {
    return c >= 'A' && c <= 'Z';
}

auto Lexer::isAlpha(char c) -> bool {
    return isLowerAlpha(c) || isUpperAlpha(c);
}

auto Lexer::isDigit(char c) -> bool {
    return c >= '0' && c <= '9';
}

auto Lexer::isHexDigit(char c) -> bool {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

auto Lexer::isIdentChar(char c) -> bool {
    return isAlpha(c) || isDigit(c) || c == '_';
}

// The codepoint starting at `index`, with the byte width of its sequence. A
// malformed sequence reads as one byte of U+FFFD, which no category accepts —
// the scanner then reports it as an unexpected character instead of looping.
auto Lexer::identCodepointAt(int index, int& width) const -> char32_t {
    std::string_view source(m_source);
    const auto start = static_cast<std::size_t>(index);
    if (start >= source.size()) {
        width = 1;
        return 0xFFFD;
    }
    const auto span = utf8::sequenceLength(source, start);
    width = static_cast<int>(span);
    const auto decoded = utf8::decode(source.substr(start, span));
    return decoded.empty() ? 0xFFFD : decoded.front();
}

// Whether the identifier continues at `index`: ASCII letters, digits and `_`
// as before, plus any letter codepoint. Marks and non-letter symbols stop it,
// so `a→b` stays three tokens.
auto Lexer::identContinues(int index, int& width) const -> bool {
    width = 1;
    if (index >= static_cast<int>(m_source.size())) return false;
    const auto byte = static_cast<unsigned char>(m_source[index]);
    if (byte < 0x80) return isIdentChar(static_cast<char>(byte));
    const auto codepoint = identCodepointAt(index, width);
    return utf8::categories::inRanges(utf8::categories::letterRanges,
                                      codepoint);
}

auto Lexer::consumeIdentRest(std::string& ident) -> void {
    int width = 1;
    while (!atEnd() && identContinues(m_pos, width))
        for (int i = 0; i < width && !atEnd(); ++i) ident += advance();
}

} // namespace kex
