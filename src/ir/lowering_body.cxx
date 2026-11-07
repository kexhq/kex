// Body lowering methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"

namespace kex::ir {

// A statement sequence. `let`/`var`/reassignments introduce SSA-renamed
// bindings (updating `subst`); every other statement's value is bound to
// a throwaway name; the last statement is the sequence's value.
auto Lowering::lowerBody(const std::vector<ast::ExprPtr>& body) -> ExprPtr {
    if (body.empty()) return lit(LitKind::Atom, "ok");
    return lowerBodyFrom(body, 0);
}

auto Lowering::lowerBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i) -> ExprPtr {
    const auto& e = body[i];
    bool isLast = (i + 1 == body.size());
    currentLoc = &e->location;

    // let PATTERN = value
    if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
        if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
            auto val = lowerBindingValue(*le);
            std::string ssa = subst.count(vp->name) ? fresh(vp->name) : vp->name;
            subst[vp->name] = ssa;
            immutableBindings.insert(vp->name);
            if (const auto* curry = std::get_if<ast::CurryExpr>(&le->value->kind);
                curry && curry->argGroups.empty() && !curry->isOperator &&
                curry->module.empty())
                curryOrigin[vp->name] = curry->name;
            else
                curryOrigin.erase(vp->name);
            auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
            return makeLet(ssa, std::move(val), std::move(rest));
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
                for (const auto& name : names) subst[name] = name;
                auto rest = isLast ? lit(LitKind::Atom, "ok")
                                   : lowerBodyFrom(body, i + 1);
                // Lowering the rest moved the location on; a missing key
                // is reported at this `let`, as the walker does.
                currentLoc = &e->location;
                return makeLet(
                    rv, lower(le->value),
                    wrapAnonymousRecordPattern(rv, *rp, std::move(rest)));
            }
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
    // var PATTERN = value  (fresh mutable bindings)
    if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
        if (auto* vp = ve->pattern
                ? std::get_if<ast::VarPattern>(&ve->pattern->kind)
                : nullptr) {
            auto val = lower(ve->value);
            std::string ssa = subst.count(vp->name) ? fresh(vp->name) : vp->name;
            subst[vp->name] = ssa;
            immutableBindings.erase(vp->name);
            if (const auto* curry = std::get_if<ast::CurryExpr>(&ve->value->kind);
                curry && curry->argGroups.empty() && !curry->isOperator &&
                curry->module.empty())
                curryOrigin[vp->name] = curry->name;
            else
                curryOrigin.erase(vp->name);
            auto rest = isLast ? var(ssa) : lowerBodyFrom(body, i + 1);
            return makeLet(ssa, std::move(val), std::move(rest));
        }
        // Record destructure, mirroring `let` above — bound mutable.
        if (auto* rp = ve->pattern
                ? std::get_if<ast::RecordPattern>(&ve->pattern->kind)
                : nullptr) {
            if (rp->typeName.empty()) {
                std::string rv = fresh("Anon");
                std::vector<std::string> names;
                collectRecordBindings(*rp, names);
                for (const auto& name : names) {
                    subst[name] = name;
                    immutableBindings.erase(name);
                }
                auto rest = isLast ? lit(LitKind::Atom, "ok")
                                   : lowerBodyFrom(body, i + 1);
                currentLoc = &e->location;
                return makeLet(
                    rv, lower(ve->value),
                    wrapAnonymousRecordPattern(rv, *rp, std::move(rest)));
            }
            std::string rv = fresh("rec");
            std::vector<std::pair<std::string, ExprPtr>> prefix;
            destructureRecordPattern(rv, *rp, prefix);
            for (auto& [nm, _] : prefix) {
                subst[nm] = nm;
                immutableBindings.erase(nm);
            }
            auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
            for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
                rest = makeLet(it->first, std::move(it->second), std::move(rest));
            return makeLet(rv, lower(ve->value), std::move(rest));
        }
        auto val = lower(ve->value);
        auto pat = lowerPattern(ve->pattern);
        if (ve->pattern) {
            std::vector<std::string> names;
            patternBoundNames(ve->pattern, names);
            for (const auto& name : names) immutableBindings.erase(name);
        }
        auto rest = isLast ? lit(LitKind::Atom, "ok") : lowerBodyFrom(body, i + 1);
        return makeMatch1(std::move(val), std::move(pat), std::move(rest));
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
                if (currentLoc) loc = std::string(currentLoc->file) + ":"
                    + std::to_string(currentLoc->line) + ":"
                    + std::to_string(currentLoc->column) + ": ";
                err->node = Call{"erlang", "error", 1,
                    one(lit(LitKind::String, loc + "runtime error: Cannot use '!' on immutable binding: " + id->name)), false};
                return err;
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
            for (const auto& v : muts) if (subst.count(v)) mutVars.push_back(v);
            std::sort(mutVars.begin(), mutVars.end());
            if (!mutVars.empty()) {
                std::function<ExprPtr()> yieldState =
                    [&]() -> ExprPtr { return stateExpr(mutVars); };
                ExprPtr caseE;
                if (we) {
                    std::string capability;
                    for (size_t k = 0; k < we->capability.parts.size(); ++k) {
                        if (k) capability += ".";
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
                        withBinds,
                        makeLet(extended,
                                callE("maps", "put", 3, std::move(putArgs)),
                                std::move(inner)));
                } else if (ie) {
                    auto branch = [&](const std::vector<ast::ExprPtr>& bb) {
                        auto snap = subst;
                        auto r = lowerLoopBodyFrom(bb, 0, "", mutVars, yieldState);
                        subst = snap; return r;
                    };
                    ExprPtr elseP = ie->elseBody ? branch(*ie->elseBody)
                                                 : stateExpr(mutVars);
                    for (auto it2 = ie->elifs.rbegin(); it2 != ie->elifs.rend(); ++it2) {
                        auto ec = lower(it2->first);
                        auto eb = branch(it2->second);
                        elseP = matchBool(std::move(ec), std::move(eb),
                                          std::move(elseP));
                    }
                    auto cc = lower(ie->condition);
                    auto cb = branch(ie->thenBody);
                    caseE = matchBool(std::move(cc), std::move(cb),
                                      std::move(elseP));
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
                        c.body = lowerLoopBodyFrom(rescue.catchAllBody, 0, "",
                                                   mutVars, yieldState);
                        subst = beforeTry;
                        tc.clauses.push_back(std::move(c));
                    } else {
                        std::vector<MatchClause> rclauses;
                        for (const auto& clause : rescue.clauses) {
                            MatchClause mc;
                            mc.patterns.push_back(
                                clause.patterns.empty()
                                    ? wildPat()
                                    : lowerPattern(clause.patterns[0]));
                            mc.guard = withTypeGuards(
                                clause.guard ? lower(*clause.guard) : nullptr);
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
                        for (const auto& p : cl.patterns)
                            mc.patterns.push_back(lowerPattern(p));
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
        return lowerLoopStmt(le->body, nullptr, body, i, &le->counter);
    if (auto* we = std::get_if<ast::WhileExpr>(&e->kind))
        return lowerLoopStmt(we->body, &we->condition, body, i);

    // `return X if cond` as a non-last statement: return only when cond
    // holds, otherwise fall through to the rest (a plain ReturnExpr lowers
    // its value directly, so `return (cond ? X : ok)` would return
    // unconditionally — wrong). Mirrors the loop-body handling.
    if (auto* re = std::get_if<ast::ReturnExpr>(&e->kind); re && !isLast) {
        if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
            // lowerBodyFrom() rebinds mutated vars — sequence it last.
            auto c = lower(ti->condition);
            auto retX = std::make_unique<Expr>(); retX->node = Return{lower(ti->expr)};
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
    if (auto* ue = std::get_if<ast::UsingExpr>(&e->kind);
        ue && ue->body.empty() && !isLast) {
        auto saved = applyUsingBindings(*ue);
        auto rest = lowerBodyFrom(body, i + 1);
        restoreUsingBindings(std::move(saved));
        return rest;
    }

    if (isLast) return lower(e);
    auto val = lower(e);
    auto rest = lowerBodyFrom(body, i + 1);
    return makeLet(fresh("S"), std::move(val), std::move(rest));
}

// `blockValue`: the rescue belongs to a block, where `rescue return X`
// supplies the BLOCK's value — the same thing the tree-walker does —
// rather than leaving the enclosing function the way a `return` written
// in the block's body does (kexhq/kex#408).
auto Lowering::lowerRescueClauses(const ast::RescueBlock& rescue, bool blockValue )
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
            mc.guard = withTypeGuards(
                clause.guard ? lower(*clause.guard) : nullptr);
            mc.body = clause.body ? lower(clause.body) : lit(LitKind::Atom, "ok");
            out.push_back(std::move(mc));
        }
    }
    return out;
}

auto Lowering::wrapWithTryCatch(ExprPtr body, const ast::RescueBlock& rescue,
                      bool blockValue ) -> ExprPtr {
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
    if (!e) return false;
    if (std::holds_alternative<TryThrow>(e->node)) return true;
    if (std::holds_alternative<Lambda>(e->node)) return false;
    if (const auto* tc = std::get_if<TryCatch>(&e->node)) {
        // The body's throws are caught here; only clause (rescue) bodies
        // can still escape.
        for (const auto& c : tc->clauses)
            if (irHasEscapingTryThrow(c.body)) return true;
        return false;
    }
    return anyChild(*e, [](const ExprPtr& child) {
        return irHasEscapingTryThrow(child);
    });
}

// Wrap a rescue-less function body so an escaped `.try` failure becomes
// `return Error(e)` — matching the interpreter, which catches an unhandled
// TryException at the function boundary (evaluator.cxx execFunctionDef).
auto Lowering::wrapPropagateTryError(ExprPtr body) -> ExprPtr {
    TryCatch tc;
    tc.body = std::move(body);
    MatchClause c;
    std::string en = fresh("_TryE");
    auto p = std::make_unique<Pattern>();
    p->kind = PatKind::Var; p->name = en;
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
