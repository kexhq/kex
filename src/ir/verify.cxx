#include "verify.hxx"

#include "clone.hxx"
#include "walk.hxx"

#include <functional>
#include <set>
#include <utility>

namespace kex::ir {

namespace {

auto named(const FunDef& function) -> std::string {
    return function.name + "/" + std::to_string(function.arity);
}

using Local = std::pair<std::string, int>;

// Every call in +expr+, nested ones included, with the local functions in
// scope there: a `LetRec` (a loop, a guard's continuation) is one in its own
// body and in what follows it.
auto forEachCall(
    const Expr& expr,
    std::set<Local>& locals,
    const std::function<void(const Call&, const std::set<Local>&)>& visit) -> void {
    if (const auto* call = std::get_if<Call>(&expr.node)) {
        visit(*call, locals);
    }
    if (const auto* letrec = std::get_if<LetRec>(&expr.node)) {
        const Local local{letrec->name, static_cast<int>(letrec->params.size())};
        const bool added = locals.insert(local).second;
        forEachChild(expr, [&](const ExprPtr& child) {
            if (child) {
                forEachCall(*child, locals, visit);
            }
        });
        if (added) {
            locals.erase(local);
        }
        return;
    }
    forEachChild(expr, [&](const ExprPtr& child) {
        if (child) {
            forEachCall(*child, locals, visit);
        }
    });
}

} // namespace

auto verify(const Module& module) -> std::vector<std::string> {
    std::vector<std::string> problems;
    std::set<std::pair<std::string, int>> defined;
    for (const auto& function : module.functions) {
        if (!defined.emplace(function.name, function.arity).second) {
            problems.push_back(named(function) + " is defined twice");
        }
    }
    for (const auto& function : module.functions) {
        for (const auto& clause : function.clauses) {
            if (static_cast<int>(clause.params.size()) != function.arity) {
                problems.push_back(
                    named(function) + " has a clause of " + std::to_string(clause.params.size()) +
                    " parameter(s)");
            }
            std::set<std::string> bound;
            for (const auto& param : clause.params) {
                if (param) {
                    bindPattern(*param, bound);
                }
            }
            std::set<std::string> unbound;
            if (clause.guard && *clause.guard) {
                collectFree(**clause.guard, bound, unbound);
            }
            if (clause.body) {
                collectFree(*clause.body, bound, unbound);
            }
            for (const auto& name : unbound) {
                problems.push_back(named(function) + " reads `" + name + "`, which nothing binds");
            }
            const auto checkCall = [&](const Call& call, const std::set<Local>& locals) {
                if (static_cast<int>(call.args.size()) != call.arity) {
                    problems.push_back(
                        named(function) + " calls " + call.name + "/" + std::to_string(call.arity) +
                        " with " + std::to_string(call.args.size()) + " argument(s)");
                }
                if (call.module.empty() && !defined.count({call.name, call.arity}) &&
                    !locals.count({call.name, call.arity})) {
                    problems.push_back(
                        named(function) + " calls " + call.name + "/" + std::to_string(call.arity) +
                        ", which this module does not define");
                }
            };
            std::set<Local> locals;
            if (clause.guard && *clause.guard) {
                forEachCall(**clause.guard, locals, checkCall);
            }
            if (clause.body) {
                forEachCall(*clause.body, locals, checkCall);
            }
        }
    }
    return problems;
}

} // namespace kex::ir
