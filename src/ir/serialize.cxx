#include "serialize.hxx"

#include "../beam/etf.hxx"

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace kex::ir {

namespace {

using beam::Term;
using beam::TermPtr;

// Bumped whenever a node's shape below changes.
constexpr int64_t kFormat = 2;

auto str(const std::string& s) -> TermPtr {
    return Term::binary(s);
}
auto boolean(bool b) -> TermPtr {
    return Term::boolean(b);
}
auto integer(int64_t i) -> TermPtr {
    return Term::integer(i);
}
auto absent() -> TermPtr {
    return Term::atom("nil");
}

auto encodeExpr(const ExprPtr& e) -> TermPtr;
auto encodePattern(const PatternPtr& p) -> TermPtr;

auto exprs(const std::vector<ExprPtr>& es) -> TermPtr {
    std::vector<TermPtr> out;
    for (const auto& e : es) {
        out.push_back(encodeExpr(e));
    }
    return Term::list(std::move(out));
}
auto patterns(const std::vector<PatternPtr>& ps) -> TermPtr {
    std::vector<TermPtr> out;
    for (const auto& p : ps) {
        out.push_back(encodePattern(p));
    }
    return Term::list(std::move(out));
}
auto strings(const std::vector<std::string>& ss) -> TermPtr {
    std::vector<TermPtr> out;
    for (const auto& s : ss) {
        out.push_back(str(s));
    }
    return Term::list(std::move(out));
}
auto optionalExpr(const std::optional<ExprPtr>& e) -> TermPtr {
    return e && *e ? encodeExpr(*e) : absent();
}
auto clauses(const std::vector<MatchClause>& cs) -> TermPtr {
    std::vector<TermPtr> out;
    for (const auto& c : cs) {
        out.push_back(
            Term::tuple({patterns(c.patterns), optionalExpr(c.guard), encodeExpr(c.body)}));
    }
    return Term::list(std::move(out));
}

auto encodePattern(const PatternPtr& p) -> TermPtr {
    if (!p) {
        return absent();
    }
    return Term::tuple(
        {integer(static_cast<int64_t>(p->kind)), integer(static_cast<int64_t>(p->litKind)),
         str(p->litText), boolean(p->litBool), str(p->name), str(p->tag), patterns(p->args),
         encodePattern(p->rest), boolean(p->ofString), str(p->alias)});
}

auto encodeExpr(const ExprPtr& e) -> TermPtr {
    if (!e) {
        return absent();
    }
    return std::visit(
        [](const auto& n) -> TermPtr {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, Lit>) {
                return Term::tuple(
                    {Term::atom("lit"), integer(static_cast<int64_t>(n.kind)), str(n.text),
                     boolean(n.boolValue)});
            } else if constexpr (std::is_same_v<T, Var>) {
                return Term::tuple({Term::atom("var"), str(n.name)});
            } else if constexpr (std::is_same_v<T, Intrinsic>) {
                return Term::tuple(
                    {Term::atom("op"), integer(static_cast<int64_t>(n.op)), exprs(n.args)});
            } else if constexpr (std::is_same_v<T, Call>) {
                return Term::tuple(
                    {Term::atom("call"), str(n.module), str(n.name), integer(n.arity),
                     exprs(n.args), boolean(n.tail)});
            } else if constexpr (std::is_same_v<T, CallIndirect>) {
                return Term::tuple(
                    {Term::atom("apply"), encodeExpr(n.callee), exprs(n.args), boolean(n.tail)});
            } else if constexpr (std::is_same_v<T, Let>) {
                return Term::tuple(
                    {Term::atom("let"), str(n.name), encodeExpr(n.value), encodeExpr(n.body)});
            } else if constexpr (std::is_same_v<T, Seq>) {
                return Term::tuple({Term::atom("seq"), exprs(n.exprs)});
            } else if constexpr (std::is_same_v<T, Match>) {
                return Term::tuple(
                    {Term::atom("match"), exprs(n.subjects), clauses(n.clauses),
                     integer(static_cast<int64_t>(n.guards)), str(n.unmatchedAt)});
            } else if constexpr (std::is_same_v<T, Construct>) {
                return Term::tuple({Term::atom("construct"), str(n.tag), exprs(n.args)});
            } else if constexpr (std::is_same_v<T, MakeTuple>) {
                return Term::tuple({Term::atom("tuple"), exprs(n.elements)});
            } else if constexpr (std::is_same_v<T, MakeList>) {
                return Term::tuple({Term::atom("list"), exprs(n.elements), optionalExpr(n.rest)});
            } else if constexpr (std::is_same_v<T, FieldGet>) {
                return Term::tuple(
                    {Term::atom("field"), encodeExpr(n.record), integer(n.position),
                     str(n.fieldName)});
            } else if constexpr (std::is_same_v<T, Lambda>) {
                return Term::tuple(
                    {Term::atom("lambda"), strings(n.params), encodeExpr(n.body),
                     boolean(n.ownReturnScope), boolean(n.pure)});
            } else if constexpr (std::is_same_v<T, Return>) {
                return Term::tuple({Term::atom("return"), encodeExpr(n.value)});
            } else if constexpr (std::is_same_v<T, TryThrow>) {
                return Term::tuple({Term::atom("throw"), encodeExpr(n.error)});
            } else if constexpr (std::is_same_v<T, TryCatch>) {
                return Term::tuple({Term::atom("try"), encodeExpr(n.body), clauses(n.clauses)});
            } else if constexpr (std::is_same_v<T, LetRec>) {
                return Term::tuple(
                    {Term::atom("letrec"), str(n.name), strings(n.params), encodeExpr(n.funBody),
                     encodeExpr(n.contBody)});
            } else {
                static_assert(std::is_same_v<T, Receive>);
                std::vector<TermPtr> arms;
                for (const auto& c : n.clauses) {
                    arms.push_back(Term::tuple({encodePattern(c.pattern), encodeExpr(c.body)}));
                }
                return Term::tuple(
                    {Term::atom("receive"), Term::list(std::move(arms)),
                     n.senderVar ? str(*n.senderVar) : absent(), optionalExpr(n.timeout),
                     optionalExpr(n.afterBody)});
            }
        },
        e->node);
}

auto encodeFunction(const FunDef& f) -> TermPtr {
    std::vector<TermPtr> cs;
    for (const auto& c : f.clauses) {
        cs.push_back(
            Term::tuple(
                {patterns(c.params), optionalExpr(c.guard), encodeExpr(c.body),
                 strings(c.routes)}));
    }
    return Term::tuple(
        {str(f.name), integer(f.arity), Term::list(std::move(cs)), boolean(f.exported),
         strings(f.dictionaryTraits), boolean(f.hasCapabilityContext), boolean(f.isFreeFunction),
         str(f.declaringModule), boolean(f.isCollisionDispatcher), boolean(f.isFieldAccessor)});
}

// ---- decoding ----

auto isAbsent(const TermPtr& t) -> bool {
    return t->isAtom("nil");
}
auto text(const TermPtr& t) -> std::string {
    return t->asBinaryStr();
}
auto flag(const TermPtr& t) -> bool {
    if (t->isAtom("true")) {
        return true;
    }
    if (t->isAtom("false")) {
        return false;
    }
    throw std::runtime_error("IR: expected a boolean");
}
auto at(const std::vector<TermPtr>& elements, std::size_t i) -> const TermPtr& {
    if (i >= elements.size()) {
        throw std::runtime_error("IR: node too short");
    }
    return elements[i];
}

auto decodeExpr(const TermPtr& t) -> ExprPtr;
auto decodePattern(const TermPtr& t) -> PatternPtr;

auto decodeExprs(const TermPtr& t) -> std::vector<ExprPtr> {
    std::vector<ExprPtr> out;
    for (const auto& e : t->asList()) {
        out.push_back(decodeExpr(e));
    }
    return out;
}
auto decodePatterns(const TermPtr& t) -> std::vector<PatternPtr> {
    std::vector<PatternPtr> out;
    for (const auto& p : t->asList()) {
        out.push_back(decodePattern(p));
    }
    return out;
}
auto decodeStrings(const TermPtr& t) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& s : t->asList()) {
        out.push_back(text(s));
    }
    return out;
}
auto decodeOptional(const TermPtr& t) -> std::optional<ExprPtr> {
    if (isAbsent(t)) {
        return std::nullopt;
    }
    return decodeExpr(t);
}
auto decodeClauses(const TermPtr& t) -> std::vector<MatchClause> {
    std::vector<MatchClause> out;
    for (const auto& c : t->asList()) {
        const auto& f = c->asTuple();
        MatchClause clause;
        clause.patterns = decodePatterns(at(f, 0));
        clause.guard = decodeOptional(at(f, 1));
        clause.body = decodeExpr(at(f, 2));
        out.push_back(std::move(clause));
    }
    return out;
}

auto decodePattern(const TermPtr& t) -> PatternPtr {
    if (isAbsent(t)) {
        return nullptr;
    }
    const auto& f = t->asTuple();
    auto p = std::make_unique<Pattern>();
    p->kind = static_cast<PatKind>(at(f, 0)->asInt());
    p->litKind = static_cast<LitKind>(at(f, 1)->asInt());
    p->litText = text(at(f, 2));
    p->litBool = flag(at(f, 3));
    p->name = text(at(f, 4));
    p->tag = text(at(f, 5));
    p->args = decodePatterns(at(f, 6));
    p->rest = decodePattern(at(f, 7));
    p->ofString = flag(at(f, 8));
    if (f.size() > 9) {
        p->alias = text(at(f, 9));
    }
    return p;
}

auto decodeExpr(const TermPtr& t) -> ExprPtr {
    if (isAbsent(t)) {
        return nullptr;
    }
    const auto& f = t->asTuple();
    const auto& kind = at(f, 0)->asAtom();
    auto e = std::make_unique<Expr>();
    if (kind == "lit") {
        e->node = Lit{static_cast<LitKind>(at(f, 1)->asInt()), text(at(f, 2)), flag(at(f, 3))};
    } else if (kind == "var") {
        e->node = Var{text(at(f, 1))};
    } else if (kind == "op") {
        e->node = Intrinsic{static_cast<Op>(at(f, 1)->asInt()), decodeExprs(at(f, 2))};
    } else if (kind == "call") {
        e->node = Call{
            text(at(f, 1)), text(at(f, 2)), static_cast<int>(at(f, 3)->asInt()),
            decodeExprs(at(f, 4)), flag(at(f, 5))};
    } else if (kind == "apply") {
        e->node = CallIndirect{decodeExpr(at(f, 1)), decodeExprs(at(f, 2)), flag(at(f, 3))};
    } else if (kind == "let") {
        e->node = Let{text(at(f, 1)), decodeExpr(at(f, 2)), decodeExpr(at(f, 3))};
    } else if (kind == "seq") {
        e->node = Seq{decodeExprs(at(f, 1))};
    } else if (kind == "match") {
        Match m;
        m.subjects = decodeExprs(at(f, 1));
        m.clauses = decodeClauses(at(f, 2));
        m.guards = static_cast<GuardExpansion>(at(f, 3)->asInt());
        m.unmatchedAt = text(at(f, 4));
        e->node = std::move(m);
    } else if (kind == "construct") {
        e->node = Construct{text(at(f, 1)), decodeExprs(at(f, 2))};
    } else if (kind == "tuple") {
        e->node = MakeTuple{decodeExprs(at(f, 1))};
    } else if (kind == "list") {
        e->node = MakeList{decodeExprs(at(f, 1)), decodeOptional(at(f, 2))};
    } else if (kind == "field") {
        e->node =
            FieldGet{decodeExpr(at(f, 1)), static_cast<int>(at(f, 2)->asInt()), text(at(f, 3))};
    } else if (kind == "lambda") {
        e->node =
            Lambda{decodeStrings(at(f, 1)), decodeExpr(at(f, 2)), flag(at(f, 3)), flag(at(f, 4))};
    } else if (kind == "return") {
        e->node = Return{decodeExpr(at(f, 1))};
    } else if (kind == "throw") {
        e->node = TryThrow{decodeExpr(at(f, 1))};
    } else if (kind == "try") {
        e->node = TryCatch{decodeExpr(at(f, 1)), decodeClauses(at(f, 2))};
    } else if (kind == "letrec") {
        e->node = LetRec{
            text(at(f, 1)), decodeStrings(at(f, 2)), decodeExpr(at(f, 3)), decodeExpr(at(f, 4))};
    } else if (kind == "receive") {
        Receive r;
        for (const auto& arm : at(f, 1)->asList()) {
            const auto& a = arm->asTuple();
            r.clauses.push_back(ReceiveClause{decodePattern(at(a, 0)), decodeExpr(at(a, 1))});
        }
        if (!isAbsent(at(f, 2))) {
            r.senderVar = text(at(f, 2));
        }
        r.timeout = decodeOptional(at(f, 3));
        r.afterBody = decodeOptional(at(f, 4));
        e->node = std::move(r);
    } else {
        throw std::runtime_error("IR: unknown node '" + kind + "'");
    }
    return e;
}

auto decodeFunction(const TermPtr& t) -> FunDef {
    const auto& f = t->asTuple();
    FunDef fn;
    fn.name = text(at(f, 0));
    fn.arity = static_cast<int>(at(f, 1)->asInt());
    for (const auto& c : at(f, 2)->asList()) {
        const auto& cf = c->asTuple();
        FunClause clause;
        clause.params = decodePatterns(at(cf, 0));
        clause.guard = decodeOptional(at(cf, 1));
        clause.body = decodeExpr(at(cf, 2));
        clause.routes = decodeStrings(at(cf, 3));
        fn.clauses.push_back(std::move(clause));
    }
    fn.exported = flag(at(f, 3));
    fn.dictionaryTraits = decodeStrings(at(f, 4));
    fn.hasCapabilityContext = flag(at(f, 5));
    fn.isFreeFunction = flag(at(f, 6));
    fn.declaringModule = text(at(f, 7));
    fn.isCollisionDispatcher = flag(at(f, 8));
    fn.isFieldAccessor = flag(at(f, 9));
    return fn;
}

} // namespace

auto encodeFunctions(const std::vector<FunDef>& functions) -> std::vector<uint8_t> {
    std::vector<TermPtr> out;
    for (const auto& f : functions) {
        out.push_back(encodeFunction(f));
    }
    return beam::encodeEtf(
        Term::tuple({Term::atom("kex_ir"), integer(kFormat), Term::list(std::move(out))}));
}

auto decodeFunctions(const std::vector<uint8_t>& bytes) -> std::vector<FunDef> {
    const auto term = beam::decodeEtf(bytes);
    const auto& top = term->asTuple();
    if (!at(top, 0)->isAtom("kex_ir") || at(top, 1)->asInt() != kFormat) {
        throw std::runtime_error("IR: another format");
    }
    std::vector<FunDef> functions;
    for (const auto& f : at(top, 2)->asList()) {
        functions.push_back(decodeFunction(f));
    }
    return functions;
}

} // namespace kex::ir
