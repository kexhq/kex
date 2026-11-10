#include "../ast/function_groups.hxx"
#include "../ast/walk.hxx"
#include "../common/mangle.hxx"
#include "typechecker.hxx"
#include "analyzer.hxx"
#include "declaration_validator.hxx"
#include "../common/type_def_utils.hxx"
#include "../common/well_known.hxx"
#include "typechecker_internal.hxx"
#include "resolve_core.hxx"
#include <cctype>
#include <functional>
#include <set>
#include <unordered_set>
#include <utility>

namespace kex::semantic {

using namespace kex::semantic::detail;

auto TypeChecker::check(const ast::Program& program,
                        std::vector<Diagnostic>& diagnostics) -> void {
    m_diagnostics = &diagnostics;
    m_patternBindings.clear();
    m_functionSignatures.clear();
    m_resolvedCalls.clear();
    m_selectedCallSignatures.clear();
    m_traitDefaultClauses.clear();
    m_traitDefaults.clear();
    m_inheritedPendingResults.clear();
    m_shownThroughDistinct.clear();
    m_referencedModules.clear();
    m_localModules.clear();
    m_moduleConstructors.clear();
    m_adtVariants.clear();
    m_adtOfConstructor.clear();
    m_nullaryConstructors.clear();
    m_methodSignatures.clear();
    m_slotMethodNames.clear();
    m_makeMethodNames.clear();
    m_makeMethodReceivers.clear();
    m_qualifiedPublished.clear();
    m_overloadPurity.clear();
    m_scopedDeclaredSignatures.clear();
    m_typeAliases.clear();
    m_distinctTypes.clear();
    m_recordFields.clear();
    m_guessedReceiverTypes.clear();
    m_constructorOwners.clear();
    m_requiredRecordFields.clear();
    if (m_importedInterfaces)
        m_distinctTypes = m_importedInterfaces->distinctTypes;
    if (m_importedInterfaces)
        m_recordFields = m_importedInterfaces->recordFields;
    m_currentModulePath.clear();
    m_scopeStack.clear();
    m_importScopeStack.clear();
    m_declarationImports.clear();
    m_functionImports.clear();
    m_makeImports.clear();
    m_mainImports.clear();
    m_recordImports.clear();
    m_typeImports.clear();
    pushScope();

    m_importedModulePaths.clear();
    auto selectionFor = [this](const ast::UsingBlock& block) {
        ImportSelection selection;
        selection.module += block.module.dotted();
        selection.onlyNames = block.onlyNames;
        selection.exceptNames = block.exceptNames;
        selection.alias = block.alias;
        m_importedModulePaths.insert(selection.module);
        return selection;
    };
    // A `private do ... end` / `visible do ... end` block is a plain
    // container: the declarations inside it see exactly the imports that are
    // active around it. Without this, annotations on those functions resolved
    // record names without the enclosing `using`, so the same record came out
    // qualified in one signature and bare in another.
    auto collectVisibilityImports =
        [&](const ast::VisibilityBlock& block,
            const std::vector<ImportSelection>& active) {
            for (const auto& visible : block.items) {
                if (const auto* fn =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible);
                    fn && *fn)
                    m_functionImports[fn->get()] = active;
                else if (const auto* make =
                             std::get_if<std::unique_ptr<ast::MakeDef>>(&visible);
                         make && *make)
                    m_makeImports[make->get()] = active;
                else if (const auto* record =
                             std::get_if<std::unique_ptr<ast::RecordDef>>(&visible);
                         record && *record)
                    m_recordImports[record->get()] = active;
                else if (const auto* type =
                             std::get_if<std::unique_ptr<ast::TypeDef>>(&visible);
                         type && *type)
                    m_typeImports[type->get()] = active;
            }
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
                    std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                    if (node) m_recordImports[node.get()] = active;
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                    if (node) m_typeImports[node.get()] = active;
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (node) collectVisibilityImports(*node, active);
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) collectModuleImports(*node, active);
                }
            }, item);
        }
    };
    std::vector<ImportSelection> topLevelImports;
    // The file each top-level `using` was written in. A module merged in from
    // another file is not in that `using`'s scope: the entry file's imports
    // reached every dependency's declarations and resolved THEIR bare names
    // against what the application happened to import (kexhq/kex#476).
    std::vector<std::string_view> topLevelImportFiles;
    auto importsVisibleIn = [&](const ast::ModuleDef& module) {
        std::vector<ImportSelection> visible;
        for (size_t i = 0; i < topLevelImports.size(); ++i)
            if (topLevelImportFiles[i] == module.location.file)
                visible.push_back(topLevelImports[i]);
        return visible;
    };
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                if (node && node->body.empty()) {
                    topLevelImports.push_back(selectionFor(*node));
                    topLevelImportFiles.push_back(node->location.file);
                }
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (node) m_recordImports[node.get()] = topLevelImports;
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (node) m_typeImports[node.get()] = topLevelImports;
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
                std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (node) collectVisibilityImports(*node, topLevelImports);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectModuleImports(*node, importsVisibleIn(*node));
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
                const auto declared = adt.constructorArities.find(ctor);
                m_constructorOwners[ctor].push_back(
                    {adt.name,
                     declared == adt.constructorArities.end()
                         ? 0 : static_cast<size_t>(declared->second),
                     adt.typeParamCount > 0});
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
                if (adt.typeParamCount ||
                    adt.constructorTypeParamSlots.contains(ctor)) {
                    ConstructorResult result;
                    result.adtName = adt.name;
                    result.typeParamCount = adt.typeParamCount;
                    if (auto slots = adt.constructorTypeParamSlots.find(ctor);
                        slots != adt.constructorTypeParamSlots.end())
                        result.slots = slots->second;
                    m_constructorResult[ctor] = std::move(result);
                }
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
        std::visit([&](const auto& node) {
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
    registerRecordFields(program, /*namesOnly=*/true);

    // Register built-in types
    // (done before registerDeclaredSignatures so annotation TypeExprs can resolve these names)
    // Int is a user-facing alias for Integer (arbitrary precision).
    // Fixed-width integers are available as Int32, Int64, etc.
    // `Atom` among them: without it the annotation resolved to an
    // unregistered NamedType("Atom") while an atom literal infers
    // PrimitiveType{Atom} — same printed name, different kind, so they never
    // matched ("expects Atom | Integer, but got Atom").
    // No "Float" entry — it's not a concrete Type, only a trait name
    // (TraitRegistry, phase 3), satisfied by Float32 and Float64.
    for (const auto name : builtinTypeNames())
        m_globals.set(std::string(name), builtinType(name));

    m_globals.set("ENV", Type::map(Type::string(), Type::string()));

    // Imported aliases first; a locally declared name of the same spelling
    // then overwrites it, which is the precedence the rest of the checker uses.
    if (m_importedInterfaces)
        m_typeAliases = m_importedInterfaces->typeAliases;

    // A slot of a `serving` block compiled on its own is called on its
    // server as one declared here is: `within:` is the call's, not an
    // argument. Only a unit of this program's (`visibleByReceiver`): the
    // prelude's own `make Server<X>` functions (`link`) are no slots.
    if (m_importedInterfaces)
        for (const auto& [name, functions] : m_importedInterfaces->receiverFunctions)
            for (const auto& function : functions)
                if (function.visibleByReceiver && !function.signature.params.empty())
                    if (const auto* server = std::get_if<NamedType>(
                            &function.signature.params.front()->kind);
                        server && server->name == "Server" &&
                        server->typeArgs.size() == 1)
                        m_slotMethodNames.insert(name);

    // Alias bodies must see the canonical built-in types. Resolving an open
    // row alias before this point made its `String` field a nominal name while
    // record declarations later used the real String type; both printed the
    // same but failed invariant row-field comparison.
    registerTypeAliases(program);

    // The imported field types seeded above were spelled against the defining
    // module's aliases and never went through resolveTypeExpr; expand them now
    // that the alias table is complete, so an imported record compares the same
    // way a locally declared one does.
    for (auto& [_, fields] : m_recordFields)
        for (auto& [__, fieldType] : fields)
            fieldType = expandTypeAliases(fieldType);

    if (m_importedInterfaces)
        for (const auto& trait : m_importedInterfaces->traits)
            m_traits.define(trait);
    registerTraits(program);
    registerCapabilityTraits(program);
    registerLocalConformances(program);
    if (m_importedInterfaces)
        for (const auto& c : m_importedInterfaces->traitConformances)
            m_traits.registerImplementation(
                c.typeName, resolveTraitReference(c.traitName, "", nullptr));
    registerRecordFields(program);
    for (const auto& [name, query] : m_computedAliases)
        m_typeAliases[name] = resolveTypeQuery(*query);
    m_computedAliases.clear();
    validateDeclarations(program, m_importedInterfaces, m_traits, diagnostics);
    m_topLevelValueNames.clear();
    for (const auto& item : program.items)
        if (const auto* main = std::get_if<std::unique_ptr<ast::MainBlock>>(&item);
            main && *main && (*main)->synthetic)
            for (const auto& e : (*main)->body)
                if (const auto* let = e ? std::get_if<ast::LetExpr>(&e->kind) : nullptr;
                    let && let->pattern)
                    if (const auto* var =
                            std::get_if<ast::VarPattern>(&let->pattern->kind))
                        m_topLevelValueNames.insert(var->name);
    registerDeclaredSignatures(program);
    registerMakeSignatures(program);
    m_moduleReexports.clear();
    registerExports(program);
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

        // A top-level value binding (`let frame = Template.html(...)`, a
        // synthetic main holding just that `let`) is a declaration too: a
        // function reading it needs its type, so it takes part in this order
        // under a key no function name can have.
        const auto valueKey = [](const std::string& name) { return "=" + name; };
        std::unordered_map<std::string, size_t> topLevelValues;
        for (size_t i = 0; i < program.items.size(); i++)
            if (const auto* main = std::get_if<std::unique_ptr<ast::MainBlock>>(&program.items[i]);
                main && *main && (*main)->synthetic && (*main)->body.size() == 1 &&
                (*main)->body.front())
                if (const auto* let = std::get_if<ast::LetExpr>(&(*main)->body.front()->kind);
                    let && let->pattern)
                    if (const auto* var = std::get_if<ast::VarPattern>(&let->pattern->kind))
                        topLevelValues.try_emplace(var->name, i);

        // Collect direct call-dependencies for each FunctionDef.
        // Only user-defined function names are tracked; imported package
        // functions are outside this compilation unit and need no ordering.
        std::function<void(const ast::Expr&, std::set<std::string>&)> collectCalls =
            [&](const ast::Expr& e, std::set<std::string>& out) {
            if (const auto* call = std::get_if<ast::FunctionCall>(&e.kind)) {
                if (userFns.count(call->name)) out.insert(call->name);
                if (topLevelValues.count(call->name)) out.insert(valueKey(call->name));
            } else if (const auto* tagged = std::get_if<ast::TaggedLiteral>(&e.kind)) {
                if (userFns.count(tagged->tag)) out.insert(tagged->tag);
            } else if (const auto* method = std::get_if<ast::MethodCall>(&e.kind)) {
                if (userFns.count(method->method)) out.insert(method->method);
            } else if (const auto* id = std::get_if<ast::Identifier>(&e.kind)) {
                if (topLevelValues.count(id->name)) out.insert(valueKey(id->name));
            }
            ast::forEachChild(e, [&](const ast::ExprPtr& child) {
                collectCalls(*child, out);
            });
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
        for (const auto& [name, index] : topLevelValues) {
            const auto& main = std::get<std::unique_ptr<ast::MainBlock>>(program.items[index]);
            std::set<std::string> d;
            collectCalls(*main->body.front(), d);
            d.erase(valueKey(name));
            deps[valueKey(name)] = std::move(d);
            fnOrder.emplace_back(valueKey(name), index);
        }
        // Source order among the nodes, so diagnostics come in the order written.
        std::stable_sort(fnOrder.begin(), fnOrder.end(),
                         [](const auto& left, const auto& right) { return left.second < right.second; });

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
        std::unordered_set<size_t> valuesChecked;
        for (const auto& name : sorted) {
            if (const auto value = topLevelValues.find(name.substr(1));
                name.starts_with('=') && value != topLevelValues.end()) {
                checkTopLevel(program.items[value->second]);
                valuesChecked.insert(value->second);
                continue;
            }
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
        for (size_t index = 0; index < program.items.size(); index++) {
            if (valuesChecked.count(index)) continue;
            const auto& item = program.items[index];
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
    // Same reasoning for pattern bindings: a binding's type is often a fresh
    // variable at the point the pattern is walked, and only a later use says
    // what it is.
    for (auto& binding : m_patternBindings)
        binding.type = resolve(binding.type);
    decideOperators(program);

    reportUnknownMethods();
}

// A method call whose NAME is not defined anywhere — the typo case. Both
// backends already fail on it at runtime ("Undefined method: shout for
// String"); saying so at check time is what makes an editor useful, and it is
// the single most common mistake a checker can catch.
//
// Deliberately name-based, not receiver-based: reporting "no such method FOR
// THIS TYPE" would need the receiver's type to be known, and it very often is
// not (a constructor-pattern binding carries none). A name that exists
// somewhere — including on a module-scoped `make` the call cannot see — is
// left to the runtime, which reports it with the receiver's real type.
auto TypeChecker::reportUnknownMethods() -> void {
    // With no imported interfaces the checker has never been told what the
    // standard library contains, so every prelude method would read as
    // undefined. Analyzers built that way (the tagged-literal validation
    // tests) are asking a different question entirely.
    if (!m_importedInterfaces) {
        m_unresolvedMethods.clear();
        m_unresolvedMethodImports.clear();
        return;
    }
    // Is this module named by a `using` in force at the call — directly, as a
    // parent of one, or as a child? `using Data.Set` brings its children
    // within reach too, and being generous here
    // only costs a diagnostic, never a false error. But only the call's own
    // imports count, and only what their `only:`/`except:` let through. The
    // whole program's imports used to: a spec file, which pulls in
    // `Net.HTTP.WebSocket` and with it that module's `using Net.HTTP`, had
    // `Net.HTTP.Server.localAddress` in reach without importing it, as did
    // `using Net.HTTP, only: [Router]`. `running.localAddress` then checked
    // clean as Unknown, and BEAM called the TCP listener's `localAddress`
    // (kexhq/kex#570).
    const std::vector<ImportSelection>* callImports = nullptr;
    auto importedHere = [&](const std::string& moduleName,
                            const std::string& member) {
        if (!callImports) return false;
        auto selected = [](const ImportSelection& import,
                           const std::string& name) {
            return resolve_core::usingSelectsName(name, import.onlyNames,
                                                  import.exceptNames);
        };
        for (const auto& import : *callImports) {
            const auto& path = import.module;
            if (moduleName == path) {
                if (selected(import, member)) return true;
            } else if (moduleName.rfind(path + ".", 0) == 0) {
                const auto rest = moduleName.substr(path.size() + 1);
                if (selected(import, rest.substr(0, rest.find('.')))) return true;
            } else if (path.rfind(moduleName + ".", 0) == 0) {
                return true;
            }
        }
        return false;
    };
    // A module is in reach when the call's imports bring it in, or when it is
    // one of the automatically imported (prelude) ones that need no import.
    auto moduleInReach = [&](const std::string& moduleName,
                             const std::string& member) {
        if (moduleName.empty()) return true;
        if (auto module = m_importedInterfaces->modules.find(moduleName);
            module != m_importedInterfaces->modules.end() &&
            module->second.automaticImport)
            return true;
        return importedHere(moduleName, member);
    };
    auto knownSomewhere = [&](const std::string& name, bool fieldMayExist) {
        if (m_methodSignatures.count(name) || m_userSignatures.count(name) ||
            m_annotatedMethods.count(name) || m_makeMethodNames.count(name))
            return true;
        if (fieldMayExist)
            for (const auto& [record, fields] : m_recordFields) {
                (void)record;
                if (fields.count(name)) return true;
            }
        for (const auto& [traitName, trait] : m_traits.all()) {
            // An imported capability synthesizes its interface as a trait
            // (registerCapabilityTraits), so its member names would otherwise
            // vouch for ANY receiver in ANY module: `running.localAddress`
            // checked clean because an unimported `Net.Socket.TCP` declares
            // `localAddress`, defeating `except:` (kexhq/kex#570). Only a
            // capability in reach may suppress the diagnostic. Ordinary traits
            // keep the historical reach-insensitive behavior, and a capability
            // declared by the program itself (short name, never dotted) is
            // trivially in reach.
            if (traitName.find('.') != std::string::npos &&
                m_capabilities.count(traitName) > 0 &&
                !moduleInReach(traitName, name))
                continue;
            for (const auto& required : trait.requiredMethods)
                if (required.name == name) return true;
            for (const auto& defaulted : trait.defaultMethods)
                if (defaulted == name) return true;
        }
        if (m_importedInterfaces) {
            // `receiverFunctions` is a flat, name-indexed copy of every
            // module member that can be reached through a receiver, opt-in
            // modules included — `ImportedFunction::sourceModule` is kept
            // precisely so lexical `using` policy still applies to it. It was
            // not applied here, so ANY member name of ANY known module passed
            // as a method on ANY receiver: `box.sha256`, `box.shiftLeft(1)`,
            // `Headers.empty.set("a", "b")` all checked clean and then died at
            // runtime with "Undefined method" (rodolfo docs/kex-issues.md #5).
            if (auto providers = m_importedInterfaces->receiverFunctions.find(name);
                providers != m_importedInterfaces->receiverFunctions.end() &&
                std::any_of(providers->second.begin(), providers->second.end(),
                            [&](const ImportedFunction& provider) {
                                return moduleInReach(provider.sourceModule, name);
                            }))
                return true;
            for (const auto& [moduleName, module] :
                 m_importedInterfaces->modules) {
                // Only a module whose members COULD be in scope may suppress
                // the diagnostic. Every known module used to, imported or not,
                // so any member name of any opt-in stdlib module passed as a
                // method on any receiver: `box.sha256`, `box.shiftLeft(1)`,
                // `headers.set("a", "b")` all checked clean and then died at
                // runtime with "Undefined method" (rodolfo docs/kex-issues.md
                // #5). A module the call's own imports bring in still
                // suppresses — and so does a prelude one, which needs no import.
                if (!moduleInReach(moduleName, name)) continue;
                if (module.exports.count(name)) return true;
            }
            if (fieldMayExist)
                for (const auto& [record, fields] :
                     m_importedInterfaces->recordFieldNames) {
                    (void)record;
                    if (fields.count(name)) return true;
                }
        }
        return false;
    };
    for (size_t i = 0; i < m_unresolvedMethods.size(); ++i) {
        const auto& unresolved = m_unresolvedMethods[i];
        callImports = i < m_unresolvedMethodImports.size()
            ? &m_unresolvedMethodImports[i] : nullptr;
        // Checked here, not at the call: the guess may come later in the file.
        if (knownSomewhere(unresolved.name,
                           unresolved.fieldMayExist ||
                               m_guessedReceiverTypes.count(unresolved.receiver)))
            continue;
        error(unresolved.location,
              "Undefined method `" + unresolved.name + "`" +
              (unresolved.receiver.empty() || unresolved.receiver == "Unknown"
                   ? std::string{}
                   : " for `" + unresolved.receiver + "`"));
    }
    m_unresolvedMethods.clear();
    m_unresolvedMethodImports.clear();
}

// `namesOnly` runs the same traversal for its record IDENTITIES alone. Type
// aliases are registered before field types can be resolved (a field may BE an
// alias), yet an alias body names records too — so the names have to exist
// first, and the field types are filled in on the second pass.
auto TypeChecker::registerRecordFields(const ast::Program& program,
                                       bool namesOnly) -> void {
    // The unique-suffix guesses (resolveRecordName) depend on the records known.
    m_recordSuffixes.clear();
    auto registerRecord = [&](const ast::RecordDef& record,
                              const std::string& owner = std::string{}) {
        const auto name = owner.empty()
            ? record.name : owner + "." + record.name;
        auto& fields = m_recordFields[name];
        m_programRecords.insert(name);
        if (namesOnly) return;
        const auto previousModule = m_currentModulePath;
        m_currentModulePath = owner;
        // A field's type is written against the names its module imported.
        const auto previousImports = m_declarationImports;
        if (auto imports = m_recordImports.find(&record);
            imports != m_recordImports.end())
            m_declarationImports = imports->second;
        // `record Box<A>` binds A to the parameter slot -1 for the whole
        // declaration, the convention `substituteInterfaceGenerics` reads and
        // the one the KexI reader rebuilds imported records with. A map
        // shared across ALL records — which this used to be — did neither:
        // every record's `A` collapsed onto one fresh positive TypeVar, so a
        // field's element type could never be recovered from a receiver's
        // type arguments (kexhq/kex#203).
        std::unordered_map<std::string, TypePtr> generics;
        for (size_t i = 0; i < record.typeParams.size(); ++i)
            generics[record.typeParams[i]] =
                Type::typeVar(-static_cast<int>(i + 1));
        for (const auto& field : record.fields)
        {
            auto fieldType = field.type
                ? resolveTypeExpr(*field.type, generics)
                : Type::unknown();
            fields[field.name] = fieldType;
            if ((!field.defaultValue || !*field.defaultValue) &&
                !std::holds_alternative<OptionalType>(fieldType->kind))
                m_requiredRecordFields[name].insert(field.name);
        }
        m_currentModulePath = previousModule;
        m_declarationImports = previousImports;
    };
    std::function<void(const ast::ModuleDef&)> registerModule;
    registerModule = [&](const ast::ModuleDef& module) {
      for (const auto& item : module.body) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (node) registerRecord(*node, module.name);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                // A `private do ... end` record is still the module's own
                // record, and its identity has to be the qualified one its
                // module constructs.
                if (!node) return;
                for (const auto& visible : node->items)
                    if (const auto* record =
                            std::get_if<std::unique_ptr<ast::RecordDef>>(&visible);
                        record && *record)
                        registerRecord(**record, module.name);
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
                std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (!node) return;
                for (const auto& visible : node->items)
                    if (const auto* record =
                            std::get_if<std::unique_ptr<ast::RecordDef>>(&visible);
                        record && *record)
                        registerRecord(**record);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerModule(*node);
            }
        }, item);
}

auto TypeChecker::qualifiedNominalType(const std::string& qualified) const
    -> std::optional<std::string> {
    auto nominal = [&](const std::string& candidate) {
        return m_recordFields.count(candidate) ||
               m_adtVariants.count(candidate) ||
               (m_importedInterfaces &&
                m_importedInterfaces->recordArities.count(candidate));
    };
    // The spelling in full names exactly one type, whatever the module being
    // checked declares under the same last segment.
    if (nominal(qualified)) return qualified;
    auto resolved = resolveRecordName(qualified);
    if (nominal(resolved)) return resolved;
    return std::nullopt;
}

auto TypeChecker::lacksBuiltinOperator(const TypePtr& type,
                                       const std::string& op) const -> bool {
    // A tuple, map, optional, open record or function has no arithmetic,
    // order or negation; a list only concatenates. The walker said "Cannot
    // add Tuple and Tuple" at run time, BEAM `function_clause`.
    if (std::holds_alternative<TupleType>(type->kind) ||
        std::holds_alternative<MapType>(type->kind) ||
        std::holds_alternative<RecordType>(type->kind) ||
        std::holds_alternative<OptionalType>(type->kind) ||
        std::holds_alternative<FuncType>(type->kind))
        return true;
    if (std::holds_alternative<ListType>(type->kind)) return op != "+";
    // A distinct type keeps exactly the operators of what it erases to:
    // `distinct type Cash = Integer` adds, `= (Number, Number)` does not.
    if (const auto backing = distinctBacking(type);
        backing && !typesEqual(backing, type))
        return lacksBuiltinOperator(backing, op);
    if (const auto* bound = std::get_if<ConstrainedType>(&type->kind)) {
        const auto* trait = m_traits.lookup(bound->traitName);
        return trait && !primitiveNumberTrait(trait->name);
    }
    const auto* named = std::get_if<NamedType>(&type->kind);
    if (!named) return false;
    // Imported ones too: `Duration * Duration` has no operator in the
    // prelude, and died at run time like any record of the program's own.
    const auto name = resolveRecordName(named->name);
    if (m_recordFields.count(name) > 0 || m_adtVariants.count(name) > 0 ||
        (m_importedInterfaces &&
         m_importedInterfaces->recordArities.count(name) > 0))
        return true;
    const auto* trait = m_traits.lookup(named->name);
    return trait && !primitiveNumberTrait(trait->name);
}

auto TypeChecker::traitDeclaresOperator(const TypePtr& type,
                                        const std::string& op) const -> bool {
    const auto* bound = std::get_if<ConstrainedType>(&type->kind);
    const auto* named = std::get_if<NamedType>(&type->kind);
    if (!bound && !named) return false;
    if (const auto* trait = m_traits.lookup(bound ? bound->traitName : named->name)) {
        for (const auto& required : trait->requiredMethods)
            if (required.name == op) return true;
        return std::find(trait->defaultMethods.begin(), trait->defaultMethods.end(),
                         op) != trait->defaultMethods.end();
    }
    return m_traits.declaresMethod(m_traits.implementorKey(type), op);
}

auto TypeChecker::declaresOperatorFor(const TypePtr& type,
                                      const std::string& op) const -> bool {
    if (traitDeclaresOperator(type, op)) return true;
    auto owns = [&](const Signature& signature) {
        if (signature.params.size() != 2) return false;
        const auto receiver = resolve(signature.params[0]);
        return !std::holds_alternative<TypeVar>(receiver->kind) &&
               !std::holds_alternative<UnknownType>(receiver->kind) &&
               argMatchesParam(type, signature.params[0]);
    };
    if (auto local = m_methodSignatures.find(op); local != m_methodSignatures.end())
        if (std::any_of(local->second.begin(), local->second.end(), owns))
            return true;
    const auto imported = importedCandidateSignatures(op);
    return std::any_of(imported.begin(), imported.end(), owns);
}

auto TypeChecker::resolveRecordName(const std::string& name) const
    -> std::string {
    // ADTs share the records' identity rule: one declared inside
    // `module Kex` is `Kex.Backend`, so it can never be mistaken for a
    // same-spelled type or constructor declared anywhere else.
    auto recordExists = [&](const std::string& candidate) {
        return m_recordFields.count(candidate) ||
               m_adtVariants.count(candidate) ||
               (m_importedInterfaces &&
                m_importedInterfaces->recordArities.count(candidate));
    };
    if (auto scoped = resolve_core::resolveInScopes(
            m_currentModulePath, name, recordExists))
        return *scoped;
    auto importedRecord = [&](const ImportSelection& import)
        -> std::optional<std::string> {
        const auto candidate = import.module + "." + name;
        if (!recordExists(candidate)) return std::nullopt;
        if (!resolve_core::usingSelectsName(name, import.onlyNames,
                                            import.exceptNames))
            return std::nullopt;
        return candidate;
    };
    for (auto scope = m_importScopeStack.rbegin();
         scope != m_importScopeStack.rend(); ++scope)
        for (auto import = scope->rbegin(); import != scope->rend(); ++import)
            if (auto found = importedRecord(*import)) return *found;
    for (auto import = m_declarationImports.rbegin();
         import != m_declarationImports.rend(); ++import)
        if (auto found = importedRecord(*import)) return *found;
    // A record declared in this file under the bare name owns that name. The
    // last-ditch suffix scan below guesses at a module nobody imported, and a
    // guess must never outrank a declaration sitting in front of the user:
    // stdlib records live in m_importedInterfaces whether or not they were
    // imported, so `record Request` in a plain script resolved to
    // `Net.HTTP.Request` and reported its fields instead.
    // So does any other type declared under that name: an alias (`type Name
    // = String`), a trait, a distinct type.
    if (m_recordFields.count(name) || m_adtVariants.count(name) ||
        m_typeAliases.count(name) || m_traits.get(name) || m_distinctTypes.count(name))
        return name;
    {
        auto [cached, fresh] = m_recordSuffixes.try_emplace(name);
        if (fresh) {
            // Records only, this program's and imported ones alike: a module
            // ADT is never guessed at: an unimported `Message` must not become
            // Net.HTTP.WebSocket.Message over a local `type Message = :hello`
            // that is resolved after this.
            const auto match = resolve_core::matchSuffix(m_recordFields, "." + name);
            // Ambiguous answers the bare name at once, as it always has; no
            // match goes on to the aliases below.
            if (match.kind == resolve_core::SuffixMatchKind::Unique)
                cached->second = match.name;
            else if (match.kind == resolve_core::SuffixMatchKind::Ambiguous)
                cached->second = name;
        }
        if (cached->second) return *cached->second;
    }
    // A leading portion of `name` is a KNOWN ALIAS (`using Geometry, as:
    // Geo`) — a "Geo.Circle" reference names the record its TARGET module
    // registers as "Geometry.Circle", not "Geo.Circle" literally. Longest
    // prefix first, matching every lookup above (most specific wins).
    // Checked last: an unaliased, directly-registered name always wins
    // first (see the declaration-precedence comment above) — construction
    // through the alias otherwise inferred a DIFFERENT type ("Geo.Circle")
    // than the SAME record's field/method-derived type elsewhere
    // ("Geometry.Circle"), so passing one to a function expecting the
    // other was rejected as a type mismatch despite being the identical
    // value (kexhq/kex#alias).
    if (const auto dot = name.rfind('.'); dot != std::string::npos) {
        auto aliasFor = [&](const std::string& prefix) -> std::optional<std::string> {
            for (auto scope = m_importScopeStack.rbegin();
                 scope != m_importScopeStack.rend(); ++scope)
                for (auto import = scope->rbegin(); import != scope->rend(); ++import)
                    if (import->alias && *import->alias == prefix) return import->module;
            for (auto import = m_declarationImports.rbegin();
                 import != m_declarationImports.rend(); ++import)
                if (import->alias && *import->alias == prefix) return import->module;
            if (auto reexport = m_moduleReexports.find(prefix);
                reexport != m_moduleReexports.end())
                return reexport->second;
            return std::nullopt;
        };
        for (size_t prefixEnd = dot; prefixEnd != std::string::npos;
             prefixEnd = prefixEnd == 0 ? std::string::npos
                                        : name.rfind('.', prefixEnd - 1)) {
            if (auto target = aliasFor(name.substr(0, prefixEnd))) {
                const auto aliased = *target + name.substr(prefixEnd);
                if (recordExists(aliased)) return aliased;
            }
        }
    }
    return name;
}

// Resolves a bare module segment against the modules `using` brought into
// scope, when `name` is not itself a registered module — `Set.from([…])`
// after `using Data.Set` writes the receiver as `Set`, but the module is
// registered as `Data.Set` (a file-level `module Data` header qualifies the
// nested `module Set do … end` constructor block that provides it).
//
// Precedence, most to least specific:
//   1. `name` is already a real key — never touched (this is what keeps
//      `Units.Data` and `Data.Set` unambiguous when a file imports both:
//      a literal `Data.Set.from(…)` matches this tier before any alias is
//      considered, so the `Data` → `Units.Data` alias below never gets a
//      chance to capture it).
//   2. An active `using M` whose last segment is `name` (`using Data.Set`
//      aliases bare `Set`), or whose immediate child `M.name` is a real
//      module (`using Net.HTTP` aliases bare `Status` to `Net.HTTP.Status`).
//      Two imports aliasing the same bare segment is an ambiguity — leave
//      `name` unresolved rather than guessing.
//   3. A globally unique module whose last segment is `name`, the same
//      last-resort `resolveRecordName` takes for record names — this is what
//      lets `UnorderedSet.from(…)` work from the same `using Data.Set` that
//      only aliases `Set` under rule 2, since `Data.Set` and `Data.UnorderedSet`
//      share a file but not a parent/child relationship.
auto TypeChecker::resolveModulePath(const std::string& name,
                                    const std::string& member) const
    -> std::string {
    auto resolved = resolveImportedModulePath(name, member);
    const auto exportsMember = [&](const std::string& module) {
        if (m_localModules.contains(module) &&
            m_userSignatures.contains(kex::mangle::qualify(module, member)))
            return true;
        if (!m_importedInterfaces) return false;
        auto found = m_importedInterfaces->modules.find(module);
        if (found == m_importedInterfaces->modules.end()) return false;
        auto exported = found->second.exports.find(member);
        return exported != found->second.exports.end() && !exported->second.empty();
    };
    // A module of the enclosing module's own comes before another module the
    // name would otherwise reach: `Clock.now()` inside `module Clocks`,
    // beside its own `capability Clock`, called the prelude's `Time.Clock`
    // and failed on BEAM with `undef`, while the walker (which looks in the
    // module first) was right. The same holds where the name reaches no
    // module with the member at all: `Router.status` inside `module Http` is
    // `Http.Router.status`.
    if (auto nested = resolve_core::resolveInScopes(
            m_currentModulePath, name, [&](const std::string& candidate) {
                return candidate != resolved &&
                       m_localModules.contains(candidate) &&
                       exportsMember(candidate);
            }))
        return *nested;
    return resolved;
}

auto TypeChecker::resolveImportedModulePath(const std::string& name,
                                            const std::string& member) const
    -> std::string {
    // A `make X<A> do ... end` block unconditionally creates a placeholder
    // `ifaces.modules[typeName]` entry while scanning its own methods
    // (`prelude_interfaces.hxx`'s `collectMakeMember`), keyed by the type's
    // BARE written name regardless of the file's enclosing `module` header —
    // so `ifaces.modules["Set"]` exists even though `Set`'s static
    // constructors (`from`, `empty`) were published under the qualified
    // `Data.Set`. A mere key match is therefore not enough to trust `name`
    // literally; the literal module must actually export `member`.
    auto exportsMember = [&](const std::string& module) {
        if (m_localModules.contains(module) &&
            m_userSignatures.contains(kex::mangle::qualify(module, member)))
            return true;
        if (!m_importedInterfaces) return false;
        auto found = m_importedInterfaces->modules.find(module);
        if (found == m_importedInterfaces->modules.end()) return false;
        auto exported = found->second.exports.find(member);
        return exported != found->second.exports.end() && !exported->second.empty();
    };
    const auto canonicalModule = [&](const std::string& module) {
        if (exportsMember(module)) return module;
        const auto backend = names::moduleAtom(module);
        return exportsMember(backend) ? backend : module;
    };
    if (exportsMember(canonicalModule(name))) return canonicalModule(name);
    // `using M, as: A` binds `A` (and `A.Child`) to `M`. Innermost scope
    // first, so a block-level alias shadows a file-level one.
    auto aliased = [&](const ImportSelection& import) -> std::optional<std::string> {
        if (!import.alias) return std::nullopt;
        const auto& alias = *import.alias;
        if (name != alias && !name.starts_with(alias + ".")) return std::nullopt;
        const auto target = canonicalModule(import.module + name.substr(alias.size()));
        return exportsMember(target) ? std::optional<std::string>{target}
                                     : std::nullopt;
    };
    for (auto scope = m_importScopeStack.rbegin();
         scope != m_importScopeStack.rend(); ++scope)
        for (const auto& import : *scope)
            if (auto target = aliased(import)) return *target;
    for (const auto& import : m_declarationImports)
        if (auto target = aliased(import)) return *target;
    auto lastSegmentOf = [](const std::string& module) {
        const auto dot = module.rfind('.');
        return dot == std::string::npos ? module : module.substr(dot + 1);
    };
    auto consider = [&](const ImportSelection& import,
                        std::vector<std::string>& candidates) {
        if (import.alias) return;
        const auto& module = import.module;
        const auto child = module + "." + name;
        const auto root = name.substr(0, name.find('.'));
        const bool included = import.onlyNames.empty() ||
            std::find(import.onlyNames.begin(), import.onlyNames.end(), root) !=
                import.onlyNames.end();
        const bool excluded =
            std::find(import.exceptNames.begin(), import.exceptNames.end(), root) !=
                import.exceptNames.end();
        if (included && !excluded && exportsMember(canonicalModule(child))) {
            candidates.push_back(canonicalModule(child));
            return;
        }
        if (lastSegmentOf(module) == name && exportsMember(canonicalModule(module)))
            candidates.push_back(canonicalModule(module));
    };
    std::vector<std::string> candidates;
    for (const auto& import : m_declarationImports) consider(import, candidates);
    for (const auto& scope : m_importScopeStack)
        for (const auto& import : scope) consider(import, candidates);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.size() == 1) return candidates.front();
    if (!candidates.empty()) return name;

    if (!m_importedInterfaces) return name;
    std::optional<std::string> unique;
    const auto suffix = "." + name;
    for (const auto& [candidate, _] : m_importedInterfaces->modules) {
        if (candidate.size() <= suffix.size() ||
            candidate.compare(candidate.size() - suffix.size(), suffix.size(),
                              suffix) != 0 ||
            !exportsMember(candidate))
            continue;
        if (unique && *unique != candidate) return name;
        unique = candidate;
    }
    return unique.value_or(name);
}

auto TypeChecker::registerAdt(const ast::TypeDef& def,
                              const std::string& modulePath) -> void {
    if (!def.variants) return;

    if (def.isDistinct || kex::isTransparentTypeAlias(def)) return;

    std::vector<std::string> names;
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) return;  // not constructor-shaped — a type alias, skip entirely
        names.push_back(*name);
        if (!modulePath.empty()) continue;
        if (std::holds_alternative<ast::TypeName>(variant->kind))
            m_nullaryConstructors.insert(*name);
        // Payload arity: `None` is 0, `Just(T)` is 1, `Between(A, B)` is 2.
        if (auto* gt = std::get_if<ast::GenericType>(&variant->kind))
            m_constructorArity[*name] = gt->args.size();
        else
            m_constructorArity[*name] = 0;
    }
    if (names.empty()) return;

    // Inside a module the ADT's identity is qualified, like a record's. Its
    // constructors stay out of the bare-name owner map: `Kex.Backend`'s
    // `Beam` is reached through the module, never by spelling alone.
    const auto identity =
        modulePath.empty() ? def.name : modulePath + "." + def.name;
    m_adtVariants[identity] = names;
    if (modulePath.empty())
        for (const auto& name : names)
            m_adtOfConstructor[name] = def.name;

    // Record what each constructor produces, so `Just(1)` infers as an
    // Optional instead of `unknown` — without that, a mismatched pattern in
    // `if let Ok(x) = Just(1)` has no scrutinee type to be checked against.
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) continue;
        ConstructorResult result;
        result.adtName = identity;
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
                // Keep the declared type too, so a pattern binding can take
                // its type from the declaration rather than waiting for a use
                // to constrain it.
                TypePtr declared;
                if (payload) {
                    std::unordered_map<std::string, TypePtr> generics;
                    declared = resolveTypeExpr(*payload, generics);
                    if (declared &&
                        std::holds_alternative<UnknownType>(declared->kind))
                        declared = nullptr;
                }
                result.payloadTypes.push_back(std::move(declared));
                result.payloadExprs.push_back(payload.get());
            }
        }
        result.modulePath = modulePath;
        result.declaredHere = true;
        const auto key = modulePath.empty() ? *name : kex::mangle::qualify(modulePath, *name);
        m_constructorResult[key] = std::move(result);
    }
}

// A nullary constructor is typed as its own singleton type (`Less`, and the
// typestate modes `Read`/`Write` that `FS.File.open` is overloaded on), so two
// branches returning different constructors of one ADT — `c then Less else
// Greater` — are not a mismatch but that ADT. Null when `a` and `b` are not
// two distinct constructors of the same ADT (an ADT meeting one of its own
// constructors already passes argMatchesParam).
auto TypeChecker::siblingConstructorJoin(const TypePtr& a, const TypePtr& b) const
    -> TypePtr {
    const auto* left = std::get_if<NamedType>(&resolve(a)->kind);
    const auto* right = std::get_if<NamedType>(&resolve(b)->kind);
    if (!left || !right || left->name == right->name ||
        !left->typeArgs.empty() || !right->typeArgs.empty())
        return nullptr;
    const auto* leftOwner = constructorOwner(*left);
    const auto* rightOwner = constructorOwner(*right);
    if (!leftOwner || !rightOwner || *leftOwner != *rightOwner) return nullptr;
    return Type::named(*leftOwner);
}

auto TypeChecker::scopedConstructorOwner(const std::string& name) const
    -> const ConstructorOwner* {
    // A spelling with one owner is already in m_adtOfConstructor.
    if (m_adtOfConstructor.count(name)) return nullptr;
    const auto owners = m_constructorOwners.find(name);
    if (owners == m_constructorOwners.end()) return nullptr;
    auto visible = [&](const std::string& adt) {
        const auto dot = adt.rfind('.');
        // A top-level stdlib ADT is file-level prelude: always in scope.
        if (dot == std::string::npos) return true;
        const auto module = adt.substr(0, dot);
        if (m_currentModulePath == module ||
            m_currentModulePath.starts_with(module + "."))
            return true;
        if (moduleMemberImported(module, name)) return true;
        if (!m_importedInterfaces) return false;
        const auto imported = m_importedInterfaces->modules.find(module);
        return imported != m_importedInterfaces->modules.end() &&
               imported->second.automaticImport;
    };
    const ConstructorOwner* chosen = nullptr;
    for (const auto& owner : owners->second) {
        if (!visible(owner.adt)) continue;
        if (chosen && chosen->adt != owner.adt) return nullptr;
        chosen = &owner;
    }
    return chosen;
}

auto TypeChecker::constructorOwner(const NamedType& named) const
    -> const std::string* {
    if (!named.typeArgs.empty()) return nullptr;
    auto owner = m_adtOfConstructor.find(named.name);
    if (owner == m_adtOfConstructor.end()) {
        // A constructor of a module's ADT — `Twomod.Access`'s `Private` — is
        // owned only where that module is in scope. Without it, `Private`
        // and `Public` did not join in a `then … else` (kexhq/kex#504) and
        // `Type.of(Public)` was not widened to its type (#505).
        if (const auto* scoped = scopedConstructorOwner(named.name))
            return &scoped->adt;
        const std::string* found = nullptr;
        for (const auto& [module, constructors] : m_moduleConstructors) {
            const auto constructor = constructors.find(named.name);
            if (constructor == constructors.end() ||
                constructor->second.typeName.empty())
                continue;
            if (!withinModule(module) && !moduleMemberImported(module, named.name))
                continue;
            if (found && *found != constructor->second.typeName) return nullptr;
            found = &constructor->second.typeName;
        }
        return found;
    }
    // `Kex.Backend` is registered as the ADT `Backend`, and `Backend` is also
    // a nullary NetErrorKind constructor. Looked up by spelling alone, every
    // `Backend` value became a NetErrorKind.
    if (owner->second != named.name &&
        (m_adtVariants.count(named.name) || m_typeAliases.count(named.name) ||
         isPrimitiveTypeName(named.name)))
        return nullptr;
    return &owner->second;
}

auto TypeChecker::constructorInfo(const std::string& name, const std::string& scope) const
    -> const ConstructorResult* {
    auto found = scope.empty() ? m_constructorResult.end()
                               : m_constructorResult.find(kex::mangle::qualify(scope, name));
    if (!scope.empty()) return found == m_constructorResult.end() ? nullptr : &found->second;
    // A constructor declared in this lexical module shadows an imported
    // constructor with the same spelling (for example a local Done versus
    // Control.Retry's Done). Do not let stdlib interfaces choose its ADT.
    for (auto module = scope.empty() ? m_currentModulePath : std::string{}; !module.empty();) {
        found = m_constructorResult.find(kex::mangle::qualify(module, name));
        if (found != m_constructorResult.end()) break;
        const auto dot = module.rfind('.');
        if (dot == std::string::npos) break;
        module.resize(dot);
    }
    if (found == m_constructorResult.end()) found = m_constructorResult.find(name);
    return found == m_constructorResult.end() ? nullptr : &found->second;
}

auto TypeChecker::declaredPayload(const ConstructorResult& info, std::size_t i) -> TypePtr {
    auto payload = i < info.payloadTypes.size() ? info.payloadTypes[i] : nullptr;
    if (i >= info.payloadExprs.size() || !info.payloadExprs[i]) return payload;
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = info.modulePath;
    std::unordered_map<std::string, TypePtr> generics;
    auto resolved = resolveTypeExpr(*info.payloadExprs[i], generics);
    m_currentModulePath = previousModule;
    if (resolved && !std::holds_alternative<UnknownType>(resolved->kind)) return resolved;
    return payload;
}

auto TypeChecker::checkConstructorPayloads(const std::string& name,
                                           const std::vector<TypePtr>& argTypes,
                                           SourceLocation loc, const std::string& scope) -> void {
    const auto* info = constructorInfo(name, scope);
    if (!info || !info->declaredHere) return;
    const auto arity = info->payloadTypes.size();
    if (argTypes.size() != arity) {
        error(loc, "`" + name + "` takes " + std::to_string(arity) +
                       (arity == 1 ? " value" : " values") + ", but got " +
                       std::to_string(argTypes.size()));
    }
}

auto TypeChecker::constructorResultType(
    const std::string& name, const std::vector<TypePtr>& argTypes,
    const std::string& scope) -> TypePtr {
    const auto* found = constructorInfo(name, scope);
    if (!scope.empty() && !found) return nullptr;
    if (!found) {
        // A payload constructor whose spelling two ADTs share (`Text(s)`):
        // its ADT is the one in scope.
        if (const auto* owner = scopedConstructorOwner(name);
            owner && owner->arity > 0 && owner->arity == argTypes.size() &&
            !owner->generic)
            return Type::named(owner->adt);
        return nullptr;
    }
    const auto& info = *found;

    std::vector<TypePtr> typeArgs;
    typeArgs.reserve(info.typeParamCount);
    for (size_t i = 0; i < info.typeParamCount; ++i)
        typeArgs.push_back(freshTypeVar());
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
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = previousModule.empty() || mod.name.starts_with(previousModule + ".")
        ? mod.name : previousModule + "." + mod.name;
    for (const auto& item : mod.body) {
        std::visit([this, &mod](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node) return;
                registerAdt(*node, m_currentModulePath);
                if (auto constructors = kex::typeConstructors(*node))
                    for (const auto& constructor : *constructors)
                        m_moduleConstructors[mod.name][constructor.name] = {
                            m_currentModulePath + "." + node->name,
                            constructor.arity, true};
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerAdtsInModule(*node);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (!node) return;
                for (const auto& visible : node->items)
                    if (const auto* type =
                            std::get_if<std::unique_ptr<ast::TypeDef>>(
                                &visible);
                        type && *type) {
                        registerAdt(**type, m_currentModulePath);
                        if (auto constructors =
                                kex::typeConstructors(**type))
                            for (const auto& constructor : *constructors)
                                m_moduleConstructors[mod.name]
                                                    [constructor.name] = {
                                    m_currentModulePath + "." + (*type)->name,
                                    constructor.arity, node->isPublic};
                    }
            }
        }, item);
    }
    m_currentModulePath = previousModule;
}

auto TypeChecker::typeDefToType(const ast::TypeDef& def) -> TypePtr {
    if (!def.variants || def.variants->empty()) return Type::unknown();
    // A generic alias's parameters take the negative TypeVar slots -1, -2, …
    // — the convention `substituteInterfaceGenerics` fills — so an applied
    // alias (`Predicate<Integer>`) can be expanded by substituting its
    // arguments, the way a distinct type's backing type already is (#337).
    std::unordered_map<std::string, TypePtr> aliasParams;
    for (size_t i = 0; i < def.typeParams.size(); ++i)
        aliasParams[def.typeParams[i]] = Type::typeVar(-static_cast<int>(i + 1));
    // The body names types the way its own module imported them — an alias
    // is expanded once, here, not re-read wherever it is used.
    struct RestoreImports {
        std::vector<ImportSelection>& target;
        std::vector<ImportSelection> saved;
        ~RestoreImports() { target = std::move(saved); }
    } restoreImports{m_declarationImports, m_declarationImports};
    if (auto imports = m_typeImports.find(&def); imports != m_typeImports.end())
        m_declarationImports = imports->second;
    // Build variant types, then fold into a union if more than one.
    std::vector<TypePtr> parts;
    for (const auto& v : *def.variants) {
        if (v) parts.push_back(resolveTypeExpr(*v, aliasParams));
    }
    if (parts.empty()) return Type::unknown();
    if (parts.size() == 1) return parts[0];
    return std::make_shared<Type>(Type{UnionType{std::move(parts)}});
}

auto TypeChecker::resolveDistinctName(const std::string& name) const
    -> std::string {
    if (name.find('.') != std::string::npos && m_distinctTypes.count(name))
        return name;
    auto scope = m_currentModulePath;
    while (!scope.empty()) {
        const auto candidate = scope + "." + name;
        if (m_distinctTypes.count(candidate)) return candidate;
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) break;
        scope.resize(dot);
    }
    return name;
}

auto TypeChecker::distinctBacking(const TypePtr& type) const -> TypePtr {
    if (!type) return type;
    const auto* named = std::get_if<NamedType>(&type->kind);
    if (!named) return type;
    auto found = m_distinctTypes.find(named->name);
    if (found == m_distinctTypes.end()) {
        const auto resolvedName = resolveDistinctName(named->name);
        found = m_distinctTypes.find(resolvedName);
    }
    if (found == m_distinctTypes.end() || !found->second.backingType)
        return type;

    std::unordered_map<int, TypePtr> substitutions;
    for (size_t i = 0; i < named->typeArgs.size(); ++i)
        substitutions[-static_cast<int>(i + 1)] = named->typeArgs[i];
    std::function<TypePtr(const TypePtr&)> substitute =
        [&](const TypePtr& part) -> TypePtr {
            if (!part) return part;
            if (const auto* var = std::get_if<TypeVar>(&part->kind)) {
                if (auto value = substitutions.find(var->id);
                    value != substitutions.end())
                    return value->second;
                return part;
            }
            return mapTypeChildren(part, substitute);
        };
    auto backing = substitute(found->second.backingType);
    return distinctBacking(backing);
}

auto TypeChecker::representationsCompatible(const TypePtr& from,
                                            const TypePtr& to) -> bool {
    const auto source = distinctBacking(resolve(from));
    const auto target = distinctBacking(resolve(to));
    if (typesEqual(source, target)) return true;
    // A value the backing type already admits: `(2, -4)` is an
    // `(Integer, Integer)`, and `(Number, Number)` holds it. Only that way
    // round — a `Number` given back as an `Integer` might be a Float.
    const bool intoDistinct = !typesEqual(resolve(to), target);
    if (intoDistinct && argMatchesParam(source, target)) return true;
    return (m_traits.satisfies(source, "Integer") &&
            m_traits.satisfies(target, "Integer")) ||
           (m_traits.satisfies(source, "Float") &&
            m_traits.satisfies(target, "Float"));
}

// `check()` runs every FunctionDef before any make block (see its two-pass
// comment), so while a function body is being checked no LOCAL trait
// conformance has been registered yet — only imported ones and the builtins.
// A trait-typed default parameter value therefore had nothing to match
// against and was rejected (kexhq/kex#175). Registering the declared
// (type, trait) pairs up front fixes that; `checkTraitImplementation` still
// runs later and still reports a make block that fails to provide the
// trait's required methods.
// `capability Name do ... end` declares an interface AND its default
// implementation in one. The interface half is a trait synthesized from the
// module's own public functions, so `make Fake, implement: Name` works with
// the machinery that already exists, and the bodies remain the default the
// capability carries outside any `with` (kexhq/kex#143).
auto TypeChecker::registerCapabilityTraits(const ast::Program& program) -> void {
    std::function<void(const ast::ModuleDef&)> walk =
        [&](const ast::ModuleDef& module) {
        if (module.isCapability) {
            m_capabilities.insert(module.name);
            TraitDef td;
            td.name = module.name;
            for (const auto& mi : module.body)
                std::visit([&](const auto& mn) {
                    using MT = std::decay_t<decltype(mn)>;
                    if constexpr (std::is_same_v<MT,
                                                 std::unique_ptr<ast::FunctionDef>>) {
                        if (!mn || mn->clauses.empty()) return;
                        // A capability's INTERFACE is its `foul` members. A
                        // pure member performs no effect, so there is nothing
                        // for a stand-in to substitute and no reason to make
                        // one implement it — `IO.inspect` is deliberately pure
                        // (it is the debug hatch purity checking ignores) and
                        // requiring it would have kept `IO` from being a
                        // capability at all (kexhq/kex#143).
                        if (!mn->isFoul) return;
                        Signature sig;
                        sig.name = mn->name;
                        sig.isFoul = mn->isFoul;
                        // Arity matters for conformance; the parameter types
                        // stay permissive here because the capability's own
                        // bodies are checked like any other module's.
                        for (size_t i = 0; i < mn->clauses.front().params.size(); i++)
                            sig.params.push_back(Type::unknown());
                        sig.result = Type::unknown();
                        td.requiredMethods.push_back(std::move(sig));
                    }
                }, mi);
            m_traits.define(std::move(td));
        }
        for (const auto& mi : module.body)
            std::visit([&](const auto& mn) {
                using MT = std::decay_t<decltype(mn)>;
                if constexpr (std::is_same_v<MT, std::unique_ptr<ast::ModuleDef>>) {
                    if (mn) walk(*mn);
                }
            }, mi);
    };
    for (const auto& item : program.items)
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) walk(*node);
            }
        }, item);

    // A capability declared in an imported module is not in this AST, so its
    // interface is the only place its members are known. Registering it here
    // is what lets a consumer write `with FS.File = ...` and
    // `make Fake, implement: FS.File` against a capability it did not declare.
    if (m_importedInterfaces)
        for (const auto& [name, interface] : m_importedInterfaces->modules) {
            if (!interface.isCapability) continue;
            m_capabilities.insert(name);
            if (m_traits.get(name)) continue;
            TraitDef td;
            td.name = name;
            for (const auto& [member, overloads] : interface.exports) {
                if (overloads.empty()) continue;
                // Foul members only — see the local branch above.
                if (!overloads.front().signature.isFoul) continue;
                Signature sig = overloads.front().signature;
                sig.name = member;
                td.requiredMethods.push_back(std::move(sig));
            }
            // `exports` is a hash map, so the loop above yields its members in
            // an order that differs between standard-library implementations.
            // A trait's required methods are its DICTIONARY LAYOUT — slot i
            // means one method to whoever builds the dictionary and another to
            // whoever indexes it — so an unsorted list makes the emitted code
            // depend on the host's hash order. It built and passed on macOS
            // and died `badarg` on Linux, on both glibc and musl
            // (kexhq/kex#143).
            std::sort(td.requiredMethods.begin(), td.requiredMethods.end(),
                      [](const Signature& a, const Signature& b) {
                          if (a.name != b.name) return a.name < b.name;
                          return a.params.size() < b.params.size();
                      });
            m_traits.define(std::move(td));
        }
}

auto TypeChecker::registerLocalConformances(const ast::Program& program) -> void {
    // A record declared in a module is known by its QUALIFIED name, so a
    // `make` block inside that module has to register its conformance under
    // the same name. Registering the bare one left `make Files, implement:
    // FS.File` inside `module Mock` conforming a type called `Files` that
    // nothing is, and `with FS.File = Mock.Files { ... }` was rejected as not
    // implementing the capability (kexhq/kex#143).
    auto registerOne = [this](const ast::MakeDef& def,
                              const std::string& modulePath) {
        if (def.implements.empty() || !def.target) return;
        std::string typeName;
        if (const auto name = writtenMakeTarget(def.target, StructuralTarget::Excluded);
            name && name->parts.size() == 1)
            typeName = name->parts[0];
        if (typeName.empty()) return;
        static const std::vector<ImportSelection> kNoImports;
        const auto imports = m_makeImports.find(&def);
        for (const auto& written : def.implements) {
            const auto traitName = resolveTraitReference(
                written, modulePath,
                imports == m_makeImports.end() ? &kNoImports : &imports->second);
            m_traits.registerImplementation(typeName, traitName);
            if (!modulePath.empty())
                m_traits.registerImplementation(modulePath + "." + typeName,
                                                traitName);
        }
    };
    std::function<void(const ast::ModuleDef&, const std::string&)> walkModule =
        [&](const ast::ModuleDef& module, const std::string& prefix) {
        const std::string path =
            prefix.empty() ? module.name : prefix + "." + module.name;
        for (const auto& mi : module.body)
            std::visit([&](const auto& mn) {
                using MT = std::decay_t<decltype(mn)>;
                if constexpr (std::is_same_v<MT, std::unique_ptr<ast::MakeDef>>) {
                    if (mn) registerOne(*mn, path);
                } else if constexpr (std::is_same_v<MT,
                                                    std::unique_ptr<ast::ModuleDef>>) {
                    if (mn) walkModule(*mn, path);
                }
            }, mi);
    };
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) registerOne(*node, "");
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) walkModule(*node, "");
            }
        }, item);
    }
}

auto TypeChecker::registerTraits(const ast::Program& program) -> void {
    kex::forEachTraitDef(program.items, [this](const ast::TraitDef& node,
                                               const std::string& modulePath) {
        TraitDef td;
        td.name = kex::traitIdentity(modulePath, node.name);
        for (const auto& bodyItem : node.body) {
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
                    if (bi) {
                        td.defaultMethods.push_back(bi->name);
                        td.defaultBodies.push_back(bi->name);
                        for (const auto& clause : bi->clauses)
                            m_traitDefaultClauses[&clause] = td.name;
                        m_traitDefaults[td.name].push_back(bi.get());
                    }
                }
            }, bodyItem);
        }
        m_traits.define(std::move(td));
    });
}

// The identity of a trait reference written in module `scope`, against the
// imports in effect there (`imports`, or the checker's current ones when
// null) — kex::resolveTraitName's rules. `lastResort` allows its final step,
// a trait exactly one module declares; a type annotation leaves it out, so a
// name that is some other module's record is never taken for a trait.
auto TypeChecker::resolveTraitReference(
    const std::string& written, const std::string& scope,
    const std::vector<ImportSelection>* imports, bool lastResort,
    std::vector<std::string>* ambiguous) const -> std::string {
    const auto bare = written.substr(written.rfind('.') + 1);
    std::vector<std::string> modules;
    std::string throughAlias;
    auto consider = [&](const ImportSelection& import) {
        // `using Geo, as: G` then `G.Shape`.
        if (import.alias && written.starts_with(*import.alias + "."))
            throughAlias = import.module + written.substr(import.alias->size());
        if (!resolve_core::usingSelectsName(bare, import.onlyNames,
                                            import.exceptNames))
            return;
        modules.push_back(import.module);
    };
    if (imports) {
        for (auto import = imports->rbegin(); import != imports->rend(); ++import)
            consider(*import);
    } else {
        for (auto frame = m_importScopeStack.rbegin();
             frame != m_importScopeStack.rend(); ++frame)
            for (auto import = frame->rbegin(); import != frame->rend(); ++import)
                consider(*import);
        for (auto import = m_declarationImports.rbegin();
             import != m_declarationImports.rend(); ++import)
            consider(*import);
    }
    const auto known = [this](const std::string& name) {
        return m_traits.get(name) != nullptr;
    };
    if (!throughAlias.empty() && known(throughAlias)) return throughAlias;
    const auto eachKnown = [&](const auto& visit) {
        if (!lastResort) return;
        for (const auto& [name, _] : m_traits.all()) visit(name);
    };
    return kex::resolveTraitName(written, scope, modules, known, eachKnown,
                                 ambiguous);
}

auto TypeChecker::registerTypeAliases(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                if (node->isDistinct) {
                    std::unordered_map<std::string, TypePtr> vars;
                    for (size_t i = 0; i < node->typeParams.size(); ++i)
                        vars[node->typeParams[i]] =
                            Type::typeVar(-static_cast<int>(i + 1));
                    m_programDistinctTypes.insert(node->name);
                    m_distinctTypes[node->name] = {
                        node->typeParams,
                        resolveTypeExpr(*node->variants->front(), vars)};
                    return;
                }
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
                    const auto& only = (*node->variants)[0]->kind;
                    // An APPLIED generic is transparent too. `type ReadOnlyFile
                    // = FileHandle<CanRead, CannotWrite>` looks
                    // constructor-shaped to the check below — `Name<args>`
                    // parses much like `Name(args)` — so it fell through and
                    // became an opaque type of its own, matching not even the
                    // type it aliases (kexhq/kex#173).
                    const auto* generic = std::get_if<ast::GenericType>(&only);
                    if (std::holds_alternative<ast::TypeName>(only) ||
                        (generic && generic->applied)) {
                        m_typeAliases[node->name] = typeDefToType(*node);
                        return;
                    }
                }
                // Only register as an alias if this is not an ADT.
                // ADT, not an alias — but only when EVERY variant is
                // constructor-shaped, which is the same bar `registerAdt`
                // sets. A union mixing a bare name with a QUALIFIED one
                // (`type Reply = String | Web.Flat`) satisfied neither: this
                // loop saw `String`, called it an ADT and skipped the alias,
                // while registerAdt saw `Web.Flat`, could not name a
                // constructor for it and registered nothing. `Reply` then
                // existed nowhere and refused every member. A qualified
                // member means a union of TYPES, so it belongs here.
                if (std::all_of(node->variants->begin(), node->variants->end(),
                                [](const auto& v) {
                                    return extractConstructorName(v).has_value();
                                }))
                    return;
                m_typeAliases[node->name] = typeDefToType(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerTypeAliasesInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::registerTypeAliasesInModule(const ast::ModuleDef& mod) -> void {
    // An alias body names types the way its own module sees them: `type
    // Handler = Request -> Response` inside `module Http` means Http's
    // records, not a same-named record some other module exports.
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = previousModule.empty() ||
                                  mod.name.rfind(previousModule + ".", 0) == 0
        ? mod.name
        : previousModule + "." + mod.name;
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                if (node->isDistinct) {
                    std::unordered_map<std::string, TypePtr> vars;
                    for (size_t i = 0; i < node->typeParams.size(); ++i)
                        vars[node->typeParams[i]] =
                            Type::typeVar(-static_cast<int>(i + 1));
                    ImportedDistinctType info{
                        node->typeParams,
                        resolveTypeExpr(*node->variants->front(), vars)};
                    m_programDistinctTypes.insert(node->name);
                    m_programDistinctTypes.insert(m_currentModulePath + "." + node->name);
                    m_distinctTypes[node->name] = info;
                    m_distinctTypes[m_currentModulePath + "." + node->name] =
                        std::move(info);
                    return;
                }
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
                    const auto& only = (*node->variants)[0]->kind;
                    // An APPLIED generic is transparent too. `type ReadOnlyFile
                    // = FileHandle<CanRead, CannotWrite>` looks
                    // constructor-shaped to the check below — `Name<args>`
                    // parses much like `Name(args)` — so it fell through and
                    // became an opaque type of its own, matching not even the
                    // type it aliases (kexhq/kex#173).
                    const auto* generic = std::get_if<ast::GenericType>(&only);
                    if (std::holds_alternative<ast::TypeName>(only) ||
                        (generic && generic->applied)) {
                        m_typeAliases[node->name] = typeDefToType(*node);
                        return;
                    }
                }
                // ADT, not an alias — but only when EVERY variant is
                // constructor-shaped, which is the same bar `registerAdt`
                // sets. A union mixing a bare name with a QUALIFIED one
                // (`type Reply = String | Web.Flat`) satisfied neither: this
                // loop saw `String`, called it an ADT and skipped the alias,
                // while registerAdt saw `Web.Flat`, could not name a
                // constructor for it and registered nothing. `Reply` then
                // existed nowhere and refused every member. A qualified
                // member means a union of TYPES, so it belongs here.
                if (std::all_of(node->variants->begin(), node->variants->end(),
                                [](const auto& v) {
                                    return extractConstructorName(v).has_value();
                                }))
                    return;
                m_typeAliases[node->name] = typeDefToType(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerTypeAliasesInModule(*node);
            }
        }, item);
    }
    m_currentModulePath = previousModule;
}

auto TypeChecker::annotationToSignature(
    const ast::TypeAnnotation& ann,
    std::unordered_map<std::string, TypePtr>* sharedGenericVars)
    -> std::optional<Signature> {
    if (!ann.type) return std::nullopt;
    // Unroll `A -> B -> C` (right-nested FunctionType) into params=[A,B], result=C.
    // `A -> B -> C` and `A -> (B -> C)` build the identical right-nested
    // FunctionType tree — the parens are otherwise transparent grouping — so
    // the two-parameter curry and the one-parameter function-returning-a-
    // function are indistinguishable without `parenthesized`. It only
    // matters once we are already inside the chain: the ANNOTATION's own
    // outermost node may itself be a redundant `(A -> B)`, which unrolls
    // exactly like unparenthesized `A -> B` would.
    std::unordered_map<std::string, TypePtr> ownGenericVars;
    auto& genericVars = sharedGenericVars ? *sharedGenericVars : ownGenericVars;
    std::vector<TypePtr> params;
    const ast::TypeExpr* cur = ann.type.get();
    bool firstNode = true;
    while (cur) {
        auto* ft = std::get_if<ast::FunctionType>(&cur->kind);
        if (!ft || (!firstNode && cur->parenthesized)) break;
        params.push_back(ft->param ? resolveTypeExpr(*ft->param, genericVars) : Type::unknown());
        cur = ft->result.get();
        firstNode = false;
    }
    TypePtr result = cur ? resolveTypeExpr(*cur, genericVars) : Type::unknown();
    // A curried/higher-order signature spelled through a TRANSPARENT alias
    // (`type Fn = Integer -> Integer; type Wrap = Fn -> Fn`) has no
    // `FunctionType` node in the annotation's own AST at all — `cur` is just
    // the bare name `Wrap` — so the syntax-level unrolling loop above never
    // runs, and `result` ends up holding the alias's ENTIRE resolved arrow
    // type as if it were the return value of a zero-parameter signature
    // (kexhq/kex#375). `resolveTypeExpr` already resolved `Wrap` all the way
    // through to its structural `FuncType` (m_typeAliases stores each alias
    // fully resolved, not just one layer), so unroll THAT the same way the
    // syntax-level loop above unrolls an explicit `(A -> B) -> (C -> D)`
    // (kexhq/kex#366 fixed the miscount for that spelling; this is the same
    // count run on the alias's resolved type instead of its surface syntax).
    if (params.empty()) {
        if (auto* resolvedFunc = std::get_if<FuncType>(&result->kind)) {
            params = resolvedFunc->params;
            result = resolvedFunc->result;
        }
    }
    if (params.empty()) {
        // Non-function annotation (e.g. `x : Int`) — treat as a zero-param
        // constant whose type IS the annotated type.
        return Signature{ann.name, {}, result};
    }
    return Signature{ann.name, std::move(params), result};
}

auto TypeChecker::registerDeclaredSignatures(const ast::Program& program) -> void {
    auto add = [&](const ast::TypeAnnotation& ann, const std::string& modulePath) {
        // A contract's type variables are its own generics: `ident : T -> T`
        // binds T per call, as an inline `let ident(x: T) -> T` does.
        const int varMark = m_nextTypeVar;
        auto sig = annotationToSignature(ann);
        if (!sig) return;
        generalizeSignature(*sig, varMark);
        sig->declaringModule = modulePath;
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
    const auto modulePath = ast::nestedModulePath(parentPath, mod.name);
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = modulePath;
    auto importsFor = [&](const std::string& name)
        -> const std::vector<ImportSelection>* {
        for (const auto& item : mod.body) {
            if (const auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
                fn && *fn && (*fn)->name == name) {
                if (auto found = m_functionImports.find(fn->get());
                    found != m_functionImports.end())
                    return &found->second;
            }
            if (const auto* visibility =
                    std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                visibility && *visibility) {
                for (const auto& visible : (*visibility)->items)
                    if (const auto* fn = std::get_if<
                            std::unique_ptr<ast::FunctionDef>>(&visible);
                        fn && *fn && (*fn)->name == name)
                        if (auto found = m_functionImports.find(fn->get());
                            found != m_functionImports.end())
                            return &found->second;
            }
        }
        return nullptr;
    };
    auto add = [this, &modulePath, &importsFor](
                   const ast::TypeAnnotation& ann, bool exposeUnqualified) {
        const auto previousImports = m_declarationImports;
        if (const auto* imports = importsFor(ann.name))
            m_declarationImports = *imports;
        const int varMark = m_nextTypeVar;
        auto sig = annotationToSignature(ann);
        m_declarationImports = previousImports;
        if (!sig) return;
        generalizeSignature(*sig, varMark);
        sig->declaringModule = modulePath;
        m_annotationDeclared.insert(ann.name);
        const auto key = modulePath + "\n" + ann.name;
        m_annotationArities[key].insert(sig->params.size());
        m_scopedDeclaredSignatures[key].push_back(*sig);
        // Reachable qualified before its definition is checked, so a call
        // above the definition resolves to it (#103). Not marked published:
        // the definition's own check still replaces this with what it checks.
        if (!m_qualifiedPublished.contains(kex::mangle::qualify(modulePath, ann.name)))
            m_userSignatures[kex::mangle::qualify(modulePath, ann.name)].push_back(*sig);
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
    m_currentModulePath = previousModule;
}

auto TypeChecker::registerMakeSignature(const ast::MakeDef& def,
                                       const std::string& modulePath) -> void {
    // Every method name this block defines, private ones included. Only the
    // NAME is needed here: it answers "does this method exist anywhere?" for
    // the unknown-method report, which is name-based by design.
    auto noteName = [&](const ast::FunctionDef& fn) {
        m_makeMethodNames.insert(fn.name);
    };
    for (const auto& item : def.body) {
        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fn) noteName(**fn);
        } else if (const auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
            if (!*visibility) continue;
            for (const auto& inner : (*visibility)->items)
                if (const auto* vfn =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner))
                    if (*vfn) noteName(**vfn);
        }
    }
    if (!def.target) return;
    // The target names a type the way source inside the module does, so it has
    // to resolve in that module's scope. Without this a `make Server` inside
    // `module CollisionWeb` resolved bare `Server` through the global
    // unique-suffix fallback and registered its methods against the prelude's
    // `Web.Server`, which checkMakeDef (which does set the path) then
    // registered a second time under the right name.
    const auto previousModulePath = m_currentModulePath;
    m_currentModulePath = modulePath;
    struct PathRestore {
        std::string& target;
        const std::string& saved;
        ~PathRestore() { target = saved; }
    } pathRestore{m_currentModulePath, previousModulePath};
    // And against the names that module imported: a `textColumn :> Atom ->
    // TextColumn` under `using Harmat.Schema, only: [TextColumn]` otherwise
    // answered a bare `TextColumn` that no `==` or `||` overload is
    // declared for, once the call was made from another module
    // (kexhq/kex#532).
    struct ImportsRestore {
        std::vector<ImportSelection>& target;
        std::vector<ImportSelection> saved;
        ~ImportsRestore() { target = std::move(saved); }
    } importsRestore{m_declarationImports, m_declarationImports};
    if (auto imports = m_makeImports.find(&def); imports != m_makeImports.end())
        m_declarationImports = imports->second;
    std::unordered_map<std::string, TypePtr> targetVars;
    // Where the target's own variables (`X` in `make List<X>`) start; see
    // the generalizeSignature call below.
    const int targetVarMark = m_nextTypeVar;
    auto receiver = resolveTypeExpr(*def.target, targetVars);
    // Every method this block defines, keyed by the RECEIVER it answers on —
    // needed before the body is checked, when the method's own signature is
    // not registered yet. A sibling defined lower in the block (a `private
    // do` helper especially) is then missing from the call-site candidate
    // list, and a same-named import's mismatch read as this call's error
    // (kexhq/kex#292).
    {
        auto record = [&](const ast::FunctionDef& fn) {
            auto& owners = m_makeMethodReceivers[fn.name];
            auto resolvedReceiver = resolve(receiver);
            if (std::none_of(owners.begin(), owners.end(),
                             [&](const TypePtr& existing) {
                                 return typesEqual(existing, resolvedReceiver);
                             }))
                owners.push_back(std::move(resolvedReceiver));
        };
        for (const auto& item : def.body) {
            if (const auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn) record(**fn);
            } else if (const auto* visibility =
                           std::get_if<std::unique_ptr<ast::VisibilityBlock>>(
                               &item)) {
                if (!*visibility) continue;
                for (const auto& inner : (*visibility)->items)
                    if (const auto* vfn =
                            std::get_if<std::unique_ptr<ast::FunctionDef>>(
                                &inner))
                        if (*vfn) record(**vfn);
            }
        }
    }
    // Record which of these methods a `private do ... end` block hides, now
    // that the receiver type has a name to key them by.
    if (auto* named = std::get_if<NamedType>(&receiver->kind))
        for (const auto& item : def.body)
            if (const auto* visibility =
                    std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                visibility && *visibility && !(*visibility)->isPublic)
                for (const auto& inner : (*visibility)->items)
                    if (const auto* vfn =
                            std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner);
                        vfn && *vfn)
                        m_privateMakeMethods.emplace(
                            named->name + "#" + (*vfn)->name, modulePath);
    std::unordered_set<std::string> slotNames;
    if (def.isServing)
        for (const auto& item : def.body)
            if (const auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
                fn && *fn && (*fn)->isSlot)
                slotNames.insert((*fn)->name);
    m_slotMethodNames.insert(slotNames.begin(), slotNames.end());

    auto remoteSlotSignature = [&](const Signature& handler,
                                   bool hasFrom) -> Signature {
        Signature remote = handler;
        remote.params.front() = Type::named("Server", {receiver});
        if (hasFrom) {
            TypePtr reply = Type::unknown();
            if (auto* named = std::get_if<NamedType>(&handler.result->kind);
                named && named->name == "Reply" && named->typeArgs.size() == 1)
                reply = named->typeArgs.front();
            remote.result = Type::named(
                "Result", {reply, Type::named("CallError")});
            remote.isFoul = true;
        } else {
            remote.result = Type::unit();
            remote.isFoul = true;
        }
        return remote;
    };

    for (const auto& item : def.body) {
        auto add = [&](const std::unique_ptr<ast::TypeAnnotation>& ann) {
            if (!ann) return;
            // `This` in `+ :> This -> This` is the receiver, as it is in a
            // `let` signature (see preRegister): resolved without the make
            // block entered, it stayed the literal placeholder, and
            // `a + b` reported "expects argument 2 to be This, but got Rv".
            const bool wasInMakeBlock = m_inMakeBlock;
            const auto previousMakeType = m_currentMakeType;
            m_inMakeBlock = true;
            m_currentMakeType = receiver;
            auto sig = annotationToSignature(*ann, &targetVars);
            m_inMakeBlock = wasInMakeBlock;
            m_currentMakeType = previousMakeType;
            if (!sig) return;
            const bool replyResult = [&] {
                auto result = resolve(sig->result);
                auto* named = std::get_if<NamedType>(&result->kind);
                return named && named->name == "Reply" &&
                       named->typeArgs.size() == 1;
            }();
            if (def.isServing && slotNames.count(ann->name) &&
                ann->implicitFrom && !replyResult)
                error(ann->location,
                      "`::>` is not valid on a cast slot — casts have no `from`; use `:>`");
            sig->params.insert(sig->params.begin(), receiver);
            // The target's `X` is one inference variable for the whole block.
            // Left as it is, every `[X]` signature shared it: a stdlib checked
            // from source bound it to String at `pieces.get(1, "")`, and
            // `this.args.first` then answered String in another file. The
            // prelude interface stores these as per-call placeholders, and
            // this makes the source agree.
            generalizeSignature(*sig, targetVarMark);
            m_annotatedMethods.insert(ann->name);
            m_annotatedReceiverKeys[ann->name].insert(annotatedReceiverKey(receiver));
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
                        [&](const Signature& other) { return sameParameters(other, *sig); });
            sig->makeModule = modulePath;
            if (!duplicate) existing.push_back(*sig);
            if (def.isServing && slotNames.count(ann->name)) {
                auto remote = remoteSlotSignature(*sig, replyResult);
                const bool remoteDuplicate = std::any_of(existing.begin(), existing.end(),
                        [&](const Signature& other) { return sameParameters(other, remote); });
                if (!remoteDuplicate) existing.push_back(std::move(remote));
            }
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

    // A method written as a plain `let` — no `:>` — was registered only when
    // its make block was CHECKED, which is after every top-level function body.
    // So `let grow(c: Crate) = c.doubled` found no `doubled` at all and typed
    // as unknown, while the identical call inside `main` (checked last) worked.
    // Registering the declared shape here makes the method visible to whatever
    // is checked first; the body still refines it later.
    const auto preRegister = [&](const ast::FunctionDef& method,
                                 const ast::FunctionClause& clause,
                                 bool inherited = false) {
        // Only THIS type's own `:>` contract for `method.name` should defer
        // to it — an unrelated type's same-named annotation (Char's
        // `string :> String` vs. Input's own plain `let string(...)`) must
        // not block this type's method from registering at all (#251).
        if (hasAnnotatedSignatureForReceiver(method.name, receiver)) return;
        // After an explicit receiver if the method matches one (`let
        // factor(@Px)` in `make CssUnit`): that first parameter IS the receiver,
        // so the arity gains none. A parameter matched by a literal, `_` or a
        // name (`let productKind(:mass, :acceleration)`) takes any value, typed
        // by the body like an untyped name; a constructor or structure pattern,
        // or a type-selector argument (`let to(String)`), is not a parameter
        // type at all, and the method registers when its body is checked.
        const bool explicitReceiver = !clause.params.empty() &&
            clause.params.front().pattern &&
            std::holds_alternative<ast::ThisPattern>((*clause.params.front().pattern)->kind);
        const auto takesAnyValue = [](const ast::Param& param) {
            if (!param.pattern) return param.name.has_value();
            const auto& kind = (*param.pattern)->kind;
            return std::holds_alternative<ast::LiteralPattern>(kind) ||
                   std::holds_alternative<ast::WildcardPattern>(kind) ||
                   std::holds_alternative<ast::VarPattern>(kind);
        };
        for (std::size_t i = explicitReceiver ? 1 : 0; i < clause.params.size(); ++i)
            if (!takesAnyValue(clause.params[i])) return;
        Signature signature;
        signature.name = method.name;
        signature.isFoul = method.isFoul;
        signature.params.push_back(receiver);
        // A param or return type written as `This` needs `m_currentMakeType`
        // set to resolve to `receiver` — `resolveTypeExpr`'s only path for
        // it — but this pre-pass runs before any make block is "entered" for
        // checking, so neither was set here otherwise. Without this, a
        // method's `This` self-registered as the literal, unsubstituted
        // placeholder rather than `receiver`: harmless for a type with its
        // own separate `:>` contract (nothing here gets consulted for it),
        // but `m_annotatedMethods` is name-only, not receiver-scoped, so a
        // method sharing a NAME with some UNRELATED type's `:>`-annotated
        // one (`Router`'s inline-only `+`, next to `Stack`/`Queue`/`Set`'s
        // own separately-declared `+`) still matched itself back out of
        // this very table later, comparing its real inline `-> This`
        // (resolved correctly, in the real per-clause check) against this
        // stale, self-registered "This" — reporting the two as disagreeing
        // (kexhq/rodolfo's docs/kex-issues.md #27, follow-up: reachable
        // simply by `using` any stdlib module that pulls in a same-named
        // `:>`-annotated method, `URI`'s `query` next to Rodolfo's own
        // `Context.query` being the original trigger).
        const bool wasInMakeBlock = m_inMakeBlock;
        const auto previousMakeType = m_currentMakeType;
        m_inMakeBlock = true;
        m_currentMakeType = receiver;
        for (std::size_t i = explicitReceiver ? 1 : 0; i < clause.params.size(); ++i)
            signature.params.push_back(
                clause.params[i].type ? resolveTypeExpr(**clause.params[i].type, targetVars)
                                      : freshTypeVar());
        if (clause.returnAnnotation) {
            signature.result = resolveTypeExpr(**clause.returnAnnotation, targetVars);
        } else {
            // Left for the body to decide (m_pendingResults).
            signature.result = freshTypeVar();
            const int id = std::get<TypeVar>(signature.result->kind).id;
            if (inherited)
                m_inheritedPendingResults.emplace(&clause, std::pair{receiver, id});
            else
                m_pendingResults[&clause] = id;
            m_pendingResultVars.insert(id);
        }
        m_inMakeBlock = wasInMakeBlock;
        m_currentMakeType = previousMakeType;
        // Without the names a call site cannot place named arguments, and
        // lowering refused `router.launchOn(port: 4000)` for a method this
        // pass — not checkFunctionDef — registered.
        for (std::size_t i = explicitReceiver ? 1 : 0; i < clause.params.size(); ++i)
            signature.paramNames.push_back(clause.params[i].name.value_or(""));
        signature.clause = &clause;
        // Trailing defaults are optional from the start, not only once the
        // body has been checked: a top-level function calling
        // `t.string(:name, nullable: true)` is checked before the make block.
        std::size_t required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue)
            required--;
        signature.requiredParams = required + (explicitReceiver ? 0 : 1);
        signature.makeModule = modulePath;
        generalizeTargetVariables(signature, receiver);
        auto& existing = m_methodSignatures[method.name];
        const bool duplicate =
            std::any_of(existing.begin(), existing.end(),
                        [&](const Signature& other) { return sameMethod(other, signature); });
        if (!duplicate) existing.push_back(signature);
        if (def.isServing && method.isSlot) {
            bool hasFrom = false;
            if (auto* named = std::get_if<NamedType>(&signature.result->kind))
                hasFrom = named->name == "Reply" &&
                          named->typeArgs.size() == 1;
            auto remote = remoteSlotSignature(signature, hasFrom);
            const bool remoteDuplicate = std::any_of(existing.begin(), existing.end(),
                        [&](const Signature& other) { return sameParameters(other, remote); });
            if (!remoteDuplicate) existing.push_back(std::move(remote));
        }
    };
    // What the block defines itself, by name and arity: a trait default it
    // would otherwise inherit stays the trait's (see below).
    std::set<std::string> overridden;
    for (const auto& item : def.body) {
        // One registration per signature the method declares: clauses folded
        // into one method may each type their parameters differently.
        const auto registerEach = [&](const ast::FunctionDef& function) {
            if (!function.clauses.empty())
                overridden.insert(function.name + "/" +
                                  std::to_string(function.clauses[0].params.size()));
            for (const auto* clause : ast::signatureClauses(function))
                preRegister(function, *clause);
        };
        if (const auto* function =
                std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*function) registerEach(**function);
        } else if (const auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items)
                if (const auto* function =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible))
                    if (*function) registerEach(**function);
        }
    }
    // A type implementing a trait has the trait's defaults as its own methods
    // (`make Coin, implement: Addable` gives Coin `twice`), unless the block
    // defines that name at that arity itself: registered for this receiver
    // like the block's own, so a call checked before either body finds it.
    for (const auto& written : def.implements) {
        const auto defaults =
            m_traitDefaults.find(resolveTraitReference(written, modulePath, nullptr));
        if (defaults == m_traitDefaults.end()) continue;
        for (const auto* inherited : defaults->second) {
            if (inherited->clauses.empty() ||
                overridden.contains(inherited->name + "/" +
                                    std::to_string(inherited->clauses[0].params.size())))
                continue;
            for (const auto* clause : ast::signatureClauses(*inherited))
                preRegister(*inherited, *clause, true);
        }
    }
}

auto TypeChecker::registerMakeSignaturesInModule(const ast::ModuleDef& mod,
                                                const std::string& parentPath)
    -> void {
    const auto path = ast::nestedModulePath(parentPath, mod.name);
    for (const auto& item : mod.body) {
        std::visit([this, &path](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) registerMakeSignature(*node, path);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerMakeSignaturesInModule(*node, path);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                // A `make` inside `private do ... end` is the module's too.
                if (node)
                    for (const auto& visible : node->items)
                        if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&visible);
                            make && *make)
                            registerMakeSignature(**make, path);
            }
        }, item);
    }
}

auto TypeChecker::registerMakeSignatures(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) registerMakeSignature(*node, "");
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerMakeSignaturesInModule(*node, "");
            }
        }, item);
    }
}

auto TypeChecker::registerExportsInModule(const ast::ModuleDef& mod,
                                          const std::string& parentPath) -> void {
    const auto path = ast::nestedModulePath(parentPath, mod.name);
    for (const auto& item : mod.body) {
        std::visit([this, &path](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ExportDecl>>) {
                if (!node) return;
                std::string target = node->module.dotted();
                const auto alias =
                    node->alias.value_or(node->module.parts.back());
                m_moduleReexports[path + "." + alias] = target;
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerExportsInModule(*node, path);
            }
        }, item);
    }
}

auto TypeChecker::registerExports(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerExportsInModule(*node, "");
            }
        }, item);
    }
}

auto TypeChecker::preRegisterFunctionSigs(const ast::Program& program) -> void {
    std::function<void(const ast::ModuleDef&)> registerModule;
    registerModule = [&](const ast::ModuleDef& module) {
        const auto previousModule = m_currentModulePath;
        m_currentModulePath = previousModule.empty() ||
                                      module.name.rfind(previousModule + ".", 0) == 0
            ? module.name
            : previousModule + "." + module.name;
        for (const auto& item : module.body) {
            std::visit([&](const auto& member) {
                using M = std::decay_t<decltype(member)>;
                if constexpr (std::is_same_v<
                                  M, std::unique_ptr<ast::FunctionDef>>) {
                    if (member) preRegisterFunctionDef(*member);
                } else if constexpr (std::is_same_v<
                                         M, std::unique_ptr<ast::ModuleDef>>) {
                    if (member) registerModule(*member);
                } else if constexpr (std::is_same_v<
                                         M, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (!member) return;
                    for (const auto& visible : member->items)
                        if (auto* def = std::get_if<
                                std::unique_ptr<ast::FunctionDef>>(&visible);
                            def && *def) {
                            if (!member->isPublic)
                                m_privateFreeFunctions.insert(
                                    m_currentModulePath + "\n" + (*def)->name);
                            preRegisterFunctionDef(**def);
                        }
                }
            }, item);
        }
        m_currentModulePath = previousModule;
    };
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) preRegisterFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerModule(*node);
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
    if (m_annotationDeclared.count(def.name)) {
        // An annotation supplies types, but only the definition supplies
        // parameter labels and defaults. Publish that shape before checking
        // any caller (including a main block preceding an imported module).
        const auto enrich = [&](std::vector<Signature>& signatures) {
            for (auto& signature : signatures)
                for (const auto& clause : def.clauses) {
                    if (signature.params.size() != clause.params.size()) continue;
                    signature.paramNames.clear();
                    for (const auto& param : clause.params)
                        signature.paramNames.push_back(param.name.value_or(""));
                    auto required = clause.params.size();
                    while (required > 0 && clause.params[required - 1].defaultValue)
                        --required;
                    signature.requiredParams = required;
                    signature.clause = &clause;
                }
        };
        if (auto scoped = m_scopedDeclaredSignatures.find(
                m_currentModulePath + "\n" + def.name);
            scoped != m_scopedDeclaredSignatures.end()) {
            enrich(scoped->second);
            auto qualified = m_userSignatures.find(
                m_currentModulePath.empty() ? def.name
                                           : kex::mangle::qualify(m_currentModulePath, def.name));
            if (qualified != m_userSignatures.end()) enrich(qualified->second);
        }
        // `m_annotationDeclared` is keyed by the bare name, so an annotation in
        // ANOTHER module (`Digest`'s `sha256 : String -> String`, merged in from
        // source) also skipped this module's unannotated `sha256` — and with it
        // the qualified `Tool::sha256` a call above the definition needs
        // (#103). File that key here; the bare-name set stays untouched.
        const auto qualified = kex::mangle::qualify(m_currentModulePath, def.name);
        if (!m_currentModulePath.empty() &&
            !m_scopedDeclaredSignatures.count(m_currentModulePath + "\n" + def.name) &&
            !m_qualifiedPublished.contains(qualified)) {
            for (const auto& clause : def.clauses) {
                std::unordered_map<std::string, TypePtr> genericVars;
                std::vector<TypePtr> paramTypes;
                for (const auto& param : clause.params)
                    paramTypes.push_back(param.type
                        ? resolveTypeExpr(**param.type, genericVars)
                        : freshTypeVar());
                std::size_t required = clause.params.size();
                while (required > 0 && clause.params[required - 1].defaultValue) required--;
                Signature signature{def.name, std::move(paramTypes), freshTypeVar(), false, required};
                signature.clause = &clause;
                m_userSignatures[qualified].push_back(std::move(signature));
            }
        }
        return;
    }
    const auto previousImports = m_declarationImports;
    if (auto imports = m_functionImports.find(&def);
        imports != m_functionImports.end())
        m_declarationImports = imports->second;
    // A name may be pre-registered more than once: clauses of one definition
    // share a def.name, but so do separate definitions that overload by arity
    // (common for module functions, e.g. `let f(a)` and `let f(a, b)`).
    // Skipping the name outright would leave every arity but the first
    // invisible to calls checked before the definitions themselves are, so
    // only arities already present are skipped.
    const auto hasArity = [](const std::vector<Signature>* signatures, size_t arity) {
        return signatures &&
               std::any_of(signatures->begin(), signatures->end(),
                           [&](const Signature& signature) {
                               return signature.params.size() == arity;
                           });
    };
    const auto signaturesAt = [&](const std::string& key) -> std::vector<Signature>* {
        auto found = m_userSignatures.find(key);
        return found == m_userSignatures.end() ? nullptr : &found->second;
    };

    // The result a caller checked BEFORE this definition sees. A declared
    // `-> T` is known already; a fresh variable in its place is shared by
    // every such caller, so the first one to use the result decides its type
    // for the rest: `Shop.items.reduce("") do |text, item| ... end` above a
    // `foul items -> [Item]` bound it to a String and typed `item` as Char.
    const auto provisionalOf = [&](const ast::FunctionClause& clause) {
        std::unordered_map<std::string, TypePtr> genericVars;
        std::vector<TypePtr> paramTypes;
        for (const auto& param : clause.params)
            paramTypes.push_back(
                param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar());
        std::size_t required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue) required--;
        auto result = clause.returnAnnotation
            ? resolveTypeExpr(**clause.returnAnnotation, genericVars)
            : freshTypeVar();
        Signature signature{def.name, std::move(paramTypes), std::move(result), false, required};
        signature.declaringModule = m_currentModulePath;
        signature.clause = &clause;
        return signature;
    };
    // An arity already present is skipped: clauses of one definition share
    // it, and separate definitions overload by arity (`let f(a)` and
    // `let f(a, b)`). An annotated same-arity clause is kept beside an earlier
    // untyped one without mutating it: they may be genuine overloads, which
    // literal clauses consult narrowly while they are checked.
    // One signature per clause, shared by the bare and qualified entries, so
    // a caller of either binds the same provisional result.
    std::vector<std::optional<Signature>> made(def.clauses.size());
    const auto provisionalFor = [&](const std::vector<Signature>* existing) {
        std::vector<Signature> provisional;
        for (std::size_t i = 0; i < def.clauses.size(); i++) {
            const auto& clause = def.clauses[i];
            const bool annotated = std::any_of(
                clause.params.begin(), clause.params.end(),
                [](const auto& param) { return bool(param.type); });
            if (hasArity(existing, clause.params.size()) && !annotated) continue;
            if (!made[i]) made[i] = provisionalOf(clause);
            provisional.push_back(*made[i]);
        }
        return provisional;
    };
    // A module function is reachable QUALIFIED before its definition is
    // checked. Without this, `Tey.Toolchain.sha256(path)` above the `sha256`
    // it names found no `Tey.Toolchain::sha256` yet, fell back to the bare
    // name, and resolved to `Digest.sha256` (#103). Not marked published, so
    // the definition's own check replaces these provisional signatures. Its
    // arities are its own: `Source.now` is not skipped because `Clock.now`
    // registered a bare `now` of the same arity first.
    if (!m_currentModulePath.empty()) {
        const auto qualified = kex::mangle::qualify(m_currentModulePath, def.name);
        if (!m_qualifiedPublished.contains(qualified))
            for (auto& signature : provisionalFor(signaturesAt(qualified)))
                m_userSignatures[qualified].push_back(std::move(signature));
    }
    for (auto& signature : provisionalFor(signaturesAt(def.name)))
        m_userSignatures[def.name].push_back(std::move(signature));
    m_declarationImports = previousImports;
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

auto TypeChecker::expandTypeAliases(const TypePtr& type, int depth) const
    -> TypePtr {
    if (!type || depth > 8) return type;
    auto recur = [&](const TypePtr& inner) {
        return expandTypeAliases(inner, depth + 1);
    };
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        std::vector<TypePtr> args;
        for (const auto& arg : named->typeArgs) args.push_back(recur(arg));
        const auto dot = named->name.rfind('.');
        const auto last = dot == std::string::npos ? named->name
                                                   : named->name.substr(dot + 1);
        // Distinct types are nominal: `distinct type Meters = Integer` must
        // NOT collapse to Integer, so mirror resolveTypeExpr's precedence.
        if (!m_distinctTypes.count(resolveDistinctName(last)) &&
            !m_recordFields.count(named->name)) {
            if (auto alias = m_typeAliases.find(last);
                alias != m_typeAliases.end())
                return expandTypeAliases(
                    args.empty() ? alias->second
                                 : substituteInterfaceGenerics(alias->second, args),
                    depth + 1);
        }
        if (args.empty()) return type;
        return Type::named(named->name, std::move(args));
    }
    if (auto* list = std::get_if<ListType>(&type->kind))
        return Type::list(recur(list->element));
    if (auto* optional = std::get_if<OptionalType>(&type->kind))
        return Type::optional(recur(optional->inner));
    if (auto* map = std::get_if<MapType>(&type->kind))
        return Type::map(recur(map->key), recur(map->value));
    if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
        std::vector<TypePtr> elements;
        for (const auto& element : tuple->elements)
            elements.push_back(recur(element));
        return Type::tuple(std::move(elements));
    }
    if (auto* fn = std::get_if<FuncType>(&type->kind)) {
        std::vector<TypePtr> params;
        for (const auto& param : fn->params) params.push_back(recur(param));
        return Type::func(std::move(params), recur(fn->result), fn->block);
    }
    if (auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
        std::vector<TypePtr> members;
        for (const auto& member : intersection->members)
            members.push_back(recur(member));
        return Type::intersection(std::move(members));
    }
    if (auto* record = std::get_if<RecordType>(&type->kind)) {
        std::vector<std::pair<std::string, TypePtr>> fields;
        for (const auto& [name, fieldType] : record->fields)
            fields.emplace_back(name, recur(fieldType));
        return Type::record(std::move(fields));
    }
    return type;
}

auto TypeChecker::normalizeIntersection(TypePtr type) const -> TypePtr {
    auto* meet = std::get_if<IntersectionType>(&type->kind);
    if (!meet) return type;

    TypePtr concreteRecord;
    for (const auto& member : meet->members) {
        auto* named = std::get_if<NamedType>(&member->kind);
        if (!named ||
            !m_recordFields.count(resolveRecordName(named->name)))
            continue;
        if (concreteRecord && !typesEqual(concreteRecord, member)) {
            const auto& left = std::get<NamedType>(concreteRecord->kind).name;
            const auto& right = named->name;
            return Type::voidType(
                "incompatible nominal record tags `" + left + "` and `" +
                right + "`");
        }
        concreteRecord = member;
    }
    if (!concreteRecord) return type;

    for (const auto& member : meet->members) {
        if (typesEqual(member, concreteRecord)) continue;
        if (!argMatchesParam(concreteRecord, member))
            return Type::voidType();
    }
    return concreteRecord;
}

auto TypeChecker::resolveTypeExpr(const ast::TypeExpr& typeExpr,
                                   std::unordered_map<std::string, TypePtr>& genericVars) -> TypePtr {
    auto resolved = resolveTypeExprNode(typeExpr, genericVars);
    // The record a written type name means here, for code that holds only
    // the annotation (lowering's run-time guards).
    if ((std::holds_alternative<ast::TypeName>(typeExpr.kind) ||
         std::holds_alternative<ast::GenericType>(typeExpr.kind)) && resolved)
        if (const auto* named = std::get_if<NamedType>(&resolve(resolved)->kind);
            named && m_recordFields.count(named->name))
            m_annotatedRecords[&typeExpr] = named->name;
    return resolved;
}

auto TypeChecker::resolveTypeExprNode(const ast::TypeExpr& typeExpr,
                                       std::unordered_map<std::string, TypePtr>& genericVars)
    -> TypePtr {
    return std::visit([this, &genericVars](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::TypeQuery>) {
            return resolveTypeQuery(node);
        }
        else if constexpr (std::is_same_v<T, ast::TypeName>) {
            if (node.parts.empty()) return Type::unknown();
            const std::string& last = node.parts.back();
            std::string qualified = node.dotted();
            // `This` inside a make block refers to the implementing type.
            if (last == names::kThisType && node.parts.size() == 1 && m_inMakeBlock && m_currentMakeType)
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
            if (node.parts.size() == 1) {
                const auto recordName = resolveRecordName(last);
                if (recordName != last) return Type::named(recordName);
            } else {
                recordWithOwner(m_referencedModules, qualified);
                if (auto nominal = qualifiedNominalType(qualified))
                    return Type::named(*nominal);
            }
            if (auto known = m_globals.get(last)) return known;
            // Trait-only names (Float, Number, Comparable, ...) have no
            // concrete Type — m_globals deliberately has no entry for them
            // (see check()'s comment) — so a param annotated `Float` means
            // "any T satisfying Float", same as an explicit constraint.
            if (const auto trait = resolveTraitReference(
                    qualified, m_currentModulePath, nullptr, false);
                m_traits.get(trait)) {
                // What the spelling means here, for code that sees only the
                // spelling (the run-time dispatchers' guards); the numeric
                // constraints are the language's own types.
                if (!isPrimitiveTypeName(trait))
                    m_writtenTraits[writtenKey(m_currentModulePath, qualified)] = trait;
                return Type::constrained(trait, trait);
            }
            if (auto distinct = resolveDistinctName(last);
                m_distinctTypes.count(distinct))
                return Type::named(distinct);
            // User type alias (e.g. `type Level = :debug | :info | ...`)
            auto aliasIt = m_typeAliases.find(last);
            if (aliasIt != m_typeAliases.end()) return aliasIt->second;
            // Bare `List` is the erased form of `[X]`, the way `type
            // List<X> = [X]` declares it. Left nominal, a `to(List)` result
            // matched no `[A]` receiver and every list method on it reported
            // whatever unrelated overload sorted first (`zip` blamed Range).
            if (last == kex::names::kListType && node.parts.size() == 1)
                return Type::list(Type::unknown());
            if (node.parts.size() > 1) {
                const bool isNominal = m_recordFields.count(qualified) ||
                    m_adtVariants.count(qualified) ||
                    (m_importedInterfaces &&
                     m_importedInterfaces->recordArities.count(qualified));
                return Type::named(isNominal ? qualified : last);
            }
            return Type::named(resolveRecordName(last));
        }
        else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::vector<TypePtr> args;
            for (const auto& arg : node.args) {
                args.push_back(arg ? resolveTypeExpr(*arg, genericVars) : Type::unknown());
            }
            std::string name = node.name.parts.empty() ? "" : node.name.parts.back();
            // A qualified name means the type it spells and no other:
            // `Net.HTTP.Request<Binary>` written inside a module declaring
            // its own `Request` resolved to that local record by its last
            // segment alone (kexhq/kex#476).
            if (node.name.parts.size() > 1) {
                std::string qualified = node.name.dotted();
                if (auto nominal = qualifiedNominalType(qualified))
                    return Type::named(*nominal, std::move(args));
            }
            if (name == "Optional" && args.size() == 1)
                return Type::optional(args[0]);
            if (name == kex::names::kMapType && args.size() == 2)
                return Type::map(args[0], args[1]);
            // `List<T>` is `[T]`, matching what the prelude-interface and
            // KexI readers already build for the same spelling.
            if (name == kex::names::kListType && args.size() == 1)
                return Type::list(args[0]);
            if (auto distinct = resolveDistinctName(name);
                m_distinctTypes.count(distinct))
                return Type::named(distinct, std::move(args));
            // An applied generic alias is its body with the arguments put in:
            // `Predicate<E>` for `type Predicate<E> = E -> Bool` is `E -> Bool`.
            // Left as a named type, no lambda ever matched it (#337).
            if (auto alias = m_typeAliases.find(name);
                alias != m_typeAliases.end() &&
                !m_recordFields.count(resolveRecordName(name)))
                return substituteInterfaceGenerics(alias->second, args);
            if (node.name.parts.size() > 1) {
                std::string qualified = node.name.dotted();
                if (m_adtVariants.count(qualified))
                    return Type::named(qualified, std::move(args));
            }
            return Type::named(resolveRecordName(name), std::move(args));
        }
        else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            // Same ambiguity as annotationToSignature: `A -> B -> C` and
            // `A -> (B -> C)` build the identical right-nested FunctionType,
            // so once inside this chain, a parenthesized result marks a
            // genuine function-typed RESULT rather than another curried
            // parameter, and stops the unroll.
            std::vector<TypePtr> params;
            params.push_back(node.param
                ? resolveTypeExpr(*node.param, genericVars)
                : Type::unknown());
            const ast::TypeExpr* resultExpr = node.result.get();
            while (resultExpr && !resultExpr->parenthesized) {
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
        else if constexpr (std::is_same_v<T, ast::BlockType>) {
            std::vector<TypePtr> params;
            for (const auto& param : node.params)
                params.push_back(param ? resolveTypeExpr(*param, genericVars)
                                       : Type::unknown());
            auto result = node.inner
                ? resolveTypeExpr(*node.inner, genericVars)
                : Type::unknown();
            return Type::func(std::move(params), std::move(result), true);
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
        else if constexpr (std::is_same_v<T, ast::IntersectionType>) {
            std::vector<TypePtr> members;
            members.push_back(node.left
                ? resolveTypeExpr(*node.left, genericVars) : Type::unknown());
            members.push_back(node.right
                ? resolveTypeExpr(*node.right, genericVars) : Type::unknown());
            return normalizeIntersection(
                Type::intersection(std::move(members)));
        }
        else if constexpr (std::is_same_v<T, ast::RecordType>) {
            std::vector<std::pair<std::string, TypePtr>> fields;
            for (const auto& [name, fieldType] : node.fields)
                fields.emplace_back(
                    name, fieldType
                        ? resolveTypeExpr(*fieldType, genericVars)
                        : Type::unknown());
            return Type::record(std::move(fields));
        }
        else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return Type::atom(node.name);
        }
        else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            auto it = genericVars.find(node.name);
            if (it != genericVars.end()) return it->second;
            auto var = freshTypeVar();
            genericVars[node.name] = var;
            return var;
        }
        else {
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

    const auto adt = adtNameOfType(expected);
    if (adt.empty()) {
        // A value whose type is settled and has no constructors at all — a
        // Bool, a number, a list, a record — can't match an ADT's
        // constructor. `match FS.File.writeBytes(...)` (a Bool) with `Ok`
        // and `Error` arms compiled and died with "no matching clause".
        // Anything still open (a type variable, a union, a trait, unknown)
        // is left alone.
        const auto owner = m_adtOfConstructor.find(ctorName);
        if (owner == m_adtOfConstructor.end() || !expected) return;
        const auto& kind = expected->kind;
        const auto* named = std::get_if<NamedType>(&kind);
        const bool closed =
            std::holds_alternative<PrimitiveType>(kind) ||
            std::holds_alternative<SizedIntType>(kind) ||
            std::holds_alternative<SizedFloatType>(kind) ||
            std::holds_alternative<FuncType>(kind) ||
            std::holds_alternative<TupleType>(kind) ||
            std::holds_alternative<ListType>(kind) ||
            std::holds_alternative<MapType>(kind) ||
            std::holds_alternative<RecordType>(kind) ||
            (named && named->typeArgs.empty() && m_recordFields.count(named->name));
        if (!closed) return;
        error(pattern.location,
              "`" + ctorName + "` is a constructor of `" + owner->second +
                  "`, but the value being matched has type `" +
                  typeToString(expected) + "` — this pattern can never match");
        return;
    }

    // The matched ADT declaring the constructor settles it. The bare-name
    // owner table cannot: `WorkQueue.Pending`'s own `Done` shares its
    // spelling with Control.Retry's, which owns that entry.
    if (const auto variants = m_adtVariants.find(adt);
        variants != m_adtVariants.end() &&
        std::find(variants->second.begin(), variants->second.end(),
                  ctorName) != variants->second.end())
        return;
    auto owner = m_adtOfConstructor.find(ctorName);
    if (owner == m_adtOfConstructor.end()) {
        // A name no type declares at all can't be one of the matched ADT's
        // variants, whose list is complete — `Some(p)` against an `X?`
        // otherwise compiled and failed at runtime with "no matching
        // clause". A qualified name may resolve elsewhere; leave it be.
        if (ctorName.find('.') != std::string::npos) return;
        const auto& variants = m_adtVariants.at(adt);
        std::string known;
        for (const auto& variant : variants)
            known += (known.empty() ? "`" : ", `") + variant + "`";
        error(pattern.location,
              "`" + ctorName + "` is not a constructor of `" + adt +
                  "` (it has " + known + ") — this pattern can never match");
        return;
    }
    if (owner->second == adt) return;

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
                const auto recordName = resolveRecordName(node.typeName);
                auto local = m_recordFields.find(recordName);
                bool known = local != m_recordFields.end();
                if (!known && m_importedInterfaces &&
                    m_importedInterfaces->recordArities.count(recordName))
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
    std::visit([this, &expected, &pat](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::VarPattern>) {
            if (node.name != "_") {
                auto bound = expected ? expected : freshTypeVar();
                defineVar(node.name, bound);
                // Kept so tooling can answer for the binding itself. Types are
                // resolved once at the end of check(), because a fresh
                // variable here may only learn what it is from a later use.
                m_patternBindings.push_back({node.name, pat.location, bound});
            }
        }
        else if constexpr (std::is_same_v<T, ast::TypePattern>) {
            // The alternative's own type is what the binding has inside the
            // arm — that is the whole point of naming it.
            if (!node.name.empty() && node.name != "_") {
                std::unordered_map<std::string, TypePtr> noGenerics;
                auto bound = node.type
                    ? resolveTypeExpr(*node.type, noGenerics)
                    : freshTypeVar();
                defineVar(node.name, bound);
                m_patternBindings.push_back({node.name, pat.location, bound});
            }
        }
        else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) bindPatternVars(*node.inner, expected);
        }
        else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            // Give each payload binding the type the constructor declares, the
            // way a record pattern below takes field types from the record.
            // Without it a binding had no type until some later use forced one,
            // so `Boxed(b) -> b.size` worked only because `.size` constrained
            // `b`, and the editor could say nothing about `b` itself.
            const ConstructorResult* declaration = nullptr;
            if (auto found = m_constructorResult.find(node.name);
                found != m_constructorResult.end())
                declaration = &found->second;
            // A constructor declared inside a module is registered as
            // `Module::Name`. Find it the way a call does — through the
            // enclosing modules — and otherwise by the ADT being matched on,
            // which is how an imported one (`using Model`) is reached. Left
            // unfound, the payload stayed untyped, and `entry.fields` on a
            // record held in it resolved to whichever `fields` METHOD existed
            // rather than the record's own field.
            for (auto module = m_currentModulePath;
                 !declaration && !module.empty();) {
                if (auto found = m_constructorResult.find(kex::mangle::qualify(module, node.name));
                    found != m_constructorResult.end()) {
                    declaration = &found->second;
                    break;
                }
                const auto dot = module.rfind('.');
                if (dot == std::string::npos) break;
                module.resize(dot);
            }
            if (!declaration && expected)
                if (const auto* scrutinee =
                        std::get_if<NamedType>(&resolve(expected)->kind)) {
                    const auto suffix = std::string(kex::mangle::kSeparator) + node.name;
                    for (const auto& [key, candidate] : m_constructorResult)
                        if (key.size() > suffix.size() && key.ends_with(suffix) &&
                            namedTypesMatch(candidate.adtName, scrutinee->name)) {
                            declaration = &candidate;
                            break;
                        }
                }
            for (size_t i = 0; i < node.args.size(); ++i) {
                if (!node.args[i]) continue;
                TypePtr payload;
                const int slot = declaration && i < declaration->slots.size()
                    ? declaration->slots[i] : -1;
                if (slot >= 0 && expected) {
                    // A payload that IS a type parameter takes its type from
                    // the scrutinee. The scrutinee's shape varies: `Result<A,
                    // B>` carries type arguments, but `A?` is an OptionalType
                    // and `[A]` a ListType, neither of which has any. The
                    // declared parameter name must NOT be used as a fallback
                    // here — doing that bound `Just(c)`'s `c` to the literal
                    // `A` and broke the prelude (`parsing.kex` charWhen).
                    const auto scrutinee = resolve(expected);
                    const auto index = static_cast<size_t>(slot);
                    if (const auto* named = std::get_if<NamedType>(&scrutinee->kind);
                        named && index < named->typeArgs.size())
                        payload = named->typeArgs[index];
                    else if (const auto* optional =
                                 std::get_if<OptionalType>(&scrutinee->kind);
                             optional && index == 0)
                        payload = optional->inner;
                    else if (const auto* list =
                                 std::get_if<ListType>(&scrutinee->kind);
                             list && index == 0)
                        payload = list->element;
                } else if (slot < 0 && declaration &&
                           i < declaration->payloadTypes.size()) {
                    // Only a CONCRETE declared payload. A type-parameter
                    // payload's declared type is the placeholder the ADT was
                    // registered with — one variable for the whole program —
                    // so reading it bound every such pattern to whatever some
                    // unrelated use had unified it with: `let Just(i) = ...`
                    // came out `Char` in the merged prelude (kexhq/kex#249).
                    payload = declaration->modulePath.empty()
                        ? declaration->payloadTypes[i]
                        : declaredPayload(*declaration, i);
                }
                // The built-in carriers come from the prelude interface, not a
                // local declaration, so they have no slot above. Left untyped,
                // `Just(item)` bound a fresh variable that unified with anything:
                // `wantsInteger(item)` on a `String?` passed, and `item.path`
                // latched onto an imported `path` function instead of the
                // record's field (#328).
                if (!payload && expected) {
                    const auto scrutinee = resolve(expected);
                    if (const auto* optional =
                            std::get_if<OptionalType>(&scrutinee->kind);
                        optional && node.name == "Just" && i == 0)
                        payload = optional->inner;
                    else if (const auto* named =
                                 std::get_if<NamedType>(&scrutinee->kind);
                             named && named->name == "Result" &&
                             named->typeArgs.size() == 2 && i == 0 &&
                             (node.name == "Ok" || node.name == "Error"))
                        payload = named->typeArgs[node.name == "Ok" ? 0 : 1];
                }
                bindPatternVars(*node.args[i], payload);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            const std::unordered_map<std::string, TypePtr>* fields = nullptr;
            // A named record pattern (`Foo { x }`) pins the record type itself,
            // so resolve field types from the declared type regardless of what
            // the scrutinee was inferred to be. Fall back to `expected` for the
            // anonymous `{ x }` form.
            if (!node.typeName.empty()) {
                const auto record = resolveRecordName(node.typeName);
                m_recordPatterns[&node] = record;
                if (auto found = m_recordFields.find(record); found != m_recordFields.end())
                    fields = &found->second;
            } else if (expected)
                if (auto* named = std::get_if<NamedType>(&expected->kind))
                    if (auto found = m_recordFields.find(
                            resolveRecordName(named->name));
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

auto TypeChecker::bindDestructuredPattern(
    const ast::Pattern& pat, const TypePtr& valueType) -> void {
    checkPatternArity(pat);
    // Reject constructor mismatches early, e.g.
    //   let Ok(v) = parsePrefix(...)   when parsePrefix returns Optional
    //   let Just(v) = parse(...)       when parse returns Result
    bool mismatchReported = false;
    if (auto* cp = std::get_if<ast::ConstructorPattern>(&pat.kind)) {
        auto resolved = resolve(valueType);
        if (isUndetermined(resolved))
            ; // permissive — can't determine the type at compile time
        else if (cp->name == "Just" && !std::holds_alternative<OptionalType>(resolved->kind)) {
            error(pat.location, "cannot match `Just` — expected Optional, got " + typeToString(resolved));
            mismatchReported = true;
        }
        else if (cp->name == "Ok" || cp->name == "Error") {
            if (auto* nt = std::get_if<NamedType>(&resolved->kind)) {
                if (nt->name != "Result") {
                    error(pat.location, "cannot match `" + cp->name + "` — expected Result, got " + typeToString(resolved));
                    mismatchReported = true;
                }
            } else {
                error(pat.location, "cannot match `" + cp->name + "` — expected Result, got " + typeToString(resolved));
                mismatchReported = true;
            }
        }
    }
    // With the value's type, as a `match` arm gets its subject's:
    // bound without it, `let Just(i) = Just(2)` typed `i` from
    // the constructor's declared payload instead (kexhq/kex#249).
    // Not after a mismatch already reported above, which the
    // owner check inside would only report a second time.
    bindPatternVars(pat, mismatchReported ? nullptr : valueType);
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
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
            if (node) checkTraitDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
            checkMainBlock(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
            checkUsingBlock(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
            checkRecordDef(*node);
        }
    }, item);
}

auto TypeChecker::checkModule(const ast::ModuleDef& mod) -> void {
    auto previousModulePath = m_currentModulePath;
    m_currentModulePath = previousModulePath.empty() ||
                                  mod.name.rfind(previousModulePath + ".", 0) == 0
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
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                if (node) checkTraitDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                checkModule(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                checkUsingBlock(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                checkRecordDef(*node);
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
                        } else if constexpr (std::is_same_v<
                                                 M, std::unique_ptr<ast::RecordDef>>) {
                            if (member) checkRecordDef(*member);
                        }
                    }, visible);
                }
            }
        }, item);
    }
    popScope();
    m_currentModulePath = std::move(previousModulePath);
}

auto TypeChecker::checkRecordDef(const ast::RecordDef& def) -> void {
    std::unordered_map<std::string, TypePtr> generics;
    for (const auto& name : def.typeParams)
        generics.emplace(name, freshTypeVar());
    for (const auto& field : def.fields) {
        if (!field.type || !field.defaultValue || !*field.defaultValue) continue;
        auto declared = resolveTypeExpr(*field.type, generics);
        auto actual = resolve(inferExpr(**field.defaultValue));
        if (!std::holds_alternative<UnknownType>(actual->kind) &&
            !std::holds_alternative<TypeVar>(actual->kind) &&
            !argMatchesParam(actual, declared) &&
            !literalCoercesTo(**field.defaultValue, declared))
            typeMismatch((*field.defaultValue)->location, declared, actual);
    }
}

auto TypeChecker::checkUsingBlock(const ast::UsingBlock& block) -> void {
    ImportSelection selection;
    selection.module += block.module.dotted();
    selection.onlyNames = block.onlyNames;
    selection.exceptNames = block.exceptNames;
    selection.alias = block.alias;
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
    // The declaration a `make` method is made for.
    if (m_currentMakeType) {
        const auto target = resolve(m_currentMakeType);
        const auto* named = std::get_if<NamedType>(&target->kind);
        m_makeTargets[&def] = named ? named->name : typeToString(target);
    }
    // A method of a `make` for a trait takes its receiver's dictionary.
    if (m_currentMakeType)
        if (auto trait = declaredTraitOf(m_currentMakeType); !trait.empty())
            m_makeTargetTraits[&def] = std::move(trait);
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_functionImports.find(&def);
        imports != m_functionImports.end())
        m_declarationImports = imports->second;
    // Purity is scoped to the RECEIVER as well as the module: two types'
    // methods are not overloads of each other, so `FileHandle`'s pure `read`
    // and a `make Mock.Files` block's `foul read` are unrelated definitions
    // that merely share a name (kexhq/kex#143).
    //
    // `declarationKey` stays module-scoped: it looks up standalone
    // annotations, which are declared at module level and matched by name.
    const auto declarationKey = m_currentModulePath + "\n" + def.name;
    const auto purityKey =
        m_currentModulePath + "\n" +
        (m_inMakeBlock && m_currentMakeType
             ? typeToString(m_currentMakeType) + "."
             : std::string()) +
        def.name;
    if (auto [purity, inserted] =
            m_overloadPurity.emplace(purityKey, def.isFoul);
        !inserted && purity->second != def.isFoul) {
        auto location = def.location;
        if (def.isFoul)
            location.column += 5; // point after `foul `, at the function name
        error(location,
              "Overloads of `" + def.name +
              "` must all have the same purity");
    }
    // The rule lowering's arity follows (ast::takesReceiverPattern): a literal
    // first parameter (`repeat(0)`) is an argument, not the receiver.
    const bool receiverIsFirstParam = m_inMakeBlock && !def.clauses.empty() &&
                                      ast::takesReceiverPattern(def.clauses[0]);
    // A "declared" signature is one from a standalone TypeAnnotation
    // (`fact : Integer -> Integer`) — tracked in m_annotationDeclared.
    // Pre-registered provisional sigs (for forward-reference/recursion) are
    // in m_userSignatures but NOT in m_annotationDeclared, so they don't
    // affect param-type selection or return-type verification here.
    const auto scopedDeclared = m_scopedDeclaredSignatures.find(declarationKey);
    // `declarationKey` is module-scoped only, not receiver-scoped — a
    // standalone annotation for a free fn (or another make block's method)
    // sharing this name would otherwise claim a make method that has no
    // relation to it at all, e.g. a free `tag : String -> String` leaking
    // onto `make String`'s own `tag` and tripping a spurious arity-mismatch
    // warning downstream (#105). A make-block method's contract comes only
    // from its own receiver-matched signature (below, `makeDeclared`), never
    // from this module-scoped, receiver-blind lookup.
    auto* declared = [&]() -> const Signature* {
        if (m_inMakeBlock) return nullptr;
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
    if (!def.clauses.empty() && def.clauses[0].returnAnnotation) {
        auto gvars = m_currentMakeTargetVars;
        inlineReturnType = resolveTypeExpr(**def.clauses[0].returnAnnotation, gvars);
        if (declared && !containsOpenType(declared->result) &&
            !containsOpenType(inlineReturnType) &&
            !typesEqual(resolve(declared->result), resolve(inlineReturnType)))
            error((*def.clauses[0].returnAnnotation)->location,
                  "inline return type for `" + def.name + "` disagrees with its separate annotation: " +
                  typeToString(resolve(inlineReturnType)) + " versus " +
                  typeToString(resolve(declared->result)));
    }

    auto returnType = declared    ? declared->result
                    : inlineReturnType ? inlineReturnType
                    : freshTypeVar();
    // The function name is deliberately NOT bound as a variable here. It used
    // to be defined as its own RETURN type, which made a function that
    // returns a function indistinguishable from that returned function: the
    // call-resolution path below looks a name up as a local FuncType binding
    // before consulting registered signatures, so `foul mk() -> (String ->
    // String)` resolved `mk()` against the CLOSURE's arity and reported
    // "`mk` expects 1 argument(s), got 0" (kexhq/kex#288). Calls resolve
    // through m_functionSignatures, so nothing needs the binding.

    // A concrete annotation on one clause is the public parameter contract
    // for sibling pattern clauses too (`factorial(0)` beside
    // `factorial(n: Int)`). A literal by itself is only a match condition,
    // however: numeric patterns deliberately match across the Integer/Float
    // tower, so `classify(0)` beside an unannotated catch-all must not narrow
    // the whole function to Integer.
    std::vector<TypePtr> siblingParamContracts;
    const auto namesItsOwnType = [](const ast::Param& param) {
        if (!param.pattern || !*param.pattern) return false;
        const auto& kind = (*param.pattern)->kind;
        if (const auto* record = std::get_if<ast::RecordPattern>(&kind))
            return !record->typeName.empty();
        return std::holds_alternative<ast::TypePattern>(kind);
    };
    for (const auto& clause : def.clauses) {
        if (siblingParamContracts.size() < clause.params.size())
            siblingParamContracts.resize(clause.params.size());
        auto clauseGenerics = m_currentMakeTargetVars;
        for (size_t pi = 0; pi < clause.params.size(); ++pi)
            if (!siblingParamContracts[pi] && clause.params[pi].type)
                siblingParamContracts[pi] = resolveTypeExpr(
                    **clause.params[pi].type, clauseGenerics);
    }
    if (auto provisional = m_userSignatures.find(def.name);
        provisional != m_userSignatures.end())
        for (const auto& signature : provisional->second) {
            if (signature.params.size() != siblingParamContracts.size())
                continue;
            for (size_t pi = 0; pi < signature.params.size(); ++pi)
                if (!siblingParamContracts[pi] &&
                    std::any_of(def.clauses.begin(), def.clauses.end(),
                        [&](const auto& clause) {
                            return pi < clause.params.size() &&
                                clause.params[pi].pattern &&
                                std::holds_alternative<ast::LiteralPattern>(
                                    (*clause.params[pi].pattern)->kind);
                        }) &&
                    !std::holds_alternative<TypeVar>(signature.params[pi]->kind) &&
                    !std::holds_alternative<UnknownType>(signature.params[pi]->kind))
                    siblingParamContracts[pi] = signature.params[pi];
        }

    // An overload set folded into one definition, a contract written above
    // each clause (`speak : Cat -> String` over `let speak(c)`, then
    // `speak : Dog -> String` over `let speak(d)`): clause i implements the
    // i-th, both in source order. Several contracts over ONE clause are one
    // permissive body's (`FS.File.open` by mode), so the counts must agree.
    std::vector<const Signature*> clauseContracts;
    if (!declared && !m_inMakeBlock && def.clauses.size() > 1 &&
        scopedDeclared != m_scopedDeclaredSignatures.end()) {
        const auto arity = def.clauses.front().params.size();
        for (const auto& signature : scopedDeclared->second)
            if (signature.params.size() == arity) clauseContracts.push_back(&signature);
        const bool oneArity = std::all_of(
            def.clauses.begin(), def.clauses.end(),
            [&](const auto& clause) { return clause.params.size() == arity; });
        if (!oneArity || clauseContracts.size() != def.clauses.size()) clauseContracts.clear();
    }

    std::vector<Signature> signatures;
    for (size_t ci = 0; ci < def.clauses.size(); ci++) {
        const auto& clause = def.clauses[ci];
        const Signature* contract = declared ? declared
            : ci < clauseContracts.size() ? clauseContracts[ci] : nullptr;
        // Every inference variable from here on belongs to THIS clause, and
        // is the clause's to quantify over once its body is checked. Anything
        // older is an enclosing inference's — see generalizeSignature.
        const int typeVarMark = m_nextTypeVar;
        // Published for checkCall, which needs the same "created by this
        // clause" test to decide whether calling a variable-typed binding may
        // constrain that variable to a function type. Restored on every exit
        // from the clause, including the nested definitions inside it.
        struct ClauseVarMark {
            int& slot;
            int previous;
            ~ClauseVarMark() { slot = previous; }
        } clauseVarMark{m_clauseVarMark, m_clauseVarMark};
        m_clauseVarMark = typeVarMark;
        pushScope();
        if (m_inMakeBlock && m_currentMakeType) {
            auto receiver = resolve(m_currentMakeType);
            if (auto* named = std::get_if<NamedType>(&receiver->kind);
                named && m_recordFields.count(resolveRecordName(named->name)))
                defineVar("new", m_currentMakeType);
        }
        const auto slotResult = resolve(returnType);
        const auto* slotReply = def.isSlot
            ? std::get_if<NamedType>(&slotResult->kind) : nullptr;
        const bool callSlot = slotReply && slotReply->name == "Reply" &&
                              slotReply->typeArgs.size() == 1;
        if (callSlot)
            defineVar("from", Type::named("From", {slotReply->typeArgs.front()}));
        const bool previousCastSlot = m_inCastSlot;
        m_inCastSlot = def.isSlot && !callSlot;
        auto genericVars = m_currentMakeTargetVars;
        std::vector<TypePtr> paramTypes;
        for (size_t pi = 0; pi < clause.params.size(); pi++) {
            const auto& param = clause.params[pi];
            // A bare type name takes that type as a value, whatever says what
            // the parameter's type is (`to : A -> Type<T> -> T?` for
            // `let to(value, String)`).
            if (typeSelectorParameter(param)) m_typeSelectorPatterns.insert(param.pattern->get());
            // Use declared param type if available and the annotation covers
            // this position; fall back to inline annotation or fresh TypeVar.
            TypePtr paramType;
            if (contract && pi < contract->params.size() && !param.type) {
                paramType = contract->params[pi];
            } else if (!param.type && !param.signatureType &&
                       !namesItsOwnType(param) &&
                       pi < siblingParamContracts.size() &&
                       siblingParamContracts[pi]) {
                // A parameter that already says what it takes belongs to that
                // clause, not to a sibling clause's annotation: `Query.from`
                // takes a list in one declaration and a Map in the next, and
                // `respond(JSON { status, body }, route)` a JSON beside a
                // `respond(reply: String, route)`.
                paramType = siblingParamContracts[pi];
            } else if (!param.type && pi == 0 && receiverIsFirstParam &&
                       m_currentMakeType && param.pattern &&
                       (std::holds_alternative<ast::RecordPattern>((*param.pattern)->kind) ||
                        std::holds_alternative<ast::ThisPattern>((*param.pattern)->kind))) {
                // A receiver destructured in place (`let distance({ x: x1,
                // y: y1 }, other)`), or matched as the explicit receiver
                // (`let factor(@Px)` in `make CssUnit`), IS the make target,
                // so its fields bind
                // with their declared types. A fresh variable left them to
                // arithmetic, which generalized `x1 * x1` to a bare `N` that
                // `Float.sqrt` then rejected.
                paramType = m_currentMakeType;
            } else if (param.type) {
                paramType = resolveTypeExpr(**param.type, genericVars);
            } else if (auto selector = typeSelectorParameter(param)) {
                paramType = std::move(selector);
            } else {
                paramType = freshTypeVar();
                if (auto* var = std::get_if<TypeVar>(&paramType->kind))
                    m_unannotatedParamVars.insert(var->id);
            }
            paramTypes.push_back(paramType);
            if (param.name.has_value() && *param.name != "_") {
                defineVar(*param.name, paramType);
            }
            if (param.pattern) {
                bindPatternVars(**param.pattern, paramType);
            }
            // A parameter declared as a trait is passed with its dictionary.
            if (param.type && *param.type &&
                std::holds_alternative<ast::TypeName>((*param.type)->kind))
                if (auto trait = declaredTraitOf(paramType); !trait.empty())
                    m_parameterTraits[&param] = std::move(trait);
            if (param.defaultValue && *param.defaultValue) {
                auto actual = resolve(inferExpr(**param.defaultValue));
                if (!std::holds_alternative<UnknownType>(actual->kind) &&
                    !std::holds_alternative<TypeVar>(actual->kind) &&
                    !argMatchesParam(actual, paramType) &&
                    !literalCoercesTo(**param.defaultValue, paramType))
                    typeMismatch((*param.defaultValue)->location, paramType,
                                 actual);
            }
        }
        if (declared && declared->params.size() != clause.params.size()) {
            bool hasMatchingAnnotation = false;
            auto it = m_annotationArities.find(declarationKey);
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
        if (clause.rescue) checkRescue(*clause.rescue);
        m_inCastSlot = previousCastSlot;
        // Verify body matches declared return type (if declared and concrete).
        // Use argMatchesParam (not typesEqual) to apply the same trait-family
        // relaxations that call-site checking uses — e.g. Int and Integer are
        // compatible, so `add : Int -> Int -> Int` with a body returning
        // Integer (inferred from literal arithmetic) isn't a mismatch.
        // An inline result annotation shares the clause's type parameters:
        // `let wrap(value: A) -> A? = None` relates its result to `value`.
        // The early resolution above uses a separate map for declaration
        // validation; publishing it would turn the result's A into another
        // generic and accept `let n: Integer? = wrap("text")`.
        auto effectiveReturnType = contract ? contract->result
            : clause.returnAnnotation
                ? resolveTypeExpr(**clause.returnAnnotation, genericVars)
                : inlineReturnType;
        const auto servingTransitionMatches = [&]() {
            if (!def.isSlot || !effectiveReturnType || clause.body.empty())
                return false;
            auto expected = resolve(effectiveReturnType);
            auto* replyType = std::get_if<NamedType>(&expected->kind);
            if (!replyType || replyType->name != "Reply" ||
                replyType->typeArgs.size() != 1)
                return false;
            const ast::Expr* result = clause.body.back().get();
            if (auto* returned = std::get_if<ast::ReturnExpr>(&result->kind))
                result = returned->value.get();
            auto* map = result ? std::get_if<ast::MapExpr>(&result->kind) : nullptr;
            if (!map) return false;
            bool hasReply = false;
            bool hasState = false;
            for (const auto& entry : map->entries) {
                if (!entry.key || !entry.value) continue;
                auto* key = std::get_if<ast::AtomLiteral>(&entry.key->kind);
                if (!key || key->name != "reply") continue;
                hasReply = true;
                auto actual = resolve(inferExpr(*entry.value));
                if (!containsOpenType(actual) &&
                    !containsOpenType(replyType->typeArgs.front()) &&
                    !argMatchesParam(actual, replyType->typeArgs.front()))
                    typeMismatch(entry.value->location,
                                 replyType->typeArgs.front(), actual);
            }
            for (const auto& entry : map->entries) {
                if (!entry.key) continue;
                auto* key = std::get_if<ast::AtomLiteral>(&entry.key->kind);
                if (key && (key->name == "new" || key->name == "state"))
                    hasState = true;
            }
            if (hasReply) return true;
            if (!hasState) return false;

            std::function<bool(const ast::Expr&)> mentionsFrom;
            mentionsFrom = [&](const ast::Expr& expression) -> bool {
                return std::visit([&](const auto& node) -> bool {
                    using E = std::decay_t<decltype(node)>;
                    auto one = [&](const ast::ExprPtr& value) {
                        return value && mentionsFrom(*value);
                    };
                    if constexpr (std::is_same_v<E, ast::Identifier>)
                        return node.name == "from";
                    else if constexpr (std::is_same_v<E, ast::MethodCall>) {
                        if (one(node.receiver)) return true;
                        for (const auto& arg : node.args) if (one(arg)) return true;
                        for (const auto& [_, value] : node.namedArgs) if (one(value)) return true;
                        return node.block && one(*node.block);
                    } else if constexpr (std::is_same_v<E, ast::FunctionCall>) {
                        for (const auto& arg : node.args) if (one(arg)) return true;
                        for (const auto& [_, value] : node.namedArgs) if (one(value)) return true;
                        return node.block && one(*node.block);
                    } else if constexpr (std::is_same_v<E, ast::Lambda> ||
                                         std::is_same_v<E, ast::BlockExpr>) {
                        for (const auto& item : node.body) if (one(item)) return true;
                    } else if constexpr (std::is_same_v<E, ast::ReturnExpr>)
                        return one(node.value);
                    else if constexpr (std::is_same_v<E, ast::MapExpr>) {
                        for (const auto& entry : node.entries)
                            if (one(entry.key) || one(entry.value)) return true;
                    } else if constexpr (std::is_same_v<E, ast::LetExpr> ||
                                         std::is_same_v<E, ast::VarExpr> ||
                                         std::is_same_v<E, ast::AssignExpr>)
                        return one(node.value);
                    else if constexpr (std::is_same_v<E, ast::IfExpr>) {
                        if (one(node.condition)) return true;
                        for (const auto& item : node.thenBody) if (one(item)) return true;
                        for (const auto& [condition, body] : node.elifs) {
                            if (one(condition)) return true;
                            for (const auto& item : body) if (one(item)) return true;
                        }
                        if (node.elseBody)
                            for (const auto& item : *node.elseBody) if (one(item)) return true;
                    }
                    return false;
                }, expression.kind);
            };
            const bool deferred = std::any_of(
                clause.body.begin(), clause.body.end(),
                [&](const ast::ExprPtr& item) { return item && mentionsFrom(*item); });
            if (!deferred)
                error(def.location,
                      "call slot returns without `reply:` but never uses `from` for a deferred reply");
            return true;
        }();
        const auto servingCastTransitionMatches = [&]() {
            if (!def.isSlot || !effectiveReturnType || clause.body.empty() ||
                !typesEqual(resolve(effectiveReturnType), Type::unit()))
                return false;
            if (m_currentMakeType &&
                argMatchesParam(resolve(bodyType), resolve(m_currentMakeType)))
                return true;
            const ast::Expr* result = clause.body.back().get();
            if (auto* returned = std::get_if<ast::ReturnExpr>(&result->kind))
                result = returned->value.get();
            auto* map = result ? std::get_if<ast::MapExpr>(&result->kind) : nullptr;
            if (!map) return false;
            return std::any_of(map->entries.begin(), map->entries.end(),
                [](const ast::MapEntry& entry) {
                    if (!entry.key) return false;
                    auto* key = std::get_if<ast::AtomLiteral>(&entry.key->kind);
                    return key && (key->name == "new" || key->name == "state" ||
                                   key->name == "stop");
                });
        }();
        if (effectiveReturnType &&
            !std::holds_alternative<TypeVar>(effectiveReturnType->kind) &&
            !std::holds_alternative<UnknownType>(effectiveReturnType->kind) &&
            !deferIfPending(bodyType, effectiveReturnType, def.location) &&
            !std::holds_alternative<UnknownType>(bodyType->kind) &&
            !std::holds_alternative<TypeVar>(bodyType->kind) &&
            !argMatchesParam(bodyType, effectiveReturnType) &&
            !servingTransitionMatches && !servingCastTransitionMatches &&
            // `foul first() -> BookId = 1`: a literal result enters the
            // distinct type it is declared to return.
            ![&] {
                if (clause.body.empty() || !clause.body.back()) return false;
                const ast::Expr* result = clause.body.back().get();
                if (const auto* returned = std::get_if<ast::ReturnExpr>(&result->kind))
                    result = returned->value.get();
                return result && literalCoercesTo(*result, effectiveReturnType);
            }()) {
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
        Signature checkedSignature{def.name, std::move(paramTypes), resultType,
                                  def.isFoul, required};
        // Kept so a call site can place NAMED arguments into this method's
        // parameter slots. Only a FunctionDef knows the names; a `:>`
        // annotation does not spell them.
        for (const auto& param : clause.params)
            checkedSignature.paramNames.push_back(param.name ? *param.name : "");
        checkedSignature.clause = &clause;
        // The pre-pass's result variable for this clause gets the body's
        // answer, before generalizing renames the clause's own variables.
        if (auto pending = m_pendingResults.find(&clause);
            pending != m_pendingResults.end()) {
            const int id = pending->second;
            m_pendingResults.erase(pending);
            answerPending(id, resultType);
        }
        // And each implementing type's copy of a trait default, `This` its own.
        for (auto [inherited, end] = m_inheritedPendingResults.equal_range(&clause);
             inherited != end; ++inherited)
            answerPending(inherited->second.second,
                          bindThis(resultType, inherited->second.first));
        m_inheritedPendingResults.erase(&clause);
        // `let id(x) = x` is polymorphic, and every call site must get its own
        // instance of it rather than all of them sharing one variable.
        generalizeSignature(checkedSignature, typeVarMark);
        signatures.push_back(std::move(checkedSignature));
    }

    if (!m_inMakeBlock)
        for (auto& signature : signatures)
            signature.declaringModule = m_currentModulePath;
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
                    // The annotation gave the types, the definition gives the
                    // names — a named argument at a call site needs both.
                    method.paramNames = checked.paramNames;
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
        !hasAnnotatedSignatureForReceiver(def.name, m_currentMakeType)) {
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
            withReceiver.makeModule = m_currentMakeModule;
            if (!receiverIsFirstParam)
                withReceiver.params.insert(withReceiver.params.begin(),
                                           m_currentMakeType);
            if (!receiverIsFirstParam)
                withReceiver.requiredParams =
                    withReceiver.requiredParams.value_or(
                        signature.params.size()) + 1;
            generalizeTargetVariables(withReceiver, m_currentMakeType);
            // One clause, one signature: the checked body replaces what the
            // pre-pass registered for it. Kept beside it, the placeholder
            // (`Files -> A -> B`) and the checked `Files -> A -> String?`
            // were two candidates, and `this.canned(path)` an overload left
            // to the value. A trait default's clause is registered once per
            // implementing type too: each of those copies is the checked
            // body with `This` that type.
            const bool traitDefault =
                signature.clause && m_traitDefaultClauses.contains(signature.clause);
            if (traitDefault)
                for (auto& other : existing) {
                    if (other.clause != signature.clause || other.params.empty()) continue;
                    const auto inheritor = resolve(other.params.front());
                    if (isThisPlaceholder(inheritor)) continue;
                    auto bound = withReceiver;
                    for (auto& param : bound.params) param = bindThis(param, inheritor);
                    bound.result = bindThis(bound.result, inheritor);
                    bound.makeModule = other.makeModule;
                    other = std::move(bound);
                }
            // Clauses of one default are one method: rebound, `repeat(0)` and
            // `repeat(n: Integer)` both take `(Word, Integer)`, which kept
            // apart read as an ambiguous overload. The first stays, as the
            // first registration does for a `make` method's own clauses.
            if (traitDefault)
                for (auto later = existing.begin(); later != existing.end();)
                    later = std::any_of(existing.begin(), later,
                                        [&](const Signature& earlier) {
                                            return sameMethod(earlier, *later);
                                        })
                        ? existing.erase(later) : std::next(later);
            const auto placeholder = std::find_if(
                existing.begin(), existing.end(),
                [&](const Signature& other) { return other.clause == signature.clause; });
            if (signature.clause && !traitDefault && placeholder != existing.end()) {
                *placeholder = std::move(withReceiver);
                continue;
            }
            const bool duplicate =
                std::any_of(existing.begin(), existing.end(),
                        [&](const Signature& other) { return sameMethod(other, withReceiver); });
            if (!duplicate) existing.push_back(std::move(withReceiver));
        }
    }

    if (!m_inMakeBlock) {
        // What this definition contributes, for the qualified publication
        // below — NOT the accumulated bare-name set, which holds every
        // same-named function of every module checked so far.
        std::vector<Signature> publishable;
        // If a declared signature already exists, update its result type with
        // the inferred one (keeping declared params) and keep one entry.
        if (hasDeclaredContracts) {
            auto& sigs = m_userSignatures[def.name];
            // Replace the placeholder declared sig with the fully-checked one.
            if (declared && !signatures.empty()) {
                auto placeholder = std::find_if(
                    sigs.begin(), sigs.end(), [&](const Signature& candidate) {
                        // Another module's same-named function is not this
                        // one's placeholder, however alike their parameters.
                        if (candidate.declaringModule != m_currentModulePath)
                            return false;
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
                    checkedInterface.paramNames = signatures[0].paramNames;
                }
                publishable.push_back(checkedInterface);
                if (placeholder != sigs.end())
                    *placeholder = checkedInterface;
                else
                    sigs.push_back(std::move(checkedInterface));
            } else if (!signatures.empty()) {
                // Several same-arity contracts leave `declared` null by
                // multiplicity (Integer.parse radix:/culture:): the body
                // still checked fine, so append its signature for call
                // sites — but only where a sibling already published.
                // Never assign here: that would wipe the pre-pass
                // contracts a single-body overload set like FS.File.open
                // resolves against. An arity no contract declares keeps
                // its silent treatment (the annotation/body warning above
                // already spoke).
                const auto arity = def.clauses.empty()
                    ? size_t(0) : def.clauses.front().params.size();
                size_t matching = 0;
                if (scopedDeclared != m_scopedDeclaredSignatures.end())
                    for (const auto& contract : scopedDeclared->second)
                        if (contract.params.size() == arity &&
                            ++matching > 1)
                            break;
                if (matching > 1) {
                    for (auto& sig : signatures)
                        m_userSignatures[def.name].push_back(sig);
                    if (!m_currentModulePath.empty()) {
                        const auto key =
                            m_currentModulePath + "::" + def.name;
                        if (m_qualifiedPublished.contains(key))
                            for (const auto& sig : signatures)
                                m_userSignatures[key].push_back(sig);
                    }
                }
            }
        } else {
            publishable = signatures;
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
        publishQualifiedSignatures(def.name, publishable);
    }
    m_declarationImports = std::move(savedDeclarationImports);
}

// A module function is also reachable QUALIFIED (`M.lines("x")`), which checks
// as the call name `M::lines` — but its signatures were only ever filed under
// the bare `lines`, so every qualified call came back Unknown. A declared
// result type then bought nothing: `let n: Integer = M.lines("x")` passed with
// `lines : String -> [String]?` in plain sight.
//
// It is not a lookup fallback to the bare name: two modules may each define
// `lines`, and only this module's belongs under this key.
auto TypeChecker::publishQualifiedSignatures(
    const std::string& name, const std::vector<Signature>& signatures) -> void {
    if (m_currentModulePath.empty() || signatures.empty()) return;
    const auto key = kex::mangle::qualify(m_currentModulePath, name);
    // Only THIS definition's signatures: `Date.parse` and `Time.parse` share
    // the bare name `parse`, and publishing the accumulated set gave Date's
    // qualified key Time's result type.
    if (m_qualifiedPublished.insert(key).second)
        m_userSignatures[key] = signatures;
    else
        for (const auto& signature : signatures)
            m_userSignatures[key].push_back(signature);
}

// A module-scoped `make` is visible only where its module is: inside the module
// itself (or one nested in it), or under a `using` that names it — top-level or
// the lexical `using M do ... end` form, both of which land in
// m_declarationImports for the enclosing declaration. A top-level `make` (empty
// module) stays global, and so does everything reached through an imported
// INTERFACE: the prelude is always imported, and its interface signatures never
// carry a makeModule.
//
// Before this, a `make` inside a module was visible everywhere with no `using`
// at all — the one module member that was not import-gated
// (docs/ufcs-dispatch-plan.md "Follow-up", case 1).
// Is the code being checked inside `module`, or inside one nested within it?
// An empty owner is the top level, which everything in the file shares.
auto TypeChecker::withinModule(const std::string& module) const -> bool {
    if (module.empty()) return true;
    return m_currentModulePath == module ||
           m_currentModulePath.rfind(module + ".", 0) == 0;
}

auto TypeChecker::makeModuleVisible(const std::string& module) const -> bool {
    if (module.empty()) return true;
    // Inside the defining module, or one nested within it.
    if (m_currentModulePath == module ||
        m_currentModulePath.rfind(module + ".", 0) == 0)
        return true;
    // `using A` also brings `A.B`'s members into scope, matching how a
    // qualified member of a nested module resolves. The lexical
    // `using M do ... end` form counts as the declaration's own does.
    const auto names = [&](const ImportSelection& import) {
        return import.module == module ||
               import.module.rfind(module + ".", 0) == 0 ||
               module.rfind(import.module + ".", 0) == 0;
    };
    for (const auto& frame : m_importScopeStack)
        if (std::any_of(frame.begin(), frame.end(), names)) return true;
    return std::any_of(m_declarationImports.begin(), m_declarationImports.end(), names);
}

auto TypeChecker::hiddenMakeTaking(const std::string& name,
                                   const std::vector<TypePtr>& argTypes) -> std::string {
    const auto methods = m_methodSignatures.find(name);
    if (methods == m_methodSignatures.end() || argTypes.empty()) return {};
    const auto receiver = resolve(argTypes.front());
    std::string hidden;
    for (const auto& method : methods->second) {
        if (method.makeModule.empty() || makeModuleVisible(method.makeModule) ||
            method.params.empty())
            continue;
        const auto required = method.requiredParams.value_or(method.params.size());
        if (argTypes.size() < required || argTypes.size() > method.params.size()) continue;
        // A make for its module's own type travels with that type's values.
        const auto declared = resolve(method.params.front());
        if (const auto* named = std::get_if<NamedType>(&declared->kind);
            named && named->name.starts_with(method.makeModule + "."))
            continue;
        if (!argMatchesParam(receiver, declared)) continue;
        if (hidden.empty() || method.makeModule < hidden) hidden = method.makeModule;
    }
    return hidden;
}

auto TypeChecker::freeFunctionVisible(const std::string& module,
                                      const std::string& name) const -> bool {
    if (withinModule(module)) return true;
    if (m_privateFreeFunctions.contains(module + "\n" + name)) return false;
    const auto admits = [&](const ImportSelection& import) {
        if (import.module != module) return false;
        if (!import.onlyNames.empty() &&
            std::find(import.onlyNames.begin(), import.onlyNames.end(), name) ==
                import.onlyNames.end())
            return false;
        return std::find(import.exceptNames.begin(), import.exceptNames.end(),
                         name) == import.exceptNames.end();
    };
    for (const auto& frame : m_importScopeStack)
        for (const auto& import : frame)
            if (admits(import)) return true;
    return std::any_of(m_declarationImports.begin(), m_declarationImports.end(),
                       admits);
}

auto TypeChecker::checkMakeDef(const ast::MakeDef& def) -> void {
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_makeImports.find(&def); imports != m_makeImports.end())
        m_declarationImports = imports->second;
    pushScope();
    bool wasInMakeBlock = m_inMakeBlock;
    auto prevMakeType = m_currentMakeType;
    auto prevMakeModule = m_currentMakeModule;
    m_inMakeBlock = true;
    // A `make` inside a module belongs to it, and is import-gated with it.
    m_currentMakeModule = m_currentModulePath;
    // Preserve the complete receiver type. This keeps primitive targets
    // canonical (Bool is PrimitiveType::Bool), retains Map/List structure,
    // and carries generic arguments for ADT/record receiver functions.
    m_currentMakeType.reset();
    auto prevMakeTargetVars = std::move(m_currentMakeTargetVars);
    m_currentMakeTargetVars.clear();
    if (def.target) m_currentMakeType = resolveTypeExpr(*def.target, m_currentMakeTargetVars);
    for (const auto& item : def.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                // A `private do ... end` block is checked as the rest are:
                // visibility is not a type-system concept, and every make
                // method is registered before any body is checked.
                if (node)
                    for (const auto& inner : node->items)
                        if (const auto* fn =
                                std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner))
                            if (*fn) checkFunctionDef(**fn);
            }
        }, item);
    }
    m_inMakeBlock = wasInMakeBlock;
    m_currentMakeType = prevMakeType;
    m_currentMakeModule = std::move(prevMakeModule);
    m_currentMakeTargetVars = std::move(prevMakeTargetVars);
    popScope();

    checkTraitImplementation(def);
    m_declarationImports = std::move(savedDeclarationImports);
}

// A trait's default bodies are checked as `make` methods: `this` is any value
// claiming the trait, and their calls are decided as every other
// body's are (lowering emits each default once per claiming type).
auto TypeChecker::checkTraitDef(const ast::TraitDef& def) -> void {
    pushScope();
    const bool wasInMakeBlock = m_inMakeBlock;
    auto prevMakeType = m_currentMakeType;
    auto prevMakeModule = m_currentMakeModule;
    m_inMakeBlock = true;
    m_currentMakeModule = m_currentModulePath;
    // `This`, as the trait's own signatures leave it.
    m_currentMakeType = Type::named(names::kThisType);
    for (const auto& item : def.body)
        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item); fn && *fn)
            checkFunctionDef(**fn);
    m_inMakeBlock = wasInMakeBlock;
    m_currentMakeType = std::move(prevMakeType);
    m_currentMakeModule = std::move(prevMakeModule);
    popScope();
}

auto TypeChecker::checkTraitImplementation(const ast::MakeDef& def) -> void {
    if (def.implements.empty()) return;

    // Extract the type name from the make target.
    std::string typeName;
    if (const auto name = writtenMakeTarget(def.target, StructuralTarget::Excluded);
        name && name->parts.size() == 1)
        typeName = name->parts[0];

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

    for (const auto& written : def.implements) {
        // A qualified trait or capability (`implement: Harmat.Connection`)
        // is a module reference like any other: when it is the only one a
        // file makes, discovery must still load the module declaring it, or
        // checking that file alone — as the language server does — reports
        // a trait the whole program knows perfectly well.
        if (written.find('.') != std::string::npos)
            recordWithOwner(m_referencedModules, written);
        std::vector<std::string> candidates;
        const auto traitName = resolveTraitReference(
            written, m_currentModulePath, nullptr, true, &candidates);
        const TraitDef* trait = m_traits.get(traitName);
        if (!trait) {
            if (candidates.size() > 1) {
                std::string listed;
                for (const auto& candidate : candidates)
                    listed += (listed.empty() ? "`" : ", `") + candidate + "`";
                error(def.location, "Ambiguous trait: `" + written +
                      "` could be " + listed + " — write the one you mean");
            } else {
                error(def.location, "Unknown trait: " + written);
            }
            // Unresolved stays empty so the recorded list aligns with
            // `def.implements` by index; the lowerer skips it as a miss.
            m_resolvedTraits[&def].push_back({});
            continue;
        }
        m_resolvedTraits[&def].push_back(traitName);
        std::string prefix = "make " + (typeName.empty() ? "?" : typeName) +
                             ", implement: " + written + " — ";
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
    if (block.rescue) checkRescue(*block.rescue);
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
    for (const auto* function : importedCandidates(name)) result.push_back(function->signature);
    return result;
}

auto TypeChecker::importedCandidates(const std::string& name) const
    -> std::vector<const ImportedFunction*> {
    std::vector<const ImportedFunction*> result;
    if (!m_importedInterfaces) return result;
    if (kex::mangle::hasScope(name)) {
        auto module = m_importedInterfaces->modules.find(kex::mangle::headOf(name));
        if (module != m_importedInterfaces->modules.end())
            if (auto functions = module->second.exports.find(kex::mangle::tailOf(name));
                functions != module->second.exports.end())
                for (const auto& function : functions->second) result.push_back(&function);
        return result;
    }
    if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
        functions != m_importedInterfaces->receiverFunctions.end())
        for (const auto& function : functions->second)
            if (importedFunctionVisible(function)) result.push_back(&function);
    for (const auto& [_, module] : m_importedInterfaces->modules) {
        if (!module.automaticImport) continue;
        if (auto functions = module.exports.find(name);
            functions != module.exports.end())
            for (const auto& function : functions->second) result.push_back(&function);
    }
    return result;
}

auto TypeChecker::usingImportedMethods(const std::string& name, bool asMethod) const
    -> std::vector<const ImportedFunction*> {
    std::vector<const ImportedFunction*> result;
    if (!m_importedInterfaces) return result;
    for (const auto& [moduleName, module] : m_importedInterfaces->modules) {
        // A module of this program keeps its own signatures, and the
        // prelude's are reached through its receiver functions already.
        if (!module.compiledApart || m_localModules.contains(moduleName) ||
            !moduleMemberImported(moduleName, name))
            continue;
        if (auto functions = module.exports.find(name); functions != module.exports.end())
            for (const auto& function : functions->second)
                if (!asMethod || !function.signature.params.empty())
                    result.push_back(&function);
    }
    return result;
}

auto TypeChecker::importedFunctionVisible(
    const ImportedFunction& function) const -> bool {
    if (function.sourceModule.empty() || function.visibleByReceiver) return true;
    if (m_importedInterfaces) {
        if (auto module = m_importedInterfaces->modules.find(function.sourceModule);
            module != m_importedInterfaces->modules.end() &&
            module->second.automaticImport)
            return true;
    }
    // `sourceName` on a receiver method is the METHOD's own name (`close`,
    // set by `addReceiverSig` in prelude_interfaces.hxx) — but `only:`/
    // `except:` on a `using` name the DECLARING TYPE (`using
    // Net.HTTP.WebSocket, only: [Connection]`), not the method. Gating a
    // dot-call candidate on the method's own name checked "close" against
    // ["Connection"] and always lost, so a same-named, same-shaped method on
    // some OTHER, always-visible type (`FileHandle`'s own nullary `close`)
    // was the only candidate left standing — silently wrong overload
    // resolution rather than a missing-import error (kexhq/kex#383). Try the
    // receiver's own type name first; fall back to the method-name check,
    // which is still right for a bare `only: [someFunction]` import of an
    // ordinary module-level export.
    if (function.isReceiverMethod && !function.signature.params.empty()) {
        if (auto* named =
                std::get_if<NamedType>(&function.signature.params.front()->kind)) {
            // A qualified receiver carries access to its own companion's
            // methods, even when obtained without a using declaration.
            if (named->name == function.sourceModule) return true;
            const auto dot = named->name.rfind('.');
            const auto bare = dot == std::string::npos
                ? named->name : named->name.substr(dot + 1);
            if (moduleMemberImported(function.sourceModule, bare)) return true;
        }
    }
    return moduleMemberImported(function.sourceModule, function.sourceName);
}

auto TypeChecker::moduleMemberImported(const std::string& module, const std::string& member,
                                       bool bare) const -> bool {
    auto selected = [&](const ImportSelection& import) {
        if (bare && !resolve_core::usingImportsNames(import.alias, import.onlyNames))
            return false;
        const bool prefixMatch = module == import.module ||
            module.rfind(import.module + ".", 0) == 0;
        if (!prefixMatch) {
            // A `make X<A> do ... end` sitting directly in a file-header
            // module (`module Data`) attributes its receivers to the
            // record's own qualified name (`Data.UnorderedSet`) — a SIBLING
            // of another type declared in the same file (`Data.Set`), not
            // its ancestor or descendant. `using Data.Set` is meant to bring
            // every flavour the file declares along with it (the header
            // comment says so), so a shared immediate parent is visibility
            // too, not just a literal prefix match (kexhq/kex#229).
            const auto parentOf = [](const std::string& qualified) {
                const auto dot = qualified.rfind('.');
                return dot == std::string::npos ? std::string()
                                                 : qualified.substr(0, dot);
            };
            const auto importParent = parentOf(import.module);
            if (importParent.empty() || importParent != parentOf(module))
                return false;
            // This sibling rule is for type companions, not unrelated
            // modules such as Units.SI and Units.Data.
            if (!m_importedInterfaces ||
                !m_importedInterfaces->typeNames.contains(import.module) ||
                !m_importedInterfaces->typeNames.contains(module))
                return false;
        }
        auto selectedMember = member;
        if (prefixMatch && module.size() > import.module.size()) {
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

// The declared result of a zero-parameter binding named `name` — what a bare
// reference to it evaluates to, since naming one auto-calls it. Only a single,
// fully concrete signature answers: a generic result (`A`, a type variable)
// would need instantiating per use, which the call path does and this doesn't.
auto TypeChecker::zeroArgBindingResult(const std::string& name) -> TypePtr {
    const std::vector<Signature>* signatures = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        scoped != m_scopedDeclaredSignatures.end())
        signatures = &scoped->second;
    else if (auto user = m_userSignatures.find(name); user != m_userSignatures.end())
        signatures = &user->second;
    if (!signatures || signatures->size() != 1) return nullptr;
    const auto& signature = signatures->front();
    if (!signature.params.empty() || !signature.result) return nullptr;
    auto result = resolve(signature.result);
    std::function<bool(const TypePtr&)> mentionsTypeParameter =
        [&](const TypePtr& type) -> bool {
        if (!type) return true;
        const auto resolved = resolve(type);
        if (const auto* named = std::get_if<NamedType>(&resolved->kind)) {
            if (named->name.size() == 1 && std::isupper(static_cast<unsigned char>(named->name[0])))
                return true;
            return std::any_of(named->typeArgs.begin(), named->typeArgs.end(),
                               mentionsTypeParameter);
        }
        if (const auto* list = std::get_if<ListType>(&resolved->kind))
            return mentionsTypeParameter(list->element);
        if (const auto* optional = std::get_if<OptionalType>(&resolved->kind))
            return mentionsTypeParameter(optional->inner);
        if (const auto* map = std::get_if<MapType>(&resolved->kind))
            return mentionsTypeParameter(map->key) || mentionsTypeParameter(map->value);
        if (const auto* tuple = std::get_if<TupleType>(&resolved->kind))
            return std::any_of(tuple->elements.begin(), tuple->elements.end(),
                               mentionsTypeParameter);
        return false;
    };
    if (containsOpenType(result) || mentionsTypeParameter(result)) return nullptr;
    return result;
}

// A bare reference to a function of one or more parameters, used as a VALUE
// rather than called (`check(addPair)`, no `~`) — lowering eta-expands this
// into an actual closure, so the checker must give it a real FuncType
// instead of leaving it Unknown (permissive), which let a function whose
// shape didn't actually match the expected parameter type through to
// misbehave or crash at runtime instead of being rejected here (#350). Only
// a single, fully concrete signature answers — an overloaded name is
// ambiguous without a call's arguments to select by, and a generic
// parameter/result would need instantiating fresh per use, which this
// narrower path does not attempt (mirrors zeroArgBindingResult).
auto TypeChecker::functionValueType(const std::string& name) -> TypePtr {
    auto it = m_userSignatures.find(name);
    if (it == m_userSignatures.end() || it->second.size() != 1) return nullptr;
    const auto& signature = it->second.front();
    if (signature.params.empty() || !signature.result) return nullptr;
    std::vector<TypePtr> params;
    for (const auto& param : signature.params) params.push_back(resolve(param));
    auto result = resolve(signature.result);
    if (containsOpenType(result) ||
        std::any_of(params.begin(), params.end(),
                    [](const TypePtr& p) { return containsOpenType(p); }))
        return nullptr;
    return Type::func(std::move(params), result);
}

namespace {
// An inference variable prints as `T7`: its number in the checker, which
// says nothing to the reader, and changes whenever unrelated code above it
// does. In a message it is renamed to a letter, the way a signature's generic
// parameters are written (`A -> B`), picking letters that message does not
// already use: `expects A -> B, but got Integer? -> T7 -> T7` reads
// `… but got Integer? -> C -> C`. Two mentions of one variable keep one
// letter, which is the information the number carried.
auto nameInferenceVariables(const std::string& message) -> std::string {
    const auto word = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    // A variable is `T` and digits, standing alone.
    const auto variableAt = [&](std::size_t i) -> std::size_t {
        if (message[i] != 'T' || (i > 0 && word(message[i - 1]))) return 0;
        std::size_t end = i + 1;
        while (end < message.size() &&
               std::isdigit(static_cast<unsigned char>(message[end])))
            ++end;
        if (end == i + 1 || (end < message.size() && word(message[end]))) return 0;
        return end - i;
    };
    std::set<char> taken;
    bool any = false;
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (variableAt(i)) any = true;
        if (std::isupper(static_cast<unsigned char>(message[i])) &&
            (i == 0 || !word(message[i - 1])) &&
            (i + 1 == message.size() || !word(message[i + 1])))
            taken.insert(message[i]);
    }
    if (!any) return message;
    std::unordered_map<std::string, std::string> names;
    char next = 'A';
    std::string renamed;
    for (std::size_t i = 0; i < message.size();) {
        const auto length = variableAt(i);
        if (!length) {
            renamed += message[i++];
            continue;
        }
        const auto variable = message.substr(i, length);
        auto name = names.find(variable);
        if (name == names.end()) {
            while (next <= 'Z' && taken.count(next)) ++next;
            // Out of letters: the number is still a name.
            std::string letter = next <= 'Z' ? std::string(1, next++) : variable;
            name = names.emplace(variable, letter).first;
        }
        renamed += name->second;
        i += length;
    }
    return renamed;
}
} // namespace

auto TypeChecker::error(SourceLocation loc, const std::string& msg) -> void {
    if (m_diagnostics) {
        m_diagnostics->push_back(
            {Diagnostic::Level::Error, loc, nameInferenceVariables(msg)});
    }
}

auto TypeChecker::typeMismatch(SourceLocation loc, const TypePtr& expected,
                               const TypePtr& actual) -> void {
    std::string message = "Type mismatch: expected " + typeToString(expected) +
        ", got " + typeToString(actual);
    if (const auto* never = std::get_if<VoidType>(&expected->kind);
        never && !never->reason.empty())
        message += " (" + never->reason + ")";
    error(loc, std::move(message));
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::constantType(const std::string& name) const -> TypePtr {
    for (auto scope = m_currentModulePath;;) {
        if (const auto found = m_userSignatures.find(kex::mangle::qualify(scope, name));
            found != m_userSignatures.end())
            for (const auto& signature : found->second)
                if (signature.params.empty()) return signature.result;
        if (scope.empty()) break;
        const auto dot = scope.rfind('.');
        scope.resize(dot == std::string::npos ? 0 : dot);
    }
    // One a `using` imports from a unit compiled apart (`STATUSES`).
    if (m_importedInterfaces)
        for (const auto& [_, module] : m_importedInterfaces->modules)
            if (const auto exports = module.exports.find(name); exports != module.exports.end())
                for (const auto& function : exports->second)
                    if (!function.isConstructor && function.signature.params.empty() &&
                        importedFunctionVisible(function))
                        return function.signature.result;
    return nullptr;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::nullaryConstructorType(const std::string& name) -> TypePtr {
    if (auto constructed = constructorResultType(name, {});
        constructed && m_nullaryConstructors.contains(name))
        return constructed;
    if (m_nullaryConstructors.contains(name))
        return Type::named(name);
    for (const auto& [module, constructors] : m_moduleConstructors) {
        auto constructor = constructors.find(name);
        if (constructor == constructors.end() || constructor->second.arity != 0) continue;
        // Inside its own module the constructor is in scope as written: `Name`
        // in `module App.Models.User` is its `UserDetail.Name`, not a
        // same-named record elsewhere (`Net.DNS.Name`).
        if (withinModule(module)) return Type::named(constructor->second.typeName);
        if (moduleMemberImported(module, name)) return Type::named(name);
    }
    if (m_importedInterfaces)
        for (const auto& [_, module] :
             m_importedInterfaces->modules)
            if (auto exports = module.exports.find(name);
                exports != module.exports.end())
                for (const auto& function : exports->second)
                    if (function.isConstructor &&
                        function.signature.params.empty() &&
                        importedFunctionVisible(function))
                        return function.signature.result;
    // A spelling two ADTs share: typed as whichever one is in scope,
    // not left unknown. `x : Integer = Timeout` checked clean.
    if (const auto* owner = scopedConstructorOwner(name);
        owner && owner->arity == 0 && !owner->generic)
        return Type::named(owner->adt);
    return nullptr;
}

} // namespace kex::semantic

namespace kex::semantic {

// A bare call that only `make` methods answer is a method call, as both
// backends run it (lowering's FunctionCall cascade): inside a make block,
// written one argument short of the block's own method, `this` is the
// receiver (`decorate(@name)`, kexhq/kex#281); otherwise the first argument
// is (`shout(tag)`, UFCS). A binding of the name, or a function at the
// written arity, still answers it instead. Null when neither applies.
auto TypeChecker::bareMethodCall(const ast::Expr& expr, const ast::FunctionCall& call,
                                 const std::vector<TypePtr>& argTypes) -> TypePtr {
    const auto& name = call.name;
    if (kex::mangle::hasScope(name) || lookupVar(name)) return nullptr;
    const auto methods = m_methodSignatures.find(name);
    if (methods == m_methodSignatures.end()) return nullptr;
    const auto suffix = std::string(kex::mangle::kSeparator) + name;
    for (const auto& [key, signatures] : m_userSignatures)
        if (key == name || key.ends_with(suffix))
            for (const auto& signature : signatures)
                if (signature.params.size() == argTypes.size()) return nullptr;
    // A method of `arity` parameters, receiver included; of `receiver`'s
    // type when one is given.
    const auto methodAt = [&](std::size_t arity, const TypePtr& receiver) {
        const auto* wanted =
            receiver ? std::get_if<NamedType>(&resolve(receiver)->kind) : nullptr;
        return std::any_of(
            methods->second.begin(), methods->second.end(), [&](const Signature& method) {
                if (method.params.size() != arity) return false;
                if (!receiver) return true;
                const auto* own = std::get_if<NamedType>(&resolve(method.params.front())->kind);
                if (wanted && own)
                    return ownTypeIdentity(own->name) == ownTypeIdentity(wanted->name);
                return typesEqual(resolve(method.params.front()), resolve(receiver));
            });
    };
    if (m_inMakeBlock && m_currentMakeType &&
        methodAt(argTypes.size() + 1, m_currentMakeType)) {
        std::vector<TypePtr> withThis{m_currentMakeType};
        withThis.insert(withThis.end(), argTypes.begin(), argTypes.end());
        auto result = checkCall(name, withThis, expr.location, true, nullptr, &expr);
        m_decidedCalls.try_emplace(
            &expr, DecidedCall{CallKind::OwnMethod, typeToString(m_currentMakeType)});
        return result;
    }
    if (argTypes.empty() || !methodAt(argTypes.size(), nullptr)) return nullptr;
    auto result = checkCall(name, argTypes, expr.location, true, nullptr, &expr);
    if (!m_decidedCalls.contains(&expr)) {
        const auto receiver = resolve(argTypes.front());
        if (const auto own = ownTypeAnswers(argTypes, name))
            m_decidedCalls[&expr] = own->decision();
        else if (isUndetermined(receiver))
            decideDispatch(&expr, name);
    }
    return result;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::decideDispatch(const ast::Expr* callExpr, const std::string& name) -> void {
    m_decidedCalls[callExpr] = {CallKind::Dispatch, name};
    // One imported module answering it, as this scope sees the providers:
    // its dispatcher is the target, and lowering calls it rather than
    // searching the providers itself.
    const auto* call = std::get_if<ast::MethodCall>(&callExpr->kind);
    if (!m_importedInterfaces || !m_linkedModules || !call || !call->receiver ||
        m_resolvedCalls.contains(call))
        return;
    if (auto choice = importedFunctionChoice(*call, name)) {
        m_resolvedCalls[call] = std::move(*choice);
        return;
    }
    // A `make` method or function of this program by that name competes
    // with the import at run time: that stays the value's to decide.
    if (m_methodSignatures.contains(name) || m_userSignatures.contains(name)) return;
    const auto providers = m_importedInterfaces->receiverFunctions.find(name);
    if (providers == m_importedInterfaces->receiverFunctions.end()) return;
    const int arity = static_cast<int>(call->args.size() + call->namedArgs.size()) + 1 +
                      (call->block ? 1 : 0);
    // Each module providing it at this arity that is in scope here: a
    // `make` for a type its module does not declare is reached only through
    // a `using` of that module, as lowering filters it.
    std::map<std::string, std::vector<const ImportedFunction*>> byModule;
    for (const auto& candidate : providers->second) {
        if (candidate.backendArity != arity) continue;
        // A module this build does not link provides nothing at run time.
        if (!m_linkedModules->contains(candidate.backendModule)) continue;
        // Nor does one declared for an alias: no value has that type.
        if (candidate.aliasReceiver) continue;
        if (candidate.foreignTarget && !makeModuleVisible(candidate.sourceModule)) continue;
        byModule[candidate.backendModule].push_back(&candidate);
    }
    // A module's dispatcher: the provider under the call's own name.
    const auto dispatcherOf = [&](const std::string& module) -> const ImportedFunction* {
        const auto found = byModule.find(module);
        if (found == byModule.end()) return nullptr;
        for (const auto* candidate : found->second)
            if (candidate->backendFunction == name) return candidate;
        return nullptr;
    };
    if (byModule.size() == 1) {
        if (const auto* dispatcher = dispatcherOf(byModule.begin()->first))
            m_resolvedCalls[call] = dispatcherTarget(*dispatcher);
        return;
    }
    // Several: the prelude's dispatcher for every value the others' providers
    // do not take, each of those chosen by the value — by its type, where
    // that is a record or a primitive a guard can test, or by its registered
    // owner, where the module's provider is a dispatcher for any receiver.
    const auto* prelude = dispatcherOf(names::kPreludeModule);
    if (!prelude || !call->namedArgs.empty()) return;
    std::vector<RuntimeAlternative> alternatives;
    for (const auto& [module, candidates] : byModule) {
        if (module == names::kPreludeModule) continue;
        std::vector<RuntimeAlternative> typed;
        bool generic = false;
        for (const auto* candidate : candidates) {
            const auto receiver = candidate->signature.params.empty()
                ? Type::unknown() : resolve(candidate->signature.params.front());
            if (std::holds_alternative<PrimitiveType>(receiver->kind) ||
                (std::holds_alternative<NamedType>(receiver->kind) &&
                 m_recordFields.contains(std::get<NamedType>(receiver->kind).name)))
                typed.push_back({typeToString(receiver), module, candidate->backendFunction});
            // A dispatcher for any receiver, or for several types at once
            // (`make TypeRef | PatternRef`): the value's owner chooses it.
            else if (isUndetermined(receiver) ||
                     std::holds_alternative<UnionType>(receiver->kind))
                generic = true;
            else
                return;  // a receiver no guard can test
        }
        if (generic) {
            if (!typed.empty() || !dispatcherOf(module)) return;
            alternatives.push_back({{}, module, name});
        } else {
            alternatives.insert(alternatives.end(), typed.begin(), typed.end());
        }
    }
    auto target = dispatcherTarget(*prelude);
    target.alternatives = std::move(alternatives);
    m_resolvedCalls[call] = std::move(target);
}

// A free function a `using` brings into scope beside records' own methods of
// the name, on a receiver of unknown type (`bag.get(name)` with `using Dsl`
// and `make Bag`): each record's method for a value of its type, the import
// for every other value, as the walker decides by the value. Functions of
// this unit are named as lowering emits them (`Dsl.get`, a record's bare
// dispatcher), with no module.
auto TypeChecker::importedFunctionChoice(const ast::MethodCall& call, const std::string& name)
    -> std::optional<ResolvedCallTarget> {
    const auto arity = call.args.size() + call.namedArgs.size() + 1 + (call.block ? 1 : 0);
    if (!call.namedArgs.empty()) return std::nullopt;
    std::string imported;
    for (const auto& module : m_localModules) {
        const auto found = m_userSignatures.find(kex::mangle::qualify(module, name));
        if (found == m_userSignatures.end() || !moduleMemberImported(module, name)) continue;
        if (std::none_of(found->second.begin(), found->second.end(),
                         [&](const Signature& signature) { return signature.params.size() == arity; }))
            continue;
        if (!imported.empty()) return std::nullopt;  // two imports: not this case
        imported = module + "." + name;
    }
    if (imported.empty()) return std::nullopt;
    std::vector<RuntimeAlternative> alternatives;
    std::set<std::string> seen;
    const auto record = [&](const TypePtr& receiver) -> std::string {
        const auto* named = std::get_if<NamedType>(&resolve(receiver)->kind);
        return named && m_recordFields.contains(named->name) ? named->name : std::string{};
    };
    if (const auto methods = m_methodSignatures.find(name); methods != m_methodSignatures.end())
        for (const auto& method : methods->second)
            if (method.params.size() == arity)
                if (const auto type = record(method.params.front());
                    !type.empty() && m_programRecords.contains(type) && seen.insert(type).second)
                    alternatives.push_back({type, {}, name});
    if (const auto providers = m_importedInterfaces->receiverFunctions.find(name);
        providers != m_importedInterfaces->receiverFunctions.end())
        for (const auto& provider : providers->second)
            if (provider.backendArity == static_cast<int>(arity) &&
                provider.backendFunction == name && !provider.signature.params.empty())
                if (const auto type = record(provider.signature.params.front());
                    !type.empty() && seen.insert(type).second)
                    alternatives.push_back({type, provider.backendModule, provider.backendFunction});
    if (alternatives.empty()) return std::nullopt;
    ResolvedCallTarget target;
    target.backendFunction = imported;
    target.backendArity = static_cast<int>(arity);
    target.passesReceiver = true;
    target.alternatives = std::move(alternatives);
    return target;
}

// A call of a module's receiver dispatcher, which picks the implementation
// by the value.
auto TypeChecker::dispatcherTarget(const ImportedFunction& dispatcher) -> ResolvedCallTarget {
    ResolvedCallTarget target;
    target.sourceModule = dispatcher.sourceModule;
    target.backendModule = dispatcher.backendModule;
    target.backendFunction = dispatcher.backendFunction;
    target.backendArity = dispatcher.backendArity;
    target.passesReceiver = true;
    target.paramNames = dispatcher.paramNames;
    target.defaultSources = dispatcher.defaultSources;
    return target;
}

} // namespace kex::semantic
