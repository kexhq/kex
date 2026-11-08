#include "../ast/function_groups.hxx"
#include "../common/mangle.hxx"
#include "resolve_core.hxx"
#include "typechecker.hxx"
#include "typechecker_internal.hxx"
#include "../ast/walk.hxx"
#include "analyzer.hxx"
#include "../common/type_def_utils.hxx"
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
    if (const auto* record = std::get_if<RecordType>(&type->kind))
        for (const auto& [fieldName, fieldType] : record->fields)
            if (fieldName == name) return fieldType;
    if (const auto* intersection = std::get_if<IntersectionType>(&type->kind))
        for (const auto& member : intersection->members)
            if (auto found = member ? structuralField(member, name) : nullptr) return found;
    return nullptr;
}

} // namespace

auto TypeChecker::resolveBlockHints(const std::string& name,
                                     const std::vector<TypePtr>& nonBlockArgTypes,
                                     bool isMethodCall,
                                     bool* collection,
                                     std::string* skippedTo) -> std::vector<TypePtr> {
    if (skippedTo) skippedTo->clear();
    auto imported = importedCandidateSignatures(name);
    if (isMethodCall)
        for (const auto* function : usingImportedMethods(name))
            imported.push_back(function->signature);
    const std::vector<Signature>* sigs = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() &&
        scoped != m_scopedDeclaredSignatures.end())
        sigs = &scoped->second;
    else if (auto user = m_userSignatures.find(name);
             user != m_userSignatures.end())
        sigs = &user->second;
    auto methodIt = isMethodCall ? m_methodSignatures.find(name)
                                 : m_methodSignatures.end();
    if (!sigs && imported.empty() && methodIt == m_methodSignatures.end())
        return {};

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
        if (sig.params.size() < nonBlockArgTypes.size() + 1) return;
        if (sig.params.size() != nonBlockArgTypes.size() + 1 &&
            (sig.requiredParams.value_or(sig.params.size()) >
                 nonBlockArgTypes.size() + 1 ||
             std::holds_alternative<FuncType>(
                 resolve(sig.params[nonBlockArgTypes.size()])->kind)))
            return;
        auto* blockParam = std::get_if<FuncType>(&sig.params.back()->kind);
        // A parameter that takes a function or something else takes the
        // block as that function (`replace : ... (String | (Match -> String))`).
        if (auto* either = std::get_if<UnionType>(&sig.params.back()->kind); !blockParam && either)
            for (const auto& member : either->members)
                if (auto* function = std::get_if<FuncType>(&member->kind)) {
                    if (blockParam) return;
                    blockParam = function;
                }
        if (!blockParam) return;

        for (size_t i = 0; i < nonBlockArgTypes.size(); i++) {
            if (!argMatchesParam(nonBlockArgTypes[i], sig.params[i])) return;
        }

        // Map the signature's type variables to concrete types from the actual
        // args, whatever their ids. Negative ids are table generics; a make
        // block's `:>` method compiled in this same unit (the prelude's own
        // `reduce :> A -> ...`) carries ordinary variables instead, and one
        // already bound by an unrelated caller would otherwise hand that
        // caller's type to every later block (kexhq/kex#408). This only
        // computes hints: nothing here is unified.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < nonBlockArgTypes.size(); i++)
            bindHintVars(sig.params[i], nonBlockArgTypes[i], sub,
                         [&](const TypePtr& t) { return resolve(t); });

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) return it2->second;
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : blockParam->params) hints.push_back(applySubst(p));
        auto result = resolve(blockParam->result);
        bool isCollection = blockParam->block &&
            std::holds_alternative<ListType>(result->kind);
        std::string skipped;
        if (sig.params.size() != nonBlockArgTypes.size() + 1) {
            if (sig.paramNames.empty()) return;
            skipped = sig.paramNames.back();
        }
        candidates.push_back({std::move(hints), isCollection,
                              isMethodCall && !sig.params.empty() &&
                                  !isOpenType(sig.params.front()),
                              std::move(skipped)});
    };

    if (methodIt != m_methodSignatures.end())
        for (const auto& sig : methodIt->second) hintsFrom(sig);
    for (const auto& sig : imported) hintsFrom(sig);
    if (sigs)
        for (const auto& sig : *sigs) hintsFrom(sig);

    if (candidates.empty()) return {};
    // A receiver of known type is matched exactly by the overloads written for
    // it; one that accepts anything (an untyped or trait-bounded receiver)
    // only guesses, so it does not get a say once a concrete one fits.
    if (isMethodCall && !nonBlockArgTypes.empty() &&
        !isOpenType(nonBlockArgTypes.front()) &&
        std::any_of(candidates.begin(), candidates.end(),
                    [](const Candidate& c) { return c.concreteReceiver; }))
        std::erase_if(candidates,
                      [](const Candidate& c) { return !c.concreteReceiver; });
    // UFCS lets a plain call like `times(3) do ... end` match both a local
    // `times(n, block)` and an unrelated `Integer.times(block)` visible as a
    // free function. When such same-arity candidates disagree on the
    // block's shape (its declared arity, or whether it collects body
    // expressions as a `Block<[X]>`), trusting whichever matched first can
    // hand back a hint (and collection flag) for the WRONG candidate —
    // reporting a bogus arity mismatch, or silently dropping the collection
    // behavior the call actually needs (kexhq/kex#321). Stay permissive
    // instead: no reliable single shape to hint from.
    for (const auto& c : candidates)
        if (c.hints.size() != candidates.front().hints.size() ||
            c.collection != candidates.front().collection) {
            if (collection) *collection = false;
            return {};
        }
    if (collection) *collection = candidates.front().collection;
    if (skippedTo &&
        std::all_of(candidates.begin(), candidates.end(), [&](const Candidate& c) {
            return !c.skippedTo.empty() &&
                   c.skippedTo == candidates.front().skippedTo;
        }))
        *skippedTo = candidates.front().skippedTo;
    std::vector<std::vector<TypePtr>> hintSets;
    for (const auto& c : candidates) hintSets.push_back(c.hints);
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
    if (hintSets.empty()) return {};
    auto hints = hintSets.front();
    for (size_t i = 0; i < hints.size(); ++i) {
        auto first = resolve(hints[i]);
        for (size_t c = 1; c < hintSets.size(); ++c) {
            if (i >= hintSets[c].size() ||
                !typesEqual(first, resolve(hintSets[c][i]))) {
                hints[i] = Type::unknown();
                break;
            }
        }
    }
    return hints;
}

auto TypeChecker::namedArgHints(const std::string& name,
                                 const std::string& label)
    -> std::vector<TypePtr> {
    auto candidates = importedCandidateSignatures(name);
    if (auto user = m_userSignatures.find(name); user != m_userSignatures.end())
        candidates.insert(candidates.begin(), user->second.begin(),
                          user->second.end());
    for (const auto& signature : candidates) {
        const auto named = std::find(signature.paramNames.begin(),
                                     signature.paramNames.end(), label);
        if (named == signature.paramNames.end()) continue;
        // A receiver prepended to `params` has no name of its own.
        const auto index =
            static_cast<size_t>(named - signature.paramNames.begin()) +
            (signature.params.size() - signature.paramNames.size());
        if (index >= signature.params.size()) continue;
        if (const auto* function =
                std::get_if<FuncType>(&resolve(signature.params[index])->kind))
            return function->params;
    }
    return {};
}

auto TypeChecker::resolveArgHints(const std::string& name,
                                   const std::vector<TypePtr>& argTypes,
                                   size_t slArgIdx,
                                   bool isMethodCall) -> std::vector<TypePtr> {
    auto imported = importedCandidateSignatures(name);
    const std::vector<Signature>* sigs = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() &&
        scoped != m_scopedDeclaredSignatures.end())
        sigs = &scoped->second;
    else if (auto user = m_userSignatures.find(name);
             user != m_userSignatures.end())
        sigs = &user->second;
    auto methodIt = isMethodCall ? m_methodSignatures.find(name)
                                 : m_methodSignatures.end();
    if (!sigs && imported.empty() && methodIt == m_methodSignatures.end())
        return {};

    auto hintsFrom = [&](const Signature& sig) -> std::vector<TypePtr> {
        if (sig.params.size() != argTypes.size()) return {};
        if (slArgIdx >= sig.params.size()) return {};
        auto* funcParam = std::get_if<FuncType>(&sig.params[slArgIdx]->kind);
        if (!funcParam) return {};

        // All non-SL positions must match.
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (i == slArgIdx) continue;
            if (!argMatchesParam(argTypes[i], sig.params[i])) return {};
        }

        // Build generic substitution from the concrete args.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < argTypes.size(); i++)
            if (i != slArgIdx)
                bindHintVars(sig.params[i], argTypes[i], sub,
                             [&](const TypePtr& t) { return resolve(t); });

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) return it2->second;
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : funcParam->params) hints.push_back(applySubst(p));
        return hints;
    };

    // Every candidate that fits, not the first: see agreedHints. As in
    // resolveBlockHints, a known receiver silences the overloads that would
    // accept any receiver.
    std::vector<std::vector<TypePtr>> hintSets;
    std::vector<std::vector<TypePtr>> concreteSets;
    const bool knownReceiver =
        isMethodCall && !argTypes.empty() && !isOpenType(argTypes.front());
    const auto consider = [&](const Signature& sig) {
        auto hints = hintsFrom(sig);
        if (hints.empty()) return;
        if (knownReceiver && !sig.params.empty() && !isOpenType(sig.params.front()))
            concreteSets.push_back(hints);
        hintSets.push_back(std::move(hints));
    };
    if (methodIt != m_methodSignatures.end())
        for (const auto& sig : methodIt->second) consider(sig);
    for (const auto& sig : imported) consider(sig);
    if (sigs)
        for (const auto& sig : *sigs) consider(sig);
    if (!concreteSets.empty()) hintSets = std::move(concreteSets);
    for (const auto& set : hintSets)
        if (set.size() != hintSets.front().size()) return {};
    return agreedHints(hintSets);
}

// A bare `~f` capture — no argument groups, no operator, unqualified. Like a
// shorthand lambda, it needs the callee's signature to know what it's being
// checked against, so it's deferred to the contextual (hinted) pass.
static auto isBareCapture(const ast::Expr* e) -> bool {
    if (!e) return false;
    const auto* ce = std::get_if<ast::CurryExpr>(&e->kind);
    return ce && ce->argGroups.empty() && !ce->isOperator && ce->module.empty();
}

auto TypeChecker::inferBlock(const ast::Expr& blockExpr,
                             const std::vector<TypePtr>& hintParams) -> TypePtr {
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
            if (!hintParams.empty()) return bodyType;
            return Type::unknown();
        }
        // `pairs.map { |glyph, words| … }` over `[(String, String)]` spreads
        // each element across the parameters on both backends, so the
        // parameters are typed by the tuple's elements, not each by the whole
        // tuple. `glyph` was `(String, String)` and passing it on to a
        // `String` parameter was rejected (kexhq/kex#537). The function-type
        // comparison accepts the block's shape (argMatchesParam).
        auto spreadHints = hintParams;
        if (lam->params.size() > 1 && hintParams.size() == 1)
            if (auto* tuple =
                    std::get_if<TupleType>(&resolve(hintParams[0])->kind);
                tuple && tuple->elements.size() == lam->params.size())
                spreadHints = tuple->elements;
        pushScope();
        std::vector<TypePtr> paramTypes;
        std::unordered_map<std::string, TypePtr> genericVars;
        for (size_t i = 0; i < lam->params.size(); i++) {
            TypePtr pt;
            if (lam->params[i].type) {
                pt = resolveTypeExpr(**lam->params[i].type, genericVars);
            } else if (i < spreadHints.size()) {
                auto hint = resolve(spreadHints[i]);
                if (!std::holds_alternative<TypeVar>(hint->kind) &&
                    !std::holds_alternative<UnknownType>(hint->kind)) {
                    pt = hint;
                } else {
                    pt = freshTypeVar();
                }
            } else {
                pt = freshTypeVar();
            }
            paramTypes.push_back(pt);
            if (lam->params[i].name != "_") {
                defineVar(lam->params[i].name, pt);
                // Recorded for tooling like any other binding. A LambdaParam
                // carries no location of its own, so the block's own position
                // is the anchor and the name is found from there — without
                // this, hovering `x` in `{ |x| … }` answered nothing while a
                // use of it answered fine.
                m_patternBindings.push_back(
                    {lam->params[i].name, blockExpr.location, pt});
            }
        }
        m_blockDepth++;
        TypePtr bodyType;
        if (lam->collection) {
            TypePtr element = freshTypeVar();
            bool haveElement = false;
            for (const auto& item : lam->body) {
                if (!item) continue;
                TypePtr itemType;
                if (const auto* spread =
                        std::get_if<ast::SpreadExpr>(&item->kind)) {
                    auto spreadType = resolve(inferExpr(*spread->inner));
                    if (auto* list = std::get_if<ListType>(&spreadType->kind))
                        itemType = list->element;
                    else {
                        error(item->location,
                              "Collection-block spread expects a list, got " +
                                  typeToString(spreadType));
                        itemType = Type::unknown();
                    }
                } else if (const auto* guarded =
                               std::get_if<ast::TrailingIf>(&item->kind)) {
                    auto condition = resolve(inferExpr(*guarded->condition));
                    if (!std::holds_alternative<UnknownType>(condition->kind) &&
                        !std::holds_alternative<TypeVar>(condition->kind) &&
                        !typesEqual(condition, Type::boolean()))
                        error(guarded->condition->location,
                              "Collection-block guard requires Bool, got " +
                                  typeToString(condition));
                    itemType = inferExpr(*guarded->expr);
                } else {
                    itemType = inferExpr(*item);
                }
                if (!haveElement) {
                    element = itemType;
                    haveElement = true;
                } else if (!argMatchesParam(itemType, element) &&
                           !argMatchesParam(element, itemType)) {
                    typeMismatch(item->location, resolve(element),
                                 resolve(itemType));
                }
            }
            bodyType = Type::list(resolve(element));
        } else {
            bodyType = inferBody(lam->body);
        }
        if (lam->rescue) checkRescue(*lam->rescue);
        m_blockDepth--;
        auto resultType = resolve(bodyType);
        if (lam->returnAnnotation) {
            auto declared = resolveTypeExpr(**lam->returnAnnotation, genericVars);
            if (!std::holds_alternative<UnknownType>(resultType->kind) &&
                !std::holds_alternative<TypeVar>(resultType->kind) &&
                !argMatchesParam(resultType, declared))
                typeMismatch(blockExpr.location, declared, resultType);
            resultType = declared;
        }
        popScope();
        // Outwardly a spreading block still takes the whole element first,
        // the shape argMatchesParam recognises as destructuring.
        if (spreadHints.size() != hintParams.size() && !lam->params[0].type)
            paramTypes[0] = hintParams[0];
        return Type::func(std::move(paramTypes), resultType);
    }
    // A bare `~f` passed where a function is expected: check it against the
    // hinted param types, so `words.filter(~even?)` reports the element-type
    // mismatch rather than inferring fresh vars that unify with anything.
    // Only plain unqualified captures — an operator or a `~Mod.fn` name isn't
    // resolvable through checkCall here, and partial applications already
    // carry their own arg types.
    if (auto* ce = std::get_if<ast::CurryExpr>(&blockExpr.kind);
        ce && ce->argGroups.empty() && !ce->isOperator && ce->module.empty()
        && !hintParams.empty()) {
        std::vector<TypePtr> paramTypes;
        for (const auto& h : hintParams) {
            auto pt = resolve(h);
            paramTypes.push_back(
                (std::holds_alternative<TypeVar>(pt->kind) ||
                 std::holds_alternative<UnknownType>(pt->kind))
                    ? freshTypeVar() : pt);
        }
        auto resultType = checkCall(ce->name, paramTypes, blockExpr.location,
                                    /*isMethodCall=*/false);
        return Type::func(std::move(paramTypes), resolve(resultType));
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

auto TypeChecker::inferExpr(const ast::Expr& expr) -> TypePtr {
    auto result = std::visit([this, &expr](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::IntLiteral>) {
            return Type::integer();
        }
        else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
            return Type::float64();
        }
        else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            for (const auto& value : node.values)
                if (value) inferExpr(*value);
            return Type::string();
        }
        else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
            return Type::boolean();
        }
        else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
            return Type::charT();
        }
        else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
            if (auto constructed = constructorResultType("None", {}))
                return constructed;
            return Type::named("None");
        }
        else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
            return Type::atom();
        }
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
            if (node.name == "from" && m_inCastSlot) {
                error(expr.location,
                      "`from` is not available in a cast — no caller is waiting for a reply");
                return Type::unknown();
            }
            auto type = lookupVar(node.name);
            // A local binding is never a zero-parameter function, even an
            // Unknown-typed one: `let root = match JSON.parse(...)` must not
            // take the type of some module's `let root -> String`.
            const bool boundLocally = std::any_of(
                m_scopeStack.begin(), m_scopeStack.end(),
                [&](const auto& scope) { return scope.get(node.name) != nullptr; });
            if (!boundLocally &&
                (!type || std::holds_alternative<UnknownType>(resolve(type)->kind))) {
                // `let ids -> [String] = [...]` is a zero-parameter function,
                // and naming it auto-calls it — so the name has the declared
                // result type. Left Unknown, `ids.reduce(...)` picked String's
                // `reduce` overload and typed each element as Char (#329).
                if (auto result = zeroArgBindingResult(node.name))
                    return result;
                if (auto result = functionValueType(node.name))
                    return result;
            }
            if (!type) {
                if (node.name == "new" && m_inMakeBlock && m_currentMakeType)
                    error(expr.location, "`new` requires a record receiver");
                return Type::unknown();
            }
            return type;
        }
        else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
            if (auto constructed = nullaryConstructorType(node.name)) return constructed;
            auto type = lookupVar(node.name);
            // A built-in type's name reaches lookupVar through the globals that
            // annotations resolve against: as a value it is still a type name.
            const bool builtinName = type && type == m_globals.get(node.name) &&
                                     builtinType(node.name) != nullptr;
            if (type && !builtinName) return type;
            // A bare TYPE NAME used as a value (`x.to(Integer)`, `let t =
            // String`) is a `Type<X>`: what a `Type<T>` parameter takes.
            if (namesKnownType(node.name)) {
                // A type named as a value is that type's `Type` record
                // (`Type<String>`), except `Void`, which is the unit value.
                if (node.name == kex::names::kVoidType) return Type::named(node.name);
                const auto named = typeNamed(node.name, expr.location);
                return Type::named(kex::names::kTypeRecord,
                                   {named ? named : Type::named(node.name)});
            }
            if (auto constant = constantType(node.name)) return constant;
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::WithExpr>) {
            std::string capability = node.capability.dotted();
            // Only a declared capability may be replaced. Without this, any
            // module is substitutable and the keyword means nothing, and a
            // typo binds nothing while the body still runs against the real
            // implementation — the silent-mock failure the feature exists to
            // remove (kexhq/kex#180).
            auto actual = node.value ? resolve(inferExpr(*node.value))
                                     : Type::unknown();
            if (!m_capabilities.count(capability)) {
                error(expr.location,
                      "'" + capability + "' is not a capability, so it cannot "
                      "be replaced by 'with' — declare it as "
                      "`capability " + capability + " do ... end`");
            } else if (!std::holds_alternative<UnknownType>(actual->kind) &&
                       !std::holds_alternative<TypeVar>(actual->kind) &&
                       !satisfiesTrait(actual, capability)) {
                // The replacement has to provide what the capability declares.
                error(expr.location,
                      "replacement for capability '" + capability +
                      "' does not implement it: " + typeToString(actual));
            }
            pushScope();
            auto bodyType = inferBody(node.body);
            popScope();
            return bodyType;
        }
        else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
            ImportSelection selection;
            selection.module += node.module.dotted();
            selection.onlyNames = node.onlyNames;
            selection.exceptNames = node.exceptNames;
            selection.alias = node.alias;
            if (node.body.empty()) {
                if (!m_importScopeStack.empty())
                    m_importScopeStack.back().push_back(std::move(selection));
                return Type::unit();
            }
            pushScope();
            m_importScopeStack.back().push_back(std::move(selection));
            auto bodyType = inferBody(node.body);
            popScope();
            return bodyType;
        }
        else if constexpr (std::is_same_v<T, ast::TryExpr>) {
            if (m_tryingBlockDepth >= 0 && m_blockDepth > m_tryingBlockDepth)
                m_tryBelowBlock++;
            auto operand = resolve(
                node.operand ? inferExpr(*node.operand) : Type::unknown());
            if (auto* optional = std::get_if<OptionalType>(&operand->kind))
                return optional->inner;
            if (auto* named = std::get_if<NamedType>(&operand->kind);
                named && !named->typeArgs.empty()) {
                if (named->name == "Optional" || named->name == "Result")
                    return named->typeArgs[0];
            }
            if (isUndetermined(operand))
                return Type::unknown();
            error(expr.location, "`.try` expects Result or Optional, got " +
                  typeToString(operand));
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
            auto outerTryingDepth = m_tryingBlockDepth;
            auto outerTryBelowBlock = m_tryBelowBlock;
            m_tryingBlockDepth = m_blockDepth;
            m_tryBelowBlock = 0;
            auto bodyType = inferBody(node.body);
            // A body that exits the function hands nothing to the `trying`
            // expression, so it must not be compared against the rescue arms.
            auto result = resolve(
                alwaysReturns(node.body) ? Type::voidType() : bodyType);
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
                    isUndetermined(result) ||
                    std::holds_alternative<VoidType>(result->kind);
                bool candidatePermissive =
                    isUndetermined(candidate) ||
                    std::holds_alternative<VoidType>(candidate->kind);
                if (resultPermissive && !candidatePermissive) {
                    result = candidate;
                } else if (!resultPermissive && !candidatePermissive &&
                           !rescueUnreachable &&
                           !argMatchesParam(candidate, result) &&
                           !argMatchesParam(result, candidate)) {
                    error(expr.location,
                          "`trying` body returns " + typeToString(result) +
                          " but rescue returns " + typeToString(candidate));
                }
            };
            checkRescue(node.rescue, merge);
            return result;
        }
        else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            TypePtr branchExpectation;
            if (node.type && *node.type && node.value &&
                (std::holds_alternative<ast::IfExpr>(node.value->kind) ||
                 std::holds_alternative<ast::MatchExpr>(node.value->kind) ||
                 std::holds_alternative<ast::ThenElseExpr>(node.value->kind))) {
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
            const auto outerExpectation =
                std::exchange(m_branchExpectation, branchExpectation);
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            m_branchExpectation = outerExpectation;
            // A local `let UPPER = ...` binds the body's value, not the
            // zero-arg function the parser wraps it in (kexhq/kex#603):
            // unwrap here so the annotation check and the binding below
            // both see the value's type.
            if (ast::isLocalConstantBind(node.pattern, node.value)) {
                auto resolvedValue = resolve(valueType);
                if (auto* func = std::get_if<FuncType>(&resolvedValue->kind))
                    if (func->params.empty()) valueType = func->result;
            }
            // `let xs: [Colour] = []` — the annotation is what the binding IS;
            // the initializer only has to be compatible with it. Binding the
            // inferred type instead would defeat the reason for writing one.
            auto declared = declaredBindingType(node.type, valueType, expr.location,
                                                node.value ? node.value.get() : nullptr);
            if (declared && node.value) checkByteLiterals(*node.value, declared);
            if (auto* varPat = std::get_if<ast::VarPattern>(&node.pattern->kind)) {
                // `x : Type.of(y)` on the line above `let x = 34`: a top-level
                // binding is a synthetic-main LetExpr, not a 0-arg function, so
                // its declaration has to be matched here. Scoped to the
                // synthetic main, or a local `let` would pick up a global
                // annotation that merely shares its name.
                if (!declared && m_inSyntheticMain)
                    declared = declaredConstantType(varPat->name, valueType,
                                                    expr.location);
                defineVar(varPat->name, declared ? declared : valueType);
                // Record `f`'s origin when bound to a bare `~greet` capture
                // (no partial application, no operator, no module prefix) so
                // a later named-argument call through `f` can find `greet`'s
                // real parameter names (kexhq/kex#309). Any other kind of
                // binding for this name clears a stale entry.
                if (const auto* curry = node.value
                        ? std::get_if<ast::CurryExpr>(&node.value->kind)
                        : nullptr;
                    curry && curry->argGroups.empty() && !curry->isOperator &&
                    curry->module.empty())
                    m_curryOrigin[varPat->name] = curry->name;
                else
                    m_curryOrigin.erase(varPat->name);
            } else if (node.pattern) {
                bindDestructuredPattern(*node.pattern, valueType);
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            auto declared = declaredBindingType(node.type, valueType, expr.location,
                                                node.value ? node.value.get() : nullptr);
            if (auto* varPat = node.pattern
                    ? std::get_if<ast::VarPattern>(&node.pattern->kind)
                    : nullptr) {
                defineVar(varPat->name, declared ? declared : valueType);
                // Same origin tracking as `let`, so a named-argument call
                // through `var f = ~greet` finds `greet`'s real parameter
                // names (kexhq/kex#309).
                if (const auto* curry = node.value
                        ? std::get_if<ast::CurryExpr>(&node.value->kind)
                        : nullptr;
                    curry && curry->argGroups.empty() && !curry->isOperator &&
                    curry->module.empty())
                    m_curryOrigin[varPat->name] = curry->name;
                else
                    m_curryOrigin.erase(varPat->name);
            } else if (node.pattern) {
                bindDestructuredPattern(*node.pattern, valueType);
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            auto varType = lookupVar(node.name);
            if (varType) m_assignmentTargetTypes[&expr] = varType;
            if (!node.path.empty()) {
                if (node.path.size() != 1) {
                    error(expr.location,
                          "nested record-field assignment is not supported yet");
                    return Type::unit();
                }
                auto resolvedVar = varType ? resolve(varType) : Type::unknown();
                auto* named = std::get_if<NamedType>(&resolvedVar->kind);
                auto record = named
                    ? m_recordFields.find(resolveRecordName(named->name))
                    : m_recordFields.end();
                if (record == m_recordFields.end()) {
                    error(expr.location, "field assignment requires a record binding");
                    return Type::unit();
                }
                auto field = record->second.find(node.path.front());
                if (field == record->second.end()) {
                    error(expr.location, "record `" + named->name +
                                             "` has no field `" +
                                             node.path.front() + "`");
                } else if (!containsOpenType(valueType) &&
                           !containsOpenType(field->second) &&
                           !argMatchesParam(valueType, field->second) &&
                           !(node.value && literalCoercesTo(*node.value, field->second))) {
                    typeMismatch(expr.location, field->second, valueType);
                }
                return Type::unit();
            }
            if (varType && !std::holds_alternative<UnknownType>(varType->kind) &&
                !std::holds_alternative<TypeVar>(varType->kind)) {
                if (!argMatchesParam(valueType, varType) &&
                    !(node.value && literalCoercesTo(*node.value, varType)) &&
                    !std::holds_alternative<UnknownType>(valueType->kind) &&
                    !std::holds_alternative<TypeVar>(valueType->kind)) {
                    typeMismatch(expr.location, varType, valueType);
                }
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            auto leftType = node.left ? inferExpr(*node.left) : Type::unknown();
            auto rightType = node.right ? inferExpr(*node.right) : Type::unknown();
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
                            outcome = checkCall(symbol, {left, right}, location,
                                                /*isMethodCall=*/true);
                        } else if (!lacksBuiltinOperator(left, symbol) &&
                                   !lacksBuiltinOperator(resolve(right), symbol)) {
                            outcome = inferBinaryOp(op, left, right, location);
                        } else {
                            error(location, "`" + symbol + "` is not defined for " +
                                                typeToString(left) + " and " +
                                                typeToString(resolve(right)) +
                                                (lacksBuiltinOperator(left, symbol)
                                                     ? "; declare it in a `make " +
                                                           typeToString(left) + "` block"
                                                     : std::string{}));
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
                    m_traits.satisfies(resolve(rightType), "Number"))
                    return inferBinaryOp(node.op, leftType, rightType,
                                         expr.location);
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
                    // The overloads of `name` the receiver's own type
                    // declares — a make block for it, not a generic one.
                    auto declaredFor = [&](const std::string& name) {
                        std::vector<Signature> owned;
                        auto consider = [&](const Signature& signature) {
                            if (signature.params.size() != size_t(2)) return;
                            const auto receiverParam = resolve(signature.params[0]);
                            if (std::holds_alternative<TypeVar>(receiverParam->kind) ||
                                std::holds_alternative<UnknownType>(receiverParam->kind))
                                return;
                            if (argMatchesParam(leftType, signature.params[0]))
                                owned.push_back(signature);
                        };
                        if (auto local = m_methodSignatures.find(name);
                            local != m_methodSignatures.end())
                            for (const auto& signature : local->second)
                                consider(signature);
                        for (const auto& signature : importedCandidateSignatures(name))
                            consider(signature);
                        return owned;
                    };
                    auto applies = [&](const Signature& signature) {
                        return argMatchesParam(rightType, signature.params[1]);
                    };
                    auto owned = declaredFor(operatorName);
                    ownsOperator = !owned.empty();
                    if (std::any_of(owned.begin(), owned.end(), applies)) {
                        m_decidedCalls[&expr] = {
                            CallKind::Operator, typeToString(resolve(leftType))};
                        return checkCall(operatorName, {leftType, rightType},
                                         expr.location, /*isMethodCall=*/true);
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
                            const bool undecided =
                                isUndetermined(equal);
                            if (!undecided && !typesEqual(equal, Type::boolean()))
                                error(expr.location,
                                          "`!=` is not defined here: `" +
                                          typeToString(resolve(leftType)) +
                                          "`'s `==` answers " + typeToString(equal) +
                                          ", not Bool, so there is no inequality "
                                          "to derive — declare `!=` beside it");
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
                        const bool equality =
                            operatorName == "==" || operatorName == "!=";
                        const bool sameType =
                            argMatchesParam(rightType, leftType) &&
                            argMatchesParam(leftType, rightType);
                        if (!(equality && sameType))
                            return checkCall(contractName, {leftType, rightType},
                                             expr.location, /*isMethodCall=*/true);
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
                const bool arithmeticOrOrder =
                    node.op == TokenType::Plus || node.op == TokenType::Minus ||
                    node.op == TokenType::Star || node.op == TokenType::Slash ||
                    node.op == TokenType::Percent || node.op == TokenType::Caret ||
                    node.op == TokenType::LessThan ||
                    node.op == TokenType::GreaterThan ||
                    node.op == TokenType::LessEq ||
                    node.op == TokenType::GreaterEq;
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
                    !std::holds_alternative<UnknownType>(receiver->kind) &&
                    !inferredBound;
                // A built-in `+` on the left already says when the right side
                // does not match ("requires matching types"); the others do
                // not check it, so `2 * point` needs saying here.
                const bool builtinPlusReports =
                    node.op == TokenType::Plus &&
                    !lacksBuiltinOperator(receiver, operatorName);
                if (arithmeticOrOrder && knownReceiver && !ownsOperator &&
                    !builtinPlusReports &&
                    !traitDeclaresOperator(receiver, operatorName) &&
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
                    error(expr.location,
                          "`" + operatorName + "` is not defined for " +
                              owner + " and " + typeToString(right) +
                              (!nominal ? std::string{}
                               : traitOwner ? "; declare it in trait `" + owner + "`"
                                            : "; declare it in a `make " + owner +
                                                  "` block"));
                    return Type::unknown();
                }
            }
            return inferBinaryOp(node.op, leftType, rightType, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            auto operandType = node.operand ? inferExpr(*node.operand) : Type::unknown();
            auto resolved = resolve(operandType);
            if (node.op == TokenType::Bang) {
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "Logical not '!' requires Bool, got " +
                          typeToString(resolved));
                }
                return Type::boolean();
            }
            if (node.op == TokenType::Minus) {
                // Unary negation requires a numeric operand.
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::constrained("N", "Number"));
                } else if (lacksBuiltinOperator(resolved, "-")) {
                    // Negation is numbers only, on both backends: a record or ADT
                    // reached "Cannot negate" on the walker and `badarith` on
                    // BEAM, with nothing said while checking.
                    error(expr.location, "Unary '-' requires a number, got " +
                          typeToString(resolved));
                    return Type::unknown();
                }
                return resolved;
            }
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            std::vector<TypePtr> argTypes;
            // First pass: infer concrete args; defer lambdas with parameters
            // until the selected signature can provide contextual types.
            std::vector<std::pair<size_t, size_t>> contextualLambdas;
            for (size_t i = 0; i < node.args.size(); i++) {
                const auto* lambda = node.args[i]
                    ? std::get_if<ast::Lambda>(&node.args[i]->kind) : nullptr;
                if (node.args[i] &&
                    (std::holds_alternative<ast::ShorthandLambda>(
                         node.args[i]->kind) ||
                     isBareCapture(node.args[i].get()) ||
                     (lambda && !lambda->params.empty()))) {
                    argTypes.push_back(Type::unknown()); // placeholder
                    contextualLambdas.emplace_back(i, i);
                } else {
                    argTypes.push_back(node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                }
            }
            // A block passed by label is typed from that parameter too, as a
            // positional one is below. Inferred on its own, its parameter had
            // no type, so `t.text(:c)` was left to run-time dispatch, which
            // could not find `text` (kexhq/kex#499).
            std::vector<size_t> labelledLambdas;
            for (size_t i = 0; i < node.namedArgs.size(); ++i) {
                const auto& arg = node.namedArgs[i].second;
                const auto* lambda =
                    arg ? std::get_if<ast::Lambda>(&arg->kind) : nullptr;
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
                auto hints = resolveBlockHints(node.name, argTypes, false,
                                               &collection, &skippedTo);
                if (collection)
                    if (auto* lambda = std::get_if<ast::Lambda>(
                            &(**node.block).kind))
                        lambda->collection = true;
                argTypes.push_back(inferBlock(**node.block, hints));
                passBlockByLabel(node, skippedTo);
            }
            if (auto constructed =
                    constructorResultType(node.name, argTypes)) {
                checkConstructorPayloads(node.name, argTypes, expr.location);
                const auto* adt =
                    std::get_if<NamedType>(&resolve(constructed)->kind);
                m_decidedCalls[&expr] = {CallKind::Constructor,
                                         adt ? adt->name : node.name};
                return constructed;
            }
            // An imported constructor a `using` brings in (`ReadFailed(path)`
            // under `using FS`): checked as its module's own spelling is.
            if (m_importedInterfaces && !lookupVar(node.name) &&
                !node.name.empty() && std::isupper(static_cast<unsigned char>(node.name.front())))
                for (const auto& [path, module] : m_importedInterfaces->modules) {
                    const auto exports = module.exports.find(node.name);
                    if (exports == module.exports.end()) continue;
                    const bool visible = std::any_of(
                        exports->second.begin(), exports->second.end(),
                        [&](const ImportedFunction& function) {
                            return function.isConstructor &&
                                   function.signature.params.size() == argTypes.size() &&
                                   importedFunctionVisible(function);
                        });
                    if (!visible) continue;
                    auto result = checkCall(kex::mangle::qualify(path, node.name), argTypes,
                                            expr.location, false, nullptr, &expr);
                    const auto* adt = std::get_if<NamedType>(&resolve(result)->kind);
                    m_decidedCalls[&expr] = {CallKind::Constructor,
                                             adt ? adt->name : node.name};
                    return result;
                }
            if (auto method = bareMethodCall(expr, node, argTypes)) return method;
            return checkCall(node.name, argTypes, expr.location,
                             false, nullptr, &expr);
        }
        else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
            for (const auto& value : node.values)
                if (value) inferExpr(*value);
            return checkCall(
                node.tag,
                {Type::list(Type::string()), Type::list(Type::unknown())},
                expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            // Local module constructors use the same namespace syntax as
            // module constants. Imported constructors are ordinary typed
            // module exports and continue through checkCall below.
            if (node.args.empty() && node.namedArgs.empty() && !node.block &&
                node.receiver) {
                std::function<std::optional<std::string>(const ast::Expr&)>
                    localModulePath;
                localModulePath = [&](const ast::Expr& receiver)
                    -> std::optional<std::string> {
                    if (const auto* root =
                            std::get_if<ast::UpperIdentifier>(
                                &receiver.kind))
                        return root->name;
                    const auto* segment =
                        std::get_if<ast::MethodCall>(&receiver.kind);
                    if (!segment || !segment->receiver ||
                        !segment->args.empty() ||
                        !segment->namedArgs.empty() || segment->block)
                        return std::nullopt;
                    auto parent = localModulePath(*segment->receiver);
                    return parent
                        ? std::optional<std::string>{
                              *parent + "." + segment->method}
                        : std::nullopt;
                };
                if (auto path = localModulePath(*node.receiver)) {
                    if (auto module = m_moduleConstructors.find(*path);
                        module != m_moduleConstructors.end())
                        if (auto constructor =
                                module->second.find(node.method);
                            constructor != module->second.end()) {
                            if (!constructor->second.isPublic &&
                                m_currentModulePath != *path)
                                error(expr.location,
                                      "cannot access private name `" +
                                          node.method + "` from " + *path);
                            m_decidedCalls[&expr] = {
                                CallKind::Constructor,
                                constructor->second.typeName.empty()
                                    ? node.method
                                    : constructor->second.typeName};
                            // A constructor reached through its full module path
                            // retains its defining ADT even when the current file
                            // did not import that ADT's bare name. This matters
                            // for reified constants containing nested records.
                            if (!moduleMemberImported(*path, node.method) &&
                                !withinModule(*path) &&
                                !constructor->second.typeName.empty())
                                return Type::named(constructor->second.typeName);
                            return Type::named(node.method);
                        }
                }
            }

            // Backend interop and the private intrinsic ABI are untyped until
            // the intrinsic declaration interface supplies their signatures.
            // Never resolve these against same-named public stdlib functions.
            {
                const ast::Expr* r = node.receiver ? &*node.receiver : nullptr;
                bool intrinsicSegment = false;
                // The path's segments, innermost first (`lists` of
                // `BEAM.lists.reverse`).
                std::vector<const ast::Expr*> segments;
                while (r) {
                    if (auto* mc = std::get_if<ast::MethodCall>(&r->kind)) {
                        // A segment with arguments is a call, and the path ends
                        // there: `.map` on `Kex.Intrinsic.Type.fieldLayoutsOf(n)`
                        // is a call on its result, not a path member.
                        if (!mc->args.empty() || !mc->namedArgs.empty() || mc->block) break;
                        segments.push_back(r);
                        intrinsicSegment = intrinsicSegment || mc->method == "Intrinsic";
                        r = mc->receiver ? &*mc->receiver : nullptr;
                    } else break;
                }
                if (r) {
                    if (auto* uid = std::get_if<ast::UpperIdentifier>(&r->kind)) {
                        // The interop roots before `BEAM.` (kexhq/kex#346).
                        // Unchecked, a leftover `Erlang.lists.reverse` passed
                        // the checker and died at runtime instead.
                        const bool retiredRoot =
                            (uid->name == "Erlang" || uid->name == "Elixir" ||
                             uid->name == "Gleam") &&
                            !m_localModules.count(uid->name) &&
                            !m_importedModulePaths.count(uid->name);
                        if (retiredRoot)
                            error(r->location,
                                  "`" + uid->name + ".` interop is now `BEAM.`, "
                                  "followed by the module's real name: write "
                                  "`BEAM.lists.reverse(xs)`, `BEAM.erlang.node()`" +
                                  std::string(uid->name == "Elixir"
                                                  ? ", `BEAM.Elixir.Enum.map(xs, f)`"
                                                  : ""));
                        if (uid->name == "BEAM" || retiredRoot ||
                            (uid->name == "Kex" && intrinsicSegment)) {
                            for (const auto& a : node.args)
                                if (a) inferExpr(*a);
                            for (const auto& [_, a] : node.namedArgs)
                                if (a) inferExpr(*a);
                            if (node.block) inferExpr(**node.block);
                            // Decided by its path: an Erlang function, or a
                            // runtime primitive; each segment names a module.
                            std::string path = uid->name;
                            for (auto segment = segments.rbegin(); segment != segments.rend();
                                 ++segment) {
                                path += "." + std::get<ast::MethodCall>((*segment)->kind).method;
                                m_decidedCalls[*segment] = {CallKind::ModuleReference, path};
                            }
                            m_decidedCalls[&expr] = {
                                uid->name == "Kex" ? CallKind::Builtin : CallKind::Foreign,
                                path + "." + node.method};
                            return Type::unknown();
                        }
                    }
                }
            }
            // `Process.spawn(Entries)` — the record's NAME, not a value of it.
            // A bare type name is typed as the type it names (that is what
            // lets `x.to(Integer)` dispatch), so this checked as a clean
            // `Server<Entries>` and the server then started with the atom
            // `Entries` as its state, dying on the first `@field`. Spawning
            // takes the initial state, so say how to write one.
            if (node.method == "spawn" && node.receiver && node.args.size() == 1 &&
                node.args.front()) {
                auto* ns = std::get_if<ast::UpperIdentifier>(&node.receiver->kind);
                auto* arg = std::get_if<ast::UpperIdentifier>(&node.args.front()->kind);
                if (ns && ns->name == "Process" && arg &&
                    !lookupVar(arg->name) &&
                    !m_nullaryConstructors.contains(arg->name) &&
                    m_recordFields.count(resolveRecordName(arg->name)))
                    error(node.args.front()->location,
                          "`Process.spawn(" + arg->name + ")` passes the type, "
                          "not a value; spawn a server with its initial state: "
                          "`Process.spawn(" + arg->name + " {})`");
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
            auto importedPath = node.receiver
                ? importedModulePath(*node.receiver) : std::nullopt;
            // The path exactly as written, before a `using` expands it.
            const auto writtenPath = importedPath;
            // A bare receiver segment (`Set.from(…)`) may name a module only
            // under its qualified identity (`Data.Set`) once a `using`
            // brought it into scope unqualified — expand it before the
            // dependency/namespace checks below, which key everything off
            // `*importedPath` (kexhq/kex#229).
            if (importedPath)
                importedPath = resolveModulePath(*importedPath, node.method);
            // A module re-exported under another's path (`export Numbers` in
            // `module Toolkit`) is that module: `Toolkit.Numbers.double`.
            if (importedPath)
                if (const auto reexport = m_moduleReexports.find(*importedPath);
                    reexport != m_moduleReexports.end())
                    importedPath = reexport->second;
            // Each segment of that path, as written, that names a module this
            // unit knows: a local one, an imported one, or one the compiled
            // interfaces describe. Anything else there is a constant or a
            // field (`Credentials.SHORTEST`, `Kex.VERSION.number`).
            const auto namesModule = [&](const std::string& path) {
                return m_localModules.contains(path) ||
                       m_moduleReexports.contains(path) ||
                       m_importedModulePaths.contains(path) ||
                       (m_importedInterfaces &&
                        (m_importedInterfaces->modules.contains(path) ||
                         m_importedInterfaces->modules.contains(names::moduleAtom(path))));
            };
            // A record or sum type used as a namespace (`TCP.Endpoint.loopback`).
            const auto namesType = [&](const std::string& path) {
                const auto canonical = resolveRecordName(path);
                return m_recordFields.contains(canonical) ||
                       m_adtVariants.contains(path) ||
                       (m_importedInterfaces &&
                        (m_importedInterfaces->recordFieldNames.contains(path) ||
                         m_importedInterfaces->recordFieldNames.contains(canonical)));
            };
            if (writtenPath) {
                auto path = *writtenPath;
                for (const ast::Expr* segment = node.receiver.get(); segment;) {
                    const auto* call = std::get_if<ast::MethodCall>(&segment->kind);
                    if (!call) break;
                    if (namesModule(path))
                        m_decidedCalls[segment] = {CallKind::ModuleReference, path};
                    else if (namesType(path))
                        m_decidedCalls[segment] = {CallKind::TypeReference, path};
                    const auto dot = path.rfind('.');
                    path.resize(dot == std::string::npos ? 0 : dot);
                    segment = call->receiver.get();
                }
            }
            // A qualified type named as a value (`App.Models.User`, the argument
            // of `select(as: Type<T>)`) is a `Type<X>`, as a bare one is.
            if (writtenPath && node.args.empty() && node.namedArgs.empty() && !node.block &&
                !node.method.empty() &&
                std::isupper(static_cast<unsigned char>(node.method.front())))
                if (const auto full = *writtenPath + "." + node.method; namesType(full)) {
                    m_decidedCalls[&expr] = {CallKind::TypeReference, full};
                    return Type::named(kex::names::kTypeRecord,
                                       {Type::named(resolveRecordName(full))});
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
                        if (dot == std::string::npos) break;
                        candidate.resize(dot);
                    }
                    if (!interfaceModule.empty())
                        dependencyPath = std::move(interfaceModule);
                }
                // A capitalized segment may be a constant rather than a
                // module: `"${App.Forms.Credentials.SHORTEST}"`, whose `show`
                // has the whole path as its receiver.
                const auto last = dependencyPath.substr(dependencyPath.rfind('.') + 1);
                if (std::none_of(last.begin(), last.end(),
                                 [](unsigned char c) { return std::islower(c); }))
                    recordWithOwner(m_referencedModules, dependencyPath);
                else
                    m_referencedModules.insert(std::move(dependencyPath));
            }
            bool isImportedNamespace = importedPath && m_importedInterfaces &&
                m_importedInterfaces->modules.count(*importedPath) > 0;
            bool isNamespaceCall = node.receiver &&
                (isNamespaceReceiver(*node.receiver) || isImportedNamespace);
            // A LOCAL nested module is qualified the same way: without this
            // `CollisionWeb.Server.build(7000)` checked as the bare name
            // `build` and picked the prelude's `Web.Server.build` — the two
            // are indistinguishable there, both `Integer -> Server`, and the
            // imported one is listed first. Only when signatures really were
            // published under that key, so value chains through a static
            // constructor (`Temperature.Fahrenheit(212)`) stay untouched.
            const bool isLocalNamespace = importedPath &&
                m_localModules.contains(*importedPath) &&
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
                if (auto problem = namespaceCallProblem(*writtenPath, *importedPath,
                                                        node.method)) {
                    error(node.receiver->location, *problem);
                    for (const auto& a : node.args)
                        if (a) inferExpr(*a);
                    for (const auto& [_, a] : node.namedArgs)
                        if (a) inferExpr(*a);
                    if (node.block) inferExpr(**node.block);
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
                if (!writtenPath || *writtenPath != *importedPath) return false;
                const auto dot = writtenPath->rfind('.');
                const auto last = dot == std::string::npos
                    ? *writtenPath : writtenPath->substr(dot + 1);
                for (const auto& local : m_localModules)
                    if (local == last || local.ends_with("." + last)) return false;
                return true;
            };
            if (isImportedNamespace && !isLocalNamespace && unambiguousImport()) {
                const auto& module = m_importedInterfaces->modules.at(*importedPath);
                // Only a module's OWN interface is complete. A `using` also
                // files a partial copy under the bare name (`Router` for
                // `Net.HTTP.Router`, which lacks the module's constants), and
                // that copy cannot say what the module does not have.
                // `Kex.embed` is expanded before checking (compiled/expand.cxx);
                // one still here failed to expand, and says why itself.
                const bool compileTimeForm =
                    *importedPath == "Kex" && node.method == "embed";
                if (module.sourceModule == *importedPath && !compileTimeForm &&
                    !module.exports.count(node.method)) {
                    std::vector<std::string> exported;
                    for (const auto& [name, _] : module.exports) exported.push_back(name);
                    std::sort(exported.begin(), exported.end());
                    auto message = "`" + *importedPath + "` has no function `" +
                                   node.method + "`";
                    if (auto hint = closestName(node.method, exported); !hint.empty())
                        message += " — did you mean `" + hint + "`?";
                    error(expr.location, message);
                    for (const auto& a : node.args)
                        if (a) inferExpr(*a);
                    for (const auto& [_, a] : node.namedArgs)
                        if (a) inferExpr(*a);
                    if (node.block) inferExpr(**node.block);
                    return Type::unknown();
                }
            }
            // A type's `make` method called through the type's name
            // (`Parser.parse(text)`) takes the type as its receiver, as both
            // backends run it: an ordinary method call, not a namespace one.
            bool throughTypeName = false;
            if (isNamespaceCall && !isImportedNamespace && !isLocalNamespace &&
                std::holds_alternative<ast::UpperIdentifier>(node.receiver->kind)) {
                const auto& written = std::get<ast::UpperIdentifier>(node.receiver->kind).name;
                // A constant or a nullary constructor is a value, whatever its
                // spelling: `LIMIT.abs`, `Monday.name`, `STATUSES.map`.
                if (!namesModule(written) && !namesType(written) &&
                    (constantType(written) || nullaryConstructorType(written)))
                    isNamespaceCall = false;
                const auto canonical = resolveRecordName(written);
                const auto methods = m_methodSignatures.find(node.method);
                if (!namesModule(written) && namesType(written) &&
                    !m_userSignatures.count(kex::mangle::qualify(written, node.method)) &&
                    methods != m_methodSignatures.end() &&
                    std::any_of(methods->second.begin(), methods->second.end(),
                                [&](const Signature& method) {
                                    if (method.params.empty()) return false;
                                    const auto* receiver = std::get_if<NamedType>(
                                        &resolve(method.params.front())->kind);
                                    return receiver && resolveRecordName(receiver->name) == canonical;
                                }))
                {
                    isNamespaceCall = false;
                    throughTypeName = true;
                }
            }
            bool structuralAlias = false;
            if (isImportedNamespace || isLocalNamespace) {
                callName = kex::mangle::qualify(*importedPath, node.method);
            } else if (isNamespaceCall &&
                std::holds_alternative<ast::UpperIdentifier>(node.receiver->kind)) {
                const auto& written = std::get<ast::UpperIdentifier>(node.receiver->kind).name;
                // A structural alias names no namespace of its own: `List.first(xs)`
                // (`type List<X> = [X]`) is `xs.first`, as both backends run it.
                structuralAlias = !namesModule(written) &&
                    !m_userSignatures.count(kex::mangle::qualify(written, node.method)) &&
                    (m_typeAliases.count(written) ||
                     (m_importedInterfaces && m_importedInterfaces->typeAliases.count(written)));
                callName = structuralAlias ? node.method
                                           : kex::mangle::qualify(written, node.method);
            }
            // A compile-time intrinsic (intrinsics.hxx): its entry types the
            // call when the arguments are no ordinary values, and folds it
            // once they are typed. The one place a callee name is consulted.
            const auto* intrinsic =
                findCompileTimeIntrinsic(callName, node.args.size());
            // A call folded to a compile-time answer is decided by it.
            const auto foldedHere = [&] {
                if (m_staticTypeOfCalls.contains(&node))
                    m_decidedCalls.try_emplace(&expr, DecidedCall{CallKind::Intrinsic, callName});
            };
            if (intrinsic && intrinsic->check)
                if (auto checked = (this->*intrinsic->check)(expr, node)) {
                    foldedHere();
                    return checked;
                }

            std::vector<TypePtr> argTypes;
            if (!isNamespaceCall) {
                auto receiver = node.receiver ? inferExpr(*node.receiver) : Type::unknown();
                // Through the type's name the receiver is the type, not its
                // `Type` record: `Response.ok(body)` is a Response method.
                if (const auto* named = std::get_if<NamedType>(&receiver->kind);
                    throughTypeName && named && named->name == kex::names::kTypeRecord &&
                    named->typeArgs.size() == 1)
                {
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
                const auto* lambda = node.args[i]
                    ? std::get_if<ast::Lambda>(&node.args[i]->kind) : nullptr;
                if (node.args[i] &&
                    (std::holds_alternative<ast::ShorthandLambda>(
                         node.args[i]->kind) ||
                     isBareCapture(node.args[i].get()) ||
                     (lambda && !lambda->params.empty()))) {
                    argTypes.push_back(Type::unknown()); // placeholder
                    contextualLambdas.emplace_back(
                        isNamespaceCall ? i : 1 + i, i);
                } else {
                    argTypes.push_back(node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                }
            }
            const bool serverSlotCall = [&] {
                if (isNamespaceCall || argTypes.empty() ||
                    !m_slotMethodNames.count(node.method))
                    return false;
                auto receiver = resolve(argTypes.front());
                auto* named = std::get_if<NamedType>(&receiver->kind);
                return named && named->name == "Server" &&
                       named->typeArgs.size() == 1;
            }();
            for (const auto& [name, arg] : node.namedArgs) {
                const auto* collecting =
                    arg ? std::get_if<ast::Lambda>(&arg->kind) : nullptr;
                auto type = !arg ? Type::unknown()
                    : collecting && collecting->collection
                        ? inferBlock(*arg, namedArgHints(callName, name))
                        : inferExpr(*arg);
                if (serverSlotCall && name == "within") {
                    auto resolved = resolve(type);
                    const bool infinity = [&] {
                        auto* atom = std::get_if<PrimitiveType>(&resolved->kind);
                        return atom && atom->kind == PrimitiveType::Atom &&
                               atom->atomName == "infinity";
                    }();
                    if (!infinity && !containsOpenType(resolved) &&
                        !typesEqual(resolved, Type::integer()))
                        error(arg ? arg->location : expr.location,
                              "`within:` expects Integer milliseconds or :infinity, got " +
                                  typeToString(resolved));
                    continue;
                }
                argTypes.push_back(std::move(type));
            }
            // Second pass: infer lambdas with parameter hints from the signature.
            for (const auto& [argIdx, rawIdx] : contextualLambdas) {
                auto hints = resolveArgHints(
                    callName, argTypes, argIdx, /*isMethodCall=*/!isNamespaceCall);
                argTypes[argIdx] = inferBlock(*node.args[rawIdx], hints);
            }
            if (node.block) {
                bool collection = false;
                std::string skippedTo;
                auto hints = resolveBlockHints(
                    callName, argTypes, /*isMethodCall=*/!isNamespaceCall,
                    &collection, &skippedTo);
                if (collection)
                    if (auto* lambda = std::get_if<ast::Lambda>(
                            &(**node.block).kind))
                        lambda->collection = true;
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
                    receiverNamed && receiverNamed->name == "Result";
                const auto* fallbackNamed = std::get_if<NamedType>(&fallback->kind);
                // A bare `Just(8080)` / `Error("bad")` is typed gradually, so
                // its spelling is the only thing that says it is wrapped.
                const auto wrappingConstructor = [](const std::string& name) {
                    return name == "Just" || name == "Ok" || name == "Error";
                };
                // `r.or { … }` passes its fallback as the trailing block, so
                // `node.args` is empty (kexhq/kex#558).
                const ast::Expr* argument = !node.args.empty()
                    ? node.args.back().get()
                    : (node.block ? node.block->get() : nullptr);
                bool constructedWrapped = false;
                if (argument) {
                    if (const auto* call = std::get_if<ast::FunctionCall>(&argument->kind))
                        constructedWrapped = wrappingConstructor(call->name);
                    else if (const auto* method = std::get_if<ast::MethodCall>(&argument->kind);
                             method && !method->receiver)
                        constructedWrapped = wrappingConstructor(method->method);
                }
                const bool wrappedFallback = constructedWrapped ||
                    std::holds_alternative<OptionalType>(fallback->kind) ||
                    (fallbackNamed && (fallbackNamed->name == "None" ||
                                       fallbackNamed->name == "Result"));
                // `or` never calls its fallback, so `r.or { |_| … }` answered
                // the block itself in place of the value (kexhq/kex#558; STYLE.md
                // `or-function-default`). A function default is still fine
                // when the value is one.
                // A bare `~f` is typed only in the deferred, hinted pass, so
                // its type is not known yet here; its signatures say whether
                // it captures a function (a parameterless one is its value).
                const auto capturesFunction = [&](const ast::Expr* e) {
                    if (!isBareCapture(e)) return false;
                    const auto& name = std::get<ast::CurryExpr>(e->kind).name;
                    auto signatures = importedCandidateSignatures(name);
                    if (auto user = m_userSignatures.find(name);
                        user != m_userSignatures.end())
                        signatures = user->second;
                    return std::any_of(signatures.begin(), signatures.end(),
                                       [](const Signature& signature) {
                                           return !signature.params.empty();
                                       });
                };
                const bool functionFallback = argument &&
                    ((node.args.empty() && node.block) ||
                     std::holds_alternative<FuncType>(fallback->kind) ||
                     capturesFunction(argument));
                if (optionalReceiver || resultReceiver) {
                    const auto unwrapped = resolve(optionalReceiver
                        ? std::get<OptionalType>(receiver->kind).inner
                        : (receiverNamed->typeArgs.empty()
                               ? Type::unknown()
                               : receiverNamed->typeArgs.front()));
                    const auto unwrappedName = typeToString(unwrapped);
                    const bool vowel = !unwrappedName.empty() &&
                        std::string("AEIOUaeiou").find(unwrappedName.front()) !=
                            std::string::npos;
                    const bool openUnwrapped = containsOpenType(unwrapped);
                    const auto wanted = openUnwrapped
                        ? std::string("the unwrapped value")
                        : (vowel ? "an " : "a ") + unwrappedName;
                    const std::string wrapper = optionalReceiver ? "optional" : "result";
                    if (wrappedFallback) {
                        error(argument ? argument->location : expr.location,
                              "`or` needs " + wanted + " to fall back to, but got " +
                                  (constructedWrapped
                                       ? "`" + std::string(
                                             std::get_if<ast::FunctionCall>(&argument->kind)
                                                 ? std::get<ast::FunctionCall>(argument->kind).name
                                                 : std::get<ast::MethodCall>(argument->kind).method) +
                                             "(…)`"
                                       // A bare `None` is typed `T?` now
                                       // that constructors carry their ADT;
                                       // its spelling is what the reader
                                       // wrote.
                                       : argument && (std::holds_alternative<ast::NoneLiteral>(argument->kind) ||
                                                      (std::holds_alternative<ast::UpperIdentifier>(argument->kind) &&
                                                       std::get<ast::UpperIdentifier>(argument->kind).name == "None"))
                                           ? std::string("None")
                                           : typeToString(fallback)) +
                                  " — `or` is what leaves the " + wrapper +
                                  "; pass a plain default, keep the value as it is, "
                                  "or `match` on it");
                    } else if (functionFallback && !openUnwrapped &&
                               !std::holds_alternative<UnknownType>(unwrapped->kind) &&
                               !std::holds_alternative<FuncType>(unwrapped->kind)) {
                        const std::string given = node.args.empty() ? "block" : "function";
                        error(argument->location,
                              "`or` needs " + wanted + " to fall back to, but got a " +
                                  given + " — `or` never calls it, so the " + given +
                                  " itself would be the answer; pass a plain default, "
                                  "or `match` on the " + wrapper + " to compute one");
                    }
                }
            }
            // A payload constructor of a module of this program, through its
            // path (`Model.RecordDecl(entry)`): the module's ADT builds it.
            if (isNamespaceCall && importedPath && m_localModules.contains(*importedPath) &&
                !node.method.empty() &&
                std::isupper(static_cast<unsigned char>(node.method.front())))
                if (auto constructed =
                        constructorResultType(node.method, argTypes, *importedPath)) {
                    const auto* adt = std::get_if<NamedType>(&resolve(constructed)->kind);
                    m_decidedCalls[&expr] = {CallKind::Constructor,
                                             adt ? adt->name : node.method};
                    return constructed;
                }
            auto result = checkCall(callName, argTypes, expr.location,
                                    /*isMethodCall=*/true, &node, &expr);
            // Through a structural alias the value is the first argument already:
            // the alias itself is no receiver to pass.
            if (structuralAlias)
                if (auto resolved = m_resolvedCalls.find(&node); resolved != m_resolvedCalls.end())
                    resolved->second.passesReceiver = false;
            // Nothing chose a target and the receiver's type is not known
            // (or is a trait's `This`, in its default bodies): the method is
            // looked up on the value when the call runs.
            if (!isNamespaceCall && !argTypes.empty() &&
                !m_decidedCalls.contains(&expr) &&
                !m_selectedCallSignatures.contains(&expr) &&
                !m_resolvedCalls.contains(&node)) {
                const auto receiver = resolve(argTypes.front());
                // On a trait's `This` the type claiming it answers, in the copy
                // of the default lowered for that type: never an import.
                if (isThisPlaceholder(receiver))
                    m_decidedCalls[&expr] = {CallKind::Dispatch, node.method};
                else if (isUndetermined(receiver))
                    decideDispatch(&expr, node.method);
            }
            return result;
        }
        else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
            // `[...xs.map(~double), 99]`: the spread value is checked like
            // any other, and in a list it stands for its elements.
            auto spread = node.inner ? resolve(inferExpr(*node.inner)) : Type::unknown();
            if (const auto* list = std::get_if<ListType>(&spread->kind))
                return list->element;
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            if (node.anyElements) {
                for (const auto& elem : node.elements)
                    if (elem) inferExpr(*elem);
                return Type::list(Type::unknown());
            }
            TypePtr elemType = Type::unknown();
            for (const auto& elem : node.elements) {
                if (elem) {
                    auto t = inferExpr(*elem);
                    bool elemPermissive = isUndetermined(elemType);
                    bool tPermissive = isUndetermined(t) ||
                                       std::holds_alternative<FuncType>(t->kind);
                    bool elemFuncPermissive = elemPermissive ||
                                              std::holds_alternative<FuncType>(elemType->kind);
                    if (elemPermissive) {
                        elemType = t; // adopt the concrete type if we have one
                    } else if (!tPermissive && !elemFuncPermissive &&
                               !argMatchesParam(t, elemType) && !argMatchesParam(elemType, t)) {
                        // Before erroring, check if both types share a common trait.
                        // If so, widen the element type to that trait.
                        std::string common = m_traits.commonTrait(elemType, t);
                        if (!common.empty()) {
                            elemType = Type::named(common);
                        } else {
                            // Sibling nullary constructors share their parent
                            // ADT as a natural list element type. Preserve
                            // constructor refinements for overload selection,
                            // but widen `[Less, Equal, Greater]` to
                            // `[Comparison]` instead of rejecting it.
                            auto* lhs = std::get_if<NamedType>(&elemType->kind);
                            auto* rhs = std::get_if<NamedType>(&t->kind);
                            const auto* lhsOwner = lhs ? constructorOwner(*lhs) : nullptr;
                            const auto* rhsOwner = rhs ? constructorOwner(*rhs) : nullptr;
                            if (lhsOwner && rhsOwner && *lhsOwner == *rhsOwner) {
                                elemType = Type::named(*lhsOwner);
                            } else {
                                error(expr.location,
                                      "List elements must be the same type. Expected " +
                                      typeToString(elemType) + ", got " + typeToString(t));
                            }
                        }
                    }
                }
            }
            // `[x | rest]`: the tail is a list of the same elements.
            if (node.rest && *node.rest) {
                const auto open = [](const TypePtr& type) {
                    return isUndetermined(type);
                };
                const auto rest = resolve(inferExpr(**node.rest));
                if (const auto* list = std::get_if<ListType>(&rest->kind)) {
                    const auto element = resolve(list->element);
                    if (open(elemType)) elemType = element;
                    else if (!open(element) && !argMatchesParam(element, elemType) &&
                             !argMatchesParam(elemType, element))
                        error((*node.rest)->location,
                              "The tail of `[x | rest]` must be a list of " +
                                  typeToString(elemType) + ", got " + typeToString(rest));
                } else if (!open(rest)) {
                    error((*node.rest)->location,
                          "The tail of `[x | rest]` must be a list, got " + typeToString(rest));
                }
            }
            return Type::list(elemType);
        }
        else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            TypePtr keyType = Type::unknown();
            TypePtr valueType = Type::unknown();
            for (const auto& entry : node.entries) {
                if (entry.key) {
                    auto k = inferExpr(*entry.key);
                    if (std::holds_alternative<UnknownType>(keyType->kind)) keyType = k;
                }
                if (entry.value) {
                    auto v = inferExpr(*entry.value);
                    if (std::holds_alternative<UnknownType>(valueType->kind)) valueType = v;
                }
            }
            return Type::map(keyType, valueType);
        }
        else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            std::vector<TypePtr> types;
            for (const auto& elem : node.elements) {
                types.push_back(elem ? inferExpr(*elem) : Type::unknown());
            }
            return Type::tuple(std::move(types));
        }
        else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            auto startType = node.start ? resolve(inferExpr(*node.start)) : Type::unknown();
            auto endType = node.end ? resolve(inferExpr(*node.end)) : Type::unknown();
            const auto elementType = [](const TypePtr& bound) -> TypePtr {
                if (std::holds_alternative<SizedFloatType>(bound->kind))
                    return Type::float64();
                if (std::holds_alternative<SizedIntType>(bound->kind))
                    return Type::integer();
                if (auto* primitive = std::get_if<PrimitiveType>(&bound->kind)) {
                    if (primitive->kind == PrimitiveType::Char) return Type::charT();
                    if (primitive->kind == PrimitiveType::Integer) return Type::integer();
                }
                return nullptr;
            };
            auto startElement = elementType(startType);
            auto endElement = elementType(endType);
            const auto unresolved = [](const TypePtr& bound) {
                return isUndetermined(bound);
            };
            if ((!startElement && !unresolved(startType)) ||
                (!endElement && !unresolved(endType)))
                error(expr.location, "Range endpoints must be Integer, Float, or Char");
            else if (startElement && endElement && !typesEqual(startElement, endElement))
                error(expr.location, "Range endpoints must have the same type");
            return Type::named("Range", {startElement ? startElement : Type::integer()});
        }
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            const auto expected = std::exchange(m_branchExpectation, nullptr);
            const auto bothFit = [&](const TypePtr& a, const TypePtr& b) {
                return expected && argMatchesParam(a, expected) &&
                       argMatchesParam(b, expected);
            };
            bool fittedExpected = false;
            if (node.letPattern) {
                // `if let Pattern = expr` — infer scrutinee, bind pattern vars
                // in a scope covering only the then-body (already scoped in
                // resolve pass). Skip Bool check — it's a pattern match.
                auto scrutinee = node.condition
                    ? inferExpr(*node.condition) : nullptr;
                pushScope();
                if (node.letPattern)
                    bindPatternVars(*node.letPattern, scrutinee);
            } else if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "If condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            // A branch that exits the function produces nothing for the
            // conditional to agree with, so it must not drive the result type.
            auto branchBodyType = [this](const std::vector<ast::ExprPtr>& body) {
                auto inferred = inferBody(body);
                return alwaysReturns(body) ? Type::voidType() : inferred;
            };
            auto thenType = branchBodyType(node.thenBody);
            if (node.letPattern) popScope();
            TypePtr branchType = resolve(thenType);  // tracks the first concrete non-Never branch
            for (const auto& [cond, body] : node.elifs) {
                if (cond) inferExpr(*cond);
                auto elifType = resolve(branchBodyType(body));
                auto rt = resolve(branchType);
                bool rtPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                    std::holds_alternative<UnknownType>(rt->kind) ||
                                    std::holds_alternative<VoidType>(rt->kind);
                bool rePermissive = std::holds_alternative<TypeVar>(elifType->kind) ||
                                    std::holds_alternative<UnknownType>(elifType->kind) ||
                                    std::holds_alternative<VoidType>(elifType->kind);
                // Without an `else` the conditional is Void whatever its
                // branches produce (see below), so they have nothing to
                // agree on: `if a ... x = 1 elif b ... opt.map(f) end`.
                if (auto joined = siblingConstructorJoin(rt, elifType)) {
                    branchType = joined;
                } else if (!rtPermissive && !rePermissive && bothFit(rt, elifType)) {
                    fittedExpected = true;
                } else if (node.elseBody && !rtPermissive && !rePermissive &&
                    !argMatchesParam(elifType, rt) && !argMatchesParam(rt, elifType)) {
                    error(expr.location, "Branch type mismatch: 'if' returns " +
                          typeToString(rt) + " but 'elif' returns " + typeToString(elifType));
                }
                if (rtPermissive && !rePermissive) branchType = elifType;
            }
            if (node.elseBody) {
                auto elseType = resolve(branchBodyType(*node.elseBody));
                auto rt = resolve(branchType);
                bool thenPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                      std::holds_alternative<UnknownType>(rt->kind) ||
                                      std::holds_alternative<VoidType>(rt->kind);
                bool elsePermissive = std::holds_alternative<TypeVar>(elseType->kind) ||
                                      std::holds_alternative<UnknownType>(elseType->kind) ||
                                      std::holds_alternative<VoidType>(elseType->kind);
                if (auto joined = siblingConstructorJoin(rt, elseType)) {
                    branchType = joined;
                } else if (!thenPermissive && !elsePermissive &&
                           bothFit(rt, elseType)) {
                    fittedExpected = true;
                } else if (!thenPermissive && !elsePermissive &&
                    !argMatchesParam(elseType, rt) && !argMatchesParam(rt, elseType)) {
                    error(expr.location, "Branch type mismatch: 'if' returns " +
                          typeToString(rt) + " but 'else' returns " + typeToString(elseType));
                }
                if (thenPermissive && !elsePermissive) branchType = elseType;
            }
            // With no `else` the conditional may produce nothing at all, so it
            // cannot hand its then-branch's type to the block it closes —
            // `if done? ... return Error(e) ... end` is not a Result-valued
            // expression.
            if (!node.elseBody) return Type::voidType();
            if (fittedExpected) return expected;
            return resolve(branchType);
        }
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            const auto expected = std::exchange(m_branchExpectation, nullptr);
            bool fittedExpected = false;
            TypePtr subjectType = node.subject ? inferExpr(*node.subject) : Type::unknown();
            TypePtr resultType = Type::unknown();
            for (const auto& clause : node.clauses) {
                pushScope();
                if (node.subjectBinding) {
                    defineVar(*node.subjectBinding, subjectType);
                }
                for (const auto& pat : clause.patterns) {
                    if (pat) {
                        checkPatternArity(*pat);
                        bindPatternVars(*pat, subjectType);
                    }
                }
                if (clause.guard && *clause.guard) inferExpr(**clause.guard);
                if (clause.body && alwaysReturns(*clause.body)) {
                    // The arm exits the function; it produces nothing for the
                    // match to agree with.
                    inferExpr(*clause.body);
                } else if (clause.body) {
                    auto t = resolve(inferExpr(*clause.body));
                    auto rt = resolve(resultType);
                    if (isUndetermined(rt)) {
                        resultType = t;  // adopt first concrete arm type
                    } else {
                        // Check subsequent arms match the first concrete arm.
                        bool armPermissive = std::holds_alternative<TypeVar>(t->kind) ||
                                            std::holds_alternative<UnknownType>(t->kind);
                        if (auto joined = siblingConstructorJoin(rt, t)) {
                            resultType = joined;
                        } else if (!armPermissive && expected &&
                                   argMatchesParam(t, expected) &&
                                   argMatchesParam(rt, expected)) {
                            fittedExpected = true;
                        } else if (!armPermissive && !argMatchesParam(t, rt) && !argMatchesParam(rt, t)) {
                            error(expr.location, "Match arm type mismatch: expected " +
                                  typeToString(rt) + " but arm returns " + typeToString(t));
                        }
                    }
                }
                popScope();
            }
            checkMatchExhaustiveness(node, expr.location);
            if (fittedExpected) return expected;
            return resultType;
        }
        else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            TypePtr resultType = Type::unknown();
            for (const auto& clause : node.clauses) {
                pushScope();
                if (node.senderBinding)
                    defineVar(*node.senderBinding,
                              Type::named("Process", {Type::unknown()}));
                for (const auto& pattern : clause.patterns)
                    if (pattern) {
                        checkPatternArity(*pattern);
                        bindPatternVars(*pattern);
                    }
                if (clause.guard && *clause.guard)
                    inferExpr(**clause.guard);
                if (clause.body) {
                    auto arm = resolve(inferExpr(*clause.body));
                    if (std::holds_alternative<UnknownType>(
                            resolve(resultType)->kind))
                        resultType = arm;
                }
                popScope();
            }
            if (node.timeout && *node.timeout) inferExpr(**node.timeout);
            if (node.afterBody && *node.afterBody) {
                auto after = resolve(inferExpr(**node.afterBody));
                if (std::holds_alternative<UnknownType>(
                        resolve(resultType)->kind))
                    resultType = after;
            }
            return resultType;
        }
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            return node.value ? inferExpr(*node.value) : Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            // Lambda used as a value (not a trailing block — that path goes
            // through inferBlock). Infer param types and body, return a proper
            // FuncType so call-site checking can validate the argument types.
            if (node.params.empty()) {
                m_blockDepth++;
                auto bodyType = inferBody(node.body);
                if (node.rescue) checkRescue(*node.rescue);
                m_blockDepth--;
                return Type::func({}, resolve(bodyType));
            }
            pushScope();
            std::vector<TypePtr> paramTypes;
            std::unordered_map<std::string, TypePtr> genericVars;
            for (const auto& param : node.params) {
                auto pt = param.type
                    ? resolveTypeExpr(**param.type, genericVars)
                    : freshTypeVar();
                paramTypes.push_back(pt);
                if (param.name != "_") {
                    defineVar(param.name, pt);
                    // Value-position lambdas need the same tooling binding as
                    // contextual/trailing blocks. Without it `{ |name| ... }`
                    // had no local hover entry and the LSP fell through to an
                    // unrelated exported symbol with the same name.
                    m_patternBindings.push_back(
                        {param.name, expr.location, pt});
                }
            }
            m_blockDepth++;
            auto bodyType = inferBody(node.body);
            if (node.rescue) checkRescue(*node.rescue);
            m_blockDepth--;
            auto resultType = resolve(bodyType);
            if (node.returnAnnotation) {
                auto declared =
                    resolveTypeExpr(**node.returnAnnotation, genericVars);
                if (!std::holds_alternative<UnknownType>(resultType->kind) &&
                    !std::holds_alternative<TypeVar>(resultType->kind) &&
                    !argMatchesParam(resultType, declared))
                    typeMismatch(expr.location, declared, resultType);
                resultType = declared;
            }
            popScope();
            // Resolve param types after body inference — body may have constrained them.
            for (auto& pt : paramTypes) pt = resolve(pt);
            return Type::func(std::move(paramTypes), resultType);
        }
        else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            pushScope();
            inferBody(node.body);
            popScope();
            // Process<Msg> — Msg is a fresh TypeVar that unification resolves
            // against the declared return-type annotation (e.g. -> Process<Counter>).
            return Type::named("Process", {freshTypeVar()});
        }
        else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            if (node.counter) {
                pushScope();
                if (*node.counter != "_") defineVar(*node.counter, Type::integer());
                inferBody(node.body);
                popScope();
            } else {
                inferBody(node.body);
            }
            return Type::voidType();  // infinite loop never returns
        }
        else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "While condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            inferBody(node.body);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            // Check each value against the field's DECLARED type. Without
            // this, `User { name: 42 }` on `name : String` was accepted and
            // only surfaced later — or not at all.
            std::string sourceName = node.typeName;
            TypePtr contextualRecordType;
            if (sourceName == names::kThisType || sourceName == "New") {
                if (!m_inMakeBlock || !m_currentMakeType) {
                    error(expr.location, "`" + sourceName +
                                             "` construction requires a make block receiver");
                    return Type::unknown();
                }
                auto receiver = resolve(m_currentMakeType);
                auto* named = std::get_if<NamedType>(&receiver->kind);
                if (!named || !m_recordFields.count(resolveRecordName(named->name))) {
                    error(expr.location, "`" + sourceName +
                                             "` requires a record receiver");
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
            if (auto dot = recordName.rfind('.'); dot != std::string::npos)
                m_referencedModules.insert(recordName.substr(0, dot));
            auto record = m_recordFields.find(recordName);
            bool hasSpread = node.typeName == "New";
            std::unordered_set<std::string> supplied;
            // Type arguments recovered from the values, slot by slot.
            std::unordered_map<int, TypePtr> typeParamSlots;
            for (const auto& entry : node.fields) {
                const auto& fieldName = entry.name;
                const auto& val = entry.value;
                if (!val) continue;
                auto valueType = resolve(inferExpr(*val));
                if (entry.spread) {
                    hasSpread = true;
                    // `Box { ...other, items: … }` keeps whatever `other`
                    // already pinned for the slots the new fields don't.
                    if (auto* spreadNamed =
                            std::get_if<NamedType>(&valueType->kind))
                        for (size_t i = 0; i < spreadNamed->typeArgs.size(); ++i)
                            bindRecordTypeParams(
                                Type::typeVar(-static_cast<int>(i + 1)),
                                spreadNamed->typeArgs[i], typeParamSlots);
                    if (!containsOpenType(valueType) &&
                        !argMatchesParam(valueType, Type::named(recordName)))
                        error(val->location, "record spread expects `" +
                                                 sourceName + "`, but got " +
                                                 typeToString(valueType));
                    continue;
                }
                supplied.insert(fieldName);
                if (record == m_recordFields.end()) continue;
                auto declared = record->second.find(fieldName);
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
                    for (const auto& [known, _] : record->second)
                        available.push_back(known);
                    std::sort(available.begin(), available.end());
                    std::string layout;
                    for (std::size_t i = 0; i < available.size(); i++) {
                        if (i) layout += ", ";
                        layout += available[i];
                    }
                    error(val->location,
                          "record `" + sourceName + "` has no field `" +
                          fieldName + "` — it has " +
                          (layout.empty() ? "no fields" : layout));
                    continue;
                }
                auto expected = resolve(declared->second);
                bindRecordTypeParams(expected, valueType, typeParamSlots);
                // Stay quiet where either side is open ANYWHERE inside it: a
                // type variable, a type the checker could not pin down, or a
                // trait constraint, which names a set rather than a concrete
                // type. Two cases this must not reject: `make Integer do …
                // Duration { seconds: this * 1.0 }` infers `Number` for the
                // value, and a `Map<Any, String>` field takes `{"k": "v"}`
                // — there the openness is nested, not at the top.
                if (containsOpenType(valueType) || containsOpenType(expected))
                    continue;
                if (!argMatchesParam(valueType, expected) &&
                    !literalCoercesTo(*val, expected))
                    error(val->location,
                          "`" + sourceName + "." + fieldName +
                          "` expects " + typeToString(expected) + ", but got " +
                          typeToString(valueType));
            }
            if (!hasSpread) {
                std::vector<std::string> missing;
                for (const auto& required : m_requiredRecordFields[recordName])
                    if (!supplied.count(required)) missing.push_back(required);
                std::sort(missing.begin(), missing.end());
                if (!missing.empty()) {
                    std::string names;
                    for (std::size_t i = 0; i < missing.size(); ++i) {
                        if (i) names += ", ";
                        names += "`" + missing[i] + "`";
                    }
                    error(expr.location, "missing mandatory field" +
                                             std::string(missing.size() == 1 ? " " : "s ") +
                                             names + " constructing `" + sourceName + "`");
                }
            }
            // `recordName` is `node.typeName` itself when no declaration was
            // found, so the resolved identity is always the right answer here:
            // a literal must have the same type name that annotations
            // mentioning the same record resolve to.
            if (contextualRecordType) return contextualRecordType;
            // `Crew {}` with no `Crew` declared is a typo or a missing
            // declaration, not a record: unchecked, the walker made an empty
            // record and BEAM a bare `:Crew` atom.
            if (record == m_recordFields.end() &&
                !(m_importedInterfaces &&
                  m_importedInterfaces->recordArities.count(recordName))) {
                error(expr.location, "Unknown record type `" + sourceName + "`");
                return Type::unknown();
            }
            // A generic record's literal has to carry its arguments, or the
            // value can never flow back into anything that names them:
            // `Box { items: [1, 2, 3] }` is a `Box<Integer>`, and only that
            // spelling matches a `Box<A>` receiver or reads `.items` back as
            // `[Integer]` (kexhq/kex#203). Slots no field pinned stay open,
            // which is what `Box { items: [] }` deserves.
            const size_t paramCount = record == m_recordFields.end()
                ? 0 : recordTypeParamCount(record->second);
            if (paramCount == 0) return Type::named(recordName);
            std::vector<TypePtr> typeArgs(paramCount, Type::unknown());
            for (const auto& [slot, bound] : typeParamSlots) {
                const auto index = static_cast<size_t>(-slot - 1);
                if (index < typeArgs.size()) typeArgs[index] = bound;
            }
            return Type::named(recordName, std::move(typeArgs));
        }
        else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                if (!std::holds_alternative<UnknownType>(condType->kind) &&
                    !std::holds_alternative<TypeVar>(condType->kind) &&
                    !typesEqual(condType, Type::boolean())) {
                    error(expr.location, "If condition must be Bool, got " +
                          typeToString(condType));
                }
            }
            // `expr if cond` is a guarded STATEMENT: when the guard is false
            // nothing is produced, so it cannot hand its expression's type to
            // the block it closes. `return Ok(x) if done?` is the shape that
            // matters — it must not make its branch look like it yields a
            // Result.
            if (node.expr) {
                inferExpr(*node.expr);
                return Type::voidType();
            }
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            return inferBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            const auto expected = std::exchange(m_branchExpectation, nullptr);
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "then/else condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            auto thenType = node.thenExpr ? inferExpr(*node.thenExpr) : Type::unknown();
            if (node.elseExpr) {
                auto elseType = inferExpr(*node.elseExpr);
                auto rt = resolve(thenType);
                auto re = resolve(elseType);
                bool thenPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                      std::holds_alternative<UnknownType>(rt->kind);
                bool elsePermissive = std::holds_alternative<TypeVar>(re->kind) ||
                                      std::holds_alternative<UnknownType>(re->kind);
                if (auto joined = siblingConstructorJoin(rt, re)) return joined;
                if (!thenPermissive && !elsePermissive && expected &&
                    argMatchesParam(rt, expected) && argMatchesParam(re, expected))
                    return expected;
                if (!thenPermissive && !elsePermissive &&
                    !argMatchesParam(re, rt) && !argMatchesParam(rt, re)) {
                    error(expr.location, "Branch type mismatch: 'then' returns " +
                          typeToString(rt) + " but 'else' returns " + typeToString(re));
                }
            }
            return resolve(thenType);
        }
        else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
            return inferExpr(*node.lambda);
        }
        else if constexpr (std::is_same_v<T, ast::CurryPlaceholder>) {
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
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
                    if (node.name == "==" || node.name == "!=" ||
                        node.name == "<" || node.name == ">" ||
                        node.name == "<=" || node.name == ">=")
                        signature.result = Type::boolean();
                    else
                        signature.result = operand;
                }
                candidates.push_back(std::move(signature));
            } else {
                const auto key = node.module.empty()
                    ? node.name : kex::mangle::qualify(node.module, node.name);
                if (auto user = m_userSignatures.find(key);
                    user != m_userSignatures.end())
                    candidates = user->second;
                if (candidates.empty())
                    candidates = importedCandidateSignatures(key);
            }

            if (candidates.empty()) {
                // Preserve gradual recovery for an unresolved capture while
                // still visiting every bound argument.
                int open = 0;
                for (const auto& group : node.argGroups)
                    for (const auto& arg : group)
                        if (std::holds_alternative<ast::CurryPlaceholder>(arg->kind))
                            ++open;
                        else
                            inferExpr(*arg);
                std::vector<TypePtr> params(open, Type::unknown());
                return params.empty() ? Type::unknown()
                                      : Type::func(std::move(params), Type::unknown());
            }

            if (node.argGroups.empty()) {
                std::vector<const Signature*> distinct;
                for (const auto& signature : candidates) {
                    const bool duplicate = std::any_of(
                        distinct.begin(), distinct.end(), [&](const Signature* other) { return sameParameters(*other, signature); });
                    if (!duplicate) distinct.push_back(&signature);
                }
                const bool sameArityAmbiguity = std::any_of(
                    distinct.begin(), distinct.end(), [&](const Signature* left) {
                        return std::any_of(
                            distinct.begin(), distinct.end(),
                            [&](const Signature* right) {
                                return left != right &&
                                    left->params.size() == right->params.size();
                            });
                    });
                if (sameArityAmbiguity) {
                    std::string message = "Cannot reference overloaded function `" +
                        node.name + "` without disambiguating arguments";
                    for (const auto* signature : distinct)
                        message += "\n\n" + displaySignature(node.name, *signature);
                    error(expr.location, message);
                }
            }

            // A capture is an application of a fresh INSTANCE of the public
            // signature. This retains relationships such as A -> A -> A,
            // while binding `~add(1)` specializes the open argument/result to
            // Integer without mutating the function's declared signature.
            std::unordered_map<int, TypePtr> instantiatedVars;
            std::function<TypePtr(const TypePtr&)> instantiate =
                [&](const TypePtr& type) -> TypePtr {
                    if (!type) return type;
                    return std::visit([&](const auto& part) -> TypePtr {
                        using Part = std::decay_t<decltype(part)>;
                        if constexpr (std::is_same_v<Part, TypeVar>) {
                            auto [it, inserted] = instantiatedVars.emplace(part.id, nullptr);
                            if (inserted) it->second = freshTypeVar();
                            return it->second;
                        } else {
                            return mapTypeChildren(type, instantiate);
                        }
                    }, type->kind);
                };

            const auto& selected = candidates.front();
            std::vector<TypePtr> params;
            for (const auto& param : selected.params)
                params.push_back(instantiate(param));
            auto resultType = instantiate(selected.result);
            std::vector<TypePtr> remaining;
            size_t position = 0;
            for (const auto& group : node.argGroups) {
                for (const auto& arg : group) {
                    if (position >= params.size()) {
                        if (!std::holds_alternative<ast::CurryPlaceholder>(arg->kind))
                            inferExpr(*arg);
                        ++position;
                        continue;
                    }
                    if (std::holds_alternative<ast::CurryPlaceholder>(arg->kind)) {
                        remaining.push_back(params[position++]);
                        continue;
                    }
                    auto actual = resolve(inferExpr(*arg));
                    auto expected = resolve(params[position++]);
                    if (auto* variable = std::get_if<TypeVar>(&expected->kind))
                        unifyVar(variable->id, actual);
                    else if (auto* variable = std::get_if<TypeVar>(&actual->kind)) {
                        if (!deferIfPending(actual, expected, arg->location))
                            unifyVar(variable->id, expected);
                    } else if (!argMatchesParam(actual, expected))
                        typeMismatch(arg->location, expected, actual);
                }
            }
            while (position < params.size())
                remaining.push_back(params[position++]);
            for (auto& param : remaining) param = resolve(param);
            resultType = resolve(resultType);

            // `~banner` names the function, even one of no parameters: a
            // `() -> String` to call later, not its result. Only arguments
            // that fill every parameter (`~add(1, 2)`) make it a call.
            if (remaining.empty() && !node.argGroups.empty()) return resultType;
            return Type::func(std::move(remaining), resultType);
        }
        else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
            // Inside a make block, `this` / `@field` has the record type.
            if (m_currentMakeType) return m_currentMakeType;
            // Anywhere else there is no receiver to bind. A module-level
            // `let` mentioning `this` used to pass here as Unknown and fail
            // only in the backends — erlc rejecting it as an unbound
            // variable, the walker misreporting a sibling member — so the
            // checker says it where it is written (kexhq/kex#293).
            error(expr.location,
                  "'this' has no receiver here — it is only bound inside a "
                  "`make` block method");
            return Type::unknown();
        }
        else {
            return Type::unknown();
        }
    }, expr.kind);
    m_typeMap[&expr] = result;
    return result;
}

auto TypeChecker::inferBinaryOp(TokenType op, const TypePtr& left, const TypePtr& right,
                                SourceLocation loc) -> TypePtr {
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

    {
        auto lhsIsVar = std::holds_alternative<TypeVar>(lhs->kind);
        auto rhsIsVar = std::holds_alternative<TypeVar>(rhs->kind);
        auto lhsIsUnk = std::holds_alternative<UnknownType>(lhs->kind);
        auto rhsIsUnk = std::holds_alternative<UnknownType>(rhs->kind);

        if (lhsIsVar || rhsIsVar || lhsIsUnk || rhsIsUnk) {
            auto varId = [](const TypePtr& t) -> int {
                if (auto* tv = std::get_if<TypeVar>(&t->kind)) return tv->id;
                return -1;
            };
            auto isConcrete = [](const TypePtr& t) {
                return !std::holds_alternative<TypeVar>(t->kind) &&
                       !std::holds_alternative<UnknownType>(t->kind);
            };

            switch (op) {
                // -, *, /, %, ^: both operands must be Number.
                case TokenType::Minus: case TokenType::Star:
                case TokenType::Slash: case TokenType::Percent: case TokenType::Caret: {
                    auto nc = Type::constrained("N", "Number");
                    if (int id = varId(lhs); id >= 0) unifyVar(id, nc);
                    if (int id = varId(rhs); id >= 0) unifyVar(id, nc);
                    if (isConcrete(lhs)) return lhs;
                    // A left operand that is UNKNOWN (as opposed to a type
                    // variable the substitution can still learn about) makes
                    // the right one no evidence at all: `3.meter ^ 2` is a
                    // Measure, and typing it as the exponent's Integer sent
                    // the following `.value` to Integer, where it became a
                    // bogus arity error against an unrelated prelude method.
                    if (lhsIsUnk) return Type::unknown();
                    if (isConcrete(rhs)) return rhs;
                    return resolve(lhs);
                }
                // +: String/Char on one side → constrain TypeVar to String;
                //    numeric on one side → constrain TypeVar to Number.
                case TokenType::Plus: {
                    if (isConcrete(lhs) && (isString(lhs) || isChar(lhs))) {
                        if (int id = varId(rhs); id >= 0) unifyVar(id, Type::string());
                        return Type::string();
                    }
                    if (isConcrete(rhs) && (isString(rhs) || isChar(rhs))) {
                        if (int id = varId(lhs); id >= 0) unifyVar(id, Type::string());
                        return Type::string();
                    }
                    if (isConcrete(lhs) && isNumeric(lhs)) {
                        if (int id = varId(rhs); id >= 0) unifyVar(id, Type::constrained("N", "Number"));
                        return lhs;
                    }
                    if (isConcrete(rhs) && isNumeric(rhs)) {
                        if (int id = varId(lhs); id >= 0) unifyVar(id, Type::constrained("N", "Number"));
                        return rhs;
                    }
                    // With two unconstrained operands, `a + b` still proves
                    // that both operands and the result have one shared type.
                    // Keeping them independent leaked signatures such as
                    // `T7 -> T8 -> T7` for the ordinary generic `add`.
                    if (int lhsId = varId(lhs); lhsId >= 0) {
                        if (varId(rhs) >= 0) unifyVar(varId(rhs), lhs);
                        return resolve(Type::typeVar(lhsId));
                    }
                    return lhs;
                }
                // Ordered comparisons: constrain TypeVar to the concrete side.
                case TokenType::LessThan: case TokenType::GreaterThan:
                case TokenType::LessEq: case TokenType::GreaterEq: {
                    if (int id = varId(lhs); id >= 0 && isConcrete(rhs)) unifyVar(id, rhs);
                    if (int id = varId(rhs); id >= 0 && isConcrete(lhs)) unifyVar(id, lhs);
                    return Type::boolean();
                }
                case TokenType::EqEq: case TokenType::NotEq:
                    return Type::boolean();
                case TokenType::AmpAmp: case TokenType::PipePipe: {
                    if (int id = varId(lhs); id >= 0) unifyVar(id, Type::boolean());
                    if (int id = varId(rhs); id >= 0) unifyVar(id, Type::boolean());
                    return Type::boolean();
                }
                default:
                    return lhs;
            }
        }
    }

    auto isFloat = [](const TypePtr& t) { return std::holds_alternative<SizedFloatType>(t->kind); };
    auto isIntegerLike = [](const TypePtr& t) {
        if (auto* prim = std::get_if<PrimitiveType>(&t->kind)) return prim->kind == PrimitiveType::Integer;
        return std::holds_alternative<SizedIntType>(t->kind);
    };
    auto isBool = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Bool;
    };

    bool leftIsString = isString(lhs), rightIsString = isString(rhs);
    bool leftIsFloat = isFloat(lhs), rightIsFloat = isFloat(rhs);
    bool leftIsInt = isIntegerLike(lhs), rightIsInt = isIntegerLike(rhs);

    switch (op) {
        case TokenType::Plus:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString && rightIsString)
                return Type::string();
            // String + Char, Char + String, Char + Char — all produce String
            // (String = [Char], so appending a Char or two Chars is valid concatenation).
            if ((leftIsString || isChar(lhs)) && (rightIsString || isChar(rhs)))
                return Type::string();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot add " + typeToString(lhs) + " and " + typeToString(rhs));
                return Type::string();
            }
            if (!argMatchesParam(lhs, rhs) && !argMatchesParam(rhs, lhs)) {
                error(loc, "Operator '+' requires matching types, got " +
                      typeToString(lhs) + " and " + typeToString(rhs));
            }
            return lhs;

        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
        case TokenType::Caret:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot use arithmetic operator on String");
                return Type::integer();
            }
            return lhs;

        case TokenType::EqEq:
        case TokenType::NotEq:
            return Type::boolean();

        case TokenType::LessThan:
        case TokenType::GreaterThan:
        case TokenType::LessEq:
        case TokenType::GreaterEq:
            if (isBool(lhs)) {
                error(loc, "Cannot compare Bool values with '<', '>', '<=', '>='");
            }
            return Type::boolean();

        case TokenType::AmpAmp:
        case TokenType::PipePipe:
            if (!isBool(lhs)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(lhs));
            }
            if (!isBool(rhs)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(rhs));
            }
            return Type::boolean();

        default:
            return Type::unknown();
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

auto TypeChecker::typeNamed(const std::string& name, SourceLocation location)
    -> TypePtr {
    if (m_nullaryConstructors.count(name) || m_adtOfConstructor.count(name))
        return nullptr;
    // A module's constructor is a value too, not the name of a type: taken
    // for one, `Type.of(Public)` answered `Public` (kexhq/kex#505).
    for (const auto& [_, constructors] : m_moduleConstructors)
        if (constructors.count(name)) return nullptr;
    ast::TypeName typeName;
    typeName.parts = {name};
    ast::TypeExpr asType;
    asType.location = location;
    asType.kind = std::move(typeName);
    std::unordered_map<std::string, TypePtr> generics;
    return resolveTypeExpr(asType, generics);
}

// A parameter written as a bare type name selects that type: the conversion
// protocol's `let to(String)` takes the type `String` itself, a `Type<String>`.
auto TypeChecker::typeSelectorParameter(const ast::Param& param) -> TypePtr {
    if (param.type || !param.pattern || !*param.pattern) return nullptr;
    const auto* selector = std::get_if<ast::ConstructorPattern>(&(*param.pattern)->kind);
    if (!selector || !selector->args.empty()) return nullptr;
    // A constructor is a value pattern, whatever imports it: `faults(Name, ...)`
    // with `using App.Models.User, only: [Name]` matches `UserDetail.Name`.
    if (!namesKnownType(selector->name) || nullaryConstructorType(selector->name))
        return nullptr;
    auto named = typeNamed(selector->name, (*param.pattern)->location);
    if (!named || std::holds_alternative<UnknownType>(resolve(named)->kind))
        return nullptr;
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
auto TypeChecker::namespaceCallProblem(const std::string& written,
                                       const std::string& resolved,
                                       const std::string& method)
    -> std::optional<std::string> {
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
        for (const auto& import : imports)
            if (import.alias && *import.alias == root)
                candidates.push_back({import.module + tail, 0});
    };
    addAliases(m_declarationImports);
    for (const auto& scope : m_importScopeStack) addAliases(scope);
    for (auto scope = m_currentModulePath; !scope.empty();) {
        candidates.push_back({scope + "." + resolved, scope.size()});
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) break;
        scope.resize(dot);
    }

    std::size_t minimumModule = 0;
    const auto relatedTo = [&](const std::string& path, const std::string& module) {
        if (module.size() <= minimumModule) return false;
        return module == path || path.rfind(module + ".", 0) == 0 ||
               module.rfind(path + ".", 0) == 0;
    };
    // `export Prelude, only: [...]` re-namespaces a module under its parent;
    // its members are the original module's, so a path through one is left
    // alone rather than checked against the parent.
    const auto reexported = [&](const std::string& path) {
        return std::any_of(m_moduleReexports.begin(), m_moduleReexports.end(),
                           [&](const auto& entry) { return relatedTo(path, entry.first); });
    };
    const auto moduleKnown = [&](const std::string& path) {
        for (const auto& module : m_localModules)
            if (relatedTo(path, module)) return true;
        for (const auto& module : m_importedModulePaths)
            if (relatedTo(path, module)) return true;
        if (reexported(path)) return true;
        if (m_importedInterfaces)
            for (const auto& [module, _] : m_importedInterfaces->modules)
                if (relatedTo(path, module) ||
                    (module.rfind("Kex.", 0) == 0 && relatedTo(path, module.substr(4))))
                    return true;
        return false;
    };
    const auto match = std::find_if(candidates.begin(), candidates.end(),
                                    [&](const Candidate& candidate) {
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
                m_scopedDeclaredSignatures.count(m_currentModulePath + "\n" + name))
                return true;
            for (auto scope = m_currentModulePath; !scope.empty();) {
                if (m_userSignatures.count(kex::mangle::qualify(scope, name))) return true;
                const auto dot = scope.rfind('.');
                if (dot == std::string::npos) break;
                scope.resize(dot);
            }
            // Or one a `using` brings in from a module compiled on its own.
            if (m_importedInterfaces)
                for (const auto& [module, interface] : m_importedInterfaces->modules)
                    if (auto exported = interface.exports.find(name);
                        exported != interface.exports.end() &&
                        moduleMemberImported(module, name))
                        return true;
            return false;
        };
        const bool rootIsSomething =
            root == "BEAM" || root == "Kex" || root == names::kThisType ||
            isPrimitiveTypeName(root) ||
            m_recordFields.count(resolveRecordName(root)) ||
            m_adtVariants.count(root) || m_typeAliases.count(root) ||
            m_nullaryConstructors.contains(root) ||
            constructorResultType(root, {}) != nullptr ||
            m_capabilities.count(root) || m_traits.get(root) != nullptr ||
            lookupVar(root) != nullptr || constantInScope(root) ||
            std::any_of(m_moduleConstructors.begin(), m_moduleConstructors.end(),
                        [&](const auto& entry) {
                            return entry.first == root || entry.second.count(root);
                        });
        if (rootIsSomething) return std::nullopt;
        return "Unknown module `" + written + "`";
    }
    const auto& path = match->path;
    if (reexported(path)) return std::nullopt;

    // Past the deepest local module it names, the next segment must be
    // something that module holds: `Shop.Inner` is known only if `Shop`
    // declares `Inner`.
    std::string deepest;
    for (const auto& module : m_localModules)
        if (path.rfind(module + ".", 0) == 0 && module.size() > deepest.size())
            deepest = module;
    // A nested module compiled on its own (`App.Views.Chat.Threads` beside
    // `App.Views.Chat`) is held there as well, through its interface.
    const auto holds = [&](const std::string& module) {
        return module == path || module.rfind(path + ".", 0) == 0;
    };
    const bool importedHere = m_importedInterfaces &&
        std::any_of(m_importedInterfaces->modules.begin(),
                    m_importedInterfaces->modules.end(),
                    [&](const auto& entry) { return holds(entry.first); });
    if (!deepest.empty() && !importedHere &&
        !std::any_of(m_localModules.begin(), m_localModules.end(), holds)) {
        const auto rest = path.substr(deepest.size() + 1);
        const auto next = rest.substr(0, rest.find('.'));
        // An uppercase module CONSTANT (`Console.RED`, which string
        // interpolation calls a method on) is held like a nested module.
        const bool held =
            !std::isupper(static_cast<unsigned char>(next.front())) ||
            m_userSignatures.count(kex::mangle::qualify(deepest, next)) ||
            m_scopedDeclaredSignatures.count(deepest + "\n" + next) ||
            m_recordFields.count(resolveRecordName(next)) ||
            m_recordFields.count(deepest + "." + next) ||
            m_adtVariants.count(next) || m_typeAliases.count(next) ||
            m_nullaryConstructors.contains(next) ||
            constructorResultType(next, {}) != nullptr ||
            (m_moduleConstructors.count(deepest) &&
             m_moduleConstructors.at(deepest).count(next));
        if (!held)
            return "`" + deepest + "` has no module `" + next + "`";
    }

    // A member of a module compiled here.
    if (!m_localModules.contains(path) || method.empty() ||
        !std::islower(static_cast<unsigned char>(method.front())))
        return std::nullopt;
    if (m_importedInterfaces && m_importedInterfaces->modules.count(path))
        return std::nullopt;
    // A module that shares its name with a type (`DateTime`) also answers
    // for that type's static functions, which `make` blocks declare.
    const auto lastDot = path.rfind('.');
    const auto last = lastDot == std::string::npos ? path : path.substr(lastDot + 1);
    if (isPrimitiveTypeName(last) || m_recordFields.count(resolveRecordName(last)) ||
        m_recordFields.count(path) || m_adtVariants.count(last) ||
        m_typeAliases.count(last))
        return std::nullopt;
    if (m_userSignatures.count(kex::mangle::qualify(path, method)) ||
        m_scopedDeclaredSignatures.count(path + "\n" + method) ||
        m_localModules.contains(path + "." + method))
        return std::nullopt;
    if (auto constructors = m_moduleConstructors.find(path);
        constructors != m_moduleConstructors.end() &&
        constructors->second.count(method))
        return std::nullopt;
    std::vector<std::string> members;
    const auto prefix = path + std::string(kex::mangle::kSeparator);
    for (const auto& [name, _] : m_userSignatures)
        if (name.rfind(prefix, 0) == 0) members.push_back(name.substr(prefix.size()));
    std::sort(members.begin(), members.end());
    auto message = "`" + written + "` has no function `" + method + "`";
    if (auto hint = closestName(method, members); !hint.empty())
        message += " — did you mean `" + hint + "`?";
    return message;
}

// The single signature of the function an expression NAMES, or null when the
// expression is not a function name (or names an overload set).
auto TypeChecker::namedFunctionSignature(const ast::Expr& expr)
    -> const Signature* {
    std::string functionName;
    if (const auto* identifier = std::get_if<ast::Identifier>(&expr.kind)) {
        functionName = identifier->name;
    } else if (const auto* qualified = std::get_if<ast::MethodCall>(&expr.kind);
               qualified && qualified->args.empty() && qualified->receiver &&
               !qualified->parenthesized) {
        if (auto module = ast::writtenModulePath(*qualified->receiver))
            functionName = kex::mangle::qualify(*module, qualified->method);
    }
    if (functionName.empty()) return nullptr;
    // The reference names the function it found; it calls nothing.
    const auto named = [&](const Signature* signature) {
        if (std::holds_alternative<ast::MethodCall>(expr.kind))
            m_decidedCalls[&expr] = {CallKind::FunctionReference, functionName};
        return signature;
    };
    if (auto user = m_userSignatures.find(functionName);
        user != m_userSignatures.end() && user->second.size() == 1)
        return named(&user->second.front());
    m_importedSignatureCache = importedCandidateSignatures(functionName);
    if (m_importedSignatureCache.size() == 1)
        return named(&m_importedSignatureCache.front());
    return nullptr;
}

auto TypeChecker::resolveTypeQuery(const ast::TypeQuery& query) -> TypePtr {
    if (!query.argument) return Type::unknown();
    if (query.query == "of") {
        if (auto referenced = typeNameReference(*query.argument)) return referenced;
        return resolve(inferExpr(*query.argument));
    }

    // returnedBy: the argument NAMES a function.
    std::string functionName;
    if (const auto* identifier = std::get_if<ast::Identifier>(&query.argument->kind)) {
        functionName = identifier->name;
    } else if (const auto* qualified =
                   std::get_if<ast::MethodCall>(&query.argument->kind);
               qualified && qualified->args.empty() && qualified->receiver) {
        if (auto module = ast::writtenModulePath(*qualified->receiver))
            functionName = kex::mangle::qualify(*module, qualified->method);
    }

    std::vector<Signature> candidates;
    if (!functionName.empty()) {
        if (auto user = m_userSignatures.find(functionName);
            user != m_userSignatures.end())
            candidates = user->second;
        if (candidates.empty())
            candidates = importedCandidateSignatures(functionName);
    }
    if (candidates.empty()) {
        // A computed ALIAS resolves before signatures are registered (the
        // signatures may themselves name the alias), so fall back to the
        // declaration's own return annotation.
        // Overloads folded into one function count once per signature.
        std::size_t declaredSignatures = 0;
        const auto [first, last] =
            m_functionDeclarations.equal_range(functionName);
        for (auto it = first; it != last; ++it)
            declaredSignatures += ast::signatureClauses(*it->second).size();
        if (declaredSignatures > 1) {
            error(query.argument->location,
                  "`Type.returnedBy` cannot choose between the overloads of `" +
                  sourceName(functionName) + "`");
            return Type::unknown();
        }
        if (auto declaration = m_functionDeclarations.find(functionName);
            declaration != m_functionDeclarations.end()) {
            const auto* function = declaration->second;
            if (!function->clauses.empty() &&
                function->clauses.front().returnAnnotation) {
                std::unordered_map<std::string, TypePtr> generics;
                return resolveTypeExpr(**function->clauses.front().returnAnnotation,
                                       generics);
            }
            error(query.argument->location,
                  "`Type.returnedBy` needs `" + sourceName(functionName) +
                  "` to declare its return type");
            return Type::unknown();
        }
        error(query.argument->location,
              "`Type.returnedBy` needs the NAME of a function; a lambda or a "
              "function value carries no signature");
        return Type::unknown();
    }
    if (candidates.size() > 1) {
        std::string message =
            "`Type.returnedBy` cannot choose between the overloads of `" +
            sourceName(functionName) + "`";
        for (const auto& candidate : candidates)
            message += "\n\n" + displaySignature(functionName, candidate);
        error(query.argument->location, message);
        return Type::unknown();
    }
    return candidates.front().result;
}

// The type a `let`/`var` annotation declares, after checking the initializer
// against it. Null when there is no annotation.
auto TypeChecker::declaredBindingType(const std::optional<ast::TypeExprPtr>& annotation,
                                      const TypePtr& valueType,
                                      SourceLocation loc,
                                      const ast::Expr* value) -> TypePtr {
    if (!annotation || !*annotation) return nullptr;
    std::unordered_map<std::string, TypePtr> generics;
    auto declared = resolveTypeExpr(**annotation, generics);
    if (!declared) return nullptr;
    auto actual = resolve(valueType);
    // A gradual initializer (`?`, a type variable) is accepted: pinning it
    // down is exactly what the annotation is for. `This` — the trait
    // placeholder — has no substitution mechanism yet and leaks out of trait
    // default methods as a literal type name, so it is treated the same way
    // (checkCall bails on it for the same reason).
    const bool unsubstitutedThis = isThisPlaceholder(actual);
    if (deferIfPending(actual, declared, loc)) return declared;
    // `let x: Tuplex = (5, 11)`: a tuple written out under a distinct type's
    // annotation is that type, as `(5, 11).as(Tuplex)` would make it, when
    // the backing type admits it. Only a literal: a tuple computed elsewhere
    // still converts with `.as`, so a distinct type is never entered by
    // accident.
    if (value && std::holds_alternative<ast::TupleExpr>(value->kind))
        if (const auto backing = distinctBacking(declared);
            backing && !typesEqual(backing, declared) &&
            argMatchesParam(actual, backing))
            return declared;
    if (value && literalCoercesTo(*value, declared)) return declared;
    if (!std::holds_alternative<UnknownType>(actual->kind) &&
        !std::holds_alternative<TypeVar>(actual->kind) && !unsubstitutedThis &&
        !argMatchesParam(actual, declared))
        typeMismatch(loc, declared, actual);
    return declared;
}

// The type a standalone `name : T` declaration gives a top-level constant,
// after checking its definition against it.
auto TypeChecker::declaredConstantType(const std::string& name,
                                       const TypePtr& valueType,
                                       SourceLocation loc) -> TypePtr {
    auto declared = m_userSignatures.find(name);
    if (declared == m_userSignatures.end() || declared->second.size() != 1)
        return nullptr;
    const auto& signature = declared->second.front();
    if (!signature.params.empty() || !signature.result) return nullptr;
    auto actual = resolve(valueType);
    const bool unsubstitutedThis = isThisPlaceholder(actual);
    if (deferIfPending(actual, signature.result, loc)) return signature.result;
    if (!std::holds_alternative<UnknownType>(actual->kind) &&
        !std::holds_alternative<TypeVar>(actual->kind) && !unsubstitutedThis &&
        !argMatchesParam(actual, signature.result))
        typeMismatch(loc, signature.result, actual);
    return signature.result;
}

auto TypeChecker::displayTypeOf(const ast::Expr* expr) const -> TypePtr {
    std::function<TypePtr(const TypePtr&)> widen =
        [&](const TypePtr& type) -> TypePtr {
        if (!type) return type;
        auto widenAll = [&](std::vector<TypePtr> types) {
            for (auto& element : types) element = widen(element);
            return types;
        };
        if (auto* named = std::get_if<NamedType>(&type->kind)) {
            // A parameterized name is a type application, not a nullary ADT
            // constructor occurrence.  In particular, Process<Message> must
            // not widen through the unrelated prelude constructor `Process`
            // to its owning `Feature` ADT in editor-facing types.
            if (const auto* owner = constructorOwner(*named);
                owner && *owner != named->name)
                return Type::named(*owner);
            // Type ARGUMENTS are left alone: a phantom typestate parameter is
            // spelled with constructors too (`FileHandle<Write>`), and
            // widening those to their ADT (`FileHandle<WritePermission>`)
            // erases exactly the distinction the parameter exists to make.
            return type;
        }
        if (auto* list = std::get_if<ListType>(&type->kind))
            return Type::list(widen(list->element));
        if (auto* optional = std::get_if<OptionalType>(&type->kind))
            return Type::optional(widen(optional->inner));
        if (auto* map = std::get_if<MapType>(&type->kind))
            return Type::map(widen(map->key), widen(map->value));
        if (auto* tuple = std::get_if<TupleType>(&type->kind))
            return Type::tuple(widenAll(tuple->elements));
        return type;
    };
    return widen(typeOf(expr));
}

auto TypeChecker::satisfiesTrait(const TypePtr& type,
                                 const std::string& traitName) const -> bool {
    if (m_traits.satisfies(type, traitName)) return true;
    // A nullary constructor's value type is the constructor name (`Dog`,
    // `Meter`), but the conformance is declared on the ADT that owns it
    // (`make Animal, implement: Speaker`). Lift to the owner and retry —
    // otherwise `describe(Dog)` rejects a value the trait plainly covers.
    auto* named = std::get_if<NamedType>(&type->kind);
    if (!named) return false;
    auto ownerSatisfies = [&](const std::string& ownerName) {
        return ownerName != named->name &&
               m_traits.satisfies(Type::named(ownerName), traitName);
    };
    if (const auto* owner = constructorOwner(*named);
        owner && ownerSatisfies(*owner))
        return true;
    // Constructors of a `using`-imported module are registered per module
    // rather than in the file-local ADT map.
    for (const auto& [module, constructors] : m_moduleConstructors) {
        auto constructor = constructors.find(named->name);
        if (constructor != constructors.end() &&
            moduleMemberImported(module, named->name) &&
            ownerSatisfies(constructor->second.typeName))
            return true;
    }
    return false;
}

auto TypeChecker::literalCoercesTo(const ast::Expr& value, const TypePtr& target) const
    -> bool {
    if (!target) return false;
    const auto resolved = resolve(target);
    if (const auto* optional = std::get_if<OptionalType>(&resolved->kind))
        return literalCoercesTo(value, optional->inner);
    if (const auto* list = std::get_if<ListType>(&resolved->kind)) {
        const auto* written = std::get_if<ast::ListExpr>(&value.kind);
        if (!written || written->rest || written->elements.empty()) return false;
        for (const auto& element : written->elements)
            if (!element || !literalCoercesTo(*element, list->element)) return false;
        return true;
    }
    const auto backing = distinctBacking(resolved);
    if (typesEqual(backing, resolved)) return false;
    const bool literal =
        std::holds_alternative<ast::IntLiteral>(value.kind) ||
        std::holds_alternative<ast::FloatLiteral>(value.kind) ||
        std::holds_alternative<ast::StringLiteral>(value.kind) ||
        [&] {
            const auto* negated = std::get_if<ast::UnaryOp>(&value.kind);
            return negated && negated->op == TokenType::Minus &&
                   negated->operand &&
                   (std::holds_alternative<ast::IntLiteral>(negated->operand->kind) ||
                    std::holds_alternative<ast::FloatLiteral>(negated->operand->kind));
        }();
    if (!literal) return false;
    const auto written = typeOf(&value);
    return written && argMatchesParam(written, backing);
}

auto TypeChecker::argMatchesParam(const TypePtr& argType, const TypePtr& paramType) const -> bool {
    // `Block<A…, R>` still spelled as a NAMED type is the block function type
    // it stands for; every resolver that knows it builds that. One spelling
    // reached a record field unconverted, and `Just({ DateTime.now() })` for
    // Mock.Clock's `onNow` was then rejected once `Just(…)` had a type.
    const auto asBlock = [](const TypePtr& type) -> TypePtr {
        const auto* named = std::get_if<NamedType>(&type->kind);
        if (!named || named->name != "Block" || named->typeArgs.empty())
            return nullptr;
        std::vector<TypePtr> params(named->typeArgs.begin(),
                                    named->typeArgs.end() - 1);
        return Type::func(std::move(params), named->typeArgs.back(), true);
    };
    if (const auto block = asBlock(paramType)) return argMatchesParam(argType, block);
    if (const auto block = asBlock(argType)) return argMatchesParam(block, paramType);
    auto isPermissive = [](const TypePtr& t) {
        return isUndetermined(t);
    };
    if (isPermissive(argType) || isPermissive(paramType)) return true;
    // A distinct type goes where its backing type is expected — a `UserId`
    // to a `String` parameter, or to a text column's `==` — but never the
    // other way: a `String` still enters a `UserId` only through `.as`, and
    // one distinct type never stands for another.
    if (const auto backing = distinctBacking(argType);
        !typesEqual(backing, argType) &&
        typesEqual(distinctBacking(paramType), paramType) &&
        argMatchesParam(backing, paramType))
        return true;
    // Never is the bottom type — a Never-typed expression (never returns) is
    // compatible with any expected type.
    if (std::holds_alternative<VoidType>(argType->kind)) return true;
    // An intersection in expected position is a conjunction: the same actual
    // value must satisfy every member.
    if (auto* intersection =
            std::get_if<IntersectionType>(&paramType->kind)) {
        for (const auto& member : intersection->members)
            if (!argMatchesParam(argType, member)) return false;
        return true;
    }
    // An intersection value may be forgotten to any one of its components.
    if (auto* intersection = std::get_if<IntersectionType>(&argType->kind)) {
        for (const auto& member : intersection->members)
            if (argMatchesParam(member, paramType)) return true;
        return false;
    }
    // A union can flow to an expected type only when every possible member
    // can. This is the directional dual of the expected-union rule below.
    if (auto* unionType = std::get_if<UnionType>(&argType->kind)) {
        for (const auto& member : unionType->members)
            if (!argMatchesParam(member, paramType)) return false;
        return true;
    }
    // Open structural records are record-only and use width compatibility.
    // Field types are deliberately invariant in the first slice.
    if (auto* required = std::get_if<RecordType>(&paramType->kind)) {
        auto fieldMatches = [&](const auto& available) {
            for (const auto& [name, expected] : required->fields) {
                auto found = available.find(name);
                if (found == available.end() ||
                    !typesEqual(found->second, expected))
                    return false;
            }
            return true;
        };
        if (auto* named = std::get_if<NamedType>(&argType->kind)) {
            auto record =
                m_recordFields.find(resolveRecordName(named->name));
            if (record == m_recordFields.end()) return false;
            std::unordered_map<std::string, TypePtr> fields;
            for (const auto& [name, fieldType] : record->second)
                fields[name] = substituteInterfaceGenerics(
                    fieldType, named->typeArgs);
            return fieldMatches(fields);
        }
        if (auto* actual = std::get_if<RecordType>(&argType->kind)) {
            std::unordered_map<std::string, TypePtr> fields;
            for (const auto& [name, fieldType] : actual->fields)
                fields[name] = fieldType;
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
        paramNamed && paramNamed->name == "Tuple" && paramNamed->typeArgs.empty())
        return std::holds_alternative<TupleType>(argType->kind);
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        if (m_traits.get(paramNamed->name)) {
            if (auto* argNamed = std::get_if<NamedType>(&argType->kind);
                argNamed && argNamed->name == paramNamed->name)
                return true; // trait-typed value matches trait param
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
    if (m_traits.satisfies(argType, "Integer") && m_traits.satisfies(paramType, "Integer")) return true;
    if (m_traits.satisfies(argType, "Float") && m_traits.satisfies(paramType, "Float")) return true;

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
        if (!argTuple || argTuple->elements.size() != paramTuple->elements.size()) return false;
        for (size_t i = 0; i < paramTuple->elements.size(); i++) {
            if (!argMatchesParam(argTuple->elements[i], paramTuple->elements[i])) return false;
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
            argNamed && argNamed->typeArgs.empty() && argNamed->name == "None")
            return true;
        auto* argOpt = std::get_if<OptionalType>(&argType->kind);
        return argOpt && argMatchesParam(argOpt->inner, paramOpt->inner);
    }
    if (auto* argOpt = std::get_if<OptionalType>(&argType->kind)) {
        return argMatchesParam(argOpt->inner, paramType);
    }
    auto isUnitLike = [](const TypePtr& t) -> bool {
        if (auto* p = std::get_if<PrimitiveType>(&t->kind))
            return p->kind == PrimitiveType::Unit;
        if (auto* tup = std::get_if<TupleType>(&t->kind))
            return tup->elements.empty();
        if (auto* n = std::get_if<NamedType>(&t->kind))
            return n->typeArgs.empty() && n->name == "Void";
        return false;
    };
    auto isStringType = [](const TypePtr& t) -> bool {
        if (auto* n = std::get_if<NamedType>(&t->kind))
            return n->typeArgs.empty() &&
                   (n->name == "String" || n->name == "FilePath");
        if (auto* p = std::get_if<PrimitiveType>(&t->kind))
            return p->kind == PrimitiveType::String;
        return false;
    };
    // FunctionType param — e.g. `(T-1) -> Bool` vs `(T81) -> Bool`. Without
    // this branch both sides fall to typesEqual which fails on mismatched
    // TypeVar ids even though both are permissive. Recurse into params and
    // result so lambda arguments to map/filter/each pass the checker.
    if (std::holds_alternative<FuncType>(paramType->kind)) {
        auto* argFn = std::get_if<FuncType>(&argType->kind);
        if (!argFn) return isPermissive(argType);
        // `(A) -> (B) -> C` and `(A, B) -> C` are the same callable shape
        // in Kex. Normalize both sides before comparing so a multi-parameter
        // lambda matches a curried source annotation (and vice versa).
        auto flatten = [](const TypePtr& type) {
            std::pair<std::vector<TypePtr>, TypePtr> result{{}, type};
            auto current = type;
            while (auto* fn = std::get_if<FuncType>(&current->kind)) {
                result.first.insert(
                    result.first.end(), fn->params.begin(), fn->params.end());
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
            if (paramParams.size() != 1 || argParams.size() < 2) return false;
            if (auto* spread = std::get_if<TupleType>(&argParams[0]->kind)) {
                if (spread->elements.size() != argParams.size()) return false;
            } else if (!isPermissive(paramParams[0]) ||
                       !std::all_of(argParams.begin(), argParams.end(),
                                    [&](const TypePtr& p) {
                                        return isPermissive(p);
                                    })) {
                // Something IS known and it does not line up: a real arity
                // error, still reported. Only the case where neither side
                // knows anything is let through — `@conditions.each do |c, v|`
                // over a record field the checker has not resolved to a tuple.
                // Rejecting there would fail a program both backends run.
                return false;
            }
            if (isUnitLike(paramResult)) return true;
            return argMatchesParam(argResult, paramResult);
        }
        for (size_t i = 0; i < paramParams.size(); i++)
            // Function parameters are contravariant: a callback offered to a
            // context must accept everything the context may pass.
            if (!argMatchesParam(paramParams[i], argParams[i])) return false;
        // Void as the expected return means "result discarded" — accept any body type.
        if (isUnitLike(paramResult)) return true;
        return argMatchesParam(argResult, paramResult);
    }
    // NamedType with type args — e.g. `Range<Number>` param vs `Range<Integer>` arg.
    // Recurse into type arguments structurally so the inner types get the same
    // trait-relaxation treatment (argMatchesParam, not typesEqual).
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        auto* argNamed = std::get_if<NamedType>(&argType->kind);
        // A bare name suffix-matches a qualified one (`Version` is
        // `Kex.Version`), but a constructor's singleton type is no spelling
        // of a type at all: NetErrorKind's `Backend` is not a `Kex.Backend`.
        const bool sameType = argNamed &&
            (argNamed->name == paramNamed->name ||
             (!constructorOwner(*argNamed) &&
              namedTypesMatch(argNamed->name, paramNamed->name)));
        if (!sameType) {
            // A nullary ADT constructor is a refined value of its parent
            // type. This relationship comes from the ADT registry rather
            // than from any constructor spelling.
            if (argNamed && argNamed->typeArgs.empty() &&
                paramNamed->typeArgs.empty()) {
                if (const auto* owner = constructorOwner(*argNamed);
                    owner && *owner == paramNamed->name)
                    return true;
                // m_adtOfConstructor keeps ONE owner per constructor, so a
                // record named by two unions belongs only to whichever was
                // declared last — adding `type Pair = Alpha | Beta` below
                // `type Answer = Alpha | String` stopped `Alpha` from being
                // an `Answer`. Membership is the question, so ask the union.
                if (m_recordFields.count(resolveRecordName(argNamed->name)))
                    if (auto members = m_adtVariants.find(paramNamed->name);
                        members != m_adtVariants.end())
                        for (const auto& member : members->second)
                            if (namedTypesMatch(argNamed->name, member))
                                return true;
                // The same constructor/ADT relationship must survive a
                // `using` boundary. Imported nullary variants are recorded
                // per provider module rather than in m_adtOfConstructor.
                for (const auto& [module, constructors] : m_moduleConstructors) {
                    for (const auto& [constructor, imported] : constructors) {
                        if (imported.typeName != paramNamed->name) continue;
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
                            if (constructor != argNamed->name) continue;
                            if (!moduleMemberImported(module, constructor) &&
                                !makeModuleVisible(module))
                                continue;
                        }
                        return true;
                    }
                }
            }
            if (isStringType(argType) && isStringType(paramType)) return true;
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
                    members != m_adtVariants.end())
                    for (const auto& member : members->second)
                        if (namedTypesMatch(actual, member)) return true;
                // The same union declared inside a MODULE is registered per
                // provider module instead, so `type Reply = String | Safe`
                // in `module Web` has to answer for its String the same way.
                for (const auto& [module, constructors] : m_moduleConstructors)
                    for (const auto& [constructor, imported] : constructors) {
                        if (imported.typeName != paramNamed->name) continue;
                        if (!namedTypesMatch(actual, constructor)) continue;
                        if (!moduleMemberImported(module, constructor) &&
                            !makeModuleVisible(module))
                            continue;
                        return true;
                    }
            }
            return false;
        }
        // An unparameterized receiver declaration is the erased/wildcard
        // form of the named type. For example, `make Range` methods apply to
        // inferred `Range<Integer>` and `Range<Char>` values.
        if (paramNamed->typeArgs.empty()) return true;
        // The same for a value of the type written without its arguments
        // (`Ask(Process)`): a `Process<A>` parameter takes it.
        if (argNamed->typeArgs.empty()) return true;
        if (argNamed->typeArgs.size() != paramNamed->typeArgs.size()) return false;
        for (size_t i = 0; i < paramNamed->typeArgs.size(); i++) {
            if (!argMatchesParam(argNamed->typeArgs[i], paramNamed->typeArgs[i])) return false;
        }
        return true;
    }
    // UnionType param (e.g. type alias `Level = :a | :b | :c`) — arg
    // matches if it matches any branch of the union.
    if (auto* paramUnion = std::get_if<UnionType>(&paramType->kind)) {
        for (const auto& member : paramUnion->members) {
            if (member && argMatchesParam(argType, member)) return true;
        }
        return false;
    }

    if (isUnitLike(argType) && isUnitLike(paramType)) return true;

    if (isStringType(argType) && isStringType(paramType)) return true;

    return typesEqual(argType, paramType);
}

auto TypeChecker::hasAnnotatedSignatureForReceiver(
    const std::string& name, const TypePtr& receiverType) const -> bool {
    auto it = m_annotatedReceiverKeys.find(name);
    if (it == m_annotatedReceiverKeys.end()) return false;
    return it->second.count(typeToString(receiverType)) > 0;
}

auto TypeChecker::displaySignature(const std::string& name, const Signature& sig) const -> std::string {
    // Collect TypeVar IDs in order of first appearance, then remap to
    // sequential negative IDs so they display as A, B, C, ... regardless
    // of the internal counter state when the signature was built.
    std::unordered_map<int, int> varRemap;
    int nextNeg = -1;
    std::function<void(const TypePtr&)> collectVars = [&](const TypePtr& t) {
        if (!t) return;
        if (const auto* var = std::get_if<TypeVar>(&t->kind)) {
            if (!varRemap.count(var->id)) varRemap[var->id] = nextNeg--;
            return;
        }
        forEachTypeChild(t, collectVars);
    };
    for (const auto& p : sig.params) collectVars(p);
    collectVars(sig.result);

    std::function<TypePtr(const TypePtr&)> remap = [&](const TypePtr& t) -> TypePtr {
        if (!t) return t;
        return std::visit([&](const auto& node) -> TypePtr {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeVar>) {
                auto it = varRemap.find(node.id);
                return it != varRemap.end() ? Type::typeVar(it->second) : t;
            } else {
                return mapTypeChildren(t, remap);
            }
        }, t->kind);
    };

    auto displayType = [&](const TypePtr& t) -> std::string {
        // Unknown is the checker's gradual internal representation. In a
        // public callable signature it corresponds to source-level `Any`, not
        // an inference failure named "unknown".
        if (std::holds_alternative<UnknownType>(t->kind)) return "Any";
        if (auto* constrained = std::get_if<ConstrainedType>(&t->kind)) return constrained->traitName;
        return typeToString(remap(t));
    };
    std::string result = sourceName(name) + " : ";
    for (const auto& param : sig.params) {
        auto text = displayType(param);
        // A function-typed parameter needs parentheses of its own, or its
        // arrow merges into the signature's: `filter : [A] -> (A -> Bool) -> [A]`
        // rather than `[A] -> A -> Bool -> [A]`, which reads as three
        // parameters.
        if (param && std::holds_alternative<FuncType>(param->kind))
            text = "(" + text + ")";
        result += text + " -> ";
    }
    // The result needs the same parenthesization as a function-typed
    // parameter: `mk : String -> (String -> String)`, not
    // `mk : String -> String -> String`, which reads as a flat 2-parameter
    // function rather than a 1-parameter function returning one (#350).
    auto resultText = displayType(sig.result);
    if (sig.result && std::holds_alternative<FuncType>(sig.result->kind))
        resultText = "(" + resultText + ")";
    result += resultText;
    return result;
}

auto TypeChecker::checkCall(const std::string& name, const std::vector<TypePtr>& suppliedArgTypes,
                            SourceLocation loc, bool isMethodCall,
                            const ast::MethodCall* methodCall,
                            const ast::Expr* callExpr,
                            const std::vector<const ast::Expr*>* argumentExprs) -> TypePtr {
    auto argTypes = suppliedArgTypes;
    // A field of a structural record answers a call on it before any
    // method of the same name another type has (`r.name`).
    if (isMethodCall && argTypes.size() == 1 && !kex::mangle::hasScope(name))
        if (auto field = structuralField(resolve(argTypes.front()), name)) {
            if (callExpr)
                m_decidedCalls[callExpr] = {CallKind::FieldRead,
                                            typeToString(resolve(argTypes.front()))};
            return field;
        }
    // The arguments as written, beside their types, so a literal can go
    // straight into a distinct type it backs (literalCoercesTo). Left empty
    // when they do not line up with argTypes one for one.
    // Laid out as the callers lay out argTypes: positional arguments, then
    // named ones as written, then the block, which is never a literal.
    std::vector<const ast::Expr*> argExprs;
    const auto layOut = [&](const auto& call) {
        for (const auto& arg : call.args) argExprs.push_back(arg.get());
        for (const auto& [_, arg] : call.namedArgs) argExprs.push_back(arg.get());
        if (call.block) argExprs.push_back(nullptr);
    };
    if (methodCall) {
        argExprs.push_back(methodCall->receiver.get());
        layOut(*methodCall);
        if (argExprs.size() == argTypes.size() + 1) argExprs.erase(argExprs.begin());
    } else if (callExpr) {
        if (const auto* call = std::get_if<ast::FunctionCall>(&callExpr->kind))
            layOut(*call);
    } else if (argumentExprs) {
        argExprs = *argumentExprs;
    }
    if (argExprs.size() != argTypes.size()) argExprs.clear();
    // A type named as the argument of a `Type<X>` parameter is that type as
    // a value (`m.to(String, in: Hour)`).
    const auto selectedType = [&](std::size_t i, const TypePtr& param) -> TypePtr {
        const auto* wanted = std::get_if<NamedType>(&resolve(param)->kind);
        if (!wanted || wanted->name != kex::names::kTypeRecord ||
            wanted->typeArgs.size() != 1 || i >= argExprs.size() || !argExprs[i])
            return nullptr;
        // A written type (`to([Integer])`, `to(List<Integer>)`) is the
        // call's `targetType`; its argument names only the head.
        TypePtr named;
        if (methodCall && methodCall->targetType && !methodCall->args.empty() &&
            argExprs[i] == methodCall->args.front().get()) {
            std::unordered_map<std::string, TypePtr> generics;
            named = resolveTypeExpr(*methodCall->targetType, generics);
        } else {
            named = typeNameReference(*argExprs[i]);
        }
        return named ? Type::named(kex::names::kTypeRecord, {named}) : nullptr;
    };
    const auto fits = [&](std::size_t i, const TypePtr& param) {
        if (auto selected = selectedType(i, param))
            return argMatchesParam(selected, param);
        return argMatchesParam(argTypes[i], param) ||
               (i < argExprs.size() && argExprs[i] &&
                literalCoercesTo(*argExprs[i], param));
    };
    // A field promised by an open record is known more specifically than an
    // unrelated global/UFCS method with the same name. Concrete record fields
    // already receive this priority later; structural receivers need it here
    // because they have no nominal registry entry to trigger that path.
    if (isMethodCall && argTypes.size() == 1) {
        auto structuralField = [&](const auto& self, const TypePtr& type)
            -> TypePtr {
            auto resolved = resolve(type);
            if (auto* record = std::get_if<RecordType>(&resolved->kind))
                for (const auto& [fieldName, fieldType] : record->fields)
                    if (fieldName == name) return fieldType;
            if (auto* intersection =
                    std::get_if<IntersectionType>(&resolved->kind))
                for (const auto& member : intersection->members)
                    if (auto found = self(self, member)) return found;
            return nullptr;
        };
        if (auto field = structuralField(structuralField, argTypes.front()))
            return field;
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
        std::string receiverTrait;
        if (auto* constrained = std::get_if<ConstrainedType>(&receiver->kind))
            receiverTrait = constrained->traitName;
        else if (auto* named = std::get_if<NamedType>(&receiver->kind);
                 named && named->typeArgs.empty() &&
                 !m_recordFields.count(named->name) &&
                 !m_adtVariants.count(named->name))
            receiverTrait = named->name;
        // A method the receiver's trait requires or defaults: dispatched on
        // the value at run time, through that trait.
        const auto viaTrait = [&](const std::string& trait) {
            if (callExpr)
                m_decidedCalls[callExpr] = {CallKind::TraitMethod, trait};
        };
        if (!receiverTrait.empty())
            if (const TraitDef* trait = m_traits.get(receiverTrait)) {
                for (const auto& required : trait->requiredMethods)
                    if (required.name == name &&
                        argTypes.size() == required.params.size() + 1) {
                        viaTrait(receiverTrait);
                        return required.result;
                    }
                if (std::find(trait->defaultMethods.begin(),
                              trait->defaultMethods.end(), name) !=
                    trait->defaultMethods.end()) {
                    viaTrait(receiverTrait);
                    return Type::unknown();
                }
            }
        if (auto* intersection =
                std::get_if<IntersectionType>(&receiver->kind)) {
            for (const auto& member : intersection->members) {
                std::string traitName;
                if (auto* named = std::get_if<NamedType>(&member->kind))
                    traitName = named->name;
                else if (auto* constrained =
                             std::get_if<ConstrainedType>(&member->kind))
                    traitName = constrained->traitName;
                if (traitName.empty()) continue;
                if (const TraitDef* trait = m_traits.get(traitName))
                    for (const auto& required : trait->requiredMethods)
                        if (required.name == name &&
                            argTypes.size() == required.params.size() + 1) {
                            viaTrait(traitName);
                            return required.result;
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
        if (resolvedLocal && callExpr)
            m_decidedCalls[callExpr] = {CallKind::ValueCall, name};
        if (resolvedLocal)
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
                const auto* call = callExpr
                    ? std::get_if<ast::FunctionCall>(&callExpr->kind) : nullptr;
                if (call && !call->namedArgs.empty()) {
                    const std::vector<std::string>* paramNames = nullptr;
                    if (auto origin = m_curryOrigin.find(name);
                        origin != m_curryOrigin.end())
                        if (auto sigs = m_userSignatures.find(origin->second);
                            sigs != m_userSignatures.end())
                            for (const auto& sig : sigs->second)
                                if (sig.params.size() == function->params.size() &&
                                    sig.paramNames.size() == sig.params.size()) {
                                    paramNames = &sig.paramNames;
                                    break;
                                }
                    if (paramNames) {
                        reordered.assign(function->params.size(), nullptr);
                        std::vector<bool> filled(function->params.size(), false);
                        for (size_t i = 0; i < call->namedArgs.size(); ++i) {
                            const auto& label = call->namedArgs[i].first;
                            for (size_t p = 0; p < paramNames->size(); ++p)
                                if ((*paramNames)[p] == label &&
                                    call->args.size() + i < argTypes.size()) {
                                    reordered[p] = argTypes[call->args.size() + i];
                                    filled[p] = true;
                                    break;
                                }
                        }
                        size_t next = 0;
                        for (size_t i = 0; i < call->args.size() &&
                                           i < argTypes.size(); ++i) {
                            while (next < reordered.size() && filled[next]) next++;
                            if (next >= reordered.size()) break;
                            reordered[next] = argTypes[i];
                            filled[next] = true;
                            next++;
                        }
                        if (std::all_of(filled.begin(), filled.end(),
                                        [](bool b) { return b; }))
                            effective = &reordered;
                    }
                }
                if (function->params.size() != effective->size()) {
                    error(loc, "`" + sourceName(name) + "` expects " +
                        std::to_string(function->params.size()) +
                        " argument(s), got " + std::to_string(effective->size()));
                    return function->result;
                }
                for (size_t i = 0; i < effective->size(); ++i)
                    if ((*effective)[i] &&
                        !argMatchesParam((*effective)[i], function->params[i]))
                        typeMismatch(loc, function->params[i], (*effective)[i]);
                return function->result;
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
        if (resolvedLocal)
            if (auto* var = std::get_if<TypeVar>(&resolvedLocal->kind);
                var && var->id >= 0 && var->id >= m_clauseVarMark) {
                auto result = freshTypeVar();
                unifyVar(var->id, Type::func(argTypes, result));
                return result;
            }
    }
    const std::vector<Signature>* userSignatures = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() &&
        scoped != m_scopedDeclaredSignatures.end())
        userSignatures = &scoped->second;
    // The same preference for a declaration the module owns WITHOUT an
    // annotation (`let render = Template.html(...)`): it is published only
    // under its qualified name, so the bare-name lookup below would pick
    // another module's same-named function instead — `Template.render`,
    // once the Template source is merged into the build (kexhq/kex#385).
    else if (auto own = m_userSignatures.find(kex::mangle::qualify(m_currentModulePath, name));
             !isMethodCall && !m_currentModulePath.empty() &&
             !kex::mangle::hasScope(name) &&
             own != m_userSignatures.end() && !own->second.empty())
        userSignatures = &own->second;
    else if (auto user = m_userSignatures.find(name);
             user != m_userSignatures.end())
        userSignatures = &user->second;
    // The bare-name table holds every module's free functions. A plain call
    // reaches only the ones its module may see: a name `using Router, only:
    // [get]` left out (`put`) is not borrowed from Router to fail at run
    // time (#496), and another module's private `table` does not stand in
    // for the `table` this module imported (#494).
    std::vector<Signature> visibleUserSignatures;
    if (userSignatures && !isMethodCall &&
        !kex::mangle::hasScope(name)) {
        std::set<std::string> hiddenModules;
        for (const auto& signature : *userSignatures) {
            if (freeFunctionVisible(signature.declaringModule, name))
                visibleUserSignatures.push_back(signature);
            else
                hiddenModules.insert(signature.declaringModule);
        }
        // Inside a module, the program's top-level function steps aside for
        // one the module declares or imports by that name: `main.kex`'s own
        // `table` took the place of the `table` a module imported with
        // `using App.Shapes, only: [table]` (#494).
        const auto topLevel = [](const Signature& signature) {
            return signature.declaringModule.empty();
        };
        if (!m_currentModulePath.empty() &&
            std::any_of(visibleUserSignatures.begin(),
                        visibleUserSignatures.end(), topLevel) &&
            !std::all_of(visibleUserSignatures.begin(),
                         visibleUserSignatures.end(), topLevel)) {
            std::erase_if(visibleUserSignatures, topLevel);
            hiddenModules.insert("");
        }
        if (!hiddenModules.empty())
            userSignatures = visibleUserSignatures.empty()
                ? nullptr : &visibleUserSignatures;
        // Nothing in scope takes these arguments, but a module's function
        // that is not in scope would: say that, rather than reporting a
        // mismatch against some unrelated prelude function of the same name.
        const auto accepts = [&](const Signature& signature) {
            const auto required =
                signature.requiredParams.value_or(signature.params.size());
            if (argTypes.size() < required ||
                argTypes.size() > signature.params.size())
                return false;
            for (size_t i = 0; i < argTypes.size(); ++i)
                if (argTypes[i] && !fits(i, signature.params[i]))
                    return false;
            return true;
        };
        // Only where the resolver let the name through — because some other
        // function of that name exists (`put` on Process.Shared); a name
        // nothing in scope declares it has reported already.
        if (!userSignatures && !hiddenModules.empty()) {
            const auto others = importedCandidateSignatures(name);
            if (!others.empty() &&
                std::none_of(others.begin(), others.end(), accepts)) {
                std::string owners;
                for (const auto& module : hiddenModules) {
                    const bool isPrivate =
                        m_privateFreeFunctions.contains(module + "\n" + name);
                    owners += (owners.empty() ? "" : ", ") +
                        std::string("`") + module + "." + name + "`" +
                        (isPrivate ? " is private to `" + module + "`"
                                   : " is not imported here");
                }
                error(loc, "Undefined function: `" + name + "` — " + owners);
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
            if (!methodCall || methodCall->parenthesized || methodCall->mutating)
                return false;
            for (const auto& [record, fields] : m_recordFields)
                if (fields.count(name)) return true;
            if (m_importedInterfaces)
                for (const auto& [record, fields] : m_importedInterfaces->recordFieldNames)
                    if (fields.count(name)) return true;
            return false;
        };
        if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
            if (auto record = m_recordFields.find(
                    resolveRecordName(named->name));
                record != m_recordFields.end())
                if (auto field = record->second.find(name);
                    field != record->second.end()) {
                    // Same substitution the main field-access path does: a
                    // `Box<Integer>` reads `.items` as `[Integer]`, not as
                    // the declaration's bare parameter slot.
                    if (callExpr)
                        m_decidedCalls[callExpr] = {
                            CallKind::FieldRead, resolveRecordName(named->name)};
                    return substituteInterfaceGenerics(field->second,
                                                       named->typeArgs);
                }
            // The same rule for a record the PRELUDE declares. Interfaces
            // carry field names but not their types, so the read is answered
            // gradually — the point is only that it is a field read at all,
            // and not a call to some same-named method: `moment.time.nanosecond`
            // is the Time record's field, never units.kex's `Integer.nanosecond`.
            if (m_importedInterfaces) {
                auto record =
                    m_importedInterfaces->recordFieldNames.find(named->name);
                if (record != m_importedInterfaces->recordFieldNames.end() &&
                    record->second.count(name)) {
                    if (callExpr)
                        m_decidedCalls[callExpr] = {CallKind::FieldRead,
                                                    named->name};
                    return Type::unknown();
                }
            }
        } else if (auto* var = std::get_if<TypeVar>(&receiver->kind);
                   var && var->id >= 0 && var->id >= m_clauseVarMark &&
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
            for (const auto& recName : m_programRecords)
                if (auto record = m_recordFields.find(recName);
                    record != m_recordFields.end())
                if (auto field = record->second.find(name);
                    field != record->second.end()) {
                    if (!matchedRecord.empty()) {
                        matchedRecord.clear();
                        break;
                    }
                    matchedRecord = recName;
                    matchedFieldType = field->second;
                }
            if (!matchedRecord.empty()) {
                unifyVar(var->id, Type::named(matchedRecord));
                if (callExpr)
                    m_decidedCalls[callExpr] = {CallKind::FieldRead,
                                                matchedRecord};
                return matchedFieldType;
            }
            // Several records may supply the field. That uncertainty does
            // not make an unrelated function its owner: keep the read
            // gradual and let runtime record dispatch select the slot.
            if (mayReadField()) return Type::unknown();
        } else if ((isUndetermined(receiver)) &&
                   mayReadField()) {
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
            auto inner = resolve(optional->inner);
            if (auto* innerNamed = std::get_if<NamedType>(&inner->kind)) {
                if (auto record = m_recordFields.find(
                        resolveRecordName(innerNamed->name));
                    record != m_recordFields.end() &&
                    record->second.count(name))
                    error(loc,
                          "`" + name + "` is a field of " +
                              typeToString(inner) + ", but the receiver is " +
                              typeToString(receiver) +
                              " — the value may be None. Map over the "
                              "optional first: `.map { |v| v." + name +
                              " }`");
                else if (m_importedInterfaces) {
                    auto record =
                        m_importedInterfaces->recordFieldNames.find(
                            innerNamed->name);
                    if (record != m_importedInterfaces->recordFieldNames.end() &&
                        record->second.count(name))
                        error(loc,
                              "`" + name + "` is a field of " +
                                  typeToString(inner) +
                                  ", but the receiver is " +
                                  typeToString(receiver) +
                                  " — the value may be None. Map over the "
                                  "optional first: `.map { |v| v." + name +
                                  " }`");
                }
            } else if (auto* innerRecord =
                           std::get_if<RecordType>(&inner->kind)) {
                if (std::any_of(innerRecord->fields.begin(),
                                innerRecord->fields.end(),
                                [&](const auto& field) {
                                    return field.first == name;
                                }))
                    error(loc,
                          "`" + name + "` is a field of " + typeToString(inner) +
                              ", but the receiver is " + typeToString(receiver) +
                              " — the value may be None. Map over the "
                              "optional first: `.map { |v| v." + name + " }`");
            }
        }
    }
    bool hasReceiverRefinementConflict = false;
    if (hasLocalMethods && !argTypes.empty()) {
        auto actual = resolve(argTypes[0]);
        for (const auto& sig : methodIt->second) {
            if (sig.params.empty()) continue;
            auto expected = resolve(sig.params[0]);
            auto* actualNamed = std::get_if<NamedType>(&actual->kind);
            auto* expectedNamed = std::get_if<NamedType>(&expected->kind);
            if (actualNamed && expectedNamed &&
                actualNamed->name == expectedNamed->name) {
                if (!argMatchesParam(actual, expected))
                    hasReceiverRefinementConflict = true;
            }
        }
    }

    std::vector<const ImportedFunction*> importedFunctions;
    std::string qualifiedModule;
    if (m_importedInterfaces) {
        if (kex::mangle::hasScope(name)) {
            qualifiedModule = kex::mangle::headOf(name);
            auto functionName = kex::mangle::tailOf(name);
            if (auto module = m_importedInterfaces->modules.find(qualifiedModule);
                module != m_importedInterfaces->modules.end())
                if (auto functions = module->second.exports.find(functionName);
                    functions != module->second.exports.end())
                    for (const auto& function : functions->second)
                        importedFunctions.push_back(&function);
        } else if (isMethodCall) {
            if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
                functions != m_importedInterfaces->receiverFunctions.end())
                for (const auto& function : functions->second)
                    if (importedFunctionVisible(function))
                        importedFunctions.push_back(&function);
            // A function of a module `using` brings in from a unit compiled
            // apart is a method on its first parameter, as one merged into
            // the program is (`text.scan(re)` under `using Regex`).
            // Only for a receiver it takes: `field.parse(text)` is Type.Field's,
            // and an unrelated `parse : OptionConfig -> ...` is no candidate.
            for (const auto* function : usingImportedMethods(name))
                if ((argTypes.empty() ||
                     argMatchesParam(argTypes.front(), function->signature.params.front())) &&
                    !offeredAlready(importedFunctions, *function))
                    importedFunctions.push_back(function);
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
            const bool declaredValueCalled = !argTypes.empty() &&
                ((hasUser && userSignatures->size() == 1 &&
                  userSignatures->front().params.empty()) ||
                 (!hasUser && m_topLevelValueNames.count(name)));
            const bool declaredShadowsImport = declaredValueCalled ||
                (hasUser &&
                 std::any_of(userSignatures->begin(), userSignatures->end(),
                             [&](const Signature& signature) {
                                 return signature.params.size() == argTypes.size();
                             }));
            if (!declaredShadowsImport) {
                for (const auto& [_, module] : m_importedInterfaces->modules) {
                    if (!module.automaticImport) continue;
                    if (auto functions = module.exports.find(name);
                        functions != module.exports.end())
                        for (const auto& function : functions->second)
                            importedFunctions.push_back(&function);
                }
                // And those of a module `using` brings in from a unit
                // compiled apart (`regex("-")` under `using Regex`).
                for (const auto* function : usingImportedMethods(name, false))
                    if (!offeredAlready(importedFunctions, *function))
                        importedFunctions.push_back(function);
            }
            if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
                !declaredShadowsImport &&
                functions != m_importedInterfaces->receiverFunctions.end())
                for (const auto& function : functions->second)
                    if (importedFunctionVisible(function))
                        importedFunctions.push_back(&function);
        }
    }
    // A module declared in the current compilation unit shadows a package
    // module with the same source identity. This matches runtime code-path
    // precedence and prevents imported ownership from hijacking local calls.
    if (!qualifiedModule.empty() && m_localModules.contains(qualifiedModule))
        importedFunctions.clear();
    // A receiver method hidden behind `using` can be the only overload whose
    // labelled arguments and types fit. Decide that here, from local methods
    // and imported interfaces, so the backends do not infer ownership.
    if (isMethodCall && methodCall && !methodCall->namedArgs.empty() &&
        !kex::mangle::hasScope(name) && !argTypes.empty() &&
        std::all_of(argTypes.begin(), argTypes.end(), [&](const TypePtr& type) {
            return !containsOpenType(resolve(type));
        })) {
        std::vector<std::string> labels;
        for (const auto& [label, _] : methodCall->namedArgs)
            labels.push_back(label);
        const auto writtenPositional = methodCall->args.size();
        const auto positional = writtenPositional + bool(methodCall->block);
        const auto accepts = [&](const Signature& sig,
                                 const std::vector<std::string>& paramNames) {
            auto names = paramNames;
            if (names.size() == sig.params.size() && !names.empty())
                names.erase(names.begin());
            if (sig.params.empty() || names.size() + 1 != sig.params.size() ||
                argTypes.size() != 1 + positional + labels.size() ||
                !fits(0, sig.params.front()))
                return false;
            const auto required = sig.requiredParams.value_or(sig.params.size());
            std::vector<bool> defaulted(names.size(), false);
            for (size_t i = required > 0 ? required - 1 : 0;
                 i < defaulted.size(); ++i)
                defaulted[i] = true;
            if (!namedArgumentsFit(names, defaulted, labels, positional))
                return false;
            const auto placed = placeArguments(names, labels, positional);
            for (size_t i = 0; i < positional; ++i)
                if (!fits(i < writtenPositional ? i + 1 : argTypes.size() - 1,
                          sig.params[placed.positionalSlots[i] + 1]))
                    return false;
            for (size_t i = 0; i < labels.size(); ++i)
                if (!fits(1 + writtenPositional + i,
                          sig.params[placed.labelSlots[i] + 1]))
                    return false;
            return true;
        };
        std::string hidden;
        if (m_importedInterfaces) {
            const auto imported = m_importedInterfaces->receiverFunctions.find(name);
            if (imported != m_importedInterfaces->receiverFunctions.end())
            for (const auto& candidate : imported->second)
                if (candidate.foreignTarget &&
                    !importedFunctionVisible(candidate) &&
                    accepts(candidate.signature, candidate.paramNames) &&
                    (hidden.empty() || candidate.sourceModule < hidden))
                    hidden = candidate.sourceModule;
        }
        if (hasLocalMethods)
            for (const auto& candidate : methodIt->second) {
                if (candidate.makeModule.empty() ||
                    makeModuleVisible(candidate.makeModule) ||
                    candidate.params.empty() ||
                    !accepts(candidate, candidate.paramNames))
                    continue;
                const auto* receiver = std::get_if<NamedType>(
                    &resolve(candidate.params.front())->kind);
                if (receiver && receiver->name.starts_with(
                                    candidate.makeModule + "."))
                    continue;
                if (hidden.empty() || candidate.makeModule < hidden)
                    hidden = candidate.makeModule;
            }
        if (!hidden.empty()) {
                const bool importedMatches = std::any_of(
                    importedFunctions.begin(), importedFunctions.end(),
                    [&](const ImportedFunction* candidate) {
                        return accepts(candidate->signature, candidate->paramNames);
                    });
                const bool localMatches = hasLocalMethods && std::any_of(
                    methodIt->second.begin(), methodIt->second.end(),
                    [&](const Signature& candidate) {
                        return makeModuleVisible(candidate.makeModule) &&
                            accepts(candidate, candidate.paramNames);
                    });
                const bool userMatches = hasUser && std::any_of(
                    userSignatures->begin(), userSignatures->end(),
                    [&](const Signature& candidate) {
                        return accepts(candidate, candidate.paramNames);
                    });
                if (!importedMatches && !localMatches && !userMatches) {
                    if (callExpr)
                        m_decidedCalls[callExpr] = {CallKind::Hidden, hidden};
                    return Type::unknown();
                }
        }
    }
    if (isMethodCall && !argTypes.empty()) {
        auto actual = resolve(argTypes[0]);
        auto* actualNamed = std::get_if<NamedType>(&actual->kind);
        if (actualNamed)
            for (const auto* function : importedFunctions) {
                if (function->signature.params.empty()) continue;
                auto expected = resolve(function->signature.params[0]);
                auto* expectedNamed = std::get_if<NamedType>(&expected->kind);
                if (expectedNamed && expectedNamed->name == actualNamed->name &&
                    !argMatchesParam(actual, expected)) {
                    hasReceiverRefinementConflict = true;
                    break;
                }
            }
    }

    auto sameParams = [](const Signature& left, const Signature& right) {
        if (left.params.size() != right.params.size()) return false;
        for (size_t i = 0; i < left.params.size(); i++)
            if (!typesEqual(left.params[i], right.params[i])) return false;
        return true;
    };
    auto matchesActual = [&](const Signature& signature) {
        if (signature.params.size() != argTypes.size()) return false;
        for (size_t i = 0; i < signature.params.size(); i++)
            if (!fits(i, signature.params[i])) return false;
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
                   std::any_of(offered.begin(), offered.end(),
                               [&](const ImportedFunction* method) {
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
        const auto receiver = argTypes.empty() ? Type::unknown()
            : resolve(argTypes.front());
        const auto* namedReceiver = std::get_if<NamedType>(&receiver->kind);
        std::erase_if(importedFunctions, [&](const ImportedFunction* function) {
            if (!namedReceiver || function->sourceModule != "Prelude" ||
                !matchesActual(function->signature)) return false;
            return std::any_of(owned.begin(), owned.end(),
                               [&](const ImportedFunction* method) {
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
    for (size_t i = 0; i < importedFunctions.size(); i++)
        for (size_t j = i + 1; j < importedFunctions.size(); j++) {
            const auto& left = *importedFunctions[i];
            const auto& right = *importedFunctions[j];
            if (left.backendModule != right.backendModule &&
                matchesActual(left.signature) && matchesActual(right.signature) &&
                sameParams(left.signature, right.signature)) {
                auto pair = left.backendModule < right.backendModule
                    ? std::make_pair(left.backendModule, right.backendModule)
                    : std::make_pair(right.backendModule, left.backendModule);
                if (!ambiguous || pair < *ambiguous) ambiguous = std::move(pair);
            }
        }
    if (ambiguous) {
        error(loc, "ambiguous imported " +
            std::string(isMethodCall && !kex::mangle::hasScope(name)
                ? "receiver function '" : "function '") + sourceName(name) +
            "' is provided by both '" + ambiguous->first + "' and '" +
            ambiguous->second + "'");
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
        if (copied.paramNames.empty() &&
            function->paramNames.size() == copied.params.size())
            copied.paramNames = function->paramNames;
    }

    std::vector<Signature> merged;
    const std::vector<Signature>* sigs = nullptr;
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
    bool useLocalMethods = hasLocalMethods &&
        (!m_inMakeBlock || hasReceiverRefinementConflict ||
         hasConcreteReceiver);
    // Permissive means "not decided here", not "the import": with this
    // unit's own methods held back, a same-named import was the only
    // candidate left, won, and bound the receiver to ITS type — `c.advance`
    // inside `make Cursor` became the prelude's `Parsing.Input.advance`, a
    // false "returns Parsing.Input" error, and a BEAM call that died on the
    // Cursor the walker handled. The value decides when the call runs.
    if (hasLocalMethods && !useLocalMethods && !importedSigs.empty()) {
        if (callExpr) decideDispatch(callExpr, name);
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
                if (!isMethodCall || argTypes.empty()) return {};
                auto* named = std::get_if<NamedType>(&resolve(argTypes[0])->kind);
                if (!named) return {};
                // `Process.spawn`'s result is `Server<T>`, not `T` itself, so
                // a slot call's receiver carries the WRAPPER's bare name
                // ("Server") here, never a dot — the exemption below always
                // missed it and fell back to requiring `T`'s module be
                // `using`-imported just to call a slot on a value whose type
                // already names it in full (`Server<Catalog.ServingShelf.
                // Catalogue>`), the one shape this whole check exists to
                // exempt. Unwrap to the carried type before reading its
                // module prefix, same as an ordinary (unwrapped) receiver.
                if (named->name == "Server" && named->typeArgs.size() == 1)
                    if (auto* inner = std::get_if<NamedType>(
                            &resolve(named->typeArgs.front())->kind))
                        named = inner;
                const auto dot = named->name.rfind('.');
                return dot == std::string::npos ? std::string{}
                                                : named->name.substr(0, dot);
            }();
            for (const auto& method : methodIt->second)
                if (makeModuleVisible(method.makeModule) ||
                    (!method.makeModule.empty() &&
                     method.makeModule == receiverModule))
                    merged.push_back(method);
            localSigCount = merged.size();
        }
        merged.insert(merged.end(), importedSigs.begin(), importedSigs.end());
        if (hasUser)
            merged.insert(
                merged.end(), userSignatures->begin(), userSignatures->end());
        // Every candidate may have been filtered out as out-of-scope (a
        // module-scoped `make` with no `using`). That is "no such method", not
        // "a method with no signatures" — fall through to the record-field
        // lookup and the undefined-method report below.
        if (!merged.empty()) sigs = &merged;
    } else if (hasUser) {
        sigs = userSignatures;
    }
    if (!sigs) {
        // Record field access: `user.name` desugars to checkCall("name", [User]).
        // Look up the field type in the record registry before giving up.
        // Resolve TypeVars first — the receiver may have been constrained by
        // a previous inference step.
        if (!argTypes.empty()) {
            auto receiver = resolve(argTypes[0]);
            if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
                auto ri = m_recordFields.find(resolveRecordName(named->name));
                if (ri != m_recordFields.end()) {
                    auto fi = ri->second.find(name);
                    if (fi != ri->second.end()) {
                        // A field holding a function is APPLIED when arguments
                        // follow it: `route.handler(request)` is a call, and
                        // its type is the handler's result, not the handler.
                        auto fieldType = substituteInterfaceGenerics(
                            fi->second, named->typeArgs);
                        for (size_t applied = 1; applied < argTypes.size();) {
                            auto* fn = std::get_if<FuncType>(&fieldType->kind);
                            if (!fn) break;
                            applied += std::max<size_t>(fn->params.size(), 1);
                            fieldType = fn->result;
                        }
                        if (callExpr)
                            m_decidedCalls[callExpr] = {
                                CallKind::FieldRead,
                                resolveRecordName(named->name)};
                        return fieldType;
                    }
                }
            }
            if (auto fieldType = structuralField(receiver, name)) {
                // A field of a structural record (`r: { name: String } & R`).
                if (callExpr)
                    m_decidedCalls[callExpr] = {CallKind::FieldRead, typeToString(receiver)};
                return fieldType;
            }
            // Trait-bounded receiver: `item.method()` where `item: SomeTrait`.
            // Look up the method in the trait's required methods to get return type.
            if (auto* ct = std::get_if<ConstrainedType>(&receiver->kind)) {
                if (const TraitDef* trait = m_traits.get(ct->traitName)) {
                    for (const auto& req : trait->requiredMethods) {
                        if (req.name == name) return req.result;
                    }
                }
            }
            if (auto* intersection =
                    std::get_if<IntersectionType>(&receiver->kind)) {
                for (const auto& member : intersection->members) {
                    auto* namedTrait = std::get_if<NamedType>(&member->kind);
                    if (!namedTrait) continue;
                    if (const TraitDef* trait = m_traits.get(namedTrait->name))
                        for (const auto& req : trait->requiredMethods)
                            if (req.name == name) return req.result;
                }
            }
            if (auto answered = claimedTraitAnswers(argTypes, name, callExpr))
                return answered;
            // If the field name is unambiguously defined on exactly one record
            // type, constrain a TypeVar receiver to that record type.
            if (std::holds_alternative<TypeVar>(receiver->kind)) {
                std::string matchedRecord;
                TypePtr matchedFieldType;
                for (const auto& [recName, fields] : m_recordFields) {
                    auto fi = fields.find(name);
                    if (fi != fields.end()) {
                        if (!matchedRecord.empty()) { matchedRecord.clear(); break; }
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
                        if (!fn) break;
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
        if (isMethodCall && concreteReceiver && !name.empty() &&
            !kex::mangle::hasScope(name) &&
            !std::isupper(static_cast<unsigned char>(name.front()))) {
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
            const auto* named = std::get_if<NamedType>(&receiver->kind);
            const bool fieldMayExist =
                std::holds_alternative<TypeVar>(argTypes[0]->kind) ||
                (!std::holds_alternative<PrimitiveType>(receiver->kind) &&
                 !(named && (isPrimitiveTypeName(named->name) ||
                             m_adtVariants.count(named->name) ||
                             constructorOwner(*named) ||
                             m_recordFields.count(resolveRecordName(named->name)))));
            m_unresolvedMethods.push_back(
                {name, loc, typeToString(receiver), fieldMayExist});
            auto& imports = m_unresolvedMethodImports.emplace_back(
                m_declarationImports);
            for (const auto& scope : m_importScopeStack)
                imports.insert(imports.end(), scope.begin(), scope.end());
        }
        // A default the receiver's type inherits from a trait it implements
        // has no signature of its own: the type answers it (OwnMethod).
        if (callExpr && isMethodCall && !argTypes.empty())
            if (const auto own = ownTypeAnswers(argTypes, name)) {
                m_decidedCalls[callExpr] = own->decision();
                return Type::unknown();
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
                methodIt->second.begin(), methodIt->second.end(),
                [&](const Signature& method) {
                    if (method.makeModule.empty() || makeModuleVisible(method.makeModule) ||
                        method.params.empty())
                        return false;
                    const auto* receiver =
                        std::get_if<NamedType>(&resolve(method.params.front())->kind);
                    if (receiver && receiver->name.starts_with(method.makeModule + "."))
                        return false;
                    if (hidden.empty() || method.makeModule < hidden)
                        hidden = method.makeModule;
                    return true;
                });
            if (allHidden && !hidden.empty())
                m_decidedCalls[callExpr] = {CallKind::Hidden, hidden};
        }
        return Type::unknown();  // unknown name, or not yet registered (forward/recursive ref)
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
        const auto* functionCall = callExpr
            ? std::get_if<ast::FunctionCall>(&callExpr->kind) : nullptr;
        const auto* named = functionCall ? &functionCall->namedArgs
                                        : methodCall ? &methodCall->namedArgs : nullptr;
        const bool throughReceiver =
            isMethodCall && !kex::mangle::hasScope(name);
        const std::size_t receiverSlots = throughReceiver ? 1 : 0;
        std::vector<const Signature*> layouts;
        for (const auto& sig : *sigs) {
            if (sig.paramNames.empty() ||
                sig.paramNames.size() + receiverSlots != sig.params.size())
                continue;
            // A receiver not yet known (a block's parameter) takes any.
            if (throughReceiver) {
                if (argTypes.empty()) continue;
                const auto receiver = resolve(argTypes.front());
                if (!containsOpenType(receiver) &&
                    !argMatchesParam(receiver, resolve(sig.params.front())))
                    continue;
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
            const auto positionalCount = functionCall ? functionCall->args.size()
                                                     : methodCall->args.size();
            const bool hasBlock = functionCall ? bool(functionCall->block)
                                              : bool(methodCall->block);
            std::vector<TypePtr> ordered(layout.params.size());
            bool valid = argTypes.size() ==
                receiverSlots + positionalCount + named->size() + hasBlock;
            if (valid && throughReceiver) ordered.front() = argTypes.front();
            for (size_t i = 0; valid && i < named->size(); ++i) {
                const auto it = std::find(layout.paramNames.begin(),
                                          layout.paramNames.end(), (*named)[i].first);
                if (it == layout.paramNames.end()) { valid = false; break; }
                const auto slot = receiverSlots +
                    std::distance(layout.paramNames.begin(), it);
                if (ordered[slot]) { valid = false; break; }
                ordered[slot] = argTypes[receiverSlots + positionalCount + i];
            }
            size_t next = receiverSlots;
            for (size_t i = 0; valid && i < positionalCount + hasBlock; ++i) {
                while (next < ordered.size() && ordered[next]) ++next;
                if (next == ordered.size()) { valid = false; break; }
                ordered[next++] = argTypes[i < positionalCount
                    ? receiverSlots + i : argTypes.size() - 1];
            }
            if (valid) {
                const auto required = layout.requiredParams.value_or(layout.params.size());
                for (size_t i = 0; i < ordered.size(); ++i) {
                    if (!ordered[i] && i < required) {
                        error(loc, "Missing required argument `" +
                                   layout.paramNames[i - receiverSlots] +
                                   "` for `" + sourceName(name) + "`");
                        return Type::unknown();
                    }
                    if (!ordered[i]) ordered[i] = Type::unknown();
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
        if (callExpr) m_decidedCalls[callExpr] = {CallKind::ValueCall, name};
        const auto held = resolve((*sigs)[0].result);
        if (const auto* function = std::get_if<FuncType>(&held->kind);
            function && function->params.size() == argTypes.size())
            return function->result;
        return Type::unknown();
    }

    // `This` (the trait placeholder — see Surface syntax for declaring a
    // trait in the plan) has no substitution mechanism implemented yet, so
    // it can leak through as a literal NamedType("This") from a `param :
    // This` annotation inside a `make` block. A call involving it can't be
    // meaningfully checked against any signature here — reporting a
    // mismatch against the wrong (imported or unrelated) candidate would be
    // actively misleading, so bail out rather than guess.
    for (const auto& argType : argTypes) {
        if (isThisPlaceholder(argType)) {
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
                    if (req.name == name) return req.result;
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
    if (!argTypes.empty() &&
        (isUndetermined(argTypes[0]))) {
        bool anyFullMatch = false;
        bool anyDroppedMatch = false;
        for (const auto& sig : *sigs) {
            if (sig.params.size() == argTypes.size()) {
                bool matches = true;
                for (size_t i = 0; i < sig.params.size(); i++)
                    if (!fits(i, sig.params[i])) {
                        matches = false;
                        break;
                    }
                anyFullMatch = anyFullMatch || matches;
            }
            if (sig.params.size() == argTypes.size() - 1) {
                bool matches = true;
                for (size_t i = 0; i < sig.params.size(); i++)
                    if (!fits(i + 1, sig.params[i])) {
                        matches = false;
                        break;
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
            const std::size_t requiredParams =
                sig.requiredParams.value_or(sig.params.size());
            if (argTypes.size() < requiredParams ||
                argTypes.size() > sig.params.size())
                continue;
            if (!sig.params.empty()) {
                if (std::holds_alternative<ConstrainedType>(sig.params[0]->kind))
                    anyConstrainedFirstParam = true;
                for (const auto& param : sig.params)
                    if (mentionsUnknownParam(param)) {
                        anyUnknownParamCandidate = true;
                        break;
                    }
                if (fits(0, sig.params[0])) {
                    anyFirstParamPlausible = true;
                    break;
                }
            }
        }
        if (isMethodCall && anyConstrainedFirstParam && !anyFirstParamPlausible) {
            auto receiverKey = m_traits.implementorKey(argTypes[0]);
            if (!m_traits.hasConformances(receiverKey))
                anyConstrainedFirstParam = false;
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
            if (!named) return false;
            if (auto record = m_recordFields.find(
                    resolveRecordName(named->name));
                record != m_recordFields.end())
                return record->second.count(name) > 0;
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
        const bool traitProvides = m_traits.declaresMethod(
            m_traits.implementorKey(argTypes[0]), name);
        const bool provableMismatch =
            !m_inMakeBlock && isFullyConcrete(headShapeOf(argTypes[0])) &&
            !receiverIsField && !traitProvides && !anyUnknownParamCandidate;
        // A receiver only an out-of-scope `make` takes (`d.number` on a `Day`
        // whose `make` is `Cal`'s, without `using Cal`) has no visible method,
        // whatever same-named methods other types have.
        if (!anyFirstParamPlausible && callExpr)
            if (auto hidden = hiddenMakeTaking(name, argTypes); !hidden.empty()) {
                m_decidedCalls[callExpr] = {CallKind::Hidden, hidden};
                return Type::unknown();
            }
        // A default the type inherits answers the call: the type's own.
        if (!anyFirstParamPlausible && traitProvides && callExpr)
            if (const auto own = ownTypeAnswers(argTypes, name)) {
                m_decidedCalls[callExpr] = own->decision();
                return Type::unknown();
            }
        // Not when the type's own method rejects this refinement: writing
        // through a read-only FileHandle is a mismatch, whatever Writable says.
        if (!anyFirstParamPlausible && traitProvides && !hasReceiverRefinementConflict)
            if (auto answered = claimedTraitAnswers(argTypes, name, callExpr))
                return answered;
        // An unreliable candidate list blocks every route to a mismatch
        // report, the trait-bound one included: `"5".to(Integer)` finds only
        // `to : Measure -> ? -> ?` from units.kex when the interfaces come
        // from source, and String is not a Measure — but String's own `to` is
        // a native builtin that never appears in these tables at all. The
        // check is confined to the case where NOTHING matched, so a call that
        // does match still gets its real result type rather than `?`.
        if (!anyFirstParamPlausible &&
            (anyUnknownParamCandidate ||
             (!anyConstrainedFirstParam && !hasReceiverRefinementConflict &&
              !provableMismatch))) {
            // A default the receiver's type inherits, with no home module to
            // answer for it (`(0..4).map`, Enumerable's on Range): the value
            // picks the body at run time.
            if (callExpr && traitProvides) decideDispatch(callExpr, name);
            return Type::unknown();
        }
    }

    const auto genericConflictOf = [&](const Signature& sig) {
        return genericConflict(sig, argTypes, selectedType);
    };
    std::vector<const Signature*> arityMatches;
    std::vector<const Signature*> fullMatches;
    for (const auto& sig : *sigs) {
        const auto* functionCall = callExpr
            ? std::get_if<ast::FunctionCall>(&callExpr->kind) : nullptr;
        const auto* named = functionCall ? &functionCall->namedArgs
                                        : methodCall ? &methodCall->namedArgs : nullptr;
        // A candidate whose parameter names cannot take these labels is no
        // candidate: `m.to(String, in: 5)` is not `to(value, t, radix:)`.
        // Through a receiver the names may or may not list the receiver,
        // which no label can name.
        const bool throughReceiver =
            isMethodCall && !kex::mangle::hasScope(name);
        auto names = sig.paramNames;
        if (throughReceiver && !names.empty() &&
            names.size() == sig.params.size())
            names.erase(names.begin());
        const std::size_t receiverSlots = throughReceiver ? 1 : 0;
        if (named && !named->empty() && !names.empty() &&
            names.size() + receiverSlots == sig.params.size()) {
            std::vector<std::string> labels;
            for (const auto& [label, _] : *named) labels.push_back(label);
            const bool labelsKnown = std::all_of(labels.begin(), labels.end(),
                [&](const std::string& label) {
                    return std::any_of(sigs->begin(), sigs->end(),
                        [&](const Signature& candidate) {
                            return std::find(candidate.paramNames.begin(),
                                             candidate.paramNames.end(), label) !=
                                   candidate.paramNames.end();
                        });
                });
            const auto required = sig.requiredParams.value_or(sig.params.size());
            std::vector<bool> defaulted(names.size(), false);
            for (std::size_t i = required > receiverSlots ? required - receiverSlots : 0;
                 i < defaulted.size(); ++i)
                defaulted[i] = true;
            const auto positional = functionCall
                ? functionCall->args.size() + bool(functionCall->block)
                : methodCall->args.size() + bool(methodCall->block);
            if (labelsKnown &&
                !namedArgumentsFit(names, defaulted, labels, positional))
                continue;
        }

        // A trailing parameter with a default may be omitted, so the accepted
        // range is [requiredParams, params.size()] rather than one number.
        const std::size_t required =
            sig.requiredParams.value_or(sig.params.size());
        if (argTypes.size() < required || argTypes.size() > sig.params.size())
            continue;
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
        if (allMatch && !genericConflictOf(sig)) fullMatches.push_back(&sig);
    }

    // A trait receiver is a fallback for concrete implementations of the
    // same method.  Once a concrete receiver signature matches, do not let a
    // universal trait implementation win merely because another parameter in
    // its signature is more precise.  In particular, Measure.to(String) must
    // keep precedence over Showable.to(String).
    if (isMethodCall && fullMatches.size() > 1 && !argTypes.empty()) {
        const bool hasConcreteReceiverMatch = std::any_of(
            fullMatches.begin(), fullMatches.end(),
            [](const Signature* signature) {
                return !signature->params.empty() &&
                    !std::holds_alternative<ConstrainedType>(
                        signature->params.front()->kind) &&
                    !std::holds_alternative<TypeVar>(
                        signature->params.front()->kind) &&
                    !std::holds_alternative<UnknownType>(
                        signature->params.front()->kind);
            });
        if (hasConcreteReceiverMatch)
            std::erase_if(fullMatches, [](const Signature* signature) {
                return !signature->params.empty() &&
                    std::holds_alternative<ConstrainedType>(
                        signature->params.front()->kind);
            });
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
                std::holds_alternative<UnknownType>(p->kind)) return 0;
            if (std::holds_alternative<ConstrainedType>(p->kind)) return 1;
            return 2;
        };
        auto dominates = [&](const Signature* a, const Signature* b) {
            bool aWins = false;
            for (size_t i = 0; i < a->params.size(); i++) {
                int sa = paramSpec(a->params[i]);
                int sb = paramSpec(b->params[i]);
                if (sb > sa) return false;
                if (sa > sb) aWins = true;
            }
            return aWins;
        };
        std::vector<const Signature*> undominated;
        for (const auto* cand : fullMatches) {
            bool dominated = false;
            for (const auto* other : fullMatches) {
                if (other == cand) continue;
                if (dominates(other, cand)) { dominated = true; break; }
            }
            if (!dominated) undominated.push_back(cand);
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
                for (size_t i = 0; matches && i < argTypes.size(); ++i)
                    matches = argMatchesParam(resolve(argTypes[i]),
                                              resolve(candidate->params[i]));
                if (matches) stillMatching.push_back(candidate);
            }
            if (!stillMatching.empty() &&
                stillMatching.size() < undominated.size())
                undominated = std::move(stillMatching);
        }
        std::vector<const Signature*> distinct;
        for (const auto* candidate : undominated) {
            bool duplicate = std::any_of(
                distinct.begin(), distinct.end(),
                [&](const Signature* other) { return sameParameters(*other, *candidate); });
            if (!duplicate) distinct.push_back(candidate);
        }
        const Signature* best =
            undominated.empty() ? fullMatches.front() : undominated.front();
        // A method call dispatches on its RECEIVER, so a candidate whose first
        // parameter is exactly the receiver's type is the one written for it.
        // Equally specific candidates otherwise fall back to declaration
        // order, which picked `factor : SIUnit -> _` for a `Measure` receiver
        // as soon as the receiver stopped being inferred as Unknown.
        if (isMethodCall && !argTypes.empty()) {
            const auto receiver = resolve(argTypes.front());
            for (const auto* candidate : undominated)
                if (!candidate->params.empty() &&
                    typesEqual(resolve(candidate->params.front()), receiver)) {
                    best = candidate;
                    break;
                }
        }
        const bool concreteArguments = std::all_of(
            argTypes.begin(), argTypes.end(),
            [&](const TypePtr& argument) {
                auto resolved = resolve(argument);
                return !std::holds_alternative<TypeVar>(resolved->kind) &&
                    !std::holds_alternative<UnknownType>(resolved->kind);
            });
        bool typedCandidates = false;
        for (size_t left = 0;
             left < distinct.size() && !typedCandidates; ++left)
            for (size_t right = left + 1;
                 right < distinct.size() && !typedCandidates; ++right)
                for (size_t i = 0;
                     i < distinct[left]->params.size(); ++i) {
                    const auto& a = distinct[left]->params[i];
                    const auto& b = distinct[right]->params[i];
                    if (typesEqual(a, b)) continue;
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
        if (distinct.size() > 1 && concreteArguments && typedCandidates) {
            std::string message =
                "Ambiguous overload for `" + sourceName(name) + "`; candidates:";
            for (const auto* candidate : distinct)
                message += "\n\n" + displaySignature(name, *candidate);
            error(loc, message);
        }
        // An unconstrained shorthand receiver (for example `&.kilo`) can
        // match several concrete receiver overloads. Picking the first one
        // would permanently specialize the closure to declaration order;
        // keep its intermediate result permissive until the surrounding
        // method chain or a call site supplies a concrete receiver.
        if (distinct.size() > 1 && !concreteArguments) {
            if (callExpr) decideDispatch(callExpr, name);
            return Type::unknown();
        }
        if (best) {
            // Rebuild fullMatches with best first so the code below uses it
            std::vector<const Signature*> reordered = {best};
            for (const auto* s : fullMatches) if (s != best) reordered.push_back(s);
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
            for (const auto& p : sig->params)
                if (!std::holds_alternative<TypeVar>(p->kind) &&
                    !std::holds_alternative<UnknownType>(p->kind) &&
                    !std::holds_alternative<ConstrainedType>(p->kind))
                    return false;
            return true;
        };
        bool allVacuous = true;
        for (const auto* fm : fullMatches)
            if (!isVacuous(fm)) { allVacuous = false; break; }
        if (allVacuous) {
            bool hasConcrete = false;
            for (const auto* am : arityMatches) {
                if (isVacuous(am) ||
                    std::find(fullMatches.begin(), fullMatches.end(), am) != fullMatches.end())
                    continue;
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
                if (!am->params.empty() && !argTypes.empty() &&
                    !fits(0, am->params[0]))
                    continue;
                hasConcrete = true;
                break;
            }
            if (hasConcrete) fullMatches.clear();
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
                if (candidate->params.empty()) return 2;
                const auto parameter = resolve(candidate->params[0]);
                if (std::holds_alternative<ConstrainedType>(parameter->kind))
                    return 0;
                // A trait may reach here as a plain named type rather than a
                // constrained one; `Showable` is a bound, not a concrete type.
                if (const auto* named = std::get_if<NamedType>(&parameter->kind);
                    named && isTrait(named->name))
                    return 0;
                if (std::holds_alternative<TypeVar>(parameter->kind) ||
                    std::holds_alternative<UnknownType>(parameter->kind))
                    return 1;
                return 2;
            };
            std::stable_sort(fullMatches.begin(), fullMatches.end(),
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
            for (const auto* candidate : fullMatches)
                if (rank(candidate) == 2)
                    receiverHeads.insert(
                        typeToString(headShapeOf(resolve(candidate->params[0]))));
            if (receiverHeads.size() > 1) {
                if (callExpr)
                    decideDispatch(callExpr, name);
                return Type::unknown();
            }
        }
    }

    if (fullMatches.size() >= 1) {
        const auto& matched = *fullMatches[0];
        if (std::getenv("KEX_DEBUG_SIG") && name == "to") {
            std::fprintf(stderr, "[to] params=");
            for (const auto& p : matched.params)
                std::fprintf(stderr, "%s ", typeToString(resolve(p)).c_str());
            std::fprintf(stderr, "=> %s | matches=%zu args=",
                         typeToString(resolve(matched.result)).c_str(),
                         fullMatches.size());
            for (const auto& a : argTypes)
                std::fprintf(stderr, "%s ", typeToString(resolve(a)).c_str());
            std::fprintf(stderr, "\n");
        }
        // Set when this call's receiver type is unknown AND more than one
        // provider could answer it, so nothing here may be treated as
        // evidence about the receiver — not the backend target, and not the
        // receiver's type.
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
                } else if (selected >= localSigCount &&
                    selected - localSigCount < importedFunctions.size())
                    resolved = importedFunctions[selected - localSigCount];
                else
                    winnerIsLocal = true;
            }
            if (!resolved && !winnerIsLocal) {
                int matchedArity = static_cast<int>(matched.params.size());
                for (const auto* candidate : importedFunctions) {
                    if (candidate->backendArity == matchedArity) {
                        resolved = candidate; break;
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
                if (!isReceiver || argTypes.empty()) return false;
                const auto receiver = resolve(argTypes[0]);
                return isUndetermined(receiver);
            }();
            if (resolved && receiverUnknown)
                for (const auto* candidate : importedFunctions) {
                    if (candidate == resolved ||
                        candidate->backendArity != resolved->backendArity ||
                        candidate->backendModule == resolved->backendModule)
                        continue;
                    resolved = nullptr;
                    ambiguousReceiver = true;
                    break;
                }
            // Same for a LOCAL make-block method: a module flattened into this
            // unit owns the name for its own receiver type, and an unknown
            // receiver is no evidence that this call means that type. Pinning
            // it sent every `get` in a program that says `using Regex` to
            // Match's accessor.
            if (!resolved && winnerIsMakeMethod && receiverUnknown)
                for (const auto* candidate : importedFunctions)
                    if (candidate->backendArity ==
                        static_cast<int>(matched.params.size())) {
                        winnerIsMakeMethod = false;
                        ambiguousReceiver = true;
                        break;
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
            if (receiverUnknown && isReceiver && argTypes.size() == 1 &&
                !matched.params.empty()) {
                const auto owner = typeToString(resolve(matched.params.front()));
                const auto ownerBase = owner.substr(0, owner.find('<'));
                auto inView = [&](const std::string& record) {
                    if (m_programRecords.count(record)) return true;
                    auto imports = [&](const std::vector<ImportSelection>& list) {
                        return std::any_of(
                            list.begin(), list.end(),
                            [&](const ImportSelection& import) {
                                return record.rfind(import.module + ".", 0) == 0;
                            });
                    };
                    if (imports(m_declarationImports)) return true;
                    return std::any_of(m_importScopeStack.begin(),
                                       m_importScopeStack.end(), imports);
                };
                for (const auto& [record, fields] : m_recordFields)
                    if (record != ownerBase && fields.count(name) &&
                        inView(record))
                        return Type::unknown();
            }
            if (!ambiguousReceiver && receiverUnknown && fullMatches.size() > 1) {
                std::string receiverType;
                for (const auto* candidate : fullMatches) {
                    if (candidate->params.empty()) continue;
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
                    for (const auto& param : matched.params)
                        target.localDispatchTypes.push_back(typeToString(resolve(param)));
                    target.isServingCast = typesEqual(resolve(matched.result), Type::unit());
                    // As for a slot of this unit's: a cast has no reply to
                    // wait for, and lowering would drop the timeout.
                    if (target.isServingCast)
                        for (const auto& [label, value] : methodCall->namedArgs)
                            if (label == "within")
                                error(value ? value->location : methodCall->receiver->location,
                                      "`within:` is not valid on an asynchronous cast slot");
                }
                for (std::size_t i = 0; i < matched.params.size(); ++i) {
                    const auto resolvedParam = resolve(matched.params[i]);
                    const auto addRequiredDictionary = [&](const std::string& traitName) {
                        const bool backendExpectsDictionary =
                            resolved->backendArity >
                            static_cast<int>(matched.params.size() +
                                             target.traitDictionaries.size());
                        if (!backendExpectsDictionary) return;
                        const auto* trait = m_traits.get(traitName);
                        const bool methodIsRequired = trait && std::any_of(
                            trait->requiredMethods.begin(),
                            trait->requiredMethods.end(),
                            [&](const auto& required) {
                                return required.name == name;
                            });
                        const bool callsTraitImplementationDirectly =
                            i == 0 && isReceiver &&
                            methodIsRequired &&
                            resolved->backendFunction == name + "/" + traitName;
                        if (trait && !trait->requiredMethods.empty() &&
                            !callsTraitImplementationDirectly)
                            target.traitDictionaries.push_back({i, traitName});
                    };
                    if (const auto* constrained =
                            std::get_if<ConstrainedType>(&resolvedParam->kind))
                        addRequiredDictionary(constrained->traitName);
                    else if (const auto* named =
                                 std::get_if<NamedType>(&resolvedParam->kind))
                        addRequiredDictionary(named->name);
                }
                if (isReceiver && !matched.params.empty() &&
                    !argTypes.empty() && m_importedInterfaces) {
                        // The CALL-SITE receiver (`argTypes[0]`), not the
                        // winner's: matching the winner against itself
                        // covers tautologically, which miscompiled
                        // `None.blank?` to `blank?/Bool` and `None.to`
                        // to `to/Atom`.
                        auto spelling =
                            resolve_core::importDispatchSpelling(
                                *resolved,
                                resolve(argTypes[0]),
                                *m_importedInterfaces,
                                [this](const TypePtr& type) {
                                    return expandTypeAliases(type);
                                });
                        if (spelling)
                            target.importDispatchSpelling =
                                std::move(*spelling);
                }
                // Who owns the call (ownerOfCall): lowering used to decide
                // this again by reading the receiver's type back.
                const bool competing = std::any_of(
                    importedFunctions.begin(), importedFunctions.end(),
                    [&](const ImportedFunction* candidate) {
                        return candidate->backendModule != target.backendModule &&
                               candidate->backendArity == target.backendArity;
                    });
                const auto owner =
                    ownerOfCall(argTypes, name, target.backendModule, competing);
                switch (owner.owner) {
                case CallOwner::Winner:
                    m_resolvedCalls[methodCall] = std::move(target);
                    break;
                case CallOwner::ReceiverType:
                    // The receiver's own type answers; lowering reaches it.
                    if (callExpr) m_decidedCalls[callExpr] = owner.own.decision();
                    // A type of a unit compiled apart: the dispatcher of the
                    // one module declaring the method for it
                    // (`connection.close`, Net.Socket's).
                    if (!owner.own.home.empty() && owner.own.moduleFunction.empty()) {
                        std::set<std::string> modules;
                        const ImportedFunction* dispatcher = nullptr;
                        for (const auto* candidate : importedFunctions) {
                            if (candidate->backendArity != target.backendArity ||
                                candidate->signature.params.empty())
                                continue;
                            const auto* receiver = std::get_if<NamedType>(
                                &resolve(candidate->signature.params.front())->kind);
                            if (!receiver || ownTypeIdentity(receiver->name) != owner.own.type)
                                continue;
                            modules.insert(candidate->backendModule);
                            if (candidate->backendFunction == name) dispatcher = candidate;
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
                            inherited.importDispatchSpelling.clear();
                            inherited.localDispatchTypes.clear();
                            m_resolvedCalls[methodCall] = std::move(inherited);
                        }
                    }
                    break;
                case CallOwner::Value:
                    if (callExpr)
                        decideDispatch(callExpr, name);
                    break;
                }
            } else if (winnerIsMakeMethod) {
                // `private do ... end` inside a `make` block means "only
                // callable within that make" (docs/modules.md). Enforced here
                // because visibility is erased by both backends: a private
                // method is an ordinary function to them, so nothing stopped
                // another module from calling one
                // (rodolfo docs/kex-issues.md #13, kexhq/kex#281).
                if (!matched.params.empty())
                    if (auto* owner = std::get_if<NamedType>(
                            &resolve(matched.params.front())->kind)) {
                        auto hidden =
                            m_privateMakeMethods.find(owner->name + "#" + name);
                        if (hidden != m_privateMakeMethods.end() &&
                            !withinModule(hidden->second))
                            error(methodCall->receiver->location,
                                  "`" + name + "` is private to `make " +
                                  owner->name + "`" +
                                  (hidden->second.empty()
                                       ? std::string{}
                                       : " in `" + hidden->second + "`") +
                                  " — it is only callable inside that block");
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
                    target.isServingCast = named && named->name == "Server" &&
                        m_slotMethodNames.count(name) &&
                        typesEqual(resolve(matched.result), Type::unit());
                }
                if (target.isServingCast)
                    for (const auto& [label, value] : methodCall->namedArgs)
                        if (label == "within")
                            error(value ? value->location : methodCall->receiver->location,
                                  "`within:` is not valid on an asynchronous cast slot");
                for (const auto& param : matched.params)
                    target.localDispatchTypes.push_back(
                        typeToString(resolve(param)));
                m_resolvedCalls[methodCall] = std::move(target);
            }
        }
        // Propagate the param types back to any TypeVar arguments so that
        // unannotated params are constrained by the functions they're passed
        // into (e.g. `let f(s) = s.split(",")` constrains `s` to String).
        // Only apply when the param is concrete — skip generic params that
        // themselves contain TypeVars (e.g. `first : [A] -> A?`).
        const auto typeContainsVar = [](const TypePtr& t) {
            return anyType(t, [](const TypePtr& node) {
                return std::holds_alternative<TypeVar>(node->kind);
            });
        };
        for (size_t i = 0; i < argTypes.size() && i < matched.params.size(); i++) {
            // The receiver of an ambiguous call is exactly the argument this
            // match says nothing about: binding it to the winner's param type
            // would then TYPE it as that overload's receiver (a Map inferred
            // as Regex's `Match`), which is worse than leaving it unknown.
            if (i == 0 && ambiguousReceiver) continue;
            auto resolved = resolve(argTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                const auto& param = matched.params[i];
                // Negative ids are per-signature generic placeholders, not
                // inference variables, and every signature numbers its own
                // from -1. Binding one in the global substitution made an
                // unrelated signature's `A` mean whatever the last such call
                // passed: after `args.at(i).or("")` (String), `(0..3).filter`
                // returned [String] and its block param typed as String.
                if (deferIfPending(resolved, param, loc)) continue;
                if (tv->id >= 0 && !typeContainsVar(param)) {
                    // A receiver typed by nothing but the method's name is
                    // a guess (`item.name` makes `item` a Weekday).
                    if (i == 0) m_guessedReceiverTypes.insert(typeToString(param));
                    unifyVar(tv->id, param);
                }
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
                if (!pattern || !actual) return;
                if (auto* tv = std::get_if<TypeVar>(&pattern->kind);
                    tv && tv->id < 0) {
                    genericSubst.try_emplace(tv->id, resolve(actual));
                    return;
                }
                if (auto* constrained =
                        std::get_if<ConstrainedType>(&pattern->kind);
                    constrained && constrained->genericId < 0) {
                    genericSubst.try_emplace(constrained->genericId,
                                             resolve(actual));
                    return;
                }
                if (auto* pn = std::get_if<NamedType>(&pattern->kind)) {
                    auto* an = std::get_if<NamedType>(&actual->kind);
                    if (!an || pn->name != an->name ||
                        pn->typeArgs.size() != an->typeArgs.size()) return;
                    for (size_t i = 0; i < pn->typeArgs.size(); i++)
                        bindGenerics(pn->typeArgs[i], an->typeArgs[i]);
                } else if (auto* pl = std::get_if<ListType>(&pattern->kind)) {
                    if (auto* al = std::get_if<ListType>(&actual->kind))
                        bindGenerics(pl->element, al->element);
                } else if (auto* pm = std::get_if<MapType>(&pattern->kind)) {
                    if (auto* am = std::get_if<MapType>(&actual->kind)) {
                        bindGenerics(pm->key, am->key);
                        bindGenerics(pm->value, am->value);
                    }
                } else if (auto* po = std::get_if<OptionalType>(&pattern->kind)) {
                    if (auto* ao = std::get_if<OptionalType>(&actual->kind))
                        bindGenerics(po->inner, ao->inner);
                } else if (auto* pt = std::get_if<TupleType>(&pattern->kind)) {
                    auto* at = std::get_if<TupleType>(&actual->kind);
                    if (!at || pt->elements.size() != at->elements.size()) return;
                    for (size_t i = 0; i < pt->elements.size(); i++)
                        bindGenerics(pt->elements[i], at->elements[i]);
                } else if (auto* pf = std::get_if<FuncType>(&pattern->kind)) {
                    auto* af = std::get_if<FuncType>(&actual->kind);
                    if (!af || pf->params.size() != af->params.size()) return;
                    for (size_t i = 0; i < pf->params.size(); i++)
                        bindGenerics(pf->params[i], af->params[i]);
                    bindGenerics(pf->result, af->result);
                } else if (auto* pi =
                               std::get_if<IntersectionType>(&pattern->kind)) {
                    // Each constraint applies to the same actual value. This
                    // is what binds R in `R & { name: String }` to the full
                    // nominal argument rather than to its visible row alone.
                    for (const auto& member : pi->members)
                        bindGenerics(member, actual);
                } else if (auto* pr = std::get_if<RecordType>(&pattern->kind)) {
                    if (auto* ar = std::get_if<RecordType>(&actual->kind)) {
                        for (const auto& [name, fieldPattern] : pr->fields) {
                            auto found = std::find_if(
                                ar->fields.begin(), ar->fields.end(),
                                [&](const auto& field) {
                                    return field.first == name;
                                });
                            if (found != ar->fields.end())
                                bindGenerics(fieldPattern, found->second);
                        }
                    } else if (auto* named =
                                   std::get_if<NamedType>(&actual->kind)) {
                        auto record = m_recordFields.find(
                            resolveRecordName(named->name));
                        if (record != m_recordFields.end())
                            for (const auto& [name, fieldPattern] : pr->fields) {
                                auto found = record->second.find(name);
                                if (found != record->second.end())
                                    bindGenerics(fieldPattern, found->second);
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
        // bind (`map : (X -> B) -> [B]`) are left exactly as they were.
        std::function<TypePtr(const TypePtr&)> applyGenerics =
            [&](const TypePtr& type) -> TypePtr {
                if (!type) return type;
                if (auto* var = std::get_if<TypeVar>(&type->kind)) {
                    if (var->id >= 0) return type;
                    auto found = genericSubst.find(var->id);
                    return found == genericSubst.end() ? type : found->second;
                }
                // A bound generic (`N: Number`) is bound by its id as well.
                if (auto* constrained = std::get_if<ConstrainedType>(&type->kind);
                    constrained && constrained->genericId < 0) {
                    auto found = genericSubst.find(constrained->genericId);
                    return found == genericSubst.end() ? type : found->second;
                }
                // A trait method's `This` is its receiver's type:
                // `"x".repeat(3)` (Monoid's default) is a String.
                if (isThisPlaceholder(type) &&
                    isMethodCall && !kex::mangle::hasScope(name) && !argTypes.empty()) {
                    auto receiver = resolve(argTypes.front());
                    if (!std::holds_alternative<UnknownType>(receiver->kind) &&
                        !std::holds_alternative<TypeVar>(receiver->kind))
                        return receiver;
                }
                // Every nested type, function types included: a function-typed
                // parameter is where a BLOCK is passed, and skipping it left
                // its variables the only ones standing — `nums.map { |x| x * 2 }`
                // reported the selected overload as `[Integer] -> (A -> B) ->
                // [Integer]`, substituting the receiver and result but not the
                // block it was actually given.
                return mapTypeChildren(type, applyGenerics);
            };
        auto selectedResult =
            normalizeIntersection(applyGenerics(matched.result));
        for (std::size_t i = 0; i < argExprs.size() && i < matched.params.size(); ++i)
            if (argExprs[i]) checkByteLiterals(*argExprs[i], applyGenerics(resolve(matched.params[i])));
        if (callExpr) {
            Signature selected = matched;
            selected.name = name;
            if (methodCall)
                if (auto target = m_resolvedCalls.find(methodCall);
                    target != m_resolvedCalls.end())
                    selected.isFoul = selected.isFoul || target->second.isFoul;
            for (auto& param : selected.params)
                param = applyGenerics(resolve(param));
            selected.result = selectedResult;
            m_selectedCallSignatures[callExpr] = std::move(selected);
            // A call no imported target took (none, or a `make` method of
            // this program): the receiver's own type may answer it
            // (ownTypeAnswers). ownerOfCall decided the imported ones.
            const auto resolved = methodCall ? m_resolvedCalls.find(methodCall)
                                             : m_resolvedCalls.end();
            if (methodCall && !argTypes.empty() &&
                (resolved == m_resolvedCalls.end() ||
                 resolved->second.backendModule.empty()) &&
                !m_decidedCalls.contains(callExpr))
                if (const auto own = ownTypeAnswers(argTypes, name))
                    m_decidedCalls[callExpr] = own->decision();
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
        if (isMethodCall && !argTypes.empty()) {
            auto receiver = resolve(argTypes.front());
            if (isUndetermined(receiver))
                return Type::unknown();
        }
        error(loc, "`" + sourceName(name) + "` expects " + std::to_string((*sigs)[0].params.size()) +
              " argument(s), got " + std::to_string(argTypes.size()));
        return (*sigs)[0].result;
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
        if (auto owners = m_makeMethodReceivers.find(name);
            owners != m_makeMethodReceivers.end()) {
            auto receiverArg = resolve(argTypes.front());
            // Only when the block's own signature is genuinely missing from
            // `arityMatches` — the registration-order gap this exists for.
            // Once it IS registered, it is already one of the candidates
            // just rejected on the merits, and must report the same
            // argument-type error it always did rather than being waved
            // through by receiver alone (`5.describe(42)` on
            // `make Int do describe :> String -> String ... end`).
            bool receiverAlreadyCandidate = std::any_of(
                arityMatches.begin(), arityMatches.end(),
                [&](const Signature* sig) {
                    return !sig->params.empty() &&
                           typesEqual(resolve(sig->params.front()), receiverArg);
                });
            if (!receiverAlreadyCandidate)
                for (const auto& owner : owners->second)
                    if (argMatchesParam(receiverArg, resolve(owner)))
                        return Type::unknown();
        }
    }
    auto isVacuousSig = [](const Signature* sig) {
        for (const auto& p : sig->params)
            if (!std::holds_alternative<TypeVar>(p->kind) &&
                !std::holds_alternative<UnknownType>(p->kind) &&
                !std::holds_alternative<ConstrainedType>(p->kind))
                return false;
        return true;
    };
    // When NO candidate agrees on the receiver — `Result<Date, _>.iso`, where
    // every `iso` belongs to some other type — the loop below never fires and
    // the headline names whatever was registered first. That is registration
    // order, so adding an unrelated overload anywhere in the prelude silently
    // rewrote the message. Fall back to the same ordering the listing uses.
    auto signatureOrder = [](const Signature* a, const Signature* b) {
        if (a->params.size() != b->params.size())
            return a->params.size() < b->params.size();
        for (size_t i = 0; i < a->params.size(); ++i) {
            auto left = typeToString(a->params[i]);
            auto right = typeToString(b->params[i]);
            if (left != right) return left < right;
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
        for (size_t i = 0; i < sig->params.size() && i < argTypes.size(); ++i)
            if (!fits(i, sig->params[i])) ++count;
        return count;
    };
    const Signature* firstPtr = nullptr;
    size_t fewestMismatches = 0;
    for (const auto* am : arityMatches) {
        if (isVacuousSig(am)) continue;
        if (isMethodCall && !am->params.empty() &&
            !fits(0, am->params[0])) continue;
        const auto mismatches = mismatchCount(am);
        if (!firstPtr || mismatches < fewestMismatches ||
            (mismatches == fewestMismatches && signatureOrder(am, firstPtr))) {
            firstPtr = am;
            fewestMismatches = mismatches;
        }
    }
    // When NO candidate qualifies, the listing's order is the only principled
    // answer left — see the comment above `signatureOrder`.
    if (!firstPtr)
        firstPtr = *std::min_element(arityMatches.begin(), arityMatches.end(),
                                     signatureOrder);
    // Same rule as the receiver-mismatch guard above, for the path a
    // PRIMITIVE receiver takes: when every candidate carries an Unknown
    // parameter, this candidate list is not the whole truth and no mismatch
    // follows from it. `"42".to(Integer)` reaches here seeing only Measure's
    // `to` from units.kex, because String's `to` is a native builtin that
    // appears in no signature table. The runtime still catches a real mistake.
    const bool unreliableCandidates =
        std::all_of(arityMatches.begin(), arityMatches.end(),
                    [](const Signature* sig) {
                        return std::any_of(sig->params.begin(), sig->params.end(),
                                           mentionsUnknownParam);
                    });
    if (unreliableCandidates) return arityMatches[0]->result;

    const Signature& first = *firstPtr;
    std::string detail = "different arguments";
    for (size_t i = 0; i < first.params.size(); i++) {
        if (!fits(i, first.params[i])) {
            auto* constrained = std::get_if<ConstrainedType>(&first.params[i]->kind);
            std::string expected = constrained ? constrained->traitName : typeToString(first.params[i]);
            detail = "argument " + std::to_string(i + 1) + " to be " + expected +
                      ", but got " + typeToString(argTypes[i]);
            break;
        }
    }
    if (detail == "different arguments")
        if (const auto clash = genericConflictOf(first)) {
            // An argument a dedicated check already rejected (`or` given a
            // block, a deferred reply of the wrong type) is reported once.
            if (m_diagnostics &&
                std::any_of(m_diagnostics->begin(), m_diagnostics->end(),
                            [&](const Diagnostic& d) {
                                return d.level == Diagnostic::Level::Error &&
                                       d.location.line == loc.line &&
                                       d.location.file == loc.file;
                            }))
                return first.result;
            detail = "argument " + std::to_string(clash->first + 1) + " to be " +
                     typeToString(resolve(clash->second)) + ", but got " +
                     typeToString(argTypes[clash->first]);
        }

    std::string message = "`" + sourceName(name) + "` expects " + detail;
    // Vacuous candidates (params all TypeVar/Unknown/trait bounds — e.g. an
    // unannotated trait default method) add noise to the listing when
    // concrete candidates exist; show them only when they are all we have.
    auto sortedSigs = *sigs;
    std::stable_sort(sortedSigs.begin(), sortedSigs.end(),
        [](const Signature& a, const Signature& b) {
            if (a.params.size() != b.params.size()) return a.params.size() < b.params.size();
            for (size_t i = 0; i < a.params.size(); ++i) {
                auto as = typeToString(a.params[i]), bs = typeToString(b.params[i]);
                if (as != bs) return as < bs;
            }
            return typeToString(a.result) < typeToString(b.result);
        });
    bool anyConcreteSig = false;
    for (const auto& sig : sortedSigs)
        if (!isVacuousSig(&sig)) { anyConcreteSig = true; break; }
    // One line per DISTINCT signature. The same overload can be registered
    // more than once — from the prelude interface, from the stdlib source, and
    // again from a merged module — and listing `split : String -> [String]`
    // four times says nothing the first line did not.
    std::unordered_set<std::string> shown;
    for (const auto& sig : sortedSigs) {
        if (anyConcreteSig && isVacuousSig(&sig)) continue;
        // An un-annotated signature this call could not reach is noise, not a
        // suggestion. The stdlib ships capability stand-ins, so `write` also
        // names `Mock.Files -> Any -> Any -> Bool`, and printing that under a
        // 2-argument error told the reader to consider a method taking three
        // (kexhq/kex#143). A CONCRETE overload at another arity still earns
        // its line — `split : String -> [String]` is worth seeing even when
        // the call passed two arguments.
        const std::size_t requiredParams =
            sig.requiredParams.value_or(sig.params.size());
        const bool arityCannotMatch = argTypes.size() < requiredParams ||
                                      argTypes.size() > sig.params.size();
        if (arityCannotMatch &&
            std::any_of(sig.params.begin(), sig.params.end(),
                        mentionsUnknownParam))
            continue;
        auto rendered = displaySignature(name, sig);
        if (!shown.insert(rendered).second) continue;
        message += "\n\n" + rendered;
    }
    error(loc, message);
    // Lowered all the same only where the check does not gate the build
    // (`--no-check`), and there the value decides, as it does for a
    // receiver of unknown type: when the receiver is one the method takes
    // and only an argument is off (a truthy block for `filter`). Where every
    // candidate is a record's method and the receiver no such record
    // (`"a b".whiteSpaces`, Parsing's Input's), no value reaches one: the
    // call fails as an undefined method, where it is written.
    const bool receiverTaken = std::any_of(
        arityMatches.begin(), arityMatches.end(), [&](const Signature* candidate) {
            return !candidate->params.empty() && !argTypes.empty() &&
                   argMatchesParam(argTypes.front(), candidate->params.front());
        });
    const bool recordsOnly = std::all_of(
        arityMatches.begin(), arityMatches.end(), [&](const Signature* candidate) {
            if (candidate->params.empty()) return false;
            const auto* owner = std::get_if<NamedType>(&resolve(candidate->params.front())->kind);
            return owner && m_recordFields.contains(owner->name);
        });
    if (isMethodCall && callExpr && !m_decidedCalls.contains(callExpr)) {
        if (receiverTaken)
            decideDispatch(callExpr, name);
        else if (recordsOnly && !argTypes.empty() && !isUndetermined(resolve(argTypes.front())))
            m_decidedCalls[callExpr] = {CallKind::Hidden, ""};
    }
    return arityMatches[0]->result;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::ownerOfCall(const std::vector<TypePtr>& argTypes,
                              const std::string& method,
                              const std::string& winnerModule, bool competing)
    -> OwnerDecision {
    if (argTypes.empty()) return {};
    const auto receiver = resolve(argTypes.front());
    const auto* named = std::get_if<NamedType>(&receiver->kind);
    const bool erased = isUndetermined(receiver) ||
                        (named && named->name == "Any");
    if (erased)
        return {winnerModule != names::kPreludeModule && competing
                    ? CallOwner::Value : CallOwner::Winner};
    if (auto own = ownTypeAnswers(argTypes, method); own && own->home != winnerModule)
        return {CallOwner::ReceiverType, std::move(*own)};
    return {};
}

// The backend module of the receiver's own type when that type answers
// What the receiver's own type answers `method` with, at one signature: its
// `make` block's (declared there, or a trait default it inherits) — not
// overloaded by argument type, which lowering dispatches — or else a function
// of its home module over it. A type of this program, or of a unit compiled
// apart (from its own module, inherited defaults only); nothing for any other.
auto TypeChecker::ownTypeAnswers(const std::vector<TypePtr>& argTypes,
                                 const std::string& method)
    -> std::optional<OwnAnswer> {
    const auto* named = std::get_if<NamedType>(&resolve(argTypes.front())->kind);
    if (!named) return std::nullopt;
    const auto owner = ownTypeIdentity(named->name);
    const auto home = typeHomeModule(owner);
    if (!home) return std::nullopt;
    // What its own module emits for it: a trait's required method or a default
    // from the trait's body. A function over a trait it claims
    // (`inspected(value: Inspectable)`) is the trait home's, not the type's.
    const auto inherits =
        m_traits.implementsInHome(m_traits.implementorKey(argTypes.front()), method);
    if (!home->empty())
        return inherits ? std::optional<OwnAnswer>({owner, *home, {}}) : std::nullopt;
    std::size_t declared = 0;
    if (auto local = m_methodSignatures.find(method);
        local != m_methodSignatures.end())
        for (const auto& signature : local->second) {
            if (signature.params.empty()) continue;
            const auto* receiver =
                std::get_if<NamedType>(&resolve(signature.params.front())->kind);
            if (receiver && ownTypeIdentity(receiver->name) == owner &&
                signature.params.size() == argTypes.size())
                ++declared;
        }
    if (declared > 1) return std::nullopt;  // overloaded by argument: dispatched
    if (declared == 1 || inherits) return OwnAnswer{owner, {}, {}};
    // A function of the type's own module whose receiver is the type:
    // `TCP.close(connection: Conn)` for a `TCP.Conn`.
    const auto dot = owner.rfind('.');
    if (dot == std::string::npos) return std::nullopt;
    const auto functions =
        m_userSignatures.find(kex::mangle::qualify(owner.substr(0, dot), method));
    if (functions == m_userSignatures.end()) return std::nullopt;
    const bool overOwnType = std::any_of(
        functions->second.begin(), functions->second.end(),
        [&](const Signature& signature) {
            if (signature.params.size() != argTypes.size()) return false;
            const auto* receiver =
                std::get_if<NamedType>(&resolve(signature.params.front())->kind);
            return receiver && ownTypeIdentity(receiver->name) == owner;
        });
    if (!overOwnType) return std::nullopt;
    return OwnAnswer{owner, {}, owner.substr(0, dot) + "." + method};
}

// One name for a type however it is written: a record's or ADT's identity
// (resolveRecordName), and for a distinct type registered under both its
// spellings (`ThingId` and `ThingIds.ThingId`) the qualified one, when
// exactly one of this program's declarations has it.
auto TypeChecker::ownTypeIdentity(const std::string& name) const -> std::string {
    const auto identity = resolveRecordName(name);
    if (identity != name || identity.find('.') != std::string::npos ||
        m_programRecords.contains(identity))
        return identity;
    std::string qualified;
    for (const auto& distinct : m_programDistinctTypes)
        if (distinct.ends_with("." + name)) {
            if (!qualified.empty()) return name;  // two modules: as written
            qualified = distinct;
        }
    return qualified.empty() ? name : qualified;
}

// Where `type` is declared: "" for this program, the backend module of the
// unit compiled apart that declares it (`Bags.Bag` is `Bags`'s), or nothing
// for the prelude's and everything else.
auto TypeChecker::typeHomeModule(const std::string& type) const
    -> std::optional<std::string> {
    if (m_programRecords.contains(type) || m_programDistinctTypes.contains(type))
        return std::string{};
    if (!m_importedInterfaces) return std::nullopt;
    const ImportedModuleInterface* home = nullptr;
    for (const auto& [name, module] : m_importedInterfaces->modules)
        if (module.compiledApart && type.starts_with(name + ".") &&
            (!home || name.size() > home->sourceModule.size()))
            home = &module;
    if (!home) return std::nullopt;
    return home->backendModule;
}

} // namespace kex::semantic

namespace kex::semantic {

// A type that claims a trait has what the trait requires, even where its own
// methods are not on record (`Kilo(Watt).factor` on Units.SI's unannotated
// `let factor(@Kilo(unit))`): the requirement types the call, and the value
// decides the implementation at run time, as for a trait-typed receiver.
auto TypeChecker::claimedTraitAnswers(const std::vector<TypePtr>& argTypes,
                                      const std::string& name,
                                      const ast::Expr* callExpr) -> TypePtr {
    if (argTypes.empty()) return nullptr;
    const auto [trait, required] = m_traits.requiredMethod(resolve(argTypes.front()), name);
    if (!trait || argTypes.size() != required->params.size() + 1) return nullptr;
    if (callExpr) m_decidedCalls[callExpr] = {CallKind::TraitMethod, trait->name};
    return required->result;
}

} // namespace kex::semantic

namespace kex::semantic {

// A rescue clause binds names exactly like a match clause: its patterns
// introduce locals its guard and body can see, and `rescue |e|` binds the
// caught error. `rescue return` exits the enclosing function, so its
// expression is no arm.
auto TypeChecker::checkRescue(const ast::RescueBlock& rescue,
                              const std::function<void(const TypePtr&)>& onArm) -> void {
    for (const auto& clause : rescue.clauses) {
        pushScope();
        for (const auto& pat : clause.patterns)
            if (pat) {
                checkPatternArity(*pat);
                bindPatternVars(*pat);
            }
        if (clause.guard && *clause.guard) inferExpr(**clause.guard);
        if (clause.body) {
            auto armType = inferExpr(*clause.body);
            if (onArm && !alwaysReturns(*clause.body)) onArm(armType);
        }
        popScope();
    }
    if (!rescue.catchAllBody.empty()) {
        pushScope();
        if (!rescue.catchAllParam.empty())
            defineVar(rescue.catchAllParam, Type::unknown());
        auto catchAllType = inferBody(rescue.catchAllBody);
        if (onArm && !alwaysReturns(rescue.catchAllBody)) onArm(catchAllType);
        popScope();
    }
    if (rescue.inlineReturnExpr) inferExpr(*rescue.inlineReturnExpr);
}

} // namespace kex::semantic

namespace kex::semantic {

// A generic several parameters share is one type: `same : A -> A -> Bool`
// does not take `1` and `"s"`, nor `Process<A>.send` a String for a
// `Process<Msg>`. The arguments before bind it; the first one after that
// fits neither way is the conflict, with the type it had to be. Two
// numbers always agree (the numeric tower); an open type agrees.
auto TypeChecker::genericConflict(
    const Signature& sig, const std::vector<TypePtr>& argTypes,
    const std::function<TypePtr(std::size_t, const TypePtr&)>& selectedType)
    -> std::optional<std::pair<std::size_t, TypePtr>> {
    std::unordered_map<int, TypePtr> bound;
    std::function<TypePtr(const TypePtr&, const TypePtr&)> conflict =
        [&](const TypePtr& pattern, const TypePtr& given) -> TypePtr {
            if (!pattern || !given) return nullptr;
            const auto actual = resolve(given);
            if (containsOpenType(actual)) return nullptr;
            int id = 0;
            if (const auto* tv = std::get_if<TypeVar>(&pattern->kind)) id = tv->id;
            if (const auto* c = std::get_if<ConstrainedType>(&pattern->kind)) id = c->genericId;
            if (id < 0) {
                const auto [it, fresh] = bound.try_emplace(id, actual);
                if (fresh || argMatchesParam(actual, it->second) ||
                    argMatchesParam(it->second, actual) ||
                    (isNumericType(actual) && isNumericType(it->second)))
                    return nullptr;
                return it->second;
            }
            const auto both = [&](const TypePtr& p, const TypePtr& a) { return conflict(p, a); };
            if (const auto* pn = std::get_if<NamedType>(&pattern->kind)) {
                const auto* an = std::get_if<NamedType>(&actual->kind);
                if (!an || pn->name != an->name || pn->typeArgs.size() != an->typeArgs.size())
                    return nullptr;
                for (std::size_t k = 0; k < pn->typeArgs.size(); ++k)
                    if (both(pn->typeArgs[k], an->typeArgs[k])) return pattern;
            } else if (const auto* pl = std::get_if<ListType>(&pattern->kind)) {
                if (const auto* al = std::get_if<ListType>(&actual->kind);
                    al && both(pl->element, al->element))
                    return pattern;
            } else if (const auto* po = std::get_if<OptionalType>(&pattern->kind)) {
                if (const auto* ao = std::get_if<OptionalType>(&actual->kind);
                    ao && both(po->inner, ao->inner))
                    return pattern;
            }
            return nullptr;
        };
    for (std::size_t i = 0; i < argTypes.size() && i < sig.params.size(); ++i) {
        const auto selected = selectedType(i, sig.params[i]);
        if (auto expected = conflict(sig.params[i], selected ? selected : argTypes[i]))
            return std::pair{i, expected};
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
            std::holds_alternative<ast::IntLiteral>(unary->operand->kind))
            return false;
        const auto* integer = std::get_if<ast::IntLiteral>(&literal.kind);
        if (!integer) return true;
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
        if (!check(value)) error(value.location, "Byte literal must be in 0..255");
        return;
    }
    const auto* list = std::get_if<ListType>(&target->kind);
    const auto* written = std::get_if<ast::ListExpr>(&value.kind);
    if (!list || !written || !isByte(list->element)) return;
    for (const auto& element : written->elements)
        if (element && !check(*element))
            error(element->location, "Byte literal must be in 0..255");
}

} // namespace kex::semantic

namespace kex::semantic {

// Whether `name` is a type this program knows by that name: a built-in, a
// record, an ADT or an alias. What a type name in value position, or a
// parameter written as one, has to be to stand for the type.
auto TypeChecker::namesKnownType(const std::string& name) const -> bool {
    return isPrimitiveTypeName(name) || m_recordFields.count(resolveRecordName(name)) ||
           m_adtVariants.count(name) || m_typeAliases.count(name);
}

} // namespace kex::semantic
