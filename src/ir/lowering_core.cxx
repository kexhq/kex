// core methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"
#include "../semantic/resolve_core.hxx"

namespace kex::ir {

// The inverse of `canonicalRecordName` for MANGLED SYMBOL spellings: given
// a type as the checker names it (qualified — `Srv.Server`), the spelling
// the declaring module's own `make`/annotation source used, which is what
// `mangleReceiverSignature` baked into the emitted function names. Returns
// the input unchanged unless the qualifier is demonstrably the module that
// declares that type here, so an unrelated `A.Server` is never confused
// with a local `Server`.
auto Lowering::moduleLocalTypeSpelling(const std::string& written) const
    -> std::string {
    std::string base = written;
    std::string applied;
    if (auto angle = base.find('<'); angle != std::string::npos) {
        applied = base.substr(angle);
        base.resize(angle);
    }
    const auto dot = base.rfind('.');
    if (dot == std::string::npos) return written;
    if (localTypeModules.count(base)) return written;
    const auto qualifier = base.substr(0, dot);
    const auto bare = base.substr(dot + 1);
    auto declaring = localTypeModules.find(bare);
    if (declaring != localTypeModules.end() &&
        declaring->second == qualifier)
        return bare + applied;
    auto declared = moduleDeclaredTypes.find(bare);
    if (declared != moduleDeclaredTypes.end() &&
        declared->second == qualifier)
        return bare + applied;
    // A record of a module compiled in this unit. Only when nothing local
    // already answers to the bare name — two modules may declare the same
    // record, and stripping then names the wrong one.
    if (records.count(base) && !records.count(bare)) return bare + applied;
    return written;
}

// The trait a name written in `scope` refers to — `Geo.Shape` for a
// `Shape` written inside `module Geo` — by the checker's own rules
// (kex::resolveTraitName), or "" when it is no trait. `lastResort` is the
// checker's too: an `implement:` list may name a trait exactly one module
// declares without importing it, a type annotation may not.
// `imports` stands in for the `using`s recorded so far when the caller
// knows better — the pre-pass runs before any are.
auto Lowering::traitIdentityOf(const std::string& written, const std::string& scope,
                     bool lastResort ,
                     const std::set<std::string>* imports ) const
    -> std::string {
    if (written.empty()) return {};
    const auto& visible = imports ? *imports : usingModules;
    const std::vector<std::string> imported(visible.begin(), visible.end());
    const auto known = [this](const std::string& name) {
        return traitMethods.count(name) > 0;
    };
    const auto eachKnown = [&](const auto& visit) {
        if (!lastResort) return;
        for (const auto& [name, _] : traitMethods) visit(name);
    };
    const auto resolved =
        kex::resolveTraitName(written, scope, imported, known, eachKnown);
    return known(resolved) ? resolved : std::string{};
}

auto Lowering::traitIdentityOf(const std::string& written) const -> std::string {
    return traitIdentityOf(written, currentModulePath);
}

// Resolve a source spelling to the canonical runtime record identity.
// Module-owned records are tagged `Module.Record`; top-level records keep
// their historical bare name. Prefer the innermost lexical module before
// a same-named top-level record.
auto Lowering::canonicalRecordName(const std::string& name) const -> std::string {
    if (auto scoped = kex::semantic::resolve_core::resolveInScopes(
            currentModulePath, name, [&](const std::string& candidate) {
                return records.count(candidate) > 0;
            }))
        return *scoped;
    // A non-record type the module being lowered declares itself — an
    // opaque `type Connection` — is that type. Carrying on would hand the
    // name to whichever other module declares a RECORD called
    // `Connection`, and the guard built from it then matched none of this
    // module's values: "Undefined method: send for Tuple" (kexhq/kex#476).
    if (kex::semantic::resolve_core::resolveInScopes(
            currentModulePath, name, [&](const std::string& candidate) {
                return declaredTypeIdentities.count(candidate) > 0;
            }))
        return name;
    if (resolvingMakeOwner && currentModulePath.empty())
        if (auto declaring = localTypeModules.find(name);
            declaring != localTypeModules.end() &&
            declaredTypeIdentities.count(declaring->second + "." + name) &&
            !records.count(declaring->second + "." + name))
            return name;
    // A record this unit declares at the top level. One that merely
    // arrived with the prelude does not outrank what a `using` names:
    // `using App.Reply, only: [Reply]` means that module's record, and
    // tagging the construction with the prelude's `Reply` left every
    // function of the imported one "Undefined method … for Reply"
    // (kexhq/kex#476).
    const bool external = externalRecordNames.count(name) > 0;
    if (records.count(name) && !external) return name;
    if (external && name.find('.') == std::string::npos) {
        for (const auto& imported : usingModules)
            if (records.count(imported + "." + name))
                return imported + "." + name;
        // A dispatcher's guard is built outside any module, for the type
        // a `make` block of this unit targets.
        if (resolvingMakeOwner)
            if (auto declaring = localTypeModules.find(name);
                declaring != localTypeModules.end() &&
                records.count(declaring->second + "." + name))
                return declaring->second + "." + name;
        return name;
    }
    // The module whose `make` block named this type. A dispatcher is
    // generated after the module walk, so `currentModulePath` no longer
    // says where the name was written, and the suffix match below gives up
    // when two modules share a last segment — `Mock.Response` and
    // `Web.Response` do, which left the guard testing a bare `Response`
    // tag that no value carries (kexhq/kex#143).
    if (auto declaring = localTypeModules.find(name);
        declaring != localTypeModules.end()) {
        const auto candidate = declaring->second + "." + name;
        if (records.count(candidate)) return candidate;
    }
    // A qualified identity the record table does not know under that
    // spelling (a reified value carries the interpreter's `Boxes.Box`)
    // still names the record declared as its last segment.
    if (const auto dot = name.rfind('.'); dot != std::string::npos) {
        // A PARTIALLY qualified spelling first: `using Net.Socket`
        // makes `TCP.Endpoint` legal for the record whose identity —
        // and BEAM tag — is `Net.Socket.TCP.Endpoint`. Without this the
        // construction was tagged 'TCP.Endpoint', which the runtime's
        // own pattern never matches: `Net.HTTP.Server.start` answered
        // "invalid HTTP server configuration" for a perfectly good
        // endpoint. Same rule as the bare-name case below — a `using`
        // names the owner outright, and a suffix match settles the rest
        // only when it is unambiguous.
        for (const auto& imported : usingModules)
            if (records.count(imported + "." + name))
                return imported + "." + name;
        {
            const auto match = kex::semantic::resolve_core::matchSuffix(
                records, "." + name);
            if (match.kind ==
                kex::semantic::resolve_core::SuffixMatchKind::unique)
                return match.name;
        }
        // A leading PORTION of the qualifier is a KNOWN ALIAS: either a
        // `using Geometry, as: Geo` single-segment alias, or a
        // multi-segment one an `export Geometry` inside `module App`
        // registers for its whole re-namespaced prefix (`App.Geometry`,
        // still meaning `Geometry`). Longest prefix first, so a re-export
        // nested inside an aliased scope resolves the more specific
        // mapping. The record's real identity is registered under the
        // alias's TARGET, not the written qualifier — construction
        // through either otherwise tagged the tuple with the qualifier
        // ITSELF ('Geo.Circle', 'App.Geometry.Circle'), which no
        // is_record/tag guard anywhere recognizes — the record table
        // only ever holds canonical module names — so a value built
        // this way looked like a bare, untagged tuple to every
        // method/pattern that later inspected it (kexhq/kex#alias).
        for (size_t prefixEnd = dot; prefixEnd != std::string::npos;
             prefixEnd = prefixEnd == 0
                 ? std::string::npos
                 : name.rfind('.', prefixEnd - 1)) {
            auto alias = moduleAliases.find(name.substr(0, prefixEnd));
            if (alias == moduleAliases.end()) continue;
            const auto aliased = alias->second + name.substr(prefixEnd);
            if (records.count(aliased)) return aliased;
        }
        if (const auto bare = name.substr(dot + 1); records.count(bare))
            return bare;
        return name;
    }
    // A bare name written against an IMPORTED record: `Input` inside JSON
    // is Parsing's record, and it must be tagged with the same qualified
    // identity Parsing itself constructs, or the two modules disagree
    // about the tuple's tag. A `using` names the owner outright; failing
    // that, a suffix match settles it only when it is unambiguous —
    // ambiguity is exactly the case that needs qualifying at the source.
    for (const auto& imported : usingModules)
        if (records.count(imported + "." + name))
            return imported + "." + name;
    const auto match =
        kex::semantic::resolve_core::matchSuffix(records, "." + name);
    if (match.kind == kex::semantic::resolve_core::SuffixMatchKind::unique)
        return match.name;
    return name;
}

auto Lowering::prefixSymbol(const std::string& name) const -> std::string {
    auto found = freeFunctionSymbols.find(name);
    return found == freeFunctionSymbols.end() ? name : found->second;
}

auto Lowering::prefixCall(const std::string& name, std::vector<ExprPtr> args) -> Call {
    auto call = localCall(name, std::move(args));
    call.name = prefixSymbol(name);
    return call;
}

// Threading is per-unit for now: a unit that declares no capability emits
// exactly what it did before. Going unconditional needs imported foulness
// at the call site first — a consumer cannot know that the prelude's
// `Task.start` became start/2 without it — see kexhq/kex#181.
auto Lowering::threadsCapabilities() const -> bool { return true; }

// `Mod.Tag` for an ADT declared inside a module (e.g. `Kex.FS`). A tag is
// just an atom at runtime, so the qualifier has no representation here —
// resolve to the bare tag. `variantOwner` covers tags declared outside
// this module (the prelude's, via externalVariants) as well as its own,
// so an unknown name still falls through to the usual error.
auto Lowering::qualifiedVariantTag(const std::string& name) const -> std::string {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return "";
    auto tag = name.substr(dot + 1);
    if (tag.empty() || !std::isupper(static_cast<unsigned char>(tag[0])))
        return "";
    return (variantTagSet.count(tag) || variantOwner.count(tag)) ? tag : "";
}

// A semantic::StructuredType as the `Type { name, args }` record it
// stands for — a tagged tuple, the same shape a record literal lowers to.
auto Lowering::structuredTypeLiteral(const semantic::StructuredType& type) -> ExprPtr {
    std::vector<ExprPtr> args;
    for (const auto& arg : type.args) args.push_back(structuredTypeLiteral(arg));
    auto list = std::make_unique<Expr>();
    list->node = MakeList{std::move(args), std::nullopt};
    auto record = std::make_unique<Expr>();
    std::vector<ExprPtr> fields;
    fields.push_back(lit(LitKind::String, type.name));
    fields.push_back(std::move(list));
    fields.push_back(litBool(type.pure));
    record->node = Construct{"Type", std::move(fields)};
    return record;
}

// An erlang:error carrying the walker's exact runtime-error text,
// prefixed with the current source location.
auto Lowering::runtimeError(const std::string& msg) -> ExprPtr {
    std::string loc;
    if (currentLoc) loc = std::string(currentLoc->file) + ":"
        + std::to_string(currentLoc->line) + ":"
        + std::to_string(currentLoc->column) + ": ";
    return callE("erlang", "error", 1, one(lit(LitKind::String,
        loc + "runtime error: " + msg)));
}

// The walker's failure for a `match` no clause covers: the same message,
// with the subject rendered the same way (`inspect`). Built at runtime
// because the value is only known then.
//
// Core Erlang needs this clause for a second reason: an inexhaustive
// `case` is not merely a runtime risk there, it makes erlc's SSA pass
// fail outright ("internal error in pass core_to_ssa"), so a program with
// one unhandled variant did not compile at all where the walker ran it
// and reported the unmatched value.
auto Lowering::noClauseMatched(ExprPtr subject) -> ExprPtr {
    std::string loc;
    if (currentLoc) loc = std::string(currentLoc->file) + ":"
        + std::to_string(currentLoc->line) + ":"
        + std::to_string(currentLoc->column) + ": ";
    std::vector<ExprPtr> parts;
    parts.push_back(lit(LitKind::String,
                        loc + "runtime error: no matching clause for "));
    parts.push_back(callE("kex_io", "inspect_plain", 1,
                          one(std::move(subject))));
    auto list = std::make_unique<Expr>();
    list->node = MakeList{std::move(parts), std::nullopt};
    return callE("erlang", "error", 1,
                 one(callE("erlang", "iolist_to_binary", 1,
                           one(std::move(list)))));
}

// One capability-substitution dispatch, shared by every path that can
// reach a capability. Enumerating sites is how prelude capabilities
// stayed broken — a missed one compiles and silently calls the real
// implementation, which is the failure this feature exists to remove
// (kexhq/kex#143). Callers supply only how to build their own direct
// call for the miss case.
auto Lowering::capabilityDispatch(
    const std::string& capability, const std::string& method,
    const std::vector<ast::ExprPtr>& sourceArgs,
    const std::function<ExprPtr(std::vector<ExprPtr>)>& directCall)
    -> ExprPtr {
    std::vector<Binding> binds;
    std::vector<ExprPtr> lowered;
    for (const auto& a : sourceArgs) lowered.push_back(atomize(a, binds));
    return capabilityDispatchLowered(capability, method, std::move(lowered),
                                     binds, directCall);
}

// The same dispatch where the arguments are already lowered — the
// qualified-call path builds its argument slots before it knows the
// target is a capability.
auto Lowering::capabilityDispatchLowered(
    const std::string& capability, const std::string& method,
    std::vector<ExprPtr> loweredArgs, std::vector<Binding>& binds,
    const std::function<ExprPtr(std::vector<ExprPtr>)>& directCall)
    -> ExprPtr {
    std::vector<std::string> argVars;
    for (auto& a : loweredArgs) {
        auto name = fresh("CapArg");
        binds.push_back({name, std::move(a)});
        argVars.push_back(name);
    }
    auto varsOf = [&]() {
        std::vector<ExprPtr> out;
        for (const auto& v : argVars) out.push_back(var(v));
        return out;
    };
    std::vector<ExprPtr> getArgs;
    getArgs.push_back(lit(LitKind::String, capability));
    getArgs.push_back(currentContext());
    getArgs.push_back(lit(LitKind::Atom, "undefined"));
    Match m;
    m.subjects.push_back(callE("maps", "get", 3, std::move(getArgs)));

    MatchClause missing;
    auto undef = std::make_unique<Pattern>();
    undef->kind = PatKind::Lit;
    undef->litKind = LitKind::Atom;
    undef->litText = "undefined";
    missing.patterns.push_back(std::move(undef));
    missing.body = directCall(varsOf());
    m.clauses.push_back(std::move(missing));

    MatchClause replaced;
    auto impl = fresh("Impl");
    auto bound = std::make_unique<Pattern>();
    bound->kind = PatKind::Var;
    bound->name = impl;
    replaced.patterns.push_back(std::move(bound));
    auto passed = varsOf();
    // A capability's members are foul, so the implementation takes the
    // context too — the runtime dispatch hands it over as a direct call
    // would.
    passed.push_back(currentContext());
    auto passedList = std::make_unique<Expr>();
    passedList->node = MakeList{std::move(passed), std::nullopt};
    std::vector<ExprPtr> dispatchArgs;
    dispatchArgs.push_back(var(impl));
    dispatchArgs.push_back(lit(LitKind::String, method));
    dispatchArgs.push_back(std::move(passedList));
    replaced.body =
        callE("kex_io", "dispatch_method", 3, std::move(dispatchArgs));
    m.clauses.push_back(std::move(replaced));

    auto chosen = std::make_unique<Expr>();
    chosen->node = std::move(m);
    return wrapLets(binds, std::move(chosen));
}

// Is this name a capability, declared here or imported?
auto Lowering::isCapabilityModule(const std::string& name) const -> bool {
    return capabilityModules.count(name) > 0 ||
           (externalModules &&
            externalModules->capabilityModules.count(name) > 0);
}

// Is `method` part of `capability`'s substitutable INTERFACE? Only its
// `foul` members are — a pure member performs no effect, so there is
// nothing to replace, and routing it through the substitution dispatch
// actively breaks it: that dispatch passes the source arguments only, so
// `IO.inspect(v)` lost the trait dictionary its `Inspectable` parameter
// needs and rendered structurally instead of calling the user's
// `inspectValue` (kexhq/kex#143).
auto Lowering::capabilityInterfaceHas(const std::string& capability,
                            const std::string& method) const -> bool {
    // `foulExports` records exactly the foul exports, one entry per
    // overload as `Module.member/arity` — which is the interface.
    if (externalModules &&
        externalModules->capabilityModules.count(capability)) {
        // Its own members, where its interface lists them: a foul helper
        // of the module beside it (`Vault.stored`) is a plain call.
        if (auto members = externalModules->capabilityMembers.find(capability);
            members != externalModules->capabilityMembers.end())
            return members->second.count(method) > 0;
        const auto prefix = capability + "." + method + "/";
        return std::any_of(externalModules->foulExports.begin(),
                           externalModules->foulExports.end(),
                           [&](const std::string& entry) {
                               return entry.rfind(prefix, 0) == 0;
                           });
    }
    // Declared in this unit: its own foul members. A module of the same
    // name may add helpers beside them (`capability Storage` next to
    // `module Storage`), and those are plain calls even while the
    // capability is replaced — routed to the replacement, they were
    // `undef` (the replacement does not define them).
    if (capabilityModules.count(capability)) {
        if (auto members = capabilityMembers.find(capability);
            members != capabilityMembers.end())
            return members->second.count(method) > 0;
        return foulFns.count(method) > 0;
    }
    return true;
}

// The context to hand a callee: whatever this function holds, or an empty
// map where there is none (a pure caller, or top level).
auto Lowering::currentContext() -> ExprPtr {
    if (!currentContextVar.empty()) return var(currentContextVar);
    return callE("maps", "new", 0, {});
}

// Does a call to this name, at this arity, need the context appended?
//
// The arity matters because `foulFns` is keyed by NAME: a bare field read
// lowers to the synthesized `name/1` accessor (makeAccessors), which is
// not foul and takes no context — yet a foul function of the same name at
// ANOTHER arity made the call go out as `patch/2` while only `patch/1`
// existed, and erlc refused the module. `Mock`'s `foul patch(url, body)`
// against a record's `patch` field is exactly that pair (kexhq/kex#191).
// Only the synthesized accessor is exempted (`accessorFields`): where a
// real `name/1` is defined, no accessor is emitted, and if that function
// is foul the call still takes its context.
auto Lowering::callNeedsContext(const std::string& name, int arity) const -> bool {
    if (!threadsCapabilities() || foulFns.count(name) == 0) return false;
    if (arity == 1 && accessorFields.count(name) &&
        !moduleReceiverNames.count(name))
        return false;
    const auto key = name + "/" + std::to_string(arity);
    if (topLevelFoulArities.count(key)) return true;
    if (pureFnArities.count(key)) return false;
    return true;
}

// Every local call goes through here so the context is appended in one
// place. Missing a call site shows up as `undef` at runtime rather than a
// compile error, so they must not be patched individually.
// A call into another compiled unit. An imported foul function takes the
// capability context exactly as a local one does, and the consumer only
// learns that from the interface — so every external call goes through
// here (kexhq/kex#181).
//
// A bare name `using` imported from a separately compiled module that is a
// constant there (`using Statuses, only: [STATUSES]`): read as the call it
// is, as the same import from this unit is. Null for anything else.
auto Lowering::externalConstant(const std::string& name) -> ExprPtr {
    if (!externalModules || subst.count(name) || moduleImports.count(name))
        return nullptr;
    const auto imported = externalImports.find(name);
    if (imported == externalImports.end()) return nullptr;
    const auto& qualKey = imported->second;
    const auto arity = externalModules->exportArity.find(qualKey);
    const auto function = externalModules->exportToBeamFn.find(qualKey);
    const auto atom = externalModules->nameToAtom.find(
        qualKey.substr(0, qualKey.rfind('.')));
    if (arity == externalModules->exportArity.end() || arity->second != 0 ||
        function == externalModules->exportToBeamFn.end() ||
        atom == externalModules->nameToAtom.end())
        return nullptr;
    return externalCallExpr(qualKey, atom->second, function->second, 0, {});
}

auto Lowering::externalCallExpr(const std::string& qualKey, const std::string& atom,
                      const std::string& beamFunction, int arity,
                      std::vector<ExprPtr> args) -> ExprPtr {
    // The context is NOT appended here yet. Doing so is only correct once
    // the unit being called also threads — the prelude declares no
    // capability today, so its foul functions are still at their original
    // arity and appending here made every capability program fail with
    // `undef`. This append and dropping the gate have to land together
    // (kexhq/kex#181). `foulExports` carries the information the append
    // will need.
    (void)qualKey;
    return callE(atom, beamFunction, arity, std::move(args));
}

auto Lowering::localCallExpr(const std::string& name, std::vector<ExprPtr> args)
    -> ExprPtr {
    auto e = std::make_unique<Expr>();
    e->node = localCall(name, std::move(args));
    return e;
}

// Does `typeName`'s make block define `method` in this compilation unit?
auto Lowering::ownsMethod(const std::string& typeName,
                const std::string& method) const -> bool {
    auto owners = methodOwners.find(method);
    if (owners == methodOwners.end()) return false;
    return std::find(owners->second.begin(), owners->second.end(),
                     typeName) != owners->second.end();
}

auto Lowering::localCall(const std::string& name, std::vector<ExprPtr> args)
    -> Call {
    if (callNeedsContext(name, static_cast<int>(args.size())))
        args.push_back(currentContext());
    const int arity = static_cast<int>(args.size());
    return Call{"", name, arity, std::move(args), false};
}

// "Undefined method: m for T" with the receiver's runtime type, matching
// the walker exactly. The type has to be read at runtime (the reason this
// call is unresolved is that lowering does not know it), so the message is
// built as an iolist the same way the receiver dispatcher builds its own.
// `hint` follows the receiver's type name in the message.
auto Lowering::undefinedMethod(const std::string& method, ExprPtr receiver,
                     const std::string& hint ) -> ExprPtr {
    std::string loc;
    if (currentLoc) loc = std::string(currentLoc->file) + ":"
        + std::to_string(currentLoc->line) + ":"
        + std::to_string(currentLoc->column) + ": ";
    std::vector<ExprPtr> parts;
    parts.push_back(lit(LitKind::String,
                        loc + "runtime error: Undefined method: " +
                        method + " for "));
    parts.push_back(callE("kex_io", "value_type_name", 1,
                          one(std::move(receiver))));
    if (!hint.empty()) parts.push_back(lit(LitKind::String, hint));
    auto list = std::make_unique<Expr>();
    list->node = MakeList{std::move(parts), std::nullopt};
    return callE("erlang", "error", 1,
                 one(callE("erlang", "iolist_to_binary", 1,
                           one(std::move(list)))));
}

// Same message, but a record field holding a function gets to answer
// first: `d.fetch(url)` should call it when nothing else claims the name.
// The method keeps precedence — reaching here means nothing did. The
// walker's dispatch fallback does the same (kexhq/kex#176). The field is
// read by name at runtime because the reason this call is unresolved is
// that lowering does not know the receiver's type.
auto Lowering::undefinedMethodOrField(const ast::MethodCall& n, ExprPtr receiver)
    -> ExprPtr {
    std::string loc;
    if (currentLoc) loc = std::string(currentLoc->file) + ":"
        + std::to_string(currentLoc->line) + ":"
        + std::to_string(currentLoc->column) + ": ";
    std::vector<Binding> binds;
    std::vector<ExprPtr> callArgs;
    for (const auto& a : n.args) callArgs.push_back(atomize(a, binds));
    for (const auto& [_, value] : n.namedArgs)
        callArgs.push_back(atomize(value, binds));
    if (n.block) callArgs.push_back(atomize(*n.block, binds));
    auto argList = std::make_unique<Expr>();
    argList->node = MakeList{std::move(callArgs), std::nullopt};
    std::vector<ExprPtr> args;
    args.push_back(std::move(receiver));
    args.push_back(lit(LitKind::String, n.method));
    args.push_back(std::move(argList));
    args.push_back(lit(LitKind::String, loc));
    return wrapLets(binds, callE("kex_io", "call_field_or_fail", 4,
                                 std::move(args)));
}

// A named argument no clause of the target declares. Dropping it would run
// the call as if the label had never been written and hand back a
// plausible-looking answer to a question nobody asked, so both backends
// report it — as a RUNTIME error, so a program that never reaches the line
// still compiles. When another module does declare that label, name it:
// that is nearly always a missing `using`. Mirrors the walker's
// Evaluator::callFunction message exactly.
auto Lowering::unknownNamedArgument(const std::string& qualified,
                          const std::string& label) -> ExprPtr {
    auto message = "unknown named argument `" + label + ":` for `" +
                   sourceName(qualified) + "`";
    const auto plain = kex::mangle::memberOf(qualified);
    // A module named `Mod.fn` whose `fn` declares this label. Three places
    // hold that, depending on where the module came from: compiled in this
    // same unit (`Units.SI` is, since the program names it), imported as an
    // ordinary export, or registered as a provider's receiver function.
    const auto owningModule =
        [&](const std::string& qualifiedName,
            const auto& params) -> std::string {
        auto dot = qualifiedName.rfind('.');
        if (dot == std::string::npos ||
            qualifiedName.substr(dot + 1) != plain)
            return {};
        if (std::find(params.begin(), params.end(), label) == params.end())
            return {};
        return qualifiedName.substr(0, dot);
    };
    std::string module;
    for (const auto& [qualifiedLocal, params] : fnParamNames) {
        module = owningModule(qualifiedLocal, params);
        if (!module.empty()) break;
    }
    if (externalModules) {
        if (module.empty())
            for (const auto& [qualifiedExport, params] :
                 externalModules->exportParamNames) {
                module = owningModule(qualifiedExport, params);
                if (!module.empty()) break;
            }
        if (module.empty())
            if (auto found = externalModules->receiverFunctions.find(plain);
                found != externalModules->receiverFunctions.end())
                for (const auto& candidate : found->second) {
                    if (std::find(candidate.paramNames.begin(),
                                  candidate.paramNames.end(),
                                  label) == candidate.paramNames.end())
                        continue;
                    for (const auto& [name, atom] :
                         externalModules->nameToAtom)
                        if (atom == candidate.moduleAtom) {
                            module = name;
                            break;
                        }
                    if (!module.empty()) break;
                }
    }
    if (!module.empty())
        message += " — `" + module + "` defines a matching `" + plain +
                   "`; add `using " + module + "`";
    return runtimeError(message);
}

auto Lowering::opSymbol(TokenType t) -> std::string {
    switch (t) {
        case TokenType::Plus: return "+";     case TokenType::Minus: return "-";
        case TokenType::Star: return "*";     case TokenType::Slash: return "/";
        case TokenType::Percent: return "%";  case TokenType::Caret: return "^";
        case TokenType::EqEq: return "==";
        case TokenType::NotEq: return "!=";   case TokenType::LessThan: return "<";
        case TokenType::GreaterThan: return ">"; case TokenType::LessEq: return "<=";
        case TokenType::GreaterEq: return ">="; default: return "";
    }
}

auto Lowering::simpleTypeName(const ast::TypeExprPtr& t) -> std::string {
    if (!t) return "";
    if (auto* tn = std::get_if<ast::TypeName>(&t->kind))
        if (!tn->parts.empty()) return tn->parts.back();
    if (auto* g = std::get_if<ast::GenericType>(&t->kind))
        if (!g->name.parts.empty()) return g->name.parts.back();
    // List/Map types get canonical names so list-vs-map methods (the
    // polymorphic HOFs) collide and dispatch by runtime type (is_list/
    // is_map) instead of falling into separate un-mangled `map/2`s.
    if (std::holds_alternative<ast::ListType>(t->kind)) return "List";
    if (std::holds_alternative<ast::MapType>(t->kind)) return "Map";
    return "";
}

// Every dispatch name a `make` target covers — one entry normally, one per
// member for a union target (`make Float | Integer`). Always non-empty so
// callers can loop unconditionally; an unnameable target yields "", which
// is what simpleTypeName returned for it before.
auto Lowering::makeTargetNameList(const ast::TypeExprPtr& t,
                               const std::string& module ,
                               const std::set<std::string>& records )
    -> std::vector<std::string> {
    auto names = kex::makeTargetNames(t, module, records);
    if (names.empty()) names.emplace_back("");
    return names;
}

auto Lowering::fresh(const std::string& hint ) -> std::string {
    return "_ir_" + hint + std::to_string(counter++);
}

auto Lowering::currentName(const std::string& kexName) -> std::string {
    auto it = subst.find(kexName);
    return it != subst.end() ? it->second : kexName;
}

auto Lowering::snap(const ExprPtr& e) -> AtomicRef {
    if (e) {
        if (auto* v = std::get_if<Var>(&e->node)) return {v->name, std::nullopt};
        if (auto* l = std::get_if<Lit>(&e->node)) return {"", *l};
    }
    return {"_snap_null", std::nullopt};
}

auto Lowering::clone(const ExprPtr& e, const char* site ) -> ExprPtr {
    if (!e) throw LowerError(std::string("IR lower: clone of null expr [") + site + "]");
    if (auto* v = std::get_if<Var>(&e->node)) return var(v->name);
    if (auto* l = std::get_if<Lit>(&e->node)) {
        auto out = std::make_unique<Expr>(); out->node = *l; return out;
    }
    if (auto* c = std::get_if<Construct>(&e->node)) {
        if (c->args.empty()) {
            auto out = std::make_unique<Expr>();
            out->node = Construct{c->tag, {}};
            return out;
        }
    }
    throw LowerError(std::string("IR lower: clone of non-atomic expr (index ")
        + std::to_string(e->node.index()) + ") [" + site + "]");
}

// Wrap `body` in the accumulated let-bindings, evaluated left-to-right
// (so binds[0] is the outermost let).
auto Lowering::wrapLets(std::vector<Binding>& binds, ExprPtr body) -> ExprPtr {
    for (auto it = binds.rbegin(); it != binds.rend(); ++it) {
        auto let = std::make_unique<Expr>();
        let->node = Let{std::move(it->name), std::move(it->value), std::move(body)};
        body = std::move(let);
    }
    return body;
}

auto Lowering::isAtomic(const Expr& e) -> bool {
    return std::holds_alternative<Lit>(e.node) || std::holds_alternative<Var>(e.node);
}

// Lower an AST expr and, if the result is compound, bind it to a fresh
// Let (recorded in `binds`) returning an atomic Var. Literals/vars pass
// through untouched. This is the ANF normalization step.
auto Lowering::atomize(const ast::ExprPtr& e, std::vector<Binding>& binds) -> ExprPtr {
    auto ir = lower(e);
    if (isAtomic(*ir)) return ir;
    auto name = fresh();
    binds.push_back({name, std::move(ir)});
    return var(name);
}

auto Lowering::opOf(TokenType t) -> Op {
    switch (t) {
        case TokenType::Plus: return Op::Add;
        case TokenType::Minus: return Op::Sub;
        case TokenType::Star: return Op::Mul;
        case TokenType::Slash: return Op::Div;
        case TokenType::Percent: return Op::Mod;
        case TokenType::Caret: return Op::Pow;
        case TokenType::EqEq: return Op::Eq;
        case TokenType::NotEq: return Op::Neq;
        case TokenType::LessThan: return Op::Lt;
        case TokenType::GreaterThan: return Op::Gt;
        case TokenType::LessEq: return Op::Lte;
        case TokenType::GreaterEq: return Op::Gte;
        case TokenType::AmpAmp: return Op::And;
        case TokenType::PipePipe: return Op::Or;
        default:
            throw LowerError("IR lower: unsupported binary operator");
    }
}

auto Lowering::isOrderingOp(Op op) -> bool {
    return op == Op::Lt || op == Op::Gt || op == Op::Lte || op == Op::Gte;
}

auto Lowering::orderingFunction(Op op) -> std::string {
    switch (op) {
        case Op::Lt:  return "lt";
        case Op::Gt:  return "gt";
        case Op::Lte: return "lte";
        default:      return "gte";
    }
}

// Is this expression statically an Integer? Only then may an arithmetic
// operator stay on the raw BIF — see the `-`/`*` routing in lowerBinary.
auto Lowering::isIntegerOperand(const ast::ExprPtr& e) const -> bool {
    if (!e) return false;
    if (std::holds_alternative<ast::IntLiteral>(e->kind)) return true;
    if (auto* id = std::get_if<ast::Identifier>(&e->kind))
        if (auto declared = declaredParamTypes.find(id->name);
            declared != declaredParamTypes.end())
            return declared->second == "Integer";
    if (!expressionTypes) return false;
    auto found = expressionTypes->find(e.get());
    if (found == expressionTypes->end() || !found->second) return false;
    return semantic::typeToString(found->second) == "Integer";
}

// Which of the walker's three comparable families this expression is
// known to belong to ("num" / "str" / "char"), or "" when lowering cannot
// tell. Only used to keep a provably well-typed comparison on the BIF.
auto Lowering::comparableKind(const ast::ExprPtr& e) const -> std::string {
    if (!e) return {};
    if (std::holds_alternative<ast::IntLiteral>(e->kind) ||
        std::holds_alternative<ast::FloatLiteral>(e->kind))
        return "num";
    if (std::holds_alternative<ast::StringLiteral>(e->kind)) return "str";
    if (std::holds_alternative<ast::CharLiteral>(e->kind)) return "char";
    if (!expressionTypes) return {};
    auto found = expressionTypes->find(e.get());
    if (found == expressionTypes->end() || !found->second) return {};
    const auto name = semantic::typeToString(found->second);
    if (name == "Integer" || name == "Float" || name == "Number")
        return "num";
    if (name == "String") return "str";
    if (name == "Char") return "char";
    return {};
}

// A type for which Kex `==` is exact Erlang identity: Integer, String and
// Char. Float is deliberately absent — `-0.0` and NaN make `=:=` disagree
// with Kex's numeric equality. Empty when the type is not pinned down.
auto Lowering::exactEqualityType(const ast::ExprPtr& e) const -> std::string {
    if (!e) return {};
    if (std::holds_alternative<ast::IntLiteral>(e->kind)) return "Integer";
    if (std::holds_alternative<ast::StringLiteral>(e->kind)) return "String";
    if (std::holds_alternative<ast::CharLiteral>(e->kind)) return "Char";
    if (auto* id = std::get_if<ast::Identifier>(&e->kind))
        if (auto declared = declaredParamTypes.find(id->name);
            declared != declaredParamTypes.end() &&
            (declared->second == "Integer" || declared->second == "String" ||
             declared->second == "Char"))
            return declared->second;
    if (!expressionTypes) return {};
    auto found = expressionTypes->find(e.get());
    if (found == expressionTypes->end() || !found->second) return {};
    const auto name = semantic::typeToString(found->second);
    if (name == "Integer" || name == "String" || name == "Char")
        return name;
    return {};
}

// The emitted symbol for a parameterless member of the module being
// lowered (or of one enclosing it), or "" when the name is not one.
auto Lowering::moduleConstantSymbol(const std::string& name) const -> std::string {
    for (auto scope = currentModulePath; !scope.empty();) {
        const auto qualified = scope + "." + name;
        if (moduleZeroArgFns.count(qualified))
            if (auto emitted = moduleFunctions.find(qualified);
                emitted != moduleFunctions.end())
                return emitted->second == currentFunctionSymbol
                    ? std::string{} : emitted->second;
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) break;
        scope.resize(dot);
    }
    return {};
}

// A field default, lowered in its declaring module rather than in the one
// doing the constructing.
auto Lowering::lowerFieldDefault(const RecordInfo& info, const ast::ExprPtr& e)
    -> ExprPtr {
    if (info.owner.empty() || info.owner == currentModulePath)
        return lower(e);
    auto savedPath = currentModulePath;
    auto savedImports = moduleImports;
    auto savedAliases = moduleAliases;
    currentModulePath = info.owner;
    if (!info.imports.empty()) moduleImports = info.imports;
    if (!info.aliases.empty()) moduleAliases = info.aliases;
    // KexI carries an external record's default source, but not the
    // declaring file's `using` clauses. Resolve a short module name in
    // that source when exactly one loaded module has it (`Headers.empty`
    // in Metro.WebRequest's default is Net.HTTP.Headers.empty).
    if (info.imports.empty() && info.aliases.empty() && externalModules) {
        std::unordered_map<std::string, std::string> shortModules;
        std::unordered_set<std::string> ambiguous;
        for (const auto& [name, _] : externalModules->nameToAtom) {
            const auto dot = name.rfind('.');
            const auto shortName = name.substr(dot == std::string::npos ? 0 : dot + 1);
            auto [it, added] = shortModules.try_emplace(shortName, name);
            if (!added && it->second != name) ambiguous.insert(shortName);
        }
        for (const auto& [shortName, name] : shortModules)
            if (!ambiguous.count(shortName))
                moduleAliases.try_emplace(shortName, name);
    }
    auto result = lower(e);
    currentModulePath = std::move(savedPath);
    moduleImports = std::move(savedImports);
    moduleAliases = std::move(savedAliases);
    return result;
}

} // namespace kex::ir
