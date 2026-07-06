#include "lower.hxx"
#include "../lexer/token.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
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
    // Make-block methods emitted WITH an implicit `this` receiver. A static
    // `Type.method(args)` call to one must pass a placeholder receiver so the
    // arities line up (the method builds its own value from the args and never
    // touches `this` — see examples/json_parser.kex's `Parser.parse(input)`).
    std::unordered_set<std::string> implicitThisMethods;
    // Record/make type names (so a namespace call can be recognized as a
    // static method dispatch, not an unknown module).
    std::unordered_set<std::string> knownTypes;
    // Top-level `let name = expr` bindings become 0-arity functions; a bare
    // reference to one compiles to `apply 'name'/0()`, not a variable.
    std::unordered_set<std::string> topLevelConstants;
    // Stdlib functions provided by the shared kex_prelude module; a UFCS call
    // to one that isn't a local method routes to `kex_prelude:<fn>`.
    std::unordered_set<std::string> preludeFns;
    // Top-level zero-parameter functions (e.g. `foul name do ... end`). A bare
    // reference to one (no parens) is a call `apply 'name'/0()`, not a var.
    std::unordered_set<std::string> zeroArgFns;
    // Top-level free function → its ordered parameter names ("" for an
    // unnamed/pattern param). Lets a call with named args reorder them into
    // positional slots.
    std::unordered_map<std::string, std::vector<std::string>> fnParamNames;
    // Names that are real functions in this module (top-level + make methods).
    // A call to a name NOT in here is an indirect apply through a variable
    // holding a fun (e.g. a `block` parameter).
    std::unordered_set<std::string> knownFns;

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
        if (!t) return "";
        if (auto* tn = std::get_if<ast::TypeName>(&t->kind))
            if (!tn->parts.empty()) return tn->parts.back();
        if (auto* g = std::get_if<ast::GenericType>(&t->kind))
            if (!g->name.parts.empty()) return g->name.parts.back();
        // List/Map types get canonical names so list-vs-map methods (the
        // polymorphic HOFs) collide and dispatch by runtime type (is_list/
        // is_map) instead of falling into separate un-mangled `map/2`s.
        if (std::holds_alternative<ast::ListType>(t->kind)) return "List";
        if (std::holds_alternative<ast::MapType>(t->kind)) return "Map";
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
                if (!subst.count(n.name) &&
                    (topLevelConstants.count(n.name) || zeroArgFns.count(n.name))) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = Call{"", n.name, 0, {}, false};
                    return ex;
                }
                return var(currentName(n.name));
            } else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
                return var(currentName("this"));
            } else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
                // An all-caps top-level constant (e.g. DEFAULT_LEVEL, defined
                // via `let NAME = value` → a 0-arity function) is a call, not a
                // nullary ADT constructor.
                if (!subst.count(n.name) &&
                    (topLevelConstants.count(n.name) || zeroArgFns.count(n.name))) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = Call{"", n.name, 0, {}, false};
                    return ex;
                }
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
                // In a guard, `case` is illegal, so use the strict and/or
                // guard BIFs instead of the short-circuit Match.
                if (n.op == TokenType::AmpAmp) {
                    if (m_inGuard) return callE("erlang","and",2,two(lower(n.left), lower(n.right)));
                    return matchBool(lower(n.left), lower(n.right), litBool(false));
                }
                if (n.op == TokenType::PipePipe) {
                    if (m_inGuard) return callE("erlang","or",2,two(lower(n.left), lower(n.right)));
                    return matchBool(lower(n.left), litBool(true), lower(n.right));
                }
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
            } else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
                return lowerCurry(n);
            } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                // A `do ... end` block as an expression (e.g. a match arm) is
                // just its body sequence, scoped so its bindings don't leak.
                return lowerBodyScoped(n.body);
            } else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
                // spawn do B end → erlang:spawn(fun() -> B end).
                Lambda lam; lam.body = lowerBody(n.body);
                auto fn = std::make_unique<Expr>(); fn->node = std::move(lam);
                std::vector<Binding> binds;
                auto fnv = atomize_ir(std::move(fn), binds);
                return wrapLets(binds, callE("erlang", "spawn", 1, one(std::move(fnv))));
            } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
                return lowerReceive(n);
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

    // Operator name (`~(+)`) → the intrinsic op it curries to. Mirrors the
    // string emitter's opBif table (+ and / stay polymorphic via kex_io).
    static auto curryOp(const std::string& name) -> std::optional<Op> {
        static const std::unordered_map<std::string, Op> t = {
            {"+",Op::Add},{"-",Op::Sub},{"*",Op::Mul},{"/",Op::Div},{"%",Op::Mod},
            {"==",Op::Eq},{"!=",Op::Neq},{"<",Op::Lt},{"<=",Op::Lte},{">",Op::Gt},{">=",Op::Gte},
        };
        auto it = t.find(name); return it == t.end() ? std::nullopt : std::optional<Op>(it->second);
    }
    // `~fn(args...)` / `~(op)` — partial or full application. Flatten every
    // paren group into ordered slots (a `_` placeholder is an open slot); if
    // all slots are bound and enough are present, apply now, else build a fun
    // taking one param per open slot plus trailing params up to the arity.
    auto lowerCurry(const ast::CurryExpr& n) -> ExprPtr {
        struct Slot { bool open; ExprPtr val; };
        std::vector<Binding> binds;
        std::vector<Slot> slots;
        for (const auto& g : n.argGroups)
            for (const auto& a : g) {
                if (std::holds_alternative<ast::CurryPlaceholder>(a->kind))
                    slots.push_back({true, nullptr});
                else slots.push_back({false, atomize(a, binds)});
            }
        int arity = -1;
        if (n.isOperator) arity = 2;
        else if (auto it = fnParamNames.find(n.name); it != fnParamNames.end())
            arity = static_cast<int>(it->second.size());

        auto buildCall = [&](std::vector<ExprPtr> args) -> ExprPtr {
            if (n.isOperator && args.size() >= 2)
                if (auto op = curryOp(n.name))
                    return intrin(*op, two(std::move(args[0]), std::move(args[1])));
            int ar = static_cast<int>(args.size());
            auto ex = std::make_unique<Expr>();
            ex->node = Call{"", n.name, ar, std::move(args), false};
            return ex;
        };

        int openCount = 0;
        for (const auto& s : slots) if (s.open) openCount++;
        bool full = openCount == 0 &&
            (arity >= 0 ? static_cast<int>(slots.size()) >= arity : !slots.empty());
        if (full) {
            std::vector<ExprPtr> args;
            for (auto& s : slots) args.push_back(std::move(s.val));
            return wrapLets(binds, buildCall(std::move(args)));
        }

        // Partial: a fresh param per open slot, plus trailing params for any
        // arity beyond the slots written (so `~add(1)` on add/2 still takes
        // one more arg).
        int trailing = (arity >= 0) ? std::max(0, arity - static_cast<int>(slots.size())) : 0;
        Lambda lam;
        std::vector<ExprPtr> finalArgs;
        for (auto& s : slots) {
            if (s.open) { std::string p = fresh("P"); lam.params.push_back(p); finalArgs.push_back(var(p)); }
            else finalArgs.push_back(std::move(s.val));
        }
        for (int i = 0; i < trailing; i++) { std::string p = fresh("T"); lam.params.push_back(p); finalArgs.push_back(var(p)); }
        lam.body = buildCall(std::move(finalArgs));
        auto ex = std::make_unique<Expr>(); ex->node = std::move(lam);
        return wrapLets(binds, std::move(ex));
    }

    auto lowerFunctionCall(const ast::FunctionCall& n) -> ExprPtr {
        // Named args → reorder into the callee's positional slots by param
        // name; then positional args (and a trailing block) fill remaining
        // slots in order, leftovers default to None. Mirrors the string
        // emitter / Evaluator::callFunction (spec/optional_parens_do.kex).
        if (!n.namedArgs.empty()) {
            auto it = fnParamNames.find(n.name);
            if (it == fnParamNames.end())
                throw LowerError("IR lower: named args to unknown function " + n.name);
            const auto& pnames = it->second;
            std::vector<Binding> binds;
            std::vector<ExprPtr> slots(pnames.size());
            for (const auto& [an, av] : n.namedArgs)
                for (size_t i = 0; i < pnames.size(); i++)
                    if (pnames[i] == an) { slots[i] = atomize(av, binds); break; }
            std::vector<ExprPtr> positional;
            for (const auto& a : n.args) positional.push_back(atomize(a, binds));
            if (n.block) positional.push_back(atomize(*n.block, binds));
            size_t next = 0;
            for (auto& p : positional) {
                while (next < slots.size() && slots[next]) next++;
                if (next >= slots.size()) break;
                slots[next] = std::move(p);
            }
            for (auto& s : slots) if (!s) s = lit(LitKind::None, "none");
            auto ex = std::make_unique<Expr>();
            ex->node = Call{"", n.name, (int)slots.size(), std::move(slots), false};
            return wrapLets(binds, std::move(ex));
        }
        std::vector<Binding> binds;
        // describe/it: the testing DSL → kex_test, block as a 0-arg fun.
        if ((n.name == "describe" || n.name == "it") && n.block && n.args.size() == 1) {
            auto name = atomize(n.args[0], binds);
            auto fn = atomize(*n.block, binds);
            return wrapLets(binds, callE("kex_test", n.name, 2, two(std::move(name), std::move(fn))));
        }
        // worker { block } → kex_supervisor:worker(fun() -> block end), unless
        // the program defines its own `worker`.
        if (n.name == "worker" && n.block && !knownFns.count("worker")) {
            auto fn = atomize(*n.block, binds);
            return wrapLets(binds, callE("kex_supervisor", "worker", 1, one(std::move(fn))));
        }
        // assert(cond[, msg]) — a plain global builtin, not a local function.
        if (n.name == "assert" && !n.args.empty() && !n.block) {
            std::vector<ExprPtr> as;
            for (const auto& a : n.args) as.push_back(atomize(a, binds));
            int ar = static_cast<int>(as.size());
            return wrapLets(binds, callE("kex_io", "assert", ar, std::move(as)));
        }
        std::vector<ExprPtr> args;
        for (const auto& a : n.args) args.push_back(atomize(a, binds));
        // A trailing do-block is passed as the function's last argument.
        if (n.block) args.push_back(atomize(*n.block, binds));
        auto ex = std::make_unique<Expr>();
        int arity = static_cast<int>(args.size());
        // A 0-arity function/constant holding a fun (e.g. `let inc = ~add(1)`)
        // called with args: evaluate the thunk, then apply the resulting fun.
        bool zeroArgThunk = !subst.count(n.name) &&
            (zeroArgFns.count(n.name) || topLevelConstants.count(n.name));
        // Capitalized name = ADT constructor with a payload → tagged tuple.
        if (!n.name.empty() && std::isupper(static_cast<unsigned char>(n.name[0]))
            && !zeroArgThunk)
            ex->node = Construct{n.name, std::move(args)};
        else if (zeroArgThunk && !args.empty()) {
            auto thunk = std::make_unique<Expr>();
            thunk->node = Call{"", n.name, 0, {}, false};
            ex->node = CallIndirect{std::move(thunk), std::move(args), false};
        } else if (zeroArgThunk)
            // `thunk()` with no args → just evaluate the 0-arity function (its
            // value may itself be a fun; matching the default emitter, we do
            // NOT auto-apply it further).
            ex->node = Call{"", n.name, 0, {}, false};
        else if (knownFns.count(n.name))
            ex->node = Call{"", n.name, arity, std::move(args), false};
        else
            // Not a module function → a variable holding a fun (e.g. a `block`
            // parameter): apply it indirectly.
            ex->node = CallIndirect{var(currentName(n.name)), std::move(args), false};
        return wrapLets(binds, std::move(ex));
    }

    // Minimal UFCS/stdlib resolution for the walking skeleton. Only the
    // handful of forms an early target program needs; everything else errors
    // (to be widened as constructs are ported from core_erlang.cxx).
    // Set while lowering a mutating `!` call's VALUE (the rebind itself is
    // handled by the enclosing statement — see lowerBodyFrom/loop handling).
    bool m_lowerMutatingAsValue = false;
    // Set while lowering a match-clause `when` guard: Core Erlang guards can't
    // call arbitrary functions, so char predicates must inline as guard-safe
    // range checks rather than a `kex_io:is_*` call.
    bool m_inGuard = false;
    auto lowerGuard(const ast::ExprPtr& g) -> ExprPtr {
        m_inGuard = true; auto r = lower(g); m_inGuard = false; return r;
    }

    // A module path `A.B.C` (nested modules, encoded by the parser as a chain
    // of MethodCall nodes with no args) flattened to the qualified name
    // ["A","B","C"], or empty if the receiver isn't a pure uppercase path.
    static auto modulePath(const ast::Expr& e, std::vector<std::string>& out) -> bool {
        if (auto* uid = std::get_if<ast::UpperIdentifier>(&e.kind)) {
            out.push_back(uid->name); return true;
        }
        if (auto* mc = std::get_if<ast::MethodCall>(&e.kind)) {
            if (!mc->args.empty() || !mc->namedArgs.empty() || mc->block) return false;
            if (!mc->method.empty() && std::isupper(static_cast<unsigned char>(mc->method[0]))) {
                if (!modulePath(*mc->receiver, out)) return false;
                out.push_back(mc->method); return true;
            }
        }
        return false;
    }

    auto lowerMethodCall(const ast::MethodCall& n) -> ExprPtr {
        // A bare mutating `!` call used where its rebind can't be applied is
        // an error (keeps the invariant that compiled code is correct); in
        // statement position the caller lowers it as a value + rebind.
        if (n.mutating && !m_lowerMutatingAsValue)
            throw LowerError("IR lower: mutating '!' call in expression position not yet ported");
        // A call into the `Kex.Intrinsic.<Category>` runtime module, e.g.
        // `Kex.Intrinsic.List.reverse(x)`. Compile to a plain cross-module call
        // `call 'kex_intrinsic_list':'reverse'(x)` — the emitter knows NOTHING
        // about `reverse`; the runtime module owns it, and the Kex prelude above
        // owns the typed semantics. Adding a primitive = add a runtime function,
        // with zero emitter changes.
        {
            std::vector<std::string> path;
            if (modulePath(*n.receiver, path) && path.size() >= 3 &&
                path[0] == "Kex" && path[1] == "Intrinsic") {
                std::string mod = "kex_intrinsic";
                for (size_t i = 2; i < path.size(); i++) {
                    mod += "_";
                    for (char c : path[i]) mod += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                std::vector<Binding> binds;
                std::vector<ExprPtr> args;
                for (const auto& a : n.args) args.push_back(atomize(a, binds));
                auto ex = std::make_unique<Expr>();
                ex->node = Call{mod, n.method, static_cast<int>(args.size()), std::move(args), false};
                return wrapLets(binds, std::move(ex));
            }
        }
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
            if (uid->name == "Process") {
                if (n.method == "self" && args.empty()) return callE("erlang", "self", 0, {});
                if (n.method == "exit" && args.size() == 2)
                    return wrapLets(binds, callE("erlang", "exit", 2, std::move(args)));
                if (n.method == "register" && args.size() == 2)
                    return wrapLets(binds, callE("erlang", "register", 2, std::move(args)));
                if (n.method == "whereis" && args.size() == 1)
                    return wrapLets(binds, callE("erlang", "whereis", 1, std::move(args)));
            }
            if (uid->name == "Task") {
                // Task.start { block } → kex_task:start(fun() -> block end).
                if (n.method == "start" && n.block) {
                    auto fn = atomize(*n.block, binds);
                    return wrapLets(binds, callE("kex_task", "start", 1, one(std::move(fn))));
                }
                if (n.method == "awaitAll" && !args.empty())
                    return wrapLets(binds, callE("kex_task", "await_all", 1, one(std::move(args[0]))));
            }
            // Supervisor.start(restart: strat) do children end →
            // kex_supervisor:start_link(#{strategy => strat, children => Kids}).
            if (uid->name == "Supervisor" && n.method == "start" && n.block) {
                ExprPtr strat = lit(LitKind::Atom, "only_crashed");
                for (const auto& [k, v] : n.namedArgs)
                    if (k == "restart") strat = lower(v);
                ExprPtr children;
                if (auto* lam = std::get_if<ast::Lambda>(&(*n.block)->kind))
                    children = lowerBody(lam->body);
                else
                    children = lower(*n.block);
                auto stratA = atomize_ir(std::move(strat), binds);
                auto childA = atomize_ir(std::move(children), binds);
                auto pair = [&](const char* key, ExprPtr val) {
                    auto t = std::make_unique<Expr>();
                    t->node = MakeTuple{two(lit(LitKind::Atom, key), std::move(val))};
                    return t;
                };
                std::vector<ExprPtr> pairs;
                pairs.push_back(pair("strategy", std::move(stratA)));
                pairs.push_back(pair("children", std::move(childA)));
                auto lst = std::make_unique<Expr>();
                lst->node = MakeList{std::move(pairs), std::nullopt};
                auto map = callE("maps", "from_list", 1, one(std::move(lst)));
                return wrapLets(binds, callE("kex_supervisor", "start_link", 1, one(std::move(map))));
            }
            if (uid->name == "Math") {
                if (n.method == "sqrt") return nsCall("math", "sqrt");
                if (n.method == "pow" || n.method == "power") return nsCall("math", "pow");
                if (n.method == "sin") return nsCall("math", "sin");
                if (n.method == "cos") return nsCall("math", "cos");
                if (n.method == "tan") return nsCall("math", "tan");
                if (n.method == "log") return nsCall("kex_io", "math_log");
                if (n.method == "abs") return nsCall("erlang", "abs");
                if (n.method == "floor") return nsCall("erlang", "floor");
                if (n.method == "ceil") return nsCall("erlang", "ceil");
                if (n.method == "PI" || n.method == "pi") return nsCall("math", "pi");
                if (n.method == "E" || n.method == "e") return nsCall("kex_io", "math_e");
                if (n.method == "atan2") return nsCall("math", "atan2");
                if (n.method == "atan") return nsCall("math", "atan");
                if (n.method == "log10") return nsCall("math", "log10");
                if (n.method == "hypot") return nsCall("kex_io", "math_hypot");
                if (n.method == "cbrt") return nsCall("kex_io", "math_cbrt");
                if (n.method == "exp") return nsCall("math", "exp");
                if (n.method == "log2") return nsCall("math", "log2");
            }
            if (uid->name == "File") {
                if (n.method == "read") return nsCall("kex_file", "read");
                if (n.method == "write") return nsCall("kex_file", "write");
                if (n.method == "append") return nsCall("kex_file", "append");
                if (n.method == "exists?") return nsCall("kex_file", "exists");
                if (n.method == "delete") return nsCall("kex_file", "delete");
                if (n.method == "size") return nsCall("kex_file", "size");
                if (n.method == "lines") return nsCall("kex_file", "lines");
            }
            // ENV is a real Map value (kex_io:env_map()); its methods are just
            // map operations on that value.
            if (uid->name == "ENV") {
                auto envMap = [&]{ return callE("kex_io", "env_map", 0, {}); };
                // get(key) -> String? : Just(value) if set, None otherwise
                // (matching the tree-walker's Optional-returning semantics).
                if (n.method == "get" && args.size() == 1) {
                    auto key = atomize_ir(std::move(args[0]), binds);
                    auto just = std::make_unique<Expr>();
                    just->node = Construct{"Just", one(
                        callE("maps", "get", 2, two(clone(key), envMap())))};
                    return wrapLets(binds, matchBool(
                        callE("maps", "is_key", 2, two(clone(key), envMap())),
                        std::move(just), lit(LitKind::None, "none")));
                }
                if (n.method == "get" && args.size() == 2)
                    return wrapLets(binds, callE("maps", "get", 3,
                        three(std::move(args[0]), envMap(), std::move(args[1]))));
                if (n.method == "has?" && args.size() == 1)
                    return wrapLets(binds, callE("maps", "is_key", 2,
                        two(std::move(args[0]), envMap())));
                if (n.method == "count" || n.method == "size")
                    return wrapLets(binds, callE("maps", "size", 1, one(envMap())));
                if (n.method == "keys")
                    return wrapLets(binds, callE("maps", "keys", 1, one(envMap())));
                if (n.method == "values")
                    return wrapLets(binds, callE("maps", "values", 1, one(envMap())));
                if (n.method == "each" && n.block) {
                    auto fn = atomize(*n.block, binds);
                    return wrapLets(binds, callE("maps", "foreach", 2, two(std::move(fn), envMap())));
                }
            }
            // Static method dispatch: `Type.method(args)` on a user type whose
            // `method` is a local function/make-method. If that method has an
            // implicit `this`, pass a placeholder receiver (the type tag).
            if (knownTypes.count(uid->name) && localMethods.count(n.method)) {
                std::vector<ExprPtr> callArgs;
                if (implicitThisMethods.count(n.method))
                    callArgs.push_back(lit(LitKind::Atom, uid->name)); // placeholder receiver
                for (auto& a : args) callArgs.push_back(std::move(a));
                int ar = static_cast<int>(callArgs.size());
                auto ex = std::make_unique<Expr>();
                ex->node = Call{"", n.method, ar, std::move(callArgs), false};
                return wrapLets(binds, std::move(ex));
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

        // Migrated stdlib functions route straight to the shared kex_prelude
        // module, bypassing the builtin ladder below (so migrating a method is
        // just adding its name to migratedPreludeFns + a prelude wrapper — no
        // ladder surgery). Block/named-arg forms carry polymorphic dispatch, so
        // they stay on the ladder for now.
        // Not in a guard: a cross-module `kex_prelude:…` call is illegal inside
        // a Core Erlang guard, so migrated methods fall through to the guard-
        // safe ladder form there (e.g. `.count` → erlang:length).
        if (!m_inGuard && preludeFns.count(m) && !n.block && n.namedArgs.empty()) {
            std::vector<ExprPtr> pargs;
            pargs.push_back(clone(rv));
            for (const auto& a : n.args) pargs.push_back(atomize_ir(lower(a), rb));
            return ret(callE("kex_prelude", m, static_cast<int>(n.args.size()) + 1, std::move(pargs)));
        }
        // Higher-order stdlib functions take a block/function argument (so
        // they're excluded from the plain routing above). Route them to the
        // prelude with the block appended as the trailing argument. Only
        // list-only HOFs (no map counterpart → no receiver-type dispatch) are
        // migrated so far.
        static const std::unordered_set<std::string> hofPreludeFns = {"reduce", "map", "each", "filter", "reject", "mapValues", "mapKeys", "all?", "any?", "find", "flatMap", "count"};
        if (!m_inGuard && hofPreludeFns.count(m) && n.namedArgs.empty()) {
            std::vector<ExprPtr> pargs;
            pargs.push_back(clone(rv));
            for (const auto& a : n.args) pargs.push_back(atomize_ir(lower(a), rb));
            if (n.block) pargs.push_back(atomize_ir(lower(*n.block), rb));
            return ret(callE("kex_prelude", m, static_cast<int>(pargs.size()), std::move(pargs)));
        }

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
            {"product","kex_intrinsic_list","list_product",0}, {"sum","lists","sum",0},
            // `reverse` migrated to the Kex prelude (kex_prelude:reverse →
            // Kex.Intrinsic.List.reverse). See docs/prelude-intrinsic-plan.md.
            {"sort","lists","sort",0},
            {"uniq","lists","usort",0}, {"unique","lists","usort",0},
            {"abs","erlang","abs",0}, {"sqrt","math","sqrt",0},
            {"upperCase","string","to_upper",0}, {"upcase","string","to_upper",0},
            {"lowerCase","string","to_lower",0}, {"downcase","string","to_lower",0},
            {"trim","string","trim",0}, {"at","kex_intrinsic_list","list_get",1},
            {"digit?","kex_io","is_digit",0}, {"alpha?","kex_io","is_alpha",0},
            {"space?","kex_io","is_space",0},
        };
        // Char predicates in a guard must inline as guard-safe range checks
        // (a `kex_io:is_*` call is an illegal guard expression).
        // Guards can't contain `case`, so use the strict erlang and/or BIFs
        // (guard-safe) rather than the short-circuit Op::And/Or (which emit a
        // case).
        if (m_inGuard && n.args.empty() && !n.block) {
            auto gand = [&](ExprPtr a, ExprPtr b){ return callE("erlang","and",2,two(std::move(a),std::move(b))); };
            auto gor  = [&](ExprPtr a, ExprPtr b){ return callE("erlang","or",2,two(std::move(a),std::move(b))); };
            auto between = [&](long lo, long hi) {
                return gand(intrin(Op::Gte, two(clone(rv), litInt(lo))),
                            intrin(Op::Lte, two(clone(rv), litInt(hi))));
            };
            if (m == "digit?") return ret(between('0', '9'));
            if (m == "alpha?") return ret(gor(between('A','Z'), between('a','z')));
            if (m == "space?") {
                ExprPtr chk;
                for (int c : {' ', '\t', '\n', '\r'}) {
                    auto eq = intrin(Op::Eq, two(clone(rv), litInt(c)));
                    chk = chk ? gor(std::move(chk), std::move(eq)) : std::move(eq);
                }
                return ret(std::move(chk));
            }
        }
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
        // pid.send(m) → erlang:send(Pid, {'kex_msg', M, self()}). Every Kex
        // message is wrapped with the sender pid for `receive |sender|`.
        if (m == "send" && n.args.size() == 1) {
            auto tup = std::make_unique<Expr>();
            tup->node = MakeTuple{[&]{
                std::vector<ExprPtr> t;
                t.push_back(lit(LitKind::Atom, "kex_msg"));
                t.push_back(arg0());
                t.push_back(callE("erlang", "self", 0, {}));
                return t;
            }()};
            return ret(callE("erlang", "send", 2, two(clone(rv), std::move(tup))));
        }
        if ((m == "link" || m == "unlink") && n.args.empty())
            return ret(callE("erlang", m, 1, one(clone(rv))));
        // task.await(timeout: T) / task.await(T) → kex_task:await/2.
        if (m == "await") {
            ExprPtr timeout;
            for (const auto& [k, v] : n.namedArgs)
                if (k == "timeout") timeout = lower(v);
            if (!n.args.empty()) timeout = arg0();
            if (!timeout) timeout = lit(LitKind::Atom, "infinity");
            return ret(callE("kex_task", "await", 2,
                two(clone(rv), atomize_ir(std::move(timeout), rb))));
        }
        // contains?: String/List/Range are all plain lists at runtime, so the
        // needle type disambiguates — a list needle means substring search
        // (string:find), a scalar means element membership (lists:member).
        if (m == "contains?" && n.args.size() == 1) {
            auto needle = atomize_ir(lower(n.args[0]), rb);
            auto substr = intrin(Op::Neq, two(
                callE("string","find",2,two(clone(rv), clone(needle))),
                lit(LitKind::Atom, "nomatch")));
            auto member = callE("lists","member",2,two(clone(needle), clone(rv)));
            return ret(matchBool(callE("erlang","is_list",1,one(clone(needle))),
                                 std::move(substr), std::move(member)));
        }
        if (m == "indexOf" && n.args.size() == 1)
            return ret(callE("kex_intrinsic_list","index_of",2,two(arg0(), clone(rv))));
        // NOTE: put/keys/values/entries/delete/merge/has? (map) and
        // zip/flatten/take/drop/split (list/string) are migrated to the prelude
        // — their ladder handlers were removed so the prelude's own (canonical)
        // definitions aren't shadowed during prelude compilation.
        if (m == "alive?"  && n.args.empty()) return ret(callE("erlang","is_process_alive",1,one(clone(rv))));
        // join is migrated to the prelude (Kex.Intrinsic.List.join →
        // lists:flatten / lists:join+flatten). Not guard-safe (lists:flatten/
        // join aren't guard BIFs), so no guard fallback — removed outright.
        // .some?/.present? on an Option — the Some/Just tag check (the block
        // form `.some? { … }` is handled with the HOFs below).
        if ((m == "some?" || m == "present?") && n.args.empty() && !n.block) {
            // Match, not element/2 — None is the bare atom 'none', not a tuple.
            Match mm; mm.subjects.push_back(clone(rv));
            for (const char* tag : {"Some", "Just"}) {
                MatchClause c; auto p = std::make_unique<Pattern>();
                p->kind = PatKind::Construct; p->tag = tag;
                auto wv = std::make_unique<Pattern>(); wv->kind = PatKind::Wild;
                p->args.push_back(std::move(wv));
                c.patterns.push_back(std::move(p)); c.body = litBool(true);
                mm.clauses.push_back(std::move(c));
            }
            MatchClause d; auto wp = std::make_unique<Pattern>(); wp->kind = PatKind::Wild;
            d.patterns.push_back(std::move(wp)); d.body = litBool(false);
            mm.clauses.push_back(std::move(d));
            auto x = std::make_unique<Expr>(); x->node = std::move(mm);
            return ret(std::move(x));
        }
        if ((m == "count" || m == "length" || m == "size") && n.args.empty() && !n.block)
            return ret(matchBool(callE("erlang","is_map",1,one(clone(rv))),
                callE("maps","size",1,one(clone(rv))),
                callE("erlang","length",1,one(clone(rv)))));
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
        // none?: an Option is None (Kex None → the 'none' atom).
        if (m == "none?" && n.args.empty() && !localMethods.count("none?"))
            return ret(intrin(Op::Eq, two(clone(rv), lit(LitKind::None, "none"))));
        // get(key, default): list → list_get/3; map → maps:get/3.
        if (m == "get" && n.args.size() == 2 && !localMethods.count("get")) {
            auto k = atomize_ir(lower(n.args[0]), rb);
            auto d = atomize_ir(lower(n.args[1]), rb);
            return ret(matchBool(callE("erlang","is_list",1,one(clone(rv))),
                callE("kex_intrinsic_list","list_get",3,three(clone(rv), clone(k), clone(d))),
                callE("maps","get",3,three(clone(k), clone(rv), clone(d)))));
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
                callE("kex_intrinsic_list","list_get",2,two(clone(rv), clone(idx))),
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
            // A 2-parameter block (|k, v|) means map iteration — route the
            // shared HOF names to their maps:* equivalents (the string emitter
            // does the same via its blockArity2 check).
            bool mapForm = false;
            if (auto* lam = std::get_if<ast::Lambda>(&(*blk)->kind))
                mapForm = lam->params.size() == 2;
            if (mapForm) {
                if (m == "each")
                    return build(callE("maps", "foreach", 2, two(std::move(fn), clone(rv))));
                if (m == "filter" || m == "select")
                    return build(callE("maps", "filter", 2, two(std::move(fn), clone(rv))));
                if (m == "map") {
                    // one result per (k,v): reverse(fold(fun(k,v,acc)->[fn(k,v)|acc], [], m)).
                    std::string kk = fresh("K"), vv = fresh("V"), acc = fresh("Acc");
                    Lambda fold; fold.params = {kk, vv, acc};
                    auto cell = std::make_unique<Expr>();
                    cell->node = MakeList{one(callE_indirect(std::move(fn), two(var(kk), var(vv)))),
                                          std::optional<ExprPtr>(var(acc))};
                    fold.body = std::move(cell);
                    auto ff = std::make_unique<Expr>(); ff->node = std::move(fold);
                    auto foldV = atomize_ir(std::move(ff), binds);
                    return build(callE("lists", "reverse", 1, one(
                        callE("maps", "fold", 3, three(std::move(foldV),
                            [&]{ auto e = std::make_unique<Expr>(); e->node = MakeList{{}, std::nullopt}; return e; }(),
                            clone(rv))))));
                }
                auto mapCount = [&](ExprPtr f) {
                    return callE("maps", "size", 1, one(callE("maps", "filter", 2, two(std::move(f), clone(rv)))));
                };
                if (m == "count") return build(mapCount(std::move(fn)));
                if (m == "any?" || m == "some?")
                    return build(intrin(Op::Gt, two(mapCount(std::move(fn)), litInt(0))));
                if (m == "all?")
                    return build(intrin(Op::Eq, two(mapCount(std::move(fn)),
                                                    callE("maps", "size", 1, one(clone(rv))))));
                if (m == "reject") {
                    std::string kk = fresh("K"), vv = fresh("V");
                    Lambda neg; neg.params = {kk, vv};
                    neg.body = intrin(Op::Not, one(callE_indirect(std::move(fn), two(var(kk), var(vv)))));
                    auto nf = std::make_unique<Expr>(); nf->node = std::move(neg);
                    auto negV = atomize_ir(std::move(nf), binds);
                    return build(callE("maps", "filter", 2, two(std::move(negV), clone(rv))));
                }
                // find { |k,v| } → maps:fold keeping the first matching {k,v} as
                // Just({k,v}), else 'none'.
                if (m == "find") {
                    std::string kk = fresh("K"), vv = fresh("V"), acc = fresh("Acc");
                    auto pair = std::make_unique<Expr>();
                    pair->node = MakeTuple{two(var(kk), var(vv))};
                    auto justPair = std::make_unique<Expr>();
                    justPair->node = Construct{"Just", one(std::move(pair))};
                    auto inner = matchBool(callE_indirect(std::move(fn), two(var(kk), var(vv))),
                                           std::move(justPair), lit(LitKind::None, "none"));
                    // case Acc of 'none' -> inner ; _ -> Acc
                    Match outer; outer.subjects.push_back(var(acc));
                    MatchClause c1; auto np = std::make_unique<Pattern>();
                    np->kind = PatKind::Lit; np->litKind = LitKind::None; np->litText = "none";
                    c1.patterns.push_back(std::move(np)); c1.body = std::move(inner);
                    MatchClause c2; c2.patterns.push_back(wildPat()); c2.body = var(acc);
                    outer.clauses.push_back(std::move(c1)); outer.clauses.push_back(std::move(c2));
                    Lambda fold; fold.params = {kk, vv, acc};
                    fold.body = std::make_unique<Expr>(); fold.body->node = std::move(outer);
                    auto ff = std::make_unique<Expr>(); ff->node = std::move(fold);
                    auto foldV = atomize_ir(std::move(ff), binds);
                    return build(callE("maps", "fold", 3, three(std::move(foldV),
                        lit(LitKind::None, "none"), clone(rv))));
                }
            }
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
            if (m == "flatMap" || m == "flat_map")
                return build(callE("lists", "flatmap", 2, two(std::move(fn), clone(rv))));
            // reject { pred } → keep elements where pred is false.
            if (m == "reject") {
                std::string nx = fresh("Nx");
                Lambda neg; neg.params = {nx};
                neg.body = intrin(Op::Not, one(callE_indirect(clone(fn), one(var(nx)))));
                auto negFn = std::make_unique<Expr>(); negFn->node = std::move(neg);
                auto negV = atomize_ir(std::move(negFn), binds);
                return build(callE("lists", "filter", 2, two(std::move(negV), clone(rv))));
            }
            // Block arity of the original `{ ... }` (−1 if not a Lambda).
            auto blockArity = [&](const ast::ExprPtr* b) -> int {
                if (b) if (auto* lam = std::get_if<ast::Lambda>(&(*b)->kind))
                    return static_cast<int>(lam->params.size());
                return -1;
            };
            // mapValues { |v| } / { |k,v| } → maps:map(fun(K,V)->NewV, m).
            if (m == "mapValues") {
                ExprPtr mapper;
                if (blockArity(blk) == 2) mapper = std::move(fn);
                else {
                    std::string kk = fresh("K"), vv = fresh("V");
                    Lambda w; w.params = {kk, vv};
                    w.body = callE_indirect(std::move(fn), one(var(vv)));
                    auto wf = std::make_unique<Expr>(); wf->node = std::move(w);
                    mapper = atomize_ir(std::move(wf), binds);
                }
                return build(callE("maps", "map", 2, two(std::move(mapper), clone(rv))));
            }
            // mapKeys { |k| } → maps:fold(fun(K,V,A)->put(fn(K),V,A), #{}, m).
            if (m == "mapKeys") {
                std::string kk = fresh("K"), vv = fresh("V"), acc = fresh("Acc");
                Lambda fold; fold.params = {kk, vv, acc};
                fold.body = callE("maps", "put", 3, three(
                    callE_indirect(std::move(fn), one(var(kk))), var(vv), var(acc)));
                auto ff = std::make_unique<Expr>(); ff->node = std::move(fold);
                auto foldV = atomize_ir(std::move(ff), binds);
                return build(callE("maps", "fold", 3, three(std::move(foldV),
                    callE("maps", "new", 0, {}), clone(rv))));
            }
            // find { pred } → Just(x) for the first match, else None. Built on
            // lists:search/2 which returns {value, X} | false.
            if (m == "find") {
                std::string found = fresh("Found");
                Match mm;
                mm.subjects.push_back(callE("lists", "search", 2, two(std::move(fn), clone(rv))));
                MatchClause hit;
                auto tp = std::make_unique<Pattern>(); tp->kind = PatKind::Tuple;
                auto vp = std::make_unique<Pattern>(); vp->kind = PatKind::Lit;
                vp->litKind = LitKind::Atom; vp->litText = "value";
                auto xp = std::make_unique<Pattern>(); xp->kind = PatKind::Var; xp->name = found;
                tp->args.push_back(std::move(vp)); tp->args.push_back(std::move(xp));
                hit.patterns.push_back(std::move(tp));
                hit.body = justOf(var(found));
                MatchClause miss;
                auto fp = std::make_unique<Pattern>(); fp->kind = PatKind::Lit;
                fp->litKind = LitKind::Bool; fp->litBool = false;
                miss.patterns.push_back(std::move(fp));
                miss.body = lit(LitKind::None, "none");
                mm.clauses.push_back(std::move(hit)); mm.clauses.push_back(std::move(miss));
                auto e = std::make_unique<Expr>(); e->node = std::move(mm);
                return build(std::move(e));
            }
            // partition → {matching, rest} 2-tuple (lists:partition returns
            // exactly that pair).
            if (m == "partition") {
                auto p = callE("lists", "partition", 2, two(std::move(fn), clone(rv)));
                return build(std::move(p));
            }
            if (m == "count") // count matching a predicate
                return build(callE("erlang", "length", 1,
                    one(callE("lists", "filter", 2, two(std::move(fn), clone(rv))))));
            // reduce(seed) { |acc, x| } / reduce(seed, fn) → lists:foldl(
            // fun(x,acc)->fn(acc,x), seed, recv). The fn is either a trailing
            // block or the 2nd positional arg (e.g. a curried `~(+)`).
            if ((m == "reduce" || m == "inject") &&
                ((n.block && n.args.size() == 1) || (!n.block && n.args.size() == 2))) {
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
        // Prelude stdlib function → the shared kex_prelude BEAM module
        // (`x.reverse` → `kex_prelude:reverse(x)`). The typed impl lives in the
        // Kex prelude; this is just the cross-module dispatch. A user-defined
        // method of the same name shadows it (localMethods is checked above).
        if (preludeFns.count(n.method) && !n.block && n.namedArgs.empty()) {
            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            args.push_back(clone(rv));
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            int arity = static_cast<int>(n.args.size()) + 1;
            return ret(wrapLets(binds, callE("kex_prelude", n.method, arity, std::move(args))));
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

    // Run `body`, then kex_test:maybe_print_summary() (which prints the
    // describe/it pass/fail tally, or nothing if no tests ran), returning
    // body's value. Wraps every main.
    auto withTestSummary(ExprPtr body) -> ExprPtr {
        std::string r = fresh("R");
        return makeLet(r, std::move(body),
            makeLet(fresh("Sum"), callE("kex_test", "maybe_print_summary", 0, {}), var(r)));
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
    auto wildPat() -> PatternPtr {
        auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w;
    }
    // Record-destructure `{ f, g: alias, h: { nested } }` → a flat list of
    // (name, accessor-expr) bindings read by field name from `baseVar`.
    // Recurses into nested record patterns.
    // Read field `fname` from `base`. Prefer a direct element(pos, base) from
    // the record layout — the accessor FUNCTION may be suppressed by a
    // same-named user method (e.g. records.kex's `city` field vs `city`
    // method), so calling `apply 'fname'/1` could hit the wrong function.
    auto fieldAccess(const std::string& fname, ExprPtr base) -> ExprPtr {
        auto it = fieldAccessors.find(fname);
        if (it != fieldAccessors.end() && !it->second.empty())
            return callE("erlang", "element", 2,
                two(litInt(it->second[0].second), std::move(base)));
        return callE("", fname, 1, one(std::move(base)));
    }
    void destructureRecordPattern(const std::string& baseVar, const ast::RecordPattern& rp,
                                  std::vector<std::pair<std::string, ExprPtr>>& prefix) {
        for (const auto& field : rp.fields) {
            auto acc = fieldAccess(field.name, var(baseVar));
            if (!field.pattern) {
                prefix.push_back({field.name, std::move(acc)});
            } else if (auto* vp = std::get_if<ast::VarPattern>(&(*field.pattern)->kind)) {
                prefix.push_back({vp->name, std::move(acc)});
            } else if (auto* nrp = std::get_if<ast::RecordPattern>(&(*field.pattern)->kind)) {
                std::string sub = fresh("rec");
                prefix.push_back({sub, std::move(acc)});
                destructureRecordPattern(sub, *nrp, prefix);
            } else {
                throw LowerError("IR lower: unsupported record-field sub-pattern");
            }
        }
    }
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
        // `if let Pat = scrutinee ... [else ...]`: match Pat, else fall through.
        if (n.letPattern) {
            auto subj = lower(n.condition); // condition holds the scrutinee
            auto snap = subst;
            auto pat = lowerPattern(n.letPattern);
            auto thenP = lowerBody(n.thenBody);
            subst = snap;
            auto elseP = n.elseBody ? lowerBodyScoped(*n.elseBody) : lit(LitKind::Atom, "ok");
            Match m; m.subjects.push_back(std::move(subj));
            MatchClause hit; hit.patterns.push_back(std::move(pat)); hit.body = std::move(thenP);
            MatchClause miss; auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild;
            miss.patterns.push_back(std::move(w)); miss.body = std::move(elseP);
            m.clauses.push_back(std::move(hit)); m.clauses.push_back(std::move(miss));
            auto e = std::make_unique<Expr>(); e->node = std::move(m); return e;
        }
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
        // Range-pattern dispatch: ranges materialize as ascending lists, so a
        // `(x..y)` / `(-10..-1)` clause can't match structurally. Bind the
        // subject once, then dispatch on its <first, last> bounds as a
        // 2-subject match — each RangePattern becomes <startPat, endPat>.
        bool rangeMode = std::any_of(n.clauses.begin(), n.clauses.end(),
            [](const ast::MatchClause& c) {
                return !c.patterns.empty() &&
                       std::holds_alternative<ast::RangePattern>(c.patterns[0]->kind);
            });
        if (rangeMode) return lowerRangeMatch(n);

        // `match subj do |x| ... end` binds the subject to `x`, in scope for
        // every clause's guard/body → let-bind it and match the bound var.
        ExprPtr subjIr = lower(n.subject);
        ExprPtr letWrap;
        std::string subjVar;
        if (n.subjectBinding) {
            subjVar = *n.subjectBinding;
            std::string ssa = subst.count(subjVar) ? fresh(subjVar) : subjVar;
            subst[subjVar] = ssa;
            letWrap = std::move(subjIr);
            subjIr = var(ssa);
        }
        Match m;
        m.subjects.push_back(std::move(subjIr));
        for (const auto& cl : n.clauses) {
            auto snap = subst;
            MatchClause mc;
            for (const auto& p : cl.patterns) mc.patterns.push_back(lowerPattern(p));
            if (cl.guard) mc.guard = lowerGuard(*cl.guard);
            mc.body = lower(cl.body);
            subst = snap;
            m.clauses.push_back(std::move(mc));
        }
        auto e = std::make_unique<Expr>();
        e->node = std::move(m);
        if (letWrap) {
            auto r = makeLet(currentName(subjVar), std::move(letWrap), std::move(e));
            subst.erase(subjVar);
            return r;
        }
        return e;
    }

    auto lowerReceive(const ast::ReceiveExpr& n) -> ExprPtr {
        Receive r;
        r.senderVar = n.senderBinding ? *n.senderBinding : fresh("Sndr");
        for (const auto& cl : n.clauses) {
            auto snap = subst;
            if (n.senderBinding) subst.erase(*n.senderBinding);
            ReceiveClause rc;
            rc.pattern = cl.patterns.empty() ? wildPat() : lowerPattern(cl.patterns[0]);
            if (cl.guard) rc.guard = lowerGuard(*cl.guard);
            rc.body = lower(cl.body);
            subst = snap;
            r.clauses.push_back(std::move(rc));
        }
        if (n.timeout && n.afterBody) {
            r.timeout = lower(*n.timeout);
            r.afterBody = lower(*n.afterBody);
        }
        auto e = std::make_unique<Expr>(); e->node = std::move(r);
        return e;
    }

    // A match with range-pattern clauses. Bind the subject, then match on its
    // <hd, last> bounds; each `(a..b)` clause → the 2-pattern `<a, b>`.
    auto lowerRangeMatch(const ast::MatchExpr& n) -> ExprPtr {
        std::string sv = fresh("RSubj");
        auto subjVal = lower(n.subject);
        Match m;
        m.subjects.push_back(callE("erlang", "hd", 1, one(var(sv))));
        m.subjects.push_back(callE("lists", "last", 1, one(var(sv))));
        for (const auto& cl : n.clauses) {
            auto snap = subst;
            MatchClause mc;
            const auto& p0 = cl.patterns[0];
            if (auto* rp = std::get_if<ast::RangePattern>(&p0->kind)) {
                mc.patterns.push_back(rp->start ? lowerPattern(rp->start) : wildPat());
                mc.patterns.push_back(rp->end ? lowerPattern(rp->end) : wildPat());
            } else {
                // A non-range clause (e.g. a bare `_`) spans both bounds.
                mc.patterns.push_back(wildPat());
                mc.patterns.push_back(wildPat());
            }
            if (cl.guard) mc.guard = lowerGuard(*cl.guard);
            mc.body = lower(cl.body);
            subst = snap;
            m.clauses.push_back(std::move(mc));
        }
        auto e = std::make_unique<Expr>(); e->node = std::move(m);
        return makeLet(sv, std::move(subjVal), std::move(e));
    }

    // ---- Loops ------------------------------------------------------------
    // Collect the names a loop body reassigns — plain `x = ...` AND mutating
    // `x.push!(..)` calls — recursing through if/match/block/nested-loops but
    // not lambdas. These become the loop's threaded state.
    void collectMutated(const ast::ExprPtr& e, std::unordered_set<std::string>& out) {
        if (!e) return;
        std::visit([&](const auto& nn) {
            using T = std::decay_t<decltype(nn)>;
            if constexpr (std::is_same_v<T, ast::AssignExpr>) out.insert(nn.name);
            else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                if (nn.mutating && nn.receiver)
                    if (auto* id = std::get_if<ast::Identifier>(&nn.receiver->kind)) out.insert(id->name);
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                for (auto& s : nn.thenBody) collectMutated(s, out);
                if (nn.elseBody) for (auto& s : *nn.elseBody) collectMutated(s, out);
                for (auto& [c, b] : nn.elifs) for (auto& s : b) collectMutated(s, out);
            } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                for (auto& cl : nn.clauses) collectMutated(cl.body, out);
            } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                for (auto& s : nn.body) collectMutated(s, out);
            } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                for (auto& s : nn.body) collectMutated(s, out);
            } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                for (auto& s : nn.body) collectMutated(s, out);
            }
        }, e->kind);
    }

    // The loop's current threaded-state value: bare var for one, tuple for
    // several, 'ok' for none.
    auto stateExpr(const std::vector<std::string>& mutVars) -> ExprPtr {
        if (mutVars.empty()) return lit(LitKind::Atom, "ok");
        if (mutVars.size() == 1) return var(currentName(mutVars[0]));
        std::vector<ExprPtr> els;
        for (const auto& v : mutVars) els.push_back(var(currentName(v)));
        auto e = std::make_unique<Expr>(); e->node = MakeTuple{std::move(els)}; return e;
    }
    auto tailCall(const std::string& loopFn, const std::vector<std::string>& mutVars) -> ExprPtr {
        std::vector<ExprPtr> args;
        for (const auto& v : mutVars) args.push_back(var(currentName(v)));
        auto e = std::make_unique<Expr>();
        e->node = Call{"", loopFn, (int)mutVars.size(), std::move(args), false};
        return e;
    }

    // If `e` is a break/next/return, its loop-control IR; else nullptr.
    auto loopControl(const ast::ExprPtr& e, const std::string& loopFn,
                     const std::vector<std::string>& mutVars) -> ExprPtr {
        if (std::holds_alternative<ast::BreakExpr>(e->kind)) return stateExpr(mutVars);
        if (std::holds_alternative<ast::NextExpr>(e->kind)) return tailCall(loopFn, mutVars);
        if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind)) {
            auto ex = std::make_unique<Expr>(); ex->node = Return{lower(re->value)}; return ex;
        }
        return nullptr;
    }

    // Lower a loop body's statements in loop context. `onEnd()` is spliced in
    // once the statements are exhausted — for the loop's own top-level body it
    // tail-calls the loop (next iteration); for an if/match branch it is "the
    // rest of the enclosing loop body". break yields the state, next
    // tail-calls, assignments/mutations rebind and thread forward, and
    // if/match/nested-loop recurse with the appropriate continuation. This one
    // callback-driven pass replaces what the string emitter did with three
    // separate, partly-overlapping loop-body walkers.
    auto lowerLoopBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i,
                           const std::string& loopFn,
                           const std::vector<std::string>& mutVars,
                           const std::function<ExprPtr()>& onEnd) -> ExprPtr {
        if (i >= body.size()) return onEnd();
        const auto& e = body[i];
        auto cont = [&]() -> ExprPtr { return lowerLoopBodyFrom(body, i + 1, loopFn, mutVars, onEnd); };
        if (std::holds_alternative<ast::BreakExpr>(e->kind)) return stateExpr(mutVars);
        if (std::holds_alternative<ast::NextExpr>(e->kind)) return tailCall(loopFn, mutVars);
        if (auto* ti = std::get_if<ast::TrailingIf>(&e->kind))
            if (auto ctrl = loopControl(ti->expr, loopFn, mutVars))
                return matchBool(lower(ti->condition), std::move(ctrl), cont());
        if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
            auto val = lower(ae->value);
            std::string nv = fresh(ae->name); subst[ae->name] = nv;
            return makeLet(nv, std::move(val), cont());
        }
        if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
            auto val = lower(ve->value);
            std::string nv = fresh(ve->name); subst[ve->name] = nv;
            return makeLet(nv, std::move(val), cont());
        }
        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                auto val = lower(le->value);
                std::string nv = fresh(vp->name); subst[vp->name] = nv;
                return makeLet(nv, std::move(val), cont());
            }
            auto val = lower(le->value); auto pat = lowerPattern(le->pattern);
            return makeMatch1(std::move(val), std::move(pat), cont());
        }
        if (auto* mc = std::get_if<ast::MethodCall>(&e->kind); mc && mc->mutating && mc->receiver)
            if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
                auto val = lowerMutatingAsValue(*mc);
                std::string nv = fresh(id->name); subst[id->name] = nv;
                return makeLet(nv, std::move(val), cont());
            }
        if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind)) {
            // `return X if cond` (ReturnExpr wrapping a TrailingIf): return
            // only when cond holds, otherwise fall through to the rest.
            if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
                auto retX = std::make_unique<Expr>(); retX->node = Return{lower(ti->expr)};
                return matchBool(lower(ti->condition), std::move(retX), cont());
            }
            auto ex = std::make_unique<Expr>(); ex->node = Return{lower(re->value)}; return ex;
        }
        if (auto* ie = std::get_if<ast::IfExpr>(&e->kind)) {
            std::function<ExprPtr()> branchEnd = [&]() -> ExprPtr { return cont(); };
            auto branch = [&](const std::vector<ast::ExprPtr>& bb) {
                auto snap = subst;
                auto r = lowerLoopBodyFrom(bb, 0, loopFn, mutVars, branchEnd);
                subst = snap; return r;
            };
            auto c = lower(ie->condition);
            auto thenP = branch(ie->thenBody);
            ExprPtr elseP = ie->elseBody ? branch(*ie->elseBody) : cont();
            return matchBool(std::move(c), std::move(thenP), std::move(elseP));
        }
        if (auto* me = std::get_if<ast::MatchExpr>(&e->kind)) {
            if (me->subjectBinding)
                throw LowerError("IR lower: match |binder| in loop not yet ported");
            std::function<ExprPtr()> armEnd = [&]() -> ExprPtr { return cont(); };
            Match m; m.subjects.push_back(lower(me->subject));
            for (const auto& cl : me->clauses) {
                auto snap = subst;
                MatchClause mcx;
                for (const auto& p : cl.patterns) mcx.patterns.push_back(lowerPattern(p));
                if (cl.guard) mcx.guard = lowerGuard(*cl.guard);
                mcx.body = lowerLoopArmU(cl.body, loopFn, mutVars, armEnd);
                subst = snap;
                m.clauses.push_back(std::move(mcx));
            }
            auto x = std::make_unique<Expr>(); x->node = std::move(m); return x;
        }
        if (auto* le2 = std::get_if<ast::LoopExpr>(&e->kind))
            return lowerLoopCore(le2->body, nullptr, false, [&]{ return cont(); });
        if (auto* we2 = std::get_if<ast::WhileExpr>(&e->kind))
            return lowerLoopCore(we2->body, &we2->condition, false, [&]{ return cont(); });
        auto val = lower(e);
        return makeLet(fresh("S"), std::move(val), cont());
    }

    // A single match-arm body in loop context (a `do` block is a statement
    // list; a bare expr is one statement), continuing with `armEnd`.
    auto lowerLoopArmU(const ast::ExprPtr& arm, const std::string& loopFn,
                       const std::vector<std::string>& mutVars,
                       const std::function<ExprPtr()>& armEnd) -> ExprPtr {
        if (auto* be = std::get_if<ast::BlockExpr>(&arm->kind))
            return lowerLoopBodyFrom(be->body, 0, loopFn, mutVars, armEnd);
        if (std::holds_alternative<ast::BreakExpr>(arm->kind)) return stateExpr(mutVars);
        if (std::holds_alternative<ast::NextExpr>(arm->kind)) return tailCall(loopFn, mutVars);
        if (auto* ti = std::get_if<ast::TrailingIf>(&arm->kind))
            if (auto ctrl = loopControl(ti->expr, loopFn, mutVars))
                return matchBool(lower(ti->condition), std::move(ctrl), armEnd());
        if (auto* re = std::get_if<ast::ReturnExpr>(&arm->kind)) {
            if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
                auto retX = std::make_unique<Expr>(); retX->node = Return{lower(ti->expr)};
                return matchBool(lower(ti->condition), std::move(retX), armEnd());
            }
            auto ex = std::make_unique<Expr>(); ex->node = Return{lower(re->value)}; return ex;
        }
        if (auto* ae = std::get_if<ast::AssignExpr>(&arm->kind)) {
            auto val = lower(ae->value);
            std::string nv = fresh(ae->name); subst[ae->name] = nv;
            return makeLet(nv, std::move(val), armEnd());
        }
        if (auto* mc = std::get_if<ast::MethodCall>(&arm->kind); mc && mc->mutating && mc->receiver)
            if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
                auto val = lowerMutatingAsValue(*mc);
                std::string nv = fresh(id->name); subst[id->name] = nv;
                return makeLet(nv, std::move(val), armEnd());
            }
        auto val = lower(arm);
        return makeLet(fresh("S"), std::move(val), armEnd());
    }

    // Lower a mutating `x.method!(args)` as the VALUE it rebinds x to (the
    // non-mutating method result).
    auto lowerMutatingAsValue(const ast::MethodCall& mc) -> ExprPtr {
        m_lowerMutatingAsValue = true;
        auto r = lowerMethodCall(mc);
        m_lowerMutatingAsValue = false;
        return r;
    }

    // Lower a branch body's statements (updating subst), ending in a value
    // that carries the final SSA names of `mergeVars` — used by the
    // conditional-assignment merge so a `var` reassigned inside an `if` branch
    // is visible after the `if` (examples/json_parser.kex's parseNumber
    // consumes a leading `-` inside an `if`).
    auto lowerBranchState(const std::vector<ast::ExprPtr>& branch,
                          const std::vector<std::string>& mergeVars) -> ExprPtr {
        std::function<ExprPtr(size_t)> go = [&](size_t j) -> ExprPtr {
            if (j >= branch.size()) return stateExpr(mergeVars);
            const auto& e = branch[j];
            if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
                auto val = lower(ae->value);
                std::string nv = fresh(ae->name); subst[ae->name] = nv;
                return makeLet(nv, std::move(val), go(j + 1));
            }
            if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
                auto val = lower(ve->value);
                std::string nv = fresh(ve->name); subst[ve->name] = nv;
                return makeLet(nv, std::move(val), go(j + 1));
            }
            if (auto* mc = std::get_if<ast::MethodCall>(&e->kind); mc && mc->mutating && mc->receiver)
                if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
                    auto val = lowerMutatingAsValue(*mc);
                    std::string nv = fresh(id->name); subst[id->name] = nv;
                    return makeLet(nv, std::move(val), go(j + 1));
                }
            if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind)) {
                auto ex = std::make_unique<Expr>(); ex->node = Return{lower(re->value)}; return ex;
            }
            auto val = lower(e);
            return makeLet(fresh("S"), std::move(val), go(j + 1));
        };
        return go(0);
    }

    // Lower a `loop`/`while` to a LetRec threading its mutable state.
    // `restIsLast`: the loop is the last statement (its result is the value).
    // `mkRest`: produces the continuation after the loop (with the extracted
    // state already in subst) — for a top-level loop this is the rest of the
    // enclosing body; for a NESTED loop it's the rest of the OUTER loop body
    // (so it tail-calls the outer loop).
    auto lowerLoopCore(const std::vector<ast::ExprPtr>& loopBody, const ast::ExprPtr* cond,
                       bool restIsLast, const std::function<ExprPtr()>& mkRest) -> ExprPtr {
        std::unordered_set<std::string> mset;
        for (const auto& s : loopBody) collectMutated(s, mset);
        std::vector<std::string> mutVars;
        for (const auto& v : mset) if (subst.count(v)) mutVars.push_back(v);
        std::sort(mutVars.begin(), mutVars.end());

        std::string loopFn = "__loop" + std::to_string(counter++);
        std::vector<ExprPtr> initArgs;
        for (const auto& v : mutVars) initArgs.push_back(var(currentName(v)));

        auto snap = subst;
        for (const auto& v : mutVars) subst[v] = v;
        // Condition and false-exit state use the ENTRY bindings.
        ExprPtr condExpr = cond ? lower(*cond) : nullptr;
        ExprPtr falseState = cond ? stateExpr(mutVars) : nullptr;
        ExprPtr inner = lowerLoopBodyFrom(loopBody, 0, loopFn, mutVars,
            [&]() -> ExprPtr { return tailCall(loopFn, mutVars); });
        if (cond)
            inner = matchBool(std::move(condExpr), std::move(inner), std::move(falseState));
        subst = snap;

        LetRec lr; lr.name = loopFn; lr.params = mutVars; lr.funBody = std::move(inner);
        auto callLoop = std::make_unique<Expr>();
        callLoop->node = Call{"", loopFn, (int)mutVars.size(), std::move(initArgs), false};

        if (mutVars.empty()) {
            lr.contBody = restIsLast ? std::move(callLoop)
                : makeLet(fresh("S"), std::move(callLoop), mkRest());
        } else {
            std::string resVar = fresh("LR");
            // Capture extracted names BEFORE mkRest() (which may itself lower a
            // loop that reassigns subst[v]).
            std::vector<std::string> boundNames;
            for (size_t k = 0; k < mutVars.size(); k++) {
                std::string nv = fresh(mutVars[k]); subst[mutVars[k]] = nv; boundNames.push_back(nv);
            }
            auto rest = restIsLast ? stateExpr(mutVars) : mkRest();
            ExprPtr chained = std::move(rest);
            if (mutVars.size() == 1) {
                chained = makeLet(boundNames[0], var(resVar), std::move(chained));
            } else {
                for (size_t k = mutVars.size(); k-- > 0; )
                    chained = makeLet(boundNames[k],
                        callE("erlang", "element", 2, two(litInt((long)k + 1), var(resVar))),
                        std::move(chained));
            }
            lr.contBody = makeLet(resVar, std::move(callLoop), std::move(chained));
        }
        auto e = std::make_unique<Expr>(); e->node = std::move(lr);
        return e;
    }
    // Top-level loop statement: continuation is the rest of the enclosing body.
    auto lowerLoopStmt(const std::vector<ast::ExprPtr>& loopBody, const ast::ExprPtr* cond,
                       const std::vector<ast::ExprPtr>& outer, size_t outerStart) -> ExprPtr {
        bool restIsLast = (outerStart + 1 >= outer.size());
        return lowerLoopCore(loopBody, cond, restIsLast,
            [&]{ return lowerBodyFrom(outer, outerStart + 1); });
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
            // Record destructure `let { f, g: { h } } = value`: fields are read
            // by name, not structurally — bind the value then prepend accessors.
            if (auto* rp = std::get_if<ast::RecordPattern>(&le->pattern->kind)) {
                std::string rv = fresh("rec");
                std::vector<std::pair<std::string, ExprPtr>> prefix;
                destructureRecordPattern(rv, *rp, prefix);
                for (auto& [nm, _] : prefix) subst[nm] = nm;
                auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
                for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
                    rest = makeLet(it->first, std::move(it->second), std::move(rest));
                return makeLet(rv, lower(le->value), std::move(rest));
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
        // x.push!(v) / name.upperCase!  → rebind x to the (non-mutating) result.
        if (auto* mc = std::get_if<ast::MethodCall>(&e->kind); mc && mc->mutating && mc->receiver) {
            if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
                auto val = lowerMutatingAsValue(*mc);
                std::string ssa = fresh(id->name);
                subst[id->name] = ssa;
                auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
                return makeLet(ssa, std::move(val), std::move(rest));
            }
        }
        // loop / while → tail-recursive local function threading mutable state.
        if (auto* le = std::get_if<ast::LoopExpr>(&e->kind))
            return lowerLoopStmt(le->body, nullptr, body, i);
        if (auto* we = std::get_if<ast::WhileExpr>(&e->kind))
            return lowerLoopStmt(we->body, &we->condition, body, i);

        // Conditional-assignment merge: a statement-level `if` whose branches
        // reassign tracked vars used AFTER the if must yield those new values
        // (a bare `if` via lower()/lowerBodyScoped would discard them). Emit a
        // case that produces the post-branch state, then rebind + continue.
        if (auto* ie = std::get_if<ast::IfExpr>(&e->kind); ie && !isLast && ie->elifs.empty()) {
            std::unordered_set<std::string> aset;
            for (auto& s : ie->thenBody) collectMutated(s, aset);
            if (ie->elseBody) for (auto& s : *ie->elseBody) collectMutated(s, aset);
            std::vector<std::string> mv;
            for (const auto& v : aset) if (subst.count(v)) mv.push_back(v);
            std::sort(mv.begin(), mv.end());
            if (!mv.empty()) {
                auto snap = subst;
                auto thenS = lowerBranchState(ie->thenBody, mv); subst = snap;
                auto elseS = ie->elseBody ? lowerBranchState(*ie->elseBody, mv) : stateExpr(mv);
                subst = snap;
                auto caseExpr = matchBool(lower(ie->condition), std::move(thenS), std::move(elseS));
                if (mv.size() == 1) {
                    std::string nv = fresh(mv[0]); subst[mv[0]] = nv;
                    return makeLet(nv, std::move(caseExpr), lowerBodyFrom(body, i + 1));
                }
                std::string merged = fresh("M");
                // Capture the extracted names BEFORE lowering the continuation
                // — a loop in the continuation reassigns subst[v] via its own
                // state extraction, so currentName() afterward would differ.
                std::vector<std::string> boundNames;
                for (const auto& v : mv) { std::string nv = fresh(v); subst[v] = nv; boundNames.push_back(nv); }
                ExprPtr chained = lowerBodyFrom(body, i + 1);
                for (size_t k = mv.size(); k-- > 0; )
                    chained = makeLet(boundNames[k],
                        callE("erlang","element",2,two(litInt((long)k + 1), var(merged))),
                        std::move(chained));
                return makeLet(merged, std::move(caseExpr), std::move(chained));
            }
        }

        // `return X if cond` as a non-last statement: return only when cond
        // holds, otherwise fall through to the rest (a plain ReturnExpr lowers
        // its value directly, so `return (cond ? X : ok)` would return
        // unconditionally — wrong). Mirrors the loop-body handling.
        if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind); re && !isLast) {
            if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
                auto retX = std::make_unique<Expr>(); retX->node = Return{lower(ti->expr)};
                return matchBool(lower(ti->condition), std::move(retX),
                                 lowerBodyFrom(body, i + 1));
            }
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
                // A record-destructure or range receiver param can't be a
                // structural Core Erlang pattern (fields are read by NAME, not
                // position, and a range is a materialized list) — bind it to a
                // fresh var and prepend field/element bindings to the body.
                std::vector<std::pair<std::string, ExprPtr>> prefix;
                for (const auto& p : clause.params) {
                    if (!p.name && p.pattern) {
                        auto& pk = (*p.pattern)->kind;
                        if (auto* rp = std::get_if<ast::RecordPattern>(&pk)) {
                            std::string rv = fresh("rec");
                            auto vp = std::make_unique<Pattern>(); vp->kind = PatKind::Var; vp->name = rv;
                            fc.params.push_back(std::move(vp));
                            destructureRecordPattern(rv, *rp, prefix);
                            continue;
                        }
                        if (auto* rgp = std::get_if<ast::RangePattern>(&pk)) {
                            std::string rv = fresh("rng");
                            auto vp = std::make_unique<Pattern>(); vp->kind = PatKind::Var; vp->name = rv;
                            fc.params.push_back(std::move(vp));
                            if (rgp->start) if (auto* sv = std::get_if<ast::VarPattern>(&rgp->start->kind))
                                prefix.push_back({sv->name, callE("erlang","hd",1,one(var(rv)))});
                            if (rgp->end) if (auto* ev = std::get_if<ast::VarPattern>(&rgp->end->kind))
                                prefix.push_back({ev->name, callE("lists","last",1,one(var(rv)))});
                            continue;
                        }
                    }
                    fc.params.push_back(lowerParam(p));
                }
                ExprPtr body = lowerBody(clause.body);
                for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
                    body = makeLet(it->first, std::move(it->second), std::move(body));
                fc.body = std::move(body);
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
        if (!receiverPattern) implicitThisMethods.insert(first.name);
        auto def = lowerFunctionGroup(group, receiverPattern ? "" : "this");
        if (collidingMethods.count(first.name) && !typeName.empty())
            def.name = first.name + "__" + typeName;
        return def;
    }

    // A dispatcher `name/arity` for a colliding method: inspect the
    // receiver's tag (element 1 of arg 0) and forward to `name__Type`.
    // A guard testing that `v` has runtime type `ty`. Primitive types use the
    // matching is_* BIF; everything else is a tagged tuple `{'ty', …}`.
    auto typeGuard(const std::string& ty, ExprPtr v) -> ExprPtr {
        static const std::unordered_map<std::string, const char*> prim = {
            {"Integer","is_integer"}, {"Char","is_integer"}, {"Float","is_float"},
            {"Number","is_number"}, {"String","is_list"}, {"Bool","is_boolean"},
            {"Map","is_map"}, {"List","is_list"},
        };
        auto it = prim.find(ty);
        if (it != prim.end())
            return callE("erlang", it->second, 1, one(std::move(v)));
        // Record/variant: is_tuple(V) and element(1,V) =:= 'ty'. Strict `and`
        // (guard-safe); element/2 in a guard just fails the clause on a non-tuple.
        return callE("erlang", "and", 2, two(
            callE("erlang", "is_tuple", 1, one(clone(v))),
            intrin(Op::Eq, two(callE("erlang", "element", 2, two(litInt(1), std::move(v))),
                               lit(LitKind::Atom, ty)))));
    }
    auto makeDispatcher(const std::string& name, int arity,
                        const std::vector<std::string>& owners) -> FunDef {
        FunDef def; def.name = name; def.arity = arity;
        FunClause fc;
        std::vector<ExprPtr> fwdArgs;
        for (int i = 0; i < arity; i++) {
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Var; pat->name = "_a" + std::to_string(i);
            fc.params.push_back(std::move(pat));
            fwdArgs.push_back(var("_a" + std::to_string(i)));
        }
        // Dispatch on the receiver's runtime type via a guard, so it works for
        // both tagged records/variants AND primitive types (Integer/Float/Map/
        // String/…), whose values aren't tagged tuples — `element(1, N)` on a
        // raw number would crash. For a record type T the guard is
        // `is_tuple(V) and element(1,V) == 'T'`; for a primitive it's the
        // matching `is_*` BIF (all guard-safe).
        Match m;
        m.subjects.push_back(var("_a0"));
        for (const auto& ty : owners) {
            MatchClause mc;
            auto gv = std::make_unique<Pattern>();
            gv->kind = PatKind::Var; gv->name = "_gv";
            mc.patterns.push_back(std::move(gv));
            mc.guard = typeGuard(ty, var("_gv"));
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

// A make-block method's BEAM arity: the AST parameter count, plus 1 for the
// implicit `this` UNLESS the first param is itself the receiver (an `@`/record/
// range pattern). So `count(@[])` is count/1 but `count(pred)` is count/2 even
// though both have one AST param. Grouping and trait-inheritance both key on
// this — a method can be overloaded by BEAM arity across those two forms.
static auto beamArity(const ast::FunctionDef* fd) -> size_t {
    if (!fd || fd->clauses.empty()) return 1;
    const auto& params = fd->clauses[0].params;
    if (params.empty()) return 1; // implicit-this only, e.g. `let reverse = …`
    const auto& p0 = params[0];
    bool receiverPat = !p0.name && p0.pattern &&
        (std::holds_alternative<ast::ThisPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<ast::RangePattern>((*p0.pattern)->kind));
    return receiverPat ? params.size() : params.size() + 1;
}

} // namespace

auto lowerProgram(const ast::Program& prog, const std::string& fileStem,
                  const std::unordered_set<std::string>& preludeFns) -> Module {
    Lowering L;
    L.preludeFns = preludeFns;
    Module mod;
    mod.name = "kex_" + fileStem;

    // Traits: a `make Type, implement: T do ... end` block inherits every
    // default method (a trait `let m = ...` with a body) of each T it doesn't
    // override itself. We splice those inherited defaults into the type's
    // method group so they go through the exact same lowering/dispatch as
    // directly-defined methods (spec/traits.kex: Bot inherits shout/passing?).
    std::unordered_map<std::string, const ast::TraitDef*> traitDefs;
    for (const auto& item : prog.items)
        if (auto* td = std::get_if<std::unique_ptr<ast::TraitDef>>(&item); td && *td)
            traitDefs[(*td)->name] = td->get();
    // Keyed by (name, BEAM arity): a type overrides a trait default only when it
    // defines the same name at the SAME arity (list.kex's count/1 must NOT block
    // inheriting Enumerable's count/2).
    using MethodKey = std::pair<std::string, size_t>;
    auto ownMethodNames = [](const ast::MakeDef& mk) {
        std::set<MethodKey> s;
        auto add = [&](const ast::FunctionDef* f){ if (f) s.insert({f->name, beamArity(f)}); };
        for (const auto& bi : mk.body) {
            if (auto* f = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi)) add(f->get());
            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi))
                if (*vb) for (const auto& vi : (*vb)->items)
                    if (auto* vf = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi)) add(vf->get());
        }
        return s;
    };
    auto inheritedDefaults = [&](const ast::MakeDef& mk) {
        std::vector<const ast::FunctionDef*> out;
        if (mk.implements.empty()) return out;
        auto own = ownMethodNames(mk);
        std::set<MethodKey> added;
        for (const auto& tn : mk.implements) {
            auto it = traitDefs.find(tn);
            if (it == traitDefs.end()) continue;
            for (const auto& ti : it->second->body) {
                auto* f = std::get_if<std::unique_ptr<ast::FunctionDef>>(&ti);
                if (!f || !*f) continue;
                MethodKey key{(*f)->name, beamArity(f->get())};
                if (own.count(key) || added.count(key)) continue;
                // Only default methods (with a body) are inherited; a bare
                // signature `describe : () -> String` carries no clause body.
                if ((*f)->clauses.empty() || (*f)->clauses[0].body.empty()) continue;
                out.push_back(f->get()); added.insert(key);
            }
        }
        return out;
    };

    // Pre-pass: collect record layouts (needed for construction/field access
    // before any body is lowered) and the set of real function/method names
    // (so field accessors that would collide with them are suppressed).
    std::unordered_set<std::string> definedFns;
    for (const auto& item : prog.items) {
        if (auto* rd = std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
            if (*rd) { L.collectRecord(**rd); L.knownTypes.insert((*rd)->name); }
        } else if (auto* mb = std::get_if<std::unique_ptr<ast::MainBlock>>(&item)) {
            if (*mb && (*mb)->synthetic)
                for (const auto& e : (*mb)->body)
                    if (auto* le = std::get_if<ast::LetExpr>(&e->kind))
                        if (le->pattern)
                            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind))
                                L.topLevelConstants.insert(vp->name);
        } else if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fd) {
                definedFns.insert((*fd)->name);
                if (!(*fd)->clauses.empty()) {
                    std::vector<std::string> pnames;
                    for (const auto& p : (*fd)->clauses[0].params)
                        pnames.push_back(p.name ? *p.name : "");
                    if (pnames.empty()) L.zeroArgFns.insert((*fd)->name);
                    L.fnParamNames[(*fd)->name] = std::move(pnames);
                }
            }
        } else if (auto* md = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
            if (!*md) continue;
            std::string typeName = Lowering::simpleTypeName((*md)->target);
            auto collectMethod = [&](const ast::FunctionDef* fd) {
                if (!fd) return;
                definedFns.insert(fd->name);
                const std::string& mn = fd->name;
                if (!mn.empty() && !std::isalnum(static_cast<unsigned char>(mn[0])) && mn[0] != '_')
                    L.overloadedOps.insert(mn);
                if (!typeName.empty()) {
                    auto& owners = L.methodOwners[fd->name];
                    if (std::find(owners.begin(), owners.end(), typeName) == owners.end())
                        owners.push_back(typeName);
                }
            };
            for (const auto& bi : (*md)->body) {
                if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi))
                    collectMethod(mfd->get());
                else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi))
                    if (*vb) for (const auto& vi : (*vb)->items)
                        if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                            collectMethod(vfd->get());
            }
            // Inherited trait defaults count as this type's methods too.
            for (const auto* fd : inheritedDefaults(**md)) collectMethod(fd);
        }
    }
    for (const auto& [name, owners] : L.methodOwners)
        if (owners.size() > 1) L.collidingMethods.insert(name);
    // A `.method` may use the local-apply UFCS fallback iff it names a real
    // local function or a record field accessor.
    L.knownFns = definedFns;
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
            if (!fnGroup.empty() && (fnGroup.front()->name != (*fdp)->name || beamArity(fnGroup.front()) != beamArity(fdp->get()))) flushGroup();
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
                std::string typeName = Lowering::simpleTypeName(node->target);
                std::vector<const ast::FunctionDef*> mgrp;
                auto flushM = [&]{ if (!mgrp.empty()) { mod.functions.push_back(L.lowerMakeGroup(mgrp, typeName)); mgrp.clear(); } };
                auto pushFn = [&](const ast::FunctionDef* fd) {
                    if (!fd) return;
                    if (!mgrp.empty() && (mgrp.front()->name != fd->name || beamArity(mgrp.front()) != beamArity(fd))) flushM();
                    mgrp.push_back(fd);
                };
                for (const auto& bi : node->body) {
                    if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi)) {
                        pushFn(mfd->get());
                    } else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi)) {
                        // `private do ... end` / `public do ... end` — its
                        // methods belong to the same type (visibility is
                        // erased at the BEAM level).
                        if (*vb) for (const auto& vi : (*vb)->items)
                            if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                pushFn(vfd->get());
                    }
                    // TypeAnnotation items in a make block are erased.
                }
                // Splice in trait default methods this type doesn't override.
                for (const auto* fd : inheritedDefaults(*node)) pushFn(fd);
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
                    fc.body = L.withTestSummary(std::move(body));
                    def.clauses.push_back(std::move(fc));
                    mod.functions.push_back(std::move(def));
                    mod.hasMain = true;
                } else {
                    // Bare top-level expression(s) → accumulate for a synthetic main.
                    for (const auto& e : node->body) bareExprs.push_back(&e);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                // Erased: signatures produce nothing; default methods are
                // spliced into each implementing type's method group above.
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
                    auto push = [&](const ast::FunctionDef* fd) {
                        if (!fd) return;
                        if (!grp.empty() && (grp.front()->name != fd->name || beamArity(grp.front()) != beamArity(fd))) flush();
                        grp.push_back(fd);
                    };
                    for (const auto& bi : node->body) {
                        if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi)) {
                            push(mfd->get());
                        } else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi)) {
                            // `private do ... end` / `public do ... end` inside a
                            // module — visibility is erased on BEAM; flatten it.
                            if (*vb) for (const auto& vi : (*vb)->items)
                                if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                    push(vfd->get());
                        } else if (std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&bi) ||
                                   std::get_if<std::unique_ptr<ast::TypeDef>>(&bi)) {
                            // Types/annotations are erased.
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
        fc.body = L.withTestSummary(bareExprs.empty() ? lit(LitKind::Atom, "ok") : chain(0));
        def.clauses.push_back(std::move(fc));
        mod.functions.push_back(std::move(def));
        mod.hasMain = true; mod.mainArity = 0;
    }

    // Cross-type collision dispatchers (bare name → runtime-type dispatch).
    // Generated PER ARITY, including only the owner types that actually have a
    // `name__Type/arity` variant — a method can be overloaded by arity across
    // types (e.g. `count/1` on List vs `count/2` on List+Map), and one dispatcher
    // per arity avoids referencing a variant that doesn't exist.
    for (const auto& name : L.collidingMethods) {
        std::map<int, std::vector<std::string>> ownersByArity;
        std::string prefix = name + "__";
        for (const auto& fn : mod.functions)
            if (fn.name.rfind(prefix, 0) == 0)
                ownersByArity[fn.arity].push_back(fn.name.substr(prefix.size()));
        for (const auto& [arity, owners] : ownersByArity)
            if (arity >= 1) mod.functions.push_back(L.makeDispatcher(name, arity, owners));
    }

    // Field accessors last (so definedFns is fully known).
    for (auto& acc : L.makeAccessors(definedFns)) mod.functions.push_back(std::move(acc));

    // Drop duplicate function definitions (same name + arity), keeping the
    // first. The prelude can legitimately repeat a method across make blocks
    // for the same type (e.g. algebra.kex defines `identity`/`combine` for
    // Integer under both Monoid and Group); erlc rejects duplicate functions.
    {
        std::set<std::pair<std::string, int>> seen;
        std::vector<FunDef> uniq;
        for (auto& f : mod.functions)
            if (seen.insert({f.name, f.arity}).second) uniq.push_back(std::move(f));
        mod.functions = std::move(uniq);
    }
    return mod;
}

} // namespace kex::ir
