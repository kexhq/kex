#include "../ast/function_groups.hxx"
#include "../common/name_case.hxx"
#include "../ast/type_text.hxx"
#include "../common/type_def_utils.hxx"
#include "../common/mangle.hxx"
#include "resolve_core.hxx"
#include "typechecker.hxx"
#include "typechecker_internal.hxx"
#include "../ast/walk.hxx"
#include "analyzer.hxx"
#include "../common/named_arguments.hxx"
#include "../common/source_name.hxx"
#include "../common/well_known.hxx"
#include <cctype>
#include <functional>
#include <set>
#include <unordered_set>
#include <utility>

namespace kex::semantic {

using namespace kex::semantic::detail;

namespace {

// The type of field `name` of a structural record type, through an
// intersection (`{ name: String } & R`), or null.
auto structuralField(const TypePtr& type, const std::string& name) -> TypePtr {
    if (const auto* record = std::get_if<RecordType>(&type->kind)) {
        for (const auto& [fieldName, fieldType] : record->fields) {
            if (fieldName == name) {
                return fieldType;
            }
        }
    }
    if (const auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
        for (const auto& member : intersection->members) {
            if (auto found = member ? structuralField(member, name) : nullptr) {
                return found;
            }
        }
    }
    return nullptr;
}

} // namespace

auto TypeChecker::resolveBlockHints(
    const std::string& name,
    const std::vector<TypePtr>& nonBlockArgTypes,
    bool isMethodCall,
    bool* collection,
    std::string* skippedTo,
    std::optional<std::size_t> writtenParams) -> std::vector<TypePtr> {
    if (skippedTo) {
        skippedTo->clear();
    }
    auto imported = importedCandidateSignatures(name);
    // A `using` brings a module's functions in for a bare call as well as a
    // dot call: `scope "/api" do ... end` under `using Rodolfo` must see
    // `scope`'s `Block<[Entry]>`, or its body is not collected and the call
    // gets only the last statement — `checkCall` already offers them.
    for (const auto* function : usingImportedMethods(name, /*asMethod=*/isMethodCall)) {
        imported.push_back(function->signature);
    }
    const std::vector<Signature>* sigs = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() && scoped != m_scopedDeclaredSignatures.end()) {
        sigs = &scoped->second;
    } else if (auto user = m_userSignatures.find(name); user != m_userSignatures.end()) {
        sigs = &user->second;
    }
    auto methodIt = isMethodCall ? m_methodSignatures.find(name) : m_methodSignatures.end();
    // The function a record field holds, called with the block, when nothing
    // else answers the name: the call's candidate as checkCallCore matches it.
    const auto field = [&]() -> std::optional<std::pair<Signature, std::string>> {
        if (!isMethodCall) {
            return std::nullopt;
        }
        auto withBlock = nonBlockArgTypes;
        withBlock.push_back(Type::unknown());
        return calledFieldSignature(name, withBlock);
    }();
    if (!sigs && imported.empty() && methodIt == m_methodSignatures.end() && !field) {
        return {};
    }

    struct Candidate {
        std::vector<TypePtr> hints;
        bool collection;
        bool concreteReceiver;
        // The block parameter's name when defaulted parameters lie between
        // the call's arguments and it; empty when the block is next.
        std::string skippedTo;
    };
    std::vector<Candidate> candidates;
    auto hintsFrom = [&](const Signature& sig) -> void {
        // Parameters the call skips between its arguments and the block must
        // all have defaults: `f(attributes = {}, children)` called as
        // `f do … end` still hands its block to `children` (kexhq/kex#554).
        // Only a slot that cannot take the block is skipped: `Retry.run do …
        // end` hands it to `operation`, its first parameter, however many
        // defaulted ones follow.
        if (sig.params.size() < nonBlockArgTypes.size() + 1) {
            return;
        }
        if (sig.params.size() != nonBlockArgTypes.size() + 1 &&
            (sig.requiredParams.value_or(sig.params.size()) > nonBlockArgTypes.size() + 1 ||
             std::holds_alternative<FuncType>(
                 resolve(sig.params[nonBlockArgTypes.size()])->kind))) {
            return;
        }
        auto* blockParam = std::get_if<FuncType>(&sig.params.back()->kind);
        // A parameter that takes a function or something else takes the
        // block as that function (`replace : ... (String | (Match -> String))`).
        if (auto* either = std::get_if<UnionType>(&sig.params.back()->kind);
            !blockParam && either) {
            for (const auto& member : either->members) {
                if (auto* function = std::get_if<FuncType>(&member->kind)) {
                    if (blockParam) {
                        return;
                    }
                    blockParam = function;
                }
            }
        }
        if (!blockParam) {
            return;
        }

        for (size_t i = 0; i < nonBlockArgTypes.size(); i++) {
            if (!argMatchesParam(nonBlockArgTypes[i], sig.params[i])) {
                return;
            }
        }

        // Map the signature's type variables to concrete types from the actual
        // args, whatever their ids. Negative ids are table generics; a make
        // block's `:>` method compiled in this same unit (the prelude's own
        // `reduce :> A -> ...`) carries ordinary variables instead, and one
        // already bound by an unrelated caller would otherwise hand that
        // caller's type to every later block (kexhq/kex#408). This only
        // computes hints: nothing here is unified.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < nonBlockArgTypes.size(); i++) {
            bindHintVars(sig.params[i], nonBlockArgTypes[i], sub, [&](const TypePtr& t) {
                return resolve(t);
            });
        }

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) {
                    return it2->second;
                }
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : blockParam->params) {
            hints.push_back(applySubst(p));
        }
        auto result = resolve(blockParam->result);
        bool isCollection = blockParam->block && std::holds_alternative<ListType>(result->kind);
        std::string skipped;
        if (sig.params.size() != nonBlockArgTypes.size() + 1) {
            if (sig.paramNames.empty()) {
                return;
            }
            skipped = sig.paramNames.back();
        }
        candidates.push_back(
            {std::move(hints), isCollection,
             isMethodCall && !sig.params.empty() && !isOpenType(sig.params.front()),
             std::move(skipped)});
    };

    if (methodIt != m_methodSignatures.end()) {
        for (const auto& sig : methodIt->second) {
            hintsFrom(sig);
        }
    }
    for (const auto& sig : imported) {
        hintsFrom(sig);
    }
    if (sigs) {
        for (const auto& sig : *sigs) {
            hintsFrom(sig);
        }
    }
    // A receiver typed by a bounded constructor letter (`xs: M<Integer>` under
    // `M is Monad`) is answered by the higher-kinded traits' requirements
    // alone, as the call itself is (§8.2): `xs.into { |x| ... }` takes an
    // Integer.
    if (isMethodCall && !nonBlockArgTypes.empty()) {
        const auto receiver = resolve(nonBlockArgTypes.front());
        if (const auto* applied = std::get_if<TypeApp>(&receiver->kind);
            applied && !std::holds_alternative<NamedType>(resolve(applied->head)->kind)) {
            if (const auto requirements = m_higherKindedRequirements.find(name);
                requirements != m_higherKindedRequirements.end()) {
                candidates.clear();
                for (const auto& sig : requirements->second) {
                    hintsFrom(sig);
                }
            }
        }
    }

    if (field && candidates.empty()) {
        hintsFrom(field->first);
    }
    // `this` in a trait's default is answered by the trait's requirement, in
    // the default's own letters: `this.fold(None) do |acc, item|` under
    // `fold :> A -> (A -> T -> A) -> A` hands `item` the default's `T`, which
    // stands for every type, not a fresh one any type fills.
    if (isMethodCall && m_currentTrait && !nonBlockArgTypes.empty() &&
        isThisPlaceholder(resolve(nonBlockArgTypes.front()))) {
        std::vector<Signature> own;
        for (const auto& required : m_currentTrait->requiredMethods) {
            if (required.name == name) {
                own.push_back(traitRequirementInDefault(required));
            }
        }
        if (!own.empty()) {
            candidates.clear();
            for (const auto& sig : own) {
                hintsFrom(sig);
            }
        }
    }

    if (candidates.empty()) {
        return {};
    }
    // A receiver of known type is matched exactly by the overloads written for
    // it; one that accepts anything (an untyped or trait-bounded receiver)
    // only guesses, so it does not get a say once a concrete one fits.
    if (isMethodCall && !nonBlockArgTypes.empty() && !isOpenType(nonBlockArgTypes.front()) &&
        std::any_of(candidates.begin(), candidates.end(), [](const Candidate& c) {
            return c.concreteReceiver;
        })) {
        std::erase_if(candidates, [](const Candidate& c) { return !c.concreteReceiver; });
    }
    // Overloads that take blocks of different shapes (`times` with the index
    // or without): the block as written says which one it is for.
    if (writtenParams && std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& c) {
            return c.hints.size() == *writtenParams;
        })) {
        std::erase_if(
            candidates, [&](const Candidate& c) { return c.hints.size() != *writtenParams; });
    }
    // UFCS lets a plain call like `times(3) do ... end` match both a local
    // `times(n, block)` and an unrelated `Integer.times(block)` visible as a
    // free function. When such same-arity candidates disagree on the
    // block's shape (its declared arity, or whether it collects body
    // expressions as a `Block<[X]>`), trusting whichever matched first can
    // hand back a hint (and collection flag) for the WRONG candidate —
    // reporting a bogus arity mismatch, or silently dropping the collection
    // behavior the call actually needs (kexhq/kex#321). Stay permissive
    // instead: no reliable single shape to hint from.
    for (const auto& c : candidates) {
        if (c.hints.size() != candidates.front().hints.size() ||
            c.collection != candidates.front().collection) {
            if (collection) {
                *collection = false;
            }
            return {};
        }
    }
    if (collection) {
        *collection = candidates.front().collection;
    }
    if (skippedTo && std::all_of(candidates.begin(), candidates.end(), [&](const Candidate& c) {
            return !c.skippedTo.empty() && c.skippedTo == candidates.front().skippedTo;
        })) {
        *skippedTo = candidates.front().skippedTo;
    }
    std::vector<std::vector<TypePtr>> hintSets;
    for (const auto& c : candidates) {
        hintSets.push_back(c.hints);
    }
    return agreedHints(hintSets);
}

// A type that accepts anything a caller passes: not yet known (a variable,
// Unknown), or only bounded by a trait.
auto TypeChecker::isOpenType(const TypePtr& type) -> bool {
    auto resolved = resolve(type);
    return std::holds_alternative<TypeVar>(resolved->kind) ||
           std::holds_alternative<UnknownType>(resolved->kind) ||
           std::holds_alternative<ConstrainedType>(resolved->kind);
}

// The block-parameter hints every candidate agrees on. Candidates that agree
// on a block's SHAPE can still disagree on its parameter types, and that is
// the normal case when the receiver's type is not known: `items.reduce("")`
// on an untyped `items` matches both `String.reduce` and the list `reduce`.
// Taking the first one typed the element as Char and rejected a valid
// program (kexhq/kex#408). A position they disagree on is left Unknown, which
// inferBlock turns into a fresh variable.
auto TypeChecker::agreedHints(const std::vector<std::vector<TypePtr>>& hintSets)
    -> std::vector<TypePtr> {
    if (hintSets.empty()) {
        return {};
    }
    auto hints = hintSets.front();
    for (size_t i = 0; i < hints.size(); ++i) {
        auto first = resolve(hints[i]);
        for (size_t c = 1; c < hintSets.size(); ++c) {
            if (i >= hintSets[c].size() || !typesEqual(first, resolve(hintSets[c][i]))) {
                hints[i] = Type::unknown();
                break;
            }
        }
    }
    return hints;
}

auto TypeChecker::namedArgHints(const std::string& name, const std::string& label)
    -> std::vector<TypePtr> {
    auto candidates = importedCandidateSignatures(name);
    if (auto user = m_userSignatures.find(name); user != m_userSignatures.end()) {
        candidates.insert(candidates.begin(), user->second.begin(), user->second.end());
    }
    for (const auto& signature : candidates) {
        const auto named =
            std::find(signature.paramNames.begin(), signature.paramNames.end(), label);
        if (named == signature.paramNames.end()) {
            continue;
        }
        // A receiver prepended to `params` has no name of its own.
        const auto index = static_cast<size_t>(named - signature.paramNames.begin()) +
                           (signature.params.size() - signature.paramNames.size());
        if (index >= signature.params.size()) {
            continue;
        }
        if (const auto* function = std::get_if<FuncType>(&resolve(signature.params[index])->kind)) {
            return function->params;
        }
    }
    return {};
}

auto TypeChecker::resolveArgHints(
    const std::string& name,
    const std::vector<TypePtr>& argTypes,
    size_t slArgIdx,
    bool isMethodCall) -> std::vector<TypePtr> {
    auto imported = importedCandidateSignatures(name);
    const std::vector<Signature>* sigs = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() && scoped != m_scopedDeclaredSignatures.end()) {
        sigs = &scoped->second;
    } else if (auto user = m_userSignatures.find(name); user != m_userSignatures.end()) {
        sigs = &user->second;
    }
    auto methodIt = isMethodCall ? m_methodSignatures.find(name) : m_methodSignatures.end();
    // The function a record field holds, when nothing else answers the name.
    const auto field = isMethodCall ? calledFieldSignature(name, argTypes) : std::nullopt;
    if (!sigs && imported.empty() && methodIt == m_methodSignatures.end() && !field) {
        return {};
    }

    auto hintsFrom = [&](const Signature& sig) -> std::vector<TypePtr> {
        if (sig.params.size() != argTypes.size()) {
            return {};
        }
        if (slArgIdx >= sig.params.size()) {
            return {};
        }
        auto* funcParam = std::get_if<FuncType>(&sig.params[slArgIdx]->kind);
        if (!funcParam) {
            return {};
        }

        // All non-SL positions must match.
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (i == slArgIdx) {
                continue;
            }
            if (!argMatchesParam(argTypes[i], sig.params[i])) {
                return {};
            }
        }

        // Build generic substitution from the concrete args.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (i != slArgIdx) {
                bindHintVars(
                    sig.params[i], argTypes[i], sub, [&](const TypePtr& t) { return resolve(t); });
            }
        }

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) {
                    return it2->second;
                }
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : funcParam->params) {
            hints.push_back(applySubst(p));
        }
        return hints;
    };

    // Every candidate that fits, not the first: see agreedHints. As in
    // resolveBlockHints, a known receiver silences the overloads that would
    // accept any receiver.
    std::vector<std::vector<TypePtr>> hintSets;
    std::vector<std::vector<TypePtr>> concreteSets;
    const bool knownReceiver = isMethodCall && !argTypes.empty() && !isOpenType(argTypes.front());
    const auto consider = [&](const Signature& sig) {
        auto hints = hintsFrom(sig);
        if (hints.empty()) {
            return;
        }
        if (knownReceiver && !sig.params.empty() && !isOpenType(sig.params.front())) {
            concreteSets.push_back(hints);
        }
        hintSets.push_back(std::move(hints));
    };
    if (methodIt != m_methodSignatures.end()) {
        for (const auto& sig : methodIt->second) {
            consider(sig);
        }
    }
    for (const auto& sig : imported) {
        consider(sig);
    }
    if (sigs) {
        for (const auto& sig : *sigs) {
            consider(sig);
        }
    }
    if (field && hintSets.empty()) {
        consider(field->first);
    }
    // A receiver typed by a bounded constructor letter (`xs: M<Integer>` under
    // `M is Monad`) is answered by the higher-kinded traits' requirements
    // alone, as the call itself is (§8.2): their `F<A>` makes the block's A
    // an Integer.
    if (isMethodCall && !argTypes.empty()) {
        const auto receiver = resolve(argTypes.front());
        if (const auto* applied = std::get_if<TypeApp>(&receiver->kind);
            applied && !std::holds_alternative<NamedType>(resolve(applied->head)->kind)) {
            if (const auto requirements = m_higherKindedRequirements.find(name);
                requirements != m_higherKindedRequirements.end()) {
                hintSets.clear();
                concreteSets.clear();
                for (const auto& sig : requirements->second) {
                    consider(sig);
                }
            }
        }
    }
    if (!concreteSets.empty()) {
        hintSets = std::move(concreteSets);
    }
    for (const auto& set : hintSets) {
        if (set.size() != hintSets.front().size()) {
            return {};
        }
    }
    return agreedHints(hintSets);
}

// A bare `~f` capture — no argument groups, no operator, unqualified. Like a
// shorthand lambda, it needs the callee's signature to know what it's being
// checked against, so it's deferred to the contextual (hinted) pass.
static auto isBareCapture(const ast::Expr* e) -> bool {
    if (!e) {
        return false;
    }
    return std::holds_alternative<ast::CurryExpr>(e->kind) && writtenFunction(*e);
}

// While it lives, the `.try`s written directly in one block's body (not in a
// block nested in it, not under a `trying`) are collected into `tries`.
struct BlockTries {
    using Tries = std::vector<std::pair<SourceLocation, TypePtr>>;
    BlockTries(Tries*& slot, int& depth, int blockDepth)
        : m_slot(slot),
          m_depth(depth),
          m_savedSlot(std::exchange(slot, &tries)),
          m_savedDepth(std::exchange(depth, blockDepth)) {}
    ~BlockTries() {
        m_slot = m_savedSlot;
        m_depth = m_savedDepth;
    }
    BlockTries(const BlockTries&) = delete;
    auto operator=(const BlockTries&) -> BlockTries& = delete;
    Tries tries;

private:
    Tries*& m_slot;
    int& m_depth;
    Tries* m_savedSlot;
    int m_savedDepth;
};

auto TypeChecker::inferBlock(const ast::Expr& blockExpr, const std::vector<TypePtr>& hintParams)
    -> TypePtr {
    if (auto* lam = std::get_if<ast::Lambda>(&blockExpr.kind)) {
        if (lam->params.empty() && !lam->collection) {
            // Zero-param lambda `{ }` / `do end`. Non-empty hintParams means
            // the call site's expected type is a known, concrete FuncType of
            // nonzero arity (see resolveBlockHints/resolveArgHints — they
            // only produce hints from an actual FuncType param), so a
            // paramless block there is a genuine arity mismatch: return the
            // block's real (zero-arg) FuncType and let the caller's normal
            // FuncType comparison report it, the same way an explicit-params
            // block with the wrong arity already is. Otherwise the expected
            // type isn't a known function shape (a generic/inferred param,
            // e.g. `retry do ... end`), so stay permissive and match anything.
            auto bodyType = inferExpr(blockExpr);
            if (!hintParams.empty()) {
                return bodyType;
            }
            return Type::unknown();
        }
        // `pairs.map { |glyph, words| … }` over `[(String, String)]` spreads
        // each element across the parameters on both backends, so the
        // parameters are typed by the tuple's elements, not each by the whole
        // tuple. `glyph` was `(String, String)` and passing it on to a
        // `String` parameter was rejected (kexhq/kex#537). The function-type
        // comparison accepts the block's shape (argMatchesParam).
        auto spreadHints = hintParams;
        if (lam->params.size() > 1 && hintParams.size() == 1) {
            if (auto* tuple = std::get_if<TupleType>(&resolve(hintParams[0])->kind);
                tuple && tuple->elements.size() == lam->params.size()) {
                spreadHints = tuple->elements;
            }
        }
        // The same with more parameters after the element: Map's
        // `mapIndexed { |k, v, i| … }` spreads the entry across `k, v` and
        // takes the index last (kexhq/kex#667). Typed only by count, the
        // block matched nothing and its call went undecided.
        std::size_t spreadAcross = 0;
        if (hintParams.size() > 1 && lam->params.size() > hintParams.size()) {
            const auto element = resolve(hintParams[0]);
            const auto across = lam->params.size() - hintParams.size() + 1;
            if (auto* tuple = std::get_if<TupleType>(&element->kind);
                tuple && tuple->elements.size() == across) {
                spreadAcross = across;
                spreadHints = tuple->elements;
                spreadHints.insert(spreadHints.end(), hintParams.begin() + 1, hintParams.end());
            } else if (
                std::holds_alternative<UnknownType>(element->kind) ||
                std::holds_alternative<TypeVar>(element->kind)) {
                // The element is not known yet (a Map's, through Enumerable):
                // the leading parameters stay untyped, as they were, but the
                // block keeps the shape it was expected to have.
                spreadAcross = across;
                spreadHints.assign(across, element);
                spreadHints.insert(spreadHints.end(), hintParams.begin() + 1, hintParams.end());
            }
        }
        pushScope();
        std::vector<TypePtr> paramTypes;
        std::unordered_map<std::string, TypePtr> genericVars;
        for (size_t i = 0; i < lam->params.size(); i++) {
            TypePtr pt;
            if (lam->params[i].type) {
                pt = resolveTypeExpr(**lam->params[i].type, genericVars);
            } else if (i < spreadHints.size()) {
                auto hint = resolve(spreadHints[i]);
                // A type variable the function's own signature writes (`X` in
                // `combine(a: X, b: X)`) is one fixed type here, as good a hint
                // as a concrete one. Replaced by a fresh variable, `[a].reduce(b)
                // { |x, y| x + y }` gave `+` no `X` to refuse, though `a + b`
                // was (kexhq/kex#777).
                // A written `Any` (an element of `[Any]`) is the top type, which
                // the block must narrow before use (decision 7), not an unknown
                // its body may settle.
                const auto rigid = typeVariableId(hint);
                const auto* unknown = std::get_if<UnknownType>(&hint->kind);
                if ((!std::holds_alternative<TypeVar>(hint->kind) ||
                     (rigid && m_rigidVars.contains(*rigid))) &&
                    (!unknown || unknown->written)) {
                    pt = hint;
                } else {
                    pt = freshTypeVar();
                }
            } else {
                pt = freshTypeVar();
            }
            paramTypes.push_back(pt);
            if (lam->params[i].name != "_") {
                defineVar(
                    lam->params[i].name, pt,
                    {false, false, lam->params[i].location.line, lam->params[i].location.column});
                if (m_recordsNames && lam->params[i].location.line > 0) {
                    m_bindingSites.emplace_back(lam->params[i].name, lam->params[i].location);
                }
                // Recorded for tooling like any other binding. A LambdaParam
                // carries no location of its own, so the block's own position
                // is the anchor and the name is found from there — without
                // this, hovering `x` in `{ |x| … }` answered nothing while a
                // use of it answered fine.
                m_patternBindings.push_back(
                    {lam->params[i].name,
                     lam->params[i].location.line > 0 ? lam->params[i].location
                                                      : blockExpr.location,
                     pt});
            }
        }
        m_blockDepth++;
        BlockTries blockTries(m_clauseTries, m_clauseTriesDepth, m_blockDepth);
        TypePtr bodyType;
        if (lam->collection) {
            TypePtr element = freshTypeVar();
            bool haveElement = false;
            for (const auto& item : lam->body) {
                if (!item) {
                    continue;
                }
                TypePtr itemType;
                if (const auto* spread = std::get_if<ast::SpreadExpr>(&item->kind)) {
                    auto spreadType = resolve(inferExpr(*spread->inner));
                    if (auto* list = std::get_if<ListType>(&spreadType->kind)) {
                        itemType = list->element;
                    } else {
                        error(
                            diag::Code::CollectionSpreadNotList, item->location,
                            {{"type", typeToString(spreadType)}});
                        itemType = Type::unknown();
                    }
                } else if (const auto* guarded = std::get_if<ast::TrailingIf>(&item->kind)) {
                    checkCondition(*guarded->condition, diag::Code::CollectionGuardNotBool);
                    itemType = inferExpr(*guarded->expr);
                } else {
                    itemType = inferExpr(*item);
                }
                if (!haveElement) {
                    element = itemType;
                    haveElement = true;
                } else if (
                    !argMatchesParam(itemType, element) && !argMatchesParam(element, itemType)) {
                    typeMismatch(item->location, resolve(element), resolve(itemType));
                }
            }
            bodyType = Type::list(resolve(element));
        } else {
            bodyType = inferBody(lam->body);
        }
        if (lam->rescue) {
            checkRescue(*lam->rescue);
        }
        m_blockDepth--;
        auto resultType = resolve(bodyType);
        if (lam->returnAnnotation) {
            auto declared = resolveTypeExpr(**lam->returnAnnotation, genericVars);
            if (!std::holds_alternative<UnknownType>(resultType->kind) &&
                !std::holds_alternative<TypeVar>(resultType->kind) &&
                !argMatchesParam(resultType, declared) &&
                !wrapsDeclaredSuccess(lam->body, declared, resultType)) {
                typeMismatch(blockExpr.location, declared, resultType);
            }
            resultType = declared;
        }
        resultType = blockFailingResult(*lam, blockTries.tries, resultType);
        popScope();
        // Outwardly a block spreading the element before more parameters
        // takes what it was expected to: the element whole, then the rest.
        if (spreadAcross > 0) {
            std::vector<TypePtr> outward{hintParams[0]};
            outward.insert(outward.end(), paramTypes.begin() + spreadAcross, paramTypes.end());
            return Type::func(std::move(outward), resultType);
        }
        // Outwardly a spreading block still takes the whole element first,
        // the shape argMatchesParam recognises as destructuring.
        if (spreadHints.size() != hintParams.size() && !lam->params[0].type) {
            paramTypes[0] = hintParams[0];
        }
        return Type::func(std::move(paramTypes), resultType);
    }
    // A bare `~f` passed where a function is expected: check it against the
    // hinted param types, so `words.filter(~even?)` reports the element-type
    // mismatch rather than inferring fresh vars that unify with anything.
    // Only plain unqualified captures — an operator or a `~Mod.fn` name isn't
    // resolvable through checkCall here, and partial applications already
    // carry their own arg types.
    if (auto* ce = std::get_if<ast::CurryExpr>(&blockExpr.kind);
        ce && ce->argGroups.empty() && !ce->isOperator && ce->module.empty() &&
        !hintParams.empty()) {
        std::vector<TypePtr> paramTypes;
        for (const auto& h : hintParams) {
            auto pt = resolve(h);
            paramTypes.push_back(
                (std::holds_alternative<TypeVar>(pt->kind) ||
                 std::holds_alternative<UnknownType>(pt->kind))
                    ? freshTypeVar()
                    : pt);
        }
        auto resultType = checkCall(
            ce->name, paramTypes, blockExpr.location,
            /*isMethodCall=*/false);
        decideCapture(blockExpr, *ce);
        // The function it names, as the capture case below records it: the
        // one reached with this many parameters (#640).
        {
            std::vector<Signature> visible;
            std::set<std::string> hidden;
            const auto* reached = reachableFreeFunctions(ce->name, false, visible, hidden);
            const auto candidates = reached ? *reached : importedCandidateSignatures(ce->name);
            for (const auto& signature : candidates) {
                if (signature.params.size() == paramTypes.size()) {
                    m_captureSignatures[&blockExpr] = signature;
                    break;
                }
            }
        }
        return Type::func(std::move(paramTypes), resolve(resultType));
    }
    // A qualified capture is the call it names, made with what it is passed:
    // `~String.upperCase` is `x.upperCase` on a String `x`, `~Mod.fn` is
    // `Mod.fn(x)`. Typed by nothing, `[1, 2].map(~String.upperCase)` was left
    // to run time while String's and Char's overloads disagreed.
    if (auto* ce = std::get_if<ast::CurryExpr>(&blockExpr.kind);
        ce && ce->argGroups.empty() && !ce->isOperator && !ce->module.empty()) {
        // What it decides on its own, as written alone.
        const auto reported = m_diagnostics ? m_diagnostics->size() : 0;
        const auto captured = inferExpr(blockExpr);
        // One naming nothing is reported as that, once.
        if (hintParams.empty() || (m_diagnostics && m_diagnostics->size() > reported)) {
            return captured;
        }
        std::vector<TypePtr> paramTypes;
        for (const auto& hint : hintParams) {
            const auto passed = resolve(hint);
            paramTypes.push_back(isOpenType(passed) ? freshTypeVar() : passed);
        }
        const bool ofType = namesKnownType(ce->module);
        // A type's own function takes a value of that type, whichever other
        // type has a function of the name.
        if (const auto owner = ofType ? typeNamed(ce->module, blockExpr.location) : nullptr;
            owner && !containsOpenType(paramTypes.front()) &&
            !argMatchesParam(paramTypes.front(), owner)) {
            error(
                diag::Code::NoOverloadArgumentMismatch, blockExpr.location,
                {{"name", ce->module + "." + ce->name},
                 {"index", "1"},
                 {"expected", typeToString(owner)},
                 {"actual", typeToString(paramTypes.front())},
                 {"signatures", ""}});
            return Type::func(std::move(paramTypes), Type::unknown());
        }
        const auto result =
            ofType ? checkCall(ce->name, paramTypes, blockExpr.location, /*isMethodCall=*/true)
                   : checkCall(
                         kex::mangle::qualify(ce->module, ce->name), paramTypes, blockExpr.location,
                         /*isMethodCall=*/false);
        return Type::func(std::move(paramTypes), resolve(result));
    }
    // `&.method(args)`: the lambda it means.
    if (auto* sl = std::get_if<ast::ShorthandLambda>(&blockExpr.kind)) {
        auto type = inferBlock(*sl->lambda, hintParams);
        m_typeMap[sl->lambda.get()] = type;
        return type;
    }
    // BlockExpr (no param list) — infer body for side effects, stay permissive.
    inferExpr(blockExpr);
    return Type::unknown();
}

auto TypeChecker::typeOf(const ast::Expr* expr) const -> TypePtr {
    auto it = m_typeMap.find(expr);
    return it != m_typeMap.end() ? it->second : nullptr;
}

auto TypeChecker::typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>& {
    return m_typeMap;
}

auto TypeChecker::assignmentTargetTypeOf(const ast::Expr* expr) const -> TypePtr {
    auto it = m_assignmentTargetTypes.find(expr);
    return it != m_assignmentTargetTypes.end() ? it->second : nullptr;
}

auto TypeChecker::functionSignatures(const ast::FunctionDef* function) const
    -> const std::vector<Signature>* {
    auto it = m_functionSignatures.find(function);
    return it == m_functionSignatures.end() ? nullptr : &it->second;
}

auto TypeChecker::resolvedCalls() const
    -> const std::unordered_map<const ast::MethodCall*, ResolvedCallTarget>& {
    return m_resolvedCalls;
}

auto TypeChecker::checkCondition(const ast::Expr& condition, diag::Code code) -> void {
    const auto resolved = resolve(inferExpr(condition));
    if (const auto* variable = std::get_if<TypeVar>(&resolved->kind)) {
        unifyVar(variable->id, Type::boolean());
        return;
    }
    // A written `Any` is not a Bool, as no other non-Bool is (decision 7): an
    // unknown the checker could not infer is the only one let through.
    const auto* unknown = std::get_if<UnknownType>(&resolved->kind);
    if ((unknown && !unknown->written) || typesEqual(resolved, Type::boolean())) {
        return;
    }
    error(code, condition.location, {{"type", typeToString(resolved)}});
}

auto TypeChecker::calledFieldSignature(
    const std::string& name, const std::vector<TypePtr>& argTypes)
    -> std::optional<std::pair<Signature, std::string>> {
    const auto receiver = argTypes.empty() ? nullptr : resolve(argTypes.front());
    const auto* named = receiver ? std::get_if<NamedType>(&receiver->kind) : nullptr;
    if (!named) {
        return std::nullopt;
    }
    const auto record = findRecordFields(*named);
    if (record == m_recordFields.end()) {
        return std::nullopt;
    }
    const auto field = record->second.find(name);
    if (field == record->second.end()) {
        return std::nullopt;
    }
    // `A -> B -> C` called with two arguments takes both.
    Signature signature;
    signature.name = name;
    signature.params.push_back(receiver);
    auto type = resolve(substituteInterfaceGenerics(field->second, named->typeArgs));
    while (signature.params.size() < argTypes.size()) {
        const auto* function = std::get_if<FuncType>(&type->kind);
        if (!function) {
            break;
        }
        signature.params.insert(
            signature.params.end(), function->params.begin(), function->params.end());
        type = resolve(function->result);
    }
    if (signature.params.size() == 1) {
        return std::nullopt;
    }
    signature.result = type;
    return std::pair{std::move(signature), resolveRecordName(named->name)};
}

auto TypeChecker::expectedBlockHints(const ast::Expr& expr) -> std::vector<TypePtr> {
    if (!m_branchExpectation || !writtenFunction(expr)) {
        return {};
    }
    const auto expected = resolve(std::exchange(m_branchExpectation, nullptr));
    const auto* function = std::get_if<FuncType>(&expected->kind);
    const auto* lambda = std::get_if<ast::Lambda>(&expr.kind);
    // A block of another shape is reported where its type is compared.
    if (!function || function->params.empty() ||
        (lambda && lambda->params.size() != function->params.size())) {
        return {};
    }
    std::vector<TypePtr> hints;
    for (const auto& param : function->params) {
        const auto taken = resolve(param);
        hints.push_back(containsOpenType(taken) ? freshTypeVar() : taken);
    }
    return hints;
}

auto TypeChecker::inferExpr(const ast::Expr& expr) -> TypePtr {
    if (m_recordsNames) {
        recordNames(expr);
    }
    // An assignment among this node's operands is an expression with the
    // value assigned (`parent.send(total = total + i)`); one standing as a
    // statement stays Void, so a branch ending in one joins as before
    // (kexhq/kex#614).
    ast::forEachOperand(expr, [this](const ast::ExprPtr& operand) {
        if (std::holds_alternative<ast::AssignExpr>(operand->kind)) {
            m_valueAssignments.insert(operand.get());
        }
    });
    // A block or a capture where a function is expected (a typed `let`, a
    // declared result, a record field) takes its parameters from it, as one
    // passed as an argument does: `let f: String -> Integer = { |s| s + 1 }`
    // adds 1 to a String.
    if (const auto hints = expectedBlockHints(expr); !hints.empty()) {
        auto type = inferBlock(expr, hints);
        m_typeMap[&expr] = type;
        return type;
    }
    auto result = std::visit(
        [this, &expr](const auto& node) -> TypePtr {
            using T = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<T, ast::IntLiteral>) {
                return Type::integer();
            } else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
                return Type::float64();
            } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                for (const auto& value : node.values) {
                    if (value) {
                        inferExpr(*value);
                    }
                }
                return Type::string();
            } else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
                return Type::boolean();
            } else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
                return Type::charT();
            } else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
                if (auto constructed = constructorResultType("None", {})) {
                    return constructed;
                }
                return Type::named("None");
            } else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
                return Type::atom();
            } else if constexpr (std::is_same_v<T, ast::Identifier>) {
                if (reportOtherFilesTopLevel("name", node.name, expr.location)) {
                    return Type::unknown();
                }
                // A local binding is never a zero-parameter function, even an
                // Unknown-typed one: `let root = match JSON.parse(...)` must not
                // take the type of some module's `let root -> String`, nor
                // `let manifest = ...` be the module's foul `manifest`.
                const bool boundLocally =
                    std::any_of(m_scopeStack.begin(), m_scopeStack.end(), [&](const auto& scope) {
                        return scope.get(node.name) != nullptr;
                    });
                if (!boundLocally) {
                    if (auto constant = constantDeclaration(node.name); !constant.empty()) {
                        m_referencedConstants[&expr] = std::move(constant);
                    }
                }
                if (node.name == "from" && m_inCastSlot) {
                    error(diag::Code::FromInCast, expr.location);
                    return Type::unknown();
                }
                if (m_recordsNames && boundLocally) {
                    if (const auto* binding = bindingOf(node.name); binding && binding->line > 0) {
                        m_localDefinitions[&expr] = {
                            expr.location.file, binding->line, binding->column};
                    }
                }
                auto type = lookupVar(node.name);
                // In a trait's default, a requirement named bare is the
                // requirement, called on `this`: not some implementer's
                // zero-parameter function of that name, which BEAM lowered as
                // a variable.
                if (!boundLocally && m_currentTrait && m_currentMakeType &&
                    isThisPlaceholder(resolve(m_currentMakeType))) {
                    if (auto own = ownFunctionOnThis(expr, node.name, {})) {
                        return own;
                    }
                }
                if (!boundLocally &&
                    (!type || std::holds_alternative<UnknownType>(resolve(type)->kind))) {
                    // `let ids -> [String] = [...]` is a zero-parameter function,
                    // and naming it auto-calls it — so the name has the declared
                    // result type. Left Unknown, `ids.reduce(...)` picked String's
                    // `reduce` overload and typed each element as Char (#329).
                    // A function out of reach here (a `using ..., only:` left it
                    // out, or it is private to its module) is not this name.
                    if (bareFunctionReachable(node.name)) {
                        if (auto result = zeroArgBindingResult(node.name)) {
                            return result;
                        }
                        if (auto result = functionValueType(node.name)) {
                            return result;
                        }
                    }
                }
                // Declared, but only private to a module this code is not in:
                // `using Config` does not bring `Config`'s private `secret` in,
                // and the walker could not find it while BEAM could
                // (kexhq/kex#781). Known only by an unknown type, not by a
                // binding of its own.
                if (!boundLocally &&
                    (!type || std::holds_alternative<UnknownType>(resolve(type)->kind)) &&
                    !bareFunctionReachable(node.name) && privateElsewhere(node.name)) {
                    reportUndefined("name", node.name, expr.location);
                    return Type::unknown();
                }
                // A sibling function of the `make` block being checked, named
                // bare: called on `this`, as the call form is (bareMethodCall).
                if (!type && !boundLocally) {
                    if (auto own = ownFunctionOnThis(expr, node.name, {})) {
                        return own;
                    }
                }
                if (!type) {
                    if (node.name == "new") {
                        error(
                            m_inMakeBlock && m_currentMakeType
                                ? diag::Code::NewRequiresRecordReceiver
                                : diag::Code::NewRequiresRecordReceiverOrBinding,
                            expr.location);
                    } else if (
                        node.name != "_" && !bareNameDeclared(node.name) &&
                        !m_referencedConstants.contains(&expr)) {
                        reportUndefined("name", node.name, expr.location);
                    }
                    return Type::unknown();
                }
                return type;
            } else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
                if (reportOtherFilesTopLevel("constructor or type", node.name, expr.location)) {
                    return Type::unknown();
                }
                if (auto constant = constantDeclaration(node.name); !constant.empty()) {
                    m_referencedConstants[&expr] = std::move(constant);
                }
                if (auto constructed = nullaryConstructorType(node.name)) {
                    // A module's constructor: which module's, so a backend binds
                    // it by that rather than by its own reading of the `using`s.
                    if (const auto* adt = std::get_if<NamedType>(&constructed->kind);
                        adt && kex::mangle::hasScope(adt->name)) {
                        m_decidedCalls[&expr] = {CallKind::Constructor, adt->name};
                    }
                    return constructed;
                }
                auto type = lookupVar(node.name);
                // A built-in type's name reaches lookupVar through the globals that
                // annotations resolve against: as a value it is still a type name.
                const bool builtinName =
                    type && type == m_globals.get(node.name) && builtinType(node.name) != nullptr;
                // A binding that says nothing (a module of the same name, `Binary`)
                // does not hide the type the name also is.
                const auto* unknownBinding =
                    type ? std::get_if<UnknownType>(&resolve(type)->kind) : nullptr;
                const bool saysNothing =
                    unknownBinding && !unknownBinding->written && namesKnownType(node.name);
                if (type && !builtinName && !saysNothing) {
                    return type;
                }
                // A bare TYPE NAME used as a value (`x.to(Integer)`, `let t =
                // String`) is a `Type<X>`: what a `Type<T>` parameter takes.
                if (namesKnownType(node.name)) {
                    // A type named as a value is that type's `Type` record
                    // (`Type<String>`), except `Void`, which is the unit value.
                    if (node.name == kex::names::kVoidType) {
                        return Type::named(node.name);
                    }
                    const auto named = typeNamed(node.name, expr.location);
                    return Type::named(
                        kex::names::kTypeRecord, {named ? named : Type::named(node.name)});
                }
                if (auto constant = constantType(node.name)) {
                    return constant;
                }
                // A constructor with payloads is no value of its own: named bare
                // (`xs.map(Wrap)`), it was "undefined", or, under the `using`
                // that brings it in, to be brought in with that `using`.
                if (const auto arity = constructorArityInScope(node.name); arity && *arity > 0) {
                    if (*arity == 1) {
                        error(
                            diag::Code::ConstructorUsedAsValueOneValue, expr.location,
                            {{"name", node.name}, {"block", "{ |x| " + node.name + "(x) }"}});
                    } else {
                        error(
                            diag::Code::ConstructorUsedAsValue, expr.location,
                            {{"name", node.name},
                             {"count", std::to_string(*arity)},
                             {"block", "{ |x| " + node.name + "(x, …) }"}});
                    }
                    return Type::unknown();
                }
                // A trait names what values have, not a value (`IO.inspect(Monad)`).
                if (const auto trait =
                        resolveTraitReference(node.name, m_currentModulePath, nullptr, false);
                    m_traits.get(trait)) {
                    error(diag::Code::TraitUsedAsValue, expr.location, {{"name", node.name}});
                    return Type::unknown();
                }
                if (!reportConstructorOutOfScope(node.name, expr.location) &&
                    !bareNameDeclared(node.name)) {
                    reportUndefined("constructor or type", node.name, expr.location);
                }
                return Type::unknown();
            } else if constexpr (std::is_same_v<T, ast::WithExpr>) {
                std::string capability = node.capability.dotted();
                // Only a declared capability may be replaced. Without this, any
                // module is substitutable and the keyword means nothing, and a
                // typo binds nothing while the body still runs against the real
                // implementation — the silent-mock failure the feature exists to
                // remove (kexhq/kex#180).
                auto actual = node.value ? resolve(inferExpr(*node.value)) : Type::unknown();
                if (!m_capabilities.count(capability)) {
                    error(diag::Code::NotACapability, expr.location, {{"capability", capability}});
                } else if (
                    !std::holds_alternative<UnknownType>(actual->kind) &&
                    !std::holds_alternative<TypeVar>(actual->kind) &&
                    !satisfiesTrait(actual, capability)) {
                    // The replacement has to provide what the capability declares.
                    error(
                        diag::Code::CapabilityReplacementMismatch, expr.location,
                        {{"capability", capability}, {"type", typeToString(actual)}});
                }
                pushScope();
                auto bodyType = inferBody(node.body);
                popScope();
                return bodyType;
            } else if constexpr (std::is_same_v<T, ast::GeneratedDecl>) {
                // `let %name(...)` in compile-time code: what runs now is the
                // name it computes. The template is checked as the declaration it
                // becomes, after expansion.
                if (node.name) {
                    inferExpr(*node.name);
                }
                return Type::voidType();
            } else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
                ImportSelection selection;
                selection.module += node.module.dotted();
                selection.onlyNames = node.onlyNames;
                selection.exceptNames = node.exceptNames;
                selection.alias = node.alias;
                selection.location = expr.location;
                if (node.body.empty()) {
                    if (!m_importScopeStack.empty()) {
                        pushImport(std::move(selection));
                    }
                    return Type::voidType();
                }
                pushScope();
                pushImport(std::move(selection));
                auto bodyType = inferBody(node.body);
                popScope();
                return bodyType;
            } else if constexpr (std::is_same_v<T, ast::TryExpr>) {
                if (m_tryingBlockDepth >= 0 && m_blockDepth > m_tryingBlockDepth) {
                    m_tryBelowBlock++;
                }
                auto operand = resolve(node.operand ? inferExpr(*node.operand) : Type::unknown());
                // In a function clause's own body, where no `trying` takes it.
                if (m_clauseTries && m_blockDepth == m_clauseTriesDepth &&
                    m_tryingBlockDepth != m_blockDepth) {
                    m_clauseTries->push_back({expr.location, operand});
                }
                if (auto payload = tryPayload(operand)) {
                    return payload;
                }
                if (isUndetermined(operand)) {
                    return Type::unknown();
                }
                error(
                    diag::Code::TryNotResultOrOptional, expr.location,
                    {{"type", typeToString(operand)}});
                return Type::unknown();
            } else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
                auto outerTryingDepth = m_tryingBlockDepth;
                auto outerTryBelowBlock = m_tryBelowBlock;
                m_tryingBlockDepth = m_blockDepth;
                m_tryBelowBlock = 0;
                auto bodyType = inferBody(node.body);
                // A body that exits the function hands nothing to the `trying`
                // expression, so it must not be compared against the rescue arms.
                auto result = resolve(alwaysReturns(node.body) ? Type::never() : bodyType);
                // A block is a function boundary: a `.try` inside one becomes the
                // BLOCK's result and never reaches this rescue, so the body's type
                // and the rescue's have no reason to agree — the rescue is dead
                // for that failure. `.try` is typed optimistically (payload only),
                // which is what makes the disagreement look real.
                const bool rescueUnreachable = m_tryBelowBlock > 0;
                m_tryingBlockDepth = outerTryingDepth;
                m_tryBelowBlock = outerTryBelowBlock;
                auto merge = [&](TypePtr candidate) {
                    candidate = resolve(candidate);
                    bool resultPermissive =
                        isUndetermined(result) || std::holds_alternative<NeverType>(result->kind);
                    bool candidatePermissive = isUndetermined(candidate) ||
                                               std::holds_alternative<NeverType>(candidate->kind);
                    if (resultPermissive && !candidatePermissive) {
                        result = candidate;
                    } else if (
                        !resultPermissive && !candidatePermissive && !rescueUnreachable &&
                        !argMatchesParam(candidate, result) &&
                        !argMatchesParam(result, candidate)) {
                        error(
                            diag::Code::TryingRescueMismatch, expr.location,
                            {{"body", typeToString(result)}, {"rescue", typeToString(candidate)}});
                    }
                };
                checkRescue(node.rescue, merge);
                return result;
            } else if constexpr (std::is_same_v<T, ast::LetExpr>) {
                if (hasNoValue(node.value.get())) {
                    error(diag::Code::IfWithoutElseHasNoValue, node.value->location);
                }
                TypePtr branchExpectation;
                if (node.type && *node.type && node.value &&
                    (std::holds_alternative<ast::IfExpr>(node.value->kind) ||
                     std::holds_alternative<ast::MatchExpr>(node.value->kind) ||
                     std::holds_alternative<ast::ThenElseExpr>(node.value->kind) ||
                     std::holds_alternative<ast::ListExpr>(node.value->kind) ||
                     writtenFunction(*node.value))) {
                    // Muted: declaredBindingType below reports a bad annotation.
                    std::vector<Diagnostic> muted;
                    auto* const reported = std::exchange(m_diagnostics, &muted);
                    std::unordered_map<std::string, TypePtr> generics;
                    branchExpectation = resolveTypeExpr(**node.type, generics);
                    // `Any` resolves to the unknown type, which every arm fits —
                    // the point of writing it. A misspelled annotation does too,
                    // and is reported as one below.
                    m_diagnostics = reported;
                }
                const auto outerExpectation = std::exchange(m_branchExpectation, branchExpectation);
                auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
                m_branchExpectation = outerExpectation;
                // A local `let UPPER = ...` binds the body's value, not the
                // zero-arg function the parser wraps it in (kexhq/kex#603):
                // unwrap here so the annotation check and the binding below
                // both see the value's type.
                if (ast::isLocalConstantBind(node.pattern, node.value)) {
                    auto resolvedValue = resolve(valueType);
                    if (auto* func = std::get_if<FuncType>(&resolvedValue->kind)) {
                        if (func->params.empty()) {
                            valueType = func->result;
                        }
                    }
                }
                // `let xs: [Colour] = []` — the annotation is what the binding IS;
                // the initializer only has to be compatible with it. Binding the
                // inferred type instead would defeat the reason for writing one.
                auto declared = declaredBindingType(
                    node.type, valueType, expr.location, node.value ? node.value.get() : nullptr);
                if (declared && node.value) {
                    checkByteLiterals(*node.value, declared);
                }
                if (auto* varPat = std::get_if<ast::VarPattern>(&node.pattern->kind)) {
                    // `x : Type.of(y)` on the line above `let x = 34`: a top-level
                    // binding is a synthetic-main LetExpr, not a 0-arg function, so
                    // its declaration has to be matched here. Scoped to the
                    // synthetic main, or a local `let` would pick up a global
                    // annotation that merely shares its name.
                    if (!declared && m_inSyntheticMain) {
                        declared = declaredConstantType(varPat->name, valueType, expr.location);
                    }
                    // At the name as written (`let total`), not the statement's start.
                    declareVar(
                        varPat->name, declared ? declared : valueType,
                        /*isVar=*/false, expr.location, node.pattern->location);
                    // For tooling, as a pattern's names are: hover on the name answers
                    // for the binding, not for some same-named function.
                    m_patternBindings.push_back(
                        {varPat->name, node.pattern->location, declared ? declared : valueType});
                    // Record `f`'s origin when bound to a bare `~greet` capture
                    // (no partial application, no operator, no module prefix) so
                    // a later named-argument call through `f` can find `greet`'s
                    // real parameter names (kexhq/kex#309). Any other kind of
                    // binding for this name clears a stale entry.
                    if (const auto* curry =
                            node.value ? std::get_if<ast::CurryExpr>(&node.value->kind) : nullptr;
                        curry && curry->argGroups.empty() && !curry->isOperator &&
                        curry->module.empty()) {
                        m_curryOrigin[varPat->name] = curry->name;
                    } else {
                        m_curryOrigin.erase(varPat->name);
                    }
                } else if (node.pattern) {
                    bindDestructuredPattern(*node.pattern, valueType);
                }
                return Type::voidType();
            } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
                if (hasNoValue(node.value.get())) {
                    error(diag::Code::IfWithoutElseHasNoValue, node.value->location);
                }
                auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
                auto declared = declaredBindingType(
                    node.type, valueType, expr.location, node.value ? node.value.get() : nullptr);
                if (auto* varPat = node.pattern ? std::get_if<ast::VarPattern>(&node.pattern->kind)
                                                : nullptr) {
                    // At the name as written (`var p`): the statement starts at `var`,
                    // and definition led there (`make fuzz-lsp`).
                    declareVar(
                        varPat->name, declared ? declared : valueType,
                        /*isVar=*/true, expr.location, node.pattern->location);
                    // For tooling, as a pattern's names are: hover on the name answers
                    // for the binding, not for some same-named function.
                    m_patternBindings.push_back(
                        {varPat->name, node.pattern->location, declared ? declared : valueType});
                    // Same origin tracking as `let`, so a named-argument call
                    // through `var f = ~greet` finds `greet`'s real parameter
                    // names (kexhq/kex#309).
                    if (const auto* curry =
                            node.value ? std::get_if<ast::CurryExpr>(&node.value->kind) : nullptr;
                        curry && curry->argGroups.empty() && !curry->isOperator &&
                        curry->module.empty()) {
                        m_curryOrigin[varPat->name] = curry->name;
                    } else {
                        m_curryOrigin.erase(varPat->name);
                    }
                } else if (node.pattern) {
                    const auto outer = std::exchange(m_patternBindsVar, true);
                    bindDestructuredPattern(*node.pattern, valueType);
                    m_patternBindsVar = outer;
                }
                return Type::voidType();
            } else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
                auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
                auto varType = lookupVar(node.name);
                if (varType) {
                    m_assignmentTargetTypes[&expr] = varType;
                }
                // `new.age = ...` in a make block writes the value being built.
                else if (node.name == "new" && m_inMakeBlock && m_currentMakeType) {
                    m_assignmentTargetTypes[&expr] = m_currentMakeType;
                } else if (node.name != "new" && !m_topLevelValueNames.contains(node.name)) {
                    reportUndefined("variable", node.name, expr.location);
                }
                if (const auto* binding = varType ? bindingOf(node.name) : nullptr;
                    binding && !binding->isVar) {
                    error(diag::Code::AssignToImmutable, expr.location, {{"name", node.name}});
                }
                if (!node.path.empty()) {
                    if (node.path.size() != 1) {
                        error(diag::Code::NestedFieldAssignment, expr.location);
                        return Type::voidType();
                    }
                    auto resolvedVar = varType ? resolve(varType) : Type::unknown();
                    auto* named = std::get_if<NamedType>(&resolvedVar->kind);
                    auto record = named ? findRecordFields(*named) : m_recordFields.end();
                    if (record == m_recordFields.end()) {
                        error(diag::Code::FieldAssignmentNeedsRecord, expr.location);
                        return Type::voidType();
                    }
                    auto field = record->second.find(node.path.front());
                    if (field == record->second.end()) {
                        error(
                            diag::Code::RecordHasNoField, expr.location,
                            {{"record", named->name}, {"field", node.path.front()}});
                    } else if (
                        !containsOpenType(valueType) && !containsOpenType(field->second) &&
                        !argMatchesParam(valueType, field->second) &&
                        !(node.value && literalCoercesTo(*node.value, field->second))) {
                        typeMismatch(expr.location, field->second, valueType);
                    }
                    // Only a field the record has: past the end there is no type
                    // to read, and reading one crashed the checker on a garbled
                    // `new.balance = 1` (found by `make fuzz-lsp`).
                    if (node.value && field != record->second.end()) {
                        noteFloatLiteral(*node.value, field->second);
                    }
                    // The updated record, as both backends answer it.
                    return m_valueAssignments.contains(&expr) ? varType : Type::voidType();
                }
                if (varType && !std::holds_alternative<UnknownType>(varType->kind) &&
                    !std::holds_alternative<TypeVar>(varType->kind)) {
                    if (node.value) {
                        noteFloatLiteral(*node.value, varType);
                    }
                    if (!argMatchesParam(valueType, varType) &&
                        !(node.value && literalCoercesTo(*node.value, varType)) &&
                        !std::holds_alternative<UnknownType>(valueType->kind) &&
                        !std::holds_alternative<TypeVar>(valueType->kind)) {
                        typeMismatch(expr.location, varType, valueType);
                    } else {
                        // The variable's type is what an open value assigned to it
                        // is (HM).
                        unify(valueType, varType, /*bindRight=*/false);
                    }
                }
                // The value assigned, as both backends answer it: `parent.send(total =
                // total + i)` sends the Integer (kexhq/kex#614).
                if (!m_valueAssignments.contains(&expr)) {
                    return Type::voidType();
                }
                return varType && !std::holds_alternative<UnknownType>(varType->kind) ? varType
                                                                                      : valueType;
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                auto leftType = node.left ? inferExpr(*node.left) : Type::unknown();
                auto rightType = node.right ? inferExpr(*node.right) : Type::unknown();
                if (node.op == TokenType::EqEq || node.op == TokenType::NotEq) {
                    checkNeverEqual(resolve(leftType), resolve(rightType), expr.location);
                }
                // The left operand is what a make-block method answers before its
                // body is checked (m_pendingResults). The built-in rules would
                // type `price * 2` as the Integer on the right. So the operator is
                // applied once the body has answered, exactly as it would have
                // been: the type's own signature checks the right operand
                // (`x + anotherType` used to pass), the built-in rules a number,
                // and a type with neither is an error. Until then the expression
                // is itself a pending result, which its outcome answers, so an
                // annotation or a further operator on it is checked too.
                if (const auto* pending = std::get_if<TypeVar>(&resolve(leftType)->kind);
                    pending && m_pendingResultVars.count(pending->id) &&
                    (node.op == TokenType::Plus || node.op == TokenType::Minus ||
                     node.op == TokenType::Star || node.op == TokenType::Slash ||
                     node.op == TokenType::Percent || node.op == TokenType::Caret)) {
                    const std::string symbol(tokenTypeName(node.op));
                    const auto op = node.op;
                    const auto right = rightType;
                    const auto location = expr.location;
                    const auto result = freshTypeVar();
                    const int resultId = std::get<TypeVar>(result->kind).id;
                    m_pendingResultVars.insert(resultId);
                    m_pendingChecks[pending->id].push_back(
                        [this, symbol, op, right, location, resultId](const TypePtr& answered) {
                            const auto left = resolve(answered);
                            TypePtr outcome = Type::unknown();
                            if (traitDeclaresOperator(left, symbol)) {
                                // A trait default: no signature of its own to
                                // apply, but declared.
                            } else if (declaresOperatorFor(left, symbol)) {
                                outcome = checkCall(
                                    symbol, {left, right}, location,
                                    /*isMethodCall=*/true);
                            } else if (
                                !lacksBuiltinOperator(left, symbol) &&
                                !lacksBuiltinOperator(resolve(right), symbol)) {
                                outcome = inferBinaryOp(op, left, right, location);
                            } else {
                                error(
                                    lacksBuiltinOperator(left, symbol)
                                        ? diag::Code::OperatorNotDefinedDeclareInMake
                                        : diag::Code::OperatorNotDefined,
                                    location,
                                    {{"operator", symbol},
                                     {"left", typeToString(left)},
                                     {"right", typeToString(resolve(right))}});
                            }
                            answerPending(resultId, outcome);
                        });
                    return result;
                }
                const auto operatorName = [&]() -> std::string {
                    switch (node.op) {
                        case TokenType::Plus: return "+";
                        case TokenType::Minus: return "-";
                        case TokenType::Star: return "*";
                        case TokenType::Slash: return "/";
                        case TokenType::Percent: return "%";
                        case TokenType::Caret: return "^";
                        case TokenType::EqEq: return "==";
                        case TokenType::NotEq: return "!=";
                        case TokenType::LessThan: return "<";
                        case TokenType::GreaterThan: return ">";
                        case TokenType::LessEq: return "<=";
                        case TokenType::GreaterEq: return ">=";
                        case TokenType::AmpAmp: return "&&";
                        case TokenType::PipePipe: return "||";
                        default: return "";
                    }
                }();
                if (!operatorName.empty()) {
                    auto& visible = m_operatorImports[&expr];
                    visible[operatorName] = importedCandidates(operatorName, leftType);
                    if (node.op == TokenType::NotEq) {
                        visible["=="] = importedCandidates("==", leftType);
                    }
                }
                // Operators share make-method signatures with UFCS. Prefer a
                // matching receiver/RHS overload, but leave ordinary operators on
                // the builtin inference path when no local signature applies.
                if (!operatorName.empty()) {
                    // Numeric primitives always use the built-in operator. A
                    // trait-shaped unit overload such as Watt * Hour must not
                    // capture Float * Float merely because both types also occur
                    // in generic receiver tables loaded from the prelude.
                    // Resolved first: a parameter `limit > 3` has already bound to
                    // Integer is still its type variable here, which satisfies
                    // nothing, so `limit - 1` went on to the overloads and picked
                    // `Duration`'s (kexhq/kex#563).
                    if (m_traits.satisfies(resolve(leftType), "Number") &&
                        m_traits.satisfies(resolve(rightType), "Number")) {
                        return inferBinaryOp(node.op, leftType, rightType, expr.location);
                    }
                    auto receiver = resolve(leftType);
                    // A trait-bounded variable is still a variable. `n - 1`
                    // constrains `n` to `N: Number` before `n * f(...)` is
                    // checked, and treating that as a concrete receiver let it
                    // match any operator overload whose parameter a constrained
                    // type satisfies: `let fact(n) = n * fact(n - 1)` picked up
                    // `make Period`'s `*(Integer) -> Period` and inferred
                    // `fact : Integer -> Period`.
                    const bool concreteReceiver =
                        !std::holds_alternative<TypeVar>(receiver->kind) &&
                        !std::holds_alternative<UnknownType>(receiver->kind) &&
                        !std::holds_alternative<ConstrainedType>(receiver->kind);
                    // An operator defined in a make block is visible either as a
                    // local declaration or — for the prelude's own `Date + Duration`
                    // and friends — only through the imported interface. Consulting
                    // just the local table sent every prelude operator down the
                    // builtin numeric/string path below, which rejects any pair of
                    // differing types outright.
                    bool ownsOperator = false;
                    if (concreteReceiver) {
                        auto declaredFor = [&](const std::string& name) {
                            std::vector<Signature> signatures;
                            for (auto& overload : operatorOverloadsFor(
                                     leftType, name, importedCandidates(name, leftType))) {
                                signatures.push_back(std::move(overload.signature));
                            }
                            return signatures;
                        };
                        auto applies = [&](const Signature& signature) {
                            return argMatchesParam(rightType, signature.params[1]);
                        };
                        auto owned = declaredFor(operatorName);
                        ownsOperator = !owned.empty();
                        // The operands as written: a literal selects a clause written
                        // with it (`m ^ 2` takes `let ^(2)`).
                        // Only for a literal: the operands otherwise leave the
                        // report on the whole expression, where it has been.
                        const bool literalOperand =
                            node.right &&
                            (std::holds_alternative<ast::IntLiteral>(node.right->kind) ||
                             std::holds_alternative<ast::AtomLiteral>(node.right->kind));
                        const std::vector<const ast::Expr*> operands{
                            node.left.get(), node.right.get()};
                        if (std::any_of(owned.begin(), owned.end(), applies)) {
                            return checkCall(
                                operatorName, {leftType, rightType}, expr.location,
                                /*isMethodCall=*/true, nullptr, nullptr,
                                literalOperand ? &operands : nullptr);
                        }
                        // `!=` with no overload of its own negates an overloaded
                        // `==`, which is only possible when that `==` answers a
                        // Bool (kexhq/kex#486).
                        auto contractName = operatorName;
                        if (operatorName == "!=" && owned.empty()) {
                            owned = declaredFor("==");
                            contractName = "==";
                            if (std::any_of(owned.begin(), owned.end(), applies)) {
                                auto equal = resolve(checkCall(
                                    "==", {leftType, rightType}, expr.location,
                                    /*isMethodCall=*/true));
                                const bool undecided = isUndetermined(equal);
                                if (!undecided && !typesEqual(equal, Type::boolean())) {
                                    error(
                                        diag::Code::InequalityNotDerivable, expr.location,
                                        {{"type", typeToString(resolve(leftType))},
                                         {"result", typeToString(equal)}});
                                }
                                return Type::boolean();
                            }
                        }
                        // The receiver's type overloads this operator, but for a
                        // different argument: a call the overload's parameter
                        // refuses — `email == 5` against `==(value: String)`.
                        // Except that two values of the same type still compare
                        // structurally: `let ==(value: String) -> Cond` must not
                        // stop two Cols comparing. Only equality has a built-in
                        // answer for any type to fall back on; a record has no
                        // built-in order.
                        if (!owned.empty()) {
                            const bool equality = operatorName == "==" || operatorName == "!=";
                            const bool sameType = argMatchesParam(rightType, leftType) &&
                                                  argMatchesParam(leftType, rightType);
                            if (!(equality && sameType)) {
                                return checkCall(
                                    contractName, {leftType, rightType}, expr.location,
                                    /*isMethodCall=*/true);
                            }
                        }
                    }
                    // A record or ADT has no built-in arithmetic or order, and
                    // neither has a value known only by a trait. Without an
                    // operator of its own, `a - b` used to pass as the left
                    // operand's type and fail only at run time: "Cannot
                    // subtract" on the walker, a bare `function_clause` on
                    // BEAM. The same for `a < b`, and for `2 * a`, where the
                    // number on the left declares nothing for the record. A
                    // trait default (`let +(other: This)` in the trait) is
                    // declared for every implementor, but has no signature
                    // of its own for `owned` to find.
                    // One the receiver's trait requires answers what the trait
                    // says: `first + second` under `+ :> This -> Integer` is an
                    // Integer, not the trait (kexhq/kex#796).
                    if (const auto answered =
                            traitOperatorResult(receiver, operatorName, rightType, expr.location)) {
                        return answered;
                    }
                    const bool arithmeticOrOrder =
                        node.op == TokenType::Plus || node.op == TokenType::Minus ||
                        node.op == TokenType::Star || node.op == TokenType::Slash ||
                        node.op == TokenType::Percent || node.op == TokenType::Caret ||
                        node.op == TokenType::LessThan || node.op == TokenType::GreaterThan ||
                        node.op == TokenType::LessEq || node.op == TokenType::GreaterEq;
                    const auto right = resolve(rightType);
                    // A bound reached through a type variable was inferred, not
                    // declared: an unannotated `n` passed to `IO.inspect` is bound
                    // to `Inspectable`, but `n * 2` beside it still says what `n`
                    // is. Only an annotated `a: Showable` is known only by its trait.
                    const bool inferredBound =
                        std::holds_alternative<TypeVar>(leftType->kind) &&
                        std::holds_alternative<ConstrainedType>(receiver->kind);
                    const bool knownReceiver =
                        !std::holds_alternative<TypeVar>(receiver->kind) &&
                        !std::holds_alternative<UnknownType>(receiver->kind) && !inferredBound;
                    // A built-in `+` on the left already says when the right side
                    // does not match ("requires matching types"); the others do
                    // not check it, so `2 * point` needs saying here.
                    const bool builtinPlusReports =
                        node.op == TokenType::Plus && !lacksBuiltinOperator(receiver, operatorName);
                    if (arithmeticOrOrder && knownReceiver && !ownsOperator &&
                        !builtinPlusReports && !traitDeclaresOperator(receiver, operatorName) &&
                        (lacksBuiltinOperator(receiver, operatorName) ||
                         lacksBuiltinOperator(right, operatorName))) {
                        const auto* bound = std::get_if<ConstrainedType>(&receiver->kind);
                        const auto owner = bound ? bound->traitName : typeToString(receiver);
                        const bool traitOwner =
                            (bound || isTrait(owner)) && !primitiveNumberTrait(owner);
                        // Only a named type has a make block to declare it in, and
                        // only the left one is worth naming: in `3 + value` with a
                        // `Result` on the right, the fix is the `Result`, not a
                        // `make Integer`.
                        const bool nominal =
                            lacksBuiltinOperator(receiver, operatorName) &&
                            (bound || std::holds_alternative<NamedType>(receiver->kind));
                        error(
                            !nominal     ? diag::Code::OperatorNotDefined
                            : traitOwner ? diag::Code::OperatorNotDefinedDeclareInTrait
                                         : diag::Code::OperatorNotDefinedDeclareInMake,
                            expr.location,
                            {{"operator", operatorName},
                             {"left", owner},
                             {"right", typeToString(right)}});
                        return Type::unknown();
                    }
                }
                return inferBinaryOp(node.op, leftType, rightType, expr.location);
            } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                auto operandType = node.operand ? inferExpr(*node.operand) : Type::unknown();
                auto resolved = resolve(operandType);
                if (node.op == TokenType::Bang) {
                    if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                        unifyVar(tv->id, Type::boolean());
                    } else if (
                        !std::holds_alternative<UnknownType>(resolved->kind) &&
                        !typesEqual(resolved, Type::boolean())) {
                        error(
                            diag::Code::LogicalNotRequiresBool, expr.location,
                            {{"type", typeToString(resolved)}});
                    }
                    return Type::boolean();
                }
                if (node.op == TokenType::Minus) {
                    // A type's own unary `-`: one taking only the receiver
                    // (`let -()`, `- : This -> This`), as `-(other)` is the
                    // binary one (kexhq/kex#798).
                    if (auto own = unaryOperatorResult(expr, resolved)) {
                        return own;
                    }
                    // Unary negation requires a numeric operand.
                    if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                        unifyVar(tv->id, Type::constrained("N", "Number"));
                    } else if (
                        lacksBuiltinOperator(resolved, "-") ||
                        (std::holds_alternative<PrimitiveType>(resolved->kind) &&
                         !isNumericType(resolved))) {
                        // Negation is numbers only, on both backends: a record or ADT
                        // reached "Cannot negate" on the walker and `badarith` on
                        // BEAM, with nothing said while checking, and so did a
                        // String, Bool or Atom (`-"5"`, kexhq/kex#769).
                        error(
                            diag::Code::NegateRequiresNumber, expr.location,
                            {{"type", typeToString(resolved)}});

                        return Type::unknown();
                    }
                    return resolved;
                }
                return Type::unknown();
            } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                // A record is built with braces, not called: `Point(x: 1, y: 2)`
                // was accepted with any arguments, and neither backend runs it.
                if (!node.reifiedConstructor && recordCalledLikeFunction(node.name)) {
                    for (const auto& arg : node.args) {
                        if (arg) {
                            inferExpr(*arg);
                        }
                    }
                    for (const auto& [_, arg] : node.namedArgs) {
                        if (arg) {
                            inferExpr(*arg);
                        }
                    }
                    error(
                        diag::Code::RecordCalledLikeFunction, expr.location, {{"name", node.name}});
                    return Type::unknown();
                }
                std::vector<TypePtr> argTypes;
                // First pass: infer concrete args; defer lambdas with parameters
                // until the selected signature can provide contextual types.
                std::vector<std::pair<size_t, size_t>> contextualLambdas;
                for (size_t i = 0; i < node.args.size(); i++) {
                    const auto* lambda =
                        node.args[i] ? std::get_if<ast::Lambda>(&node.args[i]->kind) : nullptr;
                    if (node.args[i] &&
                        (std::holds_alternative<ast::ShorthandLambda>(node.args[i]->kind) ||
                         isBareCapture(node.args[i].get()) ||
                         (lambda && !lambda->params.empty()))) {
                        argTypes.push_back(Type::unknown()); // placeholder
                        contextualLambdas.emplace_back(i, i);
                    } else {
                        argTypes.push_back(
                            node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                    }
                }
                // A block passed by label is typed from that parameter too, as a
                // positional one is below. Inferred on its own, its parameter had
                // no type, so `t.text(:c)` was left to run-time dispatch, which
                // could not find `text` (kexhq/kex#499).
                std::vector<size_t> labelledLambdas;
                for (size_t i = 0; i < node.namedArgs.size(); ++i) {
                    const auto& arg = node.namedArgs[i].second;
                    const auto* lambda = arg ? std::get_if<ast::Lambda>(&arg->kind) : nullptr;
                    // A collecting block moved here from the trailing position
                    // (passBlockByLabel) is typed as a block, or its body would
                    // read as one value rather than a list on the next check.
                    if (lambda && (!lambda->params.empty() || lambda->collection)) {
                        argTypes.push_back(Type::unknown()); // placeholder
                        labelledLambdas.push_back(i);
                    } else {
                        argTypes.push_back(arg ? inferExpr(*arg) : Type::unknown());
                    }
                }
                // Second pass: infer lambdas with parameter hints from the signature.
                for (const auto& [argIdx, rawIdx] : contextualLambdas) {
                    auto hints = resolveArgHints(node.name, argTypes, argIdx);
                    argTypes[argIdx] = inferBlock(*node.args[rawIdx], hints);
                }
                for (const auto index : labelledLambdas) {
                    const auto& [label, arg] = node.namedArgs[index];
                    argTypes[node.args.size() + index] =
                        inferBlock(*arg, namedArgHints(node.name, label));
                }
                if (node.block) {
                    bool collection = false;
                    std::string skippedTo;
                    const auto* written = std::get_if<ast::Lambda>(&(**node.block).kind);
                    auto hints = resolveBlockHints(
                        node.name, argTypes, false, &collection, &skippedTo,
                        written ? std::optional(written->params.size()) : std::nullopt);
                    if (collection) {
                        if (auto* lambda = std::get_if<ast::Lambda>(&(**node.block).kind)) {
                            lambda->collection = true;
                        }
                    }
                    argTypes.push_back(inferBlock(**node.block, hints));
                    passBlockByLabel(node, skippedTo);
                }
                // Declared by a merged dependency's file outside its module:
                // that file's own name, undefined here as it is where the file
                // is compiled apart.
                if (reportOtherFilesTopLevel("function", node.name, expr.location)) {
                    return Type::unknown();
                }
                if (auto constructed = constructorResultType(node.name, argTypes)) {
                    checkConstructorPayloads(node.name, argTypes, expr.location);
                    const auto* adt = std::get_if<NamedType>(&resolve(constructed)->kind);
                    m_decidedCalls[&expr] = {CallKind::Constructor, adt ? adt->name : node.name};
                    // A module's constructor is its function, `FS.ReadFailed`.
                    if (adt) {
                        if (const auto dot = adt->name.rfind('.'); dot != std::string::npos) {
                            m_calledFunctions[&expr] = adt->name.substr(0, dot) + "." + node.name;
                        }
                    }
                    return constructed;
                }
                // An imported constructor a `using` brings in (`ReadFailed(path)`
                // under `using FS`): checked as its module's own spelling is.
                if (m_importedInterfaces && !lookupVar(node.name) && !node.name.empty() &&
                    kex::startsUppercase(node.name)) {
                    for (const auto& [path, module] : m_importedInterfaces->modules) {
                        const auto exports = module.exports.find(node.name);
                        if (exports == module.exports.end()) {
                            continue;
                        }
                        const bool visible = std::any_of(
                            exports->second.begin(), exports->second.end(),
                            [&](const ImportedFunction& function) {
                                return function.isConstructor &&
                                       function.signature.params.size() == argTypes.size() &&
                                       importedFunctionVisible(function);
                            });
                        if (!visible) {
                            continue;
                        }
                        auto result = checkCall(
                            kex::mangle::qualify(path, node.name), argTypes, expr.location, false,
                            nullptr, &expr);
                        const auto* adt = std::get_if<NamedType>(&resolve(result)->kind);
                        m_calledFunctions[&expr] = path + "." + node.name;
                        m_decidedCalls[&expr] = {
                            CallKind::Constructor, adt ? adt->name : node.name};
                        return result;
                    }
                }
                // A module's constructor named bare where no `using` brings it
                // in (`ReadFailed(path)` outside `FS`): written `FS.ReadFailed`.
                if (!lookupVar(node.name) && !constructorInfo(node.name, {})) {
                    if (const auto declaring = moduleOfUnreachableConstructor(node.name);
                        !declaring.empty()) {
                        reportUnreachableConstructor(node.name, declaring, expr.location);
                        return Type::unknown();
                    }
                }
                if (auto method = bareMethodCall(expr, node, argTypes)) {
                    return method;
                }
                return checkCall(node.name, argTypes, expr.location, false, nullptr, &expr);
            } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
                for (const auto& value : node.values) {
                    if (value) {
                        inferExpr(*value);
                    }
                }
                return checkCall(
                    node.tag, {Type::list(Type::string()), Type::list(Type::unknown())},
                    expr.location, false, nullptr, &expr);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                // `A.identity` with `A is Monoid` (§12 step 7): a call through a
                // bounded type variable, answered by the bound trait's requirement
                // at that type. No value carries A, so the type itself is passed
                // to the function and the call dispatches on it.
                if (const auto* letter =
                        node.receiver ? std::get_if<ast::UpperIdentifier>(&node.receiver->kind)
                                      : nullptr) {
                    if (const auto bound = m_bodyTypeVars.find(letter->name);
                        bound != m_bodyTypeVars.end()) {
                        if (auto answered = boundTypeCall(bound->second, node)) {
                            m_typeVariableReceivers[&expr] = letter->name;
                            answerThroughTypeDispatcher(expr, node);
                            for (const auto& arg : node.args) {
                                if (arg) {
                                    inferExpr(*arg);
                                }
                            }
                            return answered;
                        }
                    }
                    if (auto answered = namedConstructorCall(letter->name, expr, node)) {
                        return answered;
                    }
                }
                // `x.push!(v)` rebinds `x`, so `x` has to be a `var`. A receiver
                // that is no name at all (`[3, 1].sort!`, `xs.map(f).sort!`) has
                // nothing to rebind: both backends failed it at run time with
                // "'!' requires a variable binding" (kexhq/kex#770).
                if (const auto* ident = node.mutating && node.receiver
                                            ? std::get_if<ast::Identifier>(&node.receiver->kind)
                                            : nullptr) {
                    if (const auto* binding = bindingOf(ident->name); binding && !binding->isVar) {
                        error(diag::Code::BangOnImmutable, expr.location, {{"name", ident->name}});
                    }
                } else if (node.mutating && node.receiver) {
                    error(diag::Code::BangOnNonVariable, expr.location, {{"name", node.method}});
                }
                // Local module constructors use the same namespace syntax as
                // module constants. Imported constructors are ordinary typed
                // module exports and continue through checkCall below.
                if (node.args.empty() && node.namedArgs.empty() && !node.block && node.receiver) {
                    std::function<std::optional<std::string>(const ast::Expr&)> localModulePath;
                    localModulePath = [&](const ast::Expr& receiver) -> std::optional<std::string> {
                        if (const auto* root = std::get_if<ast::UpperIdentifier>(&receiver.kind)) {
                            return root->name;
                        }
                        const auto* segment = std::get_if<ast::MethodCall>(&receiver.kind);
                        if (!segment || !segment->receiver || !segment->args.empty() ||
                            !segment->namedArgs.empty() || segment->block) {
                            return std::nullopt;
                        }
                        auto parent = localModulePath(*segment->receiver);
                        return parent ? std::optional<std::string>{*parent + "." + segment->method}
                                      : std::nullopt;
                    };
                    if (auto path = localModulePath(*node.receiver)) {
                        if (auto module = m_moduleConstructors.find(*path);
                            module != m_moduleConstructors.end()) {
                            if (auto constructor = module->second.find(node.method);
                                constructor != module->second.end() &&
                                // The program's own modules: an imported one's
                                // constructor is its module's exported function.
                                !constructor->second.imported) {
                                if (!constructor->second.isPublic && m_currentModulePath != *path) {
                                    error(
                                        diag::Code::PrivateNameAccess, expr.location,
                                        {{"name", node.method}, {"module", *path}});
                                }
                                m_decidedCalls[&expr] = {
                                    CallKind::Constructor, constructor->second.typeName.empty()
                                                               ? node.method
                                                               : constructor->second.typeName};
                                // A constructor reached through its full module path
                                // retains its defining ADT even when the current file
                                // did not import that ADT's bare name. This matters
                                // for reified constants containing nested records.
                                // Spelled out, a nullary constructor is its ADT's variant,
                                // the type its bare name has where it is in scope.
                                if (constructor->second.arity == 0 &&
                                    !constructor->second.typeName.empty()) {
                                    return Type::variantOf(
                                        constructor->second.typeName, node.method);
                                }
                                if (!moduleMemberImported(*path, node.method) &&
                                    !withinModule(*path) && !constructor->second.typeName.empty()) {
                                    return Type::named(constructor->second.typeName);
                                }
                                return Type::named(node.method);
                            }
                        }
                        // A module reached through an alias or its last segment
                        // (`Roles.Admin` under `using Grants.Roles`, where `Roles`
                        // also names a record of that module), this unit's or an
                        // imported one, names its constructor the same way.
                        const auto resolved = [&]() -> std::string {
                            if (auto module = resolveModulePath(*path, node.method);
                                module != *path) {
                                return module;
                            }
                            // A `using` names a module by its alias, or by its last
                            // segments (`Grants.Roles` as `Roles`).
                            const auto answers = [&](const ImportSelection& import) {
                                return import.alias ? *import.alias == *path
                                                    : import.module.ends_with("." + *path);
                            };
                            for (auto scope = m_importScopeStack.rbegin();
                                 scope != m_importScopeStack.rend(); ++scope) {
                                for (const auto& import : *scope) {
                                    if (answers(import)) {
                                        return import.module;
                                    }
                                }
                            }
                            for (const auto& import : m_declarationImports) {
                                if (answers(import)) {
                                    return import.module;
                                }
                            }
                            return *path;
                        }();
                        if (resolved != *path) {
                            if (auto module = m_moduleConstructors.find(resolved);
                                module != m_moduleConstructors.end()) {
                                if (auto constructor = module->second.find(node.method);
                                    constructor != module->second.end()) {
                                    const auto& typeName = constructor->second.typeName.empty()
                                                               ? node.method
                                                               : constructor->second.typeName;
                                    m_decidedCalls[&expr] = {CallKind::Constructor, typeName};
                                    if (constructor->second.arity == 0 &&
                                        !constructor->second.typeName.empty()) {
                                        return Type::variantOf(typeName, node.method);
                                    }
                                    return Type::named(typeName);
                                }
                            }
                            if (m_importedInterfaces) {
                                if (auto module = m_importedInterfaces->modules.find(resolved);
                                    module != m_importedInterfaces->modules.end()) {
                                    if (auto exported = module->second.exports.find(node.method);
                                        exported != module->second.exports.end()) {
                                        for (const auto& function : exported->second) {
                                            if (function.isConstructor &&
                                                function.signature.params.empty()) {
                                                const auto* adt = std::get_if<NamedType>(
                                                    &resolve(function.signature.result)->kind);
                                                m_decidedCalls[&expr] = {
                                                    CallKind::Constructor,
                                                    adt ? adt->name : node.method};
                                                return variantType(
                                                    function.signature.result, node.method);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Backend interop and the private intrinsic ABI are untyped until
                // the intrinsic declaration interface supplies their signatures.
                // Never resolve these against same-named public stdlib functions.
                {
                    const ast::Expr* r = node.receiver ? &*node.receiver : nullptr;
                    // The path's segments, innermost first (`lists` of
                    // `BEAM.lists.reverse`).
                    std::vector<const ast::Expr*> segments;
                    while (r) {
                        if (auto* mc = std::get_if<ast::MethodCall>(&r->kind)) {
                            // A segment with arguments is a call, and the path ends
                            // there: `.map` on `Kex.Intrinsic.Type.fieldLayoutsOf(n)`
                            // is a call on its result, not a path member.
                            if (!mc->args.empty() || !mc->namedArgs.empty() || mc->block) {
                                break;
                            }
                            segments.push_back(r);
                            r = mc->receiver ? &*mc->receiver : nullptr;
                        } else {
                            break;
                        }
                    }
                    if (r) {
                        if (auto* uid = std::get_if<ast::UpperIdentifier>(&r->kind)) {
                            // The interop roots before `BEAM.` (kexhq/kex#346).
                            // Unchecked, a leftover `Erlang.lists.reverse` passed
                            // the checker and died at runtime instead.
                            const bool retiredRoot = (uid->name == kex::names::kErlangModule ||
                                                      uid->name == kex::names::kElixirModule ||
                                                      uid->name == kex::names::kGleamModule) &&
                                                     !m_localModules.count(uid->name) &&
                                                     !m_importedModulePaths.count(uid->name);
                            if (retiredRoot) {
                                error(
                                    uid->name == kex::names::kElixirModule
                                        ? diag::Code::RetiredElixirInterop
                                        : diag::Code::RetiredInteropRoot,
                                    r->location, {{"root", uid->name}});
                            }
                            // Decided by its path: an Erlang function, or a runtime
                            // primitive; each segment names a module.
                            std::string path = uid->name;
                            std::vector<std::pair<const ast::Expr*, std::string>> segmentPaths;
                            for (auto segment = segments.rbegin(); segment != segments.rend();
                                 ++segment) {
                                path += "." + std::get<ast::MethodCall>((*segment)->kind).method;
                                segmentPaths.emplace_back(*segment, path);
                            }
                            const bool intrinsic = names::isIntrinsicPath(path + "." + node.method);
                            if (uid->name == names::kForeignRoot || retiredRoot || intrinsic) {
                                for (const auto& a : node.args) {
                                    if (a) {
                                        inferExpr(*a);
                                    }
                                }
                                for (const auto& [_, a] : node.namedArgs) {
                                    if (a) {
                                        inferExpr(*a);
                                    }
                                }
                                if (node.block) {
                                    inferExpr(**node.block);
                                }
                                for (const auto& [segment, segmentPath] : segmentPaths) {
                                    m_decidedCalls[segment] = {
                                        CallKind::ModuleReference, segmentPath};
                                }
                                m_decidedCalls[&expr] = {
                                    intrinsic ? CallKind::Builtin : CallKind::Foreign,
                                    path + "." + node.method};
                                return Type::unknown();
                            }
                        }
                    }
                }
                // Namespace call: `Integer.parse(s)` or `Web.Response.text(s)`.
                // A chain made entirely of uppercase segments is a qualified
                // namespace, not a UFCS receiver value, so don't include it as
                // argTypes[0].
                std::string callName = node.method;
                // Only a direct uppercase receiver is unambiguously a namespace.
                // For a qualified chain (`Web.Response.text`) infer `Web.Response`
                // as an Unknown receiver and let checkCall's existing namespace
                // heuristic drop it when the target arity requires that. This also
                // preserves value chains through static constructors/constants,
                // such as `Temperature.Fahrenheit(212).to(String)` and
                // `Temperature.Freezing.to(String)`.
                auto isNamespaceReceiver = [](const ast::Expr& receiver) {
                    return std::holds_alternative<ast::UpperIdentifier>(receiver.kind);
                };
                const auto importedModulePath = [](const ast::Expr& receiver) {
                    return ast::writtenModulePath(receiver);
                };
                auto importedPath =
                    node.receiver ? importedModulePath(*node.receiver) : std::nullopt;
                // The path exactly as written, before a `using` expands it.
                const auto writtenPath = importedPath;
                // A bare receiver segment (`Set.from(…)`) may name a module only
                // under its qualified identity (`Data.Set`) once a `using`
                // brought it into scope unqualified — expand it before the
                // dependency/namespace checks below, which key everything off
                // `*importedPath` (kexhq/kex#229).
                // Whether the module was found by its name alone (below): no other
                // means reaches the function then, so a missing one is an error.
                bool broughtByName = false;
                if (importedPath) {
                    const auto written = *importedPath;
                    importedPath = resolveModulePath(written, node.method);
                    // A name a member selects no module for (`URL.pa`, half typed,
                    // under `using URI`) is still the module a `using` brings in
                    // under it, when exactly one does (`URI.URL`): completion and
                    // navigation ask which module that is.
                    // Not when the name is that module itself: under `using
                    // Complex`, `Complex.from` is the module's `from`, not a
                    // member of `Complex.Complex` (the record's `make` block),
                    // and was left undecided, so typed as anything.
                    const auto namesImportedModule = [&] {
                        const auto exportsMethod = [&](const std::string& module) {
                            const auto found = m_importedInterfaces->modules.find(module);
                            return found != m_importedInterfaces->modules.end() &&
                                   found->second.exports.contains(node.method);
                        };
                        if (exportsMethod(written) || exportsMethod(names::moduleAtom(written))) {
                            return true;
                        }
                        const auto names = [&](const ImportSelection& import) {
                            return !import.alias && import.module == written;
                        };
                        return std::any_of(
                                   m_declarationImports.begin(), m_declarationImports.end(),
                                   names) ||
                               std::any_of(
                                   m_importScopeStack.begin(), m_importScopeStack.end(),
                                   [&](const auto& scope) {
                                       return std::any_of(scope.begin(), scope.end(), names);
                                   });
                    };
                    if (*importedPath == written && m_importedInterfaces &&
                        !m_localModules.contains(written) && !namesImportedModule()) {
                        std::set<std::string> brought;
                        const auto consider = [&](const ImportSelection& import) {
                            const auto root = written.substr(0, written.find('.'));
                            if (!import.alias &&
                                ((!import.onlyNames.empty() &&
                                  std::find(
                                      import.onlyNames.begin(), import.onlyNames.end(), root) ==
                                      import.onlyNames.end()) ||
                                 std::find(
                                     import.exceptNames.begin(), import.exceptNames.end(), root) !=
                                     import.exceptNames.end())) {
                                return;
                            }
                            const auto child =
                                import.alias
                                    ? (*import.alias == written ? import.module : std::string())
                                    : import.module + "." + written;
                            if (const auto found = m_importedInterfaces->modules.find(child);
                                !child.empty() && found != m_importedInterfaces->modules.end() &&
                                !found->second.exports.empty()) {
                                brought.insert(child);
                            }
                        };
                        for (const auto& import : m_declarationImports) {
                            consider(import);
                        }
                        for (const auto& scope : m_importScopeStack) {
                            for (const auto& import : scope) {
                                consider(import);
                            }
                        }
                        if (brought.size() == 1) {
                            importedPath = *brought.begin();
                            broughtByName = true;
                        }
                    }
                }
                // And as a `using` expands it, before a re-export renames it.
                const auto expandedPath = importedPath;
                // A module re-exported under another's path (`export Numbers` in
                // `module Toolkit`) is that module: `Toolkit.Numbers.double`.
                if (importedPath) {
                    if (const auto reexport = m_moduleReexports.find(*importedPath);
                        reexport != m_moduleReexports.end()) {
                        importedPath = reexport->second;
                    }
                }
                // Each segment of that path, as written, that names a module this
                // unit knows: a local one, an imported one, or one the compiled
                // interfaces describe. Anything else there is a constant or a
                // field (`Credentials.SHORTEST`, `Kex.VERSION.number`).
                const auto namesModule = [&](const std::string& path) {
                    return m_localModules.contains(path) || m_moduleReexports.contains(path) ||
                           m_importedModulePaths.contains(path) ||
                           (m_importedInterfaces &&
                            (m_importedInterfaces->modules.contains(path) ||
                             m_importedInterfaces->modules.contains(names::moduleAtom(path))));
                };
                // A record or sum type used as a namespace (`TCP.Endpoint.loopback`).
                const auto namesType = [&](const std::string& path) {
                    const auto canonical = resolveRecordName(path);
                    return m_recordFields.contains(canonical) || m_adtVariants.contains(path) ||
                           (m_importedInterfaces &&
                            (m_importedInterfaces->recordFieldNames.contains(path) ||
                             m_importedInterfaces->recordFieldNames.contains(canonical)));
                };
                if (writtenPath) {
                    auto path = *writtenPath;
                    // The same path as `using` expanded it (`URI.URL` for `URL`),
                    // while its tail is still the written one.
                    auto expanded =
                        expandedPath && expandedPath->ends_with(writtenPath->substr(
                                            std::min(writtenPath->find('.'), writtenPath->size())))
                            ? *expandedPath
                            : *writtenPath;
                    for (const ast::Expr* segment = node.receiver.get(); segment;) {
                        const auto* call = std::get_if<ast::MethodCall>(&segment->kind);
                        if (!call) {
                            // The first segment, a bare name (`Geo` in
                            // `Geo.area(2)`): the module it names is what the
                            // name index attaches the call to.
                            if (std::holds_alternative<ast::UpperIdentifier>(segment->kind) &&
                                namesModule(expanded)) {
                                m_decidedCalls[segment] = {CallKind::ModuleReference, expanded};
                            }
                            break;
                        }
                        if (namesModule(path)) {
                            m_decidedCalls[segment] = {CallKind::ModuleReference, path};
                        } else if (namesType(path)) {
                            m_decidedCalls[segment] = {CallKind::TypeReference, path};
                        }
                        const auto dot = path.rfind('.');
                        path.resize(dot == std::string::npos ? 0 : dot);
                        const auto expandedDot = expanded.rfind('.');
                        expanded.resize(expandedDot == std::string::npos ? 0 : expandedDot);
                        segment = call->receiver.get();
                    }
                }
                // A qualified type named as a value (`App.Models.User`, the argument
                // of `select(as: Type<T>)`) is a `Type<X>`, as a bare one is.
                if (writtenPath && node.args.empty() && node.namedArgs.empty() && !node.block &&
                    !node.method.empty() && kex::startsUppercase(node.method)) {
                    if (const auto full = *writtenPath + "." + node.method; namesType(full)) {
                        m_decidedCalls[&expr] = {CallKind::TypeReference, full};
                        return Type::named(
                            kex::names::kTypeRecord, {Type::named(resolveRecordName(full))});
                    }
                }
                // Keep the syntactic qualified module path even when it is not in
                // the prebuilt interface registry. Source modules are discovered
                // from these references before their interfaces exist (for
                // example, `Tey.Git.execute()` must cause tey/git.kex to load).
                // The module resolver later discards paths that do not map to a
                // source module, so ordinary static/type namespaces remain safe.
                if (importedPath) {
                    auto dependencyPath = *importedPath;
                    // A receiver chain can continue past the module into a
                    // constant or record field (`Kex.VERSION.number`). If
                    // an interface identifies a module prefix, record that prefix
                    // rather than asking source discovery to interpret VERSION as
                    // another module segment and recompiling the prelude source.
                    if (m_importedInterfaces) {
                        auto candidate = dependencyPath;
                        std::string interfaceModule;
                        while (!candidate.empty()) {
                            if (m_importedInterfaces->modules.count(candidate)) {
                                interfaceModule = candidate;
                                break;
                            }
                            // KexI source-module identities currently retain the
                            // backend's leading `Kex.` (logical Kex.Interface is
                            // stored as Kex.Kex.Interface). Accept that identity at
                            // this boundary so an automatic prelude module is
                            // still recognized as already compiled.
                            const auto backendPrefixed = names::moduleAtom(candidate);
                            if (m_importedInterfaces->modules.count(backendPrefixed)) {
                                interfaceModule = backendPrefixed;
                                break;
                            }
                            const auto dot = candidate.rfind('.');
                            if (dot == std::string::npos) {
                                break;
                            }
                            candidate.resize(dot);
                        }
                        if (!interfaceModule.empty()) {
                            dependencyPath = std::move(interfaceModule);
                        }
                    }
                    // A capitalized segment may be a constant rather than a
                    // module: `"${App.Forms.Credentials.SHORTEST}"`, whose `show`
                    // has the whole path as its receiver.
                    const auto last = dependencyPath.substr(dependencyPath.rfind('.') + 1);
                    if (std::none_of(last.begin(), last.end(), [](unsigned char c) {
                            return std::islower(c);
                        })) {
                        recordWithOwner(m_referencedModules, dependencyPath);
                    } else {
                        m_referencedModules.insert(std::move(dependencyPath));
                    }
                }
                bool isImportedNamespace = importedPath && m_importedInterfaces &&
                                           m_importedInterfaces->modules.count(*importedPath) > 0;
                bool isNamespaceCall =
                    node.receiver && (isNamespaceReceiver(*node.receiver) || isImportedNamespace);
                // A LOCAL nested module is qualified the same way: without this
                // `CollisionWeb.Server.build(7000)` checked as the bare name
                // `build` and picked the prelude's `Web.Server.build` — the two
                // are indistinguishable there, both `Integer -> Server`, and the
                // imported one is listed first. Only when signatures really were
                // published under that key, so value chains through a static
                // constructor (`Temperature.Fahrenheit(212)`) stay untouched.
                const bool isLocalNamespace =
                    importedPath && m_localModules.contains(*importedPath) &&
                    m_userSignatures.count(kex::mangle::qualify(*importedPath, node.method)) > 0;
                // ...and, being a namespace, its receiver is not an argument. A
                // dotted local module (`Tey.Commands`) is not a bare
                // UpperIdentifier, so `isNamespaceReceiver` alone missed it and the
                // receiver was checked as argument 1: every argument after it was
                // compared one parameter to the right. That was invisible while
                // the extra argument simply failed every arity (and an Unknown
                // receiver waved the call through unchecked), until a defaulted
                // parameter made the shifted count fit (kexhq/kex#408).
                isNamespaceCall = isNamespaceCall || isLocalNamespace;
                // A namespace path that names nothing, or a local module without
                // the member called. The resolve pass leaves uppercase receivers
                // to this checker, which only ever checked modules with a compiled
                // interface — so a call into a module that no longer exists
                // (`Web.Server.build`, removed by the network rewrite) checked
                // cleanly and died at run time (kexhq/kex#408).
                if (writtenPath && importedPath) {
                    if (auto problem =
                            namespaceCallProblem(*writtenPath, *importedPath, node.method)) {
                        error(problem->first, node.receiver->location, problem->second);
                        for (const auto& a : node.args) {
                            if (a) {
                                inferExpr(*a);
                            }
                        }
                        for (const auto& [_, a] : node.namedArgs) {
                            if (a) {
                                inferExpr(*a);
                            }
                        }
                        if (node.block) {
                            inferExpr(**node.block);
                        }
                        return Type::unknown();
                    }
                }
                // A module with a compiled interface lists everything it exports,
                // so a name it does not export is a mistake the checker can name —
                // left alone, `Process.whereis` after the rename to `whereIs`
                // checked cleanly and failed only at run time, as an undefined
                // method of the unrelated `Feature.Process`.
                //
                // Only where the call names that module unambiguously: written in
                // full (`Process.whereis`), not reached through a `using` that
                // expanded it (`Router.build` for `Net.HTTP.Router`, whose members
                // are found by other means), and with no module of the program
                // that the same last segment could mean instead.
                const auto unambiguousImport = [&] {
                    if (!writtenPath || (*writtenPath != *importedPath && !broughtByName)) {
                        return false;
                    }
                    const auto dot = writtenPath->rfind('.');
                    const auto last =
                        dot == std::string::npos ? *writtenPath : writtenPath->substr(dot + 1);
                    for (const auto& local : m_localModules) {
                        if (local == last || local.ends_with("." + last)) {
                            return false;
                        }
                    }
                    return true;
                };
                // A function of the `make` block for the type the imported module
                // is named like (`make Team do let handle?(text)` in
                // `App.Models.Team`, called `Team.handle?(text)` from a unit
                // compiled apart), which the interface files with the receiver
                // functions, not the exports: its receiver type, or null. Only
                // that type's: the module's `make` for another type is that
                // type's, not reached through the module.
                const auto ownMakeReceiver = [&](const std::string& name) -> TypePtr {
                    const auto functions = m_importedInterfaces->receiverFunctions.find(name);
                    if (functions == m_importedInterfaces->receiverFunctions.end()) {
                        return nullptr;
                    }
                    const auto own =
                        kex::mangle::qualify(*importedPath, kex::mangle::memberOf(*importedPath));
                    for (const auto& function : functions->second) {
                        if (function.sourceModule != *importedPath ||
                            function.signature.params.empty()) {
                            continue;
                        }
                        const auto& receiver = function.signature.params.front();
                        const auto* named = std::get_if<NamedType>(&resolve(receiver)->kind);
                        if (named &&
                            (named->name == own || resolveRecordName(named->name) == own)) {
                            return receiver;
                        }
                    }
                    return nullptr;
                };
                if (isImportedNamespace && !isLocalNamespace && unambiguousImport()) {
                    const auto& module = m_importedInterfaces->modules.at(*importedPath);
                    // Only a module's OWN interface is complete. A `using` also
                    // files a partial copy under the bare name (`Router` for
                    // `Net.HTTP.Router`, which lacks the module's constants), and
                    // that copy cannot say what the module does not have.
                    // `Kex.embed` is expanded before checking (compiled/expand.cxx);
                    // one still here failed to expand, and says why itself.
                    const bool compileTimeForm = *importedPath == names::kRuntimeRoot &&
                                                 node.method == names::kEmbedFunction;
                    // A module named like a type (`Map`, beside `type Map<K, V>`)
                    // also answers that type's static functions (`Map.get(m, k)`),
                    // which its interface does not list: a function of the name
                    // whose receiver is that type. A function of the name on any
                    // other type is not it: `Culture.named` checked clean on
                    // `Type.named`, and was undefined on both backends.
                    // The type is the module's by its full name, the module's
                    // path or a type declared inside it: a same-named type of
                    // another module (`Billing.User` for `Accounts.User`) is not
                    // it. Not by the function's module: `Map.get` is declared
                    // outside `module Map`.
                    const auto typeAnswers = [&](const std::string& name) {
                        const auto inner = kex::mangle::qualify(
                            *importedPath, kex::mangle::memberOf(*importedPath));
                        const auto headOf = [&](const TypePtr& written) -> std::string {
                            const auto type = resolve(written);
                            if (const auto* named = std::get_if<NamedType>(&type->kind)) {
                                return resolveRecordName(named->name);
                            }
                            if (std::holds_alternative<MapType>(type->kind)) {
                                return "Map";
                            }
                            if (std::holds_alternative<ListType>(type->kind)) {
                                return "List";
                            }
                            return typeToString(type);
                        };
                        const auto takes = [&](const Signature& signature) {
                            if (signature.params.empty()) {
                                return false;
                            }
                            const auto head = headOf(signature.params.front());
                            return head == *importedPath || head == inner;
                        };
                        if (const auto own = m_methodSignatures.find(name);
                            own != m_methodSignatures.end() &&
                            std::any_of(own->second.begin(), own->second.end(), takes)) {
                            return true;
                        }
                        const auto imported = m_importedInterfaces->receiverFunctions.find(name);
                        return imported != m_importedInterfaces->receiverFunctions.end() &&
                               std::any_of(
                                   imported->second.begin(), imported->second.end(),
                                   [&](const ImportedFunction& function) {
                                       return takes(function.signature);
                                   });
                    };
                    if (module.sourceModule == *importedPath && !compileTimeForm &&
                        (!moduleNamesType(*importedPath) ||
                         !(typeAnswers(node.method) || ownMakeReceiver(node.method))) &&
                        !module.exports.count(node.method)) {
                        std::vector<std::string> exported;
                        for (const auto& [name, _] : module.exports) {
                            exported.push_back(name);
                        }
                        std::sort(exported.begin(), exported.end());
                        if (auto hint = closestName(node.method, exported); !hint.empty()) {
                            error(
                                diag::Code::ModuleHasNoFunctionDidYouMean, expr.location,
                                {{"module", *importedPath}, {"name", node.method}, {"hint", hint}});
                        } else {
                            error(
                                diag::Code::ModuleHasNoFunction, expr.location,
                                {{"module", *importedPath}, {"name", node.method}});
                        }
                        for (const auto& a : node.args) {
                            if (a) {
                                inferExpr(*a);
                            }
                        }
                        for (const auto& [_, a] : node.namedArgs) {
                            if (a) {
                                inferExpr(*a);
                            }
                        }
                        if (node.block) {
                            inferExpr(**node.block);
                        }
                        return Type::unknown();
                    }
                }
                // A type's `make` method called through the type's name
                // (`Parser.parse(text)`) takes the type as its receiver, as both
                // backends run it: an ordinary method call, not a namespace one.
                bool throughTypeName = false;
                TypePtr aliasedType;
                // The same through an alias read as the module (`Team.handle?(text)`
                // under `using App.Models.Team, as: Team`): a module named like its
                // type, the function its `make` block's, not an export.
                // The alias names the module, so its type: never a constructor
                // the same name also reaches (`Command` of another module's
                // `EventKind`).
                if (isNamespaceCall && isImportedNamespace && !isLocalNamespace &&
                    std::holds_alternative<ast::UpperIdentifier>(node.receiver->kind) &&
                    moduleNamesType(*importedPath) &&
                    !m_importedInterfaces->modules.at(*importedPath).exports.count(node.method) &&
                    namesType(std::get<ast::UpperIdentifier>(node.receiver->kind).name)) {
                    aliasedType = ownMakeReceiver(node.method);
                    if (aliasedType) {
                        isImportedNamespace = false;
                        isNamespaceCall = false;
                        throughTypeName = true;
                    }
                }
                if (isNamespaceCall && !isImportedNamespace && !isLocalNamespace &&
                    std::holds_alternative<ast::UpperIdentifier>(node.receiver->kind)) {
                    const auto& written = std::get<ast::UpperIdentifier>(node.receiver->kind).name;
                    // A constant or a nullary constructor is a value, whatever its
                    // spelling: `LIMIT.abs`, `Monday.name`, `STATUSES.map`.
                    if (!namesModule(written) && !namesType(written) &&
                        (constantType(written) || nullaryConstructorType(written))) {
                        isNamespaceCall = false;
                    }
                    const auto canonical = resolveRecordName(written);
                    // A `make` method of that type, this unit's or an imported
                    // unit's (`Boxes.Box`'s `named`, with `using Boxes`).
                    const auto ofThisType = [&](const Signature& method) {
                        if (method.params.empty()) {
                            return false;
                        }
                        const auto* receiver =
                            std::get_if<NamedType>(&resolve(method.params.front())->kind);
                        return receiver && resolveRecordName(receiver->name) == canonical;
                    };
                    const auto methods = m_methodSignatures.find(node.method);
                    const auto imported = importedCandidates(node.method);
                    if (!namesModule(written) && namesType(written) &&
                        !m_userSignatures.count(kex::mangle::qualify(written, node.method)) &&
                        ((methods != m_methodSignatures.end() &&
                          std::any_of(
                              methods->second.begin(), methods->second.end(), ofThisType)) ||
                         std::any_of(
                             imported.begin(), imported.end(),
                             [&](const ImportedFunction* function) {
                                 return ofThisType(function->signature);
                             }))) {
                        isNamespaceCall = false;
                        throughTypeName = true;
                    }
                }
                // A structural alias is no namespace of its own: `List.first(xs)`
                // (`type List<X> = [X]`) is `xs.first`, as both backends run it.
                // So is a member a same-named module does not have: `Map.get(m,
                // k)` beside `module Map do let from ... end`.
                const bool structuralAlias =
                    isNamespaceCall &&
                    std::holds_alternative<ast::UpperIdentifier>(node.receiver->kind) && [&] {
                        const auto& written =
                            std::get<ast::UpperIdentifier>(node.receiver->kind).name;
                        const auto imported = isImportedNamespace
                                                  ? &m_importedInterfaces->modules.at(*importedPath)
                                                  : nullptr;
                        return !isLocalNamespace &&
                               !(imported && imported->exports.count(node.method)) &&
                               !(!imported && namesModule(written)) &&
                               !m_userSignatures.count(
                                   kex::mangle::qualify(written, node.method)) &&
                               (m_typeAliases.count(written) ||
                                (m_importedInterfaces &&
                                 m_importedInterfaces->typeAliases.count(written)));
                    }();
                if (structuralAlias) {
                    callName = node.method;
                } else if (isImportedNamespace || isLocalNamespace) {
                    callName = kex::mangle::qualify(*importedPath, node.method);
                    // A private function is its module's own, through its path
                    // as by its bare name (freeFunctionVisible): the checker
                    // refuses it, and no backend keeps a copy of the rule.
                    if (m_privateFreeFunctions.contains(*importedPath + "\n" + node.method) &&
                        !withinModule(*importedPath)) {
                        error(
                            diag::Code::PrivateNameAccess, expr.location,
                            {{"name", node.method}, {"module", *importedPath}});
                        return Type::unknown();
                    }
                } else if (
                    isNamespaceCall &&
                    std::holds_alternative<ast::UpperIdentifier>(node.receiver->kind)) {
                    callName = kex::mangle::qualify(
                        std::get<ast::UpperIdentifier>(node.receiver->kind).name, node.method);
                }
                // A compile-time intrinsic (intrinsics.hxx): its entry types the
                // call when the arguments are no ordinary values, and folds it
                // once they are typed. The one place a callee name is consulted.
                const auto* intrinsic = findCompileTimeIntrinsic(callName, node.args.size());
                // A call folded to a compile-time answer is decided by it.
                const auto foldedHere = [&] {
                    if (m_staticTypeOfCalls.contains(&node)) {
                        m_decidedCalls.try_emplace(
                            &expr, DecidedCall{CallKind::Intrinsic, callName});
                    }
                };
                if (intrinsic && intrinsic->check) {
                    if (auto checked = (this->*intrinsic->check)(expr, node)) {
                        foldedHere();
                        return checked;
                    }
                }

                std::vector<TypePtr> argTypes;
                if (aliasedType) {
                    // The type, as through its own name below: the backends
                    // evaluate the receiver as that type, not a same-named
                    // constructor.
                    m_typeMap[node.receiver.get()] = aliasedType;
                    argTypes.push_back(aliasedType);
                } else if (!isNamespaceCall) {
                    auto receiver = node.receiver ? inferExpr(*node.receiver) : Type::unknown();
                    // Through the type's name the receiver is the type, not its
                    // `Type` record: `Response.ok(body)` is a Response method.
                    if (const auto* named = std::get_if<NamedType>(&receiver->kind);
                        throughTypeName && named && named->name == kex::names::kTypeRecord &&
                        named->typeArgs.size() == 1) {
                        receiver = named->typeArgs.front();
                        // ...and the backends evaluate it as the namespace it is.
                        m_typeMap[node.receiver.get()] = receiver;
                    }
                    argTypes.push_back(std::move(receiver));
                }
                // First pass: infer concrete args; defer lambdas with parameters
                // until the selected receiver signature can provide context.
                std::vector<std::pair<size_t, size_t>> contextualLambdas;
                for (size_t i = 0; i < node.args.size(); i++) {
                    const auto* lambda =
                        node.args[i] ? std::get_if<ast::Lambda>(&node.args[i]->kind) : nullptr;
                    if (node.args[i] &&
                        (std::holds_alternative<ast::ShorthandLambda>(node.args[i]->kind) ||
                         isBareCapture(node.args[i].get()) ||
                         (lambda && !lambda->params.empty()))) {
                        argTypes.push_back(Type::unknown()); // placeholder
                        contextualLambdas.emplace_back(isNamespaceCall ? i : 1 + i, i);
                    } else {
                        argTypes.push_back(
                            node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                    }
                }
                const bool serverSlotCall = [&] {
                    if (isNamespaceCall || argTypes.empty() ||
                        !m_slotMethodNames.count(node.method)) {
                        return false;
                    }
                    auto receiver = resolve(argTypes.front());
                    auto* named = std::get_if<NamedType>(&receiver->kind);
                    return named && named->name == kex::names::kServerType &&
                           named->typeArgs.size() == 1;
                }();
                for (const auto& [name, arg] : node.namedArgs) {
                    const auto* collecting = arg ? std::get_if<ast::Lambda>(&arg->kind) : nullptr;
                    auto type = !arg ? Type::unknown()
                                : collecting && collecting->collection
                                    ? inferBlock(*arg, namedArgHints(callName, name))
                                    : inferExpr(*arg);
                    if (serverSlotCall && name == "within") {
                        auto resolved = resolve(type);
                        const bool infinity = [&] {
                            auto* atom = std::get_if<PrimitiveType>(&resolved->kind);
                            return atom && atom->kind == PrimitiveType::Atom &&
                                   atom->literal == "infinity";
                        }();
                        if (!infinity && !containsOpenType(resolved) &&
                            !typesEqual(resolved, Type::integer())) {
                            error(
                                diag::Code::WithinNotMilliseconds,
                                arg ? arg->location : expr.location,
                                {{"type", typeToString(resolved)}});
                        }
                        continue;
                    }
                    argTypes.push_back(std::move(type));
                }
                // Second pass: infer lambdas with parameter hints from the signature.
                for (const auto& [argIdx, rawIdx] : contextualLambdas) {
                    auto hints = resolveArgHints(
                        callName, argTypes, argIdx,
                        /*isMethodCall=*/!isNamespaceCall);
                    argTypes[argIdx] = inferBlock(*node.args[rawIdx], hints);
                }
                if (node.block) {
                    bool collection = false;
                    std::string skippedTo;
                    const auto* written = std::get_if<ast::Lambda>(&(**node.block).kind);
                    auto hints = resolveBlockHints(
                        callName, argTypes, /*isMethodCall=*/!isNamespaceCall, &collection,
                        &skippedTo, written ? std::optional(written->params.size()) : std::nullopt);
                    if (collection) {
                        if (auto* lambda = std::get_if<ast::Lambda>(&(**node.block).kind)) {
                            lambda->collection = true;
                        }
                    }
                    argTypes.push_back(inferBlock(**node.block, hints));
                    passBlockByLabel(node, skippedTo);
                }
                if (intrinsic && intrinsic->fold) {
                    (this->*intrinsic->fold)(expr, node, argTypes);
                    foldedHere();
                }

                // `or` is what leaves the optional (or result) world, so its default
                // is the plain unwrapped value. `.or(None)` / `.or(Just(x))` passed
                // anyway — `None` is accepted wherever an optional could go — and
                // produced a value that is neither `X` nor `X?` (kexhq/kex#338;
                // STYLE.md `or-optional-default`).
                if (callName == "or" && argTypes.size() == 2) {
                    const auto receiver = resolve(argTypes[0]);
                    const auto fallback = resolve(argTypes[1]);
                    const auto* receiverNamed = std::get_if<NamedType>(&receiver->kind);
                    const bool optionalReceiver =
                        std::holds_alternative<OptionalType>(receiver->kind);
                    const bool resultReceiver =
                        receiverNamed && receiverNamed->name == kex::names::kResultType;
                    const auto* fallbackNamed = std::get_if<NamedType>(&fallback->kind);
                    // A bare `Just(8080)` / `Error("bad")` is typed gradually, so
                    // its spelling is the only thing that says it is wrapped.
                    const auto wrappingConstructor = [](const std::string& name) {
                        return name == kex::names::kJustConstructor ||
                               name == kex::names::kOkConstructor ||
                               name == kex::names::kErrorConstructor;
                    };
                    // `r.or { … }` passes its fallback as the trailing block, so
                    // `node.args` is empty (kexhq/kex#558).
                    const ast::Expr* argument = !node.args.empty()
                                                    ? node.args.back().get()
                                                    : (node.block ? node.block->get() : nullptr);
                    bool constructedWrapped = false;
                    if (argument) {
                        if (const auto* call = std::get_if<ast::FunctionCall>(&argument->kind)) {
                            constructedWrapped = wrappingConstructor(call->name);
                        } else if (const auto* method =
                                       std::get_if<ast::MethodCall>(&argument->kind);
                                   method && !method->receiver) {
                            constructedWrapped = wrappingConstructor(method->method);
                        }
                    }
                    const bool wrappedFallback =
                        constructedWrapped ||
                        std::holds_alternative<OptionalType>(fallback->kind) ||
                        (fallbackNamed && (fallbackNamed->name == kex::names::kNoneConstructor ||
                                           fallbackNamed->name == kex::names::kResultType));
                    // `or` never calls its fallback, so `r.or { |_| … }` answered
                    // the block itself in place of the value (kexhq/kex#558; STYLE.md
                    // `or-function-default`). A function default is still fine
                    // when the value is one.
                    // A bare `~f` is typed only in the deferred, hinted pass, so
                    // its type is not known yet here; its signatures say whether
                    // it captures a function. One of no parameters is a function
                    // too (`() -> T`, as the capture is typed below), not its
                    // value: `.or(~fallback)` answered the function itself
                    // (kexhq/kex#776).
                    const auto capturesFunction = [&](const ast::Expr* e) {
                        if (!isBareCapture(e)) {
                            return false;
                        }
                        const auto& name = std::get<ast::CurryExpr>(e->kind).name;
                        auto signatures = importedCandidateSignatures(name);
                        if (auto user = m_userSignatures.find(name);
                            user != m_userSignatures.end()) {
                            signatures = user->second;
                        }
                        return !signatures.empty();
                    };
                    const bool functionFallback =
                        argument && ((node.args.empty() && node.block) ||
                                     std::holds_alternative<FuncType>(fallback->kind) ||
                                     capturesFunction(argument));
                    if (optionalReceiver || resultReceiver) {
                        const auto unwrapped = resolve(
                            optionalReceiver ? std::get<OptionalType>(receiver->kind).inner
                                             : (receiverNamed->typeArgs.empty()
                                                    ? Type::unknown()
                                                    : receiverNamed->typeArgs.front()));
                        const bool openUnwrapped = containsOpenType(unwrapped);
                        if (wrappedFallback) {
                            const std::string got =
                                constructedWrapped
                                    ? "`" +
                                          std::string(
                                              std::get_if<ast::FunctionCall>(&argument->kind)
                                                  ? std::get<ast::FunctionCall>(argument->kind).name
                                                  : std::get<ast::MethodCall>(argument->kind)
                                                        .method) +
                                          "(…)`"
                                // A bare `None` is typed `T?` now that
                                // constructors carry their ADT; its spelling is
                                // what the reader wrote.
                                : argument &&
                                        (std::holds_alternative<ast::NoneLiteral>(argument->kind) ||
                                         (std::holds_alternative<ast::UpperIdentifier>(
                                              argument->kind) &&
                                          std::get<ast::UpperIdentifier>(argument->kind).name ==
                                              "None"))
                                    ? std::string("None")
                                    : typeToString(fallback);
                            error(
                                openUnwrapped
                                    ? (optionalReceiver ? diag::Code::OrWrappedFallbackOptionalOpen
                                                        : diag::Code::OrWrappedFallbackResultOpen)
                                    : (optionalReceiver ? diag::Code::OrWrappedFallbackOptional
                                                        : diag::Code::OrWrappedFallbackResult),
                                argument ? argument->location : expr.location,
                                {{"type", typeToString(unwrapped)}, {"got", got}});
                        } else if (
                            functionFallback && !openUnwrapped &&
                            !std::holds_alternative<UnknownType>(unwrapped->kind) &&
                            !std::holds_alternative<FuncType>(unwrapped->kind)) {
                            const bool block = node.args.empty();
                            error(
                                block ? (optionalReceiver ? diag::Code::OrBlockFallbackOptional
                                                          : diag::Code::OrBlockFallbackResult)
                                      : (optionalReceiver ? diag::Code::OrFunctionFallbackOptional
                                                          : diag::Code::OrFunctionFallbackResult),
                                argument->location, {{"type", typeToString(unwrapped)}});
                        }
                    }
                }
                // A payload constructor of a module of this program, through its
                // path (`Model.RecordDecl(entry)`): the module's ADT builds it.
                if (isNamespaceCall && importedPath && m_localModules.contains(*importedPath) &&
                    !node.method.empty() && kex::startsUppercase(node.method)) {
                    if (auto constructed =
                            constructorResultType(node.method, argTypes, *importedPath)) {
                        checkConstructorPayloads(
                            node.method, argTypes, expr.location, *importedPath);
                        const auto* adt = std::get_if<NamedType>(&resolve(constructed)->kind);
                        m_decidedCalls[&expr] = {
                            CallKind::Constructor, adt ? adt->name : node.method};
                        return constructed;
                    }
                }
                auto result = checkCall(
                    callName, argTypes, expr.location,
                    /*isMethodCall=*/true, &node, &expr);
                // An imported module's payload constructor through its path
                // (`Units.SI.Kilo(Units.SI.Watt)`) is called as the module's
                // exported function, and is a value of its ADT: typed as that
                // function's result it was a bare `Kilo` nothing resolves.
                if (isNamespaceCall && importedPath && !argTypes.empty() &&
                    !m_localModules.contains(*importedPath) && kex::startsUppercase(node.method)) {
                    if (const auto module = m_moduleConstructors.find(*importedPath);
                        module != m_moduleConstructors.end() &&
                        module->second.contains(node.method)) {
                        if (auto constructed =
                                constructorResultType(node.method, argTypes, *importedPath)) {
                            result = constructed;
                        }
                    }
                }
                // An imported module's nullary constructor, reached as the
                // module's exported function: its value is that variant of its ADT,
                // the type its bare name has where it is in scope.
                if (isNamespaceCall && importedPath && argTypes.empty()) {
                    if (const auto module = m_moduleConstructors.find(*importedPath);
                        module != m_moduleConstructors.end()) {
                        if (const auto constructor = module->second.find(node.method);
                            constructor != module->second.end() && constructor->second.imported &&
                            constructor->second.arity == 0) {
                            result = Type::variantOf(constructor->second.typeName, node.method);
                        }
                    }
                }
                // Through a structural alias the value is the first argument already:
                // the alias itself is no receiver to pass.
                if (structuralAlias) {
                    if (auto resolved = m_resolvedCalls.find(&node);
                        resolved != m_resolvedCalls.end()) {
                        resolved->second.passesReceiver = false;
                    }
                }
                // Nothing chose a target and the receiver's type is not known
                // (or is a trait's `This`, in its default bodies): the method is
                // looked up on the value when the call runs.
                if (!isNamespaceCall && !argTypes.empty() && !m_decidedCalls.contains(&expr) &&
                    !m_selectedCallSignatures.contains(&expr) && !m_resolvedCalls.contains(&node)) {
                    const auto receiver = resolve(argTypes.front());
                    // On a trait's `This` the type claiming it answers, in the copy
                    // of the default lowered for that type: never an import.
                    if (isThisPlaceholder(receiver)) {
                        m_decidedCalls[&expr] = {CallKind::Dispatch, node.method};
                    } else if (isUndetermined(receiver)) {
                        decideDispatch(&expr, node.method);
                    }
                }
                return result;
            } else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
                // `[...xs.map(~double), 99]`: the spread value is checked like
                // any other, and in a list it stands for its elements.
                auto spread = node.inner ? resolve(inferExpr(*node.inner)) : Type::unknown();
                if (const auto* list = std::get_if<ListType>(&spread->kind)) {
                    return list->element;
                }
                return Type::unknown();
            } else if constexpr (std::is_same_v<T, ast::ListExpr>) {
                const auto expected = std::exchange(m_branchExpectation, nullptr);
                if (node.anyElements) {
                    for (const auto& elem : node.elements) {
                        if (elem) {
                            inferExpr(*elem);
                        }
                    }
                    return Type::list(Type::unknown());
                }
                TypePtr elemType = Type::unknown();
                std::vector<std::pair<const ast::Expr*, TypePtr>> written;
                for (const auto& elem : node.elements) {
                    if (elem) {
                        auto type = inferExpr(*elem);
                        written.emplace_back(elem.get(), type);
                        elemType = joinElementType(elemType, type);
                    }
                }
                // An element of a distinct type held as a trait it implements
                // itself (`[Labeled]` holding a `BookId`): what dispatches on it
                // later finds its backing's implementation (§14 item 4).
                if (const auto* wantedList =
                        expected ? std::get_if<ListType>(&resolve(expected)->kind) : nullptr) {
                    for (const auto& [element, type] : written) {
                        if (const auto trait = distinctTraitClash(type, wantedList->element);
                            !trait.empty()) {
                            typeMismatch(element->location, wantedList->element, type);
                        }
                    }
                }
                // Where the list's type is expected (a typed `let`, a declared
                // result), each element is checked against the element type and
                // a wrong one is reported where it is, rather than the whole list
                // as `[Integer | String]` (decision 3). A literal element is left
                // to the literal rules (a Byte's range, a distinct type).
                if (const auto* wanted = expected && !node.rest
                                             ? std::get_if<ListType>(&resolve(expected)->kind)
                                             : nullptr;
                    wanted && isConcreteValue(resolve(wanted->element))) {
                    // A literal another rule types: one entering a distinct type,
                    // or a number where a number is wanted (a Byte's range).
                    const auto literalRule = [&](const ast::Expr& e) {
                        const auto* negated = std::get_if<ast::UnaryOp>(&e.kind);
                        const ast::Expr& inner =
                            negated && negated->operand ? *negated->operand : e;
                        const bool number = std::holds_alternative<ast::IntLiteral>(inner.kind) ||
                                            std::holds_alternative<ast::FloatLiteral>(inner.kind);
                        return literalCoercesTo(e, wanted->element) ||
                               (number && isNumericType(resolve(wanted->element)));
                    };
                    bool mismatched = false;
                    for (const auto& [element, type] : written) {
                        const auto actual = resolve(type);
                        if (literalRule(*element) || !isConcreteValue(actual) ||
                            argMatchesParam(actual, wanted->element)) {
                            continue;
                        }
                        typeMismatch(element->location, wanted->element, actual);
                        mismatched = true;
                    }
                    if (mismatched) {
                        return expected;
                    }
                }
                // `[x | rest]`: the tail is a list of the same elements.
                if (node.rest && *node.rest) {
                    const auto open = [](const TypePtr& type) { return isUndetermined(type); };
                    const auto rest = resolve(inferExpr(**node.rest));
                    if (const auto* list = std::get_if<ListType>(&rest->kind)) {
                        const auto element = resolve(list->element);
                        if (open(elemType)) {
                            elemType = element;
                        } else if (
                            !open(element) && !argMatchesParam(element, elemType) &&
                            !argMatchesParam(elemType, element)) {
                            error(
                                diag::Code::ListTailElementMismatch, (*node.rest)->location,
                                {{"element", typeToString(elemType)},
                                 {"actual", typeToString(rest)}});
                        }
                    } else if (!open(rest)) {
                        error(
                            diag::Code::ListTailNotList, (*node.rest)->location,
                            {{"actual", typeToString(rest)}});
                    }
                }
                return Type::list(elemType);
            } else if constexpr (std::is_same_v<T, ast::MapExpr>) {
                // Keys and values join as a list's elements do, and where they
                // share nothing the map holds `Any`: `{"id": "u1", "width": 3}`
                // is ordinary data (a decoder's input, a `{String: Any}`). The
                // first of each was taken and the rest never looked at, so
                // `{ "ada": 36, "bob": "old" }` was a `{String: Integer}` holding
                // a String, and reading `"bob"` as an Integer passed
                // (kexhq/kex#771).
                // A map keyed by atoms is a set of named fields, not a
                // collection: a serving slot's `{ new, reply: new.count }`
                // holds the state and the reply. Its values keep their own
                // types. A spread's value is the spread map, not one value.
                const bool atomKeyed =
                    std::any_of(node.entries.begin(), node.entries.end(), [](const auto& entry) {
                        return entry.key &&
                               std::holds_alternative<ast::AtomLiteral>(entry.key->kind);
                    });
                // Keys and values of different types join into their union, as a
                // list's elements do: `{ "ada": 36, "bob": "old" }` is a
                // `{String: Integer | String}`, so reading `"bob"` as an Integer
                // is an error. They widened to `Any` before, which let it through.
                // An atom-keyed map still takes its first value's type: until
                // #200 types serving transitions, the checker reads one as named
                // fields (docs/typechecker-hm-plan.md, decision 7).
                TypePtr keyType = Type::unknown();
                TypePtr valueType = Type::unknown();
                for (const auto& entry : node.entries) {
                    if (entry.key) {
                        keyType = joinElementType(keyType, inferExpr(*entry.key));
                    }
                    if (!entry.value) {
                        continue;
                    }
                    auto value = inferExpr(*entry.value);
                    if (atomKeyed || entry.spread) {
                        if (isUndetermined(valueType) && !entry.spread) {
                            valueType = value;
                        }
                        continue;
                    }
                    valueType = joinElementType(valueType, value);
                }
                return Type::map(keyType, valueType);
            } else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
                std::vector<TypePtr> types;
                for (const auto& elem : node.elements) {
                    types.push_back(elem ? inferExpr(*elem) : Type::unknown());
                }
                return Type::tuple(std::move(types));
            } else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
                auto startType = node.start ? resolve(inferExpr(*node.start)) : Type::unknown();
                auto endType = node.end ? resolve(inferExpr(*node.end)) : Type::unknown();
                const auto elementType = [](const TypePtr& bound) -> TypePtr {
                    if (std::holds_alternative<SizedFloatType>(bound->kind)) {
                        return Type::float64();
                    }
                    if (std::holds_alternative<SizedIntType>(bound->kind)) {
                        return Type::integer();
                    }
                    if (auto* primitive = std::get_if<PrimitiveType>(&bound->kind)) {
                        if (primitive->kind == PrimitiveType::Char) {
                            return Type::charT();
                        }
                        if (primitive->kind == PrimitiveType::Integer) {
                            return Type::integer();
                        }
                    }
                    return nullptr;
                };
                auto startElement = elementType(startType);
                auto endElement = elementType(endType);
                const auto unresolved = [](const TypePtr& bound) { return isUndetermined(bound); };
                if ((!startElement && !unresolved(startType)) ||
                    (!endElement && !unresolved(endType))) {
                    error(diag::Code::RangeEndpointType, expr.location);
                } else if (startElement && endElement && !typesEqual(startElement, endElement)) {
                    error(diag::Code::RangeEndpointsDiffer, expr.location);
                }
                return Type::named("Range", {startElement ? startElement : Type::integer()});
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                const auto expected = std::exchange(m_branchExpectation, nullptr);
                if (node.letPattern) {
                    // `if let Pattern = expr` — infer scrutinee, bind pattern vars
                    // in a scope covering only the then-body (already scoped in
                    // resolve pass). Skip Bool check — it's a pattern match.
                    auto scrutinee = node.condition ? inferExpr(*node.condition) : nullptr;
                    pushScope();
                    if (node.letPattern) {
                        bindPatternVars(*node.letPattern, scrutinee);
                    }
                } else if (node.condition) {
                    checkCondition(*node.condition, diag::Code::IfConditionNotBool);
                }
                // A branch that exits the function produces nothing for the
                // conditional to agree with, so it must not drive the result type.
                auto branchBodyType = [this](const std::vector<ast::ExprPtr>& body) {
                    auto inferred = inferBody(body);
                    return alwaysReturns(body) ? Type::never() : inferred;
                };
                const auto valueOf = [&](const std::vector<ast::ExprPtr>& body) {
                    return !body.empty() && body.back() ? body.back()->location : expr.location;
                };
                std::vector<Branch> branches;
                branches.push_back({branchBodyType(node.thenBody), valueOf(node.thenBody)});
                if (node.letPattern) {
                    popScope();
                }
                for (const auto& [cond, body] : node.elifs) {
                    if (cond) {
                        checkCondition(*cond, diag::Code::ElifConditionNotBool);
                    }
                    branches.push_back({branchBodyType(body), valueOf(body)});
                }
                if (node.elseBody) {
                    branches.push_back({branchBodyType(*node.elseBody), valueOf(*node.elseBody)});
                }
                // With no `else` the conditional has no value (#809): it may
                // produce nothing at all, so it cannot hand its then-branch's type
                // to the block it closes. `if done? ... return Error(e) ... end` is
                // not a Result-valued expression. It is Void, not Never: Never fits
                // every type, so a declared `-> Integer` body ending in one was
                // accepted and returned None or :ok at run time.
                if (!node.elseBody) {
                    return Type::voidType();
                }
                return joinBranches(branches, branchCheckTarget(expected));
            } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                const auto expected = std::exchange(m_branchExpectation, nullptr);
                TypePtr subjectType = node.subject ? inferExpr(*node.subject) : Type::unknown();
                std::vector<Branch> arms;
                // A later arm's variable takes only what the earlier unguarded
                // arms left: after `Request { path } => …`, `text` on a
                // `String | Request` is a String. It narrows only while every
                // earlier arm's members are known (unionMembersTaken).
                const auto shape = expandTypeAliases(resolve(subjectType));
                const auto* subjectUnion = std::get_if<UnionType>(&shape->kind);
                const auto memberNames =
                    subjectUnion ? unionMemberNames(subjectType) : std::vector<std::string>{};
                std::vector<bool> taken(memberNames.size(), false);
                bool takenKnown =
                    subjectUnion && memberNames.size() == subjectUnion->members.size();
                const auto leftOver = [&]() -> TypePtr {
                    if (!takenKnown) {
                        return subjectType;
                    }
                    TypePtr rest = Type::never();
                    for (std::size_t i = 0; i < taken.size(); ++i) {
                        if (!taken[i]) {
                            rest = std::holds_alternative<NeverType>(rest->kind)
                                       ? subjectUnion->members[i]
                                       : unionOf(rest, subjectUnion->members[i]);
                        }
                    }
                    // Nothing left means the arm is unreachable: leave its
                    // variable the whole type rather than invent a Never.
                    return std::holds_alternative<NeverType>(rest->kind) ? subjectType : rest;
                };
                for (const auto& clause : node.clauses) {
                    pushScope();
                    if (node.subjectBinding) {
                        defineVar(*node.subjectBinding, subjectType);
                    }
                    for (const auto& pat : clause.patterns) {
                        if (pat) {
                            checkPatternArity(*pat);
                            const bool variable =
                                std::holds_alternative<ast::VarPattern>(pat->kind);
                            bindPatternVars(*pat, variable ? leftOver() : subjectType);
                        }
                    }
                    if (takenKnown && !(clause.guard && *clause.guard)) {
                        for (const auto& pat : clause.patterns) {
                            const auto armTakes =
                                pat ? unionMembersTaken(*pat, memberNames) : std::nullopt;
                            if (!armTakes) {
                                takenKnown = false;
                                break;
                            }
                            for (std::size_t i = 0; i < taken.size(); ++i) {
                                taken[i] = taken[i] || (*armTakes)[i];
                            }
                        }
                    }
                    if (clause.guard && *clause.guard) {
                        checkCondition(**clause.guard, diag::Code::GuardNotBool);
                    }
                    if (clause.body) {
                        // An arm that exits the function produces nothing for the
                        // match to agree with: it drops out of the join as Never.
                        auto armType = inferExpr(*clause.body);
                        arms.push_back(
                            {alwaysReturns(*clause.body) ? Type::never() : armType,
                             clause.body->location});
                    }
                    popScope();
                }
                if (!checkMatchExhaustiveness(node, subjectType, expr.location) &&
                    !checkUnionCoverage(node, subjectType, expr.location)) {
                    checkPatternMatrix(node, subjectType, expr.location);
                }
                return joinBranches(arms, branchCheckTarget(expected));
            } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
                TypePtr resultType = Type::unknown();
                for (const auto& clause : node.clauses) {
                    pushScope();
                    if (node.senderBinding) {
                        defineVar(*node.senderBinding, Type::named("Process", {Type::unknown()}));
                    }
                    for (const auto& pattern : clause.patterns) {
                        if (pattern) {
                            checkPatternArity(*pattern);
                            bindPatternVars(*pattern);
                        }
                    }
                    if (clause.guard && *clause.guard) {
                        checkCondition(**clause.guard, diag::Code::GuardNotBool);
                    }
                    if (clause.body) {
                        auto arm = resolve(inferExpr(*clause.body));
                        if (std::holds_alternative<UnknownType>(resolve(resultType)->kind)) {
                            resultType = arm;
                        }
                    }
                    popScope();
                }
                // `after timeout:` waits a number of milliseconds: anything else
                // (`Just(70)`, `"kex"`) checked clean, then BEAM raised
                // `timeout_value` and the walker never woke.
                if (node.timeout && *node.timeout) {
                    if (const auto waited = resolve(inferExpr(**node.timeout));
                        isConcreteValue(waited) && !argMatchesParam(waited, Type::integer())) {
                        error(
                            diag::Code::AfterTimeoutNotInteger, (*node.timeout)->location,
                            {{"type", typeToString(waited)}});
                    }
                }
                if (node.afterBody && *node.afterBody) {
                    auto after = resolve(inferExpr(**node.afterBody));
                    if (std::holds_alternative<UnknownType>(resolve(resultType)->kind)) {
                        resultType = after;
                    }
                }
                return resultType;
            } else if constexpr (
                std::is_same_v<T, ast::BreakExpr> || std::is_same_v<T, ast::NextExpr>) {
                // Leaves the loop: no value reaches what follows (decision 9).
                return Type::never();
            } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                auto value = node.value ? inferExpr(*node.value) : Type::voidType();
                // `return x if cond` is a ReturnExpr around TrailingIf{x, cond}.
                // As a statement the guard yields nothing, but when it holds
                // the function leaves with x, so x's type is what the clause
                // is answered with (kexhq/kex#723).
                auto leavesWith = value;
                if (const auto* guarded =
                        node.value ? std::get_if<ast::TrailingIf>(&node.value->kind) : nullptr;
                    guarded && guarded->expr) {
                    if (const auto found = m_typeMap.find(guarded->expr.get());
                        found != m_typeMap.end()) {
                        leavesWith = found->second;
                    }
                }
                if (m_clauseReturns && m_blockDepth == m_clauseTriesDepth) {
                    m_clauseReturns->push_back({&expr, leavesWith});
                }
                return value;
            } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                // Lambda used as a value (not a trailing block — that path goes
                // through inferBlock). Infer param types and body, return a proper
                // FuncType so call-site checking can validate the argument types.
                //
                // A `return` in it is not one of the enclosing function's: the
                // lambda may be called after that function has returned, and
                // then it is the lambda's own value (spec/return_in_block_scope).
                // So it is not checked against the function's declared result.
                const auto outerReturns = std::exchange(m_clauseReturns, nullptr);
                if (node.params.empty()) {
                    m_blockDepth++;
                    BlockTries blockTries(m_clauseTries, m_clauseTriesDepth, m_blockDepth);
                    pushScope();
                    auto bodyType = inferBody(node.body);
                    if (node.rescue) {
                        checkRescue(*node.rescue);
                    }
                    popScope();
                    m_blockDepth--;
                    m_clauseReturns = outerReturns;
                    return Type::func(
                        {}, blockFailingResult(node, blockTries.tries, resolve(bodyType)));
                }
                pushScope();
                std::vector<TypePtr> paramTypes;
                std::unordered_map<std::string, TypePtr> genericVars;
                for (const auto& param : node.params) {
                    auto pt =
                        param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar();
                    paramTypes.push_back(pt);
                    if (param.name != "_") {
                        defineVar(
                            param.name, pt,
                            {false, false, param.location.line, param.location.column});
                        if (m_recordsNames && param.location.line > 0) {
                            m_bindingSites.emplace_back(param.name, param.location);
                        }
                        // Value-position lambdas need the same tooling binding as
                        // contextual/trailing blocks. Without it `{ |name| ... }`
                        // had no local hover entry and the LSP fell through to an
                        // unrelated exported symbol with the same name.
                        m_patternBindings.push_back(
                            {param.name, param.location.line > 0 ? param.location : expr.location,
                             pt});
                    }
                }
                m_blockDepth++;
                BlockTries blockTries(m_clauseTries, m_clauseTriesDepth, m_blockDepth);
                auto bodyType = inferBody(node.body);
                if (node.rescue) {
                    checkRescue(*node.rescue);
                }
                m_blockDepth--;
                m_clauseReturns = outerReturns;
                auto resultType = resolve(bodyType);
                if (node.returnAnnotation) {
                    auto declared = resolveTypeExpr(**node.returnAnnotation, genericVars);
                    if (!std::holds_alternative<UnknownType>(resultType->kind) &&
                        !std::holds_alternative<TypeVar>(resultType->kind) &&
                        !argMatchesParam(resultType, declared) &&
                        !wrapsDeclaredSuccess(node.body, declared, resultType)) {
                        typeMismatch(expr.location, declared, resultType);
                    }
                    resultType = declared;
                }
                resultType = blockFailingResult(node, blockTries.tries, resultType);
                popScope();
                // Resolve param types after body inference — body may have constrained them.
                for (auto& pt : paramTypes) {
                    pt = resolve(pt);
                }
                return Type::func(std::move(paramTypes), resultType);
            } else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
                pushScope();
                inferBody(node.body);
                popScope();
                // Process<Msg> — Msg is a fresh TypeVar that unification resolves
                // against the declared return-type annotation (e.g. -> Process<Counter>).
                return Type::named("Process", {freshTypeVar()});
            } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                pushScope();
                if (node.counter && *node.counter != "_") {
                    defineVar(*node.counter, Type::integer());
                }
                inferBody(node.body);
                popScope();
                return Type::never(); // infinite loop never returns
            } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                if (node.condition) {
                    checkCondition(*node.condition, diag::Code::WhileConditionNotBool);
                }
                inferBody(node.body);
                return Type::voidType();
            } else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
                // Check each value against the field's DECLARED type. Without
                // this, `User { name: 42 }` on `name : String` was accepted and
                // only surfaced later — or not at all.
                std::string sourceName = node.typeName;
                TypePtr contextualRecordType;
                if (sourceName == names::kThisType || sourceName == kex::names::kNewType) {
                    if (!m_inMakeBlock || !m_currentMakeType) {
                        error(
                            diag::Code::ThisConstructionOutsideMake, expr.location,
                            {{"name", sourceName}});
                        return Type::unknown();
                    }
                    auto receiver = resolve(m_currentMakeType);
                    auto* named = std::get_if<NamedType>(&receiver->kind);
                    if (!named || !(findRecordFields(*named) != m_recordFields.end())) {
                        error(
                            diag::Code::ThisConstructionNeedsRecord, expr.location,
                            {{"name", sourceName}});
                        return Type::unknown();
                    }
                    sourceName = named->name;
                    contextualRecordType = receiver;
                }
                const auto recordName = resolveRecordName(sourceName);
                // Naming a qualified record is a reference to its MODULE: the
                // record's `make` block and field defaults live in that source,
                // and on BEAM the module has to be in the build at all
                // (kexhq/kex#143). The module is the qualifier, not the record —
                // inserting `Mock.Files` here put a type name into a list of
                // modules to compile.
                if (auto dot = recordName.rfind('.'); dot != std::string::npos) {
                    m_referencedModules.insert(recordName.substr(0, dot));
                }
                auto record = m_recordFields.find(recordName);
                bool hasSpread = node.typeName == kex::names::kNewType;
                std::unordered_set<std::string> supplied;
                // Type arguments recovered from the values, slot by slot.
                std::unordered_map<int, TypePtr> typeParamSlots;
                for (const auto& entry : node.fields) {
                    const auto& fieldName = entry.name;
                    const auto& val = entry.value;
                    if (!val) {
                        continue;
                    }
                    // A field's declared type is what its value is written for: a
                    // block there takes what the field says it takes (`Deps {
                    // fetch: { |u| 7 + u } }` for `fetch : String -> String` adds
                    // a String to 7).
                    TypePtr fieldExpectation;
                    if (!entry.spread && record != m_recordFields.end() && writtenFunction(*val)) {
                        if (const auto field = record->second.find(fieldName);
                            field != record->second.end()) {
                            fieldExpectation = field->second;
                        }
                    }
                    const auto outerExpectation =
                        std::exchange(m_branchExpectation, fieldExpectation);
                    auto valueType = resolve(inferExpr(*val));
                    m_branchExpectation = outerExpectation;
                    if (entry.spread) {
                        hasSpread = true;
                        // `Box { ...other, items: … }` keeps whatever `other`
                        // already pinned for the slots the new fields don't.
                        if (auto* spreadNamed = std::get_if<NamedType>(&valueType->kind)) {
                            for (size_t i = 0; i < spreadNamed->typeArgs.size(); ++i) {
                                bindRecordTypeParams(
                                    Type::genericLetter(-static_cast<int>(i + 1)),
                                    spreadNamed->typeArgs[i], typeParamSlots);
                            }
                        }
                        if (!containsOpenType(valueType) &&
                            !argMatchesParam(valueType, Type::named(recordName))) {
                            error(
                                diag::Code::RecordSpreadMismatch, val->location,
                                {{"record", sourceName}, {"actual", typeToString(valueType)}});
                        }
                        continue;
                    }
                    supplied.insert(fieldName);
                    // A capture given to a field of a foul function type (`Route`'s
                    // `handler : Handler`) may name a foul function (#640).
                    if (record != m_recordFields.end()) {
                        if (const auto field = record->second.find(fieldName);
                            field != record->second.end()) {
                            noteCaptureFlow(val.get(), field->second);
                        }
                    } else if (m_importedInterfaces) {
                        if (const auto imported =
                                m_importedInterfaces->recordFields.find(recordName);
                            imported != m_importedInterfaces->recordFields.end()) {
                            if (const auto field = imported->second.find(fieldName);
                                field != imported->second.end()) {
                                noteCaptureFlow(val.get(), field->second);
                            }
                        }
                    }
                    if (record == m_recordFields.end()) {
                        continue;
                    }
                    auto declared = record->second.find(fieldName);
                    // For tooling: the field this key names, of the record the
                    // checker resolved the construction to.
                    if (m_recordsNames && entry.keyLocation.line > 0 &&
                        declared != record->second.end()) {
                        m_recordKeys.push_back(
                            {entry.keyLocation, recordName, fieldName, declared->second});
                    }
                    // An unknown field name used to be skipped, which made a
                    // renamed field silently absorb the old spelling: the
                    // interpreter stored `zebra:` on a record with no such field
                    // and `.zebra` read it straight back, while BEAM only failed at
                    // runtime with "Undefined method: zebra". Renaming a field
                    // therefore broke callers with no diagnostic at all. The
                    // pattern side has always checked this (see checkPatternArity);
                    // construction just never did.
                    if (declared == record->second.end()) {
                        // List the layout: on a rename or a typo the useful
                        // question is immediately "then what ARE the fields?".
                        // Sorted so the message is stable — m_recordFields is a
                        // hash map.
                        std::vector<std::string> available;
                        available.reserve(record->second.size());
                        for (const auto& [known, _] : record->second) {
                            available.push_back(known);
                        }
                        std::sort(available.begin(), available.end());
                        std::string layout;
                        for (std::size_t i = 0; i < available.size(); i++) {
                            if (i) {
                                layout += ", ";
                            }
                            layout += available[i];
                        }
                        if (layout.empty()) {
                            error(
                                diag::Code::RecordHasNoFieldsAtAll, val->location,
                                {{"record", sourceName}, {"field", fieldName}});
                        } else {
                            error(
                                diag::Code::RecordHasNoFieldListed, val->location,
                                {{"record", sourceName}, {"field", fieldName}, {"fields", layout}});
                        }
                        continue;
                    }
                    auto expected = resolve(declared->second);
                    bindRecordTypeParams(expected, valueType, typeParamSlots);
                    // A field written `Float` is a bound, so open: the note comes
                    // first (decision 2: `Point { x: 2 }` builds a Float).
                    noteFloatLiteral(*val, expected);
                    // Stay quiet where either side is open ANYWHERE inside it: a
                    // type variable, a type the checker could not pin down, or a
                    // trait constraint, which names a set rather than a concrete
                    // type. Two cases this must not reject: `make Integer do …
                    // Duration { seconds: this * 1.0 }` infers `Number` for the
                    // value, and a `Map<Any, String>` field takes `{"k": "v"}`
                    // — there the openness is nested, not at the top.
                    if (containsOpenType(valueType) || containsOpenType(expected)) {
                        continue;
                    }
                    if (!argMatchesParam(valueType, expected) &&
                        !literalCoercesTo(*val, expected)) {
                        error(
                            diag::Code::RecordFieldMismatch, val->location,
                            {{"record", sourceName},
                             {"field", fieldName},
                             {"expected", typeToString(expected)},
                             {"actual", typeToString(valueType)}});
                    }
                }
                if (!hasSpread) {
                    std::vector<std::string> missing;
                    for (const auto& required : m_requiredRecordFields[recordName]) {
                        if (!supplied.count(required)) {
                            missing.push_back(required);
                        }
                    }
                    std::sort(missing.begin(), missing.end());
                    if (!missing.empty()) {
                        std::string names;
                        for (std::size_t i = 0; i < missing.size(); ++i) {
                            if (i) {
                                names += ", ";
                            }
                            names += "`" + missing[i] + "`";
                        }
                        error(
                            missing.size() == 1 ? diag::Code::MissingMandatoryField
                                                : diag::Code::MissingMandatoryFields,
                            expr.location, {{"fields", names}, {"record", sourceName}});
                    }
                }
                // `recordName` is `node.typeName` itself when no declaration was
                // found, so the resolved identity is always the right answer here:
                // a literal must have the same type name that annotations
                // mentioning the same record resolve to.
                if (contextualRecordType) {
                    return contextualRecordType;
                }
                // `Crew {}` with no `Crew` declared is a typo or a missing
                // declaration, not a record: unchecked, the walker made an empty
                // record and BEAM a bare `:Crew` atom.
                if (record == m_recordFields.end() &&
                    !(m_importedInterfaces &&
                      m_importedInterfaces->recordArities.count(recordName))) {
                    error(diag::Code::UnknownRecordType, expr.location, {{"record", sourceName}});
                    return Type::unknown();
                }
                // A generic record's literal has to carry its arguments, or the
                // value can never flow back into anything that names them:
                // `Box { items: [1, 2, 3] }` is a `Box<Integer>`, and only that
                // spelling matches a `Box<A>` receiver or reads `.items` back as
                // `[Integer]` (kexhq/kex#203). Slots no field pinned stay open,
                // which is what `Box { items: [] }` deserves.
                const size_t paramCount =
                    record == m_recordFields.end() ? 0 : recordTypeParamCount(record->second);
                // A literal names the program's own record when it has one of
                // a prelude record's name.
                if (paramCount == 0) {
                    return ownType(Type::named(recordName));
                }
                std::vector<TypePtr> typeArgs(paramCount, Type::unknown());
                for (const auto& [slot, bound] : typeParamSlots) {
                    const auto index = static_cast<size_t>(-slot - 1);
                    if (index < typeArgs.size()) {
                        typeArgs[index] = bound;
                    }
                }
                return ownType(Type::named(recordName, std::move(typeArgs)));
            } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                if (node.condition) {
                    checkCondition(*node.condition, diag::Code::IfConditionNotBool);
                }
                // `expr if cond` is a guarded STATEMENT: when the guard is false
                // nothing is produced, so it cannot hand its expression's type to
                // the block it closes. `return Ok(x) if done?` is the shape that
                // matters — it must not make its branch look like it yields a
                // Result. It is Void, not Never, for the same reason as an `if`
                // without `else` (#809).
                if (node.expr) {
                    inferExpr(*node.expr);
                    return Type::voidType();
                }
                return Type::unknown();
            } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                return inferBody(node.body);
            } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                const auto expected = std::exchange(m_branchExpectation, nullptr);
                if (node.condition) {
                    checkCondition(*node.condition, diag::Code::ThenElseConditionNotBool);
                }
                std::vector<Branch> branches;
                if (node.thenExpr) {
                    branches.push_back({inferExpr(*node.thenExpr), node.thenExpr->location});
                }
                if (node.elseExpr) {
                    branches.push_back({inferExpr(*node.elseExpr), node.elseExpr->location});
                }
                return joinBranches(branches, branchCheckTarget(expected));
            } else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
                return inferExpr(*node.lambda);
            } else if constexpr (std::is_same_v<T, ast::CurryPlaceholder>) {
                return Type::unknown();
            } else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
                decideCapture(expr, node);
                std::vector<Signature> candidates;
                if (node.isOperator) {
                    Signature signature;
                    signature.name = node.name;
                    if (node.name == "!") {
                        signature.params = {Type::boolean()};
                        signature.result = Type::boolean();
                    } else if (node.name == "&&" || node.name == "||") {
                        signature.params = {Type::boolean(), Type::boolean()};
                        signature.result = Type::boolean();
                    } else {
                        auto operand = freshTypeVar();
                        signature.params = {operand, operand};
                        if (node.name == "==" || node.name == "!=" || node.name == "<" ||
                            node.name == ">" || node.name == "<=" || node.name == ">=") {
                            signature.result = Type::boolean();
                        } else {
                            signature.result = operand;
                        }
                    }
                    candidates.push_back(std::move(signature));
                } else if (node.module.empty()) {
                    // A bare capture reaches what a call of the name would: a
                    // local function first (`let wrap(...)` inside `main`
                    // shadows the prelude's `wrap`), then the functions of the
                    // name.
                    std::vector<Signature> visible;
                    std::set<std::string> hidden;
                    if (const auto local = lookupVar(node.name)) {
                        if (const auto* function = std::get_if<FuncType>(&resolve(local)->kind)) {
                            Signature signature;
                            signature.name = node.name;
                            signature.params = function->params;
                            signature.result = function->result;
                            candidates.push_back(std::move(signature));
                        }
                    }
                    if (const auto* reached =
                            candidates.empty()
                                ? reachableFreeFunctions(node.name, false, visible, hidden)
                                : nullptr) {
                        candidates = *reached;
                    }
                    if (candidates.empty()) {
                        candidates = importedCandidateSignatures(node.name);
                    }
                    if (candidates.empty() && m_importedInterfaces && !node.name.empty() &&
                        std::islower(static_cast<unsigned char>(node.name.front())) &&
                        !lookupVar(node.name) && !bareNameDeclared(node.name)) {
                        reportUndefined("function", node.name, expr.location);
                    }
                } else {
                    // A qualified one names its module as a call would: through
                    // an alias, an enclosing module, or a module a `using` opened.
                    const auto key =
                        kex::mangle::qualify(resolveModulePath(node.module, node.name), node.name);
                    if (auto user = m_userSignatures.find(key); user != m_userSignatures.end()) {
                        candidates = user->second;
                    }
                    if (candidates.empty()) {
                        candidates = importedCandidateSignatures(key);
                    }
                    // As a call of it would say: `~String.nope` names no function of
                    // `String`, and both backends failed when it ran. A `make`
                    // function of the type the module names counts
                    // (`~String.upperCase`): its receiver is the first argument.
                    const auto modulePath = resolveModulePath(node.module, node.name);
                    const bool knownModule =
                        m_localModules.contains(modulePath) ||
                        m_moduleReexports.contains(modulePath) ||
                        m_importedModulePaths.contains(modulePath) ||
                        (m_importedInterfaces &&
                         (m_importedInterfaces->modules.contains(modulePath) ||
                          m_importedInterfaces->modules.contains(names::moduleAtom(modulePath))));
                    const auto onType = [&](const Signature& signature) {
                        return !signature.params.empty() &&
                               typeToString(resolve(signature.params.front())) == modulePath;
                    };
                    bool typeHasIt = false;
                    if (const auto own = m_methodSignatures.find(node.name);
                        own != m_methodSignatures.end()) {
                        typeHasIt = std::any_of(own->second.begin(), own->second.end(), onType);
                    }
                    if (!typeHasIt && m_importedInterfaces) {
                        if (const auto imported =
                                m_importedInterfaces->receiverFunctions.find(node.name);
                            imported != m_importedInterfaces->receiverFunctions.end()) {
                            typeHasIt = std::any_of(
                                imported->second.begin(), imported->second.end(),
                                [&](const ImportedFunction& function) {
                                    return onType(function.signature);
                                });
                        }
                    }
                    // `~String.upperCase` is the type's own function: the ones
                    // taking a String, never Char's. Typed by nothing, a capture
                    // bound to a name (`let f = ~String.upperCase`) took any list.
                    if (candidates.empty() && typeHasIt) {
                        if (const auto own = m_methodSignatures.find(node.name);
                            own != m_methodSignatures.end()) {
                            std::copy_if(
                                own->second.begin(), own->second.end(),
                                std::back_inserter(candidates), onType);
                        }
                        if (candidates.empty() && m_importedInterfaces) {
                            if (const auto imported =
                                    m_importedInterfaces->receiverFunctions.find(node.name);
                                imported != m_importedInterfaces->receiverFunctions.end()) {
                                for (const auto& function : imported->second) {
                                    if (onType(function.signature)) {
                                        candidates.push_back(function.signature);
                                    }
                                }
                            }
                        }
                        // Its receiver is that type: the call it stands for is
                        // decided here, as one a known receiver flows into is.
                        if (!candidates.empty() && node.argGroups.empty()) {
                            if (const auto owner = typeNamed(node.module, expr.location)) {
                                decideCaptureAsCall(expr, node, owner);
                            }
                        }
                    }
                    if (candidates.empty() && knownModule && !typeHasIt) {
                        error(
                            diag::Code::ModuleHasNoFunction, expr.location,
                            {{"module", node.module}, {"name", node.name}});
                    }
                }

                if (candidates.empty()) {
                    // Preserve gradual recovery for an unresolved capture while
                    // still visiting every bound argument.
                    int open = 0;
                    for (const auto& group : node.argGroups) {
                        for (const auto& arg : group) {
                            if (std::holds_alternative<ast::CurryPlaceholder>(arg->kind)) {
                                ++open;
                            } else {
                                inferExpr(*arg);
                            }
                        }
                    }
                    std::vector<TypePtr> params(open, Type::unknown());
                    return params.empty() ? Type::unknown()
                                          : Type::func(std::move(params), Type::unknown());
                }

                if (node.argGroups.empty()) {
                    std::vector<const Signature*> distinct;
                    for (const auto& signature : candidates) {
                        const bool duplicate = std::any_of(
                            distinct.begin(), distinct.end(), [&](const Signature* other) {
                                return sameParameters(*other, signature);
                            });
                        if (!duplicate) {
                            distinct.push_back(&signature);
                        }
                    }
                    const bool sameArityAmbiguity =
                        std::any_of(distinct.begin(), distinct.end(), [&](const Signature* left) {
                            return std::any_of(
                                distinct.begin(), distinct.end(), [&](const Signature* right) {
                                    return left != right &&
                                           left->params.size() == right->params.size();
                                });
                        });
                    if (sameArityAmbiguity) {
                        std::string signatures;
                        for (const auto* signature : distinct) {
                            signatures += "\n\n" + displaySignature(node.name, *signature);
                        }
                        error(
                            diag::Code::AmbiguousOverloadReference, expr.location,
                            {{"name", node.name}, {"signatures", signatures}});
                    }
                }

                // A capture is an application of a fresh INSTANCE of the public
                // signature. This retains relationships such as A -> A -> A,
                // while binding `~add(1)` specializes the open argument/result to
                // Integer without mutating the function's declared signature.
                std::unordered_map<int, TypePtr> instantiatedVars;
                std::function<TypePtr(const TypePtr&)> instantiate =
                    [&](const TypePtr& type) -> TypePtr {
                    if (!type) {
                        return type;
                    }
                    return std::visit(
                        [&](const auto& part) -> TypePtr {
                            using Part = std::decay_t<decltype(part)>;
                            if constexpr (std::is_same_v<Part, TypeVar>) {
                                auto [it, inserted] = instantiatedVars.emplace(part.id, nullptr);
                                if (inserted) {
                                    it->second = freshTypeVar();
                                }
                                return it->second;
                            } else {
                                return mapTypeChildren(type, instantiate);
                            }
                        },
                        type->kind);
                };

                const auto& selected = candidates.front();
                if (!node.isOperator) {
                    m_captureSignatures[&expr] = selected;
                }
                std::vector<TypePtr> params;
                for (const auto& param : selected.params) {
                    params.push_back(instantiate(param));
                }
                auto resultType = instantiate(selected.result);
                std::vector<TypePtr> remaining;
                size_t position = 0;
                for (const auto& group : node.argGroups) {
                    for (const auto& arg : group) {
                        if (position >= params.size()) {
                            if (!std::holds_alternative<ast::CurryPlaceholder>(arg->kind)) {
                                inferExpr(*arg);
                            }
                            ++position;
                            continue;
                        }
                        if (std::holds_alternative<ast::CurryPlaceholder>(arg->kind)) {
                            remaining.push_back(params[position++]);
                            continue;
                        }
                        auto actual = resolve(inferExpr(*arg));
                        auto expected = resolve(params[position++]);
                        if (auto* variable = std::get_if<TypeVar>(&expected->kind)) {
                            unifyVar(variable->id, actual);
                        } else if (auto* variable = std::get_if<TypeVar>(&actual->kind)) {
                            if (!deferIfPending(actual, expected, arg->location)) {
                                unifyVar(variable->id, expected);
                            }
                        } else if (!argMatchesParam(actual, expected)) {
                            typeMismatch(arg->location, expected, actual);
                        }
                    }
                }
                while (position < params.size()) {
                    remaining.push_back(params[position++]);
                }
                for (auto& param : remaining) {
                    param = resolve(param);
                }
                resultType = resolve(resultType);

                // `~banner` names the function, even one of no parameters: a
                // `() -> String` to call later, not its result. Only arguments
                // that fill every parameter (`~add(1, 2)`) make it a call.
                if (remaining.empty() && !node.argGroups.empty()) {
                    return resultType;
                }
                return Type::func(std::move(remaining), resultType);
            } else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
                // Inside a make block, `this` / `@field` has the record type.
                if (m_currentMakeType) {
                    return m_currentMakeType;
                }
                // Anywhere else there is no receiver to bind. A module-level
                // `let` mentioning `this` used to pass here as Unknown and fail
                // only in the backends — erlc rejecting it as an unbound
                // variable, the walker misreporting a sibling member — so the
                // checker says it where it is written (kexhq/kex#293).
                error(diag::Code::ThisWithoutReceiver, expr.location);
                return Type::unknown();
            } else {
                return Type::unknown();
            }
        },
        expr.kind);
    m_typeMap[&expr] = result;
    return result;
}

auto TypeChecker::inferBinaryOp(
    TokenType op, const TypePtr& left, const TypePtr& right, SourceLocation loc) -> TypePtr {
    // Type predicates used both in the TypeVar bail-out and the concrete section.
    auto isString = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::String;
    };
    auto isChar = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Char;
    };
    const auto isNumeric = isNumericType;

    // Resolve TypeVars through the substitution map — a prior operation in the
    // same body may have already constrained them (e.g. `n - 1` constrains n
    // to Number before `n == 0` is checked).
    auto lhs = resolve(left);
    auto rhs = resolve(right);

    // A type variable the function's own signature writes stands for every
    // type, and no operator but equality is defined for every type: `a + 1`
    // on `a: A` decided that `A` is a number (kexhq/kex#644).
    // A written `Any` is the top type (decision 7): the same holds of it.
    if (op != TokenType::EqEq && op != TokenType::NotEq) {
        for (const auto& operand : {lhs, rhs}) {
            if (const auto id = typeVariableId(operand); id && m_rigidVars.contains(*id)) {
                error(
                    diag::Code::NotDefinedForEveryType, loc,
                    {{"name", std::string(tokenTypeName(op))}, {"type", m_rigidVars.at(*id)}});

                return Type::unknown();
            }
            if (const auto* unknown = std::get_if<UnknownType>(&operand->kind);
                unknown && unknown->written) {
                error(diag::Code::AnyNotNarrowed, loc, {{"name", std::string(tokenTypeName(op))}});
                return Type::unknown();
            }
        }
    }

    {
        auto lhsIsVar = std::holds_alternative<TypeVar>(lhs->kind);
        auto rhsIsVar = std::holds_alternative<TypeVar>(rhs->kind);
        auto lhsIsUnk = std::holds_alternative<UnknownType>(lhs->kind);
        auto rhsIsUnk = std::holds_alternative<UnknownType>(rhs->kind);

        if (lhsIsVar || rhsIsVar || lhsIsUnk || rhsIsUnk) {
            // The id of an inference variable, or -1 for anything else (a
            // generic letter included, which is no unknown to bind).
            auto varId = [](const TypePtr& t) -> int {
                if (auto* tv = std::get_if<TypeVar>(&t->kind); tv && isInferenceVariable(*tv)) {
                    return tv->id;
                }
                return -1;
            };
            auto isConcrete = [](const TypePtr& t) {
                return !std::holds_alternative<TypeVar>(t->kind) &&
                       !std::holds_alternative<UnknownType>(t->kind);
            };

            switch (op) {
                // -, *, /, %, ^: both operands must be Number.
                case TokenType::Minus:
                case TokenType::Star:
                case TokenType::Slash:
                case TokenType::Percent:
                case TokenType::Caret: {
                    auto nc = Type::constrained("N", "Number");
                    if (int id = varId(lhs); id >= 0) {
                        unifyVar(id, nc);
                    }
                    if (int id = varId(rhs); id >= 0) {
                        unifyVar(id, nc);
                    }
                    if (isConcrete(lhs)) {
                        return lhs;
                    }
                    // A left operand that is UNKNOWN (as opposed to a type
                    // variable the substitution can still learn about) makes
                    // the right one no evidence at all: `3.meter ^ 2` is a
                    // Measure, and typing it as the exponent's Integer sent
                    // the following `.value` to Integer, where it became a
                    // bogus arity error against an unrelated prelude method.
                    if (lhsIsUnk) {
                        return Type::unknown();
                    }
                    if (isConcrete(rhs)) {
                        return rhs;
                    }
                    return resolve(lhs);
                }
                // +: String/Char on one side → constrain TypeVar to String;
                //    numeric on one side → constrain TypeVar to Number.
                case TokenType::Plus: {
                    if (isConcrete(lhs) && (isString(lhs) || isChar(lhs))) {
                        if (int id = varId(rhs); id >= 0) {
                            unifyVar(id, Type::string());
                        }
                        return Type::string();
                    }
                    if (isConcrete(rhs) && (isString(rhs) || isChar(rhs))) {
                        if (int id = varId(lhs); id >= 0) {
                            unifyVar(id, Type::string());
                        }
                        return Type::string();
                    }
                    if (isConcrete(lhs) && isNumeric(lhs)) {
                        if (int id = varId(rhs); id >= 0) {
                            unifyVar(id, Type::constrained("N", "Number"));
                        }
                        return lhs;
                    }
                    if (isConcrete(rhs) && isNumeric(rhs)) {
                        if (int id = varId(lhs); id >= 0) {
                            unifyVar(id, Type::constrained("N", "Number"));
                        }
                        return rhs;
                    }
                    // With two unconstrained operands, `a + b` still proves
                    // that both operands and the result have one shared type.
                    // Keeping them independent leaked signatures such as
                    // `T7 -> T8 -> T7` for the ordinary generic `add`.
                    if (int lhsId = varId(lhs); lhsId >= 0) {
                        if (varId(rhs) >= 0) {
                            unifyVar(varId(rhs), lhs);
                        }
                        return resolve(Type::inferenceVariable(lhsId));
                    }
                    return lhs;
                }
                // Ordered comparisons: constrain TypeVar to the concrete side.
                case TokenType::LessThan:
                case TokenType::GreaterThan:
                case TokenType::LessEq:
                case TokenType::GreaterEq: {
                    if (int id = varId(lhs); id >= 0 && isConcrete(rhs)) {
                        unifyVar(id, rhs);
                    }
                    if (int id = varId(rhs); id >= 0 && isConcrete(lhs)) {
                        unifyVar(id, lhs);
                    }
                    return Type::boolean();
                }
                case TokenType::EqEq:
                case TokenType::NotEq: return Type::boolean();
                case TokenType::AmpAmp:
                case TokenType::PipePipe: {
                    if (int id = varId(lhs); id >= 0) {
                        unifyVar(id, Type::boolean());
                    }
                    if (int id = varId(rhs); id >= 0) {
                        unifyVar(id, Type::boolean());
                    }
                    return Type::boolean();
                }
                default: return lhs;
            }
        }
    }

    // A `Float` parameter is typed by the `Float` bound, not a concrete float.
    // Read as neither, `count * price` on `count: Integer, price: Float` fell to
    // `return lhs` and was typed Integer, and `count + price` was refused as
    // two different types, though arithmetic promotes (docs/language-spec.md
    // §23.6) and `3 * 2.5` between bindings is a Float (kexhq/kex#766).
    auto isFloat = [](const TypePtr& t) {
        if (const auto* bound = std::get_if<ConstrainedType>(&t->kind)) {
            return bound->traitName == kex::names::kFloatType;
        }
        return std::holds_alternative<SizedFloatType>(t->kind);
    };
    auto isIntegerLike = [](const TypePtr& t) {
        if (auto* prim = std::get_if<PrimitiveType>(&t->kind)) {
            return prim->kind == PrimitiveType::Integer;
        }
        return std::holds_alternative<SizedIntType>(t->kind);
    };
    auto isBool = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Bool;
    };

    bool leftIsString = isString(lhs), rightIsString = isString(rhs);
    bool leftIsFloat = isFloat(lhs), rightIsFloat = isFloat(rhs);
    bool leftIsInt = isIntegerLike(lhs), rightIsInt = isIntegerLike(rhs);
    // A `Number` bound (`x: Number`) meets the promotion table too. With a
    // Float it is a Float: `x * 2.5` was typed as `x`'s own type, so on an
    // Integer `x` it claimed an Integer that is 7.5 (the plan's §11, item 8).
    // With an Integer it stays the bound, the Integer adapting to it as a
    // literal does (decision 2).
    const auto isNumberBound = [](const TypePtr& t) {
        const auto* bound = std::get_if<ConstrainedType>(&t->kind);
        return bound && bound->traitName == kex::names::kNumberTrait;
    };
    const auto promoted = [&]() -> TypePtr {
        const bool leftNumber = leftIsInt || leftIsFloat || isNumberBound(lhs);
        const bool rightNumber = rightIsInt || rightIsFloat || isNumberBound(rhs);
        if (!leftNumber || !rightNumber) {
            return nullptr;
        }
        if (leftIsFloat || rightIsFloat) {
            return Type::float64();
        }
        if (isNumberBound(lhs) && rightIsInt) {
            return lhs;
        }
        if (leftIsInt && isNumberBound(rhs)) {
            return rhs;
        }
        return nullptr;
    };

    switch (op) {
        case TokenType::Plus:
            if (leftIsInt && rightIsInt) {
                return Type::integer();
            }
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) &&
                (rightIsFloat || rightIsInt)) {
                return Type::float64();
            }
            if (auto number = promoted()) {
                return number;
            }
            if (leftIsString && rightIsString) {
                return Type::string();
            }
            // String + Char, Char + String, Char + Char — all produce String
            // (String = [Char], so appending a Char or two Chars is valid concatenation).
            if ((leftIsString || isChar(lhs)) && (rightIsString || isChar(rhs))) {
                return Type::string();
            }
            if (leftIsString || rightIsString) {
                error(
                    diag::Code::CannotAdd, loc,
                    {{"left", typeToString(lhs)}, {"right", typeToString(rhs)}});
                return Type::unknown();
            }
            if (!argMatchesParam(lhs, rhs) && !argMatchesParam(rhs, lhs)) {
                error(
                    diag::Code::PlusOperandsDiffer, loc,
                    {{"left", typeToString(lhs)}, {"right", typeToString(rhs)}});
                // Said once: a guess at the sum would be reported again where it goes.
                return Type::unknown();
            }
            return lhs;

        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
        case TokenType::Caret:
            if (leftIsInt && rightIsInt) {
                return Type::integer();
            }
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) &&
                (rightIsFloat || rightIsInt)) {
                return Type::float64();
            }
            if (auto number = promoted()) {
                return number;
            }
            if (leftIsString || rightIsString) {
                error(diag::Code::ArithmeticOnString, loc);
                return Type::unknown();
            }
            // A Bool, Atom, Char or Void has no arithmetic either: `:kex - 7` and
            // `true * :kex` were typed as their left operand and failed on both
            // backends (found by `make fuzz-metamorphic`).
            if (const auto nonNumber =
                    [](const TypePtr& t) {
                        return std::holds_alternative<PrimitiveType>(t->kind) && !isNumericType(t);
                    };
                nonNumber(lhs) || nonNumber(rhs)) {
                error(
                    diag::Code::ArithmeticRequiresNumbers, loc,
                    {{"op", std::string(tokenTypeName(op))},
                     {"left", typeToString(lhs)},
                     {"right", typeToString(rhs)}});
                return Type::unknown();
            }
            return lhs;

        case TokenType::EqEq:
        case TokenType::NotEq: return Type::boolean();

        case TokenType::LessThan:
        case TokenType::GreaterThan:
        case TokenType::LessEq:
        case TokenType::GreaterEq:
            if (isBool(lhs)) {
                error(diag::Code::OrderingOnBool, loc);
            } else if (!isNumeric(lhs) || !isNumeric(rhs)) {
                // Two values of unrelated types have no order: `i > "kex"` was
                // accepted and failed on both backends with the message
                // reported here (found by `make fuzz-metamorphic`).
                if (!containsOpenType(lhs) && !containsOpenType(rhs) &&
                    !argMatchesParam(lhs, rhs) && !argMatchesParam(rhs, lhs)) {
                    error(
                        diag::Code::CannotCompare, loc,
                        {{"left", typeToString(lhs)}, {"right", typeToString(rhs)}});
                }
            }
            return Type::boolean();

        case TokenType::AmpAmp:
        case TokenType::PipePipe:
            if (!isBool(lhs)) {
                error(diag::Code::LogicalOperatorNotBool, loc, {{"type", typeToString(lhs)}});
            }
            if (!isBool(rhs)) {
                error(diag::Code::LogicalOperatorNotBool, loc, {{"type", typeToString(rhs)}});
            }
            return Type::boolean();

        default: return Type::unknown();
    }
}

// `Type.of(expr)` / `Type.returnedBy(fn)` written where a TYPE goes. The
// value-level forms answer at runtime from a recording made here; this one is
// resolved entirely at check time, because there is no value to fall back on.
// The type an expression NAMES, or null when it does not name one. A bare
// `Hello` is a type reference, not a value: it is exactly what `x.to(String)`
// already relies on. A CONSTRUCTOR is deliberately excluded — `World` is a
// value whose type is `Hello`.
auto TypeChecker::typeNameReference(const ast::Expr& expr) -> TypePtr {
    const auto* named = std::get_if<ast::UpperIdentifier>(&expr.kind);
    return named ? typeNamed(named->name, expr.location) : nullptr;
}

auto TypeChecker::typeNamed(const std::string& name, SourceLocation location) -> TypePtr {
    if (m_nullaryConstructors.count(name) || m_adtOfConstructor.count(name)) {
        return nullptr;
    }
    // A module's constructor is a value too, not the name of a type: taken
    // for one, `Type.of(Public)` answered `Public` (kexhq/kex#505).
    for (const auto& [_, constructors] : m_moduleConstructors) {
        if (constructors.count(name)) {
            return nullptr;
        }
    }
    ast::TypeName typeName;
    typeName.parts = {name};
    ast::TypeExpr asType;
    asType.location = location;
    asType.kind = std::move(typeName);
    std::unordered_map<std::string, TypePtr> generics;
    // Not resolveTypeExpr: that records the annotation BY ADDRESS for
    // lowering and tooling, and this one is a temporary. Its key outlived
    // it, and the name index read a freed location (a language-server
    // crash on examples/json_parser.kex).
    return resolveTypeExprNode(asType, generics);
}

// A parameter written as a bare type name selects that type: the conversion
// protocol's `let to(String)` takes the type `String` itself, a `Type<String>`.
auto TypeChecker::typeSelectorParameter(const ast::Param& param) -> TypePtr {
    if (param.type || !param.pattern || !*param.pattern) {
        return nullptr;
    }
    const auto* selector = std::get_if<ast::ConstructorPattern>(&(*param.pattern)->kind);
    if (!selector || !selector->args.empty()) {
        return nullptr;
    }
    // A constructor is a value pattern, whatever imports it: `faults(Name, ...)`
    // with `using App.Models.User, only: [Name]` matches `UserDetail.Name`.
    if (!namesKnownType(selector->name) || nullaryConstructorType(selector->name)) {
        return nullptr;
    }
    auto named = typeNamed(selector->name, (*param.pattern)->location);
    if (!named || std::holds_alternative<UnknownType>(resolve(named)->kind)) {
        return nullptr;
    }
    return Type::named(kex::names::kTypeRecord, {named});
}

// What is wrong with `written.method(...)` as a namespace call, if anything:
// `written` names no module or type at all, or names a local module that has
// no member `method`. `resolved` is the same path after `using` expansion.
//
// Generous on purpose — a false error blocks a valid program. A path may mean
// a module through a `using ..., as:` alias, relative to an enclosing module
// (`Receipt` inside `Shop`), or through an `export` re-namespacing; it is fine
// when ANY module could be what it means (equal to it, inside it, or
// containing it: `Kex.VERSION.number` continues past its module), and when its
// root is anything else an uppercase name can be: a type, a constructor, a
// constant, a variable, a capability, a trait, or a syntax-level namespace.
// Only a module compiled in this unit is checked for the member, since only
// there is its whole source at hand.
auto TypeChecker::namespaceCallProblem(
    const std::string& written, const std::string& resolved, const std::string& method)
    -> std::optional<std::pair<diag::Code, diag::Args>> {
    const auto rootEnd = written.find('.');
    const auto root = written.substr(0, rootEnd);
    const auto tail = rootEnd == std::string::npos ? std::string{} : written.substr(rootEnd);

    // Each candidate with the enclosing scope it was spelled relative to: a
    // relative spelling only means something BELOW that scope — `UserService`
    // inside `App` is not "somewhere in App".
    struct Candidate {
        std::string path;
        std::size_t scope = 0;
    };
    std::vector<Candidate> candidates{{resolved, 0}};
    const auto addAliases = [&](const std::vector<ImportSelection>& imports) {
        for (const auto& import : imports) {
            if (import.alias && *import.alias == root) {
                candidates.push_back({import.module + tail, 0});
            }
        }
    };
    addAliases(m_declarationImports);
    for (const auto& scope : m_importScopeStack) {
        addAliases(scope);
    }
    for (auto scope = m_currentModulePath; !scope.empty();) {
        candidates.push_back({scope + "." + resolved, scope.size()});
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) {
            break;
        }
        scope.resize(dot);
    }

    std::size_t minimumModule = 0;
    const auto relatedTo = [&](const std::string& path, const std::string& module) {
        if (module.size() <= minimumModule) {
            return false;
        }
        return module == path || path.rfind(module + ".", 0) == 0 ||
               module.rfind(path + ".", 0) == 0;
    };
    // `export Prelude, only: [...]` re-namespaces a module under its parent;
    // its members are the original module's, so a path through one is left
    // alone rather than checked against the parent.
    // Only a path THROUGH one: `Shapes.Geometry.area`, not `Shapes.area`
    // beside `export Geometry`, which neither backend finds (kexhq/kex#792).
    const auto reexported = [&](const std::string& path) {
        return std::any_of(
            m_moduleReexports.begin(), m_moduleReexports.end(), [&](const auto& entry) {
                return path == entry.first || path.rfind(entry.first + ".", 0) == 0;
            });
    };
    const auto moduleKnown = [&](const std::string& path) {
        for (const auto& module : m_localModules) {
            if (relatedTo(path, module)) {
                return true;
            }
        }
        for (const auto& module : m_importedModulePaths) {
            if (relatedTo(path, module)) {
                return true;
            }
        }
        if (reexported(path)) {
            return true;
        }
        if (m_importedInterfaces) {
            for (const auto& [module, _] : m_importedInterfaces->modules) {
                if (relatedTo(path, module) ||
                    (module.rfind("Kex.", 0) == 0 && relatedTo(path, module.substr(4)))) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto match =
        std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& candidate) {
            minimumModule = candidate.scope;
            const bool known = moduleKnown(candidate.path);
            minimumModule = 0;
            return known;
        });

    if (match == candidates.end()) {
        // An uppercase CONSTANT (`let NUMS = [...]`, `let EARLIEST_YEAR =
        // 1990`) is a zero-arity function, at the top level or in this module
        // or one enclosing it; a method call on it (`NUMS.length`, the `show`
        // behind "${EARLIEST_YEAR}") has it as the root.
        const auto constantInScope = [&](const std::string& name) {
            if (m_userSignatures.count(name) ||
                m_scopedDeclaredSignatures.count(m_currentModulePath + "\n" + name)) {
                return true;
            }
            for (auto scope = m_currentModulePath; !scope.empty();) {
                if (m_userSignatures.count(kex::mangle::qualify(scope, name))) {
                    return true;
                }
                const auto dot = scope.rfind('.');
                if (dot == std::string::npos) {
                    break;
                }
                scope.resize(dot);
            }
            // Or one a `using` brings in from a module compiled on its own.
            if (m_importedInterfaces) {
                for (const auto& [module, interface] : m_importedInterfaces->modules) {
                    if (auto exported = interface.exports.find(name);
                        exported != interface.exports.end() && moduleMemberImported(module, name)) {
                        return true;
                    }
                }
            }
            return false;
        };
        // A record the root names in reach: not one of an unimported module
        // that only the suffix guess finds (`Env` for `Mock.Env`, so
        // `Env.get` checked clean and was undefined at run time,
        // kexhq/kex#562).
        const bool recordInReach = [&] {
            const auto record = resolveRecordName(root);
            if (!m_recordFields.count(record)) {
                return false;
            }
            if (record == root) {
                return true;
            }
            const auto module = kex::mangle::scopeOf(record);
            return withinModule(module) || moduleMemberImported(module, root);
        }();
        const bool rootIsSomething =
            root == names::kForeignRoot || root == names::kRuntimeRoot ||
            root == names::kThisType || isPrimitiveTypeName(root) || recordInReach ||
            m_adtVariants.count(root) || m_typeAliases.count(root) ||
            m_nullaryConstructors.contains(root) || constructorResultType(root, {}) != nullptr ||
            m_capabilities.count(root) || m_traits.get(root) != nullptr ||
            lookupVar(root) != nullptr || constantInScope(root) ||
            std::any_of(
                m_moduleConstructors.begin(), m_moduleConstructors.end(),
                [&](const auto& entry) { return entry.first == root || entry.second.count(root); });
        if (rootIsSomething) {
            return std::nullopt;
        }
        return std::pair{diag::Code::UnknownModule, diag::Args{{"module", written}}};
    }
    const auto& path = match->path;
    if (reexported(path)) {
        return std::nullopt;
    }

    // Past the deepest local module it names, the next segment must be
    // something that module holds: `Shop.Inner` is known only if `Shop`
    // declares `Inner`.
    std::string deepest;
    for (const auto& module : m_localModules) {
        if (path.rfind(module + ".", 0) == 0 && module.size() > deepest.size()) {
            deepest = module;
        }
    }
    // A nested module compiled on its own (`App.Views.Chat.Threads` beside
    // `App.Views.Chat`) is held there as well, through its interface.
    const auto holds = [&](const std::string& module) {
        return module == path || module.rfind(path + ".", 0) == 0;
    };
    const bool importedHere =
        m_importedInterfaces &&
        std::any_of(
            m_importedInterfaces->modules.begin(), m_importedInterfaces->modules.end(),
            [&](const auto& entry) { return holds(entry.first); });
    if (!deepest.empty() && !importedHere &&
        !std::any_of(m_localModules.begin(), m_localModules.end(), holds)) {
        const auto rest = path.substr(deepest.size() + 1);
        const auto next = rest.substr(0, rest.find('.'));
        // An uppercase module CONSTANT (`Console.RED`, which string
        // interpolation calls a method on) is held like a nested module.
        const bool held =
            !kex::startsUppercase(next) ||
            m_userSignatures.count(kex::mangle::qualify(deepest, next)) ||
            m_scopedDeclaredSignatures.count(deepest + "\n" + next) ||
            m_recordFields.count(resolveRecordName(next)) ||
            m_recordFields.count(deepest + "." + next) || m_adtVariants.count(next) ||
            m_typeAliases.count(next) || m_nullaryConstructors.contains(next) ||
            constructorResultType(next, {}) != nullptr ||
            (m_moduleConstructors.count(deepest) && m_moduleConstructors.at(deepest).count(next));
        if (!held) {
            return std::pair{
                diag::Code::ModuleHasNoModule, diag::Args{{"module", deepest}, {"name", next}}};
        }
    }

    // A member of a module compiled here.
    if (!m_localModules.contains(path) || method.empty() ||
        !std::islower(static_cast<unsigned char>(method.front()))) {
        return std::nullopt;
    }
    if (m_importedInterfaces && m_importedInterfaces->modules.count(path)) {
        return std::nullopt;
    }
    if (moduleNamesType(path)) {
        return std::nullopt;
    }
    if (m_userSignatures.count(kex::mangle::qualify(path, method)) ||
        m_scopedDeclaredSignatures.count(path + "\n" + method) ||
        m_localModules.contains(path + "." + method)) {
        return std::nullopt;
    }
    if (auto constructors = m_moduleConstructors.find(path);
        constructors != m_moduleConstructors.end() && constructors->second.count(method)) {
        return std::nullopt;
    }
    std::vector<std::string> members;
    const auto prefix = path + std::string(kex::mangle::kSeparator);
    for (const auto& [name, _] : m_userSignatures) {
        if (name.rfind(prefix, 0) == 0) {
            members.push_back(name.substr(prefix.size()));
        }
    }
    std::sort(members.begin(), members.end());
    if (auto hint = closestName(method, members); !hint.empty()) {
        return std::pair{
            diag::Code::ModuleHasNoFunctionDidYouMean,
            diag::Args{{"module", written}, {"name", method}, {"hint", hint}}};
    }
    return std::pair{
        diag::Code::ModuleHasNoFunction, diag::Args{{"module", written}, {"name", method}}};
}

auto TypeChecker::moduleNamesType(const std::string& path) const -> bool {
    const auto last = kex::mangle::memberOf(path);
    return isPrimitiveTypeName(last) || m_recordFields.count(resolveRecordName(last)) ||
           m_recordFields.count(path) || m_adtVariants.count(last) || m_typeAliases.count(last);
}

// The single signature of the function an expression NAMES, or null when the
// expression is not a function name (or names an overload set).
auto TypeChecker::namedFunctionSignature(const ast::Expr& expr) -> const Signature* {
    std::string functionName;
    if (const auto* identifier = std::get_if<ast::Identifier>(&expr.kind)) {
        functionName = identifier->name;
    } else if (const auto* qualified = std::get_if<ast::MethodCall>(&expr.kind);
               qualified && qualified->args.empty() && qualified->receiver &&
               !qualified->parenthesized) {
        if (auto module = ast::writtenModulePath(*qualified->receiver)) {
            functionName = kex::mangle::qualify(*module, qualified->method);
        }
    }
    if (functionName.empty()) {
        return nullptr;
    }
    // The reference names the function it found; it calls nothing.
    const auto named = [&](const Signature* signature) {
        if (std::holds_alternative<ast::MethodCall>(expr.kind)) {
            m_decidedCalls[&expr] = {CallKind::FunctionReference, functionName};
        }
        return signature;
    };
    if (auto user = m_userSignatures.find(functionName);
        user != m_userSignatures.end() && user->second.size() == 1) {
        return named(&user->second.front());
    }
    m_importedSignatureCache = importedCandidateSignatures(functionName);
    if (m_importedSignatureCache.size() == 1) {
        return named(&m_importedSignatureCache.front());
    }
    return nullptr;
}

auto TypeChecker::declaredFunctionType(const std::string& name) -> TypePtr {
    // The function this name means where it is written: in the current module
    // or one enclosing it, else at the top level. One declaration, one clause,
    // every parameter and the result annotated.
    const ast::FunctionDef* function = nullptr;
    const auto unique = [&](const std::string& key) -> bool {
        const auto [first, last] = m_functionDeclarations.equal_range(key);
        if (first == last) {
            return false;
        }
        if (std::next(first) == last) {
            function = first->second;
        }
        return true;
    };
    bool found = false;
    for (auto scope = m_currentModulePath; !scope.empty() && !found;) {
        found = unique(kex::mangle::qualify(scope, name));
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) {
            break;
        }
        scope.resize(dot);
    }
    if (!found) {
        unique(name);
    }
    if (!function || ast::signatureClauses(*function).size() != 1) {
        return nullptr;
    }
    const auto& clause = *ast::signatureClauses(*function).front();
    if (!clause.returnAnnotation || !*clause.returnAnnotation) {
        return nullptr;
    }
    std::unordered_map<std::string, TypePtr> generics;
    std::vector<TypePtr> params;
    for (const auto& param : clause.params) {
        if (!param.type || !*param.type) {
            return nullptr;
        }
        params.push_back(resolveTypeExpr(**param.type, generics));
    }
    if (params.empty()) {
        return nullptr;
    }
    auto type = Type::func(std::move(params), resolveTypeExpr(**clause.returnAnnotation, generics));
    // A foul function's type says so: what is passed where it is expected
    // may perform effects (#640).
    std::get<FuncType>(type->kind).foul = function->isFoul;
    return type;
}

auto TypeChecker::resolveTypeQuery(const ast::TypeQuery& query) -> TypePtr {
    if (!query.argument) {
        return Type::unknown();
    }
    if (query.query == "of") {
        if (auto referenced = typeNameReference(*query.argument)) {
            return referenced;
        }
        // A function named here (`Type.of(handlerType)`) before its signature
        // is registered, as a computed alias is resolved: its written
        // annotations are its type. Inferred, it was Unknown.
        if (const auto* identifier = std::get_if<ast::Identifier>(&query.argument->kind)) {
            if (auto declared = declaredFunctionType(identifier->name)) {
                return declared;
            }
        }
        return resolve(inferExpr(*query.argument));
    }

    // returnedBy: the argument NAMES a function.
    std::string functionName;
    if (const auto* identifier = std::get_if<ast::Identifier>(&query.argument->kind)) {
        functionName = identifier->name;
    } else if (const auto* qualified = std::get_if<ast::MethodCall>(&query.argument->kind);
               qualified && qualified->args.empty() && qualified->receiver) {
        if (auto module = ast::writtenModulePath(*qualified->receiver)) {
            functionName = kex::mangle::qualify(*module, qualified->method);
        }
    }

    std::vector<Signature> candidates;
    if (!functionName.empty()) {
        if (auto user = m_userSignatures.find(functionName); user != m_userSignatures.end()) {
            candidates = user->second;
        }
        if (candidates.empty()) {
            candidates = importedCandidateSignatures(functionName);
        }
    }
    if (candidates.empty()) {
        // A computed ALIAS resolves before signatures are registered (the
        // signatures may themselves name the alias), so fall back to the
        // declaration's own return annotation.
        // Overloads folded into one function count once per signature.
        std::size_t declaredSignatures = 0;
        const auto [first, last] = m_functionDeclarations.equal_range(functionName);
        for (auto it = first; it != last; ++it) {
            declaredSignatures += ast::signatureClauses(*it->second).size();
        }
        if (declaredSignatures > 1) {
            error(
                diag::Code::ReturnedByOverloaded, query.argument->location,
                {{"name", sourceName(functionName)}, {"signatures", ""}});
            return Type::unknown();
        }
        if (auto declaration = m_functionDeclarations.find(functionName);
            declaration != m_functionDeclarations.end()) {
            const auto* function = declaration->second;
            if (!function->clauses.empty() && function->clauses.front().returnAnnotation) {
                std::unordered_map<std::string, TypePtr> generics;
                return resolveTypeExpr(**function->clauses.front().returnAnnotation, generics);
            }
            error(
                diag::Code::ReturnedByUndeclaredReturn, query.argument->location,
                {{"name", sourceName(functionName)}});
            return Type::unknown();
        }
        error(diag::Code::ReturnedByNeedsName, query.argument->location);
        return Type::unknown();
    }
    if (candidates.size() > 1) {
        std::string signatures;
        for (const auto& candidate : candidates) {
            signatures += "\n\n" + displaySignature(functionName, candidate);
        }
        error(
            diag::Code::ReturnedByOverloaded, query.argument->location,
            {{"name", sourceName(functionName)}, {"signatures", signatures}});

        return Type::unknown();
    }
    return candidates.front().result;
}

// The type a `let`/`var` annotation declares, after checking the initializer
// against it. Null when there is no annotation.
auto TypeChecker::declaredBindingType(
    const std::optional<ast::TypeExprPtr>& annotation,
    const TypePtr& valueType,
    SourceLocation loc,
    const ast::Expr* value) -> TypePtr {
    if (!annotation || !*annotation) {
        return nullptr;
    }
    std::unordered_map<std::string, TypePtr> generics;
    auto declared = resolveTypeExpr(**annotation, generics);
    if (!declared) {
        return nullptr;
    }
    auto actual = resolve(valueType);
    // `let handler: Handler = ~home` may name a foul function (#640).
    noteCaptureFlow(value, declared);
    // A gradual initializer (`?`, a type variable) is accepted: pinning it
    // down is exactly what the annotation is for. `This` — the trait
    // placeholder — has no substitution mechanism yet and leaks out of trait
    // default methods as a literal type name, so it is treated the same way
    // (checkCall bails on it for the same reason).
    const bool unsubstitutedThis = isThisPlaceholder(actual);
    if (deferIfPending(actual, declared, loc)) {
        return declared;
    }
    // `let x: Tuplex = (5, 11)`: a tuple written out under a distinct type's
    // annotation is that type, as `(5, 11).as(Tuplex)` would make it, when
    // the backing type admits it. Only a literal: a tuple computed elsewhere
    // still converts with `.as`, so a distinct type is never entered by
    // accident.
    if (value && std::holds_alternative<ast::TupleExpr>(value->kind)) {
        if (const auto backing = distinctBacking(declared);
            backing && !typesEqual(backing, declared) && argMatchesParam(actual, backing)) {
            return declared;
        }
    }
    // Reported at the initializer, with the annotation the expected type came
    // from as a second place (kexhq/kex#657).
    const auto mismatch = [&](const TypePtr& found) {
        typeMismatch(value ? value->location : loc, declared, found);
        labelLast(
            "this has type " + typeToString(found), (*annotation)->location,
            "expected because of this annotation");
    };
    if (value && literalOutsideSet(*value, declared)) {
        mismatch(Type::atom(std::get<ast::AtomLiteral>(value->kind).name));
        return declared;
    }
    if (value && literalCoercesTo(*value, declared)) {
        noteFloatLiteral(*value, declared);
        return declared;
    }
    // A distinct type bound as a trait it implements itself: a later call
    // dispatches on the value, which is its backing (§14 item 4).
    if (const auto trait = distinctTraitClash(actual, declared); !trait.empty()) {
        mismatch(actual);
        return declared;
    }
    // A written `Any` is no gradual value: it is the top type, and a binding
    // declared narrower takes it only through a type pattern (decision 7).
    const auto* unknown = std::get_if<UnknownType>(&actual->kind);
    if ((!unknown || unknown->written) && !std::holds_alternative<TypeVar>(actual->kind) &&
        !unsubstitutedThis && !argMatchesParam(actual, declared)) {
        mismatch(actual);
    } else if (!unsubstitutedThis) {
        // Accepted: what the value leaves open is the declared type's (HM):
        // `let xs: [Integer] = []` makes the empty list's element an Integer.
        unify(actual, declared, /*bindRight=*/false);
    }
    return declared;
}

// The type a standalone `name : T` declaration gives a top-level constant,
// after checking its definition against it.
auto TypeChecker::declaredConstantType(
    const std::string& name, const TypePtr& valueType, SourceLocation loc) -> TypePtr {
    auto declared = m_userSignatures.find(name);
    if (declared == m_userSignatures.end() || declared->second.size() != 1) {
        return nullptr;
    }
    const auto& signature = declared->second.front();
    if (!signature.params.empty() || !signature.result) {
        return nullptr;
    }
    auto actual = resolve(valueType);
    const bool unsubstitutedThis = isThisPlaceholder(actual);
    if (deferIfPending(actual, signature.result, loc)) {
        return signature.result;
    }
    if (!std::holds_alternative<UnknownType>(actual->kind) &&
        !std::holds_alternative<TypeVar>(actual->kind) && !unsubstitutedThis &&
        !argMatchesParam(actual, signature.result)) {
        typeMismatch(loc, signature.result, actual);
    } else if (!unsubstitutedThis) {
        unify(actual, signature.result, /*bindRight=*/false);
    }
    return signature.result;
}

auto TypeChecker::displayTypeOf(const ast::Expr* expr) const -> TypePtr {
    std::function<TypePtr(const TypePtr&)> widen = [&](const TypePtr& type) -> TypePtr {
        if (!type) {
            return type;
        }
        auto widenAll = [&](std::vector<TypePtr> types) {
            for (auto& element : types) {
                element = widen(element);
            }
            return types;
        };
        if (auto* named = std::get_if<NamedType>(&type->kind)) {
            // A parameterized name is a type application, not a nullary ADT
            // constructor occurrence.  In particular, Process<Message> must
            // not widen through the unrelated prelude constructor `Process`
            // to its owning `Feature` ADT in editor-facing types.
            // A variant widens to its ADT.
            if (!named->variant.empty()) {
                return Type::named(named->name);
            }
            if (const auto* owner = constructorOwner(*named); owner && *owner != named->name) {
                return Type::named(*owner);
            }
            // Type ARGUMENTS are left alone: a phantom typestate parameter is
            // spelled with constructors too (`FileHandle<Write>`), and
            // widening those to their ADT (`FileHandle<WritePermission>`)
            // erases exactly the distinction the parameter exists to make.
            return type;
        }
        if (auto* list = std::get_if<ListType>(&type->kind)) {
            return Type::list(widen(list->element));
        }
        if (auto* optional = std::get_if<OptionalType>(&type->kind)) {
            return Type::optional(widen(optional->inner));
        }
        if (auto* map = std::get_if<MapType>(&type->kind)) {
            return Type::map(widen(map->key), widen(map->value));
        }
        if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
            return Type::tuple(widenAll(tuple->elements));
        }
        return type;
    };
    return widen(typeOf(expr));
}

auto TypeChecker::satisfiesTrait(const TypePtr& type, const std::string& traitName) const -> bool {
    if (m_traits.satisfies(type, traitName)) {
        return true;
    }
    // A nullary constructor's value type is the constructor name (`Dog`,
    // `Meter`), but the conformance is declared on the ADT that owns it
    // (`make Animal, implement: Speaker`). Lift to the owner and retry —
    // otherwise `describe(Dog)` rejects a value the trait plainly covers.
    auto* named = std::get_if<NamedType>(&type->kind);
    if (!named) {
        return false;
    }
    // A variant's conformance is its ADT's.
    if (!named->variant.empty()) {
        return m_traits.satisfies(Type::named(named->name), traitName);
    }
    auto ownerSatisfies = [&](const std::string& ownerName) {
        return ownerName != named->name && m_traits.satisfies(Type::named(ownerName), traitName);
    };
    if (const auto* owner = constructorOwner(*named); owner && ownerSatisfies(*owner)) {
        return true;
    }
    // Constructors of a `using`-imported module are registered per module
    // rather than in the file-local ADT map.
    for (const auto& [module, constructors] : m_moduleConstructors) {
        auto constructor = constructors.find(named->name);
        if (constructor != constructors.end() && moduleMemberImported(module, named->name) &&
            ownerSatisfies(constructor->second.typeName)) {
            return true;
        }
    }
    return false;
}

namespace {

// An integer literal, or one negated: what a float target takes as a float.
auto integerLiteralOf(const ast::Expr& value) -> const ast::Expr* {
    if (std::holds_alternative<ast::IntLiteral>(value.kind)) {
        return &value;
    }
    const auto* negated = std::get_if<ast::UnaryOp>(&value.kind);
    if (negated && negated->op == TokenType::Minus && negated->operand &&
        std::holds_alternative<ast::IntLiteral>(negated->operand->kind)) {
        return negated->operand.get();
    }
    return nullptr;
}

// A float type: a sized float, or the `Float` bound.
auto isFloatTarget(const TypePtr& type) -> bool {
    if (const auto* bound = std::get_if<ConstrainedType>(&type->kind)) {
        return bound->traitName == kex::names::kFloatType;
    }
    return std::holds_alternative<SizedFloatType>(type->kind);
}

} // namespace

auto TypeChecker::noteFloatLiteral(const ast::Expr& value, const TypePtr& target) -> void {
    if (!target) {
        return;
    }
    const auto resolved = resolve(target);
    if (const auto* optional = std::get_if<OptionalType>(&resolved->kind)) {
        noteFloatLiteral(value, optional->inner);
        return;
    }
    if (const auto* list = std::get_if<ListType>(&resolved->kind)) {
        if (const auto* written = std::get_if<ast::ListExpr>(&value.kind)) {
            for (const auto& element : written->elements) {
                if (element) {
                    noteFloatLiteral(*element, list->element);
                }
            }
        }
        return;
    }
    if (const auto* literal = integerLiteralOf(value); literal && isFloatTarget(resolved)) {
        m_floatLiterals.insert(literal);
    }
}

auto TypeChecker::literalCoercesTo(const ast::Expr& value, const TypePtr& target) const -> bool {
    if (!target) {
        return false;
    }
    const auto resolved = resolve(target);
    if (const auto* optional = std::get_if<OptionalType>(&resolved->kind)) {
        return literalCoercesTo(value, optional->inner);
    }
    // An integer literal is a Float where a Float is wanted: `let x: Float =
    // 1` (decision 2 of docs/typechecker-hm-plan.md). Recorded, for the
    // backends to emit a float, by noteFloatLiteral where the target is final.
    if (integerLiteralOf(value) && isFloatTarget(resolved)) {
        return true;
    }
    // A number literal is the sized number it is written for (§4.5 rule 2,
    // decision 7): `let b: UInt8 = 255`, `MAX : Float32` with a float literal.
    // Range is checked where the literal is checked against its type.
    {
        const ast::Expr* number = &value;
        if (const auto* negated = std::get_if<ast::UnaryOp>(&value.kind);
            negated && negated->operand) {
            number = negated->operand.get();
        }
        if (std::holds_alternative<ast::IntLiteral>(number->kind) &&
            std::holds_alternative<SizedIntType>(resolved->kind)) {
            return true;
        }
        if (std::holds_alternative<ast::FloatLiteral>(number->kind) &&
            std::holds_alternative<SizedFloatType>(resolved->kind)) {
            return true;
        }
    }
    if (const auto* list = std::get_if<ListType>(&resolved->kind)) {
        const auto* written = std::get_if<ast::ListExpr>(&value.kind);
        if (!written || written->rest || written->elements.empty()) {
            return false;
        }
        // Every element coerces, or already is the element type: `[1, 2.5]`
        // as a `[Float]` has one of each.
        for (const auto& element : written->elements) {
            if (!element) {
                return false;
            }
            const auto already = typeOf(element.get());
            if (!literalCoercesTo(*element, list->element) &&
                !(already && isConcreteValue(resolve(already)) &&
                  argMatchesParam(resolve(already), list->element))) {
                return false;
            }
        }
        return true;
    }
    const auto backing = distinctBacking(resolved);
    if (typesEqual(backing, resolved)) {
        return false;
    }
    const bool literal =
        std::holds_alternative<ast::IntLiteral>(value.kind) ||
        std::holds_alternative<ast::FloatLiteral>(value.kind) ||
        std::holds_alternative<ast::StringLiteral>(value.kind) || [&] {
            const auto* negated = std::get_if<ast::UnaryOp>(&value.kind);
            return negated && negated->op == TokenType::Minus && negated->operand &&
                   (std::holds_alternative<ast::IntLiteral>(negated->operand->kind) ||
                    std::holds_alternative<ast::FloatLiteral>(negated->operand->kind));
        }();
    if (!literal) {
        return false;
    }
    const auto written = typeOf(&value);
    return written && argMatchesParam(written, backing);
}

auto TypeChecker::argMatchesParam(const TypePtr& writtenArg, const TypePtr& writtenParam) const
    -> bool {
    // Each level resolved as it is compared: an unknown inside a list (`[T]`)
    // may be known by now, and left unresolved it matched any element type.
    // One known only by a bound (`n + 1` made it some Number) stays the
    // unknown it was, as before: a bound is no type to compare with.
    const auto known = [&](const TypePtr& written) -> TypePtr {
        if (!written || !std::holds_alternative<TypeVar>(written->kind)) {
            return written;
        }
        const auto resolved = resolve(written);
        return std::holds_alternative<ConstrainedType>(resolved->kind) ? written : resolved;
    };
    const auto argType = known(writtenArg);
    const auto paramType = known(writtenParam);
    // `Block<A…, R>` still spelled as a NAMED type is the block function type
    // it stands for; every resolver that knows it builds that. One spelling
    // reached a record field unconverted, and `Just({ DateTime.now() })` for
    // Mock.Clock's `onNow` was then rejected once `Just(…)` had a type.
    const auto asBlock = [](const TypePtr& type) -> TypePtr {
        const auto* named = std::get_if<NamedType>(&type->kind);
        if (!named || named->name != kex::names::kBlockType || named->typeArgs.empty()) {
            return nullptr;
        }
        std::vector<TypePtr> params(named->typeArgs.begin(), named->typeArgs.end() - 1);
        return Type::func(std::move(params), named->typeArgs.back(), true);
    };
    if (const auto block = asBlock(paramType)) {
        return argMatchesParam(argType, block);
    }
    if (const auto block = asBlock(argType)) {
        return argMatchesParam(block, paramType);
    }
    // An alias is the type it names wherever it is written: an imported
    // signature keeps `Duration?` as its module wrote it, and the alias
    // inside the optional met `Measure<Quantity.Time>` unexpanded. Distinct
    // types and records stay nominal, as expandTypeAliases keeps them.
    const auto expandedAlias = [&](const TypePtr& type) -> TypePtr {
        const auto* named = std::get_if<NamedType>(&type->kind);
        if (!named) {
            return nullptr;
        }
        const auto last = named->name.substr(named->name.rfind('.') + 1);
        if (!m_typeAliases.contains(last) || m_distinctTypes.count(resolveDistinctName(last)) ||
            m_recordFields.count(named->name)) {
            return nullptr;
        }
        auto expanded = expandTypeAliases(type);
        return typesEqual(expanded, type) ? nullptr : expanded;
    };
    if (const auto expanded = expandedAlias(paramType)) {
        return argMatchesParam(argType, expanded);
    }
    if (const auto expanded = expandedAlias(argType)) {
        return argMatchesParam(expanded, paramType);
    }
    // A written `Any` is the top type (decision 7): passed on, it fits only a
    // parameter that takes any value (a variable, `Any`, a trait every value
    // has), never a concrete one until a type pattern narrows it (#764).
    // A distinct type held as a trait it implements itself: the run time
    // would find its backing's implementation instead (§14 item 4).
    if ((std::holds_alternative<ListType>(argType->kind) ||
         std::holds_alternative<OptionalType>(argType->kind)) &&
        !distinctTraitClash(argType, paramType).empty()) {
        return false;
    }
    if (const auto* top = std::get_if<UnknownType>(&argType->kind); top && top->written) {
        const auto param = resolve(paramType);
        return isUndetermined(param) || std::holds_alternative<ConstrainedType>(param->kind);
    }
    // A letter the function being checked writes (`x: A`) is one fixed type
    // in its body: it fits only itself, or what takes any type, and nothing
    // else fits it. Open, `x` passed for an `A?` and `then x else None` made
    // an optional nothing had wrapped.
    const auto rigidLetter = [&](const TypePtr& type) {
        const auto id = typeVariableId(resolve(type));
        return id && m_rigidVars.contains(*id) ? id : std::nullopt;
    };
    const auto takesAnyType = [&](const TypePtr& type) {
        const auto resolved = resolve(type);
        const auto id = typeVariableId(resolved);
        return (id && !m_rigidVars.contains(*id)) || isUndetermined(resolved) ||
               std::holds_alternative<ConstrainedType>(resolved->kind);
    };
    if (const auto letter = rigidLetter(argType)) {
        const auto param = resolve(paramType);
        if (rigidLetter(param) == letter || takesAnyType(param)) {
            return true;
        }
        if (const auto* members = std::get_if<UnionType>(&param->kind)) {
            return std::any_of(
                members->members.begin(), members->members.end(),
                [&](const TypePtr& member) { return argMatchesParam(argType, member); });
        }
        return false;
    }
    if (rigidLetter(paramType) && !takesAnyType(argType)) {
        return false;
    }
    auto isPermissive = [](const TypePtr& t) { return isUndetermined(t); };
    if (isPermissive(argType) || isPermissive(paramType)) {
        return true;
    }
    // A type applied to arguments, a variable as its head (`F<A>`, §8.2):
    // matched head to head, then argument by argument. A head still a
    // variable takes any constructor its bound allows; a known head names one.
    if (std::holds_alternative<TypeApp>(paramType->kind) ||
        std::holds_alternative<TypeApp>(argType->kind)) {
        const auto param = applicationOf(paramType);
        auto arg = applicationOf(argType);
        if (param && arg) {
            arg = reshapedApplication(std::move(*arg), param->args.size());
        }
        if (!param || !arg || param->args.size() != arg->args.size()) {
            return false;
        }
        const auto paramHead = resolve(param->head);
        const auto argHead = resolve(arg->head);
        bool heads = false;
        if (const auto* bound = std::get_if<ConstrainedType>(&paramHead->kind)) {
            heads = paramHead == argHead || satisfiesTrait(argHead, bound->traitName);
        } else if (std::holds_alternative<TypeVar>(paramHead->kind)) {
            heads = true;
        } else {
            heads = typesEqual(paramHead, argHead);
        }
        if (!heads) {
            return false;
        }
        for (std::size_t i = 0; i < param->args.size(); ++i) {
            if (!argMatchesParam(arg->args[i], param->args[i])) {
                return false;
            }
        }
        return true;
    }
    // A distinct type goes where its backing type is expected — a `UserId`
    // to a `String` parameter, or to a text column's `==` — but never the
    // other way: a `String` still enters a `UserId` only through `.as`, and
    // one distinct type never stands for another.
    if (const auto backing = distinctBacking(argType);
        !typesEqual(backing, argType) && typesEqual(distinctBacking(paramType), paramType) &&
        argMatchesParam(backing, paramType)) {
        return true;
    }
    // Never is the bottom type — a Never-typed expression (never returns) is
    // compatible with any expected type.
    if (std::holds_alternative<NeverType>(argType->kind)) {
        return true;
    }
    // An intersection in expected position is a conjunction: the same actual
    // value must satisfy every member.
    if (auto* intersection = std::get_if<IntersectionType>(&paramType->kind)) {
        for (const auto& member : intersection->members) {
            if (!argMatchesParam(argType, member)) {
                return false;
            }
        }
        return true;
    }
    // An intersection value may be forgotten to any one of its components.
    if (auto* intersection = std::get_if<IntersectionType>(&argType->kind)) {
        for (const auto& member : intersection->members) {
            if (argMatchesParam(member, paramType)) {
                return true;
            }
        }
        return false;
    }
    // A union can flow to an expected type only when every possible member
    // can. This is the directional dual of the expected-union rule below.
    if (auto* unionType = std::get_if<UnionType>(&argType->kind)) {
        for (const auto& member : unionType->members) {
            if (!argMatchesParam(member, paramType)) {
                return false;
            }
        }
        return true;
    }
    // Open structural records are record-only and use width compatibility.
    // Field types are deliberately invariant in the first slice.
    if (auto* required = std::get_if<RecordType>(&paramType->kind)) {
        auto fieldMatches = [&](const auto& available) {
            for (const auto& [name, expected] : required->fields) {
                auto found = available.find(name);
                if (found == available.end() || !typesEqual(found->second, expected)) {
                    return false;
                }
            }
            return true;
        };
        if (auto* named = std::get_if<NamedType>(&argType->kind)) {
            auto record = findRecordFields(*named);
            if (record == m_recordFields.end()) {
                return false;
            }
            std::unordered_map<std::string, TypePtr> fields;
            for (const auto& [name, fieldType] : record->second) {
                fields[name] = substituteInterfaceGenerics(fieldType, named->typeArgs);
            }
            return fieldMatches(fields);
        }
        if (auto* actual = std::get_if<RecordType>(&argType->kind)) {
            std::unordered_map<std::string, TypePtr> fields;
            for (const auto& [name, fieldType] : actual->fields) {
                fields[name] = fieldType;
            }
            return fieldMatches(fields);
        }
        return false;
    }
    if (auto* constrained = std::get_if<ConstrainedType>(&paramType->kind)) {
        return satisfiesTrait(argType, constrained->traitName);
    }
    // NamedType param that is itself a trait name: `Shape`, `Comparable`, etc.
    // Occurs when a heterogeneous list was widened to a trait element type and
    // then each element is checked against it, or when a trait-typed value is
    // passed to a ConstrainedType param that got resolved to NamedType.
    // argType matches if it implements that trait, or if argType IS that trait.
    // `make Tuple` names the whole family: a tuple's arity is part of its
    // type, so there is no single structural spelling for "any tuple" and the
    // receiver has to be matched by name. Without this a `Tuple` receiver
    // method never applied to an actual `(Integer, String)`.
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind);
        paramNamed && paramNamed->name == kex::names::kTupleType && paramNamed->typeArgs.empty()) {
        return std::holds_alternative<TupleType>(argType->kind);
    }
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        if (m_traits.get(paramNamed->name)) {
            if (auto* argNamed = std::get_if<NamedType>(&argType->kind);
                argNamed && argNamed->name == paramNamed->name) {
                return true; // trait-typed value matches trait param
            }
            return satisfiesTrait(argType, paramNamed->name);
        }
    }
    // Sized ints/floats and arbitrary-precision Integer aren't distinguished
    // at runtime yet (IntValue is one int64_t, FloatValue one double — see
    // the type-system plan's Runtime representation section), so don't
    // hard-error on a width/precision distinction the runtime doesn't keep:
    // any two Integer-trait members are compatible with each other, and
    // likewise for Float. Integer-vs-Float itself stays a real mismatch —
    // that distinction *is* runtime-backed today.
    // A sized number is its own concrete type (decision 7): only a literal
    // adapts to one (rule 2). It widens without loss, a sized integer to an
    // `Integer` and a float to one at least as wide, but nothing narrows
    // silently: an `Integer` reaches a `Byte` through `.to(Byte)`.
    if (std::holds_alternative<SizedIntType>(argType->kind)) {
        if (const auto* wanted = std::get_if<PrimitiveType>(&paramType->kind);
            wanted && wanted->kind == PrimitiveType::Integer) {
            return true;
        }
    }
    if (const auto* given = std::get_if<SizedFloatType>(&argType->kind)) {
        if (const auto* wanted = std::get_if<SizedFloatType>(&paramType->kind);
            wanted && wanted->bits >= given->bits) {
            return true;
        }
    }

    // Recurse into compound types structurally rather than requiring exact
    // equality — e.g. `[Int]` vs `[Integer]` (the relaxation above, but
    // inside a list) and `String` (= `[Char]`) vs a generic `[A]` param
    // both need this, not just bare params.
    if (auto* paramList = std::get_if<ListType>(&paramType->kind)) {
        auto* argList = std::get_if<ListType>(&argType->kind);
        return argList && argMatchesParam(argList->element, paramList->element);
    }
    if (auto* paramTuple = std::get_if<TupleType>(&paramType->kind)) {
        auto* argTuple = std::get_if<TupleType>(&argType->kind);
        if (!argTuple || argTuple->elements.size() != paramTuple->elements.size()) {
            return false;
        }
        for (size_t i = 0; i < paramTuple->elements.size(); i++) {
            if (!argMatchesParam(argTuple->elements[i], paramTuple->elements[i])) {
                return false;
            }
        }
        return true;
    }
    if (auto* paramMap = std::get_if<MapType>(&paramType->kind)) {
        auto* argMap = std::get_if<MapType>(&argType->kind);
        return argMap && argMatchesParam(argMap->key, paramMap->key) &&
               argMatchesParam(argMap->value, paramMap->value);
    }
    if (auto* paramOpt = std::get_if<OptionalType>(&paramType->kind)) {
        // `None` has no payload from which to infer an inner type, but it is
        // valid for every optional return/parameter type.
        if (auto* argNamed = std::get_if<NamedType>(&argType->kind);
            argNamed && argNamed->typeArgs.empty() &&
            argNamed->name == kex::names::kNoneConstructor) {
            return true;
        }
        auto* argOpt = std::get_if<OptionalType>(&argType->kind);
        return argOpt && argMatchesParam(argOpt->inner, paramOpt->inner);
    }
    // An optional never stands for its payload: `Just(0)` is no Integer. It
    // is a member of a union that names it, though: `String?` is a
    // `String | String?`. Refused here before the union rule below could ask
    // the members, a block ending in a `.try` on an Optional fit no
    // `Context -> Reply | Reply?` parameter it was not already typed from.
    if (std::holds_alternative<OptionalType>(argType->kind)) {
        const auto* paramUnion = std::get_if<UnionType>(&paramType->kind);
        if (!paramUnion) {
            return false;
        }
        for (const auto& member : paramUnion->members) {
            // Only an optional member, or a union nesting one (`Reply |
            // Outcome`): a bare `String` member still refuses it.
            const auto expanded = member ? resolve(expandTypeAliases(member)) : nullptr;
            if (expanded &&
                (std::holds_alternative<OptionalType>(expanded->kind) ||
                 std::holds_alternative<UnionType>(expanded->kind)) &&
                argMatchesParam(argType, member)) {
                return true;
            }
        }
        return false;
    }
    auto isStringType = [](const TypePtr& t) -> bool {
        if (auto* n = std::get_if<NamedType>(&t->kind)) {
            return n->typeArgs.empty() &&
                   (n->name == kex::names::kStringType || n->name == kex::names::kFilePathType);
        }
        if (auto* p = std::get_if<PrimitiveType>(&t->kind)) {
            return p->kind == PrimitiveType::String;
        }
        return false;
    };
    // FunctionType param — e.g. `(T-1) -> Bool` vs `(T81) -> Bool`. Without
    // this branch both sides fall to typesEqual which fails on mismatched
    // TypeVar ids even though both are permissive. Recurse into params and
    // result so lambda arguments to map/filter/each pass the checker.
    if (std::holds_alternative<FuncType>(paramType->kind)) {
        auto* argFn = std::get_if<FuncType>(&argType->kind);
        if (!argFn) {
            return isPermissive(argType);
        }
        // `(A) -> (B) -> C` and `(A, B) -> C` are the same callable shape
        // in Kex. Normalize both sides before comparing so a multi-parameter
        // lambda matches a curried source annotation (and vice versa).
        auto flatten = [](const TypePtr& type) {
            std::pair<std::vector<TypePtr>, TypePtr> result{{}, type};
            auto current = type;
            while (auto* fn = std::get_if<FuncType>(&current->kind)) {
                result.first.insert(result.first.end(), fn->params.begin(), fn->params.end());
                current = fn->result;
            }
            result.second = current;
            return result;
        };
        auto [argParams, argResult] = flatten(argType);
        auto [paramParams, paramResult] = flatten(paramType);
        // A block may DESTRUCTURE a tuple element instead of naming it:
        // `pairs.each do |k, v|` over `[(String, Int)]` binds one element
        // across two parameters, and both backends spread it. The checker was
        // the only thing that disagreed.
        //
        // Decidable from the ARGUMENT alone, which is what makes it cheap:
        // expected-type propagation has already given the block's first
        // parameter the element type, so a 2-parameter block over
        // `[(String, Int)]` arrives here as `((String, Int), T2) -> Void`.
        // A first parameter that is a tuple of exactly as many elements as the
        // block has parameters is a destructuring block; anything else is a
        // real arity error and still reported as one.
        if (argParams.size() != paramParams.size()) {
            if (paramParams.size() != 1 || argParams.size() < 2) {
                return false;
            }
            if (auto* spread = std::get_if<TupleType>(&argParams[0]->kind)) {
                if (spread->elements.size() != argParams.size()) {
                    return false;
                }
            } else if (
                !isPermissive(paramParams[0]) ||
                !std::all_of(argParams.begin(), argParams.end(), [&](const TypePtr& p) {
                    return isPermissive(p);
                })) {
                // Something IS known and it does not line up: a real arity
                // error, still reported. Only the case where neither side
                // knows anything is let through — `@conditions.each do |c, v|`
                // over a record field the checker has not resolved to a tuple.
                // Rejecting there would fail a program both backends run.
                return false;
            }
            if (isVoidLike(paramResult)) {
                return true;
            }
            return argMatchesParam(argResult, paramResult);
        }
        for (size_t i = 0; i < paramParams.size(); i++) {
            // Function parameters are contravariant: a callback offered to a
            // context must accept everything the context may pass.
            if (!argMatchesParam(paramParams[i], argParams[i])) {
                return false;
            }
        }
        // Void as the expected return means "result discarded" — accept any body type.
        if (isVoidLike(paramResult)) {
            return true;
        }
        return argMatchesParam(argResult, paramResult);
    }
    // NamedType with type args — e.g. `Range<Number>` param vs `Range<Integer>` arg.
    // Recurse into type arguments structurally so the inner types get the same
    // trait-relaxation treatment (argMatchesParam, not typesEqual).
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        auto* argNamed = std::get_if<NamedType>(&argType->kind);
        // A variant of an ADT is a value of that ADT; a parameter written as
        // one variant (`FileMode.Write`) takes that variant only.
        if (argNamed && (!argNamed->variant.empty() || !paramNamed->variant.empty()) &&
            argNamed->name == paramNamed->name &&
            argNamed->shadowsPrelude == paramNamed->shadowsPrelude) {
            return paramNamed->variant.empty() || argNamed->variant == paramNamed->variant;
        }
        // A bare name suffix-matches a qualified one (`Version` is
        // `Kex.Version`), but a constructor's singleton type is no spelling
        // of a type at all: NetErrorKind's `Backend` is not a `Kex.Backend`.
        const bool sameType =
            argNamed && argNamed->shadowsPrelude == paramNamed->shadowsPrelude &&
            (argNamed->name == paramNamed->name ||
             (!constructorOwner(*argNamed) && namedTypesMatch(argNamed->name, paramNamed->name)));
        if (!sameType) {
            // A nullary ADT constructor is a refined value of its parent
            // type. This relationship comes from the ADT registry rather
            // than from any constructor spelling.
            if (argNamed && argNamed->typeArgs.empty() && paramNamed->typeArgs.empty()) {
                if (const auto* owner = constructorOwner(*argNamed);
                    owner && *owner == paramNamed->name) {
                    return true;
                }
                // m_adtOfConstructor keeps ONE owner per constructor, so a
                // record named by two unions belongs only to whichever was
                // declared last — adding `type Pair = Alpha | Beta` below
                // `type Answer = Alpha | String` stopped `Alpha` from being
                // an `Answer`. Membership is the question, so ask the union.
                if (m_recordFields.count(resolveRecordName(argNamed->name))) {
                    if (auto members = m_adtVariants.find(paramNamed->name);
                        members != m_adtVariants.end()) {
                        for (const auto& member : members->second) {
                            if (namedTypesMatch(argNamed->name, member)) {
                                return true;
                            }
                        }
                    }
                }
                // The same constructor/ADT relationship must survive a
                // `using` boundary. Imported nullary variants are recorded
                // per provider module rather than in m_adtOfConstructor.
                for (const auto& [module, constructors] : m_moduleConstructors) {
                    for (const auto& [constructor, imported] : constructors) {
                        if (imported.typeName != paramNamed->name) {
                            continue;
                        }
                        // A union declared in a module registers its members
                        // under the bare spelling the declaration used, while
                        // a RECORD member's values carry the module-qualified
                        // identity (`Lib.Redirect`) — so `type Reply = Safe |
                        // Redirect` inside `module Lib` rejected its own
                        // `Redirect`. The qualified spelling names the
                        // declaring module itself, which is all the evidence
                        // an identity comparison needs; an unrelated
                        // `Other.Redirect` still fails to match it. The bare
                        // spelling could be anyone's, so it stays gated on the
                        // module being visible here.
                        if (module + "." + constructor != argNamed->name) {
                            if (constructor != argNamed->name) {
                                continue;
                            }
                            if (!moduleMemberImported(module, constructor) &&
                                !makeModuleVisible(module)) {
                                continue;
                            }
                        }
                        return true;
                    }
                }
            }
            if (isStringType(argType) && isStringType(paramType)) {
                return true;
            }
            // A union alias may name a BUILTIN alongside its record members:
            // `type Answer = Alpha | String`. Every member is registered as a
            // constructor of `Answer`, which answers `Alpha` above — but a
            // plain String value carries no constructor name to look up, so
            // its printed identity is compared against the member names.
            // Without this the type could not be spelled at all, and route
            // handlers wanting "a reply record, or a string" had to say `Any`.
            if (!argNamed && paramNamed->typeArgs.empty()) {
                const auto actual = typeToString(argType);
                if (auto members = m_adtVariants.find(paramNamed->name);
                    members != m_adtVariants.end()) {
                    for (const auto& member : members->second) {
                        if (namedTypesMatch(actual, member)) {
                            return true;
                        }
                    }
                }
                // The same union declared inside a MODULE is registered per
                // provider module instead, so `type Reply = String | Safe`
                // in `module Web` has to answer for its String the same way.
                for (const auto& [module, constructors] : m_moduleConstructors) {
                    for (const auto& [constructor, imported] : constructors) {
                        if (imported.typeName != paramNamed->name) {
                            continue;
                        }
                        if (!namedTypesMatch(actual, constructor)) {
                            continue;
                        }
                        if (!moduleMemberImported(module, constructor) &&
                            !makeModuleVisible(module)) {
                            continue;
                        }
                        return true;
                    }
                }
            }
            return false;
        }
        // An unparameterized receiver declaration is the erased/wildcard
        // form of the named type. For example, `make Range` methods apply to
        // inferred `Range<Integer>` and `Range<Char>` values.
        if (paramNamed->typeArgs.empty()) {
            return true;
        }
        // The same for a value of the type written without its arguments
        // (`Ask(Process)`): a `Process<A>` parameter takes it.
        if (argNamed->typeArgs.empty()) {
            return true;
        }
        if (argNamed->typeArgs.size() != paramNamed->typeArgs.size()) {
            return false;
        }
        for (size_t i = 0; i < paramNamed->typeArgs.size(); i++) {
            if (!argMatchesParam(argNamed->typeArgs[i], paramNamed->typeArgs[i])) {
                return false;
            }
        }
        return true;
    }
    // UnionType param (e.g. type alias `Level = :a | :b | :c`) — arg
    // matches if it matches any branch of the union.
    if (auto* paramUnion = std::get_if<UnionType>(&paramType->kind)) {
        for (const auto& member : paramUnion->members) {
            if (member && argMatchesParam(argType, member)) {
                return true;
            }
        }
        return false;
    }

    if (isVoidLike(argType) && isVoidLike(paramType)) {
        return true;
    }

    if (isStringType(argType) && isStringType(paramType)) {
        return true;
    }

    return typesEqual(argType, paramType);
}

auto TypeChecker::recordCalledLikeFunction(const std::string& name) const -> bool {
    // The name's own last part is the type's: `Kex.Intrinsic.Kex.backend()`
    // is a function of a module, whatever its first letter.
    const auto last = name.substr(name.rfind('.') + 1);
    if (last.empty() || !std::isupper(static_cast<unsigned char>(last.front()))) {
        return false;
    }
    NamedType written;
    written.name = name;
    if (findRecordFields(written) == m_recordFields.end()) {
        return false;
    }
    // Unless something callable has that name: a constructor, a function.
    return !m_adtOfConstructor.contains(name) && !m_userSignatures.contains(name) &&
           importedCandidates(name).empty();
}

auto TypeChecker::withWrittenLetters(const TypePtr& type) const -> TypePtr {
    std::function<TypePtr(const TypePtr&)> written = [&](const TypePtr& part) -> TypePtr {
        const auto resolved = resolve(part);
        if (const auto id = typeVariableId(resolved)) {
            if (const auto letter = m_rigidVars.find(*id); letter != m_rigidVars.end()) {
                return Type::named(letter->second);
            }
            return resolved;
        }
        return mapTypeChildren(resolved, written);
    };
    return type ? written(type) : type;
}

auto TypeChecker::blockResultMismatch(const TypePtr& expected, const TypePtr& given) const
    -> std::optional<std::pair<TypePtr, TypePtr>> {
    // `(A, B) -> C` and `A -> B -> C` are one shape: the result is what is
    // left after every parameter.
    const auto flatten = [&](TypePtr type) {
        std::size_t params = 0;
        while (const auto* function = std::get_if<FuncType>(&resolve(type)->kind)) {
            params += function->params.size();
            type = function->result;
        }
        return std::pair{params, resolvedThroughout(type)};
    };
    const auto [wantedParams, wanted] = flatten(expected);
    const auto [givenParams, produced] = flatten(given);
    // A result still open (a letter this call did not fix, an unknown, a
    // bound), or one nothing reads, says nothing yet.
    const auto open = [&](const TypePtr& type) {
        if (const auto id = typeVariableId(type)) {
            return !m_rigidVars.contains(*id);
        }
        return isUndetermined(type) || std::holds_alternative<ConstrainedType>(type->kind);
    };
    if (wantedParams != givenParams || !wanted || !produced || open(wanted) || open(produced) ||
        isVoidLike(wanted) || std::holds_alternative<NeverType>(produced->kind) ||
        argMatchesParam(produced, wanted)) {
        return std::nullopt;
    }
    // A seed narrower than what the block makes of it is no mistake: the
    // letter is their join. `reduce(Empty) { |tree, x| tree.insert(x) }`
    // seeds with the one variant and folds into the whole Tree. Not into a
    // union the block made up: `then item else None` on an `T?` seed is a
    // `T` where the seed's `T?` is matched.
    if (!std::holds_alternative<UnionType>(produced->kind) && argMatchesParam(wanted, produced)) {
        return std::nullopt;
    }
    return std::pair{wanted, produced};
}

auto TypeChecker::hasAnnotatedSignatureForReceiver(
    const std::string& name, const TypePtr& receiverType) const -> bool {
    auto it = m_annotatedReceiverKeys.find(name);
    if (it == m_annotatedReceiverKeys.end()) {
        return false;
    }
    return it->second.count(annotatedReceiverKey(receiverType)) > 0;
}

auto TypeChecker::annotatedReceiverKey(const TypePtr& receiverType) const -> std::string {
    const auto receiver = resolve(receiverType);
    const auto* named = std::get_if<NamedType>(&receiver->kind);
    const bool generic =
        named &&
        std::all_of(named->typeArgs.begin(), named->typeArgs.end(), [&](const TypePtr& arg) {
            return std::holds_alternative<TypeVar>(resolve(arg)->kind);
        });
    return typeToString(generic ? headShapeOf(receiver) : receiver);
}

auto TypeChecker::bindThis(const TypePtr& type, const TypePtr& receiver) -> TypePtr {
    if (!type) {
        return type;
    }
    if (isThisPlaceholder(type)) {
        return receiver;
    }
    return mapTypeChildren(type, [&](const TypePtr& child) { return bindThis(child, receiver); });
}

auto TypeChecker::sameMethod(const Signature& left, const Signature& right) const -> bool {
    if (!sameParameters(left, right)) {
        return false;
    }
    const auto traitOf = [&](const Signature& signature) -> std::string {
        const auto found = signature.clause ? m_traitDefaultClauses.find(signature.clause)
                                            : m_traitDefaultClauses.end();
        return found == m_traitDefaultClauses.end() ? std::string{} : found->second;
    };
    return traitOf(left) == traitOf(right);
}

auto TypeChecker::displaySignature(const std::string& name, const Signature& sig) const
    -> std::string {
    // Collect TypeVar IDs in order of first appearance, then remap to
    // sequential negative IDs so they display as A, B, C, ... regardless
    // of the internal counter state when the signature was built.
    std::unordered_map<int, int> varRemap;
    int nextNeg = -1;
    std::function<void(const TypePtr&)> collectVars = [&](const TypePtr& t) {
        if (!t) {
            return;
        }
        if (const auto* var = std::get_if<TypeVar>(&t->kind)) {
            if (!varRemap.count(var->id)) {
                varRemap[var->id] = nextNeg--;
            }
            return;
        }
        forEachTypeChild(t, collectVars);
    };
    for (const auto& p : sig.params) {
        collectVars(p);
    }
    collectVars(sig.result);

    std::function<TypePtr(const TypePtr&)> remap = [&](const TypePtr& t) -> TypePtr {
        if (!t) {
            return t;
        }
        return std::visit(
            [&](const auto& node) -> TypePtr {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, TypeVar>) {
                    auto it = varRemap.find(node.id);
                    return it != varRemap.end() ? Type::genericLetter(it->second) : t;
                } else {
                    return mapTypeChildren(t, remap);
                }
            },
            t->kind);
    };

    auto displayType = [&](const TypePtr& t) -> std::string {
        // Unknown is the checker's gradual internal representation. In a
        // public callable signature it corresponds to source-level `Any`, not
        // an inference failure named "unknown".
        if (std::holds_alternative<UnknownType>(t->kind)) {
            return "Any";
        }
        if (auto* constrained = std::get_if<ConstrainedType>(&t->kind)) {
            return constrained->traitName;
        }
        return typeToString(remap(t));
    };
    std::string result = sourceName(name) + " : ";
    for (const auto& param : sig.params) {
        auto text = displayType(param);
        // A function-typed parameter needs parentheses of its own, or its
        // arrow merges into the signature's: `filter : [A] -> (A -> Bool) -> [A]`
        // rather than `[A] -> A -> Bool -> [A]`, which reads as three
        // parameters.
        if (param && std::holds_alternative<FuncType>(param->kind)) {
            text = "(" + text + ")";
        }
        result += text + " -> ";
    }
    // The result needs the same parenthesization as a function-typed
    // parameter: `mk : String -> (String -> String)`, not
    // `mk : String -> String -> String`, which reads as a flat 2-parameter
    // function rather than a 1-parameter function returning one (#350).
    auto resultText = displayType(sig.result);
    if (sig.result && std::holds_alternative<FuncType>(sig.result->kind)) {
        resultText = "(" + resultText + ")";
    }
    result += resultText;
    return result;
}

// The module whose function a capture names, by the rules a call of the name
// follows: `~Mod.fn` resolves `Mod` as `Mod.fn(x)` would (an alias, an
// enclosing module, a module a `using` opened); a bare `~fn` is the
// program's free function it reaches, else one a `using` imported from a
// unit compiled apart. "" for a top-level function or the prelude's.
auto TypeChecker::captureModule(const ast::CurryExpr& capture) const -> std::string {
    if (!capture.module.empty()) {
        return resolveModulePath(capture.module, capture.name);
    }
    std::vector<Signature> visible;
    std::set<std::string> hidden;
    if (const auto* reached = reachableFreeFunctions(capture.name, false, visible, hidden);
        reached && !reached->empty()) {
        return reached->front().declaringModule;
    }
    if (const auto imported = usingImportedMethods(capture.name, false); !imported.empty()) {
        return imported.front()->sourceModule;
    }
    return {};
}

// A capture of a function, decided: the function named, not called
// (`FunctionReference`, owner its qualified name). A local binding captured
// is the value it holds, nothing to decide.
auto TypeChecker::noteCaptureFlow(const ast::Expr* value, const TypePtr& expected) -> void {
    if (!value || !expected) {
        return;
    }
    const auto type = expandTypeAliases(resolve(expected));
    if (const auto* curry = std::get_if<ast::CurryExpr>(&value->kind)) {
        if (const auto* function = std::get_if<FuncType>(&type->kind); function && function->foul) {
            m_capturesIntoFoul.insert(value);
        }
        if (const auto* function = std::get_if<FuncType>(&type->kind);
            function && function->params.size() == 1 && !curry->isOperator &&
            curry->argGroups.empty() && !lookupVar(curry->name)) {
            m_pendingCaptureCalls.push_back({value, curry, function->params.front()});
        }
        return;
    }
    if (const auto* list = std::get_if<ast::ListExpr>(&value->kind)) {
        if (const auto* elements = std::get_if<ListType>(&type->kind)) {
            for (const auto& element : list->elements) {
                noteCaptureFlow(element.get(), elements->element);
            }
        }
    }
}

auto TypeChecker::decideCaptureAsCall(
    const ast::Expr& value, const ast::CurryExpr& capture, const TypePtr& receiver) -> void {
    // Only on a receiver type already known in full: checking the call must
    // not bind a type variable of the program it is checked inside.
    const auto receiverType = resolve(receiver);
    if (m_captureCalls.count(&value) || !isFullyConcrete(receiverType)) {
        return;
    }
    // A capture qualified by its module (`~String.upperCase`) is that call
    // only when the module is the receiver's own type: another module names a
    // function the receiver's method may not be.
    if (!capture.module.empty() &&
        typeToString(expandTypeAliases(receiverType)) != capture.module) {
        return;
    }
    constexpr const char* kReceiver = "__capture";
    auto receiverExpr = std::make_unique<ast::Expr>();
    receiverExpr->location = value.location;
    receiverExpr->kind = ast::Identifier{kReceiver};
    auto call = std::make_unique<ast::Expr>();
    call->location = value.location;
    ast::MethodCall method;
    method.receiver = std::move(receiverExpr);
    method.method = capture.name;
    call->kind = std::move(method);
    // Checked aside: what it would report is not the program's to hear, and
    // a call that does not check is simply not decided.
    std::vector<Diagnostic> aside;
    auto* reported = m_diagnostics;
    m_diagnostics = &aside;
    // Unknown methods are reported at the end of the check; one this call
    // met is not the program's either.
    const auto unresolved = m_unresolvedMethods;
    const auto unresolvedImports = m_unresolvedMethodImports;
    pushScope();
    defineVar(kReceiver, receiverType);
    inferExpr(*call);
    popScope();
    m_diagnostics = reported;
    m_unresolvedMethods = unresolved;
    m_unresolvedMethodImports = unresolvedImports;
    const auto* decided = &std::get<ast::MethodCall>(call->kind);
    const bool clean = std::none_of(aside.begin(), aside.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.level == Diagnostic::Level::Error;
    });
    if (clean && m_resolvedCalls.count(decided)) {
        m_captureCalls[&value] = decided;
    }
    // Kept either way: the checker's tables may point into it.
    m_captureCallNodes.push_back(std::move(call));
}

auto TypeChecker::decideCapture(const ast::Expr& expr, const ast::CurryExpr& capture) -> void {
    if (capture.isOperator || lookupVar(capture.name)) {
        return;
    }
    m_decidedCalls[&expr] = {
        CallKind::FunctionReference, kex::mangle::qualify(captureModule(capture), capture.name)};
}

// The program's free functions a bare `name` reaches from here, for a call
// and a capture alike: a declaration the current module owns first (its
// bare declaration, else its own definition, published only under the
// qualified name), else the bare-name table, which holds every module's. A
// plain name reaches only the ones its module may see: a name `using Router,
// only: [get]` left out (`put`) is not borrowed from Router to fail at run
// time (#496), and another module's private `table` does not stand in for
// the `table` this module imported (#494). The modules left out go to
// `hidden`; `visible` is the storage a filtered answer points into. Null
// when nothing is reached.
auto TypeChecker::reachableFreeFunctions(
    const std::string& name,
    bool isMethodCall,
    std::vector<Signature>& visible,
    std::set<std::string>& hidden) const -> const std::vector<Signature>* {
    const std::vector<Signature>* userSignatures = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() && scoped != m_scopedDeclaredSignatures.end()) {
        userSignatures = &scoped->second;
    }
    // The same preference for a declaration the module owns WITHOUT an
    // annotation (`let render = Template.html(...)`): it is published only
    // under its qualified name, so the bare-name lookup below would pick
    // another module's same-named function instead — `Template.render`,
    // once the Template source is merged into the build (kexhq/kex#385).
    else if (auto own = m_userSignatures.find(kex::mangle::qualify(m_currentModulePath, name));
             !isMethodCall && !m_currentModulePath.empty() && !kex::mangle::hasScope(name) &&
             own != m_userSignatures.end() && !own->second.empty()) {
        userSignatures = &own->second;
    } else if (auto user = m_userSignatures.find(name); user != m_userSignatures.end()) {
        userSignatures = &user->second;
    }
    if (!userSignatures || isMethodCall || kex::mangle::hasScope(name)) {
        return userSignatures;
    }
    for (const auto& signature : *userSignatures) {
        if (freeFunctionVisible(signature.declaringModule, name)) {
            visible.push_back(signature);
        } else {
            hidden.insert(signature.declaringModule);
        }
    }
    // Inside a module, the program's top-level function steps aside for one
    // the module declares or imports by that name: `main.kex`'s own `table`
    // took the place of the `table` a module imported with `using
    // App.Shapes, only: [table]` (#494).
    const auto topLevel = [](const Signature& signature) {
        return signature.declaringModule.empty();
    };
    if (!m_currentModulePath.empty() && std::any_of(visible.begin(), visible.end(), topLevel) &&
        !std::all_of(visible.begin(), visible.end(), topLevel)) {
        std::erase_if(visible, topLevel);
        hidden.insert("");
    }
    if (hidden.empty()) {
        return userSignatures;
    }
    return visible.empty() ? nullptr : &visible;
}

auto TypeChecker::checkCall(
    const std::string& name,
    const std::vector<TypePtr>& argTypes,
    SourceLocation loc,
    bool isMethodCall,
    const ast::MethodCall* methodCall,
    const ast::Expr* callExpr,
    const std::vector<const ast::Expr*>* argumentExprs) -> TypePtr {
    auto result =
        checkCallCore(name, argTypes, loc, isMethodCall, methodCall, callExpr, argumentExprs);
    // Decision 4 with several parameters of a bare trait result: the call's
    // type is the body's at these arguments (`combine(1, 2.5)` is a Float).
    if (result && (std::holds_alternative<ConstrainedType>(result->kind) ||
                   (std::holds_alternative<NamedType>(result->kind) &&
                    m_traits.lookup(std::get<NamedType>(result->kind).name)))) {
        if (traitResultFunction(name)) {
            if (auto atCall = callResultAt(name, argTypes)) {
                return atCall;
            }
        }
    }
    // Decision 1: an unannotated function's call is its body at these
    // arguments (`shout("a")` is a String when `shout(s) = s.upperCase`).
    if (const auto* open = result ? std::get_if<UnknownType>(&result->kind) : nullptr;
        open && !open->written) {
        if (auto atCall = callResultAt(name, argTypes)) {
            return atCall;
        }
    }
    // A call on a value whose type is not known yet answers what every
    // function it may reach answers, when they all agree on one concrete
    // type: `"${x}"` is a String whatever `x` turns out to be. Not a written
    // `Any`, on which a call needs narrowing first (decision 7).
    if (isMethodCall && !argTypes.empty() && result) {
        const auto* gap = std::get_if<UnknownType>(&result->kind);
        const auto receiver = resolve(argTypes.front());
        const auto* writtenAny = std::get_if<UnknownType>(&receiver->kind);
        if (gap && !gap->written && isUndetermined(receiver) &&
            !(writtenAny && writtenAny->written)) {
            if (auto common = commonCandidateResult(name, argTypes)) {
                return common;
            }
        }
    }
    return result;
}

auto TypeChecker::commonCandidateResult(
    const std::string& name, const std::vector<TypePtr>& argTypes) -> TypePtr {
    const auto arity = argTypes.size();
    // A field of that name may be what is read: its type is the record's.
    for (const auto& [record, fields] : m_recordFields) {
        if (fields.count(name)) {
            return nullptr;
        }
    }
    if (m_importedInterfaces) {
        for (const auto& [record, fields] : m_importedInterfaces->recordFieldNames) {
            if (fields.count(name)) {
                return nullptr;
            }
        }
    }
    TypePtr common;
    bool any = false;
    bool agree = true;
    const auto consider = [&](const Signature& signature) {
        const auto required = signature.requiredParams.value_or(signature.params.size());
        if (arity < required || arity > signature.params.size()) {
            return;
        }
        // Only a candidate the other arguments fit is one this call may reach,
        // and its letters are what they bind (`to(String)` is a `String?`).
        std::unordered_map<int, TypePtr> bound;
        std::function<void(const TypePtr&, const TypePtr&)> bind = [&](const TypePtr& param,
                                                                       const TypePtr& arg) {
            const auto p = resolve(param);
            const auto a = resolve(arg);
            if (const auto* var = std::get_if<TypeVar>(&p->kind); var && isGenericLetter(*var)) {
                bound.try_emplace(var->id, a);
                return;
            }
            const auto* pn = std::get_if<NamedType>(&p->kind);
            const auto* an = std::get_if<NamedType>(&a->kind);
            if (pn && an && pn->typeArgs.size() == an->typeArgs.size()) {
                for (std::size_t i = 0; i < pn->typeArgs.size(); ++i) {
                    bind(pn->typeArgs[i], an->typeArgs[i]);
                }
            }
        };
        for (std::size_t i = 1; i < arity && i < signature.params.size(); ++i) {
            if (!argTypes[i] || !argMatchesParam(argTypes[i], signature.params[i])) {
                return;
            }
            bind(signature.params[i], argTypes[i]);
        }
        any = true;
        std::function<TypePtr(const TypePtr&)> substitute = [&](const TypePtr& type) -> TypePtr {
            if (const auto* var = std::get_if<TypeVar>(&type->kind); var && isGenericLetter(*var)) {
                const auto found = bound.find(var->id);
                return found == bound.end() ? type : found->second;
            }
            return mapTypeChildren(type, substitute);
        };
        const auto result = signature.result ? resolve(substitute(signature.result)) : nullptr;
        if (!result || !isConcreteValue(result) || containsTypeVariable(result) ||
            anyType(result, [&](const TypePtr& node) { return isThisPlaceholder(node); })) {
            agree = false;
        } else if (!common) {
            common = result;
        } else if (!typesEqual(common, result)) {
            // Not the union of their results (§4.5 rule 4's reading): in an
            // unannotated body that reports, at the definition, errors that
            // hold only for some arguments, which decision 1 forbids. The
            // call stays open, and each call checks it with its own types.
            agree = false;
        }
    };
    for (const auto* table : {&m_methodSignatures, &m_userSignatures}) {
        if (const auto found = table->find(name); found != table->end()) {
            for (const auto& signature : found->second) {
                consider(signature);
            }
        }
    }
    // What a trait requires of a value it may hold, the value as receiver:
    // `combine : This -> This` follows the value, `to : Type<T> -> T?` does
    // not.
    for (const auto& [_, trait] : m_traits.all()) {
        for (const auto& required : trait.requiredMethods) {
            if (required.name != name) {
                continue;
            }
            auto withReceiver = required;
            withReceiver.params.insert(
                withReceiver.params.begin(), Type::named(kex::names::kThisType));
            if (withReceiver.requiredParams) {
                withReceiver.requiredParams = *withReceiver.requiredParams + 1;
            }
            consider(withReceiver);
        }
    }
    for (const auto* imported : importedCandidates(name)) {
        consider(imported->signature);
    }
    return any && agree ? common : nullptr;
}

auto TypeChecker::checkCallCore(
    const std::string& name,
    const std::vector<TypePtr>& suppliedArgTypes,
    SourceLocation loc,
    bool isMethodCall,
    const ast::MethodCall* methodCall,
    const ast::Expr* callExpr,
    const std::vector<const ast::Expr*>* argumentExprs) -> TypePtr {
    auto argTypes = suppliedArgTypes;
    // A call on an unannotated parameter is what its function asks of the
    // arguments passed for it; a call to a function of this program is judged
    // against what it asks, once every function has been checked
    // (reportUnmetDemands, kexhq/kex#782).
    if (isMethodCall && !argTypes.empty()) {
        if (const auto* var = std::get_if<TypeVar>(&resolve(argTypes[0])->kind)) {
            if (const auto owner = m_unannotatedParamOwner.find(var->id);
                owner != m_unannotatedParamOwner.end()) {
                m_parameterDemands[owner->second.first][owner->second.second].insert(name);
            }
        }
    }
    if (m_userSignatures.contains(name)) {
        m_demandedCalls.push_back(
            {name, name + "/" + std::to_string(argTypes.size()), argTypes, loc});
    }
    // A receiver whose type is a variable the function's signature writes is
    // any type: only a function taking any receiver applies (`a.showValue`),
    // not String's `upperCase` on `a: A` (kexhq/kex#644).
    // A written `Any` is the top type (decision 7): what holds of it is what
    // holds of every value, so the same rule applies. `value.upperCase` on an
    // Any compiled, and failed at run time on anything but a String.
    if (isMethodCall && !argTypes.empty()) {
        const auto receiver = resolve(argTypes.front());
        const auto* var = std::get_if<TypeVar>(&receiver->kind);
        const auto* unknown = std::get_if<UnknownType>(&receiver->kind);
        // The letters of a generic `make Box<T>` too: its bodies are written
        // for every `T`, and `@item.pretty` failed at run time for a
        // `Box<String>` (kexhq/kex#646).
        std::string makeLetter;
        if (var && m_inMakeBlock && !m_currentTrait) {
            for (const auto& [written, variable] : m_currentMakeTargetVars) {
                if (const auto id = typeVariableId(resolve(variable)); id && *id == var->id) {
                    makeLetter = written;
                }
            }
        }
        const bool rigid = var && (m_rigidVars.contains(var->id) || !makeLetter.empty());
        const bool writtenAny = unknown && unknown->written;
        if (rigid || writtenAny) {
            const auto takesAny = [&](const Signature& signature) {
                if (signature.params.empty()) {
                    return false;
                }
                const auto first = resolve(signature.params.front());
                const auto* open = std::get_if<TypeVar>(&first->kind);
                // A trait bound takes it too: Showable and Inspectable hold of
                // every value (`"${a}"`, `a.inspected`). `Any` is Unknown.
                return std::holds_alternative<UnknownType>(first->kind) ||
                       std::holds_alternative<ConstrainedType>(first->kind) ||
                       (open && (isGenericLetter(*open) || (var && open->id == var->id)));
            };
            bool anyTakes = false;
            for (const auto* imported : importedCandidates(name)) {
                anyTakes = anyTakes || takesAny(imported->signature);
            }
            for (const auto* table : {&m_methodSignatures, &m_userSignatures}) {
                if (const auto found = table->find(name); found != table->end()) {
                    anyTakes = anyTakes ||
                               std::any_of(found->second.begin(), found->second.end(), takesAny);
                }
            }
            if (!anyTakes) {
                if (rigid) {
                    error(
                        diag::Code::NotDefinedForEveryType, loc,
                        {{"name", sourceName(name)},
                         {"type", makeLetter.empty() ? m_rigidVars.at(var->id) : makeLetter}});
                } else {
                    error(diag::Code::AnyNotNarrowed, loc, {{"name", sourceName(name)}});
                }
                return Type::unknown();
            }
        }
    }
    // A field of a structural record answers a call on it before any
    // method of the same name another type has (`r.name`).
    if (isMethodCall && argTypes.size() == 1 && !kex::mangle::hasScope(name)) {
        if (auto field = structuralField(resolve(argTypes.front()), name)) {
            if (callExpr) {
                m_decidedCalls[callExpr] = {
                    CallKind::FieldRead, typeToString(resolve(argTypes.front()))};
            }
            return field;
        }
    }
    // The arguments as written, beside their types, so a literal can go
    // straight into a distinct type it backs (literalCoercesTo). Left empty
    // when they do not line up with argTypes one for one.
    // Laid out as the callers lay out argTypes: positional arguments, then
    // named ones as written, then the block, which is never a literal.
    std::vector<const ast::Expr*> argExprs;
    const auto layOut = [&](const auto& call) {
        for (const auto& arg : call.args) {
            argExprs.push_back(arg.get());
        }
        for (const auto& [_, arg] : call.namedArgs) {
            argExprs.push_back(arg.get());
        }
        if (call.block) {
            argExprs.push_back(nullptr);
        }
    };
    if (methodCall) {
        argExprs.push_back(methodCall->receiver.get());
        layOut(*methodCall);
        if (argExprs.size() == argTypes.size() + 1) {
            argExprs.erase(argExprs.begin());
        }
    } else if (callExpr) {
        if (const auto* call = std::get_if<ast::FunctionCall>(&callExpr->kind)) {
            layOut(*call);
        }
    } else if (argumentExprs) {
        argExprs = *argumentExprs;
    }
    if (argExprs.size() != argTypes.size()) {
        argExprs.clear();
    }
    // A type named as the argument of a `Type<X>` parameter is that type as
    // a value (`m.to(String, in: Hour)`).
    const auto selectedType = [&](std::size_t i, const TypePtr& param) -> TypePtr {
        const auto* wanted = std::get_if<NamedType>(&resolve(param)->kind);
        if (!wanted || wanted->name != kex::names::kTypeRecord || wanted->typeArgs.size() != 1 ||
            i >= argExprs.size() || !argExprs[i]) {
            return nullptr;
        }
        // A written type (`to([Integer])`, `to(List<Integer>)`) is the
        // call's `targetType`; its argument names only the head.
        TypePtr named;
        if (methodCall && methodCall->targetType && !methodCall->args.empty() &&
            argExprs[i] == methodCall->args.front().get()) {
            std::unordered_map<std::string, TypePtr> generics;
            named = resolveTypeExpr(*methodCall->targetType, generics);
        } else if (const auto* written = std::get_if<ast::UpperIdentifier>(&argExprs[i]->kind);
                   written && namesKnownType(written->name)) {
            // Only a name that IS a type: a constant (`EARLIEST_YEAR`) passed
            // where a `Type<X>` may go is a value, not an unknown type.
            named = typeNameReference(*argExprs[i]);
        }
        return named ? Type::named(kex::names::kTypeRecord, {named}) : nullptr;
    };
    const auto fits = [&](std::size_t i, const TypePtr& writtenParam) {
        // A trait default's `This` past the receiver is the receiver's type at
        // this call: `15.clamp(0, 10)` takes Integers, and so does
        // `clamp(15, 0, 10)`, the same call written without the dot.
        auto param = writtenParam;
        if (i > 0 && !argTypes.empty() && isThisPlaceholder(resolve(param))) {
            const auto receiver = resolve(argTypes[0]);
            if (!isUndetermined(receiver)) {
                // Numbers order across the tower: `max(1, 2.5)` compares an
                // Integer with a Float, and answers their join.
                if (i < argTypes.size() && isNumericType(receiver) &&
                    isNumericType(resolve(argTypes[i]))) {
                    return true;
                }
                param = receiver;
            }
        }
        if (auto selected = selectedType(i, param)) {
            return argMatchesParam(selected, param);
        }
        if (i < argExprs.size() && argExprs[i] && literalOutsideSet(*argExprs[i], param)) {
            return false;
        }
        // An Optional receiver is not the value it may hold: `xs[1].ok?` on a
        // `Result?` reached Result's `ok?`, which the walker then could not
        // run. Its functions are the Optional's own, or ones open to any type.
        if (isMethodCall && i == 0 && i < argTypes.size()) {
            const auto receiver = resolve(argTypes[0]);
            const auto wanted = resolve(param);
            const auto* named = std::get_if<NamedType>(&wanted->kind);
            const bool otherHead = (named && !isTrait(named->name) && !isThisPlaceholder(wanted)) ||
                                   std::holds_alternative<PrimitiveType>(wanted->kind) ||
                                   std::holds_alternative<ListType>(wanted->kind);
            if (std::holds_alternative<OptionalType>(receiver->kind) && otherHead) {
                return false;
            }
        }
        return argMatchesParam(argTypes[i], param) ||
               (i < argExprs.size() && argExprs[i] && literalCoercesTo(*argExprs[i], param));
    };
    // A field promised by an open record is known more specifically than an
    // unrelated global/UFCS method with the same name. Concrete record fields
    // already receive this priority later; structural receivers need it here
    // because they have no nominal registry entry to trigger that path.
    if (isMethodCall && argTypes.size() == 1) {
        auto structuralField = [&](const auto& self, const TypePtr& type) -> TypePtr {
            auto resolved = resolve(type);
            if (auto* record = std::get_if<RecordType>(&resolved->kind)) {
                for (const auto& [fieldName, fieldType] : record->fields) {
                    if (fieldName == name) {
                        return fieldType;
                    }
                }
            }
            if (auto* intersection = std::get_if<IntersectionType>(&resolved->kind)) {
                for (const auto& member : intersection->members) {
                    if (auto found = self(self, member)) {
                        return found;
                    }
                }
            }
            return nullptr;
        };
        if (auto field = structuralField(structuralField, argTypes.front())) {
            return field;
        }
    }
    if (isMethodCall && !argTypes.empty()) {
        auto receiver = resolve(argTypes.front());
        // A receiver known only by its trait (`s: Shape`) answers from the
        // trait itself: a required method by its declared signature, a
        // default one gradually (defaults carry no signature — see
        // TraitDef::defaultMethods). Left to the overload lookup below, the
        // call bound to whichever implementer declared the name — `describe
        // expects argument 1 to be Rectangle, but got Shape` for a default
        // that only Rectangle overrides.
        // The same for a list of mixed implementers, whose element type is
        // joined to the trait as a plain name (`[Circle {..}, Rectangle {..}]`
        // is `[Shape]`).
        // `this` in a trait's default answers to that trait's requirements.
        if (m_currentTrait && isThisPlaceholder(receiver)) {
            for (const auto& required : m_currentTrait->requiredMethods) {
                if (required.name == name &&
                    missesRequiredArity(required, argTypes.size(), name, loc)) {
                    return required.result;
                }
            }
            // One of the trait's own defaults takes what one of its clauses
            // does: `this.repeat()` beside `let repeat(n: Integer)` is a
            // missing argument, which went through to the run time.
            if (const auto defaults = m_traitDefaults.find(m_currentTrait->name);
                defaults != m_traitDefaults.end()) {
                bool defined = false;
                bool fits = false;
                std::size_t most = 0;
                for (const auto* function : defaults->second) {
                    if (!function || function->name != name) {
                        continue;
                    }
                    for (const auto& clause : function->clauses) {
                        defined = true;
                        const bool explicitReceiver = !clause.params.empty() &&
                                                      clause.params.front().pattern &&
                                                      std::holds_alternative<ast::ThisPattern>(
                                                          (*clause.params.front().pattern)->kind);
                        const std::size_t receivers = explicitReceiver ? 0 : 1;
                        std::size_t least = clause.params.size();
                        while (least > 0 && clause.params[least - 1].defaultValue) {
                            least--;
                        }
                        const auto upper = clause.params.size() + receivers;
                        most = std::max(most, upper);
                        fits = fits ||
                               (argTypes.size() >= least + receivers && argTypes.size() <= upper);
                    }
                }
                if (defined && !fits) {
                    error(
                        diag::Code::WrongArgumentCount, loc,
                        {{"name", sourceName(name)},
                         {"expected", std::to_string(most)},
                         {"actual", std::to_string(argTypes.size())}});
                    return Type::unknown();
                }
            }
            // A requirement called on `this` answers what it is required to:
            // `this.price * 2` under `price :> Integer` is an Integer.
            for (const auto& required : m_currentTrait->requiredMethods) {
                if (required.name == name && argTypes.size() == requirementArity(required) &&
                    required.result && isConcreteValue(resolve(required.result)) &&
                    !containsTypeVariable(resolve(required.result)) &&
                    !isThisPlaceholder(resolve(required.result))) {
                    if (callExpr) {
                        m_decidedCalls[callExpr] = {CallKind::TraitMethod, m_currentTrait->name};
                    }
                    return required.result;
                }
            }
        }
        std::string receiverTrait;
        if (auto* constrained = std::get_if<ConstrainedType>(&receiver->kind)) {
            receiverTrait = constrained->traitName;
        } else if (auto* named = std::get_if<NamedType>(&receiver->kind);
                   named && named->typeArgs.empty() && !m_recordFields.count(named->name) &&
                   !m_adtVariants.count(named->name)) {
            receiverTrait = named->name;
        }
        // A method the receiver's trait requires or defaults: dispatched on
        // the value at run time, through that trait.
        const auto viaTrait = [&](const std::string& trait) {
            if (callExpr) {
                m_decidedCalls[callExpr] = {CallKind::TraitMethod, trait};
            }
        };
        if (!receiverTrait.empty()) {
            if (const TraitDef* trait = m_traits.get(receiverTrait)) {
                for (const auto& required : trait->requiredMethods) {
                    if (required.name == name && argTypes.size() == requirementArity(required)) {
                        viaTrait(receiverTrait);
                        return substituteThis(required.result, receiver);
                    }
                }
                if (std::find(trait->defaultMethods.begin(), trait->defaultMethods.end(), name) !=
                    trait->defaultMethods.end()) {
                    viaTrait(receiverTrait);
                    // What the default answers, when its body says one type that
                    // does not follow the value (`doubled` is an Integer).
                    if (const auto methods = m_methodSignatures.find(name);
                        methods != m_methodSignatures.end()) {
                        for (const auto& signature : methods->second) {
                            const auto owner = signature.clause
                                                   ? m_traitDefaultClauses.find(signature.clause)
                                                   : m_traitDefaultClauses.end();
                            if (owner == m_traitDefaultClauses.end() ||
                                owner->second != trait->name ||
                                signature.params.size() != argTypes.size() || !signature.result) {
                                continue;
                            }
                            const auto result = resolve(signature.result);
                            // Not decided yet (its body comes later): the pending
                            // result, which the checks of its uses wait for.
                            if (const auto pending = typeVariableId(result);
                                pending && m_pendingResultVars.contains(*pending)) {
                                return result;
                            }
                            if (isConcreteValue(result) && !containsTypeVariable(result) &&
                                !anyType(result, [&](const TypePtr& node) {
                                    return isThisPlaceholder(node);
                                })) {
                                return result;
                            }
                        }
                    }
                    return Type::unknown();
                }
                for (const auto& required : trait->requiredMethods) {
                    if (required.name == name &&
                        missesRequiredArity(required, argTypes.size(), name, loc)) {
                        return required.result;
                    }
                }
            }
        }
        if (auto* intersection = std::get_if<IntersectionType>(&receiver->kind)) {
            for (const auto& member : intersection->members) {
                std::string traitName;
                if (auto* named = std::get_if<NamedType>(&member->kind)) {
                    traitName = named->name;
                } else if (auto* constrained = std::get_if<ConstrainedType>(&member->kind)) {
                    traitName = constrained->traitName;
                }
                if (traitName.empty()) {
                    continue;
                }
                if (const TraitDef* trait = m_traits.get(traitName)) {
                    for (const auto& required : trait->requiredMethods) {
                        if (required.name == name &&
                            argTypes.size() == requirementArity(required)) {
                            viaTrait(traitName);
                            return substituteThis(required.result, receiver);
                        }
                    }
                }
            }
        }
    }
    // A local function binding (`let f(x) ...` inside an expression block)
    // is represented as a FuncType variable rather than a top-level
    // FunctionDef signature. Resolve it before consulting global overloads.
    if (!isMethodCall && !kex::mangle::hasScope(name)) {
        auto local = lookupVar(name);
        auto resolvedLocal = local ? resolve(local) : nullptr;
        // A binding in scope shadows every function of its name, typed or
        // not: `let applyTo(f, x) = f(x)` calls the value `f` holds, and
        // `Just(rule) => rule()` the `Block<DateTime>` `rule` holds.
        if (resolvedLocal && callExpr) {
            m_decidedCalls[callExpr] = {CallKind::ValueCall, name};
        }
        if (resolvedLocal) {
            if (auto* function = std::get_if<FuncType>(&resolvedLocal->kind)) {
                // A call through a local bound by a bare `~name` capture
                // (`let f = ~greet`) carries only positional param TYPES in
                // `function` — no names. `argTypes` here is positional args
                // followed by named-arg types in the call site's WRITTEN
                // order, which is not necessarily `greet`'s declared order.
                // Reorder by `greet`'s real parameter names before comparing
                // positionally, so `f(loud: true, name: "Ada")` doesn't
                // silently check `loud` against `greet`'s first param and
                // vice versa (kexhq/kex#309).
                std::vector<TypePtr> reordered;
                const std::vector<TypePtr>* effective = &argTypes;
                const auto* call =
                    callExpr ? std::get_if<ast::FunctionCall>(&callExpr->kind) : nullptr;
                if (call && !call->namedArgs.empty()) {
                    const std::vector<std::string>* paramNames = nullptr;
                    if (auto origin = m_curryOrigin.find(name); origin != m_curryOrigin.end()) {
                        if (auto sigs = m_userSignatures.find(origin->second);
                            sigs != m_userSignatures.end()) {
                            for (const auto& sig : sigs->second) {
                                if (sig.params.size() == function->params.size() &&
                                    sig.paramNames.size() == sig.params.size()) {
                                    paramNames = &sig.paramNames;
                                    break;
                                }
                            }
                        }
                    }
                    if (paramNames) {
                        reordered.assign(function->params.size(), nullptr);
                        std::vector<bool> filled(function->params.size(), false);
                        for (size_t i = 0; i < call->namedArgs.size(); ++i) {
                            const auto& label = call->namedArgs[i].first;
                            for (size_t p = 0; p < paramNames->size(); ++p) {
                                if ((*paramNames)[p] == label &&
                                    call->args.size() + i < argTypes.size()) {
                                    reordered[p] = argTypes[call->args.size() + i];
                                    filled[p] = true;
                                    break;
                                }
                            }
                        }
                        size_t next = 0;
                        for (size_t i = 0; i < call->args.size() && i < argTypes.size(); ++i) {
                            while (next < reordered.size() && filled[next]) {
                                next++;
                            }
                            if (next >= reordered.size()) {
                                break;
                            }
                            reordered[next] = argTypes[i];
                            filled[next] = true;
                            next++;
                        }
                        if (std::all_of(filled.begin(), filled.end(), [](bool b) { return b; })) {
                            effective = &reordered;
                        }
                    }
                }
                if (function->params.size() != effective->size()) {
                    error(
                        diag::Code::WrongArgumentCount, loc,
                        {{"name", sourceName(name)},
                         {"expected", std::to_string(function->params.size())},
                         {"actual", std::to_string(effective->size())}});
                    return function->result;
                }
                for (size_t i = 0; i < effective->size(); ++i) {
                    if ((*effective)[i] && !argMatchesParam((*effective)[i], function->params[i])) {
                        typeMismatch(loc, function->params[i], (*effective)[i]);
                    }
                }
                return function->result;
            }
        }
        // The binding's type is still an inference variable — an unannotated
        // parameter that the body CALLS, as in `let applyTo(f, x) = f(x)`.
        // The call is itself the evidence that the variable is a function, so
        // bind it to one built from the argument types and a fresh result.
        // Without this the call answered `Any`, and `applyTo` inferred
        // `A -> B -> Any` instead of `(A -> B) -> A -> B`.
        //
        // Two gates, and both matter:
        //
        //  - a NEGATIVE id is an annotated signature's generic placeholder,
        //    instantiated per call site by bindGenerics. Binding one in the
        //    global substitution would leak one call's argument types into
        //    every other call of that signature;
        //  - a variable older than this clause's mark is not this clause's to
        //    constrain. Inference variables live in one substitution shared by
        //    the whole program, so one reached through an earlier
        //    instantiation can also stand for a binding elsewhere — writing a
        //    function type into it types that unrelated binding as a function.
        //    Only what this clause created is safe, which is the same rule
        //    generalizeSignature quantifies by.
        if (resolvedLocal) {
            if (auto* var = std::get_if<TypeVar>(&resolvedLocal->kind);
                var && isInferenceVariable(*var) && var->id >= m_clauseVarMark) {
                auto result = freshTypeVar();
                unifyVar(var->id, Type::func(argTypes, result));
                return result;
            }
        }
    }
    std::vector<Signature> visibleUserSignatures;
    std::set<std::string> hiddenModules;
    const auto* userSignatures =
        reachableFreeFunctions(name, isMethodCall, visibleUserSignatures, hiddenModules);
    // The function a bare call's name means here, whichever of its clauses or
    // overloads runs: the one module every reachable declaration is from.
    if (userSignatures && !isMethodCall && callExpr && !kex::mangle::hasScope(name) &&
        (std::holds_alternative<ast::FunctionCall>(callExpr->kind) ||
         std::holds_alternative<ast::TaggedLiteral>(callExpr->kind))) {
        std::set<std::string> modules;
        for (const auto& signature : *userSignatures) {
            modules.insert(signature.declaringModule);
        }
        // Several modules declare it: the one a `using` here imports.
        if (modules.size() > 1) {
            std::erase_if(modules, [&](const std::string& module) {
                return module.empty() || !moduleMemberImported(module, name, true);
            });
        }
        if (modules.size() == 1) {
            m_calledFunctions[callExpr] =
                modules.begin()->empty() ? name : *modules.begin() + "." + name;
        }
    }
    if (!hiddenModules.empty()) {
        // Nothing in scope takes these arguments, but a module's function
        // that is not in scope would: say that, rather than reporting a
        // mismatch against some unrelated prelude function of the same name.
        const auto accepts = [&](const Signature& signature) {
            const auto required = signature.requiredParams.value_or(signature.params.size());
            if (argTypes.size() < required || argTypes.size() > signature.params.size()) {
                return false;
            }
            for (size_t i = 0; i < argTypes.size(); ++i) {
                if (argTypes[i] && !fits(i, signature.params[i])) {
                    return false;
                }
            }
            return true;
        };
        // Only where the resolver let the name through — because some other
        // function of that name exists (`put` on Process.Shared); a name
        // nothing in scope declares it has reported already.
        if (!userSignatures && !hiddenModules.empty()) {
            const auto others = importedCandidateSignatures(name);
            if (!others.empty() && std::none_of(others.begin(), others.end(), accepts)) {
                // One owner says which way it is out of reach; several are
                // listed by that.
                std::string hidden;
                std::string privateOnes;
                for (const auto& module : hiddenModules) {
                    const bool isPrivate = m_privateFreeFunctions.contains(module + "\n" + name);
                    auto& list = isPrivate ? privateOnes : hidden;
                    list +=
                        (list.empty() ? "" : ", ") + std::string("`") + module + "." + name + "`";
                }
                if (hiddenModules.size() == 1) {
                    const auto& module = *hiddenModules.begin();
                    error(
                        privateOnes.empty() ? diag::Code::UndefinedFunctionNotImported
                                            : diag::Code::UndefinedFunctionPrivate,
                        loc,
                        {{"name", name}, {"function", module + "." + name}, {"module", module}});
                } else if (privateOnes.empty()) {
                    error(
                        diag::Code::UndefinedFunctionNoneImported, loc,
                        {{"name", name}, {"functions", hidden}});
                } else if (hidden.empty()) {
                    error(
                        diag::Code::UndefinedFunctionAllPrivate, loc,
                        {{"name", name}, {"functions", privateOnes}});
                } else {
                    error(
                        diag::Code::UndefinedFunctionHiddenOrPrivate, loc,
                        {{"name", name}, {"hidden", hidden}, {"private", privateOnes}});
                }
                return Type::unknown();
            }
        }
    }
    bool hasUser = userSignatures != nullptr;
    auto methodIt = m_methodSignatures.find(name);
    bool hasLocalMethods = isMethodCall && methodIt != m_methodSignatures.end();
    // A known record field is not an unrelated imported zero-argument
    // method with the same spelling (`Version.patch` vs HTTP.patch).
    if (isMethodCall && argTypes.size() == 1) {
        auto receiver = resolve(argTypes.front());
        const auto mayReadField = [&]() {
            if (!methodCall || methodCall->parenthesized || methodCall->mutating) {
                return false;
            }
            for (const auto& [record, fields] : m_recordFields) {
                if (fields.count(name)) {
                    return true;
                }
            }
            if (m_importedInterfaces) {
                for (const auto& [record, fields] : m_importedInterfaces->recordFieldNames) {
                    if (fields.count(name)) {
                        return true;
                    }
                }
            }
            return false;
        };
        if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
            if (auto record = findRecordFields(*named); record != m_recordFields.end()) {
                if (auto field = record->second.find(name); field != record->second.end()) {
                    // Same substitution the main field-access path does: a
                    // `Box<Integer>` reads `.items` as `[Integer]`, not as
                    // the declaration's bare parameter slot.
                    if (callExpr) {
                        m_decidedCalls[callExpr] = {
                            CallKind::FieldRead, resolveRecordName(named->name)};
                    }
                    return substituteInterfaceGenerics(field->second, named->typeArgs);
                }
            }
            // The same rule for a record the PRELUDE declares. Interfaces
            // carry field names but not their types, so the read is answered
            // gradually — the point is only that it is a field read at all,
            // and not a call to some same-named method: `moment.time.nanosecond`
            // is the Time record's field, never units.kex's `Integer.nanosecond`.
            if (m_importedInterfaces) {
                auto record = m_importedInterfaces->recordFieldNames.find(named->name);
                if (record != m_importedInterfaces->recordFieldNames.end() &&
                    record->second.count(name)) {
                    if (callExpr) {
                        m_decidedCalls[callExpr] = {CallKind::FieldRead, named->name};
                    }
                    return Type::unknown();
                }
            }
        } else if (auto* var = std::get_if<TypeVar>(&receiver->kind);
                   var && isInferenceVariable(*var) && var->id >= m_clauseVarMark &&
                   m_unannotatedParamVars.count(var->id)) {
            // An unannotated receiver reading a name that exactly one record
            // declares as a field is that record — decided HERE, before any
            // method is considered. Left to the method lookup below, a
            // prelude method of the same name (`Weekday.name`,
            // `OptionConfig.run`) won instead and typed the receiver as ITS
            // owner, so `let describe(c) = c.name` became `Weekday -> String`
            // (kexhq/kex#242). Only records THIS program declares count:
            // the imported ones (the prelude's `Kex.AST.*` alone declare
            // `name` several times) would make every common field name
            // ambiguous. The fallback after the method lookup applies the
            // same uniqueness rule, over every record, when no method matched
            // at all; with two candidates there is no single answer, and
            // resolution carries on as before. Gated like the unannotated-call
            // case above: only a variable this clause created is its to bind.
            std::string matchedRecord;
            TypePtr matchedFieldType;
            for (const auto& recName : m_programRecords) {
                if (auto record = m_recordFields.find(recName); record != m_recordFields.end()) {
                    if (auto field = record->second.find(name); field != record->second.end()) {
                        if (!matchedRecord.empty()) {
                            matchedRecord.clear();
                            break;
                        }
                        matchedRecord = recName;
                        matchedFieldType = field->second;
                    }
                }
            }
            if (!matchedRecord.empty()) {
                unifyVar(var->id, Type::named(matchedRecord));
                if (callExpr) {
                    m_decidedCalls[callExpr] = {CallKind::FieldRead, matchedRecord};
                }
                return matchedFieldType;
            }
            // Several records may supply the field. That uncertainty does
            // not make an unrelated function its owner: keep the read
            // gradual and let runtime record dispatch select the slot.
            if (mayReadField()) {
                return Type::unknown();
            }
        } else if ((isUndetermined(receiver)) && mayReadField()) {
            return Type::unknown();
        } else if (auto* optional = std::get_if<OptionalType>(&receiver->kind)) {
            // `.field` on an optional used to pass here and then break three
            // different ways at run time — a crash on BEAM, "Undefined
            // method" in the interpreter, or (worst) the whole record
            // silently interpolated where the field was asked for. Neither
            // backend maps a field over an optional, so the checker says it:
            // unwrap first. Only when the inner type really has the field —
            // `.or`/`.map` are method calls that must keep resolving
            // normally (kexhq/kex#294).
            // Once said, the read is done: going on to the method lookup
            // only blamed an unrelated same-named function as well
            // (`name : Weekday -> String`, kexhq/kex#699).
            bool reported = false;
            auto inner = resolve(optional->inner);
            if (auto* innerNamed = std::get_if<NamedType>(&inner->kind)) {
                if (auto record = findRecordFields(*innerNamed);
                    record != m_recordFields.end() && record->second.count(name)) {
                    error(
                        diag::Code::FieldOfOptional, loc,
                        {{"field", name},
                         {"inner", typeToString(inner)},
                         {"receiver", typeToString(receiver)},
                         {"example", ".map { |v| v." + name + " }"}});
                    reported = true;
                } else if (m_importedInterfaces) {
                    auto record = m_importedInterfaces->recordFieldNames.find(innerNamed->name);
                    if (record != m_importedInterfaces->recordFieldNames.end() &&
                        record->second.count(name)) {
                        error(
                            diag::Code::FieldOfOptional, loc,
                            {{"field", name},
                             {"inner", typeToString(inner)},
                             {"receiver", typeToString(receiver)},
                             {"example", ".map { |v| v." + name + " }"}});
                        reported = true;
                    }
                }
            } else if (auto* innerRecord = std::get_if<RecordType>(&inner->kind)) {
                if (std::any_of(
                        innerRecord->fields.begin(), innerRecord->fields.end(),
                        [&](const auto& field) { return field.first == name; })) {
                    error(
                        diag::Code::FieldOfOptional, loc,
                        {{"field", name},
                         {"inner", typeToString(inner)},
                         {"receiver", typeToString(receiver)},
                         {"example", ".map { |v| v." + name + " }"}});
                    reported = true;
                }
            }
            if (reported) {
                return Type::unknown();
            }
        }
    }
    bool hasReceiverRefinementConflict = false;
    if (hasLocalMethods && !argTypes.empty()) {
        auto actual = resolve(argTypes[0]);
        for (const auto& sig : methodIt->second) {
            if (sig.params.empty()) {
                continue;
            }
            auto expected = resolve(sig.params[0]);
            auto* actualNamed = std::get_if<NamedType>(&actual->kind);
            auto* expectedNamed = std::get_if<NamedType>(&expected->kind);
            if (actualNamed && expectedNamed && actualNamed->name == expectedNamed->name) {
                if (!argMatchesParam(actual, expected)) {
                    hasReceiverRefinementConflict = true;
                }
            }
        }
    }

    std::vector<const ImportedFunction*> importedFunctions;
    std::string qualifiedModule;
    // Of those, how many a bare call reaches only as functions of a type,
    // through UFCS (`even?(x)` for `x.even?`).
    std::size_t throughReceiverOnly = 0;
    if (m_importedInterfaces) {
        if (kex::mangle::hasScope(name)) {
            qualifiedModule = kex::mangle::scopeOf(name);
            auto functionName = kex::mangle::memberOf(name);
            if (auto module = m_importedInterfaces->modules.find(qualifiedModule);
                module != m_importedInterfaces->modules.end()) {
                if (auto functions = module->second.exports.find(functionName);
                    functions != module->second.exports.end()) {
                    for (const auto& function : functions->second) {
                        importedFunctions.push_back(&function);
                    }
                }
            }
        } else if (isMethodCall) {
            if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
                functions != m_importedInterfaces->receiverFunctions.end()) {
                for (const auto& function : functions->second) {
                    if (importedFunctionVisible(
                            function, /*throughReceiver=*/true,
                            argTypes.empty() ? nullptr : argTypes[0])) {
                        importedFunctions.push_back(&function);
                    }
                }
            }
            // A function of a module `using` brings in from a unit compiled
            // apart is a method on its first parameter, as one merged into
            // the program is (`text.scan(re)` under `using Regex`).
            // Only for a receiver it takes: `field.parse(text)` is Type.Field's,
            // and an unrelated `parse : OptionConfig -> ...` is no candidate.
            for (const auto* function : usingImportedMethods(name)) {
                if ((argTypes.empty() ||
                     argMatchesParam(argTypes.front(), function->signature.params.front())) &&
                    !offeredAlready(importedFunctions, *function)) {
                    importedFunctions.push_back(function);
                }
            }
        } else {
            // Automatically-imported module exports and receiver functions
            // (callable bare via UFCS, e.g. `even?(x)` instead of `x.even?`)
            // both step aside for a same-arity function the program declares
            // itself, which is what a bare call then runs: `get("hi", 42)`
            // under `using MyMod` dispatches to `MyMod.get`, and
            // `parse("x")` with its own `parse` declared runs that, not
            // whichever prelude module(s) export a same-arity `parse`.
            // Without this, an import pair sharing the name failed the
            // ambiguity check below before the program's own declaration
            // ever got a look (#343); `String.get : String -> Integer ->
            // Char?` matched a call and hid its wrong argument until run
            // time (#324).
            // A value binding the program declares (`let layout = { |b| … }`,
            // a single 0-param signature) called with arguments is a call
            // through the value it holds — the idiom handled below. It
            // shadows imports too: `NumberFormat.layout` in the prelude took
            // `layout(live)` over the program's own `layout` (Slick).
            // A top-level value is not a signature at all (see
            // m_topLevelValueNames).
            const bool declaredValueCalled =
                !argTypes.empty() && ((hasUser && userSignatures->size() == 1 &&
                                       userSignatures->front().params.empty()) ||
                                      (!hasUser && m_topLevelValueNames.count(name)));
            const bool declaredShadowsImport =
                declaredValueCalled ||
                (hasUser && std::any_of(
                                userSignatures->begin(), userSignatures->end(),
                                [&](const Signature& signature) {
                                    return signature.params.size() == argTypes.size();
                                }));
            if (!declaredShadowsImport) {
                for (const auto& [moduleName, module] : m_importedInterfaces->modules) {
                    // Only the file-level prelude's functions are called bare, and
                    // a module's where a `using` brings it in. The other automatic
                    // imports (IO, ...) are there by name: `printLine("a")` checked
                    // clean and was undefined on both backends, which reach it only
                    // as `IO.printLine`.
                    if (!module.automaticImport ||
                        (moduleName != kFileLevelPreludeModule && !makeModuleVisible(moduleName))) {
                        continue;
                    }
                    if (auto functions = module.exports.find(name);
                        functions != module.exports.end()) {
                        for (const auto& function : functions->second) {
                            importedFunctions.push_back(&function);
                        }
                    }
                }
                // And those of a module `using` brings in from a unit
                // compiled apart (`regex("-")` under `using Regex`).
                for (const auto* function : usingImportedMethods(name, false)) {
                    if (!offeredAlready(importedFunctions, *function)) {
                        importedFunctions.push_back(function);
                    }
                }
            }
            if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
                !declaredShadowsImport &&
                functions != m_importedInterfaces->receiverFunctions.end()) {
                for (const auto& function : functions->second) {
                    if (importedFunctionVisible(function, /*throughReceiver=*/false)) {
                        importedFunctions.push_back(&function);
                        ++throughReceiverOnly;
                    }
                }
            }
        }
    }
    // A module declared in the current compilation unit shadows a package
    // module with the same source identity. This matches runtime code-path
    // precedence and prevents imported ownership from hijacking local calls.
    // A receiver method too, on a type one of those modules declares: a
    // stdlib unit compiled with the prelude's interface saw its own
    // `Mock.Files.cannedRead` twice, the source's and the interface's, and
    // left `this.cannedRead(path)` to the value.
    if (!qualifiedModule.empty() && m_localModules.contains(qualifiedModule)) {
        importedFunctions.clear();
    }
    std::erase_if(importedFunctions, [&](const ImportedFunction* function) {
        if (!function->isReceiverMethod || function->signature.params.empty()) {
            return false;
        }
        const auto* receiver = std::get_if<NamedType>(&function->signature.params.front()->kind);
        return receiver && m_localModules.contains(kex::mangle::scopeOf(receiver->name));
    });
    // A bare call this program declares nothing for names a function a
    // `using` brought in from a unit compiled apart (`regex(...)` under
    // `using Regex`): that module's, whichever overload runs.
    if (!isMethodCall && !userSignatures && callExpr && !kex::mangle::hasScope(name) &&
        (std::holds_alternative<ast::FunctionCall>(callExpr->kind) ||
         std::holds_alternative<ast::TaggedLiteral>(callExpr->kind))) {
        std::set<std::string> modules;
        for (const auto* function : importedFunctions) {
            const auto module = m_importedInterfaces->modules.find(function->sourceModule);
            if (!function->isReceiverMethod && !function->sourceModule.empty() &&
                module != m_importedInterfaces->modules.end() &&
                moduleMemberImported(function->sourceModule, name, true)) {
                modules.insert(function->sourceModule);
            }
        }
        if (modules.size() == 1) {
            m_calledFunctions[callExpr] = *modules.begin() + "." + name;
        }
    }
    // A receiver method hidden behind `using` can be the only overload whose
    // labelled arguments and types fit. Decide that here, from local methods
    // and imported interfaces, so the backends do not infer ownership.
    if (isMethodCall && methodCall && !methodCall->namedArgs.empty() &&
        !kex::mangle::hasScope(name) && !argTypes.empty() &&
        std::all_of(argTypes.begin(), argTypes.end(), [&](const TypePtr& type) {
            return !containsOpenType(resolve(type));
        })) {
        std::vector<std::string> labels;
        for (const auto& [label, _] : methodCall->namedArgs) {
            labels.push_back(label);
        }
        const auto writtenPositional = methodCall->args.size();
        const auto positional = writtenPositional + bool(methodCall->block);
        const auto accepts = [&](const Signature& sig, const std::vector<std::string>& paramNames) {
            auto names = paramNames;
            if (names.size() == sig.params.size() && !names.empty()) {
                names.erase(names.begin());
            }
            if (sig.params.empty() || names.size() + 1 != sig.params.size() ||
                argTypes.size() != 1 + positional + labels.size() || !fits(0, sig.params.front())) {
                return false;
            }
            const auto required = sig.requiredParams.value_or(sig.params.size());
            std::vector<bool> defaulted(names.size(), false);
            for (size_t i = required > 0 ? required - 1 : 0; i < defaulted.size(); ++i) {
                defaulted[i] = true;
            }
            if (!namedArgumentsFit(names, defaulted, labels, positional)) {
                return false;
            }
            const auto placed = placeArguments(names, labels, positional);
            for (size_t i = 0; i < positional; ++i) {
                if (!fits(
                        i < writtenPositional ? i + 1 : argTypes.size() - 1,
                        sig.params[placed.positionalSlots[i] + 1])) {
                    return false;
                }
            }
            for (size_t i = 0; i < labels.size(); ++i) {
                if (!fits(1 + writtenPositional + i, sig.params[placed.labelSlots[i] + 1])) {
                    return false;
                }
            }
            return true;
        };
        std::string hidden;
        if (m_importedInterfaces) {
            const auto imported = m_importedInterfaces->receiverFunctions.find(name);
            if (imported != m_importedInterfaces->receiverFunctions.end()) {
                for (const auto& candidate : imported->second) {
                    if (candidate.foreignTarget && !importedFunctionVisible(candidate) &&
                        accepts(candidate.signature, candidate.paramNames) &&
                        (hidden.empty() || candidate.sourceModule < hidden)) {
                        hidden = candidate.sourceModule;
                    }
                }
            }
        }
        if (hasLocalMethods) {
            for (const auto& candidate : methodIt->second) {
                if (candidate.makeModule.empty() || makeModuleVisible(candidate.makeModule) ||
                    candidate.params.empty() || !accepts(candidate, candidate.paramNames)) {
                    continue;
                }
                const auto* receiver =
                    std::get_if<NamedType>(&resolve(candidate.params.front())->kind);
                if (receiver && receiver->name.starts_with(candidate.makeModule + ".")) {
                    continue;
                }
                if (hidden.empty() || candidate.makeModule < hidden) {
                    hidden = candidate.makeModule;
                }
            }
        }
        if (!hidden.empty()) {
            const bool importedMatches = std::any_of(
                importedFunctions.begin(), importedFunctions.end(),
                [&](const ImportedFunction* candidate) {
                    return accepts(candidate->signature, candidate->paramNames);
                });
            const bool localMatches =
                hasLocalMethods && std::any_of(
                                       methodIt->second.begin(), methodIt->second.end(),
                                       [&](const Signature& candidate) {
                                           return makeModuleVisible(candidate.makeModule) &&
                                                  accepts(candidate, candidate.paramNames);
                                       });
            const bool userMatches =
                hasUser && std::any_of(
                               userSignatures->begin(), userSignatures->end(),
                               [&](const Signature& candidate) {
                                   return accepts(candidate, candidate.paramNames);
                               });
            if (!importedMatches && !localMatches && !userMatches) {
                if (callExpr) {
                    m_decidedCalls[callExpr] = {CallKind::Hidden, hidden};
                }
                return Type::unknown();
            }
        }
    }
    if (isMethodCall && !argTypes.empty()) {
        auto actual = resolve(argTypes[0]);
        auto* actualNamed = std::get_if<NamedType>(&actual->kind);
        if (actualNamed) {
            for (const auto* function : importedFunctions) {
                if (function->signature.params.empty()) {
                    continue;
                }
                auto expected = resolve(function->signature.params[0]);
                auto* expectedNamed = std::get_if<NamedType>(&expected->kind);
                if (expectedNamed && expectedNamed->name == actualNamed->name &&
                    !argMatchesParam(actual, expected)) {
                    hasReceiverRefinementConflict = true;
                    break;
                }
            }
        }
    }

    auto sameParams = [](const Signature& left, const Signature& right) {
        if (left.params.size() != right.params.size()) {
            return false;
        }
        for (size_t i = 0; i < left.params.size(); i++) {
            if (!typesEqual(left.params[i], right.params[i])) {
                return false;
            }
        }
        return true;
    };
    auto matchesActual = [&](const Signature& signature) {
        if (signature.params.size() != argTypes.size()) {
            return false;
        }
        for (size_t i = 0; i < signature.params.size(); i++) {
            if (!fits(i, signature.params[i])) {
                return false;
            }
        }
        return true;
    };
    // A type's own `make` function comes before another module's function
    // reached by UFCS with the same parameters, as it does once both are of
    // the program: `user.enroll(password)` is `User`'s, beside
    // `DB.Repositories.Users.enroll(user, hash)` (two units compiled apart).
    if (isMethodCall) {
        const auto offered = importedFunctions;
        std::erase_if(importedFunctions, [&](const ImportedFunction* function) {
            return !function->isReceiverMethod && matchesActual(function->signature) &&
                   std::any_of(offered.begin(), offered.end(), [&](const ImportedFunction* method) {
                       return method->isReceiverMethod &&
                              method->backendModule != function->backendModule &&
                              matchesActual(method->signature) &&
                              sameParams(method->signature, function->signature);
                   });
        });
        // Root-level opt-in stdlib modules are also present in the prebuilt
        // artifact. Once explicitly imported, their owned receiver method
        // supersedes the artifact's duplicate Prelude provider.
        const auto owned = importedFunctions;
        const auto receiver = argTypes.empty() ? Type::unknown() : resolve(argTypes.front());
        const auto* namedReceiver = std::get_if<NamedType>(&receiver->kind);
        std::erase_if(importedFunctions, [&](const ImportedFunction* function) {
            if (!namedReceiver || function->sourceModule != kex::names::kPreludeModuleName ||
                !matchesActual(function->signature)) {
                return false;
            }
            return std::any_of(owned.begin(), owned.end(), [&](const ImportedFunction* method) {
                return method != function && method->isReceiverMethod &&
                       method->sourceModule == namedReceiver->name &&
                       matchesActual(method->signature) &&
                       sameParams(method->signature, function->signature);
            });
        });
    }
    // `importedFunctions` is built from `m_importedInterfaces`'s hash maps,
    // so both which pair a nested loop meets FIRST and which name within a
    // pair ends up `left` vs `right` depend on the host's hash order, not
    // source order — the exact trap kexhq/kex#143 already named, and with
    // three or more same-signature providers (native prelude vs the wasm
    // build's source-derived one can disagree on how many that is) a
    // short-circuiting first-found report picks a different pair per
    // platform on top of that. Scanning every colliding pair and keeping the
    // lexicographically smallest (by its own two names, sorted) makes the
    // report a pure function of WHICH modules collide, never of the order
    // anything happened to iterate them in.
    std::optional<std::pair<std::string, std::string>> ambiguous;
    for (size_t i = 0; i < importedFunctions.size(); i++) {
        for (size_t j = i + 1; j < importedFunctions.size(); j++) {
            const auto& left = *importedFunctions[i];
            const auto& right = *importedFunctions[j];
            if (left.backendModule != right.backendModule && matchesActual(left.signature) &&
                matchesActual(right.signature) && sameParams(left.signature, right.signature)) {
                auto pair = left.backendModule < right.backendModule
                                ? std::make_pair(left.backendModule, right.backendModule)
                                : std::make_pair(right.backendModule, left.backendModule);
                if (!ambiguous || pair < *ambiguous) {
                    ambiguous = std::move(pair);
                }
            }
        }
    }
    if (ambiguous) {
        error(
            isMethodCall && !kex::mangle::hasScope(name)
                ? diag::Code::AmbiguousImportedReceiverFunction
                : diag::Code::AmbiguousImportedFunction,
            loc,
            {{"name", sourceName(name)},
             {"first", ambiguous->first},
             {"second", ambiguous->second}});
        return Type::unknown();
    }

    std::vector<Signature> importedSigs;
    for (const auto* function : importedFunctions) {
        importedSigs.push_back(function->signature);
        // The labels live on the import, not its signature. Without them an
        // overloaded import never counts as a uniform layout below, so
        // `Supervisor.start(restart: :all, [...])` put the list in the
        // `restart` slot instead of the one the named argument left free.
        auto& copied = importedSigs.back();
        if (copied.paramNames.empty() && function->paramNames.size() == copied.params.size()) {
            copied.paramNames = function->paramNames;
        }
    }

    std::vector<Signature> merged;
    const std::vector<Signature>* sigs = nullptr;
    // A module's free function answers a method call (`10.double`) only
    // where its module is in scope: inside it, or under a `using` naming it.
    // Otherwise `findIndex {..}.or(..)` in the prelude's `make String` took
    // `Bits.or`. A prefix or qualified call is scoped by the resolver already.
    const auto freeFunctionInScope = [&](const Signature& signature) {
        return !isMethodCall || kex::mangle::hasScope(name) || signature.declaringModule.empty() ||
               makeModuleVisible(signature.declaringModule);
    };
    // Calls made by ordinary user code should see the full local receiver
    // interface. While checking a make block's own implementation bodies,
    // retain the existing permissive behavior unless the receiver refinement
    // itself is wrong; generic helper calls in those bodies are not yet
    // sufficiently substituted to validate without false positives.
    const bool hasConcreteReceiver = !argTypes.empty() && [&] {
        auto receiver = resolve(argTypes.front());
        return !std::holds_alternative<TypeVar>(receiver->kind) &&
               !std::holds_alternative<UnknownType>(receiver->kind);
    }();
    bool useLocalMethods =
        hasLocalMethods && (!m_inMakeBlock || hasReceiverRefinementConflict || hasConcreteReceiver);
    // Permissive means "not decided here", not "the import": with this
    // unit's own methods held back, a same-named import was the only
    // candidate left, won, and bound the receiver to ITS type — `c.advance`
    // inside `make Cursor` became the prelude's `Parsing.Input.advance`, a
    // false "returns Parsing.Input" error, and a BEAM call that died on the
    // Cursor the walker handled. The value decides when the call runs.
    if (hasLocalMethods && !useLocalMethods && !importedSigs.empty()) {
        if (callExpr) {
            decideDispatch(callExpr, name);
        }
        return Type::unknown();
    }
    // Local methods come FIRST. When a user type defines a method the prelude
    // also provides and both signatures match, the user's is the more specific
    // one. Ordering imported first let a GENERIC prelude signature win on type
    // variables alone: `Map.get :> K -> V?` matched `someMatch.get(1)` and the
    // call lowered to `kex_prelude:get`, while the 3-argument form (whose
    // prelude counterpart did not match) correctly reached the local method —
    // the same call site resolving two different ways by arity.
    // A local method whose receiver does not match still simply isn't a match.
    size_t localSigCount = 0;
    if (!importedSigs.empty() || useLocalMethods) {
        if (useLocalMethods) {
            // A module-scoped `make` is also reachable through a value whose
            // TYPE names that module: `CollisionWeb.Server` already carries
            // the qualification a `using` would supply, and requiring one
            // there would make a method unreachable from anywhere the type
            // itself is nameable.
            const auto receiverModule = [&]() -> std::string {
                if (!isMethodCall || argTypes.empty()) {
                    return {};
                }
                auto* named = std::get_if<NamedType>(&resolve(argTypes[0])->kind);
                if (!named) {
                    return {};
                }
                // `Process.spawn`'s result is `Server<T>`, not `T` itself, so
                // a slot call's receiver carries the WRAPPER's bare name
                // ("Server") here, never a dot — the exemption below always
                // missed it and fell back to requiring `T`'s module be
                // `using`-imported just to call a slot on a value whose type
                // already names it in full (`Server<Catalog.ServingShelf.
                // Catalogue>`), the one shape this whole check exists to
                // exempt. Unwrap to the carried type before reading its
                // module prefix, same as an ordinary (unwrapped) receiver.
                if (named->name == kex::names::kServerType && named->typeArgs.size() == 1) {
                    if (auto* inner =
                            std::get_if<NamedType>(&resolve(named->typeArgs.front())->kind)) {
                        named = inner;
                    }
                }
                const auto dot = named->name.rfind('.');
                return dot == std::string::npos ? std::string{} : named->name.substr(0, dot);
            }();
            for (const auto& method : methodIt->second) {
                // A trait default as the trait declares it (`This -> ...`)
                // answers only in the trait's own bodies: a type implementing
                // the trait has its own copy (registerMakeSignature).
                if (!method.params.empty() && isThisPlaceholder(resolve(method.params.front()))) {
                    if (!argTypes.empty() && isThisPlaceholder(resolve(argTypes.front()))) {
                        merged.push_back(method);
                    }
                    continue;
                }
                if (makeModuleVisible(method.makeModule) ||
                    (!method.makeModule.empty() && method.makeModule == receiverModule)) {
                    merged.push_back(method);
                }
            }
            localSigCount = merged.size();
        }
        merged.insert(merged.end(), importedSigs.begin(), importedSigs.end());
        if (hasUser) {
            for (const auto& signature : *userSignatures) {
                if (freeFunctionInScope(signature)) {
                    merged.push_back(signature);
                }
            }
        }
        // Every candidate may have been filtered out as out-of-scope (a
        // module-scoped `make` with no `using`). That is "no such method", not
        // "a method with no signatures" — fall through to the record-field
        // lookup and the undefined-method report below.
        if (!merged.empty()) {
            sigs = &merged;
        }
    } else if (hasUser) {
        for (const auto& signature : *userSignatures) {
            if (freeFunctionInScope(signature)) {
                merged.push_back(signature);
            }
        }
        if (!merged.empty()) {
            sigs = &merged;
        }
    }
    // A receiver that is a type applied with its head still a variable
    // (`xs: F<Integer>` under `F is Functor`, §8.2): only a higher-kinded
    // trait's requirements answer it, and they answer nothing else.
    if (isMethodCall && !argTypes.empty()) {
        const auto receiver = resolve(argTypes.front());
        if (const auto* applied = std::get_if<TypeApp>(&receiver->kind);
            applied && !std::holds_alternative<NamedType>(resolve(applied->head)->kind)) {
            if (const auto requirements = m_higherKindedRequirements.find(name);
                requirements != m_higherKindedRequirements.end()) {
                merged = requirements->second;
                sigs = &merged;
            }
        }
    }
    bool answeredByRequirement = false;
    // The candidates are the call's own target, not guesses among same-named
    // functions: a mismatch with them is an error, never a reason to let the
    // call through.
    bool authoritative = false;
    // A call typed by a signature that is not its target (a trait's requirement
    // on `this`, the function a record field holds) keeps the decision it had:
    // whatever the match below decides is not kept, and `decision`, when set,
    // is.
    struct Undecided {
        TypeChecker& checker;
        const ast::Expr* call = nullptr;
        const ast::MethodCall* method = nullptr;
        std::optional<DecidedCall> decision;
        ~Undecided() {
            if (call) {
                checker.m_decidedCalls.erase(call);
                checker.m_selectedCallSignatures.erase(call);
                if (decision) {
                    checker.m_decidedCalls[call] = *decision;
                }
            }
            if (method) {
                checker.m_resolvedCalls.erase(method);
            }
        }
    } undecided{*this};
    // A requirement called on `this` in its trait's default: the requirement
    // answers, in the default's own letters (`item` in `this.fold(None) do
    // |acc, item|` is the default's `T`, which `acc` cannot hold).
    if (isMethodCall && m_currentTrait && !argTypes.empty() &&
        isThisPlaceholder(resolve(argTypes.front()))) {
        std::vector<Signature> own;
        for (const auto& required : m_currentTrait->requiredMethods) {
            if (required.name == name) {
                own.push_back(traitRequirementInDefault(required));
            }
        }
        if (!own.empty()) {
            merged = std::move(own);
            sigs = &merged;
            answeredByRequirement = true;
            authoritative = true;
            undecided.call = callExpr;
            undecided.method = methodCall;
        }
    }
    // A field holding a function, called (`route.handler(request)`): that
    // function is the call's one candidate, matched as any other is, its
    // receiver the record. Lowered as the field read it is.
    if (!sigs && isMethodCall && argTypes.size() > 1) {
        if (auto field = calledFieldSignature(name, argTypes)) {
            merged = {std::move(field->first)};
            sigs = &merged;
            undecided.call = callExpr;
            undecided.method = methodCall;
            undecided.decision = DecidedCall{CallKind::FieldRead, field->second};
            authoritative = true;
        }
    }
    if (!sigs) {
        // Record field access: `user.name` desugars to checkCall("name", [User]).
        // Look up the field type in the record registry before giving up.
        // Resolve TypeVars first — the receiver may have been constrained by
        // a previous inference step.
        if (!argTypes.empty()) {
            auto receiver = resolve(argTypes[0]);
            if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
                auto ri = findRecordFields(*named);
                if (ri != m_recordFields.end()) {
                    auto fi = ri->second.find(name);
                    if (fi != ri->second.end()) {
                        // A field holding a function, called, is the call's candidate
                        // above (calledFieldSignature). Here a field is read, or one
                        // holding no function is called: an error unless what it
                        // holds is not known yet.
                        auto fieldType = substituteInterfaceGenerics(fi->second, named->typeArgs);
                        if (argTypes.size() > 1 && !isUndetermined(resolve(fieldType))) {
                            error(
                                diag::Code::WrongArgumentCount, loc,
                                {{"name", sourceName(name)},
                                 {"expected", "1"},
                                 {"actual", std::to_string(argTypes.size())}});
                            return Type::unknown();
                        }
                        if (callExpr) {
                            m_decidedCalls[callExpr] = {
                                CallKind::FieldRead, resolveRecordName(named->name)};
                        }
                        return fieldType;
                    }
                }
            }
            if (auto fieldType = structuralField(receiver, name)) {
                // A field of a structural record (`r: { name: String } & R`).
                if (callExpr) {
                    m_decidedCalls[callExpr] = {CallKind::FieldRead, typeToString(receiver)};
                }
                return fieldType;
            }
            // Trait-bounded receiver: `item.method()` where `item: SomeTrait`.
            // Look up the method in the trait's required methods to get return type.
            if (auto* ct = std::get_if<ConstrainedType>(&receiver->kind)) {
                if (const TraitDef* trait = m_traits.get(ct->traitName)) {
                    for (const auto& req : trait->requiredMethods) {
                        if (req.name == name) {
                            // Its `This` is the receiver: `acc.combine(x)` on an `A`
                            // answers an A.
                            return substituteThis(req.result, receiver);
                        }
                    }
                }
            }
            if (auto* intersection = std::get_if<IntersectionType>(&receiver->kind)) {
                for (const auto& member : intersection->members) {
                    auto* namedTrait = std::get_if<NamedType>(&member->kind);
                    if (!namedTrait) {
                        continue;
                    }
                    if (const TraitDef* trait = m_traits.get(namedTrait->name)) {
                        for (const auto& req : trait->requiredMethods) {
                            if (req.name == name) {
                                return req.result;
                            }
                        }
                    }
                }
            }
            if (auto answered = claimedTraitAnswers(argTypes, name, callExpr)) {
                return answered;
            }
            // If the field name is unambiguously defined on exactly one record
            // type, constrain a TypeVar receiver to that record type.
            if (std::holds_alternative<TypeVar>(receiver->kind)) {
                std::string matchedRecord;
                TypePtr matchedFieldType;
                for (const auto& [recName, fields] : m_recordFields) {
                    auto fi = fields.find(name);
                    if (fi != fields.end()) {
                        if (!matchedRecord.empty()) {
                            matchedRecord.clear();
                            break;
                        }
                        matchedRecord = recName;
                        matchedFieldType = fi->second;
                    }
                }
                if (!matchedRecord.empty()) {
                    if (auto* tv = std::get_if<TypeVar>(&receiver->kind)) {
                        unifyVar(tv->id, Type::named(matchedRecord));
                    }
                    // Applied for the same reason as the named-receiver path
                    // above: arguments after a function-valued field make the
                    // expression a call.
                    for (size_t applied = 1; applied < argTypes.size();) {
                        auto* fn = std::get_if<FuncType>(&matchedFieldType->kind);
                        if (!fn) {
                            break;
                        }
                        applied += std::max<size_t>(fn->params.size(), 1);
                        matchedFieldType = fn->result;
                    }
                    return matchedFieldType;
                }
            }
        }
        // A method call that resolved to nothing at all. It may still be
        // legitimate — a make block further down the file registers its `let`
        // methods as they are CHECKED, so a call above one is simply early —
        // so record it and decide at the end of the program, when everything
        // is registered (see reportUnknownMethods).
        // Reported only with a CONCRETE receiver type. Without one there is
        // nothing to be sure about: a module-qualified call carries no value
        // receiver at all (`Compiled.Late.answer()`), and an inferred TypeVar
        // means the checker simply does not know yet — a constructor-pattern
        // binding carries no type, and cross-file source modules are invisible
        // here. Uppercase names are namespaced CONSTRUCTORS, not methods.
        const bool concreteReceiver = !argTypes.empty() && [&] {
            const auto receiver = resolve(argTypes[0]);
            return !std::holds_alternative<TypeVar>(receiver->kind) &&
                   !std::holds_alternative<UnknownType>(receiver->kind);
        }();
        if (isMethodCall && concreteReceiver && !name.empty() && !kex::mangle::hasScope(name) &&
            !kex::startsUppercase(name)) {
            // `Kex.BACKEND.major` checked clean because `major` is a field
            // of Kex.Version, then died at runtime with badarg. The field
            // lookup above already searched a known record receiver, and a
            // primitive or ADT has no fields at all.
            // Only a DECLARED receiver type counts. One reached through a
            // guess may be wrong: `dep.name` binds `dep` to Weekday, the one
            // type with a `name` method, and `dep.version` is then a record
            // field after all — also once the guess has flowed on through
            // `find` and `.try` and no inference variable is left to see.
            const auto receiver = resolve(argTypes[0]);
            // An optional's field is its value's: when the value has it, the
            // field-on-optional check above reports it with its own advice,
            // and when it has not, the field is no more there than on the
            // value itself. Judged on the value, `.names` on a `Culture?`
            // is undefined as it is on a `Culture` (kexhq/kex#699), not let
            // through because some other record has a `names` field.
            const auto* optional = std::get_if<OptionalType>(&receiver->kind);
            const auto valueType = optional ? resolve(optional->inner) : receiver;
            const auto* named = std::get_if<NamedType>(&valueType->kind);
            // Nor does a list, map, tuple or function: some record in the
            // prelude having a `size` field let `xs.size` check clean on a
            // list, and fail at run time.
            const bool structural = std::holds_alternative<RecordType>(valueType->kind) ||
                                    std::holds_alternative<OptionalType>(valueType->kind) ||
                                    std::holds_alternative<IntersectionType>(valueType->kind) ||
                                    std::holds_alternative<UnionType>(valueType->kind) ||
                                    std::holds_alternative<ConstrainedType>(valueType->kind);
            const bool fieldMayExist =
                std::holds_alternative<TypeVar>(optional ? valueType->kind : argTypes[0]->kind) ||
                (optional && std::holds_alternative<UnknownType>(valueType->kind)) || structural ||
                (named &&
                 !(isPrimitiveTypeName(named->name) || m_adtVariants.count(named->name) ||
                   constructorOwner(*named) || (findRecordFields(*named) != m_recordFields.end())));
            m_unresolvedMethods.push_back(
                {name, loc, typeToString(receiver), fieldMayExist, receiver});
            auto& imports = m_unresolvedMethodImports.emplace_back(m_declarationImports);
            for (const auto& scope : m_importScopeStack) {
                imports.insert(imports.end(), scope.begin(), scope.end());
            }
        }
        // A default the receiver's type inherits from a trait it implements
        // has no signature of its own: the type answers it (OwnMethod).
        if (callExpr && isMethodCall && !argTypes.empty()) {
            if (const auto own = ownTypeAnswers(argTypes, name)) {
                m_decidedCalls[callExpr] = own->decision();
                return Type::unknown();
            }
        }
        // A type with no home module to answer for it (`"-".repeat(3)`,
        // Monoid's default on String): the value picks the body at run time.
        if (callExpr && isMethodCall && concreteReceiver &&
            m_traits.declaresMethod(m_traits.implementorKey(argTypes[0]), name)) {
            decideDispatch(callExpr, name);
            return Type::unknown();
        }
        // Every method of this name is a `make` for a type declared outside
        // its module, in a module not in scope here: the call is undefined
        // wherever it runs, and says which `using` it lacks. A make for its
        // module's own type travels with that type's values, so a call on
        // a receiver the checker could not type may still reach it.
        if (callExpr && hasLocalMethods && importedSigs.empty() && !hasUser) {
            std::string hidden;
            const bool allHidden = std::all_of(
                methodIt->second.begin(), methodIt->second.end(), [&](const Signature& method) {
                    if (method.makeModule.empty() || makeModuleVisible(method.makeModule) ||
                        method.params.empty()) {
                        return false;
                    }
                    const auto* receiver =
                        std::get_if<NamedType>(&resolve(method.params.front())->kind);
                    if (receiver && receiver->name.starts_with(method.makeModule + ".")) {
                        return false;
                    }
                    if (hidden.empty() || method.makeModule < hidden) {
                        hidden = method.makeModule;
                    }
                    return true;
                });
            if (allHidden && !hidden.empty()) {
                m_decidedCalls[callExpr] = {CallKind::Hidden, hidden};
            }
        }
        // Nothing in reach is called that. Checked without the prelude's
        // interfaces, a call may still name one of its functions.
        if (!isMethodCall && callExpr && m_importedInterfaces && !kex::mangle::hasScope(name) &&
            !lookupVar(name) && !bareNameDeclared(name)) {
            reportUndefined(
                std::holds_alternative<ast::TaggedLiteral>(callExpr->kind) ? "tag" : "function",
                name, loc);
        }
        return Type::unknown();
    }

    // Named arguments occupy their declared slots, while positional arguments
    // (including the trailing block) fill the remaining slots in order. Keep
    // omitted optional slots unknown, rather than checking the written named
    // argument order against an unrelated parameter. This is especially
    // important for callback-first APIs with independent optional settings.
    // Only normalize a uniform layout: overloaded layouts need per-candidate
    // placement and must not be guessed from the first signature.
    //
    // A call through a receiver (`t.text(:id, doc: "...")`) has the receiver
    // as its first argument, which no parameter name covers, and its layout
    // is read from the overloads that receiver can take: named arguments
    // used to be checked in written order there, so skipping a defaulted
    // parameter checked the next one against it.
    if (!sigs->empty()) {
        const auto* functionCall =
            callExpr ? std::get_if<ast::FunctionCall>(&callExpr->kind) : nullptr;
        const auto* named = functionCall ? &functionCall->namedArgs
                            : methodCall ? &methodCall->namedArgs
                                         : nullptr;
        const bool throughReceiver = isMethodCall && !kex::mangle::hasScope(name);
        const std::size_t receiverSlots = throughReceiver ? 1 : 0;
        // A label no function of this name in reach declares. Left to the
        // call where one of them does not say its parameters' names.
        if (functionCall && !functionCall->namedArgs.empty() &&
            std::none_of(sigs->begin(), sigs->end(), [](const Signature& sig) {
                return sig.paramNames.empty() && !sig.params.empty();
            })) {
            std::vector<std::string> labels;
            for (const auto& sig : *sigs) {
                labels.insert(labels.end(), sig.paramNames.begin(), sig.paramNames.end());
            }
            std::sort(labels.begin(), labels.end());
            for (const auto& [label, _] : functionCall->namedArgs) {
                if (std::binary_search(labels.begin(), labels.end(), label)) {
                    continue;
                }
                if (const auto hint = closestName(label, labels); !hint.empty()) {
                    error(
                        diag::Code::UnknownNamedArgumentDidYouMean, loc,
                        {{"label", label}, {"name", name}, {"hint", hint}});
                } else {
                    error(
                        diag::Code::UnknownNamedArgument, loc, {{"label", label}, {"name", name}});
                }
            }
        }
        std::vector<const Signature*> layouts;
        for (const auto& sig : *sigs) {
            if (sig.paramNames.empty() ||
                sig.paramNames.size() + receiverSlots != sig.params.size()) {
                continue;
            }
            // A receiver not yet known (a block's parameter) takes any.
            if (throughReceiver) {
                if (argTypes.empty()) {
                    continue;
                }
                const auto receiver = resolve(argTypes.front());
                if (!containsOpenType(receiver) &&
                    !argMatchesParam(receiver, resolve(sig.params.front()))) {
                    continue;
                }
            }
            layouts.push_back(&sig);
        }
        const bool uniform = !layouts.empty() &&
                             (throughReceiver || layouts.size() == sigs->size()) &&
                             std::all_of(layouts.begin(), layouts.end(), [&](const Signature* sig) {
                                 return sig->paramNames == layouts.front()->paramNames &&
                                        sig->params.size() == layouts.front()->params.size() &&
                                        sig->requiredParams == layouts.front()->requiredParams;
                             });
        if (named && !named->empty() && uniform) {
            const auto& layout = *layouts.front();
            const auto positionalCount =
                functionCall ? functionCall->args.size() : methodCall->args.size();
            const bool hasBlock =
                functionCall ? bool(functionCall->block) : bool(methodCall->block);
            std::vector<TypePtr> ordered(layout.params.size());
            bool valid =
                argTypes.size() == receiverSlots + positionalCount + named->size() + hasBlock;
            if (valid && throughReceiver) {
                ordered.front() = argTypes.front();
            }
            for (size_t i = 0; valid && i < named->size(); ++i) {
                const auto it = std::find(
                    layout.paramNames.begin(), layout.paramNames.end(), (*named)[i].first);
                if (it == layout.paramNames.end()) {
                    valid = false;
                    break;
                }
                const auto slot = receiverSlots + std::distance(layout.paramNames.begin(), it);
                if (ordered[slot]) {
                    valid = false;
                    break;
                }
                ordered[slot] = argTypes[receiverSlots + positionalCount + i];
            }
            size_t next = receiverSlots;
            for (size_t i = 0; valid && i < positionalCount + hasBlock; ++i) {
                while (next < ordered.size() && ordered[next]) {
                    ++next;
                }
                if (next == ordered.size()) {
                    valid = false;
                    break;
                }
                ordered[next++] =
                    argTypes[i < positionalCount ? receiverSlots + i : argTypes.size() - 1];
            }
            if (valid) {
                const auto required = layout.requiredParams.value_or(layout.params.size());
                for (size_t i = 0; i < ordered.size(); ++i) {
                    if (!ordered[i] && i < required) {
                        error(
                            diag::Code::MissingRequiredArgument, loc,
                            {{"param", layout.paramNames[i - receiverSlots]},
                             {"name", sourceName(name)}});
                        return Type::unknown();
                    }
                    if (!ordered[i]) {
                        ordered[i] = Type::unknown();
                    }
                }
                argTypes = std::move(ordered);
            }
        }
    }

    // `let hello = makeGreeter("Hello")` is a top-level zero-arg binding
    // (every top-level `let NAME = EXPR` is a 0-param function — see
    // Parser::parseFunctionDef), and referencing it as a bare identifier
    // auto-calls it (Evaluator::autoCallZeroArgConstant). `hello("Alice")`
    // is the same idiom through a call: auto-call `hello` to get the
    // closure `makeGreeter` returned, then apply "Alice" to *that* — not
    // "call hello with 1 argument," which is what zero arity would
    // otherwise mean. Only a single, unambiguous 0-param signature
    // triggers this (an overload set with a 0-param AND non-0-param
    // signature is a real arity question, not this idiom).
    if (sigs->size() == 1 && (*sigs)[0].params.empty() && !argTypes.empty()) {
        // The value the binding holds is what is called.
        if (callExpr) {
            m_decidedCalls[callExpr] = {CallKind::ValueCall, name};
        }
        const auto held = resolve((*sigs)[0].result);
        if (const auto* function = std::get_if<FuncType>(&held->kind)) {
            if (function->params.size() == argTypes.size()) {
                for (size_t i = 0; i < argTypes.size(); ++i) {
                    if (!argMatchesParam(argTypes[i], function->params[i])) {
                        typeMismatch(loc, function->params[i], argTypes[i]);
                    }
                }
                return function->result;
            }
            error(
                diag::Code::ReturnedFunctionArity, loc,
                {{"name", sourceName(name)},
                 {"expected", std::to_string(function->params.size())},
                 {"actual", std::to_string(argTypes.size())}});
            return function->result;
        }
        // What it returns is no function (`Time.now(22)`): there is nothing
        // to apply the arguments to. Undetermined, or a Block named as such,
        // it may be one.
        const auto* named = std::get_if<NamedType>(&held->kind);
        if (!containsOpenType(held) && !(named && named->name == kex::names::kBlockType)) {
            error(
                diag::Code::TakesNoArguments, loc,
                {{"name", sourceName(name)}, {"actual", std::to_string(argTypes.size())}});

            return held;
        }
        return Type::unknown();
    }

    // `This` (the trait placeholder) has no substitution mechanism implemented yet, so
    // it can leak through as a literal NamedType("This") from a `param :
    // This` annotation inside a `make` block. A call involving it can't be
    // meaningfully checked against any signature here — reporting a
    // mismatch against the wrong (imported or unrelated) candidate would be
    // actively misleading, so bail out rather than guess.
    for (const auto& argType : argTypes) {
        if (isThisPlaceholder(argType) && !answeredByRequirement) {
            return Type::unknown();
        }
    }

    // Trait-bounded receiver: `item.describe()` where `item: Describable`.
    // The trait method takes priority over imported overloads of the same name.
    if (!argTypes.empty()) {
        auto receiver = resolve(argTypes[0]);
        if (auto* ct = std::get_if<ConstrainedType>(&receiver->kind)) {
            if (const TraitDef* trait = m_traits.get(ct->traitName)) {
                for (const auto& req : trait->requiredMethods) {
                    if (req.name == name) {
                        return req.result;
                    }
                }
            }
        }
    }

    // Namespace call heuristic: `BuiltIn.foo(x)` or any call where the
    // receiver resolves to Unknown (an undefined namespace sentinel like
    // `BuiltIn`). The UFCS desugaring adds the receiver as argTypes[0], but
    // if the receiver is Unknown and no overload matches the full arity while
    // overloads DO match with the receiver dropped, treat it as a plain
    // function call through a namespace — drop argTypes[0] and re-check.
    //
    // Not for a known module's function called by its qualified name
    // (`Type.fieldValues(value, 0)`): its arguments carry no receiver, so the
    // first one is an argument, and dropping it because it is an `Any` let an
    // extra argument through to fail at run time.
    if (!argTypes.empty() && (isUndetermined(argTypes[0])) && !kex::mangle::hasScope(name)) {
        bool anyFullMatch = false;
        bool anyDroppedMatch = false;
        for (const auto& sig : *sigs) {
            if (sig.params.size() == argTypes.size()) {
                bool matches = true;
                for (size_t i = 0; i < sig.params.size(); i++) {
                    if (!fits(i, sig.params[i])) {
                        matches = false;
                        break;
                    }
                }
                anyFullMatch = anyFullMatch || matches;
            }
            if (sig.params.size() == argTypes.size() - 1) {
                bool matches = true;
                for (size_t i = 0; i < sig.params.size(); i++) {
                    if (!fits(i + 1, sig.params[i])) {
                        matches = false;
                        break;
                    }
                }
                anyDroppedMatch = anyDroppedMatch || matches;
            }
        }
        if (!anyFullMatch && anyDroppedMatch) {
            return checkCall(name, {argTypes.begin() + 1, argTypes.end()}, loc);
        }
    }

    // Name collision guard: a make-block method isn't registered here at
    // all (see checkFunctionDef), so `this.modulo(...)` inside a
    // user-defined `make CustomType do let modulo(...) ... end` would
    // otherwise get checked against an imported `modulo` signature
    // purely because the names match — a different, unrelated function.
    // Applies to any receiver type (NamedType or primitive) — if no
    // overload's first param can accept the receiver, this is a
    // make-block method call, not a known function call. For primitive
    // receivers outside of make blocks (m_currentMakeType is empty),
    // imported interfaces are authoritative and a mismatch is a real
    // type error.
    if (isMethodCall && !argTypes.empty() &&
        (std::holds_alternative<NamedType>(argTypes[0]->kind) ||
         (!hasUser && m_currentMakeType &&
          (std::holds_alternative<PrimitiveType>(argTypes[0]->kind) ||
           std::holds_alternative<SizedIntType>(argTypes[0]->kind))))) {
        bool anyFirstParamPlausible = false;
        bool anyConstrainedFirstParam = false;
        // A candidate carrying an Unknown parameter is one whose real contract
        // the checker never learned — the shape-only entries the source-derived
        // interface path records for un-annotated `make` methods, where the
        // compiled artifacts would have given real types. A method with any
        // such candidate has a candidate list that cannot be treated as the
        // whole truth, so no mismatch is provable from it. TYPE VARIABLES are
        // a different thing entirely and stay provable: `[A]` is a known,
        // genuinely generic contract.
        bool anyUnknownParamCandidate = false;
        for (const auto& sig : *sigs) {
            // A signature this call could not possibly reach says nothing
            // about it. `write` is FileHandle's at 2 parameters and a
            // capability stand-in's at 3, and letting the 3-parameter one
            // count as an "unknown candidate" switched off the mismatch report
            // for the 2-argument call — writing through a read-only handle
            // stopped being an error the moment any such stand-in existed
            // (kexhq/kex#143). Same range the overload match below uses.
            const std::size_t requiredParams = sig.requiredParams.value_or(sig.params.size());
            if (argTypes.size() < requiredParams || argTypes.size() > sig.params.size()) {
                continue;
            }
            if (!sig.params.empty()) {
                if (std::holds_alternative<ConstrainedType>(sig.params[0]->kind)) {
                    anyConstrainedFirstParam = true;
                }
                // Only an unknown RECEIVER leaves the candidate's reach
                // unknown. One whose receiver is known and refuses this one
                // is not the function called, whatever else it left
                // unannotated: Mock's `read : Mock.Files -> ? -> ...` made
                // `layer.read(1)` on a record check clean.
                if (mentionsUnknownParam(sig.params[0]) || fits(0, sig.params[0])) {
                    for (const auto& param : sig.params) {
                        if (mentionsUnknownParam(param)) {
                            anyUnknownParamCandidate = true;
                            break;
                        }
                    }
                }
                if (fits(0, sig.params[0])) {
                    anyFirstParamPlausible = true;
                    break;
                }
            }
        }
        if (isMethodCall && anyConstrainedFirstParam && !anyFirstParamPlausible) {
            auto receiverKey = m_traits.implementorKey(argTypes[0]);
            if (!m_traits.hasConformances(receiverKey)) {
                anyConstrainedFirstParam = false;
            }
        }
        // A receiver no candidate accepts is usually the guard's case above —
        // but not when the receiver type is fully known and we are in
        // ordinary code. `Date.of(...).iso` (a `Date` method on a
        // `Result<Date, TimeError>`) landed here and was waved through, which
        // is why it only failed at runtime, as `if_clause` on BEAM. Fall
        // through to the mismatch report instead.
        //
        // Inside a make block the tables are incomplete on purpose (the type
        // under check has its own methods unregistered — see checkFunctionDef),
        // and a gradual receiver genuinely isn't known, so both stay permissive.
        // `date.day` is a field read, not a call to Integer's `day`. Local
        // record layouts are known by name; an IMPORTED record's are not —
        // `recordArities` counts fields without naming them — so any field
        // access on one has to stay permissive until interfaces carry field
        // names (they exist in KexI metadata; display registration uses them).
        const bool receiverIsField = [&] {
            auto* named = std::get_if<NamedType>(&argTypes[0]->kind);
            if (!named) {
                return false;
            }
            if (auto record = findRecordFields(*named); record != m_recordFields.end()) {
                return record->second.count(name) > 0;
            }
            return m_importedInterfaces &&
                   m_importedInterfaces->recordArities.count(named->name) > 0;
        }();
        // The receiver's own shape is what decides this, not its type
        // arguments: `Ok(b)` is `Result<Box, ?>`, and the unknown error side
        // says nothing about whether some `label` accepts a Result. A receiver
        // that is ITSELF unknown or a type variable stays permissive, and the
        // enclosing condition already restricts this to those shapes.
        // A method a trait gives the receiver's type — required or defaulted —
        // is legitimate even though no signature names that type: an
        // inherited default is registered under whichever type overrode it,
        // if any (`shout : Player -> String` while `Bot` inherits it).
        const bool traitProvides =
            m_traits.declaresMethod(m_traits.implementorKey(argTypes[0]), name);
        if (!anyFirstParamPlausible && traitProvides) {
            if (const auto [trait, required] = m_traits.requiredMethod(argTypes[0], name);
                required && missesRequiredArity(*required, argTypes.size(), name, loc)) {
                return required->result;
            }
        }
        // Inside a `make` block, its own type calling one of its own functions
        // is as provable as outside it: every function of the block is
        // registered before any body is checked (`this.countdown()` for
        // `countdown(k)`).
        const bool ownBlockCall = [&] {
            if (!m_inMakeBlock || !ownFunctionOfMake(name)) {
                return false;
            }
            const auto* receiver = std::get_if<NamedType>(&resolve(argTypes[0])->kind);
            const auto* own = std::get_if<NamedType>(&resolve(m_currentMakeType)->kind);
            return receiver && own && ownTypeIdentity(receiver->name) == ownTypeIdentity(own->name);
        }();
        const bool provableMismatch =
            (!m_inMakeBlock || ownBlockCall) && isFullyConcrete(headShapeOf(argTypes[0])) &&
            !receiverIsField && !traitProvides && !anyUnknownParamCandidate;
        // A receiver only an out-of-scope `make` takes (`d.number` on a `Day`
        // whose `make` is `Cal`'s, without `using Cal`) has no visible method,
        // whatever same-named methods other types have.
        if (!anyFirstParamPlausible && callExpr) {
            if (auto hidden = hiddenMakeTaking(name, argTypes); !hidden.empty()) {
                m_decidedCalls[callExpr] = {CallKind::Hidden, hidden};
                return Type::unknown();
            }
        }
        // A default the type inherits answers the call: the type's own.
        if (!anyFirstParamPlausible && traitProvides && callExpr) {
            if (const auto own = ownTypeAnswers(argTypes, name)) {
                m_decidedCalls[callExpr] = own->decision();
                return Type::unknown();
            }
        }
        // Not when the type's own method rejects this refinement: writing
        // through a read-only FileHandle is a mismatch, whatever Writable says.
        if (!anyFirstParamPlausible && traitProvides && !hasReceiverRefinementConflict) {
            if (auto answered = claimedTraitAnswers(argTypes, name, callExpr)) {
                return answered;
            }
        }
        // A trait default stands in for a method the receiver's type has no
        // signature for. Where the type does have typed methods of this name,
        // only at other arities (Measure's `to(String)` and its overloads,
        // with Showable's default beside them), the call has the wrong number
        // of arguments: `1500.gram.to()` checked clean, and BEAM found no
        // clause for it.
        const bool typedAtOtherArity =
            traitProvides && std::any_of(sigs->begin(), sigs->end(), [&](const Signature& sig) {
                if (sig.params.empty()) {
                    return false;
                }
                const auto first = resolve(sig.params.front());
                return !std::holds_alternative<TypeVar>(first->kind) &&
                       !mentionsUnknownParam(first) && fits(0, sig.params.front());
            });
        if (!authoritative && !anyFirstParamPlausible && !typedAtOtherArity &&
            !anyConstrainedFirstParam && !hasReceiverRefinementConflict && !provableMismatch) {
            // A default the receiver's type inherits, with no home module to
            // answer for it (`(0..4).map`, Enumerable's on Range): the value
            // picks the body at run time.
            if (callExpr && traitProvides) {
                decideDispatch(callExpr, name);
            }
            return Type::unknown();
        }
    }

    const auto genericConflictOf = [&](const Signature& sig) {
        return genericConflict(sig, argTypes, selectedType);
    };
    std::vector<const Signature*> arityMatches;
    std::vector<const Signature*> fullMatches;
    for (const auto& sig : *sigs) {
        const auto* functionCall =
            callExpr ? std::get_if<ast::FunctionCall>(&callExpr->kind) : nullptr;
        const auto* named = functionCall ? &functionCall->namedArgs
                            : methodCall ? &methodCall->namedArgs
                                         : nullptr;
        // A candidate whose parameter names cannot take these labels is no
        // candidate: `m.to(String, in: 5)` is not `to(value, t, radix:)`.
        // Through a receiver the names may or may not list the receiver,
        // which no label can name.
        const bool throughReceiver = isMethodCall && !kex::mangle::hasScope(name);
        auto names = sig.paramNames;
        if (throughReceiver && !names.empty() && names.size() == sig.params.size()) {
            names.erase(names.begin());
        }
        const std::size_t receiverSlots = throughReceiver ? 1 : 0;
        if (named && !named->empty() && !names.empty() &&
            names.size() + receiverSlots == sig.params.size()) {
            std::vector<std::string> labels;
            for (const auto& [label, _] : *named) {
                labels.push_back(label);
            }
            const bool labelsKnown =
                std::all_of(labels.begin(), labels.end(), [&](const std::string& label) {
                    return std::any_of(sigs->begin(), sigs->end(), [&](const Signature& candidate) {
                        return std::find(
                                   candidate.paramNames.begin(), candidate.paramNames.end(),
                                   label) != candidate.paramNames.end();
                    });
                });
            const auto required = sig.requiredParams.value_or(sig.params.size());
            std::vector<bool> defaulted(names.size(), false);
            for (std::size_t i = required > receiverSlots ? required - receiverSlots : 0;
                 i < defaulted.size(); ++i) {
                defaulted[i] = true;
            }
            const auto positional = functionCall
                                        ? functionCall->args.size() + bool(functionCall->block)
                                        : methodCall->args.size() + bool(methodCall->block);
            if (labelsKnown && !namedArgumentsFit(names, defaulted, labels, positional)) {
                continue;
            }
        }

        // A trailing parameter with a default may be omitted, so the accepted
        // range is [requiredParams, params.size()] rather than one number.
        const std::size_t required = sig.requiredParams.value_or(sig.params.size());
        if (argTypes.size() < required || argTypes.size() > sig.params.size()) {
            continue;
        }
        arityMatches.push_back(&sig);

        // Only the arguments actually PASSED are checked: an omitted
        // defaulted parameter has no argument to compare against.
        bool allMatch = true;
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (!fits(i, sig.params[i])) {
                allMatch = false;
                break;
            }
        }
        if (allMatch && !genericConflictOf(sig)) {
            fullMatches.push_back(&sig);
        }
    }

    // A trait receiver is a fallback for concrete implementations of the
    // same method.  Once a concrete receiver signature matches, do not let a
    // universal trait implementation win merely because another parameter in
    // its signature is more precise.  In particular, Measure.to(String) must
    // keep precedence over Showable.to(String).
    if (isMethodCall && fullMatches.size() > 1 && !argTypes.empty()) {
        const bool hasConcreteReceiverMatch =
            std::any_of(fullMatches.begin(), fullMatches.end(), [](const Signature* signature) {
                return !signature->params.empty() &&
                       !std::holds_alternative<ConstrainedType>(signature->params.front()->kind) &&
                       !std::holds_alternative<TypeVar>(signature->params.front()->kind) &&
                       !std::holds_alternative<UnknownType>(signature->params.front()->kind) &&
                       !std::holds_alternative<TypeApp>(signature->params.front()->kind);
            });
        // A higher-kinded trait's requirement (`map` on any `F<A>`, §8.2) is
        // such a universal one too: `[1].map` is the list's own.
        if (hasConcreteReceiverMatch) {
            std::erase_if(fullMatches, [](const Signature* signature) {
                return !signature->params.empty() &&
                       (std::holds_alternative<ConstrainedType>(signature->params.front()->kind) ||
                        std::holds_alternative<TypeApp>(signature->params.front()->kind));
            });
        }
    }

    // 5c: Pick the most-specific full match when there are several.
    // Specificity per param: concrete named/list/func type (2) > trait-constrained (1) > TypeVar/Unknown (0).
    // A signature A dominates B if A >= B at every position and > at least one.
    // Concrete ties between explicitly overlapping union/constrained signatures
    // are ambiguous. Unknown/generic clause sets retain deterministic fallback,
    // and identical signatures remain ordinary multi-clause functions.
    if (fullMatches.size() > 1) {
        auto paramSpec = [](const TypePtr& p) -> int {
            if (std::holds_alternative<TypeVar>(p->kind) ||
                std::holds_alternative<UnknownType>(p->kind)) {
                return 0;
            }
            if (std::holds_alternative<ConstrainedType>(p->kind)) {
                return 1;
            }
            return 2;
        };
        auto dominates = [&](const Signature* a, const Signature* b) {
            bool aWins = false;
            // The positions passed: a defaulted parameter leaves one side longer.
            for (size_t i = 0; i < argTypes.size() && i < a->params.size() && i < b->params.size();
                 i++) {
                int sa = paramSpec(a->params[i]);
                int sb = paramSpec(b->params[i]);
                if (sb > sa) {
                    return false;
                }
                if (sa > sb) {
                    aWins = true;
                }
            }
            return aWins;
        };
        std::vector<const Signature*> undominated;
        for (const auto* cand : fullMatches) {
            bool dominated = false;
            for (const auto* other : fullMatches) {
                if (other == cand) {
                    continue;
                }
                if (dominates(other, cand)) {
                    dominated = true;
                    break;
                }
            }
            if (!dominated) {
                undominated.push_back(cand);
            }
        }
        // Overloads that take blocks of different shapes (`times` with the
        // index or without) both accept a block of no parameters: the block
        // as written says which one it is for.
        {
            const auto* functionCall =
                callExpr ? std::get_if<ast::FunctionCall>(&callExpr->kind) : nullptr;
            const auto* blockExpr = functionCall && functionCall->block ? functionCall->block->get()
                                    : methodCall && methodCall->block   ? methodCall->block->get()
                                                                        : nullptr;
            if (const auto* written =
                    blockExpr ? std::get_if<ast::Lambda>(&blockExpr->kind) : nullptr) {
                const auto takesIt = [&](const Signature* candidate) {
                    if (candidate->params.empty()) {
                        return false;
                    }
                    const auto* block =
                        std::get_if<FuncType>(&resolve(candidate->params.back())->kind);
                    return block && block->params.size() == written->params.size();
                };
                if (std::any_of(undominated.begin(), undominated.end(), takesIt)) {
                    std::erase_if(undominated, [&](const Signature* candidate) {
                        return !takesIt(candidate);
                    });
                }
            }
        }
        // Matching a candidate can bind an argument's type variable: a
        // tagged literal's `re` result became `Regex` while the Regex
        // overload of `replace` was tried, after the prelude's
        // `replace(String, String, String)` had already matched it unbound.
        // Choosing among the stale matches then sent `text.replace(re`y`, …)`
        // to the String function and BEAM died with badarg (kexhq/kex#560).
        // Keep only the candidates the arguments still match, as they now
        // resolve. Every candidate already passed the arity check above, a
        // defaulted trailing parameter left out included, so only the
        // arguments actually passed are compared, as there.
        {
            std::vector<const Signature*> stillMatching;
            for (const auto* candidate : undominated) {
                bool matches = true;
                for (size_t i = 0; matches && i < argTypes.size(); ++i) {
                    matches = argMatchesParam(resolve(argTypes[i]), resolve(candidate->params[i]));
                }
                if (matches) {
                    stillMatching.push_back(candidate);
                }
            }
            if (!stillMatching.empty() && stillMatching.size() < undominated.size()) {
                undominated = std::move(stillMatching);
            }
        }
        std::vector<const Signature*> distinct;
        for (const auto* candidate : undominated) {
            bool duplicate =
                std::any_of(distinct.begin(), distinct.end(), [&](const Signature* other) {
                    return sameParameters(*other, *candidate);
                });
            if (!duplicate) {
                distinct.push_back(candidate);
            }
        }
        const Signature* best = undominated.empty() ? fullMatches.front() : undominated.front();
        // A method call dispatches on its RECEIVER, so a candidate whose first
        // parameter is exactly the receiver's type is the one written for it.
        // Equally specific candidates otherwise fall back to declaration
        // order, which picked `factor : SIUnit -> _` for a `Measure` receiver
        // as soon as the receiver stopped being inferred as Unknown.
        if (isMethodCall && !argTypes.empty()) {
            const auto receiver = resolve(argTypes.front());
            for (const auto* candidate : undominated) {
                if (!candidate->params.empty() &&
                    typesEqual(resolve(candidate->params.front()), receiver)) {
                    best = candidate;
                    break;
                }
            }
        }
        const bool concreteArguments =
            std::all_of(argTypes.begin(), argTypes.end(), [&](const TypePtr& argument) {
                auto resolved = resolve(argument);
                return !std::holds_alternative<TypeVar>(resolved->kind) &&
                       !std::holds_alternative<UnknownType>(resolved->kind);
            });
        bool typedCandidates = false;
        for (size_t left = 0; left < distinct.size() && !typedCandidates; ++left) {
            for (size_t right = left + 1; right < distinct.size() && !typedCandidates; ++right) {
                // The parameters both have: a defaulted one (`string(prefix =
                // "#")` beside URI's `string`) gives one more, and reading past
                // the other's end crashed the checker (found by `make fuzz-lsp`).
                const auto shared =
                    std::min(distinct[left]->params.size(), distinct[right]->params.size());
                for (size_t i = 0; i < shared; ++i) {
                    const auto& a = distinct[left]->params[i];
                    const auto& b = distinct[right]->params[i];
                    if (typesEqual(a, b)) {
                        continue;
                    }
                    const bool explicitOverlap =
                        (std::holds_alternative<UnionType>(a->kind) &&
                         std::holds_alternative<UnionType>(b->kind)) ||
                        (std::holds_alternative<ConstrainedType>(a->kind) &&
                         std::holds_alternative<ConstrainedType>(b->kind));
                    if (explicitOverlap) {
                        typedCandidates = true;
                        break;
                    }
                }
            }
        }
        if (distinct.size() > 1 && concreteArguments && typedCandidates) {
            std::string signatures;
            for (const auto* candidate : distinct) {
                signatures += "\n\n" + displaySignature(name, *candidate);
            }
            error(
                diag::Code::AmbiguousOverload, loc,
                {{"name", sourceName(name)}, {"signatures", signatures}});
        }
        // An unconstrained shorthand receiver (for example `&.kilo`) can
        // match several concrete receiver overloads. Picking the first one
        // would permanently specialize the closure to declaration order;
        // keep its intermediate result permissive until the surrounding
        // method chain or a call site supplies a concrete receiver.
        if (distinct.size() > 1 && !concreteArguments) {
            if (callExpr) {
                decideDispatch(callExpr, name);
            }
            // A parameter of the function being checked, written without a
            // type and passed where the overloads part ways, takes what they
            // take between them: `let show(x) = describe(x)` with `describe`
            // on Integer and on String makes `x` an `Integer | String`, so
            // `show(2.5)` is refused where it is written rather than reaching
            // no clause at run time (kexhq/kex#643).
            if (!isMethodCall) {
                for (size_t i = 0; i < argTypes.size(); ++i) {
                    if (const auto* var = std::get_if<TypeVar>(&resolve(argTypes[i])->kind);
                        var && isInferenceVariable(*var) && var->id >= m_clauseVarMark) {
                        if (auto taken = overloadsTake(distinct, i)) {
                            unifyVar(var->id, taken);
                        }
                    }
                }
            }
            // One result whichever runs is that result.
            const auto result = resolve(distinct.front()->result);
            if (isConcreteValue(result) &&
                std::all_of(distinct.begin(), distinct.end(), [&](const Signature* candidate) {
                    return typesEqual(resolve(candidate->result), result);
                })) {
                return result;
            }
            return Type::unknown();
        }
        if (best) {
            // Rebuild fullMatches with best first so the code below uses it
            std::vector<const Signature*> reordered = {best};
            for (const auto* s : fullMatches) {
                if (s != best) {
                    reordered.push_back(s);
                }
            }
            fullMatches = std::move(reordered);
        }
    }

    // When every full match is vacuous (params all Unknown/TypeVar, or only
    // trait bounds — e.g. a trait default method with no type annotation) but
    // concrete-typed arity matches exist that DIDN'T match while agreeing on
    // the receiver, prefer the concrete sigs for error reporting so real type
    // mismatches aren't masked by the generic overload.
    if (!fullMatches.empty()) {
        auto isVacuous = [](const Signature* sig) {
            for (const auto& p : sig->params) {
                if (!std::holds_alternative<TypeVar>(p->kind) &&
                    !std::holds_alternative<UnknownType>(p->kind) &&
                    !std::holds_alternative<ConstrainedType>(p->kind)) {
                    return false;
                }
            }
            return true;
        };
        bool allVacuous = true;
        for (const auto* fm : fullMatches) {
            if (!isVacuous(fm)) {
                allVacuous = false;
                break;
            }
        }
        if (allVacuous) {
            bool hasConcrete = false;
            for (const auto* am : arityMatches) {
                if (isVacuous(am) ||
                    std::find(fullMatches.begin(), fullMatches.end(), am) != fullMatches.end()) {
                    continue;
                }
                // Only a concrete sig that accepts the FIRST argument speaks
                // for this call. For a receiver call that argument is the
                // receiver, and a concrete sig for some other receiver type
                // must not mask a legitimately matching generic default (a
                // user type's trait default method, say).
                //
                // The same holds for a plain call, where the rule used to be
                // skipped: the prelude's receiver functions are all callable
                // bare through UFCS, so `merge : {A: B} -> {A: B} -> {A: B}`
                // was a "concrete candidate" for a user's own
                // `merge : Number -> Number -> Number`, cleared its full match
                // and reported `merge(4, 2)` as expecting a Map. Any prelude
                // method name a program reused as a free function with
                // trait-bounded parameters was unusable (kexhq/kex#208).
                if (!am->params.empty() && !argTypes.empty() && !fits(0, am->params[0])) {
                    continue;
                }
                hasConcrete = true;
                break;
            }
            if (hasConcrete) {
                fullMatches.clear();
            }
        }
    }

    // With no evidence about the receiver, prefer an overload whose receiver
    // is itself generic. That one is written to accept anything; an overload
    // naming a concrete receiver is a specialization for a type this call has
    // no reason to believe in. Without the preference the winner was whichever
    // sorted first, so `let show(x) = x.to(String)` answered `String` — the
    // result of units.kex's `to(measure: Measure, String)` — where the general
    // `to(value, String)` in optional.kex says `String?`.
    if (isMethodCall && fullMatches.size() > 1 && !argTypes.empty()) {
        const auto receiver = resolve(argTypes[0]);
        if (std::holds_alternative<TypeVar>(receiver->kind) ||
            std::holds_alternative<UnknownType>(receiver->kind)) {
            // A trait-bounded receiver (`Showable`) outranks a bare variable:
            // it accepts anything that satisfies the bound AND says more about
            // the result. `x.to(String)` picks `to : Showable -> String?` over
            // the untyped `to(value, t)`, whose result is only `unknown?`.
            const auto rank = [&](const Signature* candidate) {
                if (candidate->params.empty()) {
                    return 2;
                }
                const auto parameter = resolve(candidate->params[0]);
                if (std::holds_alternative<ConstrainedType>(parameter->kind)) {
                    return 0;
                }
                // A trait may reach here as a plain named type rather than a
                // constrained one; `Showable` is a bound, not a concrete type.
                if (const auto* named = std::get_if<NamedType>(&parameter->kind);
                    named && isTrait(named->name)) {
                    return 0;
                }
                if (std::holds_alternative<TypeVar>(parameter->kind) ||
                    std::holds_alternative<UnknownType>(parameter->kind)) {
                    return 1;
                }
                return 2;
            };
            std::stable_sort(
                fullMatches.begin(), fullMatches.end(),
                [&](const Signature* left, const Signature* right) {
                    return rank(left) < rank(right);
                });
            // Two candidates for DIFFERENT concrete receiver types (a
            // local `Cursor.advance` beside the prelude's
            // `Parsing.Input.advance`), and nothing says which this value
            // is: no choice here is a decision. Picking the first bound the
            // receiver to it — a false "returns Parsing.Input" error, and an
            // import lowering then called on a Cursor. The value decides.
            std::set<std::string> receiverHeads;
            for (const auto* candidate : fullMatches) {
                if (rank(candidate) == 2) {
                    receiverHeads.insert(typeToString(headShapeOf(resolve(candidate->params[0]))));
                }
            }
            if (receiverHeads.size() > 1) {
                if (callExpr) {
                    decideDispatch(callExpr, name);
                }
                return Type::unknown();
            }
        }
    }

    if (fullMatches.size() >= 1) {
        const auto& matched = *fullMatches[0];
        // Set when this call's receiver type is unknown AND more than one
        // provider could answer it, so nothing here may be treated as
        // evidence about the receiver — not the backend target, and not the
        // receiver's type.
        if (!methodCall && callExpr && sigs == &merged) {
            const auto selected = static_cast<size_t>(fullMatches[0] - merged.data());
            if (selected >= localSigCount && selected - localSigCount < importedFunctions.size()) {
                m_calledModules[callExpr] =
                    importedFunctions[selected - localSigCount]->sourceModule;
            }
        }
        bool ambiguousReceiver = false;
        if (methodCall) {
            bool isReceiver = !kex::mangle::hasScope(name);
            const ImportedFunction* resolved = nullptr;
            // `merged` is importedSigs ++ local methods ++ user functions, so
            // the winner's index says where it came from. When it came from a
            // LOCAL make-block method, the call must not be bound to an
            // imported target — doing so is how `b.get(0)` on a user record
            // with its own `get` was lowered to `kex_prelude:get/2` and died
            // at runtime, purely because the prelude also has a `get`.
            bool winnerIsLocal = false;
            bool winnerIsMakeMethod = false;
            if (sigs == &merged) {
                auto selected = static_cast<size_t>(fullMatches[0] - merged.data());
                if (selected < localSigCount) {
                    winnerIsLocal = true;
                    winnerIsMakeMethod = true;
                } else if (
                    selected >= localSigCount &&
                    selected - localSigCount < importedFunctions.size()) {
                    resolved = importedFunctions[selected - localSigCount];
                } else {
                    winnerIsLocal = true;
                }
            }
            if (!resolved && !winnerIsLocal) {
                int matchedArity = static_cast<int>(matched.params.size());
                for (const auto* candidate : importedFunctions) {
                    if (candidate->backendArity == matchedArity) {
                        resolved = candidate;
                        break;
                    }
                }
            }
            // Pinning a receiver call to one provider is only sound when the
            // receiver's type is actually known. It often is not — a binding
            // from a constructor pattern (`Just(d) ->`) carries no payload
            // type yet — and then every same-arity signature "matches", so the
            // winner is whichever came first. `d.get(:year, "?")` on a Map
            // bound to `Kex.Regex:get/3` (Match's accessor) and died with
            // function_clause on BEAM, while the walker dispatched on the
            // runtime value and read the map (examples/regexes.kex).
            //
            // Leaving the call unpinned costs nothing: it lowers to the same
            // runtime-dispatching path the walker takes.
            const bool receiverUnknown = [&] {
                if (!isReceiver || argTypes.empty()) {
                    return false;
                }
                const auto receiver = resolve(argTypes[0]);
                return isUndetermined(receiver);
            }();
            if (resolved && receiverUnknown) {
                for (const auto* candidate : importedFunctions) {
                    if (candidate == resolved ||
                        candidate->backendArity != resolved->backendArity ||
                        candidate->backendModule == resolved->backendModule) {
                        continue;
                    }
                    resolved = nullptr;
                    ambiguousReceiver = true;
                    break;
                }
            }
            // Same for a LOCAL make-block method: a module flattened into this
            // unit owns the name for its own receiver type, and an unknown
            // receiver is no evidence that this call means that type. Pinning
            // it sent every `get` in a program that says `using Regex` to
            // Match's accessor.
            if (!resolved && winnerIsMakeMethod && receiverUnknown) {
                for (const auto* candidate : importedFunctions) {
                    if (candidate->backendArity == static_cast<int>(matched.params.size())) {
                        winnerIsMakeMethod = false;
                        ambiguousReceiver = true;
                        break;
                    }
                }
            }
            // Overloads that DISAGREE about what the receiver is are no
            // evidence about it either, even when they share a backend module
            // — the checks above only notice ambiguity across modules. Without
            // this, `x.to(String)` on an unconstrained `x` bound it to Measure
            // because units.kex's `to` happened to sort first, and
            // `let show(x) = x.to(String)` inferred as `Measure -> String`,
            // rejecting `show(42)`.
            // `x.route` on an unconstrained `x` is as much a read of some
            // record's `route` FIELD as a call of this function, when a record
            // in view declares one. Taking the function typed the receiver as
            // the function's owner: a closure's `context.route` became a call
            // on an unrelated record the moment that record gained a `route`
            // function (kexhq/kex#476). Neither is evidence; leave both open.
            if (receiverUnknown && isReceiver && argTypes.size() == 1 && !matched.params.empty()) {
                const auto owner = typeToString(resolve(matched.params.front()));
                const auto ownerBase = owner.substr(0, owner.find('<'));
                auto inView = [&](const std::string& record) {
                    if (m_programRecords.count(record)) {
                        return true;
                    }
                    auto imports = [&](const std::vector<ImportSelection>& list) {
                        return std::any_of(
                            list.begin(), list.end(), [&](const ImportSelection& import) {
                                return record.rfind(import.module + ".", 0) == 0;
                            });
                    };
                    if (imports(m_declarationImports)) {
                        return true;
                    }
                    return std::any_of(
                        m_importScopeStack.begin(), m_importScopeStack.end(), imports);
                };
                for (const auto& [record, fields] : m_recordFields) {
                    if (record != ownerBase && fields.count(name) && inView(record)) {
                        return Type::unknown();
                    }
                }
            }
            if (!ambiguousReceiver && receiverUnknown && fullMatches.size() > 1) {
                std::string receiverType;
                for (const auto* candidate : fullMatches) {
                    if (candidate->params.empty()) {
                        continue;
                    }
                    auto current = typeToString(resolve(candidate->params[0]));
                    if (receiverType.empty()) {
                        receiverType = std::move(current);
                    } else if (receiverType != current) {
                        ambiguousReceiver = true;
                        break;
                    }
                }
            }
            if (resolved) {
                // The export's own flag is the whole answer: the module that
                // owns it carries no effect of its own (kexhq/kex#130).
                bool resolvedFoul = resolved->signature.isFoul;
                ResolvedCallTarget target{
                    resolved->sourceModule,
                    resolved->backendModule,
                    resolved->backendFunction,
                    resolved->backendArity,
                    isReceiver,
                    resolvedFoul,
                    false,
                    resolved->paramNames,
                };
                target.defaultSources = resolved->defaultSources;
                // A slot of another unit's serving type, called on its
                // running server: lowered as a slot of this unit's is.
                if (resolved->remoteSlot) {
                    for (const auto& param : matched.params) {
                        target.localDispatchTypes.push_back(typeToString(resolve(param)));
                    }
                    target.isServingCast = typesEqual(resolve(matched.result), Type::voidType());
                    // As for a slot of this unit's: a cast has no reply to
                    // wait for, and lowering would drop the timeout.
                    if (target.isServingCast) {
                        for (const auto& [label, value] : methodCall->namedArgs) {
                            if (label == "within") {
                                error(
                                    diag::Code::WithinOnCastSlot,
                                    value ? value->location : methodCall->receiver->location);
                            }
                        }
                    }
                }
                for (std::size_t i = 0; i < matched.params.size(); ++i) {
                    const auto resolvedParam = resolve(matched.params[i]);
                    const auto addRequiredDictionary = [&](const std::string& traitName) {
                        const bool backendExpectsDictionary =
                            resolved->backendArity >
                            static_cast<int>(
                                matched.params.size() + target.traitDictionaries.size());
                        if (!backendExpectsDictionary) {
                            return;
                        }
                        const auto* trait = m_traits.get(traitName);
                        const bool methodIsRequired =
                            trait &&
                            std::any_of(
                                trait->requiredMethods.begin(), trait->requiredMethods.end(),
                                [&](const auto& required) { return required.name == name; });
                        const bool callsTraitImplementationDirectly =
                            i == 0 && isReceiver && methodIsRequired &&
                            resolved->backendFunction == name + "/" + traitName;
                        if (trait && !trait->requiredMethods.empty() &&
                            !callsTraitImplementationDirectly) {
                            target.traitDictionaries.push_back({i, traitName});
                        }
                    };
                    if (const auto* constrained =
                            std::get_if<ConstrainedType>(&resolvedParam->kind)) {
                        addRequiredDictionary(constrained->traitName);
                    } else if (const auto* named = std::get_if<NamedType>(&resolvedParam->kind)) {
                        addRequiredDictionary(named->name);
                    }
                }
                if (isReceiver && !matched.params.empty() && !argTypes.empty() &&
                    m_importedInterfaces) {
                    // The CALL-SITE receiver (`argTypes[0]`), not the
                    // winner's: matching the winner against itself
                    // covers tautologically, which miscompiled
                    // `None.blank?` to `blank?/Bool` and `None.to`
                    // to `to/Atom`.
                    auto head = resolve_core::staticReceiverHead(
                        *resolved, resolve(argTypes[0]), *m_importedInterfaces,
                        [this](const TypePtr& type) { return expandTypeAliases(type); });
                    if (head) {
                        target.staticReceiverHead = std::move(*head);
                    }
                }
                // Who owns the call (ownerOfCall): lowering used to decide
                // this again by reading the receiver's type back.
                const bool competing = std::any_of(
                    importedFunctions.begin(), importedFunctions.end(),
                    [&](const ImportedFunction* candidate) {
                        return candidate->backendModule != target.backendModule &&
                               candidate->backendArity == target.backendArity;
                    });
                const auto owner = ownerOfCall(argTypes, name, target.backendModule, competing);
                switch (owner.owner) {
                    case CallOwner::Winner: {
                        // A receiver whose type is erased (an `Any`) is no type to pick
                        // a per-type implementation for (`showValue/Showable`, the
                        // trait default): its module's dispatcher decides by the
                        // value, as the walker does.
                        const auto erased =
                            !argTypes.empty() && isUndetermined(resolve(argTypes[0]));
                        if (erased && target.backendFunction.starts_with(name + "/")) {
                            const auto dispatcher = std::find_if(
                                importedFunctions.begin(), importedFunctions.end(),
                                [&](const ImportedFunction* candidate) {
                                    return candidate->backendModule == target.backendModule &&
                                           candidate->backendFunction == name;
                                });
                            if (dispatcher != importedFunctions.end()) {
                                m_resolvedCalls[methodCall] = dispatcherTarget(**dispatcher);
                                break;
                            }
                        }
                        m_resolvedCalls[methodCall] = std::move(target);
                        break;
                    }
                    case CallOwner::ReceiverType:
                        // The receiver's own type answers; lowering reaches it.
                        if (callExpr) {
                            m_decidedCalls[callExpr] = owner.own.decision();
                        }
                        // A type of a unit compiled apart: the dispatcher of the
                        // one module declaring the method for it
                        // (`connection.close`, Net.Socket's).
                        if (!owner.own.home.empty() && owner.own.moduleFunction.empty()) {
                            std::set<std::string> modules;
                            const ImportedFunction* dispatcher = nullptr;
                            for (const auto* candidate : importedFunctions) {
                                if (candidate->backendArity != target.backendArity ||
                                    candidate->signature.params.empty()) {
                                    continue;
                                }
                                const auto* receiver = std::get_if<NamedType>(
                                    &resolve(candidate->signature.params.front())->kind);
                                if (!receiver ||
                                    ownTypeIdentity(receiver->name) != owner.own.type) {
                                    continue;
                                }
                                modules.insert(candidate->backendModule);
                                if (candidate->backendFunction == name) {
                                    dispatcher = candidate;
                                }
                            }
                            if (modules.size() == 1 && dispatcher) {
                                m_resolvedCalls[methodCall] = dispatcherTarget(*dispatcher);
                            } else if (modules.empty()) {
                                // A trait default it inherits (`set.any?`, Foldable's):
                                // its home module forwards that to where its unit
                                // emitted it, under the method's own name.
                                auto inherited = target;
                                inherited.backendModule = owner.own.home;
                                inherited.backendFunction = name;
                                inherited.staticReceiverHead.clear();
                                inherited.localDispatchTypes.clear();
                                m_resolvedCalls[methodCall] = std::move(inherited);
                            }
                        }
                        break;
                    case CallOwner::Value:
                        if (callExpr) {
                            decideDispatch(callExpr, name);
                        }
                        break;
                }
            } else if (winnerIsMakeMethod) {
                // `private do ... end` inside a `make` block means "only
                // callable within that make" (docs/modules.md). Enforced here
                // because visibility is erased by both backends: a private
                // method is an ordinary function to them, so nothing stopped
                // another module from calling one
                // (rodolfo docs/kex-issues.md #13, kexhq/kex#281).
                if (!matched.params.empty()) {
                    if (auto* owner =
                            std::get_if<NamedType>(&resolve(matched.params.front())->kind)) {
                        auto hidden = m_privateMakeMethods.find(owner->name + "#" + name);
                        // A top-level `make` belongs to no module, and every caller
                        // is "within" the empty one: `User { ... }.secret` from
                        // `main` passed (kexhq/kex#780). There, only a `make`
                        // block of the same type may call it.
                        const auto* inMake =
                            m_inMakeBlock && m_currentMakeType
                                ? std::get_if<NamedType>(&resolve(m_currentMakeType)->kind)
                                : nullptr;
                        const bool permitted =
                            hidden != m_privateMakeMethods.end() &&
                            (hidden->second.empty() ? inMake && inMake->name == owner->name
                                                    : withinModule(hidden->second));
                        if (hidden != m_privateMakeMethods.end() && !permitted) {
                            error(
                                hidden->second.empty() ? diag::Code::PrivateMakeMethod
                                                       : diag::Code::PrivateMakeMethodInModule,
                                methodCall->receiver->location,
                                {{"name", name},
                                 {"type", owner->name},
                                 {"module", hidden->second}});
                        }
                    }
                }
                ResolvedCallTarget target;
                target.backendFunction = name;
                target.backendArity = static_cast<int>(matched.params.size());
                target.passesReceiver = true;
                target.isFoul = matched.isFoul;
                // Lowering places named arguments by these. A make-method
                // reached from another compilation unit used to arrive with
                // none, and `router.start(port: 4000)` failed to lower.
                target.paramNames = matched.paramNames;
                if (!matched.params.empty()) {
                    auto receiver = resolve(matched.params.front());
                    auto* named = std::get_if<NamedType>(&receiver->kind);
                    target.isServingCast = named && named->name == kex::names::kServerType &&
                                           m_slotMethodNames.count(name) &&
                                           typesEqual(resolve(matched.result), Type::voidType());
                }
                if (target.isServingCast) {
                    for (const auto& [label, value] : methodCall->namedArgs) {
                        if (label == "within") {
                            error(
                                diag::Code::WithinOnCastSlot,
                                value ? value->location : methodCall->receiver->location);
                        }
                    }
                }
                for (const auto& param : matched.params) {
                    target.localDispatchTypes.push_back(typeToString(resolve(param)));
                }
                m_resolvedCalls[methodCall] = std::move(target);
            }
        }
        // Propagate the param types back to any TypeVar arguments so that
        // unannotated params are constrained by the functions they're passed
        // into (e.g. `let f(s) = s.split(",")` constrains `s` to String).
        // Only apply when the param is concrete — skip generic params that
        // themselves contain TypeVars (e.g. `first : [A] -> A?`).
        const auto typeContainsVar = [](const TypePtr& t) {
            return anyType(
                t, [](const TypePtr& node) { return std::holds_alternative<TypeVar>(node->kind); });
        };
        for (size_t i = 0; i < argTypes.size() && i < matched.params.size(); i++) {
            // The receiver of an ambiguous call is exactly the argument this
            // match says nothing about: binding it to the winner's param type
            // would then TYPE it as that overload's receiver (a Map inferred
            // as Regex's `Match`), which is worse than leaving it unknown.
            if (i == 0 && ambiguousReceiver) {
                continue;
            }
            auto resolved = resolve(argTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                const auto& param = matched.params[i];
                // Negative ids are per-signature generic placeholders, not
                // inference variables, and every signature numbers its own
                // from -1. Binding one in the global substitution made an
                // unrelated signature's `A` mean whatever the last such call
                // passed: after `args.at(i).or("")` (String), `(0..3).filter`
                // returned [String] and its block param typed as String.
                if (deferIfPending(resolved, param, loc)) {
                    continue;
                }
                if (isInferenceVariable(*tv) && !typeContainsVar(param)) {
                    // A receiver typed by nothing but the method's name is
                    // a guess (`item.name` makes `item` a Weekday).
                    if (i == 0) {
                        m_guessedReceiverTypes.insert(typeToString(param));
                    }
                    unifyVar(tv->id, param);
                }
            } else if (!unify(resolved, matched.params[i], /*bindRight=*/false)) {
                // Accepted by a §4.5 rule rather than as the same type (a
                // promotion, a union member): bind what still lines up.
                fixNestedVariables(resolved, matched.params[i]);
            }
        }
        // Instantiate interface-level generic placeholders in the result from
        // the actual arguments. For example,
        // `Result<X, E>.or : X -> X` called on `Result<Regex, RegexError>`
        // must return Regex; leaking the table placeholder made a following
        // overloaded call silently select a same-arity target for any type.
        std::unordered_map<int, TypePtr> genericSubst;
        std::function<void(const TypePtr&, const TypePtr&)> bindGenerics =
            [&](const TypePtr& pattern, const TypePtr& actual) {
                if (!pattern || !actual) {
                    return;
                }
                if (auto* tv = std::get_if<TypeVar>(&pattern->kind); tv && isGenericLetter(*tv)) {
                    genericSubst.try_emplace(tv->id, resolve(actual));
                    return;
                }
                if (auto* constrained = std::get_if<ConstrainedType>(&pattern->kind);
                    constrained && constrained->genericId < 0) {
                    genericSubst.try_emplace(constrained->genericId, resolve(actual));
                    return;
                }
                // `F<A>` against `Box<Integer>` binds F to the constructor `Box`
                // and A to Integer (§8.2), whatever form the actual takes.
                if (auto* pa = std::get_if<TypeApp>(&pattern->kind)) {
                    auto applied = applicationOf(resolve(actual));
                    if (applied) {
                        applied = reshapedApplication(std::move(*applied), pa->args.size());
                    }
                    if (!applied || applied->args.size() != pa->args.size()) {
                        return;
                    }
                    bindGenerics(pa->head, applied->head);
                    for (size_t i = 0; i < pa->args.size(); i++) {
                        bindGenerics(pa->args[i], applied->args[i]);
                    }
                    return;
                }
                if (auto* pn = std::get_if<NamedType>(&pattern->kind)) {
                    auto* an = std::get_if<NamedType>(&actual->kind);
                    if (!an || pn->name != an->name || pn->typeArgs.size() != an->typeArgs.size()) {
                        return;
                    }
                    for (size_t i = 0; i < pn->typeArgs.size(); i++) {
                        bindGenerics(pn->typeArgs[i], an->typeArgs[i]);
                    }
                } else if (auto* pl = std::get_if<ListType>(&pattern->kind)) {
                    if (auto* al = std::get_if<ListType>(&actual->kind)) {
                        bindGenerics(pl->element, al->element);
                    }
                } else if (auto* pm = std::get_if<MapType>(&pattern->kind)) {
                    if (auto* am = std::get_if<MapType>(&actual->kind)) {
                        bindGenerics(pm->key, am->key);
                        bindGenerics(pm->value, am->value);
                    }
                } else if (auto* po = std::get_if<OptionalType>(&pattern->kind)) {
                    if (auto* ao = std::get_if<OptionalType>(&actual->kind)) {
                        bindGenerics(po->inner, ao->inner);
                    }
                } else if (auto* pt = std::get_if<TupleType>(&pattern->kind)) {
                    auto* at = std::get_if<TupleType>(&actual->kind);
                    if (!at || pt->elements.size() != at->elements.size()) {
                        return;
                    }
                    for (size_t i = 0; i < pt->elements.size(); i++) {
                        bindGenerics(pt->elements[i], at->elements[i]);
                    }
                } else if (auto* pf = std::get_if<FuncType>(&pattern->kind)) {
                    auto* af = std::get_if<FuncType>(&actual->kind);
                    if (!af || pf->params.size() != af->params.size()) {
                        return;
                    }
                    for (size_t i = 0; i < pf->params.size(); i++) {
                        bindGenerics(pf->params[i], af->params[i]);
                    }
                    bindGenerics(pf->result, af->result);
                } else if (auto* pi = std::get_if<IntersectionType>(&pattern->kind)) {
                    // Each constraint applies to the same actual value. This
                    // is what binds R in `R & { name: String }` to the full
                    // nominal argument rather than to its visible row alone.
                    for (const auto& member : pi->members) {
                        bindGenerics(member, actual);
                    }
                } else if (auto* pr = std::get_if<RecordType>(&pattern->kind)) {
                    if (auto* ar = std::get_if<RecordType>(&actual->kind)) {
                        for (const auto& [name, fieldPattern] : pr->fields) {
                            auto found = std::find_if(
                                ar->fields.begin(), ar->fields.end(),
                                [&](const auto& field) { return field.first == name; });
                            if (found != ar->fields.end()) {
                                bindGenerics(fieldPattern, found->second);
                            }
                        }
                    } else if (auto* named = std::get_if<NamedType>(&actual->kind)) {
                        auto record = findRecordFields(*named);
                        if (record != m_recordFields.end()) {
                            for (const auto& [name, fieldPattern] : pr->fields) {
                                auto found = record->second.find(name);
                                if (found != record->second.end()) {
                                    bindGenerics(fieldPattern, found->second);
                                }
                            }
                        }
                    }
                }
            };
        for (size_t i = 0; i < argTypes.size() && i < matched.params.size(); i++) {
            const auto selected = selectedType(i, matched.params[i]);
            bindGenerics(matched.params[i], selected ? selected : resolve(argTypes[i]));
        }

        // Substitute INSIDE the result too, not only when the whole result is
        // a placeholder: `[X].dropWhile : ((X) -> Bool) -> [X]` on a [String]
        // receiver returns [String]. Leaking `[X]` instead was not merely
        // vague — String IS [Char] here, so the leaked `[A]` went on to match
        // String's `at : String -> Integer -> Char?` and a list of strings
        // indexed as a list of characters. Placeholders the arguments do not
        // bind become fresh variables of this call (below).
        std::function<TypePtr(const TypePtr&)> applyGenerics = [&](const TypePtr& type) -> TypePtr {
            if (!type) {
                return type;
            }
            if (auto* var = std::get_if<TypeVar>(&type->kind)) {
                if (isInferenceVariable(*var)) {
                    return type;
                }
                // One this call's arguments do not bind is this call's
                // own unknown, fixed by how the result is used: left the
                // signature's placeholder, it matched anything wherever
                // it went (`let one = Process.self` used as two message
                // types, `none() -> [A]` passed as [Integer] and [String]).
                auto [found, fresh] = genericSubst.try_emplace(var->id, nullptr);
                if (fresh) {
                    found->second = freshTypeVar();
                }
                return found->second;
            }
            // A bound generic (`N: Number`) is bound by its id as well.
            if (auto* constrained = std::get_if<ConstrainedType>(&type->kind);
                constrained && constrained->genericId < 0) {
                auto found = genericSubst.find(constrained->genericId);
                return found == genericSubst.end() ? type : found->second;
            }
            // A trait function's `This` is its receiver's type:
            // `"x".repeat(3)` (Monoid's default) is a String, and so is
            // `repeat("x", 3)`.
            if (isThisPlaceholder(type) && !kex::mangle::hasScope(name) && !argTypes.empty()) {
                auto receiver = resolve(argTypes.front());
                if (!std::holds_alternative<UnknownType>(receiver->kind) &&
                    !std::holds_alternative<TypeVar>(receiver->kind)) {
                    // A number's `This` joins every number in a `This` slot.
                    if (isNumericType(receiver)) {
                        for (std::size_t i = 1; i < argTypes.size() && i < matched.params.size();
                             ++i) {
                            const auto arg = resolve(argTypes[i]);
                            if (isThisPlaceholder(resolve(matched.params[i])) &&
                                isNumericType(arg) && !typesEqual(arg, receiver)) {
                                receiver = unionOf(receiver, arg);
                            }
                        }
                    }
                    return receiver;
                }
            }
            // Every nested type, function types included: a function-typed
            // parameter is where a BLOCK is passed, and skipping it left
            // its variables the only ones standing — `nums.map { |x| x * 2 }`
            // reported the selected overload as `[Integer] -> (A -> B) ->
            // [Integer]`, substituting the receiver and result but not the
            // block it was actually given.
            return mapTypeChildren(type, applyGenerics);
        };
        auto selectedResult = normalizeIntersection(applyGenerics(matched.result));
        // What a function known by its interface was passed for each letter it
        // needs (§12 step 7): its bound placeholder, as this call bound it.
        if (callExpr && !matched.clause && !matched.typeParameters.empty()) {
            std::vector<std::pair<std::string, TypePtr>> passed;
            for (const auto& letter : matched.typeParameters) {
                TypePtr bound;
                std::function<void(const TypePtr&)> find = [&](const TypePtr& type) {
                    if (bound || !type) {
                        return;
                    }
                    if (const auto* placeholder = std::get_if<ConstrainedType>(&type->kind);
                        placeholder && placeholder->varName == letter) {
                        bound = applyGenerics(type);
                        return;
                    }
                    forEachTypeChild(type, find);
                };
                for (const auto& param : matched.params) {
                    find(param);
                }
                passed.emplace_back(letter, bound);
            }
            m_importedTypeBindings[callExpr] = std::move(passed);
        }
        // A function value passed for a generic function parameter must take
        // what this call passes it: `["a"].map(~inc)` was caught, but a value
        // (`let f = ~inc; ["a"].map(f)`, or `~add(1)`) was matched against
        // `A -> B` while `A` was still open, and the call ran `inc("a")` (the
        // plan's §11, item 3). Against the parameter as this call made it
        // (`String -> B`), each of the value's parameters must accept what is
        // passed; a result still open says nothing yet.
        for (std::size_t i = 0; i < argTypes.size() && i < matched.params.size(); ++i) {
            const auto param = resolve(matched.params[i]);
            const auto actual = resolvedThroughout(argTypes[i]);
            if (!std::holds_alternative<FuncType>(param->kind) || !containsTypeVariable(param) ||
                !std::holds_alternative<FuncType>(actual->kind)) {
                continue;
            }
            const auto instantiated = resolvedThroughout(applyGenerics(param));
            // What it returns, too, once the other arguments fixed it: the
            // block of `xs.reduce(0) { |acc, x| "no" }` must give the Integer
            // its seed made `A`, and was let through as one.
            if (const auto mismatch = blockResultMismatch(instantiated, actual)) {
                const ast::Expr* argument = i < argExprs.size() ? argExprs[i] : nullptr;
                error(
                    diag::Code::BlockResultMismatch, argument ? argument->location : loc,
                    {{"name", sourceName(name)},
                     {"expected", typeToString(withWrittenLetters(mismatch->first))},
                     {"actual", typeToString(withWrittenLetters(mismatch->second))}});
                continue;
            }
            const auto* want = std::get_if<FuncType>(&instantiated->kind);
            const auto& got = std::get<FuncType>(actual->kind);
            if (!want || want->params.size() != got.params.size()) {
                continue;
            }
            for (std::size_t k = 0; k < want->params.size(); ++k) {
                const auto passed = resolvedThroughout(want->params[k]);
                const auto taken = resolvedThroughout(got.params[k]);
                if (containsUnsolved(passed) || containsUnsolved(taken) ||
                    argMatchesParam(passed, taken)) {
                    continue;
                }
                const ast::Expr* argument = i < argExprs.size() ? argExprs[i] : nullptr;
                error(
                    diag::Code::NoOverloadArgumentMismatch, argument ? argument->location : loc,
                    {{"name", name},
                     {"index", std::to_string(i + 1)},
                     {"expected", typeToString(instantiated)},
                     {"actual", typeToString(actual)},
                     {"signatures", ""}});
                break;
            }
        }
        // A function of a bounded `make` applies where its bounds hold (#651):
        // `(0.0..1.0).sum` is not, since a Float is not Discrete.
        if (!argTypes.empty() && !matched.params.empty()) {
            if (const auto unmet = unmetReceiverBound(matched, argTypes.front())) {
                error(
                    diag::Code::ReceiverBoundUnmet, loc,
                    {{"name", name},
                     {"receiver", typeToString(resolve(argTypes.front()))},
                     {"argument", typeToString(unmet->first)},
                     {"trait", unmet->second}});
            }
        }
        for (std::size_t i = 0; i < argExprs.size() && i < matched.params.size(); ++i) {
            if (argExprs[i]) {
                checkByteLiterals(*argExprs[i], applyGenerics(resolve(matched.params[i])));
                // The selected overload's parameter is final: a Float one takes
                // an integer literal as a float.
                noteFloatLiteral(*argExprs[i], applyGenerics(resolve(matched.params[i])));
            }
        }
        // A capture passed where a foul function is expected (a route's
        // `Handler`) may name a foul one, even from pure code (#640).
        // Positional arguments line up with the parameters; a named one goes
        // to the parameter of its name.
        {
            const std::vector<std::pair<std::string, ast::ExprPtr>>* namedArgs = nullptr;
            if (methodCall) {
                namedArgs = &methodCall->namedArgs;
            } else if (callExpr) {
                if (const auto* function = std::get_if<ast::FunctionCall>(&callExpr->kind)) {
                    namedArgs = &function->namedArgs;
                }
            }
            const auto isNamed = [&](const ast::Expr* expression) {
                return namedArgs &&
                       std::any_of(namedArgs->begin(), namedArgs->end(), [&](const auto& entry) {
                           return entry.second.get() == expression;
                       });
            };
            for (std::size_t i = 0; i < argExprs.size() && i < matched.params.size(); ++i) {
                if (!isNamed(argExprs[i])) {
                    // Instantiated: a generic parameter (`A -> Bool`) is what
                    // this call made it (`Integer -> Bool`).
                    noteCaptureFlow(argExprs[i], applyGenerics(resolve(matched.params[i])));
                }
            }
            if (namedArgs) {
                for (const auto& [label, arg] : *namedArgs) {
                    if (const auto at =
                            std::find(matched.paramNames.begin(), matched.paramNames.end(), label);
                        at != matched.paramNames.end()) {
                        // A receiver prepended to `params` has no name (Signature).
                        const auto index =
                            static_cast<std::size_t>(at - matched.paramNames.begin()) +
                            (matched.params.size() - matched.paramNames.size());
                        if (index < matched.params.size()) {
                            noteCaptureFlow(
                                arg.get(), applyGenerics(resolve(matched.params[index])));
                        }
                    }
                }
            }
        }
        if (callExpr) {
            Signature selected = matched;
            selected.name = name;
            if (methodCall) {
                if (auto target = m_resolvedCalls.find(methodCall);
                    target != m_resolvedCalls.end()) {
                    selected.isFoul = selected.isFoul || target->second.isFoul;
                }
            }
            for (auto& param : selected.params) {
                param = applyGenerics(resolve(param));
            }
            selected.result = selectedResult;
            // A distinct type's value runs as its backing value, so where a
            // `Showable` parameter takes one, its own `showValue` shows it.
            for (std::size_t i = 0; i < argTypes.size() && i < matched.params.size(); ++i) {
                const auto param = resolve(matched.params[i]);
                const auto* paramName = std::get_if<NamedType>(&param->kind);
                const auto* paramBound = std::get_if<ConstrainedType>(&param->kind);
                if (!(paramName && paramName->name == names::kShowableTrait) &&
                    !(paramBound && paramBound->traitName == names::kShowableTrait)) {
                    continue;
                }
                const auto given = resolve(argTypes[i]);
                const auto* argument = std::get_if<NamedType>(&given->kind);
                if (argument && distinctBacking(given) != given) {
                    m_shownThroughDistinct[callExpr].emplace_back(i, argument->name);
                }
            }
            // Any other trait a distinct type implements itself has no such route:
            // the walker dispatches on the backing value (§14 item 4).
            for (std::size_t i = 0; i < argTypes.size() && i < matched.params.size(); ++i) {
                const auto shown = m_shownThroughDistinct.find(callExpr);
                if (shown != m_shownThroughDistinct.end() &&
                    std::any_of(shown->second.begin(), shown->second.end(), [&](const auto& entry) {
                        return entry.first == i;
                    })) {
                    continue;
                }
                if (const auto trait = distinctTraitClash(argTypes[i], matched.params[i]);
                    !trait.empty()) {
                    const auto value = resolve(argTypes[i]);
                    error(
                        diag::Code::DistinctAsTraitValue,
                        i < argExprs.size() && argExprs[i] ? argExprs[i]->location : loc,
                        {{"type", typeToString(value)},
                         {"trait", trait},
                         {"backing", typeToString(distinctBacking(value))}});
                }
            }
            // A bare call the prelude's testing DSL answers (its declaration,
            // not one of the program's own): a runtime builtin.
            if (!methodCall && !matched.clause && matched.declaringModule.empty() &&
                matched.makeModule.empty()) {
                if (const auto builtin = names::testBuiltinNamed(name)) {
                    m_decidedCalls[callExpr] = {
                        CallKind::Builtin, names::testIntrinsicPath(*builtin)};
                }
            }
            m_selectedCallSignatures[callExpr] = std::move(selected);
            // A higher-kinded trait's requirement (`map` on an `F<Integer>`,
            // §8.2) names no function: the value it is called on picks the
            // implementation when the call runs, as a trait-typed receiver's does.
            if (!matched.clause && !matched.params.empty() && matched.params[0] &&
                std::holds_alternative<TypeApp>(matched.params[0]->kind)) {
                decideDispatch(callExpr, name);
                // The stdlib's implementations (List, Optional, Result) answer
                // through the prelude's dispatcher for the name, which picks one
                // by the value; a program defining its own keeps its dispatcher,
                // which falls back to the prelude's.
                if (methodCall && m_importedInterfaces) {
                    const auto defined = m_methodSignatures.find(name);
                    const bool definedHere =
                        defined != m_methodSignatures.end() &&
                        std::any_of(
                            defined->second.begin(), defined->second.end(), [](const Signature& s) {
                                return s.params.empty() || !s.params[0] ||
                                       !std::holds_alternative<TypeApp>(s.params[0]->kind);
                            });
                    const auto providers = m_importedInterfaces->receiverFunctions.find(name);
                    if (!definedHere &&
                        providers != m_importedInterfaces->receiverFunctions.end()) {
                        const int arity = static_cast<int>(argTypes.size());
                        for (const auto& provider : providers->second) {
                            if (provider.backendModule == names::kPreludeModule &&
                                provider.backendFunction == name &&
                                provider.backendArity == arity) {
                                m_resolvedCalls[methodCall] = dispatcherTarget(provider);
                                break;
                            }
                        }
                    }
                }
            }
            // A call no imported target took (none, or a `make` method of
            // this program): the receiver's own type may answer it
            // (ownTypeAnswers). ownerOfCall decided the imported ones.
            const auto resolved =
                methodCall ? m_resolvedCalls.find(methodCall) : m_resolvedCalls.end();
            if (methodCall && !argTypes.empty() &&
                (resolved == m_resolvedCalls.end() || resolved->second.backendModule.empty()) &&
                !m_decidedCalls.contains(callExpr)) {
                if (const auto own = ownTypeAnswers(argTypes, name)) {
                    m_decidedCalls[callExpr] = own->decision();
                }
            }
        }
        return selectedResult;
    }

    if (arityMatches.empty()) {
        // A receiver whose type is not known proves nothing about arity. The
        // NamedType permissive guard above never sees these, so without this
        // an unrelated same-named method anywhere in the prelude turns every
        // gradual field read into an error: `e.value` on the `Error(e)` of
        // `Integer.parse` (a ParseError field) was reported as a bad call to
        // OptionParser's `value/2` the moment OptionParser joined the prelude.
        // Only a receiver: in a module's function called by its qualified
        // name (`Type.fieldValues(value, 0)`), the first argument is an
        // argument like the rest, and an extra one is an error whatever its
        // type. An `Any` let the call through to fail at run time.
        if (isMethodCall && !argTypes.empty() && !kex::mangle::hasScope(name)) {
            auto receiver = resolve(argTypes.front());
            if (isUndetermined(receiver)) {
                return Type::unknown();
            }
            // No candidate takes this receiver at any arity: the method is
            // undefined for it, as the check of the whole program says. Its
            // argument count was blamed instead, against an unrelated
            // same-named function (`running.localAddress` with Net.HTTP's
            // out of scope: a mock's two-argument one).
            if (std::none_of(sigs->begin(), sigs->end(), [&](const Signature& signature) {
                    return !signature.params.empty() &&
                           argMatchesParam(receiver, signature.params.front());
                })) {
                error(
                    diag::Code::UndefinedMethodFor, loc,
                    {{"name", sourceName(name)}, {"receiver", typeToString(receiver)}});
                return Type::unknown();
            }
        }
        // A bare call whose only candidates are other types' functions,
        // offered through UFCS, none taking its first argument as their
        // receiver: no function of that name is in scope. `printLine("a")`
        // was blamed for its argument count against a type's two-parameter
        // `printLine`.
        if (!isMethodCall && !argTypes.empty() && !kex::mangle::hasScope(name) && !hasUser &&
            throughReceiverOnly == sigs->size() &&
            std::none_of(sigs->begin(), sigs->end(), [&](const Signature& signature) {
                return !signature.params.empty() &&
                       argMatchesParam(resolve(argTypes.front()), signature.params.front());
            })) {
            error(diag::Code::UndefinedFunction, loc, {{"name", sourceName(name)}});
            return Type::unknown();
        }
        error(
            diag::Code::WrongArgumentCount, loc,
            {{"name", sourceName(name)},
             {"expected", std::to_string((*sigs)[0].params.size())},
             {"actual", std::to_string(argTypes.size())}});
        return (*sigs)[0].result;
    }

    // A union argument the overloads cover member by member: each member
    // has one that takes it, so the value picks the clause when the call
    // runs. No single overload takes the whole union, which is why nothing
    // matched above (kexhq/kex#647).
    if (const auto covered = unionArgumentCovered(arityMatches, argTypes)) {
        if (callExpr) {
            decideDispatch(callExpr, name);
        }
        return *covered;
    }

    // Zero matches with at least one arity match: find the first
    // mismatching argument against the first arity-matching candidate for
    // the headline message, then list every candidate signature tried,
    // Elm-style. Prefer a concrete sig that agrees on the receiver over a
    // vacuous or foreign-receiver one for error reporting.
    //
    // Before any of that: a make-block method of this name whose receiver
    // accepts the call's first argument answers this call on both backends,
    // even though its signature is only registered once its definition is
    // reached — a sibling defined lower in the block (a `private do` helper
    // is the usual shape) is missing from the candidates above, and the
    // mismatch would be reported against an unrelated import that happens
    // to own the name. The runtime dispatches to the block's own method;
    // the checker must not reject what the backends run (kexhq/kex#292).
    if (!argTypes.empty()) {
        if (auto owners = m_makeMethodReceivers.find(name); owners != m_makeMethodReceivers.end()) {
            auto receiverArg = resolve(argTypes.front());
            // Only when the block's own signature is genuinely missing from
            // `arityMatches` — the registration-order gap this exists for.
            // Once it IS registered, it is already one of the candidates
            // just rejected on the merits, and must report the same
            // argument-type error it always did rather than being waved
            // through by receiver alone (`5.describe(42)` on
            // `make Int do describe :> String -> String ... end`). A
            // generic receiver (`Qty<A>` for `make Qty<D>`) takes the
            // argument without being equal to it: `q2.plus(q3)` with two
            // different `D`s was waved through.
            bool receiverAlreadyCandidate =
                std::any_of(arityMatches.begin(), arityMatches.end(), [&](const Signature* sig) {
                    if (sig->params.empty()) {
                        return false;
                    }
                    const auto candidate = resolve(sig->params.front());
                    if (typesEqual(candidate, receiverArg)) {
                        return true;
                    }
                    const auto* own = std::get_if<NamedType>(&candidate->kind);
                    const auto* given = std::get_if<NamedType>(&receiverArg->kind);
                    return own && given && own->name == given->name &&
                           argMatchesParam(receiverArg, candidate);
                });
            // The block's method must also take this many arguments:
            // `this.sum()` against `let sum(books)` is no call to it, and
            // waving it through let the backends pick different methods.
            bool arityFits = true;
            if (auto arities = m_makeMethodArities.find(name);
                arities != m_makeMethodArities.end() && !arities->second.empty()) {
                arityFits = std::any_of(
                    arities->second.begin(), arities->second.end(), [&](const auto& range) {
                        return argTypes.size() >= range.first && argTypes.size() <= range.second;
                    });
            }
            if (!receiverAlreadyCandidate && arityFits) {
                for (const auto& owner : owners->second) {
                    if (argMatchesParam(receiverArg, resolve(owner))) {
                        return Type::unknown();
                    }
                }
            }
        }
    }
    // A trait default as the trait itself declares it (`Enumerable -> (T ->
    // Bool) -> [T]`, from the prelude's interface or the trait's own check)
    // whose element type is still unbound (`Unknown`) is the template each
    // implementing type gets its own copy of, not a candidate. One whose
    // types are all written (`Showable -> Type<String> -> String?`) is.
    auto isVacuousSig = [&](const Signature* sig) {
        if (!sig->params.empty()) {
            const auto receiver = resolve(sig->params.front());
            if (isThisPlaceholder(receiver)) {
                return true;
            }
            const bool unbound =
                std::any_of(sig->params.begin() + 1, sig->params.end(), mentionsUnknownParam);
            if (const auto* bound = std::get_if<ConstrainedType>(&receiver->kind);
                unbound && bound) {
                if (const auto* trait = m_traits.get(bound->traitName);
                    trait &&
                    std::find(trait->defaultMethods.begin(), trait->defaultMethods.end(), name) !=
                        trait->defaultMethods.end()) {
                    return true;
                }
            }
        }
        for (const auto& p : sig->params) {
            if (!std::holds_alternative<TypeVar>(p->kind) &&
                !std::holds_alternative<UnknownType>(p->kind) &&
                !std::holds_alternative<ConstrainedType>(p->kind)) {
                return false;
            }
        }
        return true;
    };
    // When NO candidate agrees on the receiver — `Result<Date, _>.iso`, where
    // every `iso` belongs to some other type — the loop below never fires and
    // the headline names whatever was registered first. That is registration
    // order, so adding an unrelated overload anywhere in the prelude silently
    // rewrote the message. Fall back to the same ordering the listing uses.
    auto signatureOrder = [](const Signature* a, const Signature* b) {
        if (a->params.size() != b->params.size()) {
            return a->params.size() < b->params.size();
        }
        for (size_t i = 0; i < a->params.size(); ++i) {
            auto left = typeToString(a->params[i]);
            auto right = typeToString(b->params[i]);
            if (left != right) {
                return left < right;
            }
        }
        return typeToString(a->result) < typeToString(b->result);
    };
    // Among the candidates that could plausibly have been meant, headline the
    // one the call comes CLOSEST to: fewest arguments that do not fit, ties
    // broken by the listing's own order. Taking the first acceptable candidate
    // in registration order — which this used to do — put the message back at
    // the mercy of what the prelude happens to declare: adding `Set.add` made
    // `add("oops", 1)` on a user's own `add : Integer -> Integer -> Integer`
    // report "expects argument 1 to be Set<A>".
    auto mismatchCount = [&](const Signature* sig) {
        size_t count = 0;
        for (size_t i = 0; i < sig->params.size() && i < argTypes.size(); ++i) {
            if (!fits(i, sig->params[i])) {
                ++count;
            }
        }
        return count;
    };
    const Signature* firstPtr = nullptr;
    size_t fewestMismatches = 0;
    for (const auto* am : arityMatches) {
        if (isVacuousSig(am)) {
            continue;
        }
        if (isMethodCall && !am->params.empty() && !fits(0, am->params[0])) {
            continue;
        }
        const auto mismatches = mismatchCount(am);
        if (!firstPtr || mismatches < fewestMismatches ||
            (mismatches == fewestMismatches && signatureOrder(am, firstPtr))) {
            firstPtr = am;
            fewestMismatches = mismatches;
        }
    }
    // When NO candidate qualifies, the listing's order is the only principled
    // answer left — see the comment above `signatureOrder`.
    if (!firstPtr) {
        firstPtr = *std::min_element(arityMatches.begin(), arityMatches.end(), signatureOrder);
    }
    const Signature& first = *firstPtr;
    // Which argument is off, when one can be named, and its position.
    std::optional<diag::Args> argumentOff;
    std::size_t offIndex = 0;
    // Only the arguments passed: a defaulted parameter may have none.
    for (size_t i = 0; i < first.params.size() && i < argTypes.size(); i++) {
        if (!fits(i, first.params[i])) {
            auto* constrained = std::get_if<ConstrainedType>(&first.params[i]->kind);
            std::string expected =
                constrained ? constrained->traitName : typeToString(first.params[i]);
            argumentOff = diag::Args{
                {"index", std::to_string(i + 1)},
                {"expected", expected},
                {"actual", typeToString(resolvedThroughout(argTypes[i]))}};
            offIndex = i;
            break;
        }
    }
    if (!argumentOff) {
        if (const auto clash = genericConflictOf(first)) {
            // An argument a dedicated check already rejected (`or` given a
            // block, a deferred reply of the wrong type) is reported once.
            if (m_diagnostics &&
                std::any_of(m_diagnostics->begin(), m_diagnostics->end(), [&](const Diagnostic& d) {
                    return d.level == Diagnostic::Level::Error && d.location.line == loc.line &&
                           d.location.file == loc.file;
                })) {
                return first.result;
            }
            offIndex = clash->first;
            argumentOff = diag::Args{
                {"index", std::to_string(clash->first + 1)},
                {"expected", typeToString(resolve(clash->second))},
                {"actual", typeToString(argTypes[clash->first])}};
        }
    }

    std::string signatures;
    // Vacuous candidates (params all TypeVar/Unknown/trait bounds — e.g. an
    // unannotated trait default method) add noise to the listing when
    // concrete candidates exist; show them only when they are all we have.
    auto sortedSigs = *sigs;
    std::stable_sort(
        sortedSigs.begin(), sortedSigs.end(), [](const Signature& a, const Signature& b) {
            if (a.params.size() != b.params.size()) {
                return a.params.size() < b.params.size();
            }
            for (size_t i = 0; i < a.params.size(); ++i) {
                auto as = typeToString(a.params[i]), bs = typeToString(b.params[i]);
                if (as != bs) {
                    return as < bs;
                }
            }
            return typeToString(a.result) < typeToString(b.result);
        });
    bool anyConcreteSig = false;
    for (const auto& sig : sortedSigs) {
        if (!isVacuousSig(&sig)) {
            anyConcreteSig = true;
            break;
        }
    }
    // One line per DISTINCT signature. The same overload can be registered
    // more than once — from the prelude interface, from the stdlib source, and
    // again from a merged module — and listing `split : String -> [String]`
    // four times says nothing the first line did not.
    std::unordered_set<std::string> shown;
    for (const auto& sig : sortedSigs) {
        if (anyConcreteSig && isVacuousSig(&sig)) {
            continue;
        }
        // An un-annotated signature this call could not reach is noise, not a
        // suggestion. The stdlib ships capability stand-ins, so `write` also
        // names `Mock.Files -> Any -> Any -> Bool`, and printing that under a
        // 2-argument error told the reader to consider a method taking three
        // (kexhq/kex#143). A CONCRETE overload at another arity still earns
        // its line — `split : String -> [String]` is worth seeing even when
        // the call passed two arguments.
        const std::size_t requiredParams = sig.requiredParams.value_or(sig.params.size());
        const bool arityCannotMatch =
            argTypes.size() < requiredParams || argTypes.size() > sig.params.size();
        if (arityCannotMatch &&
            std::any_of(sig.params.begin(), sig.params.end(), mentionsUnknownParam)) {
            continue;
        }
        auto rendered = displaySignature(name, sig);
        if (!shown.insert(rendered).second) {
            continue;
        }
        signatures += "\n\n" + rendered;
    }
    // `person.name` on a record without a `name` field: no function named
    // `name` takes a Person either, so what is wrong is the field. It was
    // reported as `name` expecting a Weekday, the first unrelated function of
    // that name (the plan's §11, item 9).
    if (argumentOff && isMethodCall && offIndex == 0 && argTypes.size() == 1 && !argTypes.empty()) {
        if (const auto* named = std::get_if<NamedType>(&resolve(argTypes[0])->kind)) {
            const auto record = findRecordFields(*named);
            if (record != m_recordFields.end() && !record->second.contains(sourceName(name))) {
                std::vector<std::string> available;
                for (const auto& [field, fieldType] : record->second) {
                    available.push_back(field);
                }
                std::sort(available.begin(), available.end());
                std::string fields;
                for (const auto& field : available) {
                    fields += (fields.empty() ? "" : ", ") + field;
                }
                const ast::Expr* receiverExpr = !argExprs.empty() ? argExprs[0] : nullptr;
                if (fields.empty()) {
                    error(
                        diag::Code::RecordHasNoFieldsAtAll,
                        receiverExpr ? receiverExpr->location : loc,
                        {{"record", named->name}, {"field", sourceName(name)}});
                } else {
                    error(
                        diag::Code::RecordHasNoFieldListed,
                        receiverExpr ? receiverExpr->location : loc,
                        {{"record", named->name}, {"field", sourceName(name)}, {"fields", fields}});
                }
                return Type::unknown();
            }
        }
    }
    // A distinct type given for a trait it implements itself.
    if (argumentOff && offIndex < argTypes.size() && offIndex < firstPtr->params.size() &&
        !std::holds_alternative<NamedType>(resolve(argTypes[offIndex])->kind)) {
        if (const auto trait = distinctTraitClash(argTypes[offIndex], firstPtr->params[offIndex]);
            !trait.empty()) {
            const auto value = resolve(argTypes[offIndex]);
            error(
                diag::Code::DistinctAsTraitValue, loc,
                {{"type", typeToString(value)},
                 {"trait", trait},
                 {"backing", typeToString(distinctBacking(value))}});
            return Type::unknown();
        }
    }
    // The value a dot call goes through is an `Any` the program has not
    // narrowed: that is the mistake, not which overload it missed.
    if (argumentOff && isMethodCall && offIndex == 0 && !argTypes.empty()) {
        if (const auto* top = std::get_if<UnknownType>(&resolve(argTypes[0])->kind);
            top && top->written) {
            error(diag::Code::AnyNotNarrowed, loc, {{"name", sourceName(name)}});
            return Type::unknown();
        }
    }
    if (argumentOff) {
        argumentOff->emplace_back("name", sourceName(name));
        argumentOff->emplace_back("signatures", signatures);
        // The argument itself, underlined, against the parameter of the
        // function it was passed to, where that is declared in this file.
        const ast::Expr* argument =
            offIndex < argExprs.size() && argExprs[offIndex] ? argExprs[offIndex] : nullptr;
        std::string actual;
        for (const auto& [key, value] : *argumentOff) {
            if (key == "actual") {
                actual = value;
            }
        }
        error(
            diag::Code::NoOverloadArgumentMismatch, argument ? argument->location : loc,
            std::move(*argumentOff));
        if (argument) {
            const ast::Param* param = nullptr;
            if (first.clause) {
                // A receiver prepended to the signature has no parameter of
                // its own in the clause.
                const auto shift = first.params.size() - first.clause->params.size();
                if (offIndex >= shift && offIndex - shift < first.clause->params.size()) {
                    param = &first.clause->params[offIndex - shift];
                }
            }
            if (param && param->type) {
                labelLast(
                    "this has type " + actual, (*param->type)->location,
                    "expected because of this parameter");
            } else {
                labelLast("this has type " + actual, {}, "");
            }
        }
    } else {
        error(
            diag::Code::NoOverloadDifferentArguments, loc,
            {{"name", sourceName(name)}, {"signatures", signatures}});
    }
    // Lowered all the same only where the check does not gate the build
    // (`--no-check`), and there the value decides, as it does for a
    // receiver of unknown type: when the receiver is one the method takes
    // and only an argument is off (a truthy block for `filter`). Where every
    // candidate is a record's method and the receiver no such record
    // (`"a b".whiteSpaces`, Parsing's Input's), no value reaches one: the
    // call fails as an undefined method, where it is written.
    const bool receiverTaken =
        std::any_of(arityMatches.begin(), arityMatches.end(), [&](const Signature* candidate) {
            return !candidate->params.empty() && !argTypes.empty() &&
                   argMatchesParam(argTypes.front(), candidate->params.front());
        });
    const bool recordsOnly =
        std::all_of(arityMatches.begin(), arityMatches.end(), [&](const Signature* candidate) {
            if (candidate->params.empty()) {
                return false;
            }
            const auto* owner = std::get_if<NamedType>(&resolve(candidate->params.front())->kind);
            return owner && m_recordFields.contains(owner->name);
        });
    if (isMethodCall && callExpr && !m_decidedCalls.contains(callExpr)) {
        if (receiverTaken) {
            decideDispatch(callExpr, name);
        } else if (recordsOnly && !argTypes.empty() && !isUndetermined(resolve(argTypes.front()))) {
            m_decidedCalls[callExpr] = {CallKind::Hidden, ""};
        }
    }
    // A refused call answers what its candidate declares when that is a type
    // of its own; a letter this call never bound (`F<[C]>` of a `traverse`
    // whose step answered no container) would only be reported again where
    // the result is used.
    // Nor one its candidates disagree on (Char's `upperCase` answers a Char,
    // String's a String): which one it is is not known.
    const auto declared = arityMatches[0]->result;
    const bool agreed =
        std::all_of(arityMatches.begin(), arityMatches.end(), [&](const auto* other) {
            return other->result && declared &&
                   typesEqual(resolve(other->result), resolve(declared));
        });
    return !agreed || (declared && containsOpenType(resolve(declared))) ? Type::unknown()
                                                                        : declared;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::ownerOfCall(
    const std::vector<TypePtr>& argTypes,
    const std::string& method,
    const std::string& winnerModule,
    bool competing) -> OwnerDecision {
    if (argTypes.empty()) {
        return {};
    }
    const auto receiver = resolve(argTypes.front());
    const auto* named = std::get_if<NamedType>(&receiver->kind);
    const bool erased = isUndetermined(receiver) || (named && named->name == kex::names::kAnyType);
    if (erased) {
        return {
            winnerModule != names::kPreludeModule && competing ? CallOwner::Value
                                                               : CallOwner::Winner};
    }
    if (auto own = ownTypeAnswers(argTypes, method); own && own->home != winnerModule) {
        return {CallOwner::ReceiverType, std::move(*own)};
    }
    return {};
}

// The backend module of the receiver's own type when that type answers
// What the receiver's own type answers `method` with, at one signature: its
// `make` block's (declared there, or a trait default it inherits) — not
// overloaded by argument type, which lowering dispatches — or else a function
// of its home module over it. A type of this program, or of a unit compiled
// apart (from its own module, inherited defaults only); nothing for any other.
auto TypeChecker::ownTypeAnswers(const std::vector<TypePtr>& argTypes, const std::string& method)
    -> std::optional<OwnAnswer> {
    const auto* named = std::get_if<NamedType>(&resolve(argTypes.front())->kind);
    if (!named || (!named->shadowsPrelude && m_preludeShadows.contains(named->name))) {
        // The prelude's type under a name the program reuses is not the
        // program's: its imported functions answer for it.
        return std::nullopt;
    }
    const auto owner = ownTypeIdentity(named->name);
    const auto home = typeHomeModule(owner);
    if (!home) {
        return std::nullopt;
    }
    // What its own module emits for it: a trait's required method or a default
    // from the trait's body. A function over a trait it claims
    // (`inspected(value: Inspectable)`) is the trait home's, not the type's.
    const auto inherits =
        m_traits.implementsInHome(m_traits.implementorKey(argTypes.front()), method);
    if (!home->empty()) {
        return inherits ? std::optional<OwnAnswer>({owner, *home, {}}) : std::nullopt;
    }
    // Only a `make` the call's candidates include: in scope here, or of the
    // type's own module. One of a module not imported (`Patcher`'s `make
    // Measure`) is no answer of the type's.
    std::size_t declared = 0;
    if (auto local = m_methodSignatures.find(method); local != m_methodSignatures.end()) {
        for (const auto& signature : local->second) {
            if (signature.params.empty() ||
                !(makeModuleVisible(signature.makeModule) ||
                  signature.makeModule == kex::mangle::scopeOf(owner))) {
                continue;
            }
            const auto* receiver = std::get_if<NamedType>(&resolve(signature.params.front())->kind);
            if (receiver && ownTypeIdentity(receiver->name) == owner &&
                signature.params.size() == argTypes.size()) {
                ++declared;
            }
        }
    }
    if (declared > 1) {
        return std::nullopt; // overloaded by argument: dispatched
    }
    if (declared == 1 || inherits) {
        return OwnAnswer{owner, {}, {}};
    }
    // A function of the type's own module whose receiver is the type:
    // `TCP.close(connection: Conn)` for a `TCP.Conn`.
    const auto dot = owner.rfind('.');
    if (dot == std::string::npos) {
        return std::nullopt;
    }
    const auto functions =
        m_userSignatures.find(kex::mangle::qualify(owner.substr(0, dot), method));
    if (functions == m_userSignatures.end()) {
        return std::nullopt;
    }
    const bool overOwnType = std::any_of(
        functions->second.begin(), functions->second.end(), [&](const Signature& signature) {
            if (signature.params.size() != argTypes.size()) {
                return false;
            }
            const auto* receiver = std::get_if<NamedType>(&resolve(signature.params.front())->kind);
            return receiver && ownTypeIdentity(receiver->name) == owner;
        });
    if (!overOwnType) {
        return std::nullopt;
    }
    return OwnAnswer{owner, {}, owner.substr(0, dot) + "." + method};
}

// One name for a type however it is written: a record's or ADT's identity
// (resolveRecordName), and for a distinct type registered under both its
// spellings (`ThingId` and `ThingIds.ThingId`) the qualified one, when
// exactly one of this program's declarations has it.
auto TypeChecker::ownTypeIdentity(const std::string& name) const -> std::string {
    const auto identity = resolveRecordName(name);
    if (identity != name || identity.find('.') != std::string::npos ||
        m_programRecords.contains(identity)) {
        return identity;
    }
    std::string qualified;
    for (const auto& distinct : m_programDistinctTypes) {
        if (distinct.ends_with("." + name)) {
            if (!qualified.empty()) {
                return name; // two modules: as written
            }
            qualified = distinct;
        }
    }
    return qualified.empty() ? name : qualified;
}

// Where `type` is declared: "" for this program, the backend module of the
// unit compiled apart that declares it (`Bags.Bag` is `Bags`'s), or nothing
// for the prelude's and everything else.
auto TypeChecker::typeHomeModule(const std::string& type) const -> std::optional<std::string> {
    if (m_programRecords.contains(type) || m_programDistinctTypes.contains(type)) {
        return std::string{};
    }
    if (!m_importedInterfaces) {
        return std::nullopt;
    }
    const ImportedModuleInterface* home = nullptr;
    for (const auto& [name, module] : m_importedInterfaces->modules) {
        if (module.compiledApart && type.starts_with(name + ".") &&
            (!home || name.size() > home->sourceModule.size())) {
            home = &module;
        }
    }
    if (!home) {
        return std::nullopt;
    }
    return home->backendModule;
}

} // namespace kex::semantic

namespace kex::semantic {

// A type that claims a trait has what the trait requires, even where its own
// methods are not on record (`Kilo(Watt).factor` on Units.SI's unannotated
// `let factor(@Kilo(unit))`): the requirement types the call, and the value
// decides the implementation at run time, as for a trait-typed receiver.
auto TypeChecker::claimedTraitAnswers(
    const std::vector<TypePtr>& argTypes, const std::string& name, const ast::Expr* callExpr)
    -> TypePtr {
    if (argTypes.empty()) {
        return nullptr;
    }
    const auto [trait, required] = m_traits.requiredMethod(resolve(argTypes.front()), name);
    if (!trait || argTypes.size() != required->params.size() + 1) {
        return nullptr;
    }
    if (callExpr) {
        m_decidedCalls[callExpr] = {CallKind::TraitMethod, trait->name};
    }
    return required->result;
}

// A requirement fixes how many values its call takes, whichever type answers
// it: `w.combine()` for `combine : This -> This` lacks its argument, and was
// waved through to fail at run time (found by `make fuzz-metamorphic`).
auto TypeChecker::missesRequiredArity(
    const Signature& required, std::size_t given, const std::string& name, SourceLocation loc)
    -> bool {
    // `describe : () -> String` takes nothing but its receiver.
    const bool none = requirementParams(required).size() != required.params.size();
    const auto receivers = implicitReceivers(required);
    const auto most = requirementArity(required);
    const auto least =
        (none ? 0 : required.requiredParams.value_or(required.params.size())) + receivers;
    if (given >= least && given <= most) {
        return false;
    }
    error(
        diag::Code::WrongArgumentCount, loc,
        {{"name", sourceName(name)},
         {"expected", std::to_string(most)},
         {"actual", std::to_string(given)}});
    return true;
}

} // namespace kex::semantic

namespace kex::semantic {

// A rescue clause binds names exactly like a match clause: its patterns
// introduce locals its guard and body can see, and `rescue |e|` binds the
// caught error. `rescue return` exits the enclosing function, so its
// expression is no arm.
auto TypeChecker::checkRescue(
    const ast::RescueBlock& rescue, const std::function<void(const TypePtr&)>& onArm) -> void {
    for (const auto& clause : rescue.clauses) {
        pushScope();
        for (const auto& pat : clause.patterns) {
            if (pat) {
                checkPatternArity(*pat);
                bindPatternVars(*pat);
            }
        }
        if (clause.guard && *clause.guard) {
            checkCondition(**clause.guard, diag::Code::GuardNotBool);
        }
        if (clause.body) {
            auto armType = inferExpr(*clause.body);
            if (onArm && !alwaysReturns(*clause.body)) {
                onArm(armType);
            }
        }
        popScope();
    }
    if (!rescue.catchAllBody.empty()) {
        pushScope();
        if (!rescue.catchAllParam.empty()) {
            defineVar(rescue.catchAllParam, Type::unknown());
        }
        auto catchAllType = inferBody(rescue.catchAllBody);
        if (onArm && !alwaysReturns(rescue.catchAllBody)) {
            onArm(catchAllType);
        }
        popScope();
    }
    if (rescue.inlineReturnExpr) {
        inferExpr(*rescue.inlineReturnExpr);
    }
}

} // namespace kex::semantic

namespace kex::semantic {

// A generic several parameters share is one type: `same : A -> A -> Bool`
// does not take `1` and `"s"`, nor `Process<A>.send` a String for a
// `Process<Msg>`. The arguments before bind it; the first one after that
// fits neither way is the conflict, with the type it had to be. Two
// numbers always agree (the numeric tower); an open type agrees.
auto TypeChecker::genericConflict(
    const Signature& sig,
    const std::vector<TypePtr>& argTypes,
    const std::function<TypePtr(std::size_t, const TypePtr&)>& selectedType)
    -> std::optional<std::pair<std::size_t, TypePtr>> {
    std::unordered_map<int, TypePtr> bound;
    std::function<TypePtr(const TypePtr&, const TypePtr&)> conflict =
        [&](const TypePtr& pattern, const TypePtr& given) -> TypePtr {
        if (!pattern || !given) {
            return nullptr;
        }
        const auto actual = resolve(given);
        if (containsOpenType(actual)) {
            return nullptr;
        }
        int id = 0;
        if (const auto* tv = std::get_if<TypeVar>(&pattern->kind)) {
            id = tv->id;
        }
        if (const auto* c = std::get_if<ConstrainedType>(&pattern->kind)) {
            id = c->genericId;
        }
        if (isGenericLetter(typeVarFromId(id))) {
            const auto [it, fresh] = bound.try_emplace(id, actual);
            if (fresh || argMatchesParam(actual, it->second) ||
                argMatchesParam(it->second, actual) ||
                (isNumericType(actual) && isNumericType(it->second))) {
                return nullptr;
            }
            return it->second;
        }
        const auto both = [&](const TypePtr& p, const TypePtr& a) { return conflict(p, a); };
        // A map is `{K: V}`, and the stdlib's functions on it are declared in
        // `make Map<K, V>`: read as no match, the receiver bound neither, and
        // `m.put("b", "x")` on a `{String: Integer}` passed (kexhq/kex#771).
        const auto* am = std::get_if<MapType>(&actual->kind);
        // What a map pattern had to be, its generics as the arguments bound them.
        const auto boundTo = [&](const TypePtr& p) -> TypePtr {
            int generic = 0;
            if (const auto* tv = std::get_if<TypeVar>(&p->kind)) {
                generic = tv->id;
            } else if (const auto* c = std::get_if<ConstrainedType>(&p->kind)) {
                generic = c->genericId;
            }
            const auto it = bound.find(generic);
            return generic < 0 && it != bound.end() ? it->second : p;
        };
        const auto mapConflict = [&](const TypePtr& key, const TypePtr& value) -> TypePtr {
            if (both(key, am->key) || both(value, am->value)) {
                return Type::map(boundTo(key), boundTo(value));
            }
            return nullptr;
        };
        if (const auto* pn = std::get_if<NamedType>(&pattern->kind);
            am && pn && pn->name == kex::names::kMapType && pn->typeArgs.size() == 2) {
            return mapConflict(pn->typeArgs[0], pn->typeArgs[1]);
        }
        if (const auto* pm = std::get_if<MapType>(&pattern->kind); am && pm) {
            return mapConflict(pm->key, pm->value);
        }
        if (const auto* pn = std::get_if<NamedType>(&pattern->kind)) {
            const auto* an = std::get_if<NamedType>(&actual->kind);
            if (!an || pn->name != an->name || pn->typeArgs.size() != an->typeArgs.size()) {
                return nullptr;
            }
            for (std::size_t k = 0; k < pn->typeArgs.size(); ++k) {
                if (both(pn->typeArgs[k], an->typeArgs[k])) {
                    return pattern;
                }
            }
        } else if (const auto* pl = std::get_if<ListType>(&pattern->kind)) {
            if (const auto* al = std::get_if<ListType>(&actual->kind);
                al && both(pl->element, al->element)) {
                return pattern;
            }
        } else if (const auto* po = std::get_if<OptionalType>(&pattern->kind)) {
            if (const auto* ao = std::get_if<OptionalType>(&actual->kind);
                ao && both(po->inner, ao->inner)) {
                return pattern;
            }
        }
        return nullptr;
    };
    for (std::size_t i = 0; i < argTypes.size() && i < sig.params.size(); ++i) {
        const auto selected = selectedType(i, sig.params[i]);
        if (auto expected = conflict(sig.params[i], selected ? selected : argTypes[i])) {
            return std::pair{i, expected};
        }
    }
    return std::nullopt;
}

} // namespace kex::semantic

namespace kex::semantic {

// A literal where a Byte goes is checked against the range a Byte holds:
// `let b: Byte = 300`, `Binary.fromBytes([1, 300])`, `g(-1)` for `g(b: Byte)`.
// The literal's own type is Integer, which a Byte parameter takes.
auto TypeChecker::checkByteLiterals(const ast::Expr& value, const TypePtr& expected) -> void {
    const auto isByte = [](const TypePtr& type) {
        const auto* sized = type ? std::get_if<SizedIntType>(&type->kind) : nullptr;
        return sized && sized->bits == 8 && !sized->isSigned;
    };
    const auto check = [](const ast::Expr& literal) {
        if (const auto* unary = std::get_if<ast::UnaryOp>(&literal.kind);
            unary && unary->op == TokenType::Minus && unary->operand &&
            std::holds_alternative<ast::IntLiteral>(unary->operand->kind)) {
            return false;
        }
        const auto* integer = std::get_if<ast::IntLiteral>(&literal.kind);
        if (!integer) {
            return true;
        }
        try {
            std::size_t parsed = 0;
            const auto number = std::stoull(integer->value, &parsed, 0);
            return parsed == integer->value.size() && number <= 255;
        } catch (...) {
            return false;
        }
    };
    const auto target = resolve(expected);
    if (isByte(target)) {
        if (!check(value)) {
            error(diag::Code::ByteLiteralRange, value.location);
        }
        return;
    }
    const auto* list = std::get_if<ListType>(&target->kind);
    const auto* written = std::get_if<ast::ListExpr>(&value.kind);
    if (!list || !written || !isByte(list->element)) {
        return;
    }
    for (const auto& element : written->elements) {
        if (element && !check(*element)) {
            error(diag::Code::ByteLiteralRange, element->location);
        }
    }
}

} // namespace kex::semantic

namespace kex::semantic {

// Whether `name` is a type this program knows by that name: a built-in, a
// record, an ADT or an alias. What a type name in value position, or a
// parameter written as one, has to be to stand for the type.
auto TypeChecker::namesKnownType(const std::string& name) const -> bool {
    // A type the imported sources declare counts too: `Binary`, `List`.
    return isPrimitiveTypeName(name) || m_recordFields.count(resolveRecordName(name)) ||
           m_adtVariants.count(name) || m_typeAliases.count(name) ||
           (m_importedInterfaces && m_importedInterfaces->typeNames.count(name));
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::operatorOverloadsFor(
    const TypePtr& left,
    const std::string& op,
    const std::vector<const ImportedFunction*>& imported) -> std::vector<OperatorOverload> {
    std::vector<OperatorOverload> owned;
    // The left operand is of the overload's receiver type: a named type for a
    // named receiver (a variant for its ADT: `Watt` for `make SIUnit`), the
    // same primitive, or one satisfying the trait the receiver names (`make
    // Number do let *(m: Measure)`). argMatchesParam alone is gradual, and took
    // an `Optional<Optional<T>>` with an open `T` for Measure's `==` receiver.
    const auto resolvedLeft = resolve(left);
    const auto receiverIs = [&](const TypePtr& receiver) {
        if (const auto* param = std::get_if<NamedType>(&receiver->kind)) {
            if (m_traits.lookup(param->name)) {
                return m_traits.satisfies(resolvedLeft, param->name);
            }
            return std::holds_alternative<NamedType>(resolvedLeft->kind) &&
                   argMatchesParam(resolvedLeft, receiver);
        }
        return typesEqual(resolvedLeft, receiver);
    };
    const auto consider = [&](const Signature& signature, const ImportedFunction* imported) {
        if (signature.params.size() != std::size_t(2)) {
            return;
        }
        const auto receiverParam = resolve(signature.params[0]);
        if (std::holds_alternative<TypeVar>(receiverParam->kind) ||
            std::holds_alternative<UnknownType>(receiverParam->kind)) {
            return;
        }
        if (receiverIs(receiverParam)) {
            owned.push_back({signature, imported});
        }
    };
    if (const auto local = m_methodSignatures.find(op); local != m_methodSignatures.end()) {
        for (const auto& signature : local->second) {
            consider(signature, nullptr);
        }
    }
    for (const auto* function : imported) {
        consider(function->signature, function);
    }
    return owned;
}

auto TypeChecker::decideOperators(const ast::Program& program) -> void {
    const auto typeOf = [&](const ast::ExprPtr& operand) -> TypePtr {
        if (!operand) {
            return Type::unknown();
        }
        const auto found = m_typeMap.find(operand.get());
        return found == m_typeMap.end() ? Type::unknown() : resolve(found->second);
    };

    // The built-in operator, with what its operands are known to be.
    const auto primitive = [](const TypePtr& type, PrimitiveType::Kind kind) {
        const auto* p = std::get_if<PrimitiveType>(&type->kind);
        return p && p->kind == kind;
    };
    const auto builtinOperands = [&](const TypePtr& left, const TypePtr& right) {
        const auto both = [&](PrimitiveType::Kind kind) {
            return primitive(left, kind) && primitive(right, kind);
        };
        if (both(PrimitiveType::Integer)) {
            return BuiltinOperands::Integer;
        }
        if (m_traits.satisfies(left, "Number") && m_traits.satisfies(right, "Number")) {
            return BuiltinOperands::Number;
        }
        if (both(PrimitiveType::String)) {
            return BuiltinOperands::String;
        }
        if (both(PrimitiveType::Char)) {
            return BuiltinOperands::Char;
        }
        return BuiltinOperands::Checked;
    };
    const auto decide = [&](const ast::Expr& expr, const ast::BinaryOp& node) {
        const std::string op(tokenTypeName(node.op));
        // What the operator's scope imported, as it was checked.
        const auto captured = m_operatorImports.find(&expr);
        const auto importsOf = [&](const std::string& name) {
            if (captured != m_operatorImports.end()) {
                if (const auto found = captured->second.find(name);
                    found != captured->second.end()) {
                    return found->second;
                }
            }
            return importedCandidates(name);
        };
        const auto overloadedAnywhere = [&](const std::string& name) {
            return m_methodSignatures.contains(name) || !importsOf(name).empty();
        };
        const auto left = typeOf(node.left);
        const auto right = typeOf(node.right);
        const auto builtin = [&] {
            m_decidedCalls[&expr] = {CallKind::Builtin, {}, op, builtinOperands(left, right)};
        };
        // Two numbers: the built-in operator, whatever overloads exist.
        if (m_traits.satisfies(left, "Number") && m_traits.satisfies(right, "Number")) {
            return builtin();
        }
        // A left value whose type is not known here: its own type decides
        // when it runs.
        if (!knownWhenChecked(left)) {
            if (overloadedAnywhere(op)) {
                m_decidedCalls[&expr] = {CallKind::Dispatch, {}, op};
            } else {
                builtin();
            }
            return;
        }
        const auto owner = typeToString(left);
        // The left type's own overload of `name` for an argument it takes,
        // if any; for `==`, one it does not take leaves the two values to
        // structural equality.
        const auto overloadTaking =
            [&](const std::string& name) -> std::optional<OperatorOverload> {
            for (auto& overload : operatorOverloadsFor(left, name, importsOf(name))) {
                if (argMatchesParam(right, overload.signature.params[1])) {
                    return overload;
                }
            }
            return std::nullopt;
        };
        // The operator as `implementation` (`==` for a `!=` that negates it)
        // of the left type: the imported function, or this unit's own.
        const auto decideAs = [&](const std::string& implementation,
                                  const ImportedFunction* imported) {
            DecidedCall decided{CallKind::Operator, owner, implementation};
            decided.negated = implementation != op;
            if (imported) {
                decided.backendModule = imported->backendModule;
                decided.function = imported->backendFunction;
            }
            m_decidedCalls[&expr] = std::move(decided);
        };
        // Two of the language's own primitives (String, Char, Bool, Atom, and
        // the numbers above) take the built-in operator: an implementor of
        // Comparable is given its `<` too, and String's `compare`, built on
        // `<`, would call itself.
        if (std::holds_alternative<PrimitiveType>(left->kind) &&
            std::holds_alternative<PrimitiveType>(right->kind)) {
            return builtin();
        }
        // Then the left type's own overload, for an argument it takes (a
        // distinct type's over the operator of what it erases to; Binary's
        // `+`); `!=` with no overload of its own negates the owner's `==`.
        if (const auto own = overloadTaking(op)) {
            return decideAs(op, own->imported);
        }
        const bool notEqual = node.op == TokenType::NotEq;
        const std::string equal(tokenTypeName(TokenType::EqEq));
        if (const auto own = notEqual ? overloadTaking(equal) : std::nullopt) {
            return decideAs(equal, own->imported);
        }
        // Any two values are equal or not structurally (kexhq/kex#486): no
        // trait decides `==`.
        if (notEqual || node.op == TokenType::EqEq) {
            return builtin();
        }
        // Then the built-in operator, and only then a trait's.
        if (!lacksBuiltinOperator(left, op) && !lacksBuiltinOperator(right, op)) {
            return builtin();
        }
        if (traitDeclaresOperator(left, op)) {
            return decideAs(op, nullptr);
        }
        // No built-in, and no overload the left type names that the check
        // could select (`Units.SI`'s `*` on two measures is found when the
        // call is checked, not by the receiver's declarations): the value
        // decides when it runs.
        if (overloadedAnywhere(op)) {
            m_decidedCalls[&expr] = {CallKind::Dispatch, {}, op};
        } else {
            builtin();
        }
    };
    const auto visit = [&](const auto& self, const ast::Expr& expr) -> void {
        if (const auto* node = std::get_if<ast::BinaryOp>(&expr.kind)) {
            decide(expr, *node);
        }
        ast::forEachChild(expr, [&](const ast::ExprPtr& child) {
            if (child) {
                self(self, *child);
            }
        });
    };
    ast::forEachBodyExpr(program, [&](const ast::Expr& expr) { visit(visit, expr); });
}

} // namespace kex::semantic

namespace kex::semantic {

// A type variable, a value known only by a trait (`x: Summable`, or a bound
// one), or `This` in a trait's default body: whichever value it is answers.
auto TypeChecker::knownWhenChecked(const TypePtr& type) const -> bool {
    if (std::holds_alternative<TypeVar>(type->kind) ||
        std::holds_alternative<UnknownType>(type->kind) ||
        std::holds_alternative<ConstrainedType>(type->kind)) {
        return false;
    }
    const auto* named = std::get_if<NamedType>(&type->kind);
    return !named || (named->name != names::kThisType && !m_traits.lookup(named->name));
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::dictionaryTypeOf(const ast::Expr& value) const -> std::string {
    const auto found = m_typeMap.find(&value);
    if (found == m_typeMap.end() || !found->second) {
        return {};
    }
    const auto& type = found->second;
    if (std::holds_alternative<TypeVar>(type->kind) ||
        std::holds_alternative<UnknownType>(type->kind) ||
        std::holds_alternative<ConstrainedType>(type->kind)) {
        return {};
    }
    if (const auto* named = std::get_if<NamedType>(&type->kind)) {
        if (m_traits.get(named->name)) {
            return {};
        }
        if (const auto* owner = constructorOwner(*named)) {
            return *owner;
        }
    }
    return typeToString(type);
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::declaredTraitOf(const TypePtr& type) const -> std::string {
    const auto resolved = resolve(type);
    std::string name;
    if (const auto* constrained = std::get_if<ConstrainedType>(&resolved->kind)) {
        name = constrained->traitName;
    } else if (const auto* named = std::get_if<NamedType>(&resolved->kind)) {
        name = named->name;
    }
    // A trait some module declares: the numeric constraints are the
    // language's own types, with no dictionary.
    return !isPrimitiveTypeName(name) && m_traits.get(name) ? name : std::string{};
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::traitWrittenAs(const std::string& scope, const std::string& written) const
    -> std::string {
    const auto found = m_writtenTraits.find(writtenKey(scope, written));
    return found == m_writtenTraits.end() ? std::string{} : found->second;
}

} // namespace kex::semantic

namespace kex::semantic {

// The module constant (a parameterless member) a bare `name` means here, by
// its qualified name: one an enclosing module declares, else one a `using`
// in scope brings in, from this program or a unit compiled apart. "" when
// the name is none.
auto TypeChecker::constantDeclaration(const std::string& name) const -> std::string {
    const auto parameterless = [&](const std::string& key) {
        const auto found = m_userSignatures.find(key);
        return found != m_userSignatures.end() &&
               std::any_of(
                   found->second.begin(), found->second.end(),
                   [](const Signature& signature) { return signature.params.empty(); });
    };
    for (auto scope = m_currentModulePath; !scope.empty();) {
        if (parameterless(kex::mangle::qualify(scope, name))) {
            return scope + "." + name;
        }
        const auto dot = scope.rfind('.');
        scope.resize(dot == std::string::npos ? 0 : dot);
    }
    for (const auto& module : m_localModules) {
        if (parameterless(kex::mangle::qualify(module, name)) &&
            moduleMemberImported(module, name, true)) {
            return module + "." + name;
        }
    }
    if (m_importedInterfaces) {
        for (const auto& [_, module] : m_importedInterfaces->modules) {
            if (const auto exports = module.exports.find(name); exports != module.exports.end()) {
                for (const auto& function : exports->second) {
                    if (!function.isConstructor && function.signature.params.empty() &&
                        !function.sourceModule.empty() &&
                        moduleMemberImported(function.sourceModule, name, true)) {
                        return function.sourceModule + "." + name;
                    }
                }
            }
        }
    }
    return {};
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::registerDeclaredAliases(const ast::Program& program) -> void {
    m_declaredAliases.clear();
    m_structuralAliases.clear();
    m_moduleTypeIdentities.clear();
    m_typeDeclaringModules.clear();
    m_declaredTypeNames.clear();
    m_reportedUnknownTypes.clear();
    m_reportedTypeSplices.clear();
    m_pendingUnknownTypes.clear();
    m_declarationsRegistered = false;
    kex::collectDeclaredTypeNames(program.items, m_declaredTypeNames);
    kex::collectDeclaredTraitNames(program.items, m_declaredTypeNames);
    m_declaredBeforeMakes = m_declaredTypeNames;
    kex::collectMakeTargetNames(program.items, m_declaredTypeNames);
    kex::forEachTypeDef(program.items, [&](const ast::TypeDef& def, const std::string& module) {
        if (!module.empty()) {
            m_moduleTypeIdentities.insert(module + "." + def.name);
            m_typeDeclaringModules.emplace(def.name, module);
        }
        if (!def.variants) {
            return;
        }
        // A structural right-hand side (`type Handler = Env -> String`), or
        // the plain name a transparent alias renames (`type Slug = String`):
        // either way no value carries the alias's own name as a tag.
        if (!def.isDistinct && !def.leadingPipe && def.variants->size() == 1 &&
            (*def.variants)[0] &&
            (kex::isTransparentTypeAlias(def) || ast::dispatchNameOf((*def.variants)[0]).empty())) {
            m_structuralAliases[def.name] =
                ast::typeText(*(*def.variants)[0], ast::TypeSpelling::Dispatch);
        }
        if (const auto* target = kex::aliasTarget(def)) {
            m_declaredAliases[def.name] = target;
        }
    });
}

auto TypeChecker::declaredTypeOf(const ast::TypeExpr& type) const -> StructuredType {
    return expandDeclaredAliases(structuredTypeOfDeclared(type), m_declaredAliases);
}

auto TypeChecker::expandDeclared(StructuredType type) const -> StructuredType {
    return expandDeclaredAliases(std::move(type), m_declaredAliases);
}

auto TypeChecker::annotationSpelling(const ast::TypeExpr& type) const -> std::string {
    if (const auto record = m_annotatedRecords.find(&type); record != m_annotatedRecords.end()) {
        return record->second;
    }
    return ast::typeText(type, ast::TypeSpelling::Dispatch);
}

namespace {

// How many parameters a function type's spelling takes: its TOP-LEVEL
// arrows, so `(Env) -> String` is 1 and `(A) -> (B) -> C` is 2.
auto functionTypeArity(const std::string& written) -> int64_t {
    int depth = 0;
    int64_t arrows = 0;
    for (size_t i = 0; i < written.size(); ++i) {
        const char c = written[i];
        if (c == '(' || c == '[' || c == '{' || c == '<') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            --depth;
        } else if (c == '>' && !(i && written[i - 1] == '-')) {
            --depth;
        } else if (depth == 0 && c == '-' && i + 1 < written.size() && written[i + 1] == '>') {
            ++arrows;
            ++i;
        }
    }
    return arrows;
}

auto enclosedBy(const std::string& written, char open, char close) -> bool {
    return written.size() >= 2 && written.front() == open && written.back() == close;
}

} // namespace

auto TypeChecker::typeTestOf(const std::string& scope, const std::string& written) const
    -> TypeTest {
    auto test = valueFormOf(scope, written);
    // The type named as written, aliases not followed.
    const auto structural = written.empty() || written.find("->") != std::string::npos ||
                            std::string_view{"[{("}.find(written.front()) != std::string_view::npos;
    if (!structural) {
        test.written = written.substr(0, written.find('<'));
        test.writtenIdentity = typeIdentity(scope, test.written);
    }
    return test;
}

auto TypeChecker::valueFormOf(const std::string& scope, const std::string& written) const
    -> TypeTest {
    if (written.empty() || written == kex::names::kAnyType ||
        !traitWrittenAs(scope, written).empty()) {
        return {};
    }
    // `type Handler = Env -> String` names a SHAPE: no value carries the
    // alias's name. A structural right-hand side is never a bare name, so
    // this recurses at most once.
    if (const auto alias = m_structuralAliases.find(written);
        alias != m_structuralAliases.end() && alias->second != written) {
        return valueFormOf(scope, alias->second);
    }
    // A function type first: it is parenthesized too.
    if (written.find("->") != std::string::npos) {
        return {.form = TypeForm::Function, .arity = functionTypeArity(written)};
    }
    if (written.rfind("Block<", 0) == 0 && written.back() == '>') {
        return {.form = TypeForm::Block};
    }
    if (enclosedBy(written, '[', ']')) {
        return {.form = TypeForm::List};
    }
    if (enclosedBy(written, '{', '}')) {
        return {.form = TypeForm::Map};
    }
    if (enclosedBy(written, '(', ')')) {
        return {.form = TypeForm::Tuple};
    }
    // Type arguments say nothing about the value's own form.
    if (const auto angle = written.find('<');
        angle != std::string::npos && angle > 0 && written.back() == '>') {
        return valueFormOf(scope, written.substr(0, angle));
    }
    if (written.size() == 1) {
        return {}; // a type parameter, not a type
    }
    return {.form = TypeForm::Named, .name = written, .identity = typeIdentity(scope, written)};
}

// A type a module declares is named by its identity: the nearest enclosing
// module that declares it, else the module that does.
auto TypeChecker::typeIdentity(const std::string& scope, const std::string& name) const
    -> std::string {
    if (name.find('.') != std::string::npos || m_recordFields.count(name)) {
        return name;
    }
    for (auto module = scope; !module.empty();) {
        if (m_moduleTypeIdentities.count(module + "." + name) ||
            m_recordFields.count(module + "." + name)) {
            return module + "." + name;
        }
        const auto dot = module.rfind('.');
        module.resize(dot == std::string::npos ? 0 : dot);
    }
    if (const auto declaring = m_typeDeclaringModules.find(name);
        declaring != m_typeDeclaringModules.end()) {
        return declaring->second + "." + name;
    }
    return name;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::joinElementType(TypePtr joined, const TypePtr& next) -> TypePtr {
    if (isUndetermined(joined)) {
        return next; // adopt the concrete type if there is one
    }
    const bool nextPermissive =
        isUndetermined(next) || std::holds_alternative<FuncType>(next->kind);
    if (nextPermissive || std::holds_alternative<FuncType>(joined->kind)) {
        return joined;
    }
    // Elements join as a conditional's branches do: different types into their
    // union, never into a trait they share. `[Circle {…}, Square {…}]` is
    // `[Circle | Square]`, and a shared function still works on it, because a
    // union has a trait when every member does. Picking a shared trait took the
    // first in iteration order, needed a list of traits never to pick (`1` and
    // `"a"` are both Blankable), and rejected the literal where a
    // `[Circle | Square]` was asked for.
    return joinBranchType(joined, next);
}

auto TypeChecker::joinBranchType(const TypePtr& joined, const TypePtr& next) const -> TypePtr {
    const auto a = resolve(joined);
    const auto b = resolve(next);
    if (std::holds_alternative<NeverType>(b->kind)) {
        return a;
    }
    if (std::holds_alternative<NeverType>(a->kind)) {
        return b;
    }
    // A letter of the function being checked is a known type, not a hole:
    // `then x else None` for `x: A` is `A` or an optional, never `A?`.
    const auto rigid = [&](const TypePtr& type) {
        const auto id = typeVariableId(type);
        return id && m_rigidVars.contains(*id);
    };
    if (rigid(a) || rigid(b)) {
        if (argMatchesParam(b, a)) {
            return a;
        }
        if (argMatchesParam(a, b)) {
            return b;
        }
        return unionOf(a, b);
    }
    if (isUndetermined(a)) {
        return b;
    }
    if (isUndetermined(b)) {
        return a;
    }
    if (auto sibling = siblingConstructorJoin(a, b)) {
        return sibling;
    }
    if (auto known = moreKnownBranch(a, b)) {
        return known;
    }
    // The wider of two that fit, whichever came first: the then-branch's type
    // used to win, so `c then n else x` (Integer, Number) was an Integer and
    // `c then x else n` a Number.
    if (argMatchesParam(b, a)) {
        return a;
    }
    if (argMatchesParam(a, b)) {
        return b;
    }
    const auto isVoid = [](const TypePtr& type) { return typesEqual(type, Type::voidType()); };
    if (isVoid(a) || isVoid(b)) {
        return Type::voidType(); // a branch without a value: the conditional has none
    }
    return unionOf(a, b);
}

auto TypeChecker::branchCheckTarget(const TypePtr& expected) const -> TypePtr {
    if (!expected || typesEqual(resolve(expected), Type::voidType())) {
        return nullptr;
    }
    return expected;
}

auto TypeChecker::joinBranches(const std::vector<Branch>& branches, const TypePtr& expected)
    -> TypePtr {
    if (expected) {
        for (const auto& branch : branches) {
            const auto type = resolve(branch.type);
            if (std::holds_alternative<NeverType>(type->kind) ||
                (isUndetermined(type) &&
                 !(typeVariableId(type) && m_rigidVars.contains(*typeVariableId(type))))) {
                continue;
            }
            if (!argMatchesParam(type, expected)) {
                typeMismatch(branch.location, expected, type);
            }
        }
        return expected;
    }
    TypePtr joined = Type::never();
    for (const auto& branch : branches) {
        joined = joinBranchType(joined, branch.type);
    }
    if (!branches.empty()) {
        reportIndistinguishable(joined, branches.front().location);
    }
    return joined;
}

auto TypeChecker::unionOf(const TypePtr& a, const TypePtr& b) -> TypePtr {
    std::vector<TypePtr> members;
    const auto add = [&](const TypePtr& type) {
        if (std::none_of(members.begin(), members.end(), [&](const TypePtr& held) {
                return typesEqual(held, type);
            })) {
            members.push_back(type);
        }
    };
    for (const auto& side : {a, b}) {
        if (const auto* existing = std::get_if<UnionType>(&side->kind)) {
            for (const auto& member : existing->members) {
                add(member);
            }
        } else {
            add(side);
        }
    }
    if (members.size() == 1) {
        return members.front();
    }
    return std::make_shared<Type>(Type{UnionType{std::move(members)}});
}

auto TypeChecker::checkNeverEqual(const TypePtr& lhs, const TypePtr& rhs, SourceLocation loc)
    -> void {
    // Two primitives of different kinds are never equal: `1 == "1"` and
    // `:kex == true` were accepted and always false (kexhq/kex#773). Integer
    // and Float are one numeric tower (docs/language-spec.md §23.6), so
    // `0 == 0.0` stays. So does a Char against a String: `c == "e"` is false
    // by design, and spec/char_type.kex teaches it beside `letters == "hi"`
    // (a `[Char]` and a String), which this rule does not reach.
    const auto* leftPrimitive = std::get_if<PrimitiveType>(&lhs->kind);
    const auto* rightPrimitive = std::get_if<PrimitiveType>(&rhs->kind);
    const auto textual = [](const PrimitiveType* p) {
        return p && (p->kind == PrimitiveType::Char || p->kind == PrimitiveType::String);
    };
    const auto number = [](const TypePtr& t) {
        const auto* bound = std::get_if<ConstrainedType>(&t->kind);
        return isNumericType(t) || (bound && primitiveNumberTrait(bound->traitName));
    };
    const auto scalar = [&](const TypePtr& t) {
        return std::holds_alternative<PrimitiveType>(t->kind) || number(t);
    };
    if ((leftPrimitive || rightPrimitive) && scalar(lhs) && scalar(rhs) &&
        !(number(lhs) && number(rhs)) && !(textual(leftPrimitive) && textual(rightPrimitive)) &&
        !(leftPrimitive && rightPrimitive && leftPrimitive->kind == rightPrimitive->kind)) {
        error(
            diag::Code::UnrelatedNeverEqual, loc,
            {{"left", typeToString(lhs)}, {"right", typeToString(rhs)}});
        return;
    }
    const auto wrapper = [](const TypePtr& t) -> std::string {
        if (std::holds_alternative<OptionalType>(t->kind)) {
            return "Optional";
        }
        const auto* named = std::get_if<NamedType>(&t->kind);
        return named && (named->name == kex::names::kOptionalType ||
                         named->name == kex::names::kResultType)
                   ? named->name
                   : std::string{};
    };
    // A value whose type is settled and is no Optional or Result: not open,
    // not a trait (or a union) an Optional could be one of.
    const auto settled = [&](const TypePtr& t) {
        const auto* named = std::get_if<NamedType>(&t->kind);
        return !containsOpenType(t) && wrapper(t).empty() && !(named && isTrait(named->name)) &&
               !std::holds_alternative<UnionType>(t->kind);
    };
    const auto left = wrapper(lhs), right = wrapper(rhs);
    if (left.empty() == right.empty() || !settled(left.empty() ? lhs : rhs)) {
        return;
    }
    const auto& wrapped = left.empty() ? right : left;
    error(
        diag::Code::WrappedNeverEqual, loc,
        {{"left", typeToString(lhs)},
         {"right", typeToString(rhs)},
         {"constructor", wrapped == kex::names::kOptionalType ? "Just" : "Ok"}});
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::literalOutsideSet(const ast::Expr& value, const TypePtr& expected) const -> bool {
    const auto bare = [](std::string name) {
        if (!name.empty() && name.front() == ':') {
            name.erase(0, 1);
        }
        return name;
    };
    // The literal written, and the kind of value it is: an atom (`:mass`) or
    // an Integer (`2`, which selects `let ^(2)` over `let ^(3)`).
    std::string written;
    PrimitiveType::Kind kind{};
    if (const auto* atom = std::get_if<ast::AtomLiteral>(&value.kind)) {
        written = bare(atom->name);
        kind = PrimitiveType::Atom;
    } else if (const auto* integer = std::get_if<ast::IntLiteral>(&value.kind)) {
        written = integer->value;
        kind = PrimitiveType::Integer;
    }
    if (written.empty() || !expected) {
        return false;
    }
    // The literals `expected` admits, when it is a set of them and nothing else.
    std::vector<std::string> admitted;
    const auto member = [&](const TypePtr& type) {
        const auto* primitive = std::get_if<PrimitiveType>(&type->kind);
        if (!primitive || primitive->kind != kind || primitive->literal.empty()) {
            return false;
        }
        admitted.push_back(bare(primitive->literal));
        return true;
    };
    const auto resolved = resolve(expected);
    if (const auto* union_ = std::get_if<UnionType>(&resolved->kind)) {
        for (const auto& option : union_->members) {
            if (!option || !member(resolve(option))) {
                return false;
            }
        }
    } else if (!member(resolved)) {
        return false;
    }
    return std::find(admitted.begin(), admitted.end(), written) == admitted.end();
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::tryPayload(const TypePtr& operand) const -> TypePtr {
    const auto resolved = resolve(operand);
    if (const auto* optional = std::get_if<OptionalType>(&resolved->kind)) {
        return optional->inner;
    }
    if (const auto* named = std::get_if<NamedType>(&resolved->kind);
        named && !named->typeArgs.empty() &&
        (named->name == kex::names::kOptionalType || named->name == kex::names::kResultType)) {
        return named->typeArgs[0];
    }
    return nullptr;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::overloadsTake(
    const std::vector<const Signature*>& candidates, std::size_t position) const -> TypePtr {
    std::vector<TypePtr> taken;
    for (const auto* candidate : candidates) {
        if (position >= candidate->params.size()) {
            return nullptr;
        }
        const auto param = resolve(candidate->params[position]);
        // A generic or open parameter takes anything: no union says it.
        if (!isConcreteValue(param)) {
            return nullptr;
        }
        if (std::none_of(taken.begin(), taken.end(), [&](const TypePtr& held) {
                return typesEqual(held, param);
            })) {
            taken.push_back(param);
        }
    }
    if (taken.empty()) {
        return nullptr;
    }
    if (taken.size() == 1) {
        return taken.front();
    }
    return std::make_shared<Type>(Type{UnionType{std::move(taken)}});
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::unionArgumentCovered(
    const std::vector<const Signature*>& candidates, const std::vector<TypePtr>& argTypes) const
    -> std::optional<TypePtr> {
    for (size_t i = 0; i < argTypes.size(); ++i) {
        const auto* unionArg = std::get_if<UnionType>(&resolve(argTypes[i])->kind);
        if (!unionArg) {
            continue;
        }
        std::vector<TypePtr> results;
        const bool covered = std::all_of(
            unionArg->members.begin(), unionArg->members.end(), [&](const TypePtr& member) {
                for (const auto* candidate : candidates) {
                    bool takes = true;
                    for (size_t j = 0; takes && j < argTypes.size() && j < candidate->params.size();
                         ++j) {
                        takes = argMatchesParam(
                            j == i ? resolve(member) : resolve(argTypes[j]),
                            resolve(candidate->params[j]));
                    }
                    if (takes) {
                        results.push_back(resolve(candidate->result));
                        return true;
                    }
                }
                return false;
            });
        if (!covered) {
            continue;
        }
        const bool oneResult = std::all_of(results.begin(), results.end(), [&](const TypePtr& r) {
            return typesEqual(r, results.front());
        });
        return oneResult ? results.front() : Type::unknown();
    }
    return std::nullopt;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::moreKnownBranch(const TypePtr& held, const TypePtr& next) const -> TypePtr {
    const auto heldType = resolvedThroughout(held);
    const auto nextType = resolvedThroughout(next);
    // Only a hole counts, not a bound: `Number` beside `Integer` is the wider
    // type, not a less known one, and preferring the Integer typed a branch
    // that could be a Float as an Integer.
    if (!containsUnsolved(heldType) || containsUnsolved(nextType)) {
        return nullptr;
    }
    return argMatchesParam(nextType, heldType) ? nextType : nullptr;
}

} // namespace kex::semantic
