#pragma once

#include <string>
#include <string_view>

namespace kex {

enum class TokenType {
    // Keywords
    After,
    Compiled,
    Do,
    Elif,
    Else,
    End,
    False,
    Final,
    Foul,
    If,
    Let,
    Loop,
    Main,
    Make,
    Match,
    Module,
    None,
    Private,
    Public,
    Receive,
    Record,
    Return,
    Spawn,
    Static,
    This,
    Timeout,
    True,
    Type,
    Using,
    Var,
    When,

    // Symbols
    Arrow,        // ->
    Amp,          // &
    At,           // @
    Bang,         // !
    Colon,        // :
    TypeAnnotation, // :>
    Comma,        // ,
    Dot,          // .
    DotDot,       // ..
    DotDotDot,    // ...
    Equals,       // =
    EqEq,         // ==
    GreaterThan,  // >
    GreaterEq,    // >=
    LBrace,       // {
    RBrace,       // }
    LBracket,     // [
    RBracket,     // ]
    LessThan,     // <
    LessEq,       // <=
    LParen,       // (
    RParen,       // )
    Minus,        // -
    NotEq,        // !=
    Percent,      // %
    Pipe,         // |
    PipePipe,     // ||
    AmpAmp,       // &&
    Plus,         // +
    Question,     // ?
    Slash,        // /
    Star,         // *
    Underscore,   // _
    HashLBracket, // #[

    // Identifiers and literals
    LowerIdent,   // lowercase identifier (may end with ?)
    UpperIdent,   // uppercase identifier
    SpliceIdent,  // %identifier
    Integer,
    Float,
    String,
    Char,         // 'c'
    Atom,         // :identifier

    // Special
    Newline,
    Eof,
    Error,
};

struct SourceLocation {
    std::string_view file;
    int line;
    int column;
};

struct Token {
    TokenType type;
    std::string value;
    SourceLocation location;
};

auto tokenTypeName(TokenType type) -> std::string_view;

} // namespace kex
