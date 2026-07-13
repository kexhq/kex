#include "core_erlang.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace kex::codegen {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::erlAtom(const std::string& s) -> std::string {
    if (s.empty()) return "''";
    // Core Erlang requires ALL atoms to be single-quoted.
    return "'" + s + "'";
}

auto CoreErlangEmitter::erlVar(const std::string& s) -> std::string {
    if (s.empty()) return "_V";
    if (s == "_") return "_";
    // Prefix with _ to avoid clashes with Erlang reserved words, then uppercase.
    std::string result;
    result += static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    result += s.substr(1);
    return result;
}

auto CoreErlangEmitter::erlString(const std::string& s) -> std::string {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\t') { out += "\\t"; }
        else { out += c; }
    }
    out += '"';
    return out;
}

auto CoreErlangEmitter::freshVar(const std::string& hint) -> std::string {
    return "_" + erlVar(hint) + std::to_string(m_varCounter++);
}

auto CoreErlangEmitter::emitReturnValue(const ast::ExprPtr& v) -> std::string {
    if (m_returnThrows)
        return "call 'erlang':'throw'({'kex_return', " + emitExpr(v) + "})";
    return emitExpr(v);
}

// Wrap an emitted function body so a `throw({'kex_return', V})` from any
// `return` inside it becomes the body's value; every other exception is
// re-raised unchanged (via primop 'raise', preserving class/reason/trace).
// Structure mirrors what erlc emits for a source-level try/catch.
auto CoreErlangEmitter::wrapReturnCatch(const std::string& body) -> std::string {
    std::string rv  = freshVar("Ret");
    std::string rvV = freshVar("RV");
    std::string cls = freshVar("Cls");
    std::string rsn = freshVar("Rsn");
    std::string trc = freshVar("Trc");
    return "try\n    " + body + "\n"
           "of <" + rv + "> -> " + rv + "\n"
           "catch <" + cls + ", " + rsn + ", " + trc + "> ->\n"
           "  case <" + cls + ", " + rsn + ", " + trc + "> of\n"
           "    <'throw', {'kex_return', " + rvV + "}, " + freshVar("T") + "> when 'true' -> " + rvV + "\n"
           "    <" + freshVar("C") + ", " + freshVar("R") + ", " + freshVar("Tr") +
                "> when 'true' -> primop 'raise'(" + trc + ", " + rsn + ")\n"
           "  end";
}

auto CoreErlangEmitter::exprHasReturn(const ast::ExprPtr& e) -> bool {
    if (!e) return false;
    return std::visit([](const auto& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            return true;
        } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            if (bodyHasReturn(n.thenBody)) return true;
            for (const auto& [c, b] : n.elifs) if (bodyHasReturn(b)) return true;
            return n.elseBody && bodyHasReturn(*n.elseBody);
        } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            for (const auto& cl : n.clauses) if (exprHasReturn(cl.body)) return true;
            return false;
        } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            return bodyHasReturn(n.body);
        } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            return bodyHasReturn(n.body);
        } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            return bodyHasReturn(n.body);
        } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            return exprHasReturn(n.expr) || exprHasReturn(n.condition);
        } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            return exprHasReturn(n.thenExpr) || exprHasReturn(n.elseExpr) ||
                   exprHasReturn(n.condition);
        } else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            return exprHasReturn(n.value);
        } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            return exprHasReturn(n.value);
        } else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            return exprHasReturn(n.value);
        }
        // Don't cross Lambda boundaries — a lambda's `return` is its own.
        return false;
    }, e->kind);
}

auto CoreErlangEmitter::bodyHasReturn(const std::vector<ast::ExprPtr>& body) -> bool {
    for (const auto& e : body) if (exprHasReturn(e)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// String interpolation: "Hello, ${name}!" → iolist construction
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitInterpolatedString(const std::string& raw) -> std::string {
    // Scan for ${...} segments and build a list of string segments + expressions.
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < raw.size()) {
        auto dollar = raw.find("${", pos);
        if (dollar == std::string::npos) {
            // Trailing plain text
            if (pos < raw.size())
                parts.push_back(erlString(raw.substr(pos)));
            break;
        }
        if (dollar > pos)
            parts.push_back(erlString(raw.substr(pos, dollar - pos)));
        // Find the matching '}', respecting nested braces.
        size_t close = std::string::npos;
        {
            int depth = 1;
            for (size_t k = dollar + 2; k < raw.size(); k++) {
                if (raw[k] == '{') depth++;
                else if (raw[k] == '}') { if (--depth == 0) { close = k; break; } }
            }
        }
        if (close == std::string::npos) break; // malformed — treat as literal
        std::string inner = raw.substr(dollar + 2, close - dollar - 2);
        // Parse the inner expression and emit it, wrapped in to_string.
        kex::Lexer innerLexer(inner);
        auto toks = innerLexer.tokenizeAll();
        kex::Parser innerParser(std::move(toks));
        auto innerExpr = innerParser.parseExpr();
        if (innerExpr) {
            auto emitted = emitExpr(innerExpr);
            parts.push_back("call 'kex_io':'to_string'(" + emitted + ")");
        }
        pos = close + 1;
    }
    if (parts.empty()) return "\"\"";
    if (parts.size() == 1) return parts[0];
    // Build a charlist concatenation via erlang:'++'
    std::string result = parts[0];
    for (size_t i = 1; i < parts.size(); i++)
        result = "call 'erlang':'++'(" + result + ", " + parts[i] + ")";
    return result;
}

// ---------------------------------------------------------------------------
// Stdlib call resolution
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::resolveStdlib(const std::string& kexModule,
                                       const std::string& kexFn)
    -> std::pair<std::string, std::string>
{
    // Table: (kex_module, kex_function) → (beam_module, beam_function)
    static const std::unordered_map<std::string, std::pair<std::string,std::string>> table = {
        {"IO::printLine",   {"kex_io", "print_line"}},
        {"IO::print",       {"kex_io", "print"}},
        {"IO::printError",  {"kex_io", "print_error"}},
        {"IO::warn",        {"kex_io", "print_error"}},
        {"IO::warning",     {"kex_io", "print_error"}},
        {"IO::readLine",    {"kex_io", "read_line"}},
        {"IO::inspect",     {"kex_io", "inspect"}},
        {"System::exit",    {"erlang", "halt"}},
        {"Console::Reset",   {"kex_intrinsic_console", "Reset"}},
        {"Console::Bold",    {"kex_intrinsic_console", "Bold"}},
        {"Console::Dim",     {"kex_intrinsic_console", "Dim"}},
        {"Console::Italic",  {"kex_intrinsic_console", "Italic"}},
        {"Console::Underline", {"kex_intrinsic_console", "Underline"}},
        {"Console::Blink",   {"kex_intrinsic_console", "Blink"}},
        {"Console::Reverse", {"kex_intrinsic_console", "Reverse"}},
        {"Console::Hidden",  {"kex_intrinsic_console", "Hidden"}},
        {"Console::Strikethrough", {"kex_intrinsic_console", "Strikethrough"}},
        {"Console::Red",     {"kex_intrinsic_console", "Red"}},
        {"Console::Green",   {"kex_intrinsic_console", "Green"}},
        {"Console::Yellow",  {"kex_intrinsic_console", "Yellow"}},
        {"Console::Blue",    {"kex_intrinsic_console", "Blue"}},
        {"Console::Magenta", {"kex_intrinsic_console", "Magenta"}},
        {"Console::Cyan",    {"kex_intrinsic_console", "Cyan"}},
        {"Console::White",   {"kex_intrinsic_console", "White"}},
        {"Console::Gray",    {"kex_intrinsic_console", "Gray"}},
        {"Console::Purple",  {"kex_intrinsic_console", "Purple"}},
        {"Console::enabled?", {"kex_intrinsic_console", "enabled"}},
        {"Console::colorize", {"kex_intrinsic_console", "colorize"}},
        // Math
        {"Math::sqrt",      {"math",   "sqrt"}},
        {"Math::sin",       {"math",   "sin"}},
        {"Math::cos",       {"math",   "cos"}},
        {"Math::tan",       {"math",   "tan"}},
        // log maps to kex_intrinsic_math:log/1,2 (not a bare math:log forward) —
        // Erlang's math module has no log/2 (arbitrary base) at all; see
        // kex_intrinsic_math.erl's log for the ln(x)/ln(base) implementation
        // matching src/interpreter/stdlib/math.cxx's Math::log exactly.
        {"Math::log",       {"kex_intrinsic_math", "log"}},
        {"Math::log2",      {"math",   "log2"}},
        {"Math::log10",     {"math",   "log10"}},
        {"Math::exp",       {"math",   "exp"}},
        {"Math::floor",     {"erlang", "floor"}},
        {"Math::ceil",      {"erlang", "ceil"}},
        {"Math::round",     {"erlang", "round"}},
        {"Math::abs",       {"erlang", "abs"}},
        {"Math::pow",       {"math",   "pow"}},
        {"Math::atan2",     {"math",   "atan2"}},
        {"Math::hypot",     {"kex_intrinsic_math", "hypot"}},
        {"Math::cbrt",      {"kex_intrinsic_math", "cbrt"}},
        {"Math::pi",        {"math",   "pi"}},
        {"Math::PI",        {"math",   "pi"}},
        // e/E is a bare 0-arg constant (Euler's number) — math:exp/1 needs
        // an argument, so it can't back this directly (a real, reproduced
        // undef crash otherwise: math:exp() with no args). See
        // kex_intrinsic_math.erl's e/0.
        {"Math::e",         {"kex_intrinsic_math", "e"}},
        {"Math::E",         {"kex_intrinsic_math", "e"}},
        {"Math::inf",       {"erlang", "infinity"}},
        // Integer/Float parsing — see intrinsic modules for why these
        // need custom logic rather than a bare BIF mapping
        // (they return Ok(v)/Error(reason), matching
        // src/interpreter/stdlib/number.cxx exactly).
        {"Integer::parse",  {"kex_intrinsic_integer", "integer_parse"}},
        {"Integer::parsePrefix", {"kex_intrinsic_integer", "integer_parse_prefix"}},
        {"Float::parse",    {"kex_intrinsic_number", "float_parse"}},
        {"Float::parsePrefix",   {"kex_intrinsic_number", "float_parse_prefix"}},
        {"Number::parse",   {"kex_intrinsic_number", "number_parse"}},
        // String
        {"String::length",  {"erlang", "length"}},
        {"String::toUpper", {"string", "to_upper"}},
        {"String::toLower", {"string", "to_lower"}},
        {"String::trim",    {"string", "trim"}},
        {"String::split",   {"string", "split"}},
        // File
        {"File::exists?",   {"kex_file", "exists"}},
        {"File::lines",     {"kex_file", "lines"}},
        {"File::read",      {"kex_file", "read"}},
        {"File::write",     {"kex_file", "write"}},
        {"File::append",    {"kex_file", "append"}},
        {"File::size",      {"kex_file", "size"}},
        {"File::delete",    {"kex_file", "delete"}},
        // List
        {"List::length",    {"erlang", "length"}},
        {"List::reverse",   {"lists",  "reverse"}},
        {"List::flatten",   {"lists",  "flatten"}},
        {"List::sort",      {"lists",  "sort"}},
        {"List::min",       {"lists",  "min"}},
        {"List::max",       {"lists",  "max"}},
        {"List::sum",       {"lists",  "sum"}},
        {"List::member",    {"lists",  "member"}},
    };
    auto key = kexModule + "::" + kexFn;
    auto it = table.find(key);
    if (it != table.end()) return it->second;
    return {"", ""};
}

// ---------------------------------------------------------------------------
// Pattern emission
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitPattern(const ast::PatternPtr& pat) -> std::string {
    if (!pat) return "_";
    return std::visit([this](const auto& p) -> std::string {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, ast::WildcardPattern>) {
            return "_";
        } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            // @ in a pattern (e.g. @Ok(x), @Less) is a sigil that in BEAM
            // terms means the same thing as the plain pattern — unwrap it.
            return p.inner ? emitPattern(p.inner) : "_";
        } else if constexpr (std::is_same_v<T, ast::VarPattern>) {
            return erlVar(p.name);
        } else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
            switch (p.literal.type) {
                case TokenType::Integer:     return p.literal.value;
                case TokenType::Float:       return p.literal.value;
                case TokenType::String:      return erlString(p.literal.value);
                // A Char literal pattern (e.g. `@['_' | rest]`) — Kex Chars
                // compile to plain Erlang integers (see emitExpr's
                // CharLiteral case), so the pattern must match that same
                // integer codepoint, not fall to the "_" wildcard default
                // below. A real, reproduced bug otherwise: falling to "_"
                // means the pattern matches ANY character, not just this
                // one — spec/my_starts_with.kex's `@['-' | rest]` clause
                // became unreachable dead code because an earlier `@['_' |
                // rest]` clause's "pattern" was actually a wildcard.
                case TokenType::Char:
                    return std::to_string(static_cast<int>(
                        static_cast<unsigned char>(p.literal.value.empty() ? '\0' : p.literal.value[0])));
                case TokenType::True:        return "'true'";
                case TokenType::False:       return "'false'";
                case TokenType::None:        return "'none'";
                case TokenType::Atom:        return erlAtom(p.literal.value);
                default:                     return "_";
            }
        } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            std::string result = "{";
            for (size_t i = 0; i < p.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += emitPattern(p.elements[i]);
            }
            return result + "}";
        } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            if (p.elements.empty() && !p.rest) return "[]";
            std::string result = "[";
            for (size_t i = 0; i < p.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += emitPattern(p.elements[i]);
            }
            if (p.rest) result += "|" + emitPattern(*p.rest);
            return result + "]";
        } else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            // Sum type variant: Tag → 'Tag' (atom); Tag(args) → {'Tag', args...}
            if (p.args.empty()) return "'" + p.name + "'";
            std::string result = "{'" + p.name + "'";
            for (const auto& arg : p.args) result += ", " + emitPattern(arg);
            return result + "}";
        } else {
            return "_";
        }
    }, pat->kind);
}

// ---------------------------------------------------------------------------
// Expression emission
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitExpr(const ast::ExprPtr& expr) -> std::string {
    if (!expr) return "'undefined'";

    return std::visit([this](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;

        // --- Literals ---
        if constexpr (std::is_same_v<T, ast::IntLiteral>) {
            return node.value;
        } else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
            return node.value;
        } else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
            return node.value ? "'true'" : "'false'";
        } else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
            return "'none'";
        } else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
            return erlAtom(node.name);
        } else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
            return std::to_string(static_cast<int>(node.value));
        } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            // If the string contains ${...}, emit as iolist construction.
            if (node.value.find("${") != std::string::npos)
                return emitInterpolatedString(node.value);
            return erlString(node.value);
        }

        // --- Variables ---
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
            // Check loop/SSA substitution first (mutable var current version).
            auto subIt = m_varSubst.find(node.name);
            if (subIt != m_varSubst.end()) return subIt->second;
            // Top-level 0-arity function used as a value — call it.
            auto it = m_topLevelFns.find(node.name);
            if (it != m_topLevelFns.end() && it->second == 0)
                return "apply '" + node.name + "'/0()";
            return erlVar(node.name);
        } else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
            // ENV is a real value (a Map<String,String> snapshot of the
            // process environment — see src/interpreter/stdlib/env.cxx),
            // not a module namespace or a zero-arg ADT tag, despite being
            // spelled ALL_CAPS/UpperIdentifier like one. A real, reproduced
            // undef crash otherwise: every `ENV.xxx` call fell into the
            // module-dispatch branch below, calling the nonexistent
            // 'Kex.ENV' module.
            if (node.name == "ENV") return "call 'kex_io':'env_map'()";
            // ALL_CAPS top-level `let` constants (e.g. `let DEFAULT_LEVEL =
            // "info"`) are a real, established Kex convention (see
            // src/interpreter/stdlib/env.cxx's comment on ENV following
            // "the same convention as the ALL_CAPS ... constant" pattern) —
            // parsed as UpperIdentifier like a module/atom reference, but
            // must resolve as a variable when one is actually bound. A real,
            // reproduced bug otherwise: referencing DEFAULT_LEVEL anywhere
            // after its `let` printed the literal atom 'DEFAULT_LEVEL'
            // instead of its value (spec/env.kex).
            {
                auto subIt = m_varSubst.find(node.name);
                if (subIt != m_varSubst.end()) return subIt->second;
                auto it = m_topLevelFns.find(node.name);
                if (it != m_topLevelFns.end() && it->second == 0)
                    return "apply '" + node.name + "'/0()";
            }
            // Two distinct Kex concepts legitimately lower to a BEAM atom here:
            //   • Zero-arg variant tag (Less, Nothing) → bare atom 'Less',
            //     matching Erlang/OTP convention for sum-type constructors.
            //   • Module reference (Math, IO) → module atom for call/apply sites.
            // Both map to erlAtom intentionally; the distinction is in how
            // the surrounding call/pattern context uses the result.
            return erlAtom(node.name);
        } else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
            return "This";
        }

        // --- Binary ops ---
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            // ?? (null-coalescing): if lhs is `none`, return rhs, else return lhs.
            if (node.op == TokenType::QuestionQuestion) {
                auto lv = freshVar("QQ");
                auto l = emitExpr(node.left);
                auto r = emitExpr(node.right);
                return "let <" + lv + "> =\n    " + l + "\nin\n"
                       "case " + lv + " of\n"
                       "  'none' when 'true' -> " + r + "\n"
                       "  _ when 'true' -> " + lv + "\n"
                       "end";
            }
            // && / || must short-circuit (matches the tree-walking
            // interpreter's Evaluator::eval BinaryOp handling exactly, and
            // is what Kex's docs/examples rely on — spec/mutual_recursion.kex
            // is a real regression case: `n == 0 || isOdd(n - 1)` needs the
            // right-hand recursive call to never run once n == 0). Erlang's
            // own 'and'/'or' BIFs are NOT short-circuiting — they're strict,
            // evaluating both operands unconditionally before combining them
            // — unlike source-level andalso/orelse. Confirmed the hard way:
            // compiling this via the naive `call 'erlang':'or'(...)` form
            // caused genuine infinite recursion under BEAM (every call is a
            // real tail call, so it never stack-overflows either — it just
            // runs forever). Expand to the same case-based short-circuit
            // form the real Erlang compiler generates for andalso/orelse.
            if (node.op == TokenType::AmpAmp) {
                auto lv = freshVar("AND");
                auto l = emitExpr(node.left);
                auto r = emitExpr(node.right);
                return "let <" + lv + "> =\n    " + l + "\nin\n"
                       "case " + lv + " of\n"
                       "  'true' when 'true' -> " + r + "\n"
                       "  'false' when 'true' -> 'false'\n"
                       "end";
            }
            if (node.op == TokenType::PipePipe) {
                auto lv = freshVar("OR");
                auto l = emitExpr(node.left);
                auto r = emitExpr(node.right);
                return "let <" + lv + "> =\n    " + l + "\nin\n"
                       "case " + lv + " of\n"
                       "  'true' when 'true' -> 'true'\n"
                       "  'false' when 'true' -> " + r + "\n"
                       "end";
            }
            // / is polymorphic like +: Erlang's own '/' ALWAYS returns a
            // float, even for two integers (10/3 =:= 3.33...), but Kex's
            // `/` does integer division when both operands are integers
            // (matching Evaluator::eval's BinaryOp::Slash case, which uses
            // plain C++ int64_t division — truncating toward zero, exactly
            // like Erlang's own `div`) and float division otherwise. A
            // real, reproduced bug otherwise: `10 / 3` printed "3.333333"
            // under BEAM instead of "3" (spec/arithmetic.kex).
            // Operator overloading: `make Type do let +(other) ... end`
            // (and -, *, /, %, ==, !=, <, >, <=, >=) registers a real
            // top-level function under the literal operator symbol as its
            // name (see Parser::parseFunctionDef's tokenTypeName(...)
            // fallback for operator-named defs), with the same implicit-
            // this/collision-mangling treatment as any other method — so
            // if `m_topLevelFns` has an entry for the operator's symbol,
            // a real (possibly dispatcher, if multiple types overload it)
            // function exists and must be preferred over the builtin
            // numeric/string behavior below. Runtime-checked (`is_tuple`)
            // since operands could still be builtin values even when SOME
            // type overloads this operator (spec/operator_overloading.kex).
            auto overloadSymbol = [&]() -> std::string {
                switch (node.op) {
                    case TokenType::Plus:        return "+";
                    case TokenType::Minus:       return "-";
                    case TokenType::Star:        return "*";
                    case TokenType::Slash:       return "/";
                    case TokenType::Percent:     return "%";
                    case TokenType::EqEq:        return "==";
                    case TokenType::NotEq:       return "!=";
                    case TokenType::LessThan:    return "<";
                    case TokenType::GreaterThan: return ">";
                    case TokenType::LessEq:      return "<=";
                    case TokenType::GreaterEq:   return ">=";
                    default:                     return "";
                }
            }();
            auto overloadIt = overloadSymbol.empty() ? m_topLevelFns.end()
                                                      : m_topLevelFns.find(overloadSymbol);
            bool hasOverload = overloadIt != m_topLevelFns.end() && overloadIt->second == 2;

            if (node.op == TokenType::Slash) {
                auto lv = freshVar("L");
                auto rv = freshVar("R");
                auto l = emitExpr(node.left);
                auto r = emitExpr(node.right);
                std::string body =
                    "call 'kex_intrinsic_number':'divide'(" + lv + ", " + rv + ")";
                if (hasOverload)
                    body = "case call 'erlang':'is_tuple'(" + lv + ") of\n"
                           "  'true' when 'true' -> apply '/'/2(" + lv + ", " + rv + ")\n"
                           "  'false' when 'true' -> " + body + "\n"
                           "end";
                return "let <" + lv + "> =\n    " + l + "\nin\n"
                       "let <" + rv + "> =\n    " + r + "\nin\n" + body;
            }
            auto l = emitExpr(node.left);
            auto r = emitExpr(node.right);
            auto op = [&]() -> std::string {
                switch (node.op) {
                    case TokenType::Plus:        return "+";
                    case TokenType::Minus:       return "-";
                    case TokenType::Star:        return "*";
                    case TokenType::Slash:       return "/";
                    case TokenType::Percent:     return "rem";
                    case TokenType::EqEq:        return "=:=";
                    case TokenType::NotEq:       return "=/=";
                    case TokenType::LessThan:    return "<";
                    case TokenType::GreaterThan: return ">";
                    case TokenType::LessEq:      return "=<";
                    case TokenType::GreaterEq:   return ">=";
                    default:                     return "+";
                }
            }();
            std::string fallback;
            // + is overloaded: string concat (++) or arithmetic (+).
            // Dispatch at runtime via kex_intrinsic_number:add/2 which checks is_list.
            if (node.op == TokenType::Plus)
                fallback = "call 'kex_intrinsic_number':'add'(" + l + ", " + r + ")";
            else
                fallback = "call 'erlang':'" + op + "'(" + l + ", " + r + ")";
            if (!hasOverload) return fallback;

            auto lv = freshVar("L");
            auto rv = freshVar("R");
            return "let <" + lv + "> =\n    " + l + "\nin\n"
                   "let <" + rv + "> =\n    " + r + "\nin\n"
                   "case call 'erlang':'is_tuple'(" + lv + ") of\n"
                   "  'true' when 'true' -> apply '" + overloadSymbol + "'/2(" + lv + ", " + rv + ")\n"
                   "  'false' when 'true' -> " +
                       (node.op == TokenType::Plus
                            ? "call 'kex_intrinsic_number':'add'(" + lv + ", " + rv + ")"
                            : "call 'erlang':'" + op + "'(" + lv + ", " + rv + ")") + "\n"
                   "end";
        }

        // --- Unary ops ---
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            auto operand = emitExpr(node.operand);
            if (node.op == TokenType::Minus)
                // erlang:'-'/1 is the real unary-minus BIF — 'negate' isn't
                // a thing in Erlang at all (a real, reproduced undef crash:
                // spec/arithmetic.kex).
                return "call 'erlang':'-'(" + operand + ")";
            if (node.op == TokenType::Bang)
                return "call 'erlang':'not'(" + operand + ")";
            return operand;
        }

        // --- Tuple/List ---
        else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            std::string result = "{";
            for (size_t i = 0; i < node.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += emitExpr(node.elements[i]);
            }
            return result + "}";
        } else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            if (node.elements.empty() && !node.rest) return "[]";
            std::string result = "[";
            for (size_t i = 0; i < node.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += emitExpr(node.elements[i]);
            }
            if (node.rest) result += "|" + emitExpr(*node.rest);
            return result + "]";
        }

        // --- Let / Var bindings (handled in emitBody; here just emit value) ---
        else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            return emitExpr(node.value);
        } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            return emitExpr(node.value);
        } else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            return emitExpr(node.value);
        }

        // --- Return ---
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            // In a function with early returns, a `return` appearing as a
            // sub-expression (e.g. a match-arm value like `Error(e) ->
            // return Error(e)`) throws so it exits the whole function
            // rather than becoming that arm's value; wrapReturnCatch turns
            // it back into the function result. Without an enclosing return
            // scope this is just the value (tail position).
            return emitReturnValue(node.value);
        }

        // --- Function call (free function) ---
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            std::string args;
            for (size_t i = 0; i < node.args.size(); i++) {
                if (i > 0) args += ", ";
                args += emitExpr(node.args[i]);
            }
            // `worker { block }` → kex_supervisor:worker/1 with block as 0-arity fun.
            // Only when the program hasn't defined its own `worker`
            // itself — a user-defined `foul worker(...) do ... end` must
            // shadow this built-in sugar, not be silently overridden by
            // it (spec/optional_parens_do.kex's own 2-arg-plus-block
            // `worker`, previously always intercepted here regardless of
            // arg count).
            if (node.name == "worker" && node.block && !m_topLevelFns.count("worker")) {
                auto fnVar = freshVar("WorkerFn");
                auto fun   = emitExpr(*node.block);
                return "let <" + fnVar + "> =\n    " + fun + " in\n"
                       "call 'kex_supervisor':'worker'(" + fnVar + ")";
            }
            // `worker(Module)` / `worker(Module, args: [...])` MPA sugar.
            // Desugars to: worker { Module.start(args...) }
            // i.e. kex_supervisor:worker/1 with a 0-arity fun that calls kex_MODULE:start/N.
            if (node.name == "worker" && !node.args.empty() && !node.block &&
                !m_topLevelFns.count("worker")) {
                if (auto* uid = std::get_if<ast::UpperIdentifier>(&node.args[0]->kind)) {
                    // Lower-case the module name for kex_MODULE convention.
                    std::string modName = uid->name;
                    std::transform(modName.begin(), modName.end(), modName.begin(),
                                   [](unsigned char c){ return std::tolower(c); });
                    std::string startArgs;
                    for (const auto& [k, v] : node.namedArgs) {
                        if (k == "args") {
                            // The args value is expected to be a List; emit each element.
                            if (auto* lst = std::get_if<ast::ListExpr>(&v->kind)) {
                                for (size_t i = 0; i < lst->elements.size(); i++) {
                                    if (i > 0) startArgs += ", ";
                                    startArgs += emitExpr(lst->elements[i]);
                                }
                            } else {
                                startArgs = emitExpr(v);
                            }
                        }
                    }
                    auto fnVar = freshVar("WorkerFn");
                    std::string callExpr = "call 'kex_" + modName + "':'start'(" + startArgs + ")";
                    return "let <" + fnVar + "> =\n    fun () ->\n    " + callExpr + " in\n"
                           "call 'kex_supervisor':'worker'(" + fnVar + ")";
                }
            }
            // `supervisor(restart: :s) do BLOCK end` as a free function.
            // Creates a nested supervisor child spec.
            if (node.name == "supervisor" && node.block && !m_topLevelFns.count("supervisor")) {
                std::string strat = "'only_crashed'";
                for (const auto& [k, v] : node.namedArgs)
                    if (k == "restart") strat = emitExpr(v);
                std::string children;
                if (auto* lam = std::get_if<ast::Lambda>(&node.block->get()->kind))
                    children = emitBody(lam->body);
                else
                    children = emitExpr(*node.block);
                auto childVar = freshVar("Children");
                auto fnVar    = freshVar("SupFn");
                // The nested supervisor is itself a worker whose start fun calls start_link.
                return "let <" + childVar + "> =\n    " + children + " in\n"
                       "let <" + fnVar + "> =\n"
                       "    fun () ->\n"
                       "    call 'kex_supervisor':'start_link'("
                       "~{'strategy' => " + strat + ", 'children' => " + childVar + "}~) in\n"
                       "call 'kex_supervisor':'worker'(" + fnVar + ")";
            }
            // Intercept `send(pid, msg)` → erlang:send(Pid, {'kex_msg', Msg, self()}).
            // Every Kex send wraps the payload with the 'kex_msg' tag and the
            // sender's pid, so receiving `receive do |sender| ... end` can
            // bind the sender and bare `receive do pat -> ... end` matches
            // against the payload only. The tag also keeps Kex messages
            // distinguishable from raw Erlang tuples in the mailbox.
            if (node.name == "send" && node.args.size() == 2)
                return "call 'erlang':'send'(" + emitExpr(node.args[0]) +
                       ", {'kex_msg', " + emitExpr(node.args[1]) +
                       ", call 'erlang':'self'()})";
            // Intercept `self()` → erlang:self/0.
            if (node.name == "self" && node.args.empty())
                return "call 'erlang':'self'()";
            // assert(cond[, msg]) — part of Kex's testing DSL (see
            // src/interpreter/stdlib/test.cxx), a plain global function
            // (not stdlib-module-namespaced), so it fell through to the
            // "unknown lowercase name → treat as a variable" default below
            // otherwise, producing an "unbound variable 'Assert'" erlc
            // error (a real, reproduced bug: spec/traits.kex,
            // spec/my_starts_with.kex).
            if (node.name == "assert" && !node.args.empty())
                return "call 'kex_test':'assert'(" + args + ")";
            // describe(name) do ... end / it(name) do ... end — the rest
            // of Kex's testing DSL (see src/interpreter/stdlib/test.cxx
            // and runtime/src/kex_test.erl, which mirrors its exact
            // output format/nesting semantics). Same fallthrough gap as
            // `assert` above otherwise (spec/testing_dsl.spec.kex,
            // spec/json_parser.spec.kex, spec/optional_parens_do.kex).
            if ((node.name == "describe" || node.name == "it") &&
                !node.args.empty() && node.block) {
                auto nameArg = emitExpr(node.args[0]);
                auto fnVar = freshVar(node.name == "describe" ? "Describe" : "It");
                auto fun = emitExpr(*node.block);
                return "let <" + fnVar + "> =\n    " + fun + " in\n"
                       "call 'kex_test':'" + node.name + "'(" + nameArg + ", " + fnVar + ")";
            }
            if ((node.name == "before" || node.name == "after") &&
                node.args.size() <= 1 && node.block) {
                auto fnVar = freshVar(node.name == "before" ? "Before" : "After");
                auto fun = emitExpr(*node.block);
                auto prefix = node.args.empty() ? "" : emitExpr(node.args[0]) + ", ";
                return "let <" + fnVar + "> =\n    " + fun + " in\n"
                       "call 'kex_test':'" + node.name + "'(" + prefix + fnVar + ")";
            }
            // Check if it's a namespaced call like IO.printLine
            auto dot = node.name.find('.');
            if (dot != std::string::npos) {
                auto mod = node.name.substr(0, dot);
                auto fn  = node.name.substr(dot + 1);
                // Process.* module dispatch.
                if (mod == "Process") {
                    if (fn == "self" && node.args.empty())
                        return "call 'erlang':'self'()";
                    if (fn == "exit" && node.args.size() == 2)
                        return "call 'erlang':'exit'(" + args + ")";
                    if (fn == "register" && node.args.size() == 2)
                        return "call 'erlang':'register'(" + args + ")";
                    if (fn == "whereis" && node.args.size() == 1)
                        return "call 'erlang':'whereis'(" + args + ")";
                }
                auto [bmod, bfn] = resolveStdlib(mod, fn);
                if (!bmod.empty())
                    return "call '" + bmod + "':'" + bfn + "'(" + args + ")";
                return "call 'Kex." + mod + "':'" + fn + "'(" + args + ")";
            }
            // ADT constructor call: uppercase name → tagged tuple {'Name', args...}
            if (!node.name.empty() && std::isupper(static_cast<unsigned char>(node.name[0]))) {
                if (args.empty()) return "'" + node.name + "'";
                return "{'" + node.name + "', " + args + "}";
            }
            // Named arguments (`f(b: 2, a: 1)`, or `worker target: t,
            // restart: r do ... end`) are reordered into the callee's
            // positional parameter slots by name, mirroring
            // Evaluator::callFunction exactly: each named arg lands in its
            // matching slot first, THEN positional args — including a
            // trailing do-block, appended last — fill whatever slots
            // remain in declaration order, and any still-empty slot
            // defaults to None. A real, reproduced bug otherwise: named
            // args were dropped from the emitted call entirely, leaving
            // the arity short and erlc rejecting it with "argument count
            // mismatch" (spec/optional_parens_do.kex).
            auto paramsIt = m_topLevelParams.find(node.name);
            if (!node.namedArgs.empty() && paramsIt != m_topLevelParams.end()) {
                const auto& paramNames = paramsIt->second;
                std::vector<std::string> slots(paramNames.size()); // "" = unfilled
                for (const auto& [argName, argVal] : node.namedArgs) {
                    for (size_t i = 0; i < paramNames.size(); i++) {
                        if (paramNames[i] == argName) { slots[i] = emitExpr(argVal); break; }
                    }
                }
                std::vector<std::string> positional;
                for (const auto& a : node.args) positional.push_back(emitExpr(a));
                if (node.block) positional.push_back(emitExpr(*node.block));
                size_t nextSlot = 0;
                for (auto& p : positional) {
                    while (nextSlot < slots.size() && !slots[nextSlot].empty()) nextSlot++;
                    if (nextSlot >= slots.size()) break;
                    slots[nextSlot] = std::move(p);
                }
                std::string reordered;
                for (size_t i = 0; i < slots.size(); i++) {
                    if (i > 0) reordered += ", ";
                    reordered += slots[i].empty() ? "'none'" : slots[i];
                }
                return "apply '" + node.name + "'/" + std::to_string(slots.size()) +
                       "(" + reordered + ")";
            }

            // A trailing `do...end` block on a free-function call (with or
            // without parens around the preceding positional args — see
            // Parser's optional-parens-before-`do` sugar) is passed as
            // that function's own last parameter, exactly like any other
            // argument — e.g. `foo 1, 2, 3 do ... end` calling `foul
            // foo(a, b, c, block) do ... end`. A real, reproduced bug
            // otherwise: `node.block` was silently dropped for any
            // generic user-defined function call (only a handful of
            // builtins like `worker`/`supervisor` special-cased it),
            // leaving the callee's arity permanently short by one
            // (spec/optional_parens_do.kex).
            std::string callArgs = args;
            if (node.block) {
                if (!callArgs.empty()) callArgs += ", ";
                callArgs += emitExpr(*node.block);
            }
            // Known top-level function
            auto topIt = m_topLevelFns.find(node.name);
            if (topIt != m_topLevelFns.end()) {
                int callArity = static_cast<int>(node.args.size()) + (node.block ? 1 : 0);
                int defArity  = topIt->second;
                if (defArity == callArity) {
                    // Exact arity match → static dispatch
                    return "apply '" + node.name + "'/" + std::to_string(callArity) + "(" + callArgs + ")";
                }
                // Arity mismatch: the top-level value is a 0-arity closure; call it
                // first, then apply the resulting fun to the arguments.
                // e.g., hello("Alice") where hello/0 returns a lambda:
                //   let <_HFn> = apply 'hello'/0() in apply _HFn("Alice")
                auto tmpFn = freshVar(node.name.substr(0,1));
                return "let <" + tmpFn + "> = apply '" + node.name + "'/" +
                       std::to_string(defArity) + "() in\napply " + tmpFn + "(" + callArgs + ")";
            }
            // Unknown lowercase name: treat as a variable holding a fun (closure/param)
            // → dynamic apply through the variable
            return "apply " + erlVar(node.name) + "(" + callArgs + ")";
        }

        // --- Method call (UFCS) ---
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            // Check if receiver is a module name (UpperIdentifier) — ENV is
            // excluded: it's a real Map value (see emitExpr's UpperIdentifier
            // case above), so `ENV.get(...)`/`.each`/`.has?`/etc. must fall
            // through to the generic UFCS/value-receiver dispatch below,
            // exactly like any other Map-valued receiver.
            if (auto* uid = std::get_if<ast::UpperIdentifier>(&node.receiver->kind);
                uid && uid->name != "ENV") {
                auto [bmod, bfn] = resolveStdlib(uid->name, node.method);
                std::string args;
                for (size_t i = 0; i < node.args.size(); i++) {
                    if (i > 0) args += ", ";
                    args += emitExpr(node.args[i]);
                }
                if (!bmod.empty())
                    return "call '" + bmod + "':'" + bfn + "'(" + args + ")";
                // Task.start { block } → kex_task:start/1 with block as 0-arity fun.
                // The block emits as `fun () -> BODY` already; bind it to a var first
                // (Core Erlang requires fun literals to be let-bound before passing).
                if (uid->name == "Task" && node.method == "start" && node.block) {
                    auto fnVar = freshVar("TaskFn");
                    auto fun   = emitExpr(*node.block);
                    return "let <" + fnVar + "> =\n    " + fun + " in\n"
                           "call 'kex_task':'start'(" + fnVar + ")";
                }
                // Task.awaitAll([tasks]) → kex_task:await_all/1 — the Kex-
                // facing name is camelCase (matching every other multi-word
                // Kex builtin); the Erlang implementation function it maps
                // to keeps its own snake_case name, appropriately, since
                // that's Erlang's own convention, not Kex's.
                if (uid->name == "Task" && node.method == "awaitAll" && !node.args.empty())
                    return "call 'kex_task':'await_all'(" + args + ")";
                // Supervisor.start(restart: :s) do BLOCK end
                // BLOCK must evaluate to a list of child specs (from worker { } calls).
                // The block is parsed as a 0-arity Lambda; inline its body directly.
                if (uid->name == "Supervisor" && node.method == "start" && node.block) {
                    std::string strat = "'only_crashed'";
                    for (const auto& [k, v] : node.namedArgs)
                        if (k == "restart") strat = emitExpr(v);
                    std::string children;
                    if (auto* lam = std::get_if<ast::Lambda>(&node.block->get()->kind))
                        children = emitBody(lam->body);
                    else
                        children = emitExpr(*node.block);
                    auto childVar = freshVar("Children");
                    return "let <" + childVar + "> =\n    " + children + " in\n"
                           "call 'kex_supervisor':'start_link'("
                           "~{'strategy' => " + strat + ", 'children' => " + childVar + "}~)";
                }
                // Check if the method is a 0-arity static constant from a static do...end block.
                auto ctorIt = m_staticCtors.find(uid->name + "::" + node.method);
                if (ctorIt != m_staticCtors.end()) {
                    return "apply '" + ctorIt->second + "'/0()";
                }
                // Check if the method is a local top-level function (static make method).
                auto localIt = m_topLevelFns.find(node.method);
                if (localIt != m_topLevelFns.end()) {
                    int callArity = static_cast<int>(node.args.size());
                    int defArity  = localIt->second;
                    // A `make Type do let m(args) ... end` method is emitted
                    // with an implicit `this` receiver (defArity = args + 1).
                    // Called in STATIC `Type.m(args)` form there's no receiver
                    // to pass, so supply a placeholder (the type tag) to line
                    // the arities up — matching the tree-walker, where a
                    // static call passes no receiver and such a method simply
                    // never touches `this` (it builds its own value from the
                    // explicit args). A real, reproduced bug otherwise:
                    // `Parser.parse(input)` emitted `parse/1` but the method
                    // is `parse/2`, so erlc reported `undefined function
                    // parse/1` (examples/json_parser.kex, via
                    // spec/json_parser.spec.kex).
                    if (defArity == callArity + 1) {
                        std::string recvArg = "'" + uid->name + "'";
                        std::string allArgs = args.empty() ? recvArg : recvArg + ", " + args;
                        return "apply '" + node.method + "'/" + std::to_string(defArity) +
                               "(" + allArgs + ")";
                    }
                    return "apply '" + node.method + "'/" + std::to_string(callArity) + "(" + args + ")";
                }
                // Process.* module dispatch (MethodCall path, no parens or with parens).
                if (uid->name == "Process") {
                    if (node.method == "self" && node.args.empty())
                        return "call 'erlang':'self'()";
                    if (node.method == "exit" && node.args.size() == 2)
                        return "call 'erlang':'exit'(" + args + ")";
                    if (node.method == "register" && node.args.size() == 2)
                        return "call 'erlang':'register'(" + args + ")";
                    if (node.method == "whereis" && node.args.size() == 1)
                        return "call 'erlang':'whereis'(" + args + ")";
                }
                // Unknown module method → call as Kex module
                return "call 'Kex." + uid->name + "':'" + node.method + "'(" + args + ")";
            }
            // UFCS dispatch: map common method names to Erlang BIFs / kex_runtime.
            auto recv = emitExpr(node.receiver);
            auto firstArg = [&]() -> std::string {
                return node.args.empty() ? "" : emitExpr(node.args[0]);
            };

            // Stdlib receiver functions migrated to the Kex prelude — route to the shared
            // kex_prelude BEAM module instead of the hardcoded ladder, same as
            // the --ir path does. Gated on !m_inGuard (cross-module calls are
            // illegal inside Core Erlang guards). Excludes methods where the
            // prelude's runtime type-dispatch can't distinguish the receiver
            // (e.g. String vs List, both is_list at runtime — contains?).
            if (!m_inGuard) {
                static const std::unordered_set<std::string> preludeMethods = {
                    "reverse", "sort", "uniq", "flatten", "take", "drop", "zip", "push",
                    "sum", "product", "indexOf", "at", "min", "max", "count", "join",
                    "upperCase", "lowerCase", "trim", "split", "startsWith?", "endsWith?",
                    "digit?", "alpha?", "space?",
                    "modulo", "even?", "odd?",
                    "keys", "values", "entries", "merge", "has?", "put", "delete",
                    "abs", "sqrt", "none?", "some?", "ok?", "error?",
                    "first", "last", "empty?", "or", "in?",
                    "blank?", "present?", "truthy?", "falsy?",
                    "second", "third",
                    "floor", "ceil", "round", "toInteger",
                    "rest", "toOptional",
                    // Process/concurrency primitives backed by Kex.Intrinsic.Process.
                    "send", "link", "unlink", "monitor", "alive?",
                    "demonitor", "await",
                    // Higher-order functions provided by the Enumerable trait
                    // and per-type make blocks in the prelude.
                    "reduce", "map", "each", "filter", "reject",
                     "all?", "any?", "find", "flatMap", "collect",
                     "partition", "mapValues", "mapKeys", "times"};
                if (preludeMethods.count(node.method)) {
                    std::string allArgs = recv;
                    for (const auto& a : node.args) allArgs += ", " + emitExpr(a);
                    for (const auto& [_, v] : node.namedArgs) allArgs += ", " + emitExpr(v);
                    if (node.block) allArgs += ", " + emitExpr(*node.block);
                    return "call 'kex_prelude':'" + node.method + "'(" + allArgs + ")";
                }
            }

            // Task/process methods are routed through kex_prelude in non-guard
            // context. Their implementations use non-guard BIFs (erlang:send,
            // link, unlink, monitor, demonitor; kex_task:await) and are
            // side-effecting — never valid in guards.
            // `pid.alive?()` → erlang:is_process_alive/1 (guard-safe, keep)
            if (node.method == "alive?" && node.args.empty())
                return "call 'erlang':'is_process_alive'(" + recv + ")";
            // `pid.exit(reason)` (instance form) → erlang:exit/2
            if (node.method == "exit" && node.args.size() == 1)
                return "call 'erlang':'exit'(" + recv + ", " + firstArg() + ")";

            // Integer/Float receiver functions
            if (node.method == "modulo" && node.args.size() == 1)
                return "call 'kex_intrinsic_integer':'modulo'(" + recv + ", " + firstArg() + ")";
            if (node.method == "even?")
                return "call 'erlang':'=:='(call 'erlang':'rem'(" + recv + ", 2), 0)";
            if (node.method == "odd?")
                return "call 'erlang':'=/='(call 'erlang':'rem'(" + recv + ", 2), 0)";
            if (node.method == "abs")
                return "call 'erlang':'abs'(" + recv + ")";
            // sqrt uses math:sqrt which is not a guard BIF; routed through
            // kex_prelude in non-guard context.
            if (node.method == "floor")
                return "call 'erlang':'floor'(" + recv + ")";
            if (node.method == "ceil")
                return "call 'erlang':'ceil'(" + recv + ")";
            if (node.method == "round")
                return "call 'erlang':'round'(" + recv + ")";
            if (node.method == "toInteger")
                return "call 'erlang':'trunc'(" + recv + ")";

            // count in guard context uses erlang:length (guard-safe); in
            // non-guard context it routes through kex_prelude.
            if (node.method == "count" && node.args.empty() && !node.block)
                return "call 'erlang':'length'(" + recv + ")";
            // length (alias) uses runtime is_map dispatch with case — not
            // guard-safe, kept for backward compatibility.
            if (node.method == "length" && !node.block && node.args.empty()) {
                auto tmp = freshVar("SZ");
                return "let <" + tmp + "> =\n    " + recv + "\nin\n"
                       "case call 'erlang':'is_map'(" + tmp + ") of\n"
                       "  'true' when 'true' -> call 'maps':'size'(" + tmp + ")\n"
                       "  'false' when 'true' -> call 'erlang':'length'(" + tmp + ")\n"
                       "end";
            }
            // upperCase/lowerCase/trim/split/push are routed through kex_prelude
            // in non-guard context; their implementations use non-guard BIFs
            // (string:to_upper/to_lower/trim/split, erlang:++) that are never
            // valid in guard context.
            // first/last return Just(value)/None (matches
            // src/interpreter/stdlib/list.cxx exactly — works for both
            // String and List receivers, since a Char is just the head/
            // last element either way) — not the raw element, and NOT a
            // crash on an empty receiver. A real, reproduced bug otherwise:
            // erlang:hd([]) throws badarg, and even on a non-empty list the
            // raw element was returned unwrapped (spec/collections.kex
            // expected "Just(1)", got "1").
            // Unless a `make [A] do let first(@...) ... end`-style user
            // override exists, in which case it takes priority — see the
            // matching `to` guard above for why (spec/type_dispatch.kex
            // defines its own unwrapped first/last/empty?/head/tail over
            // `[A]`, which must win over this Just/None-wrapped default).
            // first/last are routed through kex_prelude in non-guard context;
            // their ladder implementations use let/case which are never valid
            // in Core Erlang guards.
            // reverse is routed through kex_prelude in non-guard context;
            // lists:reverse is not a guard BIF.
            // "abc".contains?("b") (substring search, needle is itself a
            // string) vs [1,2,3].contains?(2) / (1..10).contains?(5) /
            // ('a'..'z').contains?('m') (element membership, needle is a
            // scalar) both compile to the exact same method name/arity,
            // and String/List/Range receivers are ALL plain Erlang lists
            // (no runtime tag to dispatch on) — so the receiver alone
            // can't distinguish them. The needle itself can, though:
            // `string:find` expects a string pattern, which is a list —
            // a real, reproduced bug otherwise: this always used
            // `string:find` regardless of receiver, and a Range/List
            // receiver with a scalar Char/Integer needle isn't a valid
            // `string:find` pattern at all (`unicode:characters_to_list`
            // crash on the bare integer) (spec/list_extras.kex's
            // `('a'..'z').contains?('m')`).
            if (node.method == "contains?" && node.args.size() == 1) {
                auto needle = freshVar("Needle");
                return "let <" + needle + "> =\n    " + firstArg() + "\nin\n"
                       "case call 'erlang':'is_list'(" + needle + ") of\n"
                       "  'true' when 'true' -> call 'erlang':'=/='("
                             "call 'string':'find'(" + recv + ", " + needle + "), 'nomatch')\n"
                       "  'false' when 'true' -> call 'lists':'member'(" + needle + ", " + recv + ")\n"
                       "end";
            }
            // n.in?(range) / c.in?(range) — range membership. Ranges
            // compile to a materialized list (lists:seq — see emitExpr's
            // RangeExpr case), so this is just reversed lists:member — the
            // in?/join/flatten are routed through kex_prelude in non-guard
            // context; their implementations use lists:member/join/flatten
            // which are not guard BIFs.

            // Type conversion: x.to(String), x.to(Int), x.to(Float) — unless
            // a `make Type do let to(Kind) ... end` block defined its own
            // `to`, which must take priority over this builtin fallback. A
            // real, reproduced bug otherwise: Vec2/Vec3's own `to(String)`
            // overloads were always bypassed in favor of the generic
            // to_string dispatcher, printing the raw tagged tuple instead
            // of the user's formatted string (spec/type_dispatch.kex).
            if (node.method == "to" && node.args.size() == 1 && !m_topLevelFns.count("to")) {
                std::string typeName;
                if (auto* ui = std::get_if<ast::UpperIdentifier>(&node.args[0]->kind))
                    typeName = ui->name;
                else if (auto* ve = std::get_if<ast::VarExpr>(&node.args[0]->kind))
                    typeName = ve->name;
                if (typeName == "String")
                    return "call 'kex_io':'to_string_optional'(" + recv + ")";
                // "Integer" is the real Kex type name (see
                // src/interpreter/stdlib/list.cxx's own `.to` — "Int"
                // alone is never actually used at the source level); a
                // real, reproduced bug otherwise: `.to(Integer)` fell
                // through this whole special case entirely (matching
                // neither branch) and was wrongly treated as a call to a
                // nonexistent user method `to/2` (spec/fact.kex).
                // kex_io:to_integer/to_float handle String receivers
                // (parse) and Float→Integer truncation (not rounding —
                // `erlang:round` was also wrong for that case, matching
                // neither list.cxx's `static_cast<int64_t>` truncation
                // nor a String receiver at all).
                if (typeName == "Int" || typeName == "Integer")
                    return "call 'kex_intrinsic_number':'to_integer'(" + recv + ")";
                if (typeName == "Float")
                    return "call 'kex_intrinsic_number':'to_float'(" + recv + ")";
            }

            // Option/Result methods
            if (node.method == "or" && node.args.size() == 1) {
                // Just(x)/Ok(x).or(default) → x; None/Error(_).or(default) →
                // default — shared by both prelude ADTs (see
                // src/interpreter/stdlib/adt.cxx's "or" builtin, which this
                // must match: Ok/Error were missing here entirely, falling
                // through to the catch-all "return as-is" case and printing
                // the raw {Ok,V}/{Error,_} tuple instead of unwrapping it —
                // a real, reproduced bug, spec/result_option_or.kex).
                auto dflt = firstArg();
                auto tmp  = freshVar("Opt");
                return "let <" + tmp + "> =\n    " + recv + "\nin\n"
                       "case " + tmp + " of\n"
                       "  {'Just', _V} when 'true' -> _V\n"
                       "  {'Some', _V} when 'true' -> _V\n"
                       "  {'Ok', _V} when 'true' -> _V\n"
                       "  {'Error', _E} when 'true' -> " + dflt + "\n"
                       "  'none' when 'true' -> " + dflt + "\n"
                       "  _ when 'true' -> " + tmp + "\n"
                       "end";
            }
            if (node.method == "some?")
                return "call 'erlang':'=/='(" + recv + ", 'none')";
            if (node.method == "none?")
                return "call 'erlang':'=:='(" + recv + ", 'none')";
            // ok?/error? — Result predicates, matching
            // src/interpreter/stdlib/adt.cxx's regResultPredicate exactly
            // (a real, reproduced bug otherwise: these fell through to the
            // generic FunctionCall dispatch entirely, undef'd — but here
            // they're MethodCall-only since regResultPredicate is UFCS).
            if (node.method == "ok?")
                return "call 'erlang':'=:='(call 'erlang':'element'(1, " + recv + "), 'Ok')";
            if (node.method == "error?")
                return "call 'erlang':'=:='(call 'erlang':'element'(1, " + recv + "), 'Error')";
            // Char predicates — plain global Kex functions (see
            // src/interpreter/stdlib/string.cxx's digit?/alpha?/space?),
            // not stdlib-module-namespaced, called via UFCS on a Char
            // (a plain Erlang integer codepoint). Routed through real
            // kex_io functions (not inlined here) so `&alpha?`/`&digit?`/
            // `&space?` (see ShorthandLambda's Kind::Function handling
            // above) can reference the exact same implementation — a real,
            // reproduced bug otherwise: undef'd entirely
            // (spec/char_predicates.kex, spec/char_type.kex, spec/list_hof.kex).
            // In a `when` guard, a cross-module `call 'kex_intrinsic_char':...`
            // is an illegal guard expression (Erlang guards can't call arbitrary
            // functions) — so inline the exact same range checks as guard-safe
            // boolean BIF trees (erlang:'>='/'=<'/'=:='/'and'/'or' are all
            // allowed in guards). A real, reproduced bug otherwise: `Just(c) when
            // c.digit? || c == '-'` emitted `call 'kex_io':'is_digit'(C)`
            // inside the guard and erlc rejected the whole function with
            // "illegal guard expression" (examples/json_parser.kex's
            // parseValue/parseNumber, via spec/json_parser.spec.kex).
            if (node.method == "digit?" && node.args.empty()) {
                if (m_inGuard)
                    return "call 'erlang':'and'(call 'erlang':'>='(" + recv +
                           ", 48), call 'erlang':'=<'(" + recv + ", 57))";
                return "call 'kex_intrinsic_char':'is_digit'(" + recv + ")";
            }
            if (node.method == "space?" && node.args.empty()) {
                if (m_inGuard) {
                    // C =:= 32/9/10/13/11/12 (space/tab/nl/cr/vtab/ff),
                    // chained with erlang:'or' (lists:member isn't a guard BIF).
                    std::string chk;
                    for (int code : {32, 9, 10, 13, 11, 12}) {
                        std::string eq = "call 'erlang':'=:='(" + recv + ", " +
                                         std::to_string(code) + ")";
                        chk = chk.empty() ? eq : "call 'erlang':'or'(" + chk + ", " + eq + ")";
                    }
                    return chk;
                }
                return "call 'kex_intrinsic_char':'is_space'(" + recv + ")";
            }
            if (node.method == "alpha?" && node.args.empty()) {
                if (m_inGuard)
                    return "call 'erlang':'or'("
                           "call 'erlang':'and'(call 'erlang':'>='(" + recv + ", 65), "
                           "call 'erlang':'=<'(" + recv + ", 90)), "
                           "call 'erlang':'and'(call 'erlang':'>='(" + recv + ", 97), "
                           "call 'erlang':'=<'(" + recv + ", 122)))";
                return "call 'kex_intrinsic_char':'is_alpha'(" + recv + ")";
            }

            // Extra list methods — plain forms are routed through kex_prelude
            // in non-guard context; their ladder implementations use non-guard
            // size/length on lists uses erlang:length (guard-safe). Also
            // works for maps at runtime (erlang:length returns map_size).
            if (node.method == "size" || (node.method == "length" && node.args.empty()))
                return "call 'erlang':'length'(" + recv + ")";

            // Map methods: put, delete, merge are routed through kex_prelude;
            // maps:put/remove/merge are not guard BIFs.

            // .at(i) — String/List indexing, raw element (or 'none'), never
            // Just-wrapped — matches src/interpreter/stdlib/string.cxx's
            // `at` exactly. No Map ambiguity to resolve here (unlike
            // `.get`): both String and List already compile to a plain
            // Erlang list, so kex_intrinsic_list:list_get/2 (built for `.get`'s list
            // case) works unchanged.
            // at is routed through kex_prelude in non-guard context.
            if (node.method == "get" && node.args.size() == 1) {
                // `get` doubles as list indexing (`list[i]` desugars to
                // `list.get(i)` — see parsePostfix) — no static type info in
                // Erlang, so dispatch at runtime. List indexing returns the
                // raw element (or 'none'), NOT Just(value)-wrapped like
                // Map.get's 2-arg form — matches
                // src/interpreter/stdlib/map.cxx's `get` builtin exactly.
                // A real, reproduced bug otherwise: this unconditionally
                // called maps:find, which raises {badmap,...} for any list
                // receiver (spec/list_indexing.kex).
                auto kv = freshVar("V");
                return "case call 'erlang':'is_list'(" + recv + ") of\n"
                       "  'true' when 'true' -> call 'kex_intrinsic_list':'list_get'(" + recv + ", " + firstArg() + ")\n"
                       "  'false' when 'true' ->\n"
                       "    case call 'maps':'find'(" + firstArg() + ", " + recv + ") of\n"
                       "      {'ok', " + kv + "} when 'true' -> {'Just', " + kv + "}\n"
                       "      'error' when 'true' -> 'none'\n"
                       "    end\n"
                       "end";
            }
            if (node.method == "get" && node.args.size() == 2)
                return "case call 'erlang':'is_list'(" + recv + ") of\n"
                       "  'true' when 'true' -> call 'kex_intrinsic_list':'list_get'(" + recv + ", "
                       + emitExpr(node.args[0]) + ", " + emitExpr(node.args[1]) + ")\n"
                       "  'false' when 'true' -> call 'maps':'get'(" + emitExpr(node.args[0]) + ", "
                       + recv + ", " + emitExpr(node.args[1]) + ")\n"
                        "end";
            // merge is routed through kex_prelude; maps:merge is not a guard BIF.
            if (node.method == "has?" && node.args.size() == 1)
                return "call 'maps':'is_key'(" + firstArg() + ", " + recv + ")";
            // keys/values/entries are routed through kex_prelude in non-guard
            // context; their implementations use cross-module calls that are
            // not valid in guards.
            // mapKeys/mapValues are routed through kex_prelude in non-guard
            // contexts; not valid in guards.

            // Fallback: unknown method — emit as local apply
            std::string args = recv;
            for (const auto& a : node.args) args += ", " + emitExpr(a);
            int arity = 1 + static_cast<int>(node.args.size());
            return "apply '" + node.method + "'/" + std::to_string(arity) + "(" + args + ")";
        }

        // --- If/elif/else ---
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            // Build the else branch from the inside out: start with the
            // final else (or 'ok'), then wrap each elif around it.
            std::string otherwise = node.elseBody ? emitBody(*node.elseBody) : "'ok'";
            // Process elif chain in reverse
            for (int i = static_cast<int>(node.elifs.size()) - 1; i >= 0; --i) {
                auto& [elifCond, elifBody] = node.elifs[i];
                otherwise = "case " + emitExpr(elifCond) + " of\n"
                            "  'true' when 'true' ->\n    " + emitBody(elifBody) + "\n"
                            "  'false' when 'true' ->\n    " + otherwise + "\n"
                            "end";
            }
            // `if let Pattern = expr ... end`: condition holds the
            // scrutinee (NOT a boolean) and letPattern is the pattern to
            // destructure it against — a real Core Erlang case dispatch,
            // not a true/false branch. Pattern variables (e.g. `Just(n)`
            // binding `n`) are bound directly by the case match itself,
            // same as any other pattern; no separate let-chain needed. A
            // real, reproduced bug otherwise: `letPattern` was completely
            // unhandled, silently falling to the plain boolean-if path
            // below, which used the scrutinee as if it were a bare 'true'/
            // 'false' atom and left every pattern-bound name genuinely
            // unbound (erlc: "unbound variable" for each one)
            // (spec/if_let.kex).
            if (node.letPattern) {
                auto scrutinee = emitExpr(node.condition);
                auto pat = emitPattern(node.letPattern);
                auto then = emitBody(node.thenBody);
                return "case " + scrutinee + " of\n"
                       "  " + pat + " when 'true' ->\n    " + then + "\n"
                       "  _ when 'true' ->\n    " + otherwise + "\n"
                       "end";
            }

            auto cond = emitExpr(node.condition);
            auto then = emitBody(node.thenBody);
            return "case " + cond + " of\n"
                   "  'true' when 'true' ->\n    " + then + "\n"
                   "  'false' when 'true' ->\n    " + otherwise + "\n"
                   "end";
        }

        // --- Match ---
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            // When the subject is a tuple, use Core Erlang multi-value case syntax:
            //   case <V1, V2> of <P1, P2> when 'true' -> ...
            // For a single subject, use: case V of P when 'true' -> ...
            bool multiValue = std::holds_alternative<ast::TupleExpr>(node.subject->kind);

            // Ranges materialize as plain ascending lists (see RangeExpr's
            // `lists:seq` emission), so matching against a RangePattern
            // clause (`(x..y)`, `(-10..-1)`) isn't structural — there's no
            // "Range" tag to pattern-match on. Instead, rewrite the whole
            // match into a 2-tuple-of-bounds dispatch: compute the
            // subject's first/last elements ONCE, then each RangePattern
            // clause becomes an ordinary 2-tuple pattern over those
            // bounds (literal bound checks structurally match; `(x..y)`
            // binds x/y to the actual bounds directly) — every literal
            // bound is guard-safe (a==-10) but `lists:last` itself is
            // NOT guard-safe, so it must be computed up front, not
            // inside a `when` guard (spec/range.kex's second match block).
            bool rangeMode = std::any_of(node.clauses.begin(), node.clauses.end(),
                [](const ast::MatchClause& c) {
                    return !c.patterns.empty() &&
                           std::holds_alternative<ast::RangePattern>(c.patterns[0]->kind);
                });

            std::string scrutinee;
            std::string bindingPrefix;
            if (rangeMode) {
                auto subj = freshVar("RSubj");
                bindingPrefix = "let <" + subj + "> =\n    " + emitExpr(node.subject) + "\nin\n";
                scrutinee = "<call 'erlang':'hd'(" + subj + "), call 'lists':'last'(" + subj + ")>";
            } else if (multiValue) {
                auto& tup = std::get<ast::TupleExpr>(node.subject->kind);
                scrutinee = "<";
                for (size_t i = 0; i < tup.elements.size(); i++) {
                    if (i > 0) scrutinee += ", ";
                    scrutinee += emitExpr(tup.elements[i]);
                }
                scrutinee += ">";
            } else {
                scrutinee = emitExpr(node.subject);
            }

            // `match subj do |x| ... end` binds the subject to `x`, in
            // scope for every clause's guard/body (see `withBinding`/
            // `withResult` in spec/when_guards.kex, which reference the
            // binder from a bare `when cond -> ...` clause with no
            // pattern of its own at all). Bind it via a `let` up front
            // and use that bound variable as the actual case scrutinee,
            // rather than dropping the binder on the floor entirely — a
            // real, reproduced bug otherwise: erlc rejected the guard's
            // reference to `x` as an unbound variable.
            if (node.subjectBinding && !multiValue && !rangeMode) {
                auto bv = erlVar(*node.subjectBinding);
                bindingPrefix += "let <" + bv + "> =\n    " + scrutinee + "\nin\n";
                scrutinee = bv;
            }

            std::string result = bindingPrefix + "case " + scrutinee + " of\n";
            for (const auto& clause : node.clauses) {
                std::string pat;
                if (rangeMode && !clause.patterns.empty() &&
                    std::holds_alternative<ast::RangePattern>(clause.patterns[0]->kind)) {
                    auto& rp = std::get<ast::RangePattern>(clause.patterns[0]->kind);
                    pat = "<" + (rp.start ? emitPattern(rp.start) : freshVar("W")) + ", " +
                          (rp.end ? emitPattern(rp.end) : freshVar("W")) + ">";
                } else if (multiValue && !clause.patterns.empty()) {
                    if (auto* tp = std::get_if<ast::TuplePattern>(&clause.patterns[0]->kind)) {
                        pat = "<";
                        for (size_t i = 0; i < tp->elements.size(); i++) {
                            if (i > 0) pat += ", ";
                            // Wildcards in multi-value must be unique names
                            if (std::holds_alternative<ast::WildcardPattern>(tp->elements[i]->kind))
                                pat += freshVar("W");
                            else
                                pat += emitPattern(tp->elements[i]);
                        }
                        pat += ">";
                    } else {
                        pat = emitPattern(clause.patterns[0]);
                    }
                } else {
                    pat = clause.patterns.empty() ? freshVar("W") : emitPattern(clause.patterns[0]);
                }
                result += "  " + pat;
                if (clause.guard) {
                    m_inGuard = true;
                    result += " when " + emitExpr(*clause.guard);
                    m_inGuard = false;
                } else
                    result += " when 'true'";
                result += " ->\n    " + emitExpr(clause.body) + "\n";
            }
            result += "end";
            return result;
        }

        // --- Lambda ---
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            std::string params;
            for (size_t i = 0; i < node.params.size(); i++) {
                if (i > 0) params += ", ";
                params += erlVar(node.params[i].name);
            }
            auto body = emitBody(node.body);
            return "fun (" + params + ") ->\n    " + body;
        }

        // --- Record construction ---
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            // Tagged tuple {'TypeName', field1, field2, ...} with fields in
            // declaration order. Omitted fields fall back to their declared
            // default value (or 'none' if unspecified).
            std::string result = "{'" + node.typeName + "'";
            auto it = m_records.find(node.typeName);
            if (it != m_records.end()) {
                std::unordered_map<std::string, const ast::ExprPtr*> given;
                for (const auto& [name, val] : node.fields)
                    given[name] = &val;
                for (const auto& field : it->second->fields) {
                    auto gv = given.find(field.name);
                    if (gv != given.end())
                        result += ", " + emitExpr(*gv->second);
                    else if (field.defaultValue)
                        result += ", " + emitExpr(*field.defaultValue);
                    else
                        result += ", 'none'";
                }
            } else {
                // Unknown record type — fall back to as-written order.
                for (const auto& [name, val] : node.fields)
                    result += ", " + emitExpr(val);
            }
            return result + "}";
        }

        // --- Block (do...end expression) ---
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            return emitBody(node.body);
        }

        // --- Trailing if: `expr if cond` → case cond of true -> expr; false -> 'ok' end
        else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            auto cond = emitExpr(node.condition);
            auto val  = emitExpr(node.expr);
            return "case " + cond + " of\n"
                   "  'true' when 'true' -> " + val + "\n"
                   "  'false' when 'true' -> 'ok'\n"
                   "end";
        }

        // --- then/else (ternary): cond then a else b
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            auto cond = emitExpr(node.condition);
            auto t    = emitExpr(node.thenExpr);
            auto f    = emitExpr(node.elseExpr);
            return "case " + cond + " of\n"
                   "  'true' when 'true' -> " + t + "\n"
                   "  'false' when 'true' -> " + f + "\n"
                   "end";
        }

        // --- Range expression (1..N) → lists:seq(1, N) ---
        else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            return "call 'lists':'seq'(" + emitExpr(node.start) + ", " + emitExpr(node.end) + ")";
        }

        // --- Map literal: %{k => v, ...} ---
        else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            if (node.entries.empty())
                return "call 'maps':'new'()";
            std::string pairs;
            for (size_t i = 0; i < node.entries.size(); i++) {
                if (i > 0) pairs += ", ";
                pairs += "{" + emitExpr(node.entries[i].key) + ", " + emitExpr(node.entries[i].value) + "}";
            }
            return "call 'maps':'from_list'([" + pairs + "])";
        }

        // --- ShorthandLambda: &funcName, &.method, &.method(args) ---
        else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
            std::string pv = freshVar("SX");
            if (node.kind == ast::ShorthandLambda::Kind::Method ||
                node.kind == ast::ShorthandLambda::Kind::MethodWithArgs) {
                // Synthesize a MethodCall on `pv` and emit it via the UFCS dispatch.
                auto recvExpr = std::make_unique<ast::Expr>();
                recvExpr->kind = ast::Identifier{pv};  // Identifier = variable reference
                ast::MethodCall mc;
                mc.receiver = std::move(recvExpr);
                mc.method = node.name;
                // For MethodWithArgs, use placeholder Identifiers; we'll replace the
                // emitted text after calling emitExpr.
                for (const auto& a : node.args) {
                    auto aCopy = std::make_unique<ast::Expr>();
                    aCopy->kind = ast::Identifier{"__arg_placeholder__"};
                    mc.args.push_back(std::move(aCopy));
                }
                auto mcExpr = std::make_unique<ast::Expr>();
                mcExpr->kind = std::move(mc);
                std::string body = emitExpr(mcExpr);
                // If there were MethodWithArgs, replace placeholder identifier text with real args.
                if (node.kind == ast::ShorthandLambda::Kind::MethodWithArgs) {
                    // erlVar("__arg_placeholder__") = "__Arg_placeholder__" (uppercased first char after _)
                    const std::string placeholder = "__Arg_placeholder__";
                    for (const auto& a : node.args) {
                        auto argText = emitExpr(a);
                        auto pos = body.find(placeholder);
                        if (pos != std::string::npos)
                            body.replace(pos, placeholder.size(), argText);
                    }
                }
                return "fun (" + pv + ") ->\n    " + body;
            } else {
                // Kind::Function (&funcName) — produces a 1-arg wrapper.
                // Plain global Kex builtins (not stdlib-module-namespaced —
                // see src/interpreter/stdlib/string.cxx/test.cxx) aren't
                // real top-level functions in the compiled module, unlike a
                // genuine user-defined `let alpha?(c) = ...` — calling them
                // via `apply 'name'/1(...)` undefs. A real, reproduced bug
                // otherwise: `word.filter(&alpha?)` (spec/char_predicates.kex).
                static const std::unordered_map<std::string, std::pair<std::string,std::string>> builtinFns = {
                    {"digit?", {"kex_intrinsic_char", "is_digit"}},
                    {"alpha?", {"kex_intrinsic_char", "is_alpha"}},
                    {"space?", {"kex_intrinsic_char", "is_space"}},
                    {"assert", {"kex_test", "assert"}},
                };
                auto it = builtinFns.find(node.name);
                if (it != builtinFns.end())
                    return "fun (" + pv + ") ->\n    call '" + it->second.first + "':'" + it->second.second + "'(" + pv + ")";
                return "fun (" + pv + ") ->\n    apply '" + node.name + "'/1(" + pv + ")";
            }
        }

        // --- CurryExpr: ~(op) / ~fn(args, _, ...) — full or partial
        // application of an operator or a user-defined top-level function.
        // Matches Evaluator::eval's CurryExpr handling: flatten every arg
        // group into a flat slot list (each slot either bound to a real
        // value or open — a CurryPlaceholder, `_`); if every slot is bound
        // and there are enough of them, apply immediately; otherwise
        // produce a fun taking one parameter per open slot, in order.
        else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
            struct Slot { bool isOpen; std::string valueExpr; };
            std::vector<Slot> slots;
            for (const auto& group : node.argGroups)
                for (const auto& argExpr : group) {
                    if (std::holds_alternative<ast::CurryPlaceholder>(argExpr->kind))
                        slots.push_back({true, ""});
                    else
                        slots.push_back({false, emitExpr(argExpr)});
                }

            int arity = -1;
            if (node.isOperator) {
                arity = 2;
            } else {
                auto fit = m_topLevelFns.find(node.name);
                if (fit != m_topLevelFns.end()) arity = fit->second;
            }

            int openCount = 0;
            for (const auto& s : slots) if (s.isOpen) openCount++;
            bool fullyApplied = openCount == 0 &&
                                (arity >= 0 ? static_cast<int>(slots.size()) >= arity : !slots.empty());

            // Operator name -> Erlang BIF operator symbol (same set
            // BinaryOp emission supports — see its own && / || / + special
            // cases above for why those three aren't in this table: +
            // dispatches through kex_intrinsic_number:add for string-concat/Char+String
            // polymorphism, and &&/|| need short-circuit, neither of which
            // makes sense for a curried 2-arg function value here — a
            // curried `~(&&)` would need its own short-circuit-preserving
            // closure, not attempted here since it's not exercised by any
            // known spec.
            static const std::unordered_map<std::string, std::string> opBif = {
                {"-", "-"}, {"*", "*"}, {"/", "/"}, {"%", "rem"},
                {"==", "=:="}, {"!=", "=/="}, {"<", "<"}, {"<=", "=<"},
                {">", ">"}, {">=", ">="},
            };
            auto emitCall = [&](const std::vector<std::string>& args) -> std::string {
                if (node.isOperator && args.size() >= 2) {
                    if (node.name == "+")
                        return "call 'kex_intrinsic_number':'add'(" + args[0] + ", " + args[1] + ")";
                    // / is polymorphic (int/int -> integer division) — see
                    // BinaryOp's own Slash handling above for the full
                    // rationale; kex_intrinsic_number:divide/2 shares that exact logic so
                    // it isn't duplicated here. A real, reproduced bug
                    // otherwise: `~(/)(_, 2)` then `div2(10)` returned
                    // "5.0" instead of "5" (spec/currying.kex).
                    if (node.name == "/")
                        return "call 'kex_intrinsic_number':'divide'(" + args[0] + ", " + args[1] + ")";
                    auto oit = opBif.find(node.name);
                    if (oit != opBif.end())
                        return "call 'erlang':'" + oit->second + "'(" + args[0] + ", " + args[1] + ")";
                }
                std::string argList;
                for (size_t i = 0; i < args.size(); i++) { if (i) argList += ", "; argList += args[i]; }
                return "apply '" + node.name + "'/" + std::to_string(args.size()) + "(" + argList + ")";
            };

            if (fullyApplied) {
                std::vector<std::string> args;
                for (const auto& s : slots) args.push_back(s.valueExpr);
                return emitCall(args);
            }

            // Partial application: one fresh param per explicit open slot
            // (placeholder `_`), PLUS — matching Evaluator::eval's
            // CurryExpr lambda exactly — extra trailing params for any
            // args beyond what's written at all, when arity is known (e.g.
            // `~add(1)` on a 2-arity `add`: no placeholders, but only 1 of
            // 2 args given, so the result still needs to accept exactly
            // one more argument, appended after the explicit slots — not
            // just `openCount` params, which would wrongly be 0 here). A
            // real, reproduced bug otherwise: `~add(1)` produced a 0-arity
            // fun, so calling `inc(5)` was an arity mismatch
            // (spec/currying.kex).
            int trailingCount = (arity >= 0) ? std::max(0, arity - static_cast<int>(slots.size())) : 0;
            std::vector<std::string> params;
            for (const auto& s : slots) if (s.isOpen) params.push_back(freshVar("P"));
            std::vector<std::string> trailingParams;
            for (int i = 0; i < trailingCount; i++) trailingParams.push_back(freshVar("T"));
            std::string paramList;
            for (size_t i = 0; i < params.size(); i++) { if (i) paramList += ", "; paramList += params[i]; }
            for (size_t i = 0; i < trailingParams.size(); i++) {
                if (!paramList.empty() || i > 0) paramList += ", ";
                paramList += trailingParams[i];
            }
            std::vector<std::string> finalArgs;
            size_t pIdx = 0;
            for (const auto& s : slots) finalArgs.push_back(s.isOpen ? params[pIdx++] : s.valueExpr);
            for (const auto& t : trailingParams) finalArgs.push_back(t);
            return "fun (" + paramList + ") ->\n    " + emitCall(finalArgs);
        }

        // --- Spawn: bind body as a 0-arity fun, then call erlang:spawn/1.
        // Per the emitter's existing convention (see higher-order list methods
        // around line 494), Core Erlang fun literals passed as call arguments
        // are bound to a var first via a let prefix.
        else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            auto body = emitBody(node.body);
            auto fnVar = freshVar("SpawnFn");
            return "let <" + fnVar + "> =\n"
                   "    fun () ->\n"
                   "        " + body + " in\n"
                   "call 'erlang':'spawn'(" + fnVar + ")";
        }

        // --- Receive: native Core Erlang `receive ... after ...`.
        // Core Erlang receive has no 'end' keyword — the expression ends after
        // the after-clause body. A blocking receive (no user-specified timeout)
        // uses `after 'infinity' -> 'true'` as required by the grammar.
        // Patterns are wrapped as `{'kex_msg', <pat>, <sender>}` to match
        // the wire format emitted by `send`.
        else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            std::string senderVar = node.senderBinding
                ? erlVar(*node.senderBinding)
                : freshVar("W");
            std::string result = "receive\n";
            for (const auto& clause : node.clauses) {
                std::string pat = clause.patterns.empty()
                    ? freshVar("W")
                    : emitPattern(clause.patterns[0]);
                result += "  {'kex_msg', " + pat + ", " + senderVar + "}";
                if (clause.guard) {
                    m_inGuard = true;
                    result += " when " + emitExpr(*clause.guard);
                    m_inGuard = false;
                } else
                    result += " when 'true'";
                result += " ->\n    " + emitExpr(clause.body) + "\n";
            }
            if (node.timeout && node.afterBody) {
                result += "after " + emitExpr(*node.timeout) + " ->\n"
                          "    " + emitExpr(*node.afterBody);
            } else {
                result += "after 'infinity' ->\n    'true'";
            }
            return result;
        }

        // Unhandled — emit a placeholder that compiles but fails at runtime.
        else {
            return "call 'erlang':'error'({'kex_unimplemented', " + erlString(typeid(node).name()) + "})";
        }
    }, expr->kind);
}

// ---------------------------------------------------------------------------
// Body: sequence of exprs → nested let chain (forward-recursive)
// ---------------------------------------------------------------------------

// Collect loop-threaded variable names from a single expression. A var is
// "threaded" if the loop body reassigns it — via a plain `x = ...`
// (AssignExpr) OR a mutating `x.push!(..)` call (which is really `x =
// x.push(..)`, see emitBodyFrom's node.mutating handling). Recurses through
// control flow (if/match/loop/block) so a reassignment buried in a branch
// or a `do ... end` match arm is still found — a real, reproduced bug
// otherwise: examples/json_parser.kex's parseNumber accumulates into `var
// chars` only via `chars.push!(c)` inside a `do`-block match arm, so
// `chars` was never threaded and the loop lost every appended char.
void CoreErlangEmitter::collectAssignedExpr(const ast::ExprPtr& e,
                                            std::unordered_set<std::string>& out) {
    if (!e) return;
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            out.insert(node.name);
        } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            if (node.mutating && node.receiver)
                if (auto* id = std::get_if<ast::Identifier>(&node.receiver->kind))
                    out.insert(id->name);
        } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            collectAssigned(node.thenBody, out);
            if (node.elseBody) collectAssigned(*node.elseBody, out);
            for (auto& [cond, b] : node.elifs) collectAssigned(b, out);
        } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            collectAssigned(node.body, out);
        } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            collectAssigned(node.body, out);
        } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            collectAssigned(node.body, out);
        } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            for (auto& clause : node.clauses)
                collectAssignedExpr(clause.body, out);
        }
        // Don't cross Lambda boundaries — lambdas capture by value on BEAM.
    }, e->kind);
}

void CoreErlangEmitter::collectAssigned(const std::vector<ast::ExprPtr>& body,
                                         std::unordered_set<std::string>& out) {
    for (const auto& e : body) collectAssignedExpr(e, out);
}

auto CoreErlangEmitter::makeTailCall(const std::string& loopFn, int loopArity,
                                      const std::vector<std::string>& mutParams) -> std::string {
    std::string args;
    for (const auto& kexName : mutParams) {
        if (!args.empty()) args += ", ";
        auto it = m_varSubst.find(kexName);
        args += (it != m_varSubst.end()) ? it->second : erlVar(kexName);
    }
    return "apply '" + loopFn + "'/" + std::to_string(loopArity) + "(" + args + ")";
}

auto CoreErlangEmitter::emitLoopBodyFrom(const std::vector<ast::ExprPtr>& body, int start,
                                          const std::string& loopFn, int loopArity,
                                          const std::vector<std::string>& mutParams,
                                          const std::string& fallthrough) -> std::string {
    if (start >= (int)body.size())
        return fallthrough.empty() ? makeTailCall(loopFn, loopArity, mutParams) : fallthrough;

    const auto& e = body[start];

    // AssignExpr for a loop-threaded var → fresh binding, update subst, continue
    if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
        if (m_varSubst.count(ae->name)) {
            std::string newVar = freshVar(ae->name);
            std::string val = emitExpr(ae->value);
            auto prev = m_varSubst[ae->name];
            m_varSubst[ae->name] = newVar;
            std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
            m_varSubst[ae->name] = prev;
            return "let <" + newVar + "> =\n    " + val + "\nin\n" + rest;
        }
        // Unknown var — treat as side effect, bind to tmp
        std::string tmp = freshVar("S");
        std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
        return "let <" + tmp + "> =\n    " + emitExpr(ae->value) + "\nin\n" + rest;
    }

    // Mutating method call (`chars.push!(c)`) inside a loop → rebind the
    // receiver var and thread it into the next iteration, exactly like an
    // AssignExpr (see the matching statement-level handling in emitBodyFrom
    // for why a `!` call IS an assignment). Threading it is what lets a
    // loop that accumulates into a `var` via `.push!` actually make
    // progress (examples/json_parser.kex's parseNumber/parseArray).
    if (auto* mc = std::get_if<ast::MethodCall>(&e->kind);
        mc && mc->mutating && mc->receiver) {
        if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
            std::string newVar = freshVar(id->name);
            std::string val = emitExpr(e);
            bool tracked = m_varSubst.count(id->name) > 0;
            std::string prev = tracked ? m_varSubst[id->name] : std::string{};
            m_varSubst[id->name] = newVar;
            std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
            if (tracked) m_varSubst[id->name] = prev; else m_varSubst.erase(id->name);
            return "let <" + newVar + "> =\n    " + val + "\nin\n" + rest;
        }
    }

    // ReturnExpr → exit loop AND enclosing function. When the function has
    // early returns, emitReturnValue throws so the return escapes the
    // letrec loop out to the function's wrapReturnCatch — the only correct
    // behavior when the loop isn't itself in the function's tail position.
    if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind)) {
        if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
            std::string cont = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
            return "case " + emitExpr(ti->condition) + " of\n"
                   "  'true' when 'true' ->\n    " + emitReturnValue(ti->expr) + "\n"
                   "  'false' when 'true' ->\n    " + cont + "\n"
                   "end";
        }
        return emitReturnValue(re->value);  // discard loop rest
    }

    // Shared by every BreakExpr site (bare, or wrapped in a TrailingIf) —
    // returns the current mutable var state as a tuple (or 'ok' if none),
    // exiting the letrec loop function entirely.
    auto breakTuple = [&]() -> std::string {
        if (mutParams.empty()) return "'ok'";
        std::string tuple = "{";
        for (size_t i = 0; i < mutParams.size(); i++) {
            if (i > 0) tuple += ", ";
            auto it = m_varSubst.find(mutParams[i]);
            tuple += (it != m_varSubst.end()) ? it->second : "_";
        }
        return tuple + "}";
    };

    // BreakExpr → return current mutable var state as a tuple (or 'ok' if none)
    if (std::get_if<ast::BreakExpr>(&e->kind)) {
        return breakTuple();
    }

    // NextExpr → skip straight to the next iteration (tail-call the loop
    // function again with the current mutable state) — was previously
    // unhandled here at all, falling through to the generic (non-loop-
    // aware) default case below, which called plain emitExpr() on it and
    // hit codegen's catch-all "unimplemented AST node" path (NextExpr is
    // only meaningful inside a loop, so only this loop-aware emitter can
    // know what to do with it).
    if (std::get_if<ast::NextExpr>(&e->kind)) {
        return makeTailCall(loopFn, loopArity, mutParams);
    }

    // Bare TrailingIf as a loop-body statement (`break if cond`, `next if
    // cond`, or any other `expr if cond` one-liner) — same real bug class
    // as NextExpr above: falling through to plain emitExpr() couldn't
    // handle a break/next inside the trailing-if's wrapped expr at all.
    // Evaluate `expr` (loop-aware) only when `cond` holds; otherwise
    // continue with the rest of the loop body.
    if (auto* ti = std::get_if<ast::TrailingIf>(&e->kind)) {
        std::string cont = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
        std::string thenPart;
        if (std::get_if<ast::BreakExpr>(&ti->expr->kind)) {
            thenPart = breakTuple();
        } else if (std::get_if<ast::NextExpr>(&ti->expr->kind)) {
            thenPart = makeTailCall(loopFn, loopArity, mutParams);
        } else {
            thenPart = emitExpr(ti->expr);
        }
        return "case " + emitExpr(ti->condition) + " of\n"
               "  'true' when 'true' -> " + thenPart + "\n"
               "  'false' when 'true' -> " + cont + "\n"
               "end";
    }

    // Nested LoopExpr/WhileExpr (a loop statement inside another loop's
    // body) — was previously unhandled here at all (only emitBodyFrom, the
    // plain top-level body emitter, dispatched to emitLoopExpr), falling
    // through to the generic default case's plain emitExpr() and hitting
    // codegen's "unimplemented AST node" catch-all. Pass the CURRENT loop's
    // own state as outerLoop so the nested loop's own post-loop
    // continuation correctly resumes here (tail-call/fallthrough) instead
    // of assuming top-level "just return" semantics.
    if (auto* le = std::get_if<ast::LoopExpr>(&e->kind)) {
        LoopContext outerCtx{loopFn, loopArity, mutParams, fallthrough};
        return emitLoopExpr(le->body, nullptr, body, start, &outerCtx);
    }
    if (auto* we = std::get_if<ast::WhileExpr>(&e->kind)) {
        LoopContext outerCtx{loopFn, loopArity, mutParams, fallthrough};
        return emitLoopExpr(we->body, &we->condition, body, start, &outerCtx);
    }

    // IfExpr → emit branches with loop context
    if (auto* ie = std::get_if<ast::IfExpr>(&e->kind)) {
        std::string cond = emitExpr(ie->condition);
        // When the `if` is the LAST loop-body statement, a branch that falls
        // off its end restarts the loop — and the restart tail call must use
        // that branch's OWN post-assignment variable bindings. Pre-baking one
        // shared `cont` tail call here (with the pre-branch bindings) was a
        // real, reproduced infinite loop: `loop / if c / p = p.advance / i =
        // i + 1 / else return .. / end / end` recursed with the OLD p/i, so
        // the counter never advanced (examples/json_parser.kex's
        // expectLiteral, via spec/json_parser.spec.kex). So in that case pass
        // the (empty or inherited) `fallthrough` straight through, letting
        // each branch's base case regenerate the tail call against its own
        // updated m_varSubst. Only when statements actually FOLLOW the `if`
        // is a shared pre-computed continuation correct (and needed — see
        // emitLoopBodyFrom's doc comment on the break-after-if case).
        bool ifIsLast = (start + 1 >= (int)body.size());
        std::string branchFallthrough;
        std::string cont; // concrete continuation for the no-else false branch
        if (ifIsLast) {
            branchFallthrough = fallthrough;
            cont = fallthrough.empty() ? makeTailCall(loopFn, loopArity, mutParams) : fallthrough;
        } else {
            cont = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
            branchFallthrough = cont;
        }
        // Save subst state for merging branches
        auto substSnap = m_varSubst;
        std::string thenPart = emitLoopBodyFrom(ie->thenBody, 0, loopFn, loopArity, mutParams, branchFallthrough);
        m_varSubst = substSnap;
        if (ie->elseBody) {
            std::string elsePart = emitLoopBodyFrom(*ie->elseBody, 0, loopFn, loopArity, mutParams, branchFallthrough);
            m_varSubst = substSnap;
            return "case " + cond + " of\n"
                   "  'true' when 'true' ->\n    " + thenPart + "\n"
                   "  'false' when 'true' ->\n    " + elsePart + "\n"
                   "end";
        }
        // No else: false branch = continue with rest of loop body
        return "case " + cond + " of\n"
               "  'true' when 'true' ->\n    " + thenPart + "\n"
               "  'false' when 'true' ->\n    " + cont + "\n"
               "end";
    }

    // MatchExpr → emit with loop-aware arm bodies
    if (auto* me = std::get_if<ast::MatchExpr>(&e->kind)) {
        bool multiValue = std::holds_alternative<ast::TupleExpr>(me->subject->kind);
        std::string scrutinee;
        if (multiValue) {
            auto& tup = std::get<ast::TupleExpr>(me->subject->kind);
            scrutinee = "<";
            for (size_t j = 0; j < tup.elements.size(); j++) {
                if (j > 0) scrutinee += ", ";
                scrutinee += emitExpr(tup.elements[j]);
            }
            scrutinee += ">";
        } else {
            scrutinee = emitExpr(me->subject);
        }
        // Bind `|x|` subject binder, if any — see the matching non-loop
        // MatchExpr handling in emitExpr for why (spec/when_guards.kex).
        std::string bindingPrefix;
        if (me->subjectBinding && !multiValue) {
            auto bv = erlVar(*me->subjectBinding);
            bindingPrefix = "let <" + bv + "> =\n    " + scrutinee + "\nin\n";
            scrutinee = bv;
        }
        // The continuation ("whatever comes after this match in the loop
        // body") is regenerated PER ARM, after that arm's own assignment has
        // updated m_varSubst — because when it ends in a loop-restart tail
        // call, that call must carry the arm's post-assignment bindings. A
        // real, reproduced infinite loop otherwise: `loop / match p.current
        // do Just(' ') -> p = p.advance ; _ -> return p end / end`
        // (examples/json_parser.kex's skipWhitespace) baked one shared `cont`
        // with the pre-assignment `p`, so the whitespace arm advanced a fresh
        // var but restarted the loop with the OLD p — never making progress.
        auto contNow = [&]() {
            return emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
        };
        std::string result = bindingPrefix + "case " + scrutinee + " of\n";
        for (const auto& clause : me->clauses) {
            std::string pat = clause.patterns.empty() ? freshVar("W") : emitPattern(clause.patterns[0]);
            result += "  " + pat;
            if (clause.guard) {
                m_inGuard = true;
                result += " when " + emitExpr(*clause.guard);
                m_inGuard = false;
            } else
                result += " when 'true'";
            result += " ->\n";
            // Emit arm body in loop context — a match arm is exactly a
            // single loop-body statement, so this reuses the exact same
            // per-statement handling emitLoopBodyFrom itself would apply
            // (break/next/assign/return/other). A bare `break`/`next` arm —
            // e.g. `5 -> break` — previously fell into the generic "other
            // expr" case below, which called plain emitExpr() on it and hit
            // codegen's "unimplemented AST node" catch-all (break/next are
            // only meaningful in a loop context).
            auto substSnap = m_varSubst;
            std::string armBody;
            if (std::get_if<ast::BreakExpr>(&clause.body->kind)) {
                armBody = breakTuple();
            } else if (std::get_if<ast::NextExpr>(&clause.body->kind)) {
                armBody = makeTailCall(loopFn, loopArity, mutParams);
            } else if (auto* ae2 = std::get_if<ast::AssignExpr>(&clause.body->kind)) {
                if (m_varSubst.count(ae2->name)) {
                    std::string newVar = freshVar(ae2->name);
                    std::string val = emitExpr(ae2->value);
                    m_varSubst[ae2->name] = newVar;
                    armBody = "let <" + newVar + "> =\n    " + val + "\nin\n" + contNow();
                } else {
                    std::string tmp = freshVar("S");
                    armBody = "let <" + tmp + "> =\n    " + emitExpr(ae2->value) + "\nin\n" + contNow();
                }
            } else if (auto* mc2 = std::get_if<ast::MethodCall>(&clause.body->kind);
                       mc2 && mc2->mutating && mc2->receiver &&
                       std::get_if<ast::Identifier>(&mc2->receiver->kind)) {
                // A mutating `x.push!(..)` arm rebinds its receiver, exactly
                // like an AssignExpr arm — thread it into the continuation so
                // the mutation survives the loop iteration (the nested-match
                // escape arms in examples/json_parser.kex's parseString).
                auto* id = std::get_if<ast::Identifier>(&mc2->receiver->kind);
                std::string newVar = freshVar(id->name);
                std::string val = emitExpr(clause.body);
                m_varSubst[id->name] = newVar;
                armBody = "let <" + newVar + "> =\n    " + val + "\nin\n" + contNow();
            } else if (auto* re2 = std::get_if<ast::ReturnExpr>(&clause.body->kind)) {
                armBody = emitReturnValue(re2->value);  // exits the function entirely
            } else if (auto* be2 = std::get_if<ast::BlockExpr>(&clause.body->kind)) {
                // A `do ... end` arm body is a multi-statement loop-body
                // fragment — emit it loop-aware (so its own assignments/
                // returns/breaks thread correctly and the trailing loop
                // restart carries the arm's post-block bindings), with the
                // rest-after-the-match as its fallthrough. When the match is
                // the last loop statement, pass the (empty/inherited)
                // fallthrough through so the block's base case regenerates the
                // restart tail call against its updated m_varSubst — same
                // reasoning as the IfExpr `ifIsLast` case above
                // (examples/json_parser.kex's parseNumber arm).
                bool matchIsLast = (start + 1 >= (int)body.size());
                std::string blockFallthrough = matchIsLast ? fallthrough : contNow();
                armBody = emitLoopBodyFrom(be2->body, 0, loopFn, loopArity, mutParams, blockFallthrough);
            } else {
                std::string tmp = freshVar("S");
                armBody = "let <" + tmp + "> =\n    " + emitExpr(clause.body) + "\nin\n" + contNow();
            }
            m_varSubst = substSnap;
            result += "    " + armBody + "\n";
        }
        result += "end";
        return result;
    }

    // VarExpr inside loop body — local variable (not a loop param)
    if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
        std::string ceVar = erlVar(ve->name);
        m_varSubst[ve->name] = ceVar;
        std::string val = emitExpr(ve->value);
        std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
        m_varSubst.erase(ve->name);
        return "let <" + ceVar + "> =\n    " + val + "\nin\n" + rest;
    }

    // LetExpr inside loop body
    if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
        if (le->pattern) {
            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                std::string ceVar = erlVar(vp->name);
                std::string val = emitExpr(le->value);
                std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
                return "let <" + ceVar + "> =\n    " + val + "\nin\n" + rest;
            } else {
                auto tmpVar = freshVar("D");
                auto val = emitExpr(le->value);
                std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
                return "let <" + tmpVar + "> =\n    " + val + "\nin\n"
                       + bindPatternLets(le->pattern, tmpVar) + rest;
            }
        }
    }

    // Default: bind to tmp, continue
    std::string tmp = freshVar("S");
    std::string val = emitExpr(e);
    std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams, fallthrough);
    return "let <" + tmp + "> =\n    " + val + "\nin\n" + rest;
}

auto CoreErlangEmitter::emitLoopExpr(const std::vector<ast::ExprPtr>& loopBody,
                                      const ast::ExprPtr* condition,
                                      const std::vector<ast::ExprPtr>& outerBody,
                                      int outerStart,
                                      const LoopContext* outerLoop) -> std::string {
    // Find which vars from the outer scope are mutated inside the loop.
    std::unordered_set<std::string> mutated;
    collectAssigned(loopBody, mutated);

    // Filter to vars currently tracked in m_varSubst (declared before this loop).
    std::vector<std::string> mutParams;  // ordered list of kex names
    std::vector<std::string> initVals;   // current CE var names for initial call
    for (const auto& name : mutated) {
        auto it = m_varSubst.find(name);
        if (it != m_varSubst.end()) {
            mutParams.push_back(name);
            initVals.push_back(it->second);
        }
    }

    std::string loopFn = "__loop_" + std::to_string(m_loopCounter++);
    int loopArity = static_cast<int>(mutParams.size());

    // Build fresh parameter names and set up m_varSubst for loop body emission.
    auto savedSubst = m_varSubst;
    std::string paramList;
    for (size_t j = 0; j < mutParams.size(); j++) {
        std::string pv = freshVar(mutParams[j]);
        if (j > 0) paramList += ", ";
        paramList += pv;
        m_varSubst[mutParams[j]] = pv;
    }

    // Build the tuple returned by break / while-false-exit.
    // Holds the current values of all mutable params in mutParams order.
    auto makeBreakTuple = [&]() -> std::string {
        if (loopArity == 0) return "'ok'";
        std::string t = "{";
        for (size_t i = 0; i < mutParams.size(); i++) {
            if (i > 0) t += ", ";
            auto it = m_varSubst.find(mutParams[i]);
            t += (it != m_varSubst.end()) ? it->second : "_";
        }
        return t + "}";
    };

    // Emit the loop body (infinite loop or while).
    std::string loopBodyStr;
    if (condition) {
        // while: wrap body in condition check; false branch exits with current state.
        std::string innerBody = emitLoopBodyFrom(loopBody, 0, loopFn, loopArity, mutParams);
        std::string falseExit = makeBreakTuple();
        loopBodyStr = "case " + emitExpr(*condition) + " of\n"
                      "  'true' when 'true' ->\n    " + innerBody + "\n"
                      "  'false' when 'true' -> " + falseExit + "\n"
                      "end";
    } else {
        loopBodyStr = emitLoopBodyFrom(loopBody, 0, loopFn, loopArity, mutParams);
    }

    m_varSubst = savedSubst;

    // Build post-loop variable names: fresh SSA vars that receive the mutable state
    // extracted from the loop result tuple. Update m_varSubst before emitting rest.
    std::vector<std::string> postLoopVars;
    if (loopArity > 0) {
        for (const auto& name : mutParams) {
            std::string v = freshVar(name);
            postLoopVars.push_back(v);
            m_varSubst[name] = v;
        }
    }

    // Build initial call argument list.
    std::string initArgs;
    for (size_t j = 0; j < initVals.size(); j++) {
        if (j > 0) initArgs += ", ";
        initArgs += initVals[j];
    }

    // Emit the rest of the outer body with updated subst so references to mutable
    // vars after the loop resolve to the post-loop SSA names. When this loop is
    // itself nested inside another loop (outerLoop != nullptr), "the rest"
    // must continue via the OUTER loop's own loop-aware emission (tail-call/
    // fallthrough), not emitBodyFrom's plain top-level semantics.
    std::string rest = outerLoop
        ? emitLoopBodyFrom(outerBody, outerStart + 1, outerLoop->loopFn, outerLoop->loopArity,
                           outerLoop->mutParams, outerLoop->fallthrough)
        : emitBodyFrom(outerBody, outerStart + 1);

    // Restore subst (the caller's emitBodyFrom chain handles its own scope).
    m_varSubst = savedSubst;

    std::ostringstream out;
    out << "letrec '" << loopFn << "'/" << loopArity << " =\n"
        << "  fun (" << paramList << ") ->\n"
        << "    " << loopBodyStr << "\n"
        << "in\n";

    // The "just apply and return directly" shortcuts below are only valid at
    // true top level (outerLoop == nullptr): there, when this loop is the
    // final statement, its own result can legitimately BE the enclosing
    // function's return value. When nested, `rest` must always be spliced
    // in afterward (its own base case already correctly resolves to the
    // outer loop's fallthrough/tail-call even when outerStart+1 is past the
    // end of outerBody), so the shortcuts are skipped entirely.
    if (loopArity == 0) {
        // No mutable state — just run the loop and continue.
        if (!outerLoop && outerStart + 1 >= (int)outerBody.size()) {
            out << "apply '" << loopFn << "'/0(" << initArgs << ")";
        } else {
            std::string lr = freshVar("LR");
            out << "let <" << lr << "> =\n    apply '" << loopFn << "'/0(" << initArgs << ")\n"
                << "in\n" << rest;
        }
    } else if (!outerLoop && outerStart + 1 >= (int)outerBody.size()) {
        // Loop is the last statement — return the loop result directly.
        out << "apply '" << loopFn << "'/" << loopArity << "(" << initArgs << ")";
    } else {
        // Bind loop result tuple and destructure into post-loop SSA vars.
        std::string lr = freshVar("LR");
        out << "let <" << lr << "> =\n    apply '" << loopFn << "'/" << loopArity
            << "(" << initArgs << ")\n"
            << "in\n";
        for (size_t i = 0; i < postLoopVars.size(); i++) {
            // Skip extraction for vars not referenced in rest (avoids erlc unused-result warning)
            if (rest.find(postLoopVars[i]) == std::string::npos) continue;
            out << "let <" << postLoopVars[i] << "> =\n"
                << "    call 'erlang':'element'(" << (i + 1) << ", " << lr << ")\n"
                << "in\n";
        }
        out << rest;
    }
    return out.str();
}

auto CoreErlangEmitter::emitBranchResult(const std::vector<ast::ExprPtr>& stmts, int idx,
                                         const std::vector<std::string>& mergeVars) -> std::string {
    if (idx >= (int)stmts.size()) {
        auto sub = [&](const std::string& n) {
            auto it = m_varSubst.find(n);
            return it != m_varSubst.end() ? it->second : erlVar(n);
        };
        if (mergeVars.size() == 1) return sub(mergeVars[0]);
        std::string t = "{";
        for (size_t i = 0; i < mergeVars.size(); i++) {
            if (i) t += ", ";
            t += sub(mergeVars[i]);
        }
        return t + "}";
    }
    const auto& e = stmts[idx];
    // Assignment → rebind, thread into the rest of the branch.
    if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
        std::string nv = freshVar(ae->name);
        std::string val = emitExpr(ae->value);
        m_varSubst[ae->name] = nv;
        return "let <" + nv + "> =\n    " + val + "\nin\n" +
               emitBranchResult(stmts, idx + 1, mergeVars);
    }
    // Mutating call (`chars.push!(c)`) → same, rebinding its receiver var.
    if (auto* mc = std::get_if<ast::MethodCall>(&e->kind); mc && mc->mutating && mc->receiver) {
        if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
            std::string nv = freshVar(id->name);
            std::string val = emitExpr(e);
            m_varSubst[id->name] = nv;
            return "let <" + nv + "> =\n    " + val + "\nin\n" +
                   emitBranchResult(stmts, idx + 1, mergeVars);
        }
    }
    // Early return throws out of the whole function (the merge tuple after
    // is then dead code, which is fine).
    if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind))
        return emitReturnValue(re->value);
    // Any other statement: evaluate for its effect, continue.
    std::string tmp = freshVar("S");
    std::string val = emitExpr(e);
    return "let <" + tmp + "> =\n    " + val + "\nin\n" +
           emitBranchResult(stmts, idx + 1, mergeVars);
}

auto CoreErlangEmitter::emitBodyFrom(const std::vector<ast::ExprPtr>& body, int start) -> std::string {
    if (start >= (int)body.size()) return "'ok'";

    const auto& e = body[start];
    const bool isLast = (start == (int)body.size() - 1);

    // VarExpr: introduce mutable binding, track in m_varSubst.
    if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
        std::string ceVar = erlVar(ve->name);
        auto prev = m_varSubst[ve->name];  // save (empty if not set)
        m_varSubst[ve->name] = ceVar;
        std::string val = emitExpr(ve->value);
        std::string rest = isLast ? "'" + ceVar + "'" : emitBodyFrom(body, start + 1);
        // For last-in-body, VarExpr's value is the block value (unusual but handle gracefully)
        if (isLast) rest = val;
        else rest = emitBodyFrom(body, start + 1);
        m_varSubst.erase(ve->name);
        if (!prev.empty()) m_varSubst[ve->name] = prev;
        return "let <" + ceVar + "> =\n    " + val + "\nin\n" + rest;
    }

    // AssignExpr: SSA renaming (create fresh var, shadow previous binding).
    if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
        std::string newVar = freshVar(ae->name);
        std::string val = emitExpr(ae->value);
        auto prev = m_varSubst.count(ae->name) ? m_varSubst[ae->name] : std::string{};
        m_varSubst[ae->name] = newVar;
        std::string rest = isLast ? newVar : emitBodyFrom(body, start + 1);
        m_varSubst.erase(ae->name);
        if (!prev.empty()) m_varSubst[ae->name] = prev;
        return "let <" + newVar + "> =\n    " + val + "\nin\n" + rest;
    }

    // Mutating method call (`x.push!(v)`, `name.upperCase!`): rebinds the
    // receiver variable to the (non-mutating) call's result — exactly an
    // `x = x.push(v)` assignment, matching Evaluator's node.mutating
    // handling (Kex values are immutable; `!` only rebinds the binding).
    // emitExpr already emits the underlying method call regardless of the
    // `!`, so this just captures that result into a fresh SSA name for the
    // receiver var (spec/mutating_calls.kex, examples/json_parser.kex's
    // `chars.push!(c)`).
    if (auto* mc = std::get_if<ast::MethodCall>(&e->kind);
        mc && mc->mutating && mc->receiver) {
        if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
            std::string newVar = freshVar(id->name);
            std::string val = emitExpr(e);
            auto prev = m_varSubst.count(id->name) ? m_varSubst[id->name] : std::string{};
            m_varSubst[id->name] = newVar;
            std::string rest = isLast ? newVar : emitBodyFrom(body, start + 1);
            m_varSubst.erase(id->name);
            if (!prev.empty()) m_varSubst[id->name] = prev;
            return "let <" + newVar + "> =\n    " + val + "\nin\n" + rest;
        }
    }

    // LoopExpr / WhileExpr
    if (auto* le = std::get_if<ast::LoopExpr>(&e->kind))
        return emitLoopExpr(le->body, nullptr, body, start);
    if (auto* we = std::get_if<ast::WhileExpr>(&e->kind))
        return emitLoopExpr(we->body, &we->condition, body, start);

    // LetExpr: simple binding or destructuring.
    if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
        if (le->pattern) {
            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                std::string ceVar = erlVar(vp->name);
                std::string val = emitExpr(le->value);
                std::string rest = isLast ? ceVar : emitBodyFrom(body, start + 1);
                return "let <" + ceVar + "> =\n    " + val + "\nin\n" + rest;
            }
            // Destructuring: bind the value to a temp, then emit accessor-based
            // let-bindings that extract each field.
            auto tmpVar = freshVar("D");
            auto val    = emitExpr(le->value);
            std::string rest = isLast ? "'ok'" : emitBodyFrom(body, start + 1);
            return "let <" + tmpVar + "> =\n    " + val + "\nin\n"
                   + bindPatternLets(le->pattern, tmpVar) + rest;
        }
        std::string tmp = freshVar("T");
        std::string rest = isLast ? tmp : emitBodyFrom(body, start + 1);
        return "let <" + tmp + "> =\n    " + emitExpr(le->value) + "\nin\n" + rest;
    }

    // ReturnExpr: early return (discard rest). emitReturnValue throws
    // (caught by the function's wrapReturnCatch) when the function has any
    // early return, so this is correct whether the return is in tail
    // position or the conditional `return X if cond` continues to `cont`.
    if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind)) {
        if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
            std::string cont = isLast ? "'ok'" : emitBodyFrom(body, start + 1);
            return "case " + emitExpr(ti->condition) + " of\n"
                   "  'true' when 'true' ->\n    " + emitReturnValue(ti->expr) + "\n"
                   "  'false' when 'true' ->\n    " + cont + "\n"
                   "end";
        }
        if (auto* ie = std::get_if<ast::IfExpr>(&re->value->kind)) {
            if (!ie->elseBody && ie->elifs.empty()) {
                std::string cont = isLast ? "'ok'" : emitBodyFrom(body, start + 1);
                return "case " + emitExpr(ie->condition) + " of\n"
                       "  'true' when 'true' ->\n    " + emitBody(ie->thenBody) + "\n"
                       "  'false' when 'true' ->\n    " + cont + "\n"
                       "end";
            }
        }
        return emitReturnValue(re->value);  // unconditional return — discard rest
    }

    // Conditional-assignment merge: a statement-level `if` whose branches
    // reassign already-tracked mutable vars must make those new values
    // visible to the statements AFTER the `if` — otherwise the assignments
    // are lost when the branch's local SSA names go out of scope. Emit the
    // `if` as a case that yields the post-branch values (bare var, or a
    // tuple for several), then rebind them before continuing. A real,
    // reproduced bug otherwise: examples/json_parser.kex's parseNumber
    // consumes a leading `-` and advances the parser inside an `if` before
    // its digit loop, but those updates reverted, so `-3.5` reached the
    // loop with empty digits (spec/json_parser.spec.kex).
    if (auto* ie = std::get_if<ast::IfExpr>(&e->kind);
        ie && !isLast && ie->elifs.empty()) {
        std::unordered_set<std::string> assignedSet;
        collectAssigned(ie->thenBody, assignedSet);
        if (ie->elseBody) collectAssigned(*ie->elseBody, assignedSet);
        std::vector<std::string> mergeVars;
        for (const auto& name : assignedSet)
            if (m_varSubst.count(name)) mergeVars.push_back(name);
        std::sort(mergeVars.begin(), mergeVars.end());
        if (!mergeVars.empty()) {
            auto snap = m_varSubst;
            std::string thenRes = emitBranchResult(ie->thenBody, 0, mergeVars);
            m_varSubst = snap;
            std::string elseRes = ie->elseBody
                ? emitBranchResult(*ie->elseBody, 0, mergeVars)
                : emitBranchResult({}, 0, mergeVars); // no else: keep current values
            m_varSubst = snap;

            std::string caseExpr =
                "case " + emitExpr(ie->condition) + " of\n"
                "  'true' when 'true' ->\n    " + thenRes + "\n"
                "  'false' when 'true' ->\n    " + elseRes + "\n"
                "end";

            std::string out;
            if (mergeVars.size() == 1) {
                std::string nv = freshVar(mergeVars[0]);
                out = "let <" + nv + "> =\n    " + caseExpr + "\nin\n";
                m_varSubst[mergeVars[0]] = nv;
            } else {
                std::string merged = freshVar("Merged");
                out = "let <" + merged + "> =\n    " + caseExpr + "\nin\n";
                for (size_t i = 0; i < mergeVars.size(); i++) {
                    std::string nv = freshVar(mergeVars[i]);
                    out += "let <" + nv + "> =\n    call 'erlang':'element'(" +
                           std::to_string(i + 1) + ", " + merged + ")\nin\n";
                    m_varSubst[mergeVars[i]] = nv;
                }
            }
            std::string rest = emitBodyFrom(body, start + 1);
            m_varSubst = snap;
            return out + rest;
        }
    }

    // IfExpr with return-only then-branch (no else): false path continues to rest.
    if (auto* ie = std::get_if<ast::IfExpr>(&e->kind)) {
        bool thenEndsWithReturn = !ie->thenBody.empty() &&
            std::get_if<ast::ReturnExpr>(&ie->thenBody.back()->kind);
        bool noElse = !ie->elseBody && ie->elifs.empty();
        if (thenEndsWithReturn && noElse) {
            std::string cont = isLast ? "'ok'" : emitBodyFrom(body, start + 1);
            return "case " + emitExpr(ie->condition) + " of\n"
                   "  'true' when 'true' ->\n    " + emitBody(ie->thenBody) + "\n"
                   "  'false' when 'true' ->\n    " + cont + "\n"
                   "end";
        }
    }

    // Last expression: emit as value.
    if (isLast) return emitExpr(e);

    // Non-last statement: bind to tmp, continue.
    std::string tmp = freshVar("S");
    std::string val = emitExpr(e);
    std::string rest = emitBodyFrom(body, start + 1);
    return "let <" + tmp + "> =\n    " + val + "\nin\n" + rest;
}

auto CoreErlangEmitter::emitBody(const std::vector<ast::ExprPtr>& body) -> std::string {
    if (body.empty()) return "'ok'";
    // Save m_varSubst — emitBodyFrom modifies it during recursion but restores as it unwinds.
    auto savedSubst = m_varSubst;
    auto result = emitBodyFrom(body, 0);
    m_varSubst = savedSubst;
    return result;
}

// ---------------------------------------------------------------------------
// Pattern destructuring → let chain
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::bindPatternLets(const ast::PatternPtr& pat, const std::string& src) -> std::string {
    if (!pat) return "";
    return std::visit([&](const auto& p) -> std::string {
        using PT = std::decay_t<decltype(p)>;
        std::string lets;
        if constexpr (std::is_same_v<PT, ast::VarPattern>) {
            auto v = erlVar(p.name);
            if (v != src) lets += "let <" + v + "> = " + src + " in\n";
        } else if constexpr (std::is_same_v<PT, ast::RecordPattern>) {
            for (const auto& field : p.fields) {
                // Use direct element() if we know the position, otherwise apply accessor.
                // Direct element() avoids infinite recursion when a user method has the
                // same name as the auto-accessor it's trying to use internally.
                std::string extracted;
                auto it = m_fieldAccessors.find(field.name);
                if (it != m_fieldAccessors.end() && !it->second.empty()) {
                    // All entries at same position → single element call
                    int pos = it->second[0].second;
                    extracted = "call 'erlang':'element'(" + std::to_string(pos) + ", " + src + ")";
                } else {
                    extracted = "apply '" + field.name + "'/1(" + src + ")";
                }
                if (field.pattern) {
                    auto tmp = freshVar(erlVar(field.name));
                    lets += "let <" + tmp + "> = " + extracted + " in\n";
                    lets += bindPatternLets(*field.pattern, tmp);
                } else {
                    lets += "let <" + erlVar(field.name) + "> = " + extracted + " in\n";
                }
            }
        }
        return lets;
    }, pat->kind);
}

// ---------------------------------------------------------------------------
// Function definition
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitFunctionDef(const ast::FunctionDef& fn) -> std::string {
    if (fn.clauses.empty()) return "";

    int arity = static_cast<int>(fn.clauses[0].params.size());
    m_exports.push_back({fn.name, arity});

    // Build a synthetic arg list for the fun head: _Arg0, _Arg1, ...
    std::vector<std::string> argVars;
    for (int i = 0; i < arity; i++)
        argVars.push_back("_Arg" + std::to_string(i));

    std::string funHead = "(";
    for (size_t i = 0; i < argVars.size(); i++) {
        if (i > 0) funHead += ", ";
        funHead += argVars[i];
    }
    funHead += ")";

    // Single-clause function: emit directly.
    if (fn.clauses.size() == 1) {
        const auto& clause = fn.clauses[0];
        // Bind params in the body via a prefix let chain.
        std::vector<ast::ExprPtr> paramBindings;
        for (size_t i = 0; i < clause.params.size(); i++) {
            const auto& param = clause.params[i];
            // Build a synthetic LetExpr to bind the arg var to its param name.
            // For simple named params, just use the arg var directly as the variable name.
        }
        // For now: params with a name get the arg var renamed via let in the body.
        // Build body with param bindings prepended.
        std::string body;

        // Collect param name bindings
        std::string paramLets;
        for (size_t i = 0; i < clause.params.size(); i++) {
            const auto& param = clause.params[i];
            std::string paramName;
            if (param.name) {
                paramName = erlVar(*param.name);
            } else if (param.pattern) {
                if (auto* vp = std::get_if<ast::VarPattern>(&(*param.pattern)->kind))
                    paramName = erlVar(vp->name);
            }
            if (!paramName.empty() && paramName != argVars[i]) {
                paramLets += "let <" + paramName + "> = " + argVars[i] + " in\n";
            }
        }

        body = paramLets + emitBody(clause.body);

        std::ostringstream out;
        out << "'" << fn.name << "'/" << arity << " =\n";
        out << "  fun " << funHead << " ->\n";
        out << "    " << body << "\n";
        return out.str();
    }

    // Multi-clause: delegate to emitFunctionGroup with a single-element group.
    return emitFunctionGroup({&fn});
}

auto CoreErlangEmitter::emitFunctionGroup(const std::vector<const ast::FunctionDef*>& group,
                                          bool hasImplicitThis,
                                          const std::string& emitNameOverride)
    -> std::string
{
    if (group.empty()) return "";
    const auto& first = *group[0];
    const std::string& emitName = emitNameOverride.empty() ? first.name : emitNameOverride;

    // Count total clauses across all nodes
    int totalClauses = 0;
    for (const auto* fn : group) totalClauses += static_cast<int>(fn->clauses.size());
    if (totalClauses == 0) return "";

    // If any clause has an early `return`, its returns compile to throws
    // and the emitted body is wrapped in the kex_return try/catch. Scoped
    // to this function via the RAII-ish save/restore of m_returnThrows so
    // nested function emission isn't affected.
    bool anyReturn = false;
    for (const auto* fn : group)
        for (const auto& cl : fn->clauses)
            if (bodyHasReturn(cl.body)) { anyReturn = true; break; }
    bool savedReturnThrows = m_returnThrows;
    m_returnThrows = anyReturn;
    auto restoreRet = [&]() { m_returnThrows = savedReturnThrows; };

    int explicitArity = static_cast<int>(first.clauses.empty() ? 0 : first.clauses[0].params.size());
    // If the make block adds `this` as an implicit first receiver, total arity is +1.
    int arity = explicitArity + (hasImplicitThis ? 1 : 0);
    m_exports.push_back({emitName, arity});

    // Synthetic arg vars: _Arg0, _Arg1, ...
    std::vector<std::string> argVars;
    for (int i = 0; i < arity; i++)
        argVars.push_back("_Arg" + std::to_string(i));

    std::string funHead = "(";
    for (size_t i = 0; i < argVars.size(); i++) {
        if (i > 0) funHead += ", ";
        funHead += argVars[i];
    }
    funHead += ")";

    // Helper: generate let-bindings for a single parameter vs its arg variable.
    // Recursively generate let-bindings that destructure a pattern against a source expr.
    std::function<std::string(const ast::PatternPtr&, const std::string&)> bindPatternLets;
    bindPatternLets = [&](const ast::PatternPtr& pat, const std::string& src) -> std::string {
        if (!pat) return "";
        return std::visit([&](const auto& p) -> std::string {
            using PT = std::decay_t<decltype(p)>;
            std::string lets;
            if constexpr (std::is_same_v<PT, ast::VarPattern>) {
                auto v = erlVar(p.name);
                if (v != src) lets += "let <" + v + "> = " + src + " in\n";
            } else if constexpr (std::is_same_v<PT, ast::RecordPattern>) {
                for (const auto& field : p.fields) {
                    std::string extracted;
                    auto it = m_fieldAccessors.find(field.name);
                    if (it != m_fieldAccessors.end() && !it->second.empty()) {
                        int pos = it->second[0].second;
                        extracted = "call 'erlang':'element'(" + std::to_string(pos) + ", " + src + ")";
                    } else {
                        extracted = "apply '" + field.name + "'/1(" + src + ")";
                    }
                    if (field.pattern) {
                        auto tmp = freshVar(erlVar(field.name));
                        lets += "let <" + tmp + "> = " + extracted + " in\n";
                        lets += bindPatternLets(*field.pattern, tmp);
                    } else {
                        lets += "let <" + erlVar(field.name) + "> = " + extracted + " in\n";
                    }
                }
            } else if constexpr (std::is_same_v<PT, ast::RangePattern>) {
                if (p.start)
                    if (auto* sv = std::get_if<ast::VarPattern>(&p.start->kind))
                        lets += "let <" + erlVar(sv->name) + "> = call 'erlang':'hd'(" + src + ") in\n";
                if (p.end)
                    if (auto* ev = std::get_if<ast::VarPattern>(&p.end->kind))
                        lets += "let <" + erlVar(ev->name) + "> = call 'lists':'last'(" + src + ") in\n";
            }
            return lets;
        }, pat->kind);
    };


    auto bindParamLets = [&](const ast::Param& param, const std::string& argVar) -> std::string {
        std::string lets;
        if (param.name) {
            auto v = erlVar(*param.name);
            if (v != argVar) lets += "let <" + v + "> = " + argVar + " in\n";
        }
        if (param.pattern)
            lets += bindPatternLets(*param.pattern, argVar);
        return lets;
    };

    // Single clause across the whole group, AND every param's pattern (if
    // any) is something bindPatternLets can actually turn into plain lets
    // (a bare variable name or a record destructure) — emit without a case
    // dispatch at all, since a plain let-chain is simpler/cheaper here.
    // Anything else (list-cons patterns like `@[x|_]`, constructor
    // patterns, tuple patterns, wildcards, literals — anything
    // bindPatternLets doesn't handle) falls through to the general
    // case-dispatch path below instead, which handles arbitrary patterns
    // correctly via real Core Erlang pattern matching (valid even for a
    // single clause). A real, reproduced bug otherwise: `let head(@[x |
    // _]) = x` — a single-clause function whose only param is a list-cons
    // pattern — silently produced NO binding for `x` at all ("unbound
    // variable 'X'"), since bindPatternLets only knows how to destructure
    // VarPattern/RecordPattern (spec/type_dispatch.kex).
    auto patternIsSimple = [](const ast::Pattern* pat) -> bool {
        if (!pat) return true;
        return std::holds_alternative<ast::VarPattern>(pat->kind) ||
               std::holds_alternative<ast::RecordPattern>(pat->kind) ||
               std::holds_alternative<ast::RangePattern>(pat->kind);
    };
    bool allParamsSimple = true;
    for (const auto& param : first.clauses[0].params) {
        if (!patternIsSimple(param.pattern ? param.pattern->get() : nullptr)) {
            allParamsSimple = false;
            break;
        }
    }
    if (totalClauses == 1 && allParamsSimple) {
        const auto& clause = first.clauses[0];
        std::string paramLets;
        // If `this` is implicit, _Arg0 = this; explicit params start at _Arg1.
        if (hasImplicitThis)
            paramLets += "let <This> = _Arg0 in\n";
        int argOffset = hasImplicitThis ? 1 : 0;
        for (size_t i = 0; i < clause.params.size(); i++)
            paramLets += bindParamLets(clause.params[i], argVars[i + argOffset]);
        std::string bodyStr = emitBody(clause.body);
        if (anyReturn) bodyStr = wrapReturnCatch(bodyStr);
        std::ostringstream out;
        out << "'" << emitName << "'/" << arity << " =\n";
        out << "  fun " << funHead << " ->\n";
        out << "    " << paramLets << bodyStr << "\n";
        restoreRet();
        return out.str();
    }

    // Multi-clause: Core Erlang case dispatch.
    // Single-arg: case _Arg0 of Pat when 'true' -> body
    // Multi-arg:  case <_Arg0, _Arg1> of <P0, P1> when 'true' -> body
    // For implicit-this functions the case dispatches on explicit params only (_Arg1+)
    int argOffset = hasImplicitThis ? 1 : 0;
    int caseArity = arity - argOffset;
    bool multiArg = caseArity > 1;

    // Build the case dispatch into its own buffer so it can be wrapped in
    // the kex_return try/catch (when anyReturn) without the fun header.
    std::ostringstream out;
    if (multiArg) {
        out << "case <";
        for (int i = argOffset; i < arity; i++) {
            if (i > argOffset) out << ", ";
            out << argVars[i];
        }
        out << "> of\n";
    } else if (caseArity == 1) {
        out << "case " << argVars[argOffset] << " of\n";
    } else {
        // Zero explicit args: shouldn't have multi-clause but handle gracefully
        out << "case 'ok' of\n";
    }

    for (const auto* fn : group) {
        for (const auto& clause : fn->clauses) {
            if (multiArg) {
                out << "      <";
                for (size_t i = 0; i < clause.params.size(); i++) {
                    if (i > 0) out << ", ";
                    auto& p = clause.params[i];
                    if (p.pattern) {
                        auto* vp = std::get_if<ast::VarPattern>(&(*p.pattern)->kind);
                        if (std::holds_alternative<ast::WildcardPattern>((*p.pattern)->kind))
                            out << freshVar("W");
                        else if (vp) out << erlVar(vp->name);
                        else out << emitPattern(*p.pattern);
                    } else if (p.name) {
                        out << erlVar(*p.name);
                    } else {
                        out << freshVar("W");
                    }
                }
                out << "> when 'true' ->\n";
            } else {
                out << "      ";
                if (!clause.params.empty()) {
                    auto& p = clause.params[0];
                    if (p.pattern) {
                        auto* vp = std::get_if<ast::VarPattern>(&(*p.pattern)->kind);
                        if (std::holds_alternative<ast::WildcardPattern>((*p.pattern)->kind))
                            out << "_";
                        else if (vp) out << erlVar(vp->name);
                        else out << emitPattern(*p.pattern);
                    } else if (p.name) {
                        out << erlVar(*p.name);
                    } else {
                        out << "_";
                    }
                } else {
                    out << "_";
                }
                out << " when 'true' ->\n";
            }

            // Bind explicit params (offset by 1 if implicit this)
            std::string paramLets;
            for (size_t i = 0; i < clause.params.size(); i++)
                paramLets += bindParamLets(clause.params[i], argVars[i + argOffset]);
            out << "        " << paramLets << emitBody(clause.body) << "\n";
        }
    }
    // A bare `_` fallback pattern only matches a single-value scrutinee —
    // against a multi-value `<E1,...,En>` case (multiArg), Core Erlang
    // requires a same-arity value-list pattern too, each element a
    // DISTINCT fresh variable (`_` repeated is a duplicate-variable
    // error, not a wildcard, once inside a value-list pattern). A real,
    // reproduced bug otherwise: erlc rejected the whole function with
    // "pattern count mismatch" for any multi-arg multi-clause function at
    // all (spec/comparision.kex's `to(@Tag, Integer)` overload family).
    if (multiArg) {
        out << "      <";
        for (int i = 0; i < caseArity; i++) {
            if (i > 0) out << ", ";
            out << freshVar("W");
        }
        out << "> when 'true' -> call 'erlang':'error'('function_clause')\n";
    } else {
        out << "      _ when 'true' -> call 'erlang':'error'('function_clause')\n";
    }
    out << "    end";

    std::string caseStr = out.str();
    if (anyReturn) caseStr = wrapReturnCatch(caseStr);
    std::ostringstream fnOut;
    fnOut << "'" << emitName << "'/" << arity << " =\n";
    fnOut << "  fun " << funHead << " ->\n";
    if (hasImplicitThis)
        fnOut << "    let <This> = _Arg0 in\n";
    fnOut << "    " << caseStr << "\n";
    restoreRet();
    return fnOut.str();
}

// ---------------------------------------------------------------------------
// Main block
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitMainBlock(const ast::MainBlock& main) -> std::string {
    // main/0 (no args) or main/1 (args list)
    int arity = main.params.empty() ? 0 : 1;
    m_exports.push_back({"main", arity});

    // Print the describe/it pass/fail summary after main runs, exactly
    // matching Evaluator::execute's own post-run summary — but only if
    // any `it` actually ran (kex_test:maybe_print_summary/0 checks the
    // counters itself), so ordinary non-test programs see no extra
    // output (spec/optional_parens_do.kex, spec/json_parser.spec.kex).
    // An early `return` in main throws and is caught here (so the test
    // summary below still runs afterward, matching the tree-walker, which
    // catches main's ReturnException at the function boundary too).
    bool savedReturnThrows = m_returnThrows;
    m_returnThrows = bodyHasReturn(main.body);
    bool wrapMain = m_returnThrows;
    auto emitMainBody = [&]() {
        std::string b = emitBody(main.body);
        return wrapMain ? wrapReturnCatch(b) : b;
    };

    auto withTestSummary = [this](const std::string& body) {
        auto v = freshVar("MainResult");
        auto discard = freshVar("Summary");
        return "let <" + v + "> =\n    " + body + "\nin\n"
               "let <" + discard + "> =\n    call 'kex_test':'maybe_print_summary'()\nin\n" + v;
    };

    std::ostringstream out;
    out << "'main'/" << arity << " =\n";
    if (arity == 0) {
        out << "  fun () ->\n";
        out << "    " << withTestSummary(emitMainBody()) << "\n";
    } else {
        // main(args) — bind the args list from _Args
        std::string paramName = "Args";
        if (!main.params.empty() && main.params[0].name)
            paramName = erlVar(*main.params[0].name);
        out << "  fun (_Args) ->\n";
        out << "    let <" << paramName << "> = _Args in\n";
        // main(args, env) — a second param binds the ENV snapshot
        // (Map<String,String>), matching Evaluator::execMainBlock
        // exactly. There's no real second argument from erl's own entry
        // point (init:get_plain_arguments/0 only ever gives the one args
        // list) — env is synthesized here the same way bare `ENV`
        // references are elsewhere (emitExpr's UpperIdentifier case). A
        // real, reproduced bug otherwise: `main(args, env) do ... end`
        // only ever bound `args`, leaving `env` a genuinely unbound
        // variable the moment the body referenced it (spec/env_param.kex).
        if (main.params.size() >= 2 && main.params[1].name) {
            out << "    let <" << erlVar(*main.params[1].name)
                << "> = call 'kex_io':'env_map'() in\n";
        }
        out << "    " << withTestSummary(emitMainBody()) << "\n";
    }
    m_returnThrows = savedReturnThrows;
    return out.str();
}

// ---------------------------------------------------------------------------
// Program entry point
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitProgram(const ast::Program& prog,
                                     const std::string& fileStem) -> EmitResult {
    m_varCounter = 0;
    m_loopCounter = 0;
    m_exports.clear();
    m_topLevelFns.clear();
    m_staticCtors.clear();
    m_varSubst.clear();
    m_moduleName = "kex_" + fileStem;

    // Reset field accessor map for this compilation unit.
    m_fieldAccessors.clear();
    m_records.clear();
    auto& fieldAccessors = m_fieldAccessors; // alias for use in lambdas below

    // Extracts a plain record-type name from a `make TARGET do` block's
    // target, e.g. "Vec2" from `make Vec2 do`. Returns nullopt for
    // anything that isn't a bare type name (generic/list targets like
    // `make [A] do` dispatch via pattern-matching on the receiver already
    // — see emitFunctionGroup's ThisPattern/RecordPattern receiver
    // detection — so they never need the cross-type mangling below).
    auto simpleTypeNameOf = [](const ast::TypeExprPtr& target) -> std::optional<std::string> {
        if (!target) return std::nullopt;
        if (auto* tn = std::get_if<ast::TypeName>(&target->kind)) {
            if (tn->parts.empty()) return std::nullopt;
            return tn->parts.back();
        }
        return std::nullopt;
    };

    // Trait declarations, by name — used below to fill in default method
    // bodies (`let passing? = this.score > 0`) for any `make Type,
    // implement: Trait do ... end` block that doesn't override them
    // itself (spec/traits.kex's Bot, which uses BOTH Describable's and
    // Scorable's defaults untouched, and Player, which overrides `shout`
    // but still relies on the default `passing?`).
    std::unordered_map<std::string, const ast::TraitDef*> traitDefs;
    for (const auto& item : prog.items) {
        auto* td = std::get_if<std::unique_ptr<ast::TraitDef>>(&item);
        if (td && *td) traitDefs[(*td)->name] = td->get();
    }

    // Own method names directly defined in a `make Type do ... end` block
    // (NOT counting inherited trait defaults) — used both to know which
    // trait defaults still need to be filled in for that type, and to
    // build methodTypeOwners below.
    auto ownMethodNames = [](const ast::MakeDef& make) {
        std::unordered_set<std::string> names;
        for (const auto& bodyItem : make.body) {
            if (auto* fdp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bodyItem)) {
                if (*fdp) names.insert((*fdp)->name);
            } else if (auto* vbp = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bodyItem)) {
                if (*vbp)
                    for (const auto& vi : (*vbp)->items)
                        if (auto* fp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                            if (*fp) names.insert((*fp)->name);
            }
        }
        return names;
    };

    // Trait default methods a type inherits: every default FunctionDef in
    // every trait it implements, EXCEPT names the type's own make-block
    // already defines (an explicit override always wins). Keyed by type
    // name, then method name — the FunctionDef* is shared (read-only)
    // across every inheriting type, since the default body itself never
    // needs to differ per type; only its emitted name/dispatch does (see
    // the mangling/dispatcher machinery below, which treats an inherited
    // default exactly like a directly-defined method).
    std::unordered_map<std::string, std::unordered_map<std::string, const ast::FunctionDef*>>
        inheritedDefaults;
    for (const auto& item : prog.items) {
        auto* makeDef = std::get_if<std::unique_ptr<ast::MakeDef>>(&item);
        if (!makeDef || !*makeDef || (*makeDef)->implements.empty()) continue;
        auto typeName = simpleTypeNameOf((*makeDef)->target);
        if (!typeName) continue;
        auto own = ownMethodNames(**makeDef);
        for (const auto& traitName : (*makeDef)->implements) {
            auto it = traitDefs.find(traitName);
            if (it == traitDefs.end()) continue;
            for (const auto& traitItem : it->second->body) {
                auto* fdp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&traitItem);
                if (!fdp || !*fdp) continue;
                const auto& name = (*fdp)->name;
                if (own.count(name)) continue; // overridden — skip default
                inheritedDefaults[*typeName].emplace(name, fdp->get());
            }
        }
    }

    // Pre-pass: which method names are defined by more than one type's
    // `make Type do ... end` block (counting both directly-defined AND
    // inherited-trait-default methods)? Erlang has one flat function-name
    // space per module — two different types both defining e.g. `add`
    // (spec/type_dispatch.kex's Vec2/Vec3, or operator overloading's `+`)
    // can't both emit a plain `add/2` without erlc rejecting the
    // duplicate. `methodTypeOwners` also records EACH type per colliding
    // name, in declaration order, for the dispatcher emitted later.
    std::unordered_map<std::string, std::vector<std::string>> methodTypeOwners;
    for (const auto& item : prog.items) {
        auto* makeDef = std::get_if<std::unique_ptr<ast::MakeDef>>(&item);
        if (!makeDef || !*makeDef) continue;
        auto typeName = simpleTypeNameOf((*makeDef)->target);
        if (!typeName) continue;
        auto collectName = [&](const std::string& name) {
            auto& owners = methodTypeOwners[name];
            if (std::find(owners.begin(), owners.end(), *typeName) == owners.end())
                owners.push_back(*typeName);
        };
        for (const auto& bodyItem : (*makeDef)->body) {
            if (auto* fdp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bodyItem)) {
                if (*fdp) collectName((*fdp)->name);
            } else if (auto* vbp = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bodyItem)) {
                if (*vbp)
                    for (const auto& vi : (*vbp)->items)
                        if (auto* fp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                            if (*fp) collectName((*fp)->name);
            }
        }
        auto defIt = inheritedDefaults.find(*typeName);
        if (defIt != inheritedDefaults.end())
            for (const auto& [name, fd] : defIt->second) collectName(name);
    }
    std::unordered_set<std::string> collidingMethodNames;
    for (const auto& [name, owners] : methodTypeOwners)
        if (owners.size() > 1) collidingMethodNames.insert(name);

    // Recursively collect FunctionDefs from a ModuleDef's body (and any
    // nested ModuleDefs). Kex modules within a single source file are
    // namespaces for organization; they flatten into the same BEAM module
    // as their host file so calls like `Factorial.compute(n)` resolve to
    // a local apply rather than a cross-module call.
    std::function<void(const ast::ModuleDef&)> collectModuleFns =
        [&](const ast::ModuleDef& mod) {
        for (const auto& bodyItem : mod.body) {
            std::visit([&](const auto& n) {
                using DT = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<DT, std::unique_ptr<ast::FunctionDef>>) {
                    if (n) {
                        int arity = n->clauses.empty() ? 0
                                  : static_cast<int>(n->clauses[0].params.size());
                        m_topLevelFns[n->name] = arity;
                    }
                } else if constexpr (std::is_same_v<DT, std::unique_ptr<ast::ModuleDef>>) {
                    if (n) collectModuleFns(*n);
                }
            }, bodyItem);
        }
    };

    // First pass: collect all top-level function names so emitExpr can
    // distinguish static local calls from calls-through-variables.
    for (const auto& item : prog.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) {
                    int arity = node->clauses.empty() ? 0
                              : static_cast<int>(node->clauses[0].params.size());
                    m_topLevelFns[node->name] = arity;
                    if (!node->clauses.empty()) {
                        std::vector<std::string> names;
                        for (const auto& p : node->clauses[0].params)
                            names.push_back(p.name ? *p.name : std::string{});
                        m_topLevelParams[node->name] = std::move(names);
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectModuleFns(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                // Synthetic blocks hold top-level `let name = expr` declarations.
                if (node && node->synthetic) {
                    for (const auto& e : node->body) {
                        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
                            if (le->pattern) {
                                if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind))
                                    m_topLevelFns[vp->name] = 0;
                            }
                        }
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (node) {
                    m_records[node->name] = node.get();
                    for (int i = 0; i < (int)node->fields.size(); ++i) {
                        // tuple position: 1 = tag, 2..N+1 = fields
                        fieldAccessors[node->fields[i].name].push_back({node->name, i + 2});
                    }
                    // Mark each field name as a known 1-arity function.
                    for (const auto& f : node->fields)
                        m_topLevelFns[f.name] = 1;
                    // Collect static block functions.
                    if (node->staticBlock) {
                        for (const auto& fd : node->staticBlock->functions) {
                            if (!fd) continue;
                            int explArity = fd->clauses.empty() ? 0
                                          : static_cast<int>(fd->clauses[0].params.size());
                            if (explArity > 0) {
                                m_topLevelFns[fd->name] = explArity;
                            } else {
                                // 0-arity constants: emit with mangled name to avoid clashes.
                                std::string mangled = node->name + "_" + fd->name;
                                m_staticCtors[node->name + "::" + fd->name] = mangled;
                                m_topLevelFns[mangled] = 0;
                            }
                        }
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) {
                    // Collect FunctionDefs from body, including inside VisibilityBlocks.
                    std::function<void(const ast::FunctionDef&)> registerFn =
                        [&](const ast::FunctionDef& fd) {
                        int explArity = fd.clauses.empty() ? 0
                                      : static_cast<int>(fd.clauses[0].params.size());
                        // Uppercase-named functions in make blocks are static constructors
                        // with no implicit This parameter.
                        bool isStaticCtor = !fd.name.empty() &&
                                           std::isupper(static_cast<unsigned char>(fd.name[0]));
                        // An unnamed first param carrying ANY pattern (not just
                        // RecordPattern — e.g. `let myCapitalize(@[]) = ...` /
                        // `let myCapitalize(@[x|rest]) = ...` in `make String
                        // do`, a ListPattern receiver) means the receiver
                        // itself is being pattern-matched, same convention as
                        // a record-destructuring receiver. Checking only for
                        // RecordPattern here was a real, reproduced bug:
                        // spec/my_capitalize.kex's make-String methods were
                        // never recognized as having a receiver at all, so
                        // `.myCapitalize` calls used the wrong arity and
                        // fell through to erlc's "undefined function" error.
                        bool firstIsReceiver = false;
                        if (!isStaticCtor && !fd.clauses.empty() && !fd.clauses[0].params.empty()) {
                            const auto& p0 = fd.clauses[0].params[0];
                            // The @ sigil (ThisPattern, any inner kind — see
                            // Parser::parsePattern) is the real, precise
                            // marker for "this param IS the receiver, being
                            // pattern-matched" — not "any unnamed param with
                            // a pattern at all". That broader check was a
                            // real, reproduced regression: a bare type-name
                            // parameter like `to(String)` (no `@`, used for
                            // .to(Type) overload dispatch — a
                            // ConstructorPattern, not a receiver) was
                            // wrongly swallowed as "the receiver", leaving
                            // the real implicit `this` completely unbound
                            // (spec/type_dispatch.kex's `to/1`).
                            // RangePattern (`x.._`/`_..y`, no `@` needed —
                            // see Parser::parseParam's dedicated `name..`
                            // lookahead) is likewise always the receiver
                            // being destructured, never a plain 2nd
                            // parameter — spec/range.kex's `make Range do
                            // let first(x.._) = x end`.
                            if (!p0.name && p0.pattern &&
                                (std::holds_alternative<ast::ThisPattern>((*p0.pattern)->kind) ||
                                 std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind) ||
                                 std::holds_alternative<ast::RangePattern>((*p0.pattern)->kind)))
                                firstIsReceiver = true;
                        }
                        m_topLevelFns[fd.name] = explArity + (isStaticCtor || firstIsReceiver ? 0 : 1);
                    };
                    for (const auto& bodyItem : node->body) {
                        if (auto* fdp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bodyItem)) {
                            if (*fdp) registerFn(**fdp);
                        } else if (auto* vbp = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bodyItem)) {
                            if (*vbp) {
                                for (const auto& vi : (*vbp)->items)
                                    if (auto* fp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                        if (*fp) registerFn(**fp);
                            }
                        }
                    }
                    // Also register inherited trait-default methods, so
                    // m_topLevelFns knows their arity too (see
                    // inheritedDefaults above).
                    if (auto typeName = simpleTypeNameOf(node->target)) {
                        auto defIt = inheritedDefaults.find(*typeName);
                        if (defIt != inheritedDefaults.end())
                            for (const auto& [name, fd] : defIt->second) registerFn(*fd);
                    }
                }
            }
        }, item);
    }

    // Second pass: group consecutive FunctionDefs with the same name (multi-clause
    // functions written as separate `let` declarations), then emit each group once.
    // Each group is a list of pointers to the original FunctionDef nodes.
    std::vector<std::string> functionTexts;

    using FnGroup  = std::vector<const ast::FunctionDef*>;
    // emitNameOverride: non-empty when this group's method name collides
    // with another type's make-block defining the SAME method (e.g. both
    // `Vec2` and `Vec3` defining `add`) — see the `collidingMethodNames`
    // pre-pass below. Erlang has one flat function-name space per module,
    // so two same-name/arity functions from different make-blocks can't
    // coexist; each contributing type's version is instead emitted under a
    // mangled name (`add__Vec2`), and a small dispatcher function under
    // the original bare name (`add`) picks the right one at runtime via
    // the receiver's own type tag (see the dispatcher-emission loop below).
    struct OrderedItem { bool isMain; bool hasImplicitThis; FnGroup fns; const ast::MainBlock* mb; std::string emitNameOverride{}; };
    std::vector<OrderedItem> ordered;
    // Bare top-level expressions (non-synthetic, non-explicit-main MainBlocks)
    // are accumulated here and emitted as a synthetic main/0 if there's no
    // explicit main block.
    std::vector<const ast::MainBlock*> bareExprs;

    for (const auto& item : prog.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (!node) return;
                // Merge into last group if same name
                if (!ordered.empty() && !ordered.back().isMain
                    && ordered.back().fns[0]->name == node->name) {
                    ordered.back().fns.push_back(node.get());
                } else {
                    ordered.push_back({false, false, {node.get()}, nullptr});
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                // Flatten nested module functions into the current BEAM module.
                // Each FunctionDef becomes a top-level function; recursive
                // descent handles nested modules.
                if (!node) return;
                std::function<void(const ast::ModuleDef&)> pushModuleFns =
                    [&](const ast::ModuleDef& mod) {
                    for (const auto& bodyItem : mod.body) {
                        std::visit([&](const auto& n) {
                            using DT = std::decay_t<decltype(n)>;
                            if constexpr (std::is_same_v<DT, std::unique_ptr<ast::FunctionDef>>) {
                                if (!n) return;
                                if (!ordered.empty() && !ordered.back().isMain
                                    && ordered.back().fns[0]->name == n->name) {
                                    ordered.back().fns.push_back(n.get());
                                } else {
                                    ordered.push_back({false, false, {n.get()}, nullptr});
                                }
                            } else if constexpr (std::is_same_v<DT, std::unique_ptr<ast::ModuleDef>>) {
                                if (n) pushModuleFns(*n);
                            }
                        }, bodyItem);
                    }
                };
                pushModuleFns(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                // make T do ... end — emit each FunctionDef as a top-level function.
                if (!node) return;
                auto typeName = simpleTypeNameOf(node->target);
                // Helper to push one FunctionDef (from either direct body or VisibilityBlock).
                auto pushMakeFn = [&](const ast::FunctionDef* fd) {
                    if (!fd) return;
                    // Uppercase-named functions are static constructors — no implicit This.
                    bool isStaticCtor = !fd->name.empty() &&
                                       std::isupper(static_cast<unsigned char>(fd->name[0]));
                    // See the matching check above (in the first collection
                    // pass) for why this accepts any pattern kind, not just
                    // RecordPattern.
                    // See the matching check above (first collection pass)
                    // for why this checks ThisPattern/RecordPattern
                    // specifically, not any pattern at all.
                    bool firstParamIsReceiver = false;
                    if (!isStaticCtor && !fd->clauses.empty() && !fd->clauses[0].params.empty()) {
                        const auto& p0 = fd->clauses[0].params[0];
                        if (!p0.name && p0.pattern &&
                            (std::holds_alternative<ast::ThisPattern>((*p0.pattern)->kind) ||
                             std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind) ||
                             std::holds_alternative<ast::RangePattern>((*p0.pattern)->kind)))
                            firstParamIsReceiver = true;
                    }
                    bool needsImplicitThis = !isStaticCtor && !firstParamIsReceiver;
                    // Colliding method name (another type also defines it) —
                    // emit under a mangled `name__Type` instead of the bare
                    // name (see collidingMethodNames/methodTypeOwners above
                    // and the dispatcher-emission loop below). Uses the
                    // mangled name for the merge-with-previous-group check
                    // too, since two different types' same-named methods
                    // must NOT be merged into one (mangled) group together.
                    std::string effectiveName = fd->name;
                    if (typeName && collidingMethodNames.count(fd->name))
                        effectiveName = fd->name + "__" + *typeName;
                    if (!ordered.empty() && !ordered.back().isMain
                        && ordered.back().fns[0]->name == fd->name
                        && ordered.back().emitNameOverride == (effectiveName == fd->name ? "" : effectiveName)) {
                        ordered.back().fns.push_back(fd);
                    } else {
                        ordered.push_back({false, needsImplicitThis, {fd}, nullptr,
                                           effectiveName == fd->name ? "" : effectiveName});
                    }
                };
                for (const auto& bodyItem : node->body) {
                    if (auto* fdPtr = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bodyItem)) {
                        pushMakeFn(fdPtr->get());
                    } else if (auto* vbPtr = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bodyItem)) {
                        if (*vbPtr) {
                            for (const auto& vi : (*vbPtr)->items)
                                if (auto* fp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                    pushMakeFn(fp->get());
                        }
                    }
                }
                // Fill in any inherited-but-not-overridden trait default
                // methods (see inheritedDefaults above) exactly like any
                // other make-block method — pushMakeFn's mangling/
                // collision handling applies identically whether fd came
                // from this block's own body or a trait default.
                if (typeName) {
                    auto defIt = inheritedDefaults.find(*typeName);
                    if (defIt != inheritedDefaults.end())
                        for (const auto& [name, fd] : defIt->second) pushMakeFn(fd);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                // Emit non-trivial functions (arity >= 1) from the static do...end block.
                if (node && node->staticBlock) {
                    for (const auto& fd : node->staticBlock->functions) {
                        if (!fd) continue;
                        int explArity = fd->clauses.empty() ? 0
                                      : static_cast<int>(fd->clauses[0].params.size());
                        if (explArity == 0) {
                            // Emit 0-arity constants as mangled top-level functions.
                            std::string mangled = node->name + "_" + fd->name;
                            m_exports.push_back({mangled, 0});
                            std::ostringstream fnOut;
                            fnOut << "'" << mangled << "'/0 =\n";
                            fnOut << "  fun () ->\n";
                            if (!fd->clauses.empty()) {
                                fnOut << "    " << emitBody(fd->clauses[0].body) << "\n";
                            } else {
                                fnOut << "    'ok'\n";
                            }
                            functionTexts.push_back(fnOut.str());
                            continue;
                        }
                        // Static block functions are plain functions, no implicit This.
                        if (!ordered.empty() && !ordered.back().isMain
                            && ordered.back().fns[0]->name == fd->name) {
                            ordered.back().fns.push_back(fd.get());
                        } else {
                            ordered.push_back({false, false, {fd.get()}, nullptr});
                        }
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (node) {
                    if (node->synthetic) {
                        // Each `let name = expr` in a synthetic block becomes a
                        // named 0-arity function, not part of main.
                        for (const auto& e : node->body) {
                            if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
                                if (le->pattern) {
                                    if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                                        // Store as a special "global let" item
                                        // We'll handle it below as a 0-arity function.
                                        struct SyntheticLet {
                                            std::string name;
                                            const ast::ExprPtr* value;
                                        };
                                        // Can't easily store mixed types — emit immediately
                                        m_exports.push_back({vp->name, 0});
                                        std::ostringstream fnOut;
                                        fnOut << "'" << vp->name << "'/0 =\n";
                                        fnOut << "  fun () ->\n";
                                        fnOut << "    " << emitExpr(le->value) << "\n";
                                        functionTexts.push_back(fnOut.str());
                                    }
                                }
                            }
                        }
                    } else if (node->isExplicitMain) {
                        ordered.push_back({true, false, {}, node.get()});
                    } else {
                        // Bare top-level expression — stash for synthetic main
                        bareExprs.push_back(node.get());
                    }
                }
            }
        }, item);
    }

    for (const auto& oi : ordered) {
        if (oi.isMain) {
            functionTexts.push_back(emitMainBlock(*oi.mb));
        } else {
            // Emit merged: first node's metadata + all clauses from every node
            const auto& first = *oi.fns[0];
            // Build a temporary FunctionDef that holds references to all clauses
            // via a local re-emit that processes each group's nodes.
            // We can't copy FunctionDef (unique_ptrs), so instead delegate to a
            // helper that accepts the group directly.
            functionTexts.push_back(emitFunctionGroup(oi.fns, oi.hasImplicitThis, oi.emitNameOverride));
        }
    }

    // Cross-type method-name-collision dispatcher: for each colliding
    // method name (e.g. `add`, defined by both Vec2's and Vec3's make
    // blocks), emit one small function under the bare name that inspects
    // the receiver's own tuple tag (element 1) and dispatches to the
    // right type-mangled variant (`add__Vec2`, `add__Vec3`). Every
    // colliding name is guaranteed to have gone through emitFunctionGroup
    // with a non-empty emitNameOverride above, so `name__Type` always
    // exists with the arity recorded in m_topLevelFns.
    for (const auto& name : collidingMethodNames) {
        auto arityIt = m_topLevelFns.find(name);
        if (arityIt == m_topLevelFns.end()) continue;
        int arity = arityIt->second;
        if (arity < 1) continue; // needs a receiver to dispatch on

        std::vector<std::string> argVars;
        for (int i = 0; i < arity; i++) argVars.push_back("_Arg" + std::to_string(i));

        m_exports.push_back({name, arity});
        std::ostringstream out;
        out << "'" << name << "'/" << arity << " =\n";
        out << "  fun (";
        for (size_t i = 0; i < argVars.size(); i++) {
            if (i > 0) out << ", ";
            out << argVars[i];
        }
        out << ") ->\n";
        out << "    case call 'erlang':'element'(1, _Arg0) of\n";
        for (const auto& typeName : methodTypeOwners[name]) {
            out << "      '" << typeName << "' when 'true' ->\n";
            out << "        apply '" << name << "__" << typeName << "'/" << arity << "(";
            for (size_t i = 0; i < argVars.size(); i++) {
                if (i > 0) out << ", ";
                out << argVars[i];
            }
            out << ")\n";
        }
        out << "      _ when 'true' -> call 'erlang':'error'('function_clause')\n";
        out << "    end\n";
        functionTexts.push_back(out.str());
    }

    // Emit field accessor functions for each unique record field name.
    // For field "x" present in Vector2D (pos 2) and Vector3D (pos 2), emit:
    //   'x'/1 = fun (_Arg0) -> call 'erlang':'element'(2, _Arg0)
    // If the same field appears at different positions in different records,
    // emit a case on the tuple tag.
    for (const auto& [fieldName, entries] : fieldAccessors) {
        // Skip if a user-defined function with this name already exists
        // (make block methods shadow auto-accessors).
        bool userDefined = false;
        for (const auto& oi : ordered) {
            if (!oi.isMain && !oi.fns.empty() && oi.fns[0]->name == fieldName) {
                userDefined = true; break;
            }
        }
        if (userDefined) continue;

        m_exports.push_back({fieldName, 1});
        std::ostringstream acc;
        acc << "'" << fieldName << "'/1 =\n";
        acc << "  fun (_Arg0) ->\n";
        if (entries.size() == 1 || std::all_of(entries.begin(), entries.end(),
            [&](const auto& e){ return e.second == entries[0].second; })) {
            // All records have this field at the same position
            acc << "    call 'erlang':'element'(" << entries[0].second << ", _Arg0)\n";
        } else {
            // Different positions — dispatch on tag
            acc << "    case call 'erlang':'element'(1, _Arg0) of\n";
            for (const auto& [recName, pos] : entries) {
                acc << "      '" << recName << "' when 'true' ->\n";
                acc << "        call 'erlang':'element'(" << pos << ", _Arg0)\n";
            }
            acc << "    end\n";
        }
        functionTexts.push_back(acc.str());
    }

    // If there are bare top-level expressions and no explicit main was emitted,
    // bundle them into a single main/0.
    bool hasExplicitMain = std::any_of(ordered.begin(), ordered.end(),
                                        [](const auto& oi){ return oi.isMain; });
    if (!bareExprs.empty() && !hasExplicitMain) {
        m_exports.push_back({"main", 0});
        std::ostringstream mOut;
        mOut << "'main'/0 =\n  fun () ->\n";
        // Collect all body exprs from all bare blocks, emit as one body
        std::vector<ast::ExprPtr> allExprs; // can't move — emit inline
        // Build a chained body using emitBody logic
        std::vector<std::string> stmts;
        for (const auto* mb : bareExprs)
            for (const auto& e : mb->body)
                stmts.push_back(emitExpr(e));
        // Right-fold: last is result, others bound to throwaway vars
        std::string body = stmts.empty() ? "'ok'" : stmts.back();
        for (int i = static_cast<int>(stmts.size()) - 2; i >= 0; --i) {
            auto tmp = freshVar("S");
            body = "let <" + tmp + "> =\n    " + stmts[i] + "\nin\n" + body;
        }
        // Same describe/it pass/fail summary footer as emitMainBlock —
        // see its comment for why (spec/testing_dsl.spec.kex's bare
        // top-level describe/it calls, with no explicit `main do` block
        // at all, go through this synthetic-main path instead).
        auto resultVar = freshVar("MainResult");
        auto discardVar = freshVar("Summary");
        body = "let <" + resultVar + "> =\n    " + body + "\nin\n"
               "let <" + discardVar + "> =\n    call 'kex_test':'maybe_print_summary'()\nin\n" + resultVar;
        mOut << "    " << body << "\n";
        functionTexts.push_back(mOut.str());
    }

    // erlc +from_core does NOT auto-generate module_info, so we emit it explicitly.
    m_exports.push_back({"module_info", 0});
    m_exports.push_back({"module_info", 1});

    // Build export list
    std::string exportList;
    for (size_t i = 0; i < m_exports.size(); i++) {
        if (i > 0) exportList += ", ";
        exportList += "'" + m_exports[i].name + "'/" + std::to_string(m_exports[i].arity);
    }

    // Assemble the module
    std::ostringstream src;
    src << "module '" << m_moduleName << "' [" << exportList << "]\n";
    src << "  attributes []\n\n";
    for (const auto& ft : functionTexts) {
        src << ft << "\n";
    }
    // module_info stubs
    src << "'module_info'/0 =\n"
        << "  fun () ->\n"
        << "    call 'erlang':'get_module_info'\n"
        << "         ('" << m_moduleName << "')\n";
    src << "'module_info'/1 =\n"
        << "  fun (_MIKey) ->\n"
        << "    call 'erlang':'get_module_info'\n"
        << "         ('" << m_moduleName << "', _MIKey)\n";
    src << "end\n";

    // Determine main arity from exports
    int mainArity = 0;
    for (const auto& ex : m_exports)
        if (ex.name == "main") { mainArity = ex.arity; break; }

    return EmitResult{src.str(), m_moduleName, mainArity};
}

} // namespace kex::codegen
