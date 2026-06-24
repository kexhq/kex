#include "lexer.hxx"
#include <unordered_map>

namespace kex {

static const std::unordered_map<std::string, TokenType> keywords = {
    {"after", TokenType::After},
    {"and", TokenType::And},
    {"compiled", TokenType::Compiled},
    {"do", TokenType::Do},
    {"elif", TokenType::Elif},
    {"else", TokenType::Else},
    {"end", TokenType::End},
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
    {"not", TokenType::Not},
    {"or", TokenType::Or},
    {"private", TokenType::Private},
    {"public", TokenType::Public},
    {"receive", TokenType::Receive},
    {"record", TokenType::Record},
    {"return", TokenType::Return},
    {"spawn", TokenType::Spawn},
    {"static", TokenType::Static},
    {"this", TokenType::This},
    {"timeout", TokenType::Timeout},
    {"true", TokenType::True},
    {"type", TokenType::Type},
    {"using", TokenType::Using},
    {"var", TokenType::Var},
    {"when", TokenType::When},
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
        case '\n': return makeToken(TokenType::Newline);

        case '(': return makeToken(TokenType::LParen);
        case ')': return makeToken(TokenType::RParen);
        case '{': return makeToken(TokenType::LBrace);
        case '}': return makeToken(TokenType::RBrace);
        case '[': return makeToken(TokenType::LBracket);
        case ']': return makeToken(TokenType::RBracket);
        case ',': return makeToken(TokenType::Comma);
        case '@': return makeToken(TokenType::At);
        case '?': return makeToken(TokenType::Question);
        case '/': return makeToken(TokenType::Slash);
        case '*': return makeToken(TokenType::Star);
        case '+': return makeToken(TokenType::Plus);

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

        case '#':
            if (match('[')) return makeToken(TokenType::HashLBracket);
            skipComment();
            return nextToken();

        case '%':
            if (isLowerAlpha(peek())) return lexSpliceIdent();
            return makeToken(TokenType::Percent);

        case '"': return lexString();
        case '\'': return lexChar();

        default:
            return errorToken(std::string("Unexpected character: ") + c);
    }
}

auto Lexer::tokenizeAll() -> std::vector<Token> {
    std::vector<Token> tokens;
    while (true) {
        auto token = nextToken();
        tokens.push_back(token);
        if (token.type == TokenType::Eof || token.type == TokenType::Error) {
            break;
        }
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
    return Token{type, "", currentLocation()};
}

auto Lexer::makeToken(TokenType type, std::string value) -> Token {
    return Token{type, std::move(value), currentLocation()};
}

auto Lexer::errorToken(std::string message) -> Token {
    return Token{TokenType::Error, std::move(message), currentLocation()};
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
