#include "fuse.hxx"

#include "../clone.hxx"
#include "../walk.hxx"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kex::ir {

namespace {

// How many fusions one module may take: a chain of them is one per stage.
constexpr int kMaxRounds = 8;

auto isVar(const ExprPtr& e, const std::string& name) -> bool {
    const auto* v = e ? std::get_if<Var>(&e->node) : nullptr;
    return v && v->name == name;
}

auto isSelf(const Expr& e, const FunDef& fn) -> const Call* {
    const auto* call = std::get_if<Call>(&e.node);
    return call && call->module.empty() && call->name == fn.name && call->arity == fn.arity
               ? call
               : nullptr;
}

auto callsSelf(const Expr& e, const FunDef& fn) -> bool {
    if (isSelf(e, fn)) {
        return true;
    }
    return anyChild(e, [&](const ExprPtr& c) { return callsSelf(*c, fn); });
}

auto reads(const Expr& e, const std::string& name) -> std::size_t {
    std::size_t n = 0;
    if (const auto* v = std::get_if<Var>(&e.node); v && v->name == name) {
        ++n;
    }
    forEachChild(e, [&](const ExprPtr& c) {
        if (c) {
            n += reads(*c, name);
        }
    });
    return n;
}

// A copy that walks its first parameter, a list, by its shape.
struct ListWalk {
    const FunDef* fn = nullptr;
    std::string list;
    const MatchClause* empty = nullptr;
    const MatchClause* cons = nullptr;
    const MatchClause* other = nullptr;
    std::string head;
    std::string rest;
    // Its parameters after the list, by name (none is a pattern).
    std::vector<std::string> extra;
};

auto listWalk(const FunDef& fn) -> std::optional<ListWalk> {
    if (!fn.pureCopy || fn.clauses.size() != 1 || fn.arity < 1) {
        return std::nullopt;
    }
    const auto& clause = fn.clauses.front();
    if ((clause.guard && *clause.guard) || !clause.body) {
        return std::nullopt;
    }
    ListWalk walk;
    walk.fn = &fn;
    for (std::size_t i = 0; i < clause.params.size(); ++i) {
        const auto& p = clause.params[i];
        if (!p || p->kind != PatKind::Var) {
            return std::nullopt;
        }
        if (i == 0) {
            walk.list = p->name;
        } else {
            walk.extra.push_back(p->name);
        }
    }
    const auto* match = std::get_if<Match>(&clause.body->node);
    if (!match || match->subjects.size() != 1 || !isVar(match->subjects[0], walk.list)) {
        return std::nullopt;
    }
    for (const auto& c : match->clauses) {
        if ((c.guard && *c.guard) || c.patterns.size() != 1 || !c.patterns[0] || !c.body) {
            return std::nullopt;
        }
        const auto& p = *c.patterns[0];
        if (p.kind == PatKind::List && !p.ofString && p.args.empty() && !p.rest && !walk.empty) {
            walk.empty = &c;
        } else if (
            p.kind == PatKind::List && !p.ofString && p.args.size() == 1 && p.args[0] &&
            p.args[0]->kind == PatKind::Var && p.rest && p.rest->kind == PatKind::Var &&
            !walk.cons) {
            walk.cons = &c;
            walk.head = p.args[0]->name;
            walk.rest = p.rest->name;
        } else if ((p.kind == PatKind::Var || p.kind == PatKind::Wild) && !walk.other) {
            walk.other = &c;
        } else {
            return std::nullopt;
        }
    }
    if (!walk.empty || !walk.cons) {
        return std::nullopt;
    }
    if (callsSelf(*walk.empty->body, fn) || (walk.other && callsSelf(*walk.other->body, fn))) {
        return std::nullopt;
    }
    // `rest` only as the first argument of its own recursion, and every
    // recursion walks `rest`.
    std::size_t selfCalls = 0;
    bool walksRest = true;
    std::function<void(const Expr&)> count = [&](const Expr& e) {
        if (const auto* call = isSelf(e, fn)) {
            ++selfCalls;
            walksRest = walksRest && !call->args.empty() && isVar(call->args[0], walk.rest);
        }
        forEachChild(e, [&](const ExprPtr& c) {
            if (c) {
                count(*c);
            }
        });
    };
    count(*walk.cons->body);
    if (!walksRest || reads(*walk.cons->body, walk.rest) != selfCalls) {
        return std::nullopt;
    }
    // Its list is walked, never read whole.
    if (reads(*walk.cons->body, walk.list) != 0) {
        return std::nullopt;
    }
    return walk;
}

// Whether `call` passes the walk's own parameters after the list unchanged.
auto passesOwn(const Call& call, const ListWalk& walk) -> bool {
    if (call.args.size() != walk.extra.size() + 1) {
        return false;
    }
    for (std::size_t i = 0; i < walk.extra.size(); ++i) {
        if (!isVar(call.args[i + 1], walk.extra[i])) {
            return false;
        }
    }
    return true;
}

// Whether every tail of `e` is an element and the recursion (`[e | t]`, `t`
// bound to it above) or the recursion alone, the recursion nowhere else.
auto producerTail(const Expr& e, const ListWalk& walk, std::set<std::string> recursions) -> bool {
    const auto& fn = *walk.fn;
    if (const auto* let = std::get_if<Let>(&e.node)) {
        if (!let->value || !let->body) {
            return false;
        }
        if (const auto* call = isSelf(*let->value, fn)) {
            if (!passesOwn(*call, walk)) {
                return false;
            }
            recursions.insert(let->name);
        } else if (callsSelf(*let->value, fn)) {
            return false;
        }
        return producerTail(*let->body, walk, std::move(recursions));
    }
    if (const auto* seq = std::get_if<Seq>(&e.node)) {
        if (seq->exprs.empty()) {
            return false;
        }
        for (std::size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
            if (!seq->exprs[i] || callsSelf(*seq->exprs[i], fn)) {
                return false;
            }
        }
        return seq->exprs.back() && producerTail(*seq->exprs.back(), walk, std::move(recursions));
    }
    if (const auto* match = std::get_if<Match>(&e.node)) {
        for (const auto& s : match->subjects) {
            if (!s || callsSelf(*s, fn)) {
                return false;
            }
        }
        for (const auto& c : match->clauses) {
            if (c.guard && *c.guard && callsSelf(**c.guard, fn)) {
                return false;
            }
            if (!c.body || !producerTail(*c.body, walk, recursions)) {
                return false;
            }
        }
        return true;
    }
    if (const auto* call = isSelf(e, fn)) {
        return passesOwn(*call, walk);
    }
    if (const auto* list = std::get_if<MakeList>(&e.node)) {
        if (list->elements.size() != 1 || !list->rest || !*list->rest) {
            return false;
        }
        const auto* tail = std::get_if<Var>(&(*list->rest)->node);
        return tail && recursions.contains(tail->name) && list->elements[0] &&
               !callsSelf(*list->elements[0], fn);
    }
    return false;
}

auto isProducer(const ListWalk& walk) -> bool {
    const auto* none = std::get_if<MakeList>(&walk.empty->body->node);
    if (!none || !none->elements.empty() || (none->rest && *none->rest)) {
        return false;
    }
    if (!producerTail(*walk.cons->body, walk, {})) {
        return false;
    }
    // Each recursion is read once: as the tail of its element.
    std::function<bool(const Expr&)> once = [&](const Expr& e) -> bool {
        if (const auto* let = std::get_if<Let>(&e.node); let && let->value &&
                                                         isSelf(*let->value, *walk.fn) &&
                                                         reads(*walk.cons->body, let->name) != 1) {
            return false;
        }
        return !anyChild(e, [&](const ExprPtr& c) { return !once(*c); });
    };
    return once(*walk.cons->body);
}

class Fuser {
public:
    explicit Fuser(Module& mod) : m_mod(mod) {}

    auto run() -> void {
        for (const auto& fn : m_mod.functions) {
            m_names.insert(fn.name);
        }
        for (int round = 0; round < kMaxRounds; ++round) {
            index();
            bool changed = false;
            for (std::size_t i = 0; i < m_mod.functions.size(); ++i) {
                for (auto& clause : m_mod.functions[i].clauses) {
                    if (clause.body) {
                        changed = rewrite(clause.body) || changed;
                    }
                }
            }
            for (auto& fn : m_made) {
                m_mod.functions.push_back(std::move(fn));
            }
            m_made.clear();
            if (!changed) {
                break;
            }
        }
        dropUnused();
    }

private:
    auto index() -> void {
        m_walks.clear();
        for (const auto& fn : m_mod.functions) {
            if (auto walk = listWalk(fn)) {
                m_walks[{fn.name, fn.arity}] = *walk;
            }
        }
    }

    auto walkOf(const Call& call) const -> const ListWalk* {
        if (!call.module.empty()) {
            return nullptr;
        }
        const auto found = m_walks.find({call.name, call.arity});
        return found == m_walks.end() ? nullptr : &found->second;
    }

    // `let t = P(...) in ... Q(t, ...) ...`, `t` read nowhere else and `Q`
    // called where `let` evaluates, not inside a function: `Q(F(...), ...)`.
    auto rewrite(ExprPtr& e) -> bool {
        bool changed = false;
        // `let t = (let a = x in P(a)) in b` is `let a = x in let t = P(a) in b`:
        // every name is bound once, so the producer's call is brought out to
        // where its result is bound.
        while (auto* outer = std::get_if<Let>(&e->node)) {
            if (!outer->value || !std::holds_alternative<Let>(outer->value->node)) {
                break;
            }
            const auto* tail = outer->value.get();
            while (const auto* inner = std::get_if<Let>(&tail->node)) {
                tail = inner->body.get();
            }
            const auto* call = tail ? std::get_if<Call>(&tail->node) : nullptr;
            if (!call || !walkOf(*call)) {
                break;
            }
            auto value = std::move(outer->value);
            auto& inner = std::get<Let>(value->node);
            outer->value = std::move(inner.body);
            inner.body = std::move(e);
            e = std::move(value);
        }
        if (auto* let = std::get_if<Let>(&e->node); let && let->value && let->body) {
            const auto* produced = std::get_if<Call>(&let->value->node);
            const auto* producer = produced ? walkOf(*produced) : nullptr;
            if (producer && isProducer(*producer) && reads(*let->body, let->name) == 1) {
                if (Call* consumer = consumerOf(*let->body, let->name)) {
                    if (const auto* walk = walkOf(*consumer)) {
                        const auto name = fused(*producer, *walk);
                        Call call{"", name, 0, {}, false};
                        for (auto& a : std::get<Call>(let->value->node).args) {
                            call.args.push_back(std::move(a));
                        }
                        for (std::size_t k = 1; k < consumer->args.size(); ++k) {
                            call.args.push_back(std::move(consumer->args[k]));
                        }
                        call.arity = static_cast<int>(call.args.size());
                        *consumer = std::move(call);
                        auto body = std::move(let->body);
                        e = std::move(body);
                        changed = true;
                    }
                }
            }
        }
        forEachChild(*e, [&](ExprPtr& c) {
            if (c) {
                changed = rewrite(c) || changed;
            }
        });
        return changed;
    }

    // The call that walks `name`, its first argument, outside any function.
    static auto consumerOf(Expr& e, const std::string& name) -> Call* {
        if (auto* call = std::get_if<Call>(&e.node);
            call && !call->args.empty() && isVar(call->args[0], name)) {
            return call;
        }
        if (std::holds_alternative<Lambda>(e.node) || std::holds_alternative<LetRec>(e.node) ||
            std::holds_alternative<Receive>(e.node) || std::holds_alternative<TryCatch>(e.node)) {
            return nullptr;
        }
        Call* found = nullptr;
        forEachChild(e, [&](ExprPtr& c) {
            if (c && !found) {
                found = consumerOf(*c, name);
            }
        });
        return found;
    }

    // The function that walks P's list and hands each element P puts in its
    // list to Q: P's parameters, then Q's after its list.
    auto fused(const ListWalk& p, const ListWalk& q) -> std::string {
        const auto key = std::make_pair(p.fn->name, q.fn->name);
        if (const auto made = m_fused.find(key); made != m_fused.end()) {
            return made->second;
        }
        std::string name;
        do {
            name = p.fn->name + "__fused" + std::to_string(m_counter++);
        } while (!m_names.insert(name).second);
        const auto ordinal = std::to_string(m_counter);
        const Rename inP = [&](const std::string& n) { return n + "_fp" + ordinal; };
        const Rename inQ = [&](const std::string& n) { return n + "_fq" + ordinal; };
        const int arity = p.fn->arity + q.fn->arity - 1;
        const auto& pRest = inP(p.rest);

        // A call of the fused function: P's list walked on from `rest`, P's
        // parameters as they are, and Q's as `qArgs` says.
        const auto next = [&](std::vector<ExprPtr> qArgs) {
            Call call{"", name, arity, {}, false};
            call.args.push_back(var(pRest));
            for (const auto& x : p.extra) {
                call.args.push_back(var(inP(x)));
            }
            for (auto& a : qArgs) {
                call.args.push_back(std::move(a));
            }
            auto e = std::make_unique<Expr>();
            e->node = std::move(call);
            return e;
        };
        // Q's step for one element: its `[head | rest]` clause, its head the
        // element, each of its recursions walking on through P.
        const auto step = [&](ExprPtr element) -> ExprPtr {
            auto body = cloneExpr(*q.cons->body, inQ);
            std::function<void(ExprPtr&)> recurse = [&](ExprPtr& x) {
                forEachChild(*x, [&](ExprPtr& c) {
                    if (c) {
                        recurse(c);
                    }
                });
                if (auto* call = std::get_if<Call>(&x->node); call && call->module.empty() &&
                                                              call->name == q.fn->name &&
                                                              call->arity == q.fn->arity) {
                    std::vector<ExprPtr> qArgs;
                    for (std::size_t k = 1; k < call->args.size(); ++k) {
                        qArgs.push_back(std::move(call->args[k]));
                    }
                    x = next(std::move(qArgs));
                }
            };
            recurse(body);
            auto let = std::make_unique<Expr>();
            let->node = Let{inQ(q.head), std::move(element), std::move(body)};
            return let;
        };
        // P's `[head | rest]` clause, each element it puts in its list handed
        // to Q's step, each element it skips walked past with Q as it is.
        std::function<void(ExprPtr&)> emit = [&](ExprPtr& x) {
            if (auto* let = std::get_if<Let>(&x->node);
                let && let->value && isSelf(*let->value, *p.fn)) {
                auto body = std::move(let->body);
                x = std::move(body);
                emit(x);
                return;
            }
            if (auto* list = std::get_if<MakeList>(&x->node);
                list && list->elements.size() == 1 && list->rest) {
                auto element = std::move(list->elements[0]);
                x = step(std::move(element));
                return;
            }
            if (isSelf(*x, *p.fn)) {
                std::vector<ExprPtr> qArgs;
                for (const auto& y : q.extra) {
                    qArgs.push_back(var(inQ(y)));
                }
                x = next(std::move(qArgs));
                return;
            }
            if (auto* let = std::get_if<Let>(&x->node)) {
                emit(let->body);
            } else if (auto* seq = std::get_if<Seq>(&x->node)) {
                emit(seq->exprs.back());
            } else if (auto* match = std::get_if<Match>(&x->node)) {
                for (auto& c : match->clauses) {
                    emit(c.body);
                }
            }
        };

        FunDef fn;
        fn.name = name;
        fn.arity = arity;
        fn.exported = false;
        fn.pureCopy = true;
        fn.declaringModule = p.fn->declaringModule;
        FunClause clause;
        auto param = [](std::string n) {
            auto pattern = std::make_unique<Pattern>();
            pattern->kind = PatKind::Var;
            pattern->name = std::move(n);
            return pattern;
        };
        clause.params.push_back(param(inP(p.list)));
        for (const auto& x : p.extra) {
            clause.params.push_back(param(inP(x)));
        }
        for (const auto& x : q.extra) {
            clause.params.push_back(param(inQ(x)));
        }
        Match match;
        match.subjects.push_back(var(inP(p.list)));
        {
            MatchClause none;
            none.patterns.push_back(clonePattern(*p.empty->patterns[0], inP));
            none.body = cloneExpr(*q.empty->body, inQ);
            // Its list, where Q's answer for no elements reads it: none.
            if (reads(*q.empty->body, q.list) != 0) {
                auto empty = std::make_unique<Expr>();
                empty->node = MakeList{};
                auto let = std::make_unique<Expr>();
                let->node = Let{inQ(q.list), std::move(empty), std::move(none.body)};
                none.body = std::move(let);
            }
            match.clauses.push_back(std::move(none));
        }
        {
            MatchClause cons;
            cons.patterns.push_back(clonePattern(*p.cons->patterns[0], inP));
            cons.body = cloneExpr(*p.cons->body, inP);
            emit(cons.body);
            match.clauses.push_back(std::move(cons));
        }
        if (p.other) {
            MatchClause other;
            other.patterns.push_back(clonePattern(*p.other->patterns[0], inP));
            other.body = cloneExpr(*p.other->body, inP);
            match.clauses.push_back(std::move(other));
        }
        const auto* source = std::get_if<Match>(&p.fn->clauses.front().body->node);
        match.guards = source->guards;
        match.unmatchedAt = source->unmatchedAt;
        clause.body = std::make_unique<Expr>();
        clause.body->node = std::move(match);
        fn.clauses.push_back(std::move(clause));
        m_made.push_back(std::move(fn));
        m_fused.emplace(key, name);
        return name;
    }

    // The copies nothing reaches any more, a fused function standing in: a
    // copy's call of itself keeps nothing alive.
    auto dropUnused() -> void {
        std::map<std::pair<std::string, int>, const FunDef*> byName;
        for (const auto& fn : m_mod.functions) {
            byName[{fn.name, fn.arity}] = &fn;
        }
        std::set<std::pair<std::string, int>> reached;
        std::vector<const FunDef*> work;
        for (const auto& fn : m_mod.functions) {
            if (!fn.pureCopy || fn.exported) {
                if (reached.insert({fn.name, fn.arity}).second) {
                    work.push_back(&fn);
                }
            }
        }
        std::function<void(const Expr&)> follow = [&](const Expr& e) {
            if (const auto* call = std::get_if<Call>(&e.node); call && call->module.empty()) {
                if (const auto found = byName.find({call->name, call->arity});
                    found != byName.end() && reached.insert(found->first).second) {
                    work.push_back(found->second);
                }
            }
            forEachChild(e, [&](const ExprPtr& c) {
                if (c) {
                    follow(*c);
                }
            });
        };
        while (!work.empty()) {
            const auto* fn = work.back();
            work.pop_back();
            for (const auto& clause : fn->clauses) {
                if (clause.body) {
                    follow(*clause.body);
                }
                if (clause.guard && *clause.guard) {
                    follow(**clause.guard);
                }
            }
        }
        std::erase_if(m_mod.functions, [&](const FunDef& fn) {
            return fn.pureCopy && !fn.exported && !reached.contains({fn.name, fn.arity});
        });
    }

    Module& m_mod;
    std::map<std::pair<std::string, int>, ListWalk> m_walks;
    std::map<std::pair<std::string, std::string>, std::string> m_fused;
    std::vector<FunDef> m_made;
    std::set<std::string> m_names;
    int m_counter = 0;
};

} // namespace

auto fuseLists(Module& mod) -> void {
    Fuser(mod).run();
}

} // namespace kex::ir
