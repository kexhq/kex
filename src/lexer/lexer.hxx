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
    auto peek() const -> char;
    auto peekNext() const -> char;
    auto advance() -> char;
    auto atEnd() const -> bool;
    auto match(char expected) -> bool;
    auto currentLocation() const -> SourceLocation;

    auto skipWhitespace() -> void;
    auto skipComment() -> void;

    auto lexIdentifier() -> Token;
    auto lexNumber() -> Token;
    auto lexString() -> Token;
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
    static auto isIdentChar(char c) -> bool;

    std::string m_source;
    std::string_view m_filename;
    int m_pos = 0;
    int m_line = 1;
    int m_column = 1;
    int m_tokenStartLine = 1;
    int m_tokenStartColumn = 1;
    int m_parenDepth = 0;
};

} // namespace kex
