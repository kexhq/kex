// IR construction helpers methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"

namespace kex::ir {

auto Lowering::litInt(long v) -> ExprPtr { return lit(LitKind::Int, std::to_string(v)); }

// `{<<"file.kex">>, Line, Column}` for the expression being lowered, or the
// atom `none` where the AST carries no location (generated code). Used by
// the testing DSL — see the describe/it case in lowerFunctionCall.
auto Lowering::sourceLocationTuple() -> ExprPtr {
    if (!currentLoc || currentLoc->line <= 0)
        return lit(LitKind::Atom, "none");
    std::vector<ExprPtr> parts;
    parts.push_back(lit(LitKind::String, std::string(currentLoc->file)));
    parts.push_back(litInt(currentLoc->line));
    parts.push_back(litInt(currentLoc->column));
    auto tuple = std::make_unique<Expr>();
    tuple->node = MakeTuple{std::move(parts)};
    return tuple;
}

auto Lowering::one(ExprPtr a) -> std::vector<ExprPtr> {
    std::vector<ExprPtr> v; v.push_back(std::move(a)); return v;
}

auto Lowering::two(ExprPtr a, ExprPtr b) -> std::vector<ExprPtr> {
    std::vector<ExprPtr> v; v.push_back(std::move(a)); v.push_back(std::move(b)); return v;
}

auto Lowering::three(ExprPtr a, ExprPtr b, ExprPtr c) -> std::vector<ExprPtr> {
    std::vector<ExprPtr> v;
    v.push_back(std::move(a)); v.push_back(std::move(b)); v.push_back(std::move(c));
    return v;
}

auto Lowering::callE_indirect(ExprPtr callee, std::vector<ExprPtr> args) -> ExprPtr {
    auto e = std::make_unique<Expr>();
    e->node = CallIndirect{std::move(callee), std::move(args), false};
    return e;
}

// Atomize an already-lowered IR expr: bind to a fresh Let if it isn't a
// Var/Lit, recording the binding.
auto Lowering::atomize_ir(ExprPtr e, std::vector<Binding>& binds) -> ExprPtr {
    if (std::holds_alternative<Var>(e->node) || std::holds_alternative<Lit>(e->node))
        return e;
    auto name = fresh();
    binds.push_back({name, std::move(e)});
    return var(name);
}

// Foul by NAME, minus any arity known to be pure. One BEAM module holds
// both: a capability stand-in's `foul put(url, body)` and `Map`'s pure
// `put` are both `kex_prelude:put`, and marking the name alone appended a
// capability context to every `map.put(k, v)` — `kex_prelude:put/4`, which
// does not exist.
//
// Arity ALONE cannot decide it either: a capture (`~IO.printLine`) reaches
// here with the arity of the closure rather than of the call, and an
// overload set records only one arity per name, so keying on arity dropped
// the context from `ENV.get(key)` while keeping it for `get(key, default)`.
// Subtracting the arities that are positively known to be pure keeps every
// call that was right before and fixes the collisions (kexhq/kex#143).
auto Lowering::isFoulBackendTarget(const std::string& mod, const std::string& fn,
                         int arity) -> bool {
    if (!externalModules) return false;

    if (!foulBackendTargetsBuilt) {
        foulBackendTargetsBuilt = true;
        // Each entry is `Module.member/arity` — one per overload, since one
        // name can be foul at some arities and pure at others.
        for (const auto& entry : externalModules->foulExports) {
            auto slash = entry.rfind('/');
            if (slash == std::string::npos) continue;
            const auto qualKey = entry.substr(0, slash);
            // The member is after the LAST dot. Split at the first one, a
            // nested module's `Time.Clock.now` was filed as `Time`'s `now`,
            // and `Time.Clock.now()` went out without its context
            // (`undef` for 'Kex.Time.Clock':now/0).
            auto dot = qualKey.rfind('.');
            if (dot == std::string::npos) continue;
            auto atom = externalModules->nameToAtom.find(qualKey.substr(0, dot));
            auto beamFn = externalModules->exportToBeamFn.find(qualKey);
            if (atom == externalModules->nameToAtom.end() ||
                beamFn == externalModules->exportToBeamFn.end()) continue;
            foulBackendTargets.insert(foulKey(atom->second, beamFn->second));
        }
        // Receiver functions resolve through their own table, so their
        // foulness has to be folded in as well — `cli.run(...)` reaches
        // the prelude this way, not through exportToBeamFn.
        for (const auto& [_, providers] : externalModules->receiverFunctions)
            for (const auto& provider : providers) {
                if (provider.isFoul)
                    foulBackendTargets.insert(
                        foulKey(provider.moduleAtom, provider.beamFunction));
                else
                    pureBackendTargets.insert(
                        foulKey(provider.moduleAtom, provider.beamFunction,
                                provider.beamArity));
            }
    }
    return foulBackendTargets.count(foulKey(mod, fn)) > 0 &&
           pureBackendTargets.count(foulKey(mod, fn, arity)) == 0;
}

auto Lowering::foulKey(const std::string& mod, const std::string& fn)
    -> std::string {
    return mod + ":" + fn;
}

auto Lowering::foulKey(const std::string& mod, const std::string& fn, int arity)
    -> std::string {
    return mod + ":" + fn + "/" + std::to_string(arity);
}

auto Lowering::callE(std::string mod, std::string fn, int arity, std::vector<ExprPtr> args) -> ExprPtr {
    if (threadsCapabilities() && !mod.empty() &&
        isFoulBackendTarget(mod, fn, arity)) {
        args.push_back(currentContext());
        ++arity;
    }
    if (std::getenv("KEX_DEBUG_TO2") && fn == "to" && mod.rfind("Kex.", 0) == 0)
        std::fprintf(stderr, "[callE] mod=%s fn=%s arity=%d\n",
                     mod.c_str(), fn.c_str(), arity);
    auto e = std::make_unique<Expr>();
    e->node = Call{std::move(mod), std::move(fn), arity, std::move(args), false};
    return e;
}

auto Lowering::intrin(Op op, std::vector<ExprPtr> args) -> ExprPtr {
    auto e = std::make_unique<Expr>();
    e->node = Intrinsic{op, std::move(args)};
    return e;
}

// Run `body`, then kex_test:maybe_print_summary() (which prints the
// describe/it pass/fail tally, or nothing if no tests ran), returning
// body's value. Wraps every main.
auto Lowering::withTestSummary(ExprPtr body) -> ExprPtr {
    std::string r = fresh("R");
    return makeLet(r, std::move(body),
        makeLet(fresh("Sum"), callE("kex_test", "maybe_print_summary", 0, {}), var(r)));
}

// Prepend a kex_io:register_display/2 call carrying this module's record
// layouts and variant arities — only the compiler knows which tuples are
// records/variants, and the runtime needs that to render
// `Name { field: value }` / `Tag(args)` instead of plain tuples.
auto Lowering::withDisplayInfo(ExprPtr body, const std::string& moduleName )
    -> ExprPtr {
    bool anyVariant = !variantArity.empty();
    if (records.empty() && !anyVariant && distinctTypes.empty()) return std::move(body);
    auto atomLit = [&](const std::string& s) { return lit(LitKind::Atom, s); };
    auto mapFrom = [&](std::vector<ExprPtr> pairs) {
        auto lst = std::make_unique<Expr>();
        lst->node = MakeList{std::move(pairs), std::nullopt};
        return callE("maps", "from_list", 1, one(std::move(lst)));
    };
    std::vector<ExprPtr> recPairs;
    for (const auto& [name, info] : records) {
        std::vector<ExprPtr> fields;
        for (const auto& f : info.fields) fields.push_back(atomLit(f));
        auto fl = std::make_unique<Expr>();
        fl->node = MakeList{std::move(fields), std::nullopt};
        auto t = std::make_unique<Expr>();
        t->node = MakeTuple{two(atomLit(name), std::move(fl))};
        recPairs.push_back(std::move(t));
    }
    std::vector<ExprPtr> varPairs;
    for (const auto& [tag, ar] : variantArity) {
        auto metadata = std::make_unique<Expr>();
        metadata->node = MakeTuple{two(litInt(ar), atomLit(variantOwner[tag]))};
        auto t = std::make_unique<Expr>();
        t->node = MakeTuple{two(atomLit(tag), std::move(metadata))};
        varPairs.push_back(std::move(t));
    }
    // Which module implements the methods of the records and ADTs defined
    // here. A capability's override branch cannot name a local function —
    // the value replacing it is implemented in another unit — so it
    // dispatches through this registry instead (kexhq/kex#181). The
    // natural-order comparison behind `sort`/`min`/`max` reads the same
    // registry to reach a type's own `compare` (kexhq/kex#283).
    //
    // A type whose methods live in ANOTHER module of this build must name
    // that module: claiming it here sent the dispatch to a function this
    // one never defined, and a capability replaced by a stand-in from an
    // opt-in module died `undef` (kexhq/kex#143). `localTypeModules` is
    // keyed by the make target as written, which inside a module is the
    // bare name.
    auto ownerModuleFor = [&](const std::string& name) {
        auto declaring = localTypeModules.find(name);
        if (declaring == localTypeModules.end())
            if (auto dot = name.rfind('.'); dot != std::string::npos) {
                // A tail lookup can answer for a DIFFERENT module's
                // same-named record. Accept it only when the module it
                // names actually reconstructs this record's qualified
                // identity, or `Web.Response` gets registered as owned by
                // whichever module declared the last `Response`
                // (kexhq/kex#143).
                auto tail = localTypeModules.find(name.substr(dot + 1));
                if (tail != localTypeModules.end() &&
                    tail->second + "." + name.substr(dot + 1) == name)
                    declaring = tail;
            }
        if (declaring == localTypeModules.end())
            if (auto owner = externalMethodOwners.find(name);
                owner != externalMethodOwners.end())
                return owner->second;
        return declaring != localTypeModules.end()
            ? "Kex." + declaring->second : moduleName;
    };
    if (!moduleName.empty() && (!records.empty() || anyVariant)) {
        std::vector<ExprPtr> ownerPairs;
        for (const auto& [name, info] : records) {
            (void)info;
            auto t = std::make_unique<Expr>();
            t->node = MakeTuple{two(atomLit(name),
                                    atomLit(ownerModuleFor(name)))};
            ownerPairs.push_back(std::move(t));
        }
        // An ADT's methods hang off the TYPE, but a value carries the
        // VARIANT's tag — a nullary one is a bare atom, with no tuple to
        // read a type off. Registering each tag against its type's module
        // is what lets a runtime dispatch find `compare` for a `Priority`.
        // A tag that is also a record name keeps the record's entry.
        for (const auto& [tag, ar] : variantArity) {
            (void)ar;
            if (records.count(tag)) continue;
            auto owner = variantOwner.find(tag);
            auto t = std::make_unique<Expr>();
            t->node = MakeTuple{
                two(atomLit(tag),
                    atomLit(ownerModuleFor(owner == variantOwner.end()
                                               ? tag : owner->second)))};
            ownerPairs.push_back(std::move(t));
        }
        body = makeLet(fresh("Own"),
                       callE("kex_io", "register_method_owners", 1,
                             one(mapFrom(std::move(ownerPairs)))),
                       std::move(body));
    }
    if (!recordLayouts.empty() || !adtLayouts.empty()) {
        auto binaryLit = [&](const std::string& text) {
            return lit(LitKind::String, text);
        };
        auto listOf = [](std::vector<ExprPtr> items) {
            auto list = std::make_unique<Expr>();
            list->node = MakeList{std::move(items), std::nullopt};
            return list;
        };
        auto tupleOf = [](std::vector<ExprPtr> items) {
            auto tuple = std::make_unique<Expr>();
            tuple->node = MakeTuple{std::move(items)};
            return tuple;
        };
        std::vector<ExprPtr> recordEntries;
        for (const auto& [name, fields] : recordLayouts) {
            std::vector<ExprPtr> described;
            const auto info = records.find(name);
            for (size_t i = 0; i < fields.size(); i++) {
                const auto& field = fields[i];
                std::vector<ExprPtr> parts;
                parts.push_back(binaryLit(field.name));
                const auto type = semantic::expandDeclaredAliases(field.type, declaredAliases);
                parts.push_back(structuredTypeLiteral(type));
                parts.push_back(litBool(type.name == "Option"));
                parts.push_back(litBool(field.hasDefault));
                // The default itself, as a fun evaluated only when
                // `Type.construct` needs it (kexhq/kex#487).
                if (info != records.end() && i < info->second.defaults.size() &&
                    info->second.defaults[i]) {
                    Lambda thunk;
                    thunk.body = lowerFieldDefault(info->second,
                                                   *info->second.defaults[i]);
                    auto fun = std::make_unique<Expr>();
                    fun->node = std::move(thunk);
                    parts.push_back(std::move(fun));
                } else {
                    parts.push_back(atomLit("none"));
                }
                described.push_back(tupleOf(std::move(parts)));
            }
            recordEntries.push_back(tupleOf(two(atomLit(name), listOf(std::move(described)))));
        }
        std::vector<ExprPtr> adtEntries;
        for (const auto& [owner, variants] : adtLayouts) {
            std::vector<ExprPtr> described;
            for (const auto& [tag, argTypes] : variants) {
                std::vector<ExprPtr> types;
                for (const auto& type : argTypes)
                    types.push_back(structuredTypeLiteral(
                        semantic::expandDeclaredAliases(type, declaredAliases)));
                described.push_back(tupleOf(two(binaryLit(tag), listOf(std::move(types)))));
            }
            adtEntries.push_back(tupleOf(two(atomLit(owner), listOf(std::move(described)))));
        }
        body = makeLet(fresh("Layouts"),
                       callE("kex_intrinsic_type", "register_layouts", 2,
                             two(mapFrom(std::move(recordEntries)),
                                 mapFrom(std::move(adtEntries)))),
                       std::move(body));
    }
    if (!distinctTypes.empty()) {
        // What each distinct type is erased to, for code that names one
        // at run time: `.to(UserId)` converts to the backing type, and
        // `Type.of(UserId)` answers with it.
        std::vector<ExprPtr> backings;
        for (const auto& name : distinctTypes) {
            semantic::StructuredType named;
            named.name = name;
            const auto backing = semantic::expandDeclaredAliases(named, declaredAliases);
            if (backing.name == name) continue;
            auto pair = std::make_unique<Expr>();
            pair->node = MakeTuple{two(atomLit(name), structuredTypeLiteral(backing))};
            backings.push_back(std::move(pair));
        }
        if (!backings.empty())
            body = makeLet(fresh("Distinct"),
                           callE("kex_intrinsic_type", "register_distinct", 1,
                                 one(mapFrom(std::move(backings)))),
                           std::move(body));
    }
    return makeLet(fresh("Disp"),
        callE("kex_io", "register_display", 2,
              two(mapFrom(std::move(recPairs)), mapFrom(std::move(varPairs)))),
        std::move(body));
}

auto Lowering::makeLet(std::string name, ExprPtr value, ExprPtr body) -> ExprPtr {
    auto e = std::make_unique<Expr>();
    e->node = Let{std::move(name), std::move(value), std::move(body)};
    return e;
}

// A single-clause match binding `pattern` against `subject`, continuing
// with `body` — used for `let (a, b) = expr` destructuring.
// Single-clause destructuring (`let Just(x) = ...`, for-loop item patterns,
// fold state patterns). Every caller treats a non-match as an error, which
// is what the interpreter reports as "pattern mismatch" — so emit that
// explicitly. Without a catch-all the emitted `case` has no fallback, and
// Core Erlang's compiler crashes on the implicit failure with an internal
// error in beam_core_to_ssa rather than producing a runnable module.
auto Lowering::makeMatch1(ExprPtr subject, PatternPtr pattern, ExprPtr body) -> ExprPtr {
    Match m;
    m.subjects.push_back(std::move(subject));
    MatchClause mc;
    mc.patterns.push_back(std::move(pattern));
    mc.body = std::move(body);
    m.clauses.push_back(std::move(mc));
    MatchClause fallback;
    auto wild = std::make_unique<Pattern>();
    wild->kind = PatKind::Wild;
    fallback.patterns.push_back(std::move(wild));
    fallback.body = runtimeError("pattern mismatch");
    m.clauses.push_back(std::move(fallback));
    auto e = std::make_unique<Expr>();
    e->node = std::move(m);
    return e;
}

// Is this IR expression already a `Bool`, so the truthiness test below can
// be skipped? Deliberately a small allowlist of things that cannot be
// anything else — every comparison and boolean BIF, and a literal. Missing
// a case here costs three comparisons, not correctness.
auto Lowering::irIsBoolean(const ExprPtr& e) -> bool {
    if (!e) return false;
    if (auto* l = std::get_if<Lit>(&e->node))
        return l->kind == LitKind::Bool;
    // Comparisons and boolean connectives lower to an Intrinsic, not a
    // Call — `1 > 2` is `Intrinsic{Op::Gt}`, and only emit_core turns it
    // into `call 'erlang':'>'`.
    if (auto* i = std::get_if<Intrinsic>(&e->node)) {
        switch (i->op) {
            case Op::Eq: case Op::Neq:
            case Op::Lt: case Op::Gt: case Op::Lte: case Op::Gte:
            case Op::And: case Op::Or: case Op::Not:
                return true;
            default:
                return false;
        }
    }
    if (auto* c = std::get_if<Call>(&e->node)) {
        if (c->module != "erlang") return false;
        static const std::unordered_set<std::string> boolBifs = {
            "==", "/=", "=:=", "=/=", "<", ">", "=<", ">=",
            "not", "and", "or", "xor",
            "is_atom", "is_binary", "is_boolean", "is_float", "is_function",
            "is_integer", "is_list", "is_map", "is_number", "is_pid",
            "is_reference", "is_tuple",
        };
        return boolBifs.count(c->name) > 0;
    }
    return false;
}

// Only `false`, `None` and `()` are falsy; everything else is truthy —
// Crystal semantics, which `src/stdlib/truthyable.kex` states and the tree
// walker has always implemented. BEAM matched `true`/`false` literally, so
// `if Ok(1)` was a `case_clause` crash and a `find` whose block returned a
// non-Bool died with `bad_filter`.
//
// Done in the SUBJECT rather than as extra clauses because the else branch
// must stay lazy — repeating it under three falsy patterns would either
// duplicate its code or force it to be bound eagerly.
auto Lowering::truthyTest(ExprPtr cond) -> ExprPtr {
    if (irIsBoolean(cond)) return cond;
    std::string cv = fresh("_Truthy");
    auto notEq = [&](const char* atom) {
        return callE("erlang", "=/=", 2,
                     two(var(cv), lit(LitKind::Atom, atom)));
    };
    // `and`, not `andalso`: the latter is a control-flow OPERATOR, so
    // `erlang:'andalso'/2` does not exist and every condition became
    // `undef`. Both operands are comparisons, so strictness costs nothing.
    auto test = callE("erlang", "and", 2,
                      two(notEq("false"), notEq("None")));
    test = callE("erlang", "and", 2,
                 two(std::move(test), notEq("Kex.Unit")));
    return makeLet(cv, std::move(cond), std::move(test));
}

auto Lowering::matchBool(ExprPtr cond, ExprPtr thenE, ExprPtr elseE) -> ExprPtr {
    Match m;
    m.subjects.push_back(truthyTest(std::move(cond)));
    auto boolPat = [](bool b) {
        auto p = std::make_unique<Pattern>();
        p->kind = PatKind::Lit; p->litKind = LitKind::Bool; p->litBool = b;
        return p;
    };
    MatchClause t; t.patterns.push_back(boolPat(true));  t.body = std::move(thenE);
    MatchClause f; f.patterns.push_back(boolPat(false)); f.body = std::move(elseE);
    m.clauses.push_back(std::move(t));
    m.clauses.push_back(std::move(f));
    auto e = std::make_unique<Expr>();
    e->node = std::move(m);
    return e;
}

} // namespace kex::ir
