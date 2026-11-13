#include "clone.hxx"

#include "walk.hxx"

namespace kex::ir {

namespace {

struct Cloner {
    const Rename& rename;
    // The `letrec` functions bound in the copied code so far: a local call
    // to one of them is renamed with it.
    std::set<std::string> localFunctions;

    auto renamed(const std::string& n) const -> std::string { return rename ? rename(n) : n; }

    auto pattern(const Pattern& p) const -> PatternPtr {
        auto out = std::make_unique<Pattern>();
        out->kind = p.kind;
        out->litKind = p.litKind;
        out->litText = p.litText;
        out->litBool = p.litBool;
        out->name = p.kind == PatKind::Var ? renamed(p.name) : p.name;
        out->tag = p.tag;
        for (const auto& a : p.args) out->args.push_back(a ? pattern(*a) : nullptr);
        if (p.rest) out->rest = pattern(*p.rest);
        out->ofString = p.ofString;
        return out;
    }

    auto copyEach(const std::vector<ExprPtr>& es) -> std::vector<ExprPtr> {
        std::vector<ExprPtr> out;
        out.reserve(es.size());
        for (const auto& e : es) out.push_back(maybe(e));
        return out;
    }

    auto maybe(const ExprPtr& e) -> ExprPtr { return e ? expr(*e) : nullptr; }

    auto clauses(const std::vector<MatchClause>& cs) -> std::vector<MatchClause> {
        std::vector<MatchClause> out;
        for (const auto& c : cs) {
            MatchClause copy;
            for (const auto& p : c.patterns) copy.patterns.push_back(p ? pattern(*p) : nullptr);
            if (c.guard) copy.guard = maybe(*c.guard);
            copy.body = maybe(c.body);
            out.push_back(std::move(copy));
        }
        return out;
    }

    auto expr(const Expr& e) -> ExprPtr {
        auto out = std::make_unique<Expr>();
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Lit>) {
                out->node = n;
            } else if constexpr (std::is_same_v<T, Var>) {
                out->node = Var{renamed(n.name)};
            } else if constexpr (std::is_same_v<T, Intrinsic>) {
                out->node = Intrinsic{n.op, copyEach(n.args)};
            } else if constexpr (std::is_same_v<T, Call>) {
                const bool local = n.module.empty() && localFunctions.contains(n.name);
                out->node = Call{n.module, local ? renamed(n.name) : n.name, n.arity, copyEach(n.args),
                                 n.tail};
            } else if constexpr (std::is_same_v<T, CallIndirect>) {
                out->node = CallIndirect{maybe(n.callee), copyEach(n.args), n.tail};
            } else if constexpr (std::is_same_v<T, Let>) {
                out->node = Let{renamed(n.name), maybe(n.value), maybe(n.body)};
            } else if constexpr (std::is_same_v<T, Seq>) {
                out->node = Seq{copyEach(n.exprs)};
            } else if constexpr (std::is_same_v<T, Match>) {
                out->node = Match{copyEach(n.subjects), clauses(n.clauses), n.guards, n.unmatchedAt};
            } else if constexpr (std::is_same_v<T, Construct>) {
                out->node = Construct{n.tag, copyEach(n.args)};
            } else if constexpr (std::is_same_v<T, MakeTuple>) {
                out->node = MakeTuple{copyEach(n.elements)};
            } else if constexpr (std::is_same_v<T, MakeList>) {
                out->node = MakeList{copyEach(n.elements),
                                     n.rest ? std::optional(maybe(*n.rest)) : std::nullopt};
            } else if constexpr (std::is_same_v<T, FieldGet>) {
                out->node = FieldGet{maybe(n.record), n.position, n.fieldName};
            } else if constexpr (std::is_same_v<T, Lambda>) {
                std::vector<std::string> params;
                for (const auto& p : n.params) params.push_back(renamed(p));
                out->node = Lambda{std::move(params), maybe(n.body), n.ownReturnScope};
            } else if constexpr (std::is_same_v<T, Return>) {
                out->node = Return{maybe(n.value)};
            } else if constexpr (std::is_same_v<T, TryThrow>) {
                out->node = TryThrow{maybe(n.error)};
            } else if constexpr (std::is_same_v<T, TryCatch>) {
                out->node = TryCatch{maybe(n.body), clauses(n.clauses)};
            } else if constexpr (std::is_same_v<T, LetRec>) {
                const bool fresh = localFunctions.insert(n.name).second;
                std::vector<std::string> params;
                for (const auto& p : n.params) params.push_back(renamed(p));
                out->node = LetRec{renamed(n.name), std::move(params), maybe(n.funBody),
                                   maybe(n.contBody)};
                if (fresh) localFunctions.erase(n.name);
            } else if constexpr (std::is_same_v<T, Receive>) {
                Receive copy;
                for (const auto& c : n.clauses)
                    copy.clauses.push_back({c.pattern ? pattern(*c.pattern) : nullptr, maybe(c.body)});
                if (n.senderVar) copy.senderVar = renamed(*n.senderVar);
                if (n.timeout) copy.timeout = maybe(*n.timeout);
                if (n.afterBody) copy.afterBody = maybe(*n.afterBody);
                out->node = std::move(copy);
            }
        }, e.node);
        return out;
    }
};

} // namespace

auto cloneExpr(const Expr& e, const Rename& rename) -> ExprPtr {
    Cloner cloner{rename, {}};
    return cloner.expr(e);
}

auto clonePattern(const Pattern& p, const Rename& rename) -> PatternPtr {
    Cloner cloner{rename, {}};
    return cloner.pattern(p);
}

auto bindPattern(const Pattern& p, std::set<std::string>& bound) -> void {
    if (p.kind == PatKind::Var) bound.insert(p.name);
    for (const auto& a : p.args)
        if (a) bindPattern(*a, bound);
    if (p.rest) bindPattern(*p.rest, bound);
}

auto collectFree(const Expr& e, std::set<std::string> bound, std::set<std::string>& out)
    -> void {
    const auto clauses = [&](const std::vector<MatchClause>& cs) {
        for (const auto& c : cs) {
            auto inner = bound;
            for (const auto& p : c.patterns)
                if (p) bindPattern(*p, inner);
            if (c.guard && *c.guard) collectFree(**c.guard, inner, out);
            if (c.body) collectFree(*c.body, inner, out);
        }
    };
    std::visit([&](const auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, Var>) {
            if (!bound.count(n.name)) out.insert(n.name);
        } else if constexpr (std::is_same_v<T, Let>) {
            if (n.value) collectFree(*n.value, bound, out);
            auto inner = bound;
            inner.insert(n.name);
            if (n.body) collectFree(*n.body, inner, out);
        } else if constexpr (std::is_same_v<T, Match>) {
            for (const auto& s : n.subjects)
                if (s) collectFree(*s, bound, out);
            clauses(n.clauses);
        } else if constexpr (std::is_same_v<T, Lambda>) {
            auto inner = bound;
            inner.insert(n.params.begin(), n.params.end());
            if (n.body) collectFree(*n.body, inner, out);
        } else if constexpr (std::is_same_v<T, TryCatch>) {
            if (n.body) collectFree(*n.body, bound, out);
            clauses(n.clauses);
        } else if constexpr (std::is_same_v<T, LetRec>) {
            auto inner = bound;
            inner.insert(n.params.begin(), n.params.end());
            if (n.funBody) collectFree(*n.funBody, inner, out);
            if (n.contBody) collectFree(*n.contBody, bound, out);
        } else if constexpr (std::is_same_v<T, Receive>) {
            for (const auto& c : n.clauses) {
                auto inner = bound;
                if (n.senderVar) inner.insert(*n.senderVar);
                if (c.pattern) bindPattern(*c.pattern, inner);
                if (c.body) collectFree(*c.body, inner, out);
            }
            if (n.timeout && *n.timeout) collectFree(**n.timeout, bound, out);
            if (n.afterBody && *n.afterBody) collectFree(**n.afterBody, bound, out);
        } else {
            forEachChild(e, [&](const ExprPtr& c) {
                if (c) collectFree(*c, bound, out);
            });
        }
    }, e.node);
}

auto returnsEarly(const Expr& e) -> bool {
    if (std::holds_alternative<Return>(e.node)) return true;
    if (std::holds_alternative<Lambda>(e.node)) return false;
    return anyChild(e, [](const ExprPtr& c) { return returnsEarly(*c); });
}

} // namespace kex::ir
