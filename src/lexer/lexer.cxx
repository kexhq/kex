#include "lexer.hxx"
#include <algorithm>
#include <unordered_map>

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
    {"static", TokenType::Static},
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

Lexer::Lexer(std::string source, std::string_view filename)
    : m_source(std::move(source)), m_filename(filename) {}

auto Lexer::nextToken() -> Token {
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
        while (!atEnd() && isIdentChar(peek())) {
            ident += advance();
        }
        if (ident == "None") return makeToken(TokenType::None, ident);
        return makeToken(TokenType::UpperIdent, ident);
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
            if (isLowerAlpha(peek())) return lexSpliceIdent();
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

    while (!atEnd() && isIdentChar(peek())) {
        ident += advance();
    }

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
    std::string num(1, m_source[m_pos - 1]);
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

    char c;
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
        c = advance();
    }

    if (atEnd() || peek() != '\'') return errorToken("Unterminated char literal");
    advance(); // closing '

    return makeToken(TokenType::Char, std::string(1, c));
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
    while (!atEnd() && (isLowerAlpha(peek()) || isDigit(peek()) || peek() == '_')) {
        ident += advance();
    }
    return makeToken(TokenType::SpliceIdent, ident);
}

auto Lexer::makeToken(TokenType type) -> Token {
    return Token{type, "", currentLocation(), m_tokenStartOffset, m_pos};
}

auto Lexer::makeToken(TokenType type, std::string value) -> Token {
    return Token{type, std::move(value), currentLocation(),
                 m_tokenStartOffset, m_pos};
}

auto Lexer::errorToken(std::string message) -> Token {
    return Token{TokenType::Error, std::move(message), currentLocation(),
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

auto Lexer::isIdentChar(char c) -> bool {
    return isAlpha(c) || isDigit(c) || c == '_';
}

} // namespace kex
