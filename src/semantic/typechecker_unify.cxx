#include "typechecker.hxx"

namespace kex::semantic {

auto TypeChecker::deferIfPending(const TypePtr& actual, const TypePtr& expected,
                                 SourceLocation loc) -> bool {
    const auto* pending = std::get_if<TypeVar>(&resolve(actual)->kind);
    if (!pending || !m_pendingResultVars.count(pending->id)) return false;
    m_pendingChecks[pending->id].push_back(
        [this, expected, loc](const TypePtr& answered) {
            const auto resolved = resolve(answered);
            if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                !std::holds_alternative<TypeVar>(resolved->kind) &&
                !argMatchesParam(resolved, resolve(expected)))
                typeMismatch(loc, resolve(expected), resolved);
        });
    return true;
}

auto TypeChecker::answerPending(int id, const TypePtr& answer) -> void {
    m_pendingResultVars.erase(id);
    unifyVar(id, answer);
    if (auto checks = m_pendingChecks.find(id); checks != m_pendingChecks.end()) {
        auto deferred = std::move(checks->second);
        m_pendingChecks.erase(checks);
        for (const auto& check : deferred) check(answer);
    }
}

auto TypeChecker::freshTypeVar() -> TypePtr {
    return Type::typeVar(m_nextTypeVar++);
}

auto TypeChecker::resolve(TypePtr t) const -> TypePtr {
    // `seen` is the termination guard, not an optimization: a substitution
    // that leads back to a variable already on this chain would spin here
    // forever, hanging every mode of the compiler rather than reporting
    // anything. `unifyVar` refuses to build such a chain, and this is the
    // backstop that keeps a future one from costing the user their session.
    std::unordered_set<int> seen;
    while (t) {
        auto* tv = std::get_if<TypeVar>(&t->kind);
        if (!tv) break;
        if (!seen.insert(tv->id).second) break;
        auto it = m_subst.find(tv->id);
        if (it == m_subst.end()) break;
        t = it->second;
    }
    return t;
}

namespace {

// Visit each directly-nested type of `type`. Structural recursion over Type is
// written out in several places here already; these two keep the two new walks
// below from adding a third and a fourth copy.
auto forEachTypeChild(const TypePtr& type,
                      const std::function<void(const TypePtr&)>& visit) -> void {
    if (!type) return;
    std::visit([&](const auto& part) {
        using Part = std::decay_t<decltype(part)>;
        if constexpr (std::is_same_v<Part, ListType>) {
            visit(part.element);
        } else if constexpr (std::is_same_v<Part, MapType>) {
            visit(part.key);
            visit(part.value);
        } else if constexpr (std::is_same_v<Part, OptionalType>) {
            visit(part.inner);
        } else if constexpr (std::is_same_v<Part, TupleType>) {
            for (const auto& element : part.elements) visit(element);
        } else if constexpr (std::is_same_v<Part, FuncType>) {
            for (const auto& param : part.params) visit(param);
            visit(part.result);
        } else if constexpr (std::is_same_v<Part, NamedType>) {
            for (const auto& arg : part.typeArgs) visit(arg);
        } else if constexpr (std::is_same_v<Part, UnionType> ||
                             std::is_same_v<Part, IntersectionType>) {
            for (const auto& member : part.members) visit(member);
        } else if constexpr (std::is_same_v<Part, RecordType>) {
            for (const auto& field : part.fields) visit(field.second);
        }
    }, type->kind);
}

// Rebuild `type` with each directly-nested type replaced by `map(child)`.
// Returns `type` unchanged for the leaf kinds, which have no children.
auto mapTypeChildren(const TypePtr& type,
                     const std::function<TypePtr(const TypePtr&)>& map)
    -> TypePtr {
    if (!type) return type;
    return std::visit([&](const auto& part) -> TypePtr {
        using Part = std::decay_t<decltype(part)>;
        if constexpr (std::is_same_v<Part, ListType>) {
            return Type::list(map(part.element));
        } else if constexpr (std::is_same_v<Part, MapType>) {
            return Type::map(map(part.key), map(part.value));
        } else if constexpr (std::is_same_v<Part, OptionalType>) {
            return Type::optional(map(part.inner));
        } else if constexpr (std::is_same_v<Part, TupleType>) {
            std::vector<TypePtr> elements;
            for (const auto& element : part.elements)
                elements.push_back(map(element));
            return Type::tuple(std::move(elements));
        } else if constexpr (std::is_same_v<Part, FuncType>) {
            std::vector<TypePtr> params;
            for (const auto& param : part.params) params.push_back(map(param));
            return Type::func(std::move(params), map(part.result),
                              part.block);
        } else if constexpr (std::is_same_v<Part, NamedType>) {
            if (part.typeArgs.empty()) return type;
            std::vector<TypePtr> args;
            for (const auto& arg : part.typeArgs) args.push_back(map(arg));
            return Type::named(part.name, std::move(args));
        } else if constexpr (std::is_same_v<Part, IntersectionType>) {
            std::vector<TypePtr> members;
            for (const auto& member : part.members)
                members.push_back(map(member));
            return Type::intersection(std::move(members));
        } else if constexpr (std::is_same_v<Part, UnionType>) {
            std::vector<TypePtr> members;
            for (const auto& member : part.members)
                members.push_back(map(member));
            return std::make_shared<Type>(Type{UnionType{std::move(members)}});
        } else if constexpr (std::is_same_v<Part, RecordType>) {
            std::vector<std::pair<std::string, TypePtr>> fields;
            for (const auto& [name, fieldType] : part.fields)
                fields.emplace_back(name, map(fieldType));
            return Type::record(std::move(fields));
        } else {
            return type;
        }
    }, type->kind);
}

} // namespace

// Let-polymorphism, done in the one currency this checker already speaks.
//
// A signature stores the types a definition was checked with. An UNANNOTATED
// parameter is an inference variable with a NON-NEGATIVE id, and those live in
// one global substitution shared by the whole program — so `let id(x) = x`
// handed every call site the same variable. Whichever call ran first won it,
// and no call constrained its own result: `id(5)` was not an Integer, `id("a")`
// was not a String, and `id(5) + id("a")` type-checked and died at runtime.
//
// An ANNOTATED generic (`let first(xs: [A]) -> A?`) is already handled — those
// are per-signature placeholders with NEGATIVE ids, and checkCall instantiates
// them per call through bindGenerics/applyGenerics. So generalizing is simply
// moving a definition's leftover inference variables into that representation:
// no new machinery at the call site, and annotated and inferred generics behave
// identically from here on.
//
// `varMark` is `m_nextTypeVar` as it stood when this definition started, and
// it is what keeps this from over-generalizing. A variable older than the mark
// belongs to something still being inferred around this definition — an
// enclosing function's parameter a closure captured, say — and quantifying over
// it would cut a connection the enclosing inference still needs. Only what this
// definition itself created and left free is its to generalize.
auto TypeChecker::generalizeSignature(Signature& signature, int varMark)
    -> void {
    // Continue below whatever placeholders the signature already uses, so an
    // inferred generic never collides with an annotated one (`A` is -1).
    int nextGeneric = 0;
    std::function<void(const TypePtr&)> lowestGeneric =
        [&](const TypePtr& type) {
            if (!type) return;
            if (auto* var = std::get_if<TypeVar>(&type->kind))
                nextGeneric = std::min(nextGeneric, var->id);
            else if (auto* constrained = std::get_if<ConstrainedType>(&type->kind))
                nextGeneric = std::min(nextGeneric, constrained->genericId);
            forEachTypeChild(type, lowestGeneric);
        };
    for (const auto& param : signature.params) lowestGeneric(param);
    lowestGeneric(signature.result);

    std::unordered_map<int, TypePtr> quantified;
    std::function<TypePtr(const TypePtr&)> quantify =
        [&](const TypePtr& type) -> TypePtr {
            if (!type) return type;
            auto resolved = resolve(type);
            if (auto* var = std::get_if<TypeVar>(&resolved->kind)) {
                if (var->id < varMark) return resolved;
                auto [entry, inserted] = quantified.emplace(var->id, nullptr);
                if (inserted) entry->second = Type::typeVar(--nextGeneric);
                return entry->second;
            }
            return mapTypeChildren(resolved, quantify);
        };
    for (auto& param : signature.params) param = quantify(param);
    signature.result = quantify(signature.result);
}

auto TypeChecker::unifyVar(int id, TypePtr concrete) -> void {
    if (m_subst.count(id) || m_pendingResultVars.count(id)) return;
    // Never bind a variable to a chain that leads back to itself. `a + b`
    // with both operands unconstrained records "these two share a type" by
    // binding one to the other (inferBinaryOp's Plus case); when `a` and `b`
    // are the SAME variable — `let id(x) = x` gives every call one — that
    // wrote `T ↦ T` and `resolve` never terminated.
    std::unordered_set<int> seen;
    for (TypePtr t = concrete; t;) {
        auto* tv = std::get_if<TypeVar>(&t->kind);
        if (!tv) break;
        if (tv->id == id) return;
        if (!seen.insert(tv->id).second) break;
        auto it = m_subst.find(tv->id);
        if (it == m_subst.end()) break;
        t = it->second;
    }
    m_subst[id] = std::move(concrete);
}

} // namespace kex::semantic
