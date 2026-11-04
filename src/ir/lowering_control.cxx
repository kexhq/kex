// Control flow methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"
#include "../semantic/resolve_core.hxx"

namespace kex::ir {

// Lower a branch body in its own scope (branch-local bindings must not
// leak into sibling branches or the code after the `if`/`match`).
auto Lowering::lowerBodyScoped(const std::vector<ast::ExprPtr>& body) -> ExprPtr {
    auto snap = subst;
    auto r = lowerBody(body);
    subst = snap;
    return r;
}

auto Lowering::lowerIf(const ast::IfExpr& n) -> ExprPtr {
    // `if let Pat = scrutinee ... [else ...]`: match Pat, else fall through.
    if (n.letPattern) {
        auto subj = lower(n.condition); // condition holds the scrutinee
        auto snap = subst;
        // `if let { a, b } = x`: same structural test as a match arm.
        if (auto* rp = std::get_if<ast::RecordPattern>(&n.letPattern->kind);
            rp && rp->typeName.empty()) {
            std::string sv = fresh("Anon");
            std::vector<std::string> names;
            collectRecordBindings(*rp, names);
            for (const auto& name : names) subst[name] = name;
            auto thenP = wrapAnonymousRecordPattern(sv, *rp, lowerBody(n.thenBody));
            subst = snap;
            auto elseP = n.elseBody ? lowerBodyScoped(*n.elseBody)
                                    : lit(LitKind::Atom, "ok");
            return makeLet(sv, std::move(subj),
                           matchBool(anonymousRecordTest(sv, *rp),
                                     std::move(thenP), std::move(elseP)));
        }
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

auto Lowering::lowerMatch(const ast::MatchExpr& n) -> ExprPtr {
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
    // A list pattern over a STRING subject: a String is a binary here, so
    // `[c | rest]` can never match it structurally — erlc rejects the
    // clause as unreachable and then crashes lowering it. Coerce the
    // subject to its [Char] list, exactly as a `@[...]` receiver pattern
    // does, and pack the tail back so `rest` keeps the subject's type.
    //
    // Only when no clause matches a String LITERAL at that position:
    // `as_list` would turn the subject into a list those clauses could
    // never match either, trading one broken shape for another.
    std::string listCoercedSubject;
    {
        // Seen through a `@` wrapper, which lowerPattern unwraps too:
        // `@[a | rest]` is the same list pattern, and left undetected its
        // string subject was never coerced, so the arm could not match on
        // BEAM while the walker matched it (kexhq/kex#125).
        const auto firstPattern =
            [](const ast::MatchClause& c) -> const ast::Pattern* {
            if (c.patterns.empty() || !c.patterns[0]) return nullptr;
            const ast::Pattern* p = c.patterns[0].get();
            while (const auto* self = std::get_if<ast::ThisPattern>(&p->kind)) {
                if (!self->inner) return p;
                p = self->inner.get();
            }
            return p;
        };
        const bool anyListPattern = std::any_of(
            n.clauses.begin(), n.clauses.end(), [&](const auto& c) {
                const auto* p = firstPattern(c);
                return p && std::holds_alternative<ast::ListPattern>(p->kind);
            });
        const bool anyStringPattern = std::any_of(
            n.clauses.begin(), n.clauses.end(), [&](const auto& c) {
                const auto* p = firstPattern(c);
                if (!p) return false;
                auto* lit = std::get_if<ast::LiteralPattern>(&p->kind);
                return lit && (lit->literal.type == TokenType::String ||
                               lit->literal.type == TokenType::RawString);
            });
        if (anyListPattern && !anyStringPattern)
            listCoercedSubject = fresh("LSubj");
    }
    std::vector<ExprPtr> subjects;
    ExprPtr listSubjectValue;
    if (!listCoercedSubject.empty()) {
        listSubjectValue = std::move(subjIr);
        subjects.push_back(callE("kex_intrinsic_list", "as_list", 1,
                                 one(var(listCoercedSubject))));
    } else {
        subjects.push_back(std::move(subjIr));
    }
    std::vector<MatchClause> cls;
    for (const auto& cl : n.clauses) {
        auto snap = subst;
        MatchClause mc;
        if (const auto* anon = anonymousRecordPattern(cl.patterns)) {
            // Structural: bind the subject, test its tag in the guard, and
            // read the fields in the body (and in a user `when`, which can
            // only run once the fields exist).
            std::string sv = fresh("Anon");
            auto pv = std::make_unique<Pattern>();
            pv->kind = PatKind::Var;
            pv->name = sv;
            mc.patterns.push_back(std::move(pv));
            std::vector<std::string> names;
            collectRecordBindings(*anon, names);
            for (const auto& name : names) subst[name] = name;
            auto test = anonymousRecordTest(sv, *anon);
            if (cl.guard)
                test = matchBool(
                    std::move(test),
                    wrapAnonymousRecordPattern(sv, *anon, lower(*cl.guard)),
                    litBool(false));
            mc.guard = std::move(test);
            mc.body = wrapAnonymousRecordPattern(sv, *anon, lower(cl.body));
            subst = snap;
            cls.push_back(std::move(mc));
            continue;
        }
        for (const auto& p : cl.patterns) mc.patterns.push_back(lowerPattern(p));
        // `when` guards lower as ordinary expressions; expandGuards moves
        // the predicate out of BEAM guard position into a nested match.
        mc.guard = withTypeGuards(cl.guard ? lower(*cl.guard) : nullptr);
        mc.body = lower(cl.body);
        // The tail of a list pattern keeps the SUBJECT's type: over a
        // String `[c | rest]` binds `rest` to the rest of the string.
        if (!listCoercedSubject.empty() && !mc.patterns.empty() &&
            mc.patterns[0] && mc.patterns[0]->kind == PatKind::List &&
            mc.patterns[0]->rest &&
            mc.patterns[0]->rest->kind == PatKind::Var) {
            const auto& tail = mc.patterns[0]->rest->name;
            mc.body = makeLet(tail,
                              callE("kex_intrinsic_list", "repack", 2,
                                    two(var(listCoercedSubject), var(tail))),
                              std::move(mc.body));
        }
        subst = snap;
        cls.push_back(std::move(mc));
    }
    auto e = expandGuards(std::move(subjects), std::move(cls));
    if (listSubjectValue)
        e = makeLet(listCoercedSubject, std::move(listSubjectValue),
                    std::move(e));
    if (letWrap) {
        auto r = makeLet(currentName(subjVar), std::move(letWrap), std::move(e));
        if (subjPrev) subst[subjVar] = *subjPrev; else subst.erase(subjVar);
        return r;
    }
    return e;
}

auto Lowering::lowerReceive(const ast::ReceiveExpr& n) -> ExprPtr {
    Receive r;
    if (n.senderBinding) r.senderVar = *n.senderBinding;
    for (const auto& cl : n.clauses) {
        auto snap = subst;
        if (n.senderBinding) subst[*n.senderBinding] = *n.senderBinding;
        ReceiveClause rc;
        rc.pattern = cl.patterns.empty() ? wildPat() : lowerPattern(cl.patterns[0]);
        // A receive clause has no guard slot to hold the test.
        if (!pendingTypeGuards.empty())
            throw LowerError("IR lower: a type pattern (`x : T`) is not "
                             "supported in a receive clause");
        rc.body = lower(cl.body);
        subst = snap;
        r.clauses.push_back(std::move(rc));
    }
    if (n.timeout) {
        // A timeout only ever arrives with its `after` clause — the parser
        // reads the two together, and an empty body reaches here as an
        // empty BlockExpr, not an absent one. The None fallback is for a
        // ReceiveExpr assembled some other way: a timeout emitted without
        // an after branch is a receive that blocks forever instead of
        // expiring, so it yields None like the walker does
        // (scheduler.hxx:115).
        r.timeout = lower(*n.timeout);
        r.afterBody = n.afterBody ? lower(*n.afterBody)
                                  : lit(LitKind::None, "none");
    }
    auto e = std::make_unique<Expr>(); e->node = std::move(r);
    return e;
}

// A match with range-pattern clauses. Bind the subject, then match on its
// <hd, last> bounds; each `(a..b)` clause → the 2-pattern `<a, b>`.
auto Lowering::lowerRangeMatch(const ast::MatchExpr& n) -> ExprPtr {
    std::string sv = fresh("RSubj");
    auto subjVal = lower(n.subject);
    std::vector<ExprPtr> subjects;
    subjects.push_back(callE("kex_intrinsic_range", "first", 1, one(var(sv))));
    subjects.push_back(callE("kex_intrinsic_range", "last", 1, one(var(sv))));
    std::vector<MatchClause> cls;
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
        mc.guard = withTypeGuards(cl.guard ? lower(*cl.guard) : nullptr);
        mc.body = lower(cl.body);
        subst = snap;
        cls.push_back(std::move(mc));
    }
    auto e = expandGuards(std::move(subjects), std::move(cls));
    return makeLet(sv, std::move(subjVal), std::move(e));
}

// Collect the names a loop body reassigns — plain `x = ...` AND mutating
// `x.push!(..)` calls — recursing through if/match/block/nested-loops but
// not lambdas. These become the loop's threaded state.
void Lowering::collectMutated(const ast::ExprPtr& e, std::unordered_set<std::string>& out) {
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
        } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            for (auto& cl : nn.clauses) collectMutated(cl.body, out);
            if (nn.afterBody) collectMutated(*nn.afterBody, out);
        } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            for (auto& s : nn.body) collectMutated(s, out);
        } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            for (auto& s : nn.body) collectMutated(s, out);
        } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            for (auto& s : nn.body) collectMutated(s, out);
        } else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
            for (auto& s : nn.body) collectMutated(s, out);
            for (auto& cl : nn.rescue.clauses) collectMutated(cl.body, out);
            for (auto& s : nn.rescue.catchAllBody) collectMutated(s, out);
        } else if constexpr (std::is_same_v<T, ast::WithExpr>) {
            for (auto& s : nn.body) collectMutated(s, out);
        }
    }, e->kind);
}

// A loop in expression position has no continuation to thread its state
// into: the rebound names would only be in scope inside the expression
// itself, so mutations could not escape it. Reject that up front rather
// than emitting Core that references them afterwards.
auto Lowering::requireNoOuterMutation(const std::vector<ast::ExprPtr>& body,
                            const char* what) -> void {
    std::unordered_set<std::string> mset;
    for (const auto& s : body) collectMutated(s, mset);
    for (const auto& v : mset)
        if (subst.count(v))
            throw LowerError(std::string("IR lower: `") + what +
                             "` used as an expression cannot assign to `" + v +
                             "` — put it in statement position instead");
}

// The loop's current threaded-state value: bare var for one, tuple for
// several, 'ok' for none.
auto Lowering::stateExpr(const std::vector<std::string>& mutVars) -> ExprPtr {
    if (mutVars.empty()) return lit(LitKind::Atom, "ok");
    if (mutVars.size() == 1) return var(currentName(mutVars[0]));
    std::vector<ExprPtr> els;
    for (const auto& v : mutVars) els.push_back(var(currentName(v)));
    auto e = std::make_unique<Expr>(); e->node = MakeTuple{std::move(els)}; return e;
}

auto Lowering::tailCall(const std::string& loopFn, const std::vector<std::string>& mutVars) -> ExprPtr {
    auto ctrIt = loopCounters.find(loopFn);
    const std::string ctr = ctrIt == loopCounters.end() ? "" : ctrIt->second;
    std::vector<ExprPtr> args;
    std::string bumped;
    for (const auto& v : mutVars) {
        if (!ctr.empty() && v == ctr) {
            // Call arguments must be atomic, so the +1 gets its own let.
            bumped = fresh(ctr);
            args.push_back(var(bumped));
        } else {
            args.push_back(var(currentName(v)));
        }
    }
    auto e = std::make_unique<Expr>();
    e->node = Call{"", loopFn, (int)mutVars.size(), std::move(args), false};
    if (bumped.empty()) return e;
    return makeLet(bumped, intrin(Op::Add, two(var(currentName(ctr)), litInt(1))),
                   std::move(e));
}

// A `let` binding's value: a local `let UPPER = ...` binds the body's
// value, not the zero-arg function the parser wraps it in, so apply the
// fun immediately (kexhq/kex#603). The fun's own return scope keeps a
// `return` inside the body from escaping, matching the walker's eager
// call; anything else lowers as written.
auto Lowering::lowerBindingValue(const ast::LetExpr& le) -> ExprPtr {
    if (!ast::isLocalConstantBind(le.pattern, le.value))
        return lower(le.value);
    std::vector<Binding> binds;
    auto fun = atomize(le.value, binds);
    auto call = std::make_unique<Expr>();
    call->node = CallIndirect{std::move(fun), {}, false};
    return wrapLets(binds, std::move(call));
}

// If `e` is a break/next/return, its loop-control IR; else nullptr.
auto Lowering::loopControl(const ast::ExprPtr& e, const std::string& loopFn,
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
auto Lowering::lowerLoopBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i,
                       const std::string& loopFn,
                       const std::vector<std::string>& mutVars,
                       const std::function<ExprPtr()>& onEnd) -> ExprPtr {
    if (i >= body.size()) return onEnd();
    const auto& e = body[i];
    auto cont = [&]() -> ExprPtr { return lowerLoopBodyFrom(body, i + 1, loopFn, mutVars, onEnd); };
    if (std::holds_alternative<ast::BreakExpr>(e->kind)) return stateExpr(mutVars);
    if (std::holds_alternative<ast::NextExpr>(e->kind)) return tailCall(loopFn, mutVars);
    if (auto* ti = std::get_if<ast::TrailingIf>(&e->kind))
        if (auto ctrl = loopControl(ti->expr, loopFn, mutVars)) {
            // The condition must be lowered against the subst in effect
            // HERE: cont() lowers the rest of the loop body, which
            // rebinds mutated vars to fresh SSA names. Passing both as
            // arguments to matchBool() leaves them unsequenced, and a
            // compiler that evaluates right-to-left (gcc on x86_64) then
            // renders the condition with names bound later in the body —
            // an unbound variable in the emitted Core Erlang.
            auto c = lower(ti->condition);
            auto rest = cont();
            return matchBool(std::move(c), std::move(ctrl), std::move(rest));
        }
    if (auto* ae = std::get_if<ast::AssignExpr>(&e->kind)) {
        auto val = lowerAssignmentValue(*ae);
        std::string nv = fresh(ae->name); subst[ae->name] = nv;
        return makeLet(nv, std::move(val), cont());
    }
    if (auto* ve = std::get_if<ast::VarExpr>(&e->kind)) {
        if (auto* vp = ve->pattern
                ? std::get_if<ast::VarPattern>(&ve->pattern->kind)
                : nullptr) {
            auto val = lower(ve->value);
            std::string nv = fresh(vp->name); subst[vp->name] = nv;
            immutableBindings.erase(vp->name);
            if (const auto* curry = std::get_if<ast::CurryExpr>(&ve->value->kind);
                curry && curry->argGroups.empty() && !curry->isOperator &&
                curry->module.empty())
                curryOrigin[vp->name] = curry->name;
            else
                curryOrigin.erase(vp->name);
            return makeLet(nv, std::move(val), cont());
        }
        auto val = lower(ve->value); auto pat = lowerPattern(ve->pattern);
        // Destructured names are mutable: reassignment and `!` rebind
        // them, so they must not linger in immutableBindings.
        if (ve->pattern) {
            std::vector<std::string> names;
            patternBoundNames(ve->pattern, names);
            for (const auto& name : names) immutableBindings.erase(name);
        }
        return makeMatch1(std::move(val), std::move(pat), cont());
    }
    if (auto* le = std::get_if<ast::LetExpr>(&e->kind)) {
        if (auto* vp = std::get_if<ast::VarPattern>(&le->pattern->kind)) {
            auto val = lowerBindingValue(*le);
            std::string nv = fresh(vp->name); subst[vp->name] = nv;
            immutableBindings.insert(vp->name);
            if (const auto* curry = std::get_if<ast::CurryExpr>(&le->value->kind);
                curry && curry->argGroups.empty() && !curry->isOperator &&
                curry->module.empty())
                curryOrigin[vp->name] = curry->name;
            else
                curryOrigin.erase(vp->name);
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
            // Sequenced for the same reason as the TrailingIf case above.
            auto c = lower(ti->condition);
            auto retX = std::make_unique<Expr>(); retX->node = Return{lower(ti->expr)};
            auto rest = cont();
            return matchBool(std::move(c), std::move(retX), std::move(rest));
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
        auto fallthrough = [&]() {
            auto snap = subst;
            auto r = cont();
            subst = snap;
            return r;
        };
        ExprPtr elseP = ie->elseBody ? branch(*ie->elseBody)
                                     : fallthrough();
        for (auto it = ie->elifs.rbegin(); it != ie->elifs.rend(); ++it) {
            auto c = lower(it->first);
            auto body = branch(it->second);
            elseP = matchBool(std::move(c), std::move(body),
                              std::move(elseP));
        }
        auto c = lower(ie->condition);
        auto thenP = branch(ie->thenBody);
        return matchBool(std::move(c), std::move(thenP),
                         std::move(elseP));
    }
    if (auto* me = std::get_if<ast::MatchExpr>(&e->kind)) {
        if (me->subjectBinding)
            throw LowerError("IR lower: match |binder| in loop not yet ported");
        std::function<ExprPtr()> armEnd = [&]() -> ExprPtr { return cont(); };
        std::vector<ExprPtr> subjects;
        subjects.push_back(lower(me->subject));
        std::vector<MatchClause> cls;
        for (const auto& cl : me->clauses) {
            auto snap = subst;
            MatchClause mcx;
            for (const auto& p : cl.patterns) mcx.patterns.push_back(lowerPattern(p));
            mcx.guard = withTypeGuards(cl.guard ? lower(*cl.guard) : nullptr);
            mcx.body = lowerLoopArmU(cl.body, loopFn, mutVars, armEnd);
            subst = snap;
            cls.push_back(std::move(mcx));
        }
        return expandGuards(std::move(subjects), std::move(cls));
    }
    // A receive in a loop body — the imperative server shape
    // `loop do receive do :inc => n = n + 1 ... end end`. Like a match,
    // each clause (and the `after` branch) continues with the rest of the
    // loop body, so an assignment in an arm threads into the next
    // iteration instead of being lowered as a free-standing expression.
    if (auto* re = std::get_if<ast::ReceiveExpr>(&e->kind)) {
        std::function<ExprPtr()> armEnd = [&]() -> ExprPtr { return cont(); };
        Receive r;
        if (re->senderBinding) r.senderVar = *re->senderBinding;
        for (const auto& cl : re->clauses) {
            auto snap = subst;
            if (re->senderBinding) subst[*re->senderBinding] = *re->senderBinding;
            ReceiveClause rc;
            rc.pattern = cl.patterns.empty() ? wildPat() : lowerPattern(cl.patterns[0]);
            if (!pendingTypeGuards.empty())
                throw LowerError("IR lower: a type pattern (`x : T`) is not "
                                 "supported in a receive clause");
            rc.body = lowerLoopArmU(cl.body, loopFn, mutVars, armEnd);
            subst = snap;
            r.clauses.push_back(std::move(rc));
        }
        if (re->timeout) {
            r.timeout = lower(*re->timeout);
            auto snap = subst;
            r.afterBody = re->afterBody
                ? lowerLoopArmU(*re->afterBody, loopFn, mutVars, armEnd)
                : armEnd();
            subst = snap;
        }
        auto ex = std::make_unique<Expr>(); ex->node = std::move(r);
        return ex;
    }
    if (auto* le2 = std::get_if<ast::LoopExpr>(&e->kind))
        return lowerLoopCore(le2->body, nullptr, false, [&]{ return cont(); }, &le2->counter);
    if (auto* we2 = std::get_if<ast::WhileExpr>(&e->kind))
        return lowerLoopCore(we2->body, &we2->condition, false, [&]{ return cont(); });
    if (auto* te = std::get_if<ast::TryingExpr>(&e->kind)) {
        TryCatch tc;
        auto snapBeforeTry = subst;
        tc.body = lowerLoopBodyFrom(te->body, 0, loopFn, mutVars, onEnd);
        // Restore subst so rescue clauses reference pre-try variable names
        subst = snapBeforeTry;
        auto& rescue = te->rescue;
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
            c.body = lowerLoopBodyFrom(rescue.catchAllBody, 0, loopFn, mutVars, onEnd);
            tc.clauses.push_back(std::move(c));
        } else {
            std::vector<MatchClause> rclauses;
            for (const auto& clause : rescue.clauses) {
                MatchClause mc;
                if (!clause.patterns.empty())
                    mc.patterns.push_back(lowerPattern(clause.patterns[0]));
                else
                    mc.patterns.push_back(wildPat());
                mc.guard = withTypeGuards(
                    clause.guard ? lower(*clause.guard) : nullptr);
                mc.body = clause.body
                    ? lowerLoopArmU(clause.body, loopFn, mutVars, [&]{ return cont(); })
                    : cont();
                // Each arm is an independent scope: without the reset the
                // next arm's continuation references this arm's loop-var
                // rebindings (`unbound variable` from erlc). Mirrors the
                // per-arm reset in lowerBodyFrom's trying path.
                subst = snapBeforeTry;
                rclauses.push_back(std::move(mc));
            }
            tc.clauses = expandRescueGuards(std::move(rclauses));
        }
        auto ex = std::make_unique<Expr>();
        ex->node = std::move(tc);
        return ex;
    }
    auto val = lower(e);
    return makeLet(fresh("S"), std::move(val), cont());
}

// A single match-arm body in loop context (a `do` block is a statement
// list; a bare expr is one statement), continuing with `armEnd`.
auto Lowering::lowerLoopArmU(const ast::ExprPtr& arm, const std::string& loopFn,
                   const std::vector<std::string>& mutVars,
                   const std::function<ExprPtr()>& armEnd) -> ExprPtr {
    if (auto* be = std::get_if<ast::BlockExpr>(&arm->kind))
        return lowerLoopBodyFrom(be->body, 0, loopFn, mutVars, armEnd);
    if (std::holds_alternative<ast::BreakExpr>(arm->kind)) return stateExpr(mutVars);
    if (std::holds_alternative<ast::NextExpr>(arm->kind)) return tailCall(loopFn, mutVars);
    if (auto* ti = std::get_if<ast::TrailingIf>(&arm->kind))
        if (auto ctrl = loopControl(ti->expr, loopFn, mutVars)) {
            // armEnd() rebinds mutated vars — lower the condition first.
            auto c = lower(ti->condition);
            auto rest = armEnd();
            return matchBool(std::move(c), std::move(ctrl), std::move(rest));
        }
    if (auto* re = std::get_if<ast::ReturnExpr>(&arm->kind)) {
        if (auto* ti = std::get_if<ast::TrailingIf>(&re->value->kind)) {
            auto c = lower(ti->condition);
            auto retX = std::make_unique<Expr>(); retX->node = Return{lower(ti->expr)};
            auto rest = armEnd();
            return matchBool(std::move(c), std::move(retX), std::move(rest));
        }
        auto ex = std::make_unique<Expr>(); ex->node = Return{lower(re->value)}; return ex;
    }
    if (auto* ae = std::get_if<ast::AssignExpr>(&arm->kind)) {
        auto val = lowerAssignmentValue(*ae);
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
auto Lowering::lowerMutatingAsValue(const ast::MethodCall& mc) -> ExprPtr {
    m_lowerMutatingAsValue = true;
    auto r = lowerMethodCall(mc);
    m_lowerMutatingAsValue = false;
    return r;
}

auto Lowering::lowerAssignmentValue(const ast::AssignExpr& assignment) -> ExprPtr {
    if (assignment.path.empty()) return lower(assignment.value);
    if (assignment.path.size() != 1)
        throw LowerError(
            "IR lower: nested record-field assignment is not supported");
    std::vector<Binding> binds;
    auto value = atomize(assignment.value, binds);
    auto updated = callE(
        "kex_intrinsic_type", "updateRecord", 3,
        three(var(currentName(assignment.name)),
              lit(LitKind::Atom, assignment.path.front()),
              std::move(value)));
    return wrapLets(binds, std::move(updated));
}

// Lower a `loop`/`while` to a LetRec threading its mutable state.
// `restIsLast`: the loop is the last statement (its result is the value).
// `mkRest`: produces the continuation after the loop (with the extracted
// state already in subst) — for a top-level loop this is the rest of the
// enclosing body; for a NESTED loop it's the rest of the OUTER loop body
// (so it tail-calls the outer loop).
auto Lowering::lowerLoopCore(const std::vector<ast::ExprPtr>& loopBody, const ast::ExprPtr* cond,
                   bool restIsLast, const std::function<ExprPtr()>& mkRest,
                   const std::optional<std::string>* counterVar ) -> ExprPtr {
    std::unordered_set<std::string> mset;
    for (const auto& s : loopBody) collectMutated(s, mset);
    std::vector<std::string> mutVars;
    for (const auto& v : mset) if (subst.count(v)) mutVars.push_back(v);
    std::sort(mutVars.begin(), mutVars.end());

    std::string loopFn = "__loop" + std::to_string(counter++);
    // A named `loop do |i|` counter becomes an extra loop-carried slot,
    // starting at 0 and bumped by every tail call. It exists only for the
    // duration of the loop, so it is never rebound afterwards.
    std::string ctr;
    if (counterVar && *counterVar && **counterVar != "_") {
        ctr = **counterVar;
        mutVars.erase(std::remove(mutVars.begin(), mutVars.end(), ctr), mutVars.end());
        mutVars.push_back(ctr);
        loopCounters[loopFn] = ctr;
    }
    std::vector<ExprPtr> initArgs;
    for (const auto& v : mutVars)
        initArgs.push_back(v == ctr && !ctr.empty() ? litInt(0) : var(currentName(v)));

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
            // The counter slot is loop-local: leave it out of subst so it
            // is not visible (or bound) after the loop.
            if (!ctr.empty() && mutVars[k] == ctr) { boundNames.emplace_back(); continue; }
            std::string nv = fresh(mutVars[k]); subst[mutVars[k]] = nv; boundNames.push_back(nv);
        }
        auto rest = restIsLast ? (ctr.empty() ? stateExpr(mutVars) : var(resVar)) : mkRest();
        ExprPtr chained = std::move(rest);
        if (mutVars.size() == 1) {
            if (!boundNames[0].empty())
                chained = makeLet(boundNames[0], var(resVar), std::move(chained));
        } else {
            // Only unpack the loop-carried slots the continuation actually
            // reads: an `element/2` bound to a dead name makes erlc warn
            // that the call's result is ignored (loop counters hit this
            // constantly, since they're live only inside the loop).
            for (size_t k = mutVars.size(); k-- > 0; ) {
                if (boundNames[k].empty()) continue;
                if (!mentionsVar(*chained, boundNames[k])) continue;
                chained = makeLet(boundNames[k],
                    callE("erlang", "element", 2, two(litInt((long)k + 1), var(resVar))),
                    std::move(chained));
            }
        }
        lr.contBody = makeLet(resVar, std::move(callLoop), std::move(chained));
    }
    auto e = std::make_unique<Expr>(); e->node = std::move(lr);
    return e;
}

// Top-level loop statement: continuation is the rest of the enclosing body.
auto Lowering::lowerLoopStmt(const std::vector<ast::ExprPtr>& loopBody, const ast::ExprPtr* cond,
                   const std::vector<ast::ExprPtr>& outer, size_t outerStart,
                   const std::optional<std::string>* counterVar ) -> ExprPtr {
    bool restIsLast = (outerStart + 1 >= outer.size());
    return lowerLoopCore(loopBody, cond, restIsLast,
        [&]{ return lowerBodyFrom(outer, outerStart + 1); }, counterVar);
}

// `using M` where M is a separately compiled module — the prelude's
// `Supervisor`, `Task`, `Process` — imports its exports under their bare
// names. `moduleImports` only ever sees modules of this unit, so a bare
// `worker { ... }` after `using Supervisor` used to lower to a local call
// nothing defines, and died with "Undefined function" at runtime while
// the tree-walker ran it fine. A module of this unit with the same name
// keeps its own import.
auto Lowering::importExternalModule(const std::string& srcMod,
                          const std::vector<std::string>& only,
                          const std::vector<std::string>& except) -> void {
    if (!externalModules) return;
    const auto prefix = srcMod + ".";
    for (const auto& [qualKey, _] : externalModules->exportToBeamFn) {
        if (qualKey.rfind(prefix, 0) != 0) continue;
        const auto bare = qualKey.substr(prefix.size());
        if (bare.find('.') != std::string::npos) continue;
        if (!kex::semantic::resolve_core::usingSelectsName(bare, only, except))
            continue;
        if (moduleImports.count(bare)) continue;
        externalImports[bare] = qualKey;
    }
}

// A bare call to a name `using` imported from a separately compiled
// module, lowered exactly as its qualified spelling `M.name(...)` is.
auto Lowering::lowerExternalImportCall(const ast::FunctionCall& n,
                             const std::string& qualKey) -> ExprPtr {
    const auto owner = qualKey.substr(0, qualKey.rfind('.'));
    const auto atom = externalModules->nameToAtom.at(owner);
    const auto beamFn = externalModules->exportToBeamFn.at(qualKey);
    std::vector<Binding> binds;
    std::vector<ExprPtr> args;
    if (!n.namedArgs.empty()) {
        auto pit = externalModules->exportParamNames.find(qualKey);
        if (pit == externalModules->exportParamNames.end())
            throw LowerError("IR lower: named args to external function with unknown params: " + qualKey);
        const auto& pnames = pit->second;
        args.resize(pnames.size());
        for (const auto& [an, av] : n.namedArgs)
            for (size_t i = 0; i < pnames.size(); i++)
                if (pnames[i] == an) { args[i] = atomize(av, binds); break; }
        std::vector<ExprPtr> positional;
        for (const auto& a : n.args) positional.push_back(atomize(a, binds));
        if (n.block) positional.push_back(atomize(*n.block, binds));
        size_t next = 0;
        for (auto& p : positional) {
            while (next < args.size() && args[next]) next++;
            if (next >= args.size()) break;
            args[next] = std::move(p);
        }
        if (auto defaults = externalModules->exportDefaults.find(qualKey);
            defaults != externalModules->exportDefaults.end())
            fillImportedDefaults(owner, pnames, defaults->second, args, binds);
        for (auto& a : args) if (!a) a = lit(LitKind::None, "none");
    } else {
        for (const auto& a : n.args) args.push_back(atomize(a, binds));
        if (n.block) args.push_back(atomize(*n.block, binds));
        // Fewer arguments than parameters: the rest are the callee's
        // defaults, which only its interface knows.
        const auto defaults = externalModules->exportDefaults.find(qualKey);
        const auto names = externalModules->exportParamNames.find(qualKey);
        if (defaults != externalModules->exportDefaults.end() &&
            names != externalModules->exportParamNames.end() &&
            args.size() < names->second.size()) {
            args.resize(names->second.size());
            fillImportedDefaults(owner, names->second, defaults->second, args, binds);
        }
    }
    const int ar = static_cast<int>(args.size());
    return wrapLets(binds, externalCallExpr(qualKey, atom, beamFn, ar,
                                            std::move(args)));
}

// Applies one `using Module[, as: Alias][, only:/except: ...]` statement's
// import/alias bindings, returning the pre-existing state for the caller
// to restore once the bindings go out of scope. Shared by both spellings
// this can take: `using M do ... end` (an expression; the caller lowers
// `n.body` itself while the bindings are live, matching them exactly to
// that block) and a BARE `using M, as: Alias` statement with no `do` —
// the parser leaves ITS `body` empty, and its scope is instead the REST
// of the enclosing block, so `lowerBodyFrom`'s bare-statement case below
// calls this the same way and treats what follows as that scope.
auto Lowering::applyUsingBindings(const ast::UsingExpr& n) -> UsingBindingsSaved {
    UsingBindingsSaved saved{moduleImports, moduleAliases, usingModules,
                             externalImports};
    std::string srcMod;
    for (size_t i = 0; i < n.module.parts.size(); i++) {
        if (i) srcMod += ".";
        srcMod += n.module.parts[i];
    }
    applyUsingCore(srcMod, n.alias, n.onlyNames, n.exceptNames);
    return saved;
}

auto Lowering::snapshotUsingBindings() const -> UsingBindingsSaved {
    return {moduleImports, moduleAliases, usingModules, externalImports};
}

// The bindings half of a `using Module[, as: Alias][, only:/except: ...]`,
// shared by the expression form (applyUsingBindings, scoped by its caller),
// the top-level form, and the module-body form (both persistent when bare,
// scoped around a `do ... end` body). One core so the three contexts cannot
// drift apart again (Phase 2 step 2).
auto Lowering::applyUsingCore(const std::string& srcMod,
                              const std::optional<std::string>& alias,
                              const std::vector<std::string>& onlyNames,
                              const std::vector<std::string>& exceptNames)
    -> void {
    usingModules.insert(srcMod);
    if (alias) moduleAliases[*alias] = srcMod;
    // Import immediate nested modules — and M's own last segment and
    // sibling modules — under their leaf name too. The semantic resolver
    // already makes `URL.parse` available after `using URI`; BEAM
    // lowering must map that leaf back to its owning companion
    // (`URI.URL`) just as the tree walker does. Explicit aliases still
    // win: registerModuleAliases only fills a name in when nothing has
    // claimed it yet.
    registerModuleAliases(srcMod, onlyNames, exceptNames);
    if (!onlyNames.empty()) {
        for (const auto& name : onlyNames) {
            auto key = srcMod + "." + name;
            if (auto it = moduleFunctions.find(key); it != moduleFunctions.end())
                moduleImports[name] = it->second;
        }
    } else if (!alias) {
        // `as:` alone imports nothing; the alias above is all it binds.
        for (const auto& [key, val] : moduleFunctions)
            if (key.rfind(srcMod + ".", 0) == 0) {
                auto bare = key.substr(srcMod.size() + 1);
                if (bare.find('.') == std::string::npos &&
                    kex::semantic::resolve_core::usingSelectsName(
                        bare, onlyNames, exceptNames))
                    moduleImports[bare] = val;
            }
    }
    if (kex::semantic::resolve_core::usingImportsNames(alias, onlyNames))
        importExternalModule(srcMod, onlyNames, exceptNames);
}

auto Lowering::restoreUsingBindings(UsingBindingsSaved saved) -> void {
    moduleImports = std::move(saved.imports);
    moduleAliases = std::move(saved.aliases);
    usingModules = std::move(saved.using_);
    externalImports = std::move(saved.externalImports);
}

} // namespace kex::ir
