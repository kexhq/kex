#include "upgrade.hxx"
#include "../clone.hxx"
#include "../walk.hxx"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace kex::ir {

namespace {

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

// The Kex name an SSA variable was derived from: `_ir_count3` → `count`.
// Lifted-loop parameters are ordered by it rather than by the SSA name, so an
// edit elsewhere in the function that renumbers fresh names does not reorder
// the loop's parameters between two versions of the module.
auto ssaSourceName(const std::string& ssa) -> std::string {
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
                (rec->funBody && returnsEarly(*rec->funBody)))
                break;
            std::set<std::string> bound(rec->params.begin(), rec->params.end());
            std::set<std::string> freeSet;
            if (rec->funBody) collectFree(*rec->funBody, bound, freeSet);
            std::vector<std::string> captured(freeSet.begin(), freeSet.end());
            std::stable_sort(captured.begin(), captured.end(),
                             [](const std::string& a, const std::string& b) {
                                 return ssaSourceName(a) < ssaSourceName(b);
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
