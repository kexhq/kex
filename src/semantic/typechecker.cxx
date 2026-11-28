#include "../ast/function_groups.hxx"
#include "../common/name_case.hxx"
#include "../ast/clone.hxx"
#include "../ast/walk.hxx"
#include "../common/mangle.hxx"
#include "typechecker.hxx"
#include "../common/stack_guard.hxx"
#include "analyzer.hxx"
#include "collect_pass.hxx"
#include "declaration_validator.hxx"
#include "module_rules.hxx"
#include "../common/type_def_utils.hxx"
#include "../common/runtime_dir.hxx"
#include "../common/well_known.hxx"
#include "typechecker_internal.hxx"
#include "exhaustiveness.hxx"
#include "resolve_core.hxx"
#include <cctype>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>

namespace kex::semantic {

using namespace kex::semantic::detail;

namespace {

// The types a `make` target covers, one per member of a union target (`make
// Float | Integer`): each as written (`String`, `List`, `Optional`, `Number`),
// a declared type by its identity instead (`Units.SI.SIPrefix`).
auto makeOwnerNames(const ast::TypeExprPtr& written, const TypePtr& target)
    -> std::vector<std::string> {
    auto names = kex::makeTargetNames(written);
    std::vector<TypePtr> members;
    const auto flatten = [&](const auto& self, const TypePtr& type) -> void {
        if (const auto* un = type ? std::get_if<UnionType>(&type->kind) : nullptr) {
            for (const auto& member : un->members) {
                self(self, member);
            }
        } else {
            members.push_back(type);
        }
    };
    flatten(flatten, target);
    if (members.size() == names.size()) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (const auto* named =
                    members[i] ? std::get_if<NamedType>(&members[i]->kind) : nullptr) {
                names[i] = named->name;
            }
        }
    }
    return names;
}

} // namespace

auto TypeChecker::check(
    const ast::Program& program,
    std::vector<Diagnostic>& diagnostics,
    const std::vector<std::string>& entryFiles) -> void {
    m_diagnostics = &diagnostics;
    m_reportedOutOfStack = false;
    m_entryFiles = entryFiles;
    m_checkedProgram = &program;
    m_reportedIndistinguishable.clear();
    m_unannotatedFunctionsReady = false;
    m_unannotatedFunctions.clear();
    m_callResults.clear();
    m_bodiesInProgress.clear();
    m_clauseCoverageGaps.clear();
    m_clauseCoverageSites.clear();
    m_makeTargetIdentities.clear();
    m_unannotatedParamOwner.clear();
    m_parameterDemands.clear();
    m_demandedCalls.clear();
    m_nameUses.clear();
    m_scopeNames.clear();
    m_localDefinitions.clear();
    m_bindingSites.clear();
    m_scopeContexts.clear();
    m_scopeContextIds.clear();
    m_namespaceRoots.clear();
    m_patternBindings.clear();
    m_recordKeys.clear();
    m_typeReferences.clear();
    m_stringPatterns.clear();
    m_successWrappers.clear();
    m_justArguments.clear();
    m_functionSignatures.clear();
    m_resolvedCalls.clear();
    m_captureCalls.clear();
    m_pendingCaptureCalls.clear();
    m_calledModules.clear();
    m_selectedCallSignatures.clear();
    m_captureSignatures.clear();
    m_capturesIntoFoul.clear();
    m_traitDefaultClauses.clear();
    m_traitDefaults.clear();
    m_traitDefaultDeclarations.clear();
    m_inheritedPendingResults.clear();
    m_shownThroughDistinct.clear();
    m_referencedModules.clear();
    m_localModules.clear();
    m_moduleConstructors.clear();
    m_declaredTypeArity.clear();
    m_adtVariants.clear();
    m_adtOfConstructor.clear();
    m_nullaryConstructors.clear();
    m_nullaryVariants.clear();
    m_methodSignatures.clear();
    m_higherKindedRequirements.clear();
    m_traitParameterLetters.clear();
    m_importedTypeBindings.clear();
    m_slotMethodNames.clear();
    m_makeMethodNames.clear();
    m_makeMethodReceivers.clear();
    m_makeMethodArities.clear();
    m_qualifiedPublished.clear();
    m_overloadPurity.clear();
    m_scopedDeclaredSignatures.clear();
    m_typeAliases.clear();
    m_distinctTypes.clear();
    m_recordFields.clear();
    m_guessedReceiverTypes.clear();
    m_constructorOwners.clear();
    m_requiredRecordFields.clear();
    if (m_importedInterfaces) {
        m_distinctTypes = m_importedInterfaces->distinctTypes;
    }
    if (m_importedInterfaces) {
        m_recordFields = m_importedInterfaces->recordFields;
    }
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
    m_programModules = std::make_shared<ProgramModules>(collectSymbols(program));
    // What a merged dependency's file declares outside its module is that
    // file's own, as it is where the file is compiled apart.
    m_fileLocalTopLevel.clear();
    if (!m_entryFiles.empty()) {
        const auto own = [&](std::string_view file) {
            return std::find(m_entryFiles.begin(), m_entryFiles.end(), file) != m_entryFiles.end();
        };
        std::unordered_set<std::string> entryNames;
        const auto each = [&](const auto& visit) {
            for (const auto& item : program.items) {
                if (const auto* function = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
                    function && *function) {
                    visit((*function)->name, (*function)->location.file);
                } else if (const auto* type = std::get_if<std::unique_ptr<ast::TypeDef>>(&item);
                           type && *type) {
                    if (auto constructors = kex::typeConstructors(**type)) {
                        for (const auto& constructor : *constructors) {
                            visit(constructor.name, (*type)->location.file);
                        }
                    }
                }
            }
        };
        each([&](const std::string& name, std::string_view file) {
            if (own(file)) {
                entryNames.insert(name);
            }
        });
        // Not what the standard library declares at its top level: its
        // interfaces hand those names to every program, bare.
        const auto everywhere = [&](const std::string& name) {
            if (!m_importedInterfaces) {
                return false;
            }
            if (m_importedInterfaces->receiverFunctions.count(name)) {
                return true;
            }
            for (const auto& adt : m_importedInterfaces->adts) {
                if (std::find(adt.constructors.begin(), adt.constructors.end(), name) !=
                    adt.constructors.end()) {
                    return true;
                }
            }
            return std::any_of(
                m_importedInterfaces->modules.begin(), m_importedInterfaces->modules.end(),
                [&](const auto& module) {
                    return module.second.automaticImport && module.second.exports.count(name);
                });
        };
        each([&](const std::string& name, std::string_view file) {
            if (!own(file) && !entryNames.contains(name) && !everywhere(name)) {
                m_fileLocalTopLevel.try_emplace(name, std::string(file));
            }
        });
    }
    m_reportedOnce.clear();
    auto selectionFor = [this](const ast::UsingBlock& block) {
        ImportSelection selection;
        selection.module += block.module.dotted();
        selection.onlyNames = block.onlyNames;
        selection.exceptNames = block.exceptNames;
        selection.alias = block.alias;
        selection.location = block.location;
        m_importedModulePaths.insert(selection.module);
        return selection;
    };
    // A `private do ... end` / `visible do ... end` block is a plain
    // container: the declarations inside it see exactly the imports that are
    // active around it. Without this, annotations on those functions resolved
    // record names without the enclosing `using`, so the same record came out
    // qualified in one signature and bare in another.
    auto collectVisibilityImports = [&](const ast::VisibilityBlock& block,
                                        const std::vector<ImportSelection>& active) {
        for (const auto& visible : block.items) {
            if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible);
                fn && *fn) {
                m_functionImports[fn->get()] = active;
            } else if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&visible);
                       make && *make) {
                m_makeImports[make->get()] = active;
            } else if (const auto* record = std::get_if<std::unique_ptr<ast::RecordDef>>(&visible);
                       record && *record) {
                m_recordImports[record->get()] = active;
            } else if (const auto* type = std::get_if<std::unique_ptr<ast::TypeDef>>(&visible);
                       type && *type) {
                m_typeImports[type->get()] = active;
            }
        }
    };
    std::function<void(const ast::ModuleDef&, std::vector<ImportSelection>, const std::string&)>
        collectModuleImports;
    collectModuleImports = [&](const ast::ModuleDef& module, std::vector<ImportSelection> active,
                               const std::string& parent) {
        const auto path = ast::nestedModulePath(parent, module.name);
        for (const auto& item : module.body) {
            std::visit(
                [&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                        if (node && node->body.empty()) {
                            active.push_back(selectionFor(*node));
                            reportAmbiguousImports(active, path);
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        if (node) {
                            m_functionImports[node.get()] = active;
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                        if (node) {
                            m_makeImports[node.get()] = active;
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                        if (node) {
                            m_recordImports[node.get()] = active;
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                        if (node) {
                            m_typeImports[node.get()] = active;
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                        if (node) {
                            collectVisibilityImports(*node, active);
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        if (node) {
                            collectModuleImports(*node, active, path);
                        }
                    }
                },
                item);
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
        for (size_t i = 0; i < topLevelImports.size(); ++i) {
            if (topLevelImportFiles[i] == module.location.file) {
                visible.push_back(topLevelImports[i]);
            }
        }
        return visible;
    };
    for (const auto& item : program.items) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                    if (node && node->body.empty()) {
                        topLevelImports.push_back(selectionFor(*node));
                        topLevelImportFiles.push_back(node->location.file);
                        // Against the `using`s of its own file.
                        std::vector<ImportSelection> sameFile;
                        for (size_t i = 0; i < topLevelImports.size(); ++i) {
                            if (topLevelImportFiles[i] == node->location.file) {
                                sameFile.push_back(topLevelImports[i]);
                            }
                        }
                        reportAmbiguousImports(sameFile, "");
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                    if (node) {
                        m_recordImports[node.get()] = topLevelImports;
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                    if (node) {
                        m_typeImports[node.get()] = topLevelImports;
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (node) {
                        m_functionImports[node.get()] = topLevelImports;
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                    if (node) {
                        m_makeImports[node.get()] = topLevelImports;
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                    if (node) {
                        m_mainImports[node.get()] = topLevelImports;
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (node) {
                        collectVisibilityImports(*node, topLevelImports);
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        collectModuleImports(*node, importsVisibleIn(*node), "");
                    }
                }
            },
            item);
    }

    std::function<void(const ast::ModuleDef&)> collectLocalModule = [&](const ast::ModuleDef& mod) {
        m_localModules.insert(mod.name);
        for (const auto& item : mod.body) {
            std::visit(
                [&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        if (node) {
                            collectLocalModule(*node);
                        }
                    }
                },
                item);
        }
    };
    for (const auto& item : program.items) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        collectLocalModule(*node);
                    }
                }
            },
            item);
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
                if (declared == adt.constructorArities.end() || declared->second == 0) {
                    m_nullaryVariants[adt.name].insert(ctor);
                }
                const auto declaredArity = declared == adt.constructorArities.end()
                                               ? 0
                                               : static_cast<size_t>(declared->second);
                m_constructorOwners[ctor].push_back(
                    {adt.name, declaredArity, adt.typeParamCount > 0, adt.module});
                // A module's constructors are in scope by their bare names
                // only inside it or under a `using`, the way this program's
                // own module ADTs are (registerAdtsInModule): registered per
                // module, never in the bare-name tables below.
                if (!adt.module.empty()) {
                    m_moduleConstructors[adt.module].try_emplace(
                        ctor, ModuleConstructor{adt.name, declaredArity, true, true});
                    if (adt.typeParamCount || adt.constructorTypeParamSlots.contains(ctor)) {
                        ConstructorResult result;
                        result.adtName = adt.name;
                        result.typeParamCount = adt.typeParamCount;
                        if (auto slots = adt.constructorTypeParamSlots.find(ctor);
                            slots != adt.constructorTypeParamSlots.end()) {
                            result.slots = slots->second;
                        }
                        result.modulePath = adt.module;
                        m_constructorResult[kex::mangle::qualify(adt.module, ctor)] =
                            std::move(result);
                    }
                    continue;
                }
                auto [owner, fresh] = m_adtOfConstructor.try_emplace(ctor, adt.name);
                if (!fresh && owner->second != adt.name) {
                    ambiguous.insert(ctor);
                    continue;
                }
                auto arity = adt.constructorArities.find(ctor);
                if (arity != adt.constructorArities.end()) {
                    m_constructorArity[ctor] = arity->second;
                    if (arity->second == 0) {
                        m_nullaryConstructors.insert(ctor);
                    }
                }
                if (adt.typeParamCount || adt.constructorTypeParamSlots.contains(ctor)) {
                    ConstructorResult result;
                    result.adtName = adt.name;
                    result.typeParamCount = adt.typeParamCount;
                    if (auto slots = adt.constructorTypeParamSlots.find(ctor);
                        slots != adt.constructorTypeParamSlots.end()) {
                        result.slots = slots->second;
                    }
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

    // The program's own top-level types that reuse a prelude type's name
    // (`record ParseError`): its values carry NamedType::shadowsPrelude,
    // so the two stay different types under one spelling.
    m_preludeShadows.clear();
    m_shadowingConstructors.clear();
    if (m_importedInterfaces) {
        const auto preludeType = [&](const std::string& name) {
            return m_importedInterfaces->recordFields.contains(name) ||
                   m_importedInterfaces->typeAliases.contains(name) ||
                   m_importedInterfaces->distinctTypes.contains(name) ||
                   std::any_of(
                       m_importedInterfaces->adts.begin(), m_importedInterfaces->adts.end(),
                       [&](const ImportedADT& adt) {
                           return adt.module.empty() && adt.name == name;
                       });
        };
        for (const auto& item : program.items) {
            std::visit(
                [&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (
                        std::is_same_v<T, std::unique_ptr<ast::TypeDef>> ||
                        std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                        // Not the standard library's own: checked as a unit of a
                        // program (`using Date` on BEAM), its `record Date` is the
                        // prelude's, and as a shadow every `Date` in date.kex meant
                        // two types ("declared to return Date but body returns Date").
                        if (node && preludeType(node->name) &&
                            !kex::isStandardLibrarySource(node->location.file)) {
                            m_preludeShadows.insert(node->name);
                            if (const auto fields = m_recordFields.find(node->name);
                                fields != m_recordFields.end()) {
                                m_recordFields[preludeRecordKey(node->name)] = fields->second;
                            }
                        }
                    }
                },
                item);
        }
    }

    // The type names the program declares, before anything resolves an
    // annotation (ADT payloads below already do).
    registerDeclaredAliases(program);

    for (const auto& item : program.items) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                    registerAdt(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    registerAdtsInModule(*node);
                }
            },
            item);
    }

    for (const auto& item : program.items) {
        if (const auto* function = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
            function && *function) {
            m_functionDeclarations.emplace((*function)->name, function->get());
        }
    }
    // A module's functions too, private ones included, by their qualified
    // names: `type Handler = Type.of(handlerType)` inside `module Net.HTTP`
    // names `Net.HTTP.handlerType`.
    {
        const auto collect = [&](const auto& self, const auto& items,
                                 const std::string& module) -> void {
            for (const auto& item : items) {
                std::visit(
                    [&](const auto& node) {
                        using T = std::decay_t<decltype(node)>;
                        if (!node) {
                            return;
                        }
                        if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                            if (!module.empty()) {
                                m_functionDeclarations.emplace(
                                    kex::mangle::qualify(module, node->name), node.get());
                            }
                        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                            self(self, node->body, ast::nestedModulePath(module, node->name));
                        } else if constexpr (std::is_same_v<
                                                 T, std::unique_ptr<ast::VisibilityBlock>>) {
                            self(self, node->items, module);
                        }
                    },
                    item);
            }
        };
        collect(collect, program.items, {});
    }
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
    for (const auto name : builtinTypeNames()) {
        m_globals.set(std::string(name), builtinType(name));
    }

    m_globals.set("ENV", Type::map(Type::string(), Type::string()));

    // Imported aliases first; a locally declared name of the same spelling
    // then overwrites it, which is the precedence the rest of the checker uses.
    if (m_importedInterfaces) {
        m_typeAliases = m_importedInterfaces->typeAliases;
    }

    // A slot of a `serving` block compiled on its own is called on its
    // server as one declared here is: `within:` is the call's, not an
    // argument. Only a unit of this program's (`visibleByReceiver`): the
    // prelude's own `make Server<X>` functions (`link`) are no slots.
    if (m_importedInterfaces) {
        for (const auto& [name, functions] : m_importedInterfaces->receiverFunctions) {
            for (const auto& function : functions) {
                if (function.visibleByReceiver && !function.signature.params.empty()) {
                    if (const auto* server =
                            std::get_if<NamedType>(&function.signature.params.front()->kind);
                        server && server->name == kex::names::kServerType &&
                        server->typeArgs.size() == 1) {
                        m_slotMethodNames.insert(name);
                    }
                }
            }
        }
    }

    // Alias bodies must see the canonical built-in types. Resolving an open
    // row alias before this point made its `String` field a nominal name while
    // record declarations later used the real String type; both printed the
    // same but failed invariant row-field comparison.
    registerTypeAliases(program);

    // The imported field types seeded above were spelled against the defining
    // module's aliases and never went through resolveTypeExpr; expand them now
    // that the alias table is complete, so an imported record compares the same
    // way a locally declared one does.
    for (auto& [_, fields] : m_recordFields) {
        for (auto& [__, fieldType] : fields) {
            fieldType = expandTypeAliases(fieldType);
        }
    }

    if (m_importedInterfaces) {
        for (const auto& trait : m_importedInterfaces->traits) {
            m_traits.define(trait);
            // A trait over a constructor (`Functor<F<_>>`) another unit declared:
            // its requirements are functions on any `F<…>`, as a local one's are
            // (registerTraits).
            for (const auto& required : trait.requiredMethods) {
                if (!required.params.empty() && required.params[0] &&
                    std::holds_alternative<TypeApp>(required.params[0]->kind)) {
                    m_higherKindedRequirements[required.name].push_back(required);
                }
            }
        }
    }
    registerTraits(program);
    registerCapabilityTraits(program);
    registerLocalConformances(program);
    collectTypeBounds(program);
    if (m_importedInterfaces) {
        for (const auto& c : m_importedInterfaces->traitConformances) {
            const auto trait = resolveTraitReference(c.traitName, "", nullptr);
            // A type lambda (`make Tagged<_, E>`) another unit declared.
            if (!c.holes.empty()) {
                m_importedConstructorHoles[c.typeName] = c.holes;
            }
            if (c.bounds.empty()) {
                m_traits.registerImplementation(c.typeName, trait);
            } else {
                m_traits.registerConditionalImplementation(c.typeName, trait, c.bounds);
            }
        }
    }
    registerRecordFields(program);
    // Each in its own module: resolved at the top level, `Type.of(handlerType)`
    // named no function, and Net.HTTP's `Handler` was Unknown to every
    // program that imported it.
    for (const auto& alias : m_computedAliases) {
        const auto outer = std::exchange(m_currentModulePath, alias.module);
        m_typeAliases[alias.name] = resolveTypeQuery(*alias.query);
        m_currentModulePath = outer;
    }
    m_computedAliases.clear();
    validateDeclarations(program, m_traits, diagnostics);
    reportPendingUnknownTypes();
    m_topLevelValueNames.clear();
    for (const auto& item : program.items) {
        if (const auto* main = std::get_if<std::unique_ptr<ast::MainBlock>>(&item);
            main && *main && (*main)->synthetic) {
            for (const auto& e : (*main)->body) {
                if (const auto* let = e ? std::get_if<ast::LetExpr>(&e->kind) : nullptr;
                    let && let->pattern) {
                    ast::forEachSubPattern(*let->pattern, [&](const ast::Pattern& bound) {
                        if (const auto* var = std::get_if<ast::VarPattern>(&bound.kind)) {
                            m_topLevelValueNames.insert(var->name);
                        } else if (const auto* typed = std::get_if<ast::TypePattern>(&bound.kind)) {
                            m_topLevelValueNames.insert(typed->name);
                        } else if (
                            const auto* record = std::get_if<ast::RecordPattern>(&bound.kind)) {
                            for (const auto& field : record->fields) {
                                if (!field.pattern) {
                                    m_topLevelValueNames.insert(field.name);
                                }
                            }
                        }
                    });
                }
            }
        }
    }
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
            std::visit(
                [&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        if (n) {
                            userFns.insert(n->name);
                        }
                    }
                },
                item);
        }

        // A top-level value binding (`let frame = Template.html(...)`, a
        // synthetic main holding just that `let`) is a declaration too: a
        // function reading it needs its type, so it takes part in this order
        // under a key no function name can have.
        const auto valueKey = [](const std::string& name) { return "=" + name; };
        std::unordered_map<std::string, size_t> topLevelValues;
        for (size_t i = 0; i < program.items.size(); i++) {
            if (const auto* main = std::get_if<std::unique_ptr<ast::MainBlock>>(&program.items[i]);
                main && *main && (*main)->synthetic && (*main)->body.size() == 1 &&
                (*main)->body.front()) {
                // Every name the pattern binds, not just a lone `let name`: a
                // function reading `scale` from `let (scale, _) = ...` needs
                // its type as much (kexhq/kex#692's fuzz).
                if (const auto* let = std::get_if<ast::LetExpr>(&(*main)->body.front()->kind);
                    let && let->pattern) {
                    ast::forEachSubPattern(*let->pattern, [&](const ast::Pattern& bound) {
                        if (const auto* var = std::get_if<ast::VarPattern>(&bound.kind)) {
                            topLevelValues.try_emplace(var->name, i);
                        } else if (const auto* typed = std::get_if<ast::TypePattern>(&bound.kind)) {
                            topLevelValues.try_emplace(typed->name, i);
                        } else if (
                            const auto* record = std::get_if<ast::RecordPattern>(&bound.kind)) {
                            for (const auto& field : record->fields) {
                                if (!field.pattern) {
                                    topLevelValues.try_emplace(field.name, i);
                                }
                            }
                        }
                    });
                }
            }
        }

        // Collect direct call-dependencies for each FunctionDef.
        // Only user-defined function names are tracked; imported package
        // functions are outside this compilation unit and need no ordering.
        std::function<void(const ast::Expr&, std::set<std::string>&)> collectCalls =
            [&](const ast::Expr& e, std::set<std::string>& out) {
                if (const auto* call = std::get_if<ast::FunctionCall>(&e.kind)) {
                    if (userFns.count(call->name)) {
                        out.insert(call->name);
                    }
                    if (topLevelValues.count(call->name)) {
                        out.insert(valueKey(call->name));
                    }
                } else if (const auto* tagged = std::get_if<ast::TaggedLiteral>(&e.kind)) {
                    if (userFns.count(tagged->tag)) {
                        out.insert(tagged->tag);
                    }
                } else if (const auto* method = std::get_if<ast::MethodCall>(&e.kind)) {
                    if (userFns.count(method->method)) {
                        out.insert(method->method);
                    }
                } else if (const auto* id = std::get_if<ast::Identifier>(&e.kind)) {
                    if (topLevelValues.count(id->name)) {
                        out.insert(valueKey(id->name));
                    }
                }
                ast::forEachChild(e, [&](const ast::ExprPtr& child) { collectCalls(*child, out); });
            };

        std::unordered_map<std::string, std::set<std::string>> deps;
        // Keep a stable list of (name, item-index) for ordering.
        std::vector<std::pair<std::string, size_t>> fnOrder;
        for (size_t i = 0; i < program.items.size(); i++) {
            std::visit(
                [&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        if (!n) {
                            return;
                        }
                        std::set<std::string> d;
                        for (const auto& clause : n->clauses) {
                            for (const auto& ex : clause.body) {
                                if (ex) {
                                    collectCalls(*ex, d);
                                }
                            }
                        }
                        d.erase(n->name); // self-recursion: don't depend on self
                        deps[n->name] = std::move(d);
                        fnOrder.emplace_back(n->name, i);
                    }
                },
                program.items[i]);
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
        std::stable_sort(fnOrder.begin(), fnOrder.end(), [](const auto& left, const auto& right) {
            return left.second < right.second;
        });

        // DFS post-order toposort (handles cycles via visited/onStack sets;
        // back-edge functions are left in original order — pre-registration
        // ensures they still type-check via shared TypeVar unification).
        std::unordered_set<std::string> visited, onStack;
        std::vector<std::string> sorted;
        std::function<void(const std::string&)> dfs = [&](const std::string& name) {
            if (visited.count(name)) {
                return;
            }
            if (onStack.count(name)) {
                return; // back edge: cycle, skip
            }
            onStack.insert(name);
            if (deps.count(name)) {
                for (const auto& dep : deps.at(name)) {
                    dfs(dep);
                }
            }
            onStack.erase(name);
            visited.insert(name);
            sorted.push_back(name);
        };
        for (const auto& [name, _] : fnOrder) {
            dfs(name);
        }

        // Build check order: sorted fn names first (deps before callers),
        // then any non-fn top-level items in their original positions.
        // We do two passes: first check all FunctionDefs in sorted order,
        // then check everything else (types, records, make blocks, main).
        std::unordered_set<std::string> fnChecked;
        std::unordered_set<size_t> valuesChecked;
        for (const auto& name : sorted) {
            if (const auto value = topLevelValues.find(name.substr(1));
                name.starts_with('=') && value != topLevelValues.end()) {
                // Once, however many names its pattern binds.
                if (valuesChecked.insert(value->second).second) {
                    checkTopLevel(program.items[value->second]);
                }
                continue;
            }
            // Check ALL FunctionDefs with this name (handles overloaded functions
            // sharing a name — both overloads must be checked and appended to the
            // same overload set). Mark the name done only after the full sweep.
            for (const auto& item : program.items) {
                std::visit(
                    [&](const auto& n) {
                        using T = std::decay_t<decltype(n)>;
                        if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                            if (n && n->name == name) {
                                checkTopLevel(item);
                            }
                        }
                    },
                    item);
            }
            fnChecked.insert(name);
        }
        // Non-function items and any unchecked functions (e.g. inside modules).
        for (size_t index = 0; index < program.items.size(); index++) {
            if (valuesChecked.count(index)) {
                continue;
            }
            const auto& item = program.items[index];
            std::visit(
                [&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        if (n && !fnChecked.count(n->name)) {
                            checkTopLevel(item);
                        }
                    } else {
                        checkTopLevel(item);
                    }
                },
                item);
        }
    }

    popScope();

    // The method captures, now that what they flowed into is resolved.
    // The capture's own type says it best: a generic parameter's (`A -> Bool`)
    // is the signature's variable, the capture's is what it was used as.
    for (const auto& pending : m_pendingCaptureCalls) {
        auto receiver = pending.receiver;
        if (const auto found = m_typeMap.find(pending.value);
            !isFullyConcrete(resolve(receiver)) && found != m_typeMap.end()) {
            const auto type = expandTypeAliases(resolve(found->second));
            if (const auto* function = std::get_if<FuncType>(&type->kind);
                function && function->params.size() == 1) {
                receiver = function->params.front();
            }
        }
        decideCaptureAsCall(*pending.value, *pending.capture, receiver);
    }
    m_pendingCaptureCalls.clear();

    // `m_typeMap` records each expression's type as it was INFERRED, which for
    // anything inferred before its unification is a bare TypeVar; the concrete
    // type only lives in `m_subst`. Consumers outside this class (IR lowering,
    // via Analyzer::typeMap) have no access to the substitution, so they saw
    // `T1` where the checker itself would say `P` — and BEAM lowering silently
    // sent `let p = this; p.advance` to the prelude instead of the receiver's
    // own `make` block. Resolve once, here, so every consumer reads the answer
    // the checker actually reached. Unbound vars resolve to themselves, so a
    // genuinely un-inferred expression is unchanged.
    for (auto& [expr, type] : m_typeMap) {
        type = resolve(type);
    }
    // Same reasoning for pattern bindings: a binding's type is often a fresh
    // variable at the point the pattern is walked, and only a later use says
    // what it is.
    for (auto& binding : m_patternBindings) {
        binding.type = resolve(binding.type);
    }
    decideOperators(program);

    checkClauseCoverage();
    reportUnknownMethods();
    reportUnmetDemands();
    checkUnannotatedCalls(program);
    recordTypeArguments(program);
}

auto TypeChecker::typeHasFunction(const TypePtr& given, const std::string& name) -> bool {
    const auto type = resolve(given);
    if (const auto* named = std::get_if<NamedType>(&type->kind)) {
        if (const auto record = findRecordFields(*named);
            record != m_recordFields.end() && record->second.contains(name)) {
            return true;
        }
        if (m_importedInterfaces) {
            if (const auto record = m_importedInterfaces->recordFieldNames.find(named->name);
                record != m_importedInterfaces->recordFieldNames.end() &&
                record->second.contains(name)) {
                return true;
            }
        }
    }
    const auto takes = [&](const std::vector<Signature>& signatures) {
        return std::any_of(signatures.begin(), signatures.end(), [&](const Signature& sig) {
            return !sig.params.empty() && argMatchesParam(type, sig.params.front());
        });
    };
    if (const auto methods = m_methodSignatures.find(name);
        methods != m_methodSignatures.end() && takes(methods->second)) {
        return true;
    }
    if (const auto user = m_userSignatures.find(name);
        user != m_userSignatures.end() && takes(user->second)) {
        return true;
    }
    return takes(importedCandidateSignatures(name));
}

auto TypeChecker::reportUnmetDemands() -> void {
    for (const auto& call : m_demandedCalls) {
        const auto demands = m_parameterDemands.find(call.key);
        if (demands == m_parameterDemands.end()) {
            continue;
        }
        // A parameter its function settled on a type (`x.label` binds `x` to the
        // one record with a `label`) is checked as any typed argument is.
        const auto signatures = m_userSignatures.find(call.function);
        const auto stillOpen = [&](std::size_t index) {
            return signatures == m_userSignatures.end() ||
                   std::any_of(
                       signatures->second.begin(), signatures->second.end(),
                       [&](const Signature& sig) {
                           return sig.params.size() == call.argTypes.size() &&
                                  index < sig.params.size() &&
                                  containsOpenType(resolve(sig.params[index]));
                       });
        };
        for (const auto& [index, functions] : demands->second) {
            if (index >= call.argTypes.size() || !call.argTypes[index] || !stillOpen(index)) {
                continue;
            }
            // Only an argument whose type is settled: an open one says nothing yet.
            const auto argument = resolve(call.argTypes[index]);
            if (containsOpenType(argument) || std::holds_alternative<UnionType>(argument->kind)) {
                continue;
            }
            for (const auto& function : functions) {
                if (!typeHasFunction(argument, function)) {
                    error(
                        diag::Code::ArgumentLacksFunction, call.location,
                        {{"name", call.function},
                         {"function", function},
                         {"index", std::to_string(index + 1)},
                         {"type", typeToString(argument)}});
                    break;
                }
            }
        }
    }
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
    auto importedHere = [&](const std::string& moduleName, const std::string& member) {
        if (!callImports) {
            return false;
        }
        auto selected = [](const ImportSelection& import, const std::string& name) {
            return resolve_core::usingSelectsName(name, import.onlyNames, import.exceptNames);
        };
        for (const auto& import : *callImports) {
            const auto& path = import.module;
            if (moduleName == path) {
                if (selected(import, member)) {
                    return true;
                }
            } else if (moduleName.rfind(path + ".", 0) == 0) {
                const auto rest = moduleName.substr(path.size() + 1);
                if (selected(import, rest.substr(0, rest.find('.')))) {
                    return true;
                }
            } else if (path.rfind(moduleName + ".", 0) == 0) {
                return true;
            }
        }
        return false;
    };
    // A module is in reach when the call's imports bring it in, or when it is
    // one of the automatically imported (prelude) ones that need no import.
    auto moduleInReach = [&](const std::string& moduleName, const std::string& member) {
        if (moduleName.empty()) {
            return true;
        }
        if (auto module = m_importedInterfaces->modules.find(moduleName);
            module != m_importedInterfaces->modules.end() && module->second.automaticImport) {
            return true;
        }
        return importedHere(moduleName, member);
    };
    auto knownSomewhere = [&](const std::string& name, bool fieldMayExist,
                              const TypePtr& receiverType) {
        if (m_methodSignatures.count(name) || m_userSignatures.count(name) ||
            m_annotatedMethods.count(name) || m_makeMethodNames.count(name)) {
            return true;
        }
        if (fieldMayExist) {
            for (const auto& [record, fields] : m_recordFields) {
                (void)record;
                if (fields.count(name)) {
                    return true;
                }
            }
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
            if (traitName.find('.') != std::string::npos && m_capabilities.count(traitName) > 0 &&
                !moduleInReach(traitName, name)) {
                continue;
            }
            // Nor may a trait vouch for a receiver whose type is known and
            // does not implement it: `message` is Errorable's, so
            // `result.message` on a Result checked clean, then died with
            // `if_clause` on BEAM (kexhq/kex#738). A receiver that may be
            // anything (fieldMayExist) keeps the name-only answer.
            if (!fieldMayExist && receiverType && !m_traits.satisfies(receiverType, traitName)) {
                continue;
            }
            for (const auto& required : trait.requiredMethods) {
                if (required.name == name) {
                    return true;
                }
            }
            for (const auto& defaulted : trait.defaultMethods) {
                if (defaulted == name) {
                    return true;
                }
            }
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
                std::any_of(
                    providers->second.begin(), providers->second.end(),
                    [&](const ImportedFunction& provider) {
                        return moduleInReach(provider.sourceModule, name);
                    })) {
                return true;
            }
            for (const auto& [moduleName, module] : m_importedInterfaces->modules) {
                // Only a module whose members COULD be in scope may suppress
                // the diagnostic. Every known module used to, imported or not,
                // so any member name of any opt-in stdlib module passed as a
                // method on any receiver: `box.sha256`, `box.shiftLeft(1)`,
                // `headers.set("a", "b")` all checked clean and then died at
                // runtime with "Undefined method" (rodolfo docs/kex-issues.md
                // #5). A module the call's own imports bring in still
                // suppresses — and so does a prelude one, which needs no import.
                if (!moduleInReach(moduleName, name)) {
                    continue;
                }
                if (module.exports.count(name)) {
                    return true;
                }
            }
            if (fieldMayExist) {
                for (const auto& [record, fields] : m_importedInterfaces->recordFieldNames) {
                    (void)record;
                    if (fields.count(name)) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    for (size_t i = 0; i < m_unresolvedMethods.size(); ++i) {
        const auto& unresolved = m_unresolvedMethods[i];
        callImports =
            i < m_unresolvedMethodImports.size() ? &m_unresolvedMethodImports[i] : nullptr;
        // Checked here, not at the call: the guess may come later in the file.
        if (knownSomewhere(
                unresolved.name,
                unresolved.fieldMayExist || m_guessedReceiverTypes.count(unresolved.receiver),
                unresolved.receiverType)) {
            continue;
        }
        if (unresolved.receiver.empty() || unresolved.receiver == kex::names::kUnknownType) {
            error(diag::Code::UndefinedMethod, unresolved.location, {{"name", unresolved.name}});
        } else {
            error(
                diag::Code::UndefinedMethodFor, unresolved.location,
                {{"name", unresolved.name}, {"receiver", unresolved.receiver}});
        }
    }
    m_unresolvedMethods.clear();
    m_unresolvedMethodImports.clear();
}

// `namesOnly` runs the same traversal for its record IDENTITIES alone. Type
// aliases are registered before field types can be resolved (a field may BE an
// alias), yet an alias body names records too — so the names have to exist
// first, and the field types are filled in on the second pass.
auto TypeChecker::registerRecordFields(const ast::Program& program, bool namesOnly) -> void {
    // The unique-suffix guesses (resolveRecordName) depend on the records known.
    m_recordSuffixes.clear();
    auto registerRecord = [&](const ast::RecordDef& record,
                              const std::string& owner = std::string{}) {
        const auto name = owner.empty() ? record.name : owner + "." + record.name;
        auto& fields = m_recordFields[name];
        m_programRecords.insert(name);
        m_declaredTypeArity[name] = record.typeParams.size();
        if (namesOnly) {
            return;
        }
        const auto previousModule = m_currentModulePath;
        m_currentModulePath = owner;
        // A field's type is written against the names its module imported.
        const auto previousImports = m_declarationImports;
        if (auto imports = m_recordImports.find(&record); imports != m_recordImports.end()) {
            m_declarationImports = imports->second;
        }
        // `record Box<A>` binds A to the parameter slot -1 for the whole
        // declaration, the convention `substituteInterfaceGenerics` reads and
        // the one the KexI reader rebuilds imported records with. A map
        // shared across ALL records — which this used to be — did neither:
        // every record's `A` collapsed onto one fresh positive TypeVar, so a
        // field's element type could never be recovered from a receiver's
        // type arguments (kexhq/kex#203).
        std::unordered_map<std::string, TypePtr> generics;
        for (size_t i = 0; i < record.typeParams.size(); ++i) {
            generics[record.typeParams[i]] = Type::genericLetter(-static_cast<int>(i + 1));
        }
        for (const auto& field : record.fields) {
            auto fieldType = field.type ? resolveTypeExpr(*field.type, generics) : Type::unknown();
            fields[field.name] = fieldType;
            if ((!field.defaultValue || !*field.defaultValue) &&
                !std::holds_alternative<OptionalType>(fieldType->kind)) {
                m_requiredRecordFields[name].insert(field.name);
            }
        }
        m_currentModulePath = previousModule;
        m_declarationImports = previousImports;
    };
    std::function<void(const ast::ModuleDef&)> registerModule;
    registerModule = [&](const ast::ModuleDef& module) {
        for (const auto& item : module.body) {
            std::visit(
                [&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                        if (node) {
                            registerRecord(*node, module.name);
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                        // A `private do ... end` record is still the module's own
                        // record, and its identity has to be the qualified one its
                        // module constructs.
                        if (!node) {
                            return;
                        }
                        for (const auto& visible : node->items) {
                            if (const auto* record =
                                    std::get_if<std::unique_ptr<ast::RecordDef>>(&visible);
                                record && *record) {
                                registerRecord(**record, module.name);
                            }
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        if (node) {
                            registerModule(*node);
                        }
                    }
                },
                item);
        }
    };
    for (const auto& item : program.items) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                    if (node) {
                        registerRecord(*node);
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (!node) {
                        return;
                    }
                    for (const auto& visible : node->items) {
                        if (const auto* record =
                                std::get_if<std::unique_ptr<ast::RecordDef>>(&visible);
                            record && *record) {
                            registerRecord(**record);
                        }
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        registerModule(*node);
                    }
                }
            },
            item);
    }
}

auto TypeChecker::aliasReachedAs(const std::string& written) const -> std::optional<TypePtr> {
    std::set<std::string> found;
    for (auto scope = m_currentModulePath; !scope.empty();) {
        if (const auto candidate = scope + "." + written; m_typeAliases.contains(candidate)) {
            found.insert(candidate);
            break;
        }
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) {
            break;
        }
        scope.resize(dot);
    }
    const auto through = [&](const ImportSelection& import) {
        if (import.alias) {
            if (written.starts_with(*import.alias + ".")) {
                if (const auto candidate = import.module + written.substr(import.alias->size());
                    m_typeAliases.contains(candidate)) {
                    found.insert(candidate);
                }
            }
            return;
        }
        if (!resolve_core::usingSelectsName(
                written.substr(0, written.find('.')), import.onlyNames, import.exceptNames)) {
            return;
        }
        if (const auto candidate = import.module + "." + written;
            m_typeAliases.contains(candidate)) {
            found.insert(candidate);
        }
    };
    for (const auto& import : m_declarationImports) {
        through(import);
    }
    for (const auto& scope : m_importScopeStack) {
        for (const auto& import : scope) {
            through(import);
        }
    }
    if (found.size() != 1) {
        return std::nullopt;
    }
    return m_typeAliases.at(*found.begin());
}

auto TypeChecker::qualifiedNominalType(const std::string& qualified) const
    -> std::optional<std::string> {
    auto nominal = [&](const std::string& candidate) {
        return m_recordFields.count(candidate) || m_adtVariants.count(candidate) ||
               (m_importedInterfaces && m_importedInterfaces->recordArities.count(candidate));
    };
    // The spelling in full names exactly one type, whatever the module being
    // checked declares under the same last segment.
    if (nominal(qualified)) {
        return qualified;
    }
    auto resolved = resolveRecordName(qualified);
    if (nominal(resolved)) {
        return resolved;
    }
    // A path relative to where it is written: an enclosing module's
    // (`Inner.Box` inside `module Outer`), or one a `using` reaches
    // (`Server.Running` under `using Net.HTTP`, `H.Server.Running` under
    // `using Net.HTTP, as: H`). An opaque `type` is a type here as much as a
    // record. Exactly one meaning, or none.
    const auto declared = [&](const std::string& candidate) {
        return nominal(candidate) || m_moduleTypeIdentities.count(candidate) ||
               m_declaredTypeNames.count(candidate) ||
               (m_importedInterfaces && m_importedInterfaces->typeNames.count(candidate));
    };
    std::set<std::string> found;
    for (auto scope = m_currentModulePath; !scope.empty();) {
        if (const auto candidate = scope + "." + qualified; declared(candidate)) {
            found.insert(candidate);
            break;
        }
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) {
            break;
        }
        scope.resize(dot);
    }
    const auto through = [&](const ImportSelection& import) {
        if (import.alias) {
            if (qualified.starts_with(*import.alias + ".")) {
                if (const auto candidate = import.module + qualified.substr(import.alias->size());
                    declared(candidate)) {
                    found.insert(candidate);
                }
            }
            return;
        }
        if (const auto candidate = import.module + "." + qualified; declared(candidate)) {
            found.insert(candidate);
        }
    };
    for (const auto& import : m_declarationImports) {
        through(import);
    }
    for (const auto& scope : m_importScopeStack) {
        for (const auto& import : scope) {
            through(import);
        }
    }
    if (found.size() == 1) {
        return *found.begin();
    }
    return std::nullopt;
}

auto TypeChecker::lacksBuiltinOperator(const TypePtr& type, const std::string& op) const -> bool {
    // A tuple, map, optional, open record or function has no arithmetic,
    // order or negation; a list only concatenates. The walker said "Cannot
    // add Tuple and Tuple" at run time, BEAM `function_clause`.
    if (std::holds_alternative<TupleType>(type->kind) ||
        std::holds_alternative<MapType>(type->kind) ||
        std::holds_alternative<RecordType>(type->kind) ||
        std::holds_alternative<OptionalType>(type->kind) ||
        std::holds_alternative<FuncType>(type->kind)) {
        return true;
    }
    if (std::holds_alternative<ListType>(type->kind)) {
        return op != "+";
    }
    // A distinct type keeps exactly the operators of what it erases to:
    // `distinct type Cash = Integer` adds, `= (Number, Number)` does not.
    if (const auto backing = distinctBacking(type); backing && !typesEqual(backing, type)) {
        return lacksBuiltinOperator(backing, op);
    }
    if (const auto* bound = std::get_if<ConstrainedType>(&type->kind)) {
        const auto* trait = m_traits.lookup(bound->traitName);
        return trait && !primitiveNumberTrait(trait->name);
    }
    const auto* named = std::get_if<NamedType>(&type->kind);
    if (!named) {
        return false;
    }
    // Imported ones too: `Duration * Duration` has no operator in the
    // prelude, and died at run time like any record of the program's own.
    const auto name = resolveRecordName(named->name);
    if (m_recordFields.count(name) > 0 || m_adtVariants.count(name) > 0 ||
        (m_importedInterfaces && m_importedInterfaces->recordArities.count(name) > 0)) {
        return true;
    }
    const auto* trait = m_traits.lookup(named->name);
    return trait && !primitiveNumberTrait(trait->name);
}

auto TypeChecker::traitDeclaresOperator(const TypePtr& type, const std::string& op) const -> bool {
    const auto* bound = std::get_if<ConstrainedType>(&type->kind);
    const auto* named = std::get_if<NamedType>(&type->kind);
    if (!bound && !named) {
        return false;
    }
    if (const auto* trait = m_traits.lookup(bound ? bound->traitName : named->name)) {
        for (const auto& required : trait->requiredMethods) {
            if (required.name == op) {
                return true;
            }
        }
        return std::find(trait->defaultMethods.begin(), trait->defaultMethods.end(), op) !=
               trait->defaultMethods.end();
    }
    return m_traits.declaresMethod(m_traits.implementorKey(type), op);
}

auto TypeChecker::constructorPatternType(const ast::Pattern& pattern) const -> std::string {
    const auto* ctor = std::get_if<ast::ConstructorPattern>(&pattern.kind);
    const auto owner = ctor ? m_adtOfConstructor.find(ctor->name) : m_adtOfConstructor.end();
    if (owner == m_adtOfConstructor.end()) {
        return {};
    }
    // Only a type without parameters: `Just(x)` says nothing of the `T` in
    // `T?`, which a bare name would lose.
    if (const auto arity = m_declaredTypeArity.find(owner->second);
        arity != m_declaredTypeArity.end() && arity->second > 0) {
        return {};
    }
    if (m_importedInterfaces) {
        if (const auto arity = m_importedInterfaces->typeArities.find(owner->second);
            arity != m_importedInterfaces->typeArities.end() && arity->second > 0) {
            return {};
        }
    }
    return owner->second;
}

auto TypeChecker::outOfStack(SourceLocation location) -> bool {
    if (!kex::stack::nearEnd()) {
        return false;
    }
    // Once: the frames unwinding past this one would each say it again.
    if (!m_reportedOutOfStack) {
        m_reportedOutOfStack = true;
        error(diag::Code::NestedTooDeepToCheck, location);
    }
    return true;
}

auto TypeChecker::traitOperatorResult(
    const TypePtr& receiver, const std::string& op, const TypePtr& right, SourceLocation loc)
    -> TypePtr {
    const auto* bound = std::get_if<ConstrainedType>(&receiver->kind);
    const auto* named = std::get_if<NamedType>(&receiver->kind);
    const auto* trait = bound   ? m_traits.lookup(bound->traitName)
                        : named ? m_traits.lookup(named->name)
                                : nullptr;
    if (!trait) {
        return nullptr;
    }
    for (const auto& required : trait->requiredMethods) {
        if (required.name != op || requirementArity(required) != 2 || !required.result) {
            continue;
        }
        const auto taken = resolve(bindThis(required.params.back(), receiver));
        if (isConcreteOrBound(taken) && !argMatchesParam(right, taken)) {
            error(
                diag::Code::NoOverloadArgumentMismatch, loc,
                {{"name", op},
                 {"index", "2"},
                 {"expected", typeToString(taken)},
                 {"actual", typeToString(resolve(right))},
                 {"signatures", ""}});
            return Type::unknown();
        }
        return bindThis(required.result, receiver);
    }
    return nullptr;
}

auto TypeChecker::unaryOperatorResult(const ast::Expr& expr, const TypePtr& operand) -> TypePtr {
    const std::string op = "-";
    if (std::holds_alternative<TypeVar>(operand->kind) ||
        std::holds_alternative<UnknownType>(operand->kind) ||
        (std::holds_alternative<PrimitiveType>(operand->kind) && isNumericType(operand))) {
        return nullptr;
    }
    // Only the receiver: `let -()`, or `- : This -> This`.
    const auto unary = [&](const Signature& signature) {
        if (signature.params.size() != 1) {
            return false;
        }
        const auto receiver = resolve(signature.params.front());
        return !std::holds_alternative<TypeVar>(receiver->kind) &&
               !std::holds_alternative<UnknownType>(receiver->kind) &&
               argMatchesParam(operand, receiver);
    };
    const auto decide = [&](const Signature& signature, const ImportedFunction* imported) {
        DecidedCall decided{CallKind::Operator, typeToString(operand), op};
        if (imported) {
            decided.backendModule = imported->backendModule;
            decided.function = imported->backendFunction;
        }
        m_decidedCalls[&expr] = std::move(decided);
        return signature.result ? bindThis(signature.result, operand) : Type::unknown();
    };
    if (const auto local = m_methodSignatures.find(op); local != m_methodSignatures.end()) {
        for (const auto& signature : local->second) {
            if (unary(signature)) {
                return decide(signature, nullptr);
            }
        }
    }
    for (const auto* imported : importedCandidates(op, operand)) {
        if (unary(imported->signature)) {
            return decide(imported->signature, imported);
        }
    }
    // A trait's requirement, answered by the value's type when it runs.
    const auto* bound = std::get_if<ConstrainedType>(&operand->kind);
    const auto* named = std::get_if<NamedType>(&operand->kind);
    if (const auto* trait = bound   ? m_traits.lookup(bound->traitName)
                            : named ? m_traits.lookup(named->name)
                                    : nullptr) {
        for (const auto& required : trait->requiredMethods) {
            if (required.name == op && requirementArity(required) == 1) {
                m_decidedCalls[&expr] = {CallKind::Dispatch, {}, op};
                return required.result ? bindThis(required.result, operand) : Type::unknown();
            }
        }
    }
    return nullptr;
}

auto TypeChecker::declaresOperatorFor(const TypePtr& type, const std::string& op) const -> bool {
    if (traitDeclaresOperator(type, op)) {
        return true;
    }
    auto owns = [&](const Signature& signature) {
        if (signature.params.size() != 2) {
            return false;
        }
        const auto receiver = resolve(signature.params[0]);
        return !std::holds_alternative<TypeVar>(receiver->kind) &&
               !std::holds_alternative<UnknownType>(receiver->kind) &&
               argMatchesParam(type, signature.params[0]);
    };
    if (auto local = m_methodSignatures.find(op); local != m_methodSignatures.end()) {
        if (std::any_of(local->second.begin(), local->second.end(), owns)) {
            return true;
        }
    }
    const auto imported = importedCandidateSignatures(op);
    return std::any_of(imported.begin(), imported.end(), owns);
}

auto TypeChecker::resolveRecordName(const std::string& name) const -> std::string {
    // ADTs share the records' identity rule: one declared inside
    // `module Kex` is `Kex.Backend`, so it can never be mistaken for a
    // same-spelled type or constructor declared anywhere else.
    auto recordExists = [&](const std::string& candidate) {
        return m_recordFields.count(candidate) || m_adtVariants.count(candidate) ||
               (m_importedInterfaces && m_importedInterfaces->recordArities.count(candidate));
    };
    if (auto scoped = resolve_core::resolveInScopes(m_currentModulePath, name, recordExists)) {
        return *scoped;
    }
    auto importedRecord = [&](const ImportSelection& import) -> std::optional<std::string> {
        const auto candidate = import.module + "." + name;
        if (!recordExists(candidate)) {
            return std::nullopt;
        }
        if (!resolve_core::usingSelectsName(name, import.onlyNames, import.exceptNames)) {
            return std::nullopt;
        }
        return candidate;
    };
    for (auto scope = m_importScopeStack.rbegin(); scope != m_importScopeStack.rend(); ++scope) {
        for (auto import = scope->rbegin(); import != scope->rend(); ++import) {
            if (auto found = importedRecord(*import)) {
                return *found;
            }
        }
    }
    for (auto import = m_declarationImports.rbegin(); import != m_declarationImports.rend();
         ++import) {
        if (auto found = importedRecord(*import)) {
            return *found;
        }
    }
    // A record declared in this file under the bare name owns that name. The
    // last-ditch suffix scan below guesses at a module nobody imported, and a
    // guess must never outrank a declaration sitting in front of the user:
    // stdlib records live in m_importedInterfaces whether or not they were
    // imported, so `record Request` in a plain script resolved to
    // `Net.HTTP.Request` and reported its fields instead.
    // So does any other type declared under that name: an alias (`type Name
    // = String`), a trait, a distinct type.
    if (m_recordFields.count(name) || m_adtVariants.count(name) || m_typeAliases.count(name) ||
        m_traits.get(name) || m_distinctTypes.count(name)) {
        return name;
    }
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
            if (match.kind == resolve_core::SuffixMatchKind::Unique) {
                cached->second = match.name;
            } else if (match.kind == resolve_core::SuffixMatchKind::Ambiguous) {
                cached->second = name;
            }
        }
        if (cached->second) {
            return *cached->second;
        }
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
            for (auto scope = m_importScopeStack.rbegin(); scope != m_importScopeStack.rend();
                 ++scope) {
                for (auto import = scope->rbegin(); import != scope->rend(); ++import) {
                    if (import->alias && *import->alias == prefix) {
                        return import->module;
                    }
                }
            }
            for (auto import = m_declarationImports.rbegin(); import != m_declarationImports.rend();
                 ++import) {
                if (import->alias && *import->alias == prefix) {
                    return import->module;
                }
            }
            if (auto reexport = m_moduleReexports.find(prefix);
                reexport != m_moduleReexports.end()) {
                return reexport->second;
            }
            return std::nullopt;
        };
        for (size_t prefixEnd = dot; prefixEnd != std::string::npos;
             prefixEnd = prefixEnd == 0 ? std::string::npos : name.rfind('.', prefixEnd - 1)) {
            if (auto target = aliasFor(name.substr(0, prefixEnd))) {
                const auto aliased = *target + name.substr(prefixEnd);
                if (recordExists(aliased)) {
                    return aliased;
                }
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
auto TypeChecker::resolveModulePath(const std::string& name, const std::string& member) const
    -> std::string {
    auto resolved = resolveImportedModulePath(name, member);
    const auto exportsMember = [&](const std::string& module) {
        if (m_localModules.contains(module) &&
            m_userSignatures.contains(kex::mangle::qualify(module, member))) {
            return true;
        }
        if (!m_importedInterfaces) {
            return false;
        }
        auto found = m_importedInterfaces->modules.find(module);
        if (found == m_importedInterfaces->modules.end()) {
            return false;
        }
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
                return candidate != resolved && m_localModules.contains(candidate) &&
                       exportsMember(candidate);
            })) {
        return *nested;
    }
    return resolved;
}

auto TypeChecker::resolveImportedModulePath(
    const std::string& name, const std::string& member) const -> std::string {
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
            m_userSignatures.contains(kex::mangle::qualify(module, member))) {
            return true;
        }
        if (!m_importedInterfaces) {
            return false;
        }
        auto found = m_importedInterfaces->modules.find(module);
        if (found == m_importedInterfaces->modules.end()) {
            return false;
        }
        auto exported = found->second.exports.find(member);
        return exported != found->second.exports.end() && !exported->second.empty();
    };
    const auto canonicalModule = [&](const std::string& module) {
        if (exportsMember(module)) {
            return module;
        }
        const auto backend = names::moduleAtom(module);
        return exportsMember(backend) ? backend : module;
    };
    if (exportsMember(canonicalModule(name))) {
        return canonicalModule(name);
    }
    // `using M, as: A` binds `A` (and `A.Child`) to `M`. Innermost scope
    // first, so a block-level alias shadows a file-level one.
    auto aliased = [&](const ImportSelection& import) -> std::optional<std::string> {
        if (!import.alias) {
            return std::nullopt;
        }
        const auto& alias = *import.alias;
        if (name != alias && !name.starts_with(alias + ".")) {
            return std::nullopt;
        }
        const auto target = canonicalModule(import.module + name.substr(alias.size()));
        return exportsMember(target) ? std::optional<std::string>{target} : std::nullopt;
    };
    for (auto scope = m_importScopeStack.rbegin(); scope != m_importScopeStack.rend(); ++scope) {
        for (const auto& import : *scope) {
            if (auto target = aliased(import)) {
                return *target;
            }
        }
    }
    for (const auto& import : m_declarationImports) {
        if (auto target = aliased(import)) {
            return *target;
        }
    }
    auto lastSegmentOf = [](const std::string& module) {
        const auto dot = module.rfind('.');
        return dot == std::string::npos ? module : module.substr(dot + 1);
    };
    auto consider = [&](const ImportSelection& import, std::vector<std::string>& candidates) {
        if (import.alias) {
            return;
        }
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
        if (lastSegmentOf(module) == name && exportsMember(canonicalModule(module))) {
            candidates.push_back(canonicalModule(module));
        }
    };
    std::vector<std::string> candidates;
    for (const auto& import : m_declarationImports) {
        consider(import, candidates);
    }
    for (const auto& scope : m_importScopeStack) {
        for (const auto& import : scope) {
            consider(import, candidates);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.size() == 1) {
        return candidates.front();
    }
    if (!candidates.empty()) {
        return name;
    }

    if (!m_importedInterfaces) {
        return name;
    }
    std::optional<std::string> unique;
    const auto suffix = "." + name;
    for (const auto& [candidate, _] : m_importedInterfaces->modules) {
        if (candidate.size() <= suffix.size() ||
            candidate.compare(candidate.size() - suffix.size(), suffix.size(), suffix) != 0 ||
            !exportsMember(candidate)) {
            continue;
        }
        if (unique && *unique != candidate) {
            return name;
        }
        unique = candidate;
    }
    return unique.value_or(name);
}

auto TypeChecker::registerAdt(const ast::TypeDef& def, const std::string& modulePath) -> void {
    if (!def.variants) {
        return;
    }

    if (def.isDistinct || kex::isTransparentTypeAlias(def)) {
        return;
    }

    std::vector<std::string> names;
    std::unordered_set<std::string> nullary;
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) {
            return; // not constructor-shaped — a type alias, skip entirely
        }
        names.push_back(*name);
        if (std::holds_alternative<ast::TypeName>(variant->kind)) {
            nullary.insert(*name);
        }
        if (!modulePath.empty()) {
            continue;
        }
        if (std::holds_alternative<ast::TypeName>(variant->kind)) {
            m_nullaryConstructors.insert(*name);
        }
        // Payload arity: `None` is 0, `Just(T)` is 1, `Between(A, B)` is 2.
        if (auto* gt = std::get_if<ast::GenericType>(&variant->kind)) {
            m_constructorArity[*name] = gt->args.size();
        } else {
            m_constructorArity[*name] = 0;
        }
    }
    if (names.empty()) {
        return;
    }

    // Inside a module the ADT's identity is qualified, like a record's. Its
    // constructors stay out of the bare-name owner map: `Kex.Backend`'s
    // `Beam` is reached through the module, never by spelling alone.
    const auto identity = modulePath.empty() ? def.name : modulePath + "." + def.name;
    if (modulePath.empty() && m_preludeShadows.contains(def.name)) {
        m_shadowingConstructors.insert(names.begin(), names.end());
    }
    m_adtVariants[identity] = names;
    m_adtDeclarations[identity] = def.location;
    m_nullaryVariants[identity] = std::move(nullary);
    m_declaredTypeArity[identity] = def.typeParams.size();
    if (modulePath.empty()) {
        for (const auto& name : names) {
            m_adtOfConstructor[name] = def.name;
        }
    }

    // Record what each constructor produces, so `Just(1)` infers as an
    // Optional instead of `unknown` — without that, a mismatched pattern in
    // `if let Ok(x) = Just(1)` has no scrutinee type to be checked against.
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) {
            continue;
        }
        ConstructorResult result;
        result.adtName = identity;
        result.typeParamCount = def.typeParams.size();
        if (const auto* generic = std::get_if<ast::GenericType>(&variant->kind)) {
            for (const auto& payload : generic->args) {
                int slot = -1;
                if (payload) {
                    if (const auto* named = std::get_if<ast::TypeName>(&payload->kind);
                        named && named->parts.size() == 1) {
                        auto found = std::find(
                            def.typeParams.begin(), def.typeParams.end(), named->parts.front());
                        if (found != def.typeParams.end()) {
                            slot = static_cast<int>(found - def.typeParams.begin());
                        }
                    } else if (const auto* var = std::get_if<ast::GenericVar>(&payload->kind)) {
                        auto found =
                            std::find(def.typeParams.begin(), def.typeParams.end(), var->name);
                        if (found != def.typeParams.end()) {
                            slot = static_cast<int>(found - def.typeParams.begin());
                        }
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
                    if (declared && std::holds_alternative<UnknownType>(declared->kind)) {
                        declared = nullptr;
                    }
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
auto TypeChecker::siblingConstructorJoin(const TypePtr& a, const TypePtr& b) const -> TypePtr {
    const auto left = resolve(a);
    const auto right = resolve(b);
    // Where the two differ only by sibling variants, inside a tuple, list or
    // optional as well: `[(Name, "a"), (Title, "b")]` is a list of
    // `(Detail, String)`. Parts that are the same type stay as they are.
    const auto part = [&](const TypePtr& x, const TypePtr& y) -> TypePtr {
        if (typesEqual(x, y)) {
            return x;
        }
        return siblingConstructorJoin(x, y);
    };
    if (const auto* l = std::get_if<TupleType>(&left->kind)) {
        if (const auto* r = std::get_if<TupleType>(&right->kind);
            r && l->elements.size() == r->elements.size()) {
            std::vector<TypePtr> elements;
            for (std::size_t i = 0; i < l->elements.size(); ++i) {
                auto joined = part(l->elements[i], r->elements[i]);
                if (!joined) {
                    return nullptr;
                }
                elements.push_back(std::move(joined));
            }
            return Type::tuple(std::move(elements));
        }
    }
    if (const auto* l = std::get_if<ListType>(&left->kind)) {
        if (const auto* r = std::get_if<ListType>(&right->kind)) {
            if (auto joined = part(l->element, r->element)) {
                return Type::list(std::move(joined));
            }
        }
    }
    if (const auto* l = std::get_if<OptionalType>(&left->kind)) {
        if (const auto* r = std::get_if<OptionalType>(&right->kind)) {
            if (auto joined = part(l->inner, r->inner)) {
                return Type::optional(std::move(joined));
            }
        }
    }
    const auto* leftNamed = std::get_if<NamedType>(&left->kind);
    const auto* rightNamed = std::get_if<NamedType>(&right->kind);
    if (!leftNamed || !rightNamed || !leftNamed->typeArgs.empty() ||
        !rightNamed->typeArgs.empty()) {
        return nullptr;
    }
    const auto* leftOwner = constructorOwner(*leftNamed);
    const auto* rightOwner = constructorOwner(*rightNamed);
    if (!leftOwner || !rightOwner || *leftOwner != *rightOwner) {
        return nullptr;
    }
    return Type::named(*leftOwner);
}

auto TypeChecker::scopedConstructorOwner(const std::string& name) const -> const ConstructorOwner* {
    // A spelling with one owner is already in m_adtOfConstructor.
    if (m_adtOfConstructor.count(name)) {
        return nullptr;
    }
    const auto owners = m_constructorOwners.find(name);
    if (owners == m_constructorOwners.end()) {
        return nullptr;
    }
    auto visible = [&](const std::string& adt) {
        const auto dot = adt.rfind('.');
        // A top-level stdlib ADT is file-level prelude: always in scope.
        if (dot == std::string::npos) {
            return true;
        }
        const auto module = adt.substr(0, dot);
        if (m_currentModulePath == module || m_currentModulePath.starts_with(module + ".")) {
            return true;
        }
        if (moduleMemberImported(module, name)) {
            return true;
        }
        if (!m_importedInterfaces) {
            return false;
        }
        const auto imported = m_importedInterfaces->modules.find(module);
        return imported != m_importedInterfaces->modules.end() && imported->second.automaticImport;
    };
    const ConstructorOwner* chosen = nullptr;
    for (const auto& owner : owners->second) {
        if (!visible(owner.adt)) {
            continue;
        }
        if (chosen && chosen->adt != owner.adt) {
            return nullptr;
        }
        chosen = &owner;
    }
    return chosen;
}

auto TypeChecker::constructorOwner(const NamedType& named) const -> const std::string* {
    if (!named.typeArgs.empty()) {
        return nullptr;
    }
    if (!named.variant.empty()) {
        return &named.name;
    }
    auto owner = m_adtOfConstructor.find(named.name);
    if (owner == m_adtOfConstructor.end()) {
        // A constructor of a module's ADT — `Twomod.Access`'s `Private` — is
        // owned only where that module is in scope. Without it, `Private`
        // and `Public` did not join in a `then … else` (kexhq/kex#504) and
        // `Type.of(Public)` was not widened to its type (#505).
        if (const auto* scoped = scopedConstructorOwner(named.name)) {
            return &scoped->adt;
        }
        const std::string* found = nullptr;
        for (const auto& [module, constructors] : m_moduleConstructors) {
            const auto constructor = constructors.find(named.name);
            if (constructor == constructors.end() || constructor->second.typeName.empty()) {
                continue;
            }
            if (!withinModule(module) && !moduleMemberImported(module, named.name)) {
                continue;
            }
            if (found && *found != constructor->second.typeName) {
                return nullptr;
            }
            found = &constructor->second.typeName;
        }
        return found;
    }
    // `Kex.Backend` is registered as the ADT `Backend`, and `Backend` is also
    // a nullary NetErrorKind constructor. Looked up by spelling alone, every
    // `Backend` value became a NetErrorKind.
    if (owner->second != named.name &&
        (m_adtVariants.count(named.name) || m_typeAliases.count(named.name) ||
         isPrimitiveTypeName(named.name))) {
        return nullptr;
    }
    return &owner->second;
}

auto TypeChecker::constructorInfo(const std::string& name, const std::string& scope) const
    -> const ConstructorResult* {
    auto found = scope.empty() ? m_constructorResult.end()
                               : m_constructorResult.find(kex::mangle::qualify(scope, name));
    if (!scope.empty()) {
        return found == m_constructorResult.end() ? nullptr : &found->second;
    }
    // A constructor declared in this lexical module shadows an imported
    // constructor with the same spelling (for example a local Done versus
    // Control.Retry's Done). Do not let stdlib interfaces choose its ADT.
    for (auto module = scope.empty() ? m_currentModulePath : std::string{}; !module.empty();) {
        found = m_constructorResult.find(kex::mangle::qualify(module, name));
        if (found != m_constructorResult.end()) {
            break;
        }
        const auto dot = module.rfind('.');
        if (dot == std::string::npos) {
            break;
        }
        module.resize(dot);
    }
    if (found == m_constructorResult.end()) {
        found = m_constructorResult.find(name);
    }
    // A constructor of a module of this program that a `using` brings in
    // bare (`WantStore(pid)` under `using Store`), where one module does.
    if (found == m_constructorResult.end() && !name.empty() && kex::startsUppercase(name)) {
        for (const auto& [module, constructors] : m_moduleConstructors) {
            if (!constructors.count(name) || !moduleMemberImported(module, name, true)) {
                continue;
            }
            const auto imported = m_constructorResult.find(kex::mangle::qualify(module, name));
            if (imported == m_constructorResult.end()) {
                continue;
            }
            if (found != m_constructorResult.end()) {
                return nullptr;
            }
            found = imported;
        }
    }
    return found == m_constructorResult.end() ? nullptr : &found->second;
}

auto TypeChecker::declaredPayload(const ConstructorResult& info, std::size_t i) -> TypePtr {
    auto payload = i < info.payloadTypes.size() ? info.payloadTypes[i] : nullptr;
    if (i >= info.payloadExprs.size() || !info.payloadExprs[i]) {
        return payload;
    }
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = info.modulePath;
    std::unordered_map<std::string, TypePtr> generics;
    auto resolved = resolveTypeExpr(*info.payloadExprs[i], generics);
    m_currentModulePath = previousModule;
    if (resolved && !std::holds_alternative<UnknownType>(resolved->kind)) {
        return resolved;
    }
    return payload;
}

auto TypeChecker::checkConstructorPayloads(
    const std::string& name,
    const std::vector<TypePtr>& argTypes,
    SourceLocation loc,
    const std::string& scope) -> void {
    const auto* info = constructorInfo(name, scope);
    if (!info) {
        return;
    }
    // How many values, wherever the constructor is declared: `Just()` was
    // accepted, and failed on BEAM with a pattern mismatch. An imported
    // one's payload types are not kept, its arity is.
    std::optional<size_t> declaredArity;
    if (info->declaredHere) {
        declaredArity = info->payloadTypes.size();
    } else if (!info->modulePath.empty()) {
        if (const auto module = m_moduleConstructors.find(info->modulePath);
            module != m_moduleConstructors.end()) {
            if (const auto found = module->second.find(name); found != module->second.end()) {
                declaredArity = found->second.arity;
            }
        }
    } else if (const auto found = m_constructorArity.find(name);
               found != m_constructorArity.end()) {
        declaredArity = found->second;
    }
    if (!declaredArity) {
        return;
    }
    const auto arity = *declaredArity;
    // A spelling two imported types share (FS's and FileHandle's
    // `InvalidUtf8`) may have been looked up as the other one: any of them
    // taking this many values answers.
    const auto anySameNamedTakes = [&](std::size_t given) {
        if (info->declaredHere) {
            return false;
        }
        if (const auto bare = m_constructorArity.find(name);
            bare != m_constructorArity.end() && bare->second == given) {
            return true;
        }
        return std::any_of(
            m_moduleConstructors.begin(), m_moduleConstructors.end(), [&](const auto& module) {
                const auto found = module.second.find(name);
                return found != module.second.end() && found->second.arity == given;
            });
    };
    if (argTypes.size() != arity && !anySameNamedTakes(argTypes.size())) {
        if (arity == 1) {
            error(
                diag::Code::ConstructorArityOne, loc,
                {{"name", name}, {"given", std::to_string(argTypes.size())}});
        } else {
            error(
                diag::Code::ConstructorArity, loc,
                {{"name", name},
                 {"count", std::to_string(arity)},
                 {"given", std::to_string(argTypes.size())}});
        }
        return;
    }
    if (!info->declaredHere) {
        return;
    }
    // Each value against the payload as declared, where both are known:
    // a payload that is one of the type's parameters (`Just(A)`) takes any.
    for (std::size_t i = 0; i < arity; ++i) {
        if (i < info->slots.size() && info->slots[i] >= 0) {
            continue;
        }
        const auto declared = declaredPayload(*info, i);
        if (!declared || !argTypes[i]) {
            continue;
        }
        const auto expected = resolve(declared);
        const auto actual = resolve(argTypes[i]);
        if (isUndetermined(expected) || isUndetermined(actual) ||
            argMatchesParam(actual, expected)) {
            continue;
        }
        // Once, however often the call is inferred.
        const auto code =
            arity == 1 ? diag::Code::ConstructorValueType : diag::Code::ConstructorValueTypeAt;
        diag::Args args = {
            {"name", name}, {"expected", typeToString(expected)}, {"actual", typeToString(actual)}};
        if (arity != 1) {
            args.emplace_back("index", std::to_string(i + 1));
        }
        const auto key = std::string(loc.file) + ':' + std::to_string(loc.line) + ':' +
                         std::to_string(loc.column) + ':' + diag::render(code, args);
        if (m_reportedOnce.insert(key).second) {
            error(code, loc, std::move(args));
        }
    }
}

auto TypeChecker::constructorResultType(
    const std::string& name, const std::vector<TypePtr>& argTypes, const std::string& scope)
    -> TypePtr {
    const auto* found = constructorInfo(name, scope);
    if (!scope.empty() && !found) {
        return nullptr;
    }
    if (!found) {
        // A payload constructor whose spelling two ADTs share (`Text(s)`):
        // its ADT is the one in scope.
        if (const auto* owner = scopedConstructorOwner(name);
            owner && owner->arity > 0 && owner->arity == argTypes.size() && !owner->generic) {
            return Type::named(owner->adt);
        }
        return nullptr;
    }
    const auto& info = *found;
    // The program's own constructor builds its own type, also when the
    // prelude has a type of that name.
    const bool own = scope.empty() && m_shadowingConstructors.contains(name);

    std::vector<TypePtr> typeArgs;
    typeArgs.reserve(info.typeParamCount);
    for (size_t i = 0; i < info.typeParamCount; ++i) {
        typeArgs.push_back(freshTypeVar());
    }
    for (std::size_t i = 0; i < info.slots.size() && i < argTypes.size(); ++i) {
        const auto slot = info.slots[i];
        if (slot >= 0 && static_cast<std::size_t>(slot) < typeArgs.size() && argTypes[i]) {
            typeArgs[slot] = argTypes[i];
        }
    }

    // `X?` is its own type kind everywhere else in the checker, so the
    // prelude's Optional has to produce that rather than a NamedType.
    if (info.adtName == kex::names::kOptionalType) {
        return Type::optional(typeArgs.empty() ? Type::unknown() : typeArgs[0]);
    }
    auto result = Type::named(info.adtName, std::move(typeArgs));
    return own ? ownType(result) : result;
}

auto TypeChecker::registerAdtsInModule(const ast::ModuleDef& mod) -> void {
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = previousModule.empty() || mod.name.starts_with(previousModule + ".")
                              ? mod.name
                              : previousModule + "." + mod.name;
    for (const auto& item : mod.body) {
        std::visit(
            [this, &mod](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                    if (!node) {
                        return;
                    }
                    registerAdt(*node, m_currentModulePath);
                    if (auto constructors = kex::typeConstructors(*node)) {
                        for (const auto& constructor : *constructors) {
                            m_moduleConstructors[mod.name][constructor.name] = {
                                m_currentModulePath + "." + node->name, constructor.arity, true};
                        }
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        registerAdtsInModule(*node);
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (!node) {
                        return;
                    }
                    for (const auto& visible : node->items) {
                        if (const auto* type = std::get_if<std::unique_ptr<ast::TypeDef>>(&visible);
                            type && *type) {
                            registerAdt(**type, m_currentModulePath);
                            if (auto constructors = kex::typeConstructors(**type)) {
                                for (const auto& constructor : *constructors) {
                                    m_moduleConstructors[mod.name][constructor.name] = {
                                        m_currentModulePath + "." + (*type)->name,
                                        constructor.arity, node->isPublic};
                                }
                            }
                        }
                    }
                }
            },
            item);
    }
    m_currentModulePath = previousModule;
}

auto TypeChecker::typeDefToType(const ast::TypeDef& def) -> TypePtr {
    if (!def.variants || def.variants->empty()) {
        return Type::unknown();
    }
    // A generic alias's parameters take the negative TypeVar slots -1, -2, …
    // — the convention `substituteInterfaceGenerics` fills — so an applied
    // alias (`Predicate<Integer>`) can be expanded by substituting its
    // arguments, the way a distinct type's backing type already is (#337).
    std::unordered_map<std::string, TypePtr> aliasParams;
    for (size_t i = 0; i < def.typeParams.size(); ++i) {
        aliasParams[def.typeParams[i]] = Type::genericLetter(-static_cast<int>(i + 1));
    }
    // The body names types the way its own module imported them — an alias
    // is expanded once, here, not re-read wherever it is used.
    struct RestoreImports {
        std::vector<ImportSelection>& target;
        std::vector<ImportSelection> saved;
        ~RestoreImports() {
            target = std::move(saved);
        }
    } restoreImports{m_declarationImports, m_declarationImports};
    if (auto imports = m_typeImports.find(&def); imports != m_typeImports.end()) {
        m_declarationImports = imports->second;
    }
    // Build variant types, then fold into a union if more than one.
    std::vector<TypePtr> parts;
    for (const auto& v : *def.variants) {
        if (v) {
            parts.push_back(resolveTypeExpr(*v, aliasParams));
        }
    }
    if (parts.empty()) {
        return Type::unknown();
    }
    if (parts.size() == 1) {
        return parts[0];
    }
    return std::make_shared<Type>(Type{UnionType{std::move(parts)}});
}

auto TypeChecker::resolveDistinctName(const std::string& name) const -> std::string {
    if (name.find('.') != std::string::npos && m_distinctTypes.count(name)) {
        return name;
    }
    auto scope = m_currentModulePath;
    while (!scope.empty()) {
        const auto candidate = scope + "." + name;
        if (m_distinctTypes.count(candidate)) {
            return candidate;
        }
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) {
            break;
        }
        scope.resize(dot);
    }
    return name;
}

auto TypeChecker::indistinguishableMembers(const TypePtr& type) const
    -> std::optional<std::pair<TypePtr, TypePtr>> {
    const auto* members = type ? std::get_if<UnionType>(&resolve(type)->kind) : nullptr;
    if (!members) {
        return std::nullopt;
    }
    // What a value of `member` is at run time: its backing, through every
    // distinct type that wraps another.
    const auto runtime = [&](TypePtr member) {
        member = resolve(member);
        for (int depth = 0; depth < 8; ++depth) {
            auto backing = distinctBacking(member);
            if (!backing || typesEqual(backing, member)) {
                break;
            }
            member = resolve(backing);
        }
        return member;
    };
    const auto distinct = [&](const TypePtr& member) {
        const auto backing = distinctBacking(resolve(member));
        return backing && !typesEqual(backing, resolve(member));
    };
    for (std::size_t i = 0; i < members->members.size(); ++i) {
        for (std::size_t j = i + 1; j < members->members.size(); ++j) {
            const auto& left = members->members[i];
            const auto& right = members->members[j];
            if ((distinct(left) || distinct(right)) && typesEqual(runtime(left), runtime(right))) {
                return distinct(left) ? std::make_pair(left, right) : std::make_pair(right, left);
            }
        }
    }
    return std::nullopt;
}

auto TypeChecker::innermostDistinct(const TypePtr& type) const -> TypePtr {
    const auto value = resolve(type);
    if (const auto* list = std::get_if<ListType>(&value->kind)) {
        return innermostDistinct(list->element);
    }
    if (const auto* optional = std::get_if<OptionalType>(&value->kind)) {
        return innermostDistinct(optional->inner);
    }
    return value;
}

auto TypeChecker::distinctTraitClash(const TypePtr& arg, const TypePtr& param) const
    -> std::string {
    const auto value = resolve(arg);
    // Through a container both sides share: `[BookId]` held as `[Labeled]`.
    if (const auto* list = std::get_if<ListType>(&value->kind)) {
        if (const auto* wantedList = std::get_if<ListType>(&resolve(param)->kind)) {
            return distinctTraitClash(list->element, wantedList->element);
        }
    }
    if (const auto* optional = std::get_if<OptionalType>(&value->kind)) {
        if (const auto* wantedOptional = std::get_if<OptionalType>(&resolve(param)->kind)) {
            return distinctTraitClash(optional->inner, wantedOptional->inner);
        }
    }
    const auto* named = std::get_if<NamedType>(&value->kind);
    if (!named) {
        return {};
    }
    const auto backing = distinctBacking(value);
    if (!backing || typesEqual(backing, value)) {
        return {};
    }
    const auto wanted = resolve(param);
    std::string trait;
    if (const auto* bound = std::get_if<ConstrainedType>(&wanted->kind)) {
        trait = bound->traitName;
    } else if (const auto* traitNamed = std::get_if<NamedType>(&wanted->kind);
               traitNamed && m_traits.lookup(traitNamed->name)) {
        trait = traitNamed->name;
    }
    if (trait.empty() || !m_traits.implementsDirectly(named->name, trait)) {
        return {};
    }
    return trait;
}

auto TypeChecker::reportIndistinguishable(const TypePtr& type, SourceLocation location) -> void {
    if (const auto pair = indistinguishableMembers(type)) {
        const auto where = std::string(location.file) + ":" + std::to_string(location.line) + ":" +
                           std::to_string(location.column);
        if (!m_reportedIndistinguishable.insert(where).second) {
            return;
        }
        error(
            diag::Code::UnionMembersIndistinguishable, location,
            {{"left", typeToString(pair->first)}, {"right", typeToString(pair->second)}});
    }
}

auto TypeChecker::distinctBacking(const TypePtr& type) const -> TypePtr {
    if (!type) {
        return type;
    }
    const auto* named = std::get_if<NamedType>(&type->kind);
    if (!named) {
        return type;
    }
    auto found = m_distinctTypes.find(named->name);
    if (found == m_distinctTypes.end()) {
        const auto resolvedName = resolveDistinctName(named->name);
        found = m_distinctTypes.find(resolvedName);
    }
    if (found == m_distinctTypes.end() || !found->second.backingType) {
        return type;
    }

    std::unordered_map<int, TypePtr> substitutions;
    for (size_t i = 0; i < named->typeArgs.size(); ++i) {
        substitutions[-static_cast<int>(i + 1)] = named->typeArgs[i];
    }
    std::function<TypePtr(const TypePtr&)> substitute = [&](const TypePtr& part) -> TypePtr {
        if (!part) {
            return part;
        }
        if (const auto* var = std::get_if<TypeVar>(&part->kind)) {
            if (auto value = substitutions.find(var->id); value != substitutions.end()) {
                return value->second;
            }
            return part;
        }
        return mapTypeChildren(part, substitute);
    };
    auto backing = substitute(found->second.backingType);
    return distinctBacking(backing);
}

auto TypeChecker::representationsCompatible(const TypePtr& from, const TypePtr& to) -> bool {
    const auto source = distinctBacking(resolve(from));
    const auto target = distinctBacking(resolve(to));
    if (typesEqual(source, target)) {
        return true;
    }
    // A value the backing type already admits: `(2, -4)` is an
    // `(Integer, Integer)`, and `(Number, Number)` holds it. Only that way
    // round — a `Number` given back as an `Integer` might be a Float.
    const bool intoDistinct = !typesEqual(resolve(to), target);
    if (intoDistinct && argMatchesParam(source, target)) {
        return true;
    }
    return (m_traits.satisfies(source, "Integer") && m_traits.satisfies(target, "Integer")) ||
           (m_traits.satisfies(source, "Float") && m_traits.satisfies(target, "Float"));
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
    std::function<void(const ast::ModuleDef&)> walk = [&](const ast::ModuleDef& module) {
        if (module.isCapability) {
            m_capabilities.insert(module.name);
            TraitDef td;
            td.name = module.name;
            for (const auto& mi : module.body) {
                std::visit(
                    [&](const auto& mn) {
                        using MT = std::decay_t<decltype(mn)>;
                        if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                            if (!mn || mn->clauses.empty()) {
                                return;
                            }
                            // A capability's INTERFACE is its `foul` members. A
                            // pure member performs no effect, so there is nothing
                            // for a stand-in to substitute and no reason to make
                            // one implement it — `IO.inspect` is deliberately pure
                            // (it is the debug hatch purity checking ignores) and
                            // requiring it would have kept `IO` from being a
                            // capability at all (kexhq/kex#143).
                            if (!mn->isFoul) {
                                return;
                            }
                            // What the member writes, read in its module, one
                            // requirement per overload (`take(text: String)`
                            // beside `take(count: Integer)`): an implementation
                            // in a stand-in (Net.Socket's Mock) takes its
                            // parameters from here, as one of a capability
                            // another unit declares does from its interface.
                            // Unwritten stays permissive.
                            const auto outerPath = std::exchange(m_currentModulePath, module.name);
                            std::size_t declarationsTaken = 0;
                            for (const auto* clausePtr : ast::signatureClauses(*mn)) {
                                const auto& clause = *clausePtr;
                                Signature sig;
                                sig.name = mn->name;
                                sig.isFoul = mn->isFoul;
                                const int varMark = m_nextTypeVar;
                                const bool writesTypes =
                                    clause.returnAnnotation ||
                                    std::any_of(
                                        clause.params.begin(), clause.params.end(),
                                        [](const ast::Param& p) { return p.type.has_value(); });
                                // Or a declaration of its name and arity
                                // (`connect : Endpoint -> Result<...>`), the
                                // next one not taken by an earlier overload.
                                bool declaredApart = false;
                                std::size_t seen = 0;
                                for (const auto& other : module.body) {
                                    if (writesTypes) {
                                        break;
                                    }
                                    const auto* ann =
                                        std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&other);
                                    if (!ann || !*ann || (*ann)->name != mn->name) {
                                        continue;
                                    }
                                    auto declared = annotationToSignature(**ann, nullptr);
                                    if (!declared ||
                                        declared->params.size() != clause.params.size() ||
                                        seen++ < declarationsTaken) {
                                        continue;
                                    }
                                    sig.params = declared->params;
                                    sig.result = declared->result;
                                    declaredApart = true;
                                    ++declarationsTaken;
                                    break;
                                }
                                std::unordered_map<std::string, TypePtr> letters;
                                for (std::size_t i = 0; !declaredApart && i < clause.params.size();
                                     ++i) {
                                    const auto& param = clause.params[i];
                                    sig.params.push_back(
                                        param.type && *param.type
                                            ? resolveTypeExprNode(**param.type, letters)
                                            : Type::unknown());
                                }
                                if (!declaredApart) {
                                    sig.result = clause.returnAnnotation && *clause.returnAnnotation
                                                     ? resolveTypeExprNode(
                                                           **clause.returnAnnotation, letters)
                                                     : Type::unknown();
                                }
                                // Its letters are each implementation's own (`A` of
                                // `(TCPListener -> A) -> Result<A, NetError>`).
                                generalizeSignature(sig, varMark, nullptr, true);
                                td.requiredMethods.push_back(std::move(sig));
                            }
                            m_currentModulePath = outerPath;
                        }
                    },
                    mi);
            }
            m_traits.define(std::move(td));
        }
        for (const auto& mi : module.body) {
            std::visit(
                [&](const auto& mn) {
                    using MT = std::decay_t<decltype(mn)>;
                    if constexpr (std::is_same_v<MT, std::unique_ptr<ast::ModuleDef>>) {
                        if (mn) {
                            walk(*mn);
                        }
                    }
                },
                mi);
        }
    };
    for (const auto& item : program.items) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        walk(*node);
                    }
                }
            },
            item);
    }

    // A capability declared in an imported module is not in this AST, so its
    // interface is the only place its members are known. Registering it here
    // is what lets a consumer write `with FS.File = ...` and
    // `make Fake, implement: FS.File` against a capability it did not declare.
    if (m_importedInterfaces) {
        for (const auto& [name, interface] : m_importedInterfaces->modules) {
            if (!interface.isCapability) {
                continue;
            }
            m_capabilities.insert(name);
            if (m_traits.get(name)) {
                continue;
            }
            TraitDef td;
            td.name = name;
            // Every foul overload: `take(text: String)` and `take(count:
            // Integer)` are each required, and each implementation is held
            // to its own (kexhq/kex#623).
            for (const auto& [member, overloads] : interface.exports) {
                for (const auto& overload : overloads) {
                    // Foul members only — see the local branch above.
                    if (!overload.signature.isFoul) {
                        continue;
                    }
                    Signature sig = overload.signature;
                    sig.name = member;
                    td.requiredMethods.push_back(std::move(sig));
                }
            }
            // `exports` is a hash map, so the loop above yields its members in
            // an order that differs between standard-library implementations.
            // A trait's required methods are its DICTIONARY LAYOUT — slot i
            // means one method to whoever builds the dictionary and another to
            // whoever indexes it — so an unsorted list makes the emitted code
            // depend on the host's hash order. It built and passed on macOS
            // and died `badarg` on Linux, on both glibc and musl
            // (kexhq/kex#143).
            std::stable_sort(
                td.requiredMethods.begin(), td.requiredMethods.end(),
                [](const Signature& a, const Signature& b) {
                    if (a.name != b.name) {
                        return a.name < b.name;
                    }
                    return a.params.size() < b.params.size();
                });
            m_traits.define(std::move(td));
        }
    }
}

auto TypeChecker::registerLocalConformances(const ast::Program& program) -> void {
    // A record declared in a module is known by its QUALIFIED name, so a
    // `make` block inside that module has to register its conformance under
    // the same name. Registering the bare one left `make Files, implement:
    // FS.File` inside `module Mock` conforming a type called `Files` that
    // nothing is, and `with FS.File = Mock.Files { ... }` was rejected as not
    // implementing the capability (kexhq/kex#143).
    auto registerOne = [this](const ast::MakeDef& def, const std::string& modulePath) {
        if (def.implements.empty() || !def.target) {
            return;
        }
        std::string typeName;
        if (const auto name = writtenMakeTarget(def.target, StructuralTarget::Excluded);
            name && name->parts.size() == 1) {
            typeName = name->parts[0];
        }
        static const std::vector<ImportSelection> kNoImports;
        const auto imports = m_makeImports.find(&def);
        for (const auto& written : def.implements) {
            const auto traitName = resolveTraitReference(
                written, modulePath,
                imports == m_makeImports.end() ? &kNoImports : &imports->second);
            for (const auto& key : conformanceKeys(def, typeName)) {
                registerConformance(def, key, traitName);
                if (!modulePath.empty()) {
                    registerConformance(def, modulePath + "." + key, traitName);
                }
            }
        }
    };
    std::function<void(const ast::ModuleDef&, const std::string&)> walkModule =
        [&](const ast::ModuleDef& module, const std::string& prefix) {
            // A module nested in a file-header one arrives qualified already
            // (`Net.Socket.Mock`); prefixing it again registered its claims
            // under `Net.Socket.Net.Socket.Mock`, which no lookup asks for.
            const std::string path = ast::nestedModulePath(prefix, module.name);
            for (const auto& mi : module.body) {
                std::visit(
                    [&](const auto& mn) {
                        using MT = std::decay_t<decltype(mn)>;
                        if constexpr (std::is_same_v<MT, std::unique_ptr<ast::MakeDef>>) {
                            if (mn) {
                                registerOne(*mn, path);
                            }
                        } else if constexpr (std::is_same_v<MT, std::unique_ptr<ast::ModuleDef>>) {
                            if (mn) {
                                walkModule(*mn, path);
                            }
                        }
                    },
                    mi);
            }
        };
    for (const auto& item : program.items) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                    if (node) {
                        registerOne(*node, "");
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        walkModule(*node, "");
                    }
                }
            },
            item);
    }
}

auto TypeChecker::registerTraits(const ast::Program& program) -> void {
    kex::forEachTraitDef(
        program.items, [this](const ast::TraitDef& node, const std::string& modulePath) {
            TraitDef td;
            td.name = kex::traitIdentity(modulePath, node.name);
            td.paramKinds = node.typeParamArities;
            td.paramNames = node.typeParams;
            for (const auto& bodyItem : node.body) {
                std::visit(
                    [&](const auto& bi) {
                        using BT = std::decay_t<decltype(bi)>;
                        if constexpr (std::is_same_v<BT, std::unique_ptr<ast::TypeAnnotation>>) {
                            if (!bi) {
                                return;
                            }
                            const int varMark = m_nextTypeVar;
                            std::unordered_map<std::string, TypePtr> written;
                            auto sig = annotationToSignature(*bi, &written);
                            if (sig) {
                                sig->isFoul = bi->isFoul;
                                // Which of its letters are the trait's own parameters
                                // (`T` in `Folds<T>`), for the defaults that share them.
                                for (const auto& parameter : node.typeParams) {
                                    if (const auto found = written.find(parameter);
                                        found != written.end()) {
                                        if (const auto id = typeVariableId(found->second)) {
                                            m_traitParameterLetters[{td.name, sig->name}]
                                                                   [parameter] = *id;
                                        }
                                    }
                                }
                                // A trait over a constructor (`Functor<F<_>>`) writes its
                                // receiver, `F<A>`: the requirement is a function on any
                                // `F<…>` its bound allows, called on a value known only by
                                // that bound (`xs: F<Integer>`, §8.2).
                                if (!sig->params.empty() && sig->params[0] &&
                                    std::holds_alternative<TypeApp>(sig->params[0]->kind)) {
                                    auto asMethod = *sig;
                                    asMethod.makeModule = modulePath;
                                    // Its variables are each call's own: generalized.
                                    generalizeSignature(asMethod, varMark, nullptr, true);
                                    m_higherKindedRequirements[asMethod.name].push_back(
                                        std::move(asMethod));
                                }
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
                                for (const auto& clause : bi->clauses) {
                                    m_traitDefaultClauses[&clause] = td.name;
                                }
                                m_traitDefaults[td.name].push_back(bi.get());
                            }
                        }
                    },
                    bodyItem);
            }
            // A requirement with a body is a default its declaration types.
            for (const auto& bodyItem : node.body) {
                const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&bodyItem);
                if (ann && *ann &&
                    std::find(td.defaultBodies.begin(), td.defaultBodies.end(), (*ann)->name) !=
                        td.defaultBodies.end()) {
                    m_traitDefaultDeclarations[{td.name, (*ann)->name}] = ann->get();
                }
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
    const std::string& written,
    const std::string& scope,
    const std::vector<ImportSelection>* imports,
    bool lastResort,
    std::vector<std::string>* ambiguous) const -> std::string {
    const auto bare = written.substr(written.rfind('.') + 1);
    std::vector<std::string> modules;
    std::string throughAlias;
    auto consider = [&](const ImportSelection& import) {
        // `using Geo, as: G` then `G.Shape`.
        if (import.alias && written.starts_with(*import.alias + ".")) {
            throughAlias = import.module + written.substr(import.alias->size());
        }
        if (!resolve_core::usingSelectsName(bare, import.onlyNames, import.exceptNames)) {
            return;
        }
        modules.push_back(import.module);
    };
    if (imports) {
        for (auto import = imports->rbegin(); import != imports->rend(); ++import) {
            consider(*import);
        }
    } else {
        for (auto frame = m_importScopeStack.rbegin(); frame != m_importScopeStack.rend();
             ++frame) {
            for (auto import = frame->rbegin(); import != frame->rend(); ++import) {
                consider(*import);
            }
        }
        for (auto import = m_declarationImports.rbegin(); import != m_declarationImports.rend();
             ++import) {
            consider(*import);
        }
    }
    const auto known = [this](const std::string& name) { return m_traits.get(name) != nullptr; };
    if (!throughAlias.empty() && known(throughAlias)) {
        return throughAlias;
    }
    const auto eachKnown = [&](const auto& visit) {
        if (!lastResort) {
            return;
        }
        for (const auto& [name, _] : m_traits.all()) {
            visit(name);
        }
    };
    return kex::resolveTraitName(written, scope, modules, known, eachKnown, ambiguous);
}

auto TypeChecker::registerTypeAliases(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit(
            [this](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                    if (!node->variants) {
                        return;
                    }
                    if (node->isDistinct) {
                        std::unordered_map<std::string, TypePtr> vars;
                        for (size_t i = 0; i < node->typeParams.size(); ++i) {
                            vars[node->typeParams[i]] =
                                Type::genericLetter(-static_cast<int>(i + 1));
                        }
                        m_programDistinctTypes.insert(node->name);
                        m_distinctTypes[node->name] = {
                            node->typeParams, resolveTypeExpr(*node->variants->front(), vars)};
                        return;
                    }
                    // `type Row = Type.returnedBy(parseRow)` resolves against
                    // function signatures, which are registered after this pass —
                    // so it is deferred rather than resolved here.
                    if (node->variants->size() == 1) {
                        if (auto* query =
                                std::get_if<ast::TypeQuery>(&(*node->variants)[0]->kind)) {
                            // Resolved after the builtin type names are
                            // registered — before them, `String` is just a name
                            // and the alias would bind NamedType("String")
                            // instead of the real string type.
                            m_computedAliases.push_back({node->name, query, m_currentModulePath});
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
                    if (std::all_of(
                            node->variants->begin(), node->variants->end(),
                            [](const auto& v) { return extractConstructorName(v).has_value(); })) {
                        return;
                    }
                    m_typeAliases[node->name] = typeDefToType(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    registerTypeAliasesInModule(*node);
                }
            },
            item);
    }
}

auto TypeChecker::registerTypeAliasesInModule(const ast::ModuleDef& mod) -> void {
    // An alias body names types the way its own module sees them: `type
    // Handler = Request -> Response` inside `module Http` means Http's
    // records, not a same-named record some other module exports.
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = previousModule.empty() || mod.name.rfind(previousModule + ".", 0) == 0
                              ? mod.name
                              : previousModule + "." + mod.name;
    for (const auto& item : mod.body) {
        std::visit(
            [this](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                    if (!node->variants) {
                        return;
                    }
                    if (node->isDistinct) {
                        std::unordered_map<std::string, TypePtr> vars;
                        for (size_t i = 0; i < node->typeParams.size(); ++i) {
                            vars[node->typeParams[i]] =
                                Type::genericLetter(-static_cast<int>(i + 1));
                        }
                        ImportedDistinctType info{
                            node->typeParams, resolveTypeExpr(*node->variants->front(), vars)};
                        m_programDistinctTypes.insert(node->name);
                        m_programDistinctTypes.insert(m_currentModulePath + "." + node->name);
                        m_distinctTypes[node->name] = info;
                        m_distinctTypes[m_currentModulePath + "." + node->name] = std::move(info);
                        return;
                    }
                    // `type Row = Type.returnedBy(parseRow)` resolves against
                    // function signatures, which are registered after this pass —
                    // so it is deferred rather than resolved here.
                    if (node->variants->size() == 1) {
                        if (auto* query =
                                std::get_if<ast::TypeQuery>(&(*node->variants)[0]->kind)) {
                            // Resolved after the builtin type names are
                            // registered — before them, `String` is just a name
                            // and the alias would bind NamedType("String")
                            // instead of the real string type.
                            m_computedAliases.push_back({node->name, query, m_currentModulePath});
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
                    if (std::all_of(
                            node->variants->begin(), node->variants->end(),
                            [](const auto& v) { return extractConstructorName(v).has_value(); })) {
                        return;
                    }
                    m_typeAliases[node->name] = typeDefToType(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    registerTypeAliasesInModule(*node);
                }
            },
            item);
    }
    m_currentModulePath = previousModule;
}

auto TypeChecker::annotationToSignature(
    const ast::TypeAnnotation& ann, std::unordered_map<std::string, TypePtr>* sharedGenericVars)
    -> std::optional<Signature> {
    if (!ann.type) {
        return std::nullopt;
    }
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
        if (!ft || (!firstNode && cur->parenthesized)) {
            break;
        }
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
    // Non-function annotation (e.g. `x : Int`): a zero-param constant whose
    // type IS the annotated type. Recorded per contract, so an interface
    // states the contract as the checker resolved it.
    Signature signature{ann.name, std::move(params), result};
    m_contractSignatures[&ann] = quantifiedContract(signature);
    return signature;
}

auto TypeChecker::registerDeclaredSignatures(const ast::Program& program) -> void {
    auto add = [&](const ast::TypeAnnotation& ann, const std::string& modulePath) {
        // A contract's type variables are its own generics: `ident : T -> T`
        // binds T per call, as an inline `let ident(x: T) -> T` does.
        const int varMark = m_nextTypeVar;
        auto sig = annotationToSignature(ann);
        if (!sig) {
            return;
        }
        generalizeSignature(*sig, varMark, nullptr, true);
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
    for (const auto& item : program.items) {
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item); ann && *ann) {
            add(**ann, "");
        } else if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item); mod && *mod) {
            registerDeclaredSignaturesInModule(**mod);
        }
    }
}

auto TypeChecker::registerDeclaredSignaturesInModule(
    const ast::ModuleDef& mod, const std::string& parentPath) -> void {
    const auto modulePath = ast::nestedModulePath(parentPath, mod.name);
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = modulePath;
    auto importsFor = [&](const std::string& name) -> const std::vector<ImportSelection>* {
        for (const auto& item : mod.body) {
            if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
                fn && *fn && (*fn)->name == name) {
                if (auto found = m_functionImports.find(fn->get());
                    found != m_functionImports.end()) {
                    return &found->second;
                }
            }
            if (const auto* visibility = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                visibility && *visibility) {
                for (const auto& visible : (*visibility)->items) {
                    if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible);
                        fn && *fn && (*fn)->name == name) {
                        if (auto found = m_functionImports.find(fn->get());
                            found != m_functionImports.end()) {
                            return &found->second;
                        }
                    }
                }
            }
        }
        return nullptr;
    };
    auto add = [this, &modulePath,
                &importsFor](const ast::TypeAnnotation& ann, bool exposeUnqualified) {
        const auto previousImports = m_declarationImports;
        if (const auto* imports = importsFor(ann.name)) {
            m_declarationImports = *imports;
        }
        const int varMark = m_nextTypeVar;
        auto sig = annotationToSignature(ann);
        m_declarationImports = previousImports;
        if (!sig) {
            return;
        }
        generalizeSignature(*sig, varMark, nullptr, true);
        sig->declaringModule = modulePath;
        m_annotationDeclared.insert(ann.name);
        const auto key = modulePath + "\n" + ann.name;
        m_annotationArities[key].insert(sig->params.size());
        m_scopedDeclaredSignatures[key].push_back(*sig);
        // Reachable qualified before its definition is checked, so a call
        // above the definition resolves to it (#103). Not marked published:
        // the definition's own check still replaces this with what it checks.
        if (!m_qualifiedPublished.contains(kex::mangle::qualify(modulePath, ann.name))) {
            m_userSignatures[kex::mangle::qualify(modulePath, ann.name)].push_back(*sig);
        }
        if (exposeUnqualified) {
            m_userSignatures[ann.name].insert(m_userSignatures[ann.name].begin(), std::move(*sig));
        }
    };
    for (const auto& item : mod.body) {
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item); ann && *ann) {
            add(**ann, true);
        } else if (auto* visibility = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items) {
                if (auto* visibleAnn = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&visible);
                    visibleAnn && *visibleAnn) {
                    add(**visibleAnn, false);
                }
            }
        } else if (auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
                   nested && *nested) {
            registerDeclaredSignaturesInModule(**nested, modulePath);
        }
    }
    m_currentModulePath = previousModule;
}

auto TypeChecker::registerMakeSignature(const ast::MakeDef& def, const std::string& modulePath)
    -> void {
    // Every method name this block defines, private ones included. Only the
    // NAME is needed here: it answers "does this method exist anywhere?" for
    // the unknown-method report, which is name-based by design.
    auto noteName = [&](const ast::FunctionDef& fn) { m_makeMethodNames.insert(fn.name); };
    for (const auto& item : def.body) {
        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fn) {
                noteName(**fn);
            }
        } else if (
            const auto* visibility = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
            if (!*visibility) {
                continue;
            }
            for (const auto& inner : (*visibility)->items) {
                if (const auto* vfn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner)) {
                    if (*vfn) {
                        noteName(**vfn);
                    }
                }
            }
        }
    }
    if (!def.target) {
        return;
    }
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
        ~PathRestore() {
            target = saved;
        }
    } pathRestore{m_currentModulePath, previousModulePath};
    // And against the names that module imported: a `textColumn :> Atom ->
    // TextColumn` under `using Harmat.Schema, only: [TextColumn]` otherwise
    // answered a bare `TextColumn` that no `==` or `||` overload is
    // declared for, once the call was made from another module
    // (kexhq/kex#532).
    struct ImportsRestore {
        std::vector<ImportSelection>& target;
        std::vector<ImportSelection> saved;
        ~ImportsRestore() {
            target = std::move(saved);
        }
    } importsRestore{m_declarationImports, m_declarationImports};
    if (auto imports = m_makeImports.find(&def); imports != m_makeImports.end()) {
        m_declarationImports = imports->second;
    }
    std::unordered_map<std::string, TypePtr> targetVars;
    // Where the target's own variables (`X` in `make List<X>`) start; see
    // the generalizeSignature call below.
    const int targetVarMark = m_nextTypeVar;
    auto receiver = resolveTypeExpr(*def.target, targetVars);
    const auto receiverBounds = makeReceiverBounds(def);
    // `This<B>` in this block's signatures fills the target's holes.
    const auto savedMakeHoles = std::exchange(m_currentMakeHoles, ast::makeTypeLambdaHoles(def));
    struct RestoreHoles {
        std::vector<std::size_t>& target;
        std::vector<std::size_t> saved;
        ~RestoreHoles() {
            target = std::move(saved);
        }
    } restoreHoles{m_currentMakeHoles, savedMakeHoles};
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
            if (std::none_of(owners.begin(), owners.end(), [&](const TypePtr& existing) {
                    return typesEqual(existing, resolvedReceiver);
                })) {
                owners.push_back(std::move(resolvedReceiver));
            }
            auto& arities = m_makeMethodArities[fn.name];
            for (const auto& clause : fn.clauses) {
                const auto required = static_cast<std::size_t>(std::count_if(
                    clause.params.begin(), clause.params.end(),
                    [](const ast::Param& param) { return !param.defaultValue; }));
                arities.emplace_back(required + 1, clause.params.size() + 1);
            }
        };
        for (const auto& item : def.body) {
            if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn) {
                    record(**fn);
                }
            } else if (
                const auto* visibility =
                    std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
                if (!*visibility) {
                    continue;
                }
                for (const auto& inner : (*visibility)->items) {
                    if (const auto* vfn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner)) {
                        if (*vfn) {
                            record(**vfn);
                        }
                    }
                }
            }
        }
    }
    // Record which of these methods a `private do ... end` block hides, now
    // that the receiver type has a name to key them by.
    if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
        for (const auto& item : def.body) {
            if (const auto* visibility = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                visibility && *visibility && !(*visibility)->isPublic) {
                for (const auto& inner : (*visibility)->items) {
                    if (const auto* vfn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner);
                        vfn && *vfn) {
                        m_privateMakeMethods.emplace(named->name + "#" + (*vfn)->name, modulePath);
                    }
                }
            }
        }
    }
    std::unordered_set<std::string> slotNames;
    if (def.isServing) {
        for (const auto& item : def.body) {
            if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
                fn && *fn && (*fn)->isSlot) {
                slotNames.insert((*fn)->name);
            }
        }
    }
    m_slotMethodNames.insert(slotNames.begin(), slotNames.end());

    auto remoteSlotSignature = [&](const Signature& handler, bool hasFrom) -> Signature {
        Signature remote = handler;
        remote.params.front() = Type::named("Server", {receiver});
        if (hasFrom) {
            TypePtr reply = Type::unknown();
            if (auto* named = std::get_if<NamedType>(&handler.result->kind);
                named && named->name == kex::names::kReplyType && named->typeArgs.size() == 1) {
                reply = named->typeArgs.front();
            }
            remote.result = Type::named("Result", {reply, Type::named("CallError")});
            remote.isFoul = true;
        } else {
            remote.result = Type::voidType();
            remote.isFoul = true;
        }
        return remote;
    };

    for (const auto& item : def.body) {
        auto add = [&](const std::unique_ptr<ast::TypeAnnotation>& ann) {
            if (!ann) {
                return;
            }
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
            if (!sig) {
                return;
            }
            const bool replyResult = [&] {
                auto result = resolve(sig->result);
                auto* named = std::get_if<NamedType>(&result->kind);
                return named && named->name == kex::names::kReplyType &&
                       named->typeArgs.size() == 1;
            }();
            if (def.isServing && slotNames.count(ann->name) && ann->implicitFrom && !replyResult) {
                error(diag::Code::CastSlotImplicitFrom, ann->location);
            }
            sig->params.insert(sig->params.begin(), receiver);
            // The target's `X` is one inference variable for the whole block.
            // Left as it is, every `[X]` signature shared it: a stdlib checked
            // from source bound it to String at `pieces.get(1, "")`, and
            // `this.args.first` then answered String in another file. The
            // prelude interface stores these as per-call placeholders, and
            // this makes the source agree.
            generalizeSignature(*sig, targetVarMark, nullptr, true);
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
                std::any_of(existing.begin(), existing.end(), [&](const Signature& other) {
                    return sameParameters(other, *sig);
                });
            sig->makeModule = modulePath;
            sig->receiverBounds = receiverBounds;
            if (!duplicate) {
                existing.push_back(*sig);
            }
            if (def.isServing && slotNames.count(ann->name)) {
                auto remote = remoteSlotSignature(*sig, replyResult);
                const bool remoteDuplicate =
                    std::any_of(existing.begin(), existing.end(), [&](const Signature& other) {
                        return sameParameters(other, remote);
                    });
                if (!remoteDuplicate) {
                    existing.push_back(std::move(remote));
                }
            }
        };
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
            add(*ann);
        } else if (auto* visibility = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items) {
                if (auto* visibleAnn =
                        std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&visible)) {
                    add(*visibleAnn);
                }
            }
        }
    }

    // A method written as a plain `let` — no `:>` — was registered only when
    // its make block was CHECKED, which is after every top-level function body.
    // So `let grow(c: Crate) = c.doubled` found no `doubled` at all and typed
    // as unknown, while the identical call inside `main` (checked last) worked.
    // Registering the declared shape here makes the method visible to whatever
    // is checked first; the body still refines it later.
    // `letters`: what an inherited default reads its letters as, the target's
    // and the trait's parameters as this type's claim fills them.
    const auto preRegister = [&](const ast::FunctionDef& method, const ast::FunctionClause& clause,
                                 bool inherited = false,
                                 std::unordered_map<std::string, TypePtr>* letters = nullptr) {
        auto& vars = letters ? *letters : targetVars;
        // Only THIS type's own `:>` contract for `method.name` should defer
        // to it — an unrelated type's same-named annotation (Char's
        // `string :> String` vs. Input's own plain `let string(...)`) must
        // not block this type's method from registering at all (#251).
        if (hasAnnotatedSignatureForReceiver(method.name, receiver)) {
            return;
        }
        // After an explicit receiver if the method matches one (`let
        // factor(@Px)` in `make CssUnit`): that first parameter IS the receiver,
        // so the arity gains none. A parameter matched by a literal, `_` or a
        // name (`let productKind(:mass, :acceleration)`) takes any value, typed
        // by the body like an untyped name; a constructor or structure pattern,
        // or a type-selector argument (`let to(String)`), is not a parameter
        // type at all, and the method registers when its body is checked.
        const bool explicitReceiver =
            !clause.params.empty() && clause.params.front().pattern &&
            std::holds_alternative<ast::ThisPattern>((*clause.params.front().pattern)->kind);
        const auto takesAnyValue = [](const ast::Param& param) {
            if (!param.pattern) {
                return param.name.has_value();
            }
            const auto& kind = (*param.pattern)->kind;
            return std::holds_alternative<ast::LiteralPattern>(kind) ||
                   std::holds_alternative<ast::WildcardPattern>(kind) ||
                   std::holds_alternative<ast::VarPattern>(kind);
        };
        for (std::size_t i = explicitReceiver ? 1 : 0; i < clause.params.size(); ++i) {
            if (!takesAnyValue(clause.params[i])) {
                return;
            }
        }
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
        for (std::size_t i = explicitReceiver ? 1 : 0; i < clause.params.size(); ++i) {
            signature.params.push_back(
                clause.params[i].type ? resolveTypeExpr(**clause.params[i].type, vars)
                : literalParameterType(clause, clause.params[i])
                    ? literalParameterType(clause, clause.params[i])
                    : freshTypeVar());
        }
        if (clause.returnAnnotation) {
            signature.result = resolveTypeExpr(**clause.returnAnnotation, vars);
        } else {
            // Left for the body to decide (m_pendingResults).
            signature.result = freshTypeVar();
            const int id = std::get<TypeVar>(signature.result->kind).id;
            if (inherited) {
                m_inheritedPendingResults.emplace(&clause, std::pair{receiver, id});
            } else {
                m_pendingResults[&clause] = id;
            }
            m_pendingResultVars.insert(id);
        }
        m_inMakeBlock = wasInMakeBlock;
        m_currentMakeType = previousMakeType;
        // Without the names a call site cannot place named arguments, and
        // lowering refused `router.launchOn(port: 4000)` for a method this
        // pass — not checkFunctionDef — registered.
        for (std::size_t i = explicitReceiver ? 1 : 0; i < clause.params.size(); ++i) {
            signature.paramNames.push_back(clause.params[i].name.value_or(""));
        }
        signature.clause = &clause;
        // Trailing defaults are optional from the start, not only once the
        // body has been checked: a top-level function calling
        // `t.string(:name, nullable: true)` is checked before the make block.
        std::size_t required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue) {
            required--;
        }
        signature.requiredParams = required + (explicitReceiver ? 0 : 1);
        signature.makeModule = modulePath;
        signature.receiverBounds = receiverBounds;
        generalizeTargetVariables(signature, receiver);
        auto& existing = m_methodSignatures[method.name];
        const bool duplicate =
            std::any_of(existing.begin(), existing.end(), [&](const Signature& other) {
                return sameMethod(other, signature);
            });
        if (!duplicate) {
            existing.push_back(signature);
        }
        if (def.isServing && method.isSlot) {
            bool hasFrom = false;
            if (auto* named = std::get_if<NamedType>(&signature.result->kind)) {
                hasFrom = named->name == kex::names::kReplyType && named->typeArgs.size() == 1;
            }
            auto remote = remoteSlotSignature(signature, hasFrom);
            const bool remoteDuplicate =
                std::any_of(existing.begin(), existing.end(), [&](const Signature& other) {
                    return sameParameters(other, remote);
                });
            if (!remoteDuplicate) {
                existing.push_back(std::move(remote));
            }
        }
    };
    // What the block defines itself, by name and arity: a trait default it
    // would otherwise inherit stays the trait's (see below).
    std::set<std::string> overridden;
    for (const auto& item : def.body) {
        // One registration per signature the method declares: clauses folded
        // into one method may each type their parameters differently.
        const auto registerEach = [&](const ast::FunctionDef& function) {
            if (!function.clauses.empty()) {
                overridden.insert(
                    function.name + "/" + std::to_string(function.clauses[0].params.size()));
            }
            for (const auto* clause : ast::signatureClauses(function)) {
                preRegister(function, *clause);
            }
        };
        if (const auto* function = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*function) {
                registerEach(**function);
            }
        } else if (const auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items) {
                if (const auto* function =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible)) {
                    if (*function) {
                        registerEach(**function);
                    }
                }
            }
        }
    }
    // A type implementing a trait has the trait's defaults as its own methods
    // (`make Coin, implement: Addable` gives Coin `twice`), unless the type
    // defines that name at that arity itself, in this block or another of its
    // blocks: `make String, implement: Monoid` (algebra.kex) inherits no
    // `repeat`, because string.kex's `make String` defines one. Registered for
    // this receiver like the block's own, so a call checked before either body
    // finds it, and recorded for the backends (inheritedDefaultsOf).
    const auto own = m_ownMethodsByTarget.find(makeTargetKey(receiver));
    const auto ownsKey = [&](const std::string& key) {
        return overridden.contains(key) ||
               (own != m_ownMethodsByTarget.end() && own->second.contains(key));
    };
    // A default known only by name (a trait of another unit): any arity of
    // the type's own counts.
    const auto ownsName = [&](const std::string& name) {
        const auto prefix = name + "/";
        const auto any = [&](const std::set<std::string>& keys) {
            const auto found = keys.lower_bound(prefix);
            return found != keys.end() && found->starts_with(prefix);
        };
        return any(overridden) || (own != m_ownMethodsByTarget.end() && any(own->second));
    };
    auto& inheritedHere = m_inheritedDefaults[&def];
    std::set<std::string> inheritedKeys;
    std::set<std::string> inheritedNames;
    for (std::size_t claim = 0; claim < def.implements.size(); ++claim) {
        const auto& written = def.implements[claim];
        const auto traitIdentity = resolveTraitReference(written, modulePath, nullptr);
        // The trait's parameters as this claim fills them, read with the
        // target's letters: `make [X], implement: Foldable<X>` makes the
        // defaults' `T` this list's `X`.
        auto letters = targetVars;
        if (const auto* trait = m_traits.get(traitIdentity);
            trait && claim < def.implementArgs.size()) {
            const auto& args = def.implementArgs[claim];
            for (std::size_t i = 0; i < args.size() && i < trait->paramNames.size(); ++i) {
                if (args[i]) {
                    letters[trait->paramNames[i]] = resolveTypeExpr(*args[i], letters);
                }
            }
        }
        const auto inherit = [&](const std::string& method) {
            if (inheritedNames.insert(method).second) {
                inheritedHere.push_back({traitIdentity, method});
            }
        };
        const auto defaults = m_traitDefaults.find(traitIdentity);
        if (defaults == m_traitDefaults.end()) {
            // Declared in another unit: the defaults its implementors carry.
            if (const auto* trait = m_traits.get(traitIdentity)) {
                for (const auto& method : trait->defaultBodies) {
                    if (!ownsName(method)) {
                        inherit(method);
                    }
                }
            }
            continue;
        }
        std::set<std::string> fromThisTrait;
        for (const auto* inherited : defaults->second) {
            if (inherited->clauses.empty()) {
                continue;
            }
            const auto key = ownMethodKey(*inherited);
            if (ownsKey(key) || inheritedKeys.contains(key)) {
                continue;
            }
            if (const auto declared =
                    m_traitDefaultDeclarations.find({traitIdentity, inherited->name});
                declared != m_traitDefaultDeclarations.end()) {
                // The declaration types the copy: its receiver, `F<A>`, is this
                // type, which binds `F` and `A` for the rest of it.
                const int varMark = m_nextTypeVar;
                auto declaredLetters = letters;
                auto declaredSignature = annotationToSignature(*declared->second, &declaredLetters);
                if (auto signature = declaredSignature
                                         ? atImplementingType(*declaredSignature, receiver)
                                         : std::nullopt) {
                    const auto applied = signature->params.front();
                    signature->name = inherited->name;
                    signature->isFoul = inherited->isFoul;
                    signature->clause = &inherited->clauses.front();
                    for (const auto& param : inherited->clauses.front().params) {
                        signature->paramNames.push_back(param.name.value_or(""));
                    }
                    signature->makeModule = modulePath;
                    signature->receiverBounds = receiverBounds;
                    // Its letters are each call's own.
                    generalizeSignature(*signature, varMark, nullptr, true);
                    // The declaration is this type's contract for it, as a `:>` of
                    // its own would be: checking the body does not retype it.
                    m_annotatedReceiverKeys[inherited->name].insert(annotatedReceiverKey(applied));
                    // In place of an untyped one an earlier pass registered.
                    auto& existing = m_methodSignatures[inherited->name];
                    if (const auto same = std::find_if(
                            existing.begin(), existing.end(),
                            [&](const Signature& other) { return sameMethod(other, *signature); });
                        same != existing.end()) {
                        *same = std::move(*signature);
                    } else {
                        existing.push_back(std::move(*signature));
                    }
                }
            } else {
                for (const auto* clause : ast::signatureClauses(*inherited)) {
                    preRegister(*inherited, *clause, true, &letters);
                }
            }
            if (!inherited->clauses[0].body.empty()) {
                inherit(inherited->name);
            }
            fromThisTrait.insert(key);
        }
        inheritedKeys.insert(fromThisTrait.begin(), fromThisTrait.end());
    }
}

namespace {

// Every `make` block of `items` with the module path it is written in,
// nested modules and `private do` blocks included.
template <typename Items, typename Visit>
auto forEachMakeDef(const Items& items, const std::string& path, const Visit& visit) -> void {
    for (const auto& item : items) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                    if (node) {
                        visit(*node, path);
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        forEachMakeDef(node->body, ast::nestedModulePath(path, node->name), visit);
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    // A `make` inside `private do ... end` is the module's too.
                    if (node) {
                        forEachMakeDef(node->items, path, visit);
                    }
                }
            },
            item);
    }
}

} // namespace

// Two passes: every block's own methods are known, by the type they are for,
// before any block decides which trait defaults it inherits (registerMakeSignature).
auto TypeChecker::registerMakeSignatures(const ast::Program& program) -> void {
    m_ownMethodsByTarget.clear();
    m_inheritedDefaults.clear();
    forEachMakeDef(program.items, "", [this](const ast::MakeDef& def, const std::string& path) {
        noteOwnMethods(def, path);
    });
    forEachMakeDef(program.items, "", [this](const ast::MakeDef& def, const std::string& path) {
        registerMakeSignature(def, path);
    });
}

auto TypeChecker::registerExportsInModule(const ast::ModuleDef& mod, const std::string& parentPath)
    -> void {
    const auto path = ast::nestedModulePath(parentPath, mod.name);
    for (const auto& item : mod.body) {
        std::visit(
            [this, &path](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::ExportDecl>>) {
                    if (!node) {
                        return;
                    }
                    std::string target = node->module.dotted();
                    const auto alias = node->alias.value_or(node->module.parts.back());
                    m_moduleReexports[path + "." + alias] = target;
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        registerExportsInModule(*node, path);
                    }
                }
            },
            item);
    }
}

auto TypeChecker::registerExports(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit(
            [this](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        registerExportsInModule(*node, "");
                    }
                }
            },
            item);
    }
}

auto TypeChecker::preRegisterFunctionSigs(const ast::Program& program) -> void {
    std::function<void(const ast::ModuleDef&)> registerModule;
    registerModule = [&](const ast::ModuleDef& module) {
        const auto previousModule = m_currentModulePath;
        m_currentModulePath =
            previousModule.empty() || module.name.rfind(previousModule + ".", 0) == 0
                ? module.name
                : previousModule + "." + module.name;
        for (const auto& item : module.body) {
            std::visit(
                [&](const auto& member) {
                    using M = std::decay_t<decltype(member)>;
                    if constexpr (std::is_same_v<M, std::unique_ptr<ast::FunctionDef>>) {
                        if (member) {
                            preRegisterFunctionDef(*member);
                        }
                    } else if constexpr (std::is_same_v<M, std::unique_ptr<ast::ModuleDef>>) {
                        if (member) {
                            registerModule(*member);
                        }
                    } else if constexpr (std::is_same_v<M, std::unique_ptr<ast::VisibilityBlock>>) {
                        if (!member) {
                            return;
                        }
                        for (const auto& visible : member->items) {
                            if (auto* def =
                                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible);
                                def && *def) {
                                if (!member->isPublic) {
                                    m_privateFreeFunctions.insert(
                                        m_currentModulePath + "\n" + (*def)->name);
                                }
                                preRegisterFunctionDef(**def);
                            }
                        }
                    }
                },
                item);
        }
        m_currentModulePath = previousModule;
    };
    for (const auto& item : program.items) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (node) {
                        preRegisterFunctionDef(*node);
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) {
                        registerModule(*node);
                    }
                }
            },
            item);
    }
}

auto TypeChecker::preRegisterFunctionDef(const ast::FunctionDef& def) -> void {
    // Skip make-block methods (they have an implicit `this` that mis-counts
    // arity in checkCall's UFCS desugaring, same exclusion as checkFunctionDef).
    if (m_inMakeBlock) {
        return;
    }
    // Skip annotation-declared functions — registerDeclaredSignatures already
    // populated m_userSignatures for these and checkFunctionDef will use the
    // annotation as ground truth.
    if (m_annotationDeclared.count(def.name)) {
        // An annotation supplies types, but only the definition supplies
        // parameter labels and defaults. Publish that shape before checking
        // any caller (including a main block preceding an imported module).
        const auto enrich = [&](std::vector<Signature>& signatures) {
            for (auto& signature : signatures) {
                for (const auto& clause : def.clauses) {
                    if (signature.params.size() != clause.params.size()) {
                        continue;
                    }
                    signature.paramNames.clear();
                    for (const auto& param : clause.params) {
                        signature.paramNames.push_back(param.name.value_or(""));
                    }
                    auto required = clause.params.size();
                    while (required > 0 && clause.params[required - 1].defaultValue) {
                        --required;
                    }
                    signature.requiredParams = required;
                    signature.clause = &clause;
                }
            }
        };
        if (auto scoped = m_scopedDeclaredSignatures.find(m_currentModulePath + "\n" + def.name);
            scoped != m_scopedDeclaredSignatures.end()) {
            enrich(scoped->second);
            auto qualified = m_userSignatures.find(
                m_currentModulePath.empty() ? def.name
                                            : kex::mangle::qualify(m_currentModulePath, def.name));
            if (qualified != m_userSignatures.end()) {
                enrich(qualified->second);
            }
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
                for (const auto& param : clause.params) {
                    paramTypes.push_back(
                        param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar());
                }
                std::size_t required = clause.params.size();
                while (required > 0 && clause.params[required - 1].defaultValue) {
                    required--;
                }
                Signature signature{
                    def.name, std::move(paramTypes), freshTypeVar(), false, required};
                signature.clause = &clause;
                for (const auto& param : clause.params) {
                    signature.paramNames.push_back(param.name.value_or(""));
                }
                m_userSignatures[qualified].push_back(std::move(signature));
            }
        }
        return;
    }
    const auto previousImports = m_declarationImports;
    if (auto imports = m_functionImports.find(&def); imports != m_functionImports.end()) {
        m_declarationImports = imports->second;
    }
    // A name may be pre-registered more than once: clauses of one definition
    // share a def.name, but so do separate definitions that overload by arity
    // (common for module functions, e.g. `let f(a)` and `let f(a, b)`).
    // Skipping the name outright would leave every arity but the first
    // invisible to calls checked before the definitions themselves are, so
    // only arities already present are skipped.
    const auto hasArity = [](const std::vector<Signature>* signatures, size_t arity) {
        return signatures &&
               std::any_of(signatures->begin(), signatures->end(), [&](const Signature& signature) {
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
        for (const auto& param : clause.params) {
            paramTypes.push_back(
                param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar());
        }
        std::size_t required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue) {
            required--;
        }
        auto result = clause.returnAnnotation
                          ? resolveTypeExpr(**clause.returnAnnotation, genericVars)
                          : freshTypeVar();
        Signature signature{def.name, std::move(paramTypes), std::move(result), false, required};
        signature.declaringModule = m_currentModulePath;
        signature.clause = &clause;
        // A caller checked before the definition places named arguments by
        // these (#640: `register("/", hook: ~shout)`).
        for (const auto& param : clause.params) {
            signature.paramNames.push_back(param.name.value_or(""));
        }
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
            const bool annotated =
                std::any_of(clause.params.begin(), clause.params.end(), [](const auto& param) {
                    return bool(param.type);
                });
            if (hasArity(existing, clause.params.size()) && !annotated) {
                continue;
            }
            if (!made[i]) {
                made[i] = provisionalOf(clause);
            }
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
        if (!m_qualifiedPublished.contains(qualified)) {
            for (auto& signature : provisionalFor(signaturesAt(qualified))) {
                m_userSignatures[qualified].push_back(std::move(signature));
            }
        }
    }
    for (auto& signature : provisionalFor(signaturesAt(def.name))) {
        m_userSignatures[def.name].push_back(std::move(signature));
    }
    m_declarationImports = previousImports;
}

auto TypeChecker::uncoveredConstructors(
    const std::vector<CoverageArm>& arms, const TypePtr& subject)
    -> std::optional<std::pair<std::string, std::vector<std::string>>> {
    bool hasUnguardedCatchAll = false;
    std::set<std::string> covered;
    std::string adtName;
    bool inconclusive = false;

    // A sum type declared in a module (`Shades.Shade`) keeps its constructors
    // out of the bare-name owner map, so they are found through the type
    // being matched: `Tint(n)` against a `Shades.Shade` is its `Tint`,
    // however it is written. Dropping the `Light` arm of such a match was
    // accepted, and failed at run time with no matching clause (found by
    // `make fuzz-metamorphic`).
    std::string subjectAdt;
    if (subject) {
        if (const auto* named = std::get_if<NamedType>(&resolve(subject)->kind);
            named && m_adtVariants.contains(named->name)) {
            subjectAdt = named->name;
        }
    }
    const auto ownerOf = [&](const std::string& ctorName) -> std::optional<std::string> {
        if (!subjectAdt.empty()) {
            const auto& variants = m_adtVariants[subjectAdt];
            if (std::find(variants.begin(), variants.end(), ctorName) != variants.end()) {
                return subjectAdt;
            }
        }
        if (const auto it = m_adtOfConstructor.find(ctorName); it != m_adtOfConstructor.end()) {
            return it->second;
        }
        return std::nullopt;
    };

    for (const auto& arm : arms) {
        const auto* pat = arm.pattern;
        if (!pat || std::holds_alternative<ast::WildcardPattern>(pat->kind) ||
            std::holds_alternative<ast::VarPattern>(pat->kind)) {
            if (!arm.guarded) {
                hasUnguardedCatchAll = true;
            }
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
        } else if (arm.strict) {
            return std::nullopt; // a kind of pattern this verdict cannot weigh
        } else {
            continue; // literal/list/tuple/record/range patterns don't drive ADT exhaustiveness
        }

        const auto owner = ownerOf(ctorName);
        if (!owner) {
            inconclusive = true; // unregistered constructor — can't prove the closed set
            continue;
        }
        if (adtName.empty()) {
            adtName = *owner;
        } else if (adtName != *owner) {
            inconclusive = true; // patterns span more than one ADT
        }
        if (!arm.guarded) {
            covered.insert(ctorName);
        }
    }

    if (hasUnguardedCatchAll || inconclusive || adtName.empty()) {
        return std::nullopt;
    }

    std::vector<std::string> missing;
    for (const auto& ctorName : m_adtVariants[adtName]) {
        if (!covered.count(ctorName)) {
            missing.push_back(ctorName);
        }
    }
    // Every constructor taken at the top; what their arguments take is
    // the matrix's to say (`Just(1)` beside `None` leaves `Just(2)`).
    if (missing.empty()) {
        return std::nullopt;
    }
    return std::pair{adtName, std::move(missing)};
}

namespace {
auto joinedCases(const std::vector<std::string>& cases) -> std::string {
    std::string list;
    for (size_t i = 0; i < cases.size(); i++) {
        if (i) {
            list += ", ";
        }
        list += cases[i];
    }
    return list;
}
} // namespace

auto TypeChecker::checkMatchExhaustiveness(
    const ast::MatchExpr& node, const TypePtr& subject, SourceLocation loc) -> bool {
    std::vector<CoverageArm> arms;
    for (const auto& clause : node.clauses) {
        const bool guarded = clause.guard && *clause.guard;
        for (const auto& pat : clause.patterns) {
            if (pat) {
                arms.push_back({pat.get(), guarded});
            }
        }
    }
    const auto uncovered = uncoveredConstructors(arms, subject);
    if (!uncovered) {
        return false;
    }
    const auto& [adtName, missing] = *uncovered;
    error(
        diag::Code::NonExhaustiveMatchOn, loc,
        {{"type", adtName}, {"cases", joinedCases(missing)}});
    if (const auto declared = m_adtDeclarations.find(adtName);
        declared != m_adtDeclarations.end()) {
        labelLast("", declared->second, "`" + adtName + "` is declared here");
    }
    return true;
}

// The clauses of a function are the arms of a match on its arguments, so a
// constructor no clause takes in some argument position is a value the
// function has no answer for: `let area(Circle(r))` alone, called with a
// `Square`, crashed on BEAM with `function_clause` and returned nothing on
// the walker (kexhq/kex#749). Each position is judged on its own, which is
// sound: a constructor missing from one column is missing whatever the
// other columns take. Only a definition that is the sole one of its name
// and arity in its scope is judged, since clauses written apart are
// separate definitions that overload one another.
auto TypeChecker::noteClauseCoverage(const ast::FunctionDef& def) -> void {
    if (m_inMakeBlock || m_currentMakeType || def.clauses.empty() || def.computedName) {
        return;
    }
    const auto arity = def.clauses.front().params.size();
    const ClauseCoverageKey key{m_currentModulePath, def.name, arity};
    if (!m_clauseCoverageSites[key]
             .emplace(def.location.file, def.location.line, def.location.column)
             .second) {
        return;
    }
    if (!std::all_of(def.clauses.begin(), def.clauses.end(), [&](const auto& clause) {
            return clause.params.size() == arity;
        })) {
        return;
    }
    for (size_t pi = 0; pi < arity; ++pi) {
        std::vector<CoverageArm> arms;
        for (const auto& clause : def.clauses) {
            const auto& param = clause.params[pi];
            // A named parameter takes every value of its type, whether that
            // is written or not.
            arms.push_back({param.pattern ? param.pattern->get() : nullptr, false, true});
        }
        if (auto uncovered = uncoveredConstructors(arms, nullptr)) {
            m_clauseCoverageGaps.push_back(
                {key, def.name, def.location, std::move(uncovered->first),
                 std::move(uncovered->second)});
            return;
        }
    }
}

auto TypeChecker::checkClauseCoverage() -> void {
    for (const auto& gap : m_clauseCoverageGaps) {
        if (m_clauseCoverageSites[gap.key].size() != 1) {
            continue;
        }
        error(
            diag::Code::NonExhaustiveClauses, gap.location,
            {{"name", gap.name}, {"type", gap.type}, {"cases", joinedCases(gap.missing)}});
        if (const auto declared = m_adtDeclarations.find(gap.type);
            declared != m_adtDeclarations.end()) {
            labelLast("", declared->second, "`" + gap.type + "` is declared here");
        }
    }
    m_clauseCoverageGaps.clear();
    m_clauseCoverageSites.clear();
}

// A match over a union of types (`type Reply = String | Safe | Redirect`)
// must take each member: a record pattern binding every field it names
// (`Safe { html }`) or a type pattern (`s : String`) takes one whole. Any
// other kind of arm leaves the question to checkMatchExhaustiveness (see
// kexhq/kex#645). Dropping the `Redirect` arm was accepted, and failed at run
// time with no matching clause (found by `make fuzz-metamorphic`).
auto TypeChecker::blockParameterTypes(const ast::Expr& block) const -> std::vector<std::string> {
    const auto found = m_blockHints.find(&block);
    if (found == m_blockHints.end()) {
        return {};
    }
    auto params = found->second;
    if (params.size() == 1 && params[0]) {
        if (const auto* tuple = std::get_if<TupleType>(&resolve(params[0])->kind);
            tuple && tuple->elements.size() > 1) {
            params = tuple->elements;
        }
    }
    std::vector<std::string> types;
    for (const auto& param : params) {
        const auto type = param ? resolve(param) : nullptr;
        types.push_back(!type || isUndetermined(type) ? "" : typeToString(type));
    }
    return types;
}

auto TypeChecker::owedArms(const ast::MatchExpr& node, const TypePtr& subject)
    -> std::vector<OwedArm> {
    if (!subject || isUndetermined(resolve(subject))) {
        return {};
    }
    // `X?` is its own kind of type, matched by the prelude's `Optional` arms.
    auto adt = adtNameOfType(expandTypeAliases(resolve(subject)));
    auto members = unionMemberNames(subject);
    if (members.empty() && !adt.empty()) {
        members = m_adtVariants.at(adt);
    }
    if (members.empty()) {
        return {};
    }
    if (adt.empty()) {
        if (const auto* named =
                std::get_if<NamedType>(&expandTypeAliases(resolve(subject))->kind)) {
            adt = named->name;
        }
    }
    std::vector<bool> taken(members.size(), false);
    for (const auto& clause : node.clauses) {
        if (clause.guard && *clause.guard) {
            continue;
        }
        for (const auto& pattern : clause.patterns) {
            if (!pattern) {
                continue;
            }
            std::string constructor;
            if (const auto* written = std::get_if<ast::ConstructorPattern>(&pattern->kind)) {
                constructor = written->name;
            } else if (const auto* literal = std::get_if<ast::LiteralPattern>(&pattern->kind);
                       literal && literal->literal.type == TokenType::None) {
                constructor = "None";
            }
            if (!constructor.empty()) {
                for (std::size_t i = 0; i < members.size(); ++i) {
                    taken[i] = taken[i] || members[i] == constructor;
                }
                continue;
            }
            if (const auto takes = unionMembersTaken(*pattern, members)) {
                for (std::size_t i = 0; i < members.size(); ++i) {
                    taken[i] = taken[i] || (*takes)[i];
                }
            }
        }
    }
    if (std::all_of(taken.begin(), taken.end(), [](bool whole) { return whole; })) {
        return {};
    }
    std::vector<OwedArm> owed;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (taken[i]) {
            continue;
        }
        const auto& member = members[i];
        // A union of bare names is registered as a sum type, so a record or
        // a primitive among them is listed as if a constructor: it is a type.
        const bool type = adt.empty() || isPrimitiveTypeName(member) ||
                          m_recordFields.contains(resolveRecordName(member));
        std::size_t arity = 0;
        if (!type) {
            bool found = false;
            if (const auto owners = m_constructorOwners.find(member);
                owners != m_constructorOwners.end()) {
                for (const auto& owner : owners->second) {
                    if (owner.adt == adt) {
                        arity = owner.arity;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                if (const auto known = m_constructorResult.find(member);
                    known != m_constructorResult.end() && known->second.adtName == adt) {
                    arity = known->second.slots.size();
                } else if (const auto counted = m_constructorArity.find(member);
                           counted != m_constructorArity.end()) {
                    arity = counted->second;
                }
            }
        }
        owed.push_back({member, arity, type});
    }
    return owed;
}

auto TypeChecker::unionMemberNames(const TypePtr& subject) -> std::vector<std::string> {
    const auto nameOf = [](const TypePtr& type) {
        const auto* named = std::get_if<NamedType>(&type->kind);
        return named && named->typeArgs.empty() ? named->name : typeToString(type);
    };
    // A union of bare names is registered as a sum type whose variants are
    // those names; a union with a qualified or compound member as an alias.
    std::vector<std::string> members;
    const auto shape = expandTypeAliases(resolve(subject));
    if (const auto* either = std::get_if<UnionType>(&shape->kind)) {
        for (const auto& member : either->members) {
            members.push_back(nameOf(resolve(member)));
        }
    } else if (const auto* named = std::get_if<NamedType>(&shape->kind)) {
        if (const auto variants = m_adtVariants.find(named->name);
            variants != m_adtVariants.end()) {
            members = variants->second;
        }
    }
    return members;
}

auto TypeChecker::unionMembersTaken(
    const ast::Pattern& pattern, const std::vector<std::string>& members)
    -> std::optional<std::vector<bool>> {
    std::vector<bool> taken(members.size(), false);
    const auto& kind = pattern.kind;
    if (std::holds_alternative<ast::WildcardPattern>(kind) ||
        std::holds_alternative<ast::VarPattern>(kind)) {
        std::fill(taken.begin(), taken.end(), true);
        return taken;
    }
    const auto take = [&](const std::string& written) {
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (members[i] == written || members[i].ends_with("." + written) ||
                written.ends_with("." + members[i])) {
                taken[i] = true;
            }
        }
    };
    const auto takesWhole = [](const ast::FieldPattern& field) {
        if (!field.pattern || !*field.pattern) {
            return true;
        }
        const auto& fieldKind = (**field.pattern).kind;
        return std::holds_alternative<ast::VarPattern>(fieldKind) ||
               std::holds_alternative<ast::WildcardPattern>(fieldKind);
    };
    if (const auto* typed = std::get_if<ast::TypePattern>(&kind); typed && typed->type) {
        std::unordered_map<std::string, TypePtr> noGenerics;
        const auto type = resolve(resolveTypeExpr(*typed->type, noGenerics));
        const auto* named = std::get_if<NamedType>(&type->kind);
        take(named && named->typeArgs.empty() ? named->name : typeToString(type));
        return taken;
    }
    if (const auto* record = std::get_if<ast::RecordPattern>(&kind);
        record && !record->typeName.empty()) {
        if (std::all_of(record->fields.begin(), record->fields.end(), takesWhole)) {
            take(record->typeName);
        }
        return taken;
    }
    if (const auto* literal = std::get_if<ast::LiteralPattern>(&kind);
        literal && literal->literal.type != TokenType::None) {
        return taken; // a literal takes one value, no member whole
    }
    // A constructor, a structural record, a list, a tuple: which members it
    // takes is not known here.
    return std::nullopt;
}

auto TypeChecker::checkUnionCoverage(
    const ast::MatchExpr& node, const TypePtr& subject, SourceLocation loc) -> bool {
    const auto members = unionMemberNames(subject);
    if (members.size() < 2) {
        return false;
    }
    std::vector<bool> covered(members.size(), false);
    for (const auto& clause : node.clauses) {
        if (clause.guard && *clause.guard) {
            continue;
        }
        for (const auto& pattern : clause.patterns) {
            if (!pattern) {
                continue;
            }
            const auto& kind = pattern->kind;
            if (std::holds_alternative<ast::WildcardPattern>(kind) ||
                std::holds_alternative<ast::VarPattern>(kind)) {
                return false;
            }
            const auto taken = unionMembersTaken(*pattern, members);
            if (!taken) {
                return false;
            }
            for (std::size_t i = 0; i < members.size(); ++i) {
                covered[i] = covered[i] || (*taken)[i];
            }
        }
    }
    std::string missing;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (!covered[i]) {
            missing += (missing.empty() ? "" : ", ") + members[i];
        }
    }
    if (!missing.empty()) {
        error(
            diag::Code::NonExhaustiveMatchOn, loc,
            {{"type", typeToString(subject)}, {"cases", missing}});
    }
    return true;
}

// The values a match's unguarded arms leave unmatched, through nested
// patterns: a tuple's, a list's, a constructor's arguments, a literal, a
// named record (exhaustiveness.hxx). checkMatchExhaustiveness answers for
// the top-level constructors of one sum type; this one for everything it
// had no verdict on (kexhq/kex#645). A type or range pattern leaves the
// question open, as does a constructor the checker has no siblings for.
auto TypeChecker::checkPatternMatrix(
    const ast::MatchExpr& node, const TypePtr& subject, SourceLocation loc) -> void {
    using exhaustiveness::Space;
    // Whatever the value, arms that are all guarded may all refuse it.
    if (!node.clauses.empty() &&
        std::all_of(node.clauses.begin(), node.clauses.end(), [](const auto& clause) {
            return clause.guard && *clause.guard;
        })) {
        error(diag::Code::MatchArmsAllGuarded, loc);
        return;
    }
    // A value the checker knows nothing of proves nothing missing: its arms
    // may be the cases it can take (`(:updated, n)`, `(:error, reason)`).
    if (!subject || isUndetermined(resolve(subject))) {
        return;
    }
    // A String matched by list patterns is its characters: `""` is the empty
    // list there, and `"hi"` two characters.
    const bool characterLists =
        std::any_of(node.clauses.begin(), node.clauses.end(), [](const auto& clause) {
            return std::any_of(
                clause.patterns.begin(), clause.patterns.end(), [](const auto& pattern) {
                    return pattern && std::holds_alternative<ast::ListPattern>(pattern->kind);
                });
        });
    const std::string tuple = "(,";
    const std::string empty = "[]";
    const std::string cons = "::";
    const std::string record = "rec:";
    const std::string literal = "lit:";
    const std::string range = "..";
    const std::string yes = "true";
    const std::string no = "false";
    // A named record's fields, in one order for every pattern of it.
    const auto fieldsOf = [&](const std::string& name) -> std::optional<std::vector<std::string>> {
        const auto found = m_recordFields.find(resolveRecordName(name));
        if (found == m_recordFields.end()) {
            return std::nullopt;
        }
        std::vector<std::string> fields;
        for (const auto& [field, _] : found->second) {
            fields.push_back(field);
        }
        std::sort(fields.begin(), fields.end());
        return fields;
    };
    std::function<std::optional<Space>(const ast::Pattern&)> spaceOf =
        [&](const ast::Pattern& pattern) -> std::optional<Space> {
        return std::visit(
            [&](const auto& kind) -> std::optional<Space> {
                using T = std::decay_t<decltype(kind)>;
                if constexpr (
                    std::is_same_v<T, ast::WildcardPattern> || std::is_same_v<T, ast::VarPattern>) {
                    return Space{};
                } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
                    return kind.inner ? spaceOf(*kind.inner) : std::optional<Space>{Space{}};
                } else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
                    const auto& token = kind.literal;
                    if (token.type == TokenType::None) {
                        return Space{"None", {}};
                    }
                    if (token.type == TokenType::True) {
                        return Space{yes, {}};
                    }
                    if (token.type == TokenType::False) {
                        return Space{no, {}};
                    }
                    if (characterLists && token.type == TokenType::String) {
                        Space chars{empty, {}};
                        for (auto c = token.value.rbegin(); c != token.value.rend(); ++c) {
                            chars = Space{
                                cons, {Space{literal + std::string(1, *c), {}}, std::move(chars)}};
                        }
                        return chars;
                    }
                    return Space{literal + token.value, {}};
                } else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
                    const auto arity = m_constructorArity.find(kind.name);
                    if (!m_adtOfConstructor.contains(kind.name) ||
                        (arity != m_constructorArity.end() && arity->second != kind.args.size())) {
                        return std::nullopt;
                    }
                    Space space{kind.name, {}};
                    for (const auto& argument : kind.args) {
                        auto inner = argument ? spaceOf(*argument) : std::optional<Space>{Space{}};
                        if (!inner) {
                            return std::nullopt;
                        }
                        space.arguments.push_back(std::move(*inner));
                    }
                    return space;
                } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
                    Space space{tuple + std::to_string(kind.elements.size()), {}};
                    for (const auto& element : kind.elements) {
                        auto inner = element ? spaceOf(*element) : std::optional<Space>{Space{}};
                        if (!inner) {
                            return std::nullopt;
                        }
                        space.arguments.push_back(std::move(*inner));
                    }
                    return space;
                } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
                    // `[a, b | t]` is a head and a tail twice over; `[a, b]`
                    // ends in the empty list.
                    std::optional<Space> tail = Space{empty, {}};
                    if (kind.rest) {
                        tail = *kind.rest ? spaceOf(**kind.rest) : std::optional<Space>{Space{}};
                    }
                    if (!tail) {
                        return std::nullopt;
                    }
                    for (auto element = kind.elements.rbegin(); element != kind.elements.rend();
                         ++element) {
                        auto head = *element ? spaceOf(**element) : std::optional<Space>{Space{}};
                        if (!head) {
                            return std::nullopt;
                        }
                        tail = Space{cons, {std::move(*head), std::move(*tail)}};
                    }
                    return tail;
                } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
                    const auto fields =
                        kind.typeName.empty() ? std::nullopt : fieldsOf(kind.typeName);
                    if (!fields) {
                        return std::nullopt;
                    }
                    Space space{record + kind.typeName, std::vector<Space>(fields->size())};
                    for (const auto& field : kind.fields) {
                        const auto at = std::find(fields->begin(), fields->end(), field.name);
                        if (at == fields->end()) {
                            return std::nullopt;
                        }
                        if (!field.pattern || !*field.pattern) {
                            continue;
                        }
                        auto inner = spaceOf(**field.pattern);
                        if (!inner) {
                            return std::nullopt;
                        }
                        space.arguments[static_cast<std::size_t>(at - fields->begin())] =
                            std::move(*inner);
                    }
                    return space;
                } else if constexpr (std::is_same_v<T, ast::RangePattern>) {
                    // A range pattern takes a Range apart into its bounds, as a
                    // constructor of two arguments: `(x..y)` covers every
                    // range, `('a'..'z')` that one only, so arms of literal
                    // ranges alone still owe a fallback (one dropped ran into
                    // "no matching clause").
                    Space space{range, {}};
                    for (const auto* side : {&kind.start, &kind.end}) {
                        auto inner = *side ? spaceOf(**side) : std::optional<Space>{Space{}};
                        if (!inner) {
                            return std::nullopt;
                        }
                        space.arguments.push_back(std::move(*inner));
                    }
                    return space;
                } else {
                    return std::nullopt;
                }
            },
            pattern.kind);
    };

    // A map is never taken whole by `{ k }`: `match` does not destructure
    // a map (spec/record_pattern_anonymous.kex), and one can lack the key.
    // Such an arm covers what a literal does, so a fallback is still owed.
    const bool mapSubject = std::holds_alternative<MapType>(resolve(subject)->kind);
    // A named record pattern asserts the record's type, so `Pt { x }` takes
    // no `Qt` even where the two share their fields
    // (spec/named_record_pattern.kex). Against a subject of one record type,
    // a pattern naming another covers what a literal does, and a fallback is
    // still owed. Dropping the `_` beside such an arm was accepted, and failed
    // at run time with no matching clause (found by `make fuzz-metamorphic`).
    std::string subjectRecord;
    if (const auto* named = std::get_if<NamedType>(&resolve(subject)->kind)) {
        if (const auto name = resolveRecordName(named->name); m_recordFields.contains(name)) {
            subjectRecord = name;
        }
    }
    std::vector<std::vector<Space>> rows;
    for (const auto& clause : node.clauses) {
        if (clause.guard && *clause.guard) {
            continue;
        }
        for (const auto& pattern : clause.patterns) {
            if (!pattern) {
                continue;
            }
            if (const auto* structural = std::get_if<ast::RecordPattern>(&pattern->kind);
                mapSubject && structural && structural->typeName.empty()) {
                rows.push_back({Space{literal + "{}", {}}});
                continue;
            }
            if (const auto* named = std::get_if<ast::RecordPattern>(&pattern->kind);
                named && !subjectRecord.empty() && !named->typeName.empty() &&
                resolveRecordName(named->typeName) != subjectRecord) {
                rows.push_back({Space{literal + named->typeName + "{}", {}}});
                continue;
            }
            auto space = spaceOf(*pattern);
            if (!space) {
                return;
            }
            rows.push_back({std::move(*space)});
        }
    }
    if (rows.empty()) {
        return;
    }

    const exhaustiveness::Siblings siblings = [&](const std::string& constructor)
        -> std::optional<std::vector<std::pair<std::string, std::size_t>>> {
        if (constructor == yes || constructor == no) {
            return std::vector<std::pair<std::string, std::size_t>>{{yes, 0}, {no, 0}};
        }
        if (constructor == empty || constructor == cons) {
            return std::vector<std::pair<std::string, std::size_t>>{{empty, 0}, {cons, 2}};
        }
        if (constructor.starts_with(tuple)) {
            return std::vector<std::pair<std::string, std::size_t>>{
                {constructor, std::stoul(constructor.substr(tuple.size()))}};
        }
        if (constructor == range) {
            return std::vector<std::pair<std::string, std::size_t>>{{range, 2}};
        }
        if (constructor.starts_with(record)) {
            const auto fields = fieldsOf(constructor.substr(record.size()));
            if (!fields) {
                return std::nullopt;
            }
            return std::vector<std::pair<std::string, std::size_t>>{{constructor, fields->size()}};
        }
        if (constructor.starts_with(literal)) {
            return std::nullopt;
        }
        const auto adt = m_adtOfConstructor.find(constructor);
        if (adt == m_adtOfConstructor.end()) {
            return std::nullopt;
        }
        const auto variants = m_adtVariants.find(adt->second);
        if (variants == m_adtVariants.end()) {
            return std::nullopt;
        }
        std::vector<std::pair<std::string, std::size_t>> all;
        for (const auto& variant : variants->second) {
            const auto arity = m_constructorArity.find(variant);
            all.emplace_back(variant, arity == m_constructorArity.end() ? 0 : arity->second);
        }
        return all;
    };
    const auto missing = exhaustiveness::uncovered(rows, 1, siblings, 4);
    if (missing.empty()) {
        return;
    }

    std::function<std::string(const Space&)> shown = [&](const Space& space) -> std::string {
        const auto joined = [&](const std::vector<Space>& parts) {
            std::string out;
            for (const auto& part : parts) {
                out += (out.empty() ? "" : ", ") + shown(part);
            }
            return out;
        };
        if (space.constructor.empty()) {
            return "_";
        }
        if (space.constructor.starts_with(tuple)) {
            return "(" + joined(space.arguments) + ")";
        }
        if (space.constructor == range && space.arguments.size() == 2) {
            return "(" + shown(space.arguments[0]) + ".." + shown(space.arguments[1]) + ")";
        }
        if (space.constructor == empty) {
            return "[]";
        }
        if (space.constructor == cons) {
            // A run of heads, then the tail it ends in.
            std::vector<Space> heads;
            const Space* tail = &space;
            while (tail->constructor == cons) {
                heads.push_back(tail->arguments[0]);
                tail = &tail->arguments[1];
            }
            if (tail->constructor == empty) {
                return "[" + joined(heads) + "]";
            }
            return "[" + joined(heads) + " | " + shown(*tail) + "]";
        }
        if (space.constructor.starts_with(record)) {
            const auto name = space.constructor.substr(record.size());
            const auto fields = fieldsOf(name).value_or(std::vector<std::string>{});
            std::string out;
            for (std::size_t i = 0; i < space.arguments.size() && i < fields.size(); ++i) {
                if (!space.arguments[i].constructor.empty()) {
                    out += (out.empty() ? "" : ", ") + fields[i] + ": " + shown(space.arguments[i]);
                }
            }
            return name + " { " + out + (out.empty() ? "" : " ") + "}";
        }
        if (space.arguments.empty()) {
            return space.constructor;
        }
        return space.constructor + "(" + joined(space.arguments) + ")";
    };
    std::string list;
    for (const auto& row : missing) {
        list += (list.empty() ? "" : ", ") + shown(row.front());
    }
    error(diag::Code::NonExhaustiveMatch, loc, {{"cases", list}});
}

auto TypeChecker::expandTypeAliases(const TypePtr& type, int depth) const -> TypePtr {
    if (!type || depth > 8) {
        return type;
    }
    auto recur = [&](const TypePtr& inner) { return expandTypeAliases(inner, depth + 1); };
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        std::vector<TypePtr> args;
        for (const auto& arg : named->typeArgs) {
            args.push_back(recur(arg));
        }
        const auto dot = named->name.rfind('.');
        const auto last = dot == std::string::npos ? named->name : named->name.substr(dot + 1);
        // Distinct types are nominal: `distinct type Meters = Integer` must
        // NOT collapse to Integer, so mirror resolveTypeExpr's precedence.
        if (!m_distinctTypes.count(resolveDistinctName(last)) &&
            !m_recordFields.count(named->name)) {
            if (auto alias = m_typeAliases.find(last); alias != m_typeAliases.end()) {
                return expandTypeAliases(
                    args.empty() ? alias->second : substituteInterfaceGenerics(alias->second, args),
                    depth + 1);
            }
        }
        if (args.empty()) {
            return type;
        }
        return Type::named(named->name, std::move(args));
    }
    if (auto* list = std::get_if<ListType>(&type->kind)) {
        return Type::list(recur(list->element));
    }
    if (auto* optional = std::get_if<OptionalType>(&type->kind)) {
        return Type::optional(recur(optional->inner));
    }
    if (auto* map = std::get_if<MapType>(&type->kind)) {
        return Type::map(recur(map->key), recur(map->value));
    }
    if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
        std::vector<TypePtr> elements;
        for (const auto& element : tuple->elements) {
            elements.push_back(recur(element));
        }
        return Type::tuple(std::move(elements));
    }
    if (auto* fn = std::get_if<FuncType>(&type->kind)) {
        std::vector<TypePtr> params;
        for (const auto& param : fn->params) {
            params.push_back(recur(param));
        }
        return Type::func(std::move(params), recur(fn->result), fn->block, fn->foul);
    }
    if (auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
        std::vector<TypePtr> members;
        for (const auto& member : intersection->members) {
            members.push_back(recur(member));
        }
        return Type::intersection(std::move(members));
    }
    if (auto* record = std::get_if<RecordType>(&type->kind)) {
        std::vector<std::pair<std::string, TypePtr>> fields;
        for (const auto& [name, fieldType] : record->fields) {
            fields.emplace_back(name, recur(fieldType));
        }
        return Type::record(std::move(fields));
    }
    return type;
}

auto TypeChecker::normalizeIntersection(TypePtr type) const -> TypePtr {
    auto* meet = std::get_if<IntersectionType>(&type->kind);
    if (!meet) {
        return type;
    }

    TypePtr concreteRecord;
    for (const auto& member : meet->members) {
        auto* named = std::get_if<NamedType>(&member->kind);
        if (!named || !(findRecordFields(*named) != m_recordFields.end())) {
            continue;
        }
        if (concreteRecord && !typesEqual(concreteRecord, member)) {
            const auto& left = std::get<NamedType>(concreteRecord->kind).name;
            const auto& right = named->name;
            return Type::never(left, right);
        }
        concreteRecord = member;
    }
    if (!concreteRecord) {
        return type;
    }

    for (const auto& member : meet->members) {
        if (typesEqual(member, concreteRecord)) {
            continue;
        }
        if (!argMatchesParam(concreteRecord, member)) {
            return Type::never();
        }
    }
    return concreteRecord;
}

auto TypeChecker::resolveTypeExpr(
    const ast::TypeExpr& typeExpr, std::unordered_map<std::string, TypePtr>& genericVars)
    -> TypePtr {
    auto resolved = resolveTypeExprNode(typeExpr, genericVars);
    // The record a written type name means here, for code that holds only
    // the annotation (lowering's run-time guards).
    if ((std::holds_alternative<ast::TypeName>(typeExpr.kind) ||
         std::holds_alternative<ast::GenericType>(typeExpr.kind)) &&
        resolved) {
        if (const auto* named = std::get_if<NamedType>(&resolve(resolved)->kind)) {
            if (m_recordFields.count(named->name)) {
                m_annotatedRecords[&typeExpr] = named->name;
            }
            // Any declared type, for tooling's go-to-definition.
            if (m_declaredTypeArity.count(named->name)) {
                m_annotatedTypes[&typeExpr] = named->name;
            }
            // A generic type written bare is a missing argument, not "any
            // instantiation": `scaled(m: Measure)` read as `Measure<anything>`
            // (docs/typechecker-hm-plan.md §8.1, fix 3). A `make` target says
            // every instantiation on purpose, and `Type` carries no argument.
            if (const auto* written = std::get_if<ast::TypeName>(&typeExpr.kind);
                written && named->typeArgs.empty() && !m_makeTargetNodes.contains(&typeExpr) &&
                named->name != kex::names::kTypeRecord &&
                written->dotted() != kex::names::kThisType) {
                std::size_t declared = 0;
                if (const auto arity = m_declaredTypeArity.find(named->name);
                    arity != m_declaredTypeArity.end()) {
                    declared = arity->second;
                } else if (m_importedInterfaces) {
                    // An imported type: `Measure` from the stdlib.
                    if (const auto imported = m_importedInterfaces->typeArities.find(named->name);
                        imported != m_importedInterfaces->typeArities.end()) {
                        declared = imported->second;
                    }
                }
                if (declared > 0) {
                    reportTypeArgumentCount(typeExpr, written->dotted(), declared, 0);
                }
            }
        }
    }
    // Every written name with what it resolved to, for tooling's hover. An
    // annotation is resolved more than once (registration, then the check);
    // the last, made in its own scope, is the one kept.
    if (resolved && typeExpr.location.line > 0) {
        const auto* name = std::get_if<ast::TypeName>(&typeExpr.kind);
        if (const auto* generic = std::get_if<ast::GenericType>(&typeExpr.kind)) {
            name = &generic->name;
        }
        if (name && !name->parts.empty()) {
            m_typeReferences.push_back(
                {typeExpr.location, name->dotted(), resolved,
                 m_inMakeBlock ? m_currentMakeType : nullptr});
        }
    }
    return resolved;
}

auto TypeChecker::atImplementingType(Signature signature, const TypePtr& implementingType)
    -> std::optional<Signature> {
    // `make Box` is every instantiation: as `Box<V>`, it can match `F<A>`.
    auto applied = implementingType;
    if (const auto* named = std::get_if<NamedType>(&implementingType->kind);
        named && named->typeArgs.empty()) {
        std::size_t arity = 0;
        if (const auto declaredArity = m_declaredTypeArity.find(named->name);
            declaredArity != m_declaredTypeArity.end()) {
            arity = declaredArity->second;
        } else if (m_importedInterfaces) {
            if (const auto imported = m_importedInterfaces->typeArities.find(named->name);
                imported != m_importedInterfaces->typeArities.end()) {
                arity = imported->second;
            }
        }
        if (arity > 0) {
            std::vector<TypePtr> args;
            for (std::size_t i = 0; i < arity; ++i) {
                args.push_back(freshTypeVar());
            }
            applied = Type::named(named->name, std::move(args));
        }
    }
    const auto* declaredReceiver =
        signature.params.empty() ? nullptr : std::get_if<TypeApp>(&signature.params.front()->kind);
    const auto* head =
        declaredReceiver ? std::get_if<TypeVar>(&declaredReceiver->head->kind) : nullptr;
    const auto implementing = applicationOf(applied);
    if (!head || !implementing || implementing->args.size() != declaredReceiver->args.size()) {
        return std::nullopt;
    }
    // `F` is the implementing type's constructor, and the receiver's letters
    // its arguments, wherever the signature writes them.
    std::unordered_map<int, TypePtr> filled;
    for (std::size_t i = 0; i < declaredReceiver->args.size(); ++i) {
        if (const auto* letter = std::get_if<TypeVar>(&declaredReceiver->args[i]->kind)) {
            filled[letter->id] = implementing->args[i];
        }
    }
    const int headId = head->id;
    std::function<TypePtr(const TypePtr&)> fill = [&](const TypePtr& type) -> TypePtr {
        if (!type) {
            return type;
        }
        if (const auto* app = std::get_if<TypeApp>(&type->kind)) {
            const auto* appHead = std::get_if<TypeVar>(&app->head->kind);
            if (appHead && appHead->id == headId) {
                std::vector<TypePtr> args;
                for (const auto& arg : app->args) {
                    args.push_back(fill(arg));
                }
                return applyConstructor(implementing->head, std::move(args));
            }
        }
        if (const auto* letter = std::get_if<TypeVar>(&type->kind)) {
            if (const auto found = filled.find(letter->id); found != filled.end()) {
                return found->second;
            }
        }
        return mapTypeChildren(type, fill);
    };
    for (auto& param : signature.params) {
        param = fill(param);
    }
    signature.params.front() = applied;
    signature.result = fill(signature.result);
    return signature;
}

auto TypeChecker::constructorRequirementAt(
    const Signature& required, const TypePtr& implementingType) -> std::optional<Signature> {
    if (required.params.empty() || !required.params.front() ||
        !std::holds_alternative<TypeApp>(required.params.front()->kind)) {
        return std::nullopt;
    }
    // Each implementation's own letters, never the trait's.
    std::unordered_map<int, TypePtr> fresh;
    std::function<TypePtr(const TypePtr&)> freshen = [&](const TypePtr& type) -> TypePtr {
        if (const auto* letter = type ? std::get_if<TypeVar>(&type->kind) : nullptr) {
            auto [found, added] = fresh.try_emplace(letter->id);
            if (added) {
                found->second = freshTypeVar();
            }
            return found->second;
        }
        if (const auto* app = type ? std::get_if<TypeApp>(&type->kind) : nullptr) {
            // The head stays the trait's `F`, which atImplementingType replaces.
            std::vector<TypePtr> args;
            for (const auto& arg : app->args) {
                args.push_back(freshen(arg));
            }
            return Type::app(app->head, std::move(args));
        }
        return type ? mapTypeChildren(type, freshen) : type;
    };
    auto signature = required;
    for (auto& param : signature.params) {
        param = freshen(param);
    }
    signature.result = freshen(signature.result);
    auto applied = atImplementingType(std::move(signature), implementingType);
    if (applied) {
        applied->params.erase(applied->params.begin());
    }
    return applied;
}

auto TypeChecker::traitRequirementInDefault(const Signature& required) -> Signature {
    const int varMark = m_nextTypeVar;
    const auto letters = m_currentTrait
                             ? m_traitParameterLetters.find({m_currentTrait->name, required.name})
                             : m_traitParameterLetters.end();
    // The trait's parameters are the default's own letters; the rest are this
    // call's.
    std::unordered_map<int, TypePtr> renamed;
    if (letters != m_traitParameterLetters.end()) {
        for (const auto& [parameter, id] : letters->second) {
            if (const auto own = m_bodyTypeVars.find(parameter); own != m_bodyTypeVars.end()) {
                renamed.emplace(id, own->second);
            }
        }
    }
    std::function<TypePtr(const TypePtr&)> rename = [&](const TypePtr& type) -> TypePtr {
        if (const auto* letter = type ? std::get_if<TypeVar>(&type->kind) : nullptr) {
            auto [found, added] = renamed.try_emplace(letter->id);
            if (added) {
                found->second = freshTypeVar();
            }
            return found->second;
        }
        return type ? mapTypeChildren(type, rename) : type;
    };
    auto signature = required;
    signature.params = requirementParams(required);
    for (auto& param : signature.params) {
        param = rename(param);
    }
    signature.result = rename(signature.result);
    // The receiver is `this`, the default's `This`: put before the rest, or in
    // place of the `F<A>` a trait over a constructor writes.
    if (implicitReceivers(required) == 1) {
        signature.params.insert(signature.params.begin(), m_currentMakeType);
    } else if (!signature.params.empty()) {
        signature.params.front() = m_currentMakeType;
    }
    // Its own letters are each call's: one `A` for the seed and the block.
    generalizeSignature(signature, varMark, nullptr, true);
    return signature;
}

auto TypeChecker::ownType(const TypePtr& type) const -> TypePtr {
    const auto* named = type ? std::get_if<NamedType>(&type->kind) : nullptr;
    if (!named || named->shadowsPrelude || !m_preludeShadows.contains(named->name)) {
        return type;
    }
    auto own = *named;
    own.shadowsPrelude = true;
    return std::make_shared<Type>(Type{std::move(own)});
}

auto TypeChecker::findRecordFields(const NamedType& named) const
    -> std::unordered_map<std::string, std::unordered_map<std::string, TypePtr>>::const_iterator {
    if (!named.shadowsPrelude && m_preludeShadows.contains(named.name)) {
        return m_recordFields.find(preludeRecordKey(named.name));
    }
    return m_recordFields.find(resolveRecordName(named.name));
}

auto TypeChecker::resolveTypeExprNode(
    const ast::TypeExpr& typeExpr, std::unordered_map<std::string, TypePtr>& genericVars)
    -> TypePtr {
    if (outOfStack(typeExpr.location)) {
        return Type::unknown();
    }
    // A written name the program declares for itself means its own type,
    // also when the prelude has a type of that name.
    const auto* written = std::get_if<ast::TypeName>(&typeExpr.kind);
    if (const auto* generic = std::get_if<ast::GenericType>(&typeExpr.kind)) {
        written = &generic->name;
    }
    const bool ownName =
        written && written->parts.size() == 1 && m_preludeShadows.contains(written->parts.front());
    auto resolved = std::visit(
        [this, &genericVars, &typeExpr](const auto& node) -> TypePtr {
            using T = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<T, ast::TypeQuery>) {
                return resolveTypeQuery(node);
            } else if constexpr (std::is_same_v<T, ast::TypeSplice>) {
                // Generation replaces every splice it runs; one left over is
                // outside a generated declaration.
                if (m_reportedTypeSplices.insert(&typeExpr).second) {
                    error(diag::Code::TypeSpliceOutsideGeneration, typeExpr.location, {});
                }
                return Type::unknown();
            } else if constexpr (std::is_same_v<T, ast::TypeName>) {
                if (node.parts.empty()) {
                    return Type::unknown();
                }
                const std::string& last = node.parts.back();
                std::string qualified = node.dotted();
                // `This` inside a make block refers to the implementing type.
                if (last == names::kThisType && node.parts.size() == 1 && m_inMakeBlock &&
                    m_currentMakeType) {
                    return m_currentMakeType;
                }
                // `Never` — the bottom/uninhabited type for never-returning functions.
                // `Void` is an alias for the unit type `()` (Swift-style naming).
                if (last == kex::names::kNeverType && node.parts.size() == 1) {
                    return Type::never();
                }
                if (last == kex::names::kVoidType && node.parts.size() == 1) {
                    return Type::voidType();
                }
                // `Any` — escape hatch: unifies with everything (unknown message type
                // for Process<Any>, etc.). Treated as UnknownType at the type level.
                if (last == kex::names::kAnyType && node.parts.size() == 1) {
                    return Type::any();
                }
                // Single-letter identifiers are generic type variables (docs/
                // types.md Generics) — reuse the same TypeVar for repeated
                // occurrences of the same letter within this clause.
                if (last.size() == 1 && node.parts.size() == 1) {
                    auto it = genericVars.find(last);
                    if (it != genericVars.end()) {
                        return it->second;
                    }
                    auto var = boundedPlaceholder(typeExpr, last);
                    genericVars[last] = var;
                    return var;
                }
                if (node.parts.size() == 1) {
                    const auto recordName = resolveRecordName(last);
                    if (recordName != last) {
                        return Type::named(recordName);
                    }
                    // The program's own type wins over an imported alias of that
                    // bare name: `record Handler` beside Net.HTTP's `Handler`
                    // alias, which a parameter `h: Handler` used to resolve to,
                    // though nothing imported Net.HTTP.
                    if (m_programRecords.contains(last) ||
                        (m_adtDeclarations.contains(last) &&
                         !kex::isStandardLibrarySource(m_adtDeclarations.at(last).file))) {
                        return Type::named(last);
                    }
                } else {
                    // `FileMode.Write`: a nullary variant of the ADT the rest of
                    // the name resolves to, which is how a constructor is written
                    // as a type.
                    ast::TypeExpr owner;
                    owner.kind = ast::TypeName{{node.parts.begin(), node.parts.end() - 1}};
                    const auto ownerType = [&] {
                        const bool reporting = m_reportUnknownTypes;
                        m_reportUnknownTypes = false;
                        auto resolved = resolveTypeExprNode(owner, genericVars);
                        m_reportUnknownTypes = reporting;
                        return resolved;
                    }();
                    if (const auto* adt = std::get_if<NamedType>(&ownerType->kind);
                        adt && adt->typeArgs.empty() && adt->variant.empty() &&
                        isNullaryVariant(adt->name, last)) {
                        return Type::variantOf(adt->name, last);
                    }
                    recordWithOwner(m_referencedModules, qualified);
                    if (auto nominal = qualifiedNominalType(qualified)) {
                        return Type::named(*nominal);
                    }
                }
                if (auto known = m_globals.get(last)) {
                    return known;
                }
                // Trait-only names (Float, Number, Comparable, ...) have no
                // concrete Type — m_globals deliberately has no entry for them
                // (see check()'s comment) — so a param annotated `Float` means
                // "any T satisfying Float", same as an explicit constraint.
                if (const auto trait =
                        resolveTraitReference(qualified, m_currentModulePath, nullptr, false);
                    m_traits.get(trait)) {
                    // What the spelling means here, for code that sees only the
                    // spelling (the run-time dispatchers' guards); the numeric
                    // constraints are the language's own types.
                    if (!isPrimitiveTypeName(trait)) {
                        m_writtenTraits[writtenKey(m_currentModulePath, qualified)] = trait;
                    }
                    return Type::constrained(trait, trait);
                }
                if (auto distinct = resolveDistinctName(last); m_distinctTypes.count(distinct)) {
                    return Type::named(distinct);
                }
                // User type alias (e.g. `type Level = :debug | :info | ...`)
                auto aliasIt = m_typeAliases.find(last);
                if (aliasIt != m_typeAliases.end()) {
                    return aliasIt->second;
                }
                // An imported alias is keyed by its identity (`Net.HTTP.Handler`):
                // written as a `using` or an enclosing module reaches it
                // (`Handler` under `using Net.HTTP`).
                if (const auto alias = aliasReachedAs(qualified)) {
                    return *alias;
                }
                // Bare `List` is the erased form of `[X]`, the way `type
                // List<X> = [X]` declares it. Left nominal, a `to(List)` result
                // matched no `[A]` receiver and every list method on it reported
                // whatever unrelated overload sorted first (`zip` blamed Range).
                if (last == kex::names::kListType && node.parts.size() == 1) {
                    return Type::list(Type::unknown());
                }
                if (node.parts.size() > 1) {
                    const bool isNominal = m_recordFields.count(qualified) ||
                                           m_adtVariants.count(qualified) ||
                                           (m_importedInterfaces &&
                                            m_importedInterfaces->recordArities.count(qualified));
                    // A qualified name means the type it spells or none: its last
                    // segment naming some type somewhere is not that type.
                    // `OptionParser.ParsedOptions`, for a record declared outside
                    // `module OptionParser`, was accepted, and the first field read
                    // on it chose Mock.Env's `values` instead.
                    if (!namesDeclaredType(qualified, qualified)) {
                        reportUnknownType(node, typeExpr.location);
                    }
                    // Not known here, a qualified name still names the type it
                    // spells. Cut to its last segment, `Net.Socket.TCP.Endpoint`
                    // in Net.HTTP's interface became a bare `Endpoint` (Net.Socket
                    // is not loaded where Net.HTTP is checked), which no user of
                    // `Server.localAddress` could resolve: `.port` on it was
                    // Unknown, and an undefined method passed the check.
                    (void)isNominal;
                    return Type::named(qualified);
                }
                if (!namesDeclaredType(qualified, last)) {
                    reportUnknownType(node, typeExpr.location);
                }
                return Type::named(resolveRecordName(last));
            } else if constexpr (std::is_same_v<T, ast::GenericType>) {
                std::vector<TypePtr> args;
                for (const auto& arg : node.args) {
                    args.push_back(arg ? resolveTypeExpr(*arg, genericVars) : Type::unknown());
                }
                std::string name = node.name.parts.empty() ? "" : node.name.parts.back();
                // `This<B>` in a make block: its target's constructor applied again,
                // the holes a type lambda leaves filled (`make Tagged<_, E>`:
                // `Tagged<B, E>`), or every argument replaced (#651 question 3).
                if (name == kex::names::kThisType && node.name.parts.size() == 1 && m_inMakeBlock &&
                    m_currentMakeType) {
                    if (const auto applied = applicationOf(resolve(m_currentMakeType))) {
                        auto filled = applied->args;
                        if (!m_currentMakeHoles.empty() &&
                            args.size() == m_currentMakeHoles.size()) {
                            for (std::size_t i = 0; i < args.size(); ++i) {
                                if (m_currentMakeHoles[i] < filled.size()) {
                                    filled[m_currentMakeHoles[i]] = args[i];
                                }
                            }
                            return applyConstructor(applied->head, std::move(filled));
                        }
                        if (args.size() == filled.size()) {
                            return applyConstructor(applied->head, std::move(args));
                        }
                        reportTypeArgumentCount(
                            typeExpr, name,
                            m_currentMakeHoles.empty() ? filled.size() : m_currentMakeHoles.size(),
                            args.size());
                    } else if (
                        const auto* bare =
                            std::get_if<NamedType>(&resolve(m_currentMakeType)->kind)) {
                        // A bare target (`make Box`): every argument the type declares.
                        std::size_t declared = 0;
                        if (const auto local = m_declaredTypeArity.find(bare->name);
                            local != m_declaredTypeArity.end()) {
                            declared = local->second;
                        } else if (m_importedInterfaces) {
                            if (const auto imported =
                                    m_importedInterfaces->typeArities.find(bare->name);
                                imported != m_importedInterfaces->typeArities.end()) {
                                declared = imported->second;
                            }
                        }
                        if (declared > 0 && args.size() == declared) {
                            return applyConstructor(Type::named(bare->name), std::move(args));
                        }
                        reportTypeArgumentCount(typeExpr, name, declared, args.size());
                    }
                    return Type::unknown();
                }
                // `F<A>`: a type variable applied, the head a constructor not
                // known yet (docs/typechecker-hm-plan.md §8.2). One `F` per clause,
                // shared with its other occurrences, bounded where `F is Functor`.
                if (node.name.parts.size() == 1 && name.size() == 1) {
                    auto head = genericVars.find(name);
                    if (head == genericVars.end()) {
                        head = genericVars.emplace(name, boundedPlaceholder(typeExpr, name)).first;
                    }
                    // Its kind: a bound over constructors of one argument
                    // (`F is Functor`, `Functor<F<_>>`) takes one.
                    if (const auto* bound = std::get_if<ConstrainedType>(&head->second->kind)) {
                        const auto* trait = m_traits.get(bound->traitName);
                        if (trait && !trait->paramKinds.empty() && trait->paramKinds[0] > 0 &&
                            trait->paramKinds[0] != args.size()) {
                            reportTypeArgumentCount(
                                typeExpr, name, trait->paramKinds[0], args.size());
                        }
                    }
                    return Type::app(head->second, std::move(args));
                }
                // As many type arguments as the type declares (kexhq/kex#648):
                // `Optional<Integer, String, Bool>` meant an Optional of Integer.
                // A bare name stays the type over any arguments.
                {
                    std::optional<std::size_t> declared;
                    if (name == kex::names::kOptionalType || name == kex::names::kListType) {
                        declared = 1;
                    } else if (name == kex::names::kResultType || name == kex::names::kMapType) {
                        declared = 2;
                    } else if (const auto found = m_declaredTypeArity.find(
                                   node.name.parts.size() > 1 ? node.name.dotted()
                                                              : resolveRecordName(name));
                               found != m_declaredTypeArity.end()) {
                        declared = found->second;
                    }
                    // `Type<T>` names a type as a value; the record carries no T.
                    if (declared && *declared != args.size() && name != kex::names::kTypeRecord) {
                        reportTypeArgumentCount(
                            typeExpr, node.name.dotted(), *declared, args.size());
                    }
                }
                // A qualified name means the type it spells and no other:
                // `Net.HTTP.Request<Binary>` written inside a module declaring
                // its own `Request` resolved to that local record by its last
                // segment alone (kexhq/kex#476).
                if (node.name.parts.size() > 1) {
                    std::string qualified = node.name.dotted();
                    if (auto nominal = qualifiedNominalType(qualified)) {
                        return Type::named(*nominal, std::move(args));
                    }
                }
                if (name == kex::names::kOptionalType && args.size() == 1) {
                    return Type::optional(args[0]);
                }
                if (name == kex::names::kMapType && args.size() == 2) {
                    return Type::map(args[0], args[1]);
                }
                // `List<T>` is `[T]`, matching what the prelude-interface and
                // KexI readers already build for the same spelling.
                if (name == kex::names::kListType && args.size() == 1) {
                    return Type::list(args[0]);
                }
                if (auto distinct = resolveDistinctName(name); m_distinctTypes.count(distinct)) {
                    return Type::named(distinct, std::move(args));
                }
                // An applied generic alias is its body with the arguments put in:
                // `Predicate<E>` for `type Predicate<E> = E -> Bool` is `E -> Bool`.
                // Left as a named type, no lambda ever matched it (#337).
                if (auto alias = m_typeAliases.find(name);
                    alias != m_typeAliases.end() &&
                    !m_recordFields.count(resolveRecordName(name))) {
                    return substituteInterfaceGenerics(alias->second, args);
                }
                if (node.name.parts.size() > 1) {
                    std::string qualified = node.name.dotted();
                    if (m_adtVariants.count(qualified)) {
                        return Type::named(qualified, std::move(args));
                    }
                }
                if (!namesDeclaredType(node.name.dotted(), name)) {
                    reportUnknownType(node.name, typeExpr.location);
                }
                return Type::named(resolveRecordName(name), std::move(args));
            } else if constexpr (std::is_same_v<T, ast::FunctionType>) {
                // Same ambiguity as annotationToSignature: `A -> B -> C` and
                // `A -> (B -> C)` build the identical right-nested FunctionType,
                // so once inside this chain, a parenthesized result marks a
                // genuine function-typed RESULT rather than another curried
                // parameter, and stops the unroll.
                std::vector<TypePtr> params;
                params.push_back(
                    node.param ? resolveTypeExpr(*node.param, genericVars) : Type::unknown());
                const ast::TypeExpr* resultExpr = node.result.get();
                while (resultExpr && !resultExpr->parenthesized) {
                    const auto* next = std::get_if<ast::FunctionType>(&resultExpr->kind);
                    if (!next) {
                        break;
                    }
                    params.push_back(
                        next->param ? resolveTypeExpr(*next->param, genericVars) : Type::unknown());
                    resultExpr = next->result.get();
                }
                auto result =
                    resultExpr ? resolveTypeExpr(*resultExpr, genericVars) : Type::unknown();
                return Type::func(std::move(params), std::move(result));
            } else if constexpr (std::is_same_v<T, ast::BlockType>) {
                std::vector<TypePtr> params;
                for (const auto& param : node.params) {
                    params.push_back(
                        param ? resolveTypeExpr(*param, genericVars) : Type::unknown());
                }
                auto result =
                    node.inner ? resolveTypeExpr(*node.inner, genericVars) : Type::unknown();
                return Type::func(std::move(params), std::move(result), true);
            } else if constexpr (std::is_same_v<T, ast::TupleType>) {
                // `()` written as a type is the unit, Void: what `IO.printLine`
                // answers is a `()`, so a `Block<[()]>` collects it.
                if (node.elements.empty()) {
                    return Type::voidType();
                }
                std::vector<TypePtr> elements;
                for (const auto& elem : node.elements) {
                    elements.push_back(
                        elem ? resolveTypeExpr(*elem, genericVars) : Type::unknown());
                }
                return Type::tuple(std::move(elements));
            } else if constexpr (std::is_same_v<T, ast::ListType>) {
                return Type::list(
                    node.element ? resolveTypeExpr(*node.element, genericVars) : Type::unknown());
            } else if constexpr (std::is_same_v<T, ast::MapType>) {
                return Type::map(
                    node.key ? resolveTypeExpr(*node.key, genericVars) : Type::unknown(),
                    node.value ? resolveTypeExpr(*node.value, genericVars) : Type::unknown());
            } else if constexpr (std::is_same_v<T, ast::OptionalType>) {
                return Type::optional(
                    node.inner ? resolveTypeExpr(*node.inner, genericVars) : Type::unknown());
            } else if constexpr (std::is_same_v<T, ast::UnionType>) {
                std::vector<TypePtr> members;
                members.push_back(
                    node.left ? resolveTypeExpr(*node.left, genericVars) : Type::unknown());
                members.push_back(
                    node.right ? resolveTypeExpr(*node.right, genericVars) : Type::unknown());
                auto written = std::make_shared<Type>(Type{UnionType{std::move(members)}});
                reportIndistinguishable(written, typeExpr.location);
                return written;
            } else if constexpr (std::is_same_v<T, ast::IntersectionType>) {
                std::vector<TypePtr> members;
                members.push_back(
                    node.left ? resolveTypeExpr(*node.left, genericVars) : Type::unknown());
                members.push_back(
                    node.right ? resolveTypeExpr(*node.right, genericVars) : Type::unknown());
                return normalizeIntersection(Type::intersection(std::move(members)));
            } else if constexpr (std::is_same_v<T, ast::RecordType>) {
                std::vector<std::pair<std::string, TypePtr>> fields;
                for (const auto& [name, fieldType] : node.fields) {
                    fields.emplace_back(
                        name,
                        fieldType ? resolveTypeExpr(*fieldType, genericVars) : Type::unknown());
                }
                return Type::record(std::move(fields));
            } else if constexpr (std::is_same_v<T, ast::AtomType>) {
                return Type::atom(node.name);
            } else if constexpr (std::is_same_v<T, ast::GenericVar>) {
                auto it = genericVars.find(node.name);
                if (it != genericVars.end()) {
                    return it->second;
                }
                auto var = boundedPlaceholder(typeExpr, node.name);
                genericVars[node.name] = var;
                return var;
            } else {
                return Type::unknown();
            }
        },
        typeExpr.kind);
    return ownName ? ownType(resolved) : resolved;
}

// Rejects a constructor pattern that destructures the wrong number of values,
// recursively. Without this, `let Just(a, b) = Just(1)` reached the backends:
// the interpreter failed at runtime with "pattern mismatch — expected Just",
// and BEAM emitted Core Erlang that erlc rejected outright. A constructor's
// arity is known statically, so this belongs at compile time.
auto TypeChecker::alwaysReturns(const ast::Expr& expr) -> bool {
    if (std::holds_alternative<ast::ReturnExpr>(expr.kind)) {
        return true;
    }
    // A block exits iff its last expression does.
    if (const auto* block = std::get_if<ast::BlockExpr>(&expr.kind)) {
        return alwaysReturns(block->body);
    }
    return false;
}

auto TypeChecker::alwaysReturns(const std::vector<ast::ExprPtr>& body) -> bool {
    return !body.empty() && body.back() && alwaysReturns(*body.back());
}

auto TypeChecker::adtNameOfType(const TypePtr& type) const -> std::string {
    if (!type) {
        return "";
    }
    // `X?` has its own type kind rather than a NamedType, but the arms it can
    // be matched against are the prelude's `Optional` declaration.
    if (std::holds_alternative<OptionalType>(type->kind)) {
        return m_adtVariants.count("Optional") ? "Optional" : "";
    }
    if (const auto* named = std::get_if<NamedType>(&type->kind)) {
        if (m_adtVariants.count(named->name)) {
            return named->name;
        }
    }
    return "";
}

auto TypeChecker::checkPatternConstructorOwner(const ast::Pattern& pattern, const TypePtr& expected)
    -> void {
    std::string ctorName;
    if (const auto* ctor = std::get_if<ast::ConstructorPattern>(&pattern.kind)) {
        ctorName = ctor->name;
        // `Shades.Light`: the module written must be the one declaring it.
        if (!ctor->module.empty()) {
            const auto module = resolveModulePath(ctor->module, ctor->name);
            const auto owners = m_constructorOwners.find(ctor->name);
            const bool declared =
                m_constructorResult.contains(kex::mangle::qualify(module, ctor->name)) ||
                (owners != m_constructorOwners.end() &&
                 std::any_of(
                     owners->second.begin(), owners->second.end(),
                     [&](const ConstructorOwner& owner) {
                         return owner.module == module || owner.adt.starts_with(module + ".");
                     }));
            if (!declared) {
                error(
                    diag::Code::ModuleHasNoConstructor, pattern.location,
                    {{"module", ctor->module}, {"name", ctor->name}});
                return;
            }
        } else if (const auto declaring = moduleOfUnreachableConstructor(ctor->name);
                   !declaring.empty()) {
            reportUnreachableConstructor(ctor->name, declaring, pattern.location);
            return;
        }
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
        if (owner == m_adtOfConstructor.end() || !expected) {
            return;
        }
        const auto& kind = expected->kind;
        const auto* named = std::get_if<NamedType>(&kind);
        const bool closed =
            std::holds_alternative<PrimitiveType>(kind) ||
            std::holds_alternative<SizedIntType>(kind) ||
            std::holds_alternative<SizedFloatType>(kind) ||
            std::holds_alternative<FuncType>(kind) || std::holds_alternative<TupleType>(kind) ||
            std::holds_alternative<ListType>(kind) || std::holds_alternative<MapType>(kind) ||
            std::holds_alternative<RecordType>(kind) ||
            (named && named->typeArgs.empty() && m_recordFields.count(named->name));
        if (!closed) {
            return;
        }
        error(
            diag::Code::PatternConstructorOfOtherType, pattern.location,
            {{"name", ctorName}, {"owner", owner->second}, {"type", typeToString(expected)}});
        return;
    }

    // The matched ADT declaring the constructor settles it. The bare-name
    // owner table cannot: `WorkQueue.Pending`'s own `Done` shares its
    // spelling with Control.Retry's, which owns that entry.
    if (const auto variants = m_adtVariants.find(adt);
        variants != m_adtVariants.end() &&
        std::find(variants->second.begin(), variants->second.end(), ctorName) !=
            variants->second.end()) {
        // A spelling constructors of several arities share is destructured
        // as the matched ADT's (checkPatternArity leaves it to here).
        if (const auto* ctor = std::get_if<ast::ConstructorPattern>(&pattern.kind);
            ctor && spellingHasSeveralArities(ctorName)) {
            if (const auto owners = m_constructorOwners.find(ctorName);
                owners != m_constructorOwners.end()) {
                for (const auto& owner : owners->second) {
                    if (owner.adt == adt && owner.arity != ctor->args.size()) {
                        error(
                            diag::Code::ConstructorPatternArity, pattern.location,
                            {{"name", ctorName},
                             {"count", std::to_string(owner.arity)},
                             {"given", std::to_string(ctor->args.size())}});
                    }
                }
            }
        }
        return;
    }
    auto owner = m_adtOfConstructor.find(ctorName);
    if (owner == m_adtOfConstructor.end()) {
        // A name no type declares at all can't be one of the matched ADT's
        // variants, whose list is complete — `Some(p)` against an `X?`
        // otherwise compiled and failed at runtime with "no matching
        // clause". A qualified name may resolve elsewhere; leave it be.
        if (ctorName.find('.') != std::string::npos) {
            return;
        }
        const auto& variants = m_adtVariants.at(adt);
        std::string known;
        for (const auto& variant : variants) {
            known += (known.empty() ? "`" : ", `") + variant + "`";
        }
        error(
            diag::Code::PatternNotAConstructorOf, pattern.location,
            {{"name", ctorName}, {"type", adt}, {"constructors", known}});
        return;
    }
    if (owner->second == adt) {
        return;
    }

    error(
        diag::Code::PatternConstructorOfOtherType, pattern.location,
        {{"name", ctorName}, {"owner", owner->second}, {"type", adt}});
}

auto TypeChecker::checkPatternArity(const ast::Pattern& pattern) -> void {
    std::visit(
        [this, &pattern](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
                std::optional<size_t> expected;
                bool isRecord = false;
                if (auto it = m_constructorArity.find(node.name); it != m_constructorArity.end()) {
                    expected = it->second;
                } else if (auto record = m_recordFields.find(node.name);
                           record != m_recordFields.end()) {
                    expected = record->second.size();
                    isRecord = true;
                } else if (m_importedInterfaces) {
                    auto imported = m_importedInterfaces->recordArities.find(node.name);
                    if (imported != m_importedInterfaces->recordArities.end()) {
                        expected = imported->second;
                        isRecord = true;
                    }
                }
                // An unregistered name (imported/opaque) carries no arity to check,
                // and one several constructors of different arities share is
                // checked against the matched ADT's (checkPatternConstructorOwner).
                if (expected && *expected != node.args.size() &&
                    !spellingHasSeveralArities(node.name)) {
                    error(
                        isRecord ? diag::Code::RecordPatternArity
                                 : diag::Code::ConstructorPatternArity,
                        pattern.location,
                        {{"name", node.name},
                         {"count", std::to_string(*expected)},
                         {"given", std::to_string(node.args.size())}});
                }
                for (const auto& arg : node.args) {
                    if (arg) {
                        checkPatternArity(*arg);
                    }
                }
            } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
                if (node.inner) {
                    checkPatternArity(*node.inner);
                }
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
                        m_importedInterfaces->recordArities.count(recordName)) {
                        known = true;
                    }
                    if (!known) {
                        error(
                            diag::Code::NotARecordType, pattern.location,
                            {{"name", node.typeName}});
                    } else if (local != m_recordFields.end()) {
                        for (const auto& field : node.fields) {
                            if (!field.isStringKey && !local->second.count(field.name)) {
                                error(
                                    diag::Code::RecordPatternUnknownField, pattern.location,
                                    {{"record", node.typeName}, {"field", field.name}});
                            }
                        }
                    }
                }
                for (const auto& field : node.fields) {
                    if (field.pattern) {
                        checkPatternArity(**field.pattern);
                    }
                }
            } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
                for (const auto& element : node.elements) {
                    if (element) {
                        checkPatternArity(*element);
                    }
                }
                if (node.rest) {
                    checkPatternArity(**node.rest);
                }
            } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
                for (const auto& element : node.elements) {
                    if (element) {
                        checkPatternArity(*element);
                    }
                }
            }
        },
        pattern.kind);
}

auto TypeChecker::bindPatternVars(const ast::Pattern& pat, TypePtr expected) -> void {
    if (outOfStack(pat.location)) {
        return;
    }
    expected = expected ? resolve(expected) : nullptr;
    checkPatternConstructorOwner(pat, expected);
    std::visit(
        [this, &expected, &pat](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ast::VarPattern>) {
                if (node.name != "_") {
                    auto bound = expected ? expected : freshTypeVar();
                    declareVar(node.name, bound, m_patternBindsVar, pat.location);
                    // Kept so tooling can answer for the binding itself. Types are
                    // resolved once at the end of check(), because a fresh
                    // variable here may only learn what it is from a later use.
                    m_patternBindings.push_back({node.name, pat.location, bound});
                }
            } else if constexpr (std::is_same_v<T, ast::TypePattern>) {
                // The alternative's own type is what the binding has inside the
                // arm — that is the whole point of naming it.
                if (!node.name.empty() && node.name != "_") {
                    std::unordered_map<std::string, TypePtr> noGenerics;
                    auto bound =
                        node.type ? resolveTypeExpr(*node.type, noGenerics) : freshTypeVar();
                    defineVar(node.name, bound);
                    m_patternBindings.push_back({node.name, pat.location, bound});
                }
            } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
                if (node.inner) {
                    bindPatternVars(*node.inner, expected);
                }
            } else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
                // Give each payload binding the type the constructor declares, the
                // way a record pattern below takes field types from the record.
                // Without it a binding had no type until some later use forced one,
                // so `Boxed(b) -> b.size` worked only because `.size` constrained
                // `b`, and the editor could say nothing about `b` itself.
                const ConstructorResult* declaration = nullptr;
                if (auto found = m_constructorResult.find(node.name);
                    found != m_constructorResult.end()) {
                    declaration = &found->second;
                }
                // A constructor declared inside a module is registered as
                // `Module::Name`. Find it the way a call does — through the
                // enclosing modules — and otherwise by the ADT being matched on,
                // which is how an imported one (`using Model`) is reached. Left
                // unfound, the payload stayed untyped, and `entry.fields` on a
                // record held in it resolved to whichever `fields` METHOD existed
                // rather than the record's own field.
                for (auto module = m_currentModulePath; !declaration && !module.empty();) {
                    if (auto found =
                            m_constructorResult.find(kex::mangle::qualify(module, node.name));
                        found != m_constructorResult.end()) {
                        declaration = &found->second;
                        break;
                    }
                    const auto dot = module.rfind('.');
                    if (dot == std::string::npos) {
                        break;
                    }
                    module.resize(dot);
                }
                if (!declaration && expected) {
                    if (const auto* scrutinee = std::get_if<NamedType>(&resolve(expected)->kind)) {
                        const auto suffix = std::string(kex::mangle::kSeparator) + node.name;
                        for (const auto& [key, candidate] : m_constructorResult) {
                            if (key.size() > suffix.size() && key.ends_with(suffix) &&
                                namedTypesMatch(candidate.adtName, scrutinee->name)) {
                                declaration = &candidate;
                                break;
                            }
                        }
                    }
                }
                for (size_t i = 0; i < node.args.size(); ++i) {
                    if (!node.args[i]) {
                        continue;
                    }
                    TypePtr payload;
                    const int slot =
                        declaration && i < declaration->slots.size() ? declaration->slots[i] : -1;
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
                            named && index < named->typeArgs.size()) {
                            payload = named->typeArgs[index];
                        } else if (const auto* optional =
                                       std::get_if<OptionalType>(&scrutinee->kind);
                                   optional && index == 0) {
                            payload = optional->inner;
                        } else if (const auto* list = std::get_if<ListType>(&scrutinee->kind);
                                   list && index == 0) {
                            payload = list->element;
                        }
                    } else if (slot < 0 && declaration && i < declaration->payloadTypes.size()) {
                        // Only a CONCRETE declared payload. A type-parameter
                        // payload's declared type is the placeholder the ADT was
                        // registered with — one variable for the whole program —
                        // so reading it bound every such pattern to whatever some
                        // unrelated use had unified it with: `let Just(i) = ...`
                        // came out `Char` in the merged prelude (kexhq/kex#249).
                        // Re-read now that every declaration is registered: read
                        // at registration, `(Atom, [String])` kept `Atom` and
                        // `String` as unresolved names, which matched no Atom or
                        // String once a value was compared with it.
                        payload = declaredPayload(*declaration, i);
                    }
                    // The built-in carriers come from the prelude interface, not a
                    // local declaration, so they have no slot above. Left untyped,
                    // `Just(item)` bound a fresh variable that unified with anything:
                    // `wantsInteger(item)` on a `String?` passed, and `item.path`
                    // latched onto an imported `path` function instead of the
                    // record's field (#328).
                    if (!payload && expected) {
                        const auto scrutinee = resolve(expected);
                        if (const auto* optional = std::get_if<OptionalType>(&scrutinee->kind);
                            optional && node.name == kex::names::kJustConstructor && i == 0) {
                            payload = optional->inner;
                        } else if (const auto* named = std::get_if<NamedType>(&scrutinee->kind);
                                   named && named->name == kex::names::kResultType &&
                                   named->typeArgs.size() == 2 && i == 0 &&
                                   (node.name == kex::names::kOkConstructor ||
                                    node.name == kex::names::kErrorConstructor)) {
                            payload =
                                named->typeArgs[node.name == kex::names::kOkConstructor ? 0 : 1];
                        }
                    }
                    bindPatternVars(*node.args[i], payload);
                }
            } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
                const std::unordered_map<std::string, TypePtr>* fields = nullptr;
                // A named record pattern (`Foo { x }`) pins the record type itself,
                // so resolve field types from the declared type regardless of what
                // the scrutinee was inferred to be. Fall back to `expected` for the
                // anonymous `{ x }` form.
                if (!node.typeName.empty()) {
                    const auto record = resolveRecordName(node.typeName);
                    m_recordPatterns[&node] = record;
                    if (auto found = m_recordFields.find(record); found != m_recordFields.end()) {
                        fields = &found->second;
                    }
                } else if (expected) {
                    if (auto* named = std::get_if<NamedType>(&expected->kind)) {
                        if (auto found = findRecordFields(*named); found != m_recordFields.end()) {
                            fields = &found->second;
                        }
                    }
                }
                for (const auto& field : node.fields) {
                    TypePtr fieldType;
                    if (fields) {
                        if (auto found = fields->find(field.name); found != fields->end()) {
                            fieldType = found->second;
                        }
                    }
                    if (field.pattern) {
                        bindPatternVars(**field.pattern, fieldType);
                    } else if (!field.isStringKey) {
                        defineVar(field.name, fieldType ? fieldType : freshTypeVar());
                    }
                }
            } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
                TypePtr elementType;
                if (expected) {
                    if (auto* list = std::get_if<ListType>(&expected->kind)) {
                        elementType = list->element;
                    } else if (auto* primitive = std::get_if<PrimitiveType>(&expected->kind);
                               primitive && primitive->kind == PrimitiveType::String) {
                        // A String taken apart into its characters: its elements
                        // are Chars, and the tail (bound to `expected` below) is
                        // the rest of the String.
                        m_stringPatterns.insert(&pat);
                        elementType = Type::charT();
                    }
                }
                for (const auto& elem : node.elements) {
                    if (elem) {
                        bindPatternVars(*elem, elementType);
                    }
                }
                if (node.rest) {
                    bindPatternVars(**node.rest, expected);
                }
            } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
                const TupleType* tuple =
                    expected ? std::get_if<TupleType>(&expected->kind) : nullptr;
                // `let (a, b) = (1, 2, 3)` matches nothing: BEAM raised a pattern
                // mismatch, and the walker bound the first two and dropped the
                // third (kexhq/kex#767).
                if (tuple && tuple->elements.size() != node.elements.size()) {
                    error(
                        diag::Code::TuplePatternArity, pat.location,
                        {{"count", std::to_string(tuple->elements.size())},
                         {"given", std::to_string(node.elements.size())},
                         {"type", typeToString(expected)}});
                }
                // `let (a, b) = 1`, or `{ |(a, b)| a }` over a list of Integers,
                // was accepted and failed only when it ran: the walker with an
                // undefined `a`, BEAM with a badmatch.
                if (expected && (std::holds_alternative<PrimitiveType>(expected->kind) ||
                                 std::holds_alternative<ListType>(expected->kind) ||
                                 std::holds_alternative<MapType>(expected->kind) ||
                                 std::holds_alternative<FuncType>(expected->kind))) {
                    error(
                        diag::Code::TuplePatternNeverMatches, pat.location,
                        {{"type", typeToString(expected)}});
                }
                for (size_t i = 0; i < node.elements.size(); ++i) {
                    if (node.elements[i]) {
                        bindPatternVars(
                            *node.elements[i],
                            tuple && i < tuple->elements.size() ? tuple->elements[i] : nullptr);
                    }
                }
            } else if constexpr (std::is_same_v<T, ast::RangePattern>) {
                // (x..y) in a match clause binds x and y as variables.
                if (node.start) {
                    bindPatternVars(*node.start);
                }
                if (node.end) {
                    bindPatternVars(*node.end);
                }
            } else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
                // A literal of another kind than a primitive scrutinee never
                // matches: `"one" =>` in a match on an Integer was accepted and
                // only the fallback arm ever ran (kexhq/kex#773). Integer and
                // Float literals match each other's values (docs/language-spec.md
                // §23.6), and anything still open is left alone.
                const auto* scrutinee =
                    expected ? std::get_if<PrimitiveType>(&expected->kind) : nullptr;
                std::optional<PrimitiveType::Kind> literal;
                switch (node.literal.type) {
                    case TokenType::String: literal = PrimitiveType::String; break;
                    case TokenType::Char: literal = PrimitiveType::Char; break;
                    case TokenType::Atom: literal = PrimitiveType::Atom; break;
                    case TokenType::True:
                    case TokenType::False: literal = PrimitiveType::Bool; break;
                    case TokenType::Integer:
                    case TokenType::Float: literal = PrimitiveType::Integer; break;
                    default: break;
                }
                const auto shown =
                    node.literal.type == TokenType::String ? "\"" + node.literal.value + "\""
                    : node.literal.type == TokenType::Atom && !node.literal.value.starts_with(":")
                        ? ":" + node.literal.value
                        : node.literal.value;
                const bool scrutineeIsNumber = expected && isNumericType(expected);
                const bool literalIsNumber = literal == PrimitiveType::Integer;
                const bool knownScrutinee =
                    scrutinee ||
                    (expected && std::holds_alternative<SizedFloatType>(expected->kind));
                if (literal && knownScrutinee && literalIsNumber != scrutineeIsNumber) {
                    error(
                        diag::Code::LiteralPatternNeverMatches, pat.location,
                        {{"literal", shown}, {"type", typeToString(expected)}});
                } else if (
                    literal && scrutinee && !literalIsNumber && scrutinee->kind != *literal &&
                    // A Char against a String, as `==` leaves it (checkNeverEqual).
                    !((*literal == PrimitiveType::Char || *literal == PrimitiveType::String) &&
                      (scrutinee->kind == PrimitiveType::Char ||
                       scrutinee->kind == PrimitiveType::String))) {
                    error(
                        diag::Code::LiteralPatternNeverMatches, pat.location,
                        {{"literal", shown}, {"type", typeToString(expected)}});
                }
            }
            // WildcardPattern introduces nothing.
        },
        pat.kind);
}

auto TypeChecker::bindDestructuredPattern(const ast::Pattern& pat, const TypePtr& valueType)
    -> void {
    checkPatternArity(pat);
    // Reject constructor mismatches early, e.g.
    //   let Ok(v) = parsePrefix(...)   when parsePrefix returns Optional
    //   let Just(v) = parse(...)       when parse returns Result
    bool mismatchReported = false;
    if (auto* cp = std::get_if<ast::ConstructorPattern>(&pat.kind)) {
        auto resolved = resolve(valueType);
        if (isUndetermined(resolved))
            ; // permissive — can't determine the type at compile time
        else if (
            cp->name == kex::names::kJustConstructor &&
            !std::holds_alternative<OptionalType>(resolved->kind)) {
            error(
                diag::Code::DestructureConstructorMismatch, pat.location,
                {{"name", "Just"}, {"expected", "Optional"}, {"actual", typeToString(resolved)}});
            mismatchReported = true;
        } else if (
            cp->name == kex::names::kOkConstructor || cp->name == kex::names::kErrorConstructor) {
            if (auto* nt = std::get_if<NamedType>(&resolved->kind)) {
                if (nt->name != kex::names::kResultType) {
                    error(
                        diag::Code::DestructureConstructorMismatch, pat.location,
                        {{"name", cp->name},
                         {"expected", "Result"},
                         {"actual", typeToString(resolved)}});
                    mismatchReported = true;
                }
            } else {
                error(
                    diag::Code::DestructureConstructorMismatch, pat.location,
                    {{"name", cp->name},
                     {"expected", "Result"},
                     {"actual", typeToString(resolved)}});
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
    std::visit(
        [this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                checkModule(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                checkMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                if (node) {
                    checkTraitDef(*node);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                checkMainBlock(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                checkUsingBlock(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                checkRecordDef(*node);
            }
        },
        item);
}

auto TypeChecker::checkModule(const ast::ModuleDef& mod) -> void {
    auto previousModulePath = m_currentModulePath;
    const bool previousInFileModule =
        std::exchange(m_inFileModule, m_inFileModule || mod.fileModule);
    m_currentModulePath =
        previousModulePath.empty() || mod.name.rfind(previousModulePath + ".", 0) == 0
            ? mod.name
            : previousModulePath + "." + mod.name;
    pushScope();
    for (const auto& item : mod.body) {
        std::visit(
            [this](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    checkFunctionDef(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                    checkMakeDef(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                    if (node) {
                        checkTraitDef(*node);
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    checkModule(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                    checkUsingBlock(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                    checkRecordDef(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (!node) {
                        return;
                    }
                    for (const auto& visible : node->items) {
                        std::visit(
                            [this](const auto& member) {
                                using M = std::decay_t<decltype(member)>;
                                if constexpr (std::is_same_v<
                                                  M, std::unique_ptr<ast::FunctionDef>>) {
                                    if (member) {
                                        checkFunctionDef(*member);
                                    }
                                } else if constexpr (std::is_same_v<
                                                         M, std::unique_ptr<ast::MakeDef>>) {
                                    if (member) {
                                        checkMakeDef(*member);
                                    }
                                } else if constexpr (std::is_same_v<
                                                         M, std::unique_ptr<ast::UsingBlock>>) {
                                    if (member) {
                                        checkUsingBlock(*member);
                                    }
                                } else if constexpr (std::is_same_v<
                                                         M, std::unique_ptr<ast::RecordDef>>) {
                                    if (member) {
                                        checkRecordDef(*member);
                                    }
                                }
                            },
                            visible);
                    }
                }
            },
            item);
    }
    popScope();
    m_currentModulePath = std::move(previousModulePath);
    m_inFileModule = previousInFileModule;
}

auto TypeChecker::checkRecordDef(const ast::RecordDef& def) -> void {
    std::unordered_map<std::string, TypePtr> generics;
    for (const auto& name : def.typeParams) {
        generics.emplace(name, freshTypeVar());
    }
    for (const auto& field : def.fields) {
        if (!field.type || !field.defaultValue || !*field.defaultValue) {
            continue;
        }
        auto declared = resolveTypeExpr(*field.type, generics);
        auto actual = resolve(inferExpr(**field.defaultValue));
        if (!std::holds_alternative<UnknownType>(actual->kind) &&
            !std::holds_alternative<TypeVar>(actual->kind) && !argMatchesParam(actual, declared) &&
            !literalCoercesTo(**field.defaultValue, declared)) {
            typeMismatch((*field.defaultValue)->location, declared, actual);
        }
        noteFloatLiteral(**field.defaultValue, declared);
    }
}

auto TypeChecker::checkUsingBlock(const ast::UsingBlock& block) -> void {
    ImportSelection selection;
    selection.module += block.module.dotted();
    selection.onlyNames = block.onlyNames;
    selection.exceptNames = block.exceptNames;
    selection.alias = block.alias;
    selection.location = block.location;
    if (block.body.empty()) {
        if (!m_importScopeStack.empty()) {
            pushImport(std::move(selection));
        }
        return;
    }
    pushScope();
    pushImport(std::move(selection));
    inferBody(block.body);
    popScope();
}

auto TypeChecker::checkFunctionDef(const ast::FunctionDef& def) -> void {
    noteClauseCoverage(def);
    // A method of a `make` for a trait takes its receiver's dictionary.
    if (m_currentMakeType) {
        if (auto trait = declaredTraitOf(m_currentMakeType); !trait.empty()) {
            m_makeTargetTraits[&def] = std::move(trait);
        }
    }
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_functionImports.find(&def); imports != m_functionImports.end()) {
        m_declarationImports = imports->second;
    }
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
        (m_inMakeBlock && m_currentMakeType ? typeToString(m_currentMakeType) + "."
                                            : std::string()) +
        def.name;
    if (auto [purity, inserted] = m_overloadPurity.emplace(purityKey, def.isFoul);
        !inserted && purity->second != def.isFoul) {
        auto location = def.location;
        if (def.isFoul) {
            location.column += 5; // point after `foul `, at the function name
        }
        error(diag::Code::OverloadPurityMismatch, location, {{"name", def.name}});
    }
    // The rule lowering's arity follows (ast::takesReceiverPattern): a literal
    // first parameter (`repeat(0)`) is an argument, not the receiver.
    const bool receiverIsFirstParam =
        m_inMakeBlock && !def.clauses.empty() && ast::takesReceiverPattern(def.clauses[0]);
    // A "declared" signature is one from a standalone TypeAnnotation
    // (`fact : Integer -> Integer`) — tracked in m_annotationDeclared.
    // Pre-registered provisional sigs (for forward-reference/recursion) are
    // in m_userSignatures but NOT in m_annotationDeclared, so they don't
    // affect param-type selection or return-type verification here.
    auto scopedDeclared = m_scopedDeclaredSignatures.find(declarationKey);
    // A function another unit reaches through the module's interface is
    // annotated (decision 1, §14 item 5): KexI carries its signature, not
    // its body. Only a module in a file other than the entry's is one.
    // A function the compiler generated (`__kexTemplate_…`) has no source to
    // annotate.
    if (!def.name.starts_with("__") && !m_inMakeBlock && !m_currentModulePath.empty() &&
        !def.isSlot && !def.clauses.empty() &&
        (def.clauses.front().hasParamList || !def.clauses.front().params.empty()) &&
        scopedDeclared == m_scopedDeclaredSignatures.end() &&
        !m_privateFreeFunctions.contains(declarationKey) &&
        (m_inFileModule ||
         (!m_entryFiles.empty() &&
          std::find(m_entryFiles.begin(), m_entryFiles.end(), std::string(def.location.file)) ==
              m_entryFiles.end()))) {
        std::string what;
        for (const auto& clause : def.clauses) {
            for (const auto& param : clause.params) {
                if (param.name && !param.pattern && !param.type && !param.signatureType) {
                    what = "its parameter `" + *param.name + "`";
                    break;
                }
            }
            if (what.empty() && !clause.returnAnnotation) {
                what = "its result";
            }
            if (!what.empty()) {
                break;
            }
        }
        if (!what.empty()) {
            error(
                diag::Code::ExportedFunctionUnannotated, def.location,
                {{"name", def.name}, {"module", m_currentModulePath}, {"what", what}});
        }
    }
    // `declarationKey` is module-scoped only, not receiver-scoped — a
    // standalone annotation for a free fn (or another make block's method)
    // sharing this name would otherwise claim a make method that has no
    // relation to it at all, e.g. a free `tag : String -> String` leaking
    // onto `make String`'s own `tag` and tripping a spurious arity-mismatch
    // warning downstream (#105). A make-block method's contract comes only
    // from its own receiver-matched signature (below, `makeDeclared`), never
    // from this module-scoped, receiver-blind lookup.
    auto* declared = [&]() -> const Signature* {
        if (m_inMakeBlock) {
            return nullptr;
        }
        if (scopedDeclared == m_scopedDeclaredSignatures.end()) {
            return nullptr;
        }
        // A lone declaration is still the contract even when its arity is
        // wrong; checkFunctionDef then emits the established annotation/body
        // diagnostics instead of silently treating it as unrelated.
        if (scopedDeclared->second.size() == 1) {
            return &scopedDeclared->second.front();
        }
        const auto arity = def.clauses.empty() ? size_t(0) : def.clauses.front().params.size();
        const Signature* match = nullptr;
        for (const auto& signature : scopedDeclared->second) {
            if (signature.params.size() != arity) {
                continue;
            }
            // Several same-arity contracts describe a runtime overload set
            // implemented by one permissive body (for example FS.File.open
            // by mode). No single contract may type that body; preserve all
            // call-site signatures without choosing one arbitrarily.
            if (match) {
                return nullptr;
            }
            match = &signature;
        }
        return match;
    }();
    // A binding written without a parameter list holds the function its
    // annotation describes: `doubled : Step -> Step` over `let doubled =
    // around { ... }` is a value of that type, not a function of a Step. In
    // a module, the arrow was read as the binding's own parameter
    // (kexhq/kex#763).
    // Unless another definition takes what the annotation says:
    // `printLine : Showable -> Void` is `printLine(msg)`'s, beside a bare
    // `foul printLine = ...`.
    const auto definedWithArity = [&](std::size_t arity) {
        for (const auto& key :
             {def.name, m_currentModulePath.empty()
                            ? def.name
                            : kex::mangle::qualify(m_currentModulePath, def.name)}) {
            if (const auto found = m_userSignatures.find(key); found != m_userSignatures.end()) {
                for (const auto& signature : found->second) {
                    if (signature.clause && signature.params.size() == arity) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    std::optional<Signature> valueContract;
    if (declared && !declared->params.empty() && !def.clauses.empty() &&
        !def.clauses.front().hasParamList && def.clauses.front().params.empty() &&
        !definedWithArity(declared->params.size())) {
        valueContract = *declared;
        valueContract->result = Type::func(valueContract->params, valueContract->result);
        valueContract->params.clear();
        valueContract->requiredParams.reset();
        declared = &*valueContract;
    }
    // `(Integer, Integer) -> Integer` is a function of one tuple. Over a
    // definition of two parameters it almost certainly meant two, and applied
    // anyway it typed `a` as the tuple and invented a `B`, so the body and
    // every right call were errors (kexhq/kex#785). Said once, and not
    // applied: the definition is checked, and called, as written.
    if (declared && declared->params.size() == 1 && !def.clauses.empty()) {
        const auto arity = def.clauses.front().params.size();
        const auto* tuple = std::get_if<TupleType>(&resolve(declared->params.front())->kind);
        if (tuple && arity > 1 && tuple->elements.size() == arity) {
            std::string curried;
            for (const auto& element : tuple->elements) {
                curried += typeToString(element) + " -> ";
            }
            curried += typeToString(declared->result);
            error(
                diag::Code::AnnotationTupleForParameters, def.location,
                {{"name", def.name},
                 {"tuple", typeToString(declared->params.front())},
                 {"count", std::to_string(arity)},
                 {"curried", curried}});
            const auto wrong = *declared;
            declared = nullptr;
            const auto isWrong = [&](const Signature& signature) {
                return !signature.clause && signature.params.size() == 1 &&
                       typesEqual(signature.params.front(), wrong.params.front());
            };
            for (const auto& key :
                 {def.name, m_currentModulePath.empty()
                                ? def.name
                                : kex::mangle::qualify(m_currentModulePath, def.name)}) {
                if (const auto found = m_userSignatures.find(key);
                    found != m_userSignatures.end()) {
                    std::erase_if(found->second, isWrong);
                }
            }
            m_scopedDeclaredSignatures.erase(declarationKey);
            scopedDeclared = m_scopedDeclaredSignatures.end();
            m_annotationDeclared.erase(def.name);
        }
    }

    // A `:>` declaration inside `make` is the method's contract just as a
    // standalone annotation is for a top-level function. The registered
    // receiver signature includes `this`; FunctionDef clauses do not, so use
    // a receiver-less copy while checking their parameters and return bodies.
    // Without this, clause-style implementations were inferred from scratch:
    // a generic callback parameter could collapse to Void and poison calls in
    // a later method even though the declared interface returned `[X]`.
    std::optional<Signature> makeDeclared;
    if (!declared && m_inMakeBlock && m_currentMakeType && m_annotatedMethods.count(def.name)) {
        if (auto methods = m_methodSignatures.find(def.name); methods != m_methodSignatures.end()) {
            const auto arity = def.clauses.empty() ? size_t(0) : def.clauses.front().params.size();
            const auto receiverOffset = receiverIsFirstParam ? 0u : 1u;
            for (const auto& signature : methods->second) {
                if (signature.params.size() != arity + receiverOffset ||
                    !argMatchesParam(m_currentMakeType, signature.params.front())) {
                    continue;
                }
                if (makeDeclared) {
                    makeDeclared.reset();
                    break;
                }
                makeDeclared = signature;
                if (!receiverIsFirstParam) {
                    makeDeclared->params.erase(makeDeclared->params.begin());
                }
            }
        }
        if (makeDeclared) {
            declared = &*makeDeclared;
        }
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
            !typesEqual(resolve(declared->result), resolve(inlineReturnType))) {
            error(
                diag::Code::InlineReturnTypeDisagrees, (*def.clauses[0].returnAnnotation)->location,
                {{"name", def.name},
                 {"inline", typeToString(resolve(inlineReturnType))},
                 {"declared", typeToString(resolve(declared->result))}});
        }
    }

    // A member that declares no result answers to the result a trait its
    // type claims requires of it: `foul getChar = "c"` implementing IO's
    // `getChar :> String?` was accepted, and failed at its caller's first
    // `.or` (found by `make fuzz-metamorphic`). A result bounded by a trait
    // binds too: `area :> Float` takes any Float, never a String
    // (kexhq/kex#642).
    // Its parameters, where they are annotated, must take what the
    // requirement passes them.
    // Of same-arity overloads (`take(text: String)` beside `take(count:
    // Integer)`), the one this clause's annotations take answers it: held to
    // the first, the Integer overload was "required to take String"
    // (kexhq/kex#623).
    const Signature* requirement = nullptr;
    std::deque<Signature> ownedRequirements;
    // A clause written with its receiver as a pattern (`let kind(@Kilo(unit))`)
    // answers a requirement with the parameters after it.
    std::size_t requirementOffset = 0;
    if (!declared && m_inMakeBlock && m_currentMakeType && !def.clauses.empty()) {
        const auto& params = def.clauses.front().params;
        requirementOffset =
            !params.empty() && params.front().pattern && *params.front().pattern &&
                    std::holds_alternative<ast::ThisPattern>((*params.front().pattern)->kind)
                ? 1
                : 0;
        auto genericVars = m_currentMakeTargetVars;
        auto answers = [&](const Signature& required) {
            for (size_t pi = requirementOffset; pi < params.size(); ++pi) {
                if (params[pi].type &&
                    !argMatchesParam(
                        resolve(
                            bindThis(required.params[pi - requirementOffset], m_currentMakeType)),
                        resolveTypeExpr(**params[pi].type, genericVars))) {
                    return false;
                }
            }
            return true;
        };
        for (const auto* required : m_traits.requiredOverloads(m_currentMakeType, def.name)) {
            // A requirement over a constructor (`into : F<A> -> (A -> F<B>) ->
            // F<B>`) writes its receiver: at this type, and without it, it
            // types the parameters an implementation writes (`let into(f)`).
            if (const auto applied = constructorRequirementAt(*required, m_currentMakeType)) {
                ownedRequirements.push_back(*applied);
                required = &ownedRequirements.back();
            }
            if (required->params.size() + requirementOffset == params.size() &&
                (!requirement || (!answers(*requirement) && answers(*required)))) {
                requirement = required;
            }
        }
    }
    TypePtr requiredResult;
    // A result the implementation writes is still the requirement's: `let area
    // -> String` where the trait requires `area :> Float` was accepted, the
    // annotation overriding the requirement, and a caller holding a `Shape`
    // got a String (the plan's §11, item 2; #642).
    if (requirement && requirement->result && inlineReturnType) {
        const auto wanted = resolve(bindThis(requirement->result, m_currentMakeType));
        const auto written = resolve(inlineReturnType);
        if (wanted && isConcreteOrBound(wanted) && !typesEqual(wanted, Type::voidType()) &&
            !isUndetermined(written) && !argMatchesParam(written, wanted)) {
            error(
                diag::Code::RequiredDeclaredReturn, (*def.clauses[0].returnAnnotation)->location,
                {{"name", def.name},
                 {"expected", typeToString(wanted)},
                 {"actual", typeToString(written)}});
        }
    }
    if (requirement && !inlineReturnType) {
        // Letters included: `bind(address, block)` implementing `bind : String ->
        // (TCPListener -> A) -> Result<A, NetError>` answers the `A` its block
        // does, the parameters having taken the requirement's letters too.
        if (auto result = bindThis(requirement->result, m_currentMakeType);
            result && !typesEqual(resolve(result), Type::voidType()) &&
            !std::holds_alternative<UnknownType>(resolve(result)->kind) &&
            (isConcreteOrBound(resolve(result)) || containsTypeVariable(resolve(result)))) {
            requiredResult = std::move(result);
        }
    }

    auto returnType = declared           ? declared->result
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
        if (!param.pattern || !*param.pattern) {
            return false;
        }
        const auto& kind = (*param.pattern)->kind;
        if (const auto* record = std::get_if<ast::RecordPattern>(&kind)) {
            return !record->typeName.empty();
        }
        return std::holds_alternative<ast::TypePattern>(kind);
    };
    for (const auto& clause : def.clauses) {
        if (siblingParamContracts.size() < clause.params.size()) {
            siblingParamContracts.resize(clause.params.size());
        }
        auto clauseGenerics = m_currentMakeTargetVars;
        for (size_t pi = 0; pi < clause.params.size(); ++pi) {
            if (!siblingParamContracts[pi] && clause.params[pi].type) {
                siblingParamContracts[pi] =
                    resolveTypeExpr(**clause.params[pi].type, clauseGenerics);
            }
        }
    }
    if (auto provisional = m_userSignatures.find(def.name); provisional != m_userSignatures.end()) {
        for (const auto& signature : provisional->second) {
            if (signature.params.size() != siblingParamContracts.size()) {
                continue;
            }
            for (size_t pi = 0; pi < signature.params.size(); ++pi) {
                if (!siblingParamContracts[pi] &&
                    std::any_of(
                        def.clauses.begin(), def.clauses.end(),
                        [&](const auto& clause) {
                            return pi < clause.params.size() && clause.params[pi].pattern &&
                                   std::holds_alternative<ast::LiteralPattern>(
                                       (*clause.params[pi].pattern)->kind);
                        }) &&
                    !std::holds_alternative<TypeVar>(signature.params[pi]->kind) &&
                    !std::holds_alternative<UnknownType>(signature.params[pi]->kind)) {
                    siblingParamContracts[pi] = signature.params[pi];
                }
            }
        }
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
        for (const auto& signature : scopedDeclared->second) {
            if (signature.params.size() == arity) {
                clauseContracts.push_back(&signature);
            }
        }
        const bool oneArity =
            std::all_of(def.clauses.begin(), def.clauses.end(), [&](const auto& clause) {
                return clause.params.size() == arity;
            });
        if (!oneArity || clauseContracts.size() != def.clauses.size()) {
            clauseContracts.clear();
        }
    }

    std::vector<Signature> signatures;
    for (size_t ci = 0; ci < def.clauses.size(); ci++) {
        const auto& clause = def.clauses[ci];
        const Signature* contract = declared                      ? declared
                                    : ci < clauseContracts.size() ? clauseContracts[ci]
                                                                  : nullptr;
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
            ~ClauseVarMark() {
                slot = previous;
            }
        } clauseVarMark{m_clauseVarMark, m_clauseVarMark};
        m_clauseVarMark = typeVarMark;
        pushScope();
        if (m_inMakeBlock && m_currentMakeType) {
            auto receiver = resolve(m_currentMakeType);
            if (auto* named = std::get_if<NamedType>(&receiver->kind);
                named && (findRecordFields(*named) != m_recordFields.end())) {
                defineVar("new", m_currentMakeType, {.isVar = true, .implicit = true});
            }
        }
        const auto slotResult = resolve(returnType);
        const auto* slotReply = def.isSlot ? std::get_if<NamedType>(&slotResult->kind) : nullptr;
        const bool callSlot = slotReply && slotReply->name == kex::names::kReplyType &&
                              slotReply->typeArgs.size() == 1;
        if (callSlot) {
            defineVar(
                "from", Type::named("From", {slotReply->typeArgs.front()}),
                {.isVar = false, .implicit = true});
        }
        const bool previousCastSlot = m_inCastSlot;
        m_inCastSlot = def.isSlot && !callSlot;
        auto genericVars = m_currentMakeTargetVars;
        std::vector<TypePtr> paramTypes;
        for (size_t pi = 0; pi < clause.params.size(); pi++) {
            const auto& param = clause.params[pi];
            // A bare type name takes that type as a value, whatever says what
            // the parameter's type is (`to : A -> Type<T> -> T?` for
            // `let to(value, String)`).
            if (typeSelectorParameter(param)) {
                m_typeSelectorPatterns.insert(param.pattern->get());
            }
            // Use declared param type if available and the annotation covers
            // this position; fall back to inline annotation or fresh TypeVar.
            TypePtr paramType;
            if (contract && pi < contract->params.size() && !param.type) {
                paramType = contract->params[pi];
            } else if (
                requirement && pi >= requirementOffset &&
                pi - requirementOffset < requirement->params.size() && !param.type &&
                !param.pattern && param.name) {
                // A trait's requirement types what its implementation leaves
                // unwritten: `foul read(path)` implementing FS.File's
                // `read : FilePath -> ...` takes a FilePath, as the guide says
                // ("the trait already types them").
                paramType = resolve(
                    bindThis(requirement->params[pi - requirementOffset], m_currentMakeType));
            } else if (auto literal = literalParameterType(clause, param)) {
                paramType = std::move(literal);
            } else if (
                !param.type && !param.signatureType && !namesItsOwnType(param) &&
                pi < siblingParamContracts.size() && siblingParamContracts[pi]) {
                // A parameter that already says what it takes belongs to that
                // clause, not to a sibling clause's annotation: `Query.from`
                // takes a list in one declaration and a Map in the next, and
                // `respond(JSON { status, body }, route)` a JSON beside a
                // `respond(reply: String, route)`.
                paramType = siblingParamContracts[pi];
            } else if (!param.type && pi > 0 && param.pattern && [&] {
                           // `@Hour` after the receiver (`let *(@Watt, @Hour)`): a value
                           // of the type its constructor builds, TimeUnit's Hour here.
                           const auto* at = std::get_if<ast::ThisPattern>(&(*param.pattern)->kind);
                           const auto* ctor =
                               at && at->inner
                                   ? std::get_if<ast::ConstructorPattern>(&at->inner->kind)
                                   : nullptr;
                           return ctor && m_adtOfConstructor.contains(ctor->name);
                       }()) {
                const auto& at = std::get<ast::ThisPattern>((*param.pattern)->kind);
                paramType = Type::named(
                    m_adtOfConstructor.at(std::get<ast::ConstructorPattern>(at.inner->kind).name));
            } else if (
                !param.type && param.pattern && !(pi == 0 && receiverIsFirstParam) &&
                !constructorPatternType(**param.pattern).empty()) {
                // A parameter matched by a constructor (`area(Circle(r))`) is of
                // the type the constructor builds. Left a fresh variable, the
                // clauses generalized to `A -> Float`, and `xs.map(area)` was
                // typed from nothing (kexhq/kex#753).
                paramType = Type::named(constructorPatternType(**param.pattern));
            } else if (
                !param.type && pi == 0 && receiverIsFirstParam && m_currentMakeType &&
                param.pattern &&
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
                // A separate annotation is what callers are checked against:
                // the definition must take what it lets them pass. `double :
                // Integer -> Integer` over `let double(n: String)` was called
                // with 4 (kexhq/kex#791).
                if (declared && def.clauses.size() == 1 && pi < declared->params.size() &&
                    declared->params.size() == clause.params.size() &&
                    scopedDeclared != m_scopedDeclaredSignatures.end() &&
                    scopedDeclared->second.size() == 1) {
                    const auto said = resolve(declared->params[pi]);
                    const auto written = resolve(paramType);
                    if (isConcreteValue(said) && !containsTypeVariable(said) &&
                        isConcreteValue(written) && !containsTypeVariable(written) &&
                        !argMatchesParam(said, written)) {
                        error(
                            diag::Code::AnnotationParamDisagrees, (*param.type)->location,
                            {{"name", def.name},
                             {"param", param.name.value_or("_")},
                             {"declared", typeToString(said)},
                             {"written", typeToString(written)}});
                    }
                }
                if (requirement && pi >= requirementOffset &&
                    pi - requirementOffset < requirement->params.size()) {
                    if (const auto passed = resolve(bindThis(
                            requirement->params[pi - requirementOffset], m_currentMakeType));
                        passed && isConcreteOrBound(passed) &&
                        !isUndetermined(resolve(paramType)) &&
                        !argMatchesParam(passed, paramType)) {
                        error(
                            diag::Code::RequiredParamType, (*param.type)->location,
                            {{"name", def.name},
                             {"expected", typeToString(passed)},
                             {"param", param.name.value_or("_")},
                             {"actual", typeToString(paramType)}});
                    }
                }
            } else if (auto selector = typeSelectorParameter(param)) {
                paramType = std::move(selector);
            } else {
                paramType = freshTypeVar();
                if (auto* var = std::get_if<TypeVar>(&paramType->kind)) {
                    m_unannotatedParamVars.insert(var->id);
                    // A free function of one clause: what its body calls on this
                    // parameter is asked of every argument passed for it. Several
                    // clauses may each take another shape, and a `make`
                    // function's parameters sit beside an implicit receiver.
                    if (!m_inMakeBlock && def.clauses.size() == 1 && !def.name.empty()) {
                        m_unannotatedParamOwner[var->id] = {
                            def.name + "/" + std::to_string(clause.params.size()), pi};
                    }
                }
            }
            paramTypes.push_back(paramType);
            if (param.name.has_value() && *param.name != "_") {
                defineVar(
                    *param.name, paramType,
                    {false, false, param.location.line, param.location.column});
                if (m_recordsNames && param.location.line > 0) {
                    m_bindingSites.emplace_back(*param.name, param.location);
                    m_patternBindings.push_back({*param.name, param.location, paramType});
                }
            }
            if (param.pattern) {
                bindPatternVars(**param.pattern, paramType);
            }
            // A parameter declared as a trait is passed with its dictionary.
            if (param.type && *param.type &&
                std::holds_alternative<ast::TypeName>((*param.type)->kind)) {
                if (auto trait = declaredTraitOf(paramType); !trait.empty()) {
                    m_parameterTraits[&param] = std::move(trait);
                }
            }
            if (param.defaultValue && *param.defaultValue) {
                auto actual = resolve(inferExpr(**param.defaultValue));
                if (!std::holds_alternative<UnknownType>(actual->kind) &&
                    !std::holds_alternative<TypeVar>(actual->kind) &&
                    !argMatchesParam(actual, paramType) &&
                    !literalCoercesTo(**param.defaultValue, paramType)) {
                    typeMismatch((*param.defaultValue)->location, paramType, actual);
                }
                noteFloatLiteral(**param.defaultValue, paramType);
            }
        }
        if (declared && declared->params.size() != clause.params.size()) {
            bool hasMatchingAnnotation = false;
            auto it = m_annotationArities.find(declarationKey);
            if (it != m_annotationArities.end()) {
                hasMatchingAnnotation = it->second.count(clause.params.size()) > 0;
            }
            // A clause of the declared arity elsewhere makes this one an
            // overload the declaration does not describe (`printLine` beside
            // `printLine(msg)`), not a declaration that disagrees with it.
            if (!hasMatchingAnnotation && !definedWithArity(declared->params.size()) &&
                m_diagnostics) {
                warning(
                    diag::Code::AnnotationArityMismatch, def.location,
                    {{"name", def.name},
                     {"declared", std::to_string(declared->params.size())},
                     {"actual", std::to_string(clause.params.size())}});
            }
        }
        // The `.try`s of this clause's own body (not a block's, not a
        // `trying`'s), for the check against its declared result below.
        std::vector<std::pair<SourceLocation, TypePtr>> clauseTries;
        std::vector<std::pair<const ast::Expr*, TypePtr>> clauseReturns;
        auto* const outerTries = std::exchange(m_clauseTries, &clauseTries);
        auto* const outerReturns = std::exchange(m_clauseReturns, &clauseReturns);
        const auto outerTriesDepth = std::exchange(m_clauseTriesDepth, m_blockDepth);
        // An inline result annotation shares the clause's type parameters:
        // `let wrap(value: A) -> A? = None` relates its result to `value`.
        // The early resolution above uses a separate map for declaration
        // validation; publishing it would turn the result's A into another
        // generic and accept `let n: Integer? = wrap("text")`.
        auto effectiveReturnType = contract ? contract->result
                                   : clause.returnAnnotation
                                       ? resolveTypeExpr(**clause.returnAnnotation, genericVars)
                                   : inlineReturnType ? inlineReturnType
                                                      : requiredResult;
        // The variables this clause's own annotations write stand for every
        // type while its body is checked.
        const auto outerRigid = m_rigidVars;
        if (!contract) {
            for (const auto& [written, variable] : genericVars) {
                if (const auto id = typeVariableId(variable);
                    id && isInferenceVariable(typeVarFromId(*id)) &&
                    !m_currentMakeTargetVars.contains(written)) {
                    m_rigidVars.emplace(*id, written);
                }
            }
        }
        // The declared result is what a `match` or `if` ending the body must
        // produce, as a `let` annotation is for its value: each arm fits it,
        // not the first arm's type. Not a result with a letter any type fills,
        // which every arm would fit; one of this clause's own letters fits only
        // itself (`-> A?` for `then x else None`).
        const bool expectsResult =
            effectiveReturnType && !anyType(effectiveReturnType, [&](const TypePtr& part) {
                const auto id = typeVariableId(resolve(part));
                return id && !m_rigidVars.contains(*id);
            });
        // `A.identity` in the body goes through the clause's own A.
        const auto outerBodyTypeVars = std::exchange(m_bodyTypeVars, genericVars);
        auto bodyType = inferBody(clause.body, expectsResult ? effectiveReturnType : nullptr);
        m_bodyTypeVars = outerBodyTypeVars;
        // A body ending in one of them gives that variable, not the declared
        // result: `-> Integer = a` for `a: A`, `-> A = b` for `b: B`.
        if (const auto produced = typeVariableId(resolve(bodyType));
            produced && m_rigidVars.contains(*produced) && effectiveReturnType) {
            const auto declared = resolve(effectiveReturnType);
            const auto declaredVar = typeVariableId(declared);
            if ((isConcreteValue(declared) && !containsTypeVariable(declared)) ||
                (declaredVar && *declaredVar != *produced && m_rigidVars.contains(*declaredVar))) {
                error(
                    diag::Code::ReturnsRigidTypeVariable, def.location,
                    {{"name", def.name},
                     {"expected", typeToString(effectiveReturnType)},
                     {"variable", m_rigidVars.at(*produced)}});
            }
        }
        m_rigidVars = outerRigid;
        m_clauseTries = outerTries;
        m_clauseReturns = outerReturns;
        m_clauseTriesDepth = outerTriesDepth;
        if (clause.rescue) {
            checkRescue(*clause.rescue);
        }
        m_inCastSlot = previousCastSlot;
        // Verify body matches declared return type (if declared and concrete).
        // Use argMatchesParam (not typesEqual) to apply the same trait-family
        // relaxations that call-site checking uses — e.g. Int and Integer are
        // compatible, so `add : Int -> Int -> Int` with a body returning
        // Integer (inferred from literal arithmetic) isn't a mismatch.
        // Unless the clause rescues what its `.try`s fail with.
        if (effectiveReturnType && !clause.rescue) {
            refuseUnheldTries(def.name, effectiveReturnType, clauseTries);
        }
        // A `return` leaves with its value as the result, so the value must be
        // what the clause declares, as the body's last value must (below).
        // An atom outside the declared set of atoms, as the last value or a
        // `return`'s (kexhq/kex#641).
        if (effectiveReturnType && !clause.body.empty()) {
            std::vector<const ast::Expr*> results{clause.body.back().get()};
            for (const auto& [returned, _] : clauseReturns) {
                if (returned != results.front()) {
                    results.push_back(returned);
                }
            }
            for (const auto* result : results) {
                if (const auto* returned =
                        result ? std::get_if<ast::ReturnExpr>(&result->kind) : nullptr) {
                    result = returned->value.get();
                }
                if (const auto* guarded =
                        result ? std::get_if<ast::TrailingIf>(&result->kind) : nullptr) {
                    result = guarded->expr.get();
                }
                if (result && literalOutsideSet(*result, effectiveReturnType)) {
                    typeMismatch(
                        result->location, effectiveReturnType,
                        Type::atom(std::get<ast::AtomLiteral>(result->kind).name));
                }
            }
        }
        // `Float` and `Number` are trait bounds, not concrete types, so a
        // result declared as one counted as open, and no `return` from such a
        // function was checked at all: `return "kex"` from `-> Float` passed.
        const auto* numberBound =
            effectiveReturnType ? std::get_if<ConstrainedType>(&resolve(effectiveReturnType)->kind)
                                : nullptr;
        const bool primitiveNumberResult =
            numberBound && primitiveNumberTrait(numberBound->traitName);
        if (effectiveReturnType && !def.isSlot &&
            (!containsOpenType(effectiveReturnType) || primitiveNumberResult)) {
            for (const auto& [returned, type] : clauseReturns) {
                const auto* value = std::get<ast::ReturnExpr>(returned->kind).value.get();
                if (const auto* guarded =
                        value ? std::get_if<ast::TrailingIf>(&value->kind) : nullptr) {
                    value = guarded->expr.get();
                }
                const auto actual = resolve(type);
                // An open type can still be judged when no choice for its open
                // parts could make it the declared one: `return Error("bad")` is a
                // `Result<?, String>` whatever its `?`, and never an `Integer`. It
                // went unchecked, and the `Error` came out at the caller as an
                // Integer (kexhq/kex#768).
                const auto declared = resolve(effectiveReturnType);
                const bool plainDeclared = std::holds_alternative<PrimitiveType>(declared->kind) ||
                                           std::holds_alternative<SizedIntType>(declared->kind) ||
                                           std::holds_alternative<SizedFloatType>(declared->kind) ||
                                           primitiveNumberResult;
                const auto* actualNamed = std::get_if<NamedType>(&actual->kind);
                const bool shapedActual =
                    std::holds_alternative<OptionalType>(actual->kind) ||
                    std::holds_alternative<ListType>(actual->kind) ||
                    std::holds_alternative<TupleType>(actual->kind) ||
                    std::holds_alternative<MapType>(actual->kind) ||
                    std::holds_alternative<FuncType>(actual->kind) ||
                    (actualNamed && actualNamed->name == kex::names::kResultType);
                if (value) {
                    noteFloatLiteral(*value, effectiveReturnType);
                }
                const bool judged = !containsOpenType(actual) || (plainDeclared && shapedActual);
                if (returned != clause.body.back().get() && judged &&
                    !argMatchesParam(actual, effectiveReturnType) &&
                    !(value && literalCoercesTo(*value, effectiveReturnType))) {
                    error(
                        diag::Code::ReturnsOtherTypeHere, returned->location,
                        {{"name", def.name},
                         {"expected", typeToString(effectiveReturnType)},
                         {"actual", typeToString(actual)}});
                }
            }
        }
        const auto servingTransitionMatches = [&]() {
            if (!def.isSlot || !effectiveReturnType || clause.body.empty()) {
                return false;
            }
            auto expected = resolve(effectiveReturnType);
            auto* replyType = std::get_if<NamedType>(&expected->kind);
            if (!replyType || replyType->name != kex::names::kReplyType ||
                replyType->typeArgs.size() != 1) {
                return false;
            }
            const ast::Expr* result = clause.body.back().get();
            if (auto* returned = std::get_if<ast::ReturnExpr>(&result->kind)) {
                result = returned->value.get();
            }
            auto* map = result ? std::get_if<ast::MapExpr>(&result->kind) : nullptr;
            if (!map) {
                return false;
            }
            bool hasReply = false;
            bool hasState = false;
            for (const auto& entry : map->entries) {
                if (!entry.key || !entry.value) {
                    continue;
                }
                auto* key = std::get_if<ast::AtomLiteral>(&entry.key->kind);
                if (!key || key->name != "reply") {
                    continue;
                }
                hasReply = true;
                auto actual = resolve(inferExpr(*entry.value));
                if (!containsOpenType(actual) && !containsOpenType(replyType->typeArgs.front()) &&
                    !argMatchesParam(actual, replyType->typeArgs.front())) {
                    typeMismatch(entry.value->location, replyType->typeArgs.front(), actual);
                }
            }
            for (const auto& entry : map->entries) {
                if (!entry.key) {
                    continue;
                }
                auto* key = std::get_if<ast::AtomLiteral>(&entry.key->kind);
                if (key && (key->name == "new" || key->name == "state")) {
                    hasState = true;
                }
            }
            if (hasReply) {
                return true;
            }
            if (!hasState) {
                return false;
            }

            std::function<bool(const ast::Expr&)> mentionsFrom;
            mentionsFrom = [&](const ast::Expr& expression) -> bool {
                return std::visit(
                    [&](const auto& node) -> bool {
                        using E = std::decay_t<decltype(node)>;
                        auto one = [&](const ast::ExprPtr& value) {
                            return value && mentionsFrom(*value);
                        };
                        if constexpr (std::is_same_v<E, ast::Identifier>) {
                            return node.name == "from";
                        } else if constexpr (std::is_same_v<E, ast::MethodCall>) {
                            if (one(node.receiver)) {
                                return true;
                            }
                            for (const auto& arg : node.args) {
                                if (one(arg)) {
                                    return true;
                                }
                            }
                            for (const auto& [_, value] : node.namedArgs) {
                                if (one(value)) {
                                    return true;
                                }
                            }
                            return node.block && one(*node.block);
                        } else if constexpr (std::is_same_v<E, ast::FunctionCall>) {
                            for (const auto& arg : node.args) {
                                if (one(arg)) {
                                    return true;
                                }
                            }
                            for (const auto& [_, value] : node.namedArgs) {
                                if (one(value)) {
                                    return true;
                                }
                            }
                            return node.block && one(*node.block);
                        } else if constexpr (
                            std::is_same_v<E, ast::Lambda> || std::is_same_v<E, ast::BlockExpr>) {
                            for (const auto& item : node.body) {
                                if (one(item)) {
                                    return true;
                                }
                            }
                        } else if constexpr (std::is_same_v<E, ast::ReturnExpr>) {
                            return one(node.value);
                        } else if constexpr (std::is_same_v<E, ast::MapExpr>) {
                            for (const auto& entry : node.entries) {
                                if (one(entry.key) || one(entry.value)) {
                                    return true;
                                }
                            }
                        } else if constexpr (
                            std::is_same_v<E, ast::LetExpr> || std::is_same_v<E, ast::VarExpr> ||
                            std::is_same_v<E, ast::AssignExpr>) {
                            return one(node.value);
                        } else if constexpr (std::is_same_v<E, ast::IfExpr>) {
                            if (one(node.condition)) {
                                return true;
                            }
                            for (const auto& item : node.thenBody) {
                                if (one(item)) {
                                    return true;
                                }
                            }
                            for (const auto& [condition, body] : node.elifs) {
                                if (one(condition)) {
                                    return true;
                                }
                                for (const auto& item : body) {
                                    if (one(item)) {
                                        return true;
                                    }
                                }
                            }
                            if (node.elseBody) {
                                for (const auto& item : *node.elseBody) {
                                    if (one(item)) {
                                        return true;
                                    }
                                }
                            }
                        }
                        return false;
                    },
                    expression.kind);
            };
            const bool deferred =
                std::any_of(clause.body.begin(), clause.body.end(), [&](const ast::ExprPtr& item) {
                    return item && mentionsFrom(*item);
                });
            if (!deferred) {
                error(diag::Code::CallSlotNoReply, def.location);
            }
            return true;
        }();
        const auto servingCastTransitionMatches = [&]() {
            if (!def.isSlot || !effectiveReturnType || clause.body.empty() ||
                !typesEqual(resolve(effectiveReturnType), Type::voidType())) {
                return false;
            }
            if (m_currentMakeType &&
                argMatchesParam(resolve(bodyType), resolve(m_currentMakeType))) {
                return true;
            }
            const ast::Expr* result = clause.body.back().get();
            if (auto* returned = std::get_if<ast::ReturnExpr>(&result->kind)) {
                result = returned->value.get();
            }
            auto* map = result ? std::get_if<ast::MapExpr>(&result->kind) : nullptr;
            if (!map) {
                return false;
            }
            return std::any_of(
                map->entries.begin(), map->entries.end(), [](const ast::MapEntry& entry) {
                    if (!entry.key) {
                        return false;
                    }
                    auto* key = std::get_if<ast::AtomLiteral>(&entry.key->kind);
                    return key &&
                           (key->name == "new" || key->name == "state" || key->name == "stop");
                });
        }();
        if (effectiveReturnType && !std::holds_alternative<TypeVar>(effectiveReturnType->kind) &&
            !std::holds_alternative<UnknownType>(effectiveReturnType->kind) &&
            !deferIfPending(bodyType, effectiveReturnType, def.location) &&
            !std::holds_alternative<UnknownType>(bodyType->kind) &&
            !std::holds_alternative<TypeVar>(bodyType->kind) &&
            !argMatchesParam(bodyType, effectiveReturnType) &&
            // Only a result the function writes: a requirement's is the trait's.
            !(effectiveReturnType != requiredResult &&
              wrapsDeclaredSuccess(clause.body, effectiveReturnType, bodyType)) &&
            !servingTransitionMatches && !servingCastTransitionMatches &&
            // `foul first() -> BookId = 1`: a literal result enters the
            // distinct type it is declared to return.
            ![&] {
                if (clause.body.empty() || !clause.body.back()) {
                    return false;
                }
                const ast::Expr* result = clause.body.back().get();
                if (const auto* returned = std::get_if<ast::ReturnExpr>(&result->kind)) {
                    result = returned->value.get();
                }
                return result && literalCoercesTo(*result, effectiveReturnType);
            }()) {
            const bool required = effectiveReturnType == requiredResult;
            // The result that answers the wrong type, underlined against the
            // return type it breaks, when that result is the one at fault: an
            // early `return` of another type leaves it on the declaration.
            const ast::Expr* result = resultExpressionOf(clause);
            const bool resultAtFault = !required && clause.returnAnnotation && result && [&] {
                const auto typed = m_typeMap.find(result);
                return typed != m_typeMap.end() &&
                       typesEqual(resolve(typed->second), resolve(bodyType));
            }();
            if (resultAtFault && hasNoValue(result)) {
                error(diag::Code::IfWithoutElseHasNoValue, result->location);
                labelLast(
                    "this has no value when the condition is false",
                    (*clause.returnAnnotation)->location, "a value is expected because of this");
            } else {
                error(
                    required ? diag::Code::RequiredReturnTypeMismatch
                             : diag::Code::DeclaredReturnTypeMismatch,
                    resultAtFault ? result->location : def.location,
                    {{"name", def.name},
                     {"expected", typeToString(effectiveReturnType)},
                     {"actual", typeToString(bodyType)}});
                if (resultAtFault) {
                    labelLast(
                        "this has type " + typeToString(bodyType),
                        (*clause.returnAnnotation)->location,
                        "expected because of this return type");
                }
            }
        }
        // `let half -> Float = 1`: the declared result is final, and takes an
        // integer literal as a float (decision 2).
        if (effectiveReturnType) {
            if (const ast::Expr* result = resultExpressionOf(clause)) {
                noteFloatLiteral(*result, effectiveReturnType);
            }
        }
        // A result written as a type variable promises any type the caller
        // asks for: a body that answers one concrete type, which the
        // variable never became, breaks it (`let same(a: A) -> A = 1`,
        // kexhq/kex#644). The variable is still free after the body only
        // where nothing in it tied the variable to a type. Written on the
        // function itself: a trait's contract names its implementer's type.
        const auto declaredResult = effectiveReturnType ? resolve(effectiveReturnType) : nullptr;
        if (!contract && clause.returnAnnotation && declaredResult &&
            isUndetermined(declaredResult) && containsTypeVariable(declaredResult)) {
            const auto produced = resolve(bodyType);
            if (isConcreteValue(produced)) {
                error(
                    diag::Code::DeclaredReturnTypeMismatch, def.location,
                    {{"name", def.name},
                     {"expected", typeToString(effectiveReturnType)},
                     {"actual", typeToString(produced)}});
            }
        }
        // A declared `-> Number` tied to the parameter that is a `Number`
        // (decision 4) promises the caller's own number type: a body that
        // answers one concrete type (`scale : Number -> Number` with
        // `x * 2.5`, a Float) would type `scale(3)` as an Integer that is 7.5.
        if (!contract && declaredResult) {
            const auto* tied = std::get_if<ConstrainedType>(&declaredResult->kind);
            const auto produced = resolve(bodyType);
            if (tied && tied->genericId < 0 && isConcreteValue(produced)) {
                error(
                    diag::Code::DeclaredReturnTypeMismatch, def.location,
                    {{"name", def.name},
                     {"expected", typeToString(effectiveReturnType)},
                     {"actual", typeToString(produced)}});
            }
        }
        // Resolve TypeVars — body inference may have constrained unannotated
        // params via unifyVar (e.g. `n * 2` → n : Number).
        for (auto& pt : paramTypes) {
            pt = resolve(pt);
        }
        popScope();
        // Prefer the declared/annotated return type for annotated functions —
        // using the inferred body type would expose internal TypeVars to call
        // sites, which can be incorrectly constrained by the first call.
        auto concrete = [](const TypePtr& t) { return !std::holds_alternative<TypeVar>(t->kind); };
        // A written letter (`-> A`) over a body whose type is not known is the
        // contract still: Map's `reduce` answers its accumulator's A.
        auto resultType =
            (effectiveReturnType && (concrete(effectiveReturnType) ||
                                     std::holds_alternative<UnknownType>(resolve(bodyType)->kind)))
                ? effectiveReturnType
                : resolve(bodyType);
        if (!effectiveReturnType && !clause.rescue && !clauseTries.empty() &&
            !clause.body.empty()) {
            resultType =
                inferredFailingResult(def.name, clause.body, clauseTries, resultType, true);
        }
        // Trailing parameters with a default are optional at the call site.
        std::size_t required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue) {
            required--;
        }
        Signature checkedSignature{
            def.name, std::move(paramTypes), resultType, def.isFoul, required};
        // Kept so a call site can place NAMED arguments into this method's
        // parameter slots. Only a FunctionDef knows the names; a `:>`
        // annotation does not spell them.
        for (const auto& param : clause.params) {
            checkedSignature.paramNames.push_back(param.name ? *param.name : "");
        }
        checkedSignature.clause = &clause;
        // The pre-pass's result variable for this clause gets the body's
        // answer, before generalizing renames the clause's own variables.
        if (auto pending = m_pendingResults.find(&clause); pending != m_pendingResults.end()) {
            const int id = pending->second;
            m_pendingResults.erase(pending);
            answerPending(id, resultType);
        }
        // And each implementing type's copy of a trait default, `This` its own.
        for (auto [inherited, end] = m_inheritedPendingResults.equal_range(&clause);
             inherited != end; ++inherited) {
            answerPending(inherited->second.second, bindThis(resultType, inherited->second.first));
        }
        m_inheritedPendingResults.erase(&clause);
        // `let id(x) = x` is polymorphic, and every call site must get its own
        // instance of it rather than all of them sharing one variable.
        generalizeSignature(checkedSignature, typeVarMark, bodyType);
        signatures.push_back(std::move(checkedSignature));
    }

    if (!m_inMakeBlock) {
        for (auto& signature : signatures) {
            signature.declaringModule = m_currentModulePath;
        }
    }
    // Preserve the checked declaration interface before the signatures are
    // moved into the unqualified call-resolution table below.
    m_functionSignatures[&def] = signatures;

    // `:>` signatures are registered before bodies are checked, so defaults
    // are not known yet. Copy the checked required arity back now. UFCS counts
    // an implicit receiver as an argument; an explicit @/range receiver
    // pattern already occupies that slot in the clause.
    if (m_inMakeBlock && m_currentMakeType && m_annotatedMethods.count(def.name)) {
        auto methods = m_methodSignatures.find(def.name);
        if (methods != m_methodSignatures.end()) {
            for (auto& method : methods->second) {
                for (const auto& checked : signatures) {
                    const auto receiverOffset = receiverIsFirstParam ? 0u : 1u;
                    if (method.params.size() != checked.params.size() + receiverOffset) {
                        continue;
                    }
                    method.requiredParams =
                        checked.requiredParams.value_or(checked.params.size()) + receiverOffset;
                    // The annotation gave the types, the definition gives the
                    // names — a named argument at a call site needs both.
                    method.paramNames = checked.paramNames;
                }
            }
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
            withReceiver.receiverBounds = m_currentMakeBounds;
            if (!receiverIsFirstParam) {
                withReceiver.params.insert(withReceiver.params.begin(), m_currentMakeType);
            }
            if (!receiverIsFirstParam) {
                withReceiver.requiredParams =
                    withReceiver.requiredParams.value_or(signature.params.size()) + 1;
            }
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
            if (traitDefault) {
                for (auto& other : existing) {
                    if (other.clause != signature.clause || other.params.empty()) {
                        continue;
                    }
                    const auto inheritor = resolve(other.params.front());
                    if (isThisPlaceholder(inheritor)) {
                        continue;
                    }
                    // A default written with its requirement keeps what the
                    // declaration made of it for this type.
                    if (const auto trait = m_traitDefaultClauses.find(signature.clause);
                        trait != m_traitDefaultClauses.end() &&
                        m_traitDefaultDeclarations.contains({trait->second, def.name})) {
                        continue;
                    }
                    auto bound = withReceiver;
                    for (auto& param : bound.params) {
                        param = bindThis(param, inheritor);
                    }
                    bound.result = bindThis(bound.result, inheritor);
                    // What the clause writes is its contract, which this copy
                    // read with the type's claim (`-> T?` is `Char?` for
                    // `implement: Foldable<Char>`): only what it leaves
                    // unwritten comes from the body.
                    const auto& written = *signature.clause;
                    const std::size_t offset = receiverIsFirstParam ? 0 : 1;
                    if (written.returnAnnotation && other.result) {
                        bound.result = other.result;
                    }
                    if (bound.params.size() == other.params.size()) {
                        for (std::size_t i = 0; i < written.params.size(); ++i) {
                            if (written.params[i].type && i + offset < bound.params.size()) {
                                bound.params[i + offset] = other.params[i + offset];
                            }
                        }
                    }
                    bound.makeModule = other.makeModule;
                    bound.receiverBounds = other.receiverBounds;
                    other = std::move(bound);
                }
            }
            // Clauses of one default are one method: rebound, `repeat(0)` and
            // `repeat(n: Integer)` both take `(Word, Integer)`, which kept
            // apart read as an ambiguous overload. The first stays, as the
            // first registration does for a `make` method's own clauses.
            if (traitDefault) {
                for (auto later = existing.begin(); later != existing.end();) {
                    later =
                        std::any_of(
                            existing.begin(), later,
                            [&](const Signature& earlier) { return sameMethod(earlier, *later); })
                            ? existing.erase(later)
                            : std::next(later);
                }
            }
            const auto placeholder =
                std::find_if(existing.begin(), existing.end(), [&](const Signature& other) {
                    return other.clause == signature.clause;
                });
            if (signature.clause && !traitDefault && placeholder != existing.end()) {
                *placeholder = std::move(withReceiver);
                continue;
            }
            const bool duplicate =
                std::any_of(existing.begin(), existing.end(), [&](const Signature& other) {
                    return sameMethod(other, withReceiver);
                });
            if (!duplicate) {
                existing.push_back(std::move(withReceiver));
            }
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
                auto placeholder =
                    std::find_if(sigs.begin(), sigs.end(), [&](const Signature& candidate) {
                        // Another module's same-named function is not this
                        // one's placeholder, however alike their parameters.
                        if (candidate.declaringModule != m_currentModulePath) {
                            return false;
                        }
                        if (candidate.params.size() != declared->params.size()) {
                            return false;
                        }
                        for (size_t i = 0; i < candidate.params.size(); ++i) {
                            if (!typesEqual(candidate.params[i], declared->params[i])) {
                                return false;
                            }
                        }
                        return true;
                    });
                auto checkedInterface = signatures[0];
                const bool hasInlineParamContract =
                    !def.clauses.empty() &&
                    std::any_of(
                        def.clauses[0].params.begin(), def.clauses[0].params.end(),
                        [](const auto& param) { return param.type && *param.type; });
                if (hasInlineParamContract) {
                    // The standalone annotation is the public relationship
                    // (notably A -> A for IO.inspect), while an inline trait
                    // annotation supplies the implementation's dictionary
                    // constraint. Do not erase the declared relationship with
                    // the narrower body-checking parameter type.
                    checkedInterface = *declared;
                    checkedInterface.requiredParams = signatures[0].requiredParams;
                    checkedInterface.isFoul = signatures[0].isFoul;
                    checkedInterface.paramNames = signatures[0].paramNames;
                }
                publishable.push_back(checkedInterface);
                if (placeholder != sigs.end()) {
                    *placeholder = checkedInterface;
                } else {
                    sigs.push_back(std::move(checkedInterface));
                }
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
                const auto arity =
                    def.clauses.empty() ? size_t(0) : def.clauses.front().params.size();
                size_t matching = 0;
                if (scopedDeclared != m_scopedDeclaredSignatures.end()) {
                    for (const auto& contract : scopedDeclared->second) {
                        if (contract.params.size() == arity && ++matching > 1) {
                            break;
                        }
                    }
                }
                if (matching > 1) {
                    for (auto& sig : signatures) {
                        m_userSignatures[def.name].push_back(sig);
                    }
                    if (!m_currentModulePath.empty()) {
                        const auto key = kex::mangle::qualify(m_currentModulePath, def.name);
                        if (m_qualifiedPublished.contains(key)) {
                            for (const auto& sig : signatures) {
                                m_userSignatures[key].push_back(sig);
                            }
                        }
                    }
                }
            }
        } else {
            publishable = signatures;
            if (m_checkedFunctions.count(def.name)) {
                // Additional `let f(...)` with the same name: append to the
                // overload set rather than replacing (typed overloads).
                for (auto& sig : signatures) {
                    m_userSignatures[def.name].push_back(std::move(sig));
                }
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
// as the call name `M.lines` — but its signatures were only ever filed under
// the bare `lines`, so every qualified call came back Unknown. A declared
// result type then bought nothing: `let n: Integer = M.lines("x")` passed with
// `lines : String -> [String]?` in plain sight.
//
// It is not a lookup fallback to the bare name: two modules may each define
// `lines`, and only this module's belongs under this key.
auto TypeChecker::publishQualifiedSignatures(
    const std::string& name, const std::vector<Signature>& signatures) -> void {
    if (m_currentModulePath.empty() || signatures.empty()) {
        return;
    }
    const auto key = kex::mangle::qualify(m_currentModulePath, name);
    // Only THIS definition's signatures: `Date.parse` and `Time.parse` share
    // the bare name `parse`, and publishing the accumulated set gave Date's
    // qualified key Time's result type.
    if (m_qualifiedPublished.insert(key).second) {
        // The first checked definition replaces the declared contracts of its
        // own arities only. The others stay until their definitions publish:
        // `frob(a, b)` checked first wiped `frob : Integer -> String` and
        // `frob : String -> String`, and every one-argument call was refused
        // (kexhq/kex#619).
        auto& existing = m_userSignatures[key];
        std::erase_if(existing, [&](const Signature& declared) {
            return std::any_of(signatures.begin(), signatures.end(), [&](const Signature& checked) {
                return checked.params.size() == declared.params.size();
            });
        });
        existing.insert(existing.end(), signatures.begin(), signatures.end());
    } else {
        for (const auto& signature : signatures) {
            m_userSignatures[key].push_back(signature);
        }
    }
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
// (spec/module_scoped_make.kex).
// Is the code being checked inside `module`, or inside one nested within it?
// An empty owner is the top level, which everything in the file shares.
auto TypeChecker::withinModule(const std::string& module) const -> bool {
    if (module.empty()) {
        return true;
    }
    return m_currentModulePath == module || m_currentModulePath.rfind(module + ".", 0) == 0;
}

auto TypeChecker::privateElsewhere(const std::string& name) const -> bool {
    const auto suffix = "\n" + name;
    return std::any_of(
        m_privateFreeFunctions.begin(), m_privateFreeFunctions.end(), [&](const std::string& key) {
            return key.size() > suffix.size() && key.ends_with(suffix) &&
                   !withinModule(key.substr(0, key.size() - suffix.size()));
        });
}

auto TypeChecker::makeModuleVisible(const std::string& module) const -> bool {
    if (module.empty()) {
        return true;
    }
    // Inside the defining module, or one nested within it.
    if (m_currentModulePath == module || m_currentModulePath.rfind(module + ".", 0) == 0) {
        return true;
    }
    // `using A` also brings `A.B`'s members into scope, matching how a
    // qualified member of a nested module resolves. The lexical
    // `using M do ... end` form counts as the declaration's own does.
    const auto names = [&](const ImportSelection& import) {
        return import.module == module || import.module.rfind(module + ".", 0) == 0 ||
               module.rfind(import.module + ".", 0) == 0;
    };
    for (const auto& frame : m_importScopeStack) {
        if (std::any_of(frame.begin(), frame.end(), names)) {
            return true;
        }
    }
    return std::any_of(m_declarationImports.begin(), m_declarationImports.end(), names);
}

auto TypeChecker::hiddenMakeTaking(const std::string& name, const std::vector<TypePtr>& argTypes)
    -> std::string {
    const auto methods = m_methodSignatures.find(name);
    if (methods == m_methodSignatures.end() || argTypes.empty()) {
        return {};
    }
    const auto receiver = resolve(argTypes.front());
    std::string hidden;
    for (const auto& method : methods->second) {
        if (method.makeModule.empty() || makeModuleVisible(method.makeModule) ||
            method.params.empty()) {
            continue;
        }
        const auto required = method.requiredParams.value_or(method.params.size());
        if (argTypes.size() < required || argTypes.size() > method.params.size()) {
            continue;
        }
        // A make for its module's own type travels with that type's values.
        const auto declared = resolve(method.params.front());
        if (const auto* named = std::get_if<NamedType>(&declared->kind);
            named && named->name.starts_with(method.makeModule + ".")) {
            continue;
        }
        if (!argMatchesParam(receiver, declared)) {
            continue;
        }
        if (hidden.empty() || method.makeModule < hidden) {
            hidden = method.makeModule;
        }
    }
    return hidden;
}

auto TypeChecker::freeFunctionVisible(const std::string& module, const std::string& name) const
    -> bool {
    if (withinModule(module)) {
        return true;
    }
    if (m_privateFreeFunctions.contains(module + "\n" + name)) {
        return false;
    }
    const auto admits = [&](const ImportSelection& import) {
        if (import.module != module) {
            return false;
        }
        // `as:` binds the module to a name and brings none of its names in.
        if (!resolve_core::usingImportsNames(import.alias, import.onlyNames)) {
            return false;
        }
        if (!import.onlyNames.empty() &&
            std::find(import.onlyNames.begin(), import.onlyNames.end(), name) ==
                import.onlyNames.end()) {
            return false;
        }
        return std::find(import.exceptNames.begin(), import.exceptNames.end(), name) ==
               import.exceptNames.end();
    };
    for (const auto& frame : m_importScopeStack) {
        for (const auto& import : frame) {
            if (admits(import)) {
                return true;
            }
        }
    }
    return std::any_of(m_declarationImports.begin(), m_declarationImports.end(), admits);
}

auto TypeChecker::rejectUndeclaredMakeTarget(const ast::MakeDef& def) -> void {
    const auto written = kex::writtenMakeTarget(def.target, kex::StructuralTarget::Excluded);
    if (!written || written->parts.empty()) {
        return; // `make [T]`, `make {K: V}`, a union: shapes, not names
    }
    const auto dotted = written->dotted();
    const auto& last = written->parts.back();
    if (builtinType(last) || isPrimitiveTypeName(last) || m_declaredBeforeMakes.count(dotted) ||
        m_declaredBeforeMakes.count(last) || m_capabilities.count(dotted) ||
        m_localModules.count(dotted) || m_importedModulePaths.count(dotted) ||
        m_traits.lookup(dotted)) {
        return;
    }
    // Resolved, the target names a type by its identity: a record or sum type
    // of a module (`make Tcp.Conn` in `Wire` is `Wire.Tcp.Conn`), an alias,
    // or an imported one.
    const auto resolved = m_currentMakeType ? resolve(m_currentMakeType) : nullptr;
    const auto* named = resolved ? std::get_if<NamedType>(&resolved->kind) : nullptr;
    if (!named) {
        return; // a primitive, a list, a function: never a typo of a name
    }
    const auto& identity = named->name;
    if (m_recordFields.count(identity) || m_adtVariants.count(identity) ||
        m_moduleTypeIdentities.count(identity) || m_declaredAliases.count(identity) ||
        m_traits.lookup(identity) ||
        (m_importedInterfaces && (m_importedInterfaces->typeNames.count(identity) ||
                                  m_importedInterfaces->typeNames.count(last)))) {
        return;
    }
    error(diag::Code::UnknownType, def.target->location, {{"name", dotted}});
}

// A `make` giving a type a function the prelude already gives it — same name,
// arguments the two cannot tell apart — defines it twice, which the prelude
// itself may not do (receiver_conflicts). The backends disagreed on which
// one runs: the walker ran the prelude's `abs` and BEAM the program's own
// (kexhq/kex#637). An overload on another argument type (`<(other:
// Integer)` beside the prelude's `<` on two Measures) is a different function.
auto TypeChecker::rejectRedefinedImports(const ast::MakeDef& def) -> void {
    if (!m_currentMakeType || !m_importedInterfaces) {
        return;
    }
    const auto head = typeToString(headShapeOf(resolve(m_currentMakeType)));
    // Whether some value could be passed to both.
    const auto overlaps = [&](const TypePtr& left, const TypePtr& right) {
        const auto a = resolve(left);
        const auto b = resolve(right);
        return !isConcreteValue(a) || !isConcreteValue(b) || argMatchesParam(a, b) ||
               argMatchesParam(b, a);
    };
    // Whether any of `types`, type arguments included, is one this program
    // declares: a record, a sum type or a distinct type of its own.
    const auto mentionsProgramType = [&](const std::vector<TypePtr>& types) {
        bool found = false;
        const auto visit = [&](const auto& self, const TypePtr& type) -> void {
            const auto resolved = resolve(type);
            if (found || !resolved) {
                return;
            }
            if (const auto* named = std::get_if<NamedType>(&resolved->kind)) {
                const auto declared = m_adtDeclarations.find(named->name);
                found = m_programRecords.contains(named->name) ||
                        m_programDistinctTypes.contains(named->name) ||
                        (declared != m_adtDeclarations.end() &&
                         !kex::isStandardLibrarySource(declared->second.file));
            }
            mapTypeChildren(resolved, [&](const TypePtr& child) {
                self(self, child);
                return child;
            });
        };
        for (const auto& type : types) {
            visit(visit, type);
        }
        return found;
    };
    const auto check = [&](const ast::FunctionDef& function) {
        if (function.clauses.empty()) {
            return;
        }
        // The standard library's own: there the declaration IS the import.
        if (kex::isStandardLibrarySource(function.location.file)) {
            return;
        }
        // The receiver, written as an `@`-pattern first or left implicit.
        const auto& params = function.clauses.front().params;
        const bool receiverWritten =
            !params.empty() && params.front().pattern &&
            std::holds_alternative<ast::ThisPattern>((*params.front().pattern)->kind);
        const auto arity = params.size() + (receiverWritten ? 0 : 1);
        // This function as the checker registered it, when it has.
        const Signature* own = nullptr;
        if (const auto registered = m_methodSignatures.find(function.name);
            registered != m_methodSignatures.end()) {
            for (const auto& signature : registered->second) {
                if (signature.params.size() == arity &&
                    signature.makeModule == m_currentModulePath &&
                    typeToString(headShapeOf(resolve(signature.params.front()))) == head) {
                    own = &signature;
                }
            }
        }
        for (const auto* imported : importedCandidates(function.name)) {
            const auto& signature = imported->signature;
            if (signature.params.empty()) {
                continue;
            }
            const auto least = signature.requiredParams.value_or(signature.params.size());
            if (arity < least || arity > signature.params.size()) {
                continue;
            }
            if (typeToString(headShapeOf(resolve(signature.params.front()))) != head) {
                continue;
            }
            // The receiver with its arguments, as every other parameter: a `/`
            // on `Measure<Currency>` beside the prelude's on a Duration
            // (`Measure<Quantity.Time>`) is a second overload, not the same
            // function twice. Comparing only the head `Measure` stopped a
            // program from declaring its own dimension's products
            // (docs/typechecker-hm-plan.md §8.1, fix 1).
            bool indistinct = overlaps(m_currentMakeType, signature.params.front());
            for (std::size_t i = 1; own && indistinct && i < arity; ++i) {
                indistinct = overlaps(own->params[i], signature.params[i]);
            }
            if (!indistinct) {
                // Coherence (§8.1, fix 2): an overload of another module's
                // name belongs to a module that declares one of its operand
                // types, so two libraries cannot give one product two
                // answers. The orphan rule, for overloads.
                if (own && !mentionsProgramType(own->params)) {
                    error(
                        diag::Code::OrphanOverload, function.location,
                        {{"name", function.name},
                         {"receiver", typeToString(resolve(m_currentMakeType))}});
                    return;
                }
                continue;
            }
            if (imported->backendModule != names::kPreludeModule) {
                continue;
            }
            error(
                diag::Code::RedefinesPreludeMethod, function.location,
                {{"name", function.name}, {"receiver", head}});
            return;
        }
    };
    for (const auto& item : def.body) {
        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item); fn && *fn) {
            check(**fn);
        } else if (const auto* block = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   block && *block) {
            for (const auto& inner : (*block)->items) {
                if (const auto* innerFn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner);
                    innerFn && *innerFn) {
                    check(**innerFn);
                }
            }
        }
    }
}

auto TypeChecker::checkMakeDef(const ast::MakeDef& def) -> void {
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_makeImports.find(&def); imports != m_makeImports.end()) {
        m_declarationImports = imports->second;
    }
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
    auto prevMakeBounds = std::exchange(m_currentMakeBounds, makeReceiverBounds(def));
    auto prevMakeHoles = std::exchange(m_currentMakeHoles, ast::makeTypeLambdaHoles(def));
    if (!m_currentMakeBounds.empty()) {
        m_makeConformances[&def].bounds = m_currentMakeBounds;
    }
    if (def.target) {
        m_currentMakeType = resolveTypeExpr(*def.target, m_currentMakeTargetVars);
        m_makeTargetIdentities[&def] = makeOwnerNames(def.target, resolve(m_currentMakeType));
        rejectUndeclaredMakeTarget(def);
    }
    rejectRedefinedImports(def);
    for (const auto& item : def.body) {
        std::visit(
            [this](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    checkFunctionDef(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    // A `private do ... end` block is checked as the rest are:
                    // visibility is not a type-system concept, and every make
                    // method is registered before any body is checked.
                    if (node) {
                        for (const auto& inner : node->items) {
                            if (const auto* fn =
                                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner)) {
                                if (*fn) {
                                    checkFunctionDef(**fn);
                                }
                            }
                        }
                    }
                }
            },
            item);
    }
    m_inMakeBlock = wasInMakeBlock;
    m_currentMakeType = prevMakeType;
    m_currentMakeModule = std::move(prevMakeModule);
    m_currentMakeTargetVars = std::move(prevMakeTargetVars);
    m_currentMakeHoles = std::move(prevMakeHoles);
    m_currentMakeBounds = std::move(prevMakeBounds);
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
    auto prevMakeBounds = std::exchange(m_currentMakeBounds, {});
    const auto* outerTrait = std::exchange(
        m_currentTrait, m_traits.get(kex::traitIdentity(m_currentModulePath, def.name)));
    for (const auto& item : def.body) {
        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item); fn && *fn) {
            checkFunctionDef(**fn);
        }
    }
    m_currentTrait = outerTrait;
    m_inMakeBlock = wasInMakeBlock;
    m_currentMakeType = std::move(prevMakeType);
    m_currentMakeBounds = std::move(prevMakeBounds);
    m_currentMakeModule = std::move(prevMakeModule);
    popScope();
}

auto TypeChecker::checkTraitImplementation(const ast::MakeDef& def) -> void {
    m_owedFunctions.erase(&def);
    if (def.implements.empty()) {
        return;
    }

    // Extract the type name from the make target.
    std::string typeName;
    if (const auto name = writtenMakeTarget(def.target, StructuralTarget::Excluded);
        name && name->parts.size() == 1) {
        typeName = name->parts[0];
    }

    // Collect method names and their foul status from this make block, and
    // how many arguments each clause takes: at least its parameters without a
    // default, at most all of them, a receiver pattern (`@Watt`) not counted.
    std::unordered_map<std::string, bool> provided; // name -> isFoul
    struct Takes {
        std::size_t least = 0;
        std::size_t most = 0;
    };
    std::unordered_map<std::string, std::vector<Takes>> takes;
    for (const auto& item : def.body) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (node) {
                        provided[node->name] = node->isFoul;
                        for (const auto& clause : node->clauses) {
                            const auto& params = clause.params;
                            const bool receiverPattern = !params.empty() && !params[0].name &&
                                                         params[0].pattern && *params[0].pattern &&
                                                         std::holds_alternative<ast::ThisPattern>(
                                                             (*params[0].pattern)->kind);
                            Takes clauseTakes;
                            for (std::size_t i = receiverPattern ? 1 : 0; i < params.size(); ++i) {
                                ++clauseTakes.most;
                                if (!params[i].defaultValue) {
                                    ++clauseTakes.least;
                                }
                            }
                            takes[node->name].push_back(clauseTakes);
                        }
                    }
                }
            },
            item);
    }

    for (const auto& written : def.implements) {
        // A qualified trait or capability (`implement: Harmat.Connection`)
        // is a module reference like any other: when it is the only one a
        // file makes, discovery must still load the module declaring it, or
        // checking that file alone — as the language server does — reports
        // a trait the whole program knows perfectly well.
        if (written.find('.') != std::string::npos) {
            recordWithOwner(m_referencedModules, written);
        }
        std::vector<std::string> candidates;
        const auto traitName =
            resolveTraitReference(written, m_currentModulePath, nullptr, true, &candidates);
        const TraitDef* trait = m_traits.get(traitName);
        if (!trait) {
            if (candidates.size() > 1) {
                std::string listed;
                for (const auto& candidate : candidates) {
                    listed += (listed.empty() ? "`" : ", `") + candidate + "`";
                }
                error(
                    diag::Code::AmbiguousTrait, def.location,
                    {{"name", written}, {"candidates", listed}});
            } else {
                error(diag::Code::UnknownTrait, def.location, {{"name", written}});
            }
            // Unresolved stays empty so the recorded list aligns with
            // `def.implements` by index; the lowerer skips it as a miss.
            m_resolvedTraits[&def].push_back({});
            continue;
        }
        m_resolvedTraits[&def].push_back(traitName);
        const auto shownType = typeName.empty() ? std::string{"?"} : typeName;
        const auto definedElsewhere = [&](const std::string& method) {
            if (!def.target) {
                return false;
            }
            std::unordered_map<std::string, TypePtr> vars;
            const bool reporting = m_reportUnknownTypes;
            m_reportUnknownTypes = false;
            const auto key = makeTargetKey(resolveTypeExpr(*def.target, vars));
            m_reportUnknownTypes = reporting;
            const auto own = m_ownMethodsByTarget.find(key);
            if (own == m_ownMethodsByTarget.end()) {
                return false;
            }
            const auto prefix = method + "/";
            const auto found = own->second.lower_bound(prefix);
            return found != own->second.end() && found->starts_with(prefix);
        };
        std::set<std::pair<std::string, std::size_t>> reportedArities;
        for (const auto& req : trait->requiredMethods) {
            auto it = provided.find(req.name);
            if (it == provided.end()) {
                // Another block of the type may define it: `make Result<_, E>,
                // implement: Functor do end` only says which argument varies,
                // and the `map` of `make Result<X, E>` is the one it means.
                if (definedElsewhere(req.name)) {
                    continue;
                }
                // A requirement the trait gives a body (`into : ...` beside `let
                // into(f) = ...`) is a default.
                if (std::find(trait->defaultBodies.begin(), trait->defaultBodies.end(), req.name) !=
                    trait->defaultBodies.end()) {
                    continue;
                }
                // The receiver is never among a requirement's parameters, but
                // under `trait Functor<F<_>>` it is written first (`F<A>`), and
                // `describe : () -> String` takes nothing.
                std::vector<TypePtr> owedParams = req.params;
                if (!owedParams.empty() && owedParams[0] &&
                    std::holds_alternative<TypeApp>(resolve(owedParams[0])->kind)) {
                    owedParams.erase(owedParams.begin());
                }
                if (owedParams.size() == 1) {
                    const auto only = owedParams[0] ? resolve(owedParams[0]) : Type::unknown();
                    const auto* tuple = std::get_if<TupleType>(&only->kind);
                    if ((tuple && tuple->elements.empty()) || typesEqual(only, Type::voidType())) {
                        owedParams.clear();
                    }
                }
                OwedFunction owed{req.name, written, req.isFoul, {}, {}};
                for (const auto& param : owedParams) {
                    owed.params.push_back(param ? typeToString(resolve(param)) : "");
                }
                if (req.result && !isUndetermined(resolve(req.result))) {
                    owed.result = typeToString(resolve(req.result));
                }
                m_owedFunctions[&def].push_back(std::move(owed));
                error(
                    diag::Code::MissingRequiredMethod, def.location,
                    {{"type", shownType}, {"trait", written}, {"method", req.name}});
            } else if (req.isFoul && !it->second) {
                error(
                    diag::Code::RequiredMethodNotFoul, def.location,
                    {{"type", shownType}, {"trait", written}, {"method", req.name}});
            } else if (const auto clauses = takes.find(req.name);
                       clauses != takes.end() &&
                       !(req.params.empty() &&
                         (!req.result || isUndetermined(resolve(req.result))))) {
                // A stand-in taking fewer or more arguments than the requirement
                // passed the name check, and failed at run time (#793): `send`
                // required with `to` and `body`, stood in for by `send(to)`.
                // The receiver is never among a requirement's parameters (a
                // `This` there is the other operand: `combine :> This ->
                // This`), and `describe : () -> String` takes nothing.
                std::size_t wanted = req.params.size();
                // Under `trait Functor<F<_>>` the receiver is written: a first
                // parameter `F<A>` is the value the function is called on.
                if (wanted > 0 && req.params[0] &&
                    std::holds_alternative<TypeApp>(resolve(req.params[0])->kind)) {
                    --wanted;
                }
                if (wanted == 1) {
                    const auto only = req.params[0] ? resolve(req.params[0]) : Type::unknown();
                    const auto* tuple = std::get_if<TupleType>(&only->kind);
                    if ((tuple && tuple->elements.empty()) || typesEqual(only, Type::voidType())) {
                        wanted = 0;
                    }
                }
                const bool fits =
                    std::any_of(clauses->second.begin(), clauses->second.end(), [&](Takes t) {
                        return t.least <= wanted && wanted <= t.most;
                    });
                // One report per arity: `open` is required in four modes at
                // three arguments, and one missing block form is one error.
                if (!fits && reportedArities.insert({req.name, wanted}).second) {
                    error(
                        diag::Code::RequiredArity, def.location,
                        {{"type", shownType},
                         {"trait", written},
                         {"method", req.name},
                         {"expected", std::to_string(wanted)},
                         {"actual", std::to_string(clauses->second.front().most)}});
                }
            }
        }
        // Register so satisfies() / argMatchesParam can verify trait usage.
        for (const auto& key : conformanceKeys(def, typeName)) {
            registerConformance(def, key, traitName);
        }
        // What an interface exports for it: the keys, and the bounds.
        auto& exported = m_makeConformances[&def];
        exported.keys = conformanceKeys(def, typeName);
        exported.bounds = makeReceiverBounds(def);
    }
}

auto TypeChecker::checkMainBlock(const ast::MainBlock& block) -> void {
    const bool wasInSyntheticMain = m_inSyntheticMain;
    m_inSyntheticMain = block.synthetic;
    struct Restore {
        bool& flag;
        bool saved;
        ~Restore() {
            flag = saved;
        }
    } restore{m_inSyntheticMain, wasInSyntheticMain};
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_mainImports.find(&block); imports != m_mainImports.end()) {
        m_declarationImports = imports->second;
    }
    if (!block.synthetic) {
        pushScope();
    }
    for (size_t i = 0; i < block.params.size(); i++) {
        const auto& param = block.params[i];
        if (param.name.has_value() && *param.name != "_") {
            TypePtr type = (i == 0)   ? Type::list(Type::string())
                           : (i == 1) ? Type::map(Type::string(), Type::string())
                                      : freshTypeVar();
            defineVar(
                *param.name, std::move(type),
                {false, false, param.location.line, param.location.column});
            if (m_recordsNames && param.location.line > 0) {
                m_bindingSites.emplace_back(*param.name, param.location);
            }
        }
        if (param.pattern) {
            bindPatternVars(**param.pattern);
        }
    }
    inferBody(block.body);
    if (block.rescue) {
        checkRescue(*block.rescue);
    }
    if (!block.synthetic) {
        popScope();
    }
    m_declarationImports = std::move(savedDeclarationImports);
}

auto TypeChecker::inferBody(const std::vector<ast::ExprPtr>& body, const TypePtr& expected)
    -> TypePtr {
    TypePtr lastType = Type::voidType();
    for (const auto& expr : body) {
        if (!expr) {
            continue;
        }
        // Only the last expression's arms answer to the expectation.
        const bool last =
            expected && &expr == &body.back() &&
            (std::holds_alternative<ast::MatchExpr>(expr->kind) ||
             std::holds_alternative<ast::IfExpr>(expr->kind) ||
             std::holds_alternative<ast::ThenElseExpr>(expr->kind) ||
             std::holds_alternative<ast::ListExpr>(expr->kind) || writtenFunction(*expr));
        const auto outer = last ? std::exchange(m_branchExpectation, expected) : nullptr;
        lastType = inferExpr(*expr);
        if (last) {
            m_branchExpectation = outer;
        }
    }
    return lastType;
}

auto TypeChecker::importedCandidateSignatures(const std::string& name) const
    -> std::vector<Signature> {
    std::vector<Signature> result;
    for (const auto* function : importedCandidates(name)) {
        result.push_back(function->signature);
    }
    return result;
}

auto TypeChecker::importedCandidates(const std::string& name, const TypePtr& receiver) const
    -> std::vector<const ImportedFunction*> {
    std::vector<const ImportedFunction*> result;
    if (!m_importedInterfaces) {
        return result;
    }
    if (kex::mangle::hasScope(name)) {
        auto module = m_importedInterfaces->modules.find(kex::mangle::scopeOf(name));
        if (module != m_importedInterfaces->modules.end()) {
            if (auto functions = module->second.exports.find(kex::mangle::memberOf(name));
                functions != module->second.exports.end()) {
                for (const auto& function : functions->second) {
                    result.push_back(&function);
                }
            }
        }
        return result;
    }
    if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
        functions != m_importedInterfaces->receiverFunctions.end()) {
        for (const auto& function : functions->second) {
            if (importedFunctionVisible(function, /*throughReceiver=*/true, receiver)) {
                result.push_back(&function);
            }
        }
    }
    for (const auto& [_, module] : m_importedInterfaces->modules) {
        if (!module.automaticImport) {
            continue;
        }
        if (auto functions = module.exports.find(name); functions != module.exports.end()) {
            for (const auto& function : functions->second) {
                result.push_back(&function);
            }
        }
    }
    return result;
}

auto TypeChecker::usingImportedMethods(const std::string& name, bool asMethod) const
    -> std::vector<const ImportedFunction*> {
    std::vector<const ImportedFunction*> result;
    if (!m_importedInterfaces) {
        return result;
    }
    for (const auto& [moduleName, module] : m_importedInterfaces->modules) {
        // A module of this program keeps its own signatures, and the
        // prelude's are reached through its receiver functions already.
        if (!module.compiledApart || m_localModules.contains(moduleName) ||
            !moduleMemberImported(moduleName, name)) {
            continue;
        }
        if (auto functions = module.exports.find(name); functions != module.exports.end()) {
            for (const auto& function : functions->second) {
                if (!asMethod || !function.signature.params.empty()) {
                    result.push_back(&function);
                }
            }
        }
    }
    return result;
}

auto TypeChecker::importedFunctionVisible(
    const ImportedFunction& function, bool throughReceiver, const TypePtr& receiver) const -> bool {
    if (function.sourceModule.empty() || function.visibleByReceiver) {
        return true;
    }
    if (m_importedInterfaces) {
        if (auto module = m_importedInterfaces->modules.find(function.sourceModule);
            module != m_importedInterfaces->modules.end() && module->second.automaticImport) {
            return true;
        }
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
        if (auto* named = std::get_if<NamedType>(&function.signature.params.front()->kind)) {
            // A qualified receiver carries access to its own companion's
            // methods, even when obtained without a using declaration.
            if (throughReceiver && named->name == function.sourceModule) {
                return true;
            }
            // A value of a type its module declares (not foreignTarget) reaches
            // that type's `make` functions, as checked: the receiver's type
            // is the one the function was declared for (`URI.URI`).
            if (throughReceiver && receiver && function.isReceiverMethod &&
                !function.foreignTarget) {
                if (const auto* actual = std::get_if<NamedType>(&resolve(receiver)->kind);
                    actual && actual->name == named->name) {
                    return true;
                }
            }
            const auto dot = named->name.rfind('.');
            const auto bare = dot == std::string::npos ? named->name : named->name.substr(dot + 1);
            if (moduleMemberImported(function.sourceModule, bare)) {
                return true;
            }
        }
    }
    return moduleMemberImported(function.sourceModule, function.sourceName);
}

auto TypeChecker::moduleMemberImported(
    const std::string& module, const std::string& member, bool bare) const -> bool {
    // A constructor comes in with its type: `using M, only: [Claim]` brings
    // `Claim`'s constructors in too, and `except: [Claim]` leaves them out.
    std::string owningType;
    if (const auto constructors = m_moduleConstructors.find(module);
        constructors != m_moduleConstructors.end()) {
        if (const auto constructor = constructors->second.find(member);
            constructor != constructors->second.end()) {
            const auto& type = constructor->second.typeName;
            owningType = type.substr(type.rfind('.') + 1);
        }
    }
    const auto lists = [&](const std::vector<std::string>& names, const std::string& name) {
        return std::find(names.begin(), names.end(), name) != names.end() ||
               (name == member && !owningType.empty() &&
                std::find(names.begin(), names.end(), owningType) != names.end());
    };
    auto selected = [&](const ImportSelection& import) {
        if (bare && !resolve_core::usingImportsNames(import.alias, import.onlyNames)) {
            return false;
        }
        const bool prefixMatch =
            module == import.module || module.rfind(import.module + ".", 0) == 0;
        // A module nested in the one brought in is one of its names
        // (`Path` under `using FS`), reached by it: its members are not
        // bare names (`separator`), which the checker and BEAM took and
        // the walker refused (`make fuzz-lsp`).
        if (bare && prefixMatch && module != import.module) {
            return false;
        }
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
                return dot == std::string::npos ? std::string() : qualified.substr(0, dot);
            };
            const auto importParent = parentOf(import.module);
            if (importParent.empty() || importParent != parentOf(module)) {
                return false;
            }
            // This sibling rule is for type companions, not unrelated
            // modules such as Units.SI and Units.Data.
            if (!m_importedInterfaces || !m_importedInterfaces->typeNames.contains(import.module) ||
                !m_importedInterfaces->typeNames.contains(module)) {
                return false;
            }
        }
        auto selectedMember = member;
        if (prefixMatch && module.size() > import.module.size()) {
            const auto rest = module.substr(import.module.size() + 1);
            const auto dot = rest.find('.');
            selectedMember = rest.substr(0, dot);
        }
        if (!import.onlyNames.empty() && !lists(import.onlyNames, selectedMember)) {
            return false;
        }
        return !lists(import.exceptNames, selectedMember);
    };
    if (std::any_of(m_declarationImports.begin(), m_declarationImports.end(), selected)) {
        return true;
    }
    for (const auto& scope : m_importScopeStack) {
        if (std::any_of(scope.begin(), scope.end(), selected)) {
            return true;
        }
    }
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

auto TypeChecker::defineVar(const std::string& name, TypePtr type, TypeEnv::Binding binding)
    -> void {
    auto& env = m_scopeStack.empty() ? m_globals : m_scopeStack.back();
    env.set(name, std::move(type), binding);
}

auto TypeChecker::declareVar(
    const std::string& name,
    TypePtr type,
    bool isVar,
    SourceLocation where,
    SourceLocation named,
    std::optional<ConstantNumber> constant) -> void {
    if (named.line == 0) {
        named = where;
    }
    const auto& env = m_scopeStack.empty() ? m_globals : m_scopeStack.back();
    if (const auto* existing = env.binding(name); existing && !existing->implicit) {
        const auto first = *existing;
        error(
            !first.isVar ? diag::Code::RedeclareImmutable
            : isVar      ? diag::Code::RedeclareVarAgain
                         : diag::Code::RedeclareVarAsLet,
            where, {{"name", name}});
        defineVar(name, std::move(type), first);
        return;
    }
    defineVar(name, std::move(type), {isVar, false, named.line, named.column, constant});
    if (m_recordsNames) {
        m_bindingSites.emplace_back(name, named);
    }
}

auto TypeChecker::lookupVar(const std::string& name) const -> TypePtr {
    for (auto it = m_scopeStack.rbegin(); it != m_scopeStack.rend(); ++it) {
        if (auto type = it->get(name)) {
            return type;
        }
    }
    return m_globals.get(name);
}

auto TypeChecker::bindingOf(const std::string& name) const -> const TypeEnv::Binding* {
    for (auto it = m_scopeStack.rbegin(); it != m_scopeStack.rend(); ++it) {
        if (const auto* binding = it->binding(name)) {
            return binding;
        }
    }
    return m_globals.binding(name);
}

// The declared result of a zero-parameter binding named `name` — what a bare
// reference to it evaluates to, since naming one auto-calls it. Only a single,
// fully concrete signature answers: a generic result (`A`, a type variable)
// would need instantiating per use, which the call path does and this doesn't.
auto TypeChecker::zeroArgBindingResult(const std::string& name) -> TypePtr {
    const std::vector<Signature>* signatures = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(m_currentModulePath + "\n" + name);
        scoped != m_scopedDeclaredSignatures.end()) {
        signatures = &scoped->second;
    } else if (auto user = m_userSignatures.find(name); user != m_userSignatures.end()) {
        signatures = &user->second;
    }
    if (!signatures || signatures->size() != 1) {
        return nullptr;
    }
    const auto& signature = signatures->front();
    if (!signature.params.empty() || !signature.result) {
        return nullptr;
    }
    auto result = resolve(signature.result);
    std::function<bool(const TypePtr&)> mentionsTypeParameter = [&](const TypePtr& type) -> bool {
        if (!type) {
            return true;
        }
        const auto resolved = resolve(type);
        if (const auto* named = std::get_if<NamedType>(&resolved->kind)) {
            if (named->name.size() == 1 && kex::startsUppercase(named->name)) {
                return true;
            }
            return std::any_of(
                named->typeArgs.begin(), named->typeArgs.end(), mentionsTypeParameter);
        }
        if (const auto* list = std::get_if<ListType>(&resolved->kind)) {
            return mentionsTypeParameter(list->element);
        }
        if (const auto* optional = std::get_if<OptionalType>(&resolved->kind)) {
            return mentionsTypeParameter(optional->inner);
        }
        if (const auto* map = std::get_if<MapType>(&resolved->kind)) {
            return mentionsTypeParameter(map->key) || mentionsTypeParameter(map->value);
        }
        if (const auto* tuple = std::get_if<TupleType>(&resolved->kind)) {
            return std::any_of(
                tuple->elements.begin(), tuple->elements.end(), mentionsTypeParameter);
        }
        return false;
    };
    if (containsOpenType(result) || mentionsTypeParameter(result)) {
        return nullptr;
    }
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
    if (it == m_userSignatures.end() || it->second.empty()) {
        return nullptr;
    }
    const auto& signature = it->second.front();
    if (signature.params.empty() || !signature.result) {
        return nullptr;
    }
    std::vector<TypePtr> params;
    for (const auto& param : signature.params) {
        params.push_back(resolve(param));
    }
    auto result = resolve(signature.result);
    // The clauses of one function (`area(Circle(r))` beside `area(Square(s))`)
    // take the same parameters: one function of them, answering what every
    // clause does (kexhq/kex#753). Overloads by type stay ambiguous.
    for (auto other = std::next(it->second.begin()); other != it->second.end(); ++other) {
        if (!other->result || other->params.size() != params.size()) {
            return nullptr;
        }
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (!typesEqual(resolve(other->params[i]), params[i])) {
                return nullptr;
            }
        }
        result = joinBranchType(result, resolve(other->result));
        if (!result) {
            return nullptr;
        }
    }
    if (containsOpenType(result) || std::any_of(params.begin(), params.end(), [](const TypePtr& p) {
            return containsOpenType(p);
        })) {
        return nullptr;
    }
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
        if (message[i] != 'T' || (i > 0 && word(message[i - 1]))) {
            return 0;
        }
        std::size_t end = i + 1;
        while (end < message.size() && std::isdigit(static_cast<unsigned char>(message[end]))) {
            ++end;
        }
        if (end == i + 1 || (end < message.size() && word(message[end]))) {
            return 0;
        }
        return end - i;
    };
    std::set<char> taken;
    bool any = false;
    for (std::size_t i = 0; i < message.size(); ++i) {
        if (variableAt(i)) {
            any = true;
        }
        if (std::isupper(static_cast<unsigned char>(message[i])) &&
            (i == 0 || !word(message[i - 1])) &&
            (i + 1 == message.size() || !word(message[i + 1]))) {
            taken.insert(message[i]);
        }
    }
    if (!any) {
        return message;
    }
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
            while (next <= 'Z' && taken.count(next)) {
                ++next;
            }
            // Out of letters: the number is still a name.
            std::string letter = next <= 'Z' ? std::string(1, next++) : variable;
            name = names.emplace(variable, letter).first;
        }
        renamed += name->second;
        i += length;
    }
    return renamed;
}

// The inference variables in a diagnostic's values named across all of
// them, the way nameInferenceVariables names them across one message: the
// same variable gets the same letter in every value, two different ones never
// share a letter. The values are named as one text, joined by a separator no
// type ever prints.
auto nameInferenceVariablesIn(diag::Args& args) -> void {
    constexpr char separator = '\x1f';
    std::string joined;
    for (const auto& [_, value] : args) {
        joined += value;
        joined += separator;
    }
    const auto named = nameInferenceVariables(joined);
    std::size_t start = 0;
    for (auto& [_, value] : args) {
        const auto end = named.find(separator, start);
        value = named.substr(start, end - start);
        start = end + 1;
    }
}
} // namespace

auto TypeChecker::error(diag::Code code, SourceLocation loc, diag::Args args) -> void {
    if (m_diagnostics) {
        nameInferenceVariablesIn(args);
        m_diagnostics->push_back(
            makeDiagnostic(Diagnostic::Level::Error, code, loc, std::move(args)));
    }
}

auto TypeChecker::warning(diag::Code code, SourceLocation loc, diag::Args args) -> void {
    if (m_diagnostics) {
        nameInferenceVariablesIn(args);
        m_diagnostics->push_back(
            makeDiagnostic(Diagnostic::Level::Warning, code, loc, std::move(args)));
    }
}

auto TypeChecker::labelLast(std::string primary, SourceLocation other, std::string otherLabel)
    -> void {
    if (!m_diagnostics || m_diagnostics->empty()) {
        return;
    }
    auto& last = m_diagnostics->back();
    last.label = std::move(primary);
    // Only a place in the diagnostic's own file: a declaration in the standard
    // library or another unit would print that file, which the reader did not
    // write and a spec's expected output cannot name.
    if (other.line > 0 && other.file == last.location.file) {
        last.labels.push_back({other, std::move(otherLabel)});
    }
}

auto TypeChecker::resultExpressionOf(const ast::FunctionClause& clause) -> const ast::Expr* {
    if (clause.body.empty() || !clause.body.back()) {
        return nullptr;
    }
    const ast::Expr* result = clause.body.back().get();
    if (const auto* returned = std::get_if<ast::ReturnExpr>(&result->kind)) {
        result = returned->value.get();
    }
    return result;
}

auto TypeChecker::hasNoValue(const ast::Expr* expr) -> bool {
    if (!expr) {
        return false;
    }
    if (const auto* conditional = std::get_if<ast::IfExpr>(&expr->kind)) {
        return !conditional->elseBody;
    }
    return std::holds_alternative<ast::TrailingIf>(expr->kind);
}

auto TypeChecker::typeMismatch(SourceLocation loc, const TypePtr& expected, const TypePtr& actual)
    -> void {
    if (const auto trait = distinctTraitClash(actual, expected); !trait.empty()) {
        error(
            diag::Code::DistinctAsTraitValue, loc,
            {{"type", typeToString(innermostDistinct(actual))},
             {"trait", trait},
             {"backing", typeToString(distinctBacking(innermostDistinct(actual)))}});
        return;
    }
    diag::Args args = {{"expected", typeToString(expected)}, {"actual", typeToString(actual)}};
    if (const auto* never = std::get_if<NeverType>(&expected->kind);
        never && !never->leftRecord.empty()) {
        args.emplace_back("left", never->leftRecord);
        args.emplace_back("right", never->rightRecord);
        error(diag::Code::TypeMismatchIncompatibleRecords, loc, std::move(args));
        return;
    }
    error(diag::Code::TypeMismatch, loc, std::move(args));
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::constantType(const std::string& name) const -> TypePtr {
    for (auto scope = m_currentModulePath;;) {
        if (const auto found = m_userSignatures.find(kex::mangle::qualify(scope, name));
            found != m_userSignatures.end()) {
            for (const auto& signature : found->second) {
                if (signature.params.empty()) {
                    return signature.result;
                }
            }
        }
        if (scope.empty()) {
            break;
        }
        const auto dot = scope.rfind('.');
        scope.resize(dot == std::string::npos ? 0 : dot);
    }
    // One a `using` imports from a unit compiled apart (`STATUSES`).
    if (m_importedInterfaces) {
        for (const auto& [_, module] : m_importedInterfaces->modules) {
            if (const auto exports = module.exports.find(name); exports != module.exports.end()) {
                for (const auto& function : exports->second) {
                    if (!function.isConstructor && function.signature.params.empty() &&
                        importedFunctionVisible(function)) {
                        return function.signature.result;
                    }
                }
            }
        }
    }
    return nullptr;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::constructorArityInScope(const std::string& name) -> std::optional<size_t> {
    for (const auto& [module, constructors] : m_moduleConstructors) {
        if (const auto constructor = constructors.find(name);
            constructor != constructors.end() &&
            (withinModule(module) || moduleMemberImported(module, name))) {
            return constructor->second.arity;
        }
    }
    if (m_adtOfConstructor.contains(name)) {
        if (const auto arity = m_constructorArity.find(name); arity != m_constructorArity.end()) {
            return arity->second;
        }
    }
    return std::nullopt;
}

auto TypeChecker::nullaryConstructorType(const std::string& name) -> TypePtr {
    if (auto constructed = constructorResultType(name, {});
        constructed && m_nullaryConstructors.contains(name)) {
        return constructed;
    }
    // A nullary constructor's value is that variant of the ADT declaring it
    // (NamedType::variant), found through the declaration the name means here:
    // one this program declares at top level, else one its module or a `using`
    // brings in, else a top-level one of an imported unit (the prelude's
    // `CallError.Timeout` gives way to `using Net`'s `Timeout`).
    const auto topLevel = [&]() -> TypePtr {
        if (!m_nullaryConstructors.contains(name)) {
            return nullptr;
        }
        const auto adt = m_adtOfConstructor.find(name);
        return adt == m_adtOfConstructor.end() ? nullptr : Type::variantOf(adt->second, name);
    };
    if (const auto declared = m_constructorResult.find(name);
        declared != m_constructorResult.end() && declared->second.declaredHere) {
        if (auto type = topLevel()) {
            return type;
        }
    }
    for (const auto& [module, constructors] : m_moduleConstructors) {
        auto constructor = constructors.find(name);
        if (constructor == constructors.end() || constructor->second.arity != 0 ||
            constructor->second.typeName.empty()) {
            continue;
        }
        // Inside its own module the constructor is in scope as written: `Name`
        // in `module App.Models.User` is its `UserDetail.Name`, not a
        // same-named record elsewhere (`Net.DNS.Name`).
        if (withinModule(module) || moduleMemberImported(module, name)) {
            return Type::variantOf(constructor->second.typeName, name);
        }
    }
    if (auto type = topLevel()) {
        return type;
    }
    if (m_importedInterfaces) {
        for (const auto& [_, module] : m_importedInterfaces->modules) {
            if (auto exports = module.exports.find(name); exports != module.exports.end()) {
                for (const auto& function : exports->second) {
                    if (function.isConstructor && function.signature.params.empty() &&
                        importedFunctionVisible(function)) {
                        return variantType(function.signature.result, name);
                    }
                }
            }
        }
    }
    // A spelling two ADTs share: the one in scope.
    if (const auto* owner = scopedConstructorOwner(name);
        owner && owner->arity == 0 && !owner->generic) {
        return Type::variantOf(owner->adt, name);
    }
    return nullptr;
}

// The variant `name` of the ADT a constructor's declared result is, or that
// result as declared when it is not a plain ADT.
auto TypeChecker::variantType(const TypePtr& result, const std::string& name) const -> TypePtr {
    const auto* adt = std::get_if<NamedType>(&resolve(result)->kind);
    return adt && adt->typeArgs.empty() && adt->variant.empty() ? Type::variantOf(adt->name, name)
                                                                : result;
}

} // namespace kex::semantic

namespace kex::semantic {

// Inside a `make` block, `name` with `argTypes` as a function of the block's
// own type called on `this` (`decorate(@name)`, or `seed` named bare): one
// such function takes `this` and those arguments. Decided as the type's own;
// null when none does.
auto TypeChecker::ownFunctionOnThis(
    const ast::Expr& expr, const std::string& name, const std::vector<TypePtr>& argTypes)
    -> TypePtr {
    if (!m_inMakeBlock || !m_currentMakeType) {
        return nullptr;
    }
    // In a trait's default, a requirement named bare is called on `this`, as
    // `this.label` is: `let shout = label.upperCase`. It was found only when
    // some type already implemented the trait; otherwise it was reported as an
    // undefined name, whatever else the default got wrong (the plan's §11,
    // item 9). Typed by the requirement; lowering emits the default per
    // claiming type, as before.
    if (m_currentTrait && isThisPlaceholder(resolve(m_currentMakeType))) {
        for (const auto& required : m_currentTrait->requiredMethods) {
            if (required.name == name && required.params.size() == argTypes.size()) {
                // Lowered as `name(this)` in each claiming type's copy of the
                // default: a bare `label` was lowered as a variable on BEAM
                // ("unbound variable 'Label'").
                DecidedCall own{CallKind::OwnMethod, typeToString(m_currentMakeType)};
                own.onThis = true;
                m_decidedCalls[&expr] = std::move(own);
                return required.result ? required.result : Type::unknown();
            }
        }
    }
    const auto functions = m_methodSignatures.find(name);
    if (functions == m_methodSignatures.end()) {
        return nullptr;
    }
    const auto* wanted = std::get_if<NamedType>(&resolve(m_currentMakeType)->kind);
    const bool takesThis = std::any_of(
        functions->second.begin(), functions->second.end(), [&](const Signature& function) {
            if (function.params.size() != argTypes.size() + 1) {
                return false;
            }
            const auto* own = std::get_if<NamedType>(&resolve(function.params.front())->kind);
            if (wanted && own) {
                return ownTypeIdentity(own->name) == ownTypeIdentity(wanted->name);
            }
            return typesEqual(resolve(function.params.front()), resolve(m_currentMakeType));
        });
    if (!takesThis) {
        return nullptr;
    }
    std::vector<TypePtr> withThis{m_currentMakeType};
    withThis.insert(withThis.end(), argTypes.begin(), argTypes.end());
    auto result = checkCall(name, withThis, expr.location, true, nullptr, &expr);
    // Whatever the call itself decided, this is what it is.
    DecidedCall own{CallKind::OwnMethod, typeToString(m_currentMakeType)};
    own.onThis = true;
    m_decidedCalls[&expr] = std::move(own);
    return result;
}

// Inside a `make` block, whether `name` is one of the functions the block's
// own type defines (any block for it), all of which are known before any
// body is checked (noteOwnMethods).
auto TypeChecker::ownFunctionOfMake(const std::string& name) const -> bool {
    if (!m_inMakeBlock || !m_currentMakeType) {
        return false;
    }
    const auto own = m_ownMethodsByTarget.find(makeTargetKey(m_currentMakeType));
    if (own == m_ownMethodsByTarget.end()) {
        return false;
    }
    const auto prefix = name + "/";
    const auto at = own->second.lower_bound(prefix);
    return at != own->second.end() && at->starts_with(prefix);
}

// A bare call that only `make` methods answer is a method call, as both
// backends run it (lowering's FunctionCall cascade): inside a make block,
// written one argument short of the block's own method, `this` is the
// receiver (`decorate(@name)`, kexhq/kex#281); otherwise the first argument
// is (`shout(tag)`, UFCS). A binding of the name, or a function at the
// written arity, still answers it instead. Null when neither applies.
auto TypeChecker::bareMethodCall(
    const ast::Expr& expr, const ast::FunctionCall& call, const std::vector<TypePtr>& argTypes)
    -> TypePtr {
    const auto& name = call.name;
    if (kex::mangle::hasScope(name) || lookupVar(name)) {
        return nullptr;
    }
    const auto methods = m_methodSignatures.find(name);
    if (methods == m_methodSignatures.end()) {
        return nullptr;
    }
    const auto suffix = std::string(kex::mangle::kSeparator) + name;
    for (const auto& [key, signatures] : m_userSignatures) {
        if (key == name || key.ends_with(suffix)) {
            for (const auto& signature : signatures) {
                if (signature.params.size() == argTypes.size()) {
                    return nullptr;
                }
            }
        }
    }
    // A method of `arity` parameters, receiver included; of `receiver`'s
    // type when one is given.
    const auto methodAt = [&](std::size_t arity, const TypePtr& receiver) {
        const auto* wanted = receiver ? std::get_if<NamedType>(&resolve(receiver)->kind) : nullptr;
        return std::any_of(
            methods->second.begin(), methods->second.end(), [&](const Signature& method) {
                if (method.params.size() != arity) {
                    return false;
                }
                if (!receiver) {
                    return true;
                }
                const auto* own = std::get_if<NamedType>(&resolve(method.params.front())->kind);
                if (wanted && own) {
                    return ownTypeIdentity(own->name) == ownTypeIdentity(wanted->name);
                }
                return typesEqual(resolve(method.params.front()), resolve(receiver));
            });
    };
    if (auto own = ownFunctionOnThis(expr, name, argTypes)) {
        return own;
    }
    // One of the block's own functions, called with arguments no form of it
    // takes: not on `this` (`countdown()` for `countdown(k)`), nor through a
    // first argument it could be called on. Every function of a block is
    // registered before any body is checked, so the count is provably wrong.
    if (ownFunctionOfMake(name)) {
        const bool throughFirst =
            !argTypes.empty() &&
            (methodAt(argTypes.size(), argTypes.front()) ||
             (isUndetermined(resolve(argTypes.front())) && methodAt(argTypes.size(), nullptr)));
        if (!throughFirst) {
            std::set<std::size_t> arities;
            for (const auto& method : methods->second) {
                if (!method.params.empty() && methodAt(method.params.size(), m_currentMakeType)) {
                    arities.insert(method.params.size() - 1);
                }
            }
            std::string expected;
            for (const auto arity : arities) {
                expected += (expected.empty() ? "" : ", ") + std::to_string(arity);
            }
            error(
                arities.size() > 1 ? diag::Code::MethodArityChoice : diag::Code::MethodArity,
                expr.location,
                {{"name", name}, {"count", expected}, {"given", std::to_string(argTypes.size())}});
            return Type::unknown();
        }
    }
    if (argTypes.empty() || !methodAt(argTypes.size(), nullptr)) {
        return nullptr;
    }
    auto result = checkCall(name, argTypes, expr.location, true, nullptr, &expr);
    if (!m_decidedCalls.contains(&expr)) {
        const auto receiver = resolve(argTypes.front());
        if (const auto own = ownTypeAnswers(argTypes, name)) {
            m_decidedCalls[&expr] = own->decision();
        } else if (isUndetermined(receiver)) {
            decideDispatch(&expr, name);
        }
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
        m_resolvedCalls.contains(call)) {
        return;
    }
    if (auto choice = importedFunctionChoice(*call, name)) {
        m_resolvedCalls[call] = std::move(*choice);
        return;
    }
    // A `make` method or function of this program by that name competes
    // with the import at run time: that stays the value's to decide.
    // A higher-kinded trait's requirement registered under the name (`map`
    // on any `F<A>`, §8.2) is no definition of this program.
    const auto definedHere = [&] {
        const auto methods = m_methodSignatures.find(name);
        return methods != m_methodSignatures.end() &&
               std::any_of(methods->second.begin(), methods->second.end(), [](const Signature& s) {
                   return s.params.empty() || !s.params[0] ||
                          !std::holds_alternative<TypeApp>(s.params[0]->kind);
               });
    }();
    if (definedHere || m_userSignatures.contains(name)) {
        return;
    }
    const auto providers = m_importedInterfaces->receiverFunctions.find(name);
    if (providers == m_importedInterfaces->receiverFunctions.end()) {
        return;
    }
    const int arity =
        static_cast<int>(call->args.size() + call->namedArgs.size()) + 1 + (call->block ? 1 : 0);
    // Each module providing it at this arity that is in scope here: a
    // `make` for a type its module does not declare is reached only through
    // a `using` of that module, as lowering filters it.
    std::map<std::string, std::vector<const ImportedFunction*>> byModule;
    for (const auto& candidate : providers->second) {
        if (candidate.backendArity != arity) {
            continue;
        }
        // A module this build does not link provides nothing at run time.
        if (!m_linkedModules->contains(candidate.backendModule)) {
            continue;
        }
        // Nor does one declared for an alias: no value has that type.
        if (candidate.aliasReceiver) {
            continue;
        }
        if (candidate.foreignTarget && !makeModuleVisible(candidate.sourceModule)) {
            continue;
        }
        byModule[candidate.backendModule].push_back(&candidate);
    }
    // A module's dispatcher: the provider under the call's own name.
    const auto dispatcherOf = [&](const std::string& module) -> const ImportedFunction* {
        const auto found = byModule.find(module);
        if (found == byModule.end()) {
            return nullptr;
        }
        for (const auto* candidate : found->second) {
            if (candidate->backendFunction == name) {
                return candidate;
            }
        }
        return nullptr;
    };
    if (byModule.size() == 1) {
        if (const auto* dispatcher = dispatcherOf(byModule.begin()->first)) {
            m_resolvedCalls[call] = dispatcherTarget(*dispatcher);
        }
        return;
    }
    // Several: the prelude's dispatcher for every value the others' providers
    // do not take, each of those chosen by the value — by its type, where
    // that is a record or a primitive a guard can test, or by its registered
    // owner, where the module's provider is a dispatcher for any receiver.
    const auto* prelude = dispatcherOf(names::kPreludeModule);
    if (!prelude || !call->namedArgs.empty()) {
        return;
    }
    std::vector<RuntimeAlternative> alternatives;
    for (const auto& [module, candidates] : byModule) {
        if (module == names::kPreludeModule) {
            continue;
        }
        std::vector<RuntimeAlternative> typed;
        bool generic = false;
        for (const auto* candidate : candidates) {
            const auto receiver = candidate->signature.params.empty()
                                      ? Type::unknown()
                                      : resolve(candidate->signature.params.front());
            if (std::holds_alternative<PrimitiveType>(receiver->kind) ||
                (std::holds_alternative<NamedType>(receiver->kind) &&
                 m_recordFields.contains(std::get<NamedType>(receiver->kind).name))) {
                typed.push_back({typeToString(receiver), module, candidate->backendFunction});
            }
            // A dispatcher for any receiver, or for several types at once
            // (`make TypeRef | PatternRef`): the value's owner chooses it.
            else if (
                isUndetermined(receiver) || std::holds_alternative<UnionType>(receiver->kind)) {
                generic = true;
            } else {
                return; // a receiver no guard can test
            }
        }
        if (generic) {
            if (!typed.empty() || !dispatcherOf(module)) {
                return;
            }
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
    if (!call.namedArgs.empty()) {
        return std::nullopt;
    }
    std::string imported;
    for (const auto& module : m_localModules) {
        const auto found = m_userSignatures.find(kex::mangle::qualify(module, name));
        if (found == m_userSignatures.end() || !moduleMemberImported(module, name)) {
            continue;
        }
        if (std::none_of(
                found->second.begin(), found->second.end(),
                [&](const Signature& signature) { return signature.params.size() == arity; })) {
            continue;
        }
        if (!imported.empty()) {
            return std::nullopt; // two imports: not this case
        }
        imported = module + "." + name;
    }
    if (imported.empty()) {
        return std::nullopt;
    }
    std::vector<RuntimeAlternative> alternatives;
    std::set<std::string> seen;
    const auto record = [&](const TypePtr& receiver) -> std::string {
        const auto* named = std::get_if<NamedType>(&resolve(receiver)->kind);
        return named && m_recordFields.contains(named->name) ? named->name : std::string{};
    };
    if (const auto methods = m_methodSignatures.find(name); methods != m_methodSignatures.end()) {
        for (const auto& method : methods->second) {
            if (method.params.size() == arity) {
                if (const auto type = record(method.params.front());
                    !type.empty() && m_programRecords.contains(type) && seen.insert(type).second) {
                    alternatives.push_back({type, {}, name});
                }
            }
        }
    }
    if (const auto providers = m_importedInterfaces->receiverFunctions.find(name);
        providers != m_importedInterfaces->receiverFunctions.end()) {
        for (const auto& provider : providers->second) {
            if (provider.backendArity == static_cast<int>(arity) &&
                provider.backendFunction == name && !provider.signature.params.empty()) {
                if (const auto type = record(provider.signature.params.front());
                    !type.empty() && seen.insert(type).second) {
                    alternatives.push_back(
                        {type, provider.backendModule, provider.backendFunction});
                }
            }
        }
    }
    if (alternatives.empty()) {
        return std::nullopt;
    }
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

namespace kex::semantic {

// A bare `name` that resolved to nothing in scope but is a constructor some
// module declares: it is spelled out (`FS.FileMode.Write`, or `FS.Write`), or
// brought in with `using`.
auto TypeChecker::reportConstructorOutOfScope(const std::string& name, const SourceLocation& at)
    -> bool {
    const auto owners = m_constructorOwners.find(name);
    if (owners == m_constructorOwners.end()) {
        return false;
    }
    for (const auto& owner : owners->second) {
        if (!owner.module.empty()) {
            error(
                diag::Code::ConstructorOutOfScope, at,
                {{"name", name}, {"type", owner.adt}, {"module", owner.module}});
            return true;
        }
    }
    return false;
}

auto TypeChecker::moduleOfUnreachableConstructor(const std::string& name) const -> std::string {
    if (m_adtOfConstructor.contains(name)) {
        return {};
    }
    std::string declaring;
    for (const auto& [module, constructors] : m_moduleConstructors) {
        if (!constructors.contains(name)) {
            continue;
        }
        if (withinModule(module) || moduleMemberImported(module, name, true)) {
            return {};
        }
        if (declaring.empty() || module < declaring) {
            declaring = module;
        }
    }
    return declaring;
}

auto TypeChecker::reportUnreachableConstructor(
    const std::string& name, const std::string& declaring, const SourceLocation& at) -> void {
    if (!reportConstructorOutOfScope(name, at)) {
        error(diag::Code::UnreachableConstructor, at, {{"name", name}, {"module", declaring}});
    }
}

auto TypeChecker::reportAmbiguousImports(
    const std::vector<ImportSelection>& active, const std::string& within) -> void {
    if (active.empty() || !m_programModules) {
        return;
    }
    std::vector<resolve_core::ImportScope> origins(1);
    for (const auto& import : active) {
        forEachImportedName(
            *m_programModules, m_importedInterfaces, within, import.module, import.alias,
            import.onlyNames, import.exceptNames,
            [&](const std::string& name, const std::string& module, bool receiverMethod) {
                const auto merged = resolve_core::mergeImportOrigin(
                    origins, name, {module, !import.onlyNames.empty(), receiverMethod});
                if (!merged.ambiguity || &import != &active.back()) {
                    return;
                }
                const auto key = std::string(import.location.file) + ':' +
                                 std::to_string(import.location.line) + ':' +
                                 std::to_string(import.location.column) + ':' + name;
                if (m_reportedOnce.insert(key).second) {
                    error(
                        diag::Code::AmbiguousImportedName, import.location,
                        {{"name", name}, {"first", merged.ambiguousWith}, {"second", module}});
                }
            });
    }
}

auto TypeChecker::pushImport(ImportSelection selection) -> void {
    m_importScopeStack.back().push_back(std::move(selection));
    auto active = m_declarationImports;
    for (const auto& frame : m_importScopeStack) {
        active.insert(active.end(), frame.begin(), frame.end());
    }
    reportAmbiguousImports(active, m_currentModulePath);
}

auto TypeChecker::scopeContext() -> std::size_t {
    ScopeContext context{m_currentModulePath, m_declarationImports};
    for (const auto& frame : m_importScopeStack) {
        context.imports.insert(context.imports.end(), frame.begin(), frame.end());
    }
    auto key = context.module;
    for (const auto& import : context.imports) {
        key += '\n' + import.module + '\x1f' + import.alias.value_or("");
        for (const auto& name : import.onlyNames) {
            key += '\x1e' + name;
        }
        key += '\x1f';
        for (const auto& name : import.exceptNames) {
            key += '\x1e' + name;
        }
    }
    const auto [found, added] =
        m_scopeContextIds.try_emplace(std::move(key), m_scopeContexts.size());
    if (added) {
        m_scopeContexts.push_back(std::move(context));
    }
    return found->second;
}

auto TypeChecker::memberCompletions(const TypePtr& receiver, const ast::Expr& at)
    -> std::vector<MemberCompletion> {
    std::vector<MemberCompletion> offered;
    if (!receiver) {
        return offered;
    }
    const auto names = m_scopeNames.find(&at);
    if (names == m_scopeNames.end() || names->second.context >= m_scopeContexts.size()) {
        return offered;
    }
    const auto& context = m_scopeContexts[names->second.context];

    // Every name a method call could reach, with the signatures that say at
    // which arities to ask. Which of them a call here takes is checkCall's
    // to say; the tables only bound the question.
    std::map<std::string, std::vector<Signature>> candidates;
    const auto add = [&](const std::string& name, const Signature& signature) {
        if (name.empty() || kex::mangle::hasScope(name) || signature.params.empty() ||
            kex::startsUppercase(name)) {
            return;
        }
        candidates[name].push_back(signature);
    };
    for (const auto& [name, signatures] : m_methodSignatures) {
        for (const auto& signature : signatures) {
            add(name, signature);
        }
    }
    for (const auto& [name, signatures] : m_userSignatures) {
        for (const auto& signature : signatures) {
            add(name, signature);
        }
    }
    if (m_importedInterfaces) {
        for (const auto& [name, functions] : m_importedInterfaces->receiverFunctions) {
            for (const auto& function : functions) {
                add(name, function.signature);
            }
        }
        for (const auto& [_, module] : m_importedInterfaces->modules) {
            for (const auto& [name, functions] : module.exports) {
                for (const auto& function : functions) {
                    add(name, function.signature);
                }
            }
        }
    }
    std::set<std::string> fields;
    if (const auto* named = std::get_if<NamedType>(&resolve(receiver)->kind)) {
        const auto identity = resolveRecordName(named->name);
        if (const auto record = m_recordFields.find(identity); record != m_recordFields.end()) {
            for (const auto& [field, _] : record->second) {
                fields.insert(field);
            }
        } else if (m_importedInterfaces) {
            // A record of a module loaded on demand (`Culture.Culture`) is
            // known from its interface, not from this program's tables
            // (kexhq/kex#700). Its names are only candidates: the probe
            // below decides each read, as it does every member's.
            if (const auto record = m_importedInterfaces->recordFieldNames.find(identity);
                record != m_importedInterfaces->recordFieldNames.end()) {
                fields.insert(record->second.begin(), record->second.end());
            }
        }
    }

    const bool servingReceiver = [&] {
        const auto* named = std::get_if<NamedType>(&resolve(receiver)->kind);
        return named && named->name == kex::names::kServerType;
    }();
    // The probe runs where `at` was checked, and takes nothing with it.
    auto savedModule = std::exchange(m_currentModulePath, context.module);
    auto savedImports = std::exchange(m_declarationImports, context.imports);
    auto savedImportStack = std::exchange(m_importScopeStack, {});
    const auto savedInMake = std::exchange(m_inMakeBlock, false);
    auto savedMakeType = std::exchange(m_currentMakeType, nullptr);
    std::vector<Diagnostic> probeDiagnostics;
    auto* savedDiagnostics = std::exchange(m_diagnostics, &probeDiagnostics);
    std::vector<int> journal;
    auto* savedJournal = std::exchange(m_substJournal, &journal);
    const auto unresolvedMark = m_unresolvedMethods.size();
    const auto unresolvedImportsMark = m_unresolvedMethodImports.size();

    const auto probe = [&](const std::string& name, std::size_t arity) -> TypePtr {
        std::vector<TypePtr> arguments{receiver};
        while (arguments.size() < arity) {
            arguments.push_back(freshTypeVar());
        }
        probeDiagnostics.clear();
        auto result = checkCall(name, arguments, at.location, true);
        const bool accepted =
            probeDiagnostics.empty() && m_unresolvedMethods.size() == unresolvedMark;
        if (accepted && result) {
            result = resolvedThroughout(result);
        }
        m_unresolvedMethods.resize(unresolvedMark);
        m_unresolvedMethodImports.resize(unresolvedImportsMark);
        for (const int id : journal) {
            m_subst.erase(id);
        }
        journal.clear();
        return accepted ? result : nullptr;
    };
    // `.try` is syntax, not a function, and applies where its rule does.
    if (auto payload = tryPayload(receiver)) {
        Signature signature;
        signature.name = names::kTryKeyword;
        signature.params = {receiver};
        signature.result = payload;
        offered.push_back({signature.name, signature, payload, false});
    }
    // So is `.is?(T)`, on any value: the checker's own call
    // (findCompileTimeIntrinsic), not a function anything declares.
    {
        Signature signature;
        signature.name = names::kTypeTestCall;
        signature.params = {receiver, Type::named(names::kTypeRecord)};
        signature.result = Type::boolean();
        offered.push_back({signature.name, signature, signature.result, false});
    }
    for (const auto& field : fields) {
        if (auto result = probe(field, 1)) {
            Signature signature;
            signature.name = field;
            signature.params = {receiver};
            signature.result = result;
            offered.push_back({field, std::move(signature), std::move(result), true});
        }
    }
    for (const auto& [name, signatures] : candidates) {
        if (fields.contains(name)) {
            continue;
        }
        std::set<std::size_t> arities;
        for (const auto& signature : signatures) {
            arities.insert(signature.params.size());
            if (signature.requiredParams) {
                arities.insert(*signature.requiredParams);
            }
        }
        for (const auto arity : arities) {
            if (arity == 0) {
                continue;
            }
            if (auto result = probe(name, arity)) {
                // The signature this receiver takes at this arity: `min` on a
                // list is the list's, not the first `min` of one argument the
                // tables hold (Range's), which completion then showed.
                auto chosen = std::find_if(
                    signatures.begin(), signatures.end(), [&](const Signature& signature) {
                        return signature.params.size() == arity &&
                               argMatchesParam(receiver, signature.params.front());
                    });
                if (chosen == signatures.end()) {
                    chosen = std::find_if(
                        signatures.begin(), signatures.end(), [&](const Signature& signature) {
                            return signature.params.size() == arity;
                        });
                }
                const auto& signature = chosen != signatures.end() ? *chosen : signatures.front();
                const bool slot = servingReceiver && m_slotMethodNames.contains(name);
                // A slot is declared on the state: its handler's signature
                // (the state first) says whether it was written `foul`.
                bool declaredFoul = signature.isFoul;
                if (slot) {
                    for (const auto& handler : signatures) {
                        if (handler.params.size() == arity &&
                            !argMatchesParam(receiver, handler.params.front())) {
                            declaredFoul = handler.isFoul;
                            break;
                        }
                    }
                }
                offered.push_back({name, signature, std::move(result), false, slot, declaredFoul});
                break;
            }
        }
    }

    m_currentModulePath = std::move(savedModule);
    m_declarationImports = std::move(savedImports);
    m_importScopeStack = std::move(savedImportStack);
    m_inMakeBlock = savedInMake;
    m_currentMakeType = std::move(savedMakeType);
    m_diagnostics = savedDiagnostics;
    m_substJournal = savedJournal;
    return offered;
}

auto TypeChecker::bareCompletions(const ast::Expr& at) -> std::vector<std::string> {
    const auto names = m_scopeNames.find(&at);
    if (names == m_scopeNames.end() || names->second.context >= m_scopeContexts.size()) {
        return {};
    }
    const auto& context = m_scopeContexts[names->second.context];

    // Every name a bare use could mean; which of them one here reaches is
    // bareNameDeclared's to say.
    std::set<std::string> candidates;
    const auto addPath = [&](const std::string& path) {
        // A module is named by its first segment (`Net` of `Net.HTTP`).
        if (!path.empty()) {
            candidates.insert(path.substr(0, path.find('.')));
        }
    };
    for (const auto& [name, _] : m_userSignatures) {
        if (!kex::mangle::hasScope(name)) {
            candidates.insert(name);
        }
    }
    candidates.insert(m_topLevelValueNames.begin(), m_topLevelValueNames.end());
    for (const auto& [name, _] : m_declaredTypeArity) {
        if (!kex::mangle::hasScope(name)) {
            candidates.insert(name);
        }
    }
    for (const auto& [name, _] : m_constructorOwners) {
        candidates.insert(name);
    }
    for (const auto& [_, constructors] : m_moduleConstructors) {
        for (const auto& [name, _constructor] : constructors) {
            candidates.insert(name);
        }
    }
    for (const auto& module : m_localModules) {
        addPath(module);
    }
    if (m_importedInterfaces) {
        for (const auto& [path, module] : m_importedInterfaces->modules) {
            addPath(path);
            for (const auto& [name, _] : module.exports) {
                candidates.insert(name);
            }
        }
        for (const auto& adt : m_importedInterfaces->adts) {
            candidates.insert(adt.constructors.begin(), adt.constructors.end());
        }
    }

    // What a bare CALL reaches that a bare reference does not: a module's
    // function brought in by the prelude or a `using` (`die("x")`, `sqrt(2)`
    // under `using Math`). A receiver function reached by UFCS needs its
    // argument, and is offered after a dot.
    const auto bareCallReaches = [&](const std::string& name) {
        if (bareFunctionReachable(name)) {
            return true;
        }
        if (!m_importedInterfaces) {
            return false;
        }
        for (const auto& [path, module] : m_importedInterfaces->modules) {
            const auto found = module.exports.find(name);
            if (found == module.exports.end() || found->second.empty()) {
                continue;
            }
            if (module.automaticImport || withinModule(path) ||
                moduleMemberImported(path, name, true)) {
                return true;
            }
        }
        return false;
    };
    auto savedModule = std::exchange(m_currentModulePath, context.module);
    auto savedImports = std::exchange(m_declarationImports, context.imports);
    auto savedImportStack = std::exchange(m_importScopeStack, {});
    const auto savedInMake = std::exchange(m_inMakeBlock, false);
    // Every declaration is registered by now; a later pass (the names of a
    // merged dependency) can leave the flag down, and with it down every
    // name counts as declared.
    const auto savedRegistered = std::exchange(m_declarationsRegistered, true);
    std::set<std::string> offered(names->second.bound.begin(), names->second.bound.end());
    for (const auto& name : candidates) {
        // A dotted path is a qualified name (`Assert.equal`), not a bare one.
        if (!name.empty() && name.find('.') == std::string::npos &&
            (bareNameDeclared(name) || bareCallReaches(name))) {
            offered.insert(name);
        }
    }
    m_currentModulePath = std::move(savedModule);
    m_declarationImports = std::move(savedImports);
    m_importScopeStack = std::move(savedImportStack);
    m_declarationsRegistered = savedRegistered;
    m_inMakeBlock = savedInMake;
    // `new` is bound in every make block's scope, `_` is no name.
    offered.erase("_");
    return {offered.begin(), offered.end()};
}

auto TypeChecker::recordNames(const ast::Expr& expr) -> void {
    const auto context = scopeContext();
    auto& names = m_scopeNames[&expr];
    names = {{}, context};
    for (const auto& scope : m_scopeStack) {
        auto bound = scope.names();
        names.bound.insert(names.bound.end(), bound.begin(), bound.end());
    }
    names.bound.insert(names.bound.end(), m_topLevelValueNames.begin(), m_topLevelValueNames.end());
    if (m_inMakeBlock) {
        names.bound.push_back("new");
    }

    const std::string* name = nullptr;
    if (const auto* identifier = std::get_if<ast::Identifier>(&expr.kind)) {
        name = &identifier->name;
    } else if (const auto* upper = std::get_if<ast::UpperIdentifier>(&expr.kind)) {
        name = &upper->name;
    } else if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind)) {
        name = &call->name;
    } else if (const auto* tagged = std::get_if<ast::TaggedLiteral>(&expr.kind)) {
        name = &tagged->tag;
    } else if (const auto* capture = std::get_if<ast::CurryExpr>(&expr.kind)) {
        if (capture->module.empty() && !capture->isOperator) {
            name = &capture->name;
        }
    } else if (const auto* method = std::get_if<ast::MethodCall>(&expr.kind)) {
        if (method->receiver &&
            std::holds_alternative<ast::UpperIdentifier>(method->receiver->kind)) {
            m_namespaceRoots.insert(method->receiver.get());
        }
    }
    if (!name || name->empty() || *name == "_" || m_namespaceRoots.contains(&expr)) {
        return;
    }
    if (std::find(names.bound.begin(), names.bound.end(), *name) != names.bound.end()) {
        return;
    }
    m_nameUses[&expr] = context;
}

namespace {
// The codes of an undefined name, one set per kind of name a caller says
// it looked for ("function", "variable", ...).
struct UndefinedCodes {
    diag::Code plain;
    diag::Code hint;
    diag::Code notImported;
    diag::Code privateTo;
    diag::Code notVisible;
    diag::Code fileLocal;
};

auto undefinedCodes(const std::string& what) -> const UndefinedCodes& {
    using C = diag::Code;
    static const UndefinedCodes name{
        C::UndefinedName,        C::UndefinedNameHint,       C::UndefinedNameNotImported,
        C::UndefinedNamePrivate, C::UndefinedNameNotVisible, C::UndefinedNameFileLocal};
    static const UndefinedCodes constructorOrType{
        C::UndefinedConstructorOrType,
        C::UndefinedConstructorOrTypeHint,
        C::UndefinedConstructorOrTypeNotImported,
        C::UndefinedConstructorOrTypePrivate,
        C::UndefinedConstructorOrTypeNotVisible,
        C::UndefinedConstructorOrTypeFileLocal};
    static const UndefinedCodes variable{C::UndefinedVariable,
                                         C::UndefinedVariableHint,
                                         C::UndefinedVariableNotImported,
                                         C::UndefinedVariablePrivate,
                                         C::UndefinedVariableNotVisible,
                                         C::UndefinedNameFileLocal};
    static const UndefinedCodes function{C::UndefinedFunction,
                                         C::UndefinedFunctionHint,
                                         C::UndefinedFunctionNotImported,
                                         C::UndefinedFunctionPrivate,
                                         C::UndefinedFunctionNotVisible,
                                         C::UndefinedFunctionFileLocal};
    static const UndefinedCodes tag{
        C::UndefinedTag,        C::UndefinedTagHint,       C::UndefinedTagNotImported,
        C::UndefinedTagPrivate, C::UndefinedTagNotVisible, C::UndefinedNameFileLocal};
    if (what == "constructor or type") {
        return constructorOrType;
    }
    if (what == "variable") {
        return variable;
    }
    if (what == "function") {
        return function;
    }
    if (what == "tag") {
        return tag;
    }
    return name;
}
} // namespace

auto TypeChecker::reportOtherFilesTopLevel(
    const std::string& what, const std::string& name, const SourceLocation& at) -> bool {
    const auto found = m_fileLocalTopLevel.find(name);
    if (found == m_fileLocalTopLevel.end() || found->second == at.file || lookupVar(name)) {
        return false;
    }
    const auto code = undefinedCodes(what).fileLocal;
    diag::Args args = {
        {"name", name}, {"file", std::filesystem::path(found->second).filename().string()}};
    const auto key = std::string(at.file) + ':' + std::to_string(at.line) + ':' +
                     std::to_string(at.column) + ':' + diag::render(code, args);
    if (m_reportedOnce.insert(key).second) {
        error(code, at, std::move(args));
    }
    return true;
}

// A function with no declared result whose own `.try` can fail leaves with
// the failure: `None` for an Optional's, `Error(e)` for a Result's. Its result
// says so: `T?` or `T or! E`, its successful value wrapped to match (the
// backends read `successWrapperOf`). A body that already answers a Result or
// an Optional is left as it is. Not inferred, and refused instead, where the
// clause `return`s (each exit would need wrapping) or mixes an Optional's
// `.try` with a Result's (no one result holds both failures).
auto TypeChecker::inferredFailingResult(
    const std::string& who,
    const std::vector<ast::ExprPtr>& bodyExprs,
    const std::vector<std::pair<SourceLocation, TypePtr>>& tries,
    const TypePtr& body,
    bool isFunction) -> TypePtr {
    // A body whose type is undetermined (a `match` whose arms all `return`)
    // says nothing yet; only one known to hold the failure stands as it is.
    if (!isUndetermined(body) && holdsFailure(body)) {
        return body;
    }
    TypePtr errorType;
    bool optional = false;
    bool result = false;
    for (const auto& [_, operand] : tries) {
        const auto failing = resolve(operand);
        const auto* named = std::get_if<NamedType>(&failing->kind);
        if (std::holds_alternative<OptionalType>(failing->kind) ||
            (named && named->name == names::kOptionalType)) {
            optional = true;
        } else if (named && named->name == names::kResultType && named->typeArgs.size() == 2) {
            result = true;
            if (!errorType) {
                errorType = named->typeArgs[1];
            }
        } else {
            return body;
        }
    }
    const auto& first = tries.front().first;
    const auto returns =
        isFunction && std::any_of(bodyExprs.begin(), bodyExprs.end(), [](const ast::ExprPtr& e) {
            return e && ast::anyExpr(*e, [](const ast::Expr& node) {
                       // A block's `return` leaves the function too (kexhq/kex#408).
                       return std::holds_alternative<ast::ReturnExpr>(node.kind);
                   });
        });
    // Only a function refuses: a block keeps its body's result.
    const auto refuse = [&](diag::Code code) {
        // A body whose every branch leaves with `return` is Never: it says no
        // more about the result than an undetermined one does.
        const bool unknownResult =
            isUndetermined(body) || std::holds_alternative<NeverType>(resolve(body)->kind);
        const auto shown = unknownResult ? std::string{"T"} : typeToString(body);
        error(code, first, {{"name", who}, {"type", shown}});
        return body;
    };
    // No one result holds both failures. A function must say which it
    // means; a block (an `it`, a `Task.start`, most often unread) keeps
    // its body's result, as before.
    if (optional && result) {
        return isFunction ? refuse(diag::Code::TryMixedFailuresUndeclared) : body;
    }
    if (returns) {
        return refuse(diag::Code::TryReturnsUndeclared);
    }
    if (isUndetermined(body)) {
        return body;
    }
    m_successWrappers[bodyExprs.back().get()] = optional ? "Just" : "Ok";
    return optional
               ? Type::optional(body)
               : Type::named(names::kResultType, {body, errorType ? errorType : Type::unknown()});
}

// A failed `.try` leaves its body with the failure as the result. A declared
// result that is no Result and no Optional has nowhere to put it: a caller
// would get an `Error` where a `User` was promised.
auto TypeChecker::refuseUnheldTries(
    const std::string& who,
    const TypePtr& declared,
    const std::vector<std::pair<SourceLocation, TypePtr>>& tries) -> void {
    const auto promised = resolve(declared);
    if (holdsFailure(promised)) {
        return;
    }
    for (const auto& [at, _] : tries) {
        if (who.empty()) {
            error(diag::Code::TryFailureNotHeldBlock, at, {{"type", typeToString(promised)}});
        } else {
            error(
                diag::Code::TryFailureNotHeld, at,
                {{"name", who}, {"type", typeToString(promised)}});
        }
    }
}

// A block is a function boundary for `.try` as a named function is: its
// failure is the block's result (`xs.map { |x| find(x).try }` hands `map` an
// `Error` among the values). So the same rules hold. A declared result must
// be able to hold the failure, and an undeclared one is inferred to. A
// block's `return` leaves the function it is written in, not the block, so
// it needs no wrapping and refuses nothing here.
auto TypeChecker::blockFailingResult(
    const ast::Lambda& block,
    const std::vector<std::pair<SourceLocation, TypePtr>>& tries,
    const TypePtr& type) -> TypePtr {
    if (tries.empty() || block.rescue || block.collection || block.body.empty() ||
        !block.body.back()) {
        return type;
    }
    if (block.returnAnnotation) {
        refuseUnheldTries({}, type, tries);
        return type;
    }
    return inferredFailingResult({}, block.body, tries, type, false);
}

auto TypeChecker::bareFunctionReachable(const std::string& name) const -> bool {
    std::vector<Signature> visible;
    std::set<std::string> hidden;
    const auto* reached = reachableFreeFunctions(name, false, visible, hidden);
    return reached && !reached->empty();
}

auto TypeChecker::bareNameDeclared(const std::string& name) const -> bool {
    if (!m_declarationsRegistered) {
        return true;
    }
    if (m_topLevelValueNames.contains(name) || bareFunctionReachable(name)) {
        return true;
    }
    if (m_inMakeBlock && m_methodSignatures.contains(name)) {
        return true;
    }
    // A function of a module compiled apart, where a `using` brings its
    // names in bare (`as:` alone does not).
    if (m_importedInterfaces) {
        for (const auto& [moduleName, module] : m_importedInterfaces->modules) {
            if (module.compiledApart && module.exports.count(name) &&
                moduleMemberImported(moduleName, name, true)) {
                return true;
            }
        }
    }
    // A type or a module, named as a value (`x.to(Binary)`), and a
    // constructor the prelude brings in.
    if (name.empty() || !kex::startsUppercase(name)) {
        return false;
    }
    // A constructor declared in a module is reached inside it or under a
    // `using` of it, as a use of it is resolved: counted anywhere, completion
    // offered `Template`'s `UndefinedVariable` in every file, and the use was
    // then refused (reportConstructorOutOfScope).
    const auto owners = m_constructorOwners.find(name);
    const bool ownerVisible =
        owners != m_constructorOwners.end() &&
        std::any_of(owners->second.begin(), owners->second.end(), [&](const auto& owner) {
            return owner.module.empty() || withinModule(owner.module) ||
                   moduleMemberImported(owner.module, name, true);
        });
    if (namesKnownType(name) || namesDeclaredType(name, name) || ownerVisible) {
        return true;
    }
    // A module the program declares, named by its first segment (`Net` of
    // `Net.HTTP`), as an imported one is below: completion offered `Math`
    // but not the file's own `Haversine`.
    if (std::any_of(m_localModules.begin(), m_localModules.end(), [&](const std::string& module) {
            return module.substr(0, module.find('.')) == name;
        })) {
        return true;
    }
    // A module's constructor, inside the module or under a `using` of it.
    for (const auto& [module, constructors] : m_moduleConstructors) {
        if (constructors.count(name) &&
            (withinModule(module) || moduleMemberImported(module, name, true))) {
            return true;
        }
    }
    if (!m_importedInterfaces) {
        return false;
    }
    if (m_importedInterfaces->modules.count(name)) {
        return true;
    }
    for (const auto& adt : m_importedInterfaces->adts) {
        if (std::find(adt.constructors.begin(), adt.constructors.end(), name) !=
                adt.constructors.end() &&
            owners == m_constructorOwners.end()) {
            return true;
        }
    }
    return std::any_of(
        m_importedInterfaces->modules.begin(), m_importedInterfaces->modules.end(),
        [&](const auto& module) {
            return module.second.automaticImport && module.second.exports.count(name);
        });
}

auto TypeChecker::reportUndefined(
    const std::string& what, const std::string& name, const SourceLocation& at) -> void {
    // The names in reach, nearest scope first, then the program's own.
    std::vector<std::string> candidates;
    for (auto scope = m_scopeStack.rbegin(); scope != m_scopeStack.rend(); ++scope) {
        auto names = scope->names();
        std::sort(names.begin(), names.end());
        candidates.insert(candidates.end(), names.begin(), names.end());
    }
    std::vector<std::string> declared;
    for (const auto& [function, _] : m_userSignatures) {
        if (!kex::mangle::hasScope(function)) {
            declared.push_back(function);
        }
    }
    declared.insert(declared.end(), m_topLevelValueNames.begin(), m_topLevelValueNames.end());
    std::sort(declared.begin(), declared.end());
    candidates.insert(candidates.end(), declared.begin(), declared.end());
    std::erase(candidates, name);
    const auto& codes = undefinedCodes(what);
    // A module declares it, but not where this can see: name that module
    // rather than guess at a spelling.
    std::vector<Signature> visible;
    std::set<std::string> hidden;
    reachableFreeFunctions(name, false, visible, hidden);
    hidden.erase("");
    if (hidden.size() == 1) {
        const auto& module = *hidden.begin();
        if (m_privateFreeFunctions.contains(module + "\n" + name)) {
            error(codes.privateTo, at, {{"name", name}, {"module", module}});
        } else {
            error(codes.notImported, at, {{"name", name}, {"module", module}});
        }
    } else if (!hidden.empty()) {
        std::string owners;
        for (const auto& module : hidden) {
            owners += (owners.empty() ? "`" : ", `") + module + "." + name + "`";
        }
        error(codes.notVisible, at, {{"name", name}, {"owners", owners}});
    } else if (const auto hint = closestName(name, candidates); !hint.empty()) {
        error(codes.hint, at, {{"name", name}, {"hint", hint}});
    } else {
        error(codes.plain, at, {{"name", name}});
    }
}

} // namespace kex::semantic

namespace kex::semantic {

// Whether a type name an annotation wrote names a declared type: one this
// program declares (at top level by its name, in a module by its identity or
// its name), a record or sum type the checker registered, or one an imported
// interface exports.
auto TypeChecker::namesDeclaredType(const std::string& qualified, const std::string& last) const
    -> bool {
    // `This` in a trait names whichever type implements it.
    if (last == names::kThisType || builtinType(last) || isPrimitiveTypeName(last)) {
        return true;
    }
    if (m_traits.lookup(qualified)) {
        return true;
    }
    if (m_declaredTypeNames.count(qualified) || m_declaredTypeNames.count(last) ||
        m_moduleTypeIdentities.count(qualified) || m_recordFields.count(qualified) ||
        m_adtVariants.count(qualified)) {
        return true;
    }
    return m_importedInterfaces && (m_importedInterfaces->typeNames.count(qualified) ||
                                    m_importedInterfaces->typeNames.count(last));
}

// An annotation naming no type the checker could resolve, reported once per
// written name however often it is resolved.
auto TypeChecker::reportUnknownType(const ast::TypeName& name, const SourceLocation& at) -> void {
    if (!m_reportUnknownTypes || !m_reportedUnknownTypes.insert(&name).second) {
        return;
    }
    // While declarations are still being registered a name may be one
    // registered later (a trait an ADT payload names): judged once they all
    // are (reportPendingUnknownTypes).
    if (!m_declarationsRegistered) {
        m_pendingUnknownTypes.push_back({&name, at});
        return;
    }
    error(diag::Code::UnknownType, at, {{"name", name.dotted()}});
}

auto TypeChecker::reportPendingUnknownTypes() -> void {
    m_declarationsRegistered = true;
    for (const auto& [name, at] : m_pendingUnknownTypes) {
        if (!namesDeclaredType(name->dotted(), name->parts.empty() ? "" : name->parts.back())) {
            error(diag::Code::UnknownType, at, {{"name", name->dotted()}});
        }
    }
    m_pendingUnknownTypes.clear();
}

} // namespace kex::semantic

namespace kex::semantic {

// A contract's type letters, still inference variables when it is resolved,
// as a generalized signature numbers them (`A` is -1, `B` -2, ...): by first
// appearance, parameters then result. What an interface states it as.
auto TypeChecker::quantifiedContract(const Signature& contract) const -> Signature {
    std::unordered_map<int, int> letters;
    std::function<TypePtr(const TypePtr&)> quantify = [&](const TypePtr& type) -> TypePtr {
        if (!type) {
            return type;
        }
        const auto resolved = resolve(type);
        if (const auto* variable = std::get_if<TypeVar>(&resolved->kind)) {
            if (isGenericLetter(*variable)) {
                return resolved;
            }
            const auto [letter, _] =
                letters.try_emplace(variable->id, -static_cast<int>(letters.size()) - 1);
            return Type::genericLetter(letter->second);
        }
        return mapTypeChildren(resolved, quantify);
    };
    Signature quantified = contract;
    for (auto& param : quantified.params) {
        param = quantify(param);
    }
    quantified.result = quantify(quantified.result);
    return quantified;
}

} // namespace kex::semantic

namespace kex::semantic {

// Whether constructors of different arities share the spelling `name`
// (`ReadFailed` of ReadError, nullary, and of FS.FileError, with a path).
auto TypeChecker::spellingHasSeveralArities(const std::string& name) const -> bool {
    const auto owners = m_constructorOwners.find(name);
    if (owners == m_constructorOwners.end()) {
        return false;
    }
    return std::any_of(owners->second.begin(), owners->second.end(), [&](const auto& owner) {
        return owner.arity != owners->second.front().arity;
    });
}

} // namespace kex::semantic

namespace kex::semantic {

// The type a `make` block's methods belong to, as one key for every block
// of it: `make String` and `make String, implement: Monoid` are one type's.
auto TypeChecker::makeTargetKey(const TypePtr& receiver) const -> std::string {
    // The key trait implementations are filed under: one notion of which type.
    return m_traits.implementorKey(resolve(receiver));
}

// A method's key for overriding: its name and its parameters past the receiver.
auto TypeChecker::ownMethodKey(const ast::FunctionDef& method) -> std::string {
    return method.name + "/" +
           std::to_string(method.clauses.empty() ? 0 : method.clauses[0].params.size());
}

// What a `make` block defines for its target, resolved where the block is
// written (its module and its imports), so a trait default the type defines
// in another block is not inherited by this one.
auto TypeChecker::noteOwnMethods(const ast::MakeDef& def, const std::string& modulePath) -> void {
    if (!def.target) {
        return;
    }
    const auto previousModulePath = m_currentModulePath;
    auto previousImports = m_declarationImports;
    m_currentModulePath = modulePath;
    if (auto imports = m_makeImports.find(&def); imports != m_makeImports.end()) {
        m_declarationImports = imports->second;
    }
    std::unordered_map<std::string, TypePtr> targetVars;
    const bool reporting = m_reportUnknownTypes;
    m_reportUnknownTypes = false;
    const auto key = makeTargetKey(resolveTypeExpr(*def.target, targetVars));
    m_reportUnknownTypes = reporting;
    m_currentModulePath = previousModulePath;
    m_declarationImports = std::move(previousImports);
    auto& own = m_ownMethodsByTarget[key];
    for (const auto& item : def.body) {
        if (const auto* function = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*function) {
                own.insert(ownMethodKey(**function));
            }
        } else if (const auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items) {
                if (const auto* function =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible)) {
                    if (*function) {
                        own.insert(ownMethodKey(**function));
                    }
                }
            }
        }
    }
}

auto TypeChecker::recordFieldType(
    const std::string& record,
    const std::string& field,
    const std::vector<std::string>& typeParams) const -> TypePtr {
    std::vector<TypePtr> named;
    for (const auto& param : typeParams) {
        named.push_back(Type::named(param));
    }
    return substituteInterfaceGenerics(recordFieldType(record, field), named);
}

} // namespace kex::semantic

namespace kex::semantic {

// A function declared to return a Result or an Optional answers its success
// by the value its body ends in: `-> Integer or! String` ending in
// `parsed + 1` is `Ok(parsed + 1)`, `-> String?` ending in a String is
// `Just` of it, as a function whose result is inferred from its `.try`s is.
// A failure leaves through a `.try`, so a body need not spell the success
// it did not fail at.
auto TypeChecker::wrapsDeclaredSuccess(
    const std::vector<ast::ExprPtr>& exprs, const TypePtr& declared, const TypePtr& body) -> bool {
    if (!declared || !body || exprs.empty() || !exprs.back()) {
        return false;
    }
    const auto* last = exprs.back().get();
    // A `return` says what it returns; it is not the value the body ends in.
    if (std::holds_alternative<ast::ReturnExpr>(last->kind)) {
        return false;
    }
    // What the declared result holds on success, and its constructor: a
    // Result's `Ok`, an Optional's `Just`.
    const auto promised = resolve(declared);
    TypePtr success;
    std::string wrapper;
    if (const auto* result = std::get_if<NamedType>(&promised->kind);
        result && result->name == names::kResultType && result->typeArgs.size() == 2) {
        success = result->typeArgs[0];
        wrapper = "Ok";
    } else if (const auto* optional = std::get_if<OptionalType>(&promised->kind)) {
        success = optional->inner;
        wrapper = "Just";
    } else {
        return false;
    }
    // A body ending in the holder itself answers as it is.
    const auto produced = resolve(body);
    if (holdsFailure(produced) || !argMatchesParam(produced, resolve(success))) {
        return false;
    }
    m_successWrappers[last] = wrapper;
    return true;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::conformanceKeys(const ast::MakeDef& def, const std::string& typeName)
    -> std::vector<std::string> {
    // One instantiation (`Range<Integer>`), when every argument is concrete.
    const auto instance = [&](const ast::TypeExpr& target) -> std::string {
        const auto* generic = std::get_if<ast::GenericType>(&target.kind);
        if (!generic || generic->args.empty()) {
            return {};
        }
        for (const auto& arg : generic->args) {
            const auto* name = arg ? std::get_if<ast::TypeName>(&arg->kind) : nullptr;
            // A type variable (`Range<A>`): every instantiation conforms.
            if (!name || (name->parts.size() == 1 && name->parts[0].size() == 1)) {
                return {};
            }
        }
        std::unordered_map<std::string, TypePtr> genericVars;
        const auto resolved = resolveTypeExpr(target, genericVars);
        const auto* named = resolved ? std::get_if<NamedType>(&resolved->kind) : nullptr;
        return named && !named->typeArgs.empty() ? typeToString(resolved) : std::string{};
    };
    if (!def.target) {
        return {};
    }
    // `make Range<Integer> | Range<Char>`: each instantiation conforms.
    if (std::holds_alternative<ast::UnionType>(def.target->kind)) {
        std::vector<std::string> keys;
        std::vector<const ast::TypeExpr*> pending{def.target.get()};
        while (!pending.empty()) {
            const auto* member = pending.back();
            pending.pop_back();
            if (const auto* uni = member ? std::get_if<ast::UnionType>(&member->kind) : nullptr) {
                pending.push_back(uni->right.get());
                pending.push_back(uni->left.get());
                continue;
            }
            auto key = member ? instance(*member) : std::string{};
            if (key.empty()) {
                return typeName.empty() ? std::vector<std::string>{}
                                        : std::vector<std::string>{typeName};
            }
            keys.push_back(std::move(key));
        }
        return keys;
    }
    if (auto key = instance(*def.target); !key.empty()) {
        return {key};
    }
    return typeName.empty() ? std::vector<std::string>{} : std::vector<std::string>{typeName};
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::makeReceiverBounds(const ast::MakeDef& def)
    -> std::vector<std::pair<std::size_t, std::string>> {
    auto bounds = ast::makeTypeArgBounds(def);
    static const std::vector<ImportSelection> kNoImports;
    const auto imports = m_makeImports.find(&def);
    for (auto& [_, trait] : bounds) {
        trait = resolveTraitReference(
            trait, m_currentModulePath,
            imports == m_makeImports.end() ? &kNoImports : &imports->second);
    }
    return bounds;
}

auto TypeChecker::unmetReceiverBound(const Signature& signature, const TypePtr& receiver)
    -> std::optional<std::pair<TypePtr, std::string>> {
    if (signature.receiverBounds.empty() || !receiver) {
        return std::nullopt;
    }
    const auto resolved = resolve(receiver);
    const auto* named = std::get_if<NamedType>(&resolved->kind);
    if (!named) {
        return std::nullopt;
    }
    for (const auto& [index, trait] : signature.receiverBounds) {
        if (index >= named->typeArgs.size()) {
            continue;
        }
        const auto argument = resolve(named->typeArgs[index]);
        // Not yet known: the call cannot be judged here.
        if (!isConcreteValue(argument)) {
            continue;
        }
        if (!satisfiesTrait(argument, trait)) {
            return std::pair{argument, trait};
        }
    }
    return std::nullopt;
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::registerConformance(
    const ast::MakeDef& def, const std::string& key, const std::string& traitName) -> void {
    if (auto bounds = makeReceiverBounds(def); !bounds.empty()) {
        m_traits.registerConditionalImplementation(key, traitName, std::move(bounds));
        return;
    }
    m_traits.registerImplementation(key, traitName);
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::collectTypeBounds(const ast::Program& program) -> void {
    m_typeNodeBounds.clear();
    m_makeTargetNodes.clear();
    m_constructorHoles.clear();
    using Bounds = std::vector<std::pair<std::string, ast::TypeName>>;
    m_boundTraitSites.clear();
    // Where each bound's trait is written, with what it resolves to.
    const auto recordSites = [&](const Bounds& bounds, const std::vector<SourceLocation>& at) {
        for (std::size_t i = 0; i < bounds.size() && i < at.size(); ++i) {
            m_boundTraitSites.emplace_back(
                at[i],
                resolveTraitReference(bounds[i].second.dotted(), m_currentModulePath, nullptr));
        }
    };
    // Each node of `types` that names a variable `bounds` bounds.
    const auto recordAll = [&](const Bounds& bounds,
                               const std::vector<const ast::TypeExpr*>& types) {
        if (bounds.empty()) {
            return;
        }
        std::unordered_map<std::string, std::vector<std::string>> traits;
        for (const auto& [variable, trait] : bounds) {
            traits[variable].push_back(
                resolveTraitReference(trait.dotted(), m_currentModulePath, nullptr));
        }
        const auto record = [&](const ast::TypeExpr& node) {
            std::string letter;
            if (const auto* name = std::get_if<ast::TypeName>(&node.kind);
                name && name->parts.size() == 1) {
                letter = name->parts[0];
            } else if (const auto* var = std::get_if<ast::GenericVar>(&node.kind)) {
                letter = var->name;
            } else if (const auto* applied = std::get_if<ast::GenericType>(&node.kind);
                       applied && applied->name.parts.size() == 1) {
                letter = applied->name.parts[0]; // `F<Integer>` under `F is Functor`
            }
            if (const auto found = traits.find(letter); found != traits.end()) {
                m_typeNodeBounds[&node] = found->second;
            }
        };
        for (const auto* type : types) {
            if (type) {
                ast::forEachTypeExpr(*type, record);
            }
        }
    };
    ast::forEachFunction(program, [&](const ast::FunctionDef& function) {
        for (const auto& clause : function.clauses) {
            std::vector<const ast::TypeExpr*> types;
            for (const auto& param : clause.params) {
                if (param.type && *param.type) {
                    types.push_back(param.type->get());
                }
            }
            if (clause.returnAnnotation && *clause.returnAnnotation) {
                types.push_back(clause.returnAnnotation->get());
            }
            recordAll(clause.typeParamBounds, types);
            recordSites(clause.typeParamBounds, clause.typeParamBoundLocations);
        }
    });
    ast::forEachDeclared<ast::TypeAnnotation>(program, [&](const ast::TypeAnnotation& annotation) {
        recordAll(annotation.typeParamBounds, {annotation.type.get()});
        recordSites(annotation.typeParamBounds, annotation.typeParamBoundLocations);
    });
    ast::forEachDeclared<ast::MakeDef>(program, [&](const ast::MakeDef& make) {
        recordSites(make.typeParamBounds, make.typeParamBoundLocations);
        if (make.target) {
            m_makeTargetNodes.insert(make.target.get());
        }
        // The positions a type lambda leaves open: `make Tagged<_, E>`, {0}.
        if (const auto holes = ast::makeTypeLambdaHoles(make); !holes.empty()) {
            const auto written = std::get<ast::GenericType>(make.target->kind).name.dotted();
            m_constructorHoles[written] = holes;
            m_constructorHoles[resolveRecordName(written)] = holes;
        }
    });
}

auto TypeChecker::boundedPlaceholder(const ast::TypeExpr& node, const std::string& letter)
    -> TypePtr {
    const auto found = m_typeNodeBounds.find(&node);
    if (found == m_typeNodeBounds.end()) {
        return freshTypeVar();
    }
    std::vector<TypePtr> bounds;
    for (const auto& trait : found->second) {
        bounds.push_back(Type::constrained(letter, trait));
    }
    return bounds.size() == 1 ? bounds.front() : Type::intersection(std::move(bounds));
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::reportTypeArgumentCount(
    const ast::TypeExpr& at, const std::string& name, std::size_t declared, std::size_t given)
    -> void {
    if (!m_diagnostics) {
        return;
    }
    const auto code = declared == 1 ? diag::Code::TypeArgumentCountOne
                      : given == 1  ? diag::Code::TypeArgumentCountOneGiven
                                    : diag::Code::TypeArgumentCount;
    diag::Args slots = {
        {"name", name}, {"count", std::to_string(declared)}, {"given", std::to_string(given)}};
    const auto message = diag::render(code, slots);
    // Once, though an annotation is resolved by several passes.
    const bool reported =
        std::any_of(m_diagnostics->begin(), m_diagnostics->end(), [&](const Diagnostic& d) {
            return d.message == message && d.location.line == at.location.line &&
                   d.location.column == at.location.column && d.location.file == at.location.file;
        });
    if (!reported) {
        error(code, at.location, std::move(slots));
    }
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::reshapedApplication(Application application, std::size_t arity) const
    -> Application {
    const auto* head = application.head ? std::get_if<NamedType>(&application.head->kind) : nullptr;
    if (!head || application.args.size() <= arity) {
        return application;
    }
    auto holes = m_constructorHoles.find(head->name);
    if (holes == m_constructorHoles.end()) {
        holes = m_importedConstructorHoles.find(head->name);
        if (holes == m_importedConstructorHoles.end()) {
            return application;
        }
    }
    if (holes->second.size() != arity) {
        return application;
    }
    std::vector<TypePtr> kept = application.args;
    std::vector<TypePtr> open;
    for (const auto position : holes->second) {
        if (position < kept.size()) {
            open.push_back(kept[position]);
            kept[position] = Type::hole();
        }
    }
    return {Type::named(head->name, std::move(kept)), std::move(open)};
}

} // namespace kex::semantic

namespace kex::semantic {

auto TypeChecker::boundTypeCall(const TypePtr& variable, const ast::MethodCall& call) -> TypePtr {
    const auto placeholder = resolve(variable);
    std::vector<std::string> traits;
    if (const auto* bound = std::get_if<ConstrainedType>(&placeholder->kind)) {
        traits.push_back(bound->traitName);
    } else if (const auto* both = std::get_if<IntersectionType>(&placeholder->kind)) {
        for (const auto& member : both->members) {
            if (const auto* bound = std::get_if<ConstrainedType>(&resolve(member)->kind)) {
                traits.push_back(bound->traitName);
            }
        }
    }
    for (const auto& name : traits) {
        const auto* trait = m_traits.get(name);
        if (!trait) {
            continue;
        }
        for (const auto& required : trait->requiredMethods) {
            if (required.name != call.method || required.params.size() != call.args.size()) {
                continue;
            }
            // The requirement's `This` is the variable itself: `identity :> This`
            // answers an A. In a trait over constructors, the trait's parameter
            // applied is the variable applied: `wrap : A -> F<A>` through `M`
            // answers an `M<A>`.
            std::function<TypePtr(const TypePtr&)> applyVariable =
                [&](const TypePtr& type) -> TypePtr {
                if (!type) {
                    return type;
                }
                const auto mapped = mapTypeChildren(type, applyVariable);
                if (const auto* app = std::get_if<TypeApp>(&mapped->kind);
                    app && !trait->paramKinds.empty() && trait->paramKinds[0] > 0 &&
                    std::holds_alternative<TypeVar>(resolve(app->head)->kind)) {
                    return Type::app(variable, app->args);
                }
                return mapped;
            };
            return applyVariable(substituteThis(required.result, variable));
        }
    }
    return nullptr;
}

} // namespace kex::semantic

namespace kex::semantic {

// Which functions need their bounded types passed, and what each call of one
// passes (§12 step 7). A function needs them when a clause body calls through
// a letter (`A.identity`), or hands its letter on to a function that does.
// Each call's types come from matching the callee's written parameter types
// against the argument types: `xs: [A]` given a `[Purse]` makes A a Purse.
auto TypeChecker::recordTypeArguments(const ast::Program& program) -> void {
    m_typeParameters.clear();
    m_typeArguments.clear();
    m_methodTypeArguments.clear();
    std::unordered_map<const ast::FunctionClause*, const ast::FunctionDef*> defOf;
    std::unordered_map<const ast::FunctionDef*, std::vector<std::string>> letters;
    std::unordered_map<std::string, const ast::FunctionDef*> byName;
    std::unordered_set<std::string> namedTwice;
    std::vector<const ast::FunctionDef*> functions;
    ast::forEachFunction(program, [&](const ast::FunctionDef& def) {
        functions.push_back(&def);
        if (!byName.emplace(def.name, &def).second) {
            namedTwice.insert(def.name);
        }
        for (const auto& clause : def.clauses) {
            defOf[&clause] = &def;
        }
        if (!def.clauses.empty()) {
            auto& mine = letters[&def];
            for (const auto& [letter, _] : def.clauses.front().typeParamBounds) {
                if (std::find(mine.begin(), mine.end(), letter) == mine.end()) {
                    mine.push_back(letter);
                }
            }
        }
    });

    for (const auto& name : namedTwice) {
        byName.erase(name);
    }
    struct Call {
        const ast::Expr* expr;
        const ast::FunctionDef* callee;
        const ast::FunctionDef* caller;
    };
    std::vector<Call> calls;
    struct ImportedCall {
        const ast::Expr* expr;
        const Signature* callee;
        const ast::FunctionDef* caller;
    };
    std::vector<ImportedCall> importedCalls;
    std::unordered_set<const ast::FunctionDef*> needs;
    // Every function's body, and `main`'s (a caller with no letters of its own).
    const auto scan = [&](const ast::FunctionDef* def, const std::vector<ast::ExprPtr>& body) {
        std::function<void(const ast::Expr&)> visit = [&](const ast::Expr& e) {
            if (def && m_typeVariableReceivers.contains(&e)) {
                needs.insert(def);
            }
            if (const auto selected = m_selectedCallSignatures.find(&e);
                selected != m_selectedCallSignatures.end()) {
                const ast::FunctionDef* callee = nullptr;
                if (const auto byClause = defOf.find(selected->second.clause);
                    selected->second.clause && byClause != defOf.end()) {
                    callee = byClause->second;
                } else if (const auto named = byName.find(selected->second.name);
                           named != byName.end()) {
                    // A plain function call keeps no clause: the bounded function of
                    // that name, when it is the only one.
                    callee = named->second;
                }
                if (callee && !letters[callee].empty()) {
                    calls.push_back({&e, callee, def});
                } else if (!callee && !selected->second.typeParameters.empty()) {
                    // A function of another unit, known by its interface.
                    importedCalls.push_back({&e, &selected->second, def});
                }
            }
            ast::forEachChild(e, [&](const ast::ExprPtr& child) {
                if (child) {
                    visit(*child);
                }
            });
        };
        for (const auto& expr : body) {
            if (expr) {
                visit(*expr);
            }
        }
    };
    for (const auto* def : functions) {
        for (const auto& clause : def->clauses) {
            scan(def, clause.body);
        }
    }
    for (const auto& item : program.items) {
        if (const auto* main = std::get_if<std::unique_ptr<ast::MainBlock>>(&item); main && *main) {
            scan(nullptr, (*main)->body);
        }
    }

    // The argument expressions of a call, in parameter order. A module written
    // as the receiver (`Folds.total(xs)`) is none: there the arguments alone
    // fill the parameters.
    const auto argumentsOf = [](const ast::Expr& call, std::size_t parameters) {
        std::vector<const ast::Expr*> args;
        if (const auto* function = std::get_if<ast::FunctionCall>(&call.kind)) {
            for (const auto& arg : function->args) {
                args.push_back(arg.get());
            }
            if (function->block && *function->block) {
                args.push_back(function->block->get());
            }
        } else if (const auto* method = std::get_if<ast::MethodCall>(&call.kind)) {
            const auto given = method->args.size() + (method->block && *method->block ? 1 : 0);
            if (given < parameters) {
                args.push_back(method->receiver.get());
            }
            for (const auto& arg : method->args) {
                args.push_back(arg.get());
            }
            if (method->block && *method->block) {
                args.push_back(method->block->get());
            }
        }
        return args;
    };
    // What each letter is at one call.
    const auto argumentsAt = [&](const Call& call) {
        std::unordered_map<std::string, TypePtr> found;
        const auto& wanted = letters[call.callee];
        const std::function<void(const ast::TypeExpr&, const TypePtr&)> match =
            [&](const ast::TypeExpr& written, const TypePtr& given) {
                if (!given) {
                    return;
                }
                const auto actual = resolve(given);
                if (const auto* name = std::get_if<ast::TypeName>(&written.kind);
                    name && name->parts.size() == 1 &&
                    std::find(wanted.begin(), wanted.end(), name->parts[0]) != wanted.end()) {
                    found.try_emplace(name->parts[0], actual);
                } else if (const auto* list = std::get_if<ast::ListType>(&written.kind)) {
                    if (const auto* elements = std::get_if<ListType>(&actual->kind);
                        list->element) {
                        if (elements) {
                            match(*list->element, elements->element);
                        }
                    }
                } else if (const auto* optional = std::get_if<ast::OptionalType>(&written.kind)) {
                    if (const auto* inner = std::get_if<OptionalType>(&actual->kind);
                        inner && optional->inner) {
                        match(*optional->inner, inner->inner);
                    }
                } else if (const auto* function = std::get_if<ast::FunctionType>(&written.kind)) {
                    // `f: A -> F<B>` given a block answering a Result: F is Result.
                    if (const auto* given = std::get_if<FuncType>(&actual->kind)) {
                        if (function->param && given->params.size() == 1) {
                            match(*function->param, given->params.front());
                        }
                        if (function->result) {
                            match(*function->result, given->result);
                        }
                    }
                } else if (const auto* generic = std::get_if<ast::GenericType>(&written.kind)) {
                    const auto applied = applicationOf(actual);
                    // `xs: M<Integer>` given a `Crate<Integer>`: the letter is the
                    // constructor, Crate.
                    if (applied && generic->name.parts.size() == 1 &&
                        std::find(wanted.begin(), wanted.end(), generic->name.parts[0]) !=
                            wanted.end()) {
                        found.try_emplace(generic->name.parts[0], resolve(applied->head));
                    }
                    if (applied && applied->args.size() == generic->args.size()) {
                        for (std::size_t i = 0; i < generic->args.size(); ++i) {
                            if (generic->args[i]) {
                                match(*generic->args[i], applied->args[i]);
                            }
                        }
                    }
                }
            };
        const auto& params = call.callee->clauses.front().params;
        const auto args = argumentsOf(*call.expr, params.size());
        for (std::size_t i = 0; i < params.size() && i < args.size(); ++i) {
            if (params[i].type && *params[i].type && args[i]) {
                match(**params[i].type, typeOf(args[i]));
            }
        }
        // And what the call answers, for a letter only the result and a block
        // say: `traverse(xs) { |s| s.to(Integer) }` answers an `[Integer]?`, so
        // its `F<[B]>` is an Optional.
        if (const auto& returns = call.callee->clauses.front().returnAnnotation;
            returns && *returns) {
            match(**returns, typeOf(call.expr));
        }
        std::vector<TypeArgument> result;
        for (const auto& letter : wanted) {
            TypeArgument argument{letter, nullptr, {}};
            if (const auto it = found.find(letter); it != found.end()) {
                argument.type = it->second;
                // The caller's own letter, handed on.
                if (const auto* bound = std::get_if<ConstrainedType>(&it->second->kind);
                    bound && std::find(
                                 letters[call.caller].begin(), letters[call.caller].end(),
                                 bound->varName) != letters[call.caller].end()) {
                    argument.via = bound->varName;
                }
            }
            result.push_back(std::move(argument));
        }
        return result;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& call : calls) {
            if (!needs.contains(call.callee) || needs.contains(call.caller)) {
                continue;
            }
            for (const auto& argument : argumentsAt(call)) {
                if (!argument.via.empty()) {
                    needs.insert(call.caller);
                    changed = true;
                    break;
                }
            }
        }
    }
    for (const auto* def : needs) {
        m_typeParameters[def] = letters[def];
    }
    for (const auto& call : calls) {
        if (needs.contains(call.callee)) {
            m_typeArguments[call.expr] = argumentsAt(call);
        }
    }
    // A call of another unit's function: its interface's parameter types,
    // where a bounded letter reads back as a bound named by it.
    for (const auto& call : importedCalls) {
        // What the call bound each letter to, as its selection recorded.
        std::unordered_map<std::string, TypePtr> found;
        if (const auto bindings = m_importedTypeBindings.find(call.expr);
            bindings != m_importedTypeBindings.end()) {
            for (const auto& [letter, type] : bindings->second) {
                if (type) {
                    found.emplace(letter, resolve(type));
                }
            }
        }
        const auto& wanted = call.callee->typeParameters;
        std::vector<TypeArgument> passed;
        for (const auto& letter : wanted) {
            TypeArgument argument{letter, nullptr, {}};
            if (const auto it = found.find(letter); it != found.end()) {
                argument.type = it->second;
                if (const auto* bound = std::get_if<ConstrainedType>(&it->second->kind);
                    bound && call.caller &&
                    std::find(
                        letters[call.caller].begin(), letters[call.caller].end(), bound->varName) !=
                        letters[call.caller].end()) {
                    argument.via = bound->varName;
                }
            }
            passed.push_back(std::move(argument));
        }
        m_typeArguments[call.expr] = std::move(passed);
    }
    for (const auto& [expr, passed] : m_typeArguments) {
        if (const auto* method = std::get_if<ast::MethodCall>(&expr->kind)) {
            m_methodTypeArguments[method] = passed;
        }
    }
}

auto TypeChecker::unannotatedFunction(const std::string& name) -> const ast::FunctionDef* {
    if (!m_checkedProgram) {
        return nullptr;
    }
    if (!m_unannotatedFunctionsReady) {
        // A program's own top-level function of one clause, with no written
        // result and a plain parameter left unannotated, the only function of
        // its name: what decision 1 checks at each call.
        m_unannotatedFunctionsReady = true;
        for (const auto& item : m_checkedProgram->items) {
            const auto* owned = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
            if (!owned || !*owned) {
                continue;
            }
            const auto& def = **owned;
            const auto signatures = m_userSignatures.find(def.name);
            if (def.clauses.size() != 1 || def.clauses[0].returnAnnotation || def.isSlot ||
                signatures == m_userSignatures.end() || signatures->second.size() != 1 ||
                m_scopedDeclaredSignatures.contains("\n" + def.name)) {
                continue;
            }
            const auto& params = def.clauses[0].params;
            const bool plain = std::all_of(params.begin(), params.end(), [](const ast::Param& p) {
                return p.name && !p.pattern && !p.defaultValue;
            });
            const bool open = std::any_of(
                params.begin(), params.end(), [](const ast::Param& p) { return !p.type; });
            if (plain && open) {
                m_unannotatedFunctions.emplace(def.name, &def);
            }
        }
    }
    const auto found = m_unannotatedFunctions.find(name);
    return found == m_unannotatedFunctions.end() ? nullptr : found->second;
}

auto TypeChecker::settledArguments(
    const ast::FunctionDef& def, const std::vector<TypePtr>& argTypes)
    -> std::optional<std::vector<TypePtr>> {
    const auto& params = def.clauses[0].params;
    if (argTypes.size() != params.size()) {
        return std::nullopt;
    }
    // A parameter the first check settled (`x + "s"` makes `x` a String) is
    // checked at the call like a written one; an argument that does not fit
    // it is that check's error, not the body's.
    const auto& checkedParams = m_userSignatures.at(def.name).front().params;
    std::vector<TypePtr> arguments;
    for (std::size_t index = 0; index < argTypes.size(); ++index) {
        auto type = argTypes[index] ? resolve(argTypes[index]) : nullptr;
        if (!type || !isConcreteValue(type) || containsTypeVariable(type) ||
            std::holds_alternative<UnionType>(type->kind)) {
            return std::nullopt;
        }
        if (index < checkedParams.size()) {
            const auto param = resolve(checkedParams[index]);
            if (!containsOpenType(param) && !argMatchesParam(type, param)) {
                return std::nullopt;
            }
        }
        arguments.push_back(std::move(type));
    }
    return arguments;
}

auto TypeChecker::checkBodyAt(const ast::FunctionDef& def, const std::vector<TypePtr>& arguments)
    -> BodyCheck {
    BodyCheck checked;
    auto copy = ast::clone(def);
    const auto& clause = copy->clauses[0];
    auto* savedDiagnostics = std::exchange(m_diagnostics, &checked.diagnostics);
    std::vector<int> journal;
    auto* savedJournal = std::exchange(m_substJournal, &journal);
    const bool savedRecordsNames = std::exchange(m_recordsNames, false);
    const auto savedClauseVarMark = std::exchange(m_clauseVarMark, m_nextTypeVar);
    std::vector<std::pair<SourceLocation, TypePtr>> tries;
    std::vector<std::pair<const ast::Expr*, TypePtr>> returns;
    auto* const savedTries = std::exchange(m_clauseTries, &tries);
    auto* const savedReturns = std::exchange(m_clauseReturns, &returns);
    // The body is checked where it is written, not where the call is: none
    // of the caller's locals, module or `make` block.
    auto savedScopes = std::exchange(m_scopeStack, {});
    auto savedImportScopes = std::exchange(m_importScopeStack, {});
    auto savedModule = std::exchange(m_currentModulePath, std::string{});
    const bool savedInMake = std::exchange(m_inMakeBlock, false);
    auto savedMakeType = std::exchange(m_currentMakeType, nullptr);
    auto savedRigid = std::exchange(m_rigidVars, {});
    auto savedBodyTypeVars = std::exchange(m_bodyTypeVars, {});
    auto savedImports = m_declarationImports;
    if (const auto imports = m_functionImports.find(&def); imports != m_functionImports.end()) {
        m_declarationImports = imports->second;
    }
    const auto marks = std::make_tuple(
        m_demandedCalls.size(), m_unresolvedMethods.size(), m_unresolvedMethodImports.size(),
        m_pendingCaptureCalls.size(), m_patternBindings.size(), m_bindingSites.size(),
        m_scopeContexts.size(), m_typeReferences.size(), m_recordKeys.size(),
        m_clauseCoverageGaps.size());

    pushScope();
    std::unordered_map<std::string, TypePtr> genericVars;
    for (std::size_t index = 0; index < clause.params.size(); ++index) {
        const auto& param = clause.params[index];
        defineVar(
            *param.name, !param.type || traitParameter(param)
                             ? arguments[index]
                             : resolveTypeExpr(**param.type, genericVars));
    }
    checked.type = resolvedThroughout(inferBody(clause.body));
    popScope();

    checked.calls.assign(m_demandedCalls.begin() + std::get<0>(marks), m_demandedCalls.end());
    m_demandedCalls.resize(std::get<0>(marks));
    m_unresolvedMethods.resize(std::get<1>(marks));
    m_unresolvedMethodImports.resize(std::get<2>(marks));
    m_pendingCaptureCalls.resize(std::get<3>(marks));
    m_patternBindings.resize(std::get<4>(marks));
    m_bindingSites.resize(std::get<5>(marks));
    m_scopeContexts.resize(std::get<6>(marks));
    m_typeReferences.resize(std::get<7>(marks));
    m_recordKeys.resize(std::get<8>(marks));
    m_clauseCoverageGaps.resize(std::get<9>(marks));
    forgetAnswers(*copy);
    for (const int id : journal) {
        m_subst.erase(id);
    }
    m_declarationImports = std::move(savedImports);
    m_bodyTypeVars = std::move(savedBodyTypeVars);
    m_rigidVars = std::move(savedRigid);
    m_currentMakeType = std::move(savedMakeType);
    m_inMakeBlock = savedInMake;
    m_currentModulePath = std::move(savedModule);
    m_importScopeStack = std::move(savedImportScopes);
    m_scopeStack = std::move(savedScopes);
    m_clauseTries = savedTries;
    m_clauseReturns = savedReturns;
    m_clauseVarMark = savedClauseVarMark;
    m_recordsNames = savedRecordsNames;
    m_substJournal = savedJournal;
    m_diagnostics = savedDiagnostics;
    m_callChecks.push_back(std::move(copy));
    return checked;
}

auto TypeChecker::traitParameter(const ast::Param& param) const -> bool {
    const auto* written =
        param.type && *param.type ? std::get_if<ast::TypeName>(&(*param.type)->kind) : nullptr;
    return written && written->parts.size() == 1 && m_traits.lookup(written->parts.front());
}

auto TypeChecker::traitResultFunction(const std::string& name) -> const ast::FunctionDef* {
    if (!m_checkedProgram) {
        return nullptr;
    }
    for (const auto& item : m_checkedProgram->items) {
        const auto* owned = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
        if (!owned || !*owned || (*owned)->name != name || (*owned)->clauses.size() != 1) {
            continue;
        }
        const auto& clause = (*owned)->clauses.front();
        const auto* result = clause.returnAnnotation && *clause.returnAnnotation
                                 ? std::get_if<ast::TypeName>(&(*clause.returnAnnotation)->kind)
                                 : nullptr;
        if (!result || result->parts.size() != 1 || !m_traits.lookup(result->parts.front())) {
            return nullptr;
        }
        // Several parameters of the result's trait (decision 4, §4.5 rule 7,
        // decided: checked per call). With one, the result is tied to it.
        std::size_t sharing = 0;
        for (const auto& param : clause.params) {
            const auto* written = param.type && *param.type
                                      ? std::get_if<ast::TypeName>(&(*param.type)->kind)
                                      : nullptr;
            if (written && written->dotted() == result->dotted()) {
                ++sharing;
            }
        }
        return sharing > 1 ? owned->get() : nullptr;
    }
    return nullptr;
}

auto TypeChecker::callResultAt(const std::string& name, const std::vector<TypePtr>& argTypes)
    -> TypePtr {
    if (const auto* bare = traitResultFunction(name)) {
        std::vector<TypePtr> arguments;
        std::string key = "\n" + name;
        for (const auto& argument : argTypes) {
            const auto type = argument ? resolve(argument) : nullptr;
            if (!type || !isConcreteValue(type) || containsTypeVariable(type)) {
                return nullptr;
            }
            key += "\n" + typeToString(type);
            arguments.push_back(type);
        }
        if (arguments.size() != bare->clauses.front().params.size()) {
            return nullptr;
        }
        if (const auto known = m_callResults.find(key); known != m_callResults.end()) {
            return known->second;
        }
        if (!m_bodiesInProgress.insert(key).second) {
            return nullptr;
        }
        auto checked = checkBodyAt(*bare, arguments);
        m_bodiesInProgress.erase(key);
        auto result =
            checked.type && isConcreteValue(checked.type) && !containsTypeVariable(checked.type)
                ? checked.type
                : nullptr;
        m_callResults.emplace(std::move(key), result);
        return result;
    }
    const auto* def = unannotatedFunction(name);
    if (!def) {
        return nullptr;
    }
    const auto arguments = settledArguments(*def, argTypes);
    if (!arguments) {
        return nullptr;
    }
    std::string key = name;
    for (const auto& argument : *arguments) {
        key += "\n" + typeToString(argument);
    }
    if (const auto known = m_callResults.find(key); known != m_callResults.end()) {
        return known->second;
    }
    // A function reached again while its body is being checked (recursion)
    // has no answer yet; the call keeps the one the signature gives.
    if (!m_bodiesInProgress.insert(key).second) {
        return nullptr;
    }
    auto checked = checkBodyAt(*def, *arguments);
    m_bodiesInProgress.erase(key);
    auto result =
        checked.type && isConcreteValue(checked.type) && !containsTypeVariable(checked.type)
            ? checked.type
            : nullptr;
    m_callResults.emplace(std::move(key), result);
    return result;
}

auto TypeChecker::checkUnannotatedCalls(const ast::Program& program) -> void {
    (void)program;
    std::set<std::string> checked;
    const auto said = [](const Diagnostic& diagnostic) {
        const auto& at = diagnostic.location;
        return std::string(at.file) + ":" + std::to_string(at.line) + ":" +
               std::to_string(at.column) + "\n" + diagnostic.message;
    };
    const auto place = [](const SourceLocation& at) {
        return std::string(at.file) + ":" + std::to_string(at.line) + ":" +
               std::to_string(at.column);
    };
    std::set<std::string> reported;
    // A call already rejected where it is written (an argument that does not
    // fit, a function its argument lacks) has its error; checking the body
    // with that argument would say the same thing again.
    std::vector<SourceLocation> rejectedAt;
    for (const auto& diagnostic : *m_diagnostics) {
        reported.insert(said(diagnostic));
        if (diagnostic.level == Diagnostic::Level::Error) {
            rejectedAt.push_back(diagnostic.location);
        }
    }
    // Calls a check finds in a body are checked in turn, up to a bound: a
    // recursive function called with ever larger types would not end.
    std::vector<DemandedCall> pending = m_demandedCalls;
    constexpr std::size_t kMaxChecks = 512;
    for (std::size_t next = 0; next < pending.size() && checked.size() < kMaxChecks; ++next) {
        const auto call = pending[next];
        const auto* def = unannotatedFunction(call.function);
        if (!def) {
            continue;
        }
        const auto within = [&](const SourceLocation& at) {
            if (at.file != call.location.file) {
                return false;
            }
            if (call.location.startOffset >= 0 && call.location.endOffset > 0 &&
                at.startOffset >= 0) {
                return at.startOffset >= call.location.startOffset &&
                       at.startOffset < call.location.endOffset;
            }
            return place(at) == place(call.location);
        };
        if (std::any_of(rejectedAt.begin(), rejectedAt.end(), within)) {
            continue;
        }
        const auto arguments = settledArguments(*def, call.argTypes);
        if (!arguments) {
            continue;
        }
        std::string key = def->name;
        for (const auto& argument : *arguments) {
            key += "\n" + typeToString(argument);
        }
        if (!checked.insert(key).second) {
            continue;
        }
        auto body = checkBodyAt(*def, *arguments);
        // What the copy's check found in its body that the first check did
        // not: those calls are checked next, and its errors are this call's.
        pending.insert(pending.end(), body.calls.begin(), body.calls.end());

        std::string passed;
        for (const auto& argument : *arguments) {
            passed += (passed.empty() ? "" : ", ") + typeToString(argument);
        }
        for (auto& diagnostic : body.diagnostics) {
            if (diagnostic.level != Diagnostic::Level::Error ||
                !reported.insert(said(diagnostic)).second) {
                continue;
            }
            diagnostic.notes.push_back(
                {call.location, "in this call of `" + def->name + "`, with " + passed});
            m_diagnostics->push_back(std::move(diagnostic));
        }
    }
}

auto TypeChecker::forgetAnswers(const ast::FunctionDef& copy) -> void {
    std::unordered_set<const ast::Expr*> nodes;
    std::unordered_set<const ast::MethodCall*> calls;
    std::function<void(const ast::Expr&)> collect = [&](const ast::Expr& expr) {
        nodes.insert(&expr);
        if (const auto* call = std::get_if<ast::MethodCall>(&expr.kind)) {
            calls.insert(call);
        }
        ast::forEachChild(expr, [&](const ast::ExprPtr& child) { collect(*child); });
    };
    for (const auto& clause : copy.clauses) {
        for (const auto& expr : clause.body) {
            if (expr) {
                collect(*expr);
            }
        }
    }
    const auto forget = [](auto& table, const auto& keys) {
        for (const auto* key : keys) {
            table.erase(key);
        }
    };
    forget(m_typeMap, nodes);
    forget(m_assignmentTargetTypes, nodes);
    forget(m_valueAssignments, nodes);
    forget(m_decidedCalls, nodes);
    forget(m_localDefinitions, nodes);
    forget(m_nameUses, nodes);
    forget(m_scopeNames, nodes);
    forget(m_namespaceRoots, nodes);
    forget(m_captureCalls, nodes);
    forget(m_calledModules, nodes);
    forget(m_selectedCallSignatures, nodes);
    forget(m_captureSignatures, nodes);
    forget(m_capturesIntoFoul, nodes);
    forget(m_calledFunctions, nodes);
    forget(m_referencedConstants, nodes);
    forget(m_floatLiterals, nodes);
    forget(m_typeVariableReceivers, nodes);
    forget(m_typeArguments, nodes);
    forget(m_importedTypeBindings, nodes);
    forget(m_successWrappers, nodes);
    forget(m_justArguments, nodes);
    forget(m_resolvedCalls, calls);
    forget(m_staticTypeOfCalls, calls);
    forget(m_methodTypeArguments, calls);
}

} // namespace kex::semantic

namespace kex::semantic {

// `List.wrap(1)`, `Optional.wrap(1)`: a requirement with no receiver of its own
// (Monad's `wrap : A -> F<A>`) of a trait over constructors, called on a type
// named by its constructor. It is that type's own function, at the type
// applied to fresh letters (`[A]` for List), so the call is checked, and
// answered, as `[A].wrap(1)` would be.
auto TypeChecker::namedConstructorCall(
    const std::string& name, const ast::Expr& expr, const ast::MethodCall& call) -> TypePtr {
    if (lookupVar(name) || m_bodyTypeVars.contains(name)) {
        return nullptr;
    }
    for (const auto& [_, trait] : m_traits.all()) {
        if (trait.paramKinds.empty() || trait.paramKinds[0] == 0) {
            continue;
        }
        const bool required = std::any_of(
            trait.requiredMethods.begin(), trait.requiredMethods.end(), [&](const auto& r) {
                return r.name == call.method && implicitReceivers(r) == 1 &&
                       requirementParams(r).size() == call.args.size();
            });
        if (!required) {
            continue;
        }
        const auto implementors = m_traits.implementorsOf(trait.name);
        if (std::find(implementors.begin(), implementors.end(), name) == implementors.end()) {
            continue;
        }
        std::size_t arity = trait.paramKinds[0];
        if (const auto local = m_declaredTypeArity.find(name); local != m_declaredTypeArity.end()) {
            arity = local->second;
        } else if (m_importedInterfaces) {
            if (const auto imported = m_importedInterfaces->typeArities.find(name);
                imported != m_importedInterfaces->typeArities.end()) {
                arity = imported->second;
            }
        }
        std::vector<TypePtr> letters;
        for (std::size_t i = 0; i < arity; ++i) {
            letters.push_back(freshTypeVar());
        }
        std::vector<TypePtr> argTypes{applyConstructor(Type::named(name), std::move(letters))};
        for (const auto& arg : call.args) {
            argTypes.push_back(arg ? inferExpr(*arg) : Type::unknown());
        }
        auto result = checkCall(call.method, argTypes, expr.location, true, &call, &expr);
        answerThroughTypeDispatcher(expr, call);
        return result;
    }
    return nullptr;
}

auto TypeChecker::answerThroughTypeDispatcher(const ast::Expr& expr, const ast::MethodCall& call)
    -> void {
    m_decidedCalls[&expr] = {CallKind::Dispatch, call.method};
    // A program defining its own keeps its dispatcher, which falls back to it.
    if (m_methodSignatures.contains(call.method) || !m_importedInterfaces) {
        return;
    }
    const auto providers = m_importedInterfaces->receiverFunctions.find(call.method);
    if (providers == m_importedInterfaces->receiverFunctions.end()) {
        return;
    }
    const int arity = static_cast<int>(call.args.size()) + 1;
    for (const auto& provider : providers->second) {
        if (provider.backendModule == names::kPreludeModule &&
            provider.backendFunction == call.method && provider.backendArity == arity) {
            m_resolvedCalls[&call] = dispatcherTarget(provider);
            return;
        }
    }
}

} // namespace kex::semantic
