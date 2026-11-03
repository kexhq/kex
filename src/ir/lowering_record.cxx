// Records methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"

namespace kex::ir {

// One accessor clause per (record, field). A tag can be layed out twice —
// an imported record and a same-named local one (`record Request` next to
// the prelude's), or the same external layout arriving from two loaded
// modules — and the accessor matches on the tag alone, so the FIRST clause
// for a tag wins. That has to be the layout the tag actually has here:
// reading the prelude Request's `path` (element 3) out of a local
// two-element Request is a `badarg` on BEAM where the walker read the
// field fine (examples/ufcs.kex). Layouts are collected external-first, so
// replacing any earlier same-tag entry leaves the local one in place.
void Lowering::addFieldAccessor(const std::string& field, const std::string& record,
                      int position, int tupleSize, bool local) {
    auto& entries = fieldAccessors[field];
    for (const auto& entry : entries) {
        if (entry.record == record && entry.position == position &&
            entry.tupleSize == tupleSize)
            return; // the same layout seen twice (two loaded modules)
        if (entry.record == record && entry.tupleSize == tupleSize &&
            entry.position != position)
            throw LowerError(
                "ambiguous record layout for field `" + field +
                "` on `" + record + "`: indistinguishable " +
                std::to_string(tupleSize) + "-element layouts place it " +
                "at positions " + std::to_string(entry.position) +
                " and " + std::to_string(position));
    }
    // A local layout goes FIRST, so it wins wherever a tag is all the
    // dispatch has to go on (`fieldAccess`, and same-size collisions).
    if (local) entries.insert(entries.begin(), {record, position, tupleSize});
    else entries.push_back({record, position, tupleSize});
}

void Lowering::collectRecordLayout(const std::string& name,
                         const std::vector<std::string>& fields) {
    RecordInfo info;
    for (int i = 0; i < static_cast<int>(fields.size()); i++) {
        info.fields.push_back(fields[i]);
        info.defaults.push_back(nullptr);
        addFieldAccessor(fields[i], name, i + 2,
                         static_cast<int>(fields.size()) + 1, false);
    }
    records[name] = std::move(info);
}

void Lowering::collectRecord(const ast::RecordDef& rec,
                   const std::string& owner ) {
    const auto name = owner.empty() ? rec.name : owner + "." + rec.name;
    RecordInfo info;
    info.owner = owner;
    for (int i = 0; i < static_cast<int>(rec.fields.size()); i++) {
        info.fields.push_back(rec.fields[i].name);
        info.defaults.push_back(rec.fields[i].defaultValue ? &*rec.fields[i].defaultValue
                                                           : nullptr);
        addFieldAccessor(rec.fields[i].name, name, i + 2,
                         static_cast<int>(rec.fields.size()) + 1, true);
    }
    records[name] = std::move(info);
    auto& layout = recordLayouts[name];
    layout.clear();
    for (const auto& field : rec.fields)
        layout.push_back({field.name,
                          field.type ? semantic::structuredTypeOfDeclared(*field.type)
                                     : semantic::StructuredType{"Any", {}},
                          field.defaultValue.has_value()});
}

// The BEAM module providing `name/arity` as a plain prefix call, or "" if
// none does. Only an unambiguous provider counts: the name must resolve to
// the provider's own dispatcher (`beamFunction == name`) at exactly this
// arity, or — failing that — to a single module among the candidates.
auto Lowering::externalPrefixCall(const std::string& name, int arity) const
    -> std::string {
    if (!externalModules) return {};
    auto candidates = externalModules->receiverFunctions.find(name);
    if (candidates == externalModules->receiverFunctions.end()) return {};
    for (const auto& candidate : candidates->second)
        if (candidate.beamArity == arity && candidate.beamFunction == name)
            return candidate.moduleAtom;
    std::string single;
    for (const auto& candidate : candidates->second) {
        if (candidate.beamArity != arity) continue;
        if (single.empty()) single = candidate.moduleAtom;
        else if (single != candidate.moduleAtom) return {};
    }
    return single;
}

// Remember (or clear) the import gate for a make block's method names.
// A top-level block passes "" and un-gates every name it defines, so a
// module block cannot restrict a method the file also defines globally.
void Lowering::noteModuleScopedMake(const ast::MakeDef& make, const std::string& module,
                          const std::set<std::string>& declaredTypes ) {
    // Only a make for a FOREIGN type is gated. One for a type the module
    // declares is that type's own interface and travels with its values —
    // `SQL.select(:all).from(:users)` must keep working without a `using`.
    const auto targets = makeTargetNameList(make.target);
    const bool ownType = std::any_of(
        targets.begin(), targets.end(),
        [&](const std::string& target) {
            return declaredTypes.count(target) > 0 ||
                (target.starts_with(module + ".") &&
                 declaredTypes.count(target.substr(module.size() + 1)) > 0);
        });
    auto note = [&](const ast::FunctionDef& def) {
        auto& modules = moduleScopedMethods[def.name];
        if (module.empty() || ownType) modules = {""};
        else if (!modules.count(""))
            modules.insert(module);
    };
    for (const auto& item : make.body) {
        if (auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fn) note(**fn);
        } else if (auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
            if (!*visibility) continue;
            for (const auto& inner : (*visibility)->items)
                if (auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner))
                    if (*fn) note(**fn);
        }
    }
}

// Is a module-scoped `make` method reachable from here? Inside its own
// module, or under a `using` that names it — the same rule the walker's
// makeMethodInScope applies.
auto Lowering::makeMethodInScope(const std::string& method) const -> bool {
    auto owners = moduleScopedMethods.find(method);
    if (owners == moduleScopedMethods.end() || owners->second.empty() ||
        owners->second.count(""))
        return true;
    return std::any_of(owners->second.begin(), owners->second.end(),
                       [&](const std::string& module) {
                           return moduleInScope(module);
                       });
}

auto Lowering::moduleInScope(const std::string& module) const -> bool {
    if (module.empty() || currentModulePath == module ||
        currentModulePath.rfind(module + ".", 0) == 0)
        return true;
    for (const auto& imported : usingModules)
        if (imported == module ||
            imported.rfind(module + ".", 0) == 0 ||
            module.rfind(imported + ".", 0) == 0)
            return true;
    return false;
}

// The out-of-scope module whose `make` function takes this call's named
// arguments, judged by their static types: `power.to(String, in:
// Units.SI.Kilo(Units.SI.Watt))` without `using Units.SI`. The walker
// raises the same hint from the values.
auto Lowering::gatedModuleTaking(const ast::MethodCall& n) const -> std::string {
    if (n.namedArgs.empty()) return {};
    auto found = makeMethods.find(n.method);
    if (found == makeMethods.end()) return {};
    auto baseName = [](std::string written) {
        if (auto angle = written.find('<'); angle != std::string::npos)
            written.resize(angle);
        if (auto dot = written.rfind('.'); dot != std::string::npos)
            written = written.substr(dot + 1);
        return written;
    };
    std::vector<std::string> labels;
    for (const auto& [label, _] : n.namedArgs) labels.push_back(label);
    for (const auto& method : found->second) {
        if (method.modulePath.empty() || moduleInScope(method.modulePath))
            continue;
        const auto& clause = method.def->clauses[0];
        if (!namedArgumentsFit(clause, labels,
                               n.args.size() + (n.block ? 1 : 0)))
            continue;
        const bool typesFit = std::all_of(
            n.namedArgs.begin(), n.namedArgs.end(), [&](const auto& named) {
                for (const auto& param : clause.params) {
                    if (!param.name || *param.name != named.first) continue;
                    if (!param.type || !*param.type) return true;
                    const auto wanted = baseName(renderDispatchType(**param.type));
                    if (auto typed = expressionTypes
                            ? expressionTypes->find(named.second.get())
                            : decltype(expressionTypes->find(nullptr)){};
                        expressionTypes && typed != expressionTypes->end() &&
                        typed->second) {
                        const auto written =
                            semantic::typeToString(typed->second);
                        if (written != "Unknown")
                            return baseName(written) == wanted;
                    }
                    // The checker leaves the arguments of a call it could
                    // not resolve untyped, which is exactly this call. A
                    // constructor of the parameter's type says it anyway:
                    // `in: Units.SI.Kilo(Units.SI.Watt)`.
                    std::string constructor;
                    if (const auto* call =
                            std::get_if<ast::MethodCall>(&named.second->kind))
                        constructor = call->method;
                    else if (const auto* call =
                                 std::get_if<ast::FunctionCall>(&named.second->kind))
                        constructor = call->name;
                    else if (const auto* name =
                                 std::get_if<ast::UpperIdentifier>(&named.second->kind))
                        constructor = name->name;
                    auto tags = typeVariantTags.find(wanted);
                    return !constructor.empty() && tags != typeVariantTags.end() &&
                        std::find(tags->second.begin(), tags->second.end(),
                                  baseName(constructor)) != tags->second.end();
                }
                return false;
            });
        if (typesFit) return method.modulePath;
    }
    return {};
}

// The single receiver type a method of a module compiled in THIS unit
// belongs to, or "" when the method has no such unambiguous owner.
// The module a `using`-imported emitted symbol came from, or "" when this
// unit cannot say.
auto Lowering::importSourceModule(const std::string& emittedSymbol) const
    -> std::string {
    for (const auto& [qualified, emitted] : moduleFunctions) {
        if (emitted != emittedSymbol) continue;
        const auto dot = qualified.rfind('.');
        return dot == std::string::npos ? std::string()
                                        : qualified.substr(0, dot);
    }
    return {};
}

// Is the imported symbol the receiver method of the LOCAL type that owns
// this name? True only when the import came from the module declaring
// that type's `make` block — `using Regex` importing Match's `get`. An
// unrelated free function of the same name is not that, however much the
// spelling suggests it.
auto Lowering::importOwnsLocalReceiver(const std::string& method,
                             const std::string& emittedSymbol) const
    -> bool {
    const auto owner = methodReceiverOwner(method);
    if (owner.empty()) return false;
    auto declaring = localTypeModules.find(owner);
    if (declaring == localTypeModules.end()) return false;
    const auto source = importSourceModule(emittedSymbol);
    return !source.empty() && source == declaring->second;
}

auto Lowering::methodReceiverOwner(const std::string& method) const -> std::string {
    auto owners = methodOwners.find(method);
    if (owners == methodOwners.end() || owners->second.size() != 1)
        return {};
    const auto& owner = owners->second.front();
    return localTypeModules.count(owner) ? owner : std::string{};
}

// See the call site: a type-specific package provider and the prelude both
// answer to `method`, and this call cannot say statically which one the
// receiver wants. Returns the runtime-dispatching form, or nullptr when
// this situation does not apply (leaving the direct call alone).
auto Lowering::dualProviderDispatch(const ast::MethodCall& n,
                          const std::string& method,
                          const std::string& receiverType,
                          const std::string& moduleAtom,
                          const std::string& function, int arity,
                          std::vector<ExprPtr>& args) -> ExprPtr {
    if (!externalModules || receiverType.empty()) return nullptr;
    // Method metadata names the receiver as it was WRITTEN; the record
    // table is keyed by canonical identity. Compare the two in the same
    // spelling, here and against the checker's static type below.
    const auto canonicalReceiver = canonicalRecordName(receiverType);
    // Only a guard-safe type test qualifies: the tagged-tuple fallback in
    // `typeGuard` calls element/2 eagerly, which badargs rather than
    // soft-failing for the primitive receivers this exists to route.
    if (!records.count(canonicalReceiver) &&
        !primGuardBifs().count(receiverType))
        return nullptr;
    // A statically known receiver of exactly the provider's type needs no
    // dispatch — that is the case the direct call already gets right.
    // "" when the checker has nothing concrete for this receiver — an
    // inference variable or Unknown is not evidence, and `typeToString`
    // renders those as ordinary names ("unknown", "T4") that would
    // otherwise read as a type mismatch.
    std::string staticReceiver;
    if (expressionTypes && n.receiver) {
        auto found = expressionTypes->find(n.receiver.get());
        if (found != expressionTypes->end() && found->second &&
            !std::holds_alternative<semantic::UnknownType>(found->second->kind) &&
            !std::holds_alternative<semantic::TypeVar>(found->second->kind))
            staticReceiver = semantic::typeToString(found->second);
    }
    if (canonicalRecordName(staticReceiver) == canonicalReceiver)
        return nullptr;
    // The other provider: the dispatcher of whichever module also answers
    // this name at this arity (the prelude, in practice). There may be
    // none — `whiteSpaces` belongs to Parsing's `Input` and to nothing
    // else — and then the second arm is the failure the walker reports.
    // Calling the provider regardless gave `function_clause` for
    // `"a b".whiteSpaces` where the walker said "Undefined method:
    // whiteSpaces for String".
    std::string fallbackModule;
    std::string fallbackFunction;
    std::set<std::string> servedReceivers;
    if (auto candidates = externalModules->receiverFunctions.find(method);
        candidates != externalModules->receiverFunctions.end())
        for (const auto& candidate : candidates->second) {
            if (candidate.beamArity != arity) continue;
            // Canonical identity: the same record reaches this table
            // both as its module-qualified name and as the bare one it is
            // written with, and counting those as two providers hid the
            // single-provider case this check exists to find.
            servedReceivers.insert(
                canonicalRecordName(candidate.receiverType));
            // A candidate for the SAME receiver type is this provider
            // again, addressed another way (`using Parsing` compiles the
            // module into this unit, so its `whiteSpaces` shows up both as
            // a local method and as `Kex.Parsing:whiteSpaces`). Routing
            // the miss there just reaches the same clause and dies with
            // `function_clause`.
            if (canonicalRecordName(candidate.receiverType) ==
                    canonicalReceiver ||
                candidate.moduleAtom == moduleAtom ||
                candidate.beamFunction != method)
                continue;
            fallbackModule = candidate.moduleAtom;
            fallbackFunction = candidate.beamFunction;
        }
    // No other provider: the second arm can only be the failure the walker
    // reports. Take that route ONLY on positive evidence — this name is
    // served by exactly one receiver type AND the receiver is statically
    // some OTHER type. Without both, a call that is merely unprovable here
    // (a Stream reaching `take`, whose other providers are not listed as
    // receiver functions) would be rejected out of hand; those stay on the
    // direct call, exactly as before.
    if (fallbackModule.empty() &&
        (servedReceivers.size() != 1 || staticReceiver.empty()))
        return nullptr;
    // And only for a NOMINAL sole provider — a record some module owns,
    // like Parsing's `Input`. For a builtin receiver (List, String, Map)
    // the receiver-function table is an incomplete view of who answers:
    // while the prelude itself is being compiled, String's own `empty?`
    // is not in it yet, so "nobody else provides this" is wrong and the
    // failure arm would reject every String at runtime.
    if (fallbackModule.empty() &&
        !records.count(canonicalRecordName(*servedReceivers.begin())))
        return nullptr;
    std::vector<ExprPtr> fallbackArgs;
    for (const auto& arg : args) {
        try {
            fallbackArgs.push_back(clone(arg, "dualProviderDispatch"));
        } catch (const LowerError&) {
            return nullptr; // not atomic — keep the direct call
        }
    }
    auto subject = clone(args[0], "dualProviderDispatch subject");
    Match dispatch;
    dispatch.subjects.push_back(std::move(subject));
    auto arm = [&](ExprPtr guard, ExprPtr body) {
        MatchClause clause;
        auto pattern = std::make_unique<Pattern>();
        pattern->kind = PatKind::Var;
        pattern->name = fresh("_provider");
        clause.patterns.push_back(std::move(pattern));
        if (guard) clause.guard = std::move(guard);
        clause.body = std::move(body);
        dispatch.clauses.push_back(std::move(clause));
    };
    arm(typeGuard(receiverType,
                  clone(fallbackArgs[0], "dualProviderDispatch guard")),
        callE(moduleAtom, function, arity, std::move(args)));
    // A THIRD record with a FIELD of this same name (not a method) needs
    // its own arm here too: the generic fallback below only knows about
    // whichever OTHER method-provider exists, and has no idea a field of
    // this name exists on some unrelated sibling record — `Data.Set`'s
    // `items` field vs `Data.UnorderedSet`'s `items` method both being
    // reachable from the same compilation unit (kexhq/kex#234). Sound
    // only at arity 1: a field read takes no arguments past the receiver.
    if (arity == 1) {
        if (auto fieldSlots = fieldAccessors.find(method);
            fieldSlots != fieldAccessors.end())
            for (const auto& slot : fieldSlots->second) {
                if (slot.record == canonicalReceiver) continue;
                arm(recordGuard(slot, clone(fallbackArgs[0],
                                            "dualProviderDispatch field guard")),
                    callE("erlang", "element", 2,
                          two(litInt(slot.position),
                              clone(fallbackArgs[0],
                                   "dualProviderDispatch field"))));
            }
    }
    if (fallbackModule.empty()) {
        auto receiver = clone(fallbackArgs[0], "dualProviderDispatch miss");
        arm(nullptr, undefinedMethod(method, std::move(receiver)));
    } else {
        arm(nullptr, callE(fallbackModule, fallbackFunction, arity,
                           std::move(fallbackArgs)));
    }
    auto out = std::make_unique<Expr>();
    out->node = std::move(dispatch);
    return out;
}

// `hs.get(name)` where `get` is ALSO a bare function this unit imported —
// `using Rodolfo` brings its DSL verb `get(path, handler)` into scope
// alongside `Net.HTTP.Headers`'s own `get(name)`. With the receiver's
// type statically known the checker settles it; on an unannotated
// parameter it has nothing to settle with, and lowering bound the call to
// the imported free function outright — so a Headers value came back as a
// `Rodolfo.Definition` built out of the receiver and the argument, with no
// error anywhere. The walker dispatches on the value; BEAM has to as well.
// Guard on every nominal receiver type that provides this name at this
// arity and keep the imported function as the final arm.
auto Lowering::importedFreeFunctionDispatch(const ast::MethodCall& n,
                                  const std::string& method,
                                  const std::string& importedFunction,
                                  int arity,
                                  std::vector<ExprPtr>& args) -> ExprPtr {
    if (!externalModules || args.empty()) return nullptr;
    if (!expressionTypes || !n.receiver) return nullptr;
    // The receiver's NOMINAL identity, or "" when the checker has nothing
    // concrete for it. Only the type's name matters here — a provider is
    // keyed by the record, not by its type arguments — so this reads the
    // NamedType rather than rendering the whole type.
    std::string staticReceiver;
    if (auto found = expressionTypes->find(n.receiver.get());
        found != expressionTypes->end() && found->second)
        if (auto* named =
                std::get_if<semantic::NamedType>(&found->second->kind))
            if (named->name != "Any")
                staticReceiver = canonicalRecordName(named->name);
    // Only receiver shapes whose guards can safely be tested here.
    // The prelude's List.join/2 must beat an imported FS.Path.join/2
    // for a list held as Any, as it does in the merged dispatcher.
    struct Provider {
        std::string receiverType;   // as written, for the guard
        std::string moduleAtom;
        std::string function;
        // A `make` function of this unit, as opposed to a separately
        // compiled provider.
        bool local = false;
    };
    std::vector<Provider> providers;
    std::set<std::string> seen;
    auto consider = [&](const std::string& receiverType,
                        const std::string& moduleAtom,
                        const std::string& function, bool local = false) {
        if (receiverType.empty()) return;
        const auto canonical = canonicalRecordName(receiverType);
        if (!records.count(canonical) && !primGuardBifs().count(receiverType))
            return;
        if (!seen.insert(canonical).second) return;
        providers.push_back({receiverType, moduleAtom, function, local});
    };
    // A foul make function of this unit takes the capability context like any
    // other local call. Sent without it, a `founded(user, title, by)`
    // beside a same-named free function was called one argument short
    // of its definition — `undef` at run time (kexhq/kex#476).
    auto providerCall = [&](const Provider& provider,
                            std::vector<ExprPtr> callArgs) {
        if (provider.local && callNeedsContext(provider.function, arity))
            callArgs.push_back(currentContext());
        const int callArity = static_cast<int>(callArgs.size());
        return callE(provider.moduleAtom, provider.function, callArity,
                     std::move(callArgs));
    };
    // A module compiled in THIS unit (`using Net.HTTP` merges the stdlib
    // module in) is not in the external registry; its bare-name runtime
    // dispatcher is what the call would have reached without the import.
    if (auto owners = methodOwners.find(method); owners != methodOwners.end())
        if (localMethodArities.count(method + "/" + std::to_string(arity)))
            for (const auto& owner : owners->second) {
                auto declaring = localTypeModules.find(owner);
                if (declaring == localTypeModules.end()) continue;
                consider(owner, "Kex." + declaring->second, method, true);
            }
    if (auto candidates = externalModules->receiverFunctions.find(method);
        candidates != externalModules->receiverFunctions.end())
        for (const auto& candidate : candidates->second) {
            if (candidate.beamArity != arity) continue;
            if (candidate.beamFunction != method) continue;
            consider(candidate.receiverType, candidate.moduleAtom,
                     candidate.beamFunction);
        }
    if (providers.empty()) return nullptr;
    // The receiver's type is KNOWN and one of these providers owns it.
    // That settles the call outright: a receiver method answers for its
    // own type, and a bare function some `using` happens to have brought
    // into scope does not get to claim it. No guard is needed — and this
    // is the case an annotation was supposed to make safe but did not:
    // `response.headers.get(name)` with `headers` typed
    // `Net.HTTP.Headers` still lowered to `Kex.Dsl:get/2`.
    if (!staticReceiver.empty()) {
        for (const auto& provider : providers)
            if (canonicalRecordName(provider.receiverType) == staticReceiver)
                return providerCall(provider, std::move(args));
        // Known, and no provider owns it: the import is the right answer
        // (`100.meter` is units' `meter(100)`), so leave it alone.
        return nullptr;
    }
    // Every arm needs its own copy of the arguments; a non-atomic one
    // cannot be duplicated, and the direct call stays as it was.
    auto copyArgs = [&](const char* what) -> std::optional<std::vector<ExprPtr>> {
        std::vector<ExprPtr> out;
        for (const auto& arg : args) {
            try {
                out.push_back(clone(arg, what));
            } catch (const LowerError&) {
                return std::nullopt;
            }
        }
        return out;
    };
    auto subject = copyArgs("importedFreeFunctionDispatch subject");
    if (!subject) return nullptr;
    Match dispatch;
    dispatch.subjects.push_back(std::move((*subject)[0]));
    auto arm = [&](ExprPtr guard, ExprPtr body) {
        MatchClause clause;
        auto pattern = std::make_unique<Pattern>();
        pattern->kind = PatKind::Var;
        pattern->name = fresh("_provider");
        clause.patterns.push_back(std::move(pattern));
        if (guard) clause.guard = std::move(guard);
        clause.body = std::move(body);
        dispatch.clauses.push_back(std::move(clause));
    };
    for (const auto& provider : providers) {
        auto guardArgs = copyArgs("importedFreeFunctionDispatch guard");
        auto callArgs = copyArgs("importedFreeFunctionDispatch call");
        if (!guardArgs || !callArgs) return nullptr;
        arm(typeGuard(provider.receiverType, std::move((*guardArgs)[0])),
            providerCall(provider, std::move(*callArgs)));
    }
    arm(nullptr, localCallExpr(importedFunction, std::move(args)));
    auto out = std::make_unique<Expr>();
    out->node = std::move(dispatch);
    return out;
}

// Emit a `'field'/1` accessor for each record field, unless a real
// function/method of the same name exists (which shadows it), or the
// name matches an imported package-declared receiver function —
// otherwise the accessor would shadow the prelude's receiver function
// and dispatch `[1,2,3].rest` to a tuple-element read instead of
// `kex_prelude:rest/1`. When a field sits at the same position in
// every record that has it, one element/2 call suffices; otherwise
// dispatch on the record tag.
// Colliding with an IMPORTED receiver function used to suppress the field
// accessor outright, which left `box.rest` calling the prelude's `rest/1`
// — a dispatcher with no clause for a record, so it died with `if_clause`
// on BEAM while the walker read the field just fine. Instead the accessor
// is emitted and falls through to the import, so `box.rest` and
// `[1,2,3].rest` both work.
//
// Returns the imported function to fall through to, or nullptr when the
// name does not collide. Sets `blocked` when the collision cannot be
// forwarded (no 1-arity form), in which case no accessor is emitted and
// the name is left to the import — the old behavior. lowerProgram's
// localMethods seeding asks the same question, so both must agree on
// whether an accessor exists.
auto Lowering::fieldAccessorDelegate(const std::string& field, bool& blocked) const
    -> const ExternalModules::ReceiverFunction* {
    blocked = false;
    if (!externalModules) return nullptr;
    auto ext = externalModules->receiverFunctions.find(field);
    if (ext == externalModules->receiverFunctions.end()) return nullptr;
    for (const auto& candidate : ext->second)
        if (candidate.beamArity == 1) return &candidate;
    // Every import of this name takes more than a receiver, so there is
    // nothing to collide with: BEAM keys functions by name AND arity, and
    // a field read is always arity 1. `Kex.VERSION.patch` is the
    // case — `patch` is also `Http.patch/2,3`, and blocking the accessor
    // here left the field unreadable on BEAM while the walker read it
    // fine. Emitting `patch/1` cannot shadow `patch/2`.
    return nullptr;
}

auto Lowering::makeAccessors(const std::unordered_set<std::string>& definedFns,
                   const std::unordered_set<std::string>& definedFnArities)
    -> std::vector<FunDef> {
    std::vector<FunDef> out;
    for (const auto& [field, entries] : fieldAccessors) {
        // A local function of this name owns the symbol; its dispatcher
        // carries the record clauses instead (see appendFieldClauses).
        // Only an ARITY-1 one can: appendFieldClauses declines any other
        // arity, so suppressing on the name alone left the field with no
        // reader at all. `value` became exactly that the moment
        // OptionParser joined the prelude with `value/2,3` on
        // ParsedOptions — every `measure.value` then called a
        // `kex_prelude:value/1` that was never emitted, and died `undef`.
        if (definedFns.count(field) && definedFnArities.count(field + "/1"))
            continue;

        bool blocked = false;
        const auto* delegate = fieldAccessorDelegate(field, blocked);
        if (blocked) continue;

        FunDef def; def.name = field; def.arity = 1;
        def.isFieldAccessor = true;
        FunClause fc;
        auto rp = std::make_unique<Pattern>(); rp->kind = PatKind::Var; rp->name = "_rec";
        fc.params.push_back(std::move(rp));
        bool allSame = std::all_of(entries.begin(), entries.end(),
            [&](const auto& e){ return e.position == entries[0].position; });
        if (delegate) {
            // Match the record tags this field belongs to; anything else
            // is the imported function's receiver.
            Match m;
            m.subjects.push_back(var("_rec"));
            for (const auto& slot : entries) {
                MatchClause mc;
                auto gv = std::make_unique<Pattern>();
                gv->kind = PatKind::Var; gv->name = "_fv";
                mc.patterns.push_back(std::move(gv));
                mc.guard = recordGuard(slot, var("_fv"));
                mc.body = callE("erlang", "element", 2,
                                two(litInt(slot.position), var("_rec")));
                m.clauses.push_back(std::move(mc));
            }
            MatchClause fallback;
            auto wild = std::make_unique<Pattern>();
            wild->kind = PatKind::Var; wild->name = "_other";
            fallback.patterns.push_back(std::move(wild));
            fallback.body = callE(delegate->moduleAtom, delegate->beamFunction,
                                  1, one(var("_rec")));
            m.clauses.push_back(std::move(fallback));
            auto e = std::make_unique<Expr>(); e->node = std::move(m);
            fc.body = std::move(e);
        } else if (allSame) {
            fc.body = callE("erlang", "element", 2,
                            two(litInt(entries[0].position), var("_rec")));
        } else {
            // Guarded on tag AND size rather than a match on the
            // tag alone: two records can share a tag (a local one and the
            // prelude's) and only the size separates them.
            Match m;
            m.subjects.push_back(var("_rec"));
            for (const auto& slot : entries) {
                MatchClause mc;
                auto gv = std::make_unique<Pattern>();
                gv->kind = PatKind::Var; gv->name = "_fv";
                mc.patterns.push_back(std::move(gv));
                mc.guard = recordGuard(slot, var("_fv"));
                mc.body = callE("erlang", "element", 2,
                                two(litInt(slot.position), var("_rec")));
                m.clauses.push_back(std::move(mc));
            }
            auto e = std::make_unique<Expr>(); e->node = std::move(m);
            fc.body = std::move(e);
        }
        def.clauses.push_back(std::move(fc));
        out.push_back(std::move(def));
    }
    return out;
}

} // namespace kex::ir
