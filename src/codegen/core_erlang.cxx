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
        {"Math::PI",        {"math",   "pi"}},
        {"Math::e",         {"math",   "exp"}},
        {"Math::inf",       {"erlang", "infinity"}},
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
            // Check loop/SSA substitution first (mutable var current version).
            auto subIt = m_varSubst.find(node.name);
            if (subIt != m_varSubst.end()) return subIt->second;
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
            // + is overloaded: string concat (++) or arithmetic (+).
            // Dispatch at runtime via kex_io:add/2 which checks is_list.
            if (node.op == TokenType::Plus)
                return "call 'kex_io':'add'(" + l + ", " + r + ")";
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
            // `worker { block }` → kex_supervisor:worker/1 with block as 0-arity fun.
            if (node.name == "worker" && node.block) {
                auto fnVar = freshVar("WorkerFn");
                auto fun   = emitExpr(*node.block);
                return "let <" + fnVar + "> =\n    " + fun + " in\n"
                       "call 'kex_supervisor':'worker'(" + fnVar + ")";
            }
            // `worker(Module)` / `worker(Module, args: [...])` MPA sugar.
            // Desugars to: worker { Module.start(args...) }
            // i.e. kex_supervisor:worker/1 with a 0-arity fun that calls kex_MODULE:start/N.
            if (node.name == "worker" && !node.args.empty() && !node.block) {
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
            // `supervisor(strategy: :s) do BLOCK end` as a free function.
            // Creates a nested supervisor child spec.
            if (node.name == "supervisor" && node.block) {
                std::string strat = "'only_crashed'";
                for (const auto& [k, v] : node.namedArgs)
                    if (k == "strategy" || k == "restart") strat = emitExpr(v);
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
                // Task.start { block } → kex_task:start/1 with block as 0-arity fun.
                // The block emits as `fun () -> BODY` already; bind it to a var first
                // (Core Erlang requires fun literals to be let-bound before passing).
                if (uid->name == "Task" && node.method == "start" && node.block) {
                    auto fnVar = freshVar("TaskFn");
                    auto fun   = emitExpr(*node.block);
                    return "let <" + fnVar + "> =\n    " + fun + " in\n"
                           "call 'kex_task':'start'(" + fnVar + ")";
                }
                // Task.await_all([tasks]) → kex_task:await_all/1
                if (uid->name == "Task" && node.method == "await_all" && !node.args.empty())
                    return "call 'kex_task':'await_all'(" + args + ")";
                // Supervisor.start(strategy: :s) do BLOCK end
                // BLOCK must evaluate to a list of child specs (from worker { } calls).
                // The block is parsed as a 0-arity Lambda; inline its body directly.
                if (uid->name == "Supervisor" && node.method == "start" && node.block) {
                    std::string strat = "'only_crashed'";
                    for (const auto& [k, v] : node.namedArgs)
                        if (k == "strategy" || k == "restart") strat = emitExpr(v);
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
            auto secondArg = [&]() -> std::string {
                return node.args.size() > 1 ? emitExpr(node.args[1]) : "";
            };

            // Task methods (UFCS on a task handle).
            // `task.await(timeout: T)` → kex_task:await/2; default timeout = 'infinity'.
            if (node.method == "await") {
                std::string timeout = "'infinity'";
                for (const auto& [k, v] : node.namedArgs)
                    if (k == "timeout") timeout = emitExpr(v);
                if (!node.args.empty()) timeout = firstArg();
                return "call 'kex_task':'await'(" + recv + ", " + timeout + ")";
            }

            // Process / pid methods (UFCS on a pid receiver).
            // `pid.send(m)` → erlang:send(Pid, {'kex_msg', M, self()})
            if (node.method == "send" && node.args.size() == 1)
                return "call 'erlang':'send'(" + recv + ", {'kex_msg', " + firstArg() +
                       ", call 'erlang':'self'()})";
            // `pid.link()` / `pid.unlink()` → erlang:link/1 / erlang:unlink/1
            if (node.method == "link" && node.args.empty())
                return "call 'erlang':'link'(" + recv + ")";
            if (node.method == "unlink" && node.args.empty())
                return "call 'erlang':'unlink'(" + recv + ")";
            // `pid.monitor()` → erlang:monitor/2 (returns a reference)
            if (node.method == "monitor" && node.args.empty())
                return "call 'erlang':'monitor'('process', " + recv + ")";
            // `pid.alive?()` → erlang:is_process_alive/1
            if (node.method == "alive?" && node.args.empty())
                return "call 'erlang':'is_process_alive'(" + recv + ")";
            // `ref.demonitor()` → erlang:demonitor/1
            if (node.method == "demonitor" && node.args.empty())
                return "call 'erlang':'demonitor'(" + recv + ")";
            // `pid.exit(reason)` (instance form) → erlang:exit/2
            if (node.method == "exit" && node.args.size() == 1)
                return "call 'erlang':'exit'(" + recv + ", " + firstArg() + ")";

            // Integer/Float methods
            if (node.method == "modulo" && node.args.size() == 1)
                return "call 'erlang':'rem'(" + recv + ", " + firstArg() + ")";            if (node.method == "even?")
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
            if (node.method == "empty?" && node.args.empty()) {
                auto tmp = freshVar("EV");
                return "let <" + tmp + "> =\n    " + recv + "\nin\n"
                       "case call 'erlang':'is_map'(" + tmp + ") of\n"
                       "  'true' when 'true' -> call 'erlang':'=:='(call 'maps':'size'(" + tmp + "), 0)\n"
                       "  'false' when 'true' -> call 'erlang':'=:='(" + tmp + ", [])\n"
                       "end";
            }
            if ((node.method == "length" || node.method == "count") && !node.block && node.args.empty()) {
                auto tmp = freshVar("SZ");
                return "let <" + tmp + "> =\n    " + recv + "\nin\n"
                       "case call 'erlang':'is_map'(" + tmp + ") of\n"
                       "  'true' when 'true' -> call 'maps':'size'(" + tmp + ")\n"
                       "  'false' when 'true' -> call 'erlang':'length'(" + tmp + ")\n"
                       "end";
            }
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
            if (node.method == "count" && node.args.empty() && !node.block)
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
                return "call 'lists':'flatten'(call 'lists':'join'(" + firstArg() + ", " + recv + "))";
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
            // Returns true if the block/last-arg lambda has 2 parameters (map iteration).
            auto blockArity2 = [&]() -> bool {
                const ast::ExprPtr* blk = node.block ? &*node.block : (!node.args.empty() ? &node.args.back() : nullptr);
                if (!blk) return false;
                if (auto* lam = std::get_if<ast::Lambda>(&(*blk)->kind))
                    return lam->params.size() == 2;
                return false;
            };

            // Map-aware higher-order dispatch: 2-param blocks → map operations.
            if (blockArity2()) {
                if (node.method == "each") {
                    auto [fnVar, letExpr] = bindFun(rawBlock());
                    return letExpr + "call 'maps':'foreach'(" + fnVar + ", " + recv + ")";
                }
                if (node.method == "map") {
                    // produces a list: one result per (k,v) pair
                    auto [fnVar, letExpr] = bindFun(rawBlock());
                    auto kv = freshVar("K"); auto vv = freshVar("V"); auto accv = freshVar("Acc");
                    auto foldF = "fun (" + kv + ", " + vv + ", " + accv + ") ->\n    "
                                 "[apply " + fnVar + "(" + kv + ", " + vv + ")|" + accv + "]";
                    auto [foldVar, foldLet] = bindFun(foldF);
                    return letExpr + foldLet + "call 'lists':'reverse'(call 'maps':'fold'(" + foldVar + ", [], " + recv + "))";
                }
                if (node.method == "filter" || node.method == "select") {
                    auto [fnVar, letExpr] = bindFun(rawBlock());
                    return letExpr + "call 'maps':'filter'(" + fnVar + ", " + recv + ")";
                }
                if (node.method == "reject") {
                    auto [fnVar, letExpr] = bindFun(rawBlock());
                    auto kv = freshVar("K"); auto vv = freshVar("V");
                    auto notFun = "fun (" + kv + ", " + vv + ") ->\n    call 'erlang':'not'(apply " + fnVar + "(" + kv + ", " + vv + "))";
                    auto [notVar, notLet] = bindFun(notFun);
                    return letExpr + notLet + "call 'maps':'filter'(" + notVar + ", " + recv + ")";
                }
                if (node.method == "any?") {
                    auto [fnVar, letExpr] = bindFun(rawBlock());
                    return letExpr + "call 'erlang':'>'("
                           "call 'maps':'size'(call 'maps':'filter'(" + fnVar + ", " + recv + ")), 0)";
                }
                if (node.method == "all?") {
                    auto [fnVar, letExpr] = bindFun(rawBlock());
                    return letExpr + "call 'erlang':'=:='("
                           "call 'maps':'size'(call 'maps':'filter'(" + fnVar + ", " + recv + ")), "
                           "call 'maps':'size'(" + recv + "))";
                }
                if (node.method == "count" && (node.block || !node.args.empty())) {
                    auto [fnVar, letExpr] = bindFun(rawBlock());
                    return letExpr + "call 'maps':'size'(call 'maps':'filter'(" + fnVar + ", " + recv + "))";
                }
                if (node.method == "find") {
                    auto [fnVar, letExpr] = bindFun(rawBlock());
                    auto kv = freshVar("K"); auto vv = freshVar("V"); auto accv = freshVar("Acc");
                    auto foldF = "fun (" + kv + ", " + vv + ", " + accv + ") ->\n    "
                                 "case " + accv + " of\n"
                                 "  'none' when 'true' ->\n    case apply " + fnVar + "(" + kv + ", " + vv + ") of\n"
                                 "      'true' when 'true' -> {'Just', {" + kv + ", " + vv + "}}\n"
                                 "      'false' when 'true' -> 'none'\n    end\n"
                                 "  _ when 'true' -> " + accv + "\n"
                                 "end";
                    auto [foldVar, foldLet] = bindFun(foldF);
                    return letExpr + foldLet + "call 'maps':'fold'(" + foldVar + ", 'none', " + recv + ")";
                }
            }

            if (node.method == "each") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                return letExpr + "call 'lists':'foreach'(" + fnVar + ", " + recv + ")";
            }
            if (node.method == "map") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                return letExpr + "call 'lists':'map'(" + fnVar + ", " + recv + ")";
            }
            // count { |x| pred } → length(filter(pred, list))
            if (node.method == "count" && (node.block || !node.args.empty())) {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                return letExpr + "call 'erlang':'length'(call 'lists':'filter'(" + fnVar + ", " + recv + "))";
            }
            if (node.method == "filter" || node.method == "select") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                return letExpr + "call 'lists':'filter'(" + fnVar + ", " + recv + ")";
            }
            if (node.method == "reduce" || node.method == "inject") {
                if (node.args.size() >= 1 && node.block) {
                    auto init = emitExpr(node.args[0]);
                    auto [fnVar, letExpr] = bindFun(emitExpr(*node.block));
                    // Kex block args are (acc, elem); lists:foldl passes (elem, acc) — wrap to swap.
                    auto ev = freshVar("E"); auto av = freshVar("A");
                    auto swapFun = "fun (" + ev + ", " + av + ") ->\n    apply " + fnVar + "(" + av + ", " + ev + ")";
                    auto [swapVar, swapLet] = bindFun(swapFun);
                    return letExpr + swapLet + "call 'lists':'foldl'(" + swapVar + ", " + init + ", " + recv + ")";
                }
            }

            // Type conversion: x.to(String), x.to(Int), x.to(Float)
            if (node.method == "to" && node.args.size() == 1) {
                std::string typeName;
                if (auto* ui = std::get_if<ast::UpperIdentifier>(&node.args[0]->kind))
                    typeName = ui->name;
                else if (auto* ve = std::get_if<ast::VarExpr>(&node.args[0]->kind))
                    typeName = ve->name;
                if (typeName == "String")
                    return "call 'kex_io':'to_string'(" + recv + ")";
                if (typeName == "Int")
                    return "call 'erlang':'round'(" + recv + ")";
                if (typeName == "Float")
                    return "call 'erlang':'float'(" + recv + ")";
            }

            // Option methods
            if (node.method == "or" && node.args.size() == 1) {
                // Some(x).or(default) or Just(x).or(default) → x; None.or(default) → default
                auto dflt = firstArg();
                auto tmp  = freshVar("Opt");
                return "let <" + tmp + "> =\n    " + recv + "\nin\n"
                       "case " + tmp + " of\n"
                       "  {'Just', _V} when 'true' -> _V\n"
                       "  {'Some', _V} when 'true' -> _V\n"
                       "  'none' when 'true' -> " + dflt + "\n"
                       "  _ when 'true' -> " + tmp + "\n"
                       "end";
            }
            if (node.method == "some?")
                return "call 'erlang':'=/='(" + recv + ", 'none')";
            if (node.method == "none?")
                return "call 'erlang':'=:='(" + recv + ", 'none')";

            // Extra list methods
            if (node.method == "sum") {
                if (node.block || !node.args.empty()) {
                    // sum { |x| expr } → lists:sum(lists:map(fun, recv))
                    auto [fnVar, letExpr] = bindFun(rawBlock());
                    return letExpr + "call 'lists':'sum'(call 'lists':'map'(" + fnVar + ", " + recv + "))";
                }
                return "call 'lists':'sum'(" + recv + ")";
            }
            if (node.method == "min")
                return "call 'lists':'min'(" + recv + ")";
            if (node.method == "max")
                return "call 'lists':'max'(" + recv + ")";
            if (node.method == "sort")
                return "call 'lists':'sort'(" + recv + ")";
            if (node.method == "uniq" || node.method == "unique")
                return "call 'lists':'usort'(" + recv + ")";
            if (node.method == "zip" && node.args.size() == 1)
                return "call 'lists':'zip'(" + recv + ", " + firstArg() + ")";
            if (node.method == "take" && node.args.size() == 1)
                return "call 'lists':'sublist'(" + recv + ", " + firstArg() + ")";
            if (node.method == "drop" && node.args.size() == 1)
                return "call 'lists':'nthtail'(" + firstArg() + ", " + recv + ")";
            if (node.method == "size" || (node.method == "length" && node.args.empty()))
                return "call 'erlang':'length'(" + recv + ")";

            // Positional accessors — runtime-dispatch on tuple vs list
            auto nthOrElement = [&](int n) -> std::string {
                auto tmp = freshVar("Pos");
                return "let <" + tmp + "> = " + recv + " in\n"
                       "case call 'erlang':'is_tuple'(" + tmp + ") of\n"
                       "  'true' when 'true' -> call 'erlang':'element'("
                           + std::to_string(n) + ", " + tmp + ")\n"
                       "  'false' when 'true' -> call 'lists':'nth'("
                           + std::to_string(n) + ", " + tmp + ")\n"
                       "end";
            };
            if (node.method == "second" && node.args.empty()) return nthOrElement(2);
            if (node.method == "third"  && node.args.empty()) return nthOrElement(3);

            // Predicate combinators (any?, all?, none?, find, reject)
            if (node.method == "any?" || node.method == "none?") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                auto matched = "call 'lists':'any'(" + fnVar + ", " + recv + ")";
                if (node.method == "none?")
                    matched = "call 'erlang':'not'(" + matched + ")";
                return letExpr + matched;
            }
            if (node.method == "all?") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                return letExpr + "call 'lists':'all'(" + fnVar + ", " + recv + ")";
            }
            if (node.method == "reject") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                auto negVar = freshVar("Neg");
                // Core Erlang funs have no 'end'; apply VarName(Args) calls a fun variable.
                return letExpr +
                       "let <" + negVar + "> = fun (_NX) -> call 'erlang':'not'(apply " + fnVar + "(_NX)) in\n"
                       "call 'lists':'filter'(" + negVar + ", " + recv + ")";
            }
            if (node.method == "find") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                auto tmp = freshVar("Found");
                return letExpr +
                       "case call 'lists':'search'(" + fnVar + ", " + recv + ") of\n"
                       "  {'value', " + tmp + "} when 'true' -> {'" + "Just', " + tmp + "}\n"
                       "  'false' when 'true' -> 'none'\n"
                       "end";
            }
            // Map methods: put, delete, get, merge, mapKeys, mapValues, keys, values
            if (node.method == "put" && node.args.size() == 2)
                return "call 'maps':'put'(" + emitExpr(node.args[0]) + ", " + emitExpr(node.args[1]) + ", " + recv + ")";
            if (node.method == "delete" && node.args.size() == 1)
                return "call 'maps':'remove'(" + firstArg() + ", " + recv + ")";
            if (node.method == "get" && node.args.size() == 1) {
                // maps:find returns {ok,V} | error — wrap as Just/None for Kex
                auto kv = freshVar("V");
                return "case call 'maps':'find'(" + firstArg() + ", " + recv + ") of\n"
                       "  {'ok', " + kv + "} when 'true' -> {'Just', " + kv + "}\n"
                       "  'error' when 'true' -> 'none'\n"
                       "end";
            }
            if (node.method == "get" && node.args.size() == 2)
                return "call 'maps':'get'(" + emitExpr(node.args[0]) + ", " + recv + ", " + emitExpr(node.args[1]) + ")";
            if (node.method == "merge" && node.args.size() == 1)
                return "call 'maps':'merge'(" + recv + ", " + firstArg() + ")";
            if (node.method == "has?" && node.args.size() == 1)
                return "call 'maps':'is_key'(" + firstArg() + ", " + recv + ")";
            if (node.method == "keys" && node.args.empty())
                return "call 'maps':'keys'(" + recv + ")";
            if (node.method == "values" && node.args.empty())
                return "call 'maps':'values'(" + recv + ")";
            if (node.method == "entries" && node.args.empty())
                return "call 'maps':'to_list'(" + recv + ")";
            if (node.method == "size" && node.args.empty())
                return "call 'maps':'size'(" + recv + ")";
            if (node.method == "mapKeys") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                auto accVar = freshVar("MA"); auto kVar = freshVar("K"); auto vVar = freshVar("V");
                auto foldFun = "fun (" + kVar + ", " + vVar + ", " + accVar + ") ->\n    "
                               "call 'maps':'put'(apply " + fnVar + "(" + kVar + "), " + vVar + ", " + accVar + ")";
                auto [foldVar, foldLet] = bindFun(foldFun);
                return letExpr + foldLet +
                       "call 'maps':'fold'(" + foldVar + ", call 'maps':'new'(), " + recv + ")";
            }
            if (node.method == "mapValues") {
                auto [fnVar, letExpr] = bindFun(rawBlock());
                // maps:map needs fun(K, V) -> NewV. If user wrote |v| (1-arg), wrap it.
                if (!blockArity2()) {
                    auto kv = freshVar("K"); auto vv = freshVar("V");
                    auto wrapFun = "fun (" + kv + ", " + vv + ") ->\n    apply " + fnVar + "(" + vv + ")";
                    auto [wrapVar, wrapLet] = bindFun(wrapFun);
                    return letExpr + wrapLet + "call 'maps':'map'(" + wrapVar + ", " + recv + ")";
                }
                return letExpr + "call 'maps':'map'(" + fnVar + ", " + recv + ")";
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
                return "fun (" + pv + ") ->\n    apply '" + node.name + "'/1(" + pv + ")";
            }
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
                if (clause.guard)
                    result += " when " + emitExpr(*clause.guard);
                else
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

void CoreErlangEmitter::collectAssigned(const std::vector<ast::ExprPtr>& body,
                                         std::unordered_set<std::string>& out) {
    for (const auto& e : body) {
        if (!e) continue;
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ast::AssignExpr>) {
                out.insert(node.name);
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                collectAssigned(node.thenBody, out);
                if (node.elseBody) collectAssigned(*node.elseBody, out);
                for (auto& [cond, b] : node.elifs) collectAssigned(b, out);
            } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                collectAssigned(node.body, out);
            } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                collectAssigned(node.body, out);
            } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                for (auto& clause : node.clauses) {
                    if (clause.body) {
                        if (auto* ae = std::get_if<ast::AssignExpr>(&clause.body->kind))
                            out.insert(ae->name);
                        // Recurse into match arm bodies that are IfExprs or blocks
                        // (single-expr bodies only for now)
                    }
                }
            }
            // Don't cross Lambda boundaries — lambdas capture by value on BEAM.
        }, e->kind);
    }
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
                                          const std::vector<std::string>& mutParams) -> std::string {
    if (start >= (int)body.size())
        return makeTailCall(loopFn, loopArity, mutParams);  // fall-through = next iteration

    const auto& e = body[start];

    // AssignExpr for a loop-threaded var → fresh binding, update subst, continue
    if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
        if (m_varSubst.count(ae->name)) {
            std::string newVar = freshVar(ae->name);
            std::string val = emitExpr(ae->value);
            auto prev = m_varSubst[ae->name];
            m_varSubst[ae->name] = newVar;
            std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams);
            m_varSubst[ae->name] = prev;
            return "let <" + newVar + "> =\n    " + val + "\nin\n" + rest;
        }
        // Unknown var — treat as side effect, bind to tmp
        std::string tmp = freshVar("S");
        std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams);
        return "let <" + tmp + "> =\n    " + emitExpr(ae->value) + "\nin\n" + rest;
    }

    // ReturnExpr → exit loop and enclosing function
    if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind)) {
        if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
            std::string cont = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams);
            return "case " + emitExpr(ti->condition) + " of\n"
                   "  'true' when 'true' ->\n    " + emitExpr(ti->expr) + "\n"
                   "  'false' when 'true' ->\n    " + cont + "\n"
                   "end";
        }
        return emitExpr(re->value);  // discard loop rest
    }

    // BreakExpr → return current mutable var state as a tuple (or 'ok' if none)
    if (std::get_if<ast::BreakExpr>(&e->kind)) {
        if (mutParams.empty()) return "'ok'";
        std::string tuple = "{";
        for (size_t i = 0; i < mutParams.size(); i++) {
            if (i > 0) tuple += ", ";
            auto it = m_varSubst.find(mutParams[i]);
            tuple += (it != m_varSubst.end()) ? it->second : "_";
        }
        return tuple + "}";
    }

    // IfExpr → emit branches with loop context
    if (auto* ie = std::get_if<ast::IfExpr>(&e->kind)) {
        std::string cond = emitExpr(ie->condition);
        // Save subst state for merging branches
        auto substSnap = m_varSubst;
        std::string thenPart = emitLoopBodyFrom(ie->thenBody, 0, loopFn, loopArity, mutParams);
        m_varSubst = substSnap;
        if (ie->elseBody) {
            std::string elsePart = emitLoopBodyFrom(*ie->elseBody, 0, loopFn, loopArity, mutParams);
            m_varSubst = substSnap;
            return "case " + cond + " of\n"
                   "  'true' when 'true' ->\n    " + thenPart + "\n"
                   "  'false' when 'true' ->\n    " + elsePart + "\n"
                   "end";
        }
        // No else: false branch = continue with rest of loop body
        std::string cont = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams);
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
        std::string result = "case " + scrutinee + " of\n";
        for (const auto& clause : me->clauses) {
            std::string pat = clause.patterns.empty() ? freshVar("W") : emitPattern(clause.patterns[0]);
            result += "  " + pat;
            if (clause.guard)
                result += " when " + emitExpr(*clause.guard);
            else
                result += " when 'true'";
            result += " ->\n";
            // Emit arm body in loop context
            auto substSnap = m_varSubst;
            std::string armBody;
            if (clause.body) {
                // Wrap single arm expr as a 1-element body for uniform treatment
                std::vector<ast::ExprPtr> armBodyVec;
                // We can't move — make a pseudo call: treat as emitLoopBodyFrom([armExpr])
                // For simple cases: AssignExpr, ReturnExpr, other expr
                if (auto* ae2 = std::get_if<ast::AssignExpr>(&clause.body->kind)) {
                    if (m_varSubst.count(ae2->name)) {
                        std::string newVar = freshVar(ae2->name);
                        std::string val = emitExpr(ae2->value);
                        m_varSubst[ae2->name] = newVar;
                        armBody = "let <" + newVar + "> =\n    " + val + "\nin\n"
                                + makeTailCall(loopFn, loopArity, mutParams);
                    } else {
                        armBody = emitExpr(ae2->value);
                    }
                } else if (auto* re2 = std::get_if<ast::ReturnExpr>(&clause.body->kind)) {
                    armBody = emitExpr(re2->value);
                } else {
                    armBody = emitExpr(clause.body);
                }
            } else {
                armBody = makeTailCall(loopFn, loopArity, mutParams);
            }
            m_varSubst = substSnap;
            result += "    " + armBody + "\n";
        }
        result += "end";
        // After match in loop body: continue with next loop body statement
        // (This handles the case where match is not the last statement)
        if (start + 1 < (int)body.size()) {
            std::string cont = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams);
            std::string tmp = freshVar("S");
            return "let <" + tmp + "> =\n    " + result + "\nin\n" + cont;
        }
        return result;
    }

    // VarExpr inside loop body — local variable (not a loop param)
    if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
        std::string ceVar = erlVar(ve->name);
        m_varSubst[ve->name] = ceVar;
        std::string val = emitExpr(ve->value);
        std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams);
        m_varSubst.erase(ve->name);
        return "let <" + ceVar + "> =\n    " + val + "\nin\n" + rest;
    }

    // LetExpr inside loop body
    if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
        if (le->pattern) {
            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                std::string ceVar = erlVar(vp->name);
                std::string val = emitExpr(le->value);
                std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams);
                return "let <" + ceVar + "> =\n    " + val + "\nin\n" + rest;
            } else {
                auto tmpVar = freshVar("D");
                auto pat = emitPattern(le->pattern);
                auto val = emitExpr(le->value);
                std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams);
                return "let <" + tmpVar + "> =\n    " + val + "\nin\n"
                       "case " + tmpVar + " of\n"
                       "  " + pat + " when 'true' ->\n    " + rest + "\nend";
            }
        }
    }

    // Default: bind to tmp, continue
    std::string tmp = freshVar("S");
    std::string val = emitExpr(e);
    std::string rest = emitLoopBodyFrom(body, start + 1, loopFn, loopArity, mutParams);
    return "let <" + tmp + "> =\n    " + val + "\nin\n" + rest;
}

auto CoreErlangEmitter::emitLoopExpr(const std::vector<ast::ExprPtr>& loopBody,
                                      const ast::ExprPtr* condition,
                                      const std::vector<ast::ExprPtr>& outerBody,
                                      int outerStart) -> std::string {
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
    // vars after the loop resolve to the post-loop SSA names.
    std::string rest = emitBodyFrom(outerBody, outerStart + 1);

    // Restore subst (the caller's emitBodyFrom chain handles its own scope).
    m_varSubst = savedSubst;

    std::ostringstream out;
    out << "letrec '" << loopFn << "'/" << loopArity << " =\n"
        << "  fun (" << paramList << ") ->\n"
        << "    " << loopBodyStr << "\n"
        << "in\n";

    if (loopArity == 0) {
        // No mutable state — just run the loop and continue.
        if (outerStart + 1 >= (int)outerBody.size()) {
            out << "apply '" << loopFn << "'/0(" << initArgs << ")";
        } else {
            std::string lr = freshVar("LR");
            out << "let <" << lr << "> =\n    apply '" << loopFn << "'/0(" << initArgs << ")\n"
                << "in\n" << rest;
        }
    } else if (outerStart + 1 >= (int)outerBody.size()) {
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
            // Destructuring
            auto tmpVar = freshVar("D");
            auto pat    = emitPattern(le->pattern);
            auto val    = emitExpr(le->value);
            std::string rest = isLast ? "'ok'" : emitBodyFrom(body, start + 1);
            return "let <" + tmpVar + "> =\n    " + val + "\nin\n"
                   "case " + tmpVar + " of\n"
                   "  " + pat + " when 'true' ->\n    " + rest + "\nend";
        }
        std::string tmp = freshVar("T");
        std::string rest = isLast ? tmp : emitBodyFrom(body, start + 1);
        return "let <" + tmp + "> =\n    " + emitExpr(le->value) + "\nin\n" + rest;
    }

    // ReturnExpr: early return (discard rest).
    if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind)) {
        if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
            std::string cont = isLast ? "'ok'" : emitBodyFrom(body, start + 1);
            return "case " + emitExpr(ti->condition) + " of\n"
                   "  'true' when 'true' ->\n    " + emitExpr(ti->expr) + "\n"
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
        return emitExpr(re->value);  // unconditional return — discard rest
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
                                          bool hasImplicitThis)
    -> std::string
{
    if (group.empty()) return "";
    const auto& first = *group[0];

    // Count total clauses across all nodes
    int totalClauses = 0;
    for (const auto* fn : group) totalClauses += static_cast<int>(fn->clauses.size());
    if (totalClauses == 0) return "";

    int explicitArity = static_cast<int>(first.clauses.empty() ? 0 : first.clauses[0].params.size());
    // If the make block adds `this` as an implicit first receiver, total arity is +1.
    int arity = explicitArity + (hasImplicitThis ? 1 : 0);
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

    // Helper: generate let-bindings for a single parameter vs its arg variable.
    // Recursively generate let-bindings that destructure a pattern against a source expr.
    // For a VarPattern: `let <Var> = src in`
    // For a RecordPattern { age }: `let <Age> = apply 'age'/1(src) in`
    // For a RecordPattern { address: { city } }: extract address, then city from that
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

    // Single clause across the whole group: emit without case dispatch.
    if (totalClauses == 1) {
        const auto& clause = first.clauses[0];
        std::string paramLets;
        // If `this` is implicit, _Arg0 = this; explicit params start at _Arg1.
        if (hasImplicitThis)
            paramLets += "let <This> = _Arg0 in\n";
        int argOffset = hasImplicitThis ? 1 : 0;
        for (size_t i = 0; i < clause.params.size(); i++)
            paramLets += bindParamLets(clause.params[i], argVars[i + argOffset]);
        std::ostringstream out;
        out << "'" << first.name << "'/" << arity << " =\n";
        out << "  fun " << funHead << " ->\n";
        out << "    " << paramLets << emitBody(clause.body) << "\n";
        return out.str();
    }

    // Multi-clause: Core Erlang case dispatch.
    // Single-arg: case _Arg0 of Pat when 'true' -> body
    // Multi-arg:  case <_Arg0, _Arg1> of <P0, P1> when 'true' -> body
    // For implicit-this functions the case dispatches on explicit params only (_Arg1+)
    int argOffset = hasImplicitThis ? 1 : 0;
    int caseArity = arity - argOffset;
    bool multiArg = caseArity > 1;

    std::ostringstream out;
    out << "'" << first.name << "'/" << arity << " =\n";
    out << "  fun " << funHead << " ->\n";
    // Bind `this` first if implicit
    if (hasImplicitThis)
        out << "    let <This> = _Arg0 in\n";

    if (multiArg) {
        out << "    case <";
        for (int i = argOffset; i < arity; i++) {
            if (i > argOffset) out << ", ";
            out << argVars[i];
        }
        out << "> of\n";
    } else if (caseArity == 1) {
        out << "    case " << argVars[argOffset] << " of\n";
    } else {
        // Zero explicit args: shouldn't have multi-clause but handle gracefully
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

            // Bind explicit params (offset by 1 if implicit this)
            std::string paramLets;
            for (size_t i = 0; i < clause.params.size(); i++)
                paramLets += bindParamLets(clause.params[i], argVars[i + argOffset]);
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
    m_loopCounter = 0;
    m_exports.clear();
    m_topLevelFns.clear();
    m_staticCtors.clear();
    m_varSubst.clear();
    m_moduleName = "kex_" + fileStem;

    // Reset field accessor map for this compilation unit.
    m_fieldAccessors.clear();
    auto& fieldAccessors = m_fieldAccessors; // alias for use in lambdas below

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
                        bool firstIsReceiver = false;
                        if (!isStaticCtor && !fd.clauses.empty() && !fd.clauses[0].params.empty()) {
                            const auto& p0 = fd.clauses[0].params[0];
                            if (!p0.name && p0.pattern &&
                                std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind))
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
                }
            }
        }, item);
    }

    // Second pass: group consecutive FunctionDefs with the same name (multi-clause
    // functions written as separate `let` declarations), then emit each group once.
    // Each group is a list of pointers to the original FunctionDef nodes.
    std::vector<std::string> functionTexts;

    using FnGroup  = std::vector<const ast::FunctionDef*>;
    struct OrderedItem { bool isMain; bool hasImplicitThis; FnGroup fns; const ast::MainBlock* mb; };
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
                // Helper to push one FunctionDef (from either direct body or VisibilityBlock).
                auto pushMakeFn = [&](const ast::FunctionDef* fd) {
                    if (!fd) return;
                    // Uppercase-named functions are static constructors — no implicit This.
                    bool isStaticCtor = !fd->name.empty() &&
                                       std::isupper(static_cast<unsigned char>(fd->name[0]));
                    bool firstParamIsReceiver = false;
                    if (!isStaticCtor && !fd->clauses.empty() && !fd->clauses[0].params.empty()) {
                        const auto& p0 = fd->clauses[0].params[0];
                        if (!p0.name && p0.pattern &&
                            std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind))
                            firstParamIsReceiver = true;
                    }
                    bool needsImplicitThis = !isStaticCtor && !firstParamIsReceiver;
                    if (!ordered.empty() && !ordered.back().isMain
                        && ordered.back().fns[0]->name == fd->name) {
                        ordered.back().fns.push_back(fd);
                    } else {
                        ordered.push_back({false, needsImplicitThis, {fd}, nullptr});
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
            functionTexts.push_back(emitFunctionGroup(oi.fns, oi.hasImplicitThis));
        }
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
