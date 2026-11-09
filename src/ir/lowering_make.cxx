// Make blocks methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"
#include "../semantic/resolve_core.hxx"

namespace kex::ir {

// Lower one make-block method to a FunDef. First cut: implicit-`this`
// methods with named/simple params (the common `@field`/`this.method`
// shape). Static constructors and receiver-pattern methods are deferred.
// Lower a group of same-name make-block methods (multi-clause) for one
// type. A method whose first param is an unnamed pattern (`@Less`,
// `{input, pos}`, `@[x|_]`) matches the RECEIVER directly — no implicit
// `this`. Otherwise `this` is prepended.
auto Lowering::lowerMakeGroup(const std::vector<const ast::FunctionDef*>& group,
                    const std::string& typeName) -> FunDef {
    const auto& first = *group[0];
    bool isStaticCtor = !first.name.empty()
                      && std::isupper(static_cast<unsigned char>(first.name[0]));
    if (isStaticCtor)
        throw LowerError("IR lower: static constructor '" + first.name + "' not yet ported");
    // A receiver pattern is specifically the `@` sigil (ThisPattern), a
    // record destructure (RecordPattern), or a range (RangePattern) —
    // NOT any pattern. A bare type-name param like `to(String)` is a
    // ConstructorPattern *value* argument, and such a method still uses
    // its implicit `this` (spec/type_dispatch.kex's `to(String)` body is
    // `"(${@x}, ${@y})"`).
    bool receiverPattern = false;
    if (!first.clauses.empty() && !first.clauses[0].params.empty()) {
        const auto& p0 = first.clauses[0].params[0];
        if (!p0.name && p0.pattern)
            receiverPattern =
                std::holds_alternative<ast::ThisPattern>((*p0.pattern)->kind) ||
                std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind) ||
                std::holds_alternative<ast::RangePattern>((*p0.pattern)->kind);
    }
    if (!receiverPattern) implicitThisMethods.insert(first.name);
    // A make-String method matching the receiver with LIST patterns
    // (`myCapitalize(@[x | rest])`) — legal, since [Char] IS String —
    // can't match a binary receiver directly. Detect it so the whole
    // group gets wrapped in an as_list coercion below.
    bool listReceiver = false;
    if (receiverPattern && typeName == "String")
        for (const auto* fn : group)
            for (const auto& cl : fn->clauses)
                if (!cl.params.empty() && !cl.params[0].name && cl.params[0].pattern)
                    if (auto* tp = std::get_if<ast::ThisPattern>(&(*cl.params[0].pattern)->kind))
                        if (tp->inner && std::holds_alternative<ast::ListPattern>(tp->inner->kind))
                            listReceiver = true;
    auto previousMakeType = currentMakeType;
    currentMakeType = typeName;
    auto def = lowerFunctionGroup(group, receiverPattern ? "" : "this");
    currentMakeType = std::move(previousMakeType);
    // An implicit method on a record still needs to constrain its receiver
    // in Core Erlang. Without this guard, a method such as
    // `Measure.to(String)` becomes a catch-all `to/2` clause and shadows
    // the universal conversion function for every other value.
    // A TYPE-QUALIFIED call — `Parser.parse("null")`, the constructor-style
    // form the walker has always allowed for a make-block method that
    // never touches `this` — lowers its receiver to the bare type ATOM
    // (`apply 'parse'/2('Parser', Input)`). The record guard alone rejects
    // that, so every such call fell through to the emitter's synthetic
    // catch-all and raised `function_clause`. Accepting the atom as well
    // keeps the guard's real job (stopping this clause from shadowing the
    // universal `to`/`showValue` for unrelated values) while letting the
    // type-qualified form through.
    const bool guardedRecordReceiver =
        !receiverPattern && records.count(canonicalRecordName(typeName));
    if (guardedRecordReceiver)
        for (auto& clause : def.clauses)
            clause.guard = callE(
                "erlang", "or", 2,
                two(typeGuard(typeName, var("this")),
                    intrin(Op::Eq, two(var("this"),
                                       lit(LitKind::Atom, typeName)))));
    // Set when a later branch appends a clause that forwards an
    // unmatched receiver somewhere else (the prelude's `to`, a shadowed
    // trait default); such a definition must not also end in an error.
    bool forwarded = false;
    if (listReceiver) def = coerceListReceiver(std::move(def));
    // The overload set was keyed by the SOURCE arity (receiver included).
    // `def.arity` also counts the trait dictionaries and the capability
    // context lowering appends, so a `foul` overload looked up a key that
    // was never registered: call sites emitted
    // `background/Router,Integer` while the definition kept the bare name,
    // and erlc reported an undefined function.
    const auto sourceArity = static_cast<size_t>(def.arity) -
        def.dictionaryTraits.size() - (def.hasCapabilityContext ? 1u : 0u);
    if (argumentOverloadedMethods.count(
            localOverloadKey(first.name, typeName, sourceArity)))
        def.name = mangleReceiverSignature(
            first.name, methodDispatchTypes(first, typeName));
    else if (auto colliding =
                 collidingImplementationName(first.name, typeName, sourceArity))
        def.name = *colliding;
    else if (!traitIdentityOf(typeName, currentModulePath, true).empty() &&
             !typeName.empty())
        def.name = mangleReceiverImplementation(first.name, typeName);
    // A lone local `to(String)` still participates in the language-wide
    // conversion protocol. Its guarded record clause handles the local
    // type; unmatched values must continue to the prelude conversion
    // instead of ending in the emitter's synthetic function_clause.
    if (first.name == "to" && def.name == "to" &&
        !lowersPrelude) {
        // The same goes for its typed parameters: Units.SI's
        // `make Measure do let to(String, in: SIPrefix)` is a 3-ary `to`
        // too, and without a guard on `in` it claimed the prelude's
        // `to(String, decimals: 1)` and read `1` as the prefix.
        std::vector<const ast::FunctionClause*> sourceClauses;
        for (const auto* fn : group)
            for (const auto& cl : fn->clauses) sourceClauses.push_back(&cl);
        if (!receiverPattern && sourceClauses.size() == def.clauses.size())
            for (size_t c = 0; c < def.clauses.size(); ++c) {
                auto& clause = def.clauses[c];
                const auto& params = sourceClauses[c]->params;
                for (size_t j = 0; j < params.size(); ++j) {
                    if (!params[j].type || !*params[j].type ||
                        j + 1 >= clause.params.size())
                        continue;
                    const auto written = typeText(**params[j].type, TypeSpelling::Dispatch);
                    const auto& pattern = clause.params[j + 1];
                    if (!discriminatingType(written) || !pattern ||
                        pattern->kind != PatKind::Var)
                        continue;
                    auto next = typeGuard(written, var(pattern->name));
                    if (clause.guard && *clause.guard)
                        clause.guard = callE(
                            "erlang", "and", 2,
                            two(std::move(*clause.guard), std::move(next)));
                    else
                        clause.guard = std::move(next);
                }
            }
        FunClause fallback;
        std::vector<ExprPtr> args;
        for (int i = 0; i < def.arity; ++i) {
            auto param = std::make_unique<Pattern>();
            param->kind = PatKind::Var;
            param->name = "_toFallback" + std::to_string(i);
            fallback.params.push_back(std::move(param));
            args.push_back(var("_toFallback" + std::to_string(i)));
        }
        const auto next = nextExtendingModule("to", typeName, def.arity);
        fallback.body = callE(next.empty() ? names::kPreludeModule : names::moduleAtom(next),
                              "to", def.arity, std::move(args));
        def.clauses.push_back(std::move(fallback));
        forwarded = true;
    }
    // The prelude's own per-type conversions — `make Atom do let
    // to(String)`, `make Type do let to(String)` — are mangled to
    // `to/Atom` and reached through a dispatcher that routes on the
    // receiver alone, so `:a.to(Integer)` entered the Atom clause, failed
    // its `String` pattern and died with `function_clause`. What it does
    // not match continues to the generic conversion, which the dispatcher
    // keeps in this module as `to$generic`.
    // Only a plain definition: a trait default (`to/Showable`) carries a
    // hidden dictionary and already forwards through its own fallback.
    if (first.name == "to" && def.name != "to" &&
        lowersPrelude && collidingMethods.count("to") &&
        static_cast<size_t>(def.arity) == sourceArity &&
        [&] {
            // A new conversion arity need not have a generic sibling:
            // Measure's precision formatter takes four arguments, while
            // the generic conversions only take two or three.
            const auto generic = fnNamedClauses.find("to");
            return generic != fnNamedClauses.end() &&
                std::any_of(generic->second.begin(), generic->second.end(),
                            [&](const ast::FunctionClause* clause) {
                                return clause->params.size() == sourceArity;
                            });
        }()) {
        FunClause fallback;
        std::vector<ExprPtr> args;
        for (int i = 0; i < def.arity; ++i) {
            auto param = std::make_unique<Pattern>();
            param->kind = PatKind::Var;
            param->name = "_toFallback" + std::to_string(i);
            fallback.params.push_back(std::move(param));
            args.push_back(var("_toFallback" + std::to_string(i)));
        }
        auto body = std::make_unique<Expr>();
        body->node = Call{"", "to$generic", def.arity, std::move(args), false};
        fallback.body = std::move(body);
        def.clauses.push_back(std::move(fallback));
        forwarded = true;
    }
    // The same escape, for any method that shadows an imported TRAIT
    // DEFAULT. A call the value decides at run time reaches the local
    // definition with values of any type, which is only sound if the local
    // forwards what it does not match — `to` gets that above by name, and
    // nothing else did. A record with its own `showValue` then crashed the
    // moment string interpolation began routing through Showable:
    // `"badge:${this.kind}"` compiles to `showValue(Kind)`, the local
    // definition claims it, its `is_record(This,'Badge',2)` clause rejects
    // a String, and the emitter's synthetic arm raises `function_clause`.
    //
    // Only for a definition that is not already total, and only when the
    // prelude actually provides the name at this arity — otherwise the
    // fallback would be unreachable, or a call to nothing.
    const bool shadowsTraitDefault =
        externalModules &&
        externalModules->receiverFunctions.count(first.name) &&
        std::any_of(traitMethods.begin(), traitMethods.end(),
                    [&](const auto& entry) {
                        return std::any_of(
                            entry.second.begin(), entry.second.end(),
                            [&](const auto& method) {
                                return method.name == first.name;
                            });
                    });
    if (first.name != "to" && def.name == first.name && shadowsTraitDefault) {
        const auto alreadyTotal =
            std::any_of(def.clauses.begin(), def.clauses.end(),
                        [](const FunClause& clause) {
                            return !clause.guard &&
                                std::all_of(clause.params.begin(),
                                            clause.params.end(),
                                            [](const PatternPtr& p) {
                                                return p->kind == PatKind::Var ||
                                                       p->kind == PatKind::Wild;
                                            });
                        });
        const auto& candidates =
            externalModules->receiverFunctions.at(first.name);
        // The DISPATCHER, by preference — the same reason the trait
        // dictionary takes it: it routes to every implementation and ends
        // at the default, where an implementation picked by list order
        // ends at whichever one came first. With the default, a local
        // `showValue` forwarded `Just(50)` to `showValue/Showable` and
        // rendered the tuple.
        // A `foul` method's last parameter is the hidden capability
        // context, which the provider being forwarded to does not have:
        // it lives one arity DOWN, and passing the context on hands it
        // over as a user argument (kexhq/kex#143).
        const int forwardArity =
            def.arity - (def.hasCapabilityContext ? 1 : 0);
        auto provider = std::find_if(
            candidates.begin(), candidates.end(),
            [&](const auto& candidate) {
                return candidate.beamArity == forwardArity &&
                    candidate.beamFunction == first.name;
            });
        if (provider == candidates.end())
            provider = std::find_if(
                candidates.begin(), candidates.end(),
                [&](const auto& candidate) {
                    return candidate.beamArity == forwardArity;
                });
        if (!alreadyTotal && provider != candidates.end()) {
            FunClause fallback;
            std::vector<ExprPtr> args;
            for (int i = 0; i < def.arity; ++i) {
                auto param = std::make_unique<Pattern>();
                param->kind = PatKind::Var;
                param->name = "_traitShadowFallback" + std::to_string(i);
                fallback.params.push_back(std::move(param));
                if (i < forwardArity)
                    args.push_back(
                        var("_traitShadowFallback" + std::to_string(i)));
            }
            fallback.body = callE(provider->moduleAtom,
                                  provider->beamFunction, forwardArity,
                                  std::move(args));
            def.clauses.push_back(std::move(fallback));
            forwarded = true;
        }
    }
    // A receiver the guard rejects is a call that should not have
    // compiled — `"a b".whiteSpaces`, where the method belongs to
    // Parsing's Input record. Falling off the end raises a bare
    // `function_clause`; name the method and the receiver instead, in the
    // walker's wording, exactly as the multi-owner dispatcher does. Only
    // when nothing else already forwards the unmatched case onward.
    if (guardedRecordReceiver && !forwarded && def.arity >= 1 &&
        def.name == first.name) {
        FunClause fallback;
        std::vector<ExprPtr> args;
        // See forwardArity above: a foul method carries the capability
        // context the provider knows nothing about.
        const int noReceiverArity =
            def.arity - (def.hasCapabilityContext ? 1 : 0);
        for (int i = 0; i < def.arity; ++i) {
            auto param = std::make_unique<Pattern>();
            param->kind = PatKind::Var;
            param->name = "_noReceiver" + std::to_string(i);
            fallback.params.push_back(std::move(param));
            if (i < noReceiverArity)
                args.push_back(var("_noReceiver" + std::to_string(i)));
        }
        // Another module may own the same method name for a receiver this
        // one does not cover — `get` is Regex.Match's here and Map's in
        // the prelude. Forwarding keeps that call working; only a name
        // nobody else provides is a genuine "no such method".
        const ExternalModules::ReceiverFunction* provider = nullptr;
        if (externalModules) {
            auto candidates =
                externalModules->receiverFunctions.find(first.name);
            if (candidates != externalModules->receiverFunctions.end())
                for (const auto& candidate : candidates->second)
                    if (candidate.beamArity == noReceiverArity &&
                        candidate.beamFunction == first.name) {
                        provider = &candidate;
                        break;
                    }
        }
        // A slot's implementation is also where a call on a `Server` the
        // checker could not type ends up: one read back from
        // `Process.Shared` or from BEAM interop is `Any` statically, so
        // `server.broadcast(x)` lowers to this function and not to
        // `server_call` (kexhq/kex#402). Send such a call to the server,
        // as the typed call would have.
        const bool isSlot = std::any_of(
            servingSlotsByType.begin(), servingSlotsByType.end(),
            [&](const auto& entry) {
                return std::find(entry.second.begin(), entry.second.end(),
                                 first.name) != entry.second.end();
            });
        if (isSlot) {
            FunClause toServer;
            std::vector<ExprPtr> slotArgs;
            for (int i = 0; i < def.arity; ++i) {
                auto param = std::make_unique<Pattern>();
                param->kind = PatKind::Var;
                param->name = "_serverSlot" + std::to_string(i);
                toServer.params.push_back(std::move(param));
                if (i > 0 && i < noReceiverArity)
                    slotArgs.push_back(var("_serverSlot" + std::to_string(i)));
            }
            toServer.guard = callE("erlang", "is_record", 3,
                three(var("_serverSlot0"), lit(LitKind::Atom, "Server"),
                      lit(LitKind::Int, "3")));
            auto argsList = std::make_unique<Expr>();
            argsList->node = MakeList{std::move(slotArgs), std::nullopt};
            std::vector<ExprPtr> callArgs;
            callArgs.push_back(var("_serverSlot0"));
            callArgs.push_back(lit(LitKind::Atom, first.name));
            callArgs.push_back(std::move(argsList));
            if (servingCastSlots.count(first.name)) {
                toServer.body = callE("kex_intrinsic_process", "server_cast", 3,
                                      std::move(callArgs));
            } else {
                callArgs.push_back(lit(LitKind::Atom, "default"));
                toServer.body = callE("kex_intrinsic_process", "server_call", 4,
                                      std::move(callArgs));
            }
            def.clauses.push_back(std::move(toServer));
        }
        if (provider)
            fallback.body = callE(provider->moduleAtom,
                                  provider->beamFunction, noReceiverArity,
                                  std::move(args));
        else {
            auto message = callE("erlang", "iolist_to_binary", 1,
                one(makeListOf(
                    lit(LitKind::String,
                        "runtime error: Undefined method: " + first.name +
                        " for "),
                    callE("kex_io", "value_type_name", 1,
                          one(var("_noReceiver0"))))));
            fallback.body =
                callE("erlang", "error", 1, one(std::move(message)));
        }
        def.clauses.push_back(std::move(fallback));
    }
    return def;
}

// Rewrap a FunDef whose clauses pattern-match the receiver as a list:
// one wrapper clause binds raw params, then a multi-subject Match runs
// the original clause patterns against `as_list(receiver)` (a String
// binary becomes its [Char] codepoint list; lists pass through).
auto Lowering::coerceListReceiver(FunDef def) -> FunDef {
    FunDef out; out.name = def.name; out.arity = def.arity;
    FunClause fc;
    Match m;
    for (int i = 0; i < def.arity; i++) {
        std::string p = "_cr" + std::to_string(i);
        auto pat = std::make_unique<Pattern>();
        pat->kind = PatKind::Var; pat->name = p;
        fc.params.push_back(std::move(pat));
        m.subjects.push_back(i == 0
            ? callE("kex_intrinsic_list", "as_list", 1, one(var(p)))
            : var(p));
    }
    for (auto& cl : def.clauses) {
        MatchClause mc;
        mc.patterns = std::move(cl.params);
        mc.guard = std::move(cl.guard);
        mc.body = std::move(cl.body);
        // `@[x | rest]` against a String binds `rest` to the REST OF THE
        // STRING, not to a [Char] — the two are different types, and the
        // receiver's own type is the one to keep. The subject was coerced
        // to a list above, so the tail comes back as a list and has to be
        // packed back for a binary receiver.
        if (!mc.patterns.empty() && mc.patterns[0] &&
            mc.patterns[0]->kind == PatKind::List && mc.patterns[0]->rest &&
            mc.patterns[0]->rest->kind == PatKind::Var) {
            const auto& tail = mc.patterns[0]->rest->name;
            mc.body = makeLet(
                tail,
                callE("kex_intrinsic_list", "repack", 2,
                      two(var("_cr0"), var(tail))),
                std::move(mc.body));
        }
        m.clauses.push_back(std::move(mc));
    }
    auto b = std::make_unique<Expr>();
    b->node = std::move(m);
    fc.body = std::move(b);
    out.clauses.push_back(std::move(fc));
    return out;
}

auto Lowering::primGuardBifs() -> const std::unordered_map<std::string, const char*>& {
    static const std::unordered_map<std::string, const char*> m = {
        {"Integer","is_integer"}, {"Float","is_float"},
        {"Number","is_number"}, {"String","is_binary"}, {"Bool","is_boolean"},
        {"Map","is_map"}, {"List","is_list"},
        // A record, a variant and a Char are Erlang tuples too, so this
        // guard is wider than the Kex type. It only decides which `make`
        // block a receiver reaches, and the tuple family has no method a
        // record also defines — see `make Tuple` in the prelude.
        {"Tuple","is_tuple"},

        {"Pid","is_pid"}, {"Task","is_pid"}, {"Reference","is_reference"},
        // Wider than the Kex type as well: a Bool, `None` and every
        // nullary variant are atoms too. The dispatcher emits it after
        // all of those (see `deferBroader`'s neighbour below).
        {"Atom","is_atom"},
    };
    return m;
}

auto Lowering::guardTag(const std::string& canonical) const -> std::string {
    auto tagged = canonical;
    if (!records.count(tagged) && tagged.find('.') == std::string::npos) {
        std::string own;
        for (auto scope = currentModulePath; !scope.empty();) {
            if (declaredTypeIdentities.count(scope + "." + tagged)) {
                own = scope;
                break;
            }
            const auto dot = scope.rfind('.');
            if (dot == std::string::npos) break;
            scope.resize(dot);
        }
        if (!own.empty())
            tagged = own + "." + tagged;
        else if (auto declaring = localTypeModules.find(tagged);
            declaring != localTypeModules.end())
            tagged = declaring->second + "." + tagged;
        else if (auto declared = moduleDeclaredTypes.find(tagged);
                 declared != moduleDeclaredTypes.end())
            tagged = declared->second + "." + tagged;
    }
    return tagged;
}

auto Lowering::runtimeShapes(const std::string& owner)
    -> std::optional<std::set<std::string>> {
    // As primGuardBifs tells them apart, wider guards spelled as the shapes
    // they admit.
    static const std::unordered_map<std::string, std::set<std::string>> primitive = {
        {"Integer", {"integer"}}, {"Float", {"float"}},
        {"Number", {"integer", "float"}}, {"String", {"binary"}},
        {"Bool", {"bool"}}, {"Map", {"map"}}, {"List", {"list"}},
        {"Tuple", {"tuple"}}, {"Pid", {"pid"}}, {"Task", {"pid"}},
        {"Reference", {"reference"}}, {"Char", {"tuple:Char"}},
        {"Atom", {"bool", "atom"}},
    };
    if (auto found = primitive.find(owner); found != primitive.end())
        return found->second;
    // A sum type: its payload variants are tagged tuples, its nullary ones
    // bare atoms.
    if (auto tags = typeVariantTags.find(owner);
        tags != typeVariantTags.end() && !tags->second.empty()) {
        std::set<std::string> shapes;
        for (const auto& tag : tags->second)
            shapes.insert((nullaryVariantTags.count(tag) ? "atom:" : "tuple:") + tag);
        return shapes;
    }
    // A distinct value is its erased backing value: no tag says it is one.
    const auto bare = owner.substr(owner.rfind('.') + 1);
    if (distinctTypes.count(owner) || distinctTypes.count(bare)) return std::nullopt;
    // Anything else typeGuard tests as a tuple tagged with its identity: a
    // record by its canonical name, an opaque type by guardTag's.
    const bool savedMakeOwner = resolvingMakeOwner;
    resolvingMakeOwner = true;
    const auto canonical = canonicalRecordName(owner);
    resolvingMakeOwner = savedMakeOwner;
    if (owner.find_first_of("[{(<-") != std::string::npos) return std::nullopt;
    return std::set<std::string>{"tuple:" + guardTag(canonical)};
}

auto Lowering::shapesMeet(const std::optional<std::set<std::string>>& a,
                          const std::optional<std::set<std::string>>& b) -> bool {
    if (!a || !b) return true;
    const auto meet = [](const std::string& x, const std::string& y) {
        return x == y || (x == "tuple" && y.rfind("tuple:", 0) == 0) ||
               (x == "atom" && y.rfind("atom:", 0) == 0);
    };
    for (const auto& x : *a)
        for (const auto& y : *b)
            if (meet(x, y) || meet(y, x)) return true;
    return false;
}

// `is_record(V, Tag, Size)` for one field slot. Unlike `typeGuard`, which
// reads the ONE layout `records` holds per name, this uses the slot's own
// size — the only thing that separates two records sharing a tag.
auto Lowering::recordGuard(const FieldSlot& slot, ExprPtr v) -> ExprPtr {
    return callE("erlang", "is_record", 3,
                 three(std::move(v), lit(LitKind::Atom, slot.record),
                       litInt(slot.tupleSize)));
}

// An ADT names its VARIANTS, never a tag of its own: a `Shape` value is
// either the atom 'Point' or the tuple {'Line', N}. Guarding on the type
// NAME therefore matched nothing at all, which is why an ADT-typed
// parameter used to contribute no dispatch guard
// (docs/issue-free-function-overloads.md).
auto Lowering::adtGuard(const std::vector<std::string>& tags, ExprPtr v) -> ExprPtr {
    auto vRef = snap(v);
    ExprPtr guard;
    for (const auto& tag : tags) {
        ExprPtr test;
        if (nullaryVariantTags.count(tag))
            test = intrin(Op::Eq,
                          two(vRef.get(), lit(LitKind::Atom, tag)));
        else
            // `element/2` is strict, so it may only run once the value is
            // known to be a tuple — see the note on typeGuard's tagged
            // fallback for why this is a `case` and not `erlang:and`.
            test = matchBool(
                callE("erlang", "is_tuple", 1, one(vRef.get())),
                intrin(Op::Eq,
                       two(callE("erlang", "element", 2,
                                 two(litInt(1), vRef.get())),
                           lit(LitKind::Atom, tag))),
                litBool(false));
        // `orelse`, spelled as a case for the same strictness reason.
        guard = guard ? matchBool(std::move(guard), litBool(true),
                                  std::move(test))
                      : std::move(test);
    }
    return guard ? std::move(guard) : litBool(false);
}

auto Lowering::typeGuard(const std::string& written, ExprPtr v) -> ExprPtr {
    if (!traitIdentityOf(written).empty()) return litBool(true);
    // `type Handler = Env -> String` names a SHAPE. No value carries a
    // 'Handler' tag, so the tagged fallback below could never match one
    // and a `Handler` parameter contributed no guard at all — which is
    // half of why a `Block<A>` overload beside it misdispatched
    // (docs/issue-free-function-overloads.md). A structural right-hand
    // side is never a bare name, so this recurses at most once.
    if (auto alias = structuralTypeAliases.find(written);
        alias != structuralTypeAliases.end() && alias->second != written)
        return typeGuard(alias->second, std::move(v));
    // `None` is an atom on BEAM, but it is the Optional value, not an
    // Atom: `Atom`'s clause in a dispatcher caught it, so `None.to(String)`
    // reached Atom's conversion and answered "None" where the walker, and
    // Optional's own `showValue`, answer "".
    if (written == "Atom")
        if (const auto* name = std::get_if<Var>(&v->node)) {
            const auto copy = name->name;
            return callE("erlang", "and", 2,
                two(callE("erlang", "is_atom", 1, one(std::move(v))),
                    callE("erlang", "=/=", 2,
                          two(var(copy), lit(LitKind::Atom, "None")))));
        }
    auto it = primGuardBifs().find(written);
    if (it != primGuardBifs().end())
        return callE("erlang", it->second, 1, one(std::move(v)));
    // Structural spellings name a SHAPE, not a tag. `let +(other: [A])`
    // records its parameter as `[A]`, which no value carries: the tagged
    // fallback below asked `element(1, X) =:= '[A]'`, no dispatcher clause
    // could match, and an operator overloaded on a list argument died
    // `function_clause` on BEAM while the walker dispatched it
    // (kexhq/kex#205). A function type is checked first because it is
    // parenthesized too.
    // Arity matters: a `Block<A>` is a fun too, and `is_function/1`
    // accepts both, so an overload set holding a handler AND a
    // parameterless block picked whichever clause came first and died
    // `badarity` (docs/issue-free-function-overloads.md).
    if (written.find("->") != std::string::npos)
        return callE("erlang", "is_function", 2,
                     two(std::move(v), litInt(functionTypeArity(written))));
    // `Block<A>` is a PARAMETERLESS lambda — `do "x" end`. It carries no
    // tag, so the base-name path below would have guarded on 'Block',
    // which nothing is.
    if (written.rfind("Block<", 0) == 0 && written.back() == '>')
        return callE("erlang", "is_function", 2,
                     two(std::move(v), litInt(0)));
    if (written.size() >= 2 && written.front() == '[' &&
        written.back() == ']') {
        // A Kex String IS `[Char]`, and it is a binary at runtime, so
        // that one spelling has to admit both shapes.
        if (written == "[Char]") {
            auto vRef = snap(v);
            return callE("erlang", "or", 2,
                         two(callE("erlang", "is_list", 1, one(vRef.get())),
                             callE("erlang", "is_binary", 1,
                                   one(vRef.get()))));
        }
        return callE("erlang", "is_list", 1, one(std::move(v)));
    }
    if (written.size() >= 2 && written.front() == '{' &&
        written.back() == '}')
        return callE("erlang", "is_map", 1, one(std::move(v)));
    if (written.size() >= 2 && written.front() == '(' &&
        written.back() == ')')
        return callE("erlang", "is_tuple", 1, one(std::move(v)));
    // Type arguments are erased at runtime: a `Set<A>` value is tagged
    // `Set`, so guard on the base name rather than on the applied
    // spelling, which is nothing's tag.
    if (auto angle = written.find('<');
        angle != std::string::npos && angle > 0 && written.back() == '>')
        return typeGuard(written.substr(0, angle), std::move(v));
    if (auto tags = typeVariantTags.find(written);
        tags != typeVariantTags.end() && !tags->second.empty())
        return adtGuard(tags->second, std::move(v));
    // Tagged record. Core Erlang exposes the guard-safe is_record/3 BIF
    // (the source-level is_record/2 form is a compiler macro).
    // The guard tests the value's TAG, so it is the record's canonical
    // identity that has to appear here — `make Version` inside
    // Kex guards values tagged 'Kex.Version'.
    const bool savedMakeOwner = resolvingMakeOwner;
    resolvingMakeOwner = true;
    const auto ty = canonicalRecordName(written);
    resolvingMakeOwner = savedMakeOwner;
    if (auto record = records.find(ty); record != records.end())
        return callE("erlang", "is_record", 3,
                     three(std::move(v), lit(LitKind::Atom, ty),
                           litInt(static_cast<int64_t>(
                               record->second.fields.size() + 1))));
    // An OPAQUE type declared inside a module carries its QUALIFIED
    // name as its tag, the same rule records follow — `type Client` in
    // Net.HTTP is `{'Net.HTTP.Client', …}`, which is what the runtime
    // intrinsic builds. Guarding on the written `Client` matched
    // nothing, so `client.get(url)` fell through its make block's
    // dispatcher to "Undefined method: get for Tuple".
    const auto tagged = guardTag(ty);
    // Unknown tagged type fallback. An ADT variant is `{'Tag', ...}` of no
    // fixed arity, so is_record/3 above does not apply and the tag has to
    // be read with element/2.
    //
    // A `case`, NOT `erlang:and(is_tuple(V), element(1,V) =:= Ty)`. That
    // conjunction is STRICT — `element/2` runs even when V is a binary —
    // and on OTP < 28 `erlc +from_core` lets the resulting badarg ESCAPE
    // the guard instead of failing the clause. So `"${s}"` on a plain
    // String crashed wherever Optional or Result were in scope, which is
    // most programs; OTP 28 merely masks it. `is_record/2` would say this
    // directly but is not a legal Core Erlang guard, unlike is_record/3.
    auto vRef = snap(v);
    return matchBool(
        callE("erlang", "is_tuple", 1, one(vRef.get())),
        intrin(Op::Eq, two(callE("erlang", "element", 2,
                                 two(litInt(1), vRef.get())),
                           lit(LitKind::Atom, tagged))),
        litBool(false));
}

// Registers `moduleAliases` entries for a `using M`, so a later
// `Name.method(…)` call site (see the `moduleAliases.find(path.front())`
// lookup above) can resolve a bare module segment against it. Mirrors
// `TypeChecker::resolveModulePath` (kexhq/kex#229, kexhq/kex#233):
//
//   1. `M`'s own last segment aliases to `M` — `using Data.Set` aliases
//      bare `Set`, since the nested `module Set do … end` constructor
//      block that provides `Data.Set` is what the bare receiver means.
//   2. An immediate child `M.C` aliases to itself under the bare name
//      `C` — `using Net.HTTP` aliases bare `Status` to `Net.HTTP.Status`.
//   3. A SIBLING of `M` (same immediate parent, found by scanning every
//      known module for one) aliases to itself too — `Data.UnorderedSet`
//      shares a file, and a `module Data` header, with `Data.Set`, but is
//      its sibling, not its child, so `using Data.Set` alone would
//      otherwise leave `UnorderedSet.from(…)` unresolved.
//
// `moduleFunctions` already holds every module this compilation unit has
// seen (populated by the pre-pass before any body is lowered), so this
// scan sees `Data.UnorderedSet` even though only `Data.Set` was named.
auto Lowering::registerModuleAliases(const std::string& srcMod,
                           const std::vector<std::string>& onlyNames,
                           const std::vector<std::string>& exceptNames)
    -> void {
    auto allowed = [&](const std::string& name) {
        return kex::semantic::resolve_core::usingSelectsName(
            name, onlyNames, exceptNames);
    };
    std::unordered_set<std::string> moduleNames;
    for (const auto& [key, _] : moduleFunctions) {
        const auto lastDot = key.rfind('.');
        if (lastDot != std::string::npos)
            moduleNames.insert(key.substr(0, lastDot));
    }
    // A module whose own last segment names a SAME-NAMED nested
    // constructor block (`Net.HTTP.WebSocket`'s `module WebSocket do
    // ... end`, giving `Net.HTTP.WebSocket.WebSocket`) has no direct
    // members of its own in that shape — bare `WebSocket.connect(…)`
    // means the child, not the header. Prefer that child wherever it
    // exists, for both `M` itself and any sibling this function aliases.
    auto resolveTarget = [&](const std::string& candidate) {
        const auto childDot = candidate.rfind('.');
        const auto candidateLeaf = childDot == std::string::npos
            ? candidate : candidate.substr(childDot + 1);
        const auto sameNamedChild = candidate + "." + candidateLeaf;
        return moduleNames.count(sameNamedChild) ? sameNamedChild : candidate;
    };
    const auto dot = srcMod.rfind('.');
    const auto leaf = dot == std::string::npos ? srcMod : srcMod.substr(dot + 1);
    const auto parent = dot == std::string::npos ? std::string() : srcMod.substr(0, dot);
    if (allowed(leaf)) moduleAliases.try_emplace(leaf, resolveTarget(srcMod));
    const auto childPrefix = srcMod + ".";
    for (const auto& modulePath : moduleNames) {
        if (modulePath.rfind(childPrefix, 0) == 0) {
            // Immediate child of M only — a grandchild still needs its
            // own qualification.
            const auto rest = modulePath.substr(childPrefix.size());
            if (rest.find('.') == std::string::npos && allowed(rest))
                moduleAliases.try_emplace(rest, modulePath);
        } else if (!parent.empty() && modulePath != srcMod) {
            const auto modDot = modulePath.rfind('.');
            if (modDot != std::string::npos &&
                modulePath.compare(0, modDot, parent) == 0 &&
                modDot == parent.size()) {
                const auto siblingLeaf = modulePath.substr(modDot + 1);
                if (allowed(siblingLeaf))
                    moduleAliases.try_emplace(siblingLeaf, resolveTarget(modulePath));
            }
        }
    }
}

auto Lowering::makeArgumentDispatcher(
    const std::string& name, int arity,
    const std::vector<std::pair<std::string, std::vector<std::string>>>&
        overloads,
    const std::string& fallbackModule ) -> FunDef {
    FunDef def;
    def.name = name;
    def.arity = arity;
    for (const auto& [target, types] : overloads) {
        if (static_cast<int>(types.size()) != arity) continue;
        FunClause clause;
        std::vector<ExprPtr> forwarded;
        ExprPtr guard;
        for (int i = 0; i < arity; ++i) {
            auto param = std::make_unique<Pattern>();
            param->kind = PatKind::Var;
            param->name = "_a" + std::to_string(i);
            clause.params.push_back(std::move(param));
            forwarded.push_back(var("_a" + std::to_string(i)));
            // An untyped position (`Any`) admits every value; guarding
            // on it would compare the value's tag with 'Any'.
            const auto& written = types[static_cast<size_t>(i)];
            if (!discriminatingType(written)) continue;
            auto next = typeGuard(written, var("_a" + std::to_string(i)));
            guard = guard
                ? callE("erlang", "and", 2,
                        two(std::move(guard), std::move(next)))
                : std::move(next);
        }
        if (guard) clause.guard = std::move(guard);
        auto body = std::make_unique<Expr>();
        body->node = Call{"", target, arity, std::move(forwarded), false};
        clause.body = std::move(body);
        def.clauses.push_back(std::move(clause));
    }
    if (!fallbackModule.empty()) {
        FunClause fallback;
        std::vector<ExprPtr> args;
        for (int i = 0; i < arity; ++i) {
            auto param = std::make_unique<Pattern>();
            param->kind = PatKind::Var;
            param->name = "_a" + std::to_string(i);
            fallback.params.push_back(std::move(param));
            args.push_back(var("_a" + std::to_string(i)));
        }
        fallback.body = callE(fallbackModule, name, arity,
                              std::move(args));
        def.clauses.push_back(std::move(fallback));
    }
    return def;
}

// `genericFallbackArity` is the arity the fallback is called at, which is
// NOT the dispatcher's when the method is `foul`: every foul function
// carries a hidden capability context as its last parameter, and the
// prelude function being delegated to has no such parameter. Passing the
// context along handed it to the prelude as a user argument — `[..].count`
// reached `kex_prelude:count/2`, the `count(pred)` overload, with the
// context where the predicate goes, and died `badarg` (kexhq/kex#143).
//
// `contextualOwners` says the same thing about the DISPATCHER's own
// arity: its last parameter is that hidden context rather than a user
// argument. The two places that care are the ones which read the call as
// something other than a method call — a record field read takes no
// arguments at all, and a field holding a function takes only the user's.
// Without this a `foul routes` on one type turned every OTHER type's
// `routes` FIELD read into a one-argument call and died with "Undefined
// method: routes for Net.HTTP.Router" (kexhq/kex#242).
auto Lowering::makeDispatcher(const std::string& name, int arity,
                    const std::vector<std::string>& owners,
                    const std::string& genericFallback ,
                    const std::string& genericFallbackModule ,
                    int genericFallbackArity ,
                    bool contextualOwners ) -> FunDef {
    if (genericFallbackArity < 0) genericFallbackArity = arity;
    // The arity the CALL was written at, context excluded.
    const int callArity = contextualOwners ? arity - 1 : arity;
    FunDef def; def.name = name; def.arity = arity;
    def.isCollisionDispatcher = true;
    FunClause fc;
    std::vector<ExprPtr> fwdArgs;
    for (int i = 0; i < arity; i++) {
        auto pat = std::make_unique<Pattern>();
        pat->kind = PatKind::Var; pat->name = "_a" + std::to_string(i);
        fc.params.push_back(std::move(pat));
        fwdArgs.push_back(var("_a" + std::to_string(i)));
    }
    // Dispatch on the receiver's runtime type via a guard, so it works for
    // both tagged records/variants AND primitive types (Integer/Float/Map/
    // String/…), whose values aren't tagged tuples — `element(1, N)` on a
    // raw number would crash. For a record type T the guard is
    // `is_tuple(V) and element(1,V) == 'T'`; for a primitive it's the
    // matching `is_*` BIF (all guard-safe).
    Match m;
    m.subjects.push_back(var("_a0"));
    // Primitive-type guards (is_list, is_binary, …) are always safe.
    // Record/variant guards use erlang:and(is_tuple(V), element(1,V)==Tag)
    // which evaluates element/2 eagerly and crashes on non-tuples. Emit
    // safe clauses first so a primitive receiver matches before reaching
    // an element-based guard.
    std::vector<std::string> sortedOwners;
    std::vector<std::string> deferredOwners;
    std::vector<std::string> traitFallbacks;
    std::unordered_set<std::string> seen;
    for (const auto& ty : owners) {
        if (!seen.insert(ty).second) continue;
        // Built after the module walk: no scope says where `ty` was
        // written, so a trait one module declares is found by its name.
        if (!traitIdentityOf(ty, "", true).empty())
            traitFallbacks.push_back(ty);
        else if (primGuardBifs().count(ty) || ty == "Char"
            || (typeVariantTags.count(ty) && !typeVariantTags.at(ty).empty()))
            sortedOwners.push_back(ty);
        else
            deferredOwners.push_back(ty);
    }
    // When two types share the same BIF guard, the first clause
    // shadows the second.  Keep the more general type and drop the
    // shadowed one — its implementation is reachable through the
    // general type's method anyway (e.g. Range methods materialise
    // via .items and re-enter as a List).
    auto dropShadowed = [&](const std::string& keep,
                            const std::string& drop) {
        if (std::find(sortedOwners.begin(), sortedOwners.end(), keep) != sortedOwners.end()) {
            auto it = std::find(sortedOwners.begin(), sortedOwners.end(), drop);
            if (it != sortedOwners.end()) sortedOwners.erase(it);
        }
    };
    dropShadowed("List", "Range");
    dropShadowed("Task", "Pid");
    // `is_number` also answers true for an Integer and for a Float, so a
    // `make Number` block placed first would shadow the concrete ones.
    // Move the broader type after the narrower ones it subsumes, which is
    // the concrete-type-wins rule the interpreter's resolveMethodName
    // applies through dispatchSupertypes.
    auto deferBroader = [&](const std::string& broad,
                            std::initializer_list<const char*> narrower) {
        auto broadIt = std::find(sortedOwners.begin(), sortedOwners.end(), broad);
        if (broadIt == sortedOwners.end()) return;
        size_t broadIdx = static_cast<size_t>(broadIt - sortedOwners.begin());
        size_t lastNarrow = 0;
        bool found = false;
        for (const char* n : narrower) {
            auto it = std::find(sortedOwners.begin(), sortedOwners.end(), n);
            if (it == sortedOwners.end()) continue;
            found = true;
            lastNarrow = std::max(lastNarrow,
                                  static_cast<size_t>(it - sortedOwners.begin()));
        }
        if (!found || broadIdx > lastNarrow) return;
        sortedOwners.erase(sortedOwners.begin() + static_cast<long>(broadIdx));
        sortedOwners.insert(sortedOwners.begin() + static_cast<long>(lastNarrow),
                            broad);
    };
    deferBroader("Number", {"Integer", "Float"});
    // `is_atom` also answers for a Bool, `None` and a nullary variant,
    // so an `Atom` clause goes after every other primitive and ADT
    // owner. Records and payload variants are tuples; they cannot match
    // it wherever it sits.
    if (auto atom = std::find(sortedOwners.begin(), sortedOwners.end(), "Atom");
        atom != sortedOwners.end()) {
        sortedOwners.erase(atom);
        sortedOwners.push_back("Atom");
    }
    // `is_tuple` is the widest guard in the family: a record, a variant
    // with a payload and a Char are all Erlang tuples. Left among the
    // primitives, a `make Tuple` clause shadowed every record sharing the
    // method's name — `unorderedSet.items` matched Tuple's `items` and
    // came back as `tuple_to_list` of the record itself (kexhq/kex#207).
    // Held back here and re-emitted after the record field clauses, which
    // is the last position anything can occupy. `deferBroader` cannot
    // express that: the clauses Tuple has to sink below are not owners at
    // all.
    bool hasTupleOwner = false;
    if (auto tuple = std::find(sortedOwners.begin(), sortedOwners.end(),
                               "Tuple");
        tuple != sortedOwners.end()) {
        hasTupleOwner = true;
        sortedOwners.erase(tuple);
    }
    sortedOwners.insert(sortedOwners.end(), deferredOwners.begin(), deferredOwners.end());
    // A plain generic function is already the dynamic fallback. A trait
    // receiver guard is unconditional at runtime and would make that
    // fallback unreachable (and turn a non-matching type-directed clause
    // inside the trait implementation into function_clause).
    if (genericFallback.empty())
        sortedOwners.insert(sortedOwners.end(), traitFallbacks.begin(),
                            traitFallbacks.end());
    for (const auto& ty : sortedOwners) {
        // ADT types with known variant tags: generate one pattern-match
        // clause per variant instead of a single guard clause. This avoids
        // Core Erlang's strict erlang:and/or in guards (element/2 throws
        // badarg on non-tuples) and uses the correct variant tags rather
        // than the type name (e.g. 'Just' not 'Optional').
        auto vt = typeVariantTags.find(ty);
        if (vt != typeVariantTags.end() && !vt->second.empty()) {
            for (const auto& tag : vt->second) {
                MatchClause mc;
                // Nullary variant (no payload) → bare atom on BEAM.
                if (nullaryVariantTags.count(tag)) {
                    auto pat = atomPattern(tag);
                    mc.patterns.push_back(std::move(pat));
                } else {
                    // Payload variant: tuple pattern {Tag, _, ...}, one
                    // wildcard per field. A fixed {Tag, _} matched only
                    // one-field constructors, so `Node(v, l, r)` fell
                    // through to the next owner's clause.
                    auto pat = std::make_unique<Pattern>();
                    pat->kind = PatKind::Tuple;
                    auto tagPat = atomPattern(tag);
                    pat->args.push_back(std::move(tagPat));
                    const auto fields = variantArity.count(tag)
                        ? std::max(1, variantArity.at(tag)) : 1;
                    for (int field = 0; field < fields; ++field) {
                        auto wild = std::make_unique<Pattern>();
                        wild->kind = PatKind::Wild;
                        pat->args.push_back(std::move(wild));
                    }
                    mc.patterns.push_back(std::move(pat));
                }
                std::vector<ExprPtr> args;
                for (int i = 0; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
                auto call = std::make_unique<Expr>();
                call->node = Call{"",
                    mangleReceiverImplementation(name, ty),
                    arity, std::move(args), false};
                mc.body = std::move(call);
                m.clauses.push_back(std::move(mc));
            }
        } else if (ty == "Char") {
            // A Char is always the 2-tuple {'Char', _} — match it
            // structurally. (An element(1,_) guard would badarg, not
            // soft-fail, for a primitive receiver reaching this clause.)
            MatchClause mc;
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Tuple;
            auto tagPat = atomPattern("Char");
            pat->args.push_back(std::move(tagPat));
            auto wild = std::make_unique<Pattern>();
            wild->kind = PatKind::Wild;
            pat->args.push_back(std::move(wild));
            mc.patterns.push_back(std::move(pat));
            std::vector<ExprPtr> args;
            for (int i = 0; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
            auto call = std::make_unique<Expr>();
            call->node = Call{"",
                mangleReceiverImplementation(name, ty),
                arity, std::move(args), false};
            mc.body = std::move(call);
            m.clauses.push_back(std::move(mc));
        } else {
            // Primitive/record type: use a guard-based clause.
            MatchClause mc;
            auto gv = std::make_unique<Pattern>();
            gv->kind = PatKind::Var; gv->name = "_gv";
            mc.patterns.push_back(std::move(gv));
            mc.guard = typeGuard(ty, var("_gv"));
            std::vector<ExprPtr> args;
            for (int i = 0; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
            auto call = std::make_unique<Expr>();
            call->node = Call{"",
                mangleReceiverImplementation(name, ty),
                arity, std::move(args), false};
            mc.body = std::move(call);
            m.clauses.push_back(std::move(mc));
        }
    }
    // [Char] IS String: a method owned by List but not by String must
    // still take a String receiver — coerce the binary to its codepoint
    // list and forward to the List impl. This clause goes FIRST: the
    // record/variant owner clauses guard with element(1, _) which erlc
    // does not soft-fail for a binary falling through to them.
    bool hasString = std::find(owners.begin(), owners.end(), "String") != owners.end();
    bool hasList = std::find(owners.begin(), owners.end(), "List") != owners.end();
    if (hasList && !hasString) {
        MatchClause mc;
        auto gv = std::make_unique<Pattern>();
        gv->kind = PatKind::Var; gv->name = "_gv";
        mc.patterns.push_back(std::move(gv));
        mc.guard = callE("erlang", "is_binary", 1, one(var("_gv")));
        std::vector<ExprPtr> args;
        args.push_back(callE("kex_intrinsic_list", "as_list", 1, one(var("_a0"))));
        for (int i = 1; i < arity; i++) args.push_back(var("_a" + std::to_string(i)));
        auto call = std::make_unique<Expr>();
        call->node = Call{"",
            mangleReceiverImplementation(name, "List"),
            arity, std::move(args), false};
        mc.body = std::move(call);
        m.clauses.insert(m.clauses.begin(), std::move(mc));
    }
    appendFieldClauses(name, callArity, m);
    // Tuple last, for the reason its extraction above records: it accepts
    // every record and variant, so it is only ever the right answer once
    // the types that actually own the name have declined.
    if (hasTupleOwner) {
        MatchClause mc;
        auto gv = std::make_unique<Pattern>();
        gv->kind = PatKind::Var; gv->name = "_gv";
        mc.patterns.push_back(std::move(gv));
        mc.guard = typeGuard("Tuple", var("_gv"));
        std::vector<ExprPtr> args;
        for (int i = 0; i < arity; i++)
            args.push_back(var("_a" + std::to_string(i)));
        auto call = std::make_unique<Expr>();
        call->node = Call{"", mangleReceiverImplementation(name, "Tuple"),
                          arity, std::move(args), false};
        mc.body = std::move(call);
        m.clauses.push_back(std::move(mc));
    }
    // A receiver no clause covers is a call that should not have compiled
    // — `Date.of(...).iso`, where `iso` belongs to Date and the receiver
    // is a Result. The checker rejects that now, but `--no-check` and
    // gradual code still reach here, and falling off the end raised a
    // bare `if_clause`: no method name, no receiver, no location. Name it
    // instead, in the walker's wording, with the receiver's runtime type.
    if (!genericFallback.empty()) {
        MatchClause fallback;
        auto wild = std::make_unique<Pattern>();
        wild->kind = PatKind::Wild;
        fallback.patterns.push_back(std::move(wild));
        std::vector<ExprPtr> args;
        for (int i = 0; i < genericFallbackArity; ++i)
            args.push_back(var("_a" + std::to_string(i)));
        fallback.body = callE(genericFallbackModule, genericFallback,
                              genericFallbackArity, std::move(args));
        m.clauses.push_back(std::move(fallback));
    } else if (traitFallbacks.empty()) {
        MatchClause fallback;
        auto wild = std::make_unique<Pattern>();
        wild->kind = PatKind::Wild;
        fallback.patterns.push_back(std::move(wild));
        // Last resort before raising: a record field may hold a function,
        // and `d.fetch(url)` should call it. The method keeps precedence,
        // which is exactly what reaching this fallback means — no clause
        // matched the receiver. The walker's dispatch does the same
        // (kexhq/kex#176). The field is read by name at runtime because
        // the reason this call is unresolved is that lowering does not
        // know the receiver's type.
        std::vector<ExprPtr> fieldArgs;
        for (int i = 1; i < callArity; i++)
            fieldArgs.push_back(var("_a" + std::to_string(i)));
        auto fieldArgList = std::make_unique<Expr>();
        fieldArgList->node = MakeList{std::move(fieldArgs), std::nullopt};
        std::vector<ExprPtr> rescueArgs;
        rescueArgs.push_back(var("_a0"));
        rescueArgs.push_back(lit(LitKind::String, name));
        rescueArgs.push_back(std::move(fieldArgList));
        rescueArgs.push_back(lit(LitKind::String, ""));
        fallback.body = callE("kex_io", "call_field_or_fail", 4,
                              std::move(rescueArgs));
        m.clauses.push_back(std::move(fallback));
    }
    // Where a value of each owner's type lands: its own implementation when
    // no arm before its own can take it, in the order the arms were just
    // laid out — the String-to-List coercion first, then the owners, the
    // record field reads, and Tuple last. A trait's arm takes anything.
    {
        std::vector<std::pair<std::string, std::optional<std::set<std::string>>>> arms;
        if (hasList && !hasString)
            arms.push_back({{}, std::set<std::string>{"binary"}});
        for (const auto& ty : sortedOwners)
            arms.push_back({ty, std::find(traitFallbacks.begin(), traitFallbacks.end(),
                                          ty) != traitFallbacks.end()
                                    ? std::nullopt
                                    : runtimeShapes(ty)});
        if (callArity == 1)
            if (auto fields = fieldAccessors.find(name); fields != fieldAccessors.end())
                for (const auto& slot : fields->second)
                    arms.push_back({{}, std::set<std::string>{"tuple:" + slot.record}});
        if (hasTupleOwner) arms.push_back({"Tuple", runtimeShapes("Tuple")});
        for (std::size_t i = 0; i < arms.size(); ++i) {
            const auto& [owner, shapes] = arms[i];
            if (owner.empty() || !shapes) continue;
            bool reached = true;
            for (std::size_t j = 0; j < i && reached; ++j)
                if (arms[j].first != owner && shapesMeet(arms[j].second, shapes))
                    reached = false;
            if (reached)
                fc.routes.push_back(mangleReceiverImplementation(name, owner));
        }
    }
    auto body = std::make_unique<Expr>();
    body->node = std::move(m);
    fc.body = std::move(body);
    def.clauses.push_back(std::move(fc));
    return def;
}

// [a, b] as an IR list, for building an iolist at runtime.
auto Lowering::makeListOf(ExprPtr first, ExprPtr second) -> ExprPtr {
    std::vector<ExprPtr> elements;
    elements.push_back(std::move(first));
    elements.push_back(std::move(second));
    auto list = std::make_unique<Expr>();
    list->node = MakeList{std::move(elements), std::nullopt};
    return list;
}

// A record field can share its name with a receiver method — `rest` is a
// field of the prelude's ParseError and a method on List/String. The
// standalone `'rest'/1` accessor is suppressed in that case (makeAccessors)
// because it would shadow the method and send `[1,2,3].rest` to a tuple
// read, which leaves the dispatcher as the only `rest/1` in the module.
// Without these clauses it has no arm matching a record, so `err.rest`
// died with `if_clause` on BEAM while working fine in the walker.
//
// Appended last so a genuine method for the receiver's type always wins;
// these only catch records that would otherwise fall off the end.
auto Lowering::appendFieldClauses(const std::string& name, int arity, Match& m) -> void {
    if (arity != 1) return;
    auto it = fieldAccessors.find(name);
    if (it == fieldAccessors.end()) return;
    for (const auto& slot : it->second) {
        MatchClause mc;
        auto gv = std::make_unique<Pattern>();
        gv->kind = PatKind::Var; gv->name = "_fv";
        mc.patterns.push_back(std::move(gv));
        mc.guard = recordGuard(slot, var("_fv"));
        mc.body = callE("erlang", "element", 2,
                        two(litInt(slot.position), var("_a0")));
        m.clauses.push_back(std::move(mc));
    }
}

} // namespace kex::ir

namespace kex::ir {

auto Lowering::collidingImplementationName(const std::string& method,
                                           const std::string& owner,
                                           size_t sourceArity) const
    -> std::optional<std::string> {
    if (owner.empty() || !collidingMethods.count(method) ||
        argumentOverloadedMethods.count(
            localOverloadKey(method, owner, sourceArity)))
        return std::nullopt;
    return mangleReceiverImplementation(method, owner);
}

} // namespace kex::ir

namespace kex::ir {

auto Lowering::takesPlainParameters(const std::string& method,
                                    const std::string& owner) const -> bool {
    auto found = makeMethods.find(method);
    if (found == makeMethods.end()) return false;
    const auto canonical = canonicalRecordName(owner);
    bool any = false;
    for (const auto& candidate : found->second) {
        if (!candidate.def || canonicalRecordName(candidate.owner) != canonical)
            continue;
        any = true;
        for (const auto& clause : candidate.def->clauses)
            for (const auto& param : clause.params)
                if (param.pattern || !param.name) return false;
    }
    return any;
}

} // namespace kex::ir
