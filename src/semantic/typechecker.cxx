#include "typechecker.hxx"
#include "analyzer.hxx"
#include "../common/type_def_utils.hxx"
#include <functional>
#include <set>
#include <unordered_set>

namespace kex::semantic {

auto containsOpenType(const TypePtr& type) -> bool;

namespace {

// A sum-type variant is constructor-shaped (TypeName or GenericType with a
// single-part name) if it's a bare `Name` or `Name(Args...)` — anything
// else (tuple/list/function/union/atom type exprs) means the TypeDef is a
// type alias, not a real ADT, and the whole def is skipped for exhaustiveness.
auto extractConstructorName(const ast::TypeExprPtr& variant) -> std::optional<std::string> {
    if (!variant) return std::nullopt;
    if (auto* tn = std::get_if<ast::TypeName>(&variant->kind)) {
        if (tn->parts.size() == 1) return tn->parts[0];
    }
    if (auto* gt = std::get_if<ast::GenericType>(&variant->kind)) {
        if (gt->name.parts.size() == 1) return gt->name.parts[0];
    }
    return std::nullopt;
}

// The receiver without its type arguments — `Result<Box, ?>` becomes
// `Result`, which is the part that decides whether a candidate could accept
// it.
// Does a type contain an Unknown anywhere? On a signature PARAMETER this
// marks a contract the checker never learned — the shape-only entries the
// source-derived interface path records for un-annotated `make` methods,
// where compiled artifacts would supply real types. Type variables are not
// this: `[A]` is a known, genuinely generic contract.
auto mentionsUnknownType(const TypePtr& type) -> bool {
    if (!type) return false;
    if (std::holds_alternative<UnknownType>(type->kind)) return true;
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        for (const auto& arg : named->typeArgs)
            if (mentionsUnknownType(arg)) return true;
        return false;
    }
    if (auto* list = std::get_if<ListType>(&type->kind))
        return mentionsUnknownType(list->element);
    if (auto* optional = std::get_if<OptionalType>(&type->kind))
        return mentionsUnknownType(optional->inner);
    if (auto* map = std::get_if<MapType>(&type->kind))
        return mentionsUnknownType(map->key) || mentionsUnknownType(map->value);
    if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
        for (const auto& element : tuple->elements)
            if (mentionsUnknownType(element)) return true;
    }
    return false;
}

auto headShapeOf(const TypePtr& type) -> TypePtr {
    if (!type) return type;
    if (auto* named = std::get_if<NamedType>(&type->kind))
        return Type::named(named->name);
    return type;
}

} // namespace

auto TypeChecker::check(const ast::Program& program,
                        std::vector<Diagnostic>& diagnostics) -> void {
    m_diagnostics = &diagnostics;
    m_functionSignatures.clear();
    m_resolvedCalls.clear();
    m_referencedModules.clear();
    m_localModules.clear();
    m_moduleConstructors.clear();
    m_adtVariants.clear();
    m_adtOfConstructor.clear();
    m_nullaryConstructors.clear();
    m_methodSignatures.clear();
    m_overloadPurity.clear();
    m_scopedDeclaredSignatures.clear();
    m_currentModulePath.clear();
    m_scopeStack.clear();
    m_importScopeStack.clear();
    m_declarationImports.clear();
    m_functionImports.clear();
    m_makeImports.clear();
    m_mainImports.clear();
    pushScope();

    auto selectionFor = [](const ast::UsingBlock& block) {
        ImportSelection selection;
        for (size_t i = 0; i < block.module.parts.size(); ++i) {
            if (i) selection.module += ".";
            selection.module += block.module.parts[i];
        }
        selection.onlyNames = block.onlyNames;
        selection.exceptNames = block.exceptNames;
        return selection;
    };
    std::function<void(const ast::ModuleDef&, std::vector<ImportSelection>)>
        collectModuleImports;
    collectModuleImports = [&](const ast::ModuleDef& module,
                               std::vector<ImportSelection> active) {
        for (const auto& item : module.body) {
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                    if (node && node->body.empty())
                        active.push_back(selectionFor(*node));
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (node) m_functionImports[node.get()] = active;
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                    if (node) m_makeImports[node.get()] = active;
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) collectModuleImports(*node, active);
                }
            }, item);
        }
    };
    std::vector<ImportSelection> topLevelImports;
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                if (node && node->body.empty())
                    topLevelImports.push_back(selectionFor(*node));
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) m_functionImports[node.get()] = topLevelImports;
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) m_makeImports[node.get()] = topLevelImports;
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (node) m_mainImports[node.get()] = topLevelImports;
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectModuleImports(*node, topLevelImports);
            }
        }, item);
    }

    std::function<void(const ast::ModuleDef&)> collectLocalModule =
        [&](const ast::ModuleDef& mod) {
            m_localModules.insert(mod.name);
            for (const auto& item : mod.body) {
                std::visit([&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        if (node) collectLocalModule(*node);
                    }
                }, item);
            }
        };
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectLocalModule(*node);
            }
        }, item);
    }

    if (m_importedInterfaces) {
        // Constructor names are only unique per ADT: `Kilo` is a nullary
        // Units.Data prefix and a one-payload Units.SI prefix. These maps are
        // keyed by bare constructor name, so a name claimed by two different
        // ADTs is dropped rather than resolved last-writer-wins — which would
        // reject correct patterns against whichever ADT lost.
        std::unordered_set<std::string> ambiguous;
        for (const auto& adt : m_importedInterfaces->adts) {
            m_adtVariants[adt.name] = adt.constructors;
            for (const auto& ctor : adt.constructors) {
                auto [owner, fresh] = m_adtOfConstructor.try_emplace(ctor, adt.name);
                if (!fresh && owner->second != adt.name) {
                    ambiguous.insert(ctor);
                    continue;
                }
                auto arity = adt.constructorArities.find(ctor);
                if (arity != adt.constructorArities.end()) {
                    m_constructorArity[ctor] = arity->second;
                    if (arity->second == 0) m_nullaryConstructors.insert(ctor);
                }
                // NOT registered in m_constructorResult: the interface
                // carries constructor names and arities but not which payload
                // fills which type parameter, so `Ok("hi")` could only be
                // typed as a bare `Result`, losing the `Result<String, ?>`
                // the REPL reads off the runtime value. Locally declared ADTs
                // are typed precisely by registerAdt; extending KexI with the
                // payload slots would do the same for imported ones.
            }
        }
        for (const auto& ctor : ambiguous) {
            m_adtOfConstructor.erase(ctor);
            m_constructorArity.erase(ctor);
            m_nullaryConstructors.erase(ctor);
            m_constructorResult.erase(ctor);
        }
    }

    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                registerAdt(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerAdtsInModule(*node);
            }
        }, item);
    }

    for (const auto& item : program.items)
        if (const auto* function =
                std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
            function && *function)
            m_functionDeclarations.emplace((*function)->name, function->get());
    registerTypeAliases(program);

    // Register built-in types
    // (done before registerDeclaredSignatures so annotation TypeExprs can resolve these names)
    // Int is a user-facing alias for Integer (arbitrary precision).
    // Fixed-width integers are available as Int32, Int64, etc.
    m_globals.set("Int", Type::integer());
    m_globals.set("Integer", Type::integer());
    m_globals.set("Char", Type::charT());
    m_globals.set("String", Type::string());
    m_globals.set("Bool", Type::boolean());
    // Without this, the annotation `Atom` resolved to an unregistered
    // NamedType("Atom") while an atom literal infers PrimitiveType{Atom} —
    // same printed name, different kind, so they never matched. The symptom
    // was the self-contradictory "expects Atom | Integer, but got Atom".
    m_globals.set("Atom", Type::atom());
    m_globals.set("Byte", Type::byte());
    m_globals.set("Int8", Type::int8());
    m_globals.set("Int16", Type::int16());
    m_globals.set("Int32", Type::int32());
    m_globals.set("Int64", Type::int64());
    m_globals.set("UInt8", Type::uint8());
    m_globals.set("UInt16", Type::uint16());
    m_globals.set("UInt32", Type::uint32());
    m_globals.set("UInt64", Type::uint64());
    m_globals.set("Float32", Type::float32());
    m_globals.set("Float64", Type::float64());
    // Note: no "Float" entry — it's not a concrete Type, only a trait
    // name (TraitRegistry, phase 3), satisfied by Float32 and Float64.

    m_globals.set("ENV", Type::map(Type::string(), Type::string()));

    if (m_importedInterfaces)
        for (const auto& trait : m_importedInterfaces->traits)
            m_traits.define(trait);
    registerTraits(program);
    if (m_importedInterfaces)
        for (const auto& c : m_importedInterfaces->traitConformances)
            m_traits.registerImplementation(c.typeName, c.traitName);
    registerRecordFields(program);
    for (const auto& [name, query] : m_computedAliases)
        m_typeAliases[name] = resolveTypeQuery(*query);
    m_computedAliases.clear();
    registerDeclaredSignatures(program);
    registerMakeSignatures(program);
    preRegisterFunctionSigs(program);


    // Topological ordering: check functions whose callees are all known
    // before checking their callers, so forward-reference calls use real
    // (post-body-check) result types rather than provisional TypeVars.
    // SCCs (mutual recursion) are detected via back-edges in the DFS and
    // kept in their original relative order — pre-registration handles them.
    {
        // Collect names of all user-defined top-level functions.
        std::unordered_set<std::string> userFns;
        for (const auto& item : program.items) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n) userFns.insert(n->name);
                }
            }, item);
        }

        // Collect direct call-dependencies for each FunctionDef.
        // Only user-defined function names are tracked; imported package
        // functions are outside this compilation unit and need no ordering.
        std::function<void(const ast::Expr&, std::set<std::string>&)> collectCalls =
            [&](const ast::Expr& e, std::set<std::string>& out) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                    if (userFns.count(n.name)) out.insert(n.name);
                    for (const auto& a : n.args) if (a) collectCalls(*a, out);
                    if (n.block) collectCalls(**n.block, out);
                } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                    for (const auto& value : n.values)
                        if (value) collectCalls(*value, out);
                } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
                    if (userFns.count(n.tag)) out.insert(n.tag);
                    for (const auto& value : n.values)
                        if (value) collectCalls(*value, out);
                } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                    if (userFns.count(n.method)) out.insert(n.method);
                    if (n.receiver) collectCalls(*n.receiver, out);
                    for (const auto& a : n.args) if (a) collectCalls(*a, out);
                    if (n.block) collectCalls(**n.block, out);
                } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                    if (n.left) collectCalls(*n.left, out);
                    if (n.right) collectCalls(*n.right, out);
                } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                    if (n.operand) collectCalls(*n.operand, out);
                } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                    if (n.condition) collectCalls(*n.condition, out);
                    for (const auto& ex : n.thenBody) if (ex) collectCalls(*ex, out);
                    for (const auto& [c, b] : n.elifs) {
                        if (c) collectCalls(*c, out);
                        for (const auto& ex : b) if (ex) collectCalls(*ex, out);
                    }
                    if (n.elseBody)
                        for (const auto& ex : *n.elseBody) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                    if (n.subject) collectCalls(*n.subject, out);
                    for (const auto& cl : n.clauses) {
                        if (cl.guard && *cl.guard) collectCalls(**cl.guard, out);
                        if (cl.body) collectCalls(*cl.body, out);
                    }
                } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::LetExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                    if (n.condition) collectCalls(*n.condition, out);
                    if (n.thenExpr) collectCalls(*n.thenExpr, out);
                    if (n.elseExpr) collectCalls(*n.elseExpr, out);
                } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                    if (n.expr) collectCalls(*n.expr, out);
                    if (n.condition) collectCalls(*n.condition, out);
                } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                    if (n.condition) collectCalls(*n.condition, out);
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
                    if (n.inner) collectCalls(*n.inner, out);
                }
            }, e.kind);
        };

        std::unordered_map<std::string, std::set<std::string>> deps;
        // Keep a stable list of (name, item-index) for ordering.
        std::vector<std::pair<std::string, size_t>> fnOrder;
        for (size_t i = 0; i < program.items.size(); i++) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (!n) return;
                    std::set<std::string> d;
                    for (const auto& clause : n->clauses)
                        for (const auto& ex : clause.body) if (ex) collectCalls(*ex, d);
                    d.erase(n->name); // self-recursion: don't depend on self
                    deps[n->name] = std::move(d);
                    fnOrder.emplace_back(n->name, i);
                }
            }, program.items[i]);
        }

        // DFS post-order toposort (handles cycles via visited/onStack sets;
        // back-edge functions are left in original order — pre-registration
        // ensures they still type-check via shared TypeVar unification).
        std::unordered_set<std::string> visited, onStack;
        std::vector<std::string> sorted;
        std::function<void(const std::string&)> dfs = [&](const std::string& name) {
            if (visited.count(name)) return;
            if (onStack.count(name)) return; // back edge: cycle, skip
            onStack.insert(name);
            if (deps.count(name))
                for (const auto& dep : deps.at(name)) dfs(dep);
            onStack.erase(name);
            visited.insert(name);
            sorted.push_back(name);
        };
        for (const auto& [name, _] : fnOrder) dfs(name);

        // Build check order: sorted fn names first (deps before callers),
        // then any non-fn top-level items in their original positions.
        // We do two passes: first check all FunctionDefs in sorted order,
        // then check everything else (types, records, make blocks, main).
        std::unordered_set<std::string> fnChecked;
        for (const auto& name : sorted) {
            // Check ALL FunctionDefs with this name (handles overloaded functions
            // sharing a name — both overloads must be checked and appended to the
            // same overload set). Mark the name done only after the full sweep.
            for (const auto& item : program.items) {
                std::visit([&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        if (n && n->name == name) checkTopLevel(item);
                    }
                }, item);
            }
            fnChecked.insert(name);
        }
        // Non-function items and any unchecked functions (e.g. inside modules).
        for (const auto& item : program.items) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n && !fnChecked.count(n->name)) checkTopLevel(item);
                } else {
                    checkTopLevel(item);
                }
            }, item);
        }
    }

    popScope();

    // `m_typeMap` records each expression's type as it was INFERRED, which for
    // anything inferred before its unification is a bare TypeVar; the concrete
    // type only lives in `m_subst`. Consumers outside this class (IR lowering,
    // via Analyzer::typeMap) have no access to the substitution, so they saw
    // `T1` where the checker itself would say `P` — and BEAM lowering silently
    // sent `let p = this; p.advance` to the prelude instead of the receiver's
    // own `make` block. Resolve once, here, so every consumer reads the answer
    // the checker actually reached. Unbound vars resolve to themselves, so a
    // genuinely un-inferred expression is unchanged.
    for (auto& [expr, type] : m_typeMap)
        type = resolve(type);
}

auto TypeChecker::registerRecordFields(const ast::Program& program) -> void {
    std::unordered_map<std::string, TypePtr> noGenerics;
    auto registerRecord = [&](const ast::RecordDef& record) {
        auto& fields = m_recordFields[record.name];
        for (const auto& field : record.fields)
            fields[field.name] = field.type
                ? resolveTypeExpr(*field.type, noGenerics)
                : Type::unknown();
    };
    std::function<void(const ast::ModuleDef&)> registerModule;
    registerModule = [&](const ast::ModuleDef& module) {
      for (const auto& item : module.body) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (node) registerRecord(*node);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerModule(*node);
            }
        }, item);
      }
    };
    for (const auto& item : program.items)
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (node) registerRecord(*node);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerModule(*node);
            }
        }, item);
}

auto TypeChecker::registerAdt(const ast::TypeDef& def) -> void {
    if (!def.variants) return;

    if (kex::isTransparentTypeAlias(def)) return;

    std::vector<std::string> names;
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) return;  // not constructor-shaped — a type alias, skip entirely
        names.push_back(*name);
        if (std::holds_alternative<ast::TypeName>(variant->kind))
            m_nullaryConstructors.insert(*name);
        // Payload arity: `None` is 0, `Just(T)` is 1, `Between(A, B)` is 2.
        if (auto* gt = std::get_if<ast::GenericType>(&variant->kind))
            m_constructorArity[*name] = gt->args.size();
        else
            m_constructorArity[*name] = 0;
    }
    if (names.empty()) return;

    m_adtVariants[def.name] = names;
    for (const auto& name : names) {
        m_adtOfConstructor[name] = def.name;
    }

    // Record what each constructor produces, so `Just(1)` infers as an
    // Optional instead of `unknown` — without that, a mismatched pattern in
    // `if let Ok(x) = Just(1)` has no scrutinee type to be checked against.
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) continue;
        ConstructorResult result;
        result.adtName = def.name;
        result.typeParamCount = def.typeParams.size();
        if (const auto* generic = std::get_if<ast::GenericType>(&variant->kind)) {
            for (const auto& payload : generic->args) {
                int slot = -1;
                if (payload) {
                    if (const auto* named =
                            std::get_if<ast::TypeName>(&payload->kind);
                        named && named->parts.size() == 1) {
                        auto found = std::find(def.typeParams.begin(),
                                               def.typeParams.end(),
                                               named->parts.front());
                        if (found != def.typeParams.end())
                            slot = static_cast<int>(
                                found - def.typeParams.begin());
                    } else if (const auto* var =
                                   std::get_if<ast::GenericVar>(&payload->kind)) {
                        auto found = std::find(def.typeParams.begin(),
                                               def.typeParams.end(), var->name);
                        if (found != def.typeParams.end())
                            slot = static_cast<int>(
                                found - def.typeParams.begin());
                    }
                }
                result.slots.push_back(slot);
            }
        }
        m_constructorResult[*name] = std::move(result);
    }
}

auto TypeChecker::constructorResultType(
    const std::string& name, const std::vector<TypePtr>& argTypes) const
    -> TypePtr {
    auto found = m_constructorResult.find(name);
    if (found == m_constructorResult.end()) return nullptr;
    const auto& info = found->second;

    std::vector<TypePtr> typeArgs(info.typeParamCount, Type::unknown());
    for (std::size_t i = 0; i < info.slots.size() && i < argTypes.size(); ++i) {
        const auto slot = info.slots[i];
        if (slot >= 0 && static_cast<std::size_t>(slot) < typeArgs.size() &&
            argTypes[i])
            typeArgs[slot] = argTypes[i];
    }

    // `X?` is its own type kind everywhere else in the checker, so the
    // prelude's Optional has to produce that rather than a NamedType.
    if (info.adtName == "Optional")
        return Type::optional(typeArgs.empty() ? Type::unknown() : typeArgs[0]);
    return Type::named(info.adtName, std::move(typeArgs));
}

auto TypeChecker::registerAdtsInModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this, &mod](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node) return;
                if (auto constructors = kex::typeConstructors(*node))
                    for (const auto& constructor : *constructors)
                        m_moduleConstructors[mod.name][constructor.name] = {
                            node->name, constructor.arity, true};
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerAdtsInModule(*node);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (!node) return;
                for (const auto& visible : node->items)
                    if (const auto* type =
                            std::get_if<std::unique_ptr<ast::TypeDef>>(
                                &visible);
                        type && *type)
                        if (auto constructors =
                                kex::typeConstructors(**type))
                            for (const auto& constructor : *constructors)
                                m_moduleConstructors[mod.name]
                                                    [constructor.name] = {
                                    (*type)->name, constructor.arity,
                                    node->isPublic};
            }
        }, item);
    }
}

auto TypeChecker::typeDefToType(const ast::TypeDef& def) -> TypePtr {
    if (!def.variants || def.variants->empty()) return Type::unknown();
    // Build variant types, then fold into a union if more than one.
    std::unordered_map<std::string, TypePtr> noGenerics;
    std::vector<TypePtr> parts;
    for (const auto& v : *def.variants) {
        if (v) parts.push_back(resolveTypeExpr(*v, noGenerics));
    }
    if (parts.empty()) return Type::unknown();
    if (parts.size() == 1) return parts[0];
    return std::make_shared<Type>(Type{UnionType{std::move(parts)}});
}

auto TypeChecker::registerTraits(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                if (!node) return;
                TraitDef td;
                td.name = node->name;
                for (const auto& bodyItem : node->body) {
                    std::visit([&](const auto& bi) {
                        using BT = std::decay_t<decltype(bi)>;
                        if constexpr (std::is_same_v<BT, std::unique_ptr<ast::TypeAnnotation>>) {
                            if (!bi) return;
                            auto sig = annotationToSignature(*bi);
                            if (sig) {
                                sig->isFoul = bi->isFoul;
                                td.requiredMethods.push_back(std::move(*sig));
                            }
                        }
                        // FunctionDef items are default implementations: no
                        // signature of their own, but the NAME matters —
                        // every implementing type inherits it.
                        else if constexpr (std::is_same_v<BT, std::unique_ptr<ast::FunctionDef>>) {
                            if (bi) td.defaultMethods.push_back(bi->name);
                        }
                    }, bodyItem);
                }
                m_traits.define(std::move(td));
            }
        }, item);
    }
}

auto TypeChecker::registerTypeAliases(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                // `type Row = Type.returnedBy(parseRow)` resolves against
                // function signatures, which are registered after this pass —
                // so it is deferred rather than resolved here.
                if (node->variants->size() == 1) {
                    if (auto* query = std::get_if<ast::TypeQuery>(
                            &(*node->variants)[0]->kind)) {
                        // Resolved after the builtin type names are
                        // registered — before them, `String` is just a name
                        // and the alias would bind NamedType("String")
                        // instead of the real string type.
                        m_computedAliases.push_back({node->name, query});
                        return;
                    }
                }
                // Transparent type alias: single bare TypeName — register
                // immediately, before falling through to the constructor check.
                // A leading `|` opts out, declaring a one-variant ADT instead.
                if (node->variants->size() == 1 && !node->leadingPipe) {
                    auto* tn = std::get_if<ast::TypeName>(&(*node->variants)[0]->kind);
                    if (tn) { m_typeAliases[node->name] = typeDefToType(*node); return; }
                }
                // Only register as alias if no variant is constructor-shaped.
                for (const auto& v : *node->variants) {
                    if (extractConstructorName(v)) return; // ADT, not an alias
                }
                m_typeAliases[node->name] = typeDefToType(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerTypeAliasesInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::registerTypeAliasesInModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                // `type Row = Type.returnedBy(parseRow)` resolves against
                // function signatures, which are registered after this pass —
                // so it is deferred rather than resolved here.
                if (node->variants->size() == 1) {
                    if (auto* query = std::get_if<ast::TypeQuery>(
                            &(*node->variants)[0]->kind)) {
                        // Resolved after the builtin type names are
                        // registered — before them, `String` is just a name
                        // and the alias would bind NamedType("String")
                        // instead of the real string type.
                        m_computedAliases.push_back({node->name, query});
                        return;
                    }
                }
                // Transparent type alias: single bare TypeName — register
                // immediately, before falling through to the constructor check.
                // A leading `|` opts out, declaring a one-variant ADT instead.
                if (node->variants->size() == 1 && !node->leadingPipe) {
                    auto* tn = std::get_if<ast::TypeName>(&(*node->variants)[0]->kind);
                    if (tn) { m_typeAliases[node->name] = typeDefToType(*node); return; }
                }
                for (const auto& v : *node->variants) {
                    if (extractConstructorName(v)) return;
                }
                m_typeAliases[node->name] = typeDefToType(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerTypeAliasesInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::annotationToSignature(
    const ast::TypeAnnotation& ann,
    std::unordered_map<std::string, TypePtr>* sharedGenericVars)
    -> std::optional<Signature> {
    if (!ann.type) return std::nullopt;
    // Unroll `A -> B -> C` (right-nested FunctionType) into params=[A,B], result=C.
    std::unordered_map<std::string, TypePtr> ownGenericVars;
    auto& genericVars = sharedGenericVars ? *sharedGenericVars : ownGenericVars;
    std::vector<TypePtr> params;
    const ast::TypeExpr* cur = ann.type.get();
    while (cur) {
        if (auto* ft = std::get_if<ast::FunctionType>(&cur->kind)) {
            params.push_back(ft->param ? resolveTypeExpr(*ft->param, genericVars) : Type::unknown());
            cur = ft->result.get();
        } else {
            break;
        }
    }
    TypePtr result = cur ? resolveTypeExpr(*cur, genericVars) : Type::unknown();
    if (params.empty()) {
        // Non-function annotation (e.g. `x : Int`) — treat as a zero-param
        // constant whose type IS the annotated type.
        return Signature{ann.name, {}, result};
    }
    return Signature{ann.name, std::move(params), result};
}

auto TypeChecker::registerDeclaredSignatures(const ast::Program& program) -> void {
    auto add = [&](const ast::TypeAnnotation& ann, const std::string& modulePath) {
        auto sig = annotationToSignature(ann);
        if (!sig) return;
        // Declared annotation wins — stored first so checkFunctionDef
        // can find it and verify the body against the declared type.
        m_annotationDeclared.insert(ann.name);
        const auto key = modulePath + "\n" + ann.name;
        m_annotationArities[key].insert(sig->params.size());
        m_scopedDeclaredSignatures[key].push_back(*sig);
        auto& sigs = m_userSignatures[ann.name];
        // Insert declared sig at front, replacing any same-arity inferred
        // one that was somehow already there (shouldn't happen in pre-pass
        // ordering, but guard for safety).
        sigs.insert(sigs.begin(), std::move(*sig));
    };
    for (const auto& item : program.items)
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item);
            ann && *ann)
            add(**ann, "");
        else if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
                 mod && *mod)
            registerDeclaredSignaturesInModule(**mod);
}

auto TypeChecker::registerDeclaredSignaturesInModule(
    const ast::ModuleDef& mod, const std::string& parentPath) -> void {
    const auto modulePath =
        parentPath.empty() ? mod.name : parentPath + "." + mod.name;
    auto add = [this, &modulePath](
                   const ast::TypeAnnotation& ann, bool exposeUnqualified) {
        auto sig = annotationToSignature(ann);
        if (!sig) return;
        m_annotationDeclared.insert(ann.name);
        const auto key = modulePath + "\n" + ann.name;
        m_annotationArities[key].insert(sig->params.size());
        m_scopedDeclaredSignatures[key].push_back(*sig);
        if (exposeUnqualified)
            m_userSignatures[ann.name].insert(
                m_userSignatures[ann.name].begin(), std::move(*sig));
    };
    for (const auto& item : mod.body) {
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item);
            ann && *ann) {
            add(**ann, true);
        } else if (auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items)
                if (auto* visibleAnn =
                        std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&visible);
                    visibleAnn && *visibleAnn)
                    add(**visibleAnn, false);
        } else if (auto* nested =
                       std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
                   nested && *nested) {
            registerDeclaredSignaturesInModule(**nested, modulePath);
        }
    }
}

auto TypeChecker::registerMakeSignature(const ast::MakeDef& def) -> void {
    if (!def.target) return;
    std::unordered_map<std::string, TypePtr> targetVars;
    auto receiver = resolveTypeExpr(*def.target, targetVars);

    for (const auto& item : def.body) {
        auto add = [&](const std::unique_ptr<ast::TypeAnnotation>& ann) {
            if (!ann) return;
            auto sig = annotationToSignature(*ann, &targetVars);
            if (!sig) return;
            sig->params.insert(sig->params.begin(), receiver);
            m_annotatedMethods.insert(ann->name);
            // Skip a signature already registered with the same parameters —
            // the same guard checkFunctionDef applies. A prelude type whose
            // method is registered both from the imported interface and from
            // the `:>` annotation in its own source otherwise ends up with two
            // identical entries, and overload resolution reports the call
            // ambiguous against ITSELF: `m.get(1, "")` on a Regex.Match (the
            // documented example) failed with the same signature printed twice.
            auto& existing = m_methodSignatures[ann->name];
            const bool duplicate =
                std::any_of(existing.begin(), existing.end(),
                            [&](const Signature& other) {
                                if (other.params.size() != sig->params.size())
                                    return false;
                                for (size_t i = 0; i < other.params.size(); i++)
                                    if (!typesEqual(other.params[i], sig->params[i]))
                                        return false;
                                return true;
                            });
            if (!duplicate) existing.push_back(std::move(*sig));
        };
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
            add(*ann);
        } else if (auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items)
                if (auto* visibleAnn =
                        std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&visible))
                    add(*visibleAnn);
        }
    }
}

auto TypeChecker::registerMakeSignaturesInModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) registerMakeSignature(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerMakeSignaturesInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::registerMakeSignatures(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) registerMakeSignature(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerMakeSignaturesInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::preRegisterFunctionSigs(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) preRegisterFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (!node) return;
                for (const auto& modItem : node->body) {
                    std::visit([this](const auto& mn) {
                        using MT = std::decay_t<decltype(mn)>;
                        if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                            if (mn) preRegisterFunctionDef(*mn);
                        } else if constexpr (std::is_same_v<
                                                 MT,
                                                 std::unique_ptr<ast::VisibilityBlock>>) {
                            if (!mn) return;
                            for (const auto& visible : mn->items)
                                if (auto* def =
                                        std::get_if<std::unique_ptr<ast::FunctionDef>>(
                                            &visible);
                                    def && *def)
                                    preRegisterFunctionDef(**def);
                        }
                    }, modItem);
                }
            }
        }, item);
    }
}

auto TypeChecker::preRegisterFunctionDef(const ast::FunctionDef& def) -> void {
    // Skip make-block methods (they have an implicit `this` that mis-counts
    // arity in checkCall's UFCS desugaring, same exclusion as checkFunctionDef).
    if (m_inMakeBlock) return;
    // Skip annotation-declared functions — registerDeclaredSignatures already
    // populated m_userSignatures for these and checkFunctionDef will use the
    // annotation as ground truth.
    if (m_annotationDeclared.count(def.name)) return;
    // A name may be pre-registered more than once: clauses of one definition
    // share a def.name, but so do separate definitions that overload by arity
    // (common for module functions, e.g. `let f(a)` and `let f(a, b)`).
    // Skipping the name outright would leave every arity but the first
    // invisible to calls checked before the definitions themselves are, so
    // only arities already present are skipped.
    auto existing = m_userSignatures.find(def.name);
    const bool alreadyRegistered = existing != m_userSignatures.end();
    auto hasArity = [&](size_t arity) {
        if (!alreadyRegistered) return false;
        return std::any_of(existing->second.begin(), existing->second.end(),
                           [&](const Signature& signature) {
                               return signature.params.size() == arity;
                           });
    };

    std::vector<Signature> provisional;
    for (const auto& clause : def.clauses) {
        if (hasArity(clause.params.size())) continue;
        std::unordered_map<std::string, TypePtr> genericVars;
        std::vector<TypePtr> paramTypes;
        for (const auto& param : clause.params) {
            paramTypes.push_back(
                param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar());
        }
        std::size_t required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue) required--;
        provisional.push_back(Signature{def.name, std::move(paramTypes),
                                        freshTypeVar(), false, required});
    }
    if (provisional.empty()) return;
    if (alreadyRegistered)
        for (auto& signature : provisional)
            existing->second.push_back(std::move(signature));
    else
        m_userSignatures[def.name] = std::move(provisional);
}

auto TypeChecker::checkMatchExhaustiveness(const ast::MatchExpr& node, SourceLocation loc) -> void {
    bool hasUnguardedCatchAll = false;
    std::set<std::string> covered;
    std::string adtName;
    bool inconclusive = false;

    for (const auto& clause : node.clauses) {
        bool guarded = clause.guard && *clause.guard;
        for (const auto& pat : clause.patterns) {
            if (!pat) continue;
            if (std::holds_alternative<ast::WildcardPattern>(pat->kind) ||
                std::holds_alternative<ast::VarPattern>(pat->kind)) {
                if (!guarded) hasUnguardedCatchAll = true;
                continue;
            }
            std::string ctorName;
            if (auto* ctor = std::get_if<ast::ConstructorPattern>(&pat->kind)) {
                ctorName = ctor->name;
            } else if (auto* lit = std::get_if<ast::LiteralPattern>(&pat->kind);
                       lit && lit->literal.type == TokenType::None) {
                // `None` lexes as its own TokenType::None (lexer.cxx), so
                // it parses as a LiteralPattern, not ConstructorPattern{
                // "None"} — treat it as the Option constructor it is.
                ctorName = "None";
            } else {
                continue;  // literal/list/tuple/record/range patterns don't drive ADT exhaustiveness
            }

            auto it = m_adtOfConstructor.find(ctorName);
            if (it == m_adtOfConstructor.end()) {
                inconclusive = true;  // unregistered constructor — can't prove the closed set
                continue;
            }
            if (adtName.empty()) adtName = it->second;
            else if (adtName != it->second) inconclusive = true;  // patterns span more than one ADT
            if (!guarded) covered.insert(ctorName);
        }
    }

    if (hasUnguardedCatchAll || inconclusive || adtName.empty()) return;

    std::vector<std::string> missing;
    for (const auto& ctorName : m_adtVariants[adtName]) {
        if (!covered.count(ctorName)) missing.push_back(ctorName);
    }
    if (missing.empty()) return;

    std::string list;
    for (size_t i = 0; i < missing.size(); i++) {
        if (i) list += ", ";
        list += missing[i];
    }
    error(loc, "Non-exhaustive match on " + adtName + ": missing case(s) " + list);
}

auto TypeChecker::resolveTypeExpr(const ast::TypeExpr& typeExpr,
                                   std::unordered_map<std::string, TypePtr>& genericVars) -> TypePtr {
    return std::visit([this, &genericVars](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::TypeQuery>) {
            return resolveTypeQuery(node);
        }
        else if constexpr (std::is_same_v<T, ast::TypeName>) {
            if (node.parts.empty()) return Type::unknown();
            const std::string& last = node.parts.back();
            // `This` inside a make block refers to the implementing type.
            if (last == "This" && node.parts.size() == 1 && m_inMakeBlock && m_currentMakeType)
                return m_currentMakeType;
            // `Never` — the bottom/uninhabited type for never-returning functions.
            // `Void` is an alias for the unit type `()` (Swift-style naming).
            if (last == "Never" && node.parts.size() == 1)
                return Type::voidType();
            if (last == "Void" && node.parts.size() == 1)
                return Type::unit();
            // `Any` — escape hatch: unifies with everything (unknown message type
            // for Process<Any>, etc.). Treated as UnknownType at the type level.
            if (last == "Any" && node.parts.size() == 1)
                return Type::unknown();
            // Single-letter identifiers are generic type variables (docs/
            // types.md Generics) — reuse the same TypeVar for repeated
            // occurrences of the same letter within this clause.
            if (last.size() == 1 && node.parts.size() == 1) {
                auto it = genericVars.find(last);
                if (it != genericVars.end()) return it->second;
                auto var = freshTypeVar();
                genericVars[last] = var;
                return var;
            }
            if (auto known = m_globals.get(last)) return known;
            // Trait-only names (Float, Number, Comparable, ...) have no
            // concrete Type — m_globals deliberately has no entry for them
            // (see check()'s comment) — so a param annotated `Float` means
            // "any T satisfying Float", same as an explicit constraint.
            if (m_traits.get(last)) return Type::constrained(last, last);
            // User type alias (e.g. `type Level = :debug | :info | ...`)
            auto aliasIt = m_typeAliases.find(last);
            if (aliasIt != m_typeAliases.end()) return aliasIt->second;
            return Type::named(last);  // unregistered record/ADT name — nameable, not yet structurally known
        }
        else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::vector<TypePtr> args;
            for (const auto& arg : node.args) {
                args.push_back(arg ? resolveTypeExpr(*arg, genericVars) : Type::unknown());
            }
            std::string name = node.name.parts.empty() ? "" : node.name.parts.back();
            if (name == "Optional" && args.size() == 1)
                return Type::optional(args[0]);
            if (name == "Map" && args.size() == 2)
                return Type::map(args[0], args[1]);
            return Type::named(name, std::move(args));
        }
        else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            std::vector<TypePtr> params;
            params.push_back(node.param
                ? resolveTypeExpr(*node.param, genericVars)
                : Type::unknown());
            const ast::TypeExpr* resultExpr = node.result.get();
            while (resultExpr) {
                const auto* next =
                    std::get_if<ast::FunctionType>(&resultExpr->kind);
                if (!next) break;
                params.push_back(next->param
                    ? resolveTypeExpr(*next->param, genericVars)
                    : Type::unknown());
                resultExpr = next->result.get();
            }
            auto result = resultExpr
                ? resolveTypeExpr(*resultExpr, genericVars)
                : Type::unknown();
            return Type::func(std::move(params), std::move(result));
        }
        else if constexpr (std::is_same_v<T, ast::TupleType>) {
            std::vector<TypePtr> elements;
            for (const auto& elem : node.elements) {
                elements.push_back(elem ? resolveTypeExpr(*elem, genericVars) : Type::unknown());
            }
            return Type::tuple(std::move(elements));
        }
        else if constexpr (std::is_same_v<T, ast::ListType>) {
            return Type::list(node.element ? resolveTypeExpr(*node.element, genericVars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::MapType>) {
            return Type::map(node.key ? resolveTypeExpr(*node.key, genericVars) : Type::unknown(),
                              node.value ? resolveTypeExpr(*node.value, genericVars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return Type::optional(node.inner ? resolveTypeExpr(*node.inner, genericVars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::UnionType>) {
            std::vector<TypePtr> members;
            members.push_back(node.left ? resolveTypeExpr(*node.left, genericVars) : Type::unknown());
            members.push_back(node.right ? resolveTypeExpr(*node.right, genericVars) : Type::unknown());
            return std::make_shared<Type>(Type{UnionType{std::move(members)}});
        }
        else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return Type::atom();
        }
        else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            auto it = genericVars.find(node.name);
            if (it != genericVars.end()) return it->second;
            auto var = freshTypeVar();
            genericVars[node.name] = var;
            return var;
        }
        else {
            // BlockType — not modeled as a distinct semantic::Type yet.
            return Type::unknown();
        }
    }, typeExpr.kind);
}

// Rejects a constructor pattern that destructures the wrong number of values,
// recursively. Without this, `let Just(a, b) = Just(1)` reached the backends:
// the interpreter failed at runtime with "pattern mismatch — expected Just",
// and BEAM emitted Core Erlang that erlc rejected outright. A constructor's
// arity is known statically, so this belongs at compile time.
auto TypeChecker::alwaysReturns(const ast::Expr& expr) -> bool {
    if (std::holds_alternative<ast::ReturnExpr>(expr.kind)) return true;
    // A block exits iff its last expression does.
    if (const auto* block = std::get_if<ast::BlockExpr>(&expr.kind))
        return alwaysReturns(block->body);
    return false;
}

auto TypeChecker::alwaysReturns(const std::vector<ast::ExprPtr>& body) -> bool {
    return !body.empty() && body.back() && alwaysReturns(*body.back());
}

auto TypeChecker::adtNameOfType(const TypePtr& type) const -> std::string {
    if (!type) return "";
    // `X?` has its own type kind rather than a NamedType, but the arms it can
    // be matched against are the prelude's `Optional` declaration.
    if (std::holds_alternative<OptionalType>(type->kind))
        return m_adtVariants.count("Optional") ? "Optional" : "";
    if (const auto* named = std::get_if<NamedType>(&type->kind))
        if (m_adtVariants.count(named->name)) return named->name;
    return "";
}

auto TypeChecker::checkPatternConstructorOwner(const ast::Pattern& pattern,
                                               const TypePtr& expected)
    -> void {
    const auto adt = adtNameOfType(expected);
    if (adt.empty()) return;

    std::string ctorName;
    if (const auto* ctor = std::get_if<ast::ConstructorPattern>(&pattern.kind)) {
        ctorName = ctor->name;
    } else if (const auto* lit = std::get_if<ast::LiteralPattern>(&pattern.kind);
               lit && lit->literal.type == TokenType::None) {
        // `None` lexes as its own token, so it arrives as a LiteralPattern
        // rather than a nullary ConstructorPattern (same quirk
        // checkMatchExhaustiveness works around).
        ctorName = "None";
    } else {
        return;
    }

    auto owner = m_adtOfConstructor.find(ctorName);
    // An unregistered constructor (imported or opaque) proves nothing.
    if (owner == m_adtOfConstructor.end() || owner->second == adt) return;

    error(pattern.location,
          "`" + ctorName + "` is a constructor of `" + owner->second +
              "`, but the value being matched has type `" + adt +
              "` — this pattern can never match");
}

auto TypeChecker::checkPatternArity(const ast::Pattern& pattern) -> void {
    std::visit([this, &pattern](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            std::optional<size_t> expected;
            std::string kind = "constructor";
            if (auto it = m_constructorArity.find(node.name);
                it != m_constructorArity.end()) {
                expected = it->second;
            } else if (auto record = m_recordFields.find(node.name);
                       record != m_recordFields.end()) {
                expected = record->second.size();
                kind = "record";
            } else if (m_importedInterfaces) {
                auto imported = m_importedInterfaces->recordArities.find(node.name);
                if (imported != m_importedInterfaces->recordArities.end()) {
                    expected = imported->second;
                    kind = "record";
                }
            }
            // An unregistered name (imported/opaque) carries no arity to check.
            if (expected && *expected != node.args.size())
                error(pattern.location,
                      "`" + node.name + "` " + kind + " takes " +
                          std::to_string(*expected) + " value(s), but the "
                          "pattern destructures " +
                          std::to_string(node.args.size()));
            for (const auto& arg : node.args)
                if (arg) checkPatternArity(*arg);
        } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) checkPatternArity(*node.inner);
        } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            // A named record pattern asserts a concrete record type. Verify the
            // name is a known record and that every listed field belongs to it
            // (catches typos like `ParseError { valu, rest }`). Imported records
            // only expose an arity, so field names can't be checked there.
            if (!node.typeName.empty()) {
                auto local = m_recordFields.find(node.typeName);
                bool known = local != m_recordFields.end();
                if (!known && m_importedInterfaces &&
                    m_importedInterfaces->recordArities.count(node.typeName))
                    known = true;
                if (!known)
                    error(pattern.location,
                          "`" + node.typeName + "` is not a record type");
                else if (local != m_recordFields.end())
                    for (const auto& field : node.fields)
                        if (!field.isStringKey &&
                            !local->second.count(field.name))
                            error(pattern.location,
                                  "record `" + node.typeName + "` has no field `" +
                                  field.name + "`");
            }
            for (const auto& field : node.fields)
                if (field.pattern) checkPatternArity(**field.pattern);
        } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            for (const auto& element : node.elements)
                if (element) checkPatternArity(*element);
            if (node.rest) checkPatternArity(**node.rest);
        } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            for (const auto& element : node.elements)
                if (element) checkPatternArity(*element);
        }
    }, pattern.kind);
}

auto TypeChecker::bindPatternVars(
    const ast::Pattern& pat, TypePtr expected) -> void {
    expected = expected ? resolve(expected) : nullptr;
    checkPatternConstructorOwner(pat, expected);
    std::visit([this, &expected](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::VarPattern>) {
            if (node.name != "_")
                defineVar(node.name, expected ? expected : freshTypeVar());
        }
        else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) bindPatternVars(*node.inner, expected);
        }
        else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            for (const auto& arg : node.args) {
                if (arg) bindPatternVars(*arg);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            const std::unordered_map<std::string, TypePtr>* fields = nullptr;
            // A named record pattern (`Foo { x }`) pins the record type itself,
            // so resolve field types from the declared type regardless of what
            // the scrutinee was inferred to be. Fall back to `expected` for the
            // anonymous `{ x }` form.
            if (!node.typeName.empty()) {
                if (auto found = m_recordFields.find(node.typeName);
                    found != m_recordFields.end())
                    fields = &found->second;
            } else if (expected)
                if (auto* named = std::get_if<NamedType>(&expected->kind))
                    if (auto found = m_recordFields.find(named->name);
                        found != m_recordFields.end())
                        fields = &found->second;
            for (const auto& field : node.fields) {
                TypePtr fieldType;
                if (fields)
                    if (auto found = fields->find(field.name);
                        found != fields->end())
                        fieldType = found->second;
                if (field.pattern)
                    bindPatternVars(**field.pattern, fieldType);
                else if (!field.isStringKey)
                    defineVar(field.name,
                              fieldType ? fieldType : freshTypeVar());
            }
        }
        else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            TypePtr elementType;
            if (expected)
                if (auto* list = std::get_if<ListType>(&expected->kind))
                    elementType = list->element;
            for (const auto& elem : node.elements) {
                if (elem) bindPatternVars(*elem, elementType);
            }
            if (node.rest) bindPatternVars(**node.rest, expected);
        }
        else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            const TupleType* tuple = expected
                ? std::get_if<TupleType>(&expected->kind) : nullptr;
            for (size_t i = 0; i < node.elements.size(); ++i)
                if (node.elements[i])
                    bindPatternVars(
                        *node.elements[i],
                        tuple && i < tuple->elements.size()
                            ? tuple->elements[i] : nullptr);
        }
        else if constexpr (std::is_same_v<T, ast::RangePattern>) {
            // (x..y) in a match clause binds x and y as variables.
            if (node.start) bindPatternVars(*node.start);
            if (node.end) bindPatternVars(*node.end);
        }
        // LiteralPattern, WildcardPattern introduce nothing.
    }, pat.kind);
}

auto TypeChecker::checkTopLevel(const ast::TopLevelItem& item) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
            checkModule(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
            checkFunctionDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
            checkMakeDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
            checkMainBlock(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
            checkUsingBlock(*node);
        }
    }, item);
}

auto TypeChecker::checkModule(const ast::ModuleDef& mod) -> void {
    auto previousModulePath = m_currentModulePath;
    m_currentModulePath = previousModulePath.empty()
        ? mod.name
        : previousModulePath + "." + mod.name;
    pushScope();
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                checkMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                checkModule(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                checkUsingBlock(*node);
            } else if constexpr (std::is_same_v<
                                     T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (!node) return;
                for (const auto& visible : node->items) {
                    std::visit([this](const auto& member) {
                        using M = std::decay_t<decltype(member)>;
                        if constexpr (std::is_same_v<
                                          M, std::unique_ptr<ast::FunctionDef>>) {
                            if (member) checkFunctionDef(*member);
                        } else if constexpr (std::is_same_v<
                                                 M, std::unique_ptr<ast::MakeDef>>) {
                            if (member) checkMakeDef(*member);
                        } else if constexpr (std::is_same_v<
                                                 M, std::unique_ptr<ast::UsingBlock>>) {
                            if (member) checkUsingBlock(*member);
                        }
                    }, visible);
                }
            }
        }, item);
    }
    popScope();
    m_currentModulePath = std::move(previousModulePath);
}

auto TypeChecker::checkUsingBlock(const ast::UsingBlock& block) -> void {
    ImportSelection selection;
    for (size_t i = 0; i < block.module.parts.size(); ++i) {
        if (i) selection.module += ".";
        selection.module += block.module.parts[i];
    }
    selection.onlyNames = block.onlyNames;
    selection.exceptNames = block.exceptNames;
    if (block.body.empty()) {
        if (!m_importScopeStack.empty())
            m_importScopeStack.back().push_back(std::move(selection));
        return;
    }
    pushScope();
    m_importScopeStack.back().push_back(std::move(selection));
    inferBody(block.body);
    popScope();
}

auto TypeChecker::checkFunctionDef(const ast::FunctionDef& def) -> void {
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_functionImports.find(&def);
        imports != m_functionImports.end())
        m_declarationImports = imports->second;
    const auto purityKey = m_currentModulePath + "\n" + def.name;
    if (auto [purity, inserted] =
            m_overloadPurity.emplace(purityKey, def.isFoul);
        !inserted && purity->second != def.isFoul) {
        error(def.location,
              "Overloads of `" + def.name +
              "` must all have the same purity");
    }
    const bool receiverIsFirstParam = [&] {
        if (!m_inMakeBlock || def.clauses.empty() ||
            def.clauses[0].params.empty()) return false;
        const auto& first = def.clauses[0].params[0];
        if (!first.pattern) return false;
        const auto& kind = (*first.pattern)->kind;
        if (std::holds_alternative<ast::ListPattern>(kind) ||
            std::holds_alternative<ast::TuplePattern>(kind))
            return false;
        return !std::holds_alternative<ast::VarPattern>(kind) &&
               !std::holds_alternative<ast::ConstructorPattern>(kind);
    }();
    // A "declared" signature is one from a standalone TypeAnnotation
    // (`fact : Integer -> Integer`) — tracked in m_annotationDeclared.
    // Pre-registered provisional sigs (for forward-reference/recursion) are
    // in m_userSignatures but NOT in m_annotationDeclared, so they don't
    // affect param-type selection or return-type verification here.
    const auto scopedDeclared = m_scopedDeclaredSignatures.find(purityKey);
    auto* declared = [&]() -> const Signature* {
        if (scopedDeclared == m_scopedDeclaredSignatures.end()) return nullptr;
        // A lone declaration is still the contract even when its arity is
        // wrong; checkFunctionDef then emits the established annotation/body
        // diagnostics instead of silently treating it as unrelated.
        if (scopedDeclared->second.size() == 1)
            return &scopedDeclared->second.front();
        const auto arity = def.clauses.empty()
            ? size_t(0) : def.clauses.front().params.size();
        const Signature* match = nullptr;
        for (const auto& signature : scopedDeclared->second) {
            if (signature.params.size() != arity) continue;
            // Several same-arity contracts describe a runtime overload set
            // implemented by one permissive body (for example FS.File.open
            // by mode). No single contract may type that body; preserve all
            // call-site signatures without choosing one arbitrarily.
            if (match) return nullptr;
            match = &signature;
        }
        return match;
    }();

    // A `:>` declaration inside `make` is the method's contract just as a
    // standalone annotation is for a top-level function. The registered
    // receiver signature includes `this`; FunctionDef clauses do not, so use
    // a receiver-less copy while checking their parameters and return bodies.
    // Without this, clause-style implementations were inferred from scratch:
    // a generic callback parameter could collapse to Void and poison calls in
    // a later method even though the declared interface returned `[X]`.
    std::optional<Signature> makeDeclared;
    if (!declared && m_inMakeBlock && m_currentMakeType &&
        m_annotatedMethods.count(def.name)) {
        if (auto methods = m_methodSignatures.find(def.name);
            methods != m_methodSignatures.end()) {
            const auto arity = def.clauses.empty()
                ? size_t(0) : def.clauses.front().params.size();
            const auto receiverOffset = receiverIsFirstParam ? 0u : 1u;
            for (const auto& signature : methods->second) {
                if (signature.params.size() != arity + receiverOffset ||
                    !argMatchesParam(m_currentMakeType, signature.params.front()))
                    continue;
                if (makeDeclared) {
                    makeDeclared.reset();
                    break;
                }
                makeDeclared = signature;
                if (!receiverIsFirstParam)
                    makeDeclared->params.erase(makeDeclared->params.begin());
            }
        }
        if (makeDeclared) declared = &*makeDeclared;
    }
    const bool hasDeclaredContracts =
        scopedDeclared != m_scopedDeclaredSignatures.end() || makeDeclared.has_value();

    // Resolve inline return type annotation (-> Type on the function def line),
    // present when no separate TypeAnnotation declaration exists.
    TypePtr inlineReturnType;
    if (!declared && !def.clauses.empty() && def.clauses[0].returnAnnotation) {
        std::unordered_map<std::string, TypePtr> gvars;
        inlineReturnType = resolveTypeExpr(**def.clauses[0].returnAnnotation, gvars);
    }

    auto returnType = declared    ? declared->result
                    : inlineReturnType ? inlineReturnType
                    : freshTypeVar();
    defineVar(def.name, returnType);

    std::vector<Signature> signatures;
    for (size_t ci = 0; ci < def.clauses.size(); ci++) {
        const auto& clause = def.clauses[ci];
        pushScope();
        std::unordered_map<std::string, TypePtr> genericVars;
        std::vector<TypePtr> paramTypes;
        for (size_t pi = 0; pi < clause.params.size(); pi++) {
            const auto& param = clause.params[pi];
            // Use declared param type if available and the annotation covers
            // this position; fall back to inline annotation or fresh TypeVar.
            TypePtr paramType;
            if (declared && pi < declared->params.size() && !param.type) {
                paramType = declared->params[pi];
            } else {
                paramType = param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar();
            }
            paramTypes.push_back(paramType);
            if (param.name.has_value() && *param.name != "_") {
                defineVar(*param.name, paramType);
            }
            if (param.pattern) {
                bindPatternVars(**param.pattern, paramType);
            }
        }
        if (declared && declared->params.size() != clause.params.size()) {
            bool hasMatchingAnnotation = false;
            auto it = m_annotationArities.find(purityKey);
            if (it != m_annotationArities.end()) {
                hasMatchingAnnotation = it->second.count(clause.params.size()) > 0;
            }
            if (!hasMatchingAnnotation && m_diagnostics) {
                m_diagnostics->push_back({Diagnostic::Level::Warning, def.location,
                    "type annotation for '" + def.name + "' declares " +
                    std::to_string(declared->params.size()) + " parameter(s) but definition has " +
                    std::to_string(clause.params.size())});
            }
        }
        auto bodyType = inferBody(clause.body);
        // Verify body matches declared return type (if declared and concrete).
        // Use argMatchesParam (not typesEqual) to apply the same trait-family
        // relaxations that call-site checking uses — e.g. Int and Integer are
        // compatible, so `add : Int -> Int -> Int` with a body returning
        // Integer (inferred from literal arithmetic) isn't a mismatch.
        auto effectiveReturnType = declared ? declared->result : inlineReturnType;
        if (effectiveReturnType &&
            !std::holds_alternative<TypeVar>(effectiveReturnType->kind) &&
            !std::holds_alternative<UnknownType>(effectiveReturnType->kind) &&
            !std::holds_alternative<UnknownType>(bodyType->kind) &&
            !std::holds_alternative<TypeVar>(bodyType->kind) &&
            !argMatchesParam(bodyType, effectiveReturnType)) {
            error(def.location,
                  "`" + def.name + "` declared to return " + typeToString(effectiveReturnType) +
                  " but body returns " + typeToString(bodyType));
        }
        // Resolve TypeVars — body inference may have constrained unannotated
        // params via unifyVar (e.g. `n * 2` → n : Number).
        for (auto& pt : paramTypes) pt = resolve(pt);
        popScope();
        // Prefer the declared/annotated return type for annotated functions —
        // using the inferred body type would expose internal TypeVars to call
        // sites, which can be incorrectly constrained by the first call.
        auto concrete = [](const TypePtr& t) {
            return !std::holds_alternative<TypeVar>(t->kind);
        };
        auto resultType = (effectiveReturnType && concrete(effectiveReturnType))
                          ? effectiveReturnType : resolve(bodyType);
        // Trailing parameters with a default are optional at the call site.
        std::size_t required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue) required--;
        signatures.push_back(
            Signature{def.name, std::move(paramTypes), resultType, def.isFoul,
                      required});
    }

    // Preserve the checked declaration interface before the signatures are
    // moved into the unqualified call-resolution table below.
    m_functionSignatures[&def] = signatures;

    // `:>` signatures are registered before bodies are checked, so defaults
    // are not known yet. Copy the checked required arity back now. UFCS counts
    // an implicit receiver as an argument; an explicit @/range receiver
    // pattern already occupies that slot in the clause.
    if (m_inMakeBlock && m_currentMakeType &&
        m_annotatedMethods.count(def.name)) {
        auto methods = m_methodSignatures.find(def.name);
        if (methods != m_methodSignatures.end())
            for (auto& method : methods->second)
                for (const auto& checked : signatures) {
                    const auto receiverOffset = receiverIsFirstParam ? 0u : 1u;
                    if (method.params.size() !=
                        checked.params.size() + receiverOffset)
                        continue;
                    method.requiredParams =
                        checked.requiredParams.value_or(checked.params.size()) +
                        receiverOffset;
                }
    }

    // make-block methods have an implicit `this` receiver, not a regular
    // param — checkCall's UFCS desugaring (receiver as argument 0) would
    // mis-count their arity, so they're checked (body inference still
    // runs above) but not registered for call-site checking.
    // A make-block method defined with `let` (no `:>` annotation) still has to
    // be visible as a local method: m_methodSignatures was populated ONLY from
    // `:>` declarations, so `make Vec2 do let to(target) -> String? ... end`
    // registered nothing and every `v.to(String)` call resolved to the prelude's
    // generic `to` instead — on BEAM that lowered to `kex_prelude:to`, ignoring
    // the module's own `to__Vec2`.
    if (m_inMakeBlock && m_currentMakeType &&
        !m_annotatedMethods.count(def.name)) {
        // A method reaches its receiver implicitly through `this`, or as a
        // first parameter that MATCHES the receiver: `let head(@[x | _])`,
        // `let rangeStart(x.._)`. Only the implicit form needs a receiver
        // prepended — doing it for the pattern form invents a wrong arity and
        // makes `list.head` look like a 1-of-2 argument call. A plain named
        // parameter (`let to(target)`) is an ordinary argument.
        // A constructor pattern in first position is the type-selector idiom
        // (`let to(String) -> String?`), an ARGUMENT naming the conversion
        // target — not a receiver match like `@[x | _]` or `x.._`.
        auto& existing = m_methodSignatures[def.name];
        for (const auto& signature : signatures) {
            Signature withReceiver = signature;
            if (!receiverIsFirstParam)
                withReceiver.params.insert(withReceiver.params.begin(),
                                           m_currentMakeType);
            if (!receiverIsFirstParam)
                withReceiver.requiredParams =
                    withReceiver.requiredParams.value_or(
                        signature.params.size()) + 1;
            const bool duplicate =
                std::any_of(existing.begin(), existing.end(),
                            [&](const Signature& other) {
                                if (other.params.size() != withReceiver.params.size())
                                    return false;
                                for (size_t i = 0; i < other.params.size(); i++)
                                    if (!typesEqual(other.params[i],
                                                    withReceiver.params[i]))
                                        return false;
                                return true;
                            });
            if (!duplicate) existing.push_back(std::move(withReceiver));
        }
    }

    if (!m_inMakeBlock) {
        // If a declared signature already exists, update its result type with
        // the inferred one (keeping declared params) and keep one entry.
        if (hasDeclaredContracts) {
            auto& sigs = m_userSignatures[def.name];
            // Replace the placeholder declared sig with the fully-checked one.
            if (declared && !signatures.empty()) {
                auto placeholder = std::find_if(
                    sigs.begin(), sigs.end(), [&](const Signature& candidate) {
                        if (candidate.params.size() != declared->params.size())
                            return false;
                        for (size_t i = 0; i < candidate.params.size(); ++i)
                            if (!typesEqual(candidate.params[i],
                                            declared->params[i]))
                                return false;
                        return true;
                    });
                auto checkedInterface = signatures[0];
                const bool hasInlineParamContract = !def.clauses.empty() &&
                    std::any_of(
                        def.clauses[0].params.begin(),
                        def.clauses[0].params.end(),
                        [](const auto& param) {
                            return param.type && *param.type;
                        });
                if (hasInlineParamContract) {
                    // The standalone annotation is the public relationship
                    // (notably A -> A for IO.inspect), while an inline trait
                    // annotation supplies the implementation's dictionary
                    // constraint. Do not erase the declared relationship with
                    // the narrower body-checking parameter type.
                    checkedInterface = *declared;
                    checkedInterface.requiredParams =
                        signatures[0].requiredParams;
                    checkedInterface.isFoul = signatures[0].isFoul;
                }
                if (placeholder != sigs.end())
                    *placeholder = checkedInterface;
                else
                    sigs.push_back(std::move(checkedInterface));
            }
        } else {
            if (m_checkedFunctions.count(def.name)) {
                // Additional `let f(...)` with the same name: append to the
                // overload set rather than replacing (typed overloads).
                for (auto& sig : signatures)
                    m_userSignatures[def.name].push_back(std::move(sig));
            } else {
                // First real check for this name: replace the provisional
                // pre-registered signatures with the inferred ones.
                m_userSignatures[def.name] = std::move(signatures);
                m_checkedFunctions.insert(def.name);
            }
        }
    }
    m_declarationImports = std::move(savedDeclarationImports);
}

auto TypeChecker::checkMakeDef(const ast::MakeDef& def) -> void {
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_makeImports.find(&def); imports != m_makeImports.end())
        m_declarationImports = imports->second;
    pushScope();
    bool wasInMakeBlock = m_inMakeBlock;
    auto prevMakeType = m_currentMakeType;
    m_inMakeBlock = true;
    // Preserve the complete receiver type. This keeps primitive targets
    // canonical (Bool is PrimitiveType::Bool), retains Map/List structure,
    // and carries generic arguments for ADT/record receiver functions.
    m_currentMakeType.reset();
    if (def.target) {
        std::unordered_map<std::string, TypePtr> genericVars;
        m_currentMakeType = resolveTypeExpr(*def.target, genericVars);
    }
    for (const auto& item : def.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            }
        }, item);
    }
    m_inMakeBlock = wasInMakeBlock;
    m_currentMakeType = prevMakeType;
    popScope();

    checkTraitImplementation(def);
    m_declarationImports = std::move(savedDeclarationImports);
}

auto TypeChecker::checkTraitImplementation(const ast::MakeDef& def) -> void {
    if (def.implements.empty()) return;

    // Extract the type name from the make target.
    std::string typeName;
    if (def.target) {
        if (auto* tn = std::get_if<ast::TypeName>(&def.target->kind))
            if (tn->parts.size() == 1) typeName = tn->parts[0];
        if (auto* gt = std::get_if<ast::GenericType>(&def.target->kind))
            if (gt->name.parts.size() == 1) typeName = gt->name.parts[0];
    }

    // Collect method names and their foul status from this make block.
    std::unordered_map<std::string, bool> provided; // name -> isFoul
    for (const auto& item : def.body) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) provided[node->name] = node->isFoul;
            }
        }, item);
    }

    for (const auto& traitName : def.implements) {
        const TraitDef* trait = m_traits.get(traitName);
        if (!trait) {
            error(def.location, "Unknown trait: " + traitName);
            continue;
        }
        std::string prefix = "make " + (typeName.empty() ? "?" : typeName) +
                             ", implement: " + traitName + " — ";
        for (const auto& req : trait->requiredMethods) {
            auto it = provided.find(req.name);
            if (it == provided.end()) {
                error(def.location, prefix + "missing required method '" + req.name + "'");
            } else if (req.isFoul && !it->second) {
                error(def.location, prefix + "method '" + req.name +
                      "' must be declared foul (trait requires it)");
            }
        }
        // Register so satisfies() / argMatchesParam can verify trait usage.
        if (!typeName.empty()) {
            m_traits.registerImplementation(typeName, traitName);
        }
    }
}

auto TypeChecker::checkMainBlock(const ast::MainBlock& block) -> void {
    const bool wasInSyntheticMain = m_inSyntheticMain;
    m_inSyntheticMain = block.synthetic;
    struct Restore {
        bool& flag;
        bool saved;
        ~Restore() { flag = saved; }
    } restore{m_inSyntheticMain, wasInSyntheticMain};
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_mainImports.find(&block); imports != m_mainImports.end())
        m_declarationImports = imports->second;
    if (!block.synthetic) pushScope();
    for (size_t i = 0; i < block.params.size(); i++) {
        const auto& param = block.params[i];
        if (param.name.has_value() && *param.name != "_") {
            TypePtr type = (i == 0) ? Type::list(Type::string())
                         : (i == 1) ? Type::map(Type::string(), Type::string())
                                    : freshTypeVar();
            defineVar(*param.name, std::move(type));
        }
        if (param.pattern) {
            bindPatternVars(**param.pattern);
        }
    }
    inferBody(block.body);
    if (!block.synthetic) popScope();
    m_declarationImports = std::move(savedDeclarationImports);
}

auto TypeChecker::inferBody(const std::vector<ast::ExprPtr>& body) -> TypePtr {
    TypePtr lastType = Type::unit();
    for (const auto& expr : body) {
        if (expr) lastType = inferExpr(*expr);
    }
    return lastType;
}

auto TypeChecker::importedCandidateSignatures(const std::string& name) const
    -> std::vector<Signature> {
    std::vector<Signature> result;
    if (!m_importedInterfaces) return result;
    if (auto separator = name.find("::"); separator != std::string::npos) {
        auto module = m_importedInterfaces->modules.find(name.substr(0, separator));
        if (module != m_importedInterfaces->modules.end())
            if (auto functions = module->second.exports.find(name.substr(separator + 2));
                functions != module->second.exports.end())
                for (const auto& function : functions->second)
                    result.push_back(function.signature);
        return result;
    }
    if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
        functions != m_importedInterfaces->receiverFunctions.end())
        for (const auto& function : functions->second)
            if (importedFunctionVisible(function))
                result.push_back(function.signature);
    for (const auto& [_, module] : m_importedInterfaces->modules) {
        if (!module.automaticImport) continue;
        if (auto functions = module.exports.find(name);
            functions != module.exports.end())
            for (const auto& function : functions->second)
                result.push_back(function.signature);
    }
    return result;
}

auto TypeChecker::importedFunctionVisible(
    const ImportedFunction& function) const -> bool {
    if (function.sourceModule.empty()) return true;
    if (m_importedInterfaces) {
        if (auto module = m_importedInterfaces->modules.find(function.sourceModule);
            module != m_importedInterfaces->modules.end() &&
            module->second.automaticImport)
            return true;
    }
    return moduleMemberImported(function.sourceModule, function.sourceName);
}

auto TypeChecker::moduleMemberImported(const std::string& module,
                                       const std::string& member) const -> bool {
    auto selected = [&](const ImportSelection& import) {
        if (module != import.module &&
            module.rfind(import.module + ".", 0) != 0)
            return false;
        auto selectedMember = member;
        if (module.size() > import.module.size()) {
            const auto rest = module.substr(import.module.size() + 1);
            const auto dot = rest.find('.');
            selectedMember = rest.substr(0, dot);
        }
        if (!import.onlyNames.empty() &&
            std::find(import.onlyNames.begin(), import.onlyNames.end(),
                      selectedMember) == import.onlyNames.end())
            return false;
        return std::find(import.exceptNames.begin(), import.exceptNames.end(),
                         selectedMember) == import.exceptNames.end();
    };
    if (std::any_of(m_declarationImports.begin(), m_declarationImports.end(),
                    selected))
        return true;
    for (const auto& scope : m_importScopeStack)
        if (std::any_of(scope.begin(), scope.end(), selected))
            return true;
    return false;
}

auto TypeChecker::resolveBlockHints(const std::string& name,
                                     const std::vector<TypePtr>& nonBlockArgTypes,
                                     bool isMethodCall) -> std::vector<TypePtr> {
    auto imported = importedCandidateSignatures(name);
    const std::vector<Signature>* sigs = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() &&
        scoped != m_scopedDeclaredSignatures.end())
        sigs = &scoped->second;
    else if (auto user = m_userSignatures.find(name);
             user != m_userSignatures.end())
        sigs = &user->second;
    auto methodIt = isMethodCall ? m_methodSignatures.find(name)
                                 : m_methodSignatures.end();
    if (!sigs && imported.empty() && methodIt == m_methodSignatures.end())
        return {};

    auto hintsFrom = [&](const Signature& sig) -> std::vector<TypePtr> {
        if (sig.params.size() != nonBlockArgTypes.size() + 1) return {};
        auto* blockParam = std::get_if<FuncType>(&sig.params.back()->kind);
        if (!blockParam) return {};

        for (size_t i = 0; i < nonBlockArgTypes.size(); i++) {
            if (!argMatchesParam(nonBlockArgTypes[i], sig.params[i])) return {};
        }

        // Map negative-ID generic placeholders to concrete types from the actual args.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < nonBlockArgTypes.size(); i++) {
            const auto& sigP = sig.params[i];
            const auto& argT = resolve(nonBlockArgTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&sigP->kind); tv && tv->id < 0)
                sub.emplace(tv->id, argT);
            else if (auto* lt = std::get_if<ListType>(&sigP->kind))
                if (auto* tv2 = std::get_if<TypeVar>(&lt->element->kind); tv2 && tv2->id < 0)
                    if (auto* argLt = std::get_if<ListType>(&argT->kind))
                        sub.emplace(tv2->id, resolve(argLt->element));
        }

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) return it2->second;
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : blockParam->params) hints.push_back(applySubst(p));
        return hints;
    };

    if (methodIt != m_methodSignatures.end())
        for (const auto& sig : methodIt->second) {
            auto hints = hintsFrom(sig);
            if (!hints.empty()) return hints;
        }
    for (const auto& sig : imported) {
        auto hints = hintsFrom(sig);
        if (!hints.empty()) return hints;
    }
    if (sigs)
        for (const auto& sig : *sigs) {
            auto hints = hintsFrom(sig);
            if (!hints.empty()) return hints;
        }
    return {};
}

auto TypeChecker::resolveArgHints(const std::string& name,
                                   const std::vector<TypePtr>& argTypes,
                                   size_t slArgIdx,
                                   bool isMethodCall) -> std::vector<TypePtr> {
    auto imported = importedCandidateSignatures(name);
    const std::vector<Signature>* sigs = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() &&
        scoped != m_scopedDeclaredSignatures.end())
        sigs = &scoped->second;
    else if (auto user = m_userSignatures.find(name);
             user != m_userSignatures.end())
        sigs = &user->second;
    auto methodIt = isMethodCall ? m_methodSignatures.find(name)
                                 : m_methodSignatures.end();
    if (!sigs && imported.empty() && methodIt == m_methodSignatures.end())
        return {};

    auto hintsFrom = [&](const Signature& sig) -> std::vector<TypePtr> {
        if (sig.params.size() != argTypes.size()) return {};
        if (slArgIdx >= sig.params.size()) return {};
        auto* funcParam = std::get_if<FuncType>(&sig.params[slArgIdx]->kind);
        if (!funcParam) return {};

        // All non-SL positions must match.
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (i == slArgIdx) continue;
            if (!argMatchesParam(argTypes[i], sig.params[i])) return {};
        }

        // Build generic substitution from the concrete args.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (i == slArgIdx) continue;
            const auto& sigP = sig.params[i];
            const auto& argT = resolve(argTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&sigP->kind); tv && tv->id < 0)
                sub.emplace(tv->id, argT);
            else if (auto* lt = std::get_if<ListType>(&sigP->kind))
                if (auto* tv2 = std::get_if<TypeVar>(&lt->element->kind); tv2 && tv2->id < 0)
                    if (auto* argLt = std::get_if<ListType>(&argT->kind))
                        sub.emplace(tv2->id, resolve(argLt->element));
        }

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) return it2->second;
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : funcParam->params) hints.push_back(applySubst(p));
        return hints;
    };

    if (methodIt != m_methodSignatures.end())
        for (const auto& sig : methodIt->second) {
            auto hints = hintsFrom(sig);
            if (!hints.empty()) return hints;
        }
    for (const auto& sig : imported) {
        auto hints = hintsFrom(sig);
        if (!hints.empty()) return hints;
    }
    if (sigs)
        for (const auto& sig : *sigs) {
            auto hints = hintsFrom(sig);
            if (!hints.empty()) return hints;
        }
    return {};
}

// A bare `~f` capture — no argument groups, no operator, unqualified. Like a
// shorthand lambda, it needs the callee's signature to know what it's being
// checked against, so it's deferred to the contextual (hinted) pass.
static auto isBareCapture(const ast::Expr* e) -> bool {
    if (!e) return false;
    const auto* ce = std::get_if<ast::CurryExpr>(&e->kind);
    return ce && ce->argGroups.empty() && !ce->isOperator && ce->module.empty();
}

auto TypeChecker::inferBlock(const ast::Expr& blockExpr,
                             const std::vector<TypePtr>& hintParams) -> TypePtr {
    if (auto* lam = std::get_if<ast::Lambda>(&blockExpr.kind)) {
        if (lam->params.empty()) {
            // Zero-param lambda `{ }` / `do end` — infer body but stay permissive
            // so it matches any FuncType param (the block ignores the passed arg).
            inferExpr(blockExpr);
            return Type::unknown();
        }
        pushScope();
        std::vector<TypePtr> paramTypes;
        std::unordered_map<std::string, TypePtr> genericVars;
        for (size_t i = 0; i < lam->params.size(); i++) {
            TypePtr pt;
            if (lam->params[i].type) {
                pt = resolveTypeExpr(**lam->params[i].type, genericVars);
            } else if (i < hintParams.size()) {
                auto hint = resolve(hintParams[i]);
                if (!std::holds_alternative<TypeVar>(hint->kind) &&
                    !std::holds_alternative<UnknownType>(hint->kind)) {
                    pt = hint;
                } else {
                    pt = freshTypeVar();
                }
            } else {
                pt = freshTypeVar();
            }
            paramTypes.push_back(pt);
            if (lam->params[i].name != "_") defineVar(lam->params[i].name, pt);
        }
        m_blockDepth++;
        auto bodyType = inferBody(lam->body);
        m_blockDepth--;
        auto resultType = resolve(bodyType);
        if (lam->returnAnnotation) {
            auto declared = resolveTypeExpr(**lam->returnAnnotation, genericVars);
            if (!std::holds_alternative<UnknownType>(resultType->kind) &&
                !std::holds_alternative<TypeVar>(resultType->kind) &&
                !argMatchesParam(resultType, declared))
                typeMismatch(blockExpr.location, declared, resultType);
            resultType = declared;
        }
        popScope();
        return Type::func(std::move(paramTypes), resultType);
    }
    // A bare `~f` passed where a function is expected: check it against the
    // hinted param types, so `words.filter(~even?)` reports the element-type
    // mismatch rather than inferring fresh vars that unify with anything.
    // Only plain unqualified captures — an operator or a `~Mod.fn` name isn't
    // resolvable through checkCall here, and partial applications already
    // carry their own arg types.
    if (auto* ce = std::get_if<ast::CurryExpr>(&blockExpr.kind);
        ce && ce->argGroups.empty() && !ce->isOperator && ce->module.empty()
        && !hintParams.empty()) {
        std::vector<TypePtr> paramTypes;
        for (const auto& h : hintParams) {
            auto pt = resolve(h);
            paramTypes.push_back(
                (std::holds_alternative<TypeVar>(pt->kind) ||
                 std::holds_alternative<UnknownType>(pt->kind))
                    ? freshTypeVar() : pt);
        }
        auto resultType = checkCall(ce->name, paramTypes, blockExpr.location,
                                    /*isMethodCall=*/false);
        return Type::func(std::move(paramTypes), resolve(resultType));
    }
    if (auto* sl = std::get_if<ast::ShorthandLambda>(&blockExpr.kind)) {
        TypePtr paramType = (!hintParams.empty()) ? resolve(hintParams[0]) : freshTypeVar();
        if (std::holds_alternative<TypeVar>(paramType->kind) ||
            std::holds_alternative<UnknownType>(paramType->kind))
            paramType = freshTypeVar();
        // &.method or &.method(args): UFCS → checkCall(name, [receiver, ...args])
        std::vector<TypePtr> callArgs = {paramType};
        for (const auto& arg : sl->args) {
            if (arg) callArgs.push_back(inferExpr(*arg));
        }
        auto resultType = checkCall(sl->name, callArgs, blockExpr.location, true);
        return Type::func({paramType}, resolve(resultType));
    }
    // BlockExpr (no param list) — infer body for side effects, stay permissive.
    inferExpr(blockExpr);
    return Type::unknown();
}

auto TypeChecker::typeOf(const ast::Expr* expr) const -> TypePtr {
    auto it = m_typeMap.find(expr);
    return it != m_typeMap.end() ? it->second : nullptr;
}

auto TypeChecker::typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>& {
    return m_typeMap;
}

auto TypeChecker::functionSignatures(const ast::FunctionDef* function) const
    -> const std::vector<Signature>* {
    auto it = m_functionSignatures.find(function);
    return it == m_functionSignatures.end() ? nullptr : &it->second;
}

auto TypeChecker::resolvedCalls() const
    -> const std::unordered_map<const ast::MethodCall*, ResolvedCallTarget>& {
    return m_resolvedCalls;
}

auto TypeChecker::inferExpr(const ast::Expr& expr) -> TypePtr {
    auto result = std::visit([this, &expr](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::IntLiteral>) {
            return Type::integer();
        }
        else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
            return Type::float64();
        }
        else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            for (const auto& value : node.values)
                if (value) inferExpr(*value);
            return Type::string();
        }
        else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
            return Type::boolean();
        }
        else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
            return Type::charT();
        }
        else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
            return Type::named("None");
        }
        else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
            return Type::atom();
        }
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
            auto type = lookupVar(node.name);
            if (!type) {
                return Type::unknown();
            }
            return type;
        }
        else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
            if (m_nullaryConstructors.contains(node.name))
                return Type::named(node.name);
            for (const auto& [module, constructors] : m_moduleConstructors) {
                auto constructor = constructors.find(node.name);
                if (constructor != constructors.end() &&
                    constructor->second.arity == 0 &&
                    moduleMemberImported(module, node.name))
                    return Type::named(node.name);
            }
            if (m_importedInterfaces)
                for (const auto& [_, module] :
                     m_importedInterfaces->modules)
                    if (auto exports = module.exports.find(node.name);
                        exports != module.exports.end())
                        for (const auto& function : exports->second)
                            if (function.isConstructor &&
                                function.signature.params.empty() &&
                                importedFunctionVisible(function))
                                return function.signature.result;
            auto type = lookupVar(node.name);
            if (type) return type;
            // A bare TYPE NAME used as a value — `x.to(Integer)`, the argument
            // of the conversion protocol. Typing it as the type it names is
            // what makes the protocol dispatchable: left Unknown, every
            // `to` overload matched every call, so `"42".to(Integer)` was typed
            // by whichever came first (`make Showable do let to(String)`) and
            // came out `String?` no matter the target.
            if (isPrimitiveTypeName(node.name) || m_recordFields.count(node.name) ||
                m_adtVariants.count(node.name) || m_typeAliases.count(node.name))
                return Type::named(node.name);
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
            ImportSelection selection;
            for (size_t i = 0; i < node.module.parts.size(); ++i) {
                if (i) selection.module += ".";
                selection.module += node.module.parts[i];
            }
            selection.onlyNames = node.onlyNames;
            selection.exceptNames = node.exceptNames;
            if (node.body.empty()) {
                if (!m_importScopeStack.empty())
                    m_importScopeStack.back().push_back(std::move(selection));
                return Type::unit();
            }
            pushScope();
            m_importScopeStack.back().push_back(std::move(selection));
            auto bodyType = inferBody(node.body);
            popScope();
            return bodyType;
        }
        else if constexpr (std::is_same_v<T, ast::TryExpr>) {
            if (m_tryingBlockDepth >= 0 && m_blockDepth > m_tryingBlockDepth)
                m_tryBelowBlock++;
            auto operand = resolve(
                node.operand ? inferExpr(*node.operand) : Type::unknown());
            if (auto* optional = std::get_if<OptionalType>(&operand->kind))
                return optional->inner;
            if (auto* named = std::get_if<NamedType>(&operand->kind);
                named && !named->typeArgs.empty()) {
                if (named->name == "Optional" || named->name == "Result")
                    return named->typeArgs[0];
            }
            if (std::holds_alternative<UnknownType>(operand->kind) ||
                std::holds_alternative<TypeVar>(operand->kind))
                return Type::unknown();
            error(expr.location, "`.try` expects Result or Optional, got " +
                  typeToString(operand));
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
            auto outerTryingDepth = m_tryingBlockDepth;
            auto outerTryBelowBlock = m_tryBelowBlock;
            m_tryingBlockDepth = m_blockDepth;
            m_tryBelowBlock = 0;
            auto bodyType = inferBody(node.body);
            // A body that exits the function hands nothing to the `trying`
            // expression, so it must not be compared against the rescue arms.
            auto result = resolve(
                alwaysReturns(node.body) ? Type::voidType() : bodyType);
            // A block is a function boundary: a `.try` inside one becomes the
            // BLOCK's result and never reaches this rescue, so the body's type
            // and the rescue's have no reason to agree — the rescue is dead
            // for that failure. `.try` is typed optimistically (payload only),
            // which is what makes the disagreement look real.
            const bool rescueUnreachable = m_tryBelowBlock > 0;
            m_tryingBlockDepth = outerTryingDepth;
            m_tryBelowBlock = outerTryBelowBlock;
            auto merge = [&](TypePtr candidate) {
                candidate = resolve(candidate);
                bool resultPermissive =
                    std::holds_alternative<UnknownType>(result->kind) ||
                    std::holds_alternative<TypeVar>(result->kind) ||
                    std::holds_alternative<VoidType>(result->kind);
                bool candidatePermissive =
                    std::holds_alternative<UnknownType>(candidate->kind) ||
                    std::holds_alternative<TypeVar>(candidate->kind) ||
                    std::holds_alternative<VoidType>(candidate->kind);
                if (resultPermissive && !candidatePermissive) {
                    result = candidate;
                } else if (!resultPermissive && !candidatePermissive &&
                           !rescueUnreachable &&
                           !argMatchesParam(candidate, result) &&
                           !argMatchesParam(result, candidate)) {
                    error(expr.location,
                          "`trying` body returns " + typeToString(result) +
                          " but rescue returns " + typeToString(candidate));
                }
            };
            // A rescue clause binds names exactly like a match clause: its
            // patterns introduce locals its guard and body can see, and
            // `rescue |e|` binds the caught error.
            for (const auto& clause : node.rescue.clauses) {
                pushScope();
                for (const auto& pat : clause.patterns)
                    if (pat) {
                        checkPatternArity(*pat);
                        bindPatternVars(*pat);
                    }
                if (clause.guard && *clause.guard) inferExpr(**clause.guard);
                if (clause.body) {
                    auto armType = inferExpr(*clause.body);
                    if (!alwaysReturns(*clause.body)) merge(armType);
                }
                popScope();
            }
            if (!node.rescue.catchAllBody.empty()) {
                pushScope();
                if (!node.rescue.catchAllParam.empty())
                    defineVar(node.rescue.catchAllParam, Type::unknown());
                auto catchAllType = inferBody(node.rescue.catchAllBody);
                if (!alwaysReturns(node.rescue.catchAllBody))
                    merge(catchAllType);
                popScope();
            }
            // `rescue return` exits the enclosing function, so its expression
            // does not participate in the trying-expression result type.
            if (node.rescue.inlineReturnExpr)
                inferExpr(*node.rescue.inlineReturnExpr);
            return result;
        }
        else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            // `let xs: [Colour] = []` — the annotation is what the binding IS;
            // the initializer only has to be compatible with it. Binding the
            // inferred type instead would defeat the reason for writing one.
            auto declared = declaredBindingType(node.type, valueType, expr.location);
            if (auto* varPat = std::get_if<ast::VarPattern>(&node.pattern->kind)) {
                // `x : Type.of(y)` on the line above `let x = 34`: a top-level
                // binding is a synthetic-main LetExpr, not a 0-arg function, so
                // its declaration has to be matched here. Scoped to the
                // synthetic main, or a local `let` would pick up a global
                // annotation that merely shares its name.
                if (!declared && m_inSyntheticMain)
                    declared = declaredConstantType(varPat->name, valueType,
                                                    expr.location);
                defineVar(varPat->name, declared ? declared : valueType);
            } else if (node.pattern) {
                checkPatternArity(*node.pattern);
                // Reject constructor mismatches early, e.g.
                //   let Ok(v) = parsePrefix(...)   when parsePrefix returns Optional
                //   let Just(v) = parse(...)       when parse returns Result
                if (auto* cp = std::get_if<ast::ConstructorPattern>(&node.pattern->kind)) {
                    auto resolved = resolve(valueType);
                    if (std::holds_alternative<UnknownType>(resolved->kind) ||
                        std::holds_alternative<TypeVar>(resolved->kind))
                        ; // permissive — can't determine the type at compile time
                    else if (cp->name == "Just" && !std::holds_alternative<OptionalType>(resolved->kind))
                        error(node.pattern->location, "cannot match `Just` — expected Optional, got " + typeToString(resolved));
                    else if (cp->name == "Ok" || cp->name == "Error") {
                        if (auto* nt = std::get_if<NamedType>(&resolved->kind)) {
                            if (nt->name != "Result")
                                error(node.pattern->location, "cannot match `" + cp->name + "` — expected Result, got " + typeToString(resolved));
                        } else {
                            error(node.pattern->location, "cannot match `" + cp->name + "` — expected Result, got " + typeToString(resolved));
                        }
                    }
                }
                bindPatternVars(*node.pattern);
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            auto declared = declaredBindingType(node.type, valueType, expr.location);
            defineVar(node.name, declared ? declared : valueType);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            auto varType = lookupVar(node.name);
            if (varType && !std::holds_alternative<UnknownType>(varType->kind) &&
                !std::holds_alternative<TypeVar>(varType->kind)) {
                if (!argMatchesParam(valueType, varType) &&
                    !std::holds_alternative<UnknownType>(valueType->kind) &&
                    !std::holds_alternative<TypeVar>(valueType->kind)) {
                    typeMismatch(expr.location, varType, valueType);
                }
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            auto leftType = node.left ? inferExpr(*node.left) : Type::unknown();
            auto rightType = node.right ? inferExpr(*node.right) : Type::unknown();
            const auto operatorName = [&]() -> std::string {
                switch (node.op) {
                    case TokenType::Plus: return "+";
                    case TokenType::Minus: return "-";
                    case TokenType::Star: return "*";
                    case TokenType::Slash: return "/";
                    case TokenType::Percent: return "%";
                    case TokenType::Caret: return "^";
                    case TokenType::EqEq: return "==";
                    case TokenType::NotEq: return "!=";
                    case TokenType::LessThan: return "<";
                    case TokenType::GreaterThan: return ">";
                    case TokenType::LessEq: return "<=";
                    case TokenType::GreaterEq: return ">=";
                    default: return "";
                }
            }();
            // Operators share make-method signatures with UFCS. Prefer a
            // matching receiver/RHS overload, but leave ordinary operators on
            // the builtin inference path when no local signature applies.
            if (!operatorName.empty()) {
                // Numeric primitives always use the built-in operator. A
                // trait-shaped unit overload such as Watt * Hour must not
                // capture Float * Float merely because both types also occur
                // in generic receiver tables loaded from the prelude.
                if (m_traits.satisfies(leftType, "Number") &&
                    m_traits.satisfies(rightType, "Number"))
                    return inferBinaryOp(node.op, leftType, rightType,
                                         expr.location);
                auto receiver = resolve(leftType);
                const bool concreteReceiver =
                    !std::holds_alternative<TypeVar>(receiver->kind) &&
                    !std::holds_alternative<UnknownType>(receiver->kind);
                // An operator defined in a make block is visible either as a
                // local declaration or — for the prelude's own `Date + Duration`
                // and friends — only through the imported interface. Consulting
                // just the local table sent every prelude operator down the
                // builtin numeric/string path below, which rejects any pair of
                // differing types outright.
                if (concreteReceiver) {
                    auto applies = [&](const Signature& signature) {
                        return signature.params.size() == size_t(2) &&
                               argMatchesParam(leftType, signature.params[0]) &&
                               argMatchesParam(rightType, signature.params[1]);
                    };
                    bool overloaded = false;
                    if (auto local = m_methodSignatures.find(operatorName);
                        local != m_methodSignatures.end())
                        overloaded = std::any_of(local->second.begin(),
                                                 local->second.end(), applies);
                    if (!overloaded)
                        for (const auto& signature :
                             importedCandidateSignatures(operatorName))
                            if (applies(signature)) {
                                overloaded = true;
                                break;
                            }
                    if (overloaded)
                        return checkCall(operatorName, {leftType, rightType},
                                         expr.location, /*isMethodCall=*/true);
                }
            }
            return inferBinaryOp(node.op, leftType, rightType, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            auto operandType = node.operand ? inferExpr(*node.operand) : Type::unknown();
            auto resolved = resolve(operandType);
            if (node.op == TokenType::Bang) {
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "Logical not '!' requires Bool, got " +
                          typeToString(resolved));
                }
                return Type::boolean();
            }
            if (node.op == TokenType::Minus) {
                // Unary negation requires a numeric operand.
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::constrained("N", "Number"));
                }
                return resolved;
            }
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            std::vector<TypePtr> argTypes;
            // First pass: infer concrete args; defer lambdas with parameters
            // until the selected signature can provide contextual types.
            std::vector<std::pair<size_t, size_t>> contextualLambdas;
            for (size_t i = 0; i < node.args.size(); i++) {
                const auto* lambda = node.args[i]
                    ? std::get_if<ast::Lambda>(&node.args[i]->kind) : nullptr;
                if (node.args[i] &&
                    (std::holds_alternative<ast::ShorthandLambda>(
                         node.args[i]->kind) ||
                     isBareCapture(node.args[i].get()) ||
                     (lambda && !lambda->params.empty()))) {
                    argTypes.push_back(Type::unknown()); // placeholder
                    contextualLambdas.emplace_back(i, i);
                } else {
                    argTypes.push_back(node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                }
            }
            for (const auto& [_, arg] : node.namedArgs) {
                argTypes.push_back(arg ? inferExpr(*arg) : Type::unknown());
            }
            // Second pass: infer lambdas with parameter hints from the signature.
            for (const auto& [argIdx, rawIdx] : contextualLambdas) {
                auto hints = resolveArgHints(node.name, argTypes, argIdx);
                argTypes[argIdx] = inferBlock(*node.args[rawIdx], hints);
            }
            if (node.block) {
                auto hints = resolveBlockHints(node.name, argTypes);
                argTypes.push_back(inferBlock(**node.block, hints));
            }
            // send(pid, msg) — check msg type against Process<Msg> if pid type is known.
            if (node.name == "send" && argTypes.size() == 2) {
                auto pidType = resolve(argTypes[0]);
                auto msgType = resolve(argTypes[1]);
                if (auto* nt = std::get_if<NamedType>(&pidType->kind)) {
                    if (nt->name == "Process" && nt->typeArgs.size() == 1) {
                        auto declared = resolve(nt->typeArgs[0]);
                        // Process<Any> (unknown msg type) skips the check.
                        if (!std::holds_alternative<UnknownType>(declared->kind)) {
                            if (auto* tv = std::get_if<TypeVar>(&declared->kind)) {
                                unifyVar(tv->id, msgType);
                            } else if (!std::holds_alternative<UnknownType>(msgType->kind) &&
                                       !std::holds_alternative<TypeVar>(msgType->kind) &&
                                       !argMatchesParam(msgType, declared)) {
                                error(expr.location,
                                      "send: message type " + typeToString(msgType) +
                                      " does not match Process<" + typeToString(declared) + ">");
                            }
                        }
                    }
                }
                return msgType;
            }
            if (auto constructed =
                    constructorResultType(node.name, argTypes))
                return constructed;
            return checkCall(node.name, argTypes, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
            for (const auto& value : node.values)
                if (value) inferExpr(*value);
            return checkCall(
                node.tag,
                {Type::list(Type::string()), Type::list(Type::unknown())},
                expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            // Local module constructors use the same namespace syntax as
            // module constants. Imported constructors are ordinary typed
            // module exports and continue through checkCall below.
            if (node.args.empty() && node.namedArgs.empty() && !node.block &&
                node.receiver) {
                std::function<std::optional<std::string>(const ast::Expr&)>
                    localModulePath;
                localModulePath = [&](const ast::Expr& receiver)
                    -> std::optional<std::string> {
                    if (const auto* root =
                            std::get_if<ast::UpperIdentifier>(
                                &receiver.kind))
                        return root->name;
                    const auto* segment =
                        std::get_if<ast::MethodCall>(&receiver.kind);
                    if (!segment || !segment->receiver ||
                        !segment->args.empty() ||
                        !segment->namedArgs.empty() || segment->block)
                        return std::nullopt;
                    auto parent = localModulePath(*segment->receiver);
                    return parent
                        ? std::optional<std::string>{
                              *parent + "." + segment->method}
                        : std::nullopt;
                };
                if (auto path = localModulePath(*node.receiver)) {
                    if (auto module = m_moduleConstructors.find(*path);
                        module != m_moduleConstructors.end())
                        if (auto constructor =
                                module->second.find(node.method);
                            constructor != module->second.end()) {
                            if (!constructor->second.isPublic &&
                                m_currentModulePath != *path)
                                error(expr.location,
                                      "cannot access private name `" +
                                          node.method + "` from " + *path);
                            return Type::named(node.method);
                        }
                }
            }

            // Backend interop and the private intrinsic ABI are untyped until
            // the intrinsic declaration interface supplies their signatures.
            // Never resolve these against same-named public stdlib functions.
            {
                const ast::Expr* r = node.receiver ? &*node.receiver : nullptr;
                bool intrinsicSegment = false;
                while (r) {
                    if (auto* mc = std::get_if<ast::MethodCall>(&r->kind)) {
                        intrinsicSegment = intrinsicSegment || mc->method == "Intrinsic";
                        r = mc->receiver ? &*mc->receiver : nullptr;
                    } else break;
                }
                if (r) {
                    if (auto* uid = std::get_if<ast::UpperIdentifier>(&r->kind)) {
                        if (uid->name == "Erlang" || uid->name == "Elixir" ||
                            uid->name == "Gleam" ||
                            (uid->name == "Kex" && intrinsicSegment)) {
                            for (const auto& a : node.args)
                                if (a) inferExpr(*a);
                            for (const auto& [_, a] : node.namedArgs)
                                if (a) inferExpr(*a);
                            if (node.block) inferExpr(**node.block);
                            return Type::unknown();
                        }
                    }
                }
            }
            // Namespace call: `Integer.parse(s)` or `Web.Response.text(s)`.
            // A chain made entirely of uppercase segments is a qualified
            // namespace, not a UFCS receiver value, so don't include it as
            // argTypes[0].
            std::string callName = node.method;
            // Only a direct uppercase receiver is unambiguously a namespace.
            // For a qualified chain (`Web.Response.text`) infer `Web.Response`
            // as an Unknown receiver and let checkCall's existing namespace
            // heuristic drop it when the target arity requires that. This also
            // preserves value chains through static constructors/constants,
            // such as `Temperature.Fahrenheit(212).to(String)` and
            // `Temperature.Freezing.to(String)`.
            auto isNamespaceReceiver = [](const ast::Expr& receiver) {
                return std::holds_alternative<ast::UpperIdentifier>(receiver.kind);
            };
            std::function<std::optional<std::string>(const ast::Expr&)>
                importedModulePath;
            importedModulePath = [&](const ast::Expr& receiver)
                -> std::optional<std::string> {
                if (auto* root = std::get_if<ast::UpperIdentifier>(&receiver.kind))
                    return root->name;
                auto* segment = std::get_if<ast::MethodCall>(&receiver.kind);
                if (!segment || !segment->receiver || !segment->args.empty() ||
                    !segment->namedArgs.empty() || segment->block)
                    return std::nullopt;
                auto parent = importedModulePath(*segment->receiver);
                return parent ? std::optional<std::string>{*parent + "." + segment->method}
                              : std::nullopt;
            };
            auto importedPath = node.receiver
                ? importedModulePath(*node.receiver) : std::nullopt;
            if (importedPath && m_importedInterfaces &&
                m_importedInterfaces->modules.count(*importedPath))
                m_referencedModules.insert(*importedPath);
            bool isImportedNamespace = importedPath && m_importedInterfaces &&
                m_importedInterfaces->modules.count(*importedPath) > 0;
            bool isNamespaceCall = node.receiver &&
                (isNamespaceReceiver(*node.receiver) || isImportedNamespace);
            if (isImportedNamespace) {
                callName = *importedPath + "::" + node.method;
            } else if (isNamespaceCall &&
                std::holds_alternative<ast::UpperIdentifier>(node.receiver->kind)) {
                callName = std::get<ast::UpperIdentifier>(node.receiver->kind).name +
                           "::" + node.method;
            }
            // `Type.of(Hello)` where `Hello` NAMES a type answers with that
            // type. A bare type name is already a value elsewhere (`x.to(String)`),
            // and it is not a runtime value at all — the walker died on
            // "Undefined identifier: Hello" while BEAM lowered it to an atom
            // and reported `Atom`.
            if (callName == "Type::of" && node.args.size() == 1 && node.args[0]) {
                if (auto referenced = typeNameReference(*node.args[0])) {
                    if (auto structured = structuredTypeOf(referenced)) {
                        m_staticTypeOfCalls[&node] =
                            {std::move(*structured), /*evaluateArgument=*/false};
                        return Type::named("Type");
                    }
                }
                if (auto signature = namedFunctionSignature(*node.args[0])) {
                    StructuredType function;
                    function.name = "Function";
                    function.pure = !signature->isFoul;
                    bool complete = true;
                    for (const auto& param : signature->params) {
                        auto structured = structuredTypeOf(param);
                        if (!structured) { complete = false; break; }
                        function.args.push_back(std::move(*structured));
                    }
                    if (complete)
                        if (auto result = structuredTypeOf(signature->result)) {
                            function.args.push_back(std::move(*result));
                            m_staticTypeOfCalls[&node] =
                                {std::move(function), /*evaluateArgument=*/false};
                            return Type::named("Type");
                        }
                }
            }
            // `x.to(T)` answers `T?`. The clauses implementing the protocol
            // take their target as an ordinary VALUE parameter
            // (`let to(value, String) = ...`), so every one of them has an
            // `unknown` second parameter, overload resolution cannot tell them
            // apart, and the first one wins: `"34".to(Integer)` and
            // `(1..3).to(List)` both typed as `String?` while evaluating to an
            // Integer and a list. The target NAMES a type, so read it.
            //
            // A String target is left alone: `String?` is already the right
            // answer for it, and a receiver that declares its own total
            // conversion (`Measure.to(String) -> String`) must keep it.
            // Only the bare `to(T)` form. `to(T, radix: n)` carries named
            // arguments, and answering here would skip the call resolution
            // lowering needs to place them — it reported `radix:` as an
            // unknown named argument on BEAM.
            if (node.method == "to" && node.args.size() == 1 && node.args[0] &&
                node.namedArgs.empty() && !node.block) {
                const auto targetIsString = [](const TypePtr& t) {
                    auto* prim = std::get_if<PrimitiveType>(&t->kind);
                    return prim && prim->kind == PrimitiveType::String;
                };
                if (auto target = typeNameReference(*node.args[0]);
                    target && !targetIsString(target)) {
                    for (const auto& argument : node.args) inferExpr(*argument);
                    if (node.receiver) inferExpr(*node.receiver);
                    return Type::optional(target);
                }
            }
            // `Type.returnedBy(f)`: resolved entirely here — a function value
            // carries no signature at runtime. The argument must NAME a
            // function; an overloaded name has no single answer.
            if (callName == "Type::returnedBy" && node.args.size() == 1 &&
                node.args[0]) {
                const auto* identifier =
                    std::get_if<ast::Identifier>(&node.args[0]->kind);
                std::string functionName;
                if (identifier) {
                    functionName = identifier->name;
                } else if (const auto* qualified =
                               std::get_if<ast::MethodCall>(&node.args[0]->kind);
                           qualified && qualified->args.empty() &&
                           qualified->receiver) {
                    if (auto path = importedModulePath(*qualified->receiver))
                        functionName = *path + "::" + qualified->method;
                }
                std::vector<Signature> candidates;
                if (!functionName.empty()) {
                    if (auto user = m_userSignatures.find(functionName);
                        user != m_userSignatures.end())
                        candidates = user->second;
                    if (candidates.empty())
                        candidates = importedCandidateSignatures(functionName);
                }
                if (candidates.empty()) {
                    error(expr.location,
                          "`Type.returnedBy` needs the NAME of a function; "
                          "a lambda or a function value carries no signature");
                } else if (candidates.size() > 1) {
                    std::string message =
                        "`Type.returnedBy` cannot choose between the overloads of `" +
                        functionName + "`";
                    for (const auto& candidate : candidates)
                        message += "\n\n" + displaySignature(functionName, candidate);
                    error(expr.location, message);
                } else if (auto structured =
                               structuredTypeOf(candidates.front().result)) {
                    m_staticTypeOfCalls[&node] =
                        {std::move(*structured), /*evaluateArgument=*/false};
                } else {
                    error(expr.location,
                          "`Type.returnedBy` cannot answer for `" + functionName +
                          "`: its return type is not concrete");
                }
                return Type::named("Type");
            }

            std::vector<TypePtr> argTypes;
            if (!isNamespaceCall)
                argTypes.push_back(node.receiver ? inferExpr(*node.receiver) : Type::unknown());
            // First pass: infer concrete args; defer lambdas with parameters
            // until the selected receiver signature can provide context.
            std::vector<std::pair<size_t, size_t>> contextualLambdas;
            for (size_t i = 0; i < node.args.size(); i++) {
                const auto* lambda = node.args[i]
                    ? std::get_if<ast::Lambda>(&node.args[i]->kind) : nullptr;
                if (node.args[i] &&
                    (std::holds_alternative<ast::ShorthandLambda>(
                         node.args[i]->kind) ||
                     isBareCapture(node.args[i].get()) ||
                     (lambda && !lambda->params.empty()))) {
                    argTypes.push_back(Type::unknown()); // placeholder
                    contextualLambdas.emplace_back(
                        isNamespaceCall ? i : 1 + i, i);
                } else {
                    argTypes.push_back(node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                }
            }
            for (const auto& [_, arg] : node.namedArgs) {
                argTypes.push_back(arg ? inferExpr(*arg) : Type::unknown());
            }
            // Second pass: infer lambdas with parameter hints from the signature.
            for (const auto& [argIdx, rawIdx] : contextualLambdas) {
                auto hints = resolveArgHints(
                    callName, argTypes, argIdx, /*isMethodCall=*/!isNamespaceCall);
                argTypes[argIdx] = inferBlock(*node.args[rawIdx], hints);
            }
            if (node.block) {
                auto hints = resolveBlockHints(
                    callName, argTypes, /*isMethodCall=*/!isNamespaceCall);
                argTypes.push_back(inferBlock(**node.block, hints));
            }
            // `Type.of(x)`: record what the CHECKER knows about the argument.
            // A checked expression knows things the value cannot carry — the
            // unused half of a Result, an empty list's element type — and both
            // backends prefer this recording over asking the value. A nullary
            // constructor is widened to its ADT (`Red` is a `Colour`).
            if (callName == "Type::of" && argTypes.size() == 1) {
                // Widen constructor names to their ADT, through containers:
                // `[Red, Blue(2)]` is a `[Colour]`, not a `[Red]`. Type
                // ARGUMENTS are left alone — a phantom typestate parameter is
                // spelled with constructors too, and widening those erases the
                // distinction they exist to make.
                std::function<TypePtr(const TypePtr&)> widen =
                    [&](const TypePtr& type) -> TypePtr {
                    auto resolved = resolve(type);
                    if (auto* named = std::get_if<NamedType>(&resolved->kind)) {
                        if (auto owner = m_adtOfConstructor.find(named->name);
                            owner != m_adtOfConstructor.end() &&
                            owner->second != named->name)
                            return Type::named(owner->second);
                        return resolved;
                    }
                    if (auto* list = std::get_if<ListType>(&resolved->kind))
                        return Type::list(widen(list->element));
                    if (auto* optional = std::get_if<OptionalType>(&resolved->kind))
                        return Type::optional(widen(optional->inner));
                    if (auto* map = std::get_if<MapType>(&resolved->kind))
                        return Type::map(widen(map->key), widen(map->value));
                    if (auto* tuple = std::get_if<TupleType>(&resolved->kind)) {
                        std::vector<TypePtr> elements;
                        for (const auto& element : tuple->elements)
                            elements.push_back(widen(element));
                        return Type::tuple(std::move(elements));
                    }
                    return resolved;
                };
                if (auto structured = structuredTypeOf(widen(argTypes[0])))
                    m_staticTypeOfCalls[&node] = {std::move(*structured), true};
            }

            // pid.send(msg) UFCS — check msg type against Process<Msg>.
            // argTypes[0] = pid type, argTypes[1] = msg type.
            if (node.method == "send" && argTypes.size() == 2) {
                auto pidType = resolve(argTypes[0]);
                auto msgType = resolve(argTypes[1]);
                if (auto* nt = std::get_if<NamedType>(&pidType->kind)) {
                    if (nt->name == "Process" && nt->typeArgs.size() == 1) {
                        auto declared = resolve(nt->typeArgs[0]);
                        if (!std::holds_alternative<UnknownType>(declared->kind)) {
                            if (auto* tv = std::get_if<TypeVar>(&declared->kind)) {
                                unifyVar(tv->id, msgType);
                            } else if (!std::holds_alternative<UnknownType>(msgType->kind) &&
                                       !std::holds_alternative<TypeVar>(msgType->kind) &&
                                       !argMatchesParam(msgType, declared)) {
                                error(expr.location,
                                      "send: message type " + typeToString(msgType) +
                                      " does not match Process<" + typeToString(declared) + ">");
                            }
                        }
                    }
                }
                return msgType;
            }
            return checkCall(callName, argTypes, expr.location,
                             /*isMethodCall=*/true, &node);
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            TypePtr elemType = Type::unknown();
            for (const auto& elem : node.elements) {
                if (elem) {
                    auto t = inferExpr(*elem);
                    bool elemPermissive = std::holds_alternative<UnknownType>(elemType->kind) ||
                                         std::holds_alternative<TypeVar>(elemType->kind);
                    bool tPermissive = std::holds_alternative<UnknownType>(t->kind) ||
                                       std::holds_alternative<TypeVar>(t->kind) ||
                                       std::holds_alternative<FuncType>(t->kind);
                    bool elemFuncPermissive = elemPermissive ||
                                              std::holds_alternative<FuncType>(elemType->kind);
                    if (elemPermissive) {
                        elemType = t; // adopt the concrete type if we have one
                    } else if (!tPermissive && !elemFuncPermissive &&
                               !argMatchesParam(t, elemType) && !argMatchesParam(elemType, t)) {
                        // Before erroring, check if both types share a common trait.
                        // If so, widen the element type to that trait.
                        std::string common = m_traits.commonTrait(elemType, t);
                        if (!common.empty()) {
                            elemType = Type::named(common);
                        } else {
                            // Sibling nullary constructors share their parent
                            // ADT as a natural list element type. Preserve
                            // constructor refinements for overload selection,
                            // but widen `[Less, Equal, Greater]` to
                            // `[Comparison]` instead of rejecting it.
                            auto* lhs = std::get_if<NamedType>(&elemType->kind);
                            auto* rhs = std::get_if<NamedType>(&t->kind);
                            auto lhsOwner = lhs
                                ? m_adtOfConstructor.find(lhs->name)
                                : m_adtOfConstructor.end();
                            auto rhsOwner = rhs
                                ? m_adtOfConstructor.find(rhs->name)
                                : m_adtOfConstructor.end();
                            if (lhsOwner != m_adtOfConstructor.end() &&
                                rhsOwner != m_adtOfConstructor.end() &&
                                lhsOwner->second == rhsOwner->second) {
                                elemType = Type::named(lhsOwner->second);
                            } else {
                                error(expr.location,
                                      "List elements must be the same type. Expected " +
                                      typeToString(elemType) + ", got " + typeToString(t));
                            }
                        }
                    }
                }
            }
            return Type::list(elemType);
        }
        else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            TypePtr keyType = Type::unknown();
            TypePtr valueType = Type::unknown();
            for (const auto& entry : node.entries) {
                if (entry.key) {
                    auto k = inferExpr(*entry.key);
                    if (std::holds_alternative<UnknownType>(keyType->kind)) keyType = k;
                }
                if (entry.value) {
                    auto v = inferExpr(*entry.value);
                    if (std::holds_alternative<UnknownType>(valueType->kind)) valueType = v;
                }
            }
            return Type::map(keyType, valueType);
        }
        else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            std::vector<TypePtr> types;
            for (const auto& elem : node.elements) {
                types.push_back(elem ? inferExpr(*elem) : Type::unknown());
            }
            return Type::tuple(std::move(types));
        }
        else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            auto startType = node.start ? inferExpr(*node.start) : Type::unknown();
            if (node.end) inferExpr(*node.end);
            // Infer element type from the start bound: Char ranges → Range<Char>,
            // everything else → Range<Integer> (the common case).
            auto* prim = std::get_if<PrimitiveType>(&startType->kind);
            auto elemType = (prim && prim->kind == PrimitiveType::Char)
                ? Type::charT() : Type::integer();
            return Type::named("Range", {elemType});
        }
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            if (node.letPattern) {
                // `if let Pattern = expr` — infer scrutinee, bind pattern vars
                // in a scope covering only the then-body (already scoped in
                // resolve pass). Skip Bool check — it's a pattern match.
                auto scrutinee = node.condition
                    ? inferExpr(*node.condition) : nullptr;
                pushScope();
                if (node.letPattern)
                    bindPatternVars(*node.letPattern, scrutinee);
            } else if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "If condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            // A branch that exits the function produces nothing for the
            // conditional to agree with, so it must not drive the result type.
            auto branchBodyType = [this](const std::vector<ast::ExprPtr>& body) {
                auto inferred = inferBody(body);
                return alwaysReturns(body) ? Type::voidType() : inferred;
            };
            auto thenType = branchBodyType(node.thenBody);
            if (node.letPattern) popScope();
            TypePtr branchType = resolve(thenType);  // tracks the first concrete non-Never branch
            for (const auto& [cond, body] : node.elifs) {
                if (cond) inferExpr(*cond);
                auto elifType = resolve(branchBodyType(body));
                auto rt = resolve(branchType);
                bool rtPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                    std::holds_alternative<UnknownType>(rt->kind) ||
                                    std::holds_alternative<VoidType>(rt->kind);
                bool rePermissive = std::holds_alternative<TypeVar>(elifType->kind) ||
                                    std::holds_alternative<UnknownType>(elifType->kind) ||
                                    std::holds_alternative<VoidType>(elifType->kind);
                if (!rtPermissive && !rePermissive &&
                    !argMatchesParam(elifType, rt) && !argMatchesParam(rt, elifType)) {
                    error(expr.location, "Branch type mismatch: 'if' returns " +
                          typeToString(rt) + " but 'elif' returns " + typeToString(elifType));
                }
                if (rtPermissive && !rePermissive) branchType = elifType;
            }
            if (node.elseBody) {
                auto elseType = resolve(branchBodyType(*node.elseBody));
                auto rt = resolve(branchType);
                bool thenPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                      std::holds_alternative<UnknownType>(rt->kind) ||
                                      std::holds_alternative<VoidType>(rt->kind);
                bool elsePermissive = std::holds_alternative<TypeVar>(elseType->kind) ||
                                      std::holds_alternative<UnknownType>(elseType->kind) ||
                                      std::holds_alternative<VoidType>(elseType->kind);
                if (!thenPermissive && !elsePermissive &&
                    !argMatchesParam(elseType, rt) && !argMatchesParam(rt, elseType)) {
                    error(expr.location, "Branch type mismatch: 'if' returns " +
                          typeToString(rt) + " but 'else' returns " + typeToString(elseType));
                }
                if (thenPermissive && !elsePermissive) branchType = elseType;
            }
            // With no `else` the conditional may produce nothing at all, so it
            // cannot hand its then-branch's type to the block it closes —
            // `if done? ... return Error(e) ... end` is not a Result-valued
            // expression.
            if (!node.elseBody) return Type::voidType();
            return resolve(branchType);
        }
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            TypePtr subjectType = node.subject ? inferExpr(*node.subject) : Type::unknown();
            TypePtr resultType = Type::unknown();
            for (const auto& clause : node.clauses) {
                pushScope();
                if (node.subjectBinding) {
                    defineVar(*node.subjectBinding, subjectType);
                }
                for (const auto& pat : clause.patterns) {
                    if (pat) {
                        checkPatternArity(*pat);
                        bindPatternVars(*pat, subjectType);
                    }
                }
                if (clause.guard && *clause.guard) inferExpr(**clause.guard);
                if (clause.body && alwaysReturns(*clause.body)) {
                    // The arm exits the function; it produces nothing for the
                    // match to agree with.
                    inferExpr(*clause.body);
                } else if (clause.body) {
                    auto t = resolve(inferExpr(*clause.body));
                    auto rt = resolve(resultType);
                    if (std::holds_alternative<UnknownType>(rt->kind) ||
                        std::holds_alternative<TypeVar>(rt->kind)) {
                        resultType = t;  // adopt first concrete arm type
                    } else {
                        // Check subsequent arms match the first concrete arm.
                        bool armPermissive = std::holds_alternative<TypeVar>(t->kind) ||
                                            std::holds_alternative<UnknownType>(t->kind);
                        if (!armPermissive && !argMatchesParam(t, rt) && !argMatchesParam(rt, t)) {
                            error(expr.location, "Match arm type mismatch: expected " +
                                  typeToString(rt) + " but arm returns " + typeToString(t));
                        }
                    }
                }
                popScope();
            }
            checkMatchExhaustiveness(node, expr.location);
            return resultType;
        }
        else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            TypePtr resultType = Type::unknown();
            for (const auto& clause : node.clauses) {
                pushScope();
                if (node.senderBinding)
                    defineVar(*node.senderBinding,
                              Type::named("Process", {Type::unknown()}));
                for (const auto& pattern : clause.patterns)
                    if (pattern) {
                        checkPatternArity(*pattern);
                        bindPatternVars(*pattern);
                    }
                if (clause.guard && *clause.guard)
                    inferExpr(**clause.guard);
                if (clause.body) {
                    auto arm = resolve(inferExpr(*clause.body));
                    if (std::holds_alternative<UnknownType>(
                            resolve(resultType)->kind))
                        resultType = arm;
                }
                popScope();
            }
            if (node.timeout && *node.timeout) inferExpr(**node.timeout);
            if (node.afterBody && *node.afterBody) {
                auto after = resolve(inferExpr(**node.afterBody));
                if (std::holds_alternative<UnknownType>(
                        resolve(resultType)->kind))
                    resultType = after;
            }
            return resultType;
        }
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            return node.value ? inferExpr(*node.value) : Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            // Lambda used as a value (not a trailing block — that path goes
            // through inferBlock). Infer param types and body, return a proper
            // FuncType so call-site checking can validate the argument types.
            if (node.params.empty()) {
                m_blockDepth++;
                auto bodyType = inferBody(node.body);
                m_blockDepth--;
                return Type::func({}, resolve(bodyType));
            }
            pushScope();
            std::vector<TypePtr> paramTypes;
            std::unordered_map<std::string, TypePtr> genericVars;
            for (const auto& param : node.params) {
                auto pt = param.type
                    ? resolveTypeExpr(**param.type, genericVars)
                    : freshTypeVar();
                paramTypes.push_back(pt);
                if (param.name != "_") defineVar(param.name, pt);
            }
            m_blockDepth++;
            auto bodyType = inferBody(node.body);
            m_blockDepth--;
            auto resultType = resolve(bodyType);
            if (node.returnAnnotation) {
                auto declared =
                    resolveTypeExpr(**node.returnAnnotation, genericVars);
                if (!std::holds_alternative<UnknownType>(resultType->kind) &&
                    !std::holds_alternative<TypeVar>(resultType->kind) &&
                    !argMatchesParam(resultType, declared))
                    typeMismatch(expr.location, declared, resultType);
                resultType = declared;
            }
            popScope();
            // Resolve param types after body inference — body may have constrained them.
            for (auto& pt : paramTypes) pt = resolve(pt);
            return Type::func(std::move(paramTypes), resultType);
        }
        else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            pushScope();
            inferBody(node.body);
            popScope();
            // Process<Msg> — Msg is a fresh TypeVar that unification resolves
            // against the declared return-type annotation (e.g. -> Process<Counter>).
            return Type::named("Process", {freshTypeVar()});
        }
        else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            inferBody(node.body);
            return Type::voidType();  // infinite loop never returns
        }
        else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "While condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            inferBody(node.body);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            // Check each value against the field's DECLARED type. Without
            // this, `User { name: 42 }` on `name : String` was accepted and
            // only surfaced later — or not at all.
            auto record = m_recordFields.find(node.typeName);
            for (const auto& [fieldName, val] : node.fields) {
                if (!val) continue;
                auto valueType = resolve(inferExpr(*val));
                if (record == m_recordFields.end()) continue;
                auto declared = record->second.find(fieldName);
                // An unknown field name used to be skipped, which made a
                // renamed field silently absorb the old spelling: the
                // interpreter stored `zebra:` on a record with no such field
                // and `.zebra` read it straight back, while BEAM only failed at
                // runtime with "Undefined method: zebra". Renaming a field
                // therefore broke callers with no diagnostic at all. The
                // pattern side has always checked this (see checkPatternArity);
                // construction just never did.
                if (declared == record->second.end()) {
                    // List the layout: on a rename or a typo the useful
                    // question is immediately "then what ARE the fields?".
                    // Sorted so the message is stable — m_recordFields is a
                    // hash map.
                    std::vector<std::string> available;
                    available.reserve(record->second.size());
                    for (const auto& [known, _] : record->second)
                        available.push_back(known);
                    std::sort(available.begin(), available.end());
                    std::string layout;
                    for (std::size_t i = 0; i < available.size(); i++) {
                        if (i) layout += ", ";
                        layout += available[i];
                    }
                    error(val->location,
                          "record `" + node.typeName + "` has no field `" +
                          fieldName + "` — it has " +
                          (layout.empty() ? "no fields" : layout));
                    continue;
                }
                auto expected = resolve(declared->second);
                // Stay quiet where either side is open ANYWHERE inside it: a
                // type variable, a type the checker could not pin down, or a
                // trait constraint, which names a set rather than a concrete
                // type. Two cases this must not reject: `make Integer do …
                // Duration { seconds: this * 1.0 }` infers `Number` for the
                // value, and a `Map<Any, String>` field takes `{"k": "v"}`
                // — there the openness is nested, not at the top.
                if (containsOpenType(valueType) || containsOpenType(expected))
                    continue;
                if (!argMatchesParam(valueType, expected))
                    error(val->location,
                          "`" + node.typeName + "." + fieldName +
                          "` expects " + typeToString(expected) + ", but got " +
                          typeToString(valueType));
            }
            return Type::named(node.typeName);
        }
        else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                if (!std::holds_alternative<UnknownType>(condType->kind) &&
                    !std::holds_alternative<TypeVar>(condType->kind) &&
                    !typesEqual(condType, Type::boolean())) {
                    error(expr.location, "If condition must be Bool, got " +
                          typeToString(condType));
                }
            }
            // `expr if cond` is a guarded STATEMENT: when the guard is false
            // nothing is produced, so it cannot hand its expression's type to
            // the block it closes. `return Ok(x) if done?` is the shape that
            // matters — it must not make its branch look like it yields a
            // Result.
            if (node.expr) {
                inferExpr(*node.expr);
                return Type::voidType();
            }
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            return inferBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "then/else condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            auto thenType = node.thenExpr ? inferExpr(*node.thenExpr) : Type::unknown();
            if (node.elseExpr) {
                auto elseType = inferExpr(*node.elseExpr);
                auto rt = resolve(thenType);
                auto re = resolve(elseType);
                bool thenPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                      std::holds_alternative<UnknownType>(rt->kind);
                bool elsePermissive = std::holds_alternative<TypeVar>(re->kind) ||
                                      std::holds_alternative<UnknownType>(re->kind);
                if (!thenPermissive && !elsePermissive &&
                    !argMatchesParam(re, rt) && !argMatchesParam(rt, re)) {
                    error(expr.location, "Branch type mismatch: 'then' returns " +
                          typeToString(rt) + " but 'else' returns " + typeToString(re));
                }
            }
            return resolve(thenType);
        }
        else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
            // &.method → (T) -> result; T unknown until used in context.
            auto paramType = freshTypeVar();
            std::vector<TypePtr> callArgs = {paramType};
            for (const auto& arg : node.args) {
                if (arg) callArgs.push_back(inferExpr(*arg));
            }
            auto resultType = checkCall(node.name, callArgs, expr.location);
            return Type::func({paramType}, resolve(resultType));
        }
        else if constexpr (std::is_same_v<T, ast::CurryPlaceholder>) {
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
            // Infer bound args for side-effect tracking.
            int boundCount = 0, openCount = 0;
            for (const auto& group : node.argGroups)
                for (const auto& arg : group)
                    if (std::holds_alternative<ast::CurryPlaceholder>(arg->kind))
                        openCount++;
                    else { inferExpr(*arg); boundCount++; }

            // Determine arity to compute remaining open param count.
            int arity = -1;
            if (node.isOperator) {
                arity = (node.name == "!") ? 1 : 2; // `~(!)` is the unary one
            } else {
                auto usit = m_userSignatures.find(node.name);
                if (usit != m_userSignatures.end() && !usit->second.empty()) {
                    if (node.argGroups.empty()) {
                        std::vector<const Signature*> distinct;
                        for (const auto& signature : usit->second) {
                            bool duplicate = std::any_of(
                                distinct.begin(), distinct.end(),
                                [&](const Signature* other) {
                                    if (other->params.size() !=
                                        signature.params.size())
                                        return false;
                                    for (size_t i = 0;
                                         i < signature.params.size(); ++i)
                                        if (!typesEqual(
                                                other->params[i],
                                                signature.params[i]))
                                            return false;
                                    return true;
                                });
                            if (!duplicate) distinct.push_back(&signature);
                        }
                        if (distinct.size() > 1) {
                            std::string message =
                                "Cannot reference overloaded function `" +
                                node.name +
                                "` without disambiguating arguments";
                            for (const auto* signature : distinct)
                                message += "\n\n" +
                                    displaySignature(node.name, *signature);
                            error(expr.location, message);
                        }
                    }
                    arity = static_cast<int>(usit->second[0].params.size());
                }
            }

            // Remaining params = explicit placeholders + unfilled arity slots.
            int remaining = openCount;
            if (arity >= 0) remaining = std::max(openCount, arity - boundCount);

            // Build Func type with `remaining` fresh TypeVar params.
            if (remaining <= 0) return freshTypeVar(); // fully applied
            std::vector<TypePtr> params;
            for (int i = 0; i < remaining; i++) params.push_back(freshTypeVar());
            return Type::func(std::move(params), freshTypeVar());
        }
        else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
            // Inside a make block, `this` / `@field` has the record type.
            if (m_currentMakeType) return m_currentMakeType;
            return Type::unknown();
        }
        else {
            return Type::unknown();
        }
    }, expr.kind);
    m_typeMap[&expr] = result;
    return result;
}

auto TypeChecker::inferBinaryOp(TokenType op, const TypePtr& left, const TypePtr& right,
                                SourceLocation loc) -> TypePtr {
    // Type predicates used both in the TypeVar bail-out and the concrete section.
    auto isString = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::String;
    };
    auto isChar = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Char;
    };
    auto isNumeric = [](const TypePtr& t) -> bool {
        if (auto* prim = std::get_if<PrimitiveType>(&t->kind))
            return prim->kind == PrimitiveType::Integer;
        return std::holds_alternative<SizedIntType>(t->kind) ||
               std::holds_alternative<SizedFloatType>(t->kind);
    };

    // Resolve TypeVars through the substitution map — a prior operation in the
    // same body may have already constrained them (e.g. `n - 1` constrains n
    // to Number before `n == 0` is checked).
    auto lhs = resolve(left);
    auto rhs = resolve(right);

    {
        auto lhsIsVar = std::holds_alternative<TypeVar>(lhs->kind);
        auto rhsIsVar = std::holds_alternative<TypeVar>(rhs->kind);
        auto lhsIsUnk = std::holds_alternative<UnknownType>(lhs->kind);
        auto rhsIsUnk = std::holds_alternative<UnknownType>(rhs->kind);

        if (lhsIsVar || rhsIsVar || lhsIsUnk || rhsIsUnk) {
            auto varId = [](const TypePtr& t) -> int {
                if (auto* tv = std::get_if<TypeVar>(&t->kind)) return tv->id;
                return -1;
            };
            auto isConcrete = [](const TypePtr& t) {
                return !std::holds_alternative<TypeVar>(t->kind) &&
                       !std::holds_alternative<UnknownType>(t->kind);
            };

            switch (op) {
                // -, *, /, %, ^: both operands must be Number.
                case TokenType::Minus: case TokenType::Star:
                case TokenType::Slash: case TokenType::Percent: case TokenType::Caret: {
                    auto nc = Type::constrained("N", "Number");
                    if (int id = varId(lhs); id >= 0) unifyVar(id, nc);
                    if (int id = varId(rhs); id >= 0) unifyVar(id, nc);
                    if (isConcrete(lhs)) return lhs;
                    if (isConcrete(rhs)) return rhs;
                    return resolve(lhs);
                }
                // +: String/Char on one side → constrain TypeVar to String;
                //    numeric on one side → constrain TypeVar to Number.
                case TokenType::Plus: {
                    if (isConcrete(lhs) && (isString(lhs) || isChar(lhs))) {
                        if (int id = varId(rhs); id >= 0) unifyVar(id, Type::string());
                        return Type::string();
                    }
                    if (isConcrete(rhs) && (isString(rhs) || isChar(rhs))) {
                        if (int id = varId(lhs); id >= 0) unifyVar(id, Type::string());
                        return Type::string();
                    }
                    if (isConcrete(lhs) && isNumeric(lhs)) {
                        if (int id = varId(rhs); id >= 0) unifyVar(id, Type::constrained("N", "Number"));
                        return lhs;
                    }
                    if (isConcrete(rhs) && isNumeric(rhs)) {
                        if (int id = varId(lhs); id >= 0) unifyVar(id, Type::constrained("N", "Number"));
                        return rhs;
                    }
                    return lhs;
                }
                // Ordered comparisons: constrain TypeVar to the concrete side.
                case TokenType::LessThan: case TokenType::GreaterThan:
                case TokenType::LessEq: case TokenType::GreaterEq: {
                    if (int id = varId(lhs); id >= 0 && isConcrete(rhs)) unifyVar(id, rhs);
                    if (int id = varId(rhs); id >= 0 && isConcrete(lhs)) unifyVar(id, lhs);
                    return Type::boolean();
                }
                case TokenType::EqEq: case TokenType::NotEq:
                    return Type::boolean();
                case TokenType::AmpAmp: case TokenType::PipePipe: {
                    if (int id = varId(lhs); id >= 0) unifyVar(id, Type::boolean());
                    if (int id = varId(rhs); id >= 0) unifyVar(id, Type::boolean());
                    return Type::boolean();
                }
                default:
                    return lhs;
            }
        }
    }

    auto isFloat = [](const TypePtr& t) { return std::holds_alternative<SizedFloatType>(t->kind); };
    auto isIntegerLike = [](const TypePtr& t) {
        if (auto* prim = std::get_if<PrimitiveType>(&t->kind)) return prim->kind == PrimitiveType::Integer;
        return std::holds_alternative<SizedIntType>(t->kind);
    };
    auto isBool = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Bool;
    };

    bool leftIsString = isString(lhs), rightIsString = isString(rhs);
    bool leftIsFloat = isFloat(lhs), rightIsFloat = isFloat(rhs);
    bool leftIsInt = isIntegerLike(lhs), rightIsInt = isIntegerLike(rhs);

    switch (op) {
        case TokenType::Plus:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString && rightIsString)
                return Type::string();
            // String + Char, Char + String, Char + Char — all produce String
            // (String = [Char], so appending a Char or two Chars is valid concatenation).
            if ((leftIsString || isChar(lhs)) && (rightIsString || isChar(rhs)))
                return Type::string();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot add " + typeToString(lhs) + " and " + typeToString(rhs));
                return Type::string();
            }
            if (!argMatchesParam(lhs, rhs) && !argMatchesParam(rhs, lhs)) {
                error(loc, "Operator '+' requires matching types, got " +
                      typeToString(lhs) + " and " + typeToString(rhs));
            }
            return lhs;

        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
        case TokenType::Caret:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot use arithmetic operator on String");
                return Type::integer();
            }
            return lhs;

        case TokenType::EqEq:
        case TokenType::NotEq:
            return Type::boolean();

        case TokenType::LessThan:
        case TokenType::GreaterThan:
        case TokenType::LessEq:
        case TokenType::GreaterEq:
            if (isBool(lhs)) {
                error(loc, "Cannot compare Bool values with '<', '>', '<=', '>='");
            }
            return Type::boolean();

        case TokenType::AmpAmp:
        case TokenType::PipePipe:
            if (!isBool(lhs)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(lhs));
            }
            if (!isBool(rhs)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(rhs));
            }
            return Type::boolean();

        default:
            return Type::unknown();
    }
}

// `Type.of(expr)` / `Type.returnedBy(fn)` written where a TYPE goes. The
// value-level forms answer at runtime from a recording made here; this one is
// resolved entirely at check time, because there is no value to fall back on.
// The type an expression NAMES, or null when it does not name one. A bare
// `Hello` is a type reference, not a value: it is exactly what `x.to(String)`
// already relies on. A CONSTRUCTOR is deliberately excluded — `World` is a
// value whose type is `Hello`.
auto TypeChecker::typeNameReference(const ast::Expr& expr) -> TypePtr {
    const auto* named = std::get_if<ast::UpperIdentifier>(&expr.kind);
    if (!named) return nullptr;
    if (m_nullaryConstructors.count(named->name) ||
        m_adtOfConstructor.count(named->name))
        return nullptr;
    ast::TypeName typeName;
    typeName.parts = {named->name};
    ast::TypeExpr asType;
    asType.location = expr.location;
    asType.kind = std::move(typeName);
    std::unordered_map<std::string, TypePtr> generics;
    return resolveTypeExpr(asType, generics);
}

// The single signature of the function an expression NAMES, or null when the
// expression is not a function name (or names an overload set).
auto TypeChecker::namedFunctionSignature(const ast::Expr& expr)
    -> const Signature* {
    std::string functionName;
    if (const auto* identifier = std::get_if<ast::Identifier>(&expr.kind)) {
        functionName = identifier->name;
    } else if (const auto* qualified = std::get_if<ast::MethodCall>(&expr.kind);
               qualified && qualified->args.empty() && qualified->receiver &&
               !qualified->parenthesized) {
        std::function<std::optional<std::string>(const ast::Expr&)> path;
        path = [&](const ast::Expr& receiver) -> std::optional<std::string> {
            if (auto* root = std::get_if<ast::UpperIdentifier>(&receiver.kind))
                return root->name;
            auto* segment = std::get_if<ast::MethodCall>(&receiver.kind);
            if (!segment || !segment->receiver || !segment->args.empty())
                return std::nullopt;
            auto parent = path(*segment->receiver);
            return parent ? std::optional<std::string>{*parent + "." + segment->method}
                          : std::nullopt;
        };
        if (auto module = path(*qualified->receiver))
            functionName = *module + "::" + qualified->method;
    }
    if (functionName.empty()) return nullptr;
    if (auto user = m_userSignatures.find(functionName);
        user != m_userSignatures.end() && user->second.size() == 1)
        return &user->second.front();
    m_importedSignatureCache = importedCandidateSignatures(functionName);
    if (m_importedSignatureCache.size() == 1)
        return &m_importedSignatureCache.front();
    return nullptr;
}

auto TypeChecker::resolveTypeQuery(const ast::TypeQuery& query) -> TypePtr {
    if (!query.argument) return Type::unknown();
    if (query.query == "of") {
        if (auto referenced = typeNameReference(*query.argument)) return referenced;
        return resolve(inferExpr(*query.argument));
    }

    // returnedBy: the argument NAMES a function.
    std::string functionName;
    if (const auto* identifier = std::get_if<ast::Identifier>(&query.argument->kind)) {
        functionName = identifier->name;
    } else if (const auto* qualified =
                   std::get_if<ast::MethodCall>(&query.argument->kind);
               qualified && qualified->args.empty() && qualified->receiver) {
        std::function<std::optional<std::string>(const ast::Expr&)> path;
        path = [&](const ast::Expr& receiver) -> std::optional<std::string> {
            if (auto* root = std::get_if<ast::UpperIdentifier>(&receiver.kind))
                return root->name;
            auto* segment = std::get_if<ast::MethodCall>(&receiver.kind);
            if (!segment || !segment->receiver || !segment->args.empty())
                return std::nullopt;
            auto parent = path(*segment->receiver);
            return parent ? std::optional<std::string>{*parent + "." + segment->method}
                          : std::nullopt;
        };
        if (auto module = path(*qualified->receiver))
            functionName = *module + "::" + qualified->method;
    }

    std::vector<Signature> candidates;
    if (!functionName.empty()) {
        if (auto user = m_userSignatures.find(functionName);
            user != m_userSignatures.end())
            candidates = user->second;
        if (candidates.empty())
            candidates = importedCandidateSignatures(functionName);
    }
    if (candidates.empty()) {
        // A computed ALIAS resolves before signatures are registered (the
        // signatures may themselves name the alias), so fall back to the
        // declaration's own return annotation.
        if (m_functionDeclarations.count(functionName) > 1) {
            error(query.argument->location,
                  "`Type.returnedBy` cannot choose between the overloads of `" +
                  functionName + "`");
            return Type::unknown();
        }
        if (auto declaration = m_functionDeclarations.find(functionName);
            declaration != m_functionDeclarations.end()) {
            const auto* function = declaration->second;
            if (!function->clauses.empty() &&
                function->clauses.front().returnAnnotation) {
                std::unordered_map<std::string, TypePtr> generics;
                return resolveTypeExpr(**function->clauses.front().returnAnnotation,
                                       generics);
            }
            error(query.argument->location,
                  "`Type.returnedBy` needs `" + functionName +
                  "` to declare its return type");
            return Type::unknown();
        }
        error(query.argument->location,
              "`Type.returnedBy` needs the NAME of a function; a lambda or a "
              "function value carries no signature");
        return Type::unknown();
    }
    if (candidates.size() > 1) {
        std::string message =
            "`Type.returnedBy` cannot choose between the overloads of `" +
            functionName + "`";
        for (const auto& candidate : candidates)
            message += "\n\n" + displaySignature(functionName, candidate);
        error(query.argument->location, message);
        return Type::unknown();
    }
    return candidates.front().result;
}

// The type a `let`/`var` annotation declares, after checking the initializer
// against it. Null when there is no annotation.
auto TypeChecker::declaredBindingType(const std::optional<ast::TypeExprPtr>& annotation,
                                      const TypePtr& valueType,
                                      SourceLocation loc) -> TypePtr {
    if (!annotation || !*annotation) return nullptr;
    std::unordered_map<std::string, TypePtr> generics;
    auto declared = resolveTypeExpr(**annotation, generics);
    if (!declared) return nullptr;
    auto actual = resolve(valueType);
    // A gradual initializer (`?`, a type variable) is accepted: pinning it
    // down is exactly what the annotation is for. `This` — the trait
    // placeholder — has no substitution mechanism yet and leaks out of trait
    // default methods as a literal type name, so it is treated the same way
    // (checkCall bails on it for the same reason).
    const bool unsubstitutedThis =
        std::holds_alternative<NamedType>(actual->kind) &&
        std::get<NamedType>(actual->kind).name == "This";
    if (!std::holds_alternative<UnknownType>(actual->kind) &&
        !std::holds_alternative<TypeVar>(actual->kind) && !unsubstitutedThis &&
        !argMatchesParam(actual, declared))
        typeMismatch(loc, declared, actual);
    return declared;
}

// The type a standalone `name : T` declaration gives a top-level constant,
// after checking its definition against it.
auto TypeChecker::declaredConstantType(const std::string& name,
                                       const TypePtr& valueType,
                                       SourceLocation loc) -> TypePtr {
    auto declared = m_userSignatures.find(name);
    if (declared == m_userSignatures.end() || declared->second.size() != 1)
        return nullptr;
    const auto& signature = declared->second.front();
    if (!signature.params.empty() || !signature.result) return nullptr;
    auto actual = resolve(valueType);
    const bool unsubstitutedThis =
        std::holds_alternative<NamedType>(actual->kind) &&
        std::get<NamedType>(actual->kind).name == "This";
    if (!std::holds_alternative<UnknownType>(actual->kind) &&
        !std::holds_alternative<TypeVar>(actual->kind) && !unsubstitutedThis &&
        !argMatchesParam(actual, signature.result))
        typeMismatch(loc, signature.result, actual);
    return signature.result;
}

auto TypeChecker::displayTypeOf(const ast::Expr* expr) const -> TypePtr {
    std::function<TypePtr(const TypePtr&)> widen =
        [&](const TypePtr& type) -> TypePtr {
        if (!type) return type;
        auto widenAll = [&](std::vector<TypePtr> types) {
            for (auto& element : types) element = widen(element);
            return types;
        };
        if (auto* named = std::get_if<NamedType>(&type->kind)) {
            if (auto owner = m_adtOfConstructor.find(named->name);
                owner != m_adtOfConstructor.end() &&
                owner->second != named->name)
                return Type::named(owner->second);
            // Type ARGUMENTS are left alone: a phantom typestate parameter is
            // spelled with constructors too (`FileHandle<Write>`), and
            // widening those to their ADT (`FileHandle<WriteCapability>`)
            // erases exactly the distinction the parameter exists to make.
            return type;
        }
        if (auto* list = std::get_if<ListType>(&type->kind))
            return Type::list(widen(list->element));
        if (auto* optional = std::get_if<OptionalType>(&type->kind))
            return Type::optional(widen(optional->inner));
        if (auto* map = std::get_if<MapType>(&type->kind))
            return Type::map(widen(map->key), widen(map->value));
        if (auto* tuple = std::get_if<TupleType>(&type->kind))
            return Type::tuple(widenAll(tuple->elements));
        return type;
    };
    return widen(typeOf(expr));
}

auto TypeChecker::satisfiesTrait(const TypePtr& type,
                                 const std::string& traitName) const -> bool {
    if (m_traits.satisfies(type, traitName)) return true;
    // A nullary constructor's value type is the constructor name (`Dog`,
    // `Meter`), but the conformance is declared on the ADT that owns it
    // (`make Animal, implement: Speaker`). Lift to the owner and retry —
    // otherwise `describe(Dog)` rejects a value the trait plainly covers.
    auto* named = std::get_if<NamedType>(&type->kind);
    if (!named) return false;
    auto ownerSatisfies = [&](const std::string& ownerName) {
        return ownerName != named->name &&
               m_traits.satisfies(Type::named(ownerName), traitName);
    };
    if (auto owner = m_adtOfConstructor.find(named->name);
        owner != m_adtOfConstructor.end() && ownerSatisfies(owner->second))
        return true;
    // Constructors of a `using`-imported module are registered per module
    // rather than in the file-local ADT map.
    for (const auto& [module, constructors] : m_moduleConstructors) {
        auto constructor = constructors.find(named->name);
        if (constructor != constructors.end() &&
            moduleMemberImported(module, named->name) &&
            ownerSatisfies(constructor->second.typeName))
            return true;
    }
    return false;
}

auto TypeChecker::argMatchesParam(const TypePtr& argType, const TypePtr& paramType) const -> bool {
    auto isPermissive = [](const TypePtr& t) {
        return std::holds_alternative<UnknownType>(t->kind) || std::holds_alternative<TypeVar>(t->kind);
    };
    if (isPermissive(argType) || isPermissive(paramType)) return true;
    // Never is the bottom type — a Never-typed expression (never returns) is
    // compatible with any expected type.
    if (std::holds_alternative<VoidType>(argType->kind)) return true;
    if (auto* constrained = std::get_if<ConstrainedType>(&paramType->kind)) {
        return satisfiesTrait(argType, constrained->traitName);
    }
    // NamedType param that is itself a trait name: `Shape`, `Comparable`, etc.
    // Occurs when a heterogeneous list was widened to a trait element type and
    // then each element is checked against it, or when a trait-typed value is
    // passed to a ConstrainedType param that got resolved to NamedType.
    // argType matches if it implements that trait, or if argType IS that trait.
    // `make Tuple` names the whole family: a tuple's arity is part of its
    // type, so there is no single structural spelling for "any tuple" and the
    // receiver has to be matched by name. Without this a `Tuple` receiver
    // method never applied to an actual `(Integer, String)`.
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind);
        paramNamed && paramNamed->name == "Tuple" && paramNamed->typeArgs.empty())
        return std::holds_alternative<TupleType>(argType->kind);
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        if (m_traits.get(paramNamed->name)) {
            if (auto* argNamed = std::get_if<NamedType>(&argType->kind);
                argNamed && argNamed->name == paramNamed->name)
                return true; // trait-typed value matches trait param
            return satisfiesTrait(argType, paramNamed->name);
        }
    }
    // Sized ints/floats and arbitrary-precision Integer aren't distinguished
    // at runtime yet (IntValue is one int64_t, FloatValue one double — see
    // the type-system plan's Runtime representation section), so don't
    // hard-error on a width/precision distinction the runtime doesn't keep:
    // any two Integer-trait members are compatible with each other, and
    // likewise for Float. Integer-vs-Float itself stays a real mismatch —
    // that distinction *is* runtime-backed today.
    if (m_traits.satisfies(argType, "Integer") && m_traits.satisfies(paramType, "Integer")) return true;
    if (m_traits.satisfies(argType, "Float") && m_traits.satisfies(paramType, "Float")) return true;

    // Recurse into compound types structurally rather than requiring exact
    // equality — e.g. `[Int]` vs `[Integer]` (the relaxation above, but
    // inside a list) and `String` (= `[Char]`) vs a generic `[A]` param
    // both need this, not just bare params.
    if (auto* paramList = std::get_if<ListType>(&paramType->kind)) {
        auto* argList = std::get_if<ListType>(&argType->kind);
        return argList && argMatchesParam(argList->element, paramList->element);
    }
    if (auto* paramTuple = std::get_if<TupleType>(&paramType->kind)) {
        auto* argTuple = std::get_if<TupleType>(&argType->kind);
        if (!argTuple || argTuple->elements.size() != paramTuple->elements.size()) return false;
        for (size_t i = 0; i < paramTuple->elements.size(); i++) {
            if (!argMatchesParam(argTuple->elements[i], paramTuple->elements[i])) return false;
        }
        return true;
    }
    if (auto* paramMap = std::get_if<MapType>(&paramType->kind)) {
        auto* argMap = std::get_if<MapType>(&argType->kind);
        return argMap && argMatchesParam(argMap->key, paramMap->key) &&
               argMatchesParam(argMap->value, paramMap->value);
    }
    if (auto* paramOpt = std::get_if<OptionalType>(&paramType->kind)) {
        // `None` has no payload from which to infer an inner type, but it is
        // valid for every optional return/parameter type.
        if (auto* argNamed = std::get_if<NamedType>(&argType->kind);
            argNamed && argNamed->typeArgs.empty() && argNamed->name == "None")
            return true;
        auto* argOpt = std::get_if<OptionalType>(&argType->kind);
        return argOpt && argMatchesParam(argOpt->inner, paramOpt->inner);
    }
    if (auto* argOpt = std::get_if<OptionalType>(&argType->kind)) {
        return argMatchesParam(argOpt->inner, paramType);
    }
    auto isUnitLike = [](const TypePtr& t) -> bool {
        if (auto* p = std::get_if<PrimitiveType>(&t->kind))
            return p->kind == PrimitiveType::Unit;
        if (auto* tup = std::get_if<TupleType>(&t->kind))
            return tup->elements.empty();
        if (auto* n = std::get_if<NamedType>(&t->kind))
            return n->typeArgs.empty() && n->name == "Void";
        return false;
    };
    auto isStringType = [](const TypePtr& t) -> bool {
        if (auto* n = std::get_if<NamedType>(&t->kind))
            return n->typeArgs.empty() &&
                   (n->name == "String" || n->name == "FilePath");
        if (auto* p = std::get_if<PrimitiveType>(&t->kind))
            return p->kind == PrimitiveType::String;
        return false;
    };
    // FunctionType param — e.g. `(T-1) -> Bool` vs `(T81) -> Bool`. Without
    // this branch both sides fall to typesEqual which fails on mismatched
    // TypeVar ids even though both are permissive. Recurse into params and
    // result so lambda arguments to map/filter/each pass the checker.
    if (std::holds_alternative<FuncType>(paramType->kind)) {
        auto* argFn = std::get_if<FuncType>(&argType->kind);
        if (!argFn) return isPermissive(argType);
        // `(A) -> (B) -> C` and `(A, B) -> C` are the same callable shape
        // in Kex. Normalize both sides before comparing so a multi-parameter
        // lambda matches a curried source annotation (and vice versa).
        auto flatten = [](const TypePtr& type) {
            std::pair<std::vector<TypePtr>, TypePtr> result{{}, type};
            auto current = type;
            while (auto* fn = std::get_if<FuncType>(&current->kind)) {
                result.first.insert(
                    result.first.end(), fn->params.begin(), fn->params.end());
                current = fn->result;
            }
            result.second = current;
            return result;
        };
        auto [argParams, argResult] = flatten(argType);
        auto [paramParams, paramResult] = flatten(paramType);
        // A block may DESTRUCTURE a tuple element instead of naming it:
        // `pairs.each do |k, v|` over `[(String, Int)]` binds one element
        // across two parameters, and both backends spread it. The checker was
        // the only thing that disagreed.
        //
        // Decidable from the ARGUMENT alone, which is what makes it cheap:
        // expected-type propagation has already given the block's first
        // parameter the element type, so a 2-parameter block over
        // `[(String, Int)]` arrives here as `((String, Int), T2) -> Void`.
        // A first parameter that is a tuple of exactly as many elements as the
        // block has parameters is a destructuring block; anything else is a
        // real arity error and still reported as one.
        if (argParams.size() != paramParams.size()) {
            if (paramParams.size() != 1 || argParams.size() < 2) return false;
            if (auto* spread = std::get_if<TupleType>(&argParams[0]->kind)) {
                if (spread->elements.size() != argParams.size()) return false;
            } else if (!isPermissive(paramParams[0]) ||
                       !std::all_of(argParams.begin(), argParams.end(),
                                    [&](const TypePtr& p) {
                                        return isPermissive(p);
                                    })) {
                // Something IS known and it does not line up: a real arity
                // error, still reported. Only the case where neither side
                // knows anything is let through — `@conditions.each do |c, v|`
                // over a record field the checker has not resolved to a tuple.
                // Rejecting there would fail a program both backends run.
                return false;
            }
            if (isUnitLike(paramResult)) return true;
            return argMatchesParam(argResult, paramResult);
        }
        for (size_t i = 0; i < paramParams.size(); i++)
            if (!argMatchesParam(argParams[i], paramParams[i])) return false;
        // Void as the expected return means "result discarded" — accept any body type.
        if (isUnitLike(paramResult)) return true;
        return argMatchesParam(argResult, paramResult);
    }
    // NamedType with type args — e.g. `Range<Number>` param vs `Range<Integer>` arg.
    // Recurse into type arguments structurally so the inner types get the same
    // trait-relaxation treatment (argMatchesParam, not typesEqual).
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        auto* argNamed = std::get_if<NamedType>(&argType->kind);
        if (!argNamed || argNamed->name != paramNamed->name) {
            // A nullary ADT constructor is a refined value of its parent
            // type. This relationship comes from the ADT registry rather
            // than from any constructor spelling.
            if (argNamed && argNamed->typeArgs.empty() &&
                paramNamed->typeArgs.empty()) {
                auto owner = m_adtOfConstructor.find(argNamed->name);
                if (owner != m_adtOfConstructor.end() &&
                    owner->second == paramNamed->name)
                    return true;
                // The same constructor/ADT relationship must survive a
                // `using` boundary. Imported nullary variants are recorded
                // per provider module rather than in m_adtOfConstructor.
                for (const auto& [module, constructors] : m_moduleConstructors) {
                    auto imported = constructors.find(argNamed->name);
                    if (imported != constructors.end() &&
                        imported->second.typeName == paramNamed->name &&
                        moduleMemberImported(module, argNamed->name))
                        return true;
                }
            }
            if (isStringType(argType) && isStringType(paramType)) return true;
            return false;
        }
        // An unparameterized receiver declaration is the erased/wildcard
        // form of the named type. For example, `make Range` methods apply to
        // inferred `Range<Integer>` and `Range<Char>` values.
        if (paramNamed->typeArgs.empty()) return true;
        if (argNamed->typeArgs.size() != paramNamed->typeArgs.size()) return false;
        for (size_t i = 0; i < paramNamed->typeArgs.size(); i++) {
            if (!argMatchesParam(argNamed->typeArgs[i], paramNamed->typeArgs[i])) return false;
        }
        return true;
    }
    // UnionType param (e.g. type alias `Level = :a | :b | :c`) — arg
    // matches if it matches any branch of the union.
    if (auto* paramUnion = std::get_if<UnionType>(&paramType->kind)) {
        for (const auto& member : paramUnion->members) {
            if (member && argMatchesParam(argType, member)) return true;
        }
        return false;
    }

    if (isUnitLike(argType) && isUnitLike(paramType)) return true;

    if (isStringType(argType) && isStringType(paramType)) return true;

    return typesEqual(argType, paramType);
}

auto TypeChecker::displaySignature(const std::string& name, const Signature& sig) const -> std::string {
    // Collect TypeVar IDs in order of first appearance, then remap to
    // sequential negative IDs so they display as A, B, C, ... regardless
    // of the internal counter state when the signature was built.
    std::unordered_map<int, int> varRemap;
    int nextNeg = -1;
    std::function<void(const TypePtr&)> collectVars = [&](const TypePtr& t) {
        if (!t) return;
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeVar>) {
                if (!varRemap.count(node.id)) { varRemap[node.id] = nextNeg--; }
            } else if constexpr (std::is_same_v<T, ListType>) {
                collectVars(node.element);
            } else if constexpr (std::is_same_v<T, MapType>) {
                collectVars(node.key); collectVars(node.value);
            } else if constexpr (std::is_same_v<T, FuncType>) {
                for (const auto& p : node.params) collectVars(p);
                collectVars(node.result);
            } else if constexpr (std::is_same_v<T, TupleType>) {
                for (const auto& e : node.elements) collectVars(e);
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                collectVars(node.inner);
            } else if constexpr (std::is_same_v<T, NamedType>) {
                for (const auto& a : node.typeArgs) collectVars(a);
            }
        }, t->kind);
    };
    for (const auto& p : sig.params) collectVars(p);
    collectVars(sig.result);

    std::function<TypePtr(const TypePtr&)> remap = [&](const TypePtr& t) -> TypePtr {
        if (!t) return t;
        return std::visit([&](const auto& node) -> TypePtr {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeVar>) {
                auto it = varRemap.find(node.id);
                return it != varRemap.end() ? Type::typeVar(it->second) : t;
            } else if constexpr (std::is_same_v<T, ListType>) {
                return Type::list(remap(node.element));
            } else if constexpr (std::is_same_v<T, MapType>) {
                return Type::map(remap(node.key), remap(node.value));
            } else if constexpr (std::is_same_v<T, FuncType>) {
                std::vector<TypePtr> ps;
                for (const auto& p : node.params) ps.push_back(remap(p));
                return std::make_shared<Type>(Type{FuncType{std::move(ps), remap(node.result)}});
            } else if constexpr (std::is_same_v<T, TupleType>) {
                std::vector<TypePtr> es;
                for (const auto& e : node.elements) es.push_back(remap(e));
                return std::make_shared<Type>(Type{TupleType{std::move(es)}});
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                return Type::optional(remap(node.inner));
            } else if constexpr (std::is_same_v<T, NamedType>) {
                std::vector<TypePtr> args;
                for (const auto& a : node.typeArgs) args.push_back(remap(a));
                return Type::named(node.name, std::move(args));
            } else {
                return t;
            }
        }, t->kind);
    };

    auto displayType = [&](const TypePtr& t) -> std::string {
        if (auto* constrained = std::get_if<ConstrainedType>(&t->kind)) return constrained->traitName;
        return typeToString(remap(t));
    };
    std::string result = name + " : ";
    for (const auto& param : sig.params) {
        result += displayType(param) + " -> ";
    }
    result += displayType(sig.result);
    return result;
}

auto TypeChecker::checkCall(const std::string& name, const std::vector<TypePtr>& argTypes,
                            SourceLocation loc, bool isMethodCall,
                            const ast::MethodCall* methodCall) -> TypePtr {
    // A local function binding (`let f(x) ...` inside an expression block)
    // is represented as a FuncType variable rather than a top-level
    // FunctionDef signature. Resolve it before consulting global overloads.
    if (!isMethodCall && name.find("::") == std::string::npos) {
        auto local = lookupVar(name);
        auto resolvedLocal = local ? resolve(local) : nullptr;
        if (resolvedLocal)
            if (auto* function = std::get_if<FuncType>(&resolvedLocal->kind)) {
                if (function->params.size() != argTypes.size()) {
                    error(loc, "`" + name + "` expects " +
                        std::to_string(function->params.size()) +
                        " argument(s), got " + std::to_string(argTypes.size()));
                    return function->result;
                }
                for (size_t i = 0; i < argTypes.size(); ++i)
                    if (!argMatchesParam(argTypes[i], function->params[i]))
                        typeMismatch(loc, function->params[i], argTypes[i]);
                return function->result;
            }
    }
    const std::vector<Signature>* userSignatures = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() &&
        scoped != m_scopedDeclaredSignatures.end())
        userSignatures = &scoped->second;
    else if (auto user = m_userSignatures.find(name);
             user != m_userSignatures.end())
        userSignatures = &user->second;
    bool hasUser = userSignatures != nullptr;
    auto methodIt = m_methodSignatures.find(name);
    bool hasLocalMethods = isMethodCall && methodIt != m_methodSignatures.end();
    // A known record field is not an unrelated imported zero-argument
    // method with the same spelling (`Version.patch` vs HTTP.patch).
    if (isMethodCall && argTypes.size() == 1) {
        auto receiver = resolve(argTypes.front());
        if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
            if (auto record = m_recordFields.find(named->name);
                record != m_recordFields.end())
                if (auto field = record->second.find(name);
                    field != record->second.end()) {
                    return field->second;
                }
            // The same rule for a record the PRELUDE declares. Interfaces
            // carry field names but not their types, so the read is answered
            // gradually — the point is only that it is a field read at all,
            // and not a call to some same-named method: `moment.time.nanosecond`
            // is the Time record's field, never units.kex's `Integer.nanosecond`.
            if (m_importedInterfaces) {
                auto record =
                    m_importedInterfaces->recordFieldNames.find(named->name);
                if (record != m_importedInterfaces->recordFieldNames.end() &&
                    record->second.count(name))
                    return Type::unknown();
            }
        }
    }
    bool hasReceiverRefinementConflict = false;
    if (hasLocalMethods && !argTypes.empty()) {
        auto actual = resolve(argTypes[0]);
        for (const auto& sig : methodIt->second) {
            if (sig.params.empty()) continue;
            auto expected = resolve(sig.params[0]);
            auto* actualNamed = std::get_if<NamedType>(&actual->kind);
            auto* expectedNamed = std::get_if<NamedType>(&expected->kind);
            if (actualNamed && expectedNamed &&
                actualNamed->name == expectedNamed->name) {
                if (!argMatchesParam(actual, expected))
                    hasReceiverRefinementConflict = true;
            }
        }
    }

    std::vector<const ImportedFunction*> importedFunctions;
    std::string qualifiedModule;
    if (m_importedInterfaces) {
        if (auto separator = name.find("::"); separator != std::string::npos) {
            qualifiedModule = name.substr(0, separator);
            auto functionName = name.substr(separator + 2);
            if (auto module = m_importedInterfaces->modules.find(qualifiedModule);
                module != m_importedInterfaces->modules.end())
                if (auto functions = module->second.exports.find(functionName);
                    functions != module->second.exports.end())
                    for (const auto& function : functions->second)
                        importedFunctions.push_back(&function);
        } else if (isMethodCall) {
            if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
                functions != m_importedInterfaces->receiverFunctions.end())
                for (const auto& function : functions->second)
                    if (importedFunctionVisible(function))
                        importedFunctions.push_back(&function);
        } else {
            for (const auto& [_, module] : m_importedInterfaces->modules) {
                if (!module.automaticImport) continue;
                if (auto functions = module.exports.find(name);
                    functions != module.exports.end())
                    for (const auto& function : functions->second)
                        importedFunctions.push_back(&function);
            }
            // Receiver functions are also callable as bare functions via
            // UFCS (e.g. `even?(x)` instead of `x.even?`).
            if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
                functions != m_importedInterfaces->receiverFunctions.end())
                for (const auto& function : functions->second)
                    if (importedFunctionVisible(function))
                        importedFunctions.push_back(&function);
        }
    }
    // A module declared in the current compilation unit shadows a package
    // module with the same source identity. This matches runtime code-path
    // precedence and prevents imported ownership from hijacking local calls.
    if (!qualifiedModule.empty() && m_localModules.contains(qualifiedModule))
        importedFunctions.clear();
    if (isMethodCall && !argTypes.empty()) {
        auto actual = resolve(argTypes[0]);
        auto* actualNamed = std::get_if<NamedType>(&actual->kind);
        if (actualNamed)
            for (const auto* function : importedFunctions) {
                if (function->signature.params.empty()) continue;
                auto expected = resolve(function->signature.params[0]);
                auto* expectedNamed = std::get_if<NamedType>(&expected->kind);
                if (expectedNamed && expectedNamed->name == actualNamed->name &&
                    !argMatchesParam(actual, expected)) {
                    hasReceiverRefinementConflict = true;
                    break;
                }
            }
    }

    auto sameParams = [](const Signature& left, const Signature& right) {
        if (left.params.size() != right.params.size()) return false;
        for (size_t i = 0; i < left.params.size(); i++)
            if (!typesEqual(left.params[i], right.params[i])) return false;
        return true;
    };
    auto matchesActual = [&](const Signature& signature) {
        if (signature.params.size() != argTypes.size()) return false;
        for (size_t i = 0; i < signature.params.size(); i++)
            if (!argMatchesParam(argTypes[i], signature.params[i])) return false;
        return true;
    };
    for (size_t i = 0; i < importedFunctions.size(); i++)
        for (size_t j = i + 1; j < importedFunctions.size(); j++) {
            const auto& left = *importedFunctions[i];
            const auto& right = *importedFunctions[j];
            if (left.backendModule != right.backendModule &&
                matchesActual(left.signature) && matchesActual(right.signature) &&
                sameParams(left.signature, right.signature)) {
                error(loc, "ambiguous imported " +
                    std::string(isMethodCall && name.find("::") == std::string::npos
                        ? "receiver function '" : "function '") + name +
                    "' is provided by both '" + left.backendModule + "' and '" +
                    right.backendModule + "'");
                return Type::unknown();
            }
        }

    std::vector<Signature> importedSigs;
    for (const auto* function : importedFunctions)
        importedSigs.push_back(function->signature);

    std::vector<Signature> merged;
    const std::vector<Signature>* sigs = nullptr;
    // Calls made by ordinary user code should see the full local receiver
    // interface. While checking a make block's own implementation bodies,
    // retain the existing permissive behavior unless the receiver refinement
    // itself is wrong; generic helper calls in those bodies are not yet
    // sufficiently substituted to validate without false positives.
    const bool hasConcreteReceiver = !argTypes.empty() && [&] {
        auto receiver = resolve(argTypes.front());
        return !std::holds_alternative<TypeVar>(receiver->kind) &&
               !std::holds_alternative<UnknownType>(receiver->kind);
    }();
    bool useLocalMethods = hasLocalMethods &&
        (!m_inMakeBlock || hasReceiverRefinementConflict ||
         hasConcreteReceiver);
    // Local methods come FIRST. When a user type defines a method the prelude
    // also provides and both signatures match, the user's is the more specific
    // one. Ordering imported first let a GENERIC prelude signature win on type
    // variables alone: `Map.get :> K -> V?` matched `someMatch.get(1)` and the
    // call lowered to `kex_prelude:get`, while the 3-argument form (whose
    // prelude counterpart did not match) correctly reached the local method —
    // the same call site resolving two different ways by arity.
    // A local method whose receiver does not match still simply isn't a match.
    size_t localSigCount = 0;
    if (!importedSigs.empty() || useLocalMethods) {
        if (useLocalMethods) {
            merged.insert(merged.end(), methodIt->second.begin(), methodIt->second.end());
            localSigCount = merged.size();
        }
        merged.insert(merged.end(), importedSigs.begin(), importedSigs.end());
        if (hasUser)
            merged.insert(
                merged.end(), userSignatures->begin(), userSignatures->end());
        sigs = &merged;
    } else if (hasUser) {
        sigs = userSignatures;
    }
    if (!sigs) {
        // Record field access: `user.name` desugars to checkCall("name", [User]).
        // Look up the field type in the record registry before giving up.
        // Resolve TypeVars first — the receiver may have been constrained by
        // a previous inference step.
        if (!argTypes.empty()) {
            auto receiver = resolve(argTypes[0]);
            if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
                auto ri = m_recordFields.find(named->name);
                if (ri != m_recordFields.end()) {
                    auto fi = ri->second.find(name);
                    if (fi != ri->second.end()) return fi->second;
                }
            }
            // Trait-bounded receiver: `item.method()` where `item: SomeTrait`.
            // Look up the method in the trait's required methods to get return type.
            if (auto* ct = std::get_if<ConstrainedType>(&receiver->kind)) {
                if (const TraitDef* trait = m_traits.get(ct->traitName)) {
                    for (const auto& req : trait->requiredMethods) {
                        if (req.name == name) return req.result;
                    }
                }
            }
            // If the field name is unambiguously defined on exactly one record
            // type, constrain a TypeVar receiver to that record type.
            if (std::holds_alternative<TypeVar>(receiver->kind)) {
                std::string matchedRecord;
                TypePtr matchedFieldType;
                for (const auto& [recName, fields] : m_recordFields) {
                    auto fi = fields.find(name);
                    if (fi != fields.end()) {
                        if (!matchedRecord.empty()) { matchedRecord.clear(); break; }
                        matchedRecord = recName;
                        matchedFieldType = fi->second;
                    }
                }
                if (!matchedRecord.empty()) {
                    if (auto* tv = std::get_if<TypeVar>(&receiver->kind)) {
                        unifyVar(tv->id, Type::named(matchedRecord));
                    }
                    return matchedFieldType;
                }
            }
        }
        return Type::unknown();  // unknown name, or not yet registered (forward/recursive ref)
    }

    // `let hello = makeGreeter("Hello")` is a top-level zero-arg binding
    // (every top-level `let NAME = EXPR` is a 0-param function — see
    // Parser::parseFunctionDef), and referencing it as a bare identifier
    // auto-calls it (Evaluator::autoCallZeroArgConstant). `hello("Alice")`
    // is the same idiom through a call: auto-call `hello` to get the
    // closure `makeGreeter` returned, then apply "Alice" to *that* — not
    // "call hello with 1 argument," which is what zero arity would
    // otherwise mean. Only a single, unambiguous 0-param signature
    // triggers this (an overload set with a 0-param AND non-0-param
    // signature is a real arity question, not this idiom).
    if (sigs->size() == 1 && (*sigs)[0].params.empty() && !argTypes.empty()) {
        return Type::unknown();
    }

    // `This` (the trait placeholder — see Surface syntax for declaring a
    // trait in the plan) has no substitution mechanism implemented yet, so
    // it can leak through as a literal NamedType("This") from a `param :
    // This` annotation inside a `make` block. A call involving it can't be
    // meaningfully checked against any signature here — reporting a
    // mismatch against the wrong (imported or unrelated) candidate would be
    // actively misleading, so bail out rather than guess.
    for (const auto& argType : argTypes) {
        if (auto* named = std::get_if<NamedType>(&argType->kind); named && named->name == "This") {
            return Type::unknown();
        }
    }

    // Trait-bounded receiver: `item.describe()` where `item: Describable`.
    // The trait method takes priority over imported overloads of the same name.
    if (!argTypes.empty()) {
        auto receiver = resolve(argTypes[0]);
        if (auto* ct = std::get_if<ConstrainedType>(&receiver->kind)) {
            if (const TraitDef* trait = m_traits.get(ct->traitName)) {
                for (const auto& req : trait->requiredMethods) {
                    if (req.name == name) return req.result;
                }
            }
        }
    }

    // Namespace call heuristic: `BuiltIn.foo(x)` or any call where the
    // receiver resolves to Unknown (an undefined namespace sentinel like
    // `BuiltIn`). The UFCS desugaring adds the receiver as argTypes[0], but
    // if the receiver is Unknown and no overload matches the full arity while
    // overloads DO match with the receiver dropped, treat it as a plain
    // function call through a namespace — drop argTypes[0] and re-check.
    if (!argTypes.empty() &&
        (std::holds_alternative<UnknownType>(argTypes[0]->kind) ||
         std::holds_alternative<TypeVar>(argTypes[0]->kind))) {
        bool anyFullMatch = false;
        bool anyDroppedMatch = false;
        for (const auto& sig : *sigs) {
            if (sig.params.size() == argTypes.size()) {
                bool matches = true;
                for (size_t i = 0; i < sig.params.size(); i++)
                    if (!argMatchesParam(argTypes[i], sig.params[i])) {
                        matches = false;
                        break;
                    }
                anyFullMatch = anyFullMatch || matches;
            }
            if (sig.params.size() == argTypes.size() - 1) {
                bool matches = true;
                for (size_t i = 0; i < sig.params.size(); i++)
                    if (!argMatchesParam(argTypes[i + 1], sig.params[i])) {
                        matches = false;
                        break;
                    }
                anyDroppedMatch = anyDroppedMatch || matches;
            }
        }
        if (!anyFullMatch && anyDroppedMatch) {
            return checkCall(name, {argTypes.begin() + 1, argTypes.end()}, loc);
        }
    }

    // Name collision guard: a make-block method isn't registered here at
    // all (see checkFunctionDef), so `this.modulo(...)` inside a
    // user-defined `make CustomType do let modulo(...) ... end` would
    // otherwise get checked against an imported `modulo` signature
    // purely because the names match — a different, unrelated function.
    // Applies to any receiver type (NamedType or primitive) — if no
    // overload's first param can accept the receiver, this is a
    // make-block method call, not a known function call. For primitive
    // receivers outside of make blocks (m_currentMakeType is empty),
    // imported interfaces are authoritative and a mismatch is a real
    // type error.
    if (isMethodCall && !argTypes.empty() &&
        (std::holds_alternative<NamedType>(argTypes[0]->kind) ||
                              (!hasUser && m_currentMakeType &&
                               (std::holds_alternative<PrimitiveType>(argTypes[0]->kind) ||
                                std::holds_alternative<SizedIntType>(argTypes[0]->kind))))) {
        bool anyFirstParamPlausible = false;
        bool anyConstrainedFirstParam = false;
        // A candidate carrying an Unknown parameter is one whose real contract
        // the checker never learned — the shape-only entries the source-derived
        // interface path records for un-annotated `make` methods, where the
        // compiled artifacts would have given real types. A method with any
        // such candidate has a candidate list that cannot be treated as the
        // whole truth, so no mismatch is provable from it. TYPE VARIABLES are
        // a different thing entirely and stay provable: `[A]` is a known,
        // genuinely generic contract.
        bool anyUnknownParamCandidate = false;
        for (const auto& sig : *sigs) {
            if (!sig.params.empty()) {
                if (std::holds_alternative<ConstrainedType>(sig.params[0]->kind))
                    anyConstrainedFirstParam = true;
                for (const auto& param : sig.params)
                    if (mentionsUnknownType(param)) {
                        anyUnknownParamCandidate = true;
                        break;
                    }
                if (argMatchesParam(argTypes[0], sig.params[0])) {
                    anyFirstParamPlausible = true;
                    break;
                }
            }
        }
        if (isMethodCall && anyConstrainedFirstParam && !anyFirstParamPlausible) {
            auto receiverKey = m_traits.implementorKey(argTypes[0]);
            if (!m_traits.hasConformances(receiverKey))
                anyConstrainedFirstParam = false;
        }
        // A receiver no candidate accepts is usually the guard's case above —
        // but not when the receiver type is fully known and we are in
        // ordinary code. `Date.of(...).iso` (a `Date` method on a
        // `Result<Date, TimeError>`) landed here and was waved through, which
        // is why it only failed at runtime, as `if_clause` on BEAM. Fall
        // through to the mismatch report instead.
        //
        // Inside a make block the tables are incomplete on purpose (the type
        // under check has its own methods unregistered — see checkFunctionDef),
        // and a gradual receiver genuinely isn't known, so both stay permissive.
        // `date.day` is a field read, not a call to Integer's `day`. Local
        // record layouts are known by name; an IMPORTED record's are not —
        // `recordArities` counts fields without naming them — so any field
        // access on one has to stay permissive until interfaces carry field
        // names (they exist in KexI metadata; display registration uses them).
        const bool receiverIsField = [&] {
            auto* named = std::get_if<NamedType>(&argTypes[0]->kind);
            if (!named) return false;
            if (auto record = m_recordFields.find(named->name);
                record != m_recordFields.end())
                return record->second.count(name) > 0;
            return m_importedInterfaces &&
                   m_importedInterfaces->recordArities.count(named->name) > 0;
        }();
        // The receiver's own shape is what decides this, not its type
        // arguments: `Ok(b)` is `Result<Box, ?>`, and the unknown error side
        // says nothing about whether some `label` accepts a Result. A receiver
        // that is ITSELF unknown or a type variable stays permissive, and the
        // enclosing condition already restricts this to those shapes.
        // A method a trait gives the receiver's type — required or defaulted —
        // is legitimate even though no signature names that type: an
        // inherited default is registered under whichever type overrode it,
        // if any (`shout : Player -> String` while `Bot` inherits it).
        const bool traitProvides = m_traits.declaresMethod(
            m_traits.implementorKey(argTypes[0]), name);
        const bool provableMismatch =
            !m_inMakeBlock && isFullyConcrete(headShapeOf(argTypes[0])) &&
            !receiverIsField && !traitProvides && !anyUnknownParamCandidate;
        // An unreliable candidate list blocks every route to a mismatch
        // report, the trait-bound one included: `"5".to(Integer)` finds only
        // `to : Measure -> ? -> ?` from units.kex when the interfaces come
        // from source, and String is not a Measure — but String's own `to` is
        // a native builtin that never appears in these tables at all. The
        // check is confined to the case where NOTHING matched, so a call that
        // does match still gets its real result type rather than `?`.
        if (!anyFirstParamPlausible &&
            (anyUnknownParamCandidate ||
             (!anyConstrainedFirstParam && !hasReceiverRefinementConflict &&
              !provableMismatch)))
            return Type::unknown();
    }

    std::vector<const Signature*> arityMatches;
    std::vector<const Signature*> fullMatches;
    for (const auto& sig : *sigs) {
        // A trailing parameter with a default may be omitted, so the accepted
        // range is [requiredParams, params.size()] rather than one number.
        const std::size_t required =
            sig.requiredParams.value_or(sig.params.size());
        if (argTypes.size() < required || argTypes.size() > sig.params.size())
            continue;
        arityMatches.push_back(&sig);

        // Only the arguments actually PASSED are checked: an omitted
        // defaulted parameter has no argument to compare against.
        bool allMatch = true;
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (!argMatchesParam(argTypes[i], sig.params[i])) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) fullMatches.push_back(&sig);
    }

    // A trait receiver is a fallback for concrete implementations of the
    // same method.  Once a concrete receiver signature matches, do not let a
    // universal trait implementation win merely because another parameter in
    // its signature is more precise.  In particular, Measure.to(String) must
    // keep precedence over Showable.to(String).
    if (isMethodCall && fullMatches.size() > 1 && !argTypes.empty()) {
        const bool hasConcreteReceiverMatch = std::any_of(
            fullMatches.begin(), fullMatches.end(),
            [](const Signature* signature) {
                return !signature->params.empty() &&
                    !std::holds_alternative<ConstrainedType>(
                        signature->params.front()->kind) &&
                    !std::holds_alternative<TypeVar>(
                        signature->params.front()->kind) &&
                    !std::holds_alternative<UnknownType>(
                        signature->params.front()->kind);
            });
        if (hasConcreteReceiverMatch)
            std::erase_if(fullMatches, [](const Signature* signature) {
                return !signature->params.empty() &&
                    std::holds_alternative<ConstrainedType>(
                        signature->params.front()->kind);
            });
    }

    // 5c: Pick the most-specific full match when there are several.
    // Specificity per param: concrete named/list/func type (2) > trait-constrained (1) > TypeVar/Unknown (0).
    // A signature A dominates B if A >= B at every position and > at least one.
    // Concrete ties between explicitly overlapping union/constrained signatures
    // are ambiguous. Unknown/generic clause sets retain deterministic fallback,
    // and identical signatures remain ordinary multi-clause functions.
    if (fullMatches.size() > 1) {
        auto paramSpec = [](const TypePtr& p) -> int {
            if (std::holds_alternative<TypeVar>(p->kind) ||
                std::holds_alternative<UnknownType>(p->kind)) return 0;
            if (std::holds_alternative<ConstrainedType>(p->kind)) return 1;
            return 2;
        };
        auto dominates = [&](const Signature* a, const Signature* b) {
            bool aWins = false;
            for (size_t i = 0; i < a->params.size(); i++) {
                int sa = paramSpec(a->params[i]);
                int sb = paramSpec(b->params[i]);
                if (sb > sa) return false;
                if (sa > sb) aWins = true;
            }
            return aWins;
        };
        std::vector<const Signature*> undominated;
        for (const auto* cand : fullMatches) {
            bool dominated = false;
            for (const auto* other : fullMatches) {
                if (other == cand) continue;
                if (dominates(other, cand)) { dominated = true; break; }
            }
            if (!dominated) undominated.push_back(cand);
        }
        std::vector<const Signature*> distinct;
        for (const auto* candidate : undominated) {
            bool duplicate = std::any_of(
                distinct.begin(), distinct.end(),
                [&](const Signature* other) {
                    if (other->params.size() != candidate->params.size())
                        return false;
                    for (size_t i = 0; i < candidate->params.size(); ++i)
                        if (!typesEqual(
                                other->params[i], candidate->params[i]))
                            return false;
                    return true;
                });
            if (!duplicate) distinct.push_back(candidate);
        }
        const Signature* best =
            undominated.empty() ? fullMatches.front() : undominated.front();
        const bool concreteArguments = std::all_of(
            argTypes.begin(), argTypes.end(),
            [&](const TypePtr& argument) {
                auto resolved = resolve(argument);
                return !std::holds_alternative<TypeVar>(resolved->kind) &&
                    !std::holds_alternative<UnknownType>(resolved->kind);
            });
        bool typedCandidates = false;
        for (size_t left = 0;
             left < distinct.size() && !typedCandidates; ++left)
            for (size_t right = left + 1;
                 right < distinct.size() && !typedCandidates; ++right)
                for (size_t i = 0;
                     i < distinct[left]->params.size(); ++i) {
                    const auto& a = distinct[left]->params[i];
                    const auto& b = distinct[right]->params[i];
                    if (typesEqual(a, b)) continue;
                    const bool explicitOverlap =
                        (std::holds_alternative<UnionType>(a->kind) &&
                         std::holds_alternative<UnionType>(b->kind)) ||
                        (std::holds_alternative<ConstrainedType>(a->kind) &&
                         std::holds_alternative<ConstrainedType>(b->kind));
                    if (explicitOverlap) {
                        typedCandidates = true;
                        break;
                    }
                }
        if (distinct.size() > 1 && concreteArguments && typedCandidates) {
            std::string message =
                "Ambiguous overload for `" + name + "`; candidates:";
            for (const auto* candidate : distinct)
                message += "\n\n" + displaySignature(name, *candidate);
            error(loc, message);
        }
        // An unconstrained shorthand receiver (for example `&.kilo`) can
        // match several concrete receiver overloads. Picking the first one
        // would permanently specialize the closure to declaration order;
        // keep its intermediate result permissive until the surrounding
        // method chain or a call site supplies a concrete receiver.
        if (distinct.size() > 1 && !concreteArguments)
            return Type::unknown();
        if (best) {
            // Rebuild fullMatches with best first so the code below uses it
            std::vector<const Signature*> reordered = {best};
            for (const auto* s : fullMatches) if (s != best) reordered.push_back(s);
            fullMatches = std::move(reordered);
        }
    }

    // When every full match is vacuous (params all Unknown/TypeVar, or only
    // trait bounds — e.g. a trait default method with no type annotation) but
    // concrete-typed arity matches exist that DIDN'T match while agreeing on
    // the receiver, prefer the concrete sigs for error reporting so real type
    // mismatches aren't masked by the generic overload.
    if (!fullMatches.empty()) {
        auto isVacuous = [](const Signature* sig) {
            for (const auto& p : sig->params)
                if (!std::holds_alternative<TypeVar>(p->kind) &&
                    !std::holds_alternative<UnknownType>(p->kind) &&
                    !std::holds_alternative<ConstrainedType>(p->kind))
                    return false;
            return true;
        };
        bool allVacuous = true;
        for (const auto* fm : fullMatches)
            if (!isVacuous(fm)) { allVacuous = false; break; }
        if (allVacuous) {
            bool hasConcrete = false;
            for (const auto* am : arityMatches) {
                if (isVacuous(am) ||
                    std::find(fullMatches.begin(), fullMatches.end(), am) != fullMatches.end())
                    continue;
                // For receiver calls, only a concrete sig that agrees on the
                // receiver speaks for this call. Concrete sigs for other
                // receiver types must not mask a legitimately matching
                // generic default (e.g. a user type's trait default method).
                if (isMethodCall && !am->params.empty() &&
                    !argMatchesParam(argTypes[0], am->params[0]))
                    continue;
                hasConcrete = true;
                break;
            }
            if (hasConcrete) fullMatches.clear();
        }
    }

    if (fullMatches.size() >= 1) {
        const auto& matched = *fullMatches[0];
        if (std::getenv("KEX_DEBUG_SIG") && name == "to") {
            std::fprintf(stderr, "[to] params=");
            for (const auto& p : matched.params)
                std::fprintf(stderr, "%s ", typeToString(resolve(p)).c_str());
            std::fprintf(stderr, "=> %s | matches=%zu args=",
                         typeToString(resolve(matched.result)).c_str(),
                         fullMatches.size());
            for (const auto& a : argTypes)
                std::fprintf(stderr, "%s ", typeToString(resolve(a)).c_str());
            std::fprintf(stderr, "\n");
        }
        // Set when this call's receiver type is unknown AND more than one
        // provider could answer it, so nothing here may be treated as
        // evidence about the receiver — not the backend target, and not the
        // receiver's type.
        bool ambiguousReceiver = false;
        if (methodCall) {
            bool isReceiver = name.find("::") == std::string::npos;
            const ImportedFunction* resolved = nullptr;
            // `merged` is importedSigs ++ local methods ++ user functions, so
            // the winner's index says where it came from. When it came from a
            // LOCAL make-block method, the call must not be bound to an
            // imported target — doing so is how `b.get(0)` on a user record
            // with its own `get` was lowered to `kex_prelude:get/2` and died
            // at runtime, purely because the prelude also has a `get`.
            bool winnerIsLocal = false;
            bool winnerIsMakeMethod = false;
            if (sigs == &merged) {
                auto selected = static_cast<size_t>(fullMatches[0] - merged.data());
                if (selected < localSigCount) {
                    winnerIsLocal = true;
                    winnerIsMakeMethod = true;
                } else if (selected >= localSigCount &&
                    selected - localSigCount < importedFunctions.size())
                    resolved = importedFunctions[selected - localSigCount];
                else
                    winnerIsLocal = true;
            }
            if (!resolved && !winnerIsLocal) {
                int matchedArity = static_cast<int>(matched.params.size());
                for (const auto* candidate : importedFunctions) {
                    if (candidate->backendArity == matchedArity) {
                        resolved = candidate; break;
                    }
                }
            }
            // Pinning a receiver call to one provider is only sound when the
            // receiver's type is actually known. It often is not — a binding
            // from a constructor pattern (`Just(d) ->`) carries no payload
            // type yet — and then every same-arity signature "matches", so the
            // winner is whichever came first. `d.get(:year, "?")` on a Map
            // bound to `Kex.Regex:get/3` (Match's accessor) and died with
            // function_clause on BEAM, while the walker dispatched on the
            // runtime value and read the map (examples/regexes.kex).
            //
            // Leaving the call unpinned costs nothing: it lowers to the same
            // runtime-dispatching path the walker takes.
            const bool receiverUnknown = [&] {
                if (!isReceiver || argTypes.empty()) return false;
                const auto receiver = resolve(argTypes[0]);
                return std::holds_alternative<UnknownType>(receiver->kind) ||
                       std::holds_alternative<TypeVar>(receiver->kind);
            }();
            if (resolved && receiverUnknown)
                for (const auto* candidate : importedFunctions) {
                    if (candidate == resolved ||
                        candidate->backendArity != resolved->backendArity ||
                        candidate->backendModule == resolved->backendModule)
                        continue;
                    resolved = nullptr;
                    ambiguousReceiver = true;
                    break;
                }
            // Same for a LOCAL make-block method: a module flattened into this
            // unit owns the name for its own receiver type, and an unknown
            // receiver is no evidence that this call means that type. Pinning
            // it sent every `get` in a program that says `using Regex` to
            // Match's accessor.
            if (!resolved && winnerIsMakeMethod && receiverUnknown)
                for (const auto* candidate : importedFunctions)
                    if (candidate->backendArity ==
                        static_cast<int>(matched.params.size())) {
                        winnerIsMakeMethod = false;
                        ambiguousReceiver = true;
                        break;
                    }
            if (resolved) {
                ResolvedCallTarget target{
                    resolved->backendModule,
                    resolved->backendFunction,
                    resolved->backendArity,
                    isReceiver,
                    resolved->signature.isFoul,
                    resolved->paramNames,
                };
                for (std::size_t i = 0; i < matched.params.size(); ++i) {
                    const auto resolvedParam = resolve(matched.params[i]);
                    const auto addRequiredDictionary = [&](const std::string& traitName) {
                        const bool backendExpectsDictionary =
                            resolved->backendArity >
                            static_cast<int>(matched.params.size() +
                                             target.traitDictionaries.size());
                        if (!backendExpectsDictionary) return;
                        const auto* trait = m_traits.get(traitName);
                        const bool methodIsRequired = trait && std::any_of(
                            trait->requiredMethods.begin(),
                            trait->requiredMethods.end(),
                            [&](const auto& required) {
                                return required.name == name;
                            });
                        const bool callsTraitImplementationDirectly =
                            i == 0 && isReceiver &&
                            methodIsRequired &&
                            resolved->backendFunction == name + "/" + traitName;
                        if (trait && !trait->requiredMethods.empty() &&
                            !callsTraitImplementationDirectly)
                            target.traitDictionaries.push_back({i, traitName});
                    };
                    if (const auto* constrained =
                            std::get_if<ConstrainedType>(&resolvedParam->kind))
                        addRequiredDictionary(constrained->traitName);
                    else if (const auto* named =
                                 std::get_if<NamedType>(&resolvedParam->kind))
                        addRequiredDictionary(named->name);
                }
                m_resolvedCalls[methodCall] = std::move(target);
            } else if (winnerIsMakeMethod) {
                ResolvedCallTarget target;
                target.backendFunction = name;
                target.backendArity = static_cast<int>(matched.params.size());
                target.passesReceiver = true;
                target.isFoul = matched.isFoul;
                for (const auto& param : matched.params)
                    target.localDispatchTypes.push_back(
                        typeToString(resolve(param)));
                m_resolvedCalls[methodCall] = std::move(target);
            }
        }
        // Propagate the param types back to any TypeVar arguments so that
        // unannotated params are constrained by the functions they're passed
        // into (e.g. `let f(s) = s.split(",")` constrains `s` to String).
        // Only apply when the param is concrete — skip generic params that
        // themselves contain TypeVars (e.g. `first : [A] -> A?`).
        auto typeContainsVar = [](const TypePtr& t) {
            // Shallow check: top-level TypeVar, or List/Optional/Constrained
            // wrapping a TypeVar. Deep recursion isn't worth the complexity here.
            if (std::holds_alternative<TypeVar>(t->kind)) return true;
            if (auto* lt = std::get_if<ListType>(&t->kind))
                return std::holds_alternative<TypeVar>(lt->element->kind);
            if (auto* ot = std::get_if<OptionalType>(&t->kind))
                return std::holds_alternative<TypeVar>(ot->inner->kind);
            return false;
        };
        for (size_t i = 0; i < argTypes.size() && i < matched.params.size(); i++) {
            // The receiver of an ambiguous call is exactly the argument this
            // match says nothing about: binding it to the winner's param type
            // would then TYPE it as that overload's receiver (a Map inferred
            // as Regex's `Match`), which is worse than leaving it unknown.
            if (i == 0 && ambiguousReceiver) continue;
            auto resolved = resolve(argTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                const auto& param = matched.params[i];
                // Negative ids are per-signature generic placeholders, not
                // inference variables, and every signature numbers its own
                // from -1. Binding one in the global substitution made an
                // unrelated signature's `A` mean whatever the last such call
                // passed: after `args.at(i).or("")` (String), `(0..3).filter`
                // returned [String] and its block param typed as String.
                if (tv->id >= 0 && !typeContainsVar(param))
                    unifyVar(tv->id, param);
            }
        }
        // Instantiate interface-level generic placeholders in the result from
        // the actual arguments. For example,
        // `Result<X, E>.or : X -> X` called on `Result<Regex, RegexError>`
        // must return Regex; leaking the table placeholder made a following
        // overloaded call silently select a same-arity target for any type.
        std::unordered_map<int, TypePtr> genericSubst;
        std::function<void(const TypePtr&, const TypePtr&)> bindGenerics =
            [&](const TypePtr& pattern, const TypePtr& actual) {
                if (!pattern || !actual) return;
                if (auto* tv = std::get_if<TypeVar>(&pattern->kind);
                    tv && tv->id < 0) {
                    genericSubst.try_emplace(tv->id, resolve(actual));
                    return;
                }
                if (auto* constrained =
                        std::get_if<ConstrainedType>(&pattern->kind);
                    constrained && constrained->genericId < 0) {
                    genericSubst.try_emplace(constrained->genericId,
                                             resolve(actual));
                    return;
                }
                if (auto* pn = std::get_if<NamedType>(&pattern->kind)) {
                    auto* an = std::get_if<NamedType>(&actual->kind);
                    if (!an || pn->name != an->name ||
                        pn->typeArgs.size() != an->typeArgs.size()) return;
                    for (size_t i = 0; i < pn->typeArgs.size(); i++)
                        bindGenerics(pn->typeArgs[i], an->typeArgs[i]);
                } else if (auto* pl = std::get_if<ListType>(&pattern->kind)) {
                    if (auto* al = std::get_if<ListType>(&actual->kind))
                        bindGenerics(pl->element, al->element);
                } else if (auto* pm = std::get_if<MapType>(&pattern->kind)) {
                    if (auto* am = std::get_if<MapType>(&actual->kind)) {
                        bindGenerics(pm->key, am->key);
                        bindGenerics(pm->value, am->value);
                    }
                } else if (auto* po = std::get_if<OptionalType>(&pattern->kind)) {
                    if (auto* ao = std::get_if<OptionalType>(&actual->kind))
                        bindGenerics(po->inner, ao->inner);
                } else if (auto* pt = std::get_if<TupleType>(&pattern->kind)) {
                    auto* at = std::get_if<TupleType>(&actual->kind);
                    if (!at || pt->elements.size() != at->elements.size()) return;
                    for (size_t i = 0; i < pt->elements.size(); i++)
                        bindGenerics(pt->elements[i], at->elements[i]);
                } else if (auto* pf = std::get_if<FuncType>(&pattern->kind)) {
                    auto* af = std::get_if<FuncType>(&actual->kind);
                    if (!af || pf->params.size() != af->params.size()) return;
                    for (size_t i = 0; i < pf->params.size(); i++)
                        bindGenerics(pf->params[i], af->params[i]);
                    bindGenerics(pf->result, af->result);
                }
            };
        for (size_t i = 0; i < argTypes.size() && i < matched.params.size(); i++)
            bindGenerics(matched.params[i], resolve(argTypes[i]));

        // Substitute INSIDE the result too, not only when the whole result is
        // a placeholder: `[X].dropWhile : ((X) -> Bool) -> [X]` on a [String]
        // receiver returns [String]. Leaking `[X]` instead was not merely
        // vague — String IS [Char] here, so the leaked `[A]` went on to match
        // String's `at : String -> Integer -> Char?` and a list of strings
        // indexed as a list of characters. Placeholders the arguments do not
        // bind (`map : (X -> B) -> [B]`) are left exactly as they were.
        std::function<TypePtr(const TypePtr&)> applyGenerics =
            [&](const TypePtr& type) -> TypePtr {
                if (!type) return type;
                if (auto* var = std::get_if<TypeVar>(&type->kind)) {
                    if (var->id >= 0) return type;
                    auto found = genericSubst.find(var->id);
                    return found == genericSubst.end() ? type : found->second;
                }
                if (auto* list = std::get_if<ListType>(&type->kind))
                    return Type::list(applyGenerics(list->element));
                if (auto* optional = std::get_if<OptionalType>(&type->kind))
                    return Type::optional(applyGenerics(optional->inner));
                if (auto* map = std::get_if<MapType>(&type->kind))
                    return Type::map(applyGenerics(map->key),
                                     applyGenerics(map->value));
                if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
                    std::vector<TypePtr> elements;
                    for (const auto& element : tuple->elements)
                        elements.push_back(applyGenerics(element));
                    return Type::tuple(std::move(elements));
                }
                if (auto* named = std::get_if<NamedType>(&type->kind)) {
                    if (named->typeArgs.empty()) return type;
                    std::vector<TypePtr> args;
                    for (const auto& arg : named->typeArgs)
                        args.push_back(applyGenerics(arg));
                    return Type::named(named->name, std::move(args));
                }
                return type;
            };
        return applyGenerics(matched.result);
    }

    if (arityMatches.empty()) {
        error(loc, "`" + name + "` expects " + std::to_string((*sigs)[0].params.size()) +
              " argument(s), got " + std::to_string(argTypes.size()));
        return (*sigs)[0].result;
    }

    // Zero matches with at least one arity match: find the first
    // mismatching argument against the first arity-matching candidate for
    // the headline message, then list every candidate signature tried,
    // Elm-style. Prefer a concrete sig that agrees on the receiver over a
    // vacuous or foreign-receiver one for error reporting.
    auto isVacuousSig = [](const Signature* sig) {
        for (const auto& p : sig->params)
            if (!std::holds_alternative<TypeVar>(p->kind) &&
                !std::holds_alternative<UnknownType>(p->kind) &&
                !std::holds_alternative<ConstrainedType>(p->kind))
                return false;
        return true;
    };
    // When NO candidate agrees on the receiver — `Result<Date, _>.iso`, where
    // every `iso` belongs to some other type — the loop below never fires and
    // the headline names whatever was registered first. That is registration
    // order, so adding an unrelated overload anywhere in the prelude silently
    // rewrote the message. Fall back to the same ordering the listing uses.
    auto signatureOrder = [](const Signature* a, const Signature* b) {
        if (a->params.size() != b->params.size())
            return a->params.size() < b->params.size();
        for (size_t i = 0; i < a->params.size(); ++i) {
            auto left = typeToString(a->params[i]);
            auto right = typeToString(b->params[i]);
            if (left != right) return left < right;
        }
        return typeToString(a->result) < typeToString(b->result);
    };
    const Signature* firstPtr =
        *std::min_element(arityMatches.begin(), arityMatches.end(), signatureOrder);
    for (const auto* am : arityMatches) {
        if (isVacuousSig(am)) continue;
        if (isMethodCall && !am->params.empty() &&
            !argMatchesParam(argTypes[0], am->params[0])) continue;
        firstPtr = am;
        break;
    }
    // Same rule as the receiver-mismatch guard above, for the path a
    // PRIMITIVE receiver takes: when every candidate carries an Unknown
    // parameter, this candidate list is not the whole truth and no mismatch
    // follows from it. `"42".to(Integer)` reaches here seeing only Measure's
    // `to` from units.kex, because String's `to` is a native builtin that
    // appears in no signature table. The runtime still catches a real mistake.
    const bool unreliableCandidates =
        std::all_of(arityMatches.begin(), arityMatches.end(),
                    [](const Signature* sig) {
                        return std::any_of(sig->params.begin(), sig->params.end(),
                                           mentionsUnknownType);
                    });
    if (unreliableCandidates) return arityMatches[0]->result;

    const Signature& first = *firstPtr;
    std::string detail = "different arguments";
    for (size_t i = 0; i < first.params.size(); i++) {
        if (!argMatchesParam(argTypes[i], first.params[i])) {
            auto* constrained = std::get_if<ConstrainedType>(&first.params[i]->kind);
            std::string expected = constrained ? constrained->traitName : typeToString(first.params[i]);
            detail = "argument " + std::to_string(i + 1) + " to be " + expected +
                      ", but got " + typeToString(argTypes[i]);
            break;
        }
    }

    std::string message = "`" + name + "` expects " + detail;
    // Vacuous candidates (params all TypeVar/Unknown/trait bounds — e.g. an
    // unannotated trait default method) add noise to the listing when
    // concrete candidates exist; show them only when they are all we have.
    auto sortedSigs = *sigs;
    std::stable_sort(sortedSigs.begin(), sortedSigs.end(),
        [](const Signature& a, const Signature& b) {
            if (a.params.size() != b.params.size()) return a.params.size() < b.params.size();
            for (size_t i = 0; i < a.params.size(); ++i) {
                auto as = typeToString(a.params[i]), bs = typeToString(b.params[i]);
                if (as != bs) return as < bs;
            }
            return typeToString(a.result) < typeToString(b.result);
        });
    bool anyConcreteSig = false;
    for (const auto& sig : sortedSigs)
        if (!isVacuousSig(&sig)) { anyConcreteSig = true; break; }
    for (const auto& sig : sortedSigs) {
        if (anyConcreteSig && isVacuousSig(&sig)) continue;
        message += "\n\n" + displaySignature(name, sig);
    }
    error(loc, message);
    return arityMatches[0]->result;
}

auto TypeChecker::pushScope() -> void {
    m_scopeStack.emplace_back();
    m_importScopeStack.emplace_back();
}

auto TypeChecker::popScope() -> void {
    if (!m_scopeStack.empty()) {
        m_scopeStack.pop_back();
        m_importScopeStack.pop_back();
    }
}

auto TypeChecker::defineVar(const std::string& name, TypePtr type) -> void {
    if (!m_scopeStack.empty()) {
        m_scopeStack.back().set(name, std::move(type));
    } else {
        m_globals.set(name, std::move(type));
    }
}

auto TypeChecker::lookupVar(const std::string& name) const -> TypePtr {
    for (auto it = m_scopeStack.rbegin(); it != m_scopeStack.rend(); ++it) {
        if (auto type = it->get(name)) return type;
    }
    return m_globals.get(name);
}

auto TypeChecker::error(SourceLocation loc, const std::string& msg) -> void {
    if (m_diagnostics) {
        m_diagnostics->push_back({Diagnostic::Level::Error, loc, msg});
    }
}

// Whether a type has any component the checker has not pinned down — a type
// variable, an unknown, or a trait constraint. Checks that would otherwise
// report a mismatch use this to stay quiet rather than invent an error from
// incomplete information.
auto containsOpenType(const TypePtr& type) -> bool {
    if (!type) return true;
    return std::visit(
        [](const auto& node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeVar> ||
                          std::is_same_v<T, UnknownType> ||
                          std::is_same_v<T, ConstrainedType>) {
                return true;
            } else if constexpr (std::is_same_v<T, NamedType>) {
                for (const auto& arg : node.typeArgs)
                    if (containsOpenType(arg)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, FuncType>) {
                for (const auto& param : node.params)
                    if (containsOpenType(param)) return true;
                return containsOpenType(node.result);
            } else if constexpr (std::is_same_v<T, TupleType>) {
                for (const auto& element : node.elements)
                    if (containsOpenType(element)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, ListType>) {
                return containsOpenType(node.element);
            } else if constexpr (std::is_same_v<T, MapType>) {
                return containsOpenType(node.key) || containsOpenType(node.value);
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                return containsOpenType(node.inner);
            } else if constexpr (std::is_same_v<T, UnionType>) {
                for (const auto& member : node.members)
                    if (containsOpenType(member)) return true;
                return false;
            } else {
                return false;
            }
        },
        type->kind);
}

auto TypeChecker::typeMismatch(SourceLocation loc, const TypePtr& expected,
                               const TypePtr& actual) -> void {
    error(loc, "Type mismatch: expected " + typeToString(expected) +
          ", got " + typeToString(actual));
}

auto TypeChecker::freshTypeVar() -> TypePtr {
    return Type::typeVar(m_nextTypeVar++);
}

auto TypeChecker::resolve(TypePtr t) const -> TypePtr {
    while (t) {
        auto* tv = std::get_if<TypeVar>(&t->kind);
        if (!tv) break;
        auto it = m_subst.find(tv->id);
        if (it == m_subst.end()) break;
        t = it->second;
    }
    return t;
}

auto TypeChecker::unifyVar(int id, TypePtr concrete) -> void {
    if (!m_subst.count(id)) m_subst[id] = std::move(concrete);
}

} // namespace kex::semantic
