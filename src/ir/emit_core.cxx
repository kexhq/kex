#include "emit_core.hxx"
#include <sstream>

namespace kex::ir {

namespace {

// Kex name / IR fresh name → Core Erlang variable. Fresh names already start
// with '_' (valid); Kex names get their first letter uppercased.
auto erlVar(const std::string& s) -> std::string {
    if (s.empty()) return "_V";
    if (s[0] == '_') return s;
    std::string out;
    out += static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    out += s.substr(1);
    return out;
}

auto erlString(const std::string& s) -> std::string {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out + "\"";
}

struct Emitter {
    int wildCounter = 0;
    bool m_returnThrows = false;

    // Does this IR subtree contain a Return node (not crossing a Lambda /
    // LetRec, which are their own return scopes)?
    static auto hasReturn(const ExprPtr& e) -> bool {
        if (!e) return false;
        return std::visit([](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Return>) return true;
            else if constexpr (std::is_same_v<T, Let>) return hasReturn(n.value) || hasReturn(n.body);
            else if constexpr (std::is_same_v<T, Seq>) {
                for (auto& x : n.exprs) if (hasReturn(x)) return true; return false;
            } else if constexpr (std::is_same_v<T, Match>) {
                for (auto& s : n.subjects) if (hasReturn(s)) return true;
                for (auto& c : n.clauses) {
                    if (c.guard && hasReturn(*c.guard)) return true;
                    if (hasReturn(c.body)) return true;
                }
                return false;
            } else if constexpr (std::is_same_v<T, Intrinsic>) {
                for (auto& a : n.args) if (hasReturn(a)) return true; return false;
            } else if constexpr (std::is_same_v<T, Call>) {
                for (auto& a : n.args) if (hasReturn(a)) return true; return false;
            } else if constexpr (std::is_same_v<T, CallIndirect>) {
                if (hasReturn(n.callee)) return true;
                for (auto& a : n.args) if (hasReturn(a)) return true; return false;
            } else if constexpr (std::is_same_v<T, MakeTuple>) {
                for (auto& a : n.elements) if (hasReturn(a)) return true; return false;
            } else if constexpr (std::is_same_v<T, MakeList>) {
                for (auto& a : n.elements) if (hasReturn(a)) return true;
                return n.rest && hasReturn(*n.rest);
            } else if constexpr (std::is_same_v<T, Construct>) {
                for (auto& a : n.args) if (hasReturn(a)) return true; return false;
            }
            // Lambda/LetRec bodies are separate return scopes; leaves have none.
            return false;
        }, e->node);
    }

    // Wrap a body so a thrown {'kex_return', V} becomes the value; anything
    // else is re-raised. Structure mirrors what erlc emits for try/catch.
    auto uniq(const char* p) -> std::string { return std::string(p) + std::to_string(wildCounter++); }
    auto wrapReturnCatch(const std::string& body) -> std::string {
        std::string rv = uniq("_Ret"), rvv = uniq("_RV");
        std::string cls = uniq("_Cls"), rsn = uniq("_Rsn"), trc = uniq("_Trc");
        std::string t = uniq("_T"), c = uniq("_C"), r = uniq("_R"), tr = uniq("_Tr");
        return "try\n    " + body + "\n"
               "of <" + rv + "> -> " + rv + "\n"
               "catch <" + cls + ", " + rsn + ", " + trc + "> ->\n"
               "  case <" + cls + ", " + rsn + ", " + trc + "> of\n"
               "    <'throw', {'kex_return', " + rvv + "}, " + t + "> when 'true' -> " + rvv + "\n"
               "    <" + c + ", " + r + ", " + tr + "> when 'true' -> primop 'raise'(" +
                    trc + ", " + rsn + ")\n"
               "  end";
    }
    // Emit a clause body, wrapping it in the return try/catch iff it contains
    // an early return.
    auto emitClauseBody(const ExprPtr& body) -> std::string {
        if (!hasReturn(body)) return emit(body);
        bool saved = m_returnThrows;
        m_returnThrows = true;
        std::string b = wrapReturnCatch(emit(body));
        m_returnThrows = saved;
        return b;
    }
    // Core Erlang treats `_` as a real variable, so repeating it within one
    // pattern is a duplicate-variable error — each wildcard needs a distinct
    // fresh name.
    auto freshWild() -> std::string { return "_Wc" + std::to_string(wildCounter++); }
    auto emitLit(const Lit& l) -> std::string {
        switch (l.kind) {
            case LitKind::Int:    return l.text;
            case LitKind::Float:  return l.text;
            case LitKind::Char:   return std::to_string(static_cast<int>(l.text[0]));
            case LitKind::String: return erlString(l.text);
            case LitKind::Bool:   return l.boolValue ? "'true'" : "'false'";
            case LitKind::None:   return "'none'";
            case LitKind::Atom:   return "'" + l.text + "'";
        }
        return "'undefined'";
    }

    auto emitIntrinsic(const Intrinsic& in) -> std::string {
        auto a = [&](int i) { return emit(in.args[i]); };
        switch (in.op) {
            // + is polymorphic (numeric add / string concat) at runtime.
            case Op::Add:    return "call 'kex_io':'add'(" + a(0) + ", " + a(1) + ")";
            case Op::Sub:    return "call 'erlang':'-'(" + a(0) + ", " + a(1) + ")";
            case Op::Mul:    return "call 'erlang':'*'(" + a(0) + ", " + a(1) + ")";
            case Op::Div:    return "call 'kex_io':'divide'(" + a(0) + ", " + a(1) + ")";
            case Op::Mod:    return "call 'erlang':'rem'(" + a(0) + ", " + a(1) + ")";
            case Op::Neg:    return "call 'erlang':'-'(" + a(0) + ")";
            case Op::Eq:     return "call 'erlang':'=:='(" + a(0) + ", " + a(1) + ")";
            case Op::Neq:    return "call 'erlang':'=/='(" + a(0) + ", " + a(1) + ")";
            case Op::Lt:     return "call 'erlang':'<'(" + a(0) + ", " + a(1) + ")";
            case Op::Gt:     return "call 'erlang':'>'(" + a(0) + ", " + a(1) + ")";
            case Op::Lte:    return "call 'erlang':'=<'(" + a(0) + ", " + a(1) + ")";
            case Op::Gte:    return "call 'erlang':'>='(" + a(0) + ", " + a(1) + ")";
            case Op::Not:    return "call 'erlang':'not'(" + a(0) + ")";
            case Op::Concat: return "call 'erlang':'++'(" + a(0) + ", " + a(1) + ")";
            case Op::And:    return "case " + a(0) + " of 'true' when 'true' -> " + a(1) +
                                    " 'false' when 'true' -> 'false' end";
            case Op::Or:     return "case " + a(0) + " of 'true' when 'true' -> 'true'" +
                                    " 'false' when 'true' -> " + a(1) + " end";
        }
        return "'undefined'";
    }

    auto emitCall(const Call& c) -> std::string {
        std::string args;
        for (size_t i = 0; i < c.args.size(); i++) {
            if (i) args += ", ";
            args += emit(c.args[i]);
        }
        if (c.module.empty())
            return "apply '" + c.name + "'/" + std::to_string(c.arity) + "(" + args + ")";
        return "call '" + c.module + "':'" + c.name + "'(" + args + ")";
    }

    auto emitPattern(const Pattern& p) -> std::string {
        switch (p.kind) {
            case PatKind::Wild: return freshWild();
            case PatKind::Var:  return erlVar(p.name);
            case PatKind::Lit: {
                Lit l; l.kind = p.litKind; l.text = p.litText; l.boolValue = p.litBool;
                return emitLit(l);
            }
            case PatKind::Construct: {
                if (p.args.empty()) return "'" + p.tag + "'";
                std::string s = "{'" + p.tag + "'";
                for (const auto& a : p.args) s += ", " + emitPattern(*a);
                return s + "}";
            }
            case PatKind::Tuple: {
                std::string s = "{";
                for (size_t i = 0; i < p.args.size(); i++) {
                    if (i) s += ", ";
                    s += emitPattern(*p.args[i]);
                }
                return s + "}";
            }
            case PatKind::List: {
                if (p.args.empty() && !p.rest) return "[]";
                std::string s = "[";
                for (size_t i = 0; i < p.args.size(); i++) {
                    if (i) s += ", ";
                    s += emitPattern(*p.args[i]);
                }
                if (p.rest) s += "|" + emitPattern(*p.rest);
                return s + "]";
            }
        }
        return "_";
    }

    auto emitMatch(const Match& m) -> std::string {
        // Single subject: `case S of ... end`. (Multi-value dispatch is a
        // later addition — the skeleton lowers if/match/destructure to one
        // subject.)
        std::string out = "case " + emit(m.subjects[0]) + " of\n";
        for (const auto& cl : m.clauses) {
            out += "  " + emitPattern(*cl.patterns[0]);
            out += cl.guard ? " when " + emit(*cl.guard) : " when 'true'";
            out += " ->\n    " + emit(cl.body) + "\n";
        }
        out += "end";
        return out;
    }

    auto emit(const ExprPtr& e) -> std::string {
        return std::visit([&](const auto& n) -> std::string {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Lit>) {
                return emitLit(n);
            } else if constexpr (std::is_same_v<T, Var>) {
                return erlVar(n.name);
            } else if constexpr (std::is_same_v<T, Intrinsic>) {
                return emitIntrinsic(n);
            } else if constexpr (std::is_same_v<T, Call>) {
                return emitCall(n);
            } else if constexpr (std::is_same_v<T, Let>) {
                return "let <" + erlVar(n.name) + "> =\n    " + emit(n.value) +
                       "\nin\n" + emit(n.body);
            } else if constexpr (std::is_same_v<T, Match>) {
                return emitMatch(n);
            } else if constexpr (std::is_same_v<T, MakeTuple>) {
                std::string s = "{";
                for (size_t i = 0; i < n.elements.size(); i++) {
                    if (i) s += ", ";
                    s += emit(n.elements[i]);
                }
                return s + "}";
            } else if constexpr (std::is_same_v<T, MakeList>) {
                if (n.elements.empty() && !n.rest) return "[]";
                std::string s = "[";
                for (size_t i = 0; i < n.elements.size(); i++) {
                    if (i) s += ", ";
                    s += emit(n.elements[i]);
                }
                if (n.rest) s += "|" + emit(*n.rest);
                return s + "]";
            } else if constexpr (std::is_same_v<T, Construct>) {
                if (n.args.empty()) return "'" + n.tag + "'";
                std::string s = "{'" + n.tag + "'";
                for (const auto& a : n.args) s += ", " + emit(a);
                return s + "}";
            } else if constexpr (std::is_same_v<T, Lambda>) {
                std::string head = "(";
                for (size_t i = 0; i < n.params.size(); i++) {
                    if (i) head += ", ";
                    head += erlVar(n.params[i]);
                }
                head += ")";
                return "fun " + head + " ->\n    " + emit(n.body);
            } else if constexpr (std::is_same_v<T, CallIndirect>) {
                std::string args;
                for (size_t i = 0; i < n.args.size(); i++) {
                    if (i) args += ", ";
                    args += emit(n.args[i]);
                }
                return "apply " + emit(n.callee) + "(" + args + ")";
            } else if constexpr (std::is_same_v<T, LetRec>) {
                std::string head = "(";
                for (size_t i = 0; i < n.params.size(); i++) {
                    if (i) head += ", ";
                    head += erlVar(n.params[i]);
                }
                head += ")";
                return "letrec '" + n.name + "'/" + std::to_string(n.params.size()) + " =\n"
                       "  fun " + head + " ->\n    " + emit(n.funBody) + "\n"
                       "in\n" + emit(n.contBody);
            } else if constexpr (std::is_same_v<T, Return>) {
                // In a function that has any early return, a Return throws so
                // it escapes nested contexts (a match arm whose value is
                // consumed by an enclosing `let`); the function-body try/catch
                // turns it back into the result. In a return-free tail spot
                // it's just the value.
                if (m_returnThrows)
                    return "call 'erlang':'throw'({'kex_return', " + emit(n.value) + "})";
                return emit(n.value);
            } else {
                return "call 'erlang':'error'('ir_unimplemented')";
            }
        }, e->node);
    }

    auto emitFunction(const FunDef& fn, std::vector<std::pair<std::string,int>>& exports)
        -> std::string {
        exports.push_back({fn.name, fn.arity});
        std::ostringstream out;
        out << "'" << fn.name << "'/" << fn.arity << " =\n";

        // Fast path: exactly one clause with only var/wildcard params → a bare
        // fun, no case dispatch.
        const auto& cl0 = fn.clauses[0];
        bool singleSimple = fn.clauses.size() == 1 &&
            std::all_of(cl0.params.begin(), cl0.params.end(),
                [](const PatternPtr& p){ return p->kind == PatKind::Var || p->kind == PatKind::Wild; });
        if (singleSimple) {
            std::string head = "(";
            for (size_t i = 0; i < cl0.params.size(); i++) {
                if (i) head += ", ";
                head += cl0.params[i]->kind == PatKind::Wild ? "_W" + std::to_string(i)
                                                             : erlVar(cl0.params[i]->name);
            }
            head += ")";
            out << "  fun " << head << " ->\n    " << emitClauseBody(cl0.body) << "\n";
            return out.str();
        }

        // General: dispatch clauses on the argument value-list.
        std::string head = "(", subj = "<";
        for (int i = 0; i < fn.arity; i++) {
            if (i) { head += ", "; subj += ", "; }
            head += "_Arg" + std::to_string(i);
            subj += "_Arg" + std::to_string(i);
        }
        head += ")"; subj += ">";
        out << "  fun " << head << " ->\n";
        out << "    case " << subj << " of\n";
        for (const auto& cl : fn.clauses) {
            std::string pats = "<";
            for (size_t i = 0; i < cl.params.size(); i++) {
                if (i) pats += ", ";
                pats += emitPattern(*cl.params[i]);
            }
            pats += ">";
            out << "      " << pats;
            out << (cl.guard ? " when " + emit(*cl.guard) : " when 'true'");
            out << " ->\n        " << emitClauseBody(cl.body) << "\n";
        }
        // Non-exhaustive fallback: distinct fresh wildcards (a bare `_`
        // repeated in a value-list is a duplicate-variable error).
        out << "      <";
        for (int i = 0; i < fn.arity; i++) { if (i) out << ", "; out << "_Wf" << i; }
        out << "> when 'true' -> call 'erlang':'error'('function_clause')\n";
        out << "    end\n";
        return out.str();
    }
};

} // namespace

auto emitCore(const Module& mod) -> EmitResult {
    Emitter em;
    std::vector<std::pair<std::string,int>> exports;
    std::vector<std::string> fns;
    for (const auto& fn : mod.functions)
        fns.push_back(em.emitFunction(fn, exports));

    exports.push_back({"module_info", 0});
    exports.push_back({"module_info", 1});

    std::ostringstream out;
    out << "module '" << mod.name << "' [";
    for (size_t i = 0; i < exports.size(); i++) {
        if (i) out << ", ";
        out << "'" << exports[i].first << "'/" << exports[i].second;
    }
    out << "]\n  attributes []\n\n";
    for (const auto& f : fns) out << f << "\n";
    out << "'module_info'/0 =\n  fun () ->\n    call 'erlang':'get_module_info'('"
        << mod.name << "')\n";
    out << "'module_info'/1 =\n  fun (_Key) ->\n    call 'erlang':'get_module_info'('"
        << mod.name << "', _Key)\n";
    out << "end\n";

    EmitResult r;
    r.source = out.str();
    r.moduleName = mod.name;
    r.mainArity = mod.mainArity;
    return r;
}

} // namespace kex::ir
