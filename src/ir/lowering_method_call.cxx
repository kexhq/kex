// Method-call lowering methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"
#include "../common/named_arguments.hxx"
#include "../common/mangle.hxx"
#include <algorithm>
#include "../common/well_known.hxx"

namespace kex::ir {

auto Lowering::lowerMethodCallImpl(const ast::MethodCall& n) -> ExprPtr {
    // A bare mutating `!` call used where its rebind can't be applied is
    // a runtime error (matching the walker's behaviour). Statement-position
    // `!` calls are lowered by the caller as value + rebind.
    if (n.mutating && !m_lowerMutatingAsValue) {
        std::string loc;
        if (currentLoc) {
            loc = std::string(currentLoc->file) + ":" + std::to_string(currentLoc->line) + ":"
                + std::to_string(currentLoc->column) + ": ";
        }
        return callE("erlang", "error", 1, one(
            lit(LitKind::String, loc + "runtime error: '!' requires a variable binding as the receiver")));
    }
    // The receiver type's own implementation, where the run-time dispatch
    // cannot find it (ownImplementationOf): a distinct type's value is erased.
    if (const auto own = ownImplementationOf(n);
        own.reason == OwnImplementation::Reason::DistinctType) {
        std::vector<ExprPtr> args;
        args.push_back(lower(n.receiver));
        for (const auto& arg : n.args) args.push_back(lower(arg));
        const auto arity = static_cast<int>(args.size());
        auto call = std::make_unique<Expr>();
        call->node = Call{std::string{}, own.symbol, arity, std::move(args), false};
        return call;
    }
    // The calls the checker decided by what they name, each to its handler.
    switch (const auto& target = hir->targetOf(n); target.kind) {
    case hir::TargetKind::Foreign:
        return lowerRuntimeCall(n, target.owner);
    case hir::TargetKind::Builtin:
        return names::isIntrinsicPath(target.owner) ? lowerRuntimeCall(n, target.owner)
                                                    : lowerProcessSpawn(n, target.owner);
    case hir::TargetKind::Conversion:
        return lowerConversion(n, target.owner);
    case hir::TargetKind::Intrinsic:
        if (const auto* answer = hir->typeOfAnswer(n)) return lowerTypeOf(n, *answer);
        break;
    case hir::TargetKind::TraitMethod:
        if (auto call = lowerTraitDictionaryCall(n)) return call;
        break;
    case hir::TargetKind::FieldRead:
        if (auto field = thisFieldRead(n)) return field;
        if (auto field = staticFieldRead(n)) return field;
        break;
    default:
        break;
    }
    // The module a call written on a module path names, as the checker
    // decided it (`Util` of `Util.double(21)`).
    std::vector<std::string> path;
    const auto calledModule =
        modulePath(*n.receiver, path) ? checkedCallModule(n) : std::string{};
    if (const auto* resolved = hir->resolvedTargetOf(n)) {
        if (!resolved->localDispatchTypes.empty() &&
            resolved->localDispatchTypes.front().starts_with("Server<"))
            return lowerServerSlotCall(n, *resolved);
        // Semantic analysis has already applied lexical `using` policy
        // and argument-type specificity. Once it names an imported
        // target, source-import priority must not override that choice.
        // A type's own method, inherited trait defaults included, is the
        // checker's decision too (ownerOfCall): it records no target then.
        // A call into a capability must reach the substitution dispatch,
        // which lives on the module-qualified path below. If the resolver
        // claims it first the call goes straight to the real
        // implementation and `with` silently does nothing — the fail-open
        // this feature exists to prevent (kexhq/kex#143).
        const auto capabilityTarget =
            isCapabilityModule(calledModule) && capabilityInterfaceHas(calledModule, n.method)
                ? calledModule
                : std::string{};
        return lowerResolvedCall(n, *resolved, capabilityTarget);
    }
    if (!calledModule.empty())
        if (auto call = lowerModuleFunctionCall(n, calledModule, path)) return call;
    if (const auto* type = std::get_if<ast::UpperIdentifier>(&n.receiver->kind))
        if (auto call = lowerTypeNamespaceCall(n, *type)) return call;
    return lowerValueReceiverCall(n);
}

// A method call on a value receiver that no earlier case claimed: the
// receiver atomized once, then the case the checker decided.
auto Lowering::lowerValueReceiverCall(const ast::MethodCall& n) -> ExprPtr {
    // UFCS method on a value receiver. Atomize the receiver once so
    // methods that use it several times (min/get/count/…) don't
    // re-evaluate it; `ret` wraps the result in that binding.
    std::vector<Binding> rb;
    auto rv_ = atomize_ir(lower(n.receiver), rb);
    // Extract the atomic name so we can produce fresh Var refs on demand
    // without relying on cloning the original unique_ptr (avoids a
    // GCC-specific null-after-move issue on Linux CI).
    auto rvName = std::get_if<Var>(&rv_->node)
        ? std::get_if<Var>(&rv_->node)->name : std::string{};
    auto rvLit = std::get_if<Lit>(&rv_->node)
        ? std::optional<Lit>{*std::get_if<Lit>(&rv_->node)} : std::nullopt;
    auto rv = [&]() -> ExprPtr {
        if (!rvName.empty()) return var(rvName);
        if (rvLit) { auto e = std::make_unique<Expr>(); e->node = *rvLit; return e; }
        return var("_rv_bug");
    };
    auto& m = n.method;
    auto ret = [&](ExprPtr e) { return wrapLets(rb, std::move(e)); };

    // The receiver type's own implementation (ownImplementationOf): a
    // module's free function over the module's own type (kexhq/kex#230).
    if (const auto own = ownImplementationOf(n);
        own.reason == OwnImplementation::Reason::ModuleFunction) {
        std::vector<ExprPtr> args;
        args.push_back(rv());
        for (const auto& a : n.args)
            args.push_back(atomize_ir(lower(a), rb));
        return ret(localCallExpr(own.symbol, std::move(args)));
    }

    // A function of a module of this unit the checker selected for the call
    // (`10.double` under `using Utils`): `double(10)`, the receiver first.
    if (const auto* selected = hir->selectedSignatureOf(n);
        selected && !selected->declaringModule.empty() && n.namedArgs.empty())
        if (const auto emitted =
                moduleFunctions.find(selected->declaringModule + "." + n.method);
            emitted != moduleFunctions.end()) {
            std::vector<ExprPtr> args;
            args.push_back(rv());
            for (const auto& value : n.args) args.push_back(atomize_ir(lower(value), rb));
            if (n.block) args.push_back(atomize_ir(lower(*n.block), rb));
            return ret(localCallExpr(emitted->second, std::move(args)));
        }

    // A local method only claims the call when it is defined at THIS
    // arity; otherwise the external/prelude receiver function handles it.
    // `this.map.get(k, default)` inside a `make Box do get(key) ... end`
    // block used to compile to a local get/3 that was never defined.
    // A call that labels arguments or leaves defaulted ones out is written
    // shorter than the method it binds, whose arity is the one that counts.
    const MakeMethod* declared = makeMethodFor(n);
    const int ufcsArity = declared
        ? static_cast<int>(declared->def->clauses[0].params.size()) + 1
        : static_cast<int>(n.args.size() + n.namedArgs.size()) + 1 +
              (n.block ? 1 : 0);
    const bool localArityExists =
        localMethodArities.empty() ||
        localMethodArities.count(m + "/" + std::to_string(ufcsArity)) > 0;

    // The checker decided a call only a `make` of a module not in scope
    // here answers (`Hidden`): the same "Undefined method" the walker
    // raises, naming the missing `using` when labelled arguments chose it.
    if (hir->targetOf(n).kind == hir::TargetKind::Hidden) {
        const auto& hidden = hir->targetOf(n);
        const auto hint = hidden.owner.empty() || n.namedArgs.empty() ? std::string{}
            : " — `" + hidden.owner + "` defines a matching `" + n.method +
                  "`; add `using " + hidden.owner + "`";
        return ret(undefinedMethod(n.method, rv(), hint));
    }

    // A call the checker left to the value with no target of its own: the
    // one module answering it at this arity, else the prelude's dispatcher,
    // which looks the method up on the value as the walker does. Several
    // modules' providers are the checker's choice (its alternatives), never
    // made here.
    if (externalModules && (!localMethods.count(m) || !localArityExists)) {
        if (const auto found = externalModules->receiverFunctions.find(m);
            found != externalModules->receiverFunctions.end()) {
            const int arity = static_cast<int>(n.args.size() + n.namedArgs.size()) + 1 +
                              (n.block ? 1 : 0);
            std::map<std::string, std::vector<const ExternalModules::ReceiverFunction*>> byModule;
            // A unit's `make` for a type declared outside its module, out of
            // scope here, was decided `Hidden` by the checker.
            for (const auto& candidate : found->second)
                if (candidate.beamArity == arity)
                    byModule[candidate.moduleAtom].push_back(&candidate);
            const auto chosen = byModule.size() == 1 ? byModule.begin()
                                                     : byModule.find(names::kPreludeModule);
            if (chosen != byModule.end()) {
                if (!n.namedArgs.empty())
                    throw LowerError("IR lower: named arguments to `" + m +
                                     "` reached lowering undecided");
                std::vector<ExprPtr> args;
                args.push_back(rv());
                for (const auto& a : n.args) args.push_back(atomize_ir(lower(a), rb));
                if (n.block) args.push_back(atomize_ir(lower(*n.block), rb));
                const auto& providers = chosen->second;
                const auto function = providers.size() == 1 ? providers.front()->beamFunction : m;
                return ret(callE(chosen->first, function, arity, std::move(args)));
            }
            if (!byModule.empty())
                return ret(runtimeError("Ambiguous receiver function: " + m + "/" +
                                        std::to_string(arity)));
        }
    }
    // Generic UFCS fallback: a field access or a make-block method are
    // BOTH emitted as local functions taking the receiver first, so
    // `x.foo(a, b)` → `apply 'foo'/3(x, a, b)`. This is exactly how the
    // string emitter resolves record field access and make-block methods
    // without needing types. Gated on localMethods so an UNPORTED builtin
    // (e.g. `.get`/`.map`) errors loudly instead of silently becoming a
    // call to a function that doesn't exist. A trailing block (`x.some { ...
    // }`) is passed as the final argument — the block-typed parameter — to
    // match the interpreter, which appends the evaluated block to the args.
    // Only take the local path when a local definition exists at THIS
    // arity. `this.map.get(k, default)` inside a `make Box do get(key) ...`
    // block otherwise compiled to a local get/3 that was never defined,
    // because the name alone matched. Falling through lets the prelude's
    // receiver function (Map.get here) handle it, which is what the
    // interpreter already does.
    if (localMethods.count(n.method) && localArityExists) {
        std::vector<Binding> binds;
        std::vector<ExprPtr> args;
        args.push_back(rv());
        if (const auto* placed =
                declared) {
            for (auto& slot : placeMethodArguments(n, *placed, binds))
                args.push_back(std::move(slot));
        } else {
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            // Make-block methods lower receiver-first. Named arguments
            // follow positional ones in the common `method(Type, name:
            // value)` form.
            for (const auto& [_, value] : n.namedArgs)
                args.push_back(atomize(value, binds));
            if (n.block) args.push_back(atomize(*n.block, binds));
        }
        auto ex = std::make_unique<Expr>();
        // The make function the checker selected says whether it takes the
        // capability context. By name and arity alone a pure `Headers.remove(name)`
        // elsewhere in the program answered for a foul
        // `Repository.remove(id)`, which then went out one argument
        // short and reached the wrong arity's dispatcher: "Undefined
        // method: remove" once Net.HTTP was linked (kexhq/kex#451).
        if (const auto* owned = declared ? declared : selectedMakeMethod(n)) {
            if (threadsCapabilities() && owned->def->isFoul)
                args.push_back(currentContext());
            const int callArity = static_cast<int>(args.size());
            ex->node = Call{"", n.method, callArity, std::move(args), false};
        } else {
            ex->node = localCall(n.method, std::move(args));
        }
        return ret(wrapLets(binds, std::move(ex)));
    }
    // External loaded module methods (UFCS): tree.size → 'Kex.BinaryTree':'Tree.size'(tree)
    return ret(undefinedMethodOrField(n, rv()));
}

// `Type.of(x)` the checker answered (`Intrinsic`): the recorded shape as a
// literal record, ahead of every dispatch path, like the walker's.
auto Lowering::lowerTypeOf(const ast::MethodCall& n, const semantic::StaticTypeAnswer& answer)
    -> ExprPtr {
    std::vector<Binding> binds;
    // A value argument still runs (it may have effects); one that
    // NAMES a function does not — `Date.parse` alone is a call
    // missing its argument, which would not even compile.
    if (answer.evaluateArgument)
        for (const auto& arg : n.args)
            if (arg) binds.push_back({fresh("TypeArg"), lower(arg)});
    // A type token from another unit contains no call to that
    // unit. Load its companion before `Type.constructors` (or
    // record-field reflection) reads the layouts it registers.
    if (externalModules) {
        auto owner = answer.type.name;
        // `Type.of(Kinds.Red)` can record only the bare ADT name
        // (`Kind`); the written constructor still names its unit.
        if (!n.args.empty() && n.args.front())
            if (const auto* named = std::get_if<ast::MethodCall>(
                    &n.args.front()->kind)) {
                std::vector<std::string> path;
                if (named->receiver && modulePath(*named->receiver, path)) {
                    std::string source = kex::join(path, ".");
                    if (auto module = externalModules->nameToAtom.find(source);
                        module != externalModules->nameToAtom.end())
                        binds.push_back({fresh("TypeLoaded"),
                            callE("code", "ensure_loaded", 1,
                                  one(lit(LitKind::Atom, module->second)))});
                }
            }
        for (;;) {
            const auto dot = owner.rfind('.');
            if (dot == std::string::npos) break;
            owner.resize(dot);
            if (auto module = externalModules->nameToAtom.find(owner);
                module != externalModules->nameToAtom.end()) {
                binds.push_back({fresh("TypeLoaded"),
                    callE("code", "ensure_loaded", 1,
                          one(lit(LitKind::Atom, module->second)))});
                break;
            }
        }
    }
    return wrapLets(binds, structuredTypeLiteral(answer.type));
}

// A method of the trait a lexical parameter is typed by (`TraitMethod`):
// called through the hidden dictionary its caller supplied, whose tuple
// slots follow the trait declaration's required-method order. Null when
// the receiver has no dictionary holding the method.
auto Lowering::lowerTraitDictionaryCall(const ast::MethodCall& n) -> ExprPtr {
    std::string receiverName;
    if (const auto* receiver = std::get_if<ast::Identifier>(&n.receiver->kind))
        receiverName = receiver->name;
    else if (std::holds_alternative<ast::ThisExpr>(n.receiver->kind))
        receiverName = "this";
    const auto dictionaries = traitDictionaries.find(receiverName);
    if (dictionaries == traitDictionaries.end()) return nullptr;
    for (const auto& [trait, dictionary] : dictionaries->second) {
        const auto methods = traitMethods.find(trait);
        if (methods == traitMethods.end()) continue;
        const auto method = std::find_if(methods->second.begin(), methods->second.end(),
                                         [&](const auto& candidate) { return candidate.name == n.method; });
        if (method == methods->second.end()) continue;
        std::vector<Binding> binds;
        std::vector<ExprPtr> args;
        args.push_back(atomize(n.receiver, binds));
        for (const auto& arg : n.args) args.push_back(atomize(arg, binds));
        if (n.block) args.push_back(atomize(*n.block, binds));
        const auto slot = static_cast<int>(std::distance(methods->second.begin(), method)) + 1;
        auto callee = atomize_ir(callE("erlang", "element", 2, two(litInt(slot), var(dictionary))), binds);
        auto indirect = std::make_unique<Expr>();
        indirect->node = CallIndirect{std::move(callee), std::move(args), false};
        return wrapLets(binds, std::move(indirect));
    }
    return nullptr;
}

// A field of the record a `make` block is lowering, read through `this`
// (`FieldRead`): its slot in this unit's layout. Null for any other read.
auto Lowering::thisFieldRead(const ast::MethodCall& n) -> ExprPtr {
    const bool thisReceiver =
        std::holds_alternative<ast::ThisExpr>(n.receiver->kind) ||
        (std::holds_alternative<ast::Identifier>(n.receiver->kind) &&
         std::get<ast::Identifier>(n.receiver->kind).name == "this");
    if (!thisReceiver || !n.args.empty() || !n.namedArgs.empty() || n.block) return nullptr;
    const auto candidates = fieldAccessors.find(n.method);
    if (candidates == fieldAccessors.end()) return nullptr;
    const auto selected = std::find_if(candidates->second.begin(), candidates->second.end(),
                                       [&](const auto& candidate) { return candidate.record == currentMakeType; });
    if (selected == candidates->second.end()) return nullptr;
    return callE("erlang", "element", 2, two(litInt(selected->position), lower(n.receiver)));
}

// A function of the module the checker named for a call written on a module
// path (`Util.double(21)`, `Util.Math.double(21)`): this unit's, or one
// compiled apart. Null when neither emits it.
auto Lowering::lowerModuleFunctionCall(const ast::MethodCall& n, const std::string& module,
                                       const std::vector<std::string>& path) -> ExprPtr {
    const auto qualKey = module + "." + n.method;
    auto it = moduleFunctions.find(qualKey);
    // A call into a capability asks the context first and
    // falls back to the capability's own body, which is what
    // runs when nothing replaced it (kexhq/kex#181).
    const bool importedCapability =
        externalModules &&
        externalModules->capabilityModules.count(module) > 0;
    if (it != moduleFunctions.end() && threadsCapabilities() &&
        (capabilityModules.count(module) ||
         importedCapability) &&
        capabilityInterfaceHas(module, n.method) &&
        n.namedArgs.empty()) {
        std::vector<Binding> binds;
        std::vector<std::string> argVars;
        for (const auto& a : n.args) {
            auto name = fresh("CapArg");
            binds.push_back({name, atomize(a, binds)});
            argVars.push_back(name);
        }
        // A trailing block is a source argument like any
        // other; leaving it out called the replacement
        // short one argument, which is why block-form
        // capability calls used to skip substitution
        // entirely and silently reach the real
        // implementation.
        if (n.block) {
            auto name = fresh("CapArg");
            binds.push_back({name, atomize(*n.block, binds)});
            argVars.push_back(name);
        }
        auto argsFrom = [&](const std::string& lead) {
            std::vector<ExprPtr> out;
            if (!lead.empty()) out.push_back(var(lead));
            for (const auto& v : argVars) out.push_back(var(v));
            return out;
        };
        std::vector<ExprPtr> getArgs;
        getArgs.push_back(lit(LitKind::String, module));
        getArgs.push_back(currentContext());
        getArgs.push_back(lit(LitKind::Atom, "undefined"));
        auto impl = fresh("Impl");
        Match m;
        m.subjects.push_back(
            callE("maps", "get", 3, std::move(getArgs)));
        MatchClause missing;
        auto undef = atomPattern("undefined");
        missing.patterns.push_back(std::move(undef));
        auto direct = std::make_unique<Expr>();
        direct->node = localCall(it->second, argsFrom(""));
        missing.body = std::move(direct);
        m.clauses.push_back(std::move(missing));
        MatchClause replaced;
        auto bound = std::make_unique<Pattern>();
        bound->kind = PatKind::Var;
        bound->name = impl;
        replaced.patterns.push_back(std::move(bound));
        // Runtime dispatch: the implementation replacing the
        // capability is typically defined in a different unit
        // from this call site, so there is no local symbol to
        // name (kexhq/kex#181).
        std::vector<ExprPtr> passed;
        for (const auto& v : argVars) passed.push_back(var(v));
        // A capability's members are foul by construction, so
        // the implementation takes the context too — the
        // runtime dispatch has to hand it over just as a
        // direct call would.
        passed.push_back(currentContext());
        auto passedList = std::make_unique<Expr>();
        passedList->node =
            MakeList{std::move(passed), std::nullopt};
        std::vector<ExprPtr> dispatchArgs;
        dispatchArgs.push_back(var(impl));
        dispatchArgs.push_back(lit(LitKind::String, n.method));
        dispatchArgs.push_back(std::move(passedList));
        auto dispatched =
            callE("kex_io", "dispatch_method", 3,
                  std::move(dispatchArgs));
        replaced.body = std::move(dispatched);
        m.clauses.push_back(std::move(replaced));
        auto chosen = std::make_unique<Expr>();
        chosen->node = std::move(m);
        return wrapLets(binds, std::move(chosen));
    }
    if (it != moduleFunctions.end()) {
        std::vector<Binding> binds;
        if (!n.namedArgs.empty()) {
            auto pit = fnParamNames.find(qualKey);
            if (pit == fnParamNames.end())
                throw LowerError("IR lower: named args to module function with unknown params: " + qualKey);
            // The clause the checker chose for these labels.
            const auto* signature = hir->selectedSignatureOf(n);
            const auto* selected = signature ? signature->clause : nullptr;
            const auto pnames = clauseNames(selected, pit->second);
            std::vector<std::string> labels;
            for (const auto& [label, _] : n.namedArgs)
                labels.push_back(label);
            const auto placed = placeArguments(
                pnames, labels, n.args.size() + bool(n.block));
            std::vector<ExprPtr> slots(pnames.size());
            for (size_t i = 0; i < n.namedArgs.size(); ++i)
                if (placed.labelSlots[i] != PlacedArguments::npos)
                    slots[placed.labelSlots[i]] =
                        atomize(n.namedArgs[i].second, binds);
            std::vector<ExprPtr> positional;
            for (const auto& a : n.args) positional.push_back(atomize(a, binds));
            if (n.block) positional.push_back(atomize(*n.block, binds));
            for (size_t k = 0; k < positional.size(); ++k)
                if (placed.positionalSlots[k] != PlacedArguments::npos)
                    slots[placed.positionalSlots[k]] =
                        std::move(positional[k]);
            fillDefaultSlots(qualKey, slots, binds, selected);
            return wrapLets(binds, localCallExpr(it->second, std::move(slots)));
        }
        std::vector<ExprPtr> args;
        for (const auto& a : n.args) args.push_back(atomize(a, binds));
        if (n.block) args.push_back(atomize(*n.block, binds));
        if (auto defaults = fnDefaults.find(qualKey);
            defaults != fnDefaults.end() && args.size() < defaults->second.size() &&
            !(n.block && blockFitsExactOverload(qualKey, args.size()))) {
            args.resize(defaults->second.size());
            fillDefaultSlots(qualKey, args, binds);
        }
        // `Module.name` may be a zero-param VALUE binding
        // rather than a real function — `let render =
        // Template.html(...)` in `module M do ... end`,
        // reached here as `M.render(x)` from OUTSIDE it (the
        // identical call from WITHIN the same module is
        // `lowerFunctionCall`'s own `zeroArgThunk` check —
        // see its comment for the full story). Its only
        // emitted arity is 0 (the thunk that hands back the
        // closure), so calling it directly at this call's own
        // arity emitted a call to an arity that was never
        // defined and erlc rejected the module outright:
        // `undefined function 'M.render'/1`, a dangling
        // reference to something that only ever existed as
        // `render/0`. Evaluate the thunk first, then apply
        // the supplied args to what it returns.
        const auto clauses = fnNamedClauses.find(qualKey);
        const bool hasMatchingOverload =
            clauses != fnNamedClauses.end() &&
            std::any_of(clauses->second.begin(), clauses->second.end(),
                        [&](const ast::FunctionClause* clause) {
                            return clause->params.size() == args.size();
                        });
        if (!args.empty() && emittedModuleZeroArg.count(it->second) &&
            !hasMatchingOverload) {
            auto thunk = std::make_unique<Expr>();
            thunk->node = localCall(it->second, {});
            auto indirect = std::make_unique<Expr>();
            indirect->node = CallIndirect{
                std::move(thunk), std::move(args), false};
            return wrapLets(binds, std::move(indirect));
        }
        return wrapLets(binds, localCallExpr(it->second, std::move(args)));
    }
    // External loaded modules: BinaryTree.fromList → 'Kex.BinaryTree':fromList
    // Skip when the receiver names a local type that owns this
    // method — local types shadow prelude modules with the same name.
    if (!externalModules || (path.size() == 1 && typeOwnsStaticFunction(path[0], n.method)))
        return nullptr;
    auto eit = externalModules->exportToBeamFn.find(qualKey);
    if (eit != externalModules->exportToBeamFn.end()) {
        auto ait = externalModules->nameToAtom.find(module);
        if (ait != externalModules->nameToAtom.end()) {
            std::vector<Binding> binds;
            if (!n.namedArgs.empty())
                throw LowerError("IR lower: named arguments to `" + qualKey +
                                 "` reached lowering unplaced");
            std::vector<ExprPtr> args;
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            if (n.block) args.push_back(atomize(*n.block, binds));
            int ar = static_cast<int>(args.size());
            // A zero-argument binding can hold a callable
            // value, such as a Template. Apply the value
            // returned by `page/0`; there is no `page/1`
            // in the other unit to call directly.
            if (auto declared = externalModules->exportArity.find(qualKey);
                ar > 0 && declared != externalModules->exportArity.end() &&
                declared->second == 0) {
                auto callee = externalCallExpr(
                    qualKey, ait->second, eit->second, 0, {});
                auto indirect = std::make_unique<Expr>();
                indirect->node = CallIndirect{
                    std::move(callee), std::move(args), false};
                return wrapLets(binds, std::move(indirect));
            }
            return wrapLets(binds,
                externalCallExpr(qualKey, ait->second, eit->second, ar, std::move(args)));
        }
    }
    return nullptr;
}

// A call on a capitalised name no module path claimed (`Organization.slugged`,
// `Point.origin`, `Kex.FS.NotFound`): a type's static function, a module ADT's
// tag, or "Undefined function". Null when the name is a value after all (a
// nullary constructor or an upper-case binding), which the value-receiver
// path calls.
auto Lowering::lowerTypeNamespaceCall(const ast::MethodCall& n, const ast::UpperIdentifier& type)
    -> ExprPtr {
    std::vector<Binding> binds;
    // The arguments run before whatever this call turns out to be.
    for (const auto& a : n.args) (void)atomize(a, binds);
    // `Mod.Tag` for an ADT declared inside a module (e.g. `Kex.FS`).
    // The tag is just an atom at runtime, so the qualifier has no
    // representation here — emit the bare tag.
    if (n.args.empty() && !n.block && n.namedArgs.empty()) {
        if (auto tag = qualifiedVariantTag(type.name + "." + n.method);
            !tag.empty()) {
            auto ex = std::make_unique<Expr>();
            ex->node = Construct{tag, {}};
            return wrapLets(binds, std::move(ex));
        }
    }
    // A nullary CONSTRUCTOR is a value, not a namespace: `Monday.name`
    // is the same call as `let m = Monday` then `m.name`, which has
    // always worked. Fall through to the value-receiver path below
    // rather than reporting the namespace as undefined.
    const bool nullaryConstructorReceiver =
        nullaryVariantTags.count(type.name) ||
        // Prelude/imported constructors are not in the local tag set;
        // the external variant table (seeded for display) knows them.
        (variantOwner.count(type.name) &&
         variantArity.count(type.name) &&
         variantArity.at(type.name) == 0);
    // ...and so is an upper-case TOP-LEVEL BINDING. `let NUMS = [1,2,3]`
    // then `NUMS.map { }` is the same call as `let n = NUMS` then
    // `n.map { }` — which worked, while the direct form did not, purely
    // because an UpperIdentifier receiver was assumed to name a module.
    // Every module and variant lookup above has already missed by the
    // time we get here, so a name that is a known local value can only
    // be the value.
    // A MODULE's own constant is the same thing one scope in:
    // `"${EARLIEST_YEAR}"` inside `module Draft` is a `showValue`
    // call on the binding, and reading the receiver as a module name
    // reported `Undefined function: EARLIEST_YEAR.showValue`
    // (rodolfo docs/kex-issues.md #14, kexhq/kex#282).
    const bool topLevelBindingReceiver =
        zeroArgFns.count(type.name) ||
        topLevelConstants.count(type.name) ||
        !constantSymbolOf(*n.receiver).empty() ||
        // An IMPORTED zero-arg member is the same thing from another
        // module: `using Limits` + `"${LIMIT}"` is a showValue on the
        // constant's value, not a module member lookup
        // (kexhq/kex#282).
        [&]() {
            const auto constant = moduleFunctions.find(hir->referencedConstantOf(*n.receiver));
            return constant != moduleFunctions.end() &&
                emittedModuleZeroArg.count(constant->second);
        }() ||
        // ...or one from a module compiled apart.
        externalConstant(*n.receiver, type.name) != nullptr;
    if (!nullaryConstructorReceiver && !topLevelBindingReceiver)
        return wrapLets(binds,
            runtimeError("Undefined function: " + type.name + "." + n.method));
    return nullptr;
}

// A function the checker decided by its path: an Erlang one through `BEAM.`
// (`BEAM.lists.reverse(xs)` → `call 'lists':'reverse'(xs)`,
// `BEAM.Elixir.Phoenix.Router.match(c)` → `call 'Elixir.Phoenix.Router':'match'(c)`)
// or a runtime primitive (`Kex.Intrinsic.List.reverse(x)` →
// `call 'kex_intrinsic_list':'reverse'(x)`, the runtime module owning it and
// the prelude its typed semantics). A trailing block is the last argument.
auto Lowering::lowerRuntimeCall(const ast::MethodCall& n, const std::string& path) -> ExprPtr {
    const auto scope = kex::mangle::scopeOf(path);
    std::string module;
    if (names::isIntrinsicPath(path)) {
        // `Kex.Intrinsic.List` is `kex_intrinsic_list`.
        module = "kex_intrinsic";
        for (const char c : scope.substr(names::kIntrinsicRoot.size()))
            module += c == '.' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
        // After the root, the module's real name, whatever language
        // compiled it: `BEAM.beam_lib` is `beam_lib`.
        module = scope.substr(scope.find('.') + 1);
    }
    std::vector<Binding> binds;
    std::vector<ExprPtr> args;
    for (const auto& a : n.args) args.push_back(atomize(a, binds));
    if (n.block) args.push_back(atomize(*n.block, binds));
    const auto arity = static_cast<int>(args.size());
    return wrapLets(binds, callE(module, n.method, arity, std::move(args)));
}

// `.as(T)`, as the checker decided it (`Conversion`, `target` its type):
// distinct/backing conversions erase to the receiver; `.as(String)` is the
// universal total display path.
auto Lowering::lowerConversion(const ast::MethodCall& n, const std::string& target) -> ExprPtr {
    if (target == "String")
        return callE("kex_intrinsic_kex", "show", 1, one(lower(n.receiver)));
    // `"hello".as(Atom)`: the checker admits only a plain literal, so the
    // atom is a constant in the emitted code.
    if (target == "Atom")
        if (const auto* literal = std::get_if<ast::StringLiteral>(&n.receiver->kind);
            literal && literal->values.empty()) {
            // Plain text sits in `parts` (split, with nothing interpolated
            // between) or, for a literal marked non-interpolating, in `value`.
            std::string text = literal->value;
            if (!literal->parts.empty()) {
                text.clear();
                for (const auto& part : literal->parts) text += part;
            }
            return lit(LitKind::Atom, text);
        }
    return lower(n.receiver);
}

// `Process.spawn(state)`, the checker naming the state's type: the server
// remembers the module that owns its serving slots' dispatchers, which the
// prelude's wrapper cannot discover at run time.
auto Lowering::lowerProcessSpawn(const ast::MethodCall& n, const std::string& stateType) -> ExprPtr {
    std::vector<Binding> binds;
    auto state = atomize(n.args[0], binds);
    std::vector<ExprPtr> slotAtoms;
    // The module holding the slot dispatchers. A serving block at the top
    // level compiles into the entry module; one inside `module Store`
    // compiles into `Kex.Store`, and naming the entry module there sent
    // every call to a module that defines no slots (rodolfo
    // docs/kex-issues.md #2).
    std::string servingModule = outputModule;
    // The slot registry is keyed by the make target AS WRITTEN, which inside
    // a module is the bare name, while the checker types the spawned state
    // by its identity: looked up as is, `Store.Counter` found nothing and the
    // server started with an EMPTY slot list, answering
    // `{unknown_serving_slot, get}` to its first call.
    const auto key = ownerNameOf(stateType);
    if (auto slots = servingSlotsByType.find(key); slots != servingSlotsByType.end())
        for (const auto& slot : slots->second) slotAtoms.push_back(lit(LitKind::Atom, slot));
    if (auto declaring = localTypeModules.find(key); declaring != localTypeModules.end())
        servingModule = mangleModulePath(names::moduleAtom(declaring->second));
    if (slotAtoms.empty() && externalModules) {
        const auto external = externalModules->servingSlots.find(key);
        if (external != externalModules->servingSlots.end()) {
            servingModule = external->second.first;
            for (const auto& slot : external->second.second)
                slotAtoms.push_back(lit(LitKind::Atom, slot));
        }
    }
    auto allowedSlots = std::make_unique<Expr>();
    allowedSlots->node = MakeList{std::move(slotAtoms), std::nullopt};
    return wrapLets(binds, callE("kex_intrinsic_process", "spawnServing", 3,
                                 three(std::move(state), lit(LitKind::Atom, servingModule),
                                       std::move(allowedSlots))));
}

// A `serving` slot called on its running server (`Server<T>`): a cast, or a
// call waiting for the reply (`within:` its timeout).
auto Lowering::lowerServerSlotCall(const ast::MethodCall& n,
                                   const semantic::ResolvedCallTarget& target) -> ExprPtr {
    std::vector<Binding> binds;
    auto server = atomize(n.receiver, binds);
    std::vector<ExprPtr> protocolArgs;
    for (const auto& arg : n.args)
        protocolArgs.push_back(atomize(arg, binds));
    if (n.block) protocolArgs.push_back(atomize(*n.block, binds));
    auto argsList = std::make_unique<Expr>();
    argsList->node = MakeList{std::move(protocolArgs), std::nullopt};

    if (target.isServingCast) {
        std::vector<ExprPtr> castArgs;
        castArgs.push_back(std::move(server));
        castArgs.push_back(lit(LitKind::Atom, n.method));
        castArgs.push_back(std::move(argsList));
        return wrapLets(
            binds, callE("kex_intrinsic_process", "server_cast", 3,
                         std::move(castArgs)));
    }

    ExprPtr timeout = lit(LitKind::Atom, "default");
    for (const auto& [name, value] : n.namedArgs)
        if (name == "within") timeout = atomize(value, binds);
    std::vector<ExprPtr> callArgs;
    callArgs.push_back(std::move(server));
    callArgs.push_back(lit(LitKind::Atom, n.method));
    callArgs.push_back(std::move(argsList));
    callArgs.push_back(std::move(timeout));
    return wrapLets(
        binds, callE("kex_intrinsic_process", "server_call", 4,
                     std::move(callArgs)));
}

// A call the checker resolved to one target: its module and function, the
// receiver passed or not, arguments placed, trait dictionaries appended.
auto Lowering::lowerResolvedCall(const ast::MethodCall& n,
                                 const semantic::ResolvedCallTarget& target,
                                 const std::string& capabilityTarget) -> ExprPtr {
    if (!target.alternatives.empty())
        return providerChoice(n, target);
    std::vector<Binding> binds;
    std::vector<ExprPtr> args;
    if (target.passesReceiver)
        args.push_back(atomize(n.receiver, binds));
    // A make method of this unit: its declaration says where each
    // label goes and what an omitted parameter defaults to.
    const MakeMethod* declared =
        target.backendModule.empty() &&
        target.passesReceiver
            ? makeMethodFor(n) : nullptr;
    if (declared) {
        for (auto& slot : placeMethodArguments(n, *declared, binds))
            args.push_back(std::move(slot));
    } else if (!n.namedArgs.empty()) {
        const auto& pnames = target.paramNames;
        auto expected = static_cast<size_t>(
            target.backendArity -
            (target.passesReceiver ? 1 : 0) -
            target.traitDictionaries.size());
        // A foul target's arity can count the capability context
        // too, which is no parameter of its own.
        const bool contextCounted = threadsCapabilities() &&
            target.isFoul && pnames.size() + 1 == expected;
        if (pnames.size() != expected && !contextCounted)
            throw LowerError(
                "IR lower: named args to imported function with unknown params: " +
                target.backendFunction);
        std::vector<std::string> labels;
        for (const auto& [label, _] : n.namedArgs)
            labels.push_back(label);
        const auto placed = placeArguments(
            pnames, labels, n.args.size() + bool(n.block));
        std::vector<ExprPtr> slots(pnames.size());
        for (size_t i = 0; i < n.namedArgs.size(); ++i) {
            if (placed.labelSlots[i] == PlacedArguments::npos)
                throw LowerError("IR lower: unknown named arg " +
                                 n.namedArgs[i].first + " for " +
                                 target.backendFunction);
            slots[placed.labelSlots[i]] =
                atomize(n.namedArgs[i].second, binds);
        }
        std::vector<ExprPtr> positional;
        for (const auto& arg : n.args)
            positional.push_back(atomize(arg, binds));
        if (n.block) positional.push_back(atomize(*n.block, binds));
        for (size_t k = 0; k < positional.size(); ++k)
            if (placed.positionalSlots[k] != PlacedArguments::npos)
                slots[placed.positionalSlots[k]] =
                    std::move(positional[k]);
        if (!target.defaultSources.empty())
            fillOmittedArguments(target.defaultSources, slots);
        for (auto& slot : slots) args.push_back(std::move(slot));
    } else if (const auto expected = target.paramNames.size();
               !target.defaultSources.empty() &&
               n.args.size() + (n.block ? 1 : 0) < expected) {
        // Fewer arguments than parameters: the rest are the
        // callee's defaults, which only its interface knows.
        std::vector<ExprPtr> slots(expected);
        size_t next = 0;
        for (const auto& arg : n.args) slots[next++] = atomize(arg, binds);
        if (n.block) slots[next++] = atomize(*n.block, binds);
        fillOmittedArguments(target.defaultSources, slots);
        for (auto& slot : slots) args.push_back(std::move(slot));
    } else {
        for (const auto& arg : n.args)
            args.push_back(atomize(arg, binds));
        if (n.block) args.push_back(atomize(*n.block, binds));
    }
    // Only for an implementation whose parameters are plain names: a
    // clause that matches on an argument (`to(String)`, a type
    // selector) is reached through the dispatcher, which normalises
    // that argument and falls back to the prelude conversion — called
    // directly it met values it has no clause for (function_clause).
    //
    // A make method of this unit whose owner the checker decided:
    // call that owner's own implementation (`count/List`). The
    // shared dispatcher answers to the bare NAME, and lowerModules
    // files a name with whichever module declares it — `count` with
    // `module Mock` — so `parts.count` in the stdlib went to
    // Kex.Mock's gated dispatcher. The decided owner is addressed
    // directly, and the call skips the run-time dispatch too.
    if (target.backendModule.empty() &&
        target.passesReceiver &&
        target.traitDictionaries.empty() &&
        target.backendArity ==
            static_cast<int>(args.size()))
        // Built-in structural owners only (List, Map, the primitives):
        // their prelude implementation is the whole method. A record
        // type's dispatcher also forwards to `make` extensions from
        // units compiled apart (Units extends Measure), and a trait's
        // default answers only when the concrete type has none —
        // both decided at run time, so both keep the dispatcher.
        if (const auto owner = receiverOwner(n);
            !owner.empty() && ownsMethod(owner, n.method) &&
            (owner == kex::names::kListType || owner == kex::names::kMapType ||
             primGuardBifs().count(owner)) &&
            takesPlainParameters(n.method, owner))
            if (auto implementation = collidingImplementationName(
                    n.method, owner, args.size())) {
                if (threadsCapabilities() && target.isFoul)
                    args.push_back(currentContext());
                auto call = std::make_unique<Expr>();
                call->node = Call{"", *implementation,
                                  static_cast<int>(args.size()),
                                  std::move(args), false};
                return wrapLets(binds, std::move(call));
            }
    // Everything appended from here on is a trait dictionary,
    // not a source argument — the two branches of a capability
    // dispatch need them separated (see the terminal call below).
    const size_t sourceArgCount = args.size();
    for (const auto& [argument, trait] :
         target.traitDictionaries) {
        const ast::Expr* source = nullptr;
        if (target.passesReceiver) {
            if (argument == 0) source = n.receiver.get();
            else if (argument - 1 < n.args.size())
                source = n.args[argument - 1].get();
        } else if (argument < n.args.size()) {
            source = n.args[argument].get();
        }
        // Same as above: an omitted defaulted trait parameter
        // takes its dictionary from the default expression.
        if (!source) {
            const size_t index = target.passesReceiver
                ? (argument > 0 ? argument - 1 : 0) : argument;
            if (auto defaults = fnDefaults.find(n.method);
                defaults != fnDefaults.end() &&
                index < defaults->second.size() &&
                defaults->second[index])
                source = defaults->second[index]->get();
        }
        if (!source)
            throw LowerError(
                "IR lower: missing source argument for " + trait +
                " dictionary");
        args.push_back(atomize_ir(
            makeTraitDictionary(trait, *source), binds));
    }
    auto backendFunction = target.backendFunction;
    // A trait-default→dispatcher demotion used to rewrite
    // `backendFunction` here (`showValue/Showable` → `showValue`)
    // so unpinned-receiver calls reached the dispatcher's
    // variant arms. The Phase 2 trust audit removed it: with the
    // rewrite disabled, spec-beam, the 400-case fuzzer, and four
    // targeted probes (literal/bound/typed/unpinned Optional
    // interpolation, custom Showable instance) all agree, and
    // the cited `Just(50)` shape no longer reproduces —
    // resolution now reaches the dispatcher on its own.
    if (!target.localDispatchTypes.empty()) {
        auto exact = mangleReceiverSignature(
            backendFunction,
            target.localDispatchTypes);
        auto byReceiver = mangleReceiverImplementation(
            backendFunction,
            target.localDispatchTypes.front());
        if (knownFns.count(exact))
            backendFunction = std::move(exact);
        else if (knownFns.count(byReceiver))
            backendFunction = std::move(byReceiver);
        else if (const auto* method = selectedMakeMethod(n)) {
            // An argument-overloaded method the checker selected one
            // declaration of: that declaration's own function, by the
            // name its definition is emitted under (`listen/Srv.Server,
            // Integer,Options` from outside `module Srv`). Any other
            // method keeps the dispatcher, which reaches extensions.
            const auto arity = ast::methodArity(*method->def);
            if (argumentOverloadedMethods.count(
                    localOverloadKey(method->def->name, method->owner, arity)))
                if (auto symbol = makeMethodSymbol(*method->def, method->owner, arity);
                    knownFns.count(symbol))
                    backendFunction = std::move(symbol);
        }
    }
    // A proven `name/Head` for an imported make-block method —
    // unless the receiver takes a trait dictionary (clause
    // (10)), which names trait dispatch the mangled spelling
    // cannot serve. (Set only for imported receiver calls, so
    // index 0 is the receiver and `localDispatchTypes` — the
    // same-unit path above — is empty.)
    if (!target.staticReceiverHead.empty()) {
        bool receiverHasDictionary = false;
        for (const auto& [argument, _] :
             target.traitDictionaries)
            if (argument == 0) receiverHasDictionary = true;
        if (!receiverHasDictionary) {
            backendFunction =
                mangleReceiverImplementation(n.method, target.staticReceiverHead);
            // Applied-spelling audit for `make test-devirt`:
            // `module name/Head beamArity`, one line per call
            // site, appended (O_APPEND keeps parallel spec
            // runs from interleaving).
            if (const char* audit =
                    std::getenv("KEX_DEVIRT_AUDIT")) {
                if (FILE* out = std::fopen(audit, "a")) {
                    std::fprintf(
                        out, "%s %s %d\n",
                        target.backendModule.c_str(),
                        backendFunction.c_str(),
                        target.backendArity);
                    std::fclose(out);
                }
            }
        }
    }
    // Same as externalCallExpr: the imported-foul append belongs
    // with dropping the gate, not before it (kexhq/kex#181).
    if (!capabilityTarget.empty()) {
        // The two branches take DIFFERENT arguments. The real
        // implementation is an ordinary function and wants its
        // trait dictionaries; a stand-in is a `make` block method,
        // which is unannotated and takes none. Splitting them here
        // is what lets a capability have a trait-constrained
        // parameter at all — `IO.printLine(msg: Showable)` is the
        // first, and without this its dictionary went missing and
        // every value rendered structurally instead of through the
        // user's `showValue` (kexhq/kex#143).
        std::vector<ExprPtr> dictionaries;
        for (size_t i = sourceArgCount; i < args.size(); ++i)
            dictionaries.push_back(std::move(args[i]));
        args.resize(sourceArgCount);
        auto module = target.backendModule;
        auto function = backendFunction;
        const int arity = target.backendArity;
        return capabilityDispatchLowered(
            capabilityTarget, n.method, std::move(args), binds,
            [&](std::vector<ExprPtr> direct) {
                for (auto& dictionary : dictionaries)
                    direct.push_back(std::move(dictionary));
                return callE(module, function, arity,
                             std::move(direct));
            });
    }
    // A resolved target with no backend module is a call into THIS
    // unit, and callE only appends the capability context for
    // EXTERNAL foul targets — so a foul make-block method
    // resolved this way went out one argument short of its own
    // definition. `Net.HTTP`'s `make Client do foul statistics`
    // is defined as statistics/2 (receiver + context) and was
    // called as `apply 'statistics'/1`, which erlc rejects.
    // Routing it through localCall puts it back on the one code
    // path that decides about the context (kexhq/kex#181).
    if (target.backendModule.empty() &&
        target.backendArity ==
            static_cast<int>(args.size())) {
        // The resolver knows WHICH implementation won, so its
        // foulness settles the context where the name-and-arity
        // lookup in callNeedsContext cannot: `get` is PURE at
        // arity 2 for Net.HTTP's Headers and FOUL at arity 2 for
        // its Client, and one answer has to serve both.
        if (threadsCapabilities() && target.isFoul) {
            args.push_back(currentContext());
            auto call = std::make_unique<Expr>();
            call->node = Call{"", backendFunction,
                              static_cast<int>(args.size()),
                              std::move(args), false};
            return wrapLets(binds, std::move(call));
        }
        if (callNeedsContext(backendFunction,
                             static_cast<int>(args.size())))
            return wrapLets(
                binds,
                localCallExpr(backendFunction, std::move(args)));
    }
    // Source-derived interfaces know a foul target's hidden
    // capability arity even when the prebuilt external-module
    // registry does not contain that opt-in module. Honor the
    // resolved target directly; otherwise callE cannot discover
    // the context parameter and emits an arity-short call.
    if (threadsCapabilities() && target.isFoul &&
        target.backendArity ==
            static_cast<int>(args.size()) + 1) {
        args.push_back(currentContext());
        auto call = std::make_unique<Expr>();
        call->node = Call{target.backendModule,
                          backendFunction,
                          target.backendArity,
                          std::move(args), false};
        return wrapLets(binds, std::move(call));
    }
    return wrapLets(
        binds,
        callE(target.backendModule,
              backendFunction,
              target.backendArity,
              std::move(args)));
}

// record TypeName { f: v, ... } → {'TypeName', <fields in declared order,
// defaults filled for omitted ones>}. Field ORDER and defaults come from
// the record definition (the accessors read fixed positions).
auto Lowering::lowerRecordConstruction(const ast::Expr& e, const ast::RecordConstruction& n)
    -> ExprPtr {
    std::vector<Binding> binds;
    std::vector<ExprPtr> args;
    const bool fromReceiver = n.typeName == "New";
    // The record the checker typed the construction as.
    const auto recordName = recordTypeOf(e);
    if (recordName.empty())
        throw LowerError("IR lower: record construction `" + n.typeName +
                         "` the checker did not type as a known record");
    struct LoweredEntry {
        const ast::RecordEntry* source;
        AtomicRef value;
    };
    std::vector<LoweredEntry> entries;
    entries.reserve(n.fields.size());
    // A `Mock.*` stand-in is test-only like the mock functions are
    // (issue #144): building one is where a program starts to lie about
    // the world, so that is where the grant is checked. Same wording as
    // the interpreter, via the shared kex_test gate.
    if (n.typeName != "This" && !fromReceiver &&
        recordName.rfind("Mock.", 0) == 0) {
        std::vector<ExprPtr> gateArgs;
        gateArgs.push_back(lit(LitKind::String, recordName));
        binds.push_back({fresh("gate"),
                         callE("kex_test", "require_mocks_allowed", 1,
                               std::move(gateArgs))});
    }
    for (const auto& field : n.fields) {
        auto value = atomize(field.value, binds);
        entries.push_back({&field, snap(value)});
    }
    auto it = records.find(recordName);
    if (it == records.end()) {
        // Unknown record — fall back to fields as written.
        for (auto& entry : entries)
            if (!entry.source->spread) args.push_back(entry.value.get());
    } else {
        const auto& info = it->second;
        for (size_t i = 0; i < info.fields.size(); i++) {
            const LoweredEntry* provided = nullptr;
            for (auto rit = entries.rbegin(); rit != entries.rend(); ++rit)
                if (rit->source->spread ||
                    rit->source->name == info.fields[i]) {
                    provided = &*rit;
                    break;
                }
            if (provided && !provided->source->spread) {
                args.push_back(provided->value.get());
            } else if (provided || fromReceiver) {
                auto base = provided ? provided->value.get() : var("this");
                auto read = callE(
                    "erlang", "element", 2,
                    two(litInt(static_cast<int>(i) + 2), std::move(base)));
                auto name = fresh("field");
                binds.push_back({name, std::move(read)});
                args.push_back(var(name));
            } else if (i < info.defaultedApart.size() && info.defaultedApart[i]) {
                auto name = fresh("field");
                binds.push_back({name, callE(info.defaultsModule,
                                             names::kRecordDefaultFunction, 2,
                                             two(lit(LitKind::Atom, recordName),
                                                 lit(LitKind::Atom, info.fields[i])))});
                args.push_back(var(name));
            } else if (info.defaults[i]) {
                auto lowered = lowerFieldDefault(info, *info.defaults[i]);
                if (isAtomic(*lowered)) {
                    args.push_back(std::move(lowered));
                } else {
                    auto name = fresh();
                    binds.push_back({name, std::move(lowered)});
                    args.push_back(var(name));
                }
            } else {
                args.push_back(lit(LitKind::None, "none"));
            }
        }
    }
    auto ex = std::make_unique<Expr>();
    ex->node = Construct{recordName, std::move(args)};
    return wrapLets(binds, std::move(ex));
}

auto Lowering::lowerParsedInterpolatedString(
    const std::vector<std::string>& stringParts,
    const std::vector<ast::ExprPtr>& values) -> ExprPtr {
    std::vector<ExprPtr> parts;
    for (size_t i = 0; i < stringParts.size(); i++) {
        if (!stringParts[i].empty())
            parts.push_back(lit(LitKind::String, stringParts[i]));
        if (i < values.size() && values[i])
            parts.push_back(
                callE("kex_io", "to_string", 1, one(lower(values[i]))));
    }
    if (parts.empty()) return lit(LitKind::String, "");
    if (parts.size() == 1 &&
        std::holds_alternative<Lit>(parts[0]->node))
        return std::move(parts[0]);
    auto list = std::make_unique<Expr>();
    list->node = MakeList{std::move(parts), std::nullopt};
    return callE(
        "unicode", "characters_to_binary", 1, one(std::move(list)));
}

} // namespace kex::ir

namespace kex::ir {

auto Lowering::recordTypeOf(const ast::Expr& e) const -> std::string {
    auto record = hir->recordOf(e);
    return records.count(record) ? record : std::string{};
}

} // namespace kex::ir
