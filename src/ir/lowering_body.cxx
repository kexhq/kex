// Body lowering methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"

#include "../ast/walk.hxx"

#include <algorithm>

namespace kex::ir {

// A statement sequence. `let`/`var`/reassignments introduce SSA-renamed
// bindings (updating `subst`); every other statement's value is bound to
// a throwaway name; the last statement is the sequence's value.
auto Lowering::lowerBody(const std::vector<ast::ExprPtr>& body) -> ExprPtr {
    if (body.empty()) {
        return lit(LitKind::Atom, "ok");
    }
    return lowerBodyFrom(body, 0);
}

// The blocks passed to calls in `e` whose bodies reassign variables of the
// code around them, in the order they are written, and the union of those
// variables, sorted. Empty when there are none.
auto Lowering::reassigningBlocks(const ast::Expr& e, std::vector<std::string>& vars)
    -> std::vector<const ast::Lambda*> {
    std::vector<const ast::Lambda*> found;
    std::set<std::string> all;
    std::function<void(const ast::Expr&)> visit = [&](const ast::Expr& x) {
        std::vector<const ast::ExprPtr*> passed;
        if (const auto* mc = std::get_if<ast::MethodCall>(&x.kind)) {
            for (const auto& a : mc->args) {
                passed.push_back(&a);
            }
            if (mc->block) {
                passed.push_back(&*mc->block);
            }
        } else if (const auto* fc = std::get_if<ast::FunctionCall>(&x.kind)) {
            for (const auto& a : fc->args) {
                passed.push_back(&a);
            }
            if (fc->block) {
                passed.push_back(&*fc->block);
            }
        }
        for (const auto* p : passed) {
            const auto* lambda = *p ? std::get_if<ast::Lambda>(&(**p).kind) : nullptr;
            if (!lambda || lambda->collection) {
                continue;
            }
            std::unordered_set<std::string> mutated;
            for (const auto& s : lambda->body) {
                collectMutated(s, mutated);
            }
            for (const auto& param : lambda->params) {
                mutated.erase(param.name);
            }
            bool reassigns = false;
            for (const auto& v : mutated) {
                if (subst.count(v)) {
                    all.insert(v);
                    reassigns = true;
                }
            }
            if (reassigns && std::find(found.begin(), found.end(), lambda) == found.end()) {
                found.push_back(lambda);
            }
        }
        ast::forEachOperand(x, [&](const ast::ExprPtr& child) {
            if (child) {
                visit(*child);
            }
        });
    };
    visit(e);
    vars.assign(all.begin(), all.end());
    return found;
}

// `block` taking the values of `vars` after its own parameters, answering
// {Value, State}: its reassignments thread through as a loop body's do.
auto Lowering::lowerStatefulBlock(const ast::Lambda& block, const std::vector<std::string>& vars)
    -> ExprPtr {
    auto snap = subst;
    Lambda lam;
    for (const auto& p : block.params) {
        if (names::isWildcard(p.name)) {
            lam.params.push_back(fresh("Ignored"));
            continue;
        }
        lam.params.push_back(p.name);
        subst[p.name] = p.name;
    }
    const auto state = fresh("BlockState");
    lam.params.push_back(state);
    // Its reassignments are its own statement's state, returned only when the
    // call ends: pure as the analysis says, they let a fused loop run it.
    lam.pure = hir->pureBlock(block);
    PatternPtr statePattern;
    if (vars.size() == 1) {
        subst[vars[0]] = state;
    } else {
        statePattern = std::make_unique<Pattern>();
        statePattern->kind = PatKind::Tuple;
        for (const auto& v : vars) {
            const auto name = fresh(v);
            subst[v] = name;
            auto p = std::make_unique<Pattern>();
            p->kind = PatKind::Var;
            p->name = name;
            statePattern->args.push_back(std::move(p));
        }
    }
    const auto& body = block.body;
    // The block's value is its last statement's: lowered as a value when it
    // reassigns nothing, else after it runs (an assignment's value is the
    // one assigned).
    const ast::Expr* last = body.empty() ? nullptr : body.back().get();
    bool lastIsValue = false;
    if (last) {
        std::unordered_set<std::string> mutated;
        collectMutated(body.back(), mutated);
        lastIsValue = mutated.empty() && !std::holds_alternative<ast::LetExpr>(last->kind) &&
                      !std::holds_alternative<ast::VarExpr>(last->kind) &&
                      !std::holds_alternative<ast::ReturnExpr>(last->kind) &&
                      !std::holds_alternative<ast::BreakExpr>(last->kind) &&
                      !std::holds_alternative<ast::NextExpr>(last->kind);
    }
    const auto answer = [&](ExprPtr value) {
        const auto t = fresh("BlockValue");
        std::vector<ExprPtr> pair;
        pair.push_back(var(t));
        pair.push_back(stateExpr(vars));
        auto tuple = std::make_unique<Expr>();
        tuple->node = MakeTuple{std::move(pair)};
        return makeLet(t, std::move(value), std::move(tuple));
    };
    auto lowered = lowerLoopBodyFrom(
        body, 0, "", vars,
        [&]() -> ExprPtr {
            if (lastIsValue) {
                return answer(lower(body.back()));
            }
            if (const auto* assign = last ? std::get_if<ast::AssignExpr>(&last->kind) : nullptr) {
                return answer(var(currentName(assign->name)));
            }
            auto unit = std::make_unique<Expr>();
            unit->node = MakeTuple{};
            return answer(std::move(unit));
        },
        lastIsValue ? body.size() - 1 : body.size());
    lam.body = statePattern ? makeMatch1(var(state), std::move(statePattern), std::move(lowered))
                            : std::move(lowered);
    subst = snap;
    auto e = std::make_unique<Expr>();
    e->node = std::move(lam);
    return e;
}

// A statement passing a block that reassigns variables of the code around
// it: `let doubled = xs.map { |x| seen += 1; x * 2 }`. A BEAM closure
// captures values, so the block takes those variables' values after its own
// arguments and answers {Value, State} (lowerStatefulBlock), and the
// statement runs through `kex_intrinsic_fun:threadState(Block, Initial,
// fun(Placeholder) -> Statement)`, which keeps the state for the call and
// answers {Value, FinalState}. The variables are rebound to that state.
// Several such blocks in one statement (`xs.map { … }.each { … }`) each take
// the union of their variables and go through `threadStates`, sharing one
// state.
// specialize-blocks replaces the whole with a copy of the callee carrying the
// state, where it can. `rest` lowers what follows, given the statement's
// value. Null when `e` passes no such block.
auto Lowering::lowerThreadedStatement(
    const ast::ExprPtr& e, const std::function<ExprPtr(ExprPtr)>& rest) -> ExprPtr {
    const ast::ExprPtr* source = &e;
    if (const auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
        if (!std::holds_alternative<ast::VarPattern>(le->pattern->kind)) {
            return nullptr;
        }
        source = &le->value;
    } else if (const auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
        if (!ve->pattern || !std::holds_alternative<ast::VarPattern>(ve->pattern->kind)) {
            return nullptr;
        }
        source = &ve->value;
    } else if (const auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
        source = &ae->value;
    } else if (
        !std::holds_alternative<ast::MethodCall>(e->kind) &&
        !std::holds_alternative<ast::FunctionCall>(e->kind)) {
        return nullptr;
    }
    if (!*source) {
        return nullptr;
    }
    std::vector<std::string> vars;
    const auto blocks = reassigningBlocks(**source, vars);
    if (blocks.empty()) {
        return nullptr;
    }

    std::vector<Binding> binds;
    std::vector<ExprPtr> blockFns;
    for (const auto* block : blocks) {
        blockFns.push_back(atomize_ir(lowerStatefulBlock(*block, vars), binds));
    }
    auto initial = atomize_ir(stateExpr(vars), binds);
    std::vector<std::string> placeholders;
    const auto outer = threadedBlocks;
    for (const auto* block : blocks) {
        placeholders.push_back(fresh("Block"));
        threadedBlocks[block] = placeholders.back();
    }
    auto value = lower(*source);
    threadedBlocks = outer;
    auto statement = std::make_unique<Expr>();
    statement->node = Lambda{std::move(placeholders), std::move(value), false};
    auto statementFn = atomize_ir(std::move(statement), binds);
    // One block: the form specialize-blocks copies a callee for. Several
    // share one state, each seeing what the one before it left.
    ExprPtr blocksArg;
    if (blockFns.size() == 1) {
        blocksArg = std::move(blockFns.front());
    } else {
        blocksArg = std::make_unique<Expr>();
        blocksArg->node = MakeList{std::move(blockFns), std::nullopt};
    }
    auto call = callE(
        "kex_intrinsic_fun", blocks.size() == 1 ? "threadState" : "threadStates", 3,
        three(std::move(blocksArg), std::move(initial), std::move(statementFn)));

    const auto result = fresh("Threaded");
    auto pattern = std::make_unique<Pattern>();
    pattern->kind = PatKind::Tuple;
    auto resultPattern = std::make_unique<Pattern>();
    resultPattern->kind = PatKind::Var;
    resultPattern->name = result;
    pattern->args.push_back(std::move(resultPattern));
    auto statePattern = std::make_unique<Pattern>();
    if (vars.size() == 1) {
        statePattern->kind = PatKind::Var;
        statePattern->name = fresh(vars[0]);
        subst[vars[0]] = statePattern->name;
    } else {
        statePattern->kind = PatKind::Tuple;
        for (const auto& v : vars) {
            auto p = std::make_unique<Pattern>();
            p->kind = PatKind::Var;
            p->name = fresh(v);
            subst[v] = p->name;
            statePattern->args.push_back(std::move(p));
        }
    }
    pattern->args.push_back(std::move(statePattern));
    auto following = rest(var(result));
    return wrapLets(binds, makeMatch1(std::move(call), std::move(pattern), std::move(following)));
}

auto Lowering::lowerBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i) -> ExprPtr {
    const auto& e = body[i];
    bool isLast = (i + 1 == body.size());
    currentLoc = &e->location;

    // A statement passing a block that reassigns variables around it.
    {
        const auto next = [&](const std::string& name) {
            return isLast ? var(name) : lowerBodyFrom(body, i + 1);
        };
        const auto bindValue = [&](ExprPtr value) -> ExprPtr {
            if (const auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
                const auto& name = std::get<ast::VarPattern>(le->pattern->kind).name;
                const auto ssa = subst.count(name) ? fresh(name) : name;
                subst[name] = ssa;
                immutableBindings.insert(name);
                curryOrigin.erase(name);
                return makeLet(ssa, std::move(value), next(ssa));
            }
            if (const auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
                const auto& name = std::get<ast::VarPattern>(ve->pattern->kind).name;
                const auto ssa = fresh(name);
                subst[name] = ssa;
                immutableBindings.erase(name);
                curryOrigin.erase(name);
                return makeLet(ssa, std::move(value), next(ssa));
            }
            if (const auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
                const auto ssa = fresh(ae->name);
                subst[ae->name] = ssa;
                return makeLet(ssa, std::move(value), next(ssa));
            }
            if (isLast) {
                return value;
            }
            const auto discarded = fresh("Statement");
            return makeLet(discarded, std::move(value), lowerBodyFrom(body, i + 1));
        };
        if (auto threaded = lowerThreadedStatement(e, bindValue)) {
            return threaded;
        }
    }

    // let PATTERN = value
    if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
        if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
            // `let y = f(x = 1)`: the nested assignment is bound first.
            std::vector<Binding> hoisted;
            const auto assigned = hoistNestedAssignments(e, hoisted);
            auto val = lowerBindingValue(*le);
            for (const auto& [name, renamed] : assigned) {
                subst[name] = renamed;
            }
            std::string ssa = subst.count(vp->name) ? fresh(vp->name) : vp->name;
            subst[vp->name] = ssa;
            immutableBindings.insert(vp->name);
            if (const auto* curry = std::get_if<ast::CurryExpr>(&le->value->kind);
                curry && curry->argGroups.empty() && !curry->isOperator && curry->module.empty()) {
                curryOrigin[vp->name] = curry->name;
            } else {
                curryOrigin.erase(vp->name);
            }
            auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
            return wrapLets(hoisted, makeLet(ssa, std::move(val), std::move(rest)));
        }
        // Record destructure `let { f, g: { h } } = value`: fields are read
        // by name, not structurally — bind the value then prepend accessors.
        if (auto* rp = std::get_if<ast::RecordPattern>(&le->pattern->kind)) {
            // Anonymous `{ f, g }`: no type name, so the field offsets are
            // only knowable per candidate tag (or per map key) at runtime.
            if (rp->typeName.empty()) {
                std::string rv = fresh("Anon");
                std::vector<std::string> names;
                collectRecordBindings(*rp, names);
                for (const auto& name : names) {
                    subst[name] = name;
                }
                auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
                // Lowering the rest moved the location on; a missing key
                // is reported at this `let`, as the walker does.
                currentLoc = &e->location;
                return makeLet(
                    rv, lower(le->value), wrapAnonymousRecordPattern(rv, *rp, std::move(rest)));
            }
            std::string rv = fresh("rec");
            std::vector<std::pair<std::string, ExprPtr>> prefix;
            destructureRecordPattern(rv, *rp, prefix);
            for (auto& [nm, _] : prefix) {
                subst[nm] = nm;
            }
            auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
            for (auto it = prefix.rbegin(); it != prefix.rend(); ++it) {
                rest = makeLet(it->first, std::move(it->second), std::move(rest));
            }
            return makeLet(rv, lower(le->value), std::move(rest));
        }
        auto val = lower(le->value);
        auto pat = lowerPattern(le->pattern);
        auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
        return makeMatch1(std::move(val), std::move(pat), std::move(rest));
    }
    // var PATTERN = value  (fresh mutable bindings)
    if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
        if (auto* vp = ve->pattern ? std::get_if<ast::VarPattern>(&ve->pattern->kind) : nullptr) {
            auto val = lower(ve->value);
            std::string ssa = subst.count(vp->name) ? fresh(vp->name) : vp->name;
            subst[vp->name] = ssa;
            immutableBindings.erase(vp->name);
            if (const auto* curry = std::get_if<ast::CurryExpr>(&ve->value->kind);
                curry && curry->argGroups.empty() && !curry->isOperator && curry->module.empty()) {
                curryOrigin[vp->name] = curry->name;
            } else {
                curryOrigin.erase(vp->name);
            }
            auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
            return makeLet(ssa, std::move(val), std::move(rest));
        }
        // Record destructure, mirroring `let` above — bound mutable.
        if (auto* rp =
                ve->pattern ? std::get_if<ast::RecordPattern>(&ve->pattern->kind) : nullptr) {
            if (rp->typeName.empty()) {
                std::string rv = fresh("Anon");
                std::vector<std::string> names;
                collectRecordBindings(*rp, names);
                for (const auto& name : names) {
                    subst[name] = name;
                    immutableBindings.erase(name);
                }
                auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
                currentLoc = &e->location;
                return makeLet(
                    rv, lower(ve->value), wrapAnonymousRecordPattern(rv, *rp, std::move(rest)));
            }
            std::string rv = fresh("rec");
            std::vector<std::pair<std::string, ExprPtr>> prefix;
            destructureRecordPattern(rv, *rp, prefix);
            for (auto& [nm, _] : prefix) {
                subst[nm] = nm;
                immutableBindings.erase(nm);
            }
            auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
            for (auto it = prefix.rbegin(); it != prefix.rend(); ++it) {
                rest = makeLet(it->first, std::move(it->second), std::move(rest));
            }
            return makeLet(rv, lower(ve->value), std::move(rest));
        }
        // `var y = f(x = 1)`: the nested assignment is bound first.
        std::vector<Binding> hoisted;
        const auto assigned = hoistNestedAssignments(e, hoisted);
        auto val = lower(ve->value);
        for (const auto& [name, renamed] : assigned) {
            subst[name] = renamed;
        }
        auto pat = lowerPattern(ve->pattern);
        if (ve->pattern) {
            std::vector<std::string> names;
            patternBoundNames(ve->pattern, names);
            for (const auto& name : names) {
                immutableBindings.erase(name);
            }
        }
        auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
        return wrapLets(hoisted, makeMatch1(std::move(val), std::move(pat), std::move(rest)));
    }
    // name = value  (reassignment → fresh SSA name, value sees the OLD one)
    if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
        auto val = lowerAssignmentValue(*ae);
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
                if (currentLoc) {
                    loc = std::string(currentLoc->file) + ":" + std::to_string(currentLoc->line) +
                          ":" + std::to_string(currentLoc->column) + ": ";
                }
                err->node = Call{
                    "erlang", "error", 1,
                    one(lit(
                        LitKind::String,
                        loc + "runtime error: Cannot use '!' on immutable binding: " + id->name)),
                    false};
                return err;
            }
            auto val = lowerMutatingAsValue(*mc);
            std::string ssa = fresh(id->name);
            subst[id->name] = ssa;
            auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
            return makeLet(ssa, std::move(val), std::move(rest));
        }
    }
    // Statement-position if/match whose branches REASSIGN outer vars —
    // thread the mutated state through, exactly like loops do: each
    // branch yields the (possibly reassigned) values, and the code after
    // sees fresh SSA names (`var sql = …; if c sql = sql + x end; …`).
    {
        auto* ie = std::get_if<ast::IfExpr>(&e->kind);
        auto* me = std::get_if<ast::MatchExpr>(&e->kind);
        // `trying` too: an assignment in its body or a rescue arm
        // (`rescue _ => found = None`) had no expression form to lower to
        // and failed the whole build ("unimplemented expr node AssignExpr").
        auto* te = std::get_if<ast::TryingExpr>(&e->kind);
        // `with` too: its body is lowered as one nested expression, so a
        // `var` it reassigned got a fresh name nothing after it could see
        // (`unbound variable '_ir_result6'`, kexhq/kex#493).
        auto* we = std::get_if<ast::WithExpr>(&e->kind);
        if (ie || te || we || (me && !me->subjectBinding)) {
            std::unordered_set<std::string> muts;
            collectMutated(e, muts);
            std::vector<std::string> mutVars;
            for (const auto& v : muts) {
                if (subst.count(v)) {
                    mutVars.push_back(v);
                }
            }
            std::sort(mutVars.begin(), mutVars.end());
            if (!mutVars.empty()) {
                std::function<ExprPtr()> yieldState = [&]() -> ExprPtr {
                    return stateExpr(mutVars);
                };
                ExprPtr caseE;
                if (we) {
                    std::string capability;
                    for (size_t k = 0; k < we->capability.parts.size(); ++k) {
                        if (k) {
                            capability += ".";
                        }
                        capability += we->capability.parts[k];
                    }
                    std::vector<Binding> withBinds;
                    auto replacement = atomize(we->value, withBinds);
                    std::vector<ExprPtr> putArgs;
                    putArgs.push_back(lit(LitKind::String, capability));
                    putArgs.push_back(std::move(replacement));
                    putArgs.push_back(currentContext());
                    auto extended = fresh("Ctx");
                    const auto savedContext = currentContextVar;
                    currentContextVar = extended;
                    auto snap = subst;
                    auto inner = lowerLoopBodyFrom(we->body, 0, "", mutVars, yieldState);
                    subst = snap;
                    currentContextVar = savedContext;
                    caseE = wrapLets(
                        withBinds, makeLet(
                                       extended, callE("maps", "put", 3, std::move(putArgs)),
                                       std::move(inner)));
                } else if (ie) {
                    auto branch = [&](const std::vector<ast::ExprPtr>& bb) {
                        auto snap = subst;
                        auto r = lowerLoopBodyFrom(bb, 0, "", mutVars, yieldState);
                        subst = snap;
                        return r;
                    };
                    ExprPtr elseP = ie->elseBody ? branch(*ie->elseBody) : stateExpr(mutVars);
                    for (auto it2 = ie->elifs.rbegin(); it2 != ie->elifs.rend(); ++it2) {
                        auto ec = lower(it2->first);
                        auto eb = branch(it2->second);
                        elseP = matchBool(std::move(ec), std::move(eb), std::move(elseP));
                    }
                    auto cc = lower(ie->condition);
                    auto cb = branch(ie->thenBody);
                    caseE = matchBool(std::move(cc), std::move(cb), std::move(elseP));
                } else if (te) {
                    // The rescue arms see the pre-`trying` names: the
                    // body may have thrown before any of its rebinds.
                    const auto beforeTry = subst;
                    TryCatch tc;
                    tc.body = lowerLoopBodyFrom(te->body, 0, "", mutVars, yieldState);
                    subst = beforeTry;
                    const auto& rescue = te->rescue;
                    if (rescue.isInlineReturn) {
                        MatchClause c;
                        c.patterns.push_back(wildPat());
                        auto retExpr = std::make_unique<Expr>();
                        retExpr->node = Return{lower(rescue.inlineReturnExpr)};
                        c.body = std::move(retExpr);
                        tc.clauses.push_back(std::move(c));
                    } else if (rescue.isCatchAll) {
                        MatchClause c;
                        if (rescue.catchAllParam.empty()) {
                            c.patterns.push_back(wildPat());
                        } else {
                            auto p = std::make_unique<Pattern>();
                            p->kind = PatKind::Var;
                            p->name = rescue.catchAllParam;
                            subst[rescue.catchAllParam] = rescue.catchAllParam;
                            c.patterns.push_back(std::move(p));
                        }
                        c.body = lowerLoopBodyFrom(rescue.catchAllBody, 0, "", mutVars, yieldState);
                        subst = beforeTry;
                        tc.clauses.push_back(std::move(c));
                    } else {
                        std::vector<MatchClause> rclauses;
                        for (const auto& clause : rescue.clauses) {
                            MatchClause mc;
                            mc.patterns.push_back(
                                clause.patterns.empty() ? wildPat()
                                                        : lowerPattern(clause.patterns[0]));
                            mc.guard =
                                withTypeGuards(clause.guard ? lower(*clause.guard) : nullptr);
                            mc.body = clause.body
                                          ? lowerLoopArmU(clause.body, "", mutVars, yieldState)
                                          : yieldState();
                            subst = beforeTry;
                            rclauses.push_back(std::move(mc));
                        }
                        tc.clauses = expandRescueGuards(std::move(rclauses));
                    }
                    caseE = std::make_unique<Expr>();
                    caseE->node = std::move(tc);
                } else {
                    std::vector<ExprPtr> subjects;
                    subjects.push_back(lower(me->subject));
                    std::vector<MatchClause> cls;
                    for (const auto& cl : me->clauses) {
                        auto snap = subst;
                        MatchClause mc;
                        for (const auto& p : cl.patterns) {
                            mc.patterns.push_back(lowerPattern(p));
                        }
                        mc.guard = withTypeGuards(cl.guard ? lower(*cl.guard) : nullptr);
                        mc.body = lowerLoopArmU(cl.body, "", mutVars, yieldState);
                        subst = snap;
                        cls.push_back(std::move(mc));
                    }
                    caseE = sourceMatch(std::move(subjects), std::move(cls));
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
                    vp->kind = PatKind::Var;
                    vp->name = nv;
                    pat->args.push_back(std::move(vp));
                }
                auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
                return makeMatch1(std::move(caseE), std::move(pat), std::move(rest));
            }
        }
    }
    // loop / while → tail-recursive local function threading mutable state.
    if (auto* le = std::get_if<ast::LoopExpr>(&e->kind)) {
        return lowerLoopStmt(le->body, nullptr, body, i, &le->counter);
    }
    if (auto* we = std::get_if<ast::WhileExpr>(&e->kind)) {
        return lowerLoopStmt(we->body, &we->condition, body, i);
    }

    // `return X if cond` as a non-last statement: return only when cond
    // holds, otherwise fall through to the rest (a plain ReturnExpr lowers
    // its value directly, so `return (cond ? X : ok)` would return
    // unconditionally — wrong). Mirrors the loop-body handling.
    if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind); re && !isLast) {
        if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
            // lowerBodyFrom() rebinds mutated vars — sequence it last.
            auto c = lower(ti->condition);
            auto retX = std::make_unique<Expr>();
            retX->node = Return{lower(ti->expr)};
            auto rest = lowerBodyFrom(body, i + 1);
            return matchBool(std::move(c), std::move(retX), std::move(rest));
        }
    }

    // A bare `using M, as: Alias` statement (no `do ... end`) scopes to
    // the REST of this body, not to nothing: the parser leaves its own
    // `body` empty (only the explicit block form fills it), so lowering
    // it through the generic fallthrough below applied the bindings and
    // restored them immediately, before any subsequent statement — a
    // value constructed through the alias on the very next line was
    // tagged with the alias's own name instead of the module it names
    // (kexhq/kex#alias). `isLast` still means nothing follows, so it
    // falls through as an ordinary (no-op) statement in that case.
    if (auto* ue = std::get_if<ast::UsingExpr>(&e->kind); ue && ue->body.empty() && !isLast) {
        return lowerBodyFrom(body, i + 1);
    }

    return lowerStatementThen(
        e, isLast ? std::function<ExprPtr()>{}
                  : std::function<ExprPtr()>([&] { return lowerBodyFrom(body, i + 1); }));
}

// `blockValue`: the rescue belongs to a block, where `rescue return X`
// supplies the BLOCK's value — the same thing the tree-walker does —
// rather than leaving the enclosing function the way a `return` written
// in the block's body does (kexhq/kex#408).
auto Lowering::lowerRescueClauses(const ast::RescueBlock& rescue, bool blockValue)
    -> std::vector<MatchClause> {
    std::vector<MatchClause> out;
    if (rescue.isInlineReturn) {
        MatchClause c;
        c.patterns.push_back(wildPat());
        if (blockValue) {
            c.body = lower(rescue.inlineReturnExpr);
        } else {
            auto retExpr = std::make_unique<Expr>();
            retExpr->node = Return{lower(rescue.inlineReturnExpr)};
            c.body = std::move(retExpr);
        }
        out.push_back(std::move(c));
    } else if (rescue.isCatchAll) {
        MatchClause c;
        if (rescue.catchAllParam.empty()) {
            c.patterns.push_back(wildPat());
        } else {
            auto p = std::make_unique<Pattern>();
            p->kind = PatKind::Var;
            p->name = rescue.catchAllParam;
            subst[rescue.catchAllParam] = rescue.catchAllParam;
            c.patterns.push_back(std::move(p));
        }
        c.body = lowerBody(rescue.catchAllBody);
        out.push_back(std::move(c));
    } else {
        for (const auto& clause : rescue.clauses) {
            MatchClause mc;
            if (!clause.patterns.empty()) {
                mc.patterns.push_back(lowerPattern(clause.patterns[0]));
            } else {
                mc.patterns.push_back(wildPat());
            }
            mc.guard = withTypeGuards(clause.guard ? lower(*clause.guard) : nullptr);
            mc.body = clause.body ? lower(clause.body) : lit(LitKind::Atom, "ok");
            out.push_back(std::move(mc));
        }
    }
    return out;
}

auto Lowering::wrapWithTryCatch(ExprPtr body, const ast::RescueBlock& rescue, bool blockValue)
    -> ExprPtr {
    TryCatch tc;
    tc.body = std::move(body);
    tc.clauses = expandRescueGuards(lowerRescueClauses(rescue, blockValue));
    auto ex = std::make_unique<Expr>();
    ex->node = std::move(tc);
    return ex;
}

// Does an uncaught `.try` failure escape this body? A TryThrow escapes
// unless it sits inside a TryCatch's body (which catches it). Nested
// lambdas are separate scopes, so their `.try`s don't count here.
auto Lowering::irHasEscapingTryThrow(const ExprPtr& e) -> bool {
    if (!e) {
        return false;
    }
    if (std::holds_alternative<TryThrow>(e->node)) {
        return true;
    }
    if (std::holds_alternative<Lambda>(e->node)) {
        return false;
    }
    if (const auto* tc = std::get_if<TryCatch>(&e->node)) {
        // The body's throws are caught here; only clause (rescue) bodies
        // can still escape.
        for (const auto& c : tc->clauses) {
            if (irHasEscapingTryThrow(c.body)) {
                return true;
            }
        }
        return false;
    }
    return anyChild(*e, [](const ExprPtr& child) { return irHasEscapingTryThrow(child); });
}

// Wrap a rescue-less function body so an escaped `.try` failure becomes
// `return Error(e)` — matching the interpreter, which catches an unhandled
// TryException at the function boundary (evaluator.cxx execFunctionDef).
auto Lowering::wrapSuccess(ExprPtr body, const ast::Expr* last) -> ExprPtr {
    const auto wrapper = last ? hir->successWrapperAfter(*last) : std::string{};
    if (wrapper.empty()) {
        return body;
    }
    const auto value = fresh("succeeded");
    std::vector<ExprPtr> args;
    args.push_back(var(value));
    auto wrapped = std::make_unique<Expr>();
    wrapped->node = Construct{wrapper, std::move(args)};
    return makeLet(value, std::move(body), std::move(wrapped));
}

auto Lowering::wrapPropagateTryError(ExprPtr body) -> ExprPtr {
    TryCatch tc;
    tc.body = std::move(body);
    // A failed Optional's `.try` leaves the function with `None`, a failed
    // Result's with `Error(e)`: the failure as it was, not a Result made of
    // an Optional's `None`.
    MatchClause none;
    auto nonePattern = std::make_unique<Pattern>();
    nonePattern->kind = PatKind::Construct;
    nonePattern->tag = "None";
    none.patterns.push_back(std::move(nonePattern));
    auto noneValue = std::make_unique<Expr>();
    noneValue->node = Construct{"None", {}};
    none.body = std::move(noneValue);
    tc.clauses.push_back(std::move(none));
    MatchClause c;
    std::string en = fresh("_TryE");
    auto p = std::make_unique<Pattern>();
    p->kind = PatKind::Var;
    p->name = en;
    c.patterns.push_back(std::move(p));
    auto errCtor = std::make_unique<Expr>();
    errCtor->node = Construct{"Error", one(var(en))};
    c.body = std::move(errCtor);
    tc.clauses.push_back(std::move(c));
    auto ex = std::make_unique<Expr>();
    ex->node = std::move(tc);
    return ex;
}

} // namespace kex::ir
