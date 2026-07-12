#pragma once

#include <string>
#include <string_view>

namespace kex {

enum class TokenType {
    // Keywords
    After,
    Break,
    Compiled,
    Do,
    Elif,
    Else,
    End,
    Export,
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
    Next,
    None,
    Private,
    Public,
    Receive,
    Record,
    Return,
    Spawn,
    Static,
    Then,
    This,
    Timeout,
    Trait,
    True,
    Type,
    Using,
    Var,
    When,
    While,

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
    QuestionQuestion, // ??  (null-coalescing / or-else)
    Slash,        // /
    Star,         // *
    Tilde,        // ~
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
