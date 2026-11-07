// Method-call lowering methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"

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
    // BEAM interop: a direct call into any BEAM module, named verbatim.
    // `BEAM.lists.reverse(xs)` → `call 'lists':'reverse'(xs)`
    // `BEAM.Elixir.Phoenix.Router.match(c)` → `call 'Elixir.Phoenix.Router':'match'(c)`
    // A trailing block becomes the last argument.
    if (std::string mod; n.receiver && beamModulePath(*n.receiver, mod)) {
        std::vector<Binding> binds;
        std::vector<ExprPtr> args;
        for (const auto& a : n.args) args.push_back(atomize(a, binds));
        if (n.block) args.push_back(atomize(*n.block, binds));
        int ar = static_cast<int>(args.size());
        auto ex = std::make_unique<Expr>();
        ex->node = Call{mod, n.method, ar, std::move(args), false};
        return wrapLets(binds, std::move(ex));
    }
    // `.as(T)`, as the checker decided it (`Conversion`, owner the target):
    // distinct/backing conversions erase to the receiver; `.as(String)` is the
    // universal total display path.
    if (const auto* conversion = n.receiver ? &hir->targetOf(n) : nullptr;
        conversion && conversion->kind == hir::TargetKind::Conversion) {
        if (conversion->owner == "String")
            return callE("kex_intrinsic_kex", "show", 1,
                         one(lower(n.receiver)));
        // `"hello".as(Atom)`: the checker admits only a plain literal, so
        // the atom is a constant in the emitted code.
        if (conversion->owner == "Atom")
            if (const auto* literal =
                    std::get_if<ast::StringLiteral>(&n.receiver->kind);
                literal && literal->values.empty()) {
                // Plain text sits in `parts` (split, with nothing
                // interpolated between) or, for a literal marked
                // non-interpolating, in `value`.
                std::string text = literal->value;
                if (!literal->parts.empty()) {
                    text.clear();
                    for (const auto& part : literal->parts) text += part;
                }
                return lit(LitKind::Atom, text);
            }
        return lower(n.receiver);
    }
    // A method on a trait-typed lexical parameter dispatches through the
    // hidden dictionary supplied by its caller. The tuple slots follow
    // the trait declaration's required-method order.
    std::string dictionaryReceiver;
    if (const auto* receiver =
            std::get_if<ast::Identifier>(&n.receiver->kind))
        dictionaryReceiver = receiver->name;
    else if (std::holds_alternative<ast::ThisExpr>(n.receiver->kind))
        dictionaryReceiver = "this";
    if (!dictionaryReceiver.empty()) {
        if (auto dictionaries = traitDictionaries.find(dictionaryReceiver);
            dictionaries != traitDictionaries.end()) {
            for (const auto& [trait, dictionary] : dictionaries->second) {
                auto methods = traitMethods.find(trait);
                if (methods == traitMethods.end()) continue;
                auto method = std::find_if(
                    methods->second.begin(), methods->second.end(),
                    [&](const auto& candidate) {
                        return candidate.name == n.method;
                    });
                if (method == methods->second.end()) continue;
                std::vector<Binding> binds;
                std::vector<ExprPtr> args;
                args.push_back(atomize(n.receiver, binds));
                for (const auto& arg : n.args)
                    args.push_back(atomize(arg, binds));
                if (n.block) args.push_back(atomize(*n.block, binds));
                auto slot = static_cast<int>(
                    std::distance(methods->second.begin(), method)) + 1;
                auto callee = atomize_ir(
                    callE("erlang", "element", 2,
                          two(litInt(slot), var(dictionary))), binds);
                auto indirect = std::make_unique<Expr>();
                indirect->node = CallIndirect{
                    std::move(callee), std::move(args), false};
                return wrapLets(binds, std::move(indirect));
            }
        }
    }
    const bool thisReceiver =
        std::holds_alternative<ast::ThisExpr>(n.receiver->kind) ||
        (std::holds_alternative<ast::Identifier>(n.receiver->kind) &&
         std::get<ast::Identifier>(n.receiver->kind).name == "this");
    if (thisReceiver && n.args.empty() && n.namedArgs.empty() &&
        !n.block && fieldAccessors.count(n.method)) {
        const auto& candidates = fieldAccessors.at(n.method);
        auto selected = std::find_if(
            candidates.begin(), candidates.end(),
            [&](const auto& candidate) {
                return candidate.record == currentMakeType;
            });
        if (selected != candidates.end())
            return callE("erlang", "element", 2,
                two(litInt(selected->position), lower(n.receiver)));
    }
    // `Type.of(x)` the checker answered: emit the recorded shape as a
    // literal record. Ahead of every dispatch path, like the walker's.
    if (staticTypeOfCalls) {
        auto recorded = staticTypeOfCalls->find(&n);
        if (recorded != staticTypeOfCalls->end()) {
            std::vector<Binding> binds;
            // A value argument still runs (it may have effects); one that
            // NAMES a function does not — `Date.parse` alone is a call
            // missing its argument, which would not even compile.
            if (recorded->second.evaluateArgument)
                for (const auto& arg : n.args)
                    if (arg) binds.push_back({fresh("TypeArg"), lower(arg)});
            // A type token from another unit contains no call to that
            // unit. Load its companion before `Type.constructors` (or
            // record-field reflection) reads the layouts it registers.
            if (externalModules) {
                auto owner = recorded->second.type.name;
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
            return wrapLets(binds, structuredTypeLiteral(recorded->second.type));
        }
    }
    if (auto field = staticFieldRead(n)) return field;
    // Process.spawn(state) must remember the module that owns the serving
    // slot dispatchers.  The source-level prelude wrapper cannot discover
    // its caller's compiled module at runtime, so lower this call directly.
    if (n.method == "spawn" && n.receiver && n.args.size() == 1) {
        if (auto* owner = std::get_if<ast::UpperIdentifier>(&n.receiver->kind);
            owner && owner->name == "Process") {
            std::vector<Binding> binds;
            auto state = atomize(n.args[0], binds);
            std::vector<ExprPtr> slotAtoms;
            // The module holding the slot dispatchers. A serving block at
            // the top level compiles into the entry module; one inside
            // `module Store` compiles into `Kex.Store`, and naming the
            // entry module there sent every call to a module that defines
            // no slots (rodolfo docs/kex-issues.md #2).
            std::string servingModule = outputModule;
            // The state's type, as the checker decided the call.
            if (const auto* target = &hir->targetOf(n);
                target && target->kind == hir::TargetKind::Builtin) {
                    auto stateType = target->owner;
                    if (auto angle = stateType.find('<');
                        angle != std::string::npos)
                        stateType.resize(angle);
                    // The slot registry is keyed by the make target AS
                    // WRITTEN, which inside a module is the bare name,
                    // while the checker types the spawned state by its
                    // qualified identity — so `Store.Counter` found
                    // nothing and the server started with an EMPTY slot
                    // list, answering `{unknown_serving_slot, get}` to
                    // its first call.
                    auto key = stateType;
                    if (!servingSlotsByType.count(key))
                        if (auto dot = stateType.rfind('.');
                            dot != std::string::npos)
                            key = stateType.substr(dot + 1);
                    if (auto slots = servingSlotsByType.find(key);
                        slots != servingSlotsByType.end())
                        for (const auto& slot : slots->second)
                            slotAtoms.push_back(lit(LitKind::Atom, slot));
                    if (auto declaring = localTypeModules.find(key);
                        declaring != localTypeModules.end())
                        servingModule =
                            mangleModulePath("Kex." + declaring->second);
                    if (slotAtoms.empty() && externalModules) {
                        auto external = externalModules->servingSlots.find(stateType);
                        if (external == externalModules->servingSlots.end())
                            external = externalModules->servingSlots.find(key);
                        if (external != externalModules->servingSlots.end()) {
                            servingModule = external->second.first;
                            for (const auto& slot : external->second.second)
                                slotAtoms.push_back(lit(LitKind::Atom, slot));
                        }
                    }
            }
            auto allowedSlots = std::make_unique<Expr>();
            allowedSlots->node = MakeList{std::move(slotAtoms), std::nullopt};
            return wrapLets(
                binds,
                callE("kex_intrinsic_process", "spawnServing", 3,
                      three(std::move(state),
                            lit(LitKind::Atom, servingModule),
                            std::move(allowedSlots))));
        }
    }
    if (resolvedCalls) {
        // Local types shadow prelude-resolved calls (a user `record Parser`
        // must win over the prelude `module Parser`).
        bool localTypeShadows = false;
        // A local method only shadows a resolved target when it is
        // defined FOR THIS receiver — the same requirement the
        // static-type branch below states. `localMethods` alone let an
        // unrelated method of the same name capture a call on a name that
        // is both a module and a record: `DateTime.now` inside a
        // `capability Clock do foul now()` became `apply 'now'/1`, calling
        // itself, and hung the moment any `make ..., implement: Clock`
        // existed to put `now` in `localMethods` (kexhq/kex#143).
        if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.receiver->kind)) {
            localTypeShadows =
                knownTypes.count(uid->name) &&
                typeOwnsStaticFunction(uid->name, n.method);
        }
        if (auto* id = std::get_if<ast::Identifier>(&n.receiver->kind))
            localTypeShadows = id->name == "this";
        if (std::holds_alternative<ast::ThisExpr>(n.receiver->kind))
            localTypeShadows = true;
        auto resolved = resolvedCalls->find(&n);
        const bool resolvedServerSlot = resolved != resolvedCalls->end() &&
            !resolved->second.localDispatchTypes.empty() &&
            resolved->second.localDispatchTypes.front().rfind("Server<", 0) == 0;
        if (resolvedServerSlot) {
            std::vector<Binding> binds;
            auto server = atomize(n.receiver, binds);
            std::vector<ExprPtr> protocolArgs;
            for (const auto& arg : n.args)
                protocolArgs.push_back(atomize(arg, binds));
            if (n.block) protocolArgs.push_back(atomize(*n.block, binds));
            auto argsList = std::make_unique<Expr>();
            argsList->node = MakeList{std::move(protocolArgs), std::nullopt};

            if (resolved->second.isServingCast) {
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
        // Semantic analysis has already applied lexical `using` policy
        // and argument-type specificity. Once it names an imported
        // target, source-import priority must not override that choice.
        // A type's own method, inherited trait defaults included, is the
        // checker's decision too (ownerOfCall): it records no target then.
        // A bare `x.field` whose name is also an imported receiver method
        // (`name` is Weekday's and Type's) must go through THIS module's
        // accessor, which matches the record tags and delegates to the
        // import for anything else. Calling the import directly skips the
        // record arms, so an erased receiver — `opaque(p).name`, or any
        // REPL value — died with "Undefined method: name for Person"
        // while a statically typed `p.name` read the field fine.
        // Unless the checker named a function of a unit compiled apart:
        // `speed.value` is `Units.Dynamic`'s, where only other units'
        // records (`Net.Port`) have a `value` field.
        const bool resolvedApart =
            resolved != resolvedCalls->end() && externalModules &&
            std::any_of(externalModules->compiledApartModules.begin(),
                        externalModules->compiledApartModules.end(),
                        [&](const std::string& module) {
                            const auto atom = externalModules->nameToAtom.find(module);
                            return atom != externalModules->nameToAtom.end() &&
                                   atom->second == resolved->second.backendModule;
                        });
        const bool localFieldAccessor =
            n.args.empty() && n.namedArgs.empty() && !n.block &&
            !n.mutating && fieldAccessors.count(n.method) && !resolvedApart;
        // `Outer.Inner.greet(n)` is a call INTO a module, but once any
        // make block defines a method of the same name the resolver picks
        // that receiver method and the dotted path is then lowered as a
        // value — which a module is not, so it died with "Undefined
        // function: Outer.Inner" while the walker was right (kexhq/kex#182).
        // A bare receiver never had this problem, so only a dotted path
        // naming a module of this unit overrides the resolved target.
        // A call into a capability must reach the substitution dispatch,
        // which lives on the module-qualified path below. If the resolver
        // claims it first the call goes straight to the real
        // implementation and `with` silently does nothing — the fail-open
        // this feature exists to prevent (kexhq/kex#143).
        std::string capabilityTarget;
        if (n.receiver) {
            std::vector<std::string> segments;
            if (modulePath(*n.receiver, segments) && !segments.empty())
                if (const auto named = capabilityModuleNamed(segments);
                    !named.empty() && capabilityInterfaceHas(named, n.method)) capabilityTarget = named;
        }
        bool modulePathOwnsCall = false;
        if (n.receiver) {
            std::vector<std::string> segments;
            if (modulePath(*n.receiver, segments) && segments.size() >= 2) {
                std::string joined;
                for (size_t i = 0; i < segments.size(); ++i) {
                    if (i) joined += ".";
                    joined += segments[i];
                }
                modulePathOwnsCall =
                    moduleFunctions.count(joined + "." + n.method) > 0;
            }
        }
        if (resolved != resolvedCalls->end() && !localTypeShadows &&
            !localFieldAccessor &&
            !modulePathOwnsCall) {
            if (!resolved->second.alternatives.empty())
                return providerChoice(n, resolved->second);
            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            if (resolved->second.passesReceiver)
                args.push_back(atomize(n.receiver, binds));
            // A make method of this unit: its declaration says where each
            // label goes and what an omitted parameter defaults to.
            const MakeMethod* declared =
                resolved->second.backendModule.empty() &&
                resolved->second.passesReceiver
                    ? makeMethodFor(n, receiverOwner(n), true) : nullptr;
            if (declared) {
                for (auto& slot : placeMethodArguments(n, *declared, binds))
                    args.push_back(std::move(slot));
            } else if (!n.namedArgs.empty()) {
                const auto& pnames = resolved->second.paramNames;
                auto expected = static_cast<size_t>(
                    resolved->second.backendArity -
                    (resolved->second.passesReceiver ? 1 : 0) -
                    resolved->second.traitDictionaries.size());
                // A foul target's arity can count the capability context
                // too, which is no parameter of its own.
                const bool contextCounted = threadsCapabilities() &&
                    resolved->second.isFoul && pnames.size() + 1 == expected;
                if (pnames.size() != expected && !contextCounted)
                    throw LowerError(
                        "IR lower: named args to imported function with unknown params: " +
                        resolved->second.backendFunction);
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
                                         resolved->second.backendFunction);
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
                if (!resolved->second.defaultSources.empty())
                    fillImportedDefaults(resolved->second, slots, binds);
                for (auto& slot : slots)
                    if (!slot) slot = lit(LitKind::None, "none");
                for (auto& slot : slots) args.push_back(std::move(slot));
            } else if (const auto expected = resolved->second.paramNames.size();
                       !resolved->second.defaultSources.empty() &&
                       n.args.size() + (n.block ? 1 : 0) < expected) {
                // Fewer arguments than parameters: the rest are the
                // callee's defaults, which only its interface knows.
                std::vector<ExprPtr> slots(expected);
                size_t next = 0;
                for (const auto& arg : n.args) slots[next++] = atomize(arg, binds);
                if (n.block) slots[next++] = atomize(*n.block, binds);
                fillImportedDefaults(resolved->second, slots, binds);
                for (auto& slot : slots) args.push_back(std::move(slot));
            } else {
                for (const auto& arg : n.args)
                    args.push_back(atomize(arg, binds));
                if (n.block) args.push_back(atomize(*n.block, binds));
                // A local make method's defaults, for a local target only: an
                // imported one fills its own (`uri.string` beside a local
                // `string(prefix = ...)`, kexhq/kex#475).
                else if (resolved->second.backendModule.empty())
                    for (const auto* value : trailingMakeDefaults(n))
                        args.push_back(atomize(*value, binds));
            }
            // A local make-method beats an imported TRAIT DEFAULT of the
            // same name. `make Comparison do let to(@Less, Integer)` never
            // ran on BEAM: the call bound to `kex_prelude:to/Showable`
            // (every Comparison value is Showable), which answers only for
            // `String` — so `Less.to(Integer)` returned the generic
            // conversion's `None` instead of `Just(-1)`, silently. This is
            // the rule already stated above for local TYPES, one level
            // down: semantic analysis picks the imported target because a
            // trait default matches any receiver, which makes it the least
            // specific candidate, not the most.
            //
            // Only for a trait DEFAULT — a concrete imported target was
            // chosen on real type evidence and must stand. The local
            // definition carries its own fallback to the prelude for
            // receivers it does not match (see the `to` fallback clause
            // and `makeDispatcher`'s generic arm), so nothing is lost for
            // the values the default would have handled.
            const auto traitDefaultOwner = [&]() -> std::string {
                auto slash = resolved->second.backendFunction.find('/');
                if (slash == std::string::npos) return {};
                auto owner =
                    resolved->second.backendFunction.substr(slash + 1);
                return traitMethods.count(owner) ? owner : std::string{};
            }();
            // Only at an arity the local name is defined with: a program's
            // own `to(String, format)` (`to/3`) must leave `3.to(String)`
            // to the default, not call a `to/2` that does not exist.
            const bool localBeatsTraitDefault =
                !traitDefaultOwner.empty() && localMethods.count(n.method) &&
                knownFns.count(n.method) &&
                definedFnArities.count(n.method + "/" +
                                       std::to_string(args.size()));
            if (localBeatsTraitDefault) {
                return wrapLets(
                    binds,
                    localCallExpr(n.method, std::move(args)));
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
            if (resolved->second.backendModule.empty() &&
                resolved->second.passesReceiver &&
                resolved->second.traitDictionaries.empty() &&
                resolved->second.backendArity ==
                    static_cast<int>(args.size()))
                // Built-in structural owners only (List, Map, the primitives):
                // their prelude implementation is the whole method. A record
                // type's dispatcher also forwards to `make` extensions from
                // units compiled apart (Units extends Measure), and a trait's
                // default answers only when the concrete type has none —
                // both decided at run time, so both keep the dispatcher.
                if (const auto owner = receiverOwner(n);
                    !owner.empty() && ownsMethod(owner, n.method) &&
                    (owner == "List" || owner == "Map" ||
                     primGuardBifs().count(owner)) &&
                    takesPlainParameters(n.method, owner))
                    if (auto implementation = collidingImplementationName(
                            n.method, owner, args.size())) {
                        if (threadsCapabilities() && resolved->second.isFoul)
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
                 resolved->second.traitDictionaries) {
                const ast::Expr* source = nullptr;
                if (resolved->second.passesReceiver) {
                    if (argument == 0) source = n.receiver.get();
                    else if (argument - 1 < n.args.size())
                        source = n.args[argument - 1].get();
                } else if (argument < n.args.size()) {
                    source = n.args[argument].get();
                }
                // Same as above: an omitted defaulted trait parameter
                // takes its dictionary from the default expression.
                if (!source) {
                    const size_t index = resolved->second.passesReceiver
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
            if (std::getenv("KEX_DEBUG_TO2") && n.method == "to")
                std::fprintf(stderr,
                             "[to] mod=%s fn=%s arity=%d args=%zu\n",
                             resolved->second.backendModule.c_str(),
                             resolved->second.backendFunction.c_str(),
                             resolved->second.backendArity, args.size());
            auto backendFunction = resolved->second.backendFunction;
            // A trait-default→dispatcher demotion used to rewrite
            // `backendFunction` here (`showValue/Showable` → `showValue`)
            // so unpinned-receiver calls reached the dispatcher's
            // variant arms. The Phase 2 trust audit removed it: with the
            // rewrite disabled, spec-beam, the 400-case fuzzer, and four
            // targeted probes (literal/bound/typed/unpinned Optional
            // interpolation, custom Showable instance) all agree, and
            // the cited `Just(50)` shape no longer reproduces —
            // resolution now reaches the dispatcher on its own.
            if (!resolved->second.localDispatchTypes.empty()) {
                auto exact = mangleReceiverSignature(
                    backendFunction,
                    resolved->second.localDispatchTypes);
                auto byReceiver = mangleReceiverImplementation(
                    backendFunction,
                    resolved->second.localDispatchTypes.front());
                if (knownFns.count(exact))
                    backendFunction = std::move(exact);
                else if (knownFns.count(byReceiver))
                    backendFunction = std::move(byReceiver);
                else {
                    // The checker names a module's types by their
                    // QUALIFIED identity (`Srv.Server`, `Srv.Options`),
                    // but a `make Server` written inside `module Srv`
                    // mangles its per-signature implementations under the
                    // spelling as WRITTEN, which inside the module is
                    // bare. So a call from OUTSIDE the module built
                    // `listen/Srv.Server,String,Integer`, found nothing,
                    // kept the plain name — and the plain name is exactly
                    // what an argument-overloaded method is never emitted
                    // under: erlc rejected the unit with "undefined
                    // function listen/4". Retry in the declaring module's
                    // own spelling before giving up.
                    auto local = resolved->second.localDispatchTypes;
                    for (auto& type : local)
                        type = moduleLocalTypeSpelling(type);
                    if (local != resolved->second.localDispatchTypes) {
                        auto localExact =
                            mangleReceiverSignature(backendFunction, local);
                        auto localByReceiver =
                            mangleReceiverImplementation(backendFunction,
                                                         local.front());
                        if (knownFns.count(localExact))
                            backendFunction = std::move(localExact);
                        else if (knownFns.count(localByReceiver))
                            backendFunction = std::move(localByReceiver);
                    }
                }
            }
            // A proven `name/Head` for an imported make-block method —
            // unless the receiver takes a trait dictionary (clause
            // (10)), which names trait dispatch the mangled spelling
            // cannot serve. (Set only for imported receiver calls, so
            // index 0 is the receiver and `localDispatchTypes` — the
            // same-unit path above — is empty.)
            if (!resolved->second.importDispatchSpelling.empty()) {
                bool receiverHasDictionary = false;
                for (const auto& [argument, _] :
                     resolved->second.traitDictionaries)
                    if (argument == 0) receiverHasDictionary = true;
                if (!receiverHasDictionary) {
                    backendFunction =
                        resolved->second.importDispatchSpelling;
                    // Applied-spelling audit for `make test-devirt`:
                    // `module name/Head beamArity`, one line per call
                    // site, appended (O_APPEND keeps parallel spec
                    // runs from interleaving).
                    if (const char* audit =
                            std::getenv("KEX_DEVIRT_AUDIT")) {
                        if (FILE* out = std::fopen(audit, "a")) {
                            std::fprintf(
                                out, "%s %s %d\n",
                                resolved->second.backendModule.c_str(),
                                backendFunction.c_str(),
                                resolved->second.backendArity);
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
                auto module = resolved->second.backendModule;
                auto function = backendFunction;
                const int arity = resolved->second.backendArity;
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
            if (resolved->second.backendModule.empty() &&
                resolved->second.backendArity ==
                    static_cast<int>(args.size())) {
                // The resolver knows WHICH implementation won, so its
                // foulness settles the context where the name-and-arity
                // lookup in callNeedsContext cannot: `get` is PURE at
                // arity 2 for Net.HTTP's Headers and FOUL at arity 2 for
                // its Client, and one answer has to serve both.
                if (threadsCapabilities() && resolved->second.isFoul) {
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
            if (threadsCapabilities() && resolved->second.isFoul &&
                resolved->second.backendArity ==
                    static_cast<int>(args.size()) + 1) {
                args.push_back(currentContext());
                auto call = std::make_unique<Expr>();
                call->node = Call{resolved->second.backendModule,
                                  backendFunction,
                                  resolved->second.backendArity,
                                  std::move(args), false};
                return wrapLets(binds, std::move(call));
            }
            return wrapLets(
                binds,
                callE(resolved->second.backendModule,
                      backendFunction,
                      resolved->second.backendArity,
                      std::move(args)));
        }
    }
    // A call into the `Kex.Intrinsic.<Category>` runtime module, e.g.
    // `Kex.Intrinsic.List.reverse(x)`. Compile to a plain cross-module call
    // `call 'kex_intrinsic_list':'reverse'(x)` — the emitter knows NOTHING
    // about `reverse`; the runtime module owns it, and the Kex prelude above
    // owns the typed semantics. Adding a primitive = add a runtime function,
    // with zero emitter changes.
    {
        std::vector<std::string> path;
        if (modulePath(*n.receiver, path) && path.size() >= 3 &&
            path[0] == "Kex" && path[1] == "Intrinsic") {
            std::string mod = "kex_intrinsic";
            for (size_t i = 2; i < path.size(); i++) {
                mod += "_";
                for (char c : path[i]) mod += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            auto ex = std::make_unique<Expr>();
            int ar = static_cast<int>(args.size());
            ex->node = Call{mod, n.method, ar, std::move(args), false};
            return wrapLets(binds, std::move(ex));
        }
    }
    // User module function: `Util.double(21)` / `Util.Math.double(21)`.
    // Resolve first against the explicit receiver path, then relative to
    // the enclosing module path so nested modules can refer to siblings.
    {
        std::vector<std::string> path;
        if (!path.empty() || modulePath(*n.receiver, path)) {
            const auto candidates = modulePathCandidates(path);
            std::unordered_set<std::string> tried;
            for (const auto& candidate : candidates) {
                if (candidate.empty() || !tried.insert(candidate).second) continue;
                auto qualKey = candidate + "." + n.method;
                auto it = moduleFunctions.find(qualKey);
                // A call into a capability asks the context first and
                // falls back to the capability's own body, which is what
                // runs when nothing replaced it (kexhq/kex#181).
                const bool importedCapability =
                    externalModules &&
                    externalModules->capabilityModules.count(candidate) > 0;
                if (it != moduleFunctions.end() && threadsCapabilities() &&
                    (capabilityModules.count(candidate) ||
                     importedCapability) &&
                    capabilityInterfaceHas(candidate, n.method) &&
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
                    getArgs.push_back(lit(LitKind::String, candidate));
                    getArgs.push_back(currentContext());
                    getArgs.push_back(lit(LitKind::Atom, "undefined"));
                    auto impl = fresh("Impl");
                    Match m;
                    m.subjects.push_back(
                        callE("maps", "get", 3, std::move(getArgs)));
                    MatchClause missing;
                    auto undef = std::make_unique<Pattern>();
                    undef->kind = PatKind::Lit;
                    undef->litKind = LitKind::Atom;
                    undef->litText = "undefined";
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
                        const auto* selected = namedClause(
                            qualKey, n.namedArgs, n.args.size() + bool(n.block));
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
            }
            // External loaded modules: BinaryTree.fromList → 'Kex.BinaryTree':fromList
            // Skip when the receiver names a local type that owns this
            // method — local types shadow prelude modules with the same name.
            if (externalModules
                && !(path.size() == 1
                     && typeOwnsStaticFunction(path[0], n.method))) {
                for (const auto& candidate : candidates) {
                    if (candidate.empty()) continue;
                    auto qualKey = candidate + "." + n.method;
                    auto eit = externalModules->exportToBeamFn.find(qualKey);
                    // This is the route a PRELUDE capability takes —
                    // `ENV.get(...)` reaches neither the local module path
                    // nor the receiver path (kexhq/kex#143).
                    if (eit != externalModules->exportToBeamFn.end() &&
                        threadsCapabilities() &&
                        isCapabilityModule(candidate) &&
                        capabilityInterfaceHas(candidate, n.method) &&
                        n.namedArgs.empty()) {
                        auto ait = externalModules->nameToAtom.find(candidate);
                        if (ait != externalModules->nameToAtom.end()) {
                            const auto atom = ait->second;
                            const auto beamFn = eit->second;
                            const auto key = qualKey;
                            // A trailing block is a source argument like
                            // any other: without it the dispatch below
                            // would call the replacement short one
                            // argument, which is why block-form
                            // capability calls used to skip substitution
                            // entirely and silently reach the real
                            // implementation.
                            std::vector<Binding> blockBinds;
                            std::vector<ExprPtr> loweredArgs;
                            for (const auto& arg : n.args)
                                loweredArgs.push_back(
                                    atomize(arg, blockBinds));
                            if (n.block)
                                loweredArgs.push_back(
                                    atomize(*n.block, blockBinds));
                            return capabilityDispatchLowered(
                                candidate, n.method,
                                std::move(loweredArgs), blockBinds,
                                [&](std::vector<ExprPtr> callArgs) {
                                    const int ar =
                                        static_cast<int>(callArgs.size());
                                    return externalCallExpr(
                                        key, atom, beamFn, ar,
                                        std::move(callArgs));
                                });
                        }
                    }
                    if (eit != externalModules->exportToBeamFn.end()) {
                        auto ait = externalModules->nameToAtom.find(candidate);
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
                }
            }
        }
    }
    // Namespace calls on an UpperIdentifier receiver, e.g. IO.printLine.
    if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.receiver->kind)) {
        std::vector<Binding> binds;
        // A type imported from another unit can name one of its `make`
        // methods without an instance: `Organization.slugged(name)`.
        // Its companion expects the type atom as the receiver.
        if (externalModules && !typeOwnsStaticFunction(uid->name, n.method)) {
            if (auto found = externalModules->receiverFunctions.find(n.method);
                found != externalModules->receiverFunctions.end()) {
                for (const auto& provider : found->second) {
                    const auto dot = provider.receiverType.rfind('.');
                    if (dot == std::string::npos ||
                        provider.receiverType.substr(dot + 1) != uid->name ||
                        !usingModules.contains(provider.receiverType.substr(0, dot)) ||
                        !provider.compiledApart)
                        continue;
                    std::vector<ExprPtr> args;
                    args.push_back(lit(LitKind::Atom, provider.receiverType));
                    if (!n.namedArgs.empty()) continue;
                    for (const auto& value : n.args) args.push_back(atomize(value, binds));
                    if (n.block) args.push_back(atomize(*n.block, binds));
                    if (provider.isFoul) args.push_back(currentContext());
                    if (static_cast<int>(args.size()) != provider.beamArity) continue;
                    return wrapLets(binds, callE(provider.moduleAtom,
                                                 provider.beamFunction,
                                                 provider.beamArity, std::move(args)));
                }
            }
        }
        std::vector<ExprPtr> args;
        for (const auto& a : n.args) args.push_back(atomize(a, binds));
        // Static method dispatch: `Type.method(args)` on a user type whose
        // `method` is a local function/make-method. If that method has an
        // implicit `this`, pass a placeholder receiver (the type tag).
        if (typeOwnsStaticFunction(uid->name, n.method)) {
            std::string mangled =
                mangleQualifiedMember(uid->name, n.method);
            std::string callName = knownFns.count(mangled) ? mangled : n.method;
            std::vector<ExprPtr> callArgs;
            if (implicitThisMethods.count(callName))
                callArgs.push_back(lit(LitKind::Atom, canonicalRecordName(uid->name)));
            for (auto& a : args) callArgs.push_back(std::move(a));
            auto ex = std::make_unique<Expr>();
            ex->node = localCall(callName, std::move(callArgs));
            return wrapLets(binds, std::move(ex));
        }
        // `Mod.Tag` for an ADT declared inside a module (e.g. `Kex.FS`).
        // The tag is just an atom at runtime, so the qualifier has no
        // representation here — emit the bare tag.
        if (n.args.empty() && !n.block && n.namedArgs.empty()) {
            if (auto tag = qualifiedVariantTag(uid->name + "." + n.method);
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
            nullaryVariantTags.count(uid->name) ||
            // Prelude/imported constructors are not in the local tag set;
            // the external variant table (seeded for display) knows them.
            (variantOwner.count(uid->name) &&
             variantArity.count(uid->name) &&
             variantArity.at(uid->name) == 0);
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
            zeroArgFns.count(uid->name) ||
            topLevelConstants.count(uid->name) ||
            !moduleConstantSymbol(uid->name).empty() ||
            // An IMPORTED zero-arg member is the same thing from another
            // module: `using Limits` + `"${LIMIT}"` is a showValue on the
            // constant's value, not a module member lookup
            // (kexhq/kex#282).
            [&]() {
                auto imp = moduleImports.find(uid->name);
                return imp != moduleImports.end() &&
                    emittedModuleZeroArg.count(imp->second);
            }() ||
            // ...or one from a module compiled apart.
            externalConstant(uid->name) != nullptr;
        if (!nullaryConstructorReceiver && !topLevelBindingReceiver)
            return wrapLets(binds,
                runtimeError("Undefined function: " + uid->name + "." + n.method));
    }
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

    // A public function brought into scope by `using Module` also
    // participates in UFCS: `100.meter` is `meter(100)`, and
    // `distance.per(duration)` is `per(distance, duration)`. The source
    // import must win over an identically named prelude receiver method:
    // module compilation merges declarations into one flat IR first, so
    // imported functions also appear in localMethods at this point.
    auto importedMethod = moduleImports.find(m);
    bool importedArityMatches = importedMethod != moduleImports.end();
    if (importedArityMatches) {
        const auto actual = n.args.size() + n.namedArgs.size() + 1 +
            (n.block ? 1 : 0);
        for (const auto& [qualified, emitted] : moduleFunctions) {
            if (emitted != importedMethod->second) continue;
            // Labelled arguments are placed by the checker's resolved target.
            if (!n.namedArgs.empty()) {
                importedArityMatches = false;
                break;
            }
            if (auto names = fnParamNames.find(qualified);
                names != fnParamNames.end())
                importedArityMatches = names->second.size() == actual;
            break;
        }
    }
    if (importedArityMatches) {
        auto imported = importedMethod;
            std::vector<ExprPtr> args;
            {
                args.push_back(rv());
                for (const auto& value : n.args)
                    args.push_back(atomize_ir(lower(value), rb));
                for (const auto& [_, value] : n.namedArgs)
                    args.push_back(atomize_ir(lower(value), rb));
                if (n.block)
                    args.push_back(atomize_ir(lower(*n.block), rb));
            }
        const int arity = static_cast<int>(args.size());
        // An imported FREE function competing with some other module's
        // receiver method is decided between at run time.
        if (auto guarded = importedFreeFunctionDispatch(
                n, m, imported->second, arity, args))
            return ret(std::move(guarded));
        return ret(localCallExpr(imported->second, std::move(args)));
    }

    // A local method only claims the call when it is defined at THIS
    // arity; otherwise the external/prelude receiver function handles it.
    // `this.map.get(k, default)` inside a `make Box do get(key) ... end`
    // block used to compile to a local get/3 that was never defined.
    // A call that labels arguments or leaves defaulted ones out is written
    // shorter than the method it binds, whose arity is the one that counts.
    const auto owner = receiverOwner(n);
    const MakeMethod* declared =
        owner.empty() ? nullptr : makeMethodFor(n, owner, true);
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

    // External receiver functions take priority over prelude for UFCS.
    // The registry includes only package-declared provider modules here;
    // ordinary module exports never become receiver functions implicitly.
    if (externalModules
        && (!localMethods.count(m) || !localArityExists)) {
        auto found = externalModules->receiverFunctions.find(m);
        if (found != externalModules->receiverFunctions.end()) {
            // A unit's `make` for a type declared outside its module is in
            // scope only through `using` that module. A call only such a
            // `make` takes was decided `Hidden` by the checker (above).
            std::vector<ExternalModules::ReceiverFunction> providers;
            for (const auto& candidate : found->second) {
                std::string module;
                for (const auto& [name, atom] : externalModules->nameToAtom)
                    if (atom == candidate.moduleAtom) module = name;
                // The prelude's own modules are imported automatically; only a unit of
                // this build is held behind its `using`, as the checker holds it.
                if (!candidate.compiledApart || !candidate.foreignTarget || module.empty() ||
                    moduleInScope(module))
                    providers.push_back(candidate);
            }
            int actualArity = static_cast<int>(n.args.size() + n.namedArgs.size()) + 1 +
                              (n.block ? 1 : 0);
            const ExternalModules::ReceiverFunction* match = nullptr;
            bool sameModuleOverloads = false;
            // Several units' generic dispatchers beside the prelude's, for a
            // receiver whose type is known only at run time: the value's
            // registered owner picks one, and the prelude's takes the rest, as
            // a merged program's one dispatcher would.
            std::vector<const ExternalModules::ReceiverFunction*> ownerDispatchers;
            // Providers in more than one module: the one the checker
            // chose, or else the one for the receiver's static type
            // (`status.showValue` is Net.HTTP.Status's, not the
            // prelude's), when that leaves a single module.
            std::string providerModule;
            {
                std::set<std::string> modules;
                for (const auto& candidate : providers)
                    if (candidate.beamArity == actualArity)
                        modules.insert(candidate.moduleAtom);
                // The receiver's static type picks the providers declared
                // for it (`raw.close` on a TCPConnection, beside the
                // prelude's FileHandle one), when they are one module's.
                if (modules.size() > 1 && providerModule.empty())
                    if (const auto type = staticTypeOf(n.receiver.get())) {
                        const auto written = semantic::typeToString(type);
                        const auto last = [](const std::string& name) {
                            const auto dot = name.rfind('.');
                            return dot == std::string::npos ? name : name.substr(dot + 1);
                        };
                        std::set<std::string> owners;
                        for (const auto& candidate : providers)
                            if (candidate.beamArity == actualArity &&
                                !candidate.receiverType.empty() &&
                                last(candidate.receiverType) == last(written))
                                owners.insert(candidate.moduleAtom);
                        if (owners.size() == 1) providerModule = *owners.begin();
                    }
                // Still undecided between units' providers for their own
                // types and the prelude's: each of them for a value of its
                // type, the prelude's dispatcher for every other, decided
                // at run time as the merged build's local dispatcher
                // decides (`"${text}"` beside Net.HTTP's own `showValue`).
                const bool typedBesidePrelude =
                    modules.size() > 1 && providerModule.empty() &&
                    modules.count(names::kPreludeModule) &&
                    std::all_of(providers.begin(), providers.end(),
                                [&](const auto& candidate) {
                                    return candidate.beamArity != actualArity ||
                                           candidate.moduleAtom == names::kPreludeModule ||
                                           !candidate.receiverType.empty();
                                });
                if (typedBesidePrelude && modules.size() == 2) {
                    // One unit: its provider is the call, whose labels
                    // its own parameters place; dualProviderDispatch
                    // below guards it and falls back to the prelude.
                    const auto other = *std::find_if(
                        modules.begin(), modules.end(),
                        [](const std::string& module) { return module != names::kPreludeModule; });
                    const bool guardable = std::all_of(
                        providers.begin(), providers.end(),
                        [&](const auto& candidate) {
                            if (candidate.beamArity != actualArity ||
                                candidate.moduleAtom != other) return true;
                            return records.count(canonicalRecordName(candidate.receiverType)) ||
                                   primGuardBifs().count(candidate.receiverType);
                        });
                    // A type alias such as FilePath has no safe runtime
                    // guard here. For an erased receiver, the prelude's
                    // dispatcher handles List.join/2 and Range.join/2.
                    providerModule = guardable ? other : names::kPreludeModule;
                }
            }
            if (providerModule.empty() && n.namedArgs.empty()) {
                // The prelude's typed entries are its one dispatcher's arms; the
                // units' must be dispatchers too.
                std::set<std::string> modules;
                bool unitsGeneric = true, preludeGeneric = false;
                for (const auto& candidate : providers) {
                    if (candidate.beamArity != actualArity) continue;
                    modules.insert(candidate.moduleAtom);
                    if (candidate.moduleAtom == names::kPreludeModule)
                        preludeGeneric = preludeGeneric || candidate.receiverType.empty();
                    else
                        unitsGeneric = unitsGeneric && candidate.receiverType.empty();
                }
                if (unitsGeneric && preludeGeneric && modules.size() > 1) {
                    providerModule = names::kPreludeModule;
                    std::set<std::string> taken;
                    for (const auto& candidate : providers)
                        if (candidate.beamArity == actualArity &&
                            candidate.moduleAtom != names::kPreludeModule &&
                            taken.insert(candidate.moduleAtom).second)
                            ownerDispatchers.push_back(&candidate);
                }
            }
            for (const auto& candidate : providers) {
                if (candidate.beamArity != actualArity) continue;
                if (!providerModule.empty() && candidate.moduleAtom != providerModule)
                    continue;
                if (match) {
                    if (match->moduleAtom == candidate.moduleAtom) {
                        sameModuleOverloads = true;
                        continue;
                    }
                    return ret(runtimeError(
                        "Ambiguous receiver function: " + m + "/" +
                        std::to_string(actualArity)));
                }
                match = &candidate;
            }
            if (match) {
                std::vector<ExprPtr> pargs;
                pargs.push_back(rv());
                if (!n.namedArgs.empty())
                    throw LowerError("IR lower: named arguments to `" + m +
                                     "` reached the provider search unplaced");
                {
                    for (const auto& a : n.args)
                        pargs.push_back(atomize_ir(lower(a), rb));
                    if (n.block)
                        pargs.push_back(atomize_ir(lower(*n.block), rb));
                }
                if (std::getenv("KEX_DEBUG_TO2") && m == "to")
                    std::fprintf(stderr,
                                 "[ufcs] mod=%s fn=%s arity=%d same=%d\n",
                                 match->moduleAtom.c_str(),
                                 match->beamFunction.c_str(), actualArity,
                                 static_cast<int>(sameModuleOverloads));
                const auto function =
                    sameModuleOverloads ? m : match->beamFunction;
                // A package provider owns this name for ITS receiver type
                // only. Binding every call to it is right when the
                // receiver's static type says so — and wrong otherwise,
                // which is common: a constructor-pattern binding
                // (`Just(d) ->`) carries no type, so `d.get(:year, "?")`
                // on a Map went to `Kex.Regex:get/3` (Match's accessor)
                // and died with function_clause, where the walker
                // dispatched on the value and read the map
                // (examples/regexes.kex).
                //
                // When the prelude provides the same name at the same
                // arity, keep BOTH: guard on the provider's receiver type
                // and fall back to the prelude's dispatcher, which is the
                // walker's runtime dispatch spelled in Core Erlang.
                if (!ownerDispatchers.empty()) {
                    const auto callWith = [&](const std::string &module,
                                              const std::string &fn) {
                        std::vector<ExprPtr> copies;
                        for (const auto& arg : pargs) copies.push_back(clone(arg));
                        return callE(module, fn, actualArity, std::move(copies));
                    };
                    Match byOwner;
                    byOwner.subjects.push_back(
                        callE("kex_io", "method_owner", 1, one(clone(pargs.front()))));
                    for (const auto* provider : ownerDispatchers) {
                        MatchClause arm;
                        auto owner = std::make_unique<Pattern>();
                        owner->kind = PatKind::Lit;
                        owner->litKind = LitKind::Atom;
                        owner->litText = provider->moduleAtom;
                        arm.patterns.push_back(std::move(owner));
                        arm.body = callWith(provider->moduleAtom, provider->beamFunction);
                        byOwner.clauses.push_back(std::move(arm));
                    }
                    MatchClause rest;
                    auto wild = std::make_unique<Pattern>();
                    wild->kind = PatKind::Wild;
                    rest.patterns.push_back(std::move(wild));
                    rest.body = callWith(match->moduleAtom, function);
                    byOwner.clauses.push_back(std::move(rest));
                    auto chosen = std::make_unique<Expr>();
                    chosen->node = std::move(byOwner);
                    return ret(std::move(chosen));
                }
                if (auto dual = dualProviderDispatch(
                        n, m, match->receiverType, match->moduleAtom,
                        function, actualArity, pargs))
                    return ret(std::move(dual));
                return ret(callE(match->moduleAtom, function,
                                 actualArity, std::move(pargs)));
            }
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
                declared ? declared : makeMethodFor(n, owner, false)) {
            for (auto& slot : placeMethodArguments(n, *placed, binds))
                args.push_back(std::move(slot));
        } else {
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            // Make-block methods lower receiver-first. Named arguments
            // follow positional ones in the common `method(Type, name:
            // value)` form.
            for (const auto& [_, value] : n.namedArgs)
                args.push_back(atomize(value, binds));
            if (n.block) {
                args.push_back(atomize(*n.block, binds));
            } else {
                for (const auto* value : trailingMakeDefaults(n))
                    args.push_back(atomize(*value, binds));
            }
        }
        int arity = static_cast<int>(args.size());
        // The same competing-providers case as the package-provider path
        // above, one step later: a `using`-ed module compiled in this unit
        // owns `get` for its own type (Regex's Match), and the redirect
        // that moves this bare call into that module claims every `get` in
        // the program — including a Map's, which the prelude owns.
        if (auto dual = dualProviderDispatch(n, n.method,
                                             methodReceiverOwner(n.method),
                                             "", n.method, arity, args))
            return ret(wrapLets(binds, std::move(dual)));
        auto ex = std::make_unique<Expr>();
        // The receiver's own make function says whether it takes the capability
        // context. By name and arity alone a pure `Headers.remove(name)`
        // elsewhere in the program answered for a foul
        // `Repository.remove(id)`, which then went out one argument
        // short and reached the wrong arity's dispatcher: "Undefined
        // method: remove" once Net.HTTP was linked (kexhq/kex#451).
        if (const auto* owned = declared
                ? declared : ownedMakeFunction(n, args.size() - 1)) {
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

// record TypeName { f: v, ... } → {'TypeName', <fields in declared order,
// defaults filled for omitted ones>}. Field ORDER and defaults come from
// the record definition (the accessors read fixed positions).
auto Lowering::lowerRecordConstruction(const ast::RecordConstruction& n) -> ExprPtr {
    std::vector<Binding> binds;
    std::vector<ExprPtr> args;
    const bool fromReceiver = n.typeName == "New";
    const auto recordName = canonicalRecordName(
        (n.typeName == "This" || fromReceiver) ? currentMakeType
                                                : n.typeName);
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
