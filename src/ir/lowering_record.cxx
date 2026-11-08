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
    localRecordNames.insert(name);
    info.owner = owner;
    for (int i = 0; i < static_cast<int>(rec.fields.size()); i++) {
        info.fields.push_back(rec.fields[i].name);
        info.defaults.push_back(rec.fields[i].defaultValue ? &*rec.fields[i].defaultValue
                                                           : nullptr);
        addFieldAccessor(rec.fields[i].name, name, i + 2,
                         static_cast<int>(rec.fields.size()) + 1, true);
    }
    // The same name from an interface, laid out differently: a different
    // record (a local `ParseError` beside the prelude's), which a tag alone
    // does not tell apart.
    if (const auto seen = records.find(name);
        seen != records.end() && externalRecordNames.count(name) &&
        seen->second.fields != info.fields)
        conflictingRecordLayouts.insert(name);
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
// arity.
auto Lowering::externalPrefixCall(const std::string& name, int arity) const
    -> std::string {
    if (!externalModules) return {};
    auto candidates = externalModules->receiverFunctions.find(name);
    if (candidates == externalModules->receiverFunctions.end()) return {};
    for (const auto& candidate : candidates->second)
        if (candidate.beamArity == arity && candidate.beamFunction == name)
            return candidate.moduleAtom;
    return {};
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
    // The fallback takes any receiver no field arm matched, so it is a
    // dispatcher: a generic provider, else one whose function is the plain
    // name. A per-type implementation answers only its type: `Units.SI`'s
    // `kind/SIPrefix` beside the prelude's `kind` died `function_clause` on a
    // `UnitDefinition`.
    for (const auto& candidate : ext->second)
        if (candidate.beamArity == 1 && candidate.receiverType.empty()) return &candidate;
    for (const auto& candidate : ext->second)
        if (candidate.beamArity == 1 && candidate.beamFunction == field) return &candidate;
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


// The module whose function a namespaced call `Path.f(...)` reaches, as the
// checker named it: the selected signature's scope, the scope of the name a
// run-time dispatch or a value call is decided on, or the module of the
// type a constructor builds. A re-export (`Toolkit.Numbers.double`) is the
// function itself. "" when the checker named none.
auto Lowering::checkedCallModule(const ast::MethodCall& n) const -> std::string {
    const auto& target = hir->targetOf(n);
    switch (target.kind) {
    case hir::TargetKind::Selected:
    case hir::TargetKind::Imported:
        if (const auto* signature = hir->selectedSignatureOf(n))
            return kex::mangle::scopeOf(signature->name);
        return {};
    case hir::TargetKind::Dispatch:
    case hir::TargetKind::ValueCall:
        return kex::mangle::scopeOf(target.owner);
    case hir::TargetKind::Constructor: {
        const auto dot = target.owner.rfind('.');
        return dot == std::string::npos ? std::string{} : target.owner.substr(0, dot);
    }
    default:
        return {};
    }
}

} // namespace kex::ir

namespace kex::ir {

// Each typed provider for a value of its type, a module's own dispatcher for
// a value that module owns, and the target for every other value: the
// choice among several modules' providers the checker listed, made by the
// value at run time.
auto Lowering::providerChoice(const ast::MethodCall& n,
                              const semantic::ResolvedCallTarget& target) -> ExprPtr {
    std::vector<Binding> binds;
    std::vector<ExprPtr> args;
    args.push_back(atomize(n.receiver, binds));
    for (const auto& arg : n.args) args.push_back(atomize(arg, binds));
    if (n.block) args.push_back(atomize(*n.block, binds));
    const int arity = static_cast<int>(args.size());
    // The arguments are atoms, so each arm repeats them.
    const auto copies = [&] {
        std::vector<ExprPtr> copied;
        for (const auto& arg : args) copied.push_back(clone(arg));
        return copied;
    };
    // A function of this unit (no module) is called locally, with the
    // capability context a foul one takes.
    const auto call = [&](const std::string& module, const std::string& function) {
        return module.empty() ? localCallExpr(function, copies())
                              : callE(module, function, arity, copies());
    };
    auto chosen = call(target.backendModule, target.backendFunction);
    Match byOwner;
    for (const auto& alternative : target.alternatives) {
        if (!alternative.receiverType.empty()) continue;
        MatchClause arm;
        auto owner = atomPattern(alternative.backendModule);
        arm.patterns.push_back(std::move(owner));
        arm.body = call(alternative.backendModule, alternative.backendFunction);
        byOwner.clauses.push_back(std::move(arm));
    }
    if (!byOwner.clauses.empty()) {
        byOwner.subjects.push_back(callE("kex_io", "method_owner", 1, one(clone(args.front()))));
        MatchClause rest;
        auto wild = std::make_unique<Pattern>();
        wild->kind = PatKind::Wild;
        rest.patterns.push_back(std::move(wild));
        rest.body = std::move(chosen);
        byOwner.clauses.push_back(std::move(rest));
        chosen = std::make_unique<Expr>();
        chosen->node = std::move(byOwner);
    }
    for (const auto& alternative : target.alternatives)
        if (!alternative.receiverType.empty())
            chosen = matchBool(typeGuard(alternative.receiverType, clone(args.front())),
                               call(alternative.backendModule, alternative.backendFunction),
                               std::move(chosen));
    return wrapLets(binds, std::move(chosen));
}

} // namespace kex::ir
