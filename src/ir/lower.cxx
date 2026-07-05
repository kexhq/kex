#include "lower.hxx"
#include "../lexer/token.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

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
    // Record layout, by record name: field names in declared order (tuple
    // position = index + 2, since element 1 is the 'Name' tag) and their
    // default-value exprs (nullptr = no default). Drives construction (fields
    // in declared order, defaults filled) and field access.
    struct RecordInfo {
        std::vector<std::string> fields;
        std::vector<const ast::ExprPtr*> defaults;
    };
    std::unordered_map<std::string, RecordInfo> records;
    // field name → [(record name, 1-based position)]. A field can live in
    // several records at (possibly different) positions; the emitted accessor
    // dispatches on the tag when they differ. Mirrors the string emitter.
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> fieldAccessors;
    // Names that resolve to a local function (top-level fn, make-block method,
    // or record field accessor). Only these may use the "UFCS → local apply,
    // receiver-first" fallback; any other `.method` is an unported builtin and
    // must error, not silently become a call to a nonexistent function.
    std::unordered_set<std::string> localMethods;
    // Cross-type method-name collisions: a method name → the make-block type
    // names that define it, in order. When more than one type defines the
    // same name, each type's version is emitted mangled (`name__Type`) and a
    // dispatcher under the bare name selects at runtime on the receiver's tag
    // (element 1). Mirrors the string emitter's collision handling.
    std::unordered_map<std::string, std::vector<std::string>> methodOwners;
    std::unordered_set<std::string> collidingMethods;
    // Operator symbols overloaded by a make block (`make T do let +(o) ...`).
    // The corresponding binary-op Intrinsic then dispatches at runtime: a
    // tuple (record) receiver → the user's `'+'/2`, otherwise the builtin.
    std::unordered_set<std::string> overloadedOps;
    // Top-level `let name = expr` bindings become 0-arity functions; a bare
    // reference to one compiles to `apply 'name'/0()`, not a variable.
    std::unordered_set<std::string> topLevelConstants;

    static auto opSymbol(TokenType t) -> std::string {
        switch (t) {
            case TokenType::Plus: return "+";     case TokenType::Minus: return "-";
            case TokenType::Star: return "*";     case TokenType::Slash: return "/";
            case TokenType::Percent: return "%";  case TokenType::EqEq: return "==";
            case TokenType::NotEq: return "!=";   case TokenType::LessThan: return "<";
            case TokenType::GreaterThan: return ">"; case TokenType::LessEq: return "<=";
            case TokenType::GreaterEq: return ">="; default: return "";
        }
    }

    static auto simpleTypeName(const ast::TypeExprPtr& t) -> std::string {
        if (t) if (auto* tn = std::get_if<ast::TypeName>(&t->kind))
            if (!tn->parts.empty()) return tn->parts.back();
        return "";
    }
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

    // Clone an ATOMIC expr (Var/Lit) — used when an operand must appear in
    // several positions (e.g. operator-overload dispatch). Only atomic nodes
    // are ever cloned, so this stays trivial.
    auto clone(const ExprPtr& e) -> ExprPtr {
        if (auto* v = std::get_if<Var>(&e->node)) return var(v->name);
        if (auto* l = std::get_if<Lit>(&e->node)) {
            auto out = std::make_unique<Expr>(); out->node = *l; return out;
        }
        throw LowerError("IR lower: internal — clone of non-atomic expr");
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
                    return lowerInterpolatedString(n.value);
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
                // A bare reference to a top-level `let` constant (not shadowed
                // by a local) is a call to its 0-arity function.
                if (!subst.count(n.name) && topLevelConstants.count(n.name)) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = Call{"", n.name, 0, {}, false};
                    return ex;
                }
                return var(currentName(n.name));
            } else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
                return var(currentName("this"));
            } else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
                // Bare capitalized name = nullary ADT constructor / None-like
                // tag → the atom 'Name' (e.g. JsonNull, Less).
                auto ex = std::make_unique<Expr>();
                ex->node = Construct{n.name, {}};
                return ex;
            } else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
                return lowerRecordConstruction(n);
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
                // Operator overloading: if this operator is defined by a make
                // block, dispatch on the LHS at runtime — a tuple (record)
                // uses the user's `'op'/2`, anything else the builtin op
                // (spec/operator_overloading.kex).
                std::string sym = opSymbol(n.op);
                if (!sym.empty() && overloadedOps.count(sym)) {
                    auto builtin = std::make_unique<Expr>();
                    std::vector<ExprPtr> ba; ba.push_back(clone(l)); ba.push_back(clone(r));
                    builtin->node = Intrinsic{opOf(n.op), std::move(ba)};
                    std::vector<ExprPtr> ua; ua.push_back(clone(l)); ua.push_back(clone(r));
                    auto userCall = std::make_unique<Expr>();
                    userCall->node = Call{"", sym, 2, std::move(ua), false};
                    auto dispatch = matchBool(
                        callE("erlang", "is_tuple", 1, one(clone(l))),
                        std::move(userCall), std::move(builtin));
                    return wrapLets(binds, std::move(dispatch));
                }
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
            } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                // `expr if cond` → cond ? expr : ok
                return matchBool(lower(n.condition), lower(n.expr), lit(LitKind::Atom, "ok"));
            } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                return matchBool(lower(n.condition), lower(n.thenExpr), lower(n.elseExpr));
            } else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
                // `&.method` / `&func` → fun(_sx) -> _sx.method(). Reusing the
                // UFCS method path means `&digit?` (a builtin) and `&myFn` (a
                // local) both resolve correctly with no special-casing.
                if (n.kind == ast::ShorthandLambda::Kind::MethodWithArgs && !n.args.empty())
                    throw LowerError("IR lower: &.method(args) not yet ported");
                std::string sx = fresh("Sx");
                auto recvAst = std::make_unique<ast::Expr>();
                recvAst->kind = ast::Identifier{sx};
                ast::MethodCall mc;
                mc.receiver = std::move(recvAst);
                mc.method = n.name;
                auto snap = subst; subst.erase(sx);
                auto body = lowerMethodCall(mc);
                subst = snap;
                Lambda lam; lam.params = {sx}; lam.body = std::move(body);
                auto ex = std::make_unique<Expr>(); ex->node = std::move(lam); return ex;
            } else if constexpr (std::is_same_v<T, ast::MapExpr>) {
                if (n.entries.empty()) return callE("maps", "new", 0, {});
                std::vector<Binding> binds;
                std::vector<ExprPtr> pairs;
                for (const auto& ent : n.entries) {
                    auto t = std::make_unique<Expr>();
                    t->node = MakeTuple{two(atomize(ent.key, binds), atomize(ent.value, binds))};
                    pairs.push_back(std::move(t));
                }
                auto lst = std::make_unique<Expr>();
                lst->node = MakeList{std::move(pairs), std::nullopt};
                return wrapLets(binds, callE("maps", "from_list", 1, one(std::move(lst))));
            } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                // Params shadow outer bindings: resolve to themselves inside.
                auto snap = subst;
                Lambda lam;
                for (const auto& p : n.params) { lam.params.push_back(p.name); subst.erase(p.name); }
                lam.body = lowerBody(n.body);
                subst = snap;
                auto ex = std::make_unique<Expr>();
                ex->node = std::move(lam);
                return ex;
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
        // Capitalized name = ADT constructor with a payload → tagged tuple.
        if (!n.name.empty() && std::isupper(static_cast<unsigned char>(n.name[0])))
            ex->node = Construct{n.name, std::move(args)};
        else
            ex->node = Call{"", n.name, static_cast<int>(n.args.size()), std::move(args), false};
        return wrapLets(binds, std::move(ex));
    }

    // Minimal UFCS/stdlib resolution for the walking skeleton. Only the
    // handful of forms an early target program needs; everything else errors
    // (to be widened as constructs are ported from core_erlang.cxx).
    auto lowerMethodCall(const ast::MethodCall& n) -> ExprPtr {
        // A mutating `!` call rebinds its receiver — that's an assignment, not
        // a plain method call, and belongs to the (deferred) mutation IR
        // pass. Erroring here keeps the invariant that anything the IR path
        // compiles is CORRECT (spec/mutating_calls.kex).
        if (n.mutating)
            throw LowerError("IR lower: mutating '!' call not yet ported");
        // Namespace calls on an UpperIdentifier receiver, e.g. IO.printLine.
        if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.receiver->kind)) {
            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            auto nsCall = [&](const char* mod, const char* fn) {
                auto ex = std::make_unique<Expr>();
                ex->node = Call{mod, fn, static_cast<int>(args.size()), std::move(args), false};
                return wrapLets(binds, std::move(ex));
            };
            if (uid->name == "IO") {
                if (n.method == "printLine") return nsCall("kex_io", "print_line");
                if (n.method == "print")     return nsCall("kex_io", "print");
                if (n.method == "printError") return nsCall("kex_io", "print_error");
                throw LowerError("IR lower: IO." + n.method + " not yet ported");
            }
            if (uid->name == "Integer" && n.method == "parse") return nsCall("kex_io", "integer_parse");
            if (uid->name == "Float" && n.method == "parse")   return nsCall("kex_io", "float_parse");
            if (uid->name == "Math") {
                if (n.method == "sqrt") return nsCall("math", "sqrt");
                if (n.method == "pow" || n.method == "power") return nsCall("math", "pow");
                if (n.method == "sin") return nsCall("math", "sin");
                if (n.method == "cos") return nsCall("math", "cos");
                if (n.method == "log") return nsCall("kex_io", "math_log");
                if (n.method == "abs") return nsCall("erlang", "abs");
                if (n.method == "floor") return nsCall("erlang", "floor");
                if (n.method == "ceil") return nsCall("erlang", "ceil");
            }
            throw LowerError("IR lower: namespace call " + uid->name + "." + n.method
                             + " not yet ported");
        }
        // UFCS method on a value receiver. Atomize the receiver once so
        // methods that use it several times (min/get/count/…) don't
        // re-evaluate it; `ret` wraps the result in that binding.
        std::vector<Binding> rb;
        auto rv = atomize_ir(lower(n.receiver), rb);
        auto& m = n.method;
        auto ret = [&](ExprPtr e) { return wrapLets(rb, std::move(e)); };
        auto arg0 = [&]() { return lower(n.args[0]); };

        // `case rv of [] -> empty; _ -> nonEmpty end` (Just/None-on-empty).
        auto onEmpty = [&](ExprPtr empty, ExprPtr nonEmpty) -> ExprPtr {
            Match mm; mm.subjects.push_back(clone(rv));
            MatchClause e; auto ep = std::make_unique<Pattern>();
            ep->kind = PatKind::List; e.patterns.push_back(std::move(ep)); e.body = std::move(empty);
            MatchClause ne; auto np = std::make_unique<Pattern>();
            np->kind = PatKind::Wild; ne.patterns.push_back(std::move(np)); ne.body = std::move(nonEmpty);
            mm.clauses.push_back(std::move(e)); mm.clauses.push_back(std::move(ne));
            auto x = std::make_unique<Expr>(); x->node = std::move(mm); return x;
        };
        auto justOf = [&](ExprPtr v) {
            std::vector<ExprPtr> a; a.push_back(std::move(v));
            auto x = std::make_unique<Expr>(); x->node = Construct{"Just", std::move(a)}; return x;
        };

        // No-/one-arg builtins with a single runtime call (receiver first).
        struct One { const char* method; const char* mod; const char* fn; int nargs; };
        static const One calls[] = {
            {"product","kex_io","list_product",0}, {"sum","lists","sum",0},
            {"reverse","lists","reverse",0}, {"sort","lists","sort",0},
            {"uniq","lists","usort",0}, {"unique","lists","usort",0},
            {"abs","erlang","abs",0}, {"sqrt","math","sqrt",0},
            {"upperCase","string","to_upper",0}, {"upcase","string","to_upper",0},
            {"lowerCase","string","to_lower",0}, {"downcase","string","to_lower",0},
            {"trim","string","trim",0}, {"at","kex_io","list_get",1},
            {"digit?","kex_io","is_digit",0}, {"alpha?","kex_io","is_alpha",0},
            {"space?","kex_io","is_space",0},
        };
        for (const auto& b : calls)
            if (m == b.method && (int)n.args.size() == b.nargs && !n.block) {
                std::vector<ExprPtr> a; a.push_back(clone(rv));
                if (b.nargs == 1) a.push_back(arg0());
                return ret(callE(b.mod, b.fn, b.nargs + 1, std::move(a)));
            }

        if (m == "modulo" && n.args.size() == 1)
            return ret(callE("erlang","rem",2,two(clone(rv), arg0())));
        if (m == "even?" && n.args.empty())
            return ret(intrin(Op::Eq, two(callE("erlang","rem",2,two(clone(rv),litInt(2))), litInt(0))));
        if (m == "odd?" && n.args.empty())
            return ret(intrin(Op::Neq, two(callE("erlang","rem",2,two(clone(rv),litInt(2))), litInt(0))));
        if (m == "ok?" && n.args.empty())
            return ret(intrin(Op::Eq, two(callE("erlang","element",2,two(litInt(1),clone(rv))), lit(LitKind::Atom,"Ok"))));
        if (m == "error?" && n.args.empty())
            return ret(intrin(Op::Eq, two(callE("erlang","element",2,two(litInt(1),clone(rv))), lit(LitKind::Atom,"Error"))));
        if (m == "push" && n.args.size() == 1) {
            auto lst = std::make_unique<Expr>();
            lst->node = MakeList{one(arg0()), std::nullopt};
            return ret(callE("erlang","++",2,two(clone(rv), std::move(lst))));
        }
        if (m == "in?" && n.args.size() == 1)
            return ret(callE("lists","member",2,two(clone(rv), arg0())));
        if ((m == "count" || m == "length" || m == "size") && n.args.empty() && !n.block)
            return ret(callE("erlang","length",1,one(clone(rv))));
        if (m == "min" && n.args.empty())
            return ret(onEmpty(lit(LitKind::None,"none"), justOf(callE("lists","min",1,one(clone(rv))))));
        if (m == "max" && n.args.empty())
            return ret(onEmpty(lit(LitKind::None,"none"), justOf(callE("lists","max",1,one(clone(rv))))));
        if (m == "first" && n.args.empty() && !localMethods.count("first"))
            return ret(onEmpty(lit(LitKind::None,"none"), justOf(callE("erlang","hd",1,one(clone(rv))))));
        if (m == "last" && n.args.empty() && !localMethods.count("last"))
            return ret(onEmpty(lit(LitKind::None,"none"), justOf(callE("lists","last",1,one(clone(rv))))));
        // .to(Type) numeric/string conversion (unless a user `to` method).
        if (m == "to" && n.args.size() == 1 && !localMethods.count("to")) {
            std::string ty;
            if (auto* ui = std::get_if<ast::UpperIdentifier>(&n.args[0]->kind)) ty = ui->name;
            if (ty == "String") return ret(callE("kex_io","to_string",1,one(clone(rv))));
            if (ty == "Int" || ty == "Integer") return ret(callE("kex_io","to_integer",1,one(clone(rv))));
            if (ty == "Float") return ret(callE("kex_io","to_float",1,one(clone(rv))));
        }
        // empty?: works for both maps and lists (size 0 / []).
        if (m == "empty?" && n.args.empty() && !localMethods.count("empty?"))
            return ret(matchBool(callE("erlang","is_map",1,one(clone(rv))),
                intrin(Op::Eq, two(callE("maps","size",1,one(clone(rv))), litInt(0))),
                intrin(Op::Eq, two(clone(rv), [&]{ auto e=std::make_unique<Expr>(); e->node=MakeList{{},std::nullopt}; return e; }()))));
        // .or(default): unwrap Just/Some/Ok, else the default.
        if (m == "or" && n.args.size() == 1) {
            auto dflt = arg0();
            Match mm; mm.subjects.push_back(clone(rv));
            auto ctorPat = [&](const char* tag) {
                auto p = std::make_unique<Pattern>(); p->kind = PatKind::Construct; p->tag = tag;
                auto vp = std::make_unique<Pattern>(); vp->kind = PatKind::Var; vp->name = "_v";
                p->args.push_back(std::move(vp)); return p;
            };
            for (const char* tag : {"Just", "Some", "Ok"}) {
                MatchClause c; c.patterns.push_back(ctorPat(tag)); c.body = var("_v");
                mm.clauses.push_back(std::move(c));
            }
            MatchClause d; auto wp = std::make_unique<Pattern>(); wp->kind = PatKind::Wild;
            d.patterns.push_back(std::move(wp)); d.body = std::move(dflt);
            mm.clauses.push_back(std::move(d));
            auto x = std::make_unique<Expr>(); x->node = std::move(mm);
            return ret(std::move(x));
        }
        // list[i] / list.get(i): list → raw elem-or-none; map → Just/None.
        if (m == "get" && n.args.size() == 1 && !localMethods.count("get")) {
            auto idx = arg0();
            Match inner; inner.subjects.push_back(callE("maps","find",2,two(clone(idx), clone(rv))));
            MatchClause ok; auto okp = std::make_unique<Pattern>(); okp->kind = PatKind::Tuple;
            auto okt = std::make_unique<Pattern>(); okt->kind = PatKind::Lit; okt->litKind = LitKind::Atom; okt->litText = "ok";
            auto okv = std::make_unique<Pattern>(); okv->kind = PatKind::Var; okv->name = "_gv";
            okp->args.push_back(std::move(okt)); okp->args.push_back(std::move(okv));
            ok.patterns.push_back(std::move(okp)); ok.body = justOf(var("_gv"));
            MatchClause er; auto erp = std::make_unique<Pattern>(); erp->kind = PatKind::Lit; erp->litKind = LitKind::Atom; erp->litText = "error";
            er.patterns.push_back(std::move(erp)); er.body = lit(LitKind::None, "none");
            inner.clauses.push_back(std::move(ok)); inner.clauses.push_back(std::move(er));
            auto innerE = std::make_unique<Expr>(); innerE->node = std::move(inner);
            return ret(matchBool(callE("erlang","is_list",1,one(clone(rv))),
                callE("kex_io","list_get",2,two(clone(rv), clone(idx))),
                std::move(innerE)));
        }

        // Block/higher-order list methods. The block lowers to a Lambda,
        // which — being non-atomic — atomize() naturally let-binds (Core
        // Erlang requires funs to be bound before being passed as call args).
        const ast::ExprPtr* blk = n.block ? &*n.block
                                : (!n.args.empty() ? &n.args.back() : nullptr);
        if (blk) {
            std::vector<Binding> binds;
            auto fn = atomize(*blk, binds);
            // rb binds the receiver (outer), binds the block fn (inner).
            auto build = [&](ExprPtr call) { return wrapLets(rb, wrapLets(binds, std::move(call))); };
            if (m == "each")
                return build(callE("lists", "foreach", 2, two(std::move(fn), clone(rv))));
            if (m == "map")
                return build(callE("lists", "map", 2, two(std::move(fn), clone(rv))));
            if (m == "filter" || m == "select")
                return build(callE("lists", "filter", 2, two(std::move(fn), clone(rv))));
            if (m == "all?")
                return build(callE("lists", "all", 2, two(std::move(fn), clone(rv))));
            if (m == "any?" || m == "some?")
                return build(callE("lists", "any", 2, two(std::move(fn), clone(rv))));
            // reduce(seed) { |acc, x| } → lists:foldl(fun(x,acc)->fn(acc,x), seed, recv)
            if ((m == "reduce" || m == "inject") && n.args.size() == 1 && n.block) {
                auto seed = atomize(n.args[0], binds);
                // Kex block is (acc, elem); foldl passes (elem, acc) → swap.
                Lambda swap;
                swap.params = {"_e", "_a"};
                swap.body = callE_indirect(clone(fn), two(var("_a"), var("_e")));
                auto swapFn = std::make_unique<Expr>(); swapFn->node = std::move(swap);
                auto swapVar = atomize_ir(std::move(swapFn), binds);
                return build(callE("lists", "foldl", 3,
                    three(std::move(swapVar), std::move(seed), clone(rv))));
            }
        }

        // Generic UFCS fallback: a field access or a make-block method are
        // BOTH emitted as local functions taking the receiver first, so
        // `x.foo(a, b)` → `apply 'foo'/3(x, a, b)`. This is exactly how the
        // string emitter resolves record field access and make-block methods
        // without needing types. Gated on localMethods so an UNPORTED builtin
        // (e.g. `.get`/`.map`) errors loudly instead of silently becoming a
        // call to a function that doesn't exist.
        if (localMethods.count(n.method) && !n.block && n.namedArgs.empty()) {
            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            args.push_back(clone(rv));
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            int arity = static_cast<int>(n.args.size()) + 1;
            auto ex = std::make_unique<Expr>();
            ex->node = Call{"", n.method, arity, std::move(args), false};
            return ret(wrapLets(binds, std::move(ex)));
        }
        throw LowerError("IR lower: UFCS method ." + n.method + " (block/named args) not yet ported");
    }

    // record TypeName { f: v, ... } → {'TypeName', <fields in declared order,
    // defaults filled for omitted ones>}. Field ORDER and defaults come from
    // the record definition (the accessors read fixed positions).
    auto lowerRecordConstruction(const ast::RecordConstruction& n) -> ExprPtr {
        std::vector<Binding> binds;
        std::vector<ExprPtr> args;
        auto it = records.find(n.typeName);
        if (it == records.end()) {
            // Unknown record — fall back to fields as written.
            for (const auto& [name, v] : n.fields) args.push_back(atomize(v, binds));
        } else {
            const auto& info = it->second;
            for (size_t i = 0; i < info.fields.size(); i++) {
                const ast::ExprPtr* provided = nullptr;
                for (const auto& [name, v] : n.fields)
                    if (name == info.fields[i]) { provided = &v; break; }
                if (provided) args.push_back(atomize(*provided, binds));
                else if (info.defaults[i]) args.push_back(atomize(*info.defaults[i], binds));
                else args.push_back(lit(LitKind::None, "none"));
            }
        }
        auto ex = std::make_unique<Expr>();
        ex->node = Construct{n.typeName, std::move(args)};
        return wrapLets(binds, std::move(ex));
    }

    // "text ${expr} more" → a ++ chain of literal segments and to_string'd
    // sub-expressions. The `${...}` sub-expressions are raw text in the AST,
    // so they're re-lexed/parsed here and lowered like any other expression
    // (matching CoreErlangEmitter::emitInterpolatedString).
    auto lowerInterpolatedString(const std::string& raw) -> ExprPtr {
        std::vector<ExprPtr> parts;
        size_t pos = 0;
        while (pos < raw.size()) {
            auto dollar = raw.find("${", pos);
            if (dollar == std::string::npos) {
                if (pos < raw.size()) parts.push_back(lit(LitKind::String, raw.substr(pos)));
                break;
            }
            if (dollar > pos) parts.push_back(lit(LitKind::String, raw.substr(pos, dollar - pos)));
            size_t close = std::string::npos;
            int depth = 1;
            for (size_t k = dollar + 2; k < raw.size(); k++) {
                if (raw[k] == '{') depth++;
                else if (raw[k] == '}' && --depth == 0) { close = k; break; }
            }
            if (close == std::string::npos) break; // malformed → stop
            std::string inner = raw.substr(dollar + 2, close - dollar - 2);
            kex::Lexer lx(inner);
            kex::Parser ps(lx.tokenizeAll());
            auto innerAst = ps.parseExpr();
            if (innerAst)
                parts.push_back(callE("kex_io", "to_string", 1, one(lower(innerAst))));
            pos = close + 1;
        }
        if (parts.empty()) return lit(LitKind::String, "");
        ExprPtr result = std::move(parts[0]);
        for (size_t i = 1; i < parts.size(); i++)
            result = intrin(Op::Concat, two(std::move(result), std::move(parts[i])));
        return result;
    }

    // ---- IR construction helpers -----------------------------------------
    auto litInt(long v) -> ExprPtr { return lit(LitKind::Int, std::to_string(v)); }
    auto one(ExprPtr a) -> std::vector<ExprPtr> {
        std::vector<ExprPtr> v; v.push_back(std::move(a)); return v;
    }
    auto two(ExprPtr a, ExprPtr b) -> std::vector<ExprPtr> {
        std::vector<ExprPtr> v; v.push_back(std::move(a)); v.push_back(std::move(b)); return v;
    }
    auto three(ExprPtr a, ExprPtr b, ExprPtr c) -> std::vector<ExprPtr> {
        std::vector<ExprPtr> v;
        v.push_back(std::move(a)); v.push_back(std::move(b)); v.push_back(std::move(c));
        return v;
    }
    auto callE_indirect(ExprPtr callee, std::vector<ExprPtr> args) -> ExprPtr {
        auto e = std::make_unique<Expr>();
        e->node = CallIndirect{std::move(callee), std::move(args), false};
        return e;
    }
    // Atomize an already-lowered IR expr: bind to a fresh Let if it isn't a
    // Var/Lit, recording the binding.
    auto atomize_ir(ExprPtr e, std::vector<Binding>& binds) -> ExprPtr {
        if (std::holds_alternative<Var>(e->node) || std::holds_alternative<Lit>(e->node))
            return e;
        auto name = fresh();
        binds.push_back({name, std::move(e)});
        return var(name);
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
    // A single param → an IR pattern (var name or a destructuring pattern).
    auto lowerParam(const ast::Param& p) -> PatternPtr {
        if (p.name) {
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Var; pat->name = *p.name;
            return pat;
        }
        if (p.pattern) return lowerPattern(*p.pattern);
        auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w;
    }

    // Lower a group of same-name FunctionDefs (Kex writes multi-clause
    // functions as separate `let` declarations) into one multi-clause FunDef.
    // implicitThisName: when non-empty, a receiver param of that name is
    // prepended to every clause (make-block methods: `this`).
    auto lowerFunctionGroup(const std::vector<const ast::FunctionDef*>& group,
                            const std::string& implicitThisName = "") -> FunDef {
        FunDef def;
        def.name = group[0]->name;
        int explicitArity = group[0]->clauses.empty()
            ? 0 : static_cast<int>(group[0]->clauses[0].params.size());
        def.arity = explicitArity + (implicitThisName.empty() ? 0 : 1);
        for (const auto* fn : group) {
            for (const auto& clause : fn->clauses) {
                subst.clear(); // fresh scope per clause
                FunClause fc;
                if (!implicitThisName.empty()) {
                    auto pat = std::make_unique<Pattern>();
                    pat->kind = PatKind::Var; pat->name = implicitThisName;
                    fc.params.push_back(std::move(pat));
                }
                for (const auto& p : clause.params) fc.params.push_back(lowerParam(p));
                fc.body = lowerBody(clause.body);
                def.clauses.push_back(std::move(fc));
            }
        }
        return def;
    }
    auto lowerFunction(const ast::FunctionDef& fn, const std::string& implicitThisName = "")
        -> FunDef {
        return lowerFunctionGroup({&fn}, implicitThisName);
    }

    // ---- Records ----------------------------------------------------------
    void collectRecord(const ast::RecordDef& rec) {
        RecordInfo info;
        for (int i = 0; i < static_cast<int>(rec.fields.size()); i++) {
            info.fields.push_back(rec.fields[i].name);
            info.defaults.push_back(rec.fields[i].defaultValue ? &*rec.fields[i].defaultValue
                                                               : nullptr);
            fieldAccessors[rec.fields[i].name].push_back({rec.name, i + 2});
        }
        records[rec.name] = std::move(info);
    }

    // Emit a `'field'/1` accessor for each record field, unless a real
    // function/method of the same name exists (which shadows it). When a
    // field sits at the same position in every record that has it, one
    // element/2 call suffices; otherwise dispatch on the record tag.
    auto makeAccessors(const std::unordered_set<std::string>& definedFns) -> std::vector<FunDef> {
        std::vector<FunDef> out;
        for (const auto& [field, entries] : fieldAccessors) {
            if (definedFns.count(field)) continue;
            FunDef def; def.name = field; def.arity = 1;
            FunClause fc;
            auto rp = std::make_unique<Pattern>(); rp->kind = PatKind::Var; rp->name = "_rec";
            fc.params.push_back(std::move(rp));
            bool allSame = std::all_of(entries.begin(), entries.end(),
                [&](const auto& e){ return e.second == entries[0].second; });
            if (allSame) {
                fc.body = callE("erlang", "element", 2, two(litInt(entries[0].second), var("_rec")));
            } else {
                Match m;
                m.subjects.push_back(callE("erlang", "element", 2, two(litInt(1), var("_rec"))));
                for (const auto& [recName, pos] : entries) {
                    MatchClause mc;
                    auto p = std::make_unique<Pattern>();
                    p->kind = PatKind::Lit; p->litKind = LitKind::Atom; p->litText = recName;
                    mc.patterns.push_back(std::move(p));
                    mc.body = callE("erlang", "element", 2, two(litInt(pos), var("_rec")));
                    m.clauses.push_back(std::move(mc));
                }
                auto e = std::make_unique<Expr>(); e->node = std::move(m);
                fc.body = std::move(e);
            }
            def.clauses.push_back(std::move(fc));
            out.push_back(std::move(def));
        }
        return out;
    }

    // ---- Make blocks ------------------------------------------------------
    // Lower one make-block method to a FunDef. First cut: implicit-`this`
    // methods with named/simple params (the common `@field`/`this.method`
    // shape). Static constructors and receiver-pattern methods are deferred.
    // Lower a group of same-name make-block methods (multi-clause) for one
    // type. A method whose first param is an unnamed pattern (`@Less`,
    // `{input, pos}`, `@[x|_]`) matches the RECEIVER directly — no implicit
    // `this`. Otherwise `this` is prepended.
    auto lowerMakeGroup(const std::vector<const ast::FunctionDef*>& group,
                        const std::string& typeName) -> FunDef {
        const auto& first = *group[0];
        bool isStaticCtor = !first.name.empty()
                          && std::isupper(static_cast<unsigned char>(first.name[0]));
        if (isStaticCtor)
            throw LowerError("IR lower: static constructor '" + first.name + "' not yet ported");
        // A receiver pattern is specifically the `@` sigil (ThisPattern), a
        // record destructure (RecordPattern), or a range (RangePattern) —
        // NOT any pattern. A bare type-name param like `to(String)` is a
        // ConstructorPattern *value* argument, and such a method still uses
        // its implicit `this` (spec/type_dispatch.kex's `to(String)` body is
        // `"(${@x}, ${@y})"`).
        bool receiverPattern = false;
        if (!first.clauses.empty() && !first.clauses[0].params.empty()) {
            const auto& p0 = first.clauses[0].params[0];
            if (!p0.name && p0.pattern)
                receiverPattern =
                    std::holds_alternative<ast::ThisPattern>((*p0.pattern)->kind) ||
                    std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind) ||
                    std::holds_alternative<ast::RangePattern>((*p0.pattern)->kind);
        }
        auto def = lowerFunctionGroup(group, receiverPattern ? "" : "this");
        if (collidingMethods.count(first.name) && !typeName.empty())
            def.name = first.name + "__" + typeName;
        return def;
    }

    // A dispatcher `name/arity` for a colliding method: inspect the
    // receiver's tag (element 1 of arg 0) and forward to `name__Type`.
    auto makeDispatcher(const std::string& name, int arity) -> FunDef {
        FunDef def; def.name = name; def.arity = arity;
        FunClause fc;
        std::vector<ExprPtr> fwdArgs;
        for (int i = 0; i < arity; i++) {
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Var; pat->name = "_a" + std::to_string(i);
            fc.params.push_back(std::move(pat));
            fwdArgs.push_back(var("_a" + std::to_string(i)));
        }
        Match m;
        m.subjects.push_back(callE("erlang", "element", 2, two(litInt(1), var("_a0"))));
        for (const auto& ty : methodOwners[name]) {
            MatchClause mc;
            auto p = std::make_unique<Pattern>();
            p->kind = PatKind::Lit; p->litKind = LitKind::Atom; p->litText = ty;
            mc.patterns.push_back(std::move(p));
            std::vector<ExprPtr> args;
            for (int i = 0; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
            auto call = std::make_unique<Expr>();
            call->node = Call{"", name + "__" + ty, arity, std::move(args), false};
            mc.body = std::move(call);
            m.clauses.push_back(std::move(mc));
        }
        auto body = std::make_unique<Expr>();
        body->node = std::move(m);
        fc.body = std::move(body);
        def.clauses.push_back(std::move(fc));
        return def;
    }
};

} // namespace

auto lowerProgram(const ast::Program& prog, const std::string& fileStem) -> Module {
    Lowering L;
    Module mod;
    mod.name = "kex_" + fileStem;

    // Pre-pass: collect record layouts (needed for construction/field access
    // before any body is lowered) and the set of real function/method names
    // (so field accessors that would collide with them are suppressed).
    std::unordered_set<std::string> definedFns;
    for (const auto& item : prog.items) {
        if (auto* rd = std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
            if (*rd) L.collectRecord(**rd);
        } else if (auto* mb = std::get_if<std::unique_ptr<ast::MainBlock>>(&item)) {
            if (*mb && (*mb)->synthetic)
                for (const auto& e : (*mb)->body)
                    if (auto* le = std::get_if<ast::LetExpr>(&e->kind))
                        if (le->pattern)
                            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind))
                                L.topLevelConstants.insert(vp->name);
        } else if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fd) definedFns.insert((*fd)->name);
        } else if (auto* md = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
            if (!*md) continue;
            std::string typeName = Lowering::simpleTypeName((*md)->target);
            for (const auto& bi : (*md)->body)
                if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi))
                    if (*mfd) {
                        definedFns.insert((*mfd)->name);
                        // Operator-symbol method name → an overloaded operator.
                        const std::string& mn = (*mfd)->name;
                        if (!mn.empty() && !std::isalnum(static_cast<unsigned char>(mn[0])) && mn[0] != '_')
                            L.overloadedOps.insert(mn);
                        if (!typeName.empty()) {
                            auto& owners = L.methodOwners[(*mfd)->name];
                            if (std::find(owners.begin(), owners.end(), typeName) == owners.end())
                                owners.push_back(typeName);
                        }
                    }
        }
    }
    for (const auto& [name, owners] : L.methodOwners)
        if (owners.size() > 1) L.collidingMethods.insert(name);
    // A `.method` may use the local-apply UFCS fallback iff it names a real
    // local function or a record field accessor.
    L.localMethods = definedFns;
    for (const auto& [field, entries] : L.fieldAccessors) { (void)entries; L.localMethods.insert(field); }

    // Buffer consecutive same-name top-level functions so they group into one
    // multi-clause FunDef (flushed on any other item / name change / end).
    std::vector<const ast::FunctionDef*> fnGroup;
    auto flushGroup = [&]() {
        if (!fnGroup.empty()) { mod.functions.push_back(L.lowerFunctionGroup(fnGroup)); fnGroup.clear(); }
    };
    // Bare top-level expressions (no explicit `main`) → one synthetic main/0.
    std::vector<const ast::ExprPtr*> bareExprs;

    for (const auto& item : prog.items) {
        if (auto* fdp = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item); fdp && *fdp) {
            if (!fnGroup.empty() && fnGroup.front()->name != (*fdp)->name) flushGroup();
            fnGroup.push_back(fdp->get());
            continue;
        }
        flushGroup();
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                (void)node; // handled above
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                // Layout already collected; accessors emitted after the loop.
                // A record with a `static do` block isn't ported yet.
                if (node && node->staticBlock)
                    throw LowerError("IR lower: record static block not yet ported");
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (!node) return;
                if (!node->implements.empty())
                    throw LowerError("IR lower: make ... implement (traits) not yet ported");
                std::string typeName = Lowering::simpleTypeName(node->target);
                std::vector<const ast::FunctionDef*> mgrp;
                auto flushM = [&]{ if (!mgrp.empty()) { mod.functions.push_back(L.lowerMakeGroup(mgrp, typeName)); mgrp.clear(); } };
                for (const auto& bi : node->body) {
                    if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi)) {
                        if (*mfd) {
                            if (!mgrp.empty() && mgrp.front()->name != (*mfd)->name) flushM();
                            mgrp.push_back(mfd->get());
                        }
                    } else {
                        flushM();
                        throw LowerError("IR lower: make-block item (visibility/annotation) "
                                         "not yet ported");
                    }
                }
                flushM();
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (!node) return;
                if (node->synthetic) {
                    // Top-level `let name = expr` → a 0-arity function.
                    for (const auto& e : node->body) {
                        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
                            if (le->pattern) if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                                L.subst.clear();
                                FunDef def; def.name = vp->name; def.arity = 0;
                                FunClause fc; fc.body = L.lower(le->value);
                                def.clauses.push_back(std::move(fc));
                                mod.functions.push_back(std::move(def));
                            }
                        }
                    }
                } else if (node->isExplicitMain) {
                    L.subst.clear();
                    FunDef def; def.name = "main";
                    ExprPtr body = L.lowerBody(node->body);
                    FunClause fc;
                    if (node->params.empty()) {
                        def.arity = 0; mod.mainArity = 0;
                    } else {
                        // main(args) / main(args, env): param 0 is the args
                        // list (from init:get_plain_arguments); a second param
                        // is the ENV map, bound in the body.
                        def.arity = 1; mod.mainArity = 1;
                        if (node->params.size() >= 2 && node->params[1].name)
                            body = L.makeLet(*node->params[1].name,
                                             L.callE("kex_io", "env_map", 0, {}), std::move(body));
                        auto p = std::make_unique<Pattern>();
                        p->kind = PatKind::Var;
                        p->name = node->params[0].name ? *node->params[0].name : "_args";
                        fc.params.push_back(std::move(p));
                    }
                    fc.body = std::move(body);
                    def.clauses.push_back(std::move(fc));
                    mod.functions.push_back(std::move(def));
                    mod.hasMain = true;
                } else {
                    // Bare top-level expression(s) → accumulate for a synthetic main.
                    for (const auto& e : node->body) bareExprs.push_back(&e);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>> ||
                                 std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
                // Types/annotations are erased — nothing to emit.
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                // A nested module flattens its functions into this BEAM module
                // (Kex modules are organizational namespaces — see
                // beam-codegen-plan.md). First cut: direct FunctionDefs only.
                if (node) {
                    std::vector<const ast::FunctionDef*> grp;
                    auto flush = [&]{ if (!grp.empty()) { mod.functions.push_back(L.lowerFunctionGroup(grp)); grp.clear(); } };
                    for (const auto& bi : node->body) {
                        if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi)) {
                            if (*mfd) {
                                if (!grp.empty() && grp.front()->name != (*mfd)->name) flush();
                                grp.push_back(mfd->get());
                            }
                        } else { flush(); throw LowerError("IR lower: nested module non-function item not yet ported"); }
                    }
                    flush();
                }
            } else {
                throw LowerError(std::string("IR lower: unimplemented top-level item ")
                                 + typeid(T).name());
            }
        }, item);
    }
    flushGroup();

    // Synthetic main/0 from bare top-level expressions, if no explicit main.
    if (!bareExprs.empty() && !mod.hasMain) {
        L.subst.clear();
        FunDef def; def.name = "main"; def.arity = 0;
        FunClause fc;
        // Emit each expr as a statement; last is the value.
        std::function<ExprPtr(size_t)> chain = [&](size_t i) -> ExprPtr {
            if (i + 1 == bareExprs.size()) return L.lower(*bareExprs[i]);
            auto val = L.lower(*bareExprs[i]);
            auto rest = chain(i + 1);
            auto let = std::make_unique<Expr>();
            let->node = Let{L.fresh("S"), std::move(val), std::move(rest)};
            return let;
        };
        fc.body = bareExprs.empty() ? lit(LitKind::Atom, "ok") : chain(0);
        def.clauses.push_back(std::move(fc));
        mod.functions.push_back(std::move(def));
        mod.hasMain = true; mod.mainArity = 0;
    }

    // Cross-type collision dispatchers (bare name → tag dispatch). Arity is
    // taken from any emitted `name__Type` variant.
    for (const auto& name : L.collidingMethods) {
        int arity = -1;
        for (const auto& fn : mod.functions)
            if (fn.name.rfind(name + "__", 0) == 0) { arity = fn.arity; break; }
        if (arity >= 1) mod.functions.push_back(L.makeDispatcher(name, arity));
    }

    // Field accessors last (so definedFns is fully known).
    for (auto& acc : L.makeAccessors(definedFns)) mod.functions.push_back(std::move(acc));
    return mod;
}

} // namespace kex::ir
