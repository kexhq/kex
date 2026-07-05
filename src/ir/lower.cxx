#include "lower.hxx"
#include "../lexer/token.hxx"
#include <unordered_map>

namespace kex::ir {

namespace {

// A pending `let name = value in ...` binding, accumulated while lowering a
// compound expression's operands into ANF, then wrapped (outermost-first)
// around the consuming expression.
struct Binding {
    std::string name;
    ExprPtr value;
};

struct Lowering {
    int counter = 0;
    // SSA renaming: Kex source name → its current IR variable name. A `let`/
    // `var`/reassignment introduces a fresh name and updates this; Identifier
    // lowering consults it. Function params map to themselves (the emitter
    // uppercases). This is the SSA construction the string emitter did via
    // m_varSubst, now done once in the lowering pass.
    std::unordered_map<std::string, std::string> subst;

    auto fresh(const std::string& hint = "T") -> std::string {
        return "_ir_" + hint + std::to_string(counter++);
    }

    auto currentName(const std::string& kexName) -> std::string {
        auto it = subst.find(kexName);
        return it != subst.end() ? it->second : kexName;
    }

    // Wrap `body` in the accumulated let-bindings, evaluated left-to-right
    // (so binds[0] is the outermost let).
    auto wrapLets(std::vector<Binding>& binds, ExprPtr body) -> ExprPtr {
        for (auto it = binds.rbegin(); it != binds.rend(); ++it) {
            auto let = std::make_unique<Expr>();
            let->node = Let{std::move(it->name), std::move(it->value), std::move(body)};
            body = std::move(let);
        }
        return body;
    }

    static auto isAtomic(const Expr& e) -> bool {
        return std::holds_alternative<Lit>(e.node) || std::holds_alternative<Var>(e.node);
    }

    // Lower an AST expr and, if the result is compound, bind it to a fresh
    // Let (recorded in `binds`) returning an atomic Var. Literals/vars pass
    // through untouched. This is the ANF normalization step.
    auto atomize(const ast::ExprPtr& e, std::vector<Binding>& binds) -> ExprPtr {
        auto ir = lower(e);
        if (isAtomic(*ir)) return ir;
        auto name = fresh();
        binds.push_back({name, std::move(ir)});
        return var(name);
    }

    auto opOf(TokenType t) -> Op {
        switch (t) {
            case TokenType::Plus: return Op::Add;
            case TokenType::Minus: return Op::Sub;
            case TokenType::Star: return Op::Mul;
            case TokenType::Slash: return Op::Div;
            case TokenType::Percent: return Op::Mod;
            case TokenType::EqEq: return Op::Eq;
            case TokenType::NotEq: return Op::Neq;
            case TokenType::LessThan: return Op::Lt;
            case TokenType::GreaterThan: return Op::Gt;
            case TokenType::LessEq: return Op::Lte;
            case TokenType::GreaterEq: return Op::Gte;
            case TokenType::AmpAmp: return Op::And;
            case TokenType::PipePipe: return Op::Or;
            default:
                throw LowerError("IR lower: unsupported binary operator");
        }
    }

    // ---- Expression lowering ---------------------------------------------
    auto lower(const ast::ExprPtr& e) -> ExprPtr {
        if (!e) return litBool(false);
        return std::visit([&](const auto& n) -> ExprPtr {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::IntLiteral>) {
                return lit(LitKind::Int, n.value);
            } else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
                return lit(LitKind::Float, n.value);
            } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                if (n.value.find("${") != std::string::npos)
                    throw LowerError("IR lower: string interpolation not yet ported");
                return lit(LitKind::String, n.value);
            } else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
                return litBool(n.value);
            } else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
                return lit(LitKind::Char, std::string(1, n.value));
            } else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
                return lit(LitKind::Atom, n.name);
            } else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
                return lit(LitKind::None, "none");
            } else if constexpr (std::is_same_v<T, ast::Identifier>) {
                return var(currentName(n.name));
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                // && / || MUST short-circuit: the right operand is evaluated
                // lazily, so it cannot be ANF-atomized (that would force it).
                // Lower to a boolean Match instead, matching the tree-walker
                // (spec/mutual_recursion.kex relies on `n == 0 || isOdd(n-1)`
                // never recursing once n == 0).
                if (n.op == TokenType::AmpAmp)
                    return matchBool(lower(n.left), lower(n.right), litBool(false));
                if (n.op == TokenType::PipePipe)
                    return matchBool(lower(n.left), litBool(true), lower(n.right));
                std::vector<Binding> binds;
                auto l = atomize(n.left, binds);
                auto r = atomize(n.right, binds);
                auto ex = std::make_unique<Expr>();
                std::vector<ExprPtr> args;
                args.push_back(std::move(l));
                args.push_back(std::move(r));
                ex->node = Intrinsic{opOf(n.op), std::move(args)};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                std::vector<Binding> binds;
                auto a = atomize(n.operand, binds);
                Op op = (n.op == TokenType::Minus) ? Op::Neg
                       : (n.op == TokenType::Bang) ? Op::Not
                       : throw LowerError("IR lower: unsupported unary operator");
                auto ex = std::make_unique<Expr>();
                std::vector<ExprPtr> args;
                args.push_back(std::move(a));
                ex->node = Intrinsic{op, std::move(args)};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
                // a..b  →  lists:seq(a, b)  (materialized ascending list)
                std::vector<Binding> binds;
                auto s = atomize(n.start, binds);
                auto en = atomize(n.end, binds);
                auto ex = std::make_unique<Expr>();
                std::vector<ExprPtr> args;
                args.push_back(std::move(s));
                args.push_back(std::move(en));
                ex->node = Call{"lists", "seq", 2, std::move(args), false};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
                std::vector<Binding> binds;
                std::vector<ExprPtr> els;
                for (const auto& el : n.elements) els.push_back(atomize(el, binds));
                auto ex = std::make_unique<Expr>();
                ex->node = MakeTuple{std::move(els)};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::ListExpr>) {
                std::vector<Binding> binds;
                std::vector<ExprPtr> els;
                for (const auto& el : n.elements) els.push_back(atomize(el, binds));
                std::optional<ExprPtr> rest;
                if (n.rest) rest = atomize(*n.rest, binds);
                auto ex = std::make_unique<Expr>();
                ex->node = MakeList{std::move(els), std::move(rest)};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                return lowerIf(n);
            } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                return lowerMatch(n);
            } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                auto ex = std::make_unique<Expr>();
                ex->node = Return{lower(n.value)};
                return ex;
            } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                return lowerFunctionCall(n);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                return lowerMethodCall(n);
            } else {
                throw LowerError(std::string("IR lower: unimplemented expr node ")
                                 + typeid(T).name());
            }
        }, e->kind);
    }

    auto lowerFunctionCall(const ast::FunctionCall& n) -> ExprPtr {
        if (!n.namedArgs.empty() || n.block)
            throw LowerError("IR lower: named args / block calls not yet ported");
        std::vector<Binding> binds;
        std::vector<ExprPtr> args;
        for (const auto& a : n.args) args.push_back(atomize(a, binds));
        auto ex = std::make_unique<Expr>();
        ex->node = Call{"", n.name, static_cast<int>(n.args.size()), std::move(args), false};
        return wrapLets(binds, std::move(ex));
    }

    // Minimal UFCS/stdlib resolution for the walking skeleton. Only the
    // handful of forms an early target program needs; everything else errors
    // (to be widened as constructs are ported from core_erlang.cxx).
    auto lowerMethodCall(const ast::MethodCall& n) -> ExprPtr {
        // Namespace calls on an UpperIdentifier receiver, e.g. IO.printLine.
        if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.receiver->kind)) {
            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            if (uid->name == "IO") {
                std::string fn = n.method == "printLine" ? "print_line"
                               : n.method == "print"     ? "print"
                               : throw LowerError("IR lower: IO." + n.method + " not yet ported");
                auto ex = std::make_unique<Expr>();
                ex->node = Call{"kex_io", fn, static_cast<int>(args.size()), std::move(args), false};
                return wrapLets(binds, std::move(ex));
            }
            throw LowerError("IR lower: namespace call " + uid->name + "." + n.method
                             + " not yet ported");
        }
        // UFCS method on a value receiver. Pure builtins (no block, no
        // mutation) can build compound IR directly — the emitter prints
        // nested calls fine, so strict ANF atomization isn't required here.
        auto recv = lower(n.receiver);
        auto& m = n.method;

        // No-arg builtins with a single-Call runtime target (receiver-first).
        struct One { const char* method; const char* mod; const char* fn; };
        static const One oneCall[] = {
            {"product", "kex_io", "list_product"},
            {"sum",     "lists",  "sum"},
            {"reverse", "lists",  "reverse"},
            {"abs",     "erlang", "abs"},
            {"sqrt",    "math",   "sqrt"},
        };
        for (const auto& b : oneCall)
            if (m == b.method && n.args.empty() && !n.block)
                return callE(b.mod, b.fn, 1, one(std::move(recv)));

        // modulo(x) → rem(recv, x)
        if (m == "modulo" && n.args.size() == 1)
            return callE("erlang", "rem", 2, two(std::move(recv), lower(n.args[0])));
        if (m == "even?" && n.args.empty())
            return intrin(Op::Eq, two(callE("erlang","rem",2,two(std::move(recv),litInt(2))), litInt(0)));
        if (m == "odd?" && n.args.empty())
            return intrin(Op::Neq, two(callE("erlang","rem",2,two(std::move(recv),litInt(2))), litInt(0)));
        // Result predicates: tag inspection.
        if (m == "ok?" && n.args.empty())
            return intrin(Op::Eq, two(callE("erlang","element",2,two(litInt(1),std::move(recv))),
                                      lit(LitKind::Atom, "Ok")));
        if (m == "error?" && n.args.empty())
            return intrin(Op::Eq, two(callE("erlang","element",2,two(litInt(1),std::move(recv))),
                                      lit(LitKind::Atom, "Error")));

        throw LowerError("IR lower: UFCS method ." + n.method + " not yet ported");
    }

    // ---- IR construction helpers -----------------------------------------
    auto litInt(long v) -> ExprPtr { return lit(LitKind::Int, std::to_string(v)); }
    auto one(ExprPtr a) -> std::vector<ExprPtr> {
        std::vector<ExprPtr> v; v.push_back(std::move(a)); return v;
    }
    auto two(ExprPtr a, ExprPtr b) -> std::vector<ExprPtr> {
        std::vector<ExprPtr> v; v.push_back(std::move(a)); v.push_back(std::move(b)); return v;
    }
    auto callE(std::string mod, std::string fn, int arity, std::vector<ExprPtr> args) -> ExprPtr {
        auto e = std::make_unique<Expr>();
        e->node = Call{std::move(mod), std::move(fn), arity, std::move(args), false};
        return e;
    }
    auto intrin(Op op, std::vector<ExprPtr> args) -> ExprPtr {
        auto e = std::make_unique<Expr>();
        e->node = Intrinsic{op, std::move(args)};
        return e;
    }

    // ---- Constructors -----------------------------------------------------
    auto makeLet(std::string name, ExprPtr value, ExprPtr body) -> ExprPtr {
        auto e = std::make_unique<Expr>();
        e->node = Let{std::move(name), std::move(value), std::move(body)};
        return e;
    }
    // A single-clause match binding `pattern` against `subject`, continuing
    // with `body` — used for `let (a, b) = expr` destructuring.
    auto makeMatch1(ExprPtr subject, PatternPtr pattern, ExprPtr body) -> ExprPtr {
        Match m;
        m.subjects.push_back(std::move(subject));
        MatchClause mc;
        mc.patterns.push_back(std::move(pattern));
        mc.body = std::move(body);
        m.clauses.push_back(std::move(mc));
        auto e = std::make_unique<Expr>();
        e->node = std::move(m);
        return e;
    }
    auto matchBool(ExprPtr cond, ExprPtr thenE, ExprPtr elseE) -> ExprPtr {
        Match m;
        m.subjects.push_back(std::move(cond));
        auto boolPat = [](bool b) {
            auto p = std::make_unique<Pattern>();
            p->kind = PatKind::Lit; p->litKind = LitKind::Bool; p->litBool = b;
            return p;
        };
        MatchClause t; t.patterns.push_back(boolPat(true));  t.body = std::move(thenE);
        MatchClause f; f.patterns.push_back(boolPat(false)); f.body = std::move(elseE);
        m.clauses.push_back(std::move(t));
        m.clauses.push_back(std::move(f));
        auto e = std::make_unique<Expr>();
        e->node = std::move(m);
        return e;
    }

    // ---- Patterns ---------------------------------------------------------
    auto lowerPattern(const ast::PatternPtr& p) -> PatternPtr {
        if (!p) { auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w; }
        return std::visit([&](const auto& pn) -> PatternPtr {
            using T = std::decay_t<decltype(pn)>;
            auto out = std::make_unique<Pattern>();
            if constexpr (std::is_same_v<T, ast::WildcardPattern>) {
                out->kind = PatKind::Wild;
            } else if constexpr (std::is_same_v<T, ast::VarPattern>) {
                out->kind = PatKind::Var; out->name = pn.name;
            } else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
                out->kind = PatKind::Lit;
                switch (pn.literal.type) {
                    case TokenType::Integer: out->litKind = LitKind::Int; out->litText = pn.literal.value; break;
                    case TokenType::Float:   out->litKind = LitKind::Float; out->litText = pn.literal.value; break;
                    case TokenType::String:  out->litKind = LitKind::String; out->litText = pn.literal.value; break;
                    case TokenType::Char:    out->litKind = LitKind::Char; out->litText = pn.literal.value; break;
                    case TokenType::True:    out->litKind = LitKind::Bool; out->litBool = true; break;
                    case TokenType::False:   out->litKind = LitKind::Bool; out->litBool = false; break;
                    case TokenType::Atom:    out->litKind = LitKind::Atom; out->litText = pn.literal.value; break;
                    case TokenType::None:    out->litKind = LitKind::None; out->litText = "none"; break;
                    default: throw LowerError("IR lower: unsupported literal pattern");
                }
            } else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
                out->kind = PatKind::Construct; out->tag = pn.name;
                for (const auto& a : pn.args) out->args.push_back(lowerPattern(a));
            } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
                out->kind = PatKind::Tuple;
                for (const auto& a : pn.elements) out->args.push_back(lowerPattern(a));
            } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
                out->kind = PatKind::List;
                for (const auto& a : pn.elements) out->args.push_back(lowerPattern(a));
                if (pn.rest) out->rest = lowerPattern(*pn.rest);
            } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
                return pn.inner ? lowerPattern(pn.inner)
                                : [&]{ auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w; }();
            } else {
                throw LowerError("IR lower: unsupported pattern kind");
            }
            return out;
        }, p->kind);
    }

    // ---- Control flow -----------------------------------------------------
    // Lower a branch body in its own scope (branch-local bindings must not
    // leak into sibling branches or the code after the `if`/`match`).
    auto lowerBodyScoped(const std::vector<ast::ExprPtr>& body) -> ExprPtr {
        auto snap = subst;
        auto r = lowerBody(body);
        subst = snap;
        return r;
    }

    auto lowerIf(const ast::IfExpr& n) -> ExprPtr {
        if (n.letPattern) throw LowerError("IR lower: if-let not yet ported");
        ExprPtr elsePart = n.elseBody ? lowerBodyScoped(*n.elseBody)
                                      : lit(LitKind::Atom, "ok");
        for (int i = static_cast<int>(n.elifs.size()) - 1; i >= 0; --i) {
            auto cond = lower(n.elifs[i].first);
            auto thenP = lowerBodyScoped(n.elifs[i].second);
            elsePart = matchBool(std::move(cond), std::move(thenP), std::move(elsePart));
        }
        auto cond = lower(n.condition);
        auto thenP = lowerBodyScoped(n.thenBody);
        return matchBool(std::move(cond), std::move(thenP), std::move(elsePart));
    }

    auto lowerMatch(const ast::MatchExpr& n) -> ExprPtr {
        if (n.subjectBinding) throw LowerError("IR lower: match |binder| not yet ported");
        Match m;
        m.subjects.push_back(lower(n.subject));
        for (const auto& cl : n.clauses) {
            auto snap = subst;
            MatchClause mc;
            for (const auto& p : cl.patterns) mc.patterns.push_back(lowerPattern(p));
            if (cl.guard) mc.guard = lower(*cl.guard);
            mc.body = lower(cl.body);
            subst = snap;
            m.clauses.push_back(std::move(mc));
        }
        auto e = std::make_unique<Expr>();
        e->node = std::move(m);
        return e;
    }

    // ---- Body lowering ----------------------------------------------------
    // A statement sequence. `let`/`var`/reassignments introduce SSA-renamed
    // bindings (updating `subst`); every other statement's value is bound to
    // a throwaway name; the last statement is the sequence's value.
    auto lowerBody(const std::vector<ast::ExprPtr>& body) -> ExprPtr {
        if (body.empty()) return lit(LitKind::Atom, "ok");
        return lowerBodyFrom(body, 0);
    }
    auto lowerBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i) -> ExprPtr {
        const auto& e = body[i];
        bool isLast = (i + 1 == body.size());

        // let PATTERN = value
        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                auto val = lower(le->value);
                std::string ssa = subst.count(vp->name) ? fresh(vp->name) : vp->name;
                subst[vp->name] = ssa;
                auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
                return makeLet(ssa, std::move(val), std::move(rest));
            }
            auto val = lower(le->value);
            auto pat = lowerPattern(le->pattern);
            auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
            return makeMatch1(std::move(val), std::move(pat), std::move(rest));
        }
        // var name = value  (a fresh mutable binding)
        if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
            auto val = lower(ve->value);
            std::string ssa = subst.count(ve->name) ? fresh(ve->name) : ve->name;
            subst[ve->name] = ssa;
            auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
            return makeLet(ssa, std::move(val), std::move(rest));
        }
        // name = value  (reassignment → fresh SSA name, value sees the OLD one)
        if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
            auto val = lower(ae->value);
            std::string ssa = fresh(ae->name);
            subst[ae->name] = ssa;
            auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
            return makeLet(ssa, std::move(val), std::move(rest));
        }

        if (isLast) return lower(e);
        auto val = lower(e);
        auto rest = lowerBodyFrom(body, i + 1);
        return makeLet(fresh("S"), std::move(val), std::move(rest));
    }

    // ---- Function / program ----------------------------------------------
    auto lowerFunction(const ast::FunctionDef& fn) -> FunDef {
        if (fn.clauses.size() != 1)
            throw LowerError("IR lower: multi-clause functions not yet ported");
        subst.clear(); // fresh scope; params resolve to themselves
        const auto& clause = fn.clauses[0];
        FunDef def;
        def.name = fn.name;
        def.arity = static_cast<int>(clause.params.size());
        FunClause fc;
        for (const auto& p : clause.params) {
            if (!p.name)
                throw LowerError("IR lower: non-simple parameter not yet ported");
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Var;
            pat->name = *p.name;
            fc.params.push_back(std::move(pat));
        }
        fc.body = lowerBody(clause.body);
        def.clauses.push_back(std::move(fc));
        return def;
    }
};

} // namespace

auto lowerProgram(const ast::Program& prog, const std::string& fileStem) -> Module {
    Lowering L;
    Module mod;
    mod.name = "kex_" + fileStem;

    for (const auto& item : prog.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) mod.functions.push_back(L.lowerFunction(*node));
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (node && (node->isExplicitMain || !node->synthetic)) {
                    if (!node->params.empty())
                        throw LowerError("IR lower: main(args) not yet ported");
                    L.subst.clear();
                    FunDef def;
                    def.name = "main";
                    def.arity = 0;
                    FunClause fc;
                    fc.body = L.lowerBody(node->body);
                    def.clauses.push_back(std::move(fc));
                    mod.functions.push_back(std::move(def));
                    mod.hasMain = true;
                    mod.mainArity = 0;
                }
            } else {
                throw LowerError(std::string("IR lower: unimplemented top-level item ")
                                 + typeid(T).name());
            }
        }, item);
    }
    return mod;
}

} // namespace kex::ir
