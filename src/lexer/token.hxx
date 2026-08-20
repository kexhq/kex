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
    Rescue,
    Return,
    Spawn,
    Then,
    This,
    Timeout,
    Trait,
    True,
    Try,
    Trying,
    Type,
    Using,
    Var,
    When,
    While,

    // Symbols
    Arrow,        // ->
    FatArrow,     // => (match, when and receive arms)
    Amp,          // &
    At,           // @
    Bang,         // !
    Colon,        // :
    Caret,        // ^
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
    RawString,      // `raw text`, multiline and non-interpolating
    InterpolatedRawString, // $`text ${expr}`, multiline and interpolating
    Char,         // 'c'
    Atom,         // :identifier

    // Special
    Newline,
    Eof,
    Error,
};

struct SourceLocation {
    std::string_view file;
    int line = 0;
    int column = 0;
    // Byte range in the original UTF-8 source. Keeping the range on the
    // location makes one source-coordinate type usable by tokens, AST nodes,
    // diagnostics, and source-editing tools.
    int startOffset = -1;
    int endOffset = -1; // exclusive
};

struct Token {
    TokenType type;
    std::string value;
    SourceLocation location;
    int startOffset = -1; // byte offset in the original UTF-8 source
    int endOffset = -1;   // exclusive byte offset
};

auto tokenTypeName(TokenType type) -> std::string_view;

} // namespace kex
