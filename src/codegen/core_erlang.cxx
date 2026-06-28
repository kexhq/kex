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
    // Needs quoting if it starts with uppercase, contains special chars, or
    // is an Erlang reserved word.
    bool needsQuote = !std::islower(static_cast<unsigned char>(s[0])) && s[0] != '_';
    if (!needsQuote) {
        for (char c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '@') {
                needsQuote = true;
                break;
            }
        }
    }
    static const std::vector<std::string> reserved = {
        "after","begin","case","catch","cond","end","fun","if","let",
        "of","query","receive","try","when","module","attributes",
        "apply","call","do","in","letrec","primop"
    };
    if (!needsQuote)
        needsQuote = std::find(reserved.begin(), reserved.end(), s) != reserved.end();
    if (needsQuote) return "'" + s + "'";
    return s;
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
        auto close = raw.find('}', dollar + 2);
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
        {"IO::exit",        {"erlang", "halt"}},
        // Math
        {"Math::sqrt",      {"math",   "sqrt"}},
        {"Math::sin",       {"math",   "sin"}},
        {"Math::cos",       {"math",   "cos"}},
        {"Math::tan",       {"math",   "tan"}},
        {"Math::log",       {"math",   "log"}},
        {"Math::log2",      {"math",   "log2"}},
        {"Math::exp",       {"math",   "exp"}},
        {"Math::floor",     {"erlang", "floor"}},
        {"Math::ceil",      {"erlang", "ceil"}},
        {"Math::round",     {"erlang", "round"}},
        {"Math::abs",       {"erlang", "abs"}},
        {"Math::pow",       {"math",   "pow"}},
        {"Math::pi",        {"math",   "pi"}},
        // String
        {"String::length",  {"erlang", "length"}},
        {"String::toUpper", {"string", "to_upper"}},
        {"String::toLower", {"string", "to_lower"}},
        {"String::trim",    {"string", "trim"}},
        {"String::split",   {"string", "split"}},
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
        } else if constexpr (std::is_same_v<T, ast::VarPattern>) {
            return erlVar(p.name);
        } else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
            switch (p.literal.type) {
                case TokenType::Integer:     return p.literal.value;
                case TokenType::Float:       return p.literal.value;
                case TokenType::String:      return erlString(p.literal.value);
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
            // Top-level 0-arity function used as a value — call it.
            auto it = m_topLevelFns.find(node.name);
            if (it != m_topLevelFns.end() && it->second == 0)
                return "apply '" + node.name + "'/0()";
            return erlVar(node.name);
        } else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
            // Module reference used standalone — emit as atom
            return erlAtom(node.name);
        } else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
            return "This";
        }

        // --- Binary ops ---
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
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
                    case TokenType::AmpAmp:      return "and";
                    case TokenType::PipePipe:    return "or";
                    default:                     return "+";
                }
            }();
            // Special case: string concatenation (charlists use ++)
            if (node.op == TokenType::Plus) {
                // We can't know statically if it's a string; emit as ++
                // for now and let the runtime handle it (kex_runtime will
                // dispatch). For arithmetic, the caller should annotate.
                // For M1, use erlang:'+' and rely on type info later.
                return "call 'erlang':'" + op + "'(" + l + ", " + r + ")";
            }
            return "call 'erlang':'" + op + "'(" + l + ", " + r + ")";
        }

        // --- Unary ops ---
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            auto operand = emitExpr(node.operand);
            if (node.op == TokenType::Minus)
                return "call 'erlang':'negate'(" + operand + ")";
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
            // Early return not yet desugared; emit value and note the gap.
            return emitExpr(node.value);
        }

        // --- Function call (free function) ---
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            std::string args;
            for (size_t i = 0; i < node.args.size(); i++) {
                if (i > 0) args += ", ";
                args += emitExpr(node.args[i]);
            }
            // Check if it's a namespaced call like IO.printLine
            auto dot = node.name.find('.');
            if (dot != std::string::npos) {
                auto mod = node.name.substr(0, dot);
                auto fn  = node.name.substr(dot + 1);
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
            // Known top-level function
            auto topIt = m_topLevelFns.find(node.name);
            if (topIt != m_topLevelFns.end()) {
                int callArity = static_cast<int>(node.args.size());
                int defArity  = topIt->second;
                if (defArity == callArity) {
                    // Exact arity match → static dispatch
                    return "apply '" + node.name + "'/" + std::to_string(callArity) + "(" + args + ")";
                }
                // Arity mismatch: the top-level value is a 0-arity closure; call it
                // first, then apply the resulting fun to the arguments.
                // e.g., hello("Alice") where hello/0 returns a lambda:
                //   let <_HFn> = apply 'hello'/0() in apply _HFn("Alice")
                auto tmpFn = freshVar(node.name.substr(0,1));
                return "let <" + tmpFn + "> = apply '" + node.name + "'/" +
                       std::to_string(defArity) + "() in\napply " + tmpFn + "(" + args + ")";
            }
            // Unknown lowercase name: treat as a variable holding a fun (closure/param)
            // → dynamic apply through the variable
            return "apply " + erlVar(node.name) + "(" + args + ")";
        }

        // --- Method call (UFCS) ---
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            // Check if receiver is a module name (UpperIdentifier)
            if (auto* uid = std::get_if<ast::UpperIdentifier>(&node.receiver->kind)) {
                auto [bmod, bfn] = resolveStdlib(uid->name, node.method);
                std::string args;
                for (size_t i = 0; i < node.args.size(); i++) {
                    if (i > 0) args += ", ";
                    args += emitExpr(node.args[i]);
                }
                if (!bmod.empty())
                    return "call '" + bmod + "':'" + bfn + "'(" + args + ")";
                // Unknown module method → call as Kex module
                return "call 'Kex." + uid->name + "':'" + node.method + "'(" + args + ")";
            }
            // UFCS dispatch: map common method names to Erlang BIFs / kex_runtime.
            auto recv = emitExpr(node.receiver);
            auto firstArg = [&]() -> std::string {
                return node.args.empty() ? "" : emitExpr(node.args[0]);
            };
            auto secondArg = [&]() -> std::string {
                return node.args.size() > 1 ? emitExpr(node.args[1]) : "";
            };

            // Integer/Float methods
            if (node.method == "modulo" && node.args.size() == 1)
                return "call 'erlang':'rem'(" + recv + ", " + firstArg() + ")";
            if (node.method == "even?")
                return "call 'erlang':'=:='(call 'erlang':'rem'(" + recv + ", 2), 0)";
            if (node.method == "odd?")
                return "call 'erlang':'=/='(call 'erlang':'rem'(" + recv + ", 2), 0)";
            if (node.method == "abs")
                return "call 'erlang':'abs'(" + recv + ")";
            if (node.method == "sqrt")
                return "call 'math':'sqrt'(" + recv + ")";
            if (node.method == "floor")
                return "call 'erlang':'floor'(" + recv + ")";
            if (node.method == "ceil" || node.method == "ceiling")
                return "call 'erlang':'ceil'(" + recv + ")";
            if (node.method == "round")
                return "call 'erlang':'round'(" + recv + ")";
            if (node.method == "toFloat" || node.method == "to_f")
                return "call 'erlang':'float'(" + recv + ")";
            if (node.method == "toInteger" || node.method == "to_i" || node.method == "truncate")
                return "call 'erlang':'trunc'(" + recv + ")";
            if (node.method == "toString" || node.method == "to_s")
                return "call 'kex_io':'to_string'(" + recv + ")";

            // String methods
            if (node.method == "length" || node.method == "count")
                return "call 'erlang':'length'(" + recv + ")";
            if (node.method == "upperCase" || node.method == "upcase")
                return "call 'string':'to_upper'(" + recv + ")";
            if (node.method == "lowerCase" || node.method == "downcase")
                return "call 'string':'to_lower'(" + recv + ")";
            if (node.method == "trim")
                return "call 'string':'trim'(" + recv + ")";
            if (node.method == "split" && node.args.size() == 1)
                return "call 'string':'split'(" + recv + ", " + firstArg() + ", 'all')";
            if (node.method == "contains?" && node.args.size() == 1)
                return "call 'erlang':'=/='("
                       "call 'string':'find'(" + recv + ", " + firstArg() + "), 'nomatch')";

            // List methods
            if (node.method == "push" && node.args.size() == 1)
                return "call 'erlang':'++'(" + recv + ", [" + firstArg() + "])";
            if (node.method == "count" && node.args.empty())
                return "call 'erlang':'length'(" + recv + ")";
            if (node.method == "first")
                return "call 'erlang':'hd'(" + recv + ")";
            if (node.method == "last")
                return "call 'lists':'last'(" + recv + ")";
            if (node.method == "reverse")
                return "call 'lists':'reverse'(" + recv + ")";
            if (node.method == "contains?" && node.args.size() == 1)
                return "call 'lists':'member'(" + firstArg() + ", " + recv + ")";
            if (node.method == "join" && node.args.size() == 1)
                return "call 'lists':'join'(" + firstArg() + ", " + recv + ")";
            if (node.method == "flatten")
                return "call 'lists':'flatten'(" + recv + ")";

            // Higher-order list methods: bind the fun to a var first (Core Erlang
            // does not allow inline `fun` expressions as call arguments).
            auto bindFun = [&](const std::string& funExpr) -> std::pair<std::string,std::string> {
                auto fnVar = freshVar("Fn");
                return {fnVar, "let <" + fnVar + "> =\n    " + funExpr + " in\n"};
            };
            auto rawBlock = [&]() -> std::string {
                if (node.block) return emitExpr(*node.block);
                if (!node.args.empty()) return emitExpr(node.args.back());
                return "fun (_) -> 'ok'";
            };
            if (node.method == "each") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                return letExpr + "call 'lists':'foreach'(" + fnVar + ", " + recv + ")";
            }
            if (node.method == "map") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                return letExpr + "call 'lists':'map'(" + fnVar + ", " + recv + ")";
            }
            if (node.method == "filter" || node.method == "select") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                return letExpr + "call 'lists':'filter'(" + fnVar + ", " + recv + ")";
            }
            if (node.method == "reduce" || node.method == "inject") {
                if (node.args.size() >= 1 && node.block) {
                    auto init = emitExpr(node.args[0]);
                    auto [fnVar, letExpr] = bindFun(emitExpr(*node.block));
                    return letExpr + "call 'lists':'foldl'(" + fnVar + ", " + init + ", " + recv + ")";
                }
            }

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

            std::string scrutinee;
            if (multiValue) {
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

            std::string result = "case " + scrutinee + " of\n";
            for (const auto& clause : node.clauses) {
                std::string pat;
                if (multiValue && !clause.patterns.empty()) {
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
                if (clause.guard)
                    result += " when " + emitExpr(*clause.guard);
                else
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
            // Tagged tuple {'TypeName', field1, field2, ...}
            // Field order from definition — for M1 just emit in declared order.
            std::string result = "{'" + node.typeName + "'";
            for (const auto& [name, val] : node.fields)
                result += ", " + emitExpr(val);
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

        // Unhandled — emit a placeholder that compiles but fails at runtime.
        else {
            return "call 'erlang':'error'({'kex_unimplemented', " + erlString(typeid(node).name()) + "})";
        }
    }, expr->kind);
}

// ---------------------------------------------------------------------------
// Body: sequence of exprs → nested let chain
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitBody(const std::vector<ast::ExprPtr>& body) -> std::string {
    if (body.empty()) return "'ok'";
    if (body.size() == 1) return emitExpr(body[0]);

    // Right-fold: build from the last expression backwards.
    // Walk forward and collect (binding_var_or_"", expr_text) pairs.
    std::string result = emitExpr(body.back());

    for (int i = static_cast<int>(body.size()) - 2; i >= 0; i--) {
        const auto& e = body[i];
        std::string bindVar;
        std::string bindVal;

        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
            if (le->pattern) {
                if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                    bindVar = erlVar(vp->name);
                    bindVal = emitExpr(le->value);
                    result = "let <" + bindVar + "> =\n    " + bindVal + "\nin\n" + result;
                } else {
                    // Destructuring pattern (tuple, list, constructor): use case
                    auto tmpVar = freshVar("D");
                    auto pat    = emitPattern(le->pattern);
                    auto val    = emitExpr(le->value);
                    result = "let <" + tmpVar + "> =\n    " + val + "\nin\n"
                             "case " + tmpVar + " of\n"
                             "  " + pat + " when 'true' ->\n    " + result + "\nend";
                }
                continue;
            } else {
                bindVar = freshVar("T");
                bindVal = emitExpr(le->value);
            }
        } else if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
            bindVar = erlVar(ve->name);
            bindVal = emitExpr(ve->value);
        } else if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
            bindVar = erlVar(ae->name);
            bindVal = emitExpr(ae->value);
        } else {
            bindVar = freshVar("S");
            bindVal = emitExpr(e);
        }

        result = "let <" + bindVar + "> =\n    " + bindVal + "\nin\n" + result;
    }
    return result;
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

auto CoreErlangEmitter::emitFunctionGroup(const std::vector<const ast::FunctionDef*>& group)
    -> std::string
{
    if (group.empty()) return "";
    const auto& first = *group[0];

    // Count total clauses across all nodes
    int totalClauses = 0;
    for (const auto* fn : group) totalClauses += static_cast<int>(fn->clauses.size());
    if (totalClauses == 0) return "";

    int arity = static_cast<int>(first.clauses.empty() ? 0 : first.clauses[0].params.size());
    m_exports.push_back({first.name, arity});

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

    // Single clause across the whole group: emit without case dispatch.
    if (totalClauses == 1) {
        const auto& clause = first.clauses[0];
        std::string paramLets;
        for (size_t i = 0; i < clause.params.size(); i++) {
            const auto& param = clause.params[i];
            std::string paramName;
            if (param.name)
                paramName = erlVar(*param.name);
            else if (param.pattern)
                if (auto* vp = std::get_if<ast::VarPattern>(&(*param.pattern)->kind))
                    paramName = erlVar(vp->name);
            if (!paramName.empty() && paramName != argVars[i])
                paramLets += "let <" + paramName + "> = " + argVars[i] + " in\n";
        }
        std::ostringstream out;
        out << "'" << first.name << "'/" << arity << " =\n";
        out << "  fun " << funHead << " ->\n";
        out << "    " << paramLets << emitBody(clause.body) << "\n";
        return out.str();
    }

    // Multi-clause: Core Erlang case dispatch.
    // Single-arg: case _Arg0 of Pat when 'true' -> body
    // Multi-arg:  case <_Arg0, _Arg1> of <P0, P1> when 'true' -> body
    bool multiArg = arity > 1;

    std::ostringstream out;
    out << "'" << first.name << "'/" << arity << " =\n";
    out << "  fun " << funHead << " ->\n";

    if (multiArg) {
        out << "    case <";
        for (size_t i = 0; i < argVars.size(); i++) {
            if (i > 0) out << ", ";
            out << argVars[i];
        }
        out << "> of\n";
    } else if (arity == 1) {
        out << "    case " << argVars[0] << " of\n";
    } else {
        // Zero-arg: shouldn't have multi-clause but handle gracefully
        out << "    case 'ok' of\n";
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

            // For named params in the matching clause, bind them from the arg vars
            std::string paramLets;
            for (size_t i = 0; i < clause.params.size() && i < argVars.size(); i++) {
                auto& p = clause.params[i];
                if (p.name) {
                    auto varName = erlVar(*p.name);
                    // Only bind if the pattern wasn't already a variable binding
                    bool alreadyBound = false;
                    if (p.pattern)
                        if (auto* vp = std::get_if<ast::VarPattern>(&(*p.pattern)->kind))
                            alreadyBound = (erlVar(vp->name) == varName);
                    if (!alreadyBound && varName != argVars[i])
                        paramLets += "let <" + varName + "> = " + argVars[i] + " in\n";
                } else if (p.pattern) {
                    if (auto* vp = std::get_if<ast::VarPattern>(&(*p.pattern)->kind)) {
                        auto varName = erlVar(vp->name);
                        if (varName != argVars[i])
                            paramLets += "let <" + varName + "> = " + argVars[i] + " in\n";
                    }
                }
            }
            out << "        " << paramLets << emitBody(clause.body) << "\n";
        }
    }
    out << "      _ when 'true' -> call 'erlang':'error'('function_clause')\n";
    out << "    end\n";
    return out.str();
}

// ---------------------------------------------------------------------------
// Main block
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitMainBlock(const ast::MainBlock& main) -> std::string {
    // main/0 (no args) or main/1 (args list)
    int arity = main.params.empty() ? 0 : 1;
    m_exports.push_back({"main", arity});

    std::ostringstream out;
    out << "'main'/" << arity << " =\n";
    if (arity == 0) {
        out << "  fun () ->\n";
        out << "    " << emitBody(main.body) << "\n";
    } else {
        // main(args) — bind the args list from _Args
        std::string paramName = "Args";
        if (!main.params.empty() && main.params[0].name)
            paramName = erlVar(*main.params[0].name);
        out << "  fun (_Args) ->\n";
        out << "    let <" << paramName << "> = _Args in\n";
        out << "    " << emitBody(main.body) << "\n";
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// Program entry point
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitProgram(const ast::Program& prog,
                                     const std::string& fileStem) -> EmitResult {
    m_varCounter = 0;
    m_exports.clear();
    m_topLevelFns.clear();
    m_moduleName = "kex_" + fileStem;

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
                }
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
            }
        }, item);
    }

    // Second pass: group consecutive FunctionDefs with the same name (multi-clause
    // functions written as separate `let` declarations), then emit each group once.
    // Each group is a list of pointers to the original FunctionDef nodes.
    std::vector<std::string> functionTexts;

    using FnGroup  = std::vector<const ast::FunctionDef*>;
    struct OrderedItem { bool isMain; FnGroup fns; const ast::MainBlock* mb; };
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
                    ordered.push_back({false, {node.get()}, nullptr});
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
                        ordered.push_back({true, {}, node.get()});
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
            functionTexts.push_back(emitFunctionGroup(oi.fns));
        }
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
        mOut << "    " << body << "\n";
        functionTexts.push_back(mOut.str());
    }

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
    src << "end\n";

    return EmitResult{src.str(), m_moduleName};
}

} // namespace kex::codegen
