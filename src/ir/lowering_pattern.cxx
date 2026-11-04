// Patterns methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"

namespace kex::ir {

auto Lowering::wildPat() -> PatternPtr {
    auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w;
}

// Expand user-written `when` guards into nested matches. Guards lower as
// ordinary expressions (BEAM guards can't hold arbitrary calls, so the
// predicate moves out of guard position entirely). A guarded clause
// `P when G -> B` becomes `P -> case G of true -> B; _ -> Cont() end`
// followed by a wildcard clause `_ -> Cont()`, where Cont is a LetRec-bound
// 0-arity function holding the expansion of the remaining clauses — so
// clause bodies are never duplicated. A non-`true` guard result fails the
// clause, matching native guard semantics. `receive` guards are NOT
// expanded here (they must stay native; see lowerReceive).
// Does any clause match every value, making a trailing fallback dead code
// (which erlc warns about)?
auto Lowering::anyClauseIrrefutable(const std::vector<MatchClause>& clauses)
    -> bool {
    for (const auto& clause : clauses) {
        if (clause.guard) continue;
        bool irrefutable = true;
        for (const auto& p : clause.patterns)
            if (p->kind != PatKind::Var && p->kind != PatKind::Wild)
                irrefutable = false;
        if (irrefutable) return true;
    }
    return false;
}

auto Lowering::expandGuards(std::vector<ExprPtr> subjects, std::vector<MatchClause> clauses) -> ExprPtr {
    size_t gi = 0;
    while (gi < clauses.size() && !clauses[gi].guard) ++gi;
    if (gi == clauses.size()) {
        // Single-subject matches get the walker's "no matching clause"
        // failure appended; without it erlc cannot even compile an
        // inexhaustive `case` (see noClauseMatched).
        if (subjects.size() == 1 && !anyClauseIrrefutable(clauses)) {
            // The clause's own variable pattern binds the unmatched value,
            // so the subject expression is neither re-evaluated nor
            // let-bound — `case {1, 2} of` stays exactly that.
            MatchClause fallback;
            auto bound = std::make_unique<Pattern>();
            bound->kind = PatKind::Var;
            bound->name = fresh("Nomatch");
            const auto boundName = bound->name;
            fallback.patterns.push_back(std::move(bound));
            fallback.body = noClauseMatched(var(boundName));
            clauses.push_back(std::move(fallback));
        }
        auto e = std::make_unique<Expr>();
        Match m; m.subjects = std::move(subjects); m.clauses = std::move(clauses);
        e->node = std::move(m);
        return e;
    }
    // Bind non-variable subjects so the continuation can re-match them.
    std::vector<std::string> subjNames;
    std::vector<std::pair<std::string, ExprPtr>> binds;
    for (auto& s : subjects) {
        if (auto* v = std::get_if<Var>(&s->node)) {
            subjNames.push_back(v->name);
        } else {
            std::string sv = fresh("Gsubj");
            binds.push_back({sv, std::move(s)});
            subjNames.push_back(sv);
        }
    }
    auto subjExprs = [&] {
        std::vector<ExprPtr> v;
        for (const auto& n : subjNames) v.push_back(var(n));
        return v;
    };
    // Continuation: expand the remaining clauses, or reproduce the
    // no-clause-matched failure when none remain.
    std::vector<MatchClause> rest(
        std::make_move_iterator(clauses.begin() + gi + 1),
        std::make_move_iterator(clauses.end()));
    ExprPtr contBody;
    if (rest.empty()) {
        contBody = noClauseMatched(var(subjNames[0]));
    } else {
        contBody = expandGuards(subjExprs(), std::move(rest));
    }
    std::string contName = "__gcont" + std::to_string(counter++);
    auto callCont = [&] {
        auto c = std::make_unique<Expr>();
        c->node = Call{"", contName, 0, {}, false};
        return c;
    };
    Match m;
    m.subjects = subjExprs();
    for (size_t i = 0; i < gi; ++i) m.clauses.push_back(std::move(clauses[i]));
    MatchClause g;
    // An all-var/wildcard pattern already matches everything, so the
    // trailing fallback below would be dead code (erlc warns about it);
    // the guard's own `_ -> cont` arm covers the failure path.
    bool irrefutable = true;
    for (const auto& p : clauses[gi].patterns)
        if (p->kind != PatKind::Var && p->kind != PatKind::Wild)
            irrefutable = false;
    g.patterns = std::move(clauses[gi].patterns);
    // `true -> body; _ -> cont` — a non-boolean guard fails the clause.
    Match gm;
    gm.subjects.push_back(std::move(*clauses[gi].guard));
    MatchClause gt;
    auto tp = std::make_unique<Pattern>();
    tp->kind = PatKind::Lit; tp->litKind = LitKind::Bool; tp->litBool = true;
    gt.patterns.push_back(std::move(tp));
    gt.body = std::move(clauses[gi].body);
    MatchClause gf;
    gf.patterns.push_back(wildPat());
    gf.body = callCont();
    gm.clauses.push_back(std::move(gt));
    gm.clauses.push_back(std::move(gf));
    auto ge = std::make_unique<Expr>();
    ge->node = std::move(gm);
    g.body = std::move(ge);
    m.clauses.push_back(std::move(g));
    if (!irrefutable) {
        MatchClause fall;
        for (size_t i = 0; i < m.subjects.size(); ++i) fall.patterns.push_back(wildPat());
        fall.body = callCont();
        m.clauses.push_back(std::move(fall));
    }
    auto caseE = std::make_unique<Expr>();
    caseE->node = std::move(m);
    LetRec lr; lr.name = contName; lr.params = {};
    lr.funBody = std::move(contBody);
    lr.contBody = std::move(caseE);
    auto e = std::make_unique<Expr>();
    e->node = std::move(lr);
    for (auto it = binds.rbegin(); it != binds.rend(); ++it)
        e = makeLet(it->first, std::move(it->second), std::move(e));
    return e;
}

// Record-destructure `{ f, g: alias, h: { nested } }` → a flat list of
// (name, accessor-expr) bindings read by field name from `baseVar`.
// Recurses into nested record patterns.
// Read field `fname` from `base`. Prefer a direct element(pos, base) from
// the record layout — the accessor FUNCTION may be suppressed by a
// same-named user method (e.g. records.kex's `city` field vs `city`
// method), so calling `apply 'fname'/1` could hit the wrong function.
auto Lowering::fieldAccess(const std::string& fname, ExprPtr base) -> ExprPtr {
    auto it = fieldAccessors.find(fname);
    if (it != fieldAccessors.end() && !it->second.empty())
        return callE("erlang", "element", 2,
            two(litInt(it->second[0].position), std::move(base)));
    return callE("", fname, 1, one(std::move(base)));
}

// Field read when the record's type IS known — a named pattern `Foo { x }`.
// Resolving by field name alone (fieldAccess above) takes the FIRST record
// in the program declaring that name, which for common names like `value`
// or `rest` is the prelude's 5-field ParseError. That yields silently wrong
// tuple offsets for the user's own record. With a type name in hand, index
// its declared layout directly.
auto Lowering::fieldAccessIn(const std::string& typeName, const std::string& fname,
                   ExprPtr base) -> ExprPtr {
    if (!typeName.empty()) {
        if (auto rec = records.find(canonicalRecordName(typeName));
            rec != records.end()) {
            const auto& fs = rec->second.fields;
            for (size_t i = 0; i < fs.size(); i++)
                if (fs[i] == fname)
                    return callE("erlang", "element", 2,
                                 two(litInt(static_cast<int>(i) + 2),
                                     std::move(base)));
        }
    }
    return fieldAccess(fname, std::move(base));
}

void Lowering::destructureRecordPattern(const std::string& baseVar, const ast::RecordPattern& rp,
                              std::vector<std::pair<std::string, ExprPtr>>& prefix) {
    for (const auto& field : rp.fields) {
        auto acc = fieldAccessIn(rp.typeName, field.name, var(baseVar));
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
void Lowering::collectRecordBindings(const ast::RecordPattern& rp, std::vector<std::string>& names) {
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

auto Lowering::wrapRecordPattern(const std::string& baseVar, const ast::RecordPattern& rp,
                       ExprPtr body) -> ExprPtr {
    for (auto it = rp.fields.rbegin(); it != rp.fields.rend(); ++it) {
        auto value = fieldAccessIn(rp.typeName, it->name, var(baseVar));
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

// The anonymous `{ a, b }` pattern is STRUCTURAL: it matches any record
// carrying those fields, asserting no type. A record is a tagged tuple
// here, so there is no single Core Erlang pattern for that — but the set
// of record layouts is known at compile time, so the test is "the subject
// is a tuple tagged with one of the records that has every listed field".
// The fields themselves are then read through the generic accessors, which
// already dispatch on the tag.
//
// Deliberately tuple-only: a MAP does not match, matching the walker,
// whose `match` arm inspects RecordValue alone (`let { k } = map` is a
// different path and does destructure maps — see
// spec/record_pattern_anonymous.kex).
auto Lowering::anonymousRecordCandidates(const ast::RecordPattern& rp)
    -> std::vector<std::string> {
    std::vector<std::string> names;
    collectRecordBindings(rp, names);
    std::vector<std::string> tags;
    for (const auto& [record, info] : records) {
        const bool hasAll = std::all_of(
            rp.fields.begin(), rp.fields.end(), [&](const auto& field) {
                return std::find(info.fields.begin(), info.fields.end(),
                                 field.name) != info.fields.end();
            });
        if (hasAll) tags.push_back(record);
    }
    std::sort(tags.begin(), tags.end()); // stable output across runs
    return tags;
}

auto Lowering::anonymousRecordTest(const std::string& subject,
                         const ast::RecordPattern& rp) -> ExprPtr {
    auto tags = anonymousRecordCandidates(rp);
    if (tags.empty()) return litBool(false);
    Match m;
    m.subjects.push_back(
        callE("erlang", "element", 2, two(litInt(1), var(subject))));
    for (const auto& tag : tags) {
        MatchClause hit;
        auto pat = std::make_unique<Pattern>();
        pat->kind = PatKind::Lit;
        pat->litKind = LitKind::Atom;
        pat->litText = tag;
        hit.patterns.push_back(std::move(pat));
        hit.body = litBool(true);
        m.clauses.push_back(std::move(hit));
    }
    MatchClause miss;
    miss.patterns.push_back(wildPat());
    miss.body = litBool(false);
    m.clauses.push_back(std::move(miss));
    auto tagTest = std::make_unique<Expr>();
    tagTest->node = std::move(m);
    // `element/1` would raise on a non-tuple, so the tuple test has to
    // guard it rather than sit beside it in a conjunction.
    return matchBool(callE("erlang", "is_tuple", 1, one(var(subject))),
                     std::move(tagTest), litBool(false));
}

// Read `field` off a subject known to be one of `tags`. The by-name
// accessor cannot be used here: it resolves to the FIRST record declaring
// the name (`value` is the prelude ParseError's fourth field), which is a
// silently wrong offset for anyone else's record. The candidate set is
// known, so dispatch on the tag and index each layout directly.
auto Lowering::anonymousFieldRead(const std::string& subject,
                        const std::vector<std::string>& tags,
                        const std::string& field) -> ExprPtr {
    Match m;
    m.subjects.push_back(
        callE("erlang", "element", 2, two(litInt(1), var(subject))));
    for (const auto& tag : tags) {
        const auto& fields = records.at(tag).fields;
        auto at = std::find(fields.begin(), fields.end(), field);
        if (at == fields.end()) continue;
        MatchClause hit;
        auto pat = std::make_unique<Pattern>();
        pat->kind = PatKind::Lit;
        pat->litKind = LitKind::Atom;
        pat->litText = tag;
        hit.patterns.push_back(std::move(pat));
        hit.body = callE(
            "erlang", "element", 2,
            two(litInt(static_cast<int>(at - fields.begin()) + 2),
                var(subject)));
        m.clauses.push_back(std::move(hit));
    }
    // With no record declaring `field` the case had no clauses at all,
    // which erlc rejects as a syntax error, so a top-level
    // `let { k } = { k: 1 }` failed to compile.
    MatchClause miss;
    auto wild = std::make_unique<Pattern>();
    wild->kind = PatKind::Wild;
    miss.patterns.push_back(std::move(wild));
    miss.body = runtimeError("pattern mismatch");
    m.clauses.push_back(std::move(miss));
    auto byTag = std::make_unique<Expr>();
    byTag->node = std::move(m);
    // A `let { k } = someMap` destructures a map too (the walker's
    // let-binding path does). Match arms never reach this on a map — the
    // structural test above admits tuples only. The key is `"k"`, or `:k`
    // in an atom-keyed map such as `{ k: 1 }` — the string key first, as
    // in the walker — and a missing one is its "pattern mismatch", not
    // an Erlang badkey.
    auto byAtom = matchBool(
        callE("maps", "is_key", 2,
              two(lit(LitKind::Atom, field), var(subject))),
        callE("maps", "get", 2,
              two(lit(LitKind::Atom, field), var(subject))),
        runtimeError("pattern mismatch"));
    auto byKey = matchBool(
        callE("maps", "is_key", 2,
              two(lit(LitKind::String, field), var(subject))),
        callE("maps", "get", 2,
              two(lit(LitKind::String, field), var(subject))),
        std::move(byAtom));
    return matchBool(callE("erlang", "is_map", 1, one(var(subject))),
                     std::move(byKey), std::move(byTag));
}

auto Lowering::wrapAnonymousRecordPattern(const std::string& subject,
                                const ast::RecordPattern& rp,
                                ExprPtr body) -> ExprPtr {
    auto tags = anonymousRecordCandidates(rp);
    for (auto it = rp.fields.rbegin(); it != rp.fields.rend(); ++it) {
        auto value = anonymousFieldRead(subject, tags, it->name);
        if (!it->pattern) {
            body = makeLet(it->name, std::move(value), std::move(body));
        } else if (auto* vp =
                       std::get_if<ast::VarPattern>(&(*it->pattern)->kind)) {
            body = makeLet(vp->name, std::move(value), std::move(body));
        } else if (auto* nested = std::get_if<ast::RecordPattern>(
                       &(*it->pattern)->kind);
                   nested && nested->typeName.empty()) {
            std::string sub = fresh("Anon");
            body = wrapAnonymousRecordPattern(sub, *nested, std::move(body));
            body = makeLet(sub, std::move(value), std::move(body));
        } else {
            body = makeMatch1(std::move(value), lowerPattern(*it->pattern),
                              std::move(body));
        }
    }
    return body;
}

// The anonymous record pattern of a single-pattern clause, or null.
auto Lowering::anonymousRecordPattern(const std::vector<ast::PatternPtr>& pats)
    -> const ast::RecordPattern* {
    if (pats.size() != 1 || !pats[0]) return nullptr;
    auto* rp = std::get_if<ast::RecordPattern>(&pats[0]->kind);
    return rp && rp->typeName.empty() ? rp : nullptr;
}

// A top-level destructuring `let` has to expose every name it binds as its
// own 0-arity function, the same shape a simple `let name = value` gets.
// Each one re-runs the right-hand side and keeps its own component, which
// is already how top-level constants behave.
auto Lowering::topLevelPatternBinding(const ast::LetExpr& le, const std::string& name)
    -> ExprPtr {
    if (auto* rp = std::get_if<ast::RecordPattern>(&le.pattern->kind)) {
        if (rp->typeName.empty()) {
            std::string rv = fresh("Anon");
            std::vector<std::string> names;
            collectRecordBindings(*rp, names);
            for (const auto& bound : names) subst[bound] = bound;
            return makeLet(
                rv, lower(le.value),
                wrapAnonymousRecordPattern(rv, *rp, var(name)));
        }
        std::string rv = fresh("rec");
        std::vector<std::pair<std::string, ExprPtr>> prefix;
        destructureRecordPattern(rv, *rp, prefix);
        for (auto& [nm, _] : prefix) subst[nm] = nm;
        ExprPtr body = var(name);
        for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
            body = makeLet(it->first, std::move(it->second), std::move(body));
        return makeLet(rv, lower(le.value), std::move(body));
    }
    auto val = lower(le.value);
    auto pat = lowerPattern(le.pattern);
    return makeMatch1(std::move(val), std::move(pat), var(name));
}

// Combines them with whatever guard the clause already had. Always call
// it where a clause is built, even when it has no `when`: leaving a test
// behind would silently make the alternative match everything.
auto Lowering::withTypeGuards(ExprPtr existing) -> std::optional<ExprPtr> {
    ExprPtr tests;
    for (auto& test : pendingTypeGuards)
        tests = tests ? callE("erlang", "and", 2,
                              two(std::move(tests), std::move(test)))
                      : std::move(test);
    pendingTypeGuards.clear();
    if (!tests) {
        if (!existing) return std::nullopt;
        return std::optional<ExprPtr>{std::move(existing)};
    }
    if (!existing) return std::optional<ExprPtr>{std::move(tests)};
    // Type tests first, SHORT-CIRCUITED: the `when` guard may hold a
    // partial operation (a checked `kex_intrinsic_number` comparison raises
    // on mistyped values), so the old `and(guard, tests)` — strict, guard
    // first — was fatal where the walker, which type-tests first and skips
    // the guard, falls through (risk #8). The tests themselves are total
    // BIFs (`typeGuard`'s one partial shape short-circuits internally), so
    // conjoining them is safe; only the guard needs the nested case.
    // Safe in every consumer: match paths move the guard out of native
    // position via `expandGuards`, rescue drops it at emission, and
    // receive rejects type patterns outright.
    Match m;
    m.subjects.push_back(std::move(tests));
    MatchClause pass;
    auto tp = std::make_unique<Pattern>();
    tp->kind = PatKind::Lit; tp->litKind = LitKind::Bool; tp->litBool = true;
    pass.patterns.push_back(std::move(tp));
    pass.body = std::move(existing);
    MatchClause fail;
    fail.patterns.push_back(wildPat());
    fail.body = litBool(false);
    m.clauses.push_back(std::move(pass));
    m.clauses.push_back(std::move(fail));
    auto e = std::make_unique<Expr>();
    e->node = std::move(m);
    return std::optional<ExprPtr>{std::move(e)};
}

auto Lowering::lowerPattern(const ast::PatternPtr& p) -> PatternPtr {
    if (!p) { auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w; }
    return std::visit([&](const auto& pn) -> PatternPtr {
        using T = std::decay_t<decltype(pn)>;
        auto out = std::make_unique<Pattern>();
        if constexpr (std::is_same_v<T, ast::WildcardPattern>) {
            out->kind = PatKind::Wild;
        } else if constexpr (std::is_same_v<T, ast::VarPattern>) {
            out->kind = PatKind::Var; out->name = pn.name;
            subst[pn.name] = pn.name; // pattern vars are binding sites
        } else if constexpr (std::is_same_v<T, ast::TypePattern>) {
            // `s : String =>` binds like a variable and tests like a
            // guard. The test is left in pendingTypeGuards for the clause
            // being built to pick up — a pattern alone cannot carry one.
            const auto name = pn.name.empty() || pn.name == "_"
                ? fresh("Typed") : pn.name;
            out->kind = PatKind::Var; out->name = name;
            subst[name] = name;
            pendingTypeGuards.push_back(
                typeGuard(pn.type ? renderDispatchType(*pn.type) : "Any",
                          var(name)));
        } else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
            out->kind = PatKind::Lit;
            switch (pn.literal.type) {
                case TokenType::Integer: out->litKind = LitKind::Int; out->litText = pn.literal.value; break;
                case TokenType::Float:   out->litKind = LitKind::Float; out->litText = pn.literal.value; break;
                case TokenType::String:
                case TokenType::RawString:
                    out->litKind = LitKind::String;
                    out->litText = pn.literal.value;
                    break;
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
        } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            // A record is a tagged tuple {'Name', f1, f2, ...} in
            // declaration order, so a NAMED pattern is just a constructor
            // pattern: wildcard every field the pattern doesn't mention.
            // That asserts the record's type structurally, which is exactly
            // what `Foo { x }` means — no extra guard required.
            if (pn.typeName.empty())
                throw LowerError(
                    "IR lower: anonymous record pattern `{ ... }` is not "
                    "supported in match/if-let; name the record "
                    "(`Foo { ... }`) so its field layout is known");
            const auto recordName = canonicalRecordName(pn.typeName);
            auto rec = records.find(recordName);
            if (rec == records.end())
                throw LowerError("IR lower: unknown record `" + pn.typeName +
                                 "` in pattern");
            out->kind = PatKind::Construct;
            out->tag = recordName;
            for (const auto& fieldName : rec->second.fields) {
                const ast::FieldPattern* bound = nullptr;
                for (const auto& f : pn.fields)
                    if (!f.isStringKey && f.name == fieldName) { bound = &f; break; }
                if (!bound) {
                    auto w = std::make_unique<Pattern>();
                    w->kind = PatKind::Wild;
                    out->args.push_back(std::move(w));
                } else if (bound->pattern && *bound->pattern) {
                    out->args.push_back(lowerPattern(*bound->pattern));
                } else {
                    // Shorthand `{ x }` binds the field to its own name.
                    auto v = std::make_unique<Pattern>();
                    v->kind = PatKind::Var;
                    v->name = bound->name;
                    subst[bound->name] = bound->name;
                    out->args.push_back(std::move(v));
                }
            }
        } else {
            throw LowerError("IR lower: unsupported pattern kind");
        }
        return out;
    }, p->kind);
}

} // namespace kex::ir
