#pragma once

#include "token.hxx"
#include <string>
#include <string_view>
#include <vector>

namespace kex {

class Lexer {
public:
    explicit Lexer(std::string source, std::string_view filename = "<stdin>");

    auto nextToken() -> Token;
    auto tokenizeAll() -> std::vector<Token>;

private:
    // The actual scanner. `nextToken` wraps it to record m_prevType, which the
    // `%` disambiguation below depends on.
    auto scanToken() -> Token;

    auto peek() const -> char;
    auto peekNext() const -> char;
    auto peekAt(int offset) const -> char;
    auto advance() -> char;
    auto atEnd() const -> bool;
    auto match(char expected) -> bool;
    auto currentLocation() const -> SourceLocation;

    auto skipWhitespace() -> void;
    auto skipComment() -> void;

    auto lexIdentifier() -> Token;
    auto lexNumber() -> Token;
    auto lexString() -> Token;
    auto lexRawString(bool interpolating = false) -> Token;
    auto lexChar() -> Token;
    auto lexAtom() -> Token;
    auto lexSpliceIdent() -> Token;

    auto makeToken(TokenType type) -> Token;
    auto makeToken(TokenType type, std::string value) -> Token;
    auto errorToken(std::string message) -> Token;

    static auto isLowerAlpha(char c) -> bool;
    static auto isUpperAlpha(char c) -> bool;
    static auto isAlpha(char c) -> bool;
    static auto isDigit(char c) -> bool;
    static auto isHexDigit(char c) -> bool;
    static auto isIdentChar(char c) -> bool;
    // A letter beyond ASCII: `α` is as good a name as `a`. Reported in
    // codepoints, not bytes, so the scanner can consume the whole character.
    auto identCodepointAt(int index, int& width) const -> char32_t;
    auto identContinues(int index, int& width) const -> bool;
    auto consumeIdentRest(std::string& ident) -> void;

    std::string m_source;
    std::string_view m_filename;
    int m_pos = 0;
    int m_line = 1;
    int m_column = 1;
    int m_tokenStartLine = 1;
    int m_tokenStartColumn = 1;
    int m_tokenStartOffset = 0;
    int m_parenDepth = 0;
    // Previous significant token, used to tell `%` (modulo) from `%name`
    // (splice). Starts as Newline so a `%name` opening a file is a splice.
    TokenType m_prevType = TokenType::Newline;
};

} // namespace kex
