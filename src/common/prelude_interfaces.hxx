#pragma once

#include "../ast/clone.hxx"
#include "../ast/function_groups.hxx"
#include "file_io.hxx"
#include "mangle.hxx"
#include "../beam/interface_cache.hxx"
#include "../cache/cache.hxx"
#include "digest.hxx"
#include "timings.hxx"
#include "../beam/kexi_registry.hxx"
#include "../compiled/expand.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include "../semantic/imported_interfaces.hxx"
#include "prelude_loader.hxx"
#include "type_def_utils.hxx"
#include <filesystem>
#include <fstream>
#include <functional>
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
        const auto contents = readFileBytes(file);
        if (!contents)
            throw std::runtime_error("cannot read standard-library source: " + file);
        bytes.insert(bytes.end(), contents->begin(), contents->end());
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
                              const std::unordered_map<std::string, kex::semantic::TypePtr>* aliases = nullptr,
                              const std::unordered_set<std::string>* traits = nullptr)
    -> kex::semantic::TypePtr {
    using Type = kex::semantic::Type;
    return std::visit([&](const auto& node) -> kex::semantic::TypePtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::TypeName>) {
            if (node.parts.empty()) return Type::unknown();
            const auto& name = node.parts.back();
            // The checker's own table: a signature spelled `Atom` (or a sized
            // numeric) that came back as NamedType while the typechecker
            // built a PrimitiveType/SizedIntType for the same name was not
            // equal to itself, and overload dedupe reported `m.get(1, "")`
            // ambiguous against its own `Atom | Integer`.
            if (auto builtin = kex::semantic::signatureBuiltinType(name))
                return builtin;
            if (name == "Any") return Type::unknown();
            if (name.size() == 1 && std::isupper(static_cast<unsigned char>(name[0]))) {
                auto it = vars.find(name);
                if (it != vars.end()) return it->second;
                auto tv = Type::typeVar(-(static_cast<int>(vars.size()) + 1));
                vars[name] = tv;
                return tv;
            }
            if (traits && traits->count(name))
                return Type::constrained(name, name);
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
                if (a) args.push_back(resolveSourceType(*a, vars, aliases, traits));
            if (name == "Optional" && args.size() == 1)
                return Type::optional(args[0]);
            if (name == "Map" && args.size() == 2)
                return Type::map(args[0], args[1]);
            if (name == "List" && args.size() == 1)
                return Type::list(args[0]);
            if (name == "Result" && args.size() == 2)
                return Type::named("Result", std::move(args));
            // `Block<R>`, or `Block<A, R>` for a block handed an `A`.
            if (name == "Block" && !args.empty()) {
                auto result = args.back();
                args.pop_back();
                return Type::func(std::move(args), std::move(result), true);
            }
            return Type::named(name, std::move(args));
        }
        else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            auto p = node.param ? resolveSourceType(*node.param, vars, aliases, traits) : Type::unknown();
            auto r = node.result ? resolveSourceType(*node.result, vars, aliases, traits) : Type::unknown();
            return Type::func({p}, r);
        }
        else if constexpr (std::is_same_v<T, ast::TupleType>) {
            std::vector<kex::semantic::TypePtr> elems;
            for (const auto& e : node.elements)
                if (e) elems.push_back(resolveSourceType(*e, vars, aliases, traits));
            return Type::tuple(std::move(elems));
        }
        else if constexpr (std::is_same_v<T, ast::ListType>) {
            return Type::list(node.element ? resolveSourceType(*node.element, vars, aliases, traits) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::MapType>) {
            return Type::map(
                node.key ? resolveSourceType(*node.key, vars, aliases, traits) : Type::unknown(),
                node.value ? resolveSourceType(*node.value, vars, aliases, traits) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return Type::optional(node.inner ? resolveSourceType(*node.inner, vars, aliases, traits) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::UnionType>) {
            std::vector<kex::semantic::TypePtr> members;
            members.push_back(node.left
                ? resolveSourceType(*node.left, vars, aliases, traits)
                : Type::unknown());
            members.push_back(node.right
                ? resolveSourceType(*node.right, vars, aliases, traits)
                : Type::unknown());
            return std::make_shared<Type>(
                Type{kex::semantic::UnionType{std::move(members)}});
        }
        else if constexpr (std::is_same_v<T, ast::IntersectionType>) {
            return Type::intersection({
                node.left
                    ? resolveSourceType(*node.left, vars, aliases, traits)
                    : Type::unknown(),
                node.right
                    ? resolveSourceType(*node.right, vars, aliases, traits)
                    : Type::unknown()});
        }
        else if constexpr (std::is_same_v<T, ast::RecordType>) {
            std::vector<std::pair<std::string, kex::semantic::TypePtr>> fields;
            for (const auto& [name, fieldType] : node.fields)
                fields.emplace_back(
                    name, fieldType
                        ? resolveSourceType(*fieldType, vars, aliases, traits)
                        : Type::unknown());
            return Type::record(std::move(fields));
        }
        else if constexpr (std::is_same_v<T, ast::AtomType>) {
            // Without this an atom literal in an interface signature came back
            // as Unknown, so `:macos | :linux` printed as "? | ?".
            return Type::atom(node.name);
        }
        else if constexpr (std::is_same_v<T, ast::BlockType>) {
            std::vector<kex::semantic::TypePtr> params;
            for (const auto& param : node.params)
                params.push_back(
                    param ? resolveSourceType(*param, vars, aliases, traits)
                          : Type::unknown());
            return Type::func(
                std::move(params),
                node.inner
                    ? resolveSourceType(*node.inner, vars, aliases, traits)
                    : Type::unknown(),
                true);
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
    if (fn->block)
        return Type::func({}, uncurryFuncType(fn->result), true);
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
// Trait names declared anywhere in a parsed program, modules included. The
// top-level and module-body item variants are distinct types with the same
// two alternatives that matter here, so this is written once as a template.
template <typename Item>
inline auto collectTraitNames(const Item& item,
                              std::unordered_set<std::string>& out) -> void {
    if (const auto* trait = std::get_if<std::unique_ptr<ast::TraitDef>>(&item)) {
        if (*trait) out.insert((*trait)->name);
    } else if (const auto* mod =
                   std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
        if (*mod)
            for (const auto& nested : (*mod)->body) collectTraitNames(nested, out);
    }
}

// Trait names CLAIMED (`make T, implement: Trait`) anywhere in a parsed
// program, modules included. Mirrors `collectTraitNames` above.
template <typename Item>
inline auto collectImplementedTraitNames(const Item& item,
                                         std::unordered_set<std::string>& out)
    -> void {
    if (const auto* mk = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
        if (*mk)
            for (const auto& name : (*mk)->implements) out.insert(name);
    } else if (const auto* mod =
                   std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
        if (*mod)
            for (const auto& nested : (*mod)->body)
                collectImplementedTraitNames(nested, out);
    }
}

// BEAM lowering splices a trait's default methods into any type that claims
// it (`make T, implement: Trait`) by finding the trait's `trait ... do ...
// end` AST locally in the program being compiled (lower.cxx's
// `inheritedDefaults`). A prelude-tier type gets this for free: the claim and
// the trait declaration live in the same compiled prelude program. An
// OPT-IN, SOURCE-LOADED module (`using Data.Set`) is compiled as its own
// unit against the prelude's precompiled SIGNATURES only (`ExternalModules`
// carries name/arity, never a default method's body) — so a type claiming a
// prelude trait purely through inherited defaults, with no local override,
// had no source to splice `any?`/`all?`/etc. from, and died `Undefined
// method` on BEAM only (the tree-walk interpreter re-parses the prelude
// wholesale and never hits this gap) (kexhq/kex#235).
//
// Fixed generically, not by special-casing Enumerable/Foldable: whenever the
// assembled program claims a trait it does not itself declare, search the
// prelude's own source files for that trait's declaration and merge just the
// trait node in — the same node a prelude-tier type would already have found
// in its own file. A unit compiled apart claims traits of the units below it
// too (`implement: Shop.Rules.Pricing`): those are cloned from `below`, found
// by the qualified name the claim wrote, since their programs keep them.
inline auto backfillExternalTraitDefaults(ast::Program& program,
                                          const std::vector<const ast::Program*>& below = {})
    -> void {
    std::unordered_set<std::string> implemented, declared;
    for (const auto& item : program.items) {
        collectImplementedTraitNames(item, implemented);
        collectTraitNames(item, declared);
    }
    std::vector<std::string> missing;
    for (const auto& name : implemented)
        if (!declared.count(name)) missing.push_back(name);
    if (missing.empty()) return;
    std::sort(missing.begin(), missing.end());

    std::function<void(const ast::ModuleDef&, const std::string&)> fromModule =
        [&](const ast::ModuleDef& module, const std::string& parent) {
            const auto path = ast::nestedModulePath(parent, module.name);
            for (const auto& item : module.body) {
                if (const auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                    if (*nested) fromModule(**nested, path);
                    continue;
                }
                const auto* trait = std::get_if<std::unique_ptr<ast::TraitDef>>(&item);
                if (!trait || !*trait) continue;
                const auto found =
                    std::find(missing.begin(), missing.end(), path + "." + (*trait)->name);
                if (found == missing.end()) continue;
                auto copy = ast::clone(**trait);
                copy->name = *found;
                program.items.push_back(std::move(copy));
                missing.erase(found);
            }
        };
    for (const auto* unit : below)
        for (const auto& item : unit->items)
            if (const auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
                module && *module)
                fromModule(**module, "");
    if (missing.empty()) return;

    for (const auto& filePath : preludeSourceFiles()) {
        if (missing.empty()) break;
        std::ifstream input(filePath);
        if (!input) continue;
        std::string src((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
        Lexer lexer(std::move(src), filePath);
        Parser parser(lexer.tokenizeAll(), filePath);
        auto fileProgram = parser.parseProgram();
        if (!parser.diagnostics().empty()) continue;
        for (auto& item : fileProgram.items) {
            auto* trait = std::get_if<std::unique_ptr<ast::TraitDef>>(&item);
            if (!trait || !*trait) continue;
            auto found = std::find(missing.begin(), missing.end(),
                                   (*trait)->name);
            if (found == missing.end()) continue;
            program.items.push_back(std::move(item));
            missing.erase(found);
        }
    }
}

// A trait as the checker needs it: the signatures it REQUIRES, and the names
// of the methods it supplies a default body for. Without the latter, a type
// that claims the trait (`make Range, implement: Foldable`) looks like it has
// no `each` at all, because an inherited default is registered nowhere else.
inline auto importedTraitFromSource(const ast::TraitDef& trait)
    -> kex::semantic::TraitDef {
    kex::semantic::TraitDef result;
    result.name = trait.name;
    for (const auto& member : trait.body) {
        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&member)) {
            if (*fn) {
                result.defaultMethods.push_back((*fn)->name);
                result.defaultBodies.push_back((*fn)->name);
            }
        } else if (const auto* ann =
                       std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&member)) {
            if (*ann) {
                kex::semantic::Signature sig;
                sig.name = (*ann)->name;
                sig.isFoul = (*ann)->isFoul;
                result.requiredMethods.push_back(std::move(sig));
            }
        }
    }
    return result;
}

inline auto resolveSharedNullaryConstructors(kex::semantic::ImportedInterfaces& interfaces)
    -> void {
    // Unique constructors retain their refinement (FS.Read, for example).
    // Shared names such as Timeout need their ADT owner to distinguish a
    // networking error from a server-call error. Run after interface merges
    // too: the two owners may come from different source/artifact sets.
    std::unordered_map<std::string, std::unordered_set<std::string>> owners;
    for (const auto& adt : interfaces.adts)
        for (const auto& constructor : adt.constructors)
            owners[constructor].insert(adt.name);
    for (auto& [_, module] : interfaces.modules)
        for (auto& [name, functions] : module.exports) {
            if (owners[name].size() < 2) continue;
            for (auto& function : functions)
                if (function.isConstructor && function.signature.params.empty() &&
                    !function.constructorOwner.empty())
                    function.signature.result =
                        kex::semantic::Type::named(function.constructorOwner);
        }
}

inline auto sourceSemanticInterfaces(const std::vector<std::string>& sourceFiles,
                                     bool automaticImport,
                                     bool directBackendOwnership)
    -> kex::semantic::ImportedInterfaces {
    kex::semantic::ImportedInterfaces ifaces;

    // Parsed up front, because a signature cannot be resolved correctly until
    // EVERY trait name is known: `each : Foldable -> ...` in list.kex names a
    // trait declared in enumerable.kex, and resolving it before that file is
    // read would yield a nominal type named "Foldable" that no receiver
    // matches. Tier order does not save us — traits are referenced across
    // tiers in both directions.
    std::vector<ast::Program> programs;
    programs.reserve(sourceFiles.size());
    for (const auto& filePath : sourceFiles) {
        std::ifstream input(filePath);
        if (!input) continue;
        std::string src((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
        Lexer lexer(std::move(src), filePath);
        Parser parser(lexer.tokenizeAll(), filePath);
        programs.push_back(parser.parseProgram());
        // Generated declarations are part of the public interface too. The
        // BEAM pre-check reads this table before loading dependency bodies.
        std::vector<semantic::Diagnostic> diagnostics;
        compiled::ExpandOptions options;
        options.sourcePath = filePath;
        if (!compiled::expand(programs.back(), diagnostics, options)) {
            const auto reason = diagnostics.empty()
                ? std::string("unknown expansion error")
                : diagnostics.front().message;
            throw std::runtime_error("cannot expand standard-library interface " +
                                     filePath + ": " + reason);
        }
    }

    std::unordered_set<std::string> traitNames;
    for (const auto& program : programs)
        for (const auto& item : program.items)
            collectTraitNames(item, traitNames);

    // Every function declared anywhere in the parsed sources, PRIVATE ones
    // included: `type Handler = Type.of(handlerType)` (net/http.kex) names a
    // `private do ... end` helper that exists only to give the alias a shape,
    // and the rest of this builder otherwise never looks inside a private
    // block at all. Keyed by simple name, mirroring how the real
    // TypeChecker's own `m_userSignatures`/`namedFunctionSignature` resolve a
    // bare `Type.of(name)` argument — see its own doc comment for why a
    // multi-candidate name is left unresolved rather than guessed at.
    std::unordered_map<std::string, std::vector<const ast::FunctionDef*>>
        functionDefsByName;
    auto recordFunctionDef = [&](const ast::FunctionDef& fn) {
        functionDefsByName[fn.name].push_back(&fn);
    };
    auto collectFunctionDefsFromVisibility =
        [&](const ast::VisibilityBlock& vb) {
        for (const auto& item : vb.items)
            if (const auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
                if (*fn) recordFunctionDef(**fn);
    };
    std::function<void(const ast::ModuleDef&)> collectFunctionDefsFromModule =
        [&](const ast::ModuleDef& mod) {
        for (const auto& item : mod.body) {
            if (const auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn) recordFunctionDef(**fn);
            } else if (const auto* nested =
                           std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*nested) collectFunctionDefsFromModule(**nested);
            } else if (const auto* vb =
                           std::get_if<std::unique_ptr<ast::VisibilityBlock>>(
                               &item)) {
                if (*vb) collectFunctionDefsFromVisibility(**vb);
            }
        }
    };
    for (const auto& program : programs)
        for (const auto& item : program.items) {
            if (const auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn) recordFunctionDef(**fn);
            } else if (const auto* mod =
                           std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*mod) collectFunctionDefsFromModule(**mod);
            }
        }

    std::unordered_map<std::string, kex::semantic::TypePtr> typeAliases;
    // `make T, implement: Trait` claims, plus the constructors of every ADT
    // seen, so the claims can be expanded to constructors once all files are
    // read (a `make` block may precede the type it targets). Without this a
    // trait-typed param rejects values from an opt-in module — the KexI path
    // does the same expansion for prebuilt artifacts.
    // Each claim keeps the module its `make` block sits in, since the
    // target's identity is only known once every type has been read.
    // `fullTypeName` keeps the target exactly as written: a multi-part
    // spelling is already an absolute identity and needs no lookup.
    struct TraitClaim {
        std::string owner;
        std::string typeName;
        std::string fullTypeName;
        std::string traitName;
    };
    std::vector<TraitClaim> traitClaims;
    std::unordered_map<std::string, std::vector<std::string>> adtConstructors;
    // Qualified identities of the ADTs declared inside a module
    // (`Kex.Backend`). A signature spelling `Backend` inside `module Kex`
    // means this type, and nothing named `Backend` anywhere else.
    std::unordered_set<std::string> moduleAdts;
    // The ADT a bare spelling means from inside `scope`: the innermost
    // enclosing module declaring it, or nothing.
    auto moduleAdtIn = [&moduleAdts](std::string scope,
                                     const std::string& name) -> std::string {
        while (!scope.empty()) {
            if (auto candidate = scope + "." + name; moduleAdts.count(candidate))
                return candidate;
            const auto dot = scope.rfind('.');
            if (dot == std::string::npos) break;
            scope.resize(dot);
        }
        return "";
    };

    auto backendModuleFor = [directBackendOwnership](const std::string& mod) {
        return directBackendOwnership && !mod.empty() ? "Kex." + mod : "";
    };

    auto annotationToSignature = [&](const ast::TypeAnnotation& ann,
                                    kex::semantic::TypePtr selfType,
                                    std::unordered_map<std::string, kex::semantic::TypePtr> vars = {})
        -> kex::semantic::Signature {
        kex::semantic::Signature sig;
        sig.name = ann.name;
        sig.isFoul = ann.isFoul;
        if (!ann.type) { sig.result = kex::semantic::Type::unknown(); return sig; }
        auto resolved = resolveSourceType(*ann.type, vars, &typeAliases, &traitNames);
        if (ann.implicitThis && selfType)
            sig.params.push_back(selfType);
        sig.result = flattenFunctionType(resolved, sig.params);
        return sig;
    };

    // `let parse(text: String) -> Result<Date, TimeError> do ... end` carries
    // a full signature without a standalone `parse : ...` annotation, and the
    // stdlib is written that way throughout. Read only what is written: a
    // parameter or result left unannotated stays Unknown, which the checker
    // treats as "contract not known" rather than as a claim.
    auto inlineSignature = [&](const ast::FunctionDef& fn,
                               const ast::FunctionClause& clause,
                               const kex::semantic::TypePtr& selfType)
        -> std::optional<kex::semantic::Signature> {
        const bool anyAnnotation =
            clause.returnAnnotation.has_value() ||
            std::any_of(clause.params.begin(), clause.params.end(),
                        [](const ast::Param& param) { return param.type.has_value(); });
        if (!anyAnnotation) return std::nullopt;
        std::unordered_map<std::string, kex::semantic::TypePtr> vars;
        kex::semantic::Signature sig;
        sig.name = fn.name;
        sig.isFoul = fn.isFoul;
        if (selfType) sig.params.push_back(selfType);
        for (const auto& param : clause.params)
            sig.params.push_back(
                param.type && *param.type
                    ? resolveSourceType(**param.type, vars, &typeAliases, &traitNames)
                    : kex::semantic::Type::unknown());
        auto required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue)
            --required;
        sig.requiredParams = required + (selfType ? 1 : 0);
        sig.result = clause.returnAnnotation && *clause.returnAnnotation
            ? resolveSourceType(**clause.returnAnnotation, vars, &typeAliases, &traitNames)
            : kex::semantic::Type::unknown();
        return sig;
    };

    auto addModuleSig = [&](const std::string& mod,
                            const kex::semantic::Signature& sig,
                            std::vector<std::string> paramNames = {}) {
        kex::semantic::ImportedFunction ifn;
        ifn.sourceName = sig.name;
        ifn.signature = sig;
        ifn.sourceModule = mod;
        ifn.paramNames = std::move(paramNames);
        if (directBackendOwnership) {
            ifn.backendModule = backendModuleFor(mod);
            ifn.backendFunction = sig.name;
            ifn.backendArity = static_cast<int>(sig.params.size()) +
                (sig.isFoul ? 1 : 0);
        }
        ifaces.modules[mod].exports[sig.name].push_back(ifn);
    };
    // `make Range do let sum = ... end` carries no annotation, so nothing
    // above records a signature — yet the method plainly exists on Range. A
    // shape-only signature (receiver known, parameters and result unknown)
    // says exactly that much and no more: `?` matches anything, so this can
    // never turn into a false mismatch of its own, while its absence made
    // `(1..3).sum` look like a call to List's `sum` with a Range receiver.
    auto shapeOnlySignature = [](const ast::FunctionDef& fn,
                                 const ast::FunctionClause& clause,
                                 const kex::semantic::TypePtr& selfType)
        -> kex::semantic::Signature {
        kex::semantic::Signature sig;
        sig.name = fn.name;
        sig.isFoul = fn.isFoul;
        sig.params.push_back(selfType ? selfType
                                      : kex::semantic::Type::unknown());
        const size_t arity = clause.params.size();
        for (size_t i = 0; i < arity; i++)
            sig.params.push_back(kex::semantic::Type::unknown());
        sig.result = kex::semantic::Type::unknown();
        return sig;
    };

    auto addReceiverSig = [&](const std::string& mod,
                              const kex::semantic::Signature& sig,
                              std::vector<std::string> paramNames = {}) {
        kex::semantic::ImportedFunction ifn;
        ifn.sourceName = sig.name;
        ifn.signature = sig;
        ifn.sourceModule = mod;
        ifn.paramNames = std::move(paramNames);
        if (directBackendOwnership) {
            ifn.backendModule = backendModuleFor(mod);
            ifn.backendFunction = sig.name;
            ifn.backendArity = static_cast<int>(sig.params.size()) +
                (sig.isFoul ? 1 : 0);
        }
        ifn.isReceiverMethod = true;
        ifaces.modules[mod].exports[sig.name].push_back(ifn);
        ifaces.receiverFunctions[sig.name].push_back(ifn);
    };

    for (auto& program : programs) {

        auto makeTargetName = [](const ast::MakeDef& make) {
            return makeTargetLeaf(make.target);
        };

        auto makeTargetSpelling = [](const ast::MakeDef& make) {
            const auto name = writtenMakeTarget(make.target);
            return name ? name->dotted() : std::string{};
        };

        auto collectMakeAnnotations = [&](const ast::MakeDef& make,
                                          const std::string& owner = "") {
            auto typeName = makeTargetName(make);
            if (!typeName.empty())
                for (const auto& trait : make.implements)
                    traitClaims.push_back(
                        {owner, typeName, makeTargetSpelling(make), trait});
            // `make Set<A> do ... end` sits directly in a file-header
            // module's body (`module Data`), not inside the nested
            // `module Set do ... end` that provides the qualified static
            // namespace — so a bare `owner` ("Data") would attribute EVERY
            // record's receiver methods under one shared file-level module,
            // colliding `Set`'s and `UnorderedSet`'s `count`/`map`/etc. and
            // making both invisible to a `using Data.Set` that names neither
            // "Data" nor the record's own bare name. When the record itself
            // was collected under `owner` (already qualified: `Data.Set`),
            // route the make block's receiver signatures there instead.
            const auto qualifiedTarget = owner.empty() || typeName.empty()
                ? std::string()
                : owner + "." + typeName;
            const bool targetIsOwnedRecord =
                !qualifiedTarget.empty() &&
                ifaces.recordArities.count(qualifiedTarget) > 0;
            const auto sourceModule = targetIsOwnedRecord ? qualifiedTarget
                : owner.empty() ? typeName : owner;
            auto& module = ifaces.modules[sourceModule];
            module.sourceModule = sourceModule;
            module.backendModule = backendModuleFor(sourceModule);
            module.automaticImport = automaticImport;
            kex::semantic::TypePtr selfType;
            std::unordered_map<std::string, kex::semantic::TypePtr> tvars;
            if (make.target)
                selfType = resolveSourceType(*make.target, tvars);
            std::unordered_set<std::string> annotatedMembers;
            for (const auto& item : make.body) {
                if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                    if (*ann) annotatedMembers.insert((*ann)->name);
                if (const auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item))
                    if (*vb) for (const auto& vi : (*vb)->items)
                        if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&vi))
                            if (*ann) annotatedMembers.insert((*ann)->name);
            }
            auto collectMakeMember = [&](const auto& member) {
                if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&member))
                    if (*ann) addReceiverSig(sourceModule, annotationToSignature(**ann, selfType, tvars));
                if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&member))
                    if (*fn) {
                        ifaces.modules[typeName].exports.try_emplace((*fn)->name);
                        // Only where this data is the ONLY data: the
                        // source-derived prelude. For opt-in modules the
                        // prelude's compiled interfaces already describe these
                        // receivers, and a second, vaguer provider of the same
                        // name reads as an ambiguous import (`peek` from both
                        // kex_prelude and Kex.Parsing).
                        // Shape-only on purpose, even when the method looks
                        // annotated: a `make` body also holds type-DIRECTED
                        // dispatch (`let to(String) -> String` in units.kex,
                        // where `String` is a type-valued argument, not a
                        // parameter type), and reading those as ordinary
                        // parameter annotations invents contracts that do not
                        // exist. Module-level functions have no such form.
                        if (!annotatedMembers.count((*fn)->name)) {
                            if (directBackendOwnership &&
                                sourceModule.find('.') != std::string::npos) {
                                for (const auto* clause :
                                     ast::signatureClauses(**fn)) {
                                    auto sig = inlineSignature(**fn, *clause, selfType);
                                    if (!sig) continue;
                                    std::vector<std::string> paramNames;
                                    for (const auto& param : clause->params)
                                        paramNames.push_back(
                                            param.name ? *param.name : "");
                                    addReceiverSig(sourceModule, *sig,
                                                   std::move(paramNames));
                                }
                            } else if (!directBackendOwnership) {
                                for (const auto* clause :
                                     ast::signatureClauses(**fn))
                                    addReceiverSig(sourceModule,
                                                   shapeOnlySignature(**fn, *clause, selfType));
                            }
                        }
                    }
            };
            for (const auto& item : make.body) {
                collectMakeMember(item);
                if (const auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item))
                    if (*vb) for (const auto& vi : (*vb)->items) collectMakeMember(vi);
            }
        };

        // `type Handler = Type.of(handlerType)`: the argument is ordinary Kex
        // code (a bare function reference here), not a type name, so this
        // resolves it against `functionDefsByName` rather than
        // `resolveSourceType`. Returns null when the argument isn't a bare
        // function reference or its name doesn't resolve to exactly one
        // declared function — the alias is then left unregistered, same as
        // before this case existed, rather than guessed at.
        auto resolveComputedAlias = [&](const ast::TypeQuery& query)
            -> kex::semantic::TypePtr {
            if (!query.argument) return nullptr;
            const auto* identifier =
                std::get_if<ast::Identifier>(&query.argument->kind);
            if (!identifier) return nullptr;
            auto candidates = functionDefsByName.find(identifier->name);
            if (candidates == functionDefsByName.end() ||
                candidates->second.size() != 1)
                return nullptr;
            std::unordered_map<std::string, kex::semantic::TypePtr> vars;
            const ast::FunctionDef& fn = *candidates->second.front();
            // Folded overloads are as ambiguous as separate functions.
            const auto declared = ast::signatureClauses(fn);
            if (declared.size() != 1) return nullptr;
            const auto& clause = *declared.front();
            std::vector<kex::semantic::TypePtr> params;
            for (const auto& param : clause.params)
                params.push_back(
                    param.type && *param.type
                        ? resolveSourceType(**param.type, vars, &typeAliases,
                                           &traitNames)
                        : kex::semantic::Type::unknown());
            auto result = clause.returnAnnotation && *clause.returnAnnotation
                ? resolveSourceType(**clause.returnAnnotation, vars,
                                    &typeAliases, &traitNames)
                : kex::semantic::Type::unknown();
            if (query.query == "returnedBy") return result;
            return kex::semantic::Type::func(std::move(params), result);
        };

        auto collectTypeAlias = [&](const ast::TypeDef& td) {
            if (td.isDistinct && td.variants && td.variants->size() == 1) {
                std::unordered_map<std::string, kex::semantic::TypePtr> vars;
                auto backing = resolveSourceType(
                    *td.variants->front(), vars, &typeAliases);
                ifaces.distinctTypes[td.name] = {td.typeParams,
                                                 std::move(backing)};
                return;
            }
            if (td.variants && td.variants->size() == 1 && !td.leadingPipe) {
                if (const auto* query = std::get_if<ast::TypeQuery>(
                        &(*td.variants)[0]->kind)) {
                    if (auto resolved = resolveComputedAlias(*query))
                        typeAliases[td.name] = resolved;
                    return;
                }
            }
            if (kex::isTransparentTypeAlias(td)) {
                std::unordered_map<std::string, kex::semantic::TypePtr> noVars;
                auto resolved = resolveSourceType(*(*td.variants)[0], noVars, &typeAliases);
                if (!std::holds_alternative<kex::semantic::NamedType>(resolved->kind) ||
                    std::get<kex::semantic::NamedType>(resolved->kind).name != td.name)
                    typeAliases[td.name] = resolved;
                return;
            }
            // Mirror TypeChecker::registerTypeAliases' final rule: a typedef
            // that is not an ADT — not every variant constructor-shaped — is
            // an alias even when its right-hand side is structural (`type
            // List<X> = [X]`) or a union of types. Without this the compile
            // path (interfaces only) never learns `List`/`Map` while the Run
            // path (sources parsed) does, so a `List<Integer>` annotation
            // went nominal and a static `List.first(x)` failed with
            // "Unknown module `List`" on BEAM only (risk #1). Type parameters
            // bind in declared order to the negative TypeVar slots
            // `substituteInterfaceGenerics` fills, exactly as `typeDefToType`.
            if (td.variants && !td.variants->empty()) {
                const auto ctors = kex::typeConstructors(td);
                if (ctors && !ctors->empty()) return; // ADT, not an alias
                std::unordered_map<std::string, kex::semantic::TypePtr> aliasVars;
                for (size_t i = 0; i < td.typeParams.size(); ++i)
                    aliasVars[td.typeParams[i]] =
                        kex::semantic::Type::typeVar(
                            -static_cast<int>(i + 1));
                std::vector<kex::semantic::TypePtr> parts;
                for (const auto& v : *td.variants)
                    if (v) parts.push_back(resolveSourceType(*v, aliasVars, &typeAliases));
                if (parts.empty()) {
                    typeAliases[td.name] = kex::semantic::Type::unknown();
                    return;
                }
                kex::semantic::TypePtr resolved = parts.size() == 1
                    ? parts[0]
                    : std::make_shared<kex::semantic::Type>(
                          kex::semantic::Type{kex::semantic::UnionType{
                              std::move(parts)}});
                if (!std::holds_alternative<kex::semantic::NamedType>(
                        resolved->kind) ||
                    std::get<kex::semantic::NamedType>(resolved->kind).name !=
                        td.name)
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
            const auto identity = moduleName + "." + td.name;
            kex::semantic::ImportedADT adt;
            adt.name = identity;
            adt.typeParamCount = td.typeParams.size();
            adt.typeParamNames = td.typeParams;
            // Both spellings are declared type NAMES, the way a record's are;
            // identity is decided by `adt.name`, not by this set.
            ifaces.typeNames.insert(td.name);
            ifaces.typeNames.insert(identity);
            moduleAdts.insert(identity);
            for (const auto& constructor : *constructors) {
                adt.constructors.push_back(constructor.name);
                adt.constructorArities[constructor.name] =
                    static_cast<int>(constructor.arity);
                adtConstructors[identity].push_back(constructor.name);
                kex::semantic::ImportedFunction function;
                function.sourceName = constructor.name;
                function.sourceModule = moduleName;
                function.isConstructor = true;
                function.signature.name = constructor.name;
                function.signature.result =
                    kex::semantic::Type::named(constructor.name);
                function.constructorOwner = identity;
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
                        for (size_t i = 0; i < td.typeParams.size(); ++i)
                            vars[td.typeParams[i]] =
                                kex::semantic::Type::typeVar(
                                    -(static_cast<int>(i) + 1));
                        for (const auto& arg : generic->args)
                            function.signature.params.push_back(arg
                                ? resolveSourceType(*arg, vars, &typeAliases)
                                : kex::semantic::Type::unknown());
                        adt.constructorParamTypes[constructor.name] =
                            function.signature.params;
                        for (const auto& arg : generic->args) {
                            int slot = -1;
                            if (arg) {
                                std::string variable;
                                if (const auto* named =
                                        std::get_if<ast::TypeName>(&arg->kind);
                                    named && named->parts.size() == 1)
                                    variable = named->parts.front();
                                else if (const auto* genericVar =
                                             std::get_if<ast::GenericVar>(&arg->kind))
                                    variable = genericVar->name;
                                if (auto parameter = std::find(td.typeParams.begin(),
                                                               td.typeParams.end(), variable);
                                    parameter != td.typeParams.end())
                                    slot = static_cast<int>(parameter - td.typeParams.begin());
                            }
                            adt.constructorTypeParamSlots[constructor.name].push_back(slot);
                        }
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

        std::unordered_set<std::string> moduleAnnotated;
        std::function<void(const ast::ModuleDef&, const std::string&)>
            collectAnnotatedNames =
            [&](const ast::ModuleDef& mod, const std::string& parent) {
            const auto moduleName = ast::nestedModulePath(parent, mod.name);
            for (const auto& item : mod.body) {
                if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                    if (*ann) moduleAnnotated.insert(kex::mangle::qualify(moduleName, (*ann)->name));
                if (const auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                    if (*nested) collectAnnotatedNames(**nested, moduleName);
            }
        };
        std::function<void(const ast::ModuleDef&, const std::string&)>
            collectModule =
            [&](const ast::ModuleDef& mod, const std::string& parent) {
            const auto moduleName = ast::nestedModulePath(parent, mod.name);
            collectAnnotatedNames(mod, parent);
            ifaces.modules[moduleName].sourceModule = moduleName;
            ifaces.modules[moduleName].backendModule =
                backendModuleFor(moduleName);
            ifaces.modules[moduleName].automaticImport = automaticImport;
            ifaces.modules[moduleName].isCapability = mod.isCapability;
            // `foul` is written on the definition, never on the annotation
            // above it — `foul name : T` is the foul VALUE-binding form, so
            // an arrow signature cannot carry the marker. An annotated
            // function would otherwise export as pure and its effect would be
            // invisible to every importer.
            std::unordered_set<std::string> foulDefs;
            for (const auto& item : mod.body)
                if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
                    if (*fn && (*fn)->isFoul) foulDefs.insert((*fn)->name);
            for (const auto& item : mod.body) {
                if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
                    if (*fn) {
                        ifaces.modules[moduleName].exports.try_emplace((*fn)->name);
                        if (!moduleAnnotated.count(kex::mangle::qualify(moduleName, (*fn)->name)))
                            for (const auto* clause : ast::signatureClauses(**fn)) {
                                auto sig = inlineSignature(**fn, *clause, nullptr);
                                if (!sig) continue;
                                std::vector<std::string> paramNames;
                                for (const auto& param : clause->params)
                                    paramNames.push_back(
                                        param.name ? *param.name : "");
                                addModuleSig(moduleName, *sig, paramNames);
                                if (directBackendOwnership &&
                                    !sig->params.empty()) {
                                    if (auto* named = std::get_if<
                                            kex::semantic::NamedType>(
                                            &sig->params.front()->kind)) {
                                        const auto qualified =
                                            moduleName + "." + named->name;
                                        if (ifaces.typeNames.count(qualified) > 0)
                                            named->name = qualified;
                                    }
                                    // The first parameter is the UFCS
                                    // receiver here, so the receiver
                                    // registration's param names exclude it —
                                    // matching backendArity's later
                                    // subtraction of the receiver slot when
                                    // ordering named arguments during BEAM
                                    // lowering.
                                    addReceiverSig(
                                        moduleName, *sig,
                                        std::vector<std::string>(
                                            paramNames.begin() + 1,
                                            paramNames.end()));
                                }
                            }
                    }
                if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                    if (*ann) {
                        auto sig = annotationToSignature(**ann, nullptr);
                        sig.isFoul = sig.isFoul || foulDefs.count(sig.name) > 0;
                        // The annotation supplies types; its definition supplies
                        // labels and defaults, also needed by check-only imports.
                        for (const auto& member : mod.body) {
                            const auto* definition = std::get_if<
                                std::unique_ptr<ast::FunctionDef>>(&member);
                            if (!definition || !*definition ||
                                (*definition)->name != sig.name) continue;
                            for (const auto& clause : (*definition)->clauses) {
                                if (clause.params.size() != sig.params.size()) continue;
                                sig.paramNames.clear();
                                for (const auto& param : clause.params)
                                    sig.paramNames.push_back(param.name.value_or(""));
                                auto required = clause.params.size();
                                while (required > 0 && clause.params[required - 1].defaultValue)
                                    --required;
                                sig.requiredParams = required;
                            }
                        }
                        // A first parameter naming a type declared by this
                        // nested companion has the companion's nominal
                        // identity at import sites. Preserve that identity so
                        // sibling operations such as TCP.accept and
                        // Unix.accept remain distinct during UFCS selection.
                        if (!sig.params.empty())
                            if (auto* named = std::get_if<
                                    kex::semantic::NamedType>(
                                    &sig.params.front()->kind)) {
                                const auto qualified = moduleName + "." + named->name;
                                if (ifaces.typeNames.count(qualified) > 0)
                                    named->name = qualified;
                            }
                        addModuleSig(moduleName, sig, sig.paramNames);
                        if (directBackendOwnership && !sig.params.empty())
                            addReceiverSig(moduleName, sig);
                    }
                if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&item))
                    if (*make) collectMakeAnnotations(**make, moduleName);
                if (const auto* record =
                        std::get_if<std::unique_ptr<ast::RecordDef>>(&item);
                    record && *record) {
                    const auto qualified = moduleName + "." + (*record)->name;
                    ifaces.recordArities[qualified] = (*record)->fields.size();
                    ifaces.recordArities.try_emplace(
                        (*record)->name, (*record)->fields.size());
                    auto& fields = ifaces.recordFieldNames[qualified];
                    std::unordered_map<std::string,
                                       kex::semantic::TypePtr> vars;
                    auto& fieldTypes = ifaces.recordFields[qualified];
                    for (size_t i = 0; i < (*record)->typeParams.size(); ++i)
                        vars[(*record)->typeParams[i]] =
                            kex::semantic::Type::typeVar(
                                -static_cast<int>(i + 1));
                    for (const auto& field : (*record)->fields) {
                        fields.insert(field.name);
                        fieldTypes[field.name] = field.type
                            ? resolveSourceType(*field.type, vars,
                                                &typeAliases, &traitNames)
                            : kex::semantic::Type::unknown();
                    }
                    ifaces.recordFieldNames.try_emplace((*record)->name,
                                                        fields);
                    ifaces.recordFields.try_emplace((*record)->name,
                                                    fieldTypes);
                    ifaces.typeNames.insert((*record)->name);
                    ifaces.typeNames.insert(qualified);
                }
                if (const auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                    if (*nested) collectModule(**nested, moduleName);
                if (const auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&item))
                    if (*td) {
                        collectTypeAlias(**td);
                        collectModuleConstructors(moduleName, **td);
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
                            collectModuleConstructors(moduleName,
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
                if ((*td)->isDistinct || kex::isTransparentTypeAlias(**td))
                    return;
                kex::semantic::ImportedADT adt;
                adt.name = (*td)->name;
                adt.typeParamCount = (*td)->typeParams.size();
                adt.typeParamNames = (*td)->typeParams;
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
                        std::unordered_map<std::string,
                                           kex::semantic::TypePtr> vars;
                        for (size_t i = 0; i < (*td)->typeParams.size(); ++i)
                            vars[(*td)->typeParams[i]] =
                                kex::semantic::Type::typeVar(
                                    -(static_cast<int>(i) + 1));
                        for (const auto& arg : gt->args) {
                            adt.constructorParamTypes[gt->name.parts[0]].push_back(
                                arg ? resolveSourceType(*arg, vars, &typeAliases)
                                    : kex::semantic::Type::unknown());
                            int slot = -1;
                            if (arg) {
                                std::string variable;
                                if (const auto* named =
                                        std::get_if<ast::TypeName>(&arg->kind);
                                    named && named->parts.size() == 1)
                                    variable = named->parts.front();
                                else if (const auto* genericVar =
                                             std::get_if<ast::GenericVar>(&arg->kind))
                                    variable = genericVar->name;
                                if (auto parameter = std::find((*td)->typeParams.begin(),
                                                               (*td)->typeParams.end(), variable);
                                    parameter != (*td)->typeParams.end())
                                    slot = static_cast<int>(parameter - (*td)->typeParams.begin());
                            }
                            adt.constructorTypeParamSlots[gt->name.parts[0]].push_back(slot);
                        }
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
                    auto& fieldNames = ifaces.recordFieldNames[(*rd)->name];
                    std::unordered_map<std::string,
                                       kex::semantic::TypePtr> vars;
                    auto& fieldTypes = ifaces.recordFields[(*rd)->name];
                    for (size_t i = 0; i < (*rd)->typeParams.size(); ++i)
                        vars[(*rd)->typeParams[i]] =
                            kex::semantic::Type::typeVar(
                                -static_cast<int>(i + 1));
                    for (const auto& field : (*rd)->fields) {
                        fieldNames.insert(field.name);
                        fieldTypes[field.name] = field.type
                            ? resolveSourceType(*field.type, vars,
                                                &typeAliases, &traitNames)
                            : kex::semantic::Type::unknown();
                    }
                    ifaces.typeNames.insert((*rd)->name);
                }
            }
        };

        for (const auto& item : program.items) {
            collectArities(item);
            if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                if (*make) collectMakeAnnotations(**make);
            } else if (const auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*mod) collectModule(**mod, {});
            } else if (const auto* trait =
                           std::get_if<std::unique_ptr<ast::TraitDef>>(&item)) {
                if (*trait) ifaces.traits.push_back(importedTraitFromSource(**trait));
            } else if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn) ifaces.receiverFunctions.try_emplace((*fn)->name);
            } else if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
                if (*ann) addReceiverSig("", annotationToSignature(**ann, nullptr));
            } else if (const auto* td = std::get_if<std::unique_ptr<ast::TypeDef>>(&item)) {
                // Top-level aliases are interface entries too: only typedefs
                // inside `module` blocks reached collectTypeAlias, so the
                // compile path never learned `type List<X> = [X]` (risk #1).
                // ADTs need no call here — collectArities above owns them.
                if (*td) collectTypeAlias(**td);
            }
        }
    }

    // `make Animal, implement: Speaker` makes every Animal constructor a
    // Speaker too, since a nullary constructor's value type is its own name.
    for (const auto& [owner, spelled, full, traitName] : traitClaims) {
        // A multi-part target spells its identity absolutely
        // (`make Net.Socket.Mock.TCP`), so it needs no module lookup. A
        // bare one names an ADT of an enclosing module — or, what the ADT
        // table never holds, a record of the module the `make` block sits
        // in. The KexI path covers that case by also claiming the
        // module-qualified spelling (kexhq/kex#143: `make Files` inside
        // `module Mock` conforms `Mock.Files`); without the same claim
        // here, `with Net.Socket.TCP = Mock.TCP { ... }` is rejected as
        // not implementing the capability it implements.
        const bool absolute = full.find('.') != std::string::npos;
        const auto owned = absolute ? full : moduleAdtIn(owner, spelled);
        const std::string typeName = owned.empty() ? spelled : owned;
        ifaces.traitConformances.push_back({typeName, traitName});
        if (!absolute && owned.empty() && !owner.empty())
            ifaces.traitConformances.push_back(
                {owner + "." + spelled, traitName});
        // Values of an absolutely-named type come in both spellings: a
        // module-path construction (`Mock.TCP { ... }`) types qualified,
        // while an imported signature types its results exactly as written
        // (`foul close(connection: TCPConnection)` yields bare
        // `TCPConnection`). The registry lookup is exact, so the claim must
        // cover both — bare-only used to be all there was, and qualified-only
        // broke every trait bound over socket handles.
        if (absolute && full != spelled)
            ifaces.traitConformances.push_back({spelled, traitName});
        if (auto constructors = adtConstructors.find(typeName);
            constructors != adtConstructors.end())
            for (const auto& constructor : constructors->second)
                ifaces.traitConformances.push_back({constructor, traitName});
    }
    // Importers need these to expand a field or signature spelled against a
    // defining module's alias, the same way the KexI path carries them.
    for (auto& [name, body] : typeAliases)
        ifaces.typeAliases.try_emplace(name, body);
    kex::semantic::qualifyModuleAdtNames(ifaces);
    resolveSharedNullaryConstructors(ifaces);
    return ifaces;
}

inline auto sourcePreludeSemanticInterfaces()
    -> kex::semantic::ImportedInterfaces {
    return sourceSemanticInterfaces(preludeSourceFiles(), true, false);
}

inline auto sameSignature(const kex::semantic::Signature& left,
                          const kex::semantic::Signature& right) -> bool {
    if (left.params.size() != right.params.size()) return false;
    for (size_t i = 0; i < left.params.size(); i++)
        if (!kex::semantic::typesEqual(left.params[i], right.params[i]))
            return false;
    return kex::semantic::typesEqual(left.result, right.result);
}

inline auto mergeSemanticInterfaces(kex::semantic::ImportedInterfaces base,
                                    kex::semantic::ImportedInterfaces extra)
    -> kex::semantic::ImportedInterfaces {
    for (auto& [name, module] : extra.modules) {
        auto [it, inserted] = base.modules.try_emplace(name, std::move(module));
        if (inserted) continue;
        for (auto& [exportName, functions] : module.exports) {
            auto& destination = it->second.exports[exportName];
            for (auto& function : functions) {
                const auto duplicate = std::find_if(
                    destination.begin(), destination.end(),
                    [&](const kex::semantic::ImportedFunction& existing) {
                        return existing.sourceName == function.sourceName &&
                            (sameSignature(existing.signature,
                                           function.signature) ||
                             (existing.backendFunction ==
                                  function.backendFunction &&
                              existing.signature.params.size() ==
                                  function.signature.params.size()));
                    });
                // The compiled interface owns backend ABI details such as
                // hidden protocol dictionaries. Source extraction augments
                // missing type shapes, but must not append a source-arity
                // duplicate that can later win overload selection.
                if (duplicate == destination.end()) {
                    destination.push_back(std::move(function));
                } else {
                    // Keep the compiled ABI and enrich the source call shape.
                    // KexI does not currently store default-argument counts.
                    if (duplicate->paramNames.empty() && !function.paramNames.empty())
                        duplicate->paramNames = std::move(function.paramNames);
                    // Only a compiled unit's interface writes its defaults
                    // down; the call that skips one needs them.
                    if (duplicate->defaultSources.empty())
                        duplicate->defaultSources = std::move(function.defaultSources);
                    if (function.signature.requiredParams)
                        duplicate->signature.requiredParams = function.signature.requiredParams;
                    if (function.isConstructor) {
                        duplicate->isConstructor = true;
                        duplicate->constructorOwner = function.constructorOwner;
                    }
                }
            }
        }
    }
    for (auto& [name, functions] : extra.receiverFunctions) {
        auto& destination = base.receiverFunctions[name];
        for (auto& function : functions) {
            // Both sides can describe the SAME function: a module compiled
            // into an entry unit is covered by that unit's interface AND by
            // its own companion interface. That is one function with two
            // backend spellings, not two providers — left as two, a call to
            // it is reported ambiguous against itself. The description that
            // owns the function wins: it routes to the module the function is
            // actually written in, and it carries that module as its source,
            // which is what `using` visibility is decided on.
            auto duplicate = std::find_if(
                destination.begin(), destination.end(),
                [&](const kex::semantic::ImportedFunction& existing) {
                    // A type's `make` function and another module's function
                    // of the same shape are two (`Team.create(by)` and
                    // `Teams.create(team, by)`).
                    return existing.sourceName == function.sourceName &&
                        existing.isReceiverMethod == function.isReceiverMethod &&
                        sameSignature(existing.signature, function.signature);
                });
            if (duplicate == destination.end())
                destination.push_back(std::move(function));
            else if (!function.backendModule.empty()) {
                // An opt-in provider must not hide the prelude's method on
                // a shared receiver type (for example Measure.kind).
                const auto provider = base.modules.find(duplicate->sourceModule);
                const bool ownsAutomaticMethod = !duplicate->backendModule.empty() &&
                    provider != base.modules.end() && provider->second.automaticImport;
                if (!ownsAutomaticMethod || duplicate->sourceModule == function.sourceModule)
                    *duplicate = std::move(function);
                else if (duplicate->defaultSources.empty())
                    duplicate->defaultSources = std::move(function.defaultSources);
            }
        }
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
    for (auto& [name, fields] : extra.recordFieldNames)
        base.recordFieldNames.try_emplace(name, std::move(fields));
    for (auto& [name, fields] : extra.recordFields)
        base.recordFields.try_emplace(name, std::move(fields));
    for (auto& [name, body] : extra.typeAliases)
        base.typeAliases.try_emplace(name, std::move(body));
    // And what each distinct type backs, without which none takes a literal
    // of its backing type or widens to it (`UserId` from another unit).
    for (auto& [name, distinct] : extra.distinctTypes)
        base.distinctTypes.try_emplace(name, std::move(distinct));
    base.typeNames.insert(extra.typeNames.begin(), extra.typeNames.end());
    base.traits.insert(base.traits.end(),
                       std::make_move_iterator(extra.traits.begin()),
                       std::make_move_iterator(extra.traits.end()));
    resolveSharedNullaryConstructors(base);
    return base;
}

// Build the ImportedInterfaces snapshot from the prebuilt prelude beam in
// `runtimeDir`. Cached per process; safe to call from any thread after
// first construction. Falls back to source-based extraction when no
// prebuilt BEAM artifacts are available (e.g. the wasm build).
inline auto preludeSemanticInterfaces(const std::string& runtimeDir)
    -> const kex::semantic::ImportedInterfaces& {
    static const auto cached = [&]() -> kex::semantic::ImportedInterfaces {
        // KEX_TIMINGS=1 splits this one-time load (kexhq/kex#323).
        const bool timed = kex::timings::enabled();
        kex::timings::PhaseTimer loadTimer(4, false);
        auto phase = [&](const char* name) { loadTimer.mark(name); };

        // The snapshot is a pure function of the stdlib sources AND the
        // compiler that derives it, so it is cached keyed by the toolchain
        // fingerprint, which covers both: every stdlib source (prelude and
        // opt-in, subdirectories included) and the compiler binary. A
        // same-version rebuild never serves the previous build's interfaces
        // (risk #2, the #372 class), and an edited opt-in module never
        // serves its old signature.
        std::optional<std::filesystem::path> cachePath;
        if (!runtimeDir.empty()) {
            if (const auto dir = kex::cache::directory("prelude-interface"))
                cachePath = *dir / (kex::sha256Hex(
                                        "kex-prelude-interface-v1\n" +
                                        kex::cache::toolchainFingerprint()) +
                                    ".bin");
        }
        const auto noteMiss = [&](const char* why) {
            if (timed) std::fprintf(stderr, "      [cache] miss: %s\n", why);
        };
        if (cachePath) {
            const auto bytes = readFileBytes(cachePath->string());
            if (!bytes) {
                noteMiss("absent");
            } else {
                // The key (stats of the stdlib files, see
                // toolchainFingerprint) plus this read, apart from the
                // deserialize below.
                phase("cache key + read");
                kex::beam::CacheMissReason reason =
                    kex::beam::CacheMissReason::Corrupt;
                if (auto loaded = kex::beam::deserializeImportedInterfaces(
                        *bytes, &reason)) {
                    phase("cache deserialize");
                    return std::move(*loaded);
                }
                noteMiss(kex::beam::cacheMissReasonName(reason));
            }
        }

        auto interfaces = [&]() -> kex::semantic::ImportedInterfaces {
            if (runtimeDir.empty()) return sourcePreludeSemanticInterfaces();
            const auto& registry = preludeRegistry(runtimeDir);
            return registry.buildSemanticInterfaces();
        }();
        phase("prebuilt KexI interfaces");
        auto stdlib = sourceSemanticInterfaces(
            standardLibrarySourceFiles(), false, true);
        phase("opt-in stdlib from source");
        auto merged =
            mergeSemanticInterfaces(std::move(interfaces), std::move(stdlib));

        if (cachePath) {
            auto bytes = kex::beam::serializeImportedInterfaces(merged);
            if (kex::cache::atomicWrite(
                    *cachePath,
                    std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                     bytes.size())))
                kex::beam::pruneInterfaceCacheDirectory(
                    cachePath->parent_path(), *cachePath);
        }
        return merged;
    }();
    return cached;
}

} // namespace kex
