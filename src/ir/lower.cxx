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
    std::string sourceFile;
    const SourceLocation* currentLoc = nullptr;
    // Names bound with `let` (immutable) — a mutating `!` call on one is a
    // runtime error matching the walker's behaviour.
    std::unordered_set<std::string> immutableBindings;
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
    // Make-method trailing defaults, indexed by explicit parameter position.
    std::unordered_map<std::string, std::vector<const ast::ExprPtr*>> methodDefaults;
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
    // Qualified Kex module function (`Util.Math.double`) -> local BEAM name.
    // Modules in a single source file still flatten into one BEAM module, so
    // this preserves qualification without a cross-module call.
    std::unordered_map<std::string, std::string> moduleFunctions;
    // Current module path while lowering a module body. Nested module-relative
    // qualified calls (`Router.get` inside `module Http`) resolve against it.
    std::string currentModulePath;
    // Bare imported function name → mangled name. Populated by `using M, only:`
    // inside module bodies so bare calls resolve to the correct cross-module fn.
    std::unordered_map<std::string, std::string> moduleImports;
    // Lexically visible `using Source, as: Alias` mappings. The receiver's
    // first path segment is expanded before qualified module lookup.
    std::unordered_map<std::string, std::string> moduleAliases;
    // Loaded external modules from KexI registry (/load).
    const ExternalModules* externalModules = nullptr;
    // Exact imported call ownership selected by semantic analysis.
    const std::unordered_map<const ast::MethodCall*,
        semantic::ResolvedCallTarget>* resolvedCalls = nullptr;
    // ADT/variant type → its tag names (e.g. Optional → {"Just","None"}). Used
    // by the dispatcher to wildcard-match any variant of a type, not just the
    // type name itself (which isn't set as element(1) on any variant value).
    std::unordered_map<std::string, std::vector<std::string>> typeVariantTags;
    // Nullary variant tags (no payload) — dispatched as atoms, not tuples.
    std::unordered_set<std::string> nullaryVariantTags;
    // Every variant tag of every ADT (flat), and every record static-block
    // member name. A bare use of a static-only name is out of scope — it
    // lives behind its record (Temperature.Fahrenheit) — so it must be a
    // runtime error like the walker's, not a silent constructor tuple.
    std::unordered_set<std::string> variantTagSet;
    std::unordered_set<std::string> staticCtorNames;
    auto staticCtorOutOfScope(const std::string& name) -> bool {
        return staticCtorNames.count(name) && !variantTagSet.count(name)
            && !records.count(name) && !knownFns.count(name);
    }
    // An erlang:error carrying the walker's exact runtime-error text,
    // prefixed with the current source location.
    auto runtimeError(const std::string& msg) -> ExprPtr {
        std::string loc;
        if (currentLoc) loc = std::string(currentLoc->file) + ":"
            + std::to_string(currentLoc->line) + ":"
            + std::to_string(currentLoc->column) + ": ";
        return callE("erlang", "error", 1, one(lit(LitKind::String,
            loc + "runtime error: " + msg)));
    }

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
    // Produce a fresh copy of an atomic (Var/Lit) expression. All call sites
    // pass a result of atomize/atomize_ir, which is guaranteed to be Var or Lit.
    // To work around a GCC-specific issue where a unique_ptr argument appears
    // null due to indeterminate function-parameter initialization ordering, we
    // store the atomic identity (name or lit value) eagerly and reproduce it on
    // demand rather than reading through the pointer at clone-time.
    struct AtomicRef {
        std::string name;
        std::optional<Lit> lit;
        auto get() const -> ExprPtr {
            if (!name.empty()) return var(name);
            if (lit) { auto e = std::make_unique<Expr>(); e->node = *lit; return e; }
            return var("_clone_bug");
        }
    };
    auto snap(const ExprPtr& e) -> AtomicRef {
        if (e) {
            if (auto* v = std::get_if<Var>(&e->node)) return {v->name, std::nullopt};
            if (auto* l = std::get_if<Lit>(&e->node)) return {"", *l};
        }
        return {"_snap_null", std::nullopt};
    }

    auto clone(const ExprPtr& e, const char* site = "?") -> ExprPtr {
        if (!e) throw LowerError(std::string("IR lower: clone of null expr [") + site + "]");
        if (auto* v = std::get_if<Var>(&e->node)) return var(v->name);
        if (auto* l = std::get_if<Lit>(&e->node)) {
            auto out = std::make_unique<Expr>(); out->node = *l; return out;
        }
        if (auto* c = std::get_if<Construct>(&e->node)) {
            if (c->args.empty()) {
                auto out = std::make_unique<Expr>();
                out->node = Construct{c->tag, {}};
                return out;
            }
        }
        throw LowerError(std::string("IR lower: clone of non-atomic expr (index ")
            + std::to_string(e->node.index()) + ") [" + site + "]");
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

    // The kex_intrinsic_fun `*2` helper backing a pair-iterating HOF (a
    // 2-param `{ |k, v| }` block), or nullptr for methods whose 2-param
    // block means something else (reduce's (acc, x), sort's comparator).
    static auto hof2Name(const std::string& m) -> const char* {
        static const std::unordered_map<std::string, const char*> hof2 = {
            {"each","each2"}, {"filter","filter2"}, {"select","filter2"},
            {"map","map2"}, {"count","count2"},
            {"any?","any2"}, {"some?","any2"}, {"all?","all2"},
            {"reject","reject2"}, {"find","find2"},
        };
        auto it = hof2.find(m);
        return it != hof2.end() ? it->second : nullptr;
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
        auto prevLoc = currentLoc;
        currentLoc = &e->location;
        auto r = std::visit([&](const auto& n) -> ExprPtr {
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
                // Uppercase bare name not a known constant → nullary ADT
                // constructor (matching the UpperIdentifier path). Without
                // this, uppercase names in expression context become unbound
                // variables in Core Erlang and erlc rejects them.
                if (!n.name.empty() && std::isupper(static_cast<unsigned char>(n.name[0]))) {
                    if (staticCtorOutOfScope(n.name))
                        return runtimeError("Undefined identifier: " + n.name);
                    auto ex = std::make_unique<Expr>();
                    ex->node = Construct{n.name, {}};
                    return ex;
                }
                if (auto it = subst.find(n.name); it != subst.end())
                    return var(it->second);
                // Bare reference to a defined function keeps the old free-var
                // lowering (erlc rejects it loudly); guards can't call
                // erlang:error, so they keep it too.
                if (m_inGuard || knownFns.count(n.name))
                    return var(n.name);
                // Genuinely unbound: every binding site registers itself in
                // `subst`, so absence means the walker would raise at runtime —
                // emit the same error so `it`-caught failures match exactly.
                return runtimeError("Undefined variable: " + n.name);
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
                if (staticCtorOutOfScope(n.name))
                    return runtimeError("Undefined identifier: " + n.name);
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
                // Division by literal zero → compile-time error with location,
                // matching the walker's runtime error format.
                if (n.op == TokenType::Slash && n.right) {
                    if (auto* il = std::get_if<ast::IntLiteral>(&n.right->kind)) {
                        if (il->value == "0") {
                            std::string loc;
                            if (currentLoc) loc = std::string(currentLoc->file) + ":"
                                + std::to_string(currentLoc->line) + ":"
                                + std::to_string(currentLoc->column) + ": ";
                            return callE("erlang", "error", 1, one(
                                lit(LitKind::String, loc + "runtime error: Division by zero")));
                        }
                    }
                }
                std::vector<Binding> binds;
                auto l = atomize(n.left, binds);
                auto r = atomize(n.right, binds);
                // Operator overloading: if this operator is defined by a make
                // block, dispatch on the LHS at runtime — a tuple (record)
                // uses the user's `'op'/2`, anything else the builtin op
                // (spec/operator_overloading.kex).
                std::string sym = opSymbol(n.op);
                // User == / != (outside guards, which only allow BIFs) go
                // through the coercing runtime eq: a [Char] charlist and a
                // String binary holding the same text ARE equal in Kex.
                auto builtinOp = [&](ExprPtr a, ExprPtr b) -> ExprPtr {
                    Op op = opOf(n.op);
                    if ((op == Op::Eq || op == Op::Neq) && !m_inGuard)
                        return callE("kex_intrinsic_number",
                                     op == Op::Eq ? "eq" : "neq", 2,
                                     two(std::move(a), std::move(b)));
                    auto ex = std::make_unique<Expr>();
                    ex->node = Intrinsic{op, two(std::move(a), std::move(b))};
                    return ex;
                };
                if (!sym.empty() && overloadedOps.count(sym)) {
                    auto lRef = snap(l); auto rRef = snap(r);
                    auto builtin = builtinOp(lRef.get(), rRef.get());
                    std::vector<ExprPtr> ua; ua.push_back(lRef.get()); ua.push_back(rRef.get());
                    auto userCall = std::make_unique<Expr>();
                    userCall->node = Call{"", sym, 2, std::move(ua), false};
                    auto dispatch = matchBool(
                        callE("erlang", "is_tuple", 1, one(lRef.get())),
                        std::move(userCall), std::move(builtin));
                    return wrapLets(binds, std::move(dispatch));
                }
                return wrapLets(binds, builtinOp(std::move(l), std::move(r)));
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
                // a..b → a materialized ascending list. kex_intrinsic_range
                // wraps lists:seq and handles Char endpoints ({'Char', N}
                // bounds produce a [Char], not an [Int]).
                std::vector<Binding> binds;
                auto s = atomize(n.start, binds);
                auto en = atomize(n.end, binds);
                auto ex = std::make_unique<Expr>();
                std::vector<ExprPtr> args;
                args.push_back(std::move(s));
                args.push_back(std::move(en));
                ex->node = Call{"kex_intrinsic_range", "make", 2, std::move(args), false};
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
                // `&name` where name is a LOCAL binding holding a fun (e.g.
                // `let inc = { |x| … }; xs.map(&inc)`) — the reference IS the
                // fun; pass it through (the UFCS path below would look for a
                // method `.inc`, which doesn't exist).
                if (n.kind == ast::ShorthandLambda::Kind::Function &&
                    n.args.empty() && subst.count(n.name))
                    return var(currentName(n.name));
                // `&.method` / `&func` / `&.method(args)` → fun(_sx) ->
                // _sx.method(args). Reusing the UFCS method path means
                // `&digit?` (a builtin) and `&myFn` (a local) both resolve
                // correctly with no special-casing.
                std::string sx = fresh("Sx");
                auto recvAst = std::make_unique<ast::Expr>();
                recvAst->kind = ast::Identifier{sx};
                ast::MethodCall mc;
                mc.receiver = std::move(recvAst);
                mc.method = n.name;
                // Borrow the shorthand's arg exprs for the synthetic call and
                // restore them after — the AST node is shared and re-lowered
                // (multi-clause functions), so it must be left intact.
                auto& lentArgs = const_cast<std::vector<ast::ExprPtr>&>(n.args);
                mc.args = std::move(lentArgs);
                auto snap = subst; subst[sx] = sx;
                auto body = lowerMethodCall(mc);
                subst = snap;
                lentArgs = std::move(mc.args);
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
                for (const auto& p : n.params) { lam.params.push_back(p.name); subst[p.name] = p.name; }
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
            } else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
                std::string srcMod;
                for (size_t i = 0; i < n.module.parts.size(); i++) {
                    if (i) srcMod += ".";
                    srcMod += n.module.parts[i];
                }
                auto saved = moduleImports;
                auto savedAliases = moduleAliases;
                if (n.alias) moduleAliases[*n.alias] = srcMod;
                if (!n.onlyNames.empty()) {
                    for (const auto& name : n.onlyNames) {
                        auto key = srcMod + "." + name;
                        if (auto it = moduleFunctions.find(key); it != moduleFunctions.end())
                            moduleImports[name] = it->second;
                    }
                } else {
                    for (const auto& [key, val] : moduleFunctions)
                        if (key.rfind(srcMod + ".", 0) == 0) {
                            auto bare = key.substr(srcMod.size() + 1);
                            if (bare.find('.') == std::string::npos
                                && std::find(n.exceptNames.begin(), n.exceptNames.end(), bare)
                                    == n.exceptNames.end())
                                moduleImports[bare] = val;
                        }
                }
                auto result = lowerBody(n.body);
                moduleImports = std::move(saved);
                moduleAliases = std::move(savedAliases);
                return result;
            } else {
                throw LowerError(std::string("IR lower: unimplemented expr node ")
                                 + typeid(T).name());
            }
        }, e->kind);
        currentLoc = prevLoc;
        return r;
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
        // Supervisor child helpers are syntax-level builders because their
        // blocks become zero-arity start functions rather than ordinary
        // trailing function arguments.
        if (n.name == "worker" && !knownFns.count("worker")) {
            std::vector<Binding> binds;
            ExprPtr startFn;
            if (n.block && n.args.empty() && n.namedArgs.empty()) {
                startFn = atomize(*n.block, binds);
            } else if (!n.args.empty()) {
                auto* module = std::get_if<ast::UpperIdentifier>(&n.args[0]->kind);
                if (module) {
                    std::string beamModule = "kex_";
                    for (char c : module->name)
                        beamModule += static_cast<char>(
                            std::tolower(static_cast<unsigned char>(c)));
                    std::vector<ExprPtr> startArgs;
                    for (const auto& [name, value] : n.namedArgs) {
                        if (name != "args") continue;
                        if (auto* list = std::get_if<ast::ListExpr>(&value->kind))
                            for (const auto& item : list->elements)
                                startArgs.push_back(lower(item));
                    }
                    Lambda start;
                    start.body = callE(beamModule, "start",
                        static_cast<int>(startArgs.size()), std::move(startArgs));
                    auto fn = std::make_unique<Expr>();
                    fn->node = std::move(start);
                    startFn = atomize_ir(std::move(fn), binds);
                }
            }
            if (startFn)
                return wrapLets(binds, callE("kex_supervisor", "worker", 1,
                                             one(std::move(startFn))));
        }
        if (n.name == "supervisor" && n.block && !knownFns.count("supervisor")) {
            std::vector<Binding> binds;
            ExprPtr strategy = lit(LitKind::Atom, "only_crashed");
            for (const auto& [name, value] : n.namedArgs)
                if (name == "strategy" || name == "restart")
                    strategy = lower(value);
            ExprPtr children;
            if (auto* lambda = std::get_if<ast::Lambda>(&(*n.block)->kind))
                children = lowerBody(lambda->body);
            else
                children = lower(*n.block);
            auto pair = [&](const char* key, ExprPtr value) {
                auto tuple = std::make_unique<Expr>();
                tuple->node = MakeTuple{two(lit(LitKind::Atom, key),
                                            std::move(value))};
                return tuple;
            };
            std::vector<ExprPtr> pairs;
            pairs.push_back(pair("strategy", atomize_ir(std::move(strategy), binds)));
            pairs.push_back(pair("children", atomize_ir(std::move(children), binds)));
            auto list = std::make_unique<Expr>();
            list->node = MakeList{std::move(pairs), std::nullopt};
            auto spec = callE("maps", "from_list", 1, one(std::move(list)));
            return wrapLets(binds, callE("kex_supervisor", "start_link", 1,
                                         one(std::move(spec))));
        }
        // Named args → reorder into the callee's positional slots by param
        // name; then positional args (and a trailing block) fill remaining
        // slots in order, leftovers default to None. Mirrors the string
        // emitter / Evaluator::callFunction (spec/optional_parens_do.kex).
        if (!n.namedArgs.empty()) {
            // Sequence(from: Seed) { |x| next } → an infinite lazy stream
            // ({'Stream', Thunk} — see runtime/src/kex_intrinsic_stream.erl).
            if (n.name == "Sequence" && n.block && n.namedArgs.size() == 1 &&
                n.namedArgs[0].first == "from" && !knownFns.count("Sequence")) {
                std::vector<Binding> binds;
                auto seed = atomize(n.namedArgs[0].second, binds);
                auto fn = atomize(*n.block, binds);
                return wrapLets(binds, callE("kex_intrinsic_stream", "make", 2,
                                             two(std::move(seed), std::move(fn))));
            }
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
            int ar = (int)slots.size();
            ex->node = Call{"", n.name, ar, std::move(slots), false};
            return wrapLets(binds, std::move(ex));
        }
        std::vector<Binding> binds;
        // describe/it: the testing DSL → kex_test, block as a 0-arg fun.
        if ((n.name == "describe" || n.name == "it") && n.block && n.args.size() == 1) {
            auto name = atomize(n.args[0], binds);
            auto fn = atomize(*n.block, binds);
            return wrapLets(binds, callE("kex_test", n.name, 2, two(std::move(name), std::move(fn))));
        }
        // before/after hooks register a 0-arg block in the current describe;
        // an optional :each/:all atom selects its scope.
        if ((n.name == "before" || n.name == "after") && n.block && n.args.size() <= 1) {
            std::vector<ExprPtr> args;
            for (const auto& arg : n.args) args.push_back(atomize(arg, binds));
            auto fn = atomize(*n.block, binds);
            args.push_back(std::move(fn));
            auto arity = static_cast<int>(args.size());
            return wrapLets(binds, callE("kex_test", n.name, arity, std::move(args)));
        }
        // assert(cond[, msg]) — a plain global builtin, not a local function.
        if (n.name == "assert" && !n.args.empty() && !n.block) {
            std::vector<ExprPtr> as;
            for (const auto& a : n.args) as.push_back(atomize(a, binds));
            int ar = static_cast<int>(as.size());
            return wrapLets(binds, callE("kex_test", "assert", ar, std::move(as)));
        }
        std::vector<ExprPtr> args;
        for (const auto& a : n.args) args.push_back(atomize(a, binds));
        // A trailing do-block is passed as the function's last argument.
        if (n.block) args.push_back(atomize(*n.block, binds));
        if (n.name == "self" && args.empty() && !knownFns.count("self"))
            return callE("erlang", "self", 0, {});
        if (n.name == "send" && args.size() == 2 && !knownFns.count("send")) {
            auto message = std::make_unique<Expr>();
            message->node = MakeTuple{three(lit(LitKind::Atom, "kex_msg"),
                                            std::move(args[1]),
                                            callE("erlang", "self", 0, {}))};
            return wrapLets(binds, callE("erlang", "send", 2,
                                         two(std::move(args[0]),
                                             std::move(message))));
        }
        auto ex = std::make_unique<Expr>();
        int arity = static_cast<int>(args.size());
        // A 0-arity function/constant holding a fun (e.g. `let inc = ~add(1)`)
        // called with args: evaluate the thunk, then apply the resulting fun.
        bool zeroArgThunk = !subst.count(n.name) &&
            (zeroArgFns.count(n.name) || topLevelConstants.count(n.name));
        // Capitalized name = ADT constructor with a payload → tagged tuple.
        if (!n.name.empty() && std::isupper(static_cast<unsigned char>(n.name[0]))
            && !zeroArgThunk) {
            if (staticCtorOutOfScope(n.name))
                return wrapLets(binds, runtimeError("Undefined function: " + n.name));
            ex->node = Construct{n.name, std::move(args)};
        }
        else if (zeroArgThunk && (!args.empty() || topLevelConstants.count(n.name))) {
            // A `let` constant holding a fun: `name(...)` resolves the
            // constant, then applies — including `thunk()` with NO args (the
            // walker looks the identifier up and applies the lambda once).
            // A real 0-arity FUNCTION `f()` stays a plain call below: its
            // result is returned as-is, never auto-applied.
            auto thunk = std::make_unique<Expr>();
            thunk->node = Call{"", n.name, 0, {}, false};
            ex->node = CallIndirect{std::move(thunk), std::move(args), false};
        } else if (zeroArgThunk)
            ex->node = Call{"", n.name, 0, {}, false};
        else if (knownFns.count(n.name))
            ex->node = Call{"", n.name, arity, std::move(args), false};
        else if (auto imp = moduleImports.find(n.name); imp != moduleImports.end())
            ex->node = Call{"", imp->second, arity, std::move(args), false};
        else if (subst.count(n.name))
            // A lexical binding (for example a `block` parameter) can hold a
            // callable value. Keep this indirect apply distinct from a truly
            // unknown free function, which must fail only if executed.
            ex->node = CallIndirect{var(currentName(n.name)), std::move(args), false};
        else
            return wrapLets(binds, runtimeError("Undefined function: " + n.name));
        return wrapLets(binds, std::move(ex));
    }

    // Receiver-call and stdlib compatibility resolution. Unknown forms fail
    // explicitly rather than silently emitting a nonexistent function.
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
        // a runtime error (matching the walker's behaviour). Statement-position
        // `!` calls are lowered by the caller as value + rebind.
        if (n.mutating && !m_lowerMutatingAsValue) {
            std::string loc;
            if (currentLoc) {
                loc = std::string(currentLoc->file) + ":" + std::to_string(currentLoc->line) + ":"
                    + std::to_string(currentLoc->column) + ": ";
            }
            return callE("erlang", "error", 1, one(
                lit(LitKind::String, loc + "runtime error: '!' requires a variable binding as the receiver")));
        }
        if (resolvedCalls) {
            auto resolved = resolvedCalls->find(&n);
            if (resolved != resolvedCalls->end()) {
                if (!n.namedArgs.empty())
                    throw LowerError(
                        "IR lower: named args to imported function not yet ported");
                std::vector<Binding> binds;
                std::vector<ExprPtr> args;
                if (resolved->second.passesReceiver)
                    args.push_back(atomize(n.receiver, binds));
                for (const auto& arg : n.args)
                    args.push_back(atomize(arg, binds));
                if (n.block) args.push_back(atomize(*n.block, binds));
                return wrapLets(
                    binds,
                    callE(resolved->second.backendModule,
                          resolved->second.backendFunction,
                          resolved->second.backendArity,
                          std::move(args)));
            }
        }
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
                int ar = static_cast<int>(args.size());
                ex->node = Call{mod, n.method, ar, std::move(args), false};
                return wrapLets(binds, std::move(ex));
            }
        }
        // Erlang.*/Elixir.*/Gleam.* interop: direct BEAM module calls.
        // `Erlang.lists.reverse(xs)` → `call 'lists':'reverse'(xs)`
        // `Elixir.Phoenix.Router.match(c)` → `call 'Elixir.Phoenix.Router':'match'(c)`
        // `Gleam.wisp.serve(h)` → `call 'wisp':'serve'(h)`
        {
            std::vector<std::string> path;
            if (modulePath(*n.receiver, path) && !path.empty() &&
                (path[0] == "Erlang" || path[0] == "Elixir" || path[0] == "Gleam")) {
                std::string mod;
                if (path[0] == "Elixir") {
                    for (size_t i = 1; i < path.size(); i++) {
                        if (i > 1) mod += ".";
                        mod += path[i];
                    }
                    mod = "Elixir." + mod;
                } else {
                    // Erlang/Gleam: lowercase all segments
                    for (size_t i = 1; i < path.size(); i++) {
                        if (i > 1) mod += ".";
                        std::string seg = path[i];
                        for (auto& c : seg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        mod += seg;
                    }
                }
                std::vector<Binding> binds;
                std::vector<ExprPtr> args;
                for (const auto& a : n.args) args.push_back(atomize(a, binds));
                if (n.block) args.push_back(atomize(*n.block, binds));
                int ar = static_cast<int>(args.size());
                auto ex = std::make_unique<Expr>();
                ex->node = Call{mod, n.method, ar, std::move(args), false};
                return wrapLets(binds, std::move(ex));
            }
        }
        // Mock.FS.File(path, content) / Mock.FS.Directory(path) /
        // Mock.FS.clear() — the in-memory test filesystem, mirrored by
        // kex_file's mock registry.
        {
            std::vector<std::string> path;
            if (modulePath(*n.receiver, path) &&
                path.size() == 2 && path[0] == "Mock" && path[1] == "FS") {
                const char* fn = n.method == "File" ? "mock_file"
                               : n.method == "Directory" ? "mock_dir"
                               : n.method == "clear" ? "mock_clear" : nullptr;
                if (!fn) throw LowerError("IR lower: Mock.FS." + n.method + " not supported");
                std::vector<Binding> binds;
                std::vector<ExprPtr> args;
                for (const auto& a : n.args) args.push_back(atomize(a, binds));
                int ar = static_cast<int>(args.size());
                return wrapLets(binds, callE("kex_file", fn, ar, std::move(args)));
            }
        }
        // Mock.IO keeps its buffers in the executing BEAM process, mirroring
        // the interpreter's per-evaluator mock state.
        {
            std::vector<std::string> path;
            if (modulePath(*n.receiver, path) && path.size() == 2 &&
                path[0] == "Mock" && path[1] == "IO") {
                std::vector<Binding> binds;
                if (n.method == "input") {
                    std::vector<ExprPtr> lines;
                    for (const auto& a : n.args)
                        lines.push_back(atomize(a, binds));
                    auto list = std::make_unique<Expr>();
                    list->node = MakeList{std::move(lines), std::nullopt};
                    return wrapLets(binds, callE("kex_io", "mock_input", 1,
                                                 one(std::move(list))));
                }
                const char* fn = n.method == "start" ? "mock_start"
                               : n.method == "output" ? "mock_output"
                               : n.method == "clear" ? "mock_clear"
                               : n.method == "stop" ? "mock_stop" : nullptr;
                if (!fn || !n.args.empty() || !n.namedArgs.empty() || n.block)
                    throw LowerError("IR lower: Mock.IO." + n.method + " not supported");
                return callE("kex_io", fn, 0, {});
            }
        }
        // Stream.Sequence(from: Seed) { |x| next } / Stream.Iterate(Seed) { }
        // are intrinsic constructors until uppercase function declarations are
        // expressible in Kex. Handle them before ordinary module resolution,
        // which intentionally requires interface-known parameter names.
        {
            std::vector<std::string> path;
            if (modulePath(*n.receiver, path) && path.size() == 1 &&
                path[0] == "Stream" &&
                (n.method == "Sequence" || n.method == "Iterate") && n.block) {
                std::vector<Binding> binds;
                ExprPtr seed;
                if (n.namedArgs.size() == 1 && n.namedArgs[0].first == "from")
                    seed = atomize(n.namedArgs[0].second, binds);
                else if (n.namedArgs.empty() && n.args.size() == 1)
                    seed = atomize(n.args[0], binds);
                if (!seed)
                    throw LowerError("IR lower: Stream." + n.method +
                                     " expects one seed argument");
                auto fn = atomize(*n.block, binds);
                return wrapLets(binds, callE("kex_intrinsic_stream", "make", 2,
                                             two(std::move(seed), std::move(fn))));
            }
        }
        // User module function: `Util.double(21)` / `Util.Math.double(21)`.
        // Resolve first against the explicit receiver path, then relative to
        // the enclosing module path so nested modules can refer to siblings.
        {
            std::vector<std::string> path;
            if (!path.empty() || modulePath(*n.receiver, path)) {
                auto pathToString = [&](const std::vector<std::string>& p) {
                    std::string out;
                    for (size_t i = 0; i < p.size(); i++) {
                        if (i) out += ".";
                        out += p[i];
                    }
                    return out;
                };
                std::vector<std::string> candidates;
                auto explicitPath = pathToString(path);
                if (!path.empty()) {
                    if (auto alias = moduleAliases.find(path.front());
                        alias != moduleAliases.end()) {
                        explicitPath = alias->second;
                        for (size_t i = 1; i < path.size(); ++i)
                            explicitPath += "." + path[i];
                    }
                }
                candidates.push_back(explicitPath);
                if (!currentModulePath.empty() && path.size() == 1) {
                    std::string relative = currentModulePath;
                    relative += "." + pathToString(path);
                    candidates.push_back(std::move(relative));
                }
                std::unordered_set<std::string> tried;
                for (const auto& candidate : candidates) {
                    if (candidate.empty() || !tried.insert(candidate).second) continue;
                    if (preludeFns.count(candidate + "." + n.method)) {
                        if (!n.namedArgs.empty())
                            throw LowerError("IR lower: named args to module function not yet ported");
                        std::vector<Binding> binds;
                        std::vector<ExprPtr> args;
                        for (const auto& a : n.args) args.push_back(atomize(a, binds));
                        if (n.block) args.push_back(atomize(*n.block, binds));
                        std::string emitted;
                        for (char c : candidate)
                            emitted += c == '.' ? "__" : std::string(1, c);
                        emitted += "__" + n.method;
                        auto arity = static_cast<int>(args.size());
                        return wrapLets(binds,
                            callE("kex_prelude", emitted, arity, std::move(args)));
                    }
                    auto it = moduleFunctions.find(candidate + "." + n.method);
                    if (it != moduleFunctions.end()) {
                        if (!n.namedArgs.empty())
                            throw LowerError("IR lower: named args to module function not yet ported");
                        std::vector<Binding> binds;
                        std::vector<ExprPtr> args;
                        for (const auto& a : n.args) args.push_back(atomize(a, binds));
                        if (n.block) args.push_back(atomize(*n.block, binds));
                        int ar = static_cast<int>(args.size());
                        return wrapLets(binds, callE("", it->second, ar, std::move(args)));
                    }
                }
                // External loaded modules: BinaryTree.fromList → 'Kex.BinaryTree':fromList
                if (externalModules) {
                    for (const auto& candidate : candidates) {
                        if (candidate.empty()) continue;
                        auto qualKey = candidate + "." + n.method;
                        auto eit = externalModules->exportToBeamFn.find(qualKey);
                        if (eit != externalModules->exportToBeamFn.end()) {
                            if (!n.namedArgs.empty())
                                throw LowerError("IR lower: named args to module function not yet ported");
                            auto ait = externalModules->nameToAtom.find(candidate);
                            if (ait != externalModules->nameToAtom.end()) {
                                std::vector<Binding> binds;
                                std::vector<ExprPtr> args;
                                for (const auto& a : n.args) args.push_back(atomize(a, binds));
                                if (n.block) args.push_back(atomize(*n.block, binds));
                                int ar = static_cast<int>(args.size());
                                return wrapLets(binds,
                                    callE(ait->second, eit->second, ar, std::move(args)));
                            }
                        }
                    }
                }
                if (auto rit = records.find(n.method); rit != records.end()) {
                    std::vector<Binding> binds;
                    const auto& info = rit->second;
                    std::unordered_map<std::string, const ast::ExprPtr*> provided;
                    for (const auto& [name, val] : n.namedArgs)
                        provided[name] = &val;
                    if (n.block) {
                        auto extractMap = [&](const ast::MapExpr* map) {
                            for (const auto& entry : map->entries)
                                if (auto* atom = std::get_if<ast::AtomLiteral>(&entry.key->kind))
                                    provided[atom->name] = &entry.value;
                        };
                        if (auto* lam = std::get_if<ast::Lambda>(&(*n.block)->kind)) {
                            if (!lam->body.empty())
                                if (auto* map = std::get_if<ast::MapExpr>(&lam->body.back()->kind))
                                    extractMap(map);
                        } else if (auto* map = std::get_if<ast::MapExpr>(&(*n.block)->kind)) {
                            extractMap(map);
                        }
                    }
                    std::vector<ExprPtr> fieldArgs;
                    for (size_t i = 0; i < info.fields.size(); i++) {
                        auto pit = provided.find(info.fields[i]);
                        if (pit != provided.end() && *pit->second)
                            fieldArgs.push_back(lower(*pit->second));
                        else if (info.defaults[i])
                            fieldArgs.push_back(lower(*info.defaults[i]));
                        else
                            fieldArgs.push_back(lit(LitKind::None, "none"));
                    }
                    auto ex = std::make_unique<Expr>();
                    ex->node = Construct{n.method, std::move(fieldArgs)};
                    return wrapLets(binds, std::move(ex));
                }
            }
        }
        // Namespace calls on an UpperIdentifier receiver, e.g. IO.printLine.
        if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.receiver->kind)) {
            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            if (auto it = moduleFunctions.find(uid->name + "." + n.method);
                it != moduleFunctions.end()) {
                if (!n.namedArgs.empty())
                    throw LowerError("IR lower: named args to module function not yet ported");
                if (n.block) args.push_back(atomize(*n.block, binds));
                int ar = static_cast<int>(args.size());
                return wrapLets(binds, callE("", it->second, ar, std::move(args)));
            }
            if (externalModules) {
                auto qualKey = uid->name + "." + n.method;
                auto eit = externalModules->exportToBeamFn.find(qualKey);
                if (eit != externalModules->exportToBeamFn.end()) {
                    auto ait = externalModules->nameToAtom.find(uid->name);
                    if (ait != externalModules->nameToAtom.end()) {
                        if (n.block) args.push_back(atomize(*n.block, binds));
                        int ar = static_cast<int>(args.size());
                        return wrapLets(binds,
                            callE(ait->second, eit->second, ar, std::move(args)));
                    }
                }
            }
            auto nsCall = [&](const char* mod, const char* fn) {
                auto ex = std::make_unique<Expr>();
                int ar = static_cast<int>(args.size());
                ex->node = Call{mod, fn, ar, std::move(args), false};
                return wrapLets(binds, std::move(ex));
            };
            if (uid->name == "IO") {
                auto ioCall = [&](const char* fn) {
                    // No-arg print/printLine/printError: print an empty string
                    // (match the walker's behaviour).
                    if (args.empty()) args.push_back(atomize_ir(lit(LitKind::String, ""), binds));
                    auto ex = std::make_unique<Expr>();
                    int ar = static_cast<int>(args.size());
                    ex->node = Call{"kex_io", fn, ar, std::move(args), false};
                    return wrapLets(binds, std::move(ex));
                };
                if (n.method == "getLine" && args.empty())
                    return callE("kex_io", "read_line", 0, {});
                if (n.method == "get" && args.empty())
                    return callE("kex_io", "read_char", 0, {});
                if (n.method == "printLine" || n.method == "putLine")
                    return ioCall("print_line");
                if (n.method == "print" || n.method == "put")
                    return ioCall("print");
                if (n.method == "printError") return ioCall("print_error");
                if (n.method == "warn")       return ioCall("print_error");
                if (n.method == "warning")    return ioCall("print_error");
                if (n.method == "inspect")    return ioCall("inspect");
                // IO.read is still an aspirational API. Preserve the walker's
                // lazy failure semantics: an unused function containing it
                // must not prevent the program from compiling on BEAM.
                if (n.method == "read")
                    return wrapLets(binds, runtimeError("Undefined function: IO.read"));
                throw LowerError("IR lower: IO." + n.method + " not yet ported");
            }
            if (uid->name == "System" && n.method == "exit" && args.size() == 1)
                return wrapLets(binds, callE("erlang", "halt", 1, std::move(args)));
            if (uid->name == "Integer" && n.method == "parse") return nsCall("kex_intrinsic_integer", "integer_parse");
            if (uid->name == "Integer" && n.method == "parsePrefix") return nsCall("kex_intrinsic_integer", "integer_parse_prefix");
            if (uid->name == "Float" && n.method == "parse")   return nsCall("kex_intrinsic_number", "float_parse");
            if (uid->name == "Float" && n.method == "parsePrefix")   return nsCall("kex_intrinsic_number", "float_parse_prefix");
            if (uid->name == "Number" && n.method == "parse")  return nsCall("kex_intrinsic_number", "number_parse");
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
                    if (k == "restart" || k == "strategy") strat = lower(v);
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
            if (uid->name == "Console") {
                if (n.method == "enabled?") return nsCall("kex_intrinsic_console", "enabled");
                return nsCall("kex_intrinsic_console", n.method.c_str());
            }
            if (uid->name == "Math") {
                if (n.method == "sqrt") return nsCall("math", "sqrt");
                if (n.method == "pow" || n.method == "power") return nsCall("math", "pow");
                if (n.method == "sin") return nsCall("math", "sin");
                if (n.method == "cos") return nsCall("math", "cos");
                if (n.method == "tan") return nsCall("math", "tan");
                if (n.method == "log") return nsCall("kex_intrinsic_math", "log");
                if (n.method == "abs") return nsCall("erlang", "abs");
                if (n.method == "floor") return nsCall("erlang", "floor");
                if (n.method == "ceil") return nsCall("erlang", "ceil");
                if (n.method == "PI" || n.method == "pi") return nsCall("math", "pi");
                if (n.method == "E" || n.method == "e") return nsCall("kex_intrinsic_math", "e");
                if (n.method == "atan2") return nsCall("math", "atan2");
                if (n.method == "atan") return nsCall("math", "atan");
                if (n.method == "log10") return nsCall("math", "log10");
                if (n.method == "hypot") return nsCall("kex_intrinsic_math", "hypot");
                if (n.method == "cbrt") return nsCall("kex_intrinsic_math", "cbrt");
                if (n.method == "exp") return nsCall("math", "exp");
                if (n.method == "log2") return nsCall("math", "log2");
                if (n.method == "asin") return nsCall("math", "asin");
                if (n.method == "acos") return nsCall("math", "acos");
                if (n.method == "sinh") return nsCall("math", "sinh");
                if (n.method == "cosh") return nsCall("math", "cosh");
                if (n.method == "tanh") return nsCall("math", "tanh");
            }
            if (uid->name == "Http") {
                if (n.method == "get") return nsCall("kex_http", "get");
                if (n.method == "post") return nsCall("kex_http", "post");
                if (n.method == "put") return nsCall("kex_http", "put");
                if (n.method == "patch") return nsCall("kex_http", "patch");
                if (n.method == "delete") return nsCall("kex_http", "delete");
                if (n.method == "head") return nsCall("kex_http", "head");
                if (n.method == "options") return nsCall("kex_http", "options");
                throw LowerError("IR lower: Http." + n.method + " not yet ported");
            }
            if (uid->name == "File") {
                // File.open(path) do |handle| … end → kex_file:open(Path, Fun).
                // The handle on BEAM is the path itself (kex_file ops are
                // path-based); open returns the block's result, or 'none'
                // when the file doesn't exist — matching the walker.
                if (n.method == "open" && n.block && args.size() == 1) {
                    auto fn = atomize(*n.block, binds);
                    return wrapLets(binds, callE("kex_file", "open", 2,
                                                 two(std::move(args[0]), std::move(fn))));
                }
                // File.open(path, Mode) do |f| … end → a real io-device
                // handle, closed when the block returns.
                if (n.method == "open" && n.block && args.size() == 2) {
                    auto fn = atomize(*n.block, binds);
                    return wrapLets(binds, callE("kex_file", "open", 3,
                        three(std::move(args[0]), std::move(args[1]), std::move(fn))));
                }
                if (n.method == "read") return nsCall("kex_file", "read");
                if (n.method == "write") return nsCall("kex_file", "write");
                if (n.method == "append") return nsCall("kex_file", "append");
                if (n.method == "exists?") return nsCall("kex_file", "exists");
                if (n.method == "delete") return nsCall("kex_file", "delete");
                if (n.method == "lines") return nsCall("kex_file", "lines");
                if (n.method == "feed") return nsCall("kex_file", "feed");
                if (n.method == "size") return nsCall("kex_file", "size");
                if (n.method == "file?") return nsCall("kex_file", "file?");
                if (n.method == "directory?") return nsCall("kex_file", "directory?");
                if (n.method == "basename") return nsCall("kex_file", "basename");
                if (n.method == "dirname") return nsCall("kex_file", "dirname");
                if (n.method == "extension") return nsCall("kex_file", "extension");
                if (n.method == "join") return nsCall("kex_file", "join");
                if (n.method == "absolute") return nsCall("kex_file", "absolute");
                if (n.method == "copy") return nsCall("kex_file", "copy");
                if (n.method == "rename") return nsCall("kex_file", "rename");
                throw LowerError("IR lower: File." + n.method + " not yet ported");
            }
            if (uid->name == "Directory") {
                if (n.method == "current") return nsCall("kex_file", "dir_current");
                if (n.method == "home") return nsCall("kex_file", "dir_home");
                if (n.method == "exists?" || n.method == "directory?")
                    return nsCall("kex_file", "dir_exists?");
                if (n.method == "file?") return nsCall("kex_file", "dir_file?");
                if (n.method == "create") return nsCall("kex_file", "dir_create");
                if (n.method == "delete") return nsCall("kex_file", "dir_delete");
                if (n.method == "deleteAll") return nsCall("kex_file", "dir_delete_all");
                if (n.method == "list") return nsCall("kex_file", "dir_list");
                if (n.method == "files") return nsCall("kex_file", "dir_files");
                if (n.method == "directories") return nsCall("kex_file", "dir_directories");
                throw LowerError("IR lower: Directory." + n.method + " not yet ported");
            }
            // ENV is a real Map value (kex_io:env_map()); its methods are just
            // map operations on that value.
            if (uid->name == "ENV") {
                auto envMap = [&]{ return callE("kex_io", "env_map", 0, {}); };
                // get(key) -> String? : Just(value) if set, None otherwise
                // (matching the tree-walker's Optional-returning semantics).
                if (n.method == "get" && args.size() == 1) {
                    auto keyRef = snap(atomize_ir(std::move(args[0]), binds));
                    auto just = std::make_unique<Expr>();
                    just->node = Construct{"Just", one(
                        callE("maps", "get", 2, two(keyRef.get(), envMap())))};
                    return wrapLets(binds, matchBool(
                        callE("maps", "is_key", 2, two(keyRef.get(), envMap())),
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
                std::string mangled = uid->name + "__" + n.method;
                std::string callName = knownFns.count(mangled) ? mangled : n.method;
                std::vector<ExprPtr> callArgs;
                if (implicitThisMethods.count(callName))
                    callArgs.push_back(lit(LitKind::Atom, uid->name));
                for (auto& a : args) callArgs.push_back(std::move(a));
                int ar = static_cast<int>(callArgs.size());
                auto ex = std::make_unique<Expr>();
                ex->node = Call{"", callName, ar, std::move(callArgs), false};
                return wrapLets(binds, std::move(ex));
            }
            if (auto rit = records.find(n.method); rit != records.end()) {
                const auto& info = rit->second;
                std::unordered_map<std::string, const ast::ExprPtr*> provided;
                for (const auto& [name, val] : n.namedArgs)
                    provided[name] = &val;
                if (n.block) {
                    auto extractMap = [&](const ast::MapExpr* map) {
                        for (const auto& entry : map->entries)
                            if (auto* atom = std::get_if<ast::AtomLiteral>(&entry.key->kind))
                                provided[atom->name] = &entry.value;
                    };
                    if (auto* lam = std::get_if<ast::Lambda>(&(*n.block)->kind)) {
                        if (!lam->body.empty())
                            if (auto* map = std::get_if<ast::MapExpr>(&lam->body.back()->kind))
                                extractMap(map);
                    } else if (auto* map = std::get_if<ast::MapExpr>(&(*n.block)->kind)) {
                        extractMap(map);
                    }
                }
                std::vector<ExprPtr> fieldArgs;
                for (size_t i = 0; i < info.fields.size(); i++) {
                    auto pit = provided.find(info.fields[i]);
                    if (pit != provided.end() && *pit->second)
                        fieldArgs.push_back(lower(*pit->second));
                    else if (info.defaults[i])
                        fieldArgs.push_back(lower(*info.defaults[i]));
                    else
                        fieldArgs.push_back(lit(LitKind::None, "none"));
                }
                auto ex = std::make_unique<Expr>();
                ex->node = Construct{n.method, std::move(fieldArgs)};
                return wrapLets(binds, std::move(ex));
            }
            return wrapLets(binds, runtimeError("Undefined function: " + uid->name + "." + n.method));
        }
        // UFCS method on a value receiver. Atomize the receiver once so
        // methods that use it several times (min/get/count/…) don't
        // re-evaluate it; `ret` wraps the result in that binding.
        std::vector<Binding> rb;
        auto rv_ = atomize_ir(lower(n.receiver), rb);
        // Extract the atomic name so we can produce fresh Var refs on demand
        // without relying on cloning the original unique_ptr (avoids a
        // GCC-specific null-after-move issue on Linux CI).
        auto rvName = std::get_if<Var>(&rv_->node)
            ? std::get_if<Var>(&rv_->node)->name : std::string{};
        auto rvLit = std::get_if<Lit>(&rv_->node)
            ? std::optional<Lit>{*std::get_if<Lit>(&rv_->node)} : std::nullopt;
        auto rv = [&]() -> ExprPtr {
            if (!rvName.empty()) return var(rvName);
            if (rvLit) { auto e = std::make_unique<Expr>(); e->node = *rvLit; return e; }
            return var("_rv_bug");
        };
        auto& m = n.method;
        auto ret = [&](ExprPtr e) { return wrapLets(rb, std::move(e)); };
        auto arg0 = [&]() { return lower(n.args[0]); };

        // `.or(default)` is universal in the walker — Just/Ok unwrap,
        // None/Error yield the default, anything else returns itself — so it
        // can't go through the Optional-owned prelude dispatcher (a plain
        // value receiver would be a function_clause error).
        if (!m_inGuard && m == "or" && n.args.size() == 1 && !n.block
            && !localMethods.count(m))
            return ret(callE("kex_intrinsic_fun", "or_else", 2,
                             two(rv(), atomize_ir(lower(n.args[0]), rb))));
        // External receiver functions take priority over prelude for UFCS.
        // The registry includes only package-declared provider modules here;
        // ordinary module exports never become receiver functions implicitly.
        if (!m_inGuard && externalModules && n.namedArgs.empty()
            && !localMethods.count(m)) {
            auto found = externalModules->receiverFunctions.find(m);
            if (found != externalModules->receiverFunctions.end()) {
                int actualArity = static_cast<int>(n.args.size()) + 1 +
                                  (n.block ? 1 : 0);
                const ExternalModules::ReceiverFunction* match = nullptr;
                for (const auto& candidate : found->second) {
                    if (candidate.beamArity != actualArity) continue;
                    if (match)
                        return ret(runtimeError(
                            "Ambiguous receiver function: " + m + "/" +
                            std::to_string(actualArity)));
                    match = &candidate;
                }
                if (match) {
                    std::vector<ExprPtr> pargs;
                    pargs.push_back(rv());
                    for (const auto& a : n.args)
                        pargs.push_back(atomize_ir(lower(a), rb));
                    if (n.block)
                        pargs.push_back(atomize_ir(lower(*n.block), rb));
                    return ret(callE(match->moduleAtom, match->beamFunction,
                                     actualArity, std::move(pargs)));
                }
            }
        }
        // Prelude stdlib functions → kex_prelude module.
        // Not in a guard (cross-module calls are illegal in Core Erlang guards).
        // Methods that have explicit inline lowerings below must not be
        // short-circuited to kex_prelude — they need the intrinsic path.
        static const std::unordered_set<std::string> inlinedMethods = {
            "or",
            "length", "size", "to", "get",
        };
        if (!m_inGuard && preludeFns.count(m) && !n.block && n.namedArgs.empty()
            && !localMethods.count(m) && !inlinedMethods.count(m)) {
            std::vector<ExprPtr> pargs;
            pargs.push_back(rv());
            for (const auto& a : n.args) pargs.push_back(atomize_ir(lower(a), rb));
            return ret(callE("kex_prelude", m, static_cast<int>(n.args.size()) + 1, std::move(pargs)));
        }
        // `{ |k, v| }` two-param blocks iterate PAIRS: the receiver is a Map
        // or a list of pairs (`m.entries.map { |k, v| … }`), which only the
        // runtime can tell apart — kex_intrinsic_fun's *2 helpers dispatch
        // on is_map. This must intercept before the kex_prelude routing
        // below, whose dispatchers send a Map to the reduce-based List
        // defaults (which can't iterate a map). reduce/sort also take
        // 2-param blocks but are NOT pair iteration — hof2Name excludes them.
        if (!m_inGuard && n.block && n.args.empty() && n.namedArgs.empty()
            && !localMethods.count(m)) {
            auto* lam = std::get_if<ast::Lambda>(&(*n.block)->kind);
            if (lam && lam->params.size() == 2) {
                if (const char* fn2 = hof2Name(m)) {
                    auto fnV = atomize_ir(lower(*n.block), rb);
                    return ret(callE("kex_intrinsic_fun", fn2, 2,
                                     two(rv(), std::move(fnV))));
                }
            }
        }
        // n.times { |i| block } → kex_intrinsic_integer:times(n, fun).
        if (m == "times" && n.block && n.args.empty()) {
            auto fn = atomize_ir(lower(*n.block), rb);
            return ret(callE("kex_intrinsic_integer", "times", 2, two(rv(), std::move(fn))));
        }
        // Any remaining prelude receiver function may accept a trailing block.
        // Its Kex declaration supplies the name; lowering only appends the
        // block as the final argument. Runtime-special block shapes above keep
        // their dedicated intrinsic path.
        if (!m_inGuard && n.block && preludeFns.count(m) && n.namedArgs.empty()
            && !localMethods.count(m)) {
            std::vector<ExprPtr> pargs;
            pargs.push_back(rv());
            for (const auto& a : n.args) pargs.push_back(atomize_ir(lower(a), rb));
            if (n.block) pargs.push_back(atomize_ir(lower(*n.block), rb));
            int ar = static_cast<int>(pargs.size());
            return ret(callE("kex_prelude", m, ar, std::move(pargs)));
        }

        // `case rv of [] -> empty; _ -> nonEmpty end` (Just/None-on-empty).
        auto onEmpty = [&](ExprPtr empty, ExprPtr nonEmpty) -> ExprPtr {
            Match mm; mm.subjects.push_back(rv());
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
            {"sort","lists","sort",0},
            // handle.feed — the File.open block handle is the path on BEAM.
            {"feed","kex_file","feed",0},
            // FileHandle methods (File.open(path, Mode) block handles).
            {"readLine","kex_file","handle_read_line",0},
            {"writeLine","kex_file","handle_write_line",1},
            {"eof?","kex_file","handle_eof?",0},
            {"uniq","lists","usort",0}, {"unique","lists","usort",0},
            {"abs","erlang","abs",0}, {"sqrt","math","sqrt",0},
            // x.inspect returns the pretty-printed representation; IO.inspect
            // is the separate diagnostic form handled above.
            {"inspect","kex_io","inspect_value",0},
            // kex_intrinsic_string handles both String binaries and bare
            // Char codepoints (string:uppercase alone rejects integers).
            {"upperCase","kex_intrinsic_string","upperCase",0}, {"upcase","kex_intrinsic_string","upperCase",0},
            {"lowerCase","kex_intrinsic_string","lowerCase",0}, {"downcase","kex_intrinsic_string","lowerCase",0},
            {"trim","string","trim",0}, {"at","kex_intrinsic_list","list_get",1},
            // digit?/alpha?/space? — guard-inlined form below is the guard fallback.
        };
        // Char predicates in a guard must inline as guard-safe range checks
        // (a `kex_io:is_*` call is an illegal guard expression).
        // Guards can't contain `case`, so use the strict erlang and/or BIFs
        // (guard-safe) rather than the short-circuit Op::And/Or (which emit a
        // case).
        if (m_inGuard && n.args.empty() && !n.block) {
            auto gand = [&](ExprPtr a, ExprPtr b){ return callE("erlang","and",2,two(std::move(a),std::move(b))); };
            auto gor  = [&](ExprPtr a, ExprPtr b){ return callE("erlang","or",2,two(std::move(a),std::move(b))); };
            // A Char is {'Char', Codepoint} — compare its element(2, _)
            // (guard-safe; the receiver is always a Char where these fire).
            auto code = [&]{ return callE("erlang","element",2,two(litInt(2), rv())); };
            auto between = [&](long lo, long hi) {
                return gand(intrin(Op::Gte, two(code(), litInt(lo))),
                            intrin(Op::Lte, two(code(), litInt(hi))));
            };
            if (m == "digit?") return ret(between('0', '9'));
            if (m == "alpha?") return ret(gor(between('A','Z'), between('a','z')));
            if (m == "space?") {
                ExprPtr chk;
                for (int c : {' ', '\t', '\n', '\r'}) {
                    auto eq = intrin(Op::Eq, two(code(), litInt(c)));
                    chk = chk ? gor(std::move(chk), std::move(eq)) : std::move(eq);
                }
                return ret(std::move(chk));
            }
        }
        for (const auto& b : calls)
            if (m == b.method && (int)n.args.size() == b.nargs && !n.block) {
                std::vector<ExprPtr> a;
                // lists:* entries also serve String receivers ([Char] IS
                // String) — coerce a binary to its codepoint list first.
                bool coerce = std::string(b.mod) == "lists" ||
                              std::string(b.fn) == "list_product";
                a.push_back(coerce
                    ? callE("kex_intrinsic_list", "as_list", 1, one(rv()))
                    : rv());
                if (b.nargs == 1) a.push_back(arg0());
                return ret(callE(b.mod, b.fn, b.nargs + 1, std::move(a)));
            }

        if (m == "modulo" && n.args.size() == 1)
            return ret(callE("kex_intrinsic_integer", "modulo", 2,
                             two(rv(), arg0())));
        if (m == "even?" && n.args.empty())
            return ret(intrin(Op::Eq, two(callE("erlang","rem",2,two(rv(),litInt(2))), litInt(0))));
        if (m == "odd?" && n.args.empty())
            return ret(intrin(Op::Neq, two(callE("erlang","rem",2,two(rv(),litInt(2))), litInt(0))));
        if (m == "ok?" && n.args.empty())
            return ret(intrin(Op::Eq, two(callE("erlang","element",2,two(litInt(1),rv())), lit(LitKind::Atom,"Ok"))));
        if (m == "error?" && n.args.empty())
            return ret(intrin(Op::Eq, two(callE("erlang","element",2,two(litInt(1),rv())), lit(LitKind::Atom,"Error"))));
        if (m == "push" && n.args.size() == 1) {
            auto lst = std::make_unique<Expr>();
            lst->node = MakeList{one(arg0()), std::nullopt};
            return ret(callE("erlang","++",2,two(rv(), std::move(lst))));
        }
        // in? guard fallback (lists:member is guard-safe on OTP≥21).
        if (m_inGuard && m == "in?" && n.args.size() == 1)
            return ret(callE("lists","member",2,two(rv(), arg0())));
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
            return ret(callE("erlang", "send", 2, two(rv(), std::move(tup))));
        }
        if ((m == "link" || m == "unlink") && n.args.empty())
            return ret(callE("erlang", m, 1, one(rv())));
        // task.await(timeout: T) / task.await(T) → kex_task:await/2.
        if (m == "await") {
            ExprPtr timeout;
            for (const auto& [k, v] : n.namedArgs)
                if (k == "timeout") timeout = lower(v);
            if (!n.args.empty()) timeout = arg0();
            if (!timeout) timeout = lit(LitKind::Atom, "infinity");
            return ret(callE("kex_task", "await", 2,
                two(rv(), atomize_ir(std::move(timeout), rb))));
        }
        // contains?: a String (binary) needle means substring search
        // (string:find handles binary and charlist receivers alike); a
        // scalar needle means element membership (lists:member).
        if (m == "contains?" && n.args.size() == 1) {
            auto needleRef = snap(atomize_ir(lower(n.args[0]), rb));
            auto substr = intrin(Op::Neq, two(
                callE("string","find",2,two(rv(), needleRef.get())),
                lit(LitKind::Atom, "nomatch")));
            auto substr2 = intrin(Op::Neq, two(
                callE("string","find",2,two(rv(), needleRef.get())),
                lit(LitKind::Atom, "nomatch")));
            auto member = callE("lists","member",2,two(needleRef.get(), rv()));
            return ret(matchBool(callE("erlang","is_binary",1,one(needleRef.get())),
                std::move(substr),
                matchBool(callE("erlang","is_list",1,one(needleRef.get())),
                    std::move(substr2), std::move(member))));
        }
        if (m == "indexOf" && n.args.size() == 1)
            return ret(callE("kex_intrinsic_list","index_of",2,two(arg0(), rv())));
        if (m == "alive?"  && n.args.empty()) return ret(callE("erlang","is_process_alive",1,one(rv())));
        if ((m == "count" || m == "length" || m == "size") && n.args.empty() && !n.block
            && !localMethods.count(m))
            return ret(matchBool(callE("erlang","is_map",1,one(rv())),
                callE("maps","size",1,one(rv())),
                matchBool(callE("erlang","is_binary",1,one(rv())),
                    callE("string","length",1,one(rv())),
                    callE("erlang","length",1,one(rv())))));
        if (m == "first" && n.args.empty() && !localMethods.count("first"))
            return ret(onEmpty(lit(LitKind::None,"none"), justOf(callE("erlang","hd",1,one(rv())))));
        if (m == "rest" && n.args.empty()) {
            auto empty = std::make_unique<Expr>(); empty->node = MakeList{{}, std::nullopt};
            return ret(onEmpty(std::move(empty), callE("erlang","tl",1,one(rv()))));
        }
        // .to(Type) numeric/string conversion (unless a user `to` method).
        if (m == "to" && n.args.size() == 1 && !localMethods.count("to")) {
            std::string ty;
            if (auto* ui = std::get_if<ast::UpperIdentifier>(&n.args[0]->kind)) ty = ui->name;
            if (ty == "String")
                return ret(callE("kex_io","to_string_optional",1,one(rv())));
            if (ty == "Int" || ty == "Integer")
                return ret(callE("kex_intrinsic_number","to_integer",1,one(rv())));
            if (ty == "Float")
                return ret(callE("kex_intrinsic_number","to_float",1,one(rv())));
            // to(List) — ranges (and lists) are already real lists on BEAM.
            if (ty == "List") return ret(justOf(rv()));
        }
        // none?: an Option is None (Kex None → the 'None' atom).
        if (m == "none?" && n.args.empty() && !localMethods.count("none?"))
            return ret(intrin(Op::Eq, two(rv(), lit(LitKind::None, "none"))));
        // get(key, default): list → list_get/3; map → maps:get/3.
        if (m == "get" && n.args.size() == 2 && !localMethods.count("get")) {
            auto kRef = snap(atomize_ir(lower(n.args[0]), rb));
            auto dRef = snap(atomize_ir(lower(n.args[1]), rb));
            auto collectionGet = matchBool(callE("erlang","is_list",1,one(rv())),
                callE("kex_intrinsic_list","list_get",3,three(rv(), kRef.get(), dRef.get())),
                callE("maps","get",3,three(kRef.get(), rv(), dRef.get())));
            // Web.Server.get(path, handler) has the same name and arity as
            // collection get(key, default). Until receiver ownership is
            // carried in typed IR, record/ADT tuple receivers must continue
            // through the prelude dispatcher.
            return ret(matchBool(callE("erlang", "is_tuple", 1, one(rv())),
                callE("kex_prelude", "get", 3,
                      three(rv(), kRef.get(), dRef.get())),
                std::move(collectionGet)));
        }
        // empty?: works for both maps and lists (size 0 / []).
        if (m == "empty?" && n.args.empty() && !localMethods.count("empty?"))
            return ret(matchBool(callE("erlang","is_map",1,one(rv())),
                intrin(Op::Eq, two(callE("maps","size",1,one(rv())), litInt(0))),
                matchBool(callE("erlang","is_binary",1,one(rv())),
                    intrin(Op::Eq, two(callE("erlang","byte_size",1,one(rv())), litInt(0))),
                    intrin(Op::Eq, two(rv(), [&]{ auto e=std::make_unique<Expr>(); e->node=MakeList{{},std::nullopt}; return e; }())))));
        // .or(default): unwrap Just/Some/Ok, else the default.
        if (m == "or" && n.args.size() == 1) {
            auto dflt = arg0();
            Match mm; mm.subjects.push_back(rv());
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
            auto idxRef = snap(atomize_ir(lower(n.args[0]), rb));
            Match inner; inner.subjects.push_back(callE("maps","find",2,two(idxRef.get(), rv())));
            MatchClause ok; auto okp = std::make_unique<Pattern>(); okp->kind = PatKind::Tuple;
            auto okt = std::make_unique<Pattern>(); okt->kind = PatKind::Lit; okt->litKind = LitKind::Atom; okt->litText = "ok";
            auto okv = std::make_unique<Pattern>(); okv->kind = PatKind::Var; okv->name = "_gv";
            okp->args.push_back(std::move(okt)); okp->args.push_back(std::move(okv));
            ok.patterns.push_back(std::move(okp)); ok.body = justOf(var("_gv"));
            MatchClause er; auto erp = std::make_unique<Pattern>(); erp->kind = PatKind::Lit; erp->litKind = LitKind::Atom; erp->litText = "error";
            er.patterns.push_back(std::move(erp)); er.body = lit(LitKind::None, "none");
            inner.clauses.push_back(std::move(ok)); inner.clauses.push_back(std::move(er));
            auto innerE = std::make_unique<Expr>(); innerE->node = std::move(inner);
            return ret(matchBool(callE("erlang","is_list",1,one(rv())),
                callE("kex_intrinsic_list","list_get",2,two(rv(), idxRef.get())),
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
            auto fnVar = [&]() -> ExprPtr {
                if (auto* v = std::get_if<Var>(&fn->node)) return var(v->name);
                if (auto* l = std::get_if<Lit>(&fn->node)) { auto e = std::make_unique<Expr>(); e->node = *l; return e; }
                return clone(fn, "fnVar");
            };
            // rb binds the receiver (outer), binds the block fn (inner).
            auto build = [&](ExprPtr call) { return wrapLets(rb, wrapLets(binds, std::move(call))); };
            // A 2-parameter block (|k, v|) iterates pairs — but the receiver
            // may be a Map OR a list of pairs (`m.entries.map { |k, v| … }`),
            // which only the runtime can tell apart. kex_intrinsic_fun's *2
            // helpers dispatch on is_map and auto-splat tuples for lists.
            bool mapForm = false;
            if (auto* lam = std::get_if<ast::Lambda>(&(*blk)->kind))
                mapForm = lam->params.size() == 2;
            if (mapForm) {
                if (const char* fn2 = hof2Name(m))
                    return build(callE("kex_intrinsic_fun", fn2, 2,
                                       two(rv(), std::move(fn))));
            }
            // Aggregations with a key block: `.sum { |x| k }` maps then sums,
            // `.max { |x| k }` picks the element with the greatest key.
            {
                static const std::unordered_map<std::string, const char*> aggBy = {
                    {"sum","sum_by"}, {"product","product_by"},
                    {"min","minBy"}, {"max","maxBy"},
                };
                if (auto it = aggBy.find(m); it != aggBy.end())
                    return build(callE("kex_intrinsic_list", it->second, 2,
                                       two(rv(), std::move(fn))));
            }
            // List HOFs also serve String receivers ([Char] IS String):
            // as_list coerces a binary to its codepoint list, everything
            // else passes through.
            auto rvList = [&]{ return callE("kex_intrinsic_list", "as_list", 1, one(rv())); };
            if (m == "each")
                return build(callE("lists", "foreach", 2, two(std::move(fn), rvList())));
            if (m == "map")
                return build(callE("lists", "map", 2, two(std::move(fn), rvList())));
            if (m == "filter" || m == "select")
                return build(callE("lists", "filter", 2, two(std::move(fn), rvList())));
            if (m == "all?")
                return build(callE("lists", "all", 2, two(std::move(fn), rvList())));
            if (m == "any?" || m == "some?")
                return build(callE("lists", "any", 2, two(std::move(fn), rvList())));
            if (m == "flatMap" || m == "flat_map")
                return build(callE("lists", "flatmap", 2, two(std::move(fn), rvList())));
            // reject { pred } → keep elements where pred is false.
            if (m == "reject") {
                std::string nx = fresh("Nx");
                Lambda neg; neg.params = {nx};
                neg.body = intrin(Op::Not, one(callE_indirect(fnVar(), one(var(nx)))));
                auto negFn = std::make_unique<Expr>(); negFn->node = std::move(neg);
                auto negV = atomize_ir(std::move(negFn), binds);
                return build(callE("lists", "filter", 2, two(std::move(negV), rvList())));
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
                return build(callE("maps", "map", 2, two(std::move(mapper), rv())));
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
                    callE("maps", "new", 0, {}), rv())));
            }
            // find { pred } → Just(x) for the first match, else None. Built on
            // lists:search/2 which returns {value, X} | false.
            if (m == "find") {
                std::string found = fresh("Found");
                Match mm;
                mm.subjects.push_back(callE("lists", "search", 2, two(std::move(fn), rvList())));
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
            if (m == "count") // count matching a predicate
                return build(callE("erlang", "length", 1,
                    one(callE("lists", "filter", 2, two(std::move(fn), rvList())))));
            // reduce(seed) { |acc, x| } / reduce(seed, fn) → lists:foldl(
            // fun(x,acc)->fn(acc,x), seed, recv). The fn is either a trailing
            // block or the 2nd positional arg (e.g. a curried `~(+)`).
            if ((m == "reduce" || m == "inject") &&
                ((n.block && n.args.size() == 1) || (!n.block && n.args.size() == 2))) {
                auto seed = atomize(n.args[0], binds);
                // Kex block is (acc, elem); Erlang foldl passes (elem, acc) → swap.
                Lambda swap;
                swap.params = {"_e", "_a"};
                swap.body = callE_indirect(fnVar(), two(var("_a"), var("_e")));
                auto swapFn = std::make_unique<Expr>(); swapFn->node = std::move(swap);
                auto swapVar = atomize_ir(std::move(swapFn), binds);
                return build(callE("lists", "foldl", 3,
                    three(std::move(swapVar), std::move(seed), rvList())));
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
            args.push_back(rv());
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            if (auto it = methodDefaults.find(n.method); it != methodDefaults.end())
                for (size_t i = n.args.size(); i < it->second.size(); i++)
                    if (it->second[i]) args.push_back(atomize(*it->second[i], binds));
            int arity = static_cast<int>(args.size());
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
            args.push_back(rv());
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            int arity = static_cast<int>(n.args.size()) + 1;
            return ret(wrapLets(binds, callE("kex_prelude", n.method, arity, std::move(args))));
        }
        // External loaded module methods (UFCS): tree.size → 'Kex.BinaryTree':'Tree.size'(tree)
        return ret(runtimeError("Undefined method: " + n.method));
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
    // This keeps interpolation evaluation order explicit in IR.
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
        // A single literal segment IS the string; otherwise build an iolist
        // (binary literals + to_string charlists both qualify) and flatten it
        // to the UTF-8 binary that a Kex String is on BEAM.
        if (parts.size() == 1 && std::holds_alternative<Lit>(parts[0]->node))
            return std::move(parts[0]);
        auto lst = std::make_unique<Expr>();
        lst->node = MakeList{std::move(parts), std::nullopt};
        return callE("unicode", "characters_to_binary", 1, one(std::move(lst)));
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

    // Payload arity per ADT variant tag (nullary variants lower to atoms and
    // never need display info).
    std::unordered_map<std::string, int> variantArity;
    std::unordered_map<std::string, std::string> variantOwner;

    // Prepend a kex_io:register_display/2 call carrying this module's record
    // layouts and variant arities — only the compiler knows which tuples are
    // records/variants, and the runtime needs that to render
    // `Name { field: value }` / `Tag(args)` instead of plain tuples.
    auto withDisplayInfo(ExprPtr body) -> ExprPtr {
        bool anyVariant = !variantArity.empty();
        if (records.empty() && !anyVariant) return std::move(body);
        auto atomLit = [&](const std::string& s) { return lit(LitKind::Atom, s); };
        auto mapFrom = [&](std::vector<ExprPtr> pairs) {
            auto lst = std::make_unique<Expr>();
            lst->node = MakeList{std::move(pairs), std::nullopt};
            return callE("maps", "from_list", 1, one(std::move(lst)));
        };
        std::vector<ExprPtr> recPairs;
        for (const auto& [name, info] : records) {
            std::vector<ExprPtr> fields;
            for (const auto& f : info.fields) fields.push_back(atomLit(f));
            auto fl = std::make_unique<Expr>();
            fl->node = MakeList{std::move(fields), std::nullopt};
            auto t = std::make_unique<Expr>();
            t->node = MakeTuple{two(atomLit(name), std::move(fl))};
            recPairs.push_back(std::move(t));
        }
        std::vector<ExprPtr> varPairs;
        for (const auto& [tag, ar] : variantArity) {
            auto metadata = std::make_unique<Expr>();
            metadata->node = MakeTuple{two(litInt(ar), atomLit(variantOwner[tag]))};
            auto t = std::make_unique<Expr>();
            t->node = MakeTuple{two(atomLit(tag), std::move(metadata))};
            varPairs.push_back(std::move(t));
        }
        return makeLet(fresh("Disp"),
            callE("kex_io", "register_display", 2,
                  two(mapFrom(std::move(recPairs)), mapFrom(std::move(varPairs)))),
            std::move(body));
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
    // Function-head record patterns also need to preserve non-variable field
    // constraints (`{ x: 0.0 }`, tuple/list patterns). Register the variables
    // before lowering the clause body, then wrap that body in field reads and
    // Core Erlang matches so the constraint is enforced at call time.
    void collectRecordBindings(const ast::RecordPattern& rp, std::vector<std::string>& names) {
        for (const auto& field : rp.fields) {
            if (!field.pattern) {
                names.push_back(field.name);
            } else if (auto* vp = std::get_if<ast::VarPattern>(&(*field.pattern)->kind)) {
                names.push_back(vp->name);
            } else if (auto* nested = std::get_if<ast::RecordPattern>(&(*field.pattern)->kind)) {
                collectRecordBindings(*nested, names);
            }
        }
    }
    auto wrapRecordPattern(const std::string& baseVar, const ast::RecordPattern& rp,
                           ExprPtr body) -> ExprPtr {
        for (auto it = rp.fields.rbegin(); it != rp.fields.rend(); ++it) {
            auto value = fieldAccess(it->name, var(baseVar));
            if (!it->pattern) {
                body = makeLet(it->name, std::move(value), std::move(body));
            } else if (auto* vp = std::get_if<ast::VarPattern>(&(*it->pattern)->kind)) {
                body = makeLet(vp->name, std::move(value), std::move(body));
            } else if (auto* nested = std::get_if<ast::RecordPattern>(&(*it->pattern)->kind)) {
                std::string sub = fresh("rec");
                body = wrapRecordPattern(sub, *nested, std::move(body));
                body = makeLet(sub, std::move(value), std::move(body));
            } else {
                body = makeMatch1(std::move(value), lowerPattern(*it->pattern), std::move(body));
            }
        }
        return body;
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
                subst[pn.name] = pn.name; // pattern vars are binding sites
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
        std::optional<std::string> subjPrev; // outer mapping to restore on exit
        if (n.subjectBinding) {
            subjVar = *n.subjectBinding;
            if (auto it = subst.find(subjVar); it != subst.end()) subjPrev = it->second;
            std::string ssa = subjPrev ? fresh(subjVar) : subjVar;
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
            if (subjPrev) subst[subjVar] = *subjPrev; else subst.erase(subjVar);
            return r;
        }
        return e;
    }

    auto lowerReceive(const ast::ReceiveExpr& n) -> ExprPtr {
        Receive r;
        r.senderVar = n.senderBinding ? *n.senderBinding : fresh("Sndr");
        for (const auto& cl : n.clauses) {
            auto snap = subst;
            if (n.senderBinding) subst[*n.senderBinding] = *n.senderBinding;
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
            immutableBindings.erase(ve->name);
            return makeLet(nv, std::move(val), cont());
        }
        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                auto val = lower(le->value);
                std::string nv = fresh(vp->name); subst[vp->name] = nv;
                immutableBindings.insert(vp->name);
                return makeLet(nv, std::move(val), cont());
            }
            auto val = lower(le->value); auto pat = lowerPattern(le->pattern);
            // Collect destructured names as immutable.
            if (le->pattern)
                if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind))
                    immutableBindings.insert(vp->name);
            return makeMatch1(std::move(val), std::move(pat), cont());
        }
        if (auto* mc = std::get_if<ast::MethodCall>(&e->kind); mc && mc->mutating && mc->receiver)
            if (auto* id = std::get_if<ast::Identifier>(&mc->receiver->kind)) {
                if (immutableBindings.count(id->name)) {
                    std::string loc;
                    if (currentLoc) loc = std::string(currentLoc->file) + ":"
                        + std::to_string(currentLoc->line) + ":"
                        + std::to_string(currentLoc->column) + ": ";
                    auto ex = std::make_unique<Expr>();
                    ex->node = Call{"erlang", "error", 1,
                        one(lit(LitKind::String, loc + "runtime error: Cannot use '!' on immutable binding: " + id->name)), false};
                    return std::move(ex);
                }
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
        auto prevLoc = currentLoc;
        currentLoc = &e->location;

        // let PATTERN = value
        if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
                auto val = lower(le->value);
                std::string ssa = subst.count(vp->name) ? fresh(vp->name) : vp->name;
                subst[vp->name] = ssa;
                immutableBindings.insert(vp->name);
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
            immutableBindings.erase(ve->name);
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
                if (immutableBindings.count(id->name)) {
                    auto err = std::make_unique<Expr>();
                    std::string loc;
                    if (currentLoc) loc = std::string(currentLoc->file) + ":"
                        + std::to_string(currentLoc->line) + ":"
                        + std::to_string(currentLoc->column) + ": ";
                    err->node = Call{"erlang", "error", 1,
                        one(lit(LitKind::String, loc + "runtime error: Cannot use '!' on immutable binding: " + id->name)), false};
                    return std::move(err);
                }
                auto val = lowerMutatingAsValue(*mc);
                std::string ssa = fresh(id->name);
                subst[id->name] = ssa;
                auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
                return makeLet(ssa, std::move(val), std::move(rest));
            }
        }
        // A closure captures the walker's mutable bindings by reference, but
        // BEAM closures only capture values. For finite iteration receiver
        // functions (`each` and `times`) whose callback reassigns an outer
        // variable, lower the iteration to lists:foldl and use the accumulator
        // as explicit mutable state. Once the callback completes, rebind that
        // state in the enclosing SSA scope.
        if (auto* mc = std::get_if<ast::MethodCall>(&e->kind);
            mc && (mc->method == "each" || mc->method == "times") &&
            mc->receiver && mc->args.empty()
            && mc->namedArgs.empty() && mc->block) {
            auto* lam = std::get_if<ast::Lambda>(&(*mc->block)->kind);
            std::unordered_set<std::string> muts;
            if (lam) for (const auto& stmt : lam->body) collectMutated(stmt, muts);
            std::vector<std::string> mutVars;
            for (const auto& v : muts) if (subst.count(v)) mutVars.push_back(v);
            std::sort(mutVars.begin(), mutVars.end());
            bool supportedArity = lam &&
                ((mc->method == "times" && lam->params.size() == 1) ||
                 (mc->method == "each" &&
                  (lam->params.size() == 1 || lam->params.size() == 2)));
            if (supportedArity && !mutVars.empty()) {
                std::vector<Binding> binds;
                auto receiver = atomize_ir(lower(mc->receiver), binds);
                auto snap = subst;
                std::string item = lam->params.size() == 1 ? lam->params[0].name : fresh("EachItem");
                std::string state = fresh("EachState");
                Lambda fold;
                fold.params = {item, state};
                if (lam->params.size() == 1) {
                    subst[lam->params[0].name] = item;
                } else {
                    subst[lam->params[0].name] = lam->params[0].name;
                    subst[lam->params[1].name] = lam->params[1].name;
                }
                auto bindItem = [&](ExprPtr callback) -> ExprPtr {
                    if (lam->params.size() == 1) return callback;
                    auto pair = std::make_unique<Pattern>();
                    pair->kind = PatKind::Tuple;
                    for (const auto& p : lam->params) {
                        auto name = std::make_unique<Pattern>();
                        name->kind = PatKind::Var; name->name = p.name;
                        pair->args.push_back(std::move(name));
                    }
                    return makeMatch1(var(item), std::move(pair), std::move(callback));
                };

                if (mutVars.size() == 1) {
                    subst[mutVars[0]] = state;
                    fold.body = bindItem(lowerLoopBodyFrom(lam->body, 0, "", mutVars,
                        [&] { return stateExpr(mutVars); }));
                } else {
                    auto statePat = std::make_unique<Pattern>();
                    statePat->kind = PatKind::Tuple;
                    for (const auto& v : mutVars) {
                        std::string sv = fresh(v);
                        subst[v] = sv;
                        auto p = std::make_unique<Pattern>();
                        p->kind = PatKind::Var; p->name = sv;
                        statePat->args.push_back(std::move(p));
                    }
                    auto body = lowerLoopBodyFrom(lam->body, 0, "", mutVars,
                        [&] { return stateExpr(mutVars); });
                    fold.body = makeMatch1(var(state), std::move(statePat), bindItem(std::move(body)));
                }
                subst = snap;

                auto foldExpr = std::make_unique<Expr>();
                foldExpr->node = std::move(fold);
                auto foldFn = atomize_ir(std::move(foldExpr), binds);
                auto initial = atomize_ir(stateExpr(mutVars), binds);
                ExprPtr items;
                if (mc->method == "times") {
                    auto last = intrin(Op::Sub,
                        two(std::move(receiver), litInt(1)));
                    items = callE("lists", "seq", 2,
                                  two(litInt(0), std::move(last)));
                } else {
                    items = callE("kex_intrinsic_fun", "items", 1,
                                  one(std::move(receiver)));
                }
                auto result = wrapLets(binds, callE("lists", "foldl", 3,
                    three(std::move(foldFn), std::move(initial), std::move(items))));

                if (mutVars.size() == 1) {
                    std::string nv = fresh(mutVars[0]);
                    subst[mutVars[0]] = nv;
                    auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
                    return makeLet(nv, std::move(result), std::move(rest));
                }
                auto resultPat = std::make_unique<Pattern>();
                resultPat->kind = PatKind::Tuple;
                for (const auto& v : mutVars) {
                    std::string nv = fresh(v);
                    subst[v] = nv;
                    auto p = std::make_unique<Pattern>();
                    p->kind = PatKind::Var; p->name = nv;
                    resultPat->args.push_back(std::move(p));
                }
                auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
                return makeMatch1(std::move(result), std::move(resultPat), std::move(rest));
            }
        }
        // Statement-position if/match whose branches REASSIGN outer vars —
        // thread the mutated state through, exactly like loops do: each
        // branch yields the (possibly reassigned) values, and the code after
        // sees fresh SSA names (`var sql = …; if c sql = sql + x end; …`).
        {
            auto* ie = std::get_if<ast::IfExpr>(&e->kind);
            auto* me = std::get_if<ast::MatchExpr>(&e->kind);
            if (ie || (me && !me->subjectBinding)) {
                std::unordered_set<std::string> muts;
                collectMutated(e, muts);
                std::vector<std::string> mutVars;
                for (const auto& v : muts) if (subst.count(v)) mutVars.push_back(v);
                for (const auto& v : muts) if (subst.count(v)) mutVars.push_back(v);
                std::sort(mutVars.begin(), mutVars.end());
                if (!mutVars.empty()) {
                    std::function<ExprPtr()> yieldState =
                        [&]() -> ExprPtr { return stateExpr(mutVars); };
                    ExprPtr caseE;
                    if (ie) {
                        auto branch = [&](const std::vector<ast::ExprPtr>& bb) {
                            auto snap = subst;
                            auto r = lowerLoopBodyFrom(bb, 0, "", mutVars, yieldState);
                            subst = snap; return r;
                        };
                        ExprPtr elseP = ie->elseBody ? branch(*ie->elseBody)
                                                     : stateExpr(mutVars);
                        for (auto it2 = ie->elifs.rbegin(); it2 != ie->elifs.rend(); ++it2)
                            elseP = matchBool(lower(it2->first), branch(it2->second),
                                              std::move(elseP));
                        caseE = matchBool(lower(ie->condition), branch(ie->thenBody),
                                          std::move(elseP));
                    } else {
                        Match m;
                        m.subjects.push_back(lower(me->subject));
                        for (const auto& cl : me->clauses) {
                            auto snap = subst;
                            MatchClause mc;
                            for (const auto& p : cl.patterns)
                                mc.patterns.push_back(lowerPattern(p));
                            if (cl.guard) mc.guard = lowerGuard(*cl.guard);
                            mc.body = lowerLoopArmU(cl.body, "", mutVars, yieldState);
                            subst = snap;
                            m.clauses.push_back(std::move(mc));
                        }
                        auto x = std::make_unique<Expr>();
                        x->node = std::move(m);
                        caseE = std::move(x);
                    }
                    // Rebind the mutated names to the yielded values.
                    if (mutVars.size() == 1) {
                        std::string nv = fresh(mutVars[0]);
                        subst[mutVars[0]] = nv;
                        auto rest = isLast ? var(nv) : lowerBodyFrom(body, i + 1);
                        return makeLet(nv, std::move(caseE), std::move(rest));
                    }
                    auto pat = std::make_unique<Pattern>();
                    pat->kind = PatKind::Tuple;
                    for (const auto& v : mutVars) {
                        std::string nv = fresh(v);
                        subst[v] = nv;
                        auto vp = std::make_unique<Pattern>();
                        vp->kind = PatKind::Var; vp->name = nv;
                        pat->args.push_back(std::move(vp));
                    }
                    auto rest = isLast ? lit(LitKind::Atom, "ok")
                                       : lowerBodyFrom(body, i + 1);
                    return makeMatch1(std::move(caseE), std::move(pat), std::move(rest));
                }
            }
        }
        // loop / while → tail-recursive local function threading mutable state.
        if (auto* le = std::get_if<ast::LoopExpr>(&e->kind))
            return lowerLoopStmt(le->body, nullptr, body, i);
        if (auto* we = std::get_if<ast::WhileExpr>(&e->kind))
            return lowerLoopStmt(we->body, &we->condition, body, i);

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
            subst[*p.name] = *p.name;
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
                            const std::string& implicitThisName = "",
                            const std::string& nameOverride = "") -> FunDef {
        FunDef def;
        def.name = nameOverride.empty() ? group[0]->name : nameOverride;
        int explicitArity = group[0]->clauses.empty()
            ? 0 : static_cast<int>(group[0]->clauses[0].params.size());
        def.arity = explicitArity + (implicitThisName.empty() ? 0 : 1);
        auto savedModulePath = currentModulePath;
        for (const auto* fn : group) {
            for (const auto& clause : fn->clauses) {
                subst.clear(); // fresh scope per clause
                FunClause fc;
                if (!implicitThisName.empty()) {
                    subst[implicitThisName] = implicitThisName;
                    auto pat = std::make_unique<Pattern>();
                    pat->kind = PatKind::Var; pat->name = implicitThisName;
                    fc.params.push_back(std::move(pat));
                }
                // A record-destructure or range receiver param can't be a
                // structural Core Erlang pattern (fields are read by NAME, not
                // position, and a range is a materialized list) — bind it to a
                // fresh var and prepend field/element bindings to the body.
                std::vector<std::pair<std::string, ExprPtr>> prefix;
                std::vector<std::pair<std::string, const ast::RecordPattern*>> recordPatterns;
                for (const auto& p : clause.params) {
                    if (!p.name && p.pattern) {
                        auto& pk = (*p.pattern)->kind;
                        if (auto* rp = std::get_if<ast::RecordPattern>(&pk)) {
                            std::string rv = fresh("rec");
                            auto vp = std::make_unique<Pattern>(); vp->kind = PatKind::Var; vp->name = rv;
                            fc.params.push_back(std::move(vp));
                            recordPatterns.push_back({rv, rp});
                            std::vector<std::string> names;
                            collectRecordBindings(*rp, names);
                            for (const auto& name : names) subst[name] = name;
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
                for (const auto& [nm, _] : prefix) subst[nm] = nm;
                ExprPtr body = lowerBody(clause.body);
                for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
                    body = makeLet(it->first, std::move(it->second), std::move(body));
                for (auto it = recordPatterns.rbegin(); it != recordPatterns.rend(); ++it)
                    body = wrapRecordPattern(it->first, *it->second, std::move(body));
                fc.body = std::move(body);
                def.clauses.push_back(std::move(fc));
            }
        }
        currentModulePath = std::move(savedModulePath);
        return def;
    }
    auto lowerFunction(const ast::FunctionDef& fn, const std::string& implicitThisName = "")
        -> FunDef {
        return lowerFunctionGroup({&fn}, implicitThisName);
    }

    // ---- Records ----------------------------------------------------------
    void collectRecordLayout(const std::string& name,
                             const std::vector<std::string>& fields) {
        RecordInfo info;
        for (int i = 0; i < static_cast<int>(fields.size()); i++) {
            info.fields.push_back(fields[i]);
            info.defaults.push_back(nullptr);
            fieldAccessors[fields[i]].push_back({name, i + 2});
        }
        records[name] = std::move(info);
    }

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
        // A make-String method matching the receiver with LIST patterns
        // (`myCapitalize(@[x | rest])`) — legal, since [Char] IS String —
        // can't match a binary receiver directly. Detect it so the whole
        // group gets wrapped in an as_list coercion below.
        bool listReceiver = false;
        if (receiverPattern && typeName == "String")
            for (const auto* fn : group)
                for (const auto& cl : fn->clauses)
                    if (!cl.params.empty() && !cl.params[0].name && cl.params[0].pattern)
                        if (auto* tp = std::get_if<ast::ThisPattern>(&(*cl.params[0].pattern)->kind))
                            if (tp->inner && std::holds_alternative<ast::ListPattern>(tp->inner->kind))
                                listReceiver = true;
        auto def = lowerFunctionGroup(group, receiverPattern ? "" : "this");
        if (listReceiver) def = coerceListReceiver(std::move(def));
        if (collidingMethods.count(first.name) && !typeName.empty())
            def.name = first.name + "__" + typeName;
        return def;
    }

    // Rewrap a FunDef whose clauses pattern-match the receiver as a list:
    // one wrapper clause binds raw params, then a multi-subject Match runs
    // the original clause patterns against `as_list(receiver)` (a String
    // binary becomes its [Char] codepoint list; lists pass through).
    auto coerceListReceiver(FunDef def) -> FunDef {
        FunDef out; out.name = def.name; out.arity = def.arity;
        FunClause fc;
        Match m;
        for (int i = 0; i < def.arity; i++) {
            std::string p = "_cr" + std::to_string(i);
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Var; pat->name = p;
            fc.params.push_back(std::move(pat));
            m.subjects.push_back(i == 0
                ? callE("kex_intrinsic_list", "as_list", 1, one(var(p)))
                : var(p));
        }
        for (auto& cl : def.clauses) {
            MatchClause mc;
            mc.patterns = std::move(cl.params);
            mc.guard = std::move(cl.guard);
            mc.body = std::move(cl.body);
            m.clauses.push_back(std::move(mc));
        }
        auto b = std::make_unique<Expr>();
        b->node = std::move(m);
        fc.body = std::move(b);
        out.clauses.push_back(std::move(fc));
        return out;
    }

    // A dispatcher `name/arity` for a colliding method: inspect the
    // receiver's tag (element 1 of arg 0) and forward to `name__Type`.
    // A guard testing that `v` has runtime type `ty`. Primitive types use the
    // matching is_* BIF; everything else is a tagged tuple `{'ty', …}`.
    auto typeGuard(const std::string& ty, ExprPtr v) -> ExprPtr {
        // Char has no is_* entry: a Char is the tagged tuple {'Char', N},
        // so it falls to the record/variant branch below.
        static const std::unordered_map<std::string, const char*> prim = {
            {"Integer","is_integer"}, {"Float","is_float"},
            {"Number","is_number"}, {"String","is_binary"}, {"Bool","is_boolean"},
            {"Map","is_map"}, {"List","is_list"},
            // A range is a real list at BEAM runtime (`a..b` lowers to
            // lists:seq/2 before any method call), so Range-owned prelude
            // methods dispatch on is_list.
            {"Range","is_list"},
            {"Pid","is_pid"}, {"Task","is_pid"}, {"Reference","is_reference"},
        };
        auto it = prim.find(ty);
        if (it != prim.end())
            return callE("erlang", it->second, 1, one(std::move(v)));
        // Record/variant: is_tuple(V) and element(1,V) =:= 'ty'. Strict `and`
        // (guard-safe); element/2 in a guard just fails the clause on a non-tuple.
        auto vRef = snap(v);
        return callE("erlang", "and", 2, two(
            callE("erlang", "is_tuple", 1, one(vRef.get())),
            intrin(Op::Eq, two(callE("erlang", "element", 2, two(litInt(1), vRef.get())),
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
            // ADT types with known variant tags: generate one pattern-match
            // clause per variant instead of a single guard clause. This avoids
            // Core Erlang's strict erlang:and/or in guards (element/2 throws
            // badarg on non-tuples) and uses the correct variant tags rather
            // than the type name (e.g. 'Just' not 'Optional').
            auto vt = typeVariantTags.find(ty);
            if (vt != typeVariantTags.end() && !vt->second.empty()) {
                for (const auto& tag : vt->second) {
                    MatchClause mc;
                    // Nullary variant (no payload) → bare atom on BEAM.
                    if (nullaryVariantTags.count(tag)) {
                        auto pat = std::make_unique<Pattern>();
                        pat->kind = PatKind::Lit;
                        pat->litKind = LitKind::Atom;
                        pat->litText = tag;
                        mc.patterns.push_back(std::move(pat));
                    } else {
                        // Payload variant: tuple pattern {Tag, _}.
                        auto pat = std::make_unique<Pattern>();
                        pat->kind = PatKind::Tuple;
                        auto tagPat = std::make_unique<Pattern>();
                        tagPat->kind = PatKind::Lit;
                        tagPat->litKind = LitKind::Atom;
                        tagPat->litText = tag;
                        pat->args.push_back(std::move(tagPat));
                        auto wild = std::make_unique<Pattern>();
                        wild->kind = PatKind::Wild;
                        pat->args.push_back(std::move(wild));
                        mc.patterns.push_back(std::move(pat));
                    }
                    std::vector<ExprPtr> args;
                    for (int i = 0; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
                    auto call = std::make_unique<Expr>();
                    call->node = Call{"", name + "__" + ty, arity, std::move(args), false};
                    mc.body = std::move(call);
                    m.clauses.push_back(std::move(mc));
                }
            } else if (ty == "Char") {
                // A Char is always the 2-tuple {'Char', _} — match it
                // structurally. (An element(1,_) guard would badarg, not
                // soft-fail, for a primitive receiver reaching this clause.)
                MatchClause mc;
                auto pat = std::make_unique<Pattern>();
                pat->kind = PatKind::Tuple;
                auto tagPat = std::make_unique<Pattern>();
                tagPat->kind = PatKind::Lit;
                tagPat->litKind = LitKind::Atom;
                tagPat->litText = "Char";
                pat->args.push_back(std::move(tagPat));
                auto wild = std::make_unique<Pattern>();
                wild->kind = PatKind::Wild;
                pat->args.push_back(std::move(wild));
                mc.patterns.push_back(std::move(pat));
                std::vector<ExprPtr> args;
                for (int i = 0; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
                auto call = std::make_unique<Expr>();
                call->node = Call{"", name + "__" + ty, arity, std::move(args), false};
                mc.body = std::move(call);
                m.clauses.push_back(std::move(mc));
            } else {
                // Primitive/record type: use a guard-based clause.
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
        }
        // [Char] IS String: a method owned by List but not by String must
        // still take a String receiver — coerce the binary to its codepoint
        // list and forward to the List impl. This clause goes FIRST: the
        // record/variant owner clauses guard with element(1, _) which erlc
        // does not soft-fail for a binary falling through to them.
        bool hasString = std::find(owners.begin(), owners.end(), "String") != owners.end();
        bool hasList = std::find(owners.begin(), owners.end(), "List") != owners.end();
        if (hasList && !hasString) {
            MatchClause mc;
            auto gv = std::make_unique<Pattern>();
            gv->kind = PatKind::Var; gv->name = "_gv";
            mc.patterns.push_back(std::move(gv));
            mc.guard = callE("erlang", "is_binary", 1, one(var("_gv")));
            std::vector<ExprPtr> args;
            args.push_back(callE("kex_intrinsic_list", "as_list", 1, one(var("_a0"))));
            for (int i = 1; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
            auto call = std::make_unique<Expr>();
            call->node = Call{"", name + "__List", arity, std::move(args), false};
            mc.body = std::move(call);
            m.clauses.insert(m.clauses.begin(), std::move(mc));
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
                  const std::unordered_set<std::string>& preludeFns,
                  const std::string& sourcePath,
                  const ExternalModules* externals,
                  const std::vector<ExternalRecordLayout>* externalRecords,
                  const std::unordered_map<const ast::MethodCall*,
                      semantic::ResolvedCallTarget>* resolvedCalls)
    -> Module {
    Lowering L;
    L.preludeFns = preludeFns;
    L.sourceFile = sourcePath;
    L.externalModules = externals;
    L.resolvedCalls = resolvedCalls;
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
    // Factored into per-kind lambdas so the flattened module items go
    // through the exact same collection as top-level items.
    std::unordered_set<std::string> definedFns;
    std::unordered_set<std::string> staticMethodNames; // record static blocks
    auto preRecord = [&](const ast::RecordDef& rd) {
        L.collectRecord(rd);
        L.knownTypes.insert(rd.name);
        if (rd.staticBlock)
            for (const auto& sf : rd.staticBlock->functions)
                if (sf) {
                    definedFns.insert(rd.name + "__" + sf->name);
                    staticMethodNames.insert(sf->name);
                }
    };
    if (externalRecords)
        for (const auto& record : *externalRecords) {
            L.collectRecordLayout(record.name, record.fields);
            L.knownTypes.insert(record.name);
        }
    auto preFn = [&](const ast::FunctionDef& fd) {
        definedFns.insert(fd.name);
        if (!fd.clauses.empty()) {
            std::vector<std::string> pnames;
            for (const auto& p : fd.clauses[0].params)
                pnames.push_back(p.name ? *p.name : "");
            if (pnames.empty()) L.zeroArgFns.insert(fd.name);
            L.fnParamNames[fd.name] = std::move(pnames);
        }
    };
    auto preType = [&](const ast::TypeDef& td) {
        if (!td.variants) return;
        // Transparent type alias: single bare TypeName (e.g. `type FilePath = String`)
        // — skip variant-tag registration entirely.
        if (td.variants->size() == 1) {
            auto* tn = std::get_if<ast::TypeName>(&(*td.variants)[0]->kind);
            if (tn) return;
        }
        for (const auto& v : *td.variants) {
            auto t = Lowering::simpleTypeName(v);
            if (t.empty()) continue;
            L.variantTagSet.insert(t);
            if (auto* g = std::get_if<ast::GenericType>(&v->kind))
                L.variantArity[t] = static_cast<int>(g->args.size());
            else
                L.variantArity[t] = 0;
            L.variantOwner[t] = td.name;
        }
    };
    auto preMake = [&](const ast::MakeDef& md) {
        std::string typeName = Lowering::simpleTypeName(md.target);
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
            if (!fd->clauses.empty()) {
                std::vector<const ast::ExprPtr*> defaults;
                for (const auto& p : fd->clauses[0].params)
                    defaults.push_back(p.defaultValue ? &*p.defaultValue : nullptr);
                if (std::any_of(defaults.begin(), defaults.end(), [](auto* d) { return d != nullptr; }))
                    L.methodDefaults[fd->name] = std::move(defaults);
            }
        };
        for (const auto& bi : md.body) {
            if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi))
                collectMethod(mfd->get());
            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi))
                if (*vb) for (const auto& vi : (*vb)->items)
                    if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                        collectMethod(vfd->get());
        }
        // Inherited trait defaults count as this type's methods too.
        for (const auto* fd : inheritedDefaults(md)) collectMethod(fd);
    };
    std::function<void(const ast::ModuleDef&)> preModule;
    preModule = [&](const ast::ModuleDef& module) {
        const auto& path = module.name;
        const std::string mangledPrefix = [&] {
            std::string out;
            for (char c : path) out += c == '.' ? "__" : std::string(1, c);
            return out;
        }();
        auto preModuleFn = [&](const ast::FunctionDef* fd) {
            if (!fd) return;
            const std::string emitted = mangledPrefix + "__" + fd->name;
            definedFns.insert(emitted);
            L.moduleFunctions[path + "." + fd->name] = emitted;
        };
        for (const auto& item : module.body) {
            if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
                preModuleFn(fd->get());
            else if (auto* rd = std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
                if (*rd) preRecord(**rd);
            } else if (auto* md = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                if (*md) preMake(**md);
            } else if (auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&item)) {
                if (*td) preType(**td);
            } else if (auto* cb = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&item)) {
                if (*cb) for (const auto& ci : (*cb)->items) {
                    if (auto* cfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&ci))
                        preModuleFn(cfd->get());
                    else if (auto* crd = std::get_if<std::unique_ptr<ast::RecordDef>>(&ci)) {
                        if (*crd) preRecord(**crd);
                    } else if (auto* cmd = std::get_if<std::unique_ptr<ast::MakeDef>>(&ci)) {
                        if (*cmd) preMake(**cmd);
                    } else if (auto* ctd = std::get_if<std::unique_ptr<ast::TypeDef>>(&ci)) {
                        if (*ctd) preType(**ctd);
                    }
                }
            }
            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
                if (*vb) for (const auto& vi : (*vb)->items)
                    if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                        preModuleFn(vfd->get());
            } else if (auto* ed = std::get_if<std::unique_ptr<ast::ExportDecl>>(&item)) {
                if (*ed) {
                    std::string srcMod;
                    for (size_t i = 0; i < (*ed)->module.parts.size(); i++) {
                        if (i) srcMod += ".";
                        srcMod += (*ed)->module.parts[i];
                    }
                    auto alias = (*ed)->alias.value_or((*ed)->module.parts.back());
                    auto nsPath = path + "." + alias;
                    for (const auto& [key, val] : L.moduleFunctions) {
                        if (key.rfind(srcMod + ".", 0) != 0) continue;
                        auto bare = key.substr(srcMod.size() + 1);
                        if (bare.find('.') != std::string::npos) continue;
                        if (!(*ed)->onlyNames.empty()
                            && std::find((*ed)->onlyNames.begin(), (*ed)->onlyNames.end(), bare)
                                == (*ed)->onlyNames.end()) continue;
                        if (std::find((*ed)->exceptNames.begin(), (*ed)->exceptNames.end(), bare)
                            != (*ed)->exceptNames.end()) continue;
                        L.moduleFunctions[nsPath + "." + bare] = val;
                    }
                }
            } else if (auto* child = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*child) preModule(**child);
            }
        }
    };
    for (const auto& item : prog.items) {
        if (auto* rd = std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
            if (*rd) preRecord(**rd);
        } else if (auto* mb = std::get_if<std::unique_ptr<ast::MainBlock>>(&item)) {
            if (*mb && (*mb)->synthetic)
                for (const auto& e : (*mb)->body)
                    if (auto* le = std::get_if<ast::LetExpr>(&e->kind))
                        if (le->pattern)
                            if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind))
                                L.topLevelConstants.insert(vp->name);
        } else if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fd) preFn(**fd);
        } else if (auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&item)) {
            if (*td) preType(**td);
        } else if (auto* md = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
            if (*md) preMake(**md);
        } else if (auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
            if (*module) preModule(**module);
        }
    }
    for (const auto& [name, owners] : L.methodOwners)
        if (owners.size() > 1) L.collidingMethods.insert(name);
    // A `.method` may use the local-apply UFCS fallback iff it names a real
    // local function or a record field accessor.
    L.knownFns = definedFns;
    L.localMethods = definedFns;
    L.staticCtorNames = staticMethodNames;
    for (const auto& n : staticMethodNames) L.localMethods.insert(n);
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
                // Record static-block functions emit as top-level functions so
                // `RecordName.method(...)` namespace calls find them.
                if (node && node->staticBlock) {
                    for (const auto& sf : node->staticBlock->functions) {
                        if (!sf) continue;
                        std::vector<const ast::FunctionDef*> tmp{sf.get()};
                        std::string mangled = node->name + "__" + sf->name;
                        mod.functions.push_back(L.lowerFunctionGroup(tmp, "", mangled));
                    }
                }
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
                    for (const auto& p : node->params)
                        if (p.name) L.subst[*p.name] = *p.name;
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
                    fc.body = L.withTestSummary(L.withDisplayInfo(std::move(body)));
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
                // Types/annotations are erased, but collect variant tags first
                // so dispatchers can wildcard-match them (see the nested module
                // handler below for the same logic).
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                    if (node && node->variants) {
                        std::vector<std::string> tags;
                        for (const auto& v : *node->variants) {
                            auto t = Lowering::simpleTypeName(v);
                            if (!t.empty()) {
                                tags.push_back(t);
                                if (std::holds_alternative<ast::TypeName>(v->kind))
                                    L.nullaryVariantTags.insert(t);
                            }
                        }
                        if (!tags.empty() && tags.size() >= 2)
                            L.typeVariantTags[node->name] = std::move(tags);
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) {
                    std::vector<const ast::FunctionDef*> grp;
                    auto flush = [&]{
                        if (!grp.empty()) {
                            auto it = L.moduleFunctions.find(L.currentModulePath + "." + grp.front()->name);
                            mod.functions.push_back(L.lowerFunctionGroup(grp, "",
                                it == L.moduleFunctions.end() ? grp.front()->name : it->second));
                            grp.clear();
                        }
                    };
                    auto push = [&](const ast::FunctionDef* fd) {
                        if (!fd) return;
                        if (!grp.empty() && (grp.front()->name != fd->name || beamArity(grp.front()) != beamArity(fd))) flush();
                        grp.push_back(fd);
                    };
                    auto emitMake = [&](const ast::MakeDef* mk) {
                        if (!mk) return;
                        std::string typeName = Lowering::simpleTypeName(mk->target);
                        std::vector<const ast::FunctionDef*> methods;
                        auto flushMethods = [&] {
                            if (!methods.empty()) {
                                mod.functions.push_back(L.lowerMakeGroup(methods, typeName));
                                methods.clear();
                            }
                        };
                        auto pushMethod = [&](const ast::FunctionDef* fd) {
                            if (!fd) return;
                            if (!methods.empty() && (methods.front()->name != fd->name ||
                                beamArity(methods.front()) != beamArity(fd))) flushMethods();
                            methods.push_back(fd);
                        };
                        for (const auto& mi : mk->body) {
                            if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&mi))
                                pushMethod(fd->get());
                            else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&mi))
                                if (*vb) for (const auto& vi : (*vb)->items)
                                    if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                        pushMethod(fd->get());
                        }
                        for (const auto* fd : inheritedDefaults(*mk)) pushMethod(fd);
                        flushMethods();
                    };
                    std::function<void(const ast::ModuleDef&)> lowerModuleBody;
                    lowerModuleBody = [&](const ast::ModuleDef& m) {
                        auto savedModulePath = L.currentModulePath;
                        L.currentModulePath = m.name;
                        auto savedImports = L.moduleImports;
                        auto savedAliases = L.moduleAliases;
                        auto prefix = m.name + ".";
                        for (const auto& [key, val] : L.moduleFunctions)
                            if (key.rfind(prefix, 0) == 0) {
                                auto bare = key.substr(prefix.size());
                                if (bare.find('.') == std::string::npos)
                                    L.moduleImports[bare] = val;
                            }
                        for (const auto& bi : m.body) {
                            if (auto* ub = std::get_if<std::unique_ptr<ast::UsingBlock>>(&bi)) {
                                if (!*ub) continue;
                                std::string srcMod;
                                for (size_t i = 0; i < (*ub)->module.parts.size(); i++) {
                                    if (i) srcMod += ".";
                                    srcMod += (*ub)->module.parts[i];
                                }
                                if ((*ub)->alias) L.moduleAliases[*(*ub)->alias] = srcMod;
                                auto importName = [&](const std::string& name) {
                                    auto key = srcMod + "." + name;
                                    if (auto it = L.moduleFunctions.find(key); it != L.moduleFunctions.end())
                                        L.moduleImports[name] = it->second;
                                };
                                if (!(*ub)->onlyNames.empty()) {
                                    for (const auto& name : (*ub)->onlyNames) importName(name);
                                } else if ((*ub)->body.empty()) {
                                    for (const auto& [key, val] : L.moduleFunctions)
                                        if (key.rfind(srcMod + ".", 0) == 0) {
                                            auto bare = key.substr(srcMod.size() + 1);
                                            if (bare.find('.') == std::string::npos
                                                && std::find((*ub)->exceptNames.begin(),
                                                             (*ub)->exceptNames.end(), bare)
                                                    == (*ub)->exceptNames.end())
                                                L.moduleImports[bare] = val;
                                        }
                                }
                                continue;
                            } else if (auto* mfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi)) {
                                push(mfd->get());
                            } else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi)) {
                                if (*vb) for (const auto& vi : (*vb)->items)
                                    if (auto* vfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                                        push(vfd->get());
                            } else if (auto* mk = std::get_if<std::unique_ptr<ast::MakeDef>>(&bi)) {
                                flush();
                                emitMake(mk->get());
                            } else if (auto* cb = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&bi)) {
                                if (*cb) for (const auto& ci : (*cb)->items) {
                                    if (auto* cfd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&ci)) {
                                        push(cfd->get());
                                    } else if (auto* cmd = std::get_if<std::unique_ptr<ast::MakeDef>>(&ci)) {
                                        flush();
                                        emitMake(cmd->get());
                                    }
                                }
                            } else if (auto* child = std::get_if<std::unique_ptr<ast::ModuleDef>>(&bi)) {
                                flush();
                                if (*child) lowerModuleBody(**child);
                            } else if (std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&bi) ||
                                       std::get_if<std::unique_ptr<ast::TypeDef>>(&bi)) {
                                if (auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&bi)) {
                                    if (*td && (*td)->variants) {
                                        // Skip transparent type aliases (single bare TypeName).
                                        bool transparentAlias =
                                            (*td)->variants->size() == 1 &&
                                            std::holds_alternative<ast::TypeName>(
                                                (*(*td)->variants)[0]->kind);
                                        if (!transparentAlias) {
                                            std::vector<std::string> tags;
                                            for (const auto& v : *(*td)->variants) {
                                                auto t = Lowering::simpleTypeName(v);
                                                if (!t.empty()) {
                                                    tags.push_back(t);
                                                    if (std::holds_alternative<ast::TypeName>(v->kind))
                                                        L.nullaryVariantTags.insert(t);
                                                }
                                            }
                                            if (!tags.empty())
                                                L.typeVariantTags[(*td)->name] = std::move(tags);
                                        }
                                    }
                                }
                            } else {
                                flush();
                            }
                        }
                        flush();
                        L.moduleImports = std::move(savedImports);
                        L.moduleAliases = std::move(savedAliases);
                        L.currentModulePath = std::move(savedModulePath);
                    };
                    lowerModuleBody(*node);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                if (!node) return;
                std::string srcMod;
                for (size_t i = 0; i < node->module.parts.size(); i++) {
                    if (i) srcMod += ".";
                    srcMod += node->module.parts[i];
                }
                if (node->alias) L.moduleAliases[*node->alias] = srcMod;
                if (!node->onlyNames.empty()) {
                    for (const auto& name : node->onlyNames) {
                        auto key = srcMod + "." + name;
                        if (auto it = L.moduleFunctions.find(key); it != L.moduleFunctions.end())
                            L.moduleImports[name] = it->second;
                    }
                } else {
                    for (const auto& [key, val] : L.moduleFunctions)
                        if (key.rfind(srcMod + ".", 0) == 0) {
                            auto bare = key.substr(srcMod.size() + 1);
                            if (bare.find('.') == std::string::npos
                                && std::find(node->exceptNames.begin(),
                                             node->exceptNames.end(), bare)
                                    == node->exceptNames.end())
                                L.moduleImports[bare] = val;
                        }
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
        fc.body = L.withTestSummary(L.withDisplayInfo(
            bareExprs.empty() ? lit(LitKind::Atom, "ok") : chain(0)));
        def.clauses.push_back(std::move(fc));
        mod.functions.push_back(std::move(def));
        mod.hasMain = true; mod.mainArity = 0;
    }
    // Pure-declaration program (no main block, no bare exprs): synthesize an
    // empty main/0 so `kex -R file.kex` runs it as the no-op it is on the
    // walker, instead of erl dying with undef on the missing main.
    if (!mod.hasMain) {
        FunDef def; def.name = "main"; def.arity = 0;
        FunClause fc; fc.body = lit(LitKind::Atom, "ok");
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
        for (const auto& [arity, owners] : ownersByArity) {
            if (arity < 1) continue;
            // If every owner is an ADT type (has known variant tags), the
            // clauses from the mangled functions have distinct top-level
            // patterns — merge them into one function and skip the guard-
            // based dispatcher entirely. Pattern matching handles dispatch
            // natively, avoiding Core Erlang guard short-circuit issues.
            bool allAdt = !owners.empty();
            for (const auto& o : owners) {
                if (!L.typeVariantTags.count(o)) { allAdt = false; break; }
            }
            if (allAdt) {
                FunDef merged; merged.name = name; merged.arity = arity;
                for (size_t i = 0; i < mod.functions.size(); ) {
                    auto& f = mod.functions[i];
                    std::string prefix = name + "__";
                    if (f.name.rfind(prefix, 0) == 0 && f.arity == arity) {
                        for (auto& c : f.clauses)
                            merged.clauses.push_back(std::move(c));
                        // Remove the mangled function (avoid duplicate and keep
                        // mod.functions clean).
                        mod.functions.erase(mod.functions.begin() + i);
                    } else {
                        ++i;
                    }
                }
                mod.functions.push_back(std::move(merged));
            } else {
                mod.functions.push_back(L.makeDispatcher(name, arity, owners));
            }
        }
    }

    // Field accessors last (so definedFns is fully known).
    for (auto& acc : L.makeAccessors(definedFns)) mod.functions.push_back(std::move(acc));

    // Merge duplicate function definitions (same name + arity) by concatenating
    // their clauses. The prelude legitimately repeats a method across make blocks
    // for different types with different clause patterns (e.g. optional.kex
    // defines `or` for both Optional<X> and Result<X,E>); erlc needs a single
    // function with all the clauses unified, not two conflicting definitions.
    {
        std::map<std::pair<std::string, int>, FunDef> merged;
        for (auto& f : mod.functions) {
            auto key = std::make_pair(f.name, f.arity);
            auto it = merged.find(key);
            if (it == merged.end()) {
                merged.emplace(key, std::move(f));
            } else {
                auto& existing = it->second;
                existing.clauses.insert(existing.clauses.end(),
                    std::make_move_iterator(f.clauses.begin()),
                    std::make_move_iterator(f.clauses.end()));
            }
        }
        mod.functions.clear();
        for (auto& [_, f] : merged) mod.functions.push_back(std::move(f));
    }
    mod.typeVariantTags = L.typeVariantTags;
    return mod;
}

namespace {

auto rewriteModuleCalls(ExprPtr& expr,
                        const std::unordered_map<std::string, std::pair<std::string, std::string>>& targets)
    -> void {
    if (!expr) return;
    std::visit([&](auto& node) {
        using T = std::decay_t<decltype(node)>;
        auto visit = [&](ExprPtr& child) { rewriteModuleCalls(child, targets); };
        if constexpr (std::is_same_v<T, Call>) {
            if (node.module.empty()) {
                if (auto it = targets.find(node.name); it != targets.end()) {
                    node.module = it->second.first;
                    node.name = it->second.second;
                }
            }
            for (auto& arg : node.args) visit(arg);
        } else if constexpr (std::is_same_v<T, Intrinsic>) {
            for (auto& arg : node.args) visit(arg);
        } else if constexpr (std::is_same_v<T, CallIndirect>) {
            visit(node.callee); for (auto& arg : node.args) visit(arg);
        } else if constexpr (std::is_same_v<T, Let>) {
            visit(node.value); visit(node.body);
        } else if constexpr (std::is_same_v<T, Seq>) {
            for (auto& item : node.exprs) visit(item);
        } else if constexpr (std::is_same_v<T, Match>) {
            for (auto& subject : node.subjects) visit(subject);
            for (auto& clause : node.clauses) {
                if (clause.guard) visit(*clause.guard);
                visit(clause.body);
            }
        } else if constexpr (std::is_same_v<T, Construct>) {
            for (auto& arg : node.args) visit(arg);
        } else if constexpr (std::is_same_v<T, MakeTuple>) {
            for (auto& item : node.elements) visit(item);
        } else if constexpr (std::is_same_v<T, MakeList>) {
            for (auto& item : node.elements) visit(item);
            if (node.rest) visit(*node.rest);
        } else if constexpr (std::is_same_v<T, FieldGet>) {
            visit(node.record);
        } else if constexpr (std::is_same_v<T, Lambda>) {
            visit(node.body);
        } else if constexpr (std::is_same_v<T, Return>) {
            visit(node.value);
        } else if constexpr (std::is_same_v<T, LetRec>) {
            visit(node.funBody); visit(node.contBody);
        } else if constexpr (std::is_same_v<T, Receive>) {
            for (auto& clause : node.clauses) {
                if (clause.guard) visit(*clause.guard);
                visit(clause.body);
            }
            if (node.timeout) visit(*node.timeout);
            if (node.afterBody) visit(*node.afterBody);
        }
    }, expr->node);
}

} // namespace

auto lowerModules(const ast::Program& prog, const std::string& fileStem,
                  const std::unordered_set<std::string>& preludeFns,
                  const std::string& sourcePath,
                  const std::vector<ExternalRecordLayout>* externalRecords,
                  const ExternalModules* externals,
                  const std::unordered_map<const ast::MethodCall*,
                      semantic::ResolvedCallTarget>* resolvedCalls)
    -> std::vector<Module> {
    auto flat = lowerProgram(prog, fileStem, preludeFns, sourcePath, externals,
                             externalRecords, resolvedCalls);

    struct Definition { std::string path; std::string sourceName; bool exported; };
    std::unordered_map<std::string, Definition> definitions;
    std::vector<std::string> modulePaths;
    std::unordered_set<std::string> seenModulePaths;
    std::function<void(const ast::ModuleDef&)> collect;
    collect = [&](const ast::ModuleDef& module) {
        const auto& path = module.name;
        if (seenModulePaths.insert(path).second)
            modulePaths.push_back(path);
        std::string prefix;
        for (char c : path) prefix += c == '.' ? "__" : std::string(1, c);
        auto add = [&](const ast::FunctionDef* fn, bool exported) {
            if (fn) definitions[prefix + "__" + fn->name] = {path, fn->name, exported};
        };
        auto addMake = [&](const ast::MakeDef* mk) {
            if (!mk) return;
            auto collectMethod = [&](const ast::FunctionDef* fd) {
                if (!fd) return;
                if (!definitions.count(fd->name))
                    definitions[fd->name] = {path, fd->name, true};
            };
            for (const auto& mi : mk->body) {
                if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&mi))
                    collectMethod(fd->get());
                else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&mi))
                    if (*vb) for (const auto& vi : (*vb)->items)
                        if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                            collectMethod(fd->get());
            }
        };
        auto addRecord = [&](const ast::RecordDef* rd) {
            if (!rd) return;
            for (const auto& field : rd->fields)
                if (!definitions.count(field.name))
                    definitions[field.name] = {path, field.name, true};
        };
        for (const auto& item : module.body) {
            if (auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) add(fn->get(), true);
            else if (auto* visibility = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
                if (*visibility) for (const auto& entry : (*visibility)->items)
                    if (auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&entry))
                        add(fn->get(), (*visibility)->isPublic);
            } else if (auto* mk = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                addMake(mk->get());
            } else if (auto* rd = std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
                addRecord(rd->get());
            } else if (auto* child = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*child) collect(**child);
            }
        }
    };
    for (const auto& item : prog.items)
        if (auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item); module && *module)
            collect(**module);

    std::unordered_map<std::string, std::pair<std::string, std::string>> targets;
    for (const auto& [emitted, def] : definitions)
        targets[emitted] = {"Kex." + def.path, def.sourceName};

    std::vector<Module> result;
    std::unordered_map<std::string, std::vector<FunDef>> moduleBuckets;
    std::vector<FunDef> globalFunctions;
    for (auto& fn : flat.functions) {
        auto found = definitions.find(fn.name);
        if (found == definitions.end()) {
            globalFunctions.push_back(std::move(fn));
            continue;
        }
        const auto& def = found->second;
        fn.name = def.sourceName;
        fn.exported = def.exported;
        moduleBuckets[def.path].push_back(std::move(fn));
    }
    std::unordered_map<std::string, std::pair<std::string, std::string>>
        globalTargets;
    for (const auto& fn : globalFunctions)
        globalTargets[fn.name] = {flat.name, fn.name};
    flat.functions = std::move(globalFunctions);

    result.push_back(std::move(flat));
    for (const auto& path : modulePaths) {
        Module module;
        module.name = "Kex." + path;
        if (auto it = moduleBuckets.find(path); it != moduleBuckets.end())
            module.functions = std::move(it->second);
        result.push_back(std::move(module));
    }

    for (size_t moduleIndex = 0; moduleIndex < result.size(); moduleIndex++) {
        auto& module = result[moduleIndex];
        for (auto& fn : module.functions)
            for (auto& clause : fn.clauses) {
                if (clause.guard) rewriteModuleCalls(*clause.guard, targets);
                rewriteModuleCalls(clause.body, targets);
                if (moduleIndex > 0) {
                    if (clause.guard)
                        rewriteModuleCalls(*clause.guard, globalTargets);
                    rewriteModuleCalls(clause.body, globalTargets);
                }
            }
    }
    return result;
}

} // namespace kex::ir
