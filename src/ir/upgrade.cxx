#include "upgrade.hxx"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <utility>

namespace kex::ir {

namespace {

// Calls `f` on every direct child expression of `e`, without interpreting
// scopes. The walkers below decide for themselves which children to enter.
template <typename F>
auto forEachChild(Expr& e, F&& f) -> void {
    std::visit([&](auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, Intrinsic> || std::is_same_v<T, Call> ||
                      std::is_same_v<T, Construct>) {
            for (auto& a : n.args) f(a);
        } else if constexpr (std::is_same_v<T, CallIndirect>) {
            f(n.callee);
            for (auto& a : n.args) f(a);
        } else if constexpr (std::is_same_v<T, Let>) {
            f(n.value);
            f(n.body);
        } else if constexpr (std::is_same_v<T, Seq>) {
            for (auto& x : n.exprs) f(x);
        } else if constexpr (std::is_same_v<T, Match>) {
            for (auto& s : n.subjects) f(s);
            for (auto& c : n.clauses) {
                if (c.guard) f(*c.guard);
                f(c.body);
            }
        } else if constexpr (std::is_same_v<T, MakeTuple>) {
            for (auto& x : n.elements) f(x);
        } else if constexpr (std::is_same_v<T, MakeList>) {
            for (auto& x : n.elements) f(x);
            if (n.rest) f(*n.rest);
        } else if constexpr (std::is_same_v<T, FieldGet>) {
            f(n.record);
        } else if constexpr (std::is_same_v<T, Lambda>) {
            f(n.body);
        } else if constexpr (std::is_same_v<T, Return>) {
            f(n.value);
        } else if constexpr (std::is_same_v<T, TryThrow>) {
            f(n.error);
        } else if constexpr (std::is_same_v<T, TryCatch>) {
            f(n.body);
            for (auto& c : n.clauses) {
                if (c.guard) f(*c.guard);
                f(c.body);
            }
        } else if constexpr (std::is_same_v<T, LetRec>) {
            f(n.funBody);
            f(n.contBody);
        } else if constexpr (std::is_same_v<T, Receive>) {
            for (auto& c : n.clauses) f(c.body);
            if (n.timeout) f(*n.timeout);
            if (n.afterBody) f(*n.afterBody);
        }
    }, e.node);
}

// Does this body receive — in its own process-level control flow, not inside
// a lambda (a lambda runs wherever it is called, and is judged on its own)?
auto receives(ExprPtr& e) -> bool {
    if (!e) return false;
    if (std::holds_alternative<Receive>(e->node)) return true;
    if (std::holds_alternative<Lambda>(e->node)) return false;
    bool found = false;
    forEachChild(*e, [&](ExprPtr& c) { found = found || receives(c); });
    return found;
}

// Does an early `return` inside this body escape to the enclosing function?
// Such a loop cannot move to a function of its own: the emitter catches a
// `return` per function, so it would return from the lifted loop instead.
auto returns(ExprPtr& e) -> bool {
    if (!e) return false;
    if (std::holds_alternative<Return>(e->node)) return true;
    if (std::holds_alternative<Lambda>(e->node)) return false;
    bool found = false;
    forEachChild(*e, [&](ExprPtr& c) { found = found || returns(c); });
    return found;
}

auto bindPattern(const Pattern& p, std::set<std::string>& bound) -> void {
    if (p.kind == PatKind::Var) bound.insert(p.name);
    for (const auto& a : p.args)
        if (a) bindPattern(*a, bound);
    if (p.rest) bindPattern(*p.rest, bound);
}

// Variables referenced in `e` but bound outside it.
auto collectFree(ExprPtr& e, std::set<std::string> bound,
                 std::set<std::string>& out) -> void {
    if (!e) return;
    std::visit([&](auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, Var>) {
            if (!bound.count(n.name)) out.insert(n.name);
        } else if constexpr (std::is_same_v<T, Let>) {
            collectFree(n.value, bound, out);
            auto inner = bound;
            inner.insert(n.name);
            collectFree(n.body, inner, out);
        } else if constexpr (std::is_same_v<T, Match>) {
            for (auto& s : n.subjects) collectFree(s, bound, out);
            for (auto& c : n.clauses) {
                auto inner = bound;
                for (const auto& p : c.patterns)
                    if (p) bindPattern(*p, inner);
                if (c.guard) collectFree(*c.guard, inner, out);
                collectFree(c.body, inner, out);
            }
        } else if constexpr (std::is_same_v<T, Lambda>) {
            auto inner = bound;
            inner.insert(n.params.begin(), n.params.end());
            collectFree(n.body, inner, out);
        } else if constexpr (std::is_same_v<T, TryCatch>) {
            collectFree(n.body, bound, out);
            for (auto& c : n.clauses) {
                auto inner = bound;
                for (const auto& p : c.patterns)
                    if (p) bindPattern(*p, inner);
                if (c.guard) collectFree(*c.guard, inner, out);
                collectFree(c.body, inner, out);
            }
        } else if constexpr (std::is_same_v<T, LetRec>) {
            auto inner = bound;
            inner.insert(n.params.begin(), n.params.end());
            collectFree(n.funBody, inner, out);
            collectFree(n.contBody, bound, out);
        } else if constexpr (std::is_same_v<T, Receive>) {
            for (auto& c : n.clauses) {
                auto inner = bound;
                if (n.senderVar) inner.insert(*n.senderVar);
                if (c.pattern) bindPattern(*c.pattern, inner);
                collectFree(c.body, inner, out);
            }
            if (n.timeout) collectFree(*n.timeout, bound, out);
            if (n.afterBody) collectFree(*n.afterBody, bound, out);
        } else {
            forEachChild(*e, [&](ExprPtr& c) { collectFree(c, bound, out); });
        }
    }, e->node);
}

// The Kex name an SSA variable was derived from: `_ir_count3` → `count`.
// Lifted-loop parameters are ordered by it rather than by the SSA name, so an
// edit elsewhere in the function that renumbers fresh names does not reorder
// the loop's parameters between two versions of the module.
auto sourceName(const std::string& ssa) -> std::string {
    std::string s = ssa;
    if (s.rfind("_ir_", 0) == 0) s = s.substr(4);
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

// Redirects every local call to `name/arity` into the lifted function, which
// takes the captured variables as trailing arguments.
auto redirectLoopCalls(ExprPtr& e, const std::string& name, int arity,
                       const std::string& module, const std::string& lifted,
                       const std::vector<std::string>& captured) -> void {
    if (!e) return;
    if (auto* call = std::get_if<Call>(&e->node);
        call && call->module.empty() && call->name == name && call->arity == arity) {
        call->module = module;
        call->name = lifted;
        for (const auto& v : captured) call->args.push_back(var(v));
        call->arity = static_cast<int>(call->args.size());
    }
    forEachChild(*e, [&](ExprPtr& c) {
        redirectLoopCalls(c, name, arity, module, lifted, captured);
    });
}

struct UpgradePass {
    Module& mod;
    std::set<std::pair<std::string, int>> exported;
    std::vector<FunDef> lifted;

    // Lambda-lift each receiving loop under `e` into its own module function.
    auto liftLoops(ExprPtr& e, const std::string& owner, int& ordinal) -> void {
        if (!e) return;
        while (auto* rec = std::get_if<LetRec>(&e->node)) {
            if (rec->name.rfind("__loop", 0) != 0 || !receives(rec->funBody) ||
                returns(rec->funBody))
                break;
            std::set<std::string> bound(rec->params.begin(), rec->params.end());
            std::set<std::string> freeSet;
            collectFree(rec->funBody, bound, freeSet);
            std::vector<std::string> captured(freeSet.begin(), freeSet.end());
            std::stable_sort(captured.begin(), captured.end(),
                             [](const std::string& a, const std::string& b) {
                                 return sourceName(a) < sourceName(b);
                             });

            // Named after the function it came from and its position there, so
            // the next version of the module exports the same name for a
            // process still running this one to call into.
            std::string liftedName =
                "__kex_loop_" + owner + "_" + std::to_string(ordinal++);
            int arity = static_cast<int>(rec->params.size() + captured.size());

            redirectLoopCalls(rec->funBody, rec->name, static_cast<int>(rec->params.size()),
                              mod.name, liftedName, captured);
            redirectLoopCalls(rec->contBody, rec->name, static_cast<int>(rec->params.size()),
                              mod.name, liftedName, captured);

            FunDef fn;
            fn.name = liftedName;
            fn.arity = arity;
            fn.exported = true;
            FunClause clause;
            for (const auto& p : rec->params) {
                auto pat = std::make_unique<Pattern>();
                pat->kind = PatKind::Var;
                pat->name = p;
                clause.params.push_back(std::move(pat));
            }
            for (const auto& v : captured) {
                auto pat = std::make_unique<Pattern>();
                pat->kind = PatKind::Var;
                pat->name = v;
                clause.params.push_back(std::move(pat));
            }
            clause.body = std::move(rec->funBody);
            fn.clauses.push_back(std::move(clause));
            exported.insert({fn.name, fn.arity});
            lifted.push_back(std::move(fn));

            ExprPtr cont = std::move(rec->contBody);
            e = std::move(cont);
        }
        forEachChild(*e, [&](ExprPtr& c) { liftLoops(c, owner, ordinal); });
    }

    // Within a receiving body, turn calls to this module's exported functions
    // into remote calls. `active` says whether the current scope receives.
    auto remoteCalls(ExprPtr& e, bool active) -> void {
        if (!e) return;
        if (auto* lam = std::get_if<Lambda>(&e->node)) {
            remoteCalls(lam->body, receives(lam->body));
            return;
        }
        if (auto* call = std::get_if<Call>(&e->node);
            active && call && call->module.empty() &&
            exported.count({call->name, call->arity}))
            call->module = mod.name;
        forEachChild(*e, [&](ExprPtr& c) { remoteCalls(c, active); });
    }

    auto run() -> void {
        for (const auto& fn : mod.functions)
            if (fn.exported) exported.insert({fn.name, fn.arity});

        // A lifted loop is appended and visited in turn, so a receiving loop
        // nested inside it is lifted as well. Indices, not references: the
        // append reallocates.
        for (size_t i = 0; i < mod.functions.size(); i++) {
            int ordinal = 0;
            const std::string owner = mod.functions[i].name;
            for (auto& clause : mod.functions[i].clauses)
                liftLoops(clause.body, owner, ordinal);
            for (auto& l : lifted) mod.functions.push_back(std::move(l));
            lifted.clear();
        }

        for (auto& fn : mod.functions)
            for (auto& clause : fn.clauses)
                remoteCalls(clause.body, receives(clause.body));
    }
};

} // namespace

auto insertUpgradePoints(Module& mod) -> void {
    UpgradePass pass{mod, {}, {}};
    pass.run();
}

} // namespace kex::ir
