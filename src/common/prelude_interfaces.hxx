#pragma once

#include "../beam/kexi_registry.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include "../semantic/imported_interfaces.hxx"
#include "prelude_loader.hxx"
#include "type_def_utils.hxx"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace kex {

inline auto preludeSourceHash(const std::vector<std::string>& files)
    -> kex::beam::Hash128 {
    auto canonicalFiles = files;
    std::sort(canonicalFiles.begin(), canonicalFiles.end(),
              [](const auto& left, const auto& right) {
                  return std::filesystem::path(left).filename().string() <
                         std::filesystem::path(right).filename().string();
              });
    std::vector<uint8_t> bytes;
    for (const auto& file : canonicalFiles) {
        const auto name = std::filesystem::path(file).filename().string();
        bytes.insert(bytes.end(), name.begin(), name.end());
        bytes.push_back(0);
        std::ifstream input(file, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot read standard-library source: " + file);
        std::vector<uint8_t> contents{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        bytes.insert(bytes.end(), contents.begin(), contents.end());
        bytes.push_back(0);
    }
    return kex::beam::computeContentHash(bytes);
}

// Load and validate the prebuilt `kex_prelude.beam` once per process. Every
// compiler consumer derives its view from this same immutable registry, so
// checking and lowering cannot observe independently loaded artifact states.
// The beam must have been produced by a matching `kex --build-prelude` run;
// throws if the artifact is missing or its KexI chunk is malformed.
inline auto preludeRegistry(const std::string& runtimeDir)
    -> const kex::beam::KexiRegistry& {
    static const auto cached = [&]() -> kex::beam::KexiRegistry {
        if (runtimeDir.empty()) return {};
        kex::beam::KexiRegistry registry;
        auto path =
            (std::filesystem::path{runtimeDir} / "kex_prelude.beam").string();
        auto errors = registry.loadUnit(path);
        if (!errors.empty())
            throw std::runtime_error("invalid prebuilt standard library: " +
                                     errors.front().message);
        const auto files = standardLibraryArtifactSourceFiles();
        if (files.empty())
            throw std::runtime_error(
                "invalid prebuilt standard library: source package is missing");
        const auto* unit = registry.getUnit(registry.lastLoadedEntryAtom());
        if (!unit || unit->modules.empty())
            throw std::runtime_error(
                "invalid prebuilt standard library: entry unit is missing");
        const auto entry = std::find_if(
            unit->modules.begin(), unit->modules.end(), [](const auto& module) {
                return module.chunk.metadata.role ==
                       kex::beam::KexiModuleRole::Entry;
            });
        if (entry == unit->modules.end() ||
            entry->chunk.sourceHash != preludeSourceHash(files))
            throw std::runtime_error(
                "invalid prebuilt standard library: source digest mismatch — "
                "rebuild the stdlib artifacts");
        return registry;
    }();
    return cached;
}

// Simplified AST→Type converter for the source fallback below.
// Handles the subset of types used in prelude annotations.
inline auto resolveSourceType(const ast::TypeExpr& expr,
                              std::unordered_map<std::string, kex::semantic::TypePtr>& vars,
                              const std::unordered_map<std::string, kex::semantic::TypePtr>* aliases = nullptr)
    -> kex::semantic::TypePtr {
    using Type = kex::semantic::Type;
    return std::visit([&](const auto& node) -> kex::semantic::TypePtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::TypeName>) {
            if (node.parts.empty()) return Type::unknown();
            const auto& name = node.parts.back();
            if (name == "Integer" || name == "Int") return Type::integer();
            if (name == "String") return Type::string();
            if (name == "Bool") return Type::boolean();
            if (name == "Char") return Type::charT();
            if (name == "Float") return Type::constrained("Float", "Float");
            if (name == "Number") return Type::constrained("Number", "Number");
            if (name == "Void") return Type::unit();
            if (name == "Any") return Type::unknown();
            if (name.size() == 1 && std::isupper(static_cast<unsigned char>(name[0]))) {
                auto it = vars.find(name);
                if (it != vars.end()) return it->second;
                auto tv = Type::typeVar(-(static_cast<int>(vars.size()) + 1));
                vars[name] = tv;
                return tv;
            }
            if (aliases) {
                if (auto it = aliases->find(name); it != aliases->end())
                    return it->second;
            }
            return Type::named(name);
        }
        else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::string name = node.name.parts.empty() ? "" : node.name.parts.back();
            std::vector<kex::semantic::TypePtr> args;
            for (const auto& a : node.args)
                if (a) args.push_back(resolveSourceType(*a, vars, aliases));
            if (name == "Optional" && args.size() == 1)
                return Type::optional(args[0]);
            if (name == "Map" && args.size() == 2)
                return Type::map(args[0], args[1]);
            if (name == "List" && args.size() == 1)
                return Type::list(args[0]);
            if (name == "Result" && args.size() == 2)
                return Type::named("Result", std::move(args));
            if (name == "Block" && args.size() == 1)
                return Type::func({}, args[0]);
            return Type::named(name, std::move(args));
        }
        else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            auto p = node.param ? resolveSourceType(*node.param, vars, aliases) : Type::unknown();
            auto r = node.result ? resolveSourceType(*node.result, vars, aliases) : Type::unknown();
            return Type::func({p}, r);
        }
        else if constexpr (std::is_same_v<T, ast::TupleType>) {
            std::vector<kex::semantic::TypePtr> elems;
            for (const auto& e : node.elements)
                if (e) elems.push_back(resolveSourceType(*e, vars, aliases));
            return Type::tuple(std::move(elems));
        }
        else if constexpr (std::is_same_v<T, ast::ListType>) {
            return Type::list(node.element ? resolveSourceType(*node.element, vars, aliases) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::MapType>) {
            return Type::map(
                node.key ? resolveSourceType(*node.key, vars, aliases) : Type::unknown(),
                node.value ? resolveSourceType(*node.value, vars, aliases) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return Type::optional(node.inner ? resolveSourceType(*node.inner, vars, aliases) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::UnionType>) {
            std::vector<kex::semantic::TypePtr> members;
            members.push_back(node.left
                ? resolveSourceType(*node.left, vars, aliases)
                : Type::unknown());
            members.push_back(node.right
                ? resolveSourceType(*node.right, vars, aliases)
                : Type::unknown());
            return std::make_shared<Type>(
                Type{kex::semantic::UnionType{std::move(members)}});
        }
        else { return Type::unknown(); }
    }, expr.kind);
}

// Uncurry a nested function type: A -> B -> C becomes (A, B) -> C.
// Applied recursively so block parameters like (X -> X -> Bool) become
// (X, X) -> Bool, matching the typechecker's expected representation.
inline auto uncurryFuncType(kex::semantic::TypePtr type)
    -> kex::semantic::TypePtr {
    using Type = kex::semantic::Type;
    auto* fn = std::get_if<kex::semantic::FuncType>(&type->kind);
    if (!fn) return type;
    std::vector<kex::semantic::TypePtr> allParams;
    auto cur = type;
    while (auto* f = std::get_if<kex::semantic::FuncType>(&cur->kind)) {
        for (const auto& p : f->params) allParams.push_back(uncurryFuncType(p));
        cur = f->result;
    }
    return Type::func(std::move(allParams), uncurryFuncType(cur));
}

// Decompose a curried function type A -> B -> C into params [A, B] and
// result C, suitable for a Signature.
inline auto flattenFunctionType(kex::semantic::TypePtr type,
                                std::vector<kex::semantic::TypePtr>& params)
    -> kex::semantic::TypePtr {
    if (auto* fn = std::get_if<kex::semantic::FuncType>(&type->kind)) {
        for (const auto& p : fn->params) params.push_back(uncurryFuncType(p));
        return flattenFunctionType(fn->result, params);
    }
    return type;
}

// Source-based fallback for builds without prebuilt BEAM artifacts (wasm).
// Parses prelude .kex sources, resolves type annotations into Signatures,
// and builds ImportedInterfaces with real type information.
inline auto sourceSemanticInterfaces(const std::vector<std::string>& sourceFiles,
                                     bool automaticImport,
                                     bool directBackendOwnership)
    -> kex::semantic::ImportedInterfaces {
    kex::semantic::ImportedInterfaces ifaces;

    std::unordered_map<std::string, kex::semantic::TypePtr> typeAliases;
    // `make T, implement: Trait` claims, plus the constructors of every ADT
    // seen, so the claims can be expanded to constructors once all files are
    // read (a `make` block may precede the type it targets). Without this a
    // trait-typed param rejects values from an opt-in module — the KexI path
    // does the same expansion for prebuilt artifacts.
    std::vector<std::pair<std::string, std::string>> traitClaims;
    std::unordered_map<std::string, std::vector<std::string>> adtConstructors;

    auto backendModuleFor = [directBackendOwnership](const std::string& mod) {
        return directBackendOwnership && !mod.empty() ? "Kex." + mod : "";
    };

    auto annotationToSignature = [&typeAliases](const ast::TypeAnnotation& ann,
                                    kex::semantic::TypePtr selfType,
                                    std::unordered_map<std::string, kex::semantic::TypePtr> vars = {})
        -> kex::semantic::Signature {
        kex::semantic::Signature sig;
        sig.name = ann.name;
        sig.isFoul = ann.isFoul;
        if (!ann.type) { sig.result = kex::semantic::Type::unknown(); return sig; }
        auto resolved = resolveSourceType(*ann.type, vars, &typeAliases);
        if (ann.implicitThis && selfType)
            sig.params.push_back(selfType);
        sig.result = flattenFunctionType(resolved, sig.params);
        return sig;
    };

    auto addModuleSig = [&](const std::string& mod,
                            const kex::semantic::Signature& sig) {
        kex::semantic::ImportedFunction ifn;
        ifn.sourceName = sig.name;
        ifn.signature = sig;
        ifn.sourceModule = mod;
        if (directBackendOwnership) {
            ifn.backendModule = backendModuleFor(mod);
            ifn.backendFunction = sig.name;
            ifn.backendArity = static_cast<int>(sig.params.size());
        }
        ifaces.modules[mod].exports[sig.name].push_back(ifn);
    };
    auto addReceiverSig = [&](const std::string& mod,
                              const kex::semantic::Signature& sig) {
        kex::semantic::ImportedFunction ifn;
        ifn.sourceName = sig.name;
        ifn.signature = sig;
        ifn.sourceModule = mod;
        if (directBackendOwnership) {
            ifn.backendModule = backendModuleFor(mod);
            ifn.backendFunction = sig.name;
            ifn.backendArity = static_cast<int>(sig.params.size());
        }
        ifaces.modules[mod].exports[sig.name].push_back(ifn);
        ifaces.receiverFunctions[sig.name].push_back(ifn);
    };

    for (const auto& filePath : sourceFiles) {
        std::ifstream input(filePath);
        if (!input) continue;
        std::string src((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
        Lexer lexer(std::move(src), filePath);
        Parser parser(lexer.tokenizeAll(), filePath);
        auto program = parser.parseProgram();

        auto makeTargetName = [](const ast::MakeDef& make) -> std::string {
            if (!make.target) return "";
            if (const auto* tn = std::get_if<ast::TypeName>(&make.target->kind))
                return tn->parts.empty() ? "" : tn->parts.back();
            if (const auto* gt = std::get_if<ast::GenericType>(&make.target->kind))
                return gt->name.parts.empty() ? "" : gt->name.parts.back();
            if (const auto* lt = std::get_if<ast::ListType>(&make.target->kind))
                return "List";
            if (const auto* mt = std::get_if<ast::MapType>(&make.target->kind))
                return "Map";
            return "";
        };

        auto collectMakeAnnotations = [&](const ast::MakeDef& make,
                                          const std::string& owner = "") {
            auto typeName = makeTargetName(make);
            if (!typeName.empty())
                for (const auto& trait : make.implements)
                    traitClaims.push_back({typeName, trait});
            const auto sourceModule = owner.empty() ? typeName : owner;
            auto& module = ifaces.modules[sourceModule];
            module.sourceModule = sourceModule;
            module.backendModule = backendModuleFor(sourceModule);
            module.automaticImport = automaticImport;
            kex::semantic::TypePtr selfType;
            std::unordered_map<std::string, kex::semantic::TypePtr> tvars;
            if (make.target)
                selfType = resolveSourceType(*make.target, tvars);
            for (const auto& item : make.body) {
                if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                    if (*ann) addReceiverSig(sourceModule, annotationToSignature(**ann, selfType, tvars));
                if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
                    if (*fn) ifaces.modules[typeName].exports.try_emplace((*fn)->name);
                if (const auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item))
                    if (*vb) for (const auto& vi : (*vb)->items) {
                        if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&vi))
                            if (*ann) addReceiverSig(sourceModule, annotationToSignature(**ann, selfType, tvars));
                        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                            if (*fn) ifaces.modules[typeName].exports.try_emplace((*fn)->name);
                    }
            }
        };

        auto collectTypeAlias = [&](const ast::TypeDef& td) {
            if (kex::isTransparentTypeAlias(td)) {
                std::unordered_map<std::string, kex::semantic::TypePtr> noVars;
                auto resolved = resolveSourceType(*(*td.variants)[0], noVars, &typeAliases);
                if (!std::holds_alternative<kex::semantic::NamedType>(resolved->kind) ||
                    std::get<kex::semantic::NamedType>(resolved->kind).name != td.name)
                    typeAliases[td.name] = resolved;
            }
        };

        auto collectModuleConstructors = [&](const std::string& moduleName,
                                             const ast::TypeDef& td) {
            auto constructors = kex::typeConstructors(td);
            if (!constructors) return;
            // A module's ADTs need the same registration top-level ones get
            // from collectArities below — without it nothing knows `MB`
            // belongs to `DataUnit`, so passing it to a `DataUnit` param is
            // rejected.
            kex::semantic::ImportedADT adt;
            adt.name = td.name;
            ifaces.typeNames.insert(td.name);
            for (const auto& constructor : *constructors) {
                adt.constructors.push_back(constructor.name);
                adt.constructorArities[constructor.name] =
                    static_cast<int>(constructor.arity);
                adtConstructors[td.name].push_back(constructor.name);
                kex::semantic::ImportedFunction function;
                function.sourceName = constructor.name;
                function.sourceModule = moduleName;
                function.isConstructor = true;
                function.signature.name = constructor.name;
                function.signature.result =
                    kex::semantic::Type::named(constructor.name);
                if (td.variants)
                    for (const auto& variant : *td.variants) {
                        const auto* generic =
                            variant
                            ? std::get_if<ast::GenericType>(
                                  &variant->kind)
                            : nullptr;
                        if (!generic || generic->name.parts.empty() ||
                            generic->name.parts.back() != constructor.name)
                            continue;
                        std::unordered_map<std::string,
                                           kex::semantic::TypePtr>
                            vars;
                        for (const auto& arg : generic->args)
                            function.signature.params.push_back(
                                resolveSourceType(*arg, vars,
                                                  &typeAliases));
                        break;
                    }
                if (directBackendOwnership) {
                    function.backendModule =
                        backendModuleFor(moduleName);
                    function.backendFunction = constructor.name;
                    function.backendArity =
                        static_cast<int>(
                            function.signature.params.size());
                }
                ifaces.modules[moduleName]
                    .exports[constructor.name]
                    .push_back(std::move(function));
            }
            if (!adt.constructors.empty())
                ifaces.adts.push_back(std::move(adt));
        };

        std::function<void(const ast::ModuleDef&)> collectModule =
            [&](const ast::ModuleDef& mod) {
            ifaces.modules[mod.name].sourceModule = mod.name;
            ifaces.modules[mod.name].backendModule =
                backendModuleFor(mod.name);
            ifaces.modules[mod.name].automaticImport = automaticImport;
            ifaces.modules[mod.name].isFoul = mod.isFoul;
            for (const auto& item : mod.body) {
                if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
                    if (*fn) ifaces.modules[mod.name].exports.try_emplace((*fn)->name);
                if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                    if (*ann) {
                        auto sig = annotationToSignature(**ann, nullptr);
                        if (directBackendOwnership && !sig.params.empty())
                            addReceiverSig(mod.name, sig);
                        else
                            addModuleSig(mod.name, sig);
                    }
                if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&item))
                    if (*make) collectMakeAnnotations(**make, mod.name);
                if (const auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                    if (*nested) collectModule(**nested);
                if (const auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&item))
                    if (*td) {
                        collectTypeAlias(**td);
                        collectModuleConstructors(mod.name, **td);
                    }
                if (const auto* visibility =
                        std::get_if<std::unique_ptr<
                            ast::VisibilityBlock>>(&item);
                    visibility && *visibility &&
                    (*visibility)->isPublic)
                    for (const auto& visible :
                         (*visibility)->items) {
                        if (const auto* td =
                                std::get_if<std::unique_ptr<
                                    ast::TypeDef>>(&visible);
                            td && *td) {
                            collectTypeAlias(**td);
                            collectModuleConstructors(mod.name,
                                                      **td);
                        }
                    }
            }
        };

        // ADT constructor arities and record field counts, so a pattern that
        // destructures the wrong number of values is rejected here too. The
        // BEAM-artifact path gets these from KexI metadata; without collecting
        // them from source as well, the same program type-checks differently
        // on the wasm build, which has no prebuilt artifacts.
        auto collectArities = [&](const auto& item) {
            if (const auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&item)) {
                if (!*td) return;
                // Record the type's NAME before the constructor walk below:
                // an alias like `type List<X> = [X]` contributes no
                // constructors and so never reaches `ifaces.adts`, but `List`
                // is still a declared type name that resolves.
                ifaces.typeNames.insert((*td)->name);
                if (!(*td)->variants) return;
                // `type FilePath = String` is a transparent alias, not an ADT
                // whose constructor happens to be spelled `String` — treating
                // it as one made `String` a constructor of `FilePath`, so a
                // bare `String` widened to `FilePath`.
                if (kex::isTransparentTypeAlias(**td)) return;
                kex::semantic::ImportedADT adt;
                adt.name = (*td)->name;
                for (const auto& variant : *(*td)->variants) {
                    if (!variant) continue;
                    if (const auto* tn = std::get_if<ast::TypeName>(&variant->kind)) {
                        if (tn->parts.size() != 1) continue;
                        adt.constructors.push_back(tn->parts[0]);
                        adt.constructorArities[tn->parts[0]] = 0;
                    } else if (const auto* gt =
                                   std::get_if<ast::GenericType>(&variant->kind)) {
                        if (gt->name.parts.size() != 1) continue;
                        adt.constructors.push_back(gt->name.parts[0]);
                        adt.constructorArities[gt->name.parts[0]] =
                            static_cast<int>(gt->args.size());
                    }
                }
                if (adt.constructors.empty()) return;
                for (const auto& ctor : adt.constructors)
                    adtConstructors[adt.name].push_back(ctor);
                ifaces.adts.push_back(std::move(adt));
            } else if (const auto* rd =
                           std::get_if<std::unique_ptr<ast::RecordDef>>(&item)) {
                if (*rd) {
                    ifaces.recordArities[(*rd)->name] = (*rd)->fields.size();
                    ifaces.typeNames.insert((*rd)->name);
                }
            }
        };

        for (const auto& item : program.items) {
            collectArities(item);
            if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                if (*make) collectMakeAnnotations(**make);
            } else if (const auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*mod) collectModule(**mod);
            } else if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn) ifaces.receiverFunctions.try_emplace((*fn)->name);
            } else if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
                if (*ann) addReceiverSig("", annotationToSignature(**ann, nullptr));
            }
        }
    }

    // `make Animal, implement: Speaker` makes every Animal constructor a
    // Speaker too, since a nullary constructor's value type is its own name.
    for (const auto& [typeName, traitName] : traitClaims) {
        ifaces.traitConformances.push_back({typeName, traitName});
        if (auto constructors = adtConstructors.find(typeName);
            constructors != adtConstructors.end())
            for (const auto& constructor : constructors->second)
                ifaces.traitConformances.push_back({constructor, traitName});
    }
    return ifaces;
}

inline auto sourcePreludeSemanticInterfaces()
    -> kex::semantic::ImportedInterfaces {
    return sourceSemanticInterfaces(preludeSourceFiles(), true, false);
}

inline auto mergeSemanticInterfaces(kex::semantic::ImportedInterfaces base,
                                    kex::semantic::ImportedInterfaces extra)
    -> kex::semantic::ImportedInterfaces {
    for (auto& [name, module] : extra.modules) {
        auto [it, inserted] = base.modules.try_emplace(name, std::move(module));
        if (inserted) continue;
        for (auto& [exportName, functions] : module.exports) {
            auto& destination = it->second.exports[exportName];
            destination.insert(destination.end(),
                               std::make_move_iterator(functions.begin()),
                               std::make_move_iterator(functions.end()));
        }
    }
    for (auto& [name, functions] : extra.receiverFunctions) {
        auto& destination = base.receiverFunctions[name];
        destination.insert(destination.end(),
                           std::make_move_iterator(functions.begin()),
                           std::make_move_iterator(functions.end()));
    }
    base.traitConformances.insert(
        base.traitConformances.end(),
        std::make_move_iterator(extra.traitConformances.begin()),
        std::make_move_iterator(extra.traitConformances.end()));
    base.adts.insert(base.adts.end(),
                     std::make_move_iterator(extra.adts.begin()),
                     std::make_move_iterator(extra.adts.end()));
    for (auto& [name, arity] : extra.recordArities)
        base.recordArities.try_emplace(name, arity);
    base.typeNames.insert(extra.typeNames.begin(), extra.typeNames.end());
    base.traits.insert(base.traits.end(),
                       std::make_move_iterator(extra.traits.begin()),
                       std::make_move_iterator(extra.traits.end()));
    return base;
}

// Build the ImportedInterfaces snapshot from the prebuilt prelude beam in
// `runtimeDir`. Cached per process; safe to call from any thread after
// first construction. Falls back to source-based extraction when no
// prebuilt BEAM artifacts are available (e.g. the wasm build).
inline auto preludeSemanticInterfaces(const std::string& runtimeDir)
    -> const kex::semantic::ImportedInterfaces& {
    static const auto cached = [&]() -> kex::semantic::ImportedInterfaces {
        auto interfaces = runtimeDir.empty()
            ? sourcePreludeSemanticInterfaces()
            : preludeRegistry(runtimeDir).buildSemanticInterfaces();
        auto stdlib = sourceSemanticInterfaces(
            standardLibrarySourceFiles(), false, true);
        return mergeSemanticInterfaces(std::move(interfaces), std::move(stdlib));
    }();
    return cached;
}

} // namespace kex
