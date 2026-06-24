#include "token.hxx"

namespace kex {

auto tokenTypeName(TokenType type) -> std::string_view {
    switch (type) {
        case TokenType::After: return "after";
        case TokenType::Compiled: return "compiled";
        case TokenType::Do: return "do";
        case TokenType::Elif: return "elif";
        case TokenType::Else: return "else";
        case TokenType::End: return "end";
        case TokenType::False: return "false";
        case TokenType::Final: return "final";
        case TokenType::Foul: return "foul";
        case TokenType::If: return "if";
        case TokenType::Let: return "let";
        case TokenType::Loop: return "loop";
        case TokenType::Main: return "main";
        case TokenType::Make: return "make";
        case TokenType::Match: return "match";
        case TokenType::Module: return "module";
        case TokenType::None: return "None";
        case TokenType::Private: return "private";
        case TokenType::Public: return "public";
        case TokenType::Receive: return "receive";
        case TokenType::Record: return "record";
        case TokenType::Return: return "return";
        case TokenType::Spawn: return "spawn";
        case TokenType::Static: return "static";
        case TokenType::Then: return "then";
        case TokenType::This: return "this";
        case TokenType::Timeout: return "timeout";
        case TokenType::True: return "true";
        case TokenType::Type: return "type";
        case TokenType::Using: return "using";
        case TokenType::Var: return "var";
        case TokenType::When: return "when";
        case TokenType::While: return "while";
        case TokenType::Arrow: return "->";
        case TokenType::Amp: return "&";
        case TokenType::At: return "@";
        case TokenType::Bang: return "!";
        case TokenType::Colon: return ":";
        case TokenType::TypeAnnotation: return ":>";
        case TokenType::Comma: return ",";
        case TokenType::Dot: return ".";
        case TokenType::DotDot: return "..";
        case TokenType::DotDotDot: return "...";
        case TokenType::Equals: return "=";
        case TokenType::EqEq: return "==";
        case TokenType::GreaterThan: return ">";
        case TokenType::GreaterEq: return ">=";
        case TokenType::LBrace: return "{";
        case TokenType::RBrace: return "}";
        case TokenType::LBracket: return "[";
        case TokenType::RBracket: return "]";
        case TokenType::LessThan: return "<";
        case TokenType::LessEq: return "<=";
        case TokenType::LParen: return "(";
        case TokenType::RParen: return ")";
        case TokenType::Minus: return "-";
        case TokenType::NotEq: return "!=";
        case TokenType::Percent: return "%";
        case TokenType::Pipe: return "|";
        case TokenType::PipePipe: return "||";
        case TokenType::AmpAmp: return "&&";
        case TokenType::Plus: return "+";
        case TokenType::Question: return "?";
        case TokenType::Slash: return "/";
        case TokenType::Star: return "*";
        case TokenType::Underscore: return "_";
        case TokenType::HashLBracket: return "#[";
        case TokenType::LowerIdent: return "LowerIdent";
        case TokenType::UpperIdent: return "UpperIdent";
        case TokenType::SpliceIdent: return "SpliceIdent";
        case TokenType::Integer: return "Integer";
        case TokenType::Float: return "Float";
        case TokenType::String: return "String";
        case TokenType::Char: return "Char";
        case TokenType::Atom: return "Atom";
        case TokenType::Newline: return "Newline";
        case TokenType::Eof: return "Eof";
        case TokenType::Error: return "Error";
    }
    return "Unknown";
}

} // namespace kex
