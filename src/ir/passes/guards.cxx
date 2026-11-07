#include "guards.hxx"
#include "../walk.hxx"

#include <string>
#include <utility>
#include <vector>

namespace kex::ir {

namespace {

template <typename Node>
auto exprOf(Node n) -> ExprPtr {
    auto e = std::make_unique<Expr>();
    e->node = std::move(n);
    return e;
}

auto patternOf(PatKind kind, std::string name = {}) -> PatternPtr {
    auto p = std::make_unique<Pattern>();
    p->kind = kind;
    p->name = std::move(name);
    return p;
}

auto remoteCall(std::string module, std::string function, std::vector<ExprPtr> args) -> ExprPtr {
    const int arity = static_cast<int>(args.size());
    return exprOf(Call{std::move(module), std::move(function), arity, std::move(args), false});
}

auto matchesAnything(const MatchClause& clause) -> bool {
    for (const auto& p : clause.patterns)
        if (p->kind != PatKind::Var && p->kind != PatKind::Wild) return false;
    return true;
}

// Does any unguarded clause match every value, making a trailing fallback
// dead code (which erlc warns about)?
auto anyMatchesAnything(const std::vector<MatchClause>& clauses) -> bool {
    for (const auto& clause : clauses)
        if (!clause.guard && matchesAnything(clause)) return true;
    return false;
}

class Expander {
public:
    // `P when G -> B` becomes `P -> case G of true -> B; _ -> Cont() end`
    // followed by `_ -> Cont()`, where Cont is a LetRec-bound 0-arity function
    // holding the expansion of the remaining clauses, so no clause body is
    // duplicated. A guard that is not `true` fails the clause, as a native
    // guard would; rescue inverts it (`false -> Cont(); _ -> B`), since the
    // walker's rescue guards skip only on a literal `false`.
    auto expandMatch(Match match) -> ExprPtr {
        const bool rescue = match.guards == GuardExpansion::Rescue;
        auto noMatch = [&](const std::string& subject) -> ExprPtr {
            if (rescue) return exprOf(TryThrow{var(subject)});
            std::vector<ExprPtr> parts;
            parts.push_back(lit(LitKind::String,
                                match.unmatchedAt + "runtime error: no matching clause for "));
            std::vector<ExprPtr> shown;
            shown.push_back(var(subject));
            parts.push_back(remoteCall("kex_io", "inspect_plain", std::move(shown)));
            std::vector<ExprPtr> list;
            list.push_back(exprOf(MakeList{std::move(parts), std::nullopt}));
            std::vector<ExprPtr> message;
            message.push_back(remoteCall("erlang", "iolist_to_binary", std::move(list)));
            return remoteCall("erlang", "error", std::move(message));
        };
        return expandFrom(std::move(match.subjects), std::move(match.clauses), rescue, noMatch);
    }

private:
    template <typename NoMatch>
    auto expandFrom(std::vector<ExprPtr> subjects, std::vector<MatchClause> clauses,
                    bool rescue, const NoMatch& noMatch) -> ExprPtr {
        std::size_t guarded = 0;
        while (guarded < clauses.size() && !clauses[guarded].guard) ++guarded;
        if (guarded == clauses.size()) {
            // A single-subject match ends in the walker's failure: without
            // it erlc cannot compile an inexhaustive `case` (its SSA pass
            // fails, "internal error in pass core_to_ssa"). The fallback's
            // own variable binds the unmatched value, so the subject is
            // neither evaluated again nor let-bound.
            if (subjects.size() == 1 && !anyMatchesAnything(clauses)) {
                MatchClause fallback;
                const auto bound = freshName("Nomatch");
                fallback.patterns.push_back(patternOf(PatKind::Var, bound));
                fallback.body = noMatch(bound);
                clauses.push_back(std::move(fallback));
            }
            return exprOf(Match{std::move(subjects), std::move(clauses)});
        }
        // Subjects the continuation re-matches are bound to variables.
        std::vector<std::string> names;
        std::vector<std::pair<std::string, ExprPtr>> binds;
        for (auto& subject : subjects) {
            if (const auto* v = std::get_if<Var>(&subject->node)) {
                names.push_back(v->name);
            } else {
                names.push_back(freshName("Gsubj"));
                binds.push_back({names.back(), std::move(subject)});
            }
        }
        const auto subjectVars = [&] {
            std::vector<ExprPtr> vars;
            for (const auto& name : names) vars.push_back(var(name));
            return vars;
        };
        std::vector<MatchClause> rest(std::make_move_iterator(clauses.begin() + guarded + 1),
                                      std::make_move_iterator(clauses.end()));
        auto contBody = rest.empty()
            ? noMatch(names.front())
            : expandFrom(subjectVars(), std::move(rest), rescue, noMatch);
        const auto contName = "__gcont_p" + std::to_string(m_counter++);
        const auto callCont = [&] { return exprOf(Call{"", contName, 0, {}, false}); };

        Match outer;
        outer.subjects = subjectVars();
        for (std::size_t i = 0; i < guarded; ++i) outer.clauses.push_back(std::move(clauses[i]));
        auto& clause = clauses[guarded];
        const bool coversAll = matchesAnything(clause);
        Match test;
        test.subjects.push_back(std::move(*clause.guard));
        MatchClause pass;
        auto literal = patternOf(PatKind::Lit);
        literal->litKind = LitKind::Bool;
        literal->litBool = !rescue;
        pass.patterns.push_back(std::move(literal));
        MatchClause fail;
        fail.patterns.push_back(patternOf(PatKind::Wild));
        pass.body = rescue ? callCont() : std::move(clause.body);
        fail.body = rescue ? std::move(clause.body) : callCont();
        test.clauses.push_back(std::move(pass));
        test.clauses.push_back(std::move(fail));
        MatchClause guardedClause;
        guardedClause.patterns = std::move(clause.patterns);
        guardedClause.body = exprOf(std::move(test));
        outer.clauses.push_back(std::move(guardedClause));
        // An all-variable pattern already matches everything: the guard's
        // own `_ -> Cont()` arm covers failure, and a fallback would be dead.
        if (!coversAll) {
            MatchClause fallback;
            for (std::size_t i = 0; i < outer.subjects.size(); ++i)
                fallback.patterns.push_back(patternOf(PatKind::Wild));
            fallback.body = callCont();
            outer.clauses.push_back(std::move(fallback));
        }
        LetRec letrec;
        letrec.name = contName;
        letrec.funBody = std::move(contBody);
        letrec.contBody = exprOf(std::move(outer));
        auto e = exprOf(std::move(letrec));
        for (auto it = binds.rbegin(); it != binds.rend(); ++it)
            e = exprOf(Let{it->first, std::move(it->second), std::move(e)});
        return e;
    }

    auto freshName(const std::string& hint) -> std::string {
        return "_ir_p" + hint + std::to_string(m_counter++);
    }

    int m_counter = 0;

public:
    auto expandUnder(ExprPtr& e) -> void {
        if (!e) return;
        forEachChild(*e, [&](ExprPtr& child) { expandUnder(child); });
        if (auto* match = std::get_if<Match>(&e->node);
            match && match->guards != GuardExpansion::Native)
            e = expandMatch(std::move(*match));
    }
};

} // namespace

auto expandGuards(Module& module) -> void {
    Expander expander;
    for (auto& function : module.functions)
        for (auto& clause : function.clauses) {
            if (clause.guard) expander.expandUnder(*clause.guard);
            expander.expandUnder(clause.body);
        }
}

} // namespace kex::ir
