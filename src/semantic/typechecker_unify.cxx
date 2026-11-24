#include "typechecker.hxx"

namespace kex::semantic {

auto TypeChecker::deferIfPending(const TypePtr& actual, const TypePtr& expected, SourceLocation loc)
    -> bool {
    const auto* pending = std::get_if<TypeVar>(&resolve(actual)->kind);
    if (!pending || !m_pendingResultVars.count(pending->id)) {
        return false;
    }
    m_pendingChecks[pending->id].push_back([this, expected, loc](const TypePtr& answered) {
        const auto resolved = resolve(answered);
        if (!std::holds_alternative<UnknownType>(resolved->kind) &&
            !std::holds_alternative<TypeVar>(resolved->kind) &&
            !argMatchesParam(resolved, resolve(expected))) {
            typeMismatch(loc, resolve(expected), resolved);
        }
    });
    return true;
}

auto TypeChecker::answerPending(int id, const TypePtr& answer) -> void {
    m_pendingResultVars.erase(id);
    unifyVar(id, answer);
    if (auto checks = m_pendingChecks.find(id); checks != m_pendingChecks.end()) {
        auto deferred = std::move(checks->second);
        m_pendingChecks.erase(checks);
        for (const auto& check : deferred) {
            check(answer);
        }
    }
}

auto TypeChecker::freshTypeVar() -> TypePtr {
    return Type::inferenceVariable(m_nextTypeVar++);
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
        if (!tv) {
            break;
        }
        if (!seen.insert(tv->id).second) {
            break;
        }
        auto it = m_subst.find(tv->id);
        if (it == m_subst.end()) {
            break;
        }
        t = it->second;
    }
    return t;
}

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
auto TypeChecker::generalizeSignature(
    Signature& signature, int varMark, const TypePtr& body, bool declared) -> void {
    // Decision 4 first, so the generic it makes counts among those used below.
    // A checked body must answer the parameter's own type, or be one that
    // cannot be checked (an intrinsic answers Unknown): `scale(x: Number) ->
    // Number = x * 2.5` answers a Float, and tying it would type `scale(3)` as
    // an Integer that is 7.5. A declared contract is tied, and its body is
    // checked against the tie (checkFunctionDef).
    if (body || declared) {
        const auto answered = body ? resolve(body) : nullptr;
        tieTraitResult(
            signature, [this](const TypePtr& type) { return resolve(type); },
            [&](const Type* occurrence) {
                return declared || isUndetermined(answered) || answered.get() == occurrence;
            });
    }

    // Continue below whatever placeholders the signature already uses, so an
    // inferred generic never collides with an annotated one (`A` is -1).
    int nextGeneric = 0;
    std::function<void(const TypePtr&)> lowestGeneric = [&](const TypePtr& type) {
        if (!type) {
            return;
        }
        if (auto* var = std::get_if<TypeVar>(&type->kind)) {
            nextGeneric = std::min(nextGeneric, var->id);
        } else if (auto* constrained = std::get_if<ConstrainedType>(&type->kind)) {
            nextGeneric = std::min(nextGeneric, constrained->genericId);
        }
        forEachTypeChild(type, lowestGeneric);
    };
    for (const auto& param : signature.params) {
        lowestGeneric(param);
    }
    lowestGeneric(signature.result);

    // One bound placeholder shared by several positions relates them: `p * q`
    // makes both operands and the result one `N: Number`, so the signature
    // is `(N, N) -> N` for a single N, and `sub(12.5, 4.0)` is a Float, not a
    // bare `N`. Such a placeholder becomes a generic, as a free variable does.
    // A trait type written in two places (`x: Showable` and `-> Showable`) is
    // two separate types, related by nothing, and stays as written.
    std::unordered_map<const Type*, int> occurrences;
    std::function<void(const TypePtr&)> countPlaceholders = [&](const TypePtr& type) {
        if (!type) {
            return;
        }
        const auto resolved = resolve(type);
        if (auto* constrained = std::get_if<ConstrainedType>(&resolved->kind);
            constrained && constrained->genericId == 0) {
            ++occurrences[resolved.get()];
        }
        forEachTypeChild(resolved, countPlaceholders);
    };
    for (const auto& param : signature.params) {
        countPlaceholders(param);
    }
    countPlaceholders(signature.result);
    std::unordered_set<const Type*> placeholders;
    for (const auto& [type, count] : occurrences) {
        if (count > 1) {
            placeholders.insert(type);
        }
    }

    std::unordered_map<int, TypePtr> quantified;
    std::unordered_map<const Type*, TypePtr> quantifiedPlaceholders;
    std::function<TypePtr(const TypePtr&)> quantify = [&](const TypePtr& type) -> TypePtr {
        if (!type) {
            return type;
        }
        auto resolved = resolve(type);
        if (auto* var = std::get_if<TypeVar>(&resolved->kind)) {
            if (var->id < varMark) {
                return resolved;
            }
            auto [entry, inserted] = quantified.emplace(var->id, nullptr);
            if (inserted) {
                entry->second = Type::genericLetter(--nextGeneric);
            }
            return entry->second;
        }
        if (placeholders.contains(resolved.get())) {
            const auto& constrained = std::get<ConstrainedType>(resolved->kind);
            auto [entry, inserted] = quantifiedPlaceholders.emplace(resolved.get(), nullptr);
            if (inserted) {
                entry->second =
                    Type::constrained(constrained.varName, constrained.traitName, --nextGeneric);
            }
            return entry->second;
        }
        return mapTypeChildren(resolved, quantify);
    };
    for (auto& param : signature.params) {
        param = quantify(param);
    }
    signature.result = quantify(signature.result);
}

// An argument's unknowns inside a list, an optional, a tuple, a map, a
// function or a generic type, fixed by the concrete parameter they are passed
// as: `none()` passed where a `[Integer]` goes is a list of Integers from then
// on, as a bare unknown argument already was (checkCall). Only where the
// parameter is concrete there: a generic one says nothing about the argument.
// The two types are walked side by side while they have the same shape; a
// union or intersection lists its members in no order to pair them by.
auto TypeChecker::fixNestedVariables(const TypePtr& argument, const TypePtr& parameter) -> void {
    const auto actual = resolve(argument);
    const auto wanted = resolve(parameter);
    if (const auto* var = std::get_if<TypeVar>(&actual->kind)) {
        if (isInferenceVariable(*var) && !containsOpenType(wanted)) {
            unifyVar(var->id, wanted);
        }
        return;
    }
    if (actual->kind.index() != wanted->kind.index() ||
        std::holds_alternative<UnionType>(actual->kind) ||
        std::holds_alternative<IntersectionType>(actual->kind)) {
        return;
    }
    if (const auto* named = std::get_if<NamedType>(&actual->kind);
        named && !sameNamedType(std::get<NamedType>(wanted->kind), *named)) {
        return;
    }
    std::vector<TypePtr> given;
    std::vector<TypePtr> expected;
    forEachTypeChild(actual, [&](const TypePtr& child) { given.push_back(child); });
    forEachTypeChild(wanted, [&](const TypePtr& child) { expected.push_back(child); });
    if (given.size() != expected.size()) {
        return;
    }
    for (std::size_t i = 0; i < given.size(); ++i) {
        fixNestedVariables(given[i], expected[i]);
    }
}

// Unification (docs/typechecker-hm-plan.md §4.1, §12 step 1): makes `a` and `b`
// the same type by binding inference variables on either side, or fails and
// binds nothing. A generic letter (a signature's own `A`, instantiated per
// call elsewhere), an unknown and a trait bound match without binding: what
// they admit is §4.5's rules, checked by argMatchesParam.
auto TypeChecker::unify(const TypePtr& a, const TypePtr& b, bool bindRight) -> bool {
    const bool savedBindRight = std::exchange(m_unifyBindsRight, bindRight);
    std::vector<int> journal;
    auto* const outer = std::exchange(m_substJournal, &journal);
    const bool unified = unifyStep(a, b);
    m_substJournal = outer;
    m_unifyBindsRight = savedBindRight;
    if (!unified) {
        for (const int id : journal) {
            m_subst.erase(id);
        }
        return false;
    }
    if (outer) {
        outer->insert(outer->end(), journal.begin(), journal.end());
    }
    return true;
}

auto TypeChecker::unifyStep(const TypePtr& a, const TypePtr& b) -> bool {
    const auto left = resolve(a);
    const auto right = resolve(b);
    if (left == right) {
        return true;
    }
    // Whether inference variable `id` occurs in `type`: binding it there would
    // make an infinite type.
    const auto occurs = [&](int id, const TypePtr& type) {
        return anyType(type, [&](const TypePtr& node) {
            const auto* var = std::get_if<TypeVar>(&resolve(node)->kind);
            return var && var->id == id;
        });
    };
    for (const auto& [variable, other] : {std::pair{left, right}, std::pair{right, left}}) {
        // A variable of the other side (a callee's own, in its parameter) is
        // not this call's to bind unless asked: it matches as an open type.
        if (variable == right && !m_unifyBindsRight) {
            continue;
        }
        if (const auto* var = std::get_if<TypeVar>(&variable->kind);
            var && isInferenceVariable(*var)) {
            if (const auto* same = std::get_if<TypeVar>(&other->kind);
                same && same->id == var->id) {
                return true;
            }
            if (occurs(var->id, other)) {
                return false;
            }
            // Not to a type still holding a generic letter or an unknown: a
            // signature's `A` means nothing outside its own call, and an
            // unknown says nothing yet.
            if (containsOpenType(other)) {
                return true;
            }
            unifyVar(var->id, other);
            return true;
        }
    }
    const auto open = [&](const TypePtr& type) {
        const auto* var = std::get_if<TypeVar>(&type->kind);
        return (var && (isGenericLetter(*var) || !m_unifyBindsRight)) ||
               std::holds_alternative<UnknownType>(type->kind) ||
               std::holds_alternative<ConstrainedType>(type->kind);
    };
    if (open(left) || open(right)) {
        return true;
    }
    if (left->kind.index() != right->kind.index()) {
        return false;
    }
    if (const auto* named = std::get_if<NamedType>(&left->kind);
        named && !sameNamedType(std::get<NamedType>(right->kind), *named)) {
        return false;
    }
    std::vector<TypePtr> leftChildren;
    std::vector<TypePtr> rightChildren;
    forEachTypeChild(left, [&](const TypePtr& child) { leftChildren.push_back(child); });
    forEachTypeChild(right, [&](const TypePtr& child) { rightChildren.push_back(child); });
    if (leftChildren.empty() && rightChildren.empty()) {
        return typesEqual(left, right);
    }
    if (leftChildren.size() != rightChildren.size()) {
        return false;
    }
    for (std::size_t i = 0; i < leftChildren.size(); ++i) {
        if (!unifyStep(leftChildren[i], rightChildren[i])) {
            return false;
        }
    }
    return true;
}

// `type` with every unknown that is known by now replaced, all the way down:
// what a message shows (`[Integer]`, where the type as recorded is `[T3]`).
auto TypeChecker::resolvedThroughout(const TypePtr& type) const -> TypePtr {
    if (!type) {
        return type;
    }
    std::function<TypePtr(const TypePtr&)> through = [&](const TypePtr& part) -> TypePtr {
        return part ? mapTypeChildren(resolve(part), through) : part;
    };
    return through(type);
}

auto TypeChecker::unifyVar(int id, TypePtr concrete) -> void {
    if (m_subst.count(id) || m_pendingResultVars.count(id)) {
        return;
    }
    // Never bind a variable to a chain that leads back to itself. `a + b`
    // with both operands unconstrained records "these two share a type" by
    // binding one to the other (inferBinaryOp's Plus case); when `a` and `b`
    // are the SAME variable — `let id(x) = x` gives every call one — that
    // wrote `T ↦ T` and `resolve` never terminated.
    std::unordered_set<int> seen;
    for (TypePtr t = concrete; t;) {
        auto* tv = std::get_if<TypeVar>(&t->kind);
        if (!tv) {
            break;
        }
        if (tv->id == id) {
            return;
        }
        if (!seen.insert(tv->id).second) {
            break;
        }
        auto it = m_subst.find(tv->id);
        if (it == m_subst.end()) {
            break;
        }
        t = it->second;
    }
    if (m_substJournal && !m_subst.contains(id)) {
        m_substJournal->push_back(id);
    }
    m_subst[id] = std::move(concrete);
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::generalizeTargetVariables(Signature& signature, const TypePtr& receiver) -> void {
    // Below whatever placeholders the signature already uses (`A` is -1).
    int next = 0;
    std::function<void(const TypePtr&)> lowest = [&](const TypePtr& type) {
        if (!type) {
            return;
        }
        if (const auto* var = std::get_if<TypeVar>(&type->kind)) {
            next = std::min(next, var->id);
        } else if (const auto* bound = std::get_if<ConstrainedType>(&type->kind)) {
            next = std::min(next, bound->genericId);
        }
        forEachTypeChild(type, lowest);
    };
    for (const auto& param : signature.params) {
        lowest(param);
    }
    lowest(signature.result);
    std::unordered_map<int, TypePtr> generic;
    std::function<void(const TypePtr&)> collect = [&](const TypePtr& type) {
        if (!type) {
            return;
        }
        const auto resolved = resolve(type);
        if (const auto* var = std::get_if<TypeVar>(&resolved->kind);
            var && isInferenceVariable(*var)) {
            generic.try_emplace(var->id, Type::genericLetter(--next));
        }
        forEachTypeChild(resolved, collect);
    };
    collect(receiver);
    if (generic.empty()) {
        return;
    }
    std::function<TypePtr(const TypePtr&)> substitute = [&](const TypePtr& type) -> TypePtr {
        if (!type) {
            return type;
        }
        const auto resolved = resolve(type);
        if (const auto* var = std::get_if<TypeVar>(&resolved->kind)) {
            if (const auto found = generic.find(var->id); found != generic.end()) {
                return found->second;
            }
        }
        return mapTypeChildren(resolved, substitute);
    };
    for (auto& param : signature.params) {
        param = substitute(param);
    }
    signature.result = substitute(signature.result);
}

} // namespace kex::semantic
