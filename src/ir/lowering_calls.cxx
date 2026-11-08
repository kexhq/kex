// Call lowering methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"

namespace kex::ir {

auto Lowering::lowerFunctionCall(const ast::FunctionCall& n) -> ExprPtr {
    // A `using` import from a separately compiled module. A local binding
    // or an import from this unit still wins; anything else follows the
    // rule that a `using` import outranks a same-named function.
    if (!subst.count(n.name) && !moduleImports.count(n.name))
        if (auto ext = externalImports.find(n.name);
            ext != externalImports.end())
            return lowerExternalImportCall(n, ext->second);
    // Named args → reorder into the callee's positional slots by param
    // name; then positional args (and a trailing block) fill remaining
    // slots in order, leftovers default to None. Mirrors the string
    // emitter / Evaluator::callFunction (spec/optional_parens_do.kex).
    if (!n.namedArgs.empty()) {
        // A named-arg call through a local bound by a bare `~name`
        // capture (`let f = ~greet; f(loud: true, name: "Ada")`): `f` is
        // a lexical local, not a declared function, so `fnParamNames`
        // has no entry for "f" — resolve through the capture's real
        // origin instead of falling through to the "unknown function"
        // error, and apply the local (a runtime fun value) via
        // CallIndirect rather than a static call, matching how a
        // positional call through the same local is already lowered
        // (kexhq/kex#309).
        if (subst.count(n.name)) {
            if (auto origin = curryOrigin.find(n.name);
                origin != curryOrigin.end()) {
                if (auto oit = fnParamNames.find(origin->second);
                    oit != fnParamNames.end()) {
                    if (auto declared = fnAllParamNames.find(origin->second);
                        declared != fnAllParamNames.end())
                        for (const auto& [label, _] : n.namedArgs)
                            if (!declared->second.count(label))
                                return unknownNamedArgument(origin->second, label);
                    const auto& pnames = oit->second;
                    std::vector<Binding> binds;
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
                    for (auto& s : slots) if (!s) s = lit(LitKind::None, "none");
                    auto ex = std::make_unique<Expr>();
                    ex->node = CallIndirect{var(currentName(n.name)),
                                            std::move(slots), false};
                    return wrapLets(binds, std::move(ex));
                }
            }
        }
        auto it = fnParamNames.find(n.name);
        std::string emittedName = n.name;
        // The import this scope names, ahead of the scan below, which
        // takes whichever module happens to declare the name first.
        if (it == fnParamNames.end())
            if (const auto key = importedKey(n.name); !key.empty())
                if (auto pit = fnParamNames.find(key);
                    pit != fnParamNames.end()) {
                    it = pit;
                    emittedName = moduleFunctions.at(key);
                }
        if (it == fnParamNames.end()) {
            for (const auto& [key, val] : moduleFunctions) {
                auto dot = key.rfind('.');
                if (dot != std::string::npos && key.substr(dot + 1) == n.name) {
                    auto pit = fnParamNames.find(key);
                    if (pit != fnParamNames.end()) {
                        it = pit;
                        emittedName = val;
                        break;
                    }
                }
            }
        }
        if (it == fnParamNames.end() && externalModules) {
            for (const auto& [qualKey, pnames] : externalModules->exportParamNames) {
                auto dot = qualKey.rfind('.');
                if (dot != std::string::npos && qualKey.substr(dot + 1) == n.name) {
                    auto eit = externalModules->exportToBeamFn.find(qualKey);
                    if (eit != externalModules->exportToBeamFn.end()) {
                        auto modName = qualKey.substr(0, dot);
                        auto ait = externalModules->nameToAtom.find(modName);
                        if (ait != externalModules->nameToAtom.end()) {
                            std::vector<Binding> binds;
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
                            for (auto& s : slots) if (!s) s = lit(LitKind::None, "none");
                            int ar = static_cast<int>(slots.size());
                            return wrapLets(binds,
                                externalCallExpr(qualKey, ait->second, eit->second, ar, std::move(slots)));
                        }
                    }
                }
            }
        }
        if (it == fnParamNames.end())
            throw LowerError("IR lower: named args to unknown function " + n.name);
        if (auto declared = fnAllParamNames.find(n.name);
            declared != fnAllParamNames.end())
            for (const auto& [label, _] : n.namedArgs)
                if (!declared->second.count(label))
                    return unknownNamedArgument(n.name, label);
        // The clause the checker chose for these labels.
        const auto* signature = hir->selectedSignatureOf(n);
        const auto* selected = signature ? signature->clause : nullptr;
        const auto pnames = clauseNames(selected, it->second);
        std::vector<Binding> binds;
        std::vector<std::string> labels;
        for (const auto& [label, _] : n.namedArgs) labels.push_back(label);
        const auto placed = placeArguments(pnames, labels,
                                           n.args.size() + bool(n.block));
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
                slots[placed.positionalSlots[k]] = std::move(positional[k]);
        fillDefaultSlots(it->first, slots, binds, selected);
        auto ex = std::make_unique<Expr>();
        ex->node = localCall(emittedName, std::move(slots));
        return wrapLets(binds, std::move(ex));
    }
    std::vector<Binding> binds;
    // describe/it: the testing DSL → kex_test, block as a 0-arg fun.
    // The call's own source location travels with it as a third argument:
    // an editor's test tree needs to know which line a case is written on,
    // and by the time this is a fun in a .beam nothing else remembers
    // (kexhq/kex#199). Captured before atomize recurses, since lowering the
    // arguments moves `currentLoc` into them.
    if ((n.name == "describe" || n.name == "it") && n.block && n.args.size() == 1) {
        auto where = sourceLocationTuple();
        auto name = atomize(n.args[0], binds);
        auto fn = atomize(*n.block, binds);
        std::vector<ExprPtr> args;
        args.push_back(std::move(name));
        args.push_back(std::move(fn));
        args.push_back(std::move(where));
        return wrapLets(binds, callE("kex_test", n.name, 3, std::move(args)));
    }
    // before/after hooks register a 0-arg block in the current describe;
    // an optional :each/:all atom selects its scope.
    if ((n.name == "before" || n.name == "after") && n.block && n.args.size() <= 1) {
        std::vector<ExprPtr> args;
        for (const auto& arg : n.args) args.push_back(atomize(arg, binds));
        auto fn = atomize(*n.block, binds);
        args.push_back(std::move(fn));
        auto arity = static_cast<int>(args.size());
        return wrapLets(binds, callE("kex_test", n.name, arity, std::move(args)));
    }
    // assert(cond[, msg]) — a plain global builtin, not a local function.
    // Lowered to the located form so a failure can name the line it
    // happened on (kexhq/kex#199); kex_test:assert/1,2 remain for callers
    // that have no location to give, the BEAM REPL among them.
    if (n.name == "assert" && !n.args.empty() && !n.block) {
        auto where = sourceLocationTuple();
        std::vector<ExprPtr> as;
        for (const auto& a : n.args) as.push_back(atomize(a, binds));
        as.push_back(std::move(where));
        int ar = static_cast<int>(as.size());
        return wrapLets(binds, callE("kex_test", "assert_at", ar, std::move(as)));
    }
    std::vector<ExprPtr> args;
    for (const auto& a : n.args) args.push_back(atomize(a, binds));
    // A trailing do-block is passed as the function's last argument.
    if (n.block) args.push_back(atomize(*n.block, binds));
    else if (auto defaults = fnDefaults.find(n.name);
             defaults != fnDefaults.end())
        for (size_t i = n.args.size(); i < defaults->second.size(); ++i)
            if (defaults->second[i])
                args.push_back(atomize(*defaults->second[i], binds));
    // A `using` import files its defaults under the qualified name, and
    // they are written against the owning module's scope.
    if (const auto key = importedKey(n.name); !n.block && !key.empty())
        if (auto defaults = fnDefaults.find(key);
            defaults != fnDefaults.end() &&
            args.size() < defaults->second.size()) {
            args.resize(defaults->second.size());
            fillDefaultSlots(key, args, binds);
        }
    if (auto requirements = fnTraitParams.find(n.name);
        requirements != fnTraitParams.end())
        for (const auto& [index, trait] : requirements->second) {
            const ast::Expr* source =
                index < n.args.size() ? n.args[index].get() : nullptr;
            // A trait-typed parameter with a default may be omitted at the
            // call site, and then the dictionary comes from the default
            // value — the same expression the loop above passes as the
            // argument itself (kexhq/kex#175).
            if (!source)
                if (auto defaults = fnDefaults.find(n.name);
                    defaults != fnDefaults.end() &&
                    index < defaults->second.size() &&
                    defaults->second[index])
                    source = defaults->second[index]->get();
            if (!source)
                throw LowerError(
                    "IR lower: missing source argument for " + trait +
                    " dictionary");
            args.push_back(
                atomize_ir(makeTraitDictionary(trait, *source), binds));
        }
    if (n.name == "self" && args.empty() && !knownFns.count("self"))
        return callE("erlang", "self", 0, {});
    if (n.name == "send" && args.size() == 2 && !knownFns.count("send")) {
        return wrapLets(binds, callE("erlang", "send", 2,
                                     two(std::move(args[0]),
                                         std::move(args[1]))));
    }
    if (n.name == "sendFrom" && args.size() == 2 &&
        !knownFns.count("sendFrom")) {
        auto message = std::make_unique<Expr>();
        message->node = MakeTuple{two(callE("erlang", "self", 0, {}),
                                      std::move(args[1]))};
        return wrapLets(binds, callE("erlang", "send", 2,
                                     two(std::move(args[0]),
                                         std::move(message))));
    }
    auto ex = std::make_unique<Expr>();
    int arity = static_cast<int>(args.size());
    // A 0-arity function/constant holding a fun (e.g. `let inc = ~add(1)`)
    // called with args: evaluate the thunk, then apply the resulting fun.
    // `zeroArgFns`/`topLevelConstants` only ever collect from `prog.items`
    // directly — a top-level `let render = Template.html(...)` — so the
    // identical declaration one level down, inside `module M do ... end`,
    // was invisible here and fell through to an ordinary direct call at
    // its own (wrong) arity: `render(x)` inside the SAME module compiled
    // to `apply 'M.render'/1(X)`, and erlc rejected the module outright
    // since only `render/0` (the thunk itself) was ever emitted — a
    // dangling reference the interpreter never hit, since it resolves
    // calls dynamically rather than by declared arity (kexhq/rodolfo's
    // docs/kex-issues.md, follow-up on kexhq/kex#385). `moduleZeroArgFns`
    // is where a module-scoped 0-param declaration DOES get recorded;
    // walk the same innermost-first enclosing-module chain the bare
    // lookup a few lines up uses to find it under this call's own module.
    // A module member's EMITTED name can differ from its bare source
    // name (mangling), unlike a top-level one, which is always bare —
    // `moduleFunctions` has the emitted spelling actually safe to call.
    std::string zeroArgCallName = prefixSymbol(n.name);
    // The enclosing modules' own bindings first, innermost out: a
    // top-level `layout` answered `Views.layout(live)` inside `module
    // Views` while the walker called the module's own.
    bool zeroArgThunk = !subst.count(n.name) &&
        ([&] {
             for (auto scope = currentModulePath; !scope.empty();) {
                 if (moduleZeroArgFns.count(scope + "." + n.name)) {
                     if (auto emitted = moduleFunctions.find(scope + "." + n.name);
                         emitted != moduleFunctions.end())
                         zeroArgCallName = emitted->second;
                     return true;
                 }
                 const auto dot = scope.rfind('.');
                 if (dot == std::string::npos) break;
                 scope.resize(dot);
             }
             return false;
         }() ||
         zeroArgFns.count(n.name) || topLevelConstants.count(n.name));
    // Capitalized name = ADT constructor with a payload → tagged tuple.
    if (!n.name.empty() && std::isupper(static_cast<unsigned char>(n.name[0]))
        && !zeroArgThunk) {
        if (auto tag = qualifiedVariantTag(n.name); !tag.empty()) {
            ex->node = Construct{tag, std::move(args)};
            return wrapLets(binds, std::move(ex));
        }
        ex->node = Construct{n.name, std::move(args)};
    }
    else if (zeroArgThunk && (!args.empty() || topLevelConstants.count(n.name))) {
        // A `let` constant holding a fun: `name(...)` resolves the
        // constant, then applies — including `thunk()` with NO args (the
        // walker looks the identifier up and applies the lambda once).
        // A real 0-arity FUNCTION `f()` stays a plain call below: its
        // result is returned as-is, never auto-applied.
        auto thunk = std::make_unique<Expr>();
        thunk->node = localCall(zeroArgCallName, {});
        ex->node = CallIndirect{std::move(thunk), std::move(args), false};
    } else if (zeroArgThunk)
        ex->node = localCall(zeroArgCallName, {});
    // A lexical binding (a `block` parameter, or any local holding a
    // callable) outranks a `using` import of the same name too, not just
    // the flat knownFns set below: innermost wins, as it does for every
    // other local. Ahead of `moduleImports` because the reverse order
    // silently discarded the binding and called the import instead —
    // `let render = Template.html(...)` calling `render(x)` compiled to
    // a call to the `using Template`-imported `Template.render/2`
    // instead of applying the local, and erlc rejected the resulting
    // arity mismatch (kexhq/kex#362). Same fix shape as the knownFns
    // case below (kexhq/kex#289); kept as its own check because
    // `moduleImports` must still outrank knownFns once no local shadows
    // it, and `CallIndirect` (not `localCall`) is how a runtime value is
    // applied.
    else if (subst.count(n.name))
        ex->node = CallIndirect{var(currentName(n.name)), std::move(args), false};
    // A `using` import of this lexical scope outranks the flat knownFns
    // set. That set spans the whole merged program and carries every
    // module's `make` methods under their bare names, so `post` in main
    // after `using Rodolfo` lowered to a nonexistent local `post/2` (or,
    // when a colliding function happens to exist at the same arity, to
    // that function) although the tree-walker honours the import. The
    // receiver path already lets the source import win over identically
    // named prelude methods; see lowerMethodCall.
    else if (auto imp = moduleImports.find(n.name); imp != moduleImports.end())
        ex->node = localCall(imp->second, std::move(args));
    // A bare call inside a `make` block naming one of its own methods.
    // The receiver is implicit at the call site exactly as it is in the
    // definition, so `decorate(@name)` means `this.decorate(name)` — which
    // is what lets a method call a sibling, or itself recursively, without
    // spelling `this.` (rodolfo docs/kex-issues.md #13, kexhq/kex#281).
    //
    // Ahead of `knownFns` because that set is name-only and already holds
    // every make method: `decorate` looked defined, so the call was
    // emitted as `decorate/1`, which nothing defines and erlc rejected.
    // Only when NO definition answers at the written arity, so an ordinary
    // function of the same name still wins.
    else if (!currentMakeType.empty() && subst.count("this") &&
             !definedFnArities.count(n.name + "/" + std::to_string(arity)) &&
             definedFnArities.count(n.name + "/" +
                                    std::to_string(arity + 1)) &&
             ownsMethod(currentMakeType, n.name)) {
        args.insert(args.begin(), var(currentName("this")));
        ex->node = localCall(n.name, std::move(args));
    }
    // knownFns is checked last among the name-based fallbacks: the local
    // binding and `using`-import checks above already outrank it
    // (kexhq/kex#289, kexhq/kex#362).
    else if (knownFns.count(n.name))
        ex->node = prefixCall(n.name, std::move(args));
    else if (auto external = externalPrefixCall(n.name, arity); !external.empty())
        // UFCS runs both ways: `xs.length` and `length(xs)` are the same
        // call, and the walker resolves the prefix spelling against the
        // prelude just as it does the receiver spelling. Nothing here had
        // done that, so `map(admin, &.upperCase)` — examples/ufcs.kex's
        // own demonstration of the equivalence — died with "Undefined
        // function: map" on BEAM.
        ex->node = Call{external, n.name, arity, std::move(args), false};
    else
        return wrapLets(binds, runtimeError("Undefined function: " + n.name));
    return wrapLets(binds, std::move(ex));
}

// The module of this unit whose `make` covers `type`. The checker names a
// module's type qualified (`Units.SI.SIPrefix`) while `make SIPrefix`
// registers it bare, so a qualified name is also tried by its last part
// when that part was registered by the qualifying module. Without it
// `5000.meter.convertTo(Kilo(Meter))` built its Unit dictionary from the
// prelude's `factor`, which has never heard of SIPrefix.
auto Lowering::localTypeModule(const std::string& type) const
    -> std::unordered_map<std::string, std::string>::const_iterator {
    auto found = localTypeModules.find(type);
    if (found != localTypeModules.end()) return found;
    const auto dot = type.rfind('.');
    if (dot == std::string::npos) return found;
    auto bare = localTypeModules.find(type.substr(dot + 1));
    return bare != localTypeModules.end() &&
            bare->second == type.substr(0, dot)
        ? bare : localTypeModules.end();
}

auto Lowering::makeTraitDictionary(const std::string& trait,
                         const ast::Expr& value) -> ExprPtr {
    std::string concreteType;
    if (const auto type = staticTypeOf(&value);
        type && !std::holds_alternative<semantic::TypeVar>(type->kind) &&
        !std::holds_alternative<semantic::UnknownType>(type->kind) &&
        !std::holds_alternative<semantic::ConstrainedType>(type->kind))
        concreteType = semantic::typeToString(type);
    // A value known only by a trait (`x: Addable`, or what a trait's
    // default answers) has no concrete type to pick an implementation by.
    if (traitMethods.count(concreteType)) concreteType.clear();
    if (auto owner = variantOwner.find(concreteType);
        owner != variantOwner.end())
        concreteType = owner->second;
    return makeTraitDictionaryFor(trait, concreteType);
}

// The dictionary itself, for an already-resolved receiver type. An empty
// `concreteType` means "not known here" and yields the provider's generic
// dispatcher for every method, which routes on the runtime tag.
auto Lowering::makeTraitDictionaryFor(const std::string& trait,
                            const std::string& concreteType) -> ExprPtr {
    auto contract = traitMethods.find(trait);
    if (contract == traitMethods.end())
        throw LowerError("IR lower: unknown trait dictionary " + trait);
    std::vector<ExprPtr> slots;
    std::vector<Binding> dictionaryBindings;
    for (const auto& method : contract->second) {
        std::string module;
        std::string function = method.name;
        const auto owners = methodOwners.find(method.name);
        const bool localOwnsType = owners != methodOwners.end() &&
            std::find(owners->second.begin(), owners->second.end(),
                      concreteType) != owners->second.end();
        const bool localDispatcher =
            owners != methodOwners.end() && !owners->second.empty() &&
            localMethodArities.count(method.name + "/" +
                                     std::to_string(method.arity)) > 0;
        if (concreteType.empty() && localDispatcher) {
            // Type not known here, and a make block in this module
            // implements the method: its dispatcher routes on the runtime
            // tag and falls back to the provider's, so it answers for every
            // value. The provider's alone never reaches this module's
            // types: `IO.printLine(total(a, b))` printed a bare record on
            // BEAM, where the walker used the record's `showValue`.
        } else if (localOwnsType && localMethodArities.count(
                method.name + "/" + std::to_string(method.arity))) {
            if (collidingMethods.count(method.name) &&
                !concreteType.empty())
                function = mangleReceiverImplementation(
                    method.name, concreteType);
        } else if (auto ownerModule = localTypeModule(concreteType);
                   ownerModule != localTypeModules.end()) {
            // A source module compiled in this same unit is not in the
            // external KexI registry yet, so target its companion's
            // runtime dispatcher directly.
            module = names::moduleAtom(ownerModule->second);
            function = method.name;
        } else if (externalModules) {
            auto candidates =
                externalModules->receiverFunctions.find(method.name);
            if (candidates !=
                externalModules->receiverFunctions.end()) {
                auto selected = std::find_if(
                    candidates->second.begin(), candidates->second.end(),
                    [&](const auto& candidate) {
                        return candidate.beamArity == method.arity &&
                            !concreteType.empty() &&
                            candidate.receiverType == concreteType;
                    });
                // No implementation for THIS receiver type: take the
                // provider's DISPATCHER (`showValue/1`), not whichever
                // implementation happens to come first. The dispatcher
                // routes on the runtime tag and ends at the trait default,
                // so it answers for every receiver — including one whose
                // type is not known here, which is exactly when this path
                // runs. Picking an implementation instead made
                // `IO.printLine(Just(50))` print `Just(50)` on BEAM and
                // `50` on the walker, because the dictionary was pinned to
                // `showValue/Showable` and never reached the Optional
                // implementation the prelude had just gained.
                if (selected == candidates->second.end()) {
                    auto dispatcher = std::find_if(
                        candidates->second.begin(), candidates->second.end(),
                        [&](const auto& candidate) {
                            return candidate.beamArity == method.arity &&
                                candidate.beamFunction == method.name;
                        });
                    if (dispatcher != candidates->second.end())
                        selected = dispatcher;
                    else
                        selected = std::find_if(
                            candidates->second.begin(),
                            candidates->second.end(),
                            [&](const auto& candidate) {
                                return candidate.beamArity == method.arity;
                            });
                }
                if (selected != candidates->second.end()) {
                    module = selected->moduleAtom;
                    function = selected->beamFunction;
                }
            }
        }
        // A trait declared and implemented in THIS file has no external
        // provider to name, so the generic dictionary (empty
        // `concreteType`) has to point at the local function — which is
        // this module's own dispatcher for the method, exactly the generic
        // answer wanted. Without it no generic dictionary could be built
        // for a local trait at all, so `makeDictionaryWrapper` skipped the
        // source-arity wrapper and `~printShape` referenced a
        // `printShape/1` that did not exist — erlc rejected the whole
        // module (examples/trait-constraint.kex).
        const bool localAnswers = localOwnsType ||
            localMethodArities.count(method.name + "/" +
                                     std::to_string(method.arity)) > 0;
        if (module.empty() && !localAnswers)
            throw LowerError("IR lower: no implementation for " + trait +
                             "." + method.name + " on " + concreteType);
        Lambda lambda;
        std::vector<ExprPtr> callArgs;
        for (int i = 0; i < method.arity; ++i) {
            auto param = fresh("DictArg");
            lambda.params.push_back(param);
            callArgs.push_back(var(param));
        }
        lambda.body = callE(module, function, method.arity,
                            std::move(callArgs));
        auto slot = std::make_unique<Expr>();
        slot->node = std::move(lambda);
        slots.push_back(atomize_ir(std::move(slot), dictionaryBindings));
    }
    auto dictionary = std::make_unique<Expr>();
    dictionary->node = MakeTuple{std::move(slots)};
    return wrapLets(dictionaryBindings, std::move(dictionary));
}

// A trait-typed parameter is compiled as TWO backend arguments: the value
// and a dictionary the CALL SITE builds from the argument's static type.
// `IO.printLine(msg: Showable)` is therefore `printLine`/2 on BEAM and
// there is no `printLine`/1 at all — so every call site that cannot name
// the argument's type emits an `undef`. Two shapes reach that state and
// neither is a mistake at the source level:
//
//   - a `~Module.fn` capture, which has no argument to inspect yet, and
//   - a call whose argument type is genuinely unknown here, e.g. the
//     return of a cross-file function the checker could not infer
//     (spec/crossfile_using.kex's recursive `Utils.Strings.repeat`).
//
// So the definition also publishes the SOURCE arity, filling each
// dictionary with the provider's generic dispatcher — exactly what the
// call site emits when it knows nothing about the type. Dispatch stays
// identical; only the place the dictionary is built moves.
auto Lowering::makeDictionaryWrapper(const FunDef& def) -> std::optional<FunDef> {
    if (def.dictionaryTraits.empty()) return std::nullopt;
    const int contextSlots = def.hasCapabilityContext ? 1 : 0;
    const int wrapped =
        def.arity - static_cast<int>(def.dictionaryTraits.size()) -
        contextSlots;
    if (wrapped < 0) return std::nullopt;
    std::vector<ExprPtr> args;
    std::vector<std::string> params;
    for (int i = 0; i < wrapped; ++i) {
        params.push_back(fresh("Fwd"));
        args.push_back(var(params.back()));
    }
    std::vector<Binding> binds;
    for (const auto& trait : def.dictionaryTraits) {
        // A trait whose dictionary cannot be built without a concrete type
        // has no generic answer to give; leave that arity undefined rather
        // than emit a call to a function that does not exist.
        ExprPtr dictionary;
        try {
            dictionary = makeTraitDictionaryFor(trait, "");
        } catch (const LowerError&) {
            return std::nullopt;
        }
        auto name = fresh("FwdDict");
        binds.push_back({name, std::move(dictionary)});
        args.push_back(var(name));
    }
    // The context stays LAST, after the dictionaries the wrapper builds,
    // so the wrapped function receives (args..., dictionaries..., context)
    // — the order its own parameters were emitted in. The wrapper takes
    // the context as its own trailing parameter and passes it through.
    if (def.hasCapabilityContext) {
        params.push_back(fresh("FwdCtx"));
        args.push_back(var(params.back()));
    }
    FunDef wrapper;
    wrapper.name = def.name;
    wrapper.arity = wrapped + contextSlots;
    wrapper.exported = def.exported;
    wrapper.hasCapabilityContext = def.hasCapabilityContext;
    FunClause clause;
    for (const auto& name : params) {
        auto pat = std::make_unique<Pattern>();
        pat->kind = PatKind::Var;
        pat->name = name;
        clause.params.push_back(std::move(pat));
    }
    clause.body = wrapLets(
        binds, callE("", def.name, def.arity, std::move(args)));
    wrapper.clauses.push_back(std::move(clause));
    return wrapper;
}

// A module path `A.B.C` (nested modules, encoded by the parser as a chain
// of MethodCall nodes with no args) flattened to the qualified name
// ["A","B","C"], or empty if the receiver isn't a pure uppercase path.
auto Lowering::modulePath(const ast::Expr& e, std::vector<std::string>& out) -> bool {
    if (auto* uid = std::get_if<ast::UpperIdentifier>(&e.kind)) {
        out.push_back(uid->name); return true;
    }
    if (auto* mc = std::get_if<ast::MethodCall>(&e.kind)) {
        if (!mc->args.empty() || !mc->namedArgs.empty() || mc->block) return false;
        if (!mc->method.empty() && std::isupper(static_cast<unsigned char>(mc->method[0]))) {
            if (!modulePath(*mc->receiver, out)) return false;
            out.push_back(mc->method); return true;
        }
    }
    return false;
}

// The receiver of a `BEAM.<module>.fn(...)` interop call: root `BEAM`
// followed by one or more no-argument segments of ANY case, joined
// verbatim into the BEAM module atom. `BEAM.lists` is 'lists',
// `BEAM.beam_lib` is 'beam_lib', `BEAM.Elixir.Enum` is 'Elixir.Enum' —
// what you write is the real module name, whatever language compiled it.
auto Lowering::beamModulePath(const ast::Expr& e, std::string& out) -> bool {
    auto* mc = std::get_if<ast::MethodCall>(&e.kind);
    if (!mc || !mc->receiver || !mc->args.empty() || !mc->namedArgs.empty() ||
        mc->block || mc->method.empty())
        return false;
    if (auto* uid = std::get_if<ast::UpperIdentifier>(&mc->receiver->kind)) {
        if (uid->name != "BEAM") return false;
        out = mc->method;
        return true;
    }
    if (!beamModulePath(*mc->receiver, out)) return false;
    out += "." + mc->method;
    return true;
}

auto Lowering::lowerMethodCall(const ast::MethodCall& n) -> ExprPtr {
    // `App.Models.User` as a value is that type's token, the same atom a
    // bare `User` is (kexhq/kex#509): what `select(as: App.Models.User)`
    // passes and `Type.of` reads. Sum types are registered by bare name.
    if (n.receiver && n.args.empty() && n.namedArgs.empty() && !n.block &&
        !n.mutating && !n.method.empty() &&
        std::isupper(static_cast<unsigned char>(n.method[0]))) {
        std::vector<std::string> path;
        if (modulePath(*n.receiver, path)) {
            std::string qualified;
            for (const auto& segment : path) qualified += segment + ".";
            qualified += n.method;
            if (records.count(qualified) || adtLayouts.count(n.method)) {
                auto ex = std::make_unique<Expr>();
                ex->node = Construct{n.method, {}};
                return ex;
            }
        }
    }
    // A record field wins over a same-named function even when a block's
    // receiver type was not known to the checker. Keep UFCS as the lazy
    // fallback for values that do not declare this field (#476).
    if (!n.receiver || n.parenthesized || n.mutating || !n.args.empty() ||
        !n.namedArgs.empty() || n.block || !fieldAccessors.count(n.method))
        return lowerMethodCallImpl(n);
    std::vector<std::string> path;
    if (modulePath(*n.receiver, path)) return lowerMethodCallImpl(n);
    if (auto field = staticFieldRead(n)) return field;

    auto receiver = lower(n.receiver);
    const auto name = fresh("FieldReceiver");
    loweredReceivers[n.receiver.get()] = name;
    auto fallback = lowerMethodCallImpl(n);
    loweredReceivers.erase(n.receiver.get());

    Match match;
    match.subjects.push_back(var(name));
    for (const auto& slot : fieldAccessors.at(n.method)) {
        MatchClause clause;
        auto value = std::make_unique<Pattern>();
        value->kind = PatKind::Var;
        value->name = fresh("FieldValue");
        clause.guard = recordGuard(slot, var(value->name));
        clause.patterns.push_back(std::move(value));
        clause.body = callE("erlang", "element", 2,
                            two(litInt(slot.position), var(name)));
        match.clauses.push_back(std::move(clause));
    }
    MatchClause otherwise;
    auto wildcard = std::make_unique<Pattern>();
    wildcard->kind = PatKind::Wild;
    otherwise.patterns.push_back(std::move(wildcard));
    otherwise.body = std::move(fallback);
    match.clauses.push_back(std::move(otherwise));
    auto result = std::make_unique<Expr>();
    result->node = std::move(match);
    return makeLet(name, std::move(receiver), std::move(result));
}

} // namespace kex::ir
