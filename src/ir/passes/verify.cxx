#include "verify.hxx"
#include "../walk.hxx"

#include <set>
#include <utility>

namespace kex::ir {

namespace {

auto check(const Expr& expr, const Module& module,
           std::set<std::pair<std::string, int>>& defined,
           const std::string& function, std::vector<std::string>& problems)
    -> void {
    // A `letrec` (a lifted loop, a guard continuation) defines a local
    // function for its own body and for what follows it.
    bool scoped = false;
    if (const auto* local = std::get_if<LetRec>(&expr.node))
        scoped = defined.insert({local->name,
                                 static_cast<int>(local->params.size())})
                     .second;
    if (const auto* call = std::get_if<Call>(&expr.node)) {
        const auto where = module.name + ":" + function + ": call to " +
            (call->module.empty() ? "" : call->module + ":") + call->name +
            "/" + std::to_string(call->arity);
        if (static_cast<size_t>(call->arity) != call->args.size())
            problems.push_back(where + " passes " +
                               std::to_string(call->args.size()) +
                               " arguments");
        if (call->module.empty() && !defined.count({call->name, call->arity}))
            problems.push_back(where + " names no function of this module");
    }
    forEachChild(expr, [&](const ExprPtr& child) {
        if (child) check(*child, module, defined, function, problems);
    });
    if (scoped)
        defined.erase({std::get<LetRec>(expr.node).name,
                       static_cast<int>(std::get<LetRec>(expr.node).params.size())});
}

} // namespace

auto verifyModule(const Module& module) -> std::vector<std::string> {
    std::set<std::pair<std::string, int>> defined;
    for (const auto& function : module.functions)
        defined.insert({function.name, function.arity});
    std::vector<std::string> problems;
    for (const auto& function : module.functions) {
        const auto name = function.name + "/" + std::to_string(function.arity);
        for (const auto& clause : function.clauses) {
            if (clause.guard && *clause.guard)
                check(**clause.guard, module, defined, name, problems);
            if (clause.body) check(*clause.body, module, defined, name, problems);
        }
    }
    return problems;
}

} // namespace kex::ir
