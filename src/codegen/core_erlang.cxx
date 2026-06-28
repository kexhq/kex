#include "core_erlang.hxx"
#include <algorithm>
#include <sstream>

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
            // Sum type variant: Tag(args...) → {'Tag', args...}
            if (p.args.empty()) return "{'" + p.name + "'}";
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
            return erlString(node.value);
        }

        // --- Variables ---
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
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
            // Local function — use apply
            int arity = static_cast<int>(node.args.size());
            return "apply '" + node.name + "'/" + std::to_string(arity) + "(" + args + ")";
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
            // Regular UFCS: x.foo(args) → apply foo/N+1(x, args...)
            // Type-directed dispatch will resolve this properly in M3.
            auto recv = emitExpr(node.receiver);
            std::string args = recv;
            for (const auto& a : node.args) args += ", " + emitExpr(a);
            int arity = 1 + static_cast<int>(node.args.size());
            return "apply '" + node.method + "'/" + std::to_string(arity) + "(" + args + ")";
        }

        // --- If/else ---
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            auto cond = emitExpr(node.condition);
            auto then = emitBody(node.thenBody);
            std::string otherwise;
            if (node.elseBody) {
                otherwise = emitBody(*node.elseBody);
            } else {
                otherwise = "'ok'";
            }
            // Handle elif chains
            std::string result = "case " + cond + " of\n"
                                 "  'true' ->\n    " + then + ";\n"
                                 "  'false' ->\n    " + otherwise + "\n"
                                 "end";
            return result;
        }

        // --- Match ---
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            auto subject = emitExpr(node.subject);
            std::string result = "case " + subject + " of\n";
            for (size_t i = 0; i < node.clauses.size(); i++) {
                const auto& clause = node.clauses[i];
                // Multiple patterns in one clause → separate cases (for now emit first)
                auto pat = clause.patterns.empty() ? std::string("_")
                                                   : emitPattern(clause.patterns[0]);
                result += "  " + pat;
                if (clause.guard)
                    result += " when " + emitExpr(*clause.guard);
                result += " ->\n    " + emitBody({}) + emitExpr(clause.body);
                if (i + 1 < node.clauses.size()) result += ";\n";
            }
            result += "\nend";
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
            // let Pattern = value → extract binding name from pattern
            if (le->pattern) {
                if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind))
                    bindVar = erlVar(vp->name);
                else
                    bindVar = emitPattern(le->pattern);
            } else {
                bindVar = freshVar("T");
            }
            bindVal = emitExpr(le->value);
        } else if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
            bindVar = erlVar(ve->name);
            bindVal = emitExpr(ve->value);
        } else if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
            // Reassignment (mutable) — SSA will handle versioning properly in M4;
            // for now rebind to a fresh name and shadow.
            bindVar = erlVar(ae->name);
            bindVal = emitExpr(ae->value);
        } else {
            // Side-effecting expression: bind to throwaway
            bindVar = freshVar("S");
            bindVal = emitExpr(e);
        }

        result = "let " + bindVar + " =\n    " + bindVal + "\nin\n" + result;
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
                // Need a let binding
                paramLets += "let " + paramName + " = " + argVars[i] + " in\n";
            }
        }

        body = paramLets + emitBody(clause.body);

        std::ostringstream out;
        out << "'" << fn.name << "'/" << arity << " =\n";
        out << "  fun " << funHead << " ->\n";
        out << "    " << body << "\n";
        return out.str();
    }

    // Multi-clause: case on args tuple (M2+ feature — emit stub for now).
    std::ostringstream out;
    out << "'" << fn.name << "'/" << arity << " =\n";
    out << "  fun " << funHead << " ->\n";
    out << "    case {";
    for (size_t i = 0; i < argVars.size(); i++) {
        if (i > 0) out << ", ";
        out << argVars[i];
    }
    out << "} of\n";
    for (const auto& clause : fn.clauses) {
        out << "      {";
        for (size_t i = 0; i < clause.params.size(); i++) {
            if (i > 0) out << ", ";
            if (clause.params[i].pattern)
                out << emitPattern(*clause.params[i].pattern);
            else if (clause.params[i].name)
                out << erlVar(*clause.params[i].name);
            else
                out << "_";
        }
        out << "} ->\n        " << emitBody(clause.body) << ";\n";
    }
    out << "      _ -> call 'erlang':'error'('function_clause')\n";
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
    } else {
        out << "  fun (_Args) ->\n";
    }
    out << "    " << emitBody(main.body) << "\n";
    return out.str();
}

// ---------------------------------------------------------------------------
// Program entry point
// ---------------------------------------------------------------------------

auto CoreErlangEmitter::emitProgram(const ast::Program& prog,
                                     const std::string& fileStem) -> EmitResult {
    m_varCounter = 0;
    m_exports.clear();
    m_moduleName = "kex_" + fileStem;

    // First pass: collect and emit each function / main block.
    std::vector<std::string> functionTexts;

    for (const auto& item : prog.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) functionTexts.push_back(emitFunctionDef(*node));
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (node) functionTexts.push_back(emitMainBlock(*node));
            }
            // TODO M3: ModuleDef, RecordDef, TypeDef
        }, item);
    }

    // If no main was emitted but there are top-level expressions (synthetic
    // main blocks), they were already handled above.

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
