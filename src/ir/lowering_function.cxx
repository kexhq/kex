// Function / program methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"

namespace kex::ir {

// A single param → an IR pattern (var name or a destructuring pattern).
auto Lowering::lowerParam(const ast::Param& p) -> PatternPtr {
    // A `_` parameter is a wildcard, like an unnamed one: as a variable,
    // two of them (`let seventh(_: Integer, _: Integer)`) were a
    // duplicate `_` in the clause head and erlc refused the module.
    if (p.name && *p.name != "_") {
        auto pat = std::make_unique<Pattern>();
        pat->kind = PatKind::Var; pat->name = *p.name;
        subst[*p.name] = *p.name;
        return pat;
    }
    if (p.pattern) return lowerPattern(*p.pattern);
    auto w = std::make_unique<Pattern>(); w->kind = PatKind::Wild; return w;
}

// Lower a group of same-name FunctionDefs (Kex writes multi-clause
// functions as separate `let` declarations) into one multi-clause FunDef.
// implicitThisName: when non-empty, a receiver param of that name is
// prepended to every clause (make-block methods: `this`).
// Can `typeGuard` actually tell this written type apart at runtime? A
// record tag, an ADT, a primitive with a guard BIF, or a structural
// spelling can; a type variable or an unconstrained name cannot, and
// guarding on one would emit a tag comparison nothing satisfies.
auto Lowering::discriminatingType(const std::string& written) const -> bool {
    if (written.empty() || written == "Any") return false;
    if (!traitIdentityOf(written).empty()) return false;
    if (auto alias = structuralTypeAliases.find(written);
        alias != structuralTypeAliases.end() && alias->second != written)
        return discriminatingType(alias->second);
    if (written.find("->") != std::string::npos) return true;
    if (written.rfind("Block<", 0) == 0 && written.back() == '>')
        return true;
    const char first = written.front(), last = written.back();
    if ((first == '[' && last == ']') || (first == '{' && last == '}') ||
        (first == '(' && last == ')'))
        return true;
    auto base = written;
    if (auto angle = base.find('<');
        angle != std::string::npos && base.back() == '>')
        base.resize(angle);
    if (base.size() == 1) return false;  // a type PARAMETER, not a type
    if (primGuardBifs().count(base) || base == "Char") return true;
    // An ADT TYPE discriminates through `adtGuard`, which tests each of
    // its variant tags — the nullary ones as bare atoms. Before that
    // existed, typeGuard's tagged fallback asked `element(1, V)`, which no
    // atom satisfies, so guarding on an ADT rejected every call and such a
    // clause had to be left unguarded. That is how a free function
    // overloaded on two ADTs answered from its first clause on BEAM while
    // the walker dispatched it (docs/issue-free-function-overloads.md).
    if (auto tags = typeVariantTags.find(base);
        tags != typeVariantTags.end())
        return !tags->second.empty();
    if (records.count(canonicalRecordName(base))) return true;
    return localTypeModules.count(base) > 0 ||
           moduleDeclaredTypes.count(base) > 0;
}

auto Lowering::lowerFunctionGroup(const std::vector<const ast::FunctionDef*>& group,
                        const std::string& implicitThisName ,
                        const std::string& nameOverride ) -> FunDef {
    FunDef def;
    def.name = nameOverride.empty() ? group[0]->name : nameOverride;
    int explicitArity = group[0]->clauses.empty()
        ? 0 : static_cast<int>(group[0]->clauses[0].params.size());
    auto traitNameOf = [&](const ast::Param& param) -> std::string {
        if (!param.type || !*param.type) return "";
        const auto* name = std::get_if<ast::TypeName>(&(*param.type)->kind);
        if (!name || name->parts.empty()) return "";
        std::string written;
        for (const auto& part : name->parts)
            written += (written.empty() ? "" : ".") + part;
        return traitIdentityOf(written);
    };
    int dictionaryArity = 0;
    if (!group[0]->clauses.empty())
        for (const auto& param : group[0]->clauses[0].params)
            if (auto trait = traitNameOf(param); !trait.empty()) {
                ++dictionaryArity;
                def.dictionaryTraits.push_back(trait);
            }
    // `make Shape do` inside `module Geo` targets the trait `Geo.Shape`.
    const auto makeTrait = implicitThisName.empty()
        ? std::string{} : traitIdentityOf(currentMakeType, currentModulePath, true);
    const bool implicitThisDictionary = !makeTrait.empty() &&
        std::none_of(
            traitMethods.at(makeTrait).begin(),
            traitMethods.at(makeTrait).end(),
            [&](const auto& required) {
                return required.name == group[0]->name;
            });
    if (implicitThisDictionary) {
        ++dictionaryArity;
        def.dictionaryTraits.push_back(makeTrait);
    }
    // Overloads of a name must share purity, so the group's first clause
    // decides. A capability's OWN members are excluded: they are the
    // default implementation, and giving them a context would change
    // 'Clock.now'/0 into /1 and break the very fallback dispatch uses.
    const bool foulDef = !group.empty() && group[0]->isFoul;
    // A capability's own members are foul like any others and take the
    // context too. Excluding them made their arity disagree with every
    // threaded caller — the substitution dispatch passes the context
    // through as well (kexhq/kex#181).
    const bool wantsContext = threadsCapabilities() && foulDef;
    def.arity = explicitArity + dictionaryArity +
                (implicitThisName.empty() ? 0 : 1) +
                (wantsContext ? 1 : 0);
    def.hasCapabilityContext = wantsContext;
    auto savedModulePath = currentModulePath;
    auto savedTraitDictionaries = traitDictionaries;
    auto savedFunctionSymbol = currentFunctionSymbol;
    currentFunctionSymbol = def.name;
    // Do the clauses in this group differ in their declared parameter
    // types? Only then is a guard wanted — a plain multi-clause function
    // discriminates with patterns and must be left alone.
    // A NAMED record-destructure param (`JSON { status, body }`) discards
    // its own tag at the Core Erlang pattern level — it becomes a plain
    // Var (see the record-pattern special case in the clause loop below)
    // because a record's fields are read by name, not position, so no
    // single structural pattern can express it. That makes it invisible
    // to Core Erlang's own dispatch, unlike a constructor pattern like
    // `@Equal` (a real tagged-tuple pattern needing no guard at all) —
    // so unlike those, it NEEDS the same runtime type guard a plain
    // `reply: JSON` parameter would get. An ANONYMOUS record pattern
    // (`{ status, body }`, no type name) stays excluded: it matches any
    // record with those fields, which is structural, not a single tag.
    auto namedRecordPatternType = [](const ast::Param& p) -> std::string {
        if (p.name || !p.pattern) return "";
        if (auto* rp = std::get_if<ast::RecordPattern>(&(*p.pattern)->kind))
            return rp->typeName;
        return "";
    };
    const bool overloadedByParamType = [&] {
        // Clauses, not declarations: overloads folded into one function
        // (kexhq/kex#262) arrive as a single FunctionDef.
        std::size_t clauses = 0;
        for (const auto* fn : group) clauses += fn->clauses.size();
        if (clauses < 2) return false;
        std::vector<std::string> signature;
        bool differs = false;
        for (const auto* fn : group)
            for (const auto& clause : fn->clauses) {
                std::vector<std::string> current;
                for (const auto& p : clause.params) {
                    if (auto rpType = namedRecordPatternType(p); !rpType.empty()) {
                        current.push_back(rpType);
                        continue;
                    }
                    // A group that already discriminates with PATTERNS
                    // needs no guard, and adding one breaks it: Ordering's
                    // `combine(@Equal, other: Ordering)` / `combine(@Less,
                    // _)` differ in their declared types only because the
                    // later clauses spell their second parameter `_`.
                    // Guarding the first then rejected `Equal.combine(Less)`
                    // and the whole clause set fell through to
                    // function_clause.
                    const auto* declared = dispatchParamType(p);
                    if (!p.name || !declared) return false;
                    current.push_back(renderDispatchType(*declared));
                }
                if (signature.empty()) signature = current;
                else if (signature != current) differs = true;
            }
        return differs;
    }();
    std::size_t clauseCount = 0;
    for (const auto* fn : group) clauseCount += fn->clauses.size();
    std::size_t clauseIndex = 0;
    for (const auto* fn : group) {
        for (const auto& clause : fn->clauses) {
            const bool isLastInGroup = ++clauseIndex == clauseCount;
            subst.clear(); // fresh scope per clause
            traitDictionaries = savedTraitDictionaries;
            FunClause fc;
            if (!implicitThisName.empty()) {
                subst[implicitThisName] = implicitThisName;
                // The make-local `new` begins as a reference copy of the
                // receiver. It emits no binding or runtime check; only a
                // later field assignment creates a fresh record value.
                subst["new"] = implicitThisName;
                immutableBindings.erase("new");
                auto pat = std::make_unique<Pattern>();
                pat->kind = PatKind::Var; pat->name = implicitThisName;
                fc.params.push_back(std::move(pat));
            }
            // A record-destructure or range receiver param can't be a
            // structural Core Erlang pattern (fields are read by NAME, not
            // position, and a range is a materialized list) — bind it to a
            // fresh var and prepend field/element bindings to the body.
            std::vector<std::pair<std::string, ExprPtr>> prefix;
            std::vector<std::pair<std::string, const ast::RecordPattern*>> recordPatterns;
            // (writtenType, freshVarName) for each NAMED record-pattern
            // param — the guard-generation step below tests these
            // against the fresh var substituted for the pattern, since
            // there is no `p.name` to test against directly.
            std::vector<std::pair<std::string, std::string>> recordDispatchTargets;
            for (const auto& p : clause.params) {
                if (!p.name && p.pattern) {
                    auto& pk = (*p.pattern)->kind;
                    if (auto* rp = std::get_if<ast::RecordPattern>(&pk)) {
                        std::string rv = fresh("rec");
                        auto vp = std::make_unique<Pattern>(); vp->kind = PatKind::Var; vp->name = rv;
                        fc.params.push_back(std::move(vp));
                        recordPatterns.push_back({rv, rp});
                        if (!rp->typeName.empty())
                            recordDispatchTargets.push_back({rp->typeName, rv});
                        std::vector<std::string> names;
                        collectRecordBindings(*rp, names);
                        for (const auto& name : names) subst[name] = name;
                        continue;
                    }
                    if (auto* rgp = std::get_if<ast::RangePattern>(&pk)) {
                        std::string rv = fresh("rng");
                        auto vp = std::make_unique<Pattern>(); vp->kind = PatKind::Var; vp->name = rv;
                        fc.params.push_back(std::move(vp));
                        if (rgp->start) if (auto* sv = std::get_if<ast::VarPattern>(&rgp->start->kind))
                            prefix.push_back({sv->name, callE("kex_intrinsic_range","first",1,one(var(rv)))});
                        if (rgp->end) if (auto* ev = std::get_if<ast::VarPattern>(&rgp->end->kind))
                            prefix.push_back({ev->name, callE("kex_intrinsic_range","last",1,one(var(rv)))});
                        continue;
                    }
                }
                fc.params.push_back(lowerParam(p));
            }
            // Declared parameter types, for the arithmetic routing in
            // lowerBinary: `let double(n: Integer) = n * 2` must keep the
            // raw BIF, and the annotation says so even when no type
            // information from the checker is available (the codegen tests
            // lower without it).
            auto savedParamTypes = declaredParamTypes;
            for (const auto& p : clause.params)
                if (p.name && p.type && *p.type)
                    declaredParamTypes[*p.name] = renderDispatchType(**p.type);
            for (const auto& p : clause.params) {
                auto trait = traitNameOf(p);
                if (trait.empty()) continue;
                if (!p.name)
                    throw LowerError(
                        "IR lower: trait-typed dictionary parameter must be named");
                auto dictionary = fresh("Dict_" + *p.name + "_" + trait);
                auto pat = std::make_unique<Pattern>();
                pat->kind = PatKind::Var;
                pat->name = dictionary;
                fc.params.push_back(std::move(pat));
                traitDictionaries[*p.name][trait] = dictionary;
            }
            if (implicitThisDictionary) {
                auto dictionary = fresh("Dict_" + implicitThisName + "_" +
                                        currentMakeType);
                auto pat = std::make_unique<Pattern>();
                pat->kind = PatKind::Var;
                pat->name = dictionary;
                fc.params.push_back(std::move(pat));
                traitDictionaries[implicitThisName][currentMakeType] =
                    dictionary;
            }
            // The context is the last parameter, after any trait
            // dictionaries, and is in scope for the whole body so calls
            // inside it pass it along (kexhq/kex#181).
            auto savedContextVar = currentContextVar;
            if (wantsContext) {
                auto contextName = fresh("Ctx");
                auto contextPat = std::make_unique<Pattern>();
                contextPat->kind = PatKind::Var;
                contextPat->name = contextName;
                fc.params.push_back(std::move(contextPat));
                currentContextVar = contextName;
            } else {
                currentContextVar.clear();
            }
            // Overloads discriminated by PARAMETER TYPE need a runtime
            // guard: a declared type is not a Core Erlang pattern, so two
            // clauses whose params are plain variables are identical to
            // the emitter and the FIRST one swallowed every call.
            // `let describe(v: Alpha)` / `let describe(v: Beta)` answered
            // "Alpha" for both on BEAM while the walker dispatched
            // correctly, and moving Net.Socket's `close`/`closed?`
            // overloads inside `module TCP` made a listener reach the
            // connection clause (kexhq/kex#230). The LAST clause of a
            // group stays unguarded so it remains the fallback, which is
            // the behaviour every existing single-clause group already
            // has.
            if (overloadedByParamType && !isLastInGroup) {
                ExprPtr guard;
                auto addGuardTest = [&](const std::string& written, ExprPtr subject) {
                    if (!discriminatingType(written)) return;
                    auto test = typeGuard(written, std::move(subject));
                    guard = guard ? intrin(Op::And, two(std::move(guard),
                                                        std::move(test)))
                                  : std::move(test);
                };
                for (const auto& p : clause.params) {
                    const auto* declared = dispatchParamType(p);
                    if (!p.name || !declared) continue;
                    addGuardTest(renderDispatchType(*declared), var(*p.name));
                }
                // Named record-pattern params (`JSON { status, body }`)
                // dispatch on their tag exactly like a plain `reply: JSON`
                // parameter would, but against the fresh var substituted
                // for the pattern (kexhq/kex#347).
                for (const auto& [written, rv] : recordDispatchTargets)
                    addGuardTest(written, var(rv));
                if (guard) fc.guard = std::move(guard);
            }
            for (const auto& [nm, _] : prefix) subst[nm] = nm;
            if (fn->isSlot) subst["from"] = "from";
            ExprPtr body = lowerBody(clause.body);
            if (fn->isSlot)
                body = makeLet(
                    "from",
                    callE("erlang", "get", 1,
                          one(lit(LitKind::Atom, "kex_serving_from"))),
                    std::move(body));
            for (auto it = prefix.rbegin(); it != prefix.rend(); ++it)
                body = makeLet(it->first, std::move(it->second), std::move(body));
            for (auto it = recordPatterns.rbegin(); it != recordPatterns.rend(); ++it)
                body = wrapRecordPattern(it->first, *it->second, std::move(body));
            if (clause.rescue) body = wrapWithTryCatch(std::move(body), *clause.rescue);
            else if (irHasEscapingTryThrow(body)) body = wrapPropagateTryError(std::move(body));
            fc.body = std::move(body);
            currentContextVar = savedContextVar;
            declaredParamTypes = std::move(savedParamTypes);
            def.clauses.push_back(std::move(fc));
        }
    }
    traitDictionaries = std::move(savedTraitDictionaries);
    currentModulePath = std::move(savedModulePath);
    currentFunctionSymbol = std::move(savedFunctionSymbol);
    return def;
}

auto Lowering::lowerFunction(const ast::FunctionDef& fn, const std::string& implicitThisName )
    -> FunDef {
    return lowerFunctionGroup({&fn}, implicitThisName);
}

} // namespace kex::ir
