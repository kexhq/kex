// Expression lowering methods of Lowering, out of line (see lowering.hxx).
#include "lowering.hxx"

namespace kex::ir {

auto Lowering::lower(const ast::ExprPtr& e) -> ExprPtr {
    if (!e) return litBool(false);
    if (auto found = loweredReceivers.find(e.get()); found != loweredReceivers.end())
        return var(found->second);
    // A type named as a value (`String`, `App.Models.User`) is its `Type`
    // record, as the checker typed it: what a type-name parameter matches.
    if (hir)
        if (auto value = hir->typeValueOf(*e)) return structuredTypeLiteral(*value);
    auto prevLoc = currentLoc;
    currentLoc = &e->location;
    auto r = std::visit([&](const auto& n) -> ExprPtr {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ast::IntLiteral>) {
            return lit(LitKind::Int, n.value);
        } else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
            return lit(LitKind::Float, n.value);
        } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            // The parser splits every interpolating literal, so a
            // parts-less one is plain text (including a `compiled`-reified
            // string, which is marked non-interpolating precisely so its
            // `${...}` is NOT re-expanded).
            if (!n.parts.empty())
                return lowerParsedInterpolatedString(n.parts, n.values);
            return lit(LitKind::String, n.value);
        } else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
            return litBool(n.value);
        } else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
            return lit(LitKind::Char, utf8::encode(n.value));
        } else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
            return lit(LitKind::Atom, n.name);
        } else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
            return lit(LitKind::None, "none");
        } else if constexpr (std::is_same_v<T, ast::Identifier>) {
            // A bare reference to a top-level `let` constant (not shadowed
            // by a local) is a call to its 0-arity function. A MODULE's own
            // constant counts: `let EARLIEST_YEAR = 1450` inside `module
            // Draft` is `Draft.EARLIEST_YEAR/0`, and a sibling function
            // referring to it unqualified must reach that, not the
            // constructor fallback below (rodolfo docs/kex-issues.md #14,
            // kexhq/kex#282).
            if (!subst.count(n.name)) {
                if (topLevelConstants.count(n.name) ||
                    zeroArgFns.count(n.name)) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = prefixCall(n.name, {});
                    return ex;
                }
                if (auto symbol = moduleConstantSymbol(n.name);
                    !symbol.empty()) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = localCall(symbol, {});
                    return ex;
                }
                // A bare reference inside a `make` method to one of that
                // block's own PARAMETERLESS methods — the identifier form
                // of the implicit-receiver call rule: `seed` means
                // `this.seed` exactly as `decorate(x)` means
                // `this.decorate(x)`. A LOCAL binding of the same name
                // always wins (the guard above), as does a zero-arity
                // function — an ordinary `let seed = …` constant. Ahead of
                // the constructor fallback and `knownFns` (a name-only set
                // holding every make method) (kexhq/kex#292).
                if (!currentMakeType.empty() && subst.count("this") &&
                    !definedFnArities.count(n.name + "/0") &&
                    definedFnArities.count(n.name + "/1") &&
                    ownsMethod(currentMakeType, n.name)) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = localCall(n.name,
                                         one(var(currentName("this"))));
                    return ex;
                }
                // An IMPORTED module constant under its bare name: `using
                // Limits, only: [LIMIT]` then a bare `LIMIT` is a call to
                // the constant, not a constructor (kexhq/kex#282). The
                // currently-lowered function is excluded — a module
                // pre-imports its OWN members too, so `let Monday = Monday`
                // would otherwise read its own constant and recurse
                // forever (spec/module_self_alias.kex).
                if (auto imp = moduleImports.find(n.name);
                    imp != moduleImports.end() &&
                    imp->second != currentFunctionSymbol &&
                    emittedModuleZeroArg.count(imp->second)) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = localCall(imp->second, {});
                    return ex;
                }
                if (auto constant = externalConstant(n.name)) return constant;
            }
            // Uppercase bare name not a known constant → nullary ADT
            // constructor (matching the UpperIdentifier path). Without
            // this, uppercase names in expression context become unbound
            // variables in Core Erlang and erlc rejects them.
            if (!n.name.empty() && std::isupper(static_cast<unsigned char>(n.name[0]))) {
                auto ex = std::make_unique<Expr>();
                ex->node = Construct{n.name, {}};
                return ex;
            }
            if (auto it = subst.find(n.name); it != subst.end())
                return var(it->second);
            // A bare reference to a known function used as a VALUE, not a
            // call (`check(addPair)`, no `~`) — there is no Core Erlang
            // variable bound for a top-level function, only a callable
            // symbol reachable via `apply`/`fun`. Eta-expand into an
            // actual fun value the same way `~name`'s bare-capture
            // partial-application branch (lowerCurry) already does for
            // the `~` spelling. Without this, `var(n.name)` emitted a
            // reference to a Core Erlang variable that was never bound in
            // this scope and erlc rejected it (kexhq/kex#350).
            if (auto pit = fnParamNames.find(n.name);
                knownFns.count(n.name) && pit != fnParamNames.end()) {
                Lambda lam;
                std::vector<ExprPtr> args;
                for (size_t i = 0; i < pit->second.size(); i++) {
                    std::string p = fresh("P");
                    lam.params.push_back(p);
                    args.push_back(var(p));
                }
                lam.body = std::make_unique<Expr>();
                lam.body->node = prefixCall(n.name, std::move(args));
                auto ex = std::make_unique<Expr>();
                ex->node = std::move(lam);
                return ex;
            }
            if (knownFns.count(n.name))
                return var(n.name);
            if (auto symbol = moduleConstantSymbol(n.name); !symbol.empty()) {
                auto ex = std::make_unique<Expr>();
                ex->node = localCall(symbol, {});
                return ex;
            }
            // Genuinely unbound: every binding site registers itself in
            // `subst`, so absence means the walker would raise at runtime —
            // emit the same error so `it`-caught failures match exactly.
            return runtimeError("Undefined variable: " + n.name);
        } else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
            return var(currentName("this"));
        } else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
            // An all-caps top-level constant (e.g. DEFAULT_LEVEL, defined
            // via `let NAME = value` → a 0-arity function) is a call, not a
            // nullary ADT constructor. A MODULE's own constant is the same
            // thing one scope in: without this, `"${EARLIEST_YEAR}"` inside
            // `module Draft` became the atom 'EARLIEST_YEAR' and died with
            // `Undefined function: EARLIEST_YEAR.showValue`, while the
            // walker read the binding (rodolfo docs/kex-issues.md #14,
            // kexhq/kex#282).
            // A local binding — a `let UPPER` constant — shadows any
            // same-named constructor or top-level constant, as on the
            // walker (kexhq/kex#603). Without this the reference below
            // decayed into the atom 'Name'.
            if (subst.count(n.name)) return var(currentName(n.name));
            else {
                if (topLevelConstants.count(n.name) ||
                    zeroArgFns.count(n.name)) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = localCall(n.name, {});
                    return ex;
                }
                if (auto symbol = moduleConstantSymbol(n.name);
                    !symbol.empty()) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = localCall(symbol, {});
                    return ex;
                }
                // An IMPORTED module constant under its bare name: `using
                // Limits, only: [LIMIT]` then a bare `LIMIT` is a call to
                // the constant, not a nullary ADT constructor — the
                // qualified spelling `Limits.LIMIT` always worked, and the
                // checker accepts the bare one, so both backends have to
                // agree with them (kexhq/kex#282). The currently-lowered
                // function is excluded: a module pre-imports its own
                // members, and `let Monday = Monday`'s right-hand side is
                // the outer variant, never the constant being defined
                // (spec/module_self_alias.kex).
                if (auto imp = moduleImports.find(n.name);
                    imp != moduleImports.end() &&
                    imp->second != currentFunctionSymbol &&
                    emittedModuleZeroArg.count(imp->second)) {
                    auto ex = std::make_unique<Expr>();
                    ex->node = localCall(imp->second, {});
                    return ex;
                }
                if (auto constant = externalConstant(n.name)) return constant;
            }
            // Bare capitalized name = nullary ADT constructor / None-like
            // tag → the atom 'Name' (e.g. JsonNull, Less).
            auto ex = std::make_unique<Expr>();
            if (auto tag = qualifiedVariantTag(n.name); !tag.empty()) {
                ex->node = Construct{tag, {}};
                return ex;
            }
            // A record named as a value (`first(as: Session)`) is its
            // record's own tag. The bare 'Session' was looked up by short
            // name at run time, which a same-named record elsewhere in
            // the program (Net.HTTP.WebSocket's) made ambiguous:
            // "Not a record type: Atom" (kexhq/kex#539).
            // Never ahead of a constructor this unit declares: `Name` in
            // `module People` is its `Detail.Name`, whatever record
            // `Directory.Name` the bare spelling also resolves to.
            if (auto record = canonicalRecordName(n.name);
                record != n.name && records.count(record) && !variantTagSet.count(n.name)) {
                ex->node = Construct{record, {}};
                return ex;
            }
            ex->node = Construct{n.name, {}};
            return ex;
        } else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            return lowerRecordConstruction(n);
        } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            // && / || MUST short-circuit: the right operand is evaluated
            // lazily, so it cannot be ANF-atomized (that would force it).
            // Lower to a boolean Match instead, matching the tree-walker
            // (spec/mutual_recursion.kex relies on `n == 0 || isOdd(n-1)`
            // never recursing once n == 0).
            //
            // A make block may overload them (kexhq/kex#507): an overload
            // builds a value from both sides, so it evaluates the right
            // one. It is taken only for a left value of a type declaring
            // it; any other left value keeps the short-circuit.
            if (n.op == TokenType::AmpAmp || n.op == TokenType::PipePipe) {
                const bool isAnd = n.op == TokenType::AmpAmp;
                const std::string sym = isAnd ? "&&" : "||";
                // An external union's value carries a variant tag, not
                // the union name. Its overloaded operator is known from
                // the static receiver type, so call that implementation
                // directly instead of treating the value as a Bool.
                if (externalModules && n.left) {
                    if (auto type = operatorOwner(n); !type.empty()) {
                        type.resize(std::min(type.size(), type.find('<')));
                        const auto bare = [](const std::string& name) {
                            return name.substr(name.rfind('.') + 1);
                        };
                        if (auto candidates = externalModules->receiverFunctions.find(sym);
                            candidates != externalModules->receiverFunctions.end())
                            for (const auto& candidate : candidates->second) {
                                if (candidate.beamArity != 2 ||
                                    candidate.receiverType.empty() ||
                                    (candidate.receiverType != type &&
                                     bare(candidate.receiverType) != bare(type)))
                                    continue;
                                std::vector<Binding> binds;
                                auto left = atomize(n.left, binds);
                                auto right = atomize(n.right, binds);
                                return wrapLets(binds,
                                    callE(candidate.moduleAtom, candidate.beamFunction,
                                          2, two(std::move(left), std::move(right))));
                            }
                    }
                }
                if (!overloadedOps.count(sym))
                    return isAnd
                        ? matchBool(lower(n.left), lower(n.right), litBool(false))
                        : matchBool(lower(n.left), litBool(true), lower(n.right));
                std::vector<Binding> binds;
                auto l = atomize(n.left, binds);
                auto lRef = snap(l);
                auto shortCircuit = isAnd
                    ? matchBool(lRef.get(), lower(n.right), litBool(false))
                    : matchBool(lRef.get(), litBool(true), lower(n.right));
                ExprPtr ownerGuard;
                if (auto owners = methodOwners.find(sym);
                    owners != methodOwners.end())
                    for (const auto& owner : owners->second) {
                        auto next = typeGuard(owner, lRef.get());
                        ownerGuard = ownerGuard
                            ? callE("erlang", "or", 2,
                                    two(std::move(ownerGuard), std::move(next)))
                            : std::move(next);
                    }
                if (!ownerGuard)
                    return wrapLets(binds, std::move(shortCircuit));
                std::string userModule;
                std::string userFunction = sym;
                if (!knownFns.count(sym) && externalModules)
                    if (auto it = externalModules->receiverFunctions.find(sym);
                        it != externalModules->receiverFunctions.end())
                        for (const auto& candidate : it->second)
                            if (candidate.beamArity == 2) {
                                userModule = candidate.moduleAtom;
                                userFunction = candidate.beamFunction;
                                break;
                            }
                const auto right = fresh("R");
                auto userCall = std::make_unique<Expr>();
                userCall->node = Call{std::move(userModule),
                                      std::move(userFunction), 2,
                                      two(lRef.get(), var(right)), false};
                return wrapLets(binds,
                    matchBool(std::move(ownerGuard),
                              makeLet(right, lower(n.right), std::move(userCall)),
                              std::move(shortCircuit)));
            }
            // Division by literal zero → compile-time error with location,
            // matching the walker's runtime error format.
            if (n.op == TokenType::Slash && n.right) {
                if (auto* il = std::get_if<ast::IntLiteral>(&n.right->kind)) {
                    if (il->value == "0") {
                        std::string loc;
                        if (currentLoc) loc = std::string(currentLoc->file) + ":"
                            + std::to_string(currentLoc->line) + ":"
                            + std::to_string(currentLoc->column) + ": ";
                        return callE("erlang", "error", 1, one(
                            lit(LitKind::String, loc + "runtime error: Division by zero")));
                    }
                }
            }
            std::vector<Binding> binds;
            auto l = atomize(n.left, binds);
            auto r = atomize(n.right, binds);
            // Operator overloading: if this operator is defined by a make
            // block, dispatch on the LHS at runtime — a tuple (record)
            // uses the user's `'op'/2`, anything else the builtin op
            // (spec/operator_overloading.kex).
            std::string sym = opSymbol(n.op);
            // A distinct type's own operator, picked by the static type:
            // the runtime guard below tests a record tag that an erased
            // `Tuplex` pair never carries, so `x + y` fell through to
            // the prelude's `+` and died with `function_clause`.
            if (!sym.empty() && n.left) {
                const std::string type = operatorOwner(n);
                const auto bare = type.substr(type.rfind('.') + 1);
                if (!type.empty() &&
                    (distinctTypes.count(type) || distinctTypes.count(bare)))
                    for (const auto& owner : {type, bare}) {
                        const auto owners = methodOwners.find(sym);
                        if (owners == methodOwners.end() ||
                            std::find(owners->second.begin(),
                                      owners->second.end(),
                                      owner) == owners->second.end())
                            continue;
                        auto implementation = collidingMethods.count(sym)
                            ? mangleReceiverImplementation(sym, owner)
                            : sym;
                        auto lRef = snap(l); auto rRef = snap(r);
                        auto call = std::make_unique<Expr>();
                        call->node = Call{std::string{}, std::move(implementation),
                                          2, two(lRef.get(), rRef.get()), false};
                        return wrapLets(binds, std::move(call));
                    }
            }
            // User == / != (outside guards, which only allow BIFs) go
            // through the coercing runtime eq: a [Char] charlist and a
            // String binary holding the same text ARE equal in Kex.
            auto builtinOp = [&](ExprPtr a, ExprPtr b) -> ExprPtr {
                Op op = opOf(n.op);
                if (op == Op::Eq || op == Op::Neq) {
                    // Two provably-identical exact-comparable operands
                    // (Integer, String or Char): Kex `==` is exact
                    // identity there (no charlist/string coercion, no
                    // Int/Float widening), so the raw BIF answers it and
                    // the 12-clause kex_intrinsic_number:eq/neq call is
                    // skipped on the hot path.
                    const auto lhs = exactEqualityType(n.left);
                    if (!lhs.empty() && lhs == exactEqualityType(n.right)) {
                        auto ex = std::make_unique<Expr>();
                        ex->node = Intrinsic{op, two(std::move(a), std::move(b))};
                        return ex;
                    }
                    return callE("kex_intrinsic_number",
                                 op == Op::Eq ? "eq" : "neq", 2,
                                 two(std::move(a), std::move(b)));
                }
                // `<`/`>`/`<=`/`>=` are Erlang's TOTAL term order on BEAM
                // but a typed comparison in the walker, which raises
                // "Cannot compare String and Int" where BEAM quietly
                // answered `false` — and the checker accepts `"x" < 1`,
                // so the divergence reaches checked programs.
                //
                // Route through the runtime check only when the operands
                // are not provably the same comparable kind; a numeric or
                // string comparison (the hot path) keeps the raw BIF.
                // `-` and `*` are raw Erlang ops, which answer
                // `badarith` for a float overflow and for a non-number
                // operand alike — where the walker says "Float
                // multiplication: result overflowed (Infinity)" or
                // "Cannot multiply Int and String". Provably-integer
                // operands keep the BIF (integer arithmetic cannot
                // overflow on BEAM); anything else takes the checked
                // runtime path, which reports what the walker reports.
                const bool arithmetic = op == Op::Sub || op == Op::Mul ||
                                        op == Op::Div || op == Op::Pow;
                if (arithmetic &&
                    !(isIntegerOperand(n.left) && isIntegerOperand(n.right))) {
                    std::string loc;
                    if (currentLoc)
                        loc = std::string(currentLoc->file) + ":"
                            + std::to_string(currentLoc->line) + ":"
                            + std::to_string(currentLoc->column) + ": ";
                    std::vector<ExprPtr> args;
                    args.push_back(std::move(a));
                    args.push_back(std::move(b));
                    args.push_back(lit(LitKind::String, loc));
                    return callE("kex_intrinsic_number",
                                 op == Op::Sub ? "sub"
                                 : op == Op::Mul ? "mul"
                                 : op == Op::Div ? "divide" : "pow",
                                 3, std::move(args));
                }
                if (isOrderingOp(op)) {
                    const auto kind = comparableKind(n.left);
                    if (kind.empty() || kind != comparableKind(n.right)) {
                        std::string loc;
                        if (currentLoc)
                            loc = std::string(currentLoc->file) + ":"
                                + std::to_string(currentLoc->line) + ":"
                                + std::to_string(currentLoc->column) + ": ";
                        std::vector<ExprPtr> args;
                        args.push_back(std::move(a));
                        args.push_back(std::move(b));
                        args.push_back(lit(LitKind::String, loc));
                        return callE("kex_intrinsic_number",
                                     orderingFunction(op), 3,
                                     std::move(args));
                    }
                }
                // `+` is polymorphic (numeric add / string concat) at
                // runtime; only provably-integer addition keeps the raw
                // BIF, which is exact there.
                if (op == Op::Add &&
                    !(isIntegerOperand(n.left) && isIntegerOperand(n.right)))
                    return callE("kex_intrinsic_number", "add", 2,
                                 two(std::move(a), std::move(b)));
                auto ex = std::make_unique<Expr>();
                ex->node = Intrinsic{op, two(std::move(a), std::move(b))};
                return ex;
            };
            // The test that `left OP right` reaches a local overload of
            // `opSym`: the receiver is a type that declares one AND the
            // argument is what that overload's parameter takes. Null when
            // no local make block declares it.
            auto overloadGuard = [&](const std::string& opSym,
                                     const auto& lRef, const auto& rRef) {
                ExprPtr guard;
                const auto methods = makeMethods.find(opSym);
                if (methods == makeMethods.end()) return guard;
                for (const auto& method : methods->second) {
                    if (!method.def || method.owner.empty()) continue;
                    for (const auto& clause : method.def->clauses) {
                        auto test = typeGuard(method.owner, lRef.get());
                        if (!clause.params.empty())
                            if (const auto* declared =
                                    dispatchParamType(clause.params.back())) {
                                const auto written = typeText(*declared, TypeSpelling::Dispatch);
                                if (discriminatingType(written))
                                    test = callE("erlang", "and", 2,
                                        two(std::move(test),
                                            typeGuard(written, rRef.get())));
                            }
                        guard = guard
                            ? callE("erlang", "or", 2,
                                    two(std::move(guard), std::move(test)))
                            : std::move(test);
                    }
                }
                return guard;
            };
            // `!=` where no `!=` overload applies is the negation of an
            // overloaded `==` wherever that one does, so `a != b` and
            // `!(a == b)` always agree; the checker allows it only where
            // that `==` answers a Bool (kexhq/kex#486). `otherwise` is
            // what is left: the built-in comparison.
            // `==` of a type of a unit compiled apart (`Data.Queue`'s,
            // which compares the elements, not how they are split), for a
            // left value of that type; `otherwise` for every other. With
            // `negate`, its `!=`.
            auto externalEquality = [&](const auto& lRef, const auto& rRef,
                                        ExprPtr otherwise, bool negate) -> ExprPtr {
                if (!externalModules) return otherwise;
                const auto providers = externalModules->receiverFunctions.find("==");
                if (providers == externalModules->receiverFunctions.end()) return otherwise;
                for (const auto& provider : providers->second) {
                    if (provider.beamArity != 2 || provider.receiverType.empty() ||
                        !provider.compiledApart)
                        continue;
                    auto equal = callE(provider.moduleAtom, provider.beamFunction, 2,
                                       two(lRef.get(), rRef.get()));
                    if (negate) equal = callE("erlang", "not", 1, one(std::move(equal)));
                    otherwise = matchBool(typeGuard(provider.receiverType, lRef.get()),
                                          std::move(equal), std::move(otherwise));
                }
                return otherwise;
            };
            auto derivedInequality = [&](const auto& lRef, const auto& rRef,
                                         ExprPtr otherwise) -> ExprPtr {
                if (!knownFns.count("=="))
                    return externalEquality(lRef, rRef, std::move(otherwise), true);
                auto guard = overloadGuard("==", lRef, rRef);
                if (!guard) return otherwise;
                auto equal = std::make_unique<Expr>();
                equal->node = Call{std::string{}, "==", 2,
                                   two(lRef.get(), rRef.get()), false};
                return matchBool(std::move(guard),
                                 callE("erlang", "not", 1, one(std::move(equal))),
                                 std::move(otherwise));
            };
            if (sym == "!=" && !overloadedOps.count("!=") &&
                overloadedOps.count("==")) {
                auto lRef = snap(l); auto rRef = snap(r);
                return wrapLets(binds, derivedInequality(
                    lRef, rRef, builtinOp(lRef.get(), rRef.get())));
            }
            // Two numbers, two strings or two chars, known to be so: the
            // built-in operator, with no dispatch. Comparable's `<`
            // (comparable.kex) made every `i < n` test `is_tuple(I)` and
            // then the Char tag before reaching `erlang:'<'`, as `+` had
            // since a make block first overloaded it.
            if (const auto kind = comparableKind(n.left);
                !kind.empty() && kind == comparableKind(n.right))
                return wrapLets(binds, builtinOp(std::move(l), std::move(r)));
            if (!sym.empty() && overloadedOps.count(sym)) {
                auto lRef = snap(l); auto rRef = snap(r);
                auto builtin = builtinOp(lRef.get(), rRef.get());
                std::vector<ExprPtr> ua; ua.push_back(lRef.get()); ua.push_back(rRef.get());
                std::string userModule;
                std::string userFunction = sym;
                // A locally-defined operator stays a local apply. When the
                // overload comes from an imported receiver interface
                // (notably the prelude), route it to that provider module.
                if (!knownFns.count(sym) && externalModules) {
                    if (auto it = externalModules->receiverFunctions.find(sym);
                        it != externalModules->receiverFunctions.end()) {
                        // The one for the left operand's static type when
                        // there is one: units compiled apart each provide
                        // their own (`Units.Dynamic`'s `/` and
                        // `Dimensions`'), and the first of them took
                        // every other type's values too.
                        const auto bare = [](std::string name) {
                            name.resize(std::min(name.size(), name.find('<')));
                            return name.substr(name.rfind('.') + 1);
                        };
                        const std::string left = bare(operatorOwner(n));
                        const ExternalModules::ReceiverFunction* chosen = nullptr;
                        for (const auto& candidate : it->second) {
                            if (candidate.beamArity != 2) continue;
                            if (!chosen) chosen = &candidate;
                            if (!left.empty() && candidate.compiledApart &&
                                !candidate.receiverType.empty() &&
                                bare(candidate.receiverType) == left) {
                                chosen = &candidate;
                                break;
                            }
                        }
                        if (chosen) {
                            userModule = chosen->moduleAtom;
                            userFunction = chosen->beamFunction;
                        }
                    }
                }
                auto userCall = std::make_unique<Expr>();
                userCall->node = Call{
                    std::move(userModule), std::move(userFunction),
                    2, std::move(ua), false};
                // Numeric receivers can overload an operator for a
                // nonnumeric RHS (for example, Integer > Measure).
                // Keep numeric/numeric pairs on the builtin path.
                ExprPtr numericOwnerGuard;
                auto addNumericOwner = [&](const std::string& owner) {
                    if (owner != "Integer" && owner != "Float" &&
                        owner != "Number")
                        return;
                    auto guard = typeGuard(owner, lRef.get());
                    numericOwnerGuard = numericOwnerGuard
                        ? callE("erlang", "or", 2,
                                two(std::move(numericOwnerGuard),
                                    std::move(guard)))
                        : std::move(guard);
                };
                if (auto owners = methodOwners.find(sym);
                    owners != methodOwners.end())
                    for (const auto& owner : owners->second)
                        addNumericOwner(owner);
                if (externalModules)
                    if (auto candidates = externalModules->receiverFunctions.find(sym);
                        candidates != externalModules->receiverFunctions.end())
                        for (const auto& candidate : candidates->second)
                            if (candidate.beamArity == 2)
                                addNumericOwner(candidate.receiverType);
                if (numericOwnerGuard)
                    numericOwnerGuard = callE("erlang", "and", 2,
                        two(std::move(numericOwnerGuard),
                            callE("erlang", "not", 1,
                                one(callE("erlang", "is_number", 1,
                                          one(rRef.get()))))));
                ExprPtr numericUserCall;
                if (numericOwnerGuard) {
                    const auto& call = std::get<Call>(userCall->node);
                    numericUserCall = std::make_unique<Expr>();
                    numericUserCall->node = Call{
                        call.module, call.name, 2,
                        two(lRef.get(), rRef.get()), false};
                }
                // Nullary ADT values such as `Watt` are atoms rather
                // than tuples. If their declared type owns this operator,
                // statically select that overload instead of falling
                // through to Erlang arithmetic.
                if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.left->kind)) {
                    if (auto owner = variantOwner.find(uid->name);
                        owner != variantOwner.end()) {
                        if (auto methods = methodOwners.find(sym);
                            methods != methodOwners.end() &&
                            std::find(methods->second.begin(), methods->second.end(),
                                      owner->second) != methods->second.end())
                            return wrapLets(binds, std::move(userCall));
                        // A type of a unit compiled apart, whose own
                        // operator its interface names by receiver.
                        const auto bare = [](const std::string& type) {
                            return type.substr(type.rfind('.') + 1);
                        };
                        if (externalModules)
                            if (auto candidates =
                                    externalModules->receiverFunctions.find(sym);
                                candidates != externalModules->receiverFunctions.end())
                                for (const auto& candidate : candidates->second)
                                    if (candidate.beamArity == 2 &&
                                        candidate.compiledApart &&
                                        !candidate.receiverType.empty() &&
                                        bare(candidate.receiverType) == bare(owner->second)) {
                                        auto call = std::make_unique<Expr>();
                                        call->node = Call{candidate.moduleAtom,
                                                          candidate.beamFunction, 2,
                                                          two(lRef.get(), rRef.get()),
                                                          false};
                                        return wrapLets(binds, std::move(call));
                                    }
                    }
                }
                // `==`/`!=` have a sensible structural fallback for any
                // tuple THIS operator's local override does not own —
                // `kex_intrinsic_number:eq` already answers correctly for
                // an unrelated record, ADT payload, or opt-in type
                // (`Just(1) == Just(1)`). Blindly routing every tuple to
                // the user's override once ANY local type overloads `==`
                // (`make Queue do let ==(other: Queue<A>)`) left every
                // OTHER tuple with no owner to fall back to — the
                // dispatcher only knows Queue, so `Optional<Integer>`'s `==`
                // died `Undefined method: == for Optional<Integer>` for a
                // comparison the language answers fine on its own
                // (kexhq/kex#235). Other operators keep the old path: they
                // have no such structural default, so the wider tuple
                // guard plus the callee's own external-provider fallback
                // is still the right way to reach an unrelated owner.
                const Op opKind = opOf(n.op);
                if (opKind == Op::Eq || opKind == Op::Neq) {
                    // An overload is reached only for an argument its
                    // parameter takes: `let ==(value: String) -> Cond`
                    // must leave two Cols to structural equality
                    // (kexhq/kex#486).
                    ExprPtr ownerGuard = overloadGuard(sym, lRef, rRef);
                    // No local make block declares it: the owners an
                    // imported interface names, by receiver alone.
                    if (auto owners = methodOwners.find(sym);
                        !ownerGuard && owners != methodOwners.end())
                        for (const auto& owner : owners->second) {
                            auto next = typeGuard(owner, lRef.get());
                            ownerGuard = ownerGuard
                                ? callE("erlang", "or", 2,
                                        two(std::move(ownerGuard),
                                            std::move(next)))
                                : std::move(next);
                        }
                    if (opKind == Op::Neq)
                        builtin = derivedInequality(lRef, rRef, std::move(builtin));
                    else if (!ownerGuard)
                        builtin = externalEquality(lRef, rRef, std::move(builtin), false);
                    auto dispatch = ownerGuard
                        ? matchBool(std::move(ownerGuard),
                                    std::move(userCall), std::move(builtin))
                        : std::move(builtin);
                    return wrapLets(binds, std::move(dispatch));
                }
                // A Char is `{'Char', N}` — a TUPLE, so the test above
                // sent every `Char` operand to the user operator, whose
                // dispatcher only knows the types that actually define one
                // (Measure, Duration) and answers anything else with
                // "Undefined method: + for Char". `x.upperCase + rest`
                // (spec/my_capitalize.kex) died there while the walker
                // concatenated, and `kex_intrinsic_number:add/2` had
                // handled every Char/String pairing all along — nothing
                // ever reached it. Chars take the builtin, like the
                // primitives they stand in for.
                auto charTag = callE("kex_intrinsic_number", "eq", 2,
                    two(callE("erlang", "element", 2,
                              two(lit(LitKind::Int, "1"), lRef.get())),
                        lit(LitKind::Atom, "Char")));
                auto dispatch = matchBool(
                    callE("erlang", "is_tuple", 1, one(lRef.get())),
                    matchBool(std::move(charTag),
                              builtinOp(lRef.get(), rRef.get()),
                              std::move(userCall)),
                    std::move(builtin));
                if (numericOwnerGuard)
                    dispatch = matchBool(std::move(numericOwnerGuard),
                                         std::move(numericUserCall),
                                         std::move(dispatch));
                return wrapLets(binds, std::move(dispatch));
            }
            return wrapLets(binds, builtinOp(std::move(l), std::move(r)));
        } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            std::vector<Binding> binds;
            auto a = atomize(n.operand, binds);
            Op op = (n.op == TokenType::Minus) ? Op::Neg
                   : (n.op == TokenType::Bang) ? Op::Not
                   : throw LowerError("IR lower: unsupported unary operator");
            auto ex = std::make_unique<Expr>();
            std::vector<ExprPtr> args;
            args.push_back(std::move(a));
            ex->node = Intrinsic{op, std::move(args)};
            return wrapLets(binds, std::move(ex));
        } else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            // a..b → a materialized ascending list. kex_intrinsic_range
            // wraps lists:seq and handles Char endpoints ({'Char', N}
            // bounds produce a [Char], not an [Int]).
            std::vector<Binding> binds;
            auto s = atomize(n.start, binds);
            auto en = atomize(n.end, binds);
            auto ex = std::make_unique<Expr>();
            std::vector<ExprPtr> args;
            args.push_back(std::move(s));
            args.push_back(std::move(en));
            ex->node = Call{"kex_intrinsic_range", "make", 2, std::move(args), false};
            return wrapLets(binds, std::move(ex));
        } else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            std::vector<Binding> binds;
            std::vector<ExprPtr> els;
            for (const auto& el : n.elements) els.push_back(atomize(el, binds));
            auto ex = std::make_unique<Expr>();
            ex->node = MakeTuple{std::move(els)};
            return wrapLets(binds, std::move(ex));
        } else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            std::vector<Binding> binds;
            bool hasSpreads = false;
            for (const auto& el : n.elements)
                if (el && std::holds_alternative<ast::SpreadExpr>(el->kind)) { hasSpreads = true; break; }

            if (!hasSpreads) {
                std::vector<ExprPtr> els;
                for (const auto& el : n.elements) els.push_back(atomize(el, binds));
                std::optional<ExprPtr> rest;
                if (n.rest) rest = atomize(*n.rest, binds);
                auto ex = std::make_unique<Expr>();
                ex->node = MakeList{std::move(els), std::move(rest)};
                return wrapLets(binds, std::move(ex));
            }
            // [a, ...b, c, ...d] → lists:append([a], lists:append(b, [c | d]))
            // Build segments: each run of non-spread elements forms a literal
            // list, each spread is its own list. Concatenate with lists:append.
            std::vector<ExprPtr> segments;
            std::vector<ExprPtr> currentRun;
            auto flushRun = [&]() {
                if (currentRun.empty()) return;
                auto lst = std::make_unique<Expr>();
                lst->node = MakeList{std::move(currentRun), std::nullopt};
                segments.push_back(std::move(lst));
                currentRun.clear();
            };
            for (const auto& el : n.elements) {
                if (el && std::holds_alternative<ast::SpreadExpr>(el->kind)) {
                    flushRun();
                    segments.push_back(lower(std::get<ast::SpreadExpr>(el->kind).inner));
                } else {
                    currentRun.push_back(atomize(el, binds));
                }
            }
            flushRun();
            // Fold right with lists:append
            ExprPtr result = std::move(segments.back()); segments.pop_back();
            while (!segments.empty()) {
                auto left = std::move(segments.back()); segments.pop_back();
                std::vector<ExprPtr> args;
                args.push_back(std::move(left));
                args.push_back(std::move(result));
                result = callE("lists", "append", 2, std::move(args));
            }
            return wrapLets(binds, std::move(result));
        } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            return lowerIf(n);
        } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            return lowerMatch(n);
        } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            // `expr if cond` → cond ? expr : None. The walker answers
            // None for the failed case, and the value is observable:
            // `let x = value if cond` binds it. Answering `:ok` here made
            // the same binding print `:ok` on BEAM and nothing in the
            // walker.
            return matchBool(lower(n.condition), lower(n.expr),
                             lit(LitKind::None, "none"));
        } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            return matchBool(lower(n.condition), lower(n.thenExpr), lower(n.elseExpr));
        } else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
            // `&.method` / `&.method(args)` → fun(_sx) -> _sx.method(args).
            return lowerNameAsUfcsFun(n.name, n.args);
        } else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            if (n.entries.empty()) return callE("maps", "new", 0, {});
            std::vector<Binding> binds;
            // Runs of literal pairs become one `maps:from_list`; each
            // `...other` is a map in its own right. Folding the segments
            // with `maps:merge` gives later-wins, since merge/2 lets its
            // SECOND argument take precedence.
            std::vector<ExprPtr> segments;
            std::vector<ExprPtr> pairs;
            auto flushPairs = [&]() {
                if (pairs.empty()) return;
                auto lst = std::make_unique<Expr>();
                lst->node = MakeList{std::move(pairs), std::nullopt};
                pairs.clear();
                segments.push_back(
                    callE("maps", "from_list", 1, one(std::move(lst))));
            };
            for (const auto& ent : n.entries) {
                if (ent.spread) {
                    flushPairs();
                    segments.push_back(atomize(ent.value, binds));
                    continue;
                }
                auto t = std::make_unique<Expr>();
                t->node = MakeTuple{two(atomize(ent.key, binds), atomize(ent.value, binds))};
                pairs.push_back(std::move(t));
            }
            flushPairs();
            auto merged = std::move(segments.front());
            for (size_t i = 1; i < segments.size(); i++)
                merged = callE("maps", "merge", 2,
                               two(std::move(merged), std::move(segments[i])));
            return wrapLets(binds, std::move(merged));
        } else if constexpr (std::is_same_v<T, ast::Lambda>) {
            // Params shadow outer bindings: resolve to themselves inside.
            auto snap = subst;
            Lambda lam;
            for (const auto& p : n.params) {
                // `_` is an ordinary variable in Core Erlang, so two of
                // them in one head (`{ |key, _, _| key }`) were a
                // duplicate variable and erlc refused the module. Each
                // one is a fresh name nothing reads.
                if (p.name == "_") {
                    lam.params.push_back(fresh("Ignored"));
                    continue;
                }
                lam.params.push_back(p.name);
                subst[p.name] = p.name;
            }
            if (n.collection) {
                auto singleton = [&](const ast::ExprPtr& item) {
                    std::vector<Binding> binds;
                    std::vector<ExprPtr> values;
                    values.push_back(atomize(item, binds));
                    auto list = std::make_unique<Expr>();
                    list->node = MakeList{std::move(values), std::nullopt};
                    return wrapLets(binds, std::move(list));
                };
                std::vector<ExprPtr> segments;
                for (const auto& item : n.body) {
                    if (!item) continue;
                    if (const auto* spread =
                            std::get_if<ast::SpreadExpr>(&item->kind)) {
                        segments.push_back(lower(spread->inner));
                    } else if (const auto* guarded =
                                   std::get_if<ast::TrailingIf>(
                                       &item->kind)) {
                        auto empty = std::make_unique<Expr>();
                        empty->node = MakeList{{}, std::nullopt};
                        segments.push_back(matchBool(
                            lower(guarded->condition),
                            singleton(guarded->expr), std::move(empty)));
                    } else {
                        segments.push_back(singleton(item));
                    }
                }
                if (segments.empty()) {
                    lam.body = std::make_unique<Expr>();
                    lam.body->node = MakeList{{}, std::nullopt};
                } else {
                    auto result = std::move(segments.back());
                    segments.pop_back();
                    while (!segments.empty()) {
                        auto left = std::move(segments.back());
                        segments.pop_back();
                        result = callE("lists", "append", 2,
                                       two(std::move(left),
                                           std::move(result)));
                    }
                    lam.body = std::move(result);
                }
            } else {
                lam.body = lowerBody(n.body);
            }
            // Same rule a named function gets: an unrescued `.try` failure
            // makes THIS body's result `Error(e)` rather than unwinding
            // past it. A lambda is a function, so `["no"].map { |v|
            // f(v).try }` yields `[Error(...)]` instead of crashing the
            // process — which is what the walker has always done, and what
            // `each` relies on to drop the error with the block's result.
            if (n.rescue)
                lam.body = wrapWithTryCatch(std::move(lam.body), *n.rescue,
                                            /*blockValue=*/!n.namedFunction);
            else if (irHasEscapingTryThrow(lam.body))
                lam.body = wrapPropagateTryError(std::move(lam.body));
            lam.ownReturnScope = n.namedFunction;
            subst = snap;
            auto ex = std::make_unique<Expr>();
            ex->node = std::move(lam);
            return ex;
        } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            // `return X if cond` (ReturnExpr wrapping a TrailingIf) gates
            // the return itself, not just its value: when cond is false it
            // is a statement that evaluates to None and control carries
            // on. The body walkers special-case it where a rest-of-body
            // exists; everywhere else — notably as the LAST statement of
            // an if/match branch — it reaches here, and lowering it as
            // `return (cond ? X : None)` returned None from the whole
            // function whenever the guard was not taken (kexhq/kex#408).
            if (n.value)
                if (const auto* ti = std::get_if<ast::TrailingIf>(&n.value->kind)) {
                    auto c = lower(ti->condition);
                    auto retX = std::make_unique<Expr>();
                    retX->node = Return{ti->expr ? lower(ti->expr)
                                                 : lit(LitKind::None, "none")};
                    return matchBool(std::move(c), std::move(retX),
                                     lit(LitKind::None, "none"));
                }
            auto ex = std::make_unique<Expr>();
            ex->node = Return{lower(n.value)};
            return ex;
        } else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
            return lowerCurry(n);
        } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            // A `do ... end` block as an expression (e.g. a match arm) is
            // just its body sequence, scoped so its bindings don't leak.
            return lowerBodyScoped(n.body);
        } else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            // spawn do B end → erlang:spawn(fun() -> B end).
            Lambda lam; lam.body = lowerBody(n.body);
            auto fn = std::make_unique<Expr>(); fn->node = std::move(lam);
            std::vector<Binding> binds;
            auto fnv = atomize_ir(std::move(fn), binds);
            return wrapLets(binds, callE("erlang", "spawn", 1, one(std::move(fnv))));
        } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            return lowerReceive(n);
        } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            return lowerFunctionCall(n);
        } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
            std::vector<ExprPtr> partItems;
            for (const auto& part : n.parts) {
                partItems.push_back(lit(LitKind::String, part));
            }
            auto parts = std::make_unique<Expr>();
            parts->node = MakeList{std::move(partItems), std::nullopt};

            std::vector<ExprPtr> valueItems;
            for (const auto& value : n.values) {
                if (value) valueItems.push_back(lower(value));
            }
            auto values = std::make_unique<Expr>();
            values->node = MakeList{std::move(valueItems), std::nullopt};

            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            args.push_back(atomize_ir(std::move(parts), binds));
            args.push_back(atomize_ir(std::move(values), binds));
            auto call = std::make_unique<Expr>();
            if (knownFns.count(n.tag))
                call->node = Call{"", n.tag, 2, std::move(args), false};
            else if (auto imp = moduleImports.find(n.tag);
                     imp != moduleImports.end())
                call->node =
                    Call{"", imp->second, 2, std::move(args), false};
            else if (auto ext = externalImports.find(n.tag);
                     !subst.count(n.tag) && ext != externalImports.end() &&
                     externalModules->compiledApartModules.contains(
                         ext->second.substr(0, ext->second.rfind('.')))) {
                // A tag imported from a unit compiled apart, as
                // lowerFunctionCall calls one (`using Regex`, regex`\d`).
                const auto& key = ext->second;
                call->node = Call{
                    externalModules->nameToAtom.at(key.substr(0, key.rfind('.'))),
                    externalModules->exportToBeamFn.at(key), 2, std::move(args),
                    false};
            } else if (subst.count(n.tag))
                call->node = CallIndirect{
                    var(currentName(n.tag)), std::move(args), false};
            else
                return wrapLets(
                    binds, runtimeError("Undefined function: " + n.tag));
            return wrapLets(binds, std::move(call));
        } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            return lowerMethodCall(n);
        } else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
            // A bare `using M, as: Alias` statement (no `do`) reaches
            // here with an EMPTY `n.body` — the parser only fills it for
            // the explicit block form. Lowering it standalone applied
            // the bindings and immediately restored them around nothing,
            // so a value constructed through the alias on the very next
            // statement was tagged with the alias's own name instead of
            // the module it names (kexhq/kex#alias). That statement form
            // is caught and handled by `lowerBodyFrom` before a body's
            // generic per-statement fallthrough ever reaches `lower()`,
            // so by the time a `UsingExpr` arrives here its body is
            // always the real, explicit scope to lower under the
            // bindings.
            auto saved = applyUsingBindings(n);
            auto result = lowerBody(n.body);
            restoreUsingBindings(std::move(saved));
            return result;
        } else if constexpr (std::is_same_v<T, ast::TryExpr>) {
            // .try desugars to: case operand of Ok(v) -> v; Error(e) -> return Error(e)
            auto subject = lower(n.operand);
            std::vector<Binding> binds;
            auto sAtom = atomize_ir(std::move(subject), binds);
            auto okVar = fresh("_try_ok");
            auto errVar = fresh("_try_err");

            auto mkConstructPat = [](const std::string& tag, std::vector<PatternPtr> args) {
                auto p = std::make_unique<Pattern>();
                p->kind = PatKind::Construct;
                p->tag = tag;
                p->args = std::move(args);
                return p;
            };
            auto mkVarPat = [](const std::string& name) {
                auto p = std::make_unique<Pattern>();
                p->kind = PatKind::Var;
                p->name = name;
                return p;
            };

            // Ok(v) -> v
            MatchClause okClause;
            std::vector<PatternPtr> okArgs; okArgs.push_back(mkVarPat(okVar));
            okClause.patterns.push_back(mkConstructPat("Ok", std::move(okArgs)));
            okClause.body = var(okVar);
            // Error(e) -> throw try error
            MatchClause errClause;
            std::vector<PatternPtr> errArgs; errArgs.push_back(mkVarPat(errVar));
            errClause.patterns.push_back(mkConstructPat("Error", std::move(errArgs)));
            auto throwExpr = std::make_unique<Expr>();
            throwExpr->node = TryThrow{var(errVar)};
            errClause.body = std::move(throwExpr);
            // Just(v) -> v
            auto justVar = fresh("_try_just");
            MatchClause justClause;
            std::vector<PatternPtr> justArgs; justArgs.push_back(mkVarPat(justVar));
            justClause.patterns.push_back(mkConstructPat("Just", std::move(justArgs)));
            justClause.body = var(justVar);
            // None -> throw try error(none)
            MatchClause noneClause;
            noneClause.patterns.push_back(mkConstructPat("None", {}));
            auto noneThrow = std::make_unique<Expr>();
            noneThrow->node = TryThrow{lit(LitKind::Atom, "none")};
            noneClause.body = std::move(noneThrow);
            // Native Optional-returning APIs may erase Just on success
            // and return the payload directly. Static checking guarantees
            // the operand is Optional/Result; preserve that ABI here.
            auto erasedVar = fresh("_try_erased");
            MatchClause erasedSuccessClause;
            erasedSuccessClause.patterns.push_back(mkVarPat(erasedVar));
            erasedSuccessClause.body = var(erasedVar);

            Match m;
            m.subjects.push_back(std::move(sAtom));
            m.clauses.push_back(std::move(okClause));
            m.clauses.push_back(std::move(justClause));
            m.clauses.push_back(std::move(errClause));
            m.clauses.push_back(std::move(noneClause));
            m.clauses.push_back(std::move(erasedSuccessClause));
            auto ex = std::make_unique<Expr>();
            ex->node = std::move(m);
            return wrapLets(binds, std::move(ex));
        } else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
            TryCatch tc;
            tc.body = lowerBody(n.body);
            tc.clauses = expandRescueGuards(lowerRescueClauses(n.rescue));
            auto ex = std::make_unique<Expr>();
            ex->node = std::move(tc);
            return ex;
        } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            // A loop in expression position — how the REPL evaluates a
            // bare `loop do ... end`, since it wraps the input in an
            // inspect call. It is its own last statement, so there is no
            // continuation to splice.
            requireNoOuterMutation(n.body, "loop");
            return lowerLoopCore(n.body, nullptr, true,
                                 []() -> ExprPtr { return nullptr; }, &n.counter);
        } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            requireNoOuterMutation(n.body, "while");
            return lowerLoopCore(n.body, &n.condition, true,
                                 []() -> ExprPtr { return nullptr; });
        } else if constexpr (std::is_same_v<T, ast::WithExpr>) {
            std::string capability = n.capability.dotted();
            // A replacement is a value put into the context, not state:
            // the body is lowered against the extended map, so there is
            // nothing to restore and an escaping error cannot leak it.
            std::vector<Binding> binds;
            auto replacement = atomize(n.value, binds);
            std::vector<ExprPtr> putArgs;
            putArgs.push_back(lit(LitKind::String, capability));
            putArgs.push_back(std::move(replacement));
            putArgs.push_back(currentContext());
            auto extended = fresh("Ctx");
            auto saved = currentContextVar;
            currentContextVar = extended;
            auto body = lowerBody(n.body);
            currentContextVar = saved;
            return wrapLets(
                binds,
                makeLet(extended,
                        callE("maps", "put", 3, std::move(putArgs)),
                        std::move(body)));
        } else {
            throw LowerError(std::string("IR lower: unimplemented expr node ")
                             + typeid(T).name());
        }
    }, e->kind);
    currentLoc = prevLoc;
    return r;
}

// Operator name (`~(+)`) → the intrinsic op it curries to. Mirrors the
// string emitter's opBif table (+ and / stay polymorphic via kex_io).
auto Lowering::curryOp(const std::string& name) -> std::optional<Op> {
    static const std::unordered_map<std::string, Op> t = {
        {"+",Op::Add},{"-",Op::Sub},{"*",Op::Mul},{"/",Op::Div},{"%",Op::Mod},{"^",Op::Pow},
        {"==",Op::Eq},{"!=",Op::Neq},{"<",Op::Lt},{"<=",Op::Lte},{">",Op::Gt},{">=",Op::Gte},
        {"&&",Op::And},{"||",Op::Or},
    };
    auto it = t.find(name); return it == t.end() ? std::nullopt : std::optional<Op>(it->second);
}

// `name` used as a bare function value, with no arity known statically.
// Routing through the UFCS method path means a builtin (`digit?`), a
// module-local function (`stringify`) and a local binding holding a fun
// all resolve with no special-casing here.
auto Lowering::lowerNameAsUfcsFun(const std::string& name,
                        const std::vector<ast::ExprPtr>& args) -> ExprPtr {
    // A LOCAL binding holding a fun IS the fun — pass it through, since
    // the UFCS path would instead look for a method `.name`.
    if (args.empty() && subst.count(name)) return var(currentName(name));

    std::string sx = fresh("Sx");
    auto recvAst = std::make_unique<ast::Expr>();
    recvAst->kind = ast::Identifier{sx};
    ast::MethodCall mc;
    mc.receiver = std::move(recvAst);
    mc.method = name;
    // Borrow the caller's arg exprs for the synthetic call and restore
    // them after — the AST node is shared and re-lowered (multi-clause
    // functions), so it must be left intact.
    auto& lentArgs = const_cast<std::vector<ast::ExprPtr>&>(args);
    mc.args = std::move(lentArgs);
    auto snap = subst; subst[sx] = sx;
    auto body = lowerMethodCall(mc);
    subst = snap;
    lentArgs = std::move(mc.args);
    Lambda lam; lam.params = {sx}; lam.body = std::move(body);
    auto ex = std::make_unique<Expr>(); ex->node = std::move(lam);
    return ex;
}

// `~fn(args...)` / `~(op)` — partial or full application. Flatten every
// paren group into ordered slots (a `_` placeholder is an open slot); if
// all slots are bound and enough are present, apply now, else build a fun
// taking one param per open slot plus trailing params up to the arity.
auto Lowering::lowerCurry(const ast::CurryExpr& n) -> ExprPtr {
    struct Slot { bool open; ExprPtr val; };
    std::vector<Binding> binds;
    std::vector<Slot> slots;
    for (const auto& g : n.argGroups)
        for (const auto& a : g) {
            if (std::holds_alternative<ast::CurryPlaceholder>(a->kind))
                slots.push_back({true, nullptr});
            else slots.push_back({false, atomize(a, binds)});
        }
    // Inside a module, a bare `~name` means THAT module's member. Falling
    // through to the global bare name let an application's function of
    // the same name answer a library's `values.map(~rendered)` — where
    // `rendered` was the library's own `private do` helper.
    std::string owningModule = n.module;
    if (owningModule.empty() && !n.isOperator && !subst.count(n.name))
        for (auto scope = currentModulePath; !scope.empty();) {
            if (moduleFunctions.count(scope + "." + n.name)) {
                owningModule = scope;
                break;
            }
            const auto dot = scope.rfind('.');
            if (dot == std::string::npos) break;
            scope.resize(dot);
        }
    // A name a `using` imported is that module's function. Left bare it
    // had no known arity and became a by-name dispatch on whatever the
    // first argument turned out to be: `~joined` for an imported
    // `joined(request, socket)` raised "Undefined method: joined"
    // (kexhq/kex#476).
    if (owningModule.empty() && !n.isOperator && !subst.count(n.name)) {
        if (const auto key = importedKey(n.name); !key.empty())
            owningModule = key.substr(0, key.rfind('.'));
        else if (auto ext = externalImports.find(n.name);
                 ext != externalImports.end() && !moduleImports.count(n.name))
            if (const auto dot = ext->second.rfind('.');
                dot != std::string::npos)
                owningModule = ext->second.substr(0, dot);
    }
    // A qualified reference names its module the way a call would: through
    // an alias (`using App.Controllers.Chat, as: Chat` then `~Chat.show`),
    // an enclosing module, or a module a `using` opened.
    if (!n.module.empty() && !n.isOperator) {
        auto known = [&](const std::string& module) {
            const auto key = module + "." + n.name;
            return moduleFunctions.count(key) > 0 ||
                   (externalModules &&
                    externalModules->exportToBeamFn.count(key) &&
                    externalModules->nameToAtom.count(module));
        };
        if (!known(owningModule)) {
            std::vector<std::string> candidates;
            const auto firstDot = n.module.find('.');
            const auto head = n.module.substr(0, firstDot);
            if (auto alias = moduleAliases.find(head);
                alias != moduleAliases.end())
                candidates.push_back(
                    alias->second + (firstDot == std::string::npos
                                         ? std::string{}
                                         : n.module.substr(firstDot)));
            for (auto scope = currentModulePath; !scope.empty();) {
                candidates.push_back(scope + "." + n.module);
                const auto dot = scope.rfind('.');
                if (dot == std::string::npos) break;
                scope.resize(dot);
            }
            for (const auto& imported : usingModules)
                candidates.push_back(imported + "." + n.module);
            for (const auto& candidate : candidates)
                if (known(candidate)) {
                    owningModule = candidate;
                    break;
                }
        }
    }
    const std::string qualKey =
        owningModule.empty() ? n.name : owningModule + "." + n.name;

    const bool isUnaryOp = n.isOperator && n.name == "!";

    int arity = -1;
    if (n.isOperator) arity = isUnaryOp ? 1 : 2;
    else if (auto it = fnParamNames.find(qualKey); it != fnParamNames.end())
        arity = static_cast<int>(it->second.size());
    else if (!owningModule.empty() && externalModules) {
        auto pit = externalModules->exportParamNames.find(qualKey);
        if (pit != externalModules->exportParamNames.end())
            arity = static_cast<int>(pit->second.size());
    }

    // Can this qualified name be emitted as a direct module call?
    const bool qualifiedKnown = !owningModule.empty()
        && (moduleFunctions.count(qualKey)
            || (externalModules
                && externalModules->exportToBeamFn.count(qualKey)
                && externalModules->nameToAtom.count(owningModule)));

    // A bare reference whose arity isn't known here: an unqualified
    // builtin/module-local/local binding, or a qualified name belonging to
    // a prelude module, which is an intrinsic rather than a BEAM module.
    // Emitting Call{name, 0} would be wrong; defer to the UFCS path, where
    // `String.upperCase(s)` and `s.upperCase` are the same call anyway.
    if (!n.isOperator && slots.empty() && arity < 0 && !qualifiedKnown)
        return lowerNameAsUfcsFun(n.name, {});

    auto buildCall = [&](std::vector<ExprPtr> args) -> ExprPtr {
        if (isUnaryOp && args.size() >= 1)
            return intrin(Op::Not, one(std::move(args[0])));
        if (n.isOperator && args.size() >= 2)
            if (auto op = curryOp(n.name))
                return intrin(*op, two(std::move(args[0]), std::move(args[1])));
        int ar = static_cast<int>(args.size());
        if (!owningModule.empty()) {
            // Same two lookups the namespace MethodCall path uses: a
            // function of a module in this compilation unit, else an
            // export of a separately compiled one.
            if (auto it = moduleFunctions.find(qualKey); it != moduleFunctions.end())
                return localCallExpr(it->second, std::move(args));
            if (externalModules) {
                auto eit = externalModules->exportToBeamFn.find(qualKey);
                auto ait = externalModules->nameToAtom.find(owningModule);
                if (eit != externalModules->exportToBeamFn.end()
                    && ait != externalModules->nameToAtom.end())
                    return externalCallExpr(qualKey, ait->second, eit->second, ar, std::move(args));
            }
            throw LowerError("IR lower: unknown module function in `~"
                             + qualKey + "`");
        }
        auto ex = std::make_unique<Expr>();
        ex->node = prefixCall(n.name, std::move(args));
        return ex;
    };

    int openCount = 0;
    for (const auto& s : slots) if (s.open) openCount++;
    // A bare `~name` with NO argument group written at all (`slots`
    // built from zero groups) must yield the function VALUE, never call
    // it — even when the callee happens to take zero arguments, where
    // `slots.size() >= arity` (0 >= 0) would otherwise look "fully
    // applied" and auto-call it here. That auto-call is only correct for
    // an explicit application (`~greet()`, one written — if empty —
    // argument group), the same distinction the tree-walker's
    // `fullyApplied` already makes (kexhq/kex#312).
    bool full = !n.argGroups.empty() && openCount == 0 &&
        (arity >= 0 ? static_cast<int>(slots.size()) >= arity : !slots.empty());
    if (full) {
        std::vector<ExprPtr> args;
        for (auto& s : slots) args.push_back(std::move(s.val));
        // A fully applied call into a capability goes through the
        // substitution dispatch. This is the path a PRELUDE capability
        // takes — `ENV.get(...)` never reaches the receiver or module
        // paths (kexhq/kex#143).
        if (threadsCapabilities() && !owningModule.empty() &&
            isCapabilityModule(owningModule) &&
            capabilityInterfaceHas(owningModule, n.name))
            return capabilityDispatchLowered(
                owningModule, n.name, std::move(args), binds,
                [&](std::vector<ExprPtr> callArgs) {
                    return buildCall(std::move(callArgs));
                });
        return wrapLets(binds, buildCall(std::move(args)));
    }

    // Partial: ONE fun over a fresh param per open slot plus one per
    // argument beyond the slots written — `~add(1)` is `{ |b| add(1, b) }`,
    // as the language spec defines it, and `~greet("Hi")` on a 3-arity
    // function takes the other two together. That is the arity an Erlang
    // caller uses: the HTTP server calls a `~handler(x)` route with
    // (request, context) in one go, which the nested one-argument funs
    // this used to build rejected with `badarity` (kexhq/kex#408). Kex
    // code that supplies the rest over several calls still works: an
    // indirect call whose argument count differs from the fun's arity
    // goes through `kex_intrinsic_fun:applyFlexible` (see CallIndirect in
    // emit_core), which returns a partial for the remainder.
    int trailing = (arity >= 0) ? std::max(0, arity - static_cast<int>(slots.size())) : 0;
    Lambda lam;
    std::vector<ExprPtr> finalArgs;
    for (auto& s : slots) {
        if (s.open) { std::string p = fresh("P"); lam.params.push_back(p); finalArgs.push_back(var(p)); }
        else finalArgs.push_back(std::move(s.val));
    }
    for (int i = 0; i < trailing; i++) { std::string p = fresh("T"); lam.params.push_back(p); finalArgs.push_back(var(p)); }
    lam.body = buildCall(std::move(finalArgs));
    auto ex = std::make_unique<Expr>(); ex->node = std::move(lam);
    return wrapLets(binds, std::move(ex));
}

auto Lowering::clauseNames(const ast::FunctionClause* clause,
                 const std::vector<std::string>& fallback) -> std::vector<std::string> {
    if (!clause) return fallback;
    std::vector<std::string> names;
    for (const auto& param : clause->params)
        names.push_back(param.name.value_or(""));
    return names;
}

// Whether a qualified call's trailing block is meant for an overload that
// takes exactly the arguments written. `fnDefaults` holds one overload's
// defaults, the last one declared, so padding to it moved
// `Html.p { … }` from `p(children = …)` to `p(attributes, children = …)`
// with the block as `attributes` (kexhq/kex#553). Padding is wrong
// whenever it puts the block in a parameter that is not a function.
auto Lowering::blockFitsExactOverload(const std::string& key, size_t count) const
    -> bool {
    auto clauses = fnNamedClauses.find(key);
    if (clauses == fnNamedClauses.end() || count == 0) return false;
    auto takesBlock = [](const ast::Param& param) {
        return !param.type || !*param.type ||
               std::holds_alternative<ast::BlockType>((*param.type)->kind) ||
               std::holds_alternative<ast::FunctionType>((*param.type)->kind);
    };
    bool exact = false;
    bool misplaced = false;
    for (const auto* clause : clauses->second) {
        const auto& params = clause->params;
        if (params.size() == count && takesBlock(params.back()))
            exact = true;
        else if (params.size() > count && !takesBlock(params[count - 1]))
            misplaced = true;
    }
    return exact && misplaced;
}

// A callee compiled apart: no AST of its own to read a default from,
// only the source text its interface keeps. Parsed once; null for a
// parameter without one, or a default that no longer parses alone.
auto Lowering::importedDefault(const std::string& source) -> const ast::ExprPtr* {
    if (source.empty()) return nullptr;
    auto [entry, added] = importedDefaults.try_emplace(source);
    if (added) {
        Lexer lexer(source, "<default>");
        Parser parser(lexer.tokenizeAll(), "<default>");
        auto expr = parser.parseExpr();
        if (parser.diagnostics().empty()) entry->second = std::move(expr);
    }
    return entry->second ? &entry->second : nullptr;
}

// fillDefaultSlots for such a callee: `slots` are its parameters after
// the receiver, a missing one filled with its default, read in the
// callee's own module, where an earlier parameter is in scope by name.
auto Lowering::fillImportedDefaults(const std::string& sourceModule,
                          const std::vector<std::string>& paramNames,
                          const std::vector<std::string>& defaultSources,
                          std::vector<ExprPtr>& slots,
                          std::vector<Binding>& binds) -> void {
    const auto savedSubst = subst;
    const auto savedModule = currentModulePath;
    currentModulePath = sourceModule;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i]) {
            const auto* fallback = i < defaultSources.size()
                ? importedDefault(defaultSources[i]) : nullptr;
            slots[i] = fallback ? atomize(*fallback, binds)
                                : lit(LitKind::None, "none");
        }
        const auto bound = fresh("DefaultArg");
        binds.push_back({bound, std::move(slots[i])});
        slots[i] = var(bound);
        if (i < paramNames.size() && !paramNames[i].empty())
            subst[paramNames[i]] = bound;
    }
    subst = savedSubst;
    currentModulePath = savedModule;
}

auto Lowering::fillImportedDefaults(const semantic::ResolvedCallTarget& target,
                          std::vector<ExprPtr>& slots,
                          std::vector<Binding>& binds) -> void {
    fillImportedDefaults(target.sourceModule, target.paramNames,
                         target.defaultSources, slots, binds);
}

auto Lowering::fillDefaultSlots(const std::string& name,
                      std::vector<ExprPtr>& slots,
                      std::vector<Binding>& binds,
                      const ast::FunctionClause* clause ) -> void {
    auto defaults = fnDefaults.find(name);
    auto names = fnParamNames.find(name);
    std::vector<const ast::ExprPtr*> selectedDefaults;
    std::vector<std::string> selectedNames;
    if (clause) {
        for (const auto& param : clause->params) {
            selectedDefaults.push_back(param.defaultValue ? &*param.defaultValue : nullptr);
            selectedNames.push_back(param.name.value_or(""));
        }
    } else {
        if (defaults == fnDefaults.end() || names == fnParamNames.end()) {
            for (auto& slot : slots)
                if (!slot) slot = lit(LitKind::None, "none");
            return;
        }
        selectedDefaults = defaults->second;
        selectedNames = names->second;
    }
    const auto savedSubst = subst;
    const auto savedModule = currentModulePath;
    if (auto dot = name.rfind('.'); dot != std::string::npos)
        currentModulePath = name.substr(0, dot);
    for (size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i]) {
            if (i < selectedDefaults.size() && selectedDefaults[i])
                slots[i] = atomize(*selectedDefaults[i], binds);
            else
                slots[i] = lit(LitKind::None, "none");
        }
        // Bind once even when a later default references the argument.
        const auto bound = fresh("DefaultArg");
        binds.push_back({bound, std::move(slots[i])});
        slots[i] = var(bound);
        if (i < selectedNames.size() && !selectedNames[i].empty())
            subst[selectedNames[i]] = bound;
    }
    subst = savedSubst;
    currentModulePath = savedModule;
}

// The qualified name a bare call reaches through a `using` import, ""
// when the name is a local or was not imported. The parameter tables file
// a module's function under this, not under the bare spelling.
auto Lowering::importedKey(const std::string& name) const -> std::string {
    if (subst.count(name)) return "";
    auto imported = moduleImports.find(name);
    if (imported == moduleImports.end()) return "";
    for (const auto& [qualified, emitted] : moduleFunctions)
        if (emitted == imported->second) return qualified;
    return "";
}

// Several modules of one unit may extend the same type with a function of
// the same name: Units.SI and Units.Data both `make Measure do let
// to(String, in: …)`. A call reaches only one of them, so each forwards
// what it does not take to the next module after this one, in
// module-path order, and the last to the prelude; a call enters at the
// first of them (`makeMethodFor`, and `lowerModules`' bare-name
// routing). Empty when there is no next one.
auto Lowering::nextExtendingModule(const std::string& method,
                         const std::string& typeName, int arity) const
    -> std::string {
    auto found = makeMethods.find(method);
    if (found == makeMethods.end()) return {};
    const auto owner = canonicalRecordName(typeName);
    std::string next;
    for (const auto& candidate : found->second) {
        if (candidate.modulePath.empty() ||
            candidate.modulePath <= currentModulePath ||
            canonicalRecordName(candidate.owner) != owner || !candidate.def ||
            candidate.def->clauses.empty() ||
            static_cast<int>(candidate.def->clauses[0].params.size()) + 1 !=
                arity)
            continue;
        if (next.empty() || candidate.modulePath < next)
            next = candidate.modulePath;
    }
    return next;
}

// The make-block method a receiver call binds when it does not spell out
// every parameter in order: labels may name any subset, and defaulted
// parameters may be left out. `owner` is the receiver's type when it is
// known. A call already committed to this unit's methods may still take
// the first one its arguments fit when that type declares none (`strict`
// off); deciding whether the call is local at all may not.
auto Lowering::makeMethodFor(const ast::MethodCall& n, std::string owner,
                   bool strict) const -> const MakeMethod* {
    auto found = makeMethods.find(n.method);
    if (found == makeMethods.end()) return nullptr;
    if (auto angle = owner.find('<'); angle != std::string::npos)
        owner.resize(angle);
    owner = canonicalRecordName(owner);
    std::vector<std::string> labels;
    for (const auto& [label, _] : n.namedArgs) labels.push_back(label);
    const auto positional = n.args.size() + (n.block ? 1 : 0);
    // Whether the receiver's own type declares the function at all. When
    // it does and the call needs nothing placed — every parameter given,
    // in order — there is no method to return, and no other type's to
    // look for: two `Connection`s, of which only one's `send` has a
    // default, had the other's `send(text)` padded out with it and sent
    // to an arity nobody defined (kexhq/kex#476).
    bool declaredByOwner = false;
    // Of several modules' extensions that fit, the one a call enters by:
    // the first in module-path order, which forwards to the rest (see
    // `nextExtendingModule`).
    const MakeMethod* entry = nullptr;
    for (const auto& method : found->second) {
        if (!owner.empty() && canonicalRecordName(method.owner) != owner) continue;
        declaredByOwner = !owner.empty();
        const auto& clause = method.def->clauses[0];
        if (labels.empty() && positional >= clause.params.size()) continue;
        if (namedArgumentsFit(clause, labels, positional) &&
            (!entry || method.modulePath < entry->modulePath))
            entry = &method;
    }
    if (entry) return entry;
    return strict || owner.empty() || declaredByOwner
        ? nullptr : makeMethodFor(n, "", false);
}

// `value.field` the checker decided is a read of a record's field
// (`decidedFieldReads`): read the slot. The name is not looked up at all, so a function
// or another type's make function that shares it cannot take the call
// — `incoming.target` beside a `target(incoming)` function ran the
// function (kexhq/kex#448, kexhq/kex#476). Returns nullptr for anything
// else, including a call the checker bound to a make function.
auto Lowering::staticFieldRead(const ast::MethodCall& n) -> ExprPtr {
    if (!n.receiver || n.mutating || n.parenthesized ||
        !n.args.empty() || !n.namedArgs.empty() || n.block)
        return nullptr;
    if (resolvedCalls && resolvedCalls->count(&n)) return nullptr;
    if (std::holds_alternative<ast::UpperIdentifier>(n.receiver->kind))
        return nullptr;
    // The record read, as the checker decided it. Lowering used to work it
    // out again from the receiver's recorded type; every read that found
    // came from a field read the checker had already decided.
    if (!hir) return nullptr;
    const auto& target = hir->targetOf(n);
    if (target.kind != hir::TargetKind::FieldRead) return nullptr;
    const auto& recordName = target.owner;
    // A bare name both this unit and an imported one declare says too
    // little: a local `ParseError` and the prelude's are both `ParseError`
    // to the checker, and they keep their field at different positions. The
    // accessor tells them apart by tuple size. An imported record no local
    // one shadows is read from its interface's layout.
    if (externalRecordNames.count(recordName) && localRecordNames.count(recordName))
        return nullptr;
    auto record = records.find(recordName);
    if (record == records.end()) return nullptr;
    const auto& fields = record->second.fields;
    const auto found = std::find(fields.begin(), fields.end(), n.method);
    if (found == fields.end()) return nullptr;
    const auto dot = recordName.rfind('.');
    const auto bare = dot == std::string::npos
        ? recordName : recordName.substr(dot + 1);
    if (ownsMethod(recordName, n.method) || ownsMethod(bare, n.method))
        return nullptr;
    std::vector<Binding> binds;
    auto receiver = atomize(n.receiver, binds);
    return wrapLets(
        binds,
        callE("erlang", "element", 2,
              two(litInt(static_cast<int64_t>(found - fields.begin()) + 2),
                  std::move(receiver))));
}

// Whether `Type.name(...)` is a static call on a type of this unit. A
// function some OTHER type's `make` block declares does not make it one: a
// record anywhere gaining `empty` turned every `Headers.empty` — a module
// constant — into a call to a local `empty/1` nobody defined
// (kexhq/kex#476). A name no `make` block claims keeps the old reading.
auto Lowering::typeOwnsStaticFunction(const std::string& type,
                          const std::string& method) const -> bool {
    if (!knownTypes.count(type) || !localMethods.count(method)) return false;
    if (knownFns.count(mangleQualifiedMember(type, method))) return true;
    auto owners = methodOwners.find(method);
    if (owners == methodOwners.end()) return true;
    const auto canonical = canonicalRecordName(type);
    return std::any_of(owners->second.begin(), owners->second.end(),
                       [&](const std::string& owner) {
                           return canonicalRecordName(owner) == canonical;
                       });
}

// The `make` function of the receiver's own type that a call written with
// `explicitArgs` arguments reaches, or nullptr when the receiver's type
// is unknown or declares none of that shape.
auto Lowering::ownedMakeFunction(const ast::MethodCall& n, size_t explicitArgs) const
    -> const MakeMethod* {
    auto methods = makeMethods.find(n.method);
    if (methods == makeMethods.end()) return nullptr;
    auto qualified = receiverOwner(n);
    if (auto angle = qualified.find('<'); angle != std::string::npos)
        qualified.resize(angle);
    if (qualified.empty()) return nullptr;
    const auto dot = qualified.rfind('.');
    const auto bare =
        dot == std::string::npos ? qualified : qualified.substr(dot + 1);
    const MakeMethod* loose = nullptr;
    for (const auto& method : methods->second) {
        if (method.owner != bare && method.owner != qualified) continue;
        if (method.def->clauses.empty() ||
            method.def->clauses[0].params.size() != explicitArgs)
            continue;
        if (method.owner == qualified ||
            method.modulePath + "." + method.owner == qualified)
            return &method;
        if (!loose) loose = &method;
    }
    return loose;
}

// The defaults a positional receiver call leaves out, in order. They are
// the ones of the function the RECEIVER's type declares: by name alone, a
// `Builder.string(label, primaryKey = false, ...)` anywhere in the unit
// padded every other type's `string` out to its arity (kexhq/kex#475).
auto Lowering::trailingMakeDefaults(const ast::MethodCall& n) const
    -> std::vector<const ast::ExprPtr*> {
    std::vector<const ast::ExprPtr*> trailing;
    auto owner = receiverOwner(n);
    if (auto angle = owner.find('<'); angle != std::string::npos)
        owner.resize(angle);
    owner = canonicalRecordName(owner);
    const std::vector<MakeMethod>* methods = nullptr;
    if (auto found = makeMethods.find(n.method); found != makeMethods.end())
        methods = &found->second;
    const bool owned = methods && !owner.empty() &&
        std::any_of(methods->begin(), methods->end(),
                    [&](const MakeMethod& method) {
                        return method.owner == owner;
                    });
    if (owned) {
        // An overload the arguments already fill needs nothing padded,
        // and one that leaves out a parameter with no default cannot be
        // the call's: `this.all` beside `all(as: T, renamed = {})` was
        // padded with `renamed` alone, to an `all/3` nobody declared
        // (kexhq/kex#531).
        for (const auto& method : *methods)
            if (method.owner == owner &&
                method.def->clauses[0].params.size() == n.args.size())
                return trailing;
        for (const auto& method : *methods) {
            if (method.owner != owner) continue;
            const auto& params = method.def->clauses[0].params;
            if (params.size() <= n.args.size()) continue;
            if (!std::all_of(params.begin() + static_cast<std::ptrdiff_t>(n.args.size()),
                             params.end(),
                             [](const ast::Param& param) {
                                 return param.defaultValue.has_value();
                             }))
                continue;
            for (size_t i = n.args.size(); i < params.size(); ++i)
                trailing.push_back(&*params[i].defaultValue);
            break;
        }
        return trailing;
    }

    if (auto found = methodDefaults.find(n.method);
        found != methodDefaults.end())
        for (size_t i = n.args.size(); i < found->second.size(); ++i)
            if (found->second[i]) trailing.push_back(found->second[i]);
    return trailing;
}

// The receiver type the checker settled on for this call, "" when it did
// not: the resolved target's own first, then the receiver's static type.
// The name a `make` block's owner is filed under (`makeTargetNames`), for a
// receiver type as the checker prints it: `[String]` is List's, `{K: V}`
// Map's, `Box<Integer>` Box's. The checker's printed type used to be compared
// to owner names as is, so `[String]` matched no owner and a resolved
// `parts.count` fell back to the bare dispatcher name.
auto Lowering::ownerNameOf(const std::string& printed) -> std::string {
    if (printed.empty()) return printed;
    if (printed.front() == '[') return "List";
    if (printed.front() == '{') return "Map";
    return printed.substr(0, printed.find('<'));
}

auto Lowering::receiverOwner(const ast::MethodCall& n) const -> std::string {
    if (resolvedCalls)
        if (auto resolved = resolvedCalls->find(&n);
            resolved != resolvedCalls->end() &&
            !resolved->second.localDispatchTypes.empty())
            return ownerNameOf(resolved->second.localDispatchTypes.front());
    // A call the checker left to this unit's own `make` blocks
    // (ownerOfCall's ReceiverType): the receiver's type is the owner.
    if (const auto type = staticTypeOf(n.receiver.get()))
        return semantic::typeToString(type);
    return "";
}

// A receiver call's explicit arguments in `method`'s parameter order:
// labels by name, positional arguments (then the block) in the slots left,
// and every slot still empty from its declared default.
auto Lowering::placeMethodArguments(const ast::MethodCall& n, const MakeMethod& method,
                          std::vector<Binding>& binds) -> std::vector<ExprPtr> {
    const auto& clause = method.def->clauses[0];
    const auto pnames = clauseNames(&clause, {});
    std::vector<std::string> labels;
    for (const auto& [label, _] : n.namedArgs) labels.push_back(label);
    const auto placed =
        placeArguments(pnames, labels, n.args.size() + bool(n.block));
    std::vector<ExprPtr> slots(clause.params.size());
    for (size_t i = 0; i < n.namedArgs.size(); ++i)
        if (placed.labelSlots[i] != PlacedArguments::npos)
            slots[placed.labelSlots[i]] =
                atomize(n.namedArgs[i].second, binds);
    std::vector<ExprPtr> positional;
    for (const auto& arg : n.args) positional.push_back(atomize(arg, binds));
    if (n.block) positional.push_back(atomize(*n.block, binds));
    for (size_t k = 0; k < positional.size(); ++k)
        if (placed.positionalSlots[k] != PlacedArguments::npos)
            slots[placed.positionalSlots[k]] = std::move(positional[k]);
    fillDefaultSlots(method.modulePath.empty()
                         ? n.method : method.modulePath + "." + n.method,
                     slots, binds, &clause);
    return slots;
}

} // namespace kex::ir

namespace kex::ir {

auto Lowering::operatorOwner(const ast::BinaryOp& n) const -> std::string {
    if (!hir) return {};
    const auto& target = hir->targetOf(n);
    return target.kind == hir::TargetKind::Operator ? target.owner : std::string{};
}

} // namespace kex::ir

namespace kex::ir {

// The receiver type's own implementation of `n.method`, as the checker
// decided it (OwnMethod, ModuleFunction), where the run-time dispatch cannot
// reach it. A distinct type's value is its erased backing value, so a guard
// never recognizes it (`"${x + y}"` on a Tuplex printed the bare pair); a
// module's free function over the module's OWN type shares a bare name
// another module may own (kexhq/kex#230: `conn.close` for a `TCP.Conn` went
// to a Client's `close`). Lowering only names what the checker chose.
auto Lowering::ownImplementationOf(const ast::MethodCall& n) const
    -> OwnImplementation {
    if (!hir || !n.receiver || !n.namedArgs.empty() || n.block) return {};
    const auto& target = hir->targetOf(n);
    if (target.kind == hir::TargetKind::ModuleFunction) {
        const auto symbol = moduleFunctions.find(target.owner);
        if (symbol == moduleFunctions.end()) return {};
        return {OwnImplementation::Reason::ModuleFunction, symbol->second};
    }
    if (target.kind != hir::TargetKind::OwnMethod) return {};
    // Only an erased distinct value needs its implementation by name, and
    // only a name several types implement is emitted per type.
    const auto& m = n.method;
    const auto& type = target.owner;
    const auto bare = type.substr(type.rfind('.') + 1);
    const auto owners = methodOwners.find(m);
    if (!collidingMethods.count(m) || owners == methodOwners.end() ||
        !(distinctTypes.count(type) || distinctTypes.count(bare)))
        return {};
    for (const auto& owner : {type, bare})
        if (std::find(owners->second.begin(), owners->second.end(), owner) !=
            owners->second.end())
            return {OwnImplementation::Reason::DistinctType,
                    mangleReceiverImplementation(m, owner)};
    return {};
}

} // namespace kex::ir
