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
                              const std::unordered_map<std::string, kex::semantic::TypePtr>* aliases = nullptr,
                              const std::unordered_set<std::string>* traits = nullptr)
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
            // Without these, an interface signature spelled `Atom` (or a
            // sized numeric) came back as NamedType while the typechecker's
            // own path builds a PrimitiveType/SizedIntType for the same name.
            // Same printed name, different kind: typesEqual said no, so a
            // signature was not equal to itself and overload dedupe reported
            // `m.get(1, "")` ambiguous against its own `Atom | Integer`.
            if (name == "Atom") return Type::atom();
            if (name == "Byte") return Type::byte();
            if (name == "Int8") return Type::int8();
            if (name == "Int16") return Type::int16();
            if (name == "Int32") return Type::int32();
            if (name == "Int64") return Type::int64();
            if (name == "UInt8") return Type::uint8();
            if (name == "UInt16") return Type::uint16();
            if (name == "UInt32") return Type::uint32();
            if (name == "UInt64") return Type::uint64();
            if (name == "Float32") return Type::float32();
            if (name == "Float64") return Type::float64();
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
            if (name == "Block" && args.size() == 1)
                return Type::func({}, args[0], true);
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
            return Type::func(
                {}, node.inner
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
// in its own file.
inline auto backfillExternalTraitDefaults(ast::Program& program) -> void {
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
            if (*fn) result.defaultMethods.push_back((*fn)->name);
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
    }

    std::unordered_set<std::string> traitNames;
    for (const auto& program : programs)
        for (const auto& item : program.items)
            collectTraitNames(item, traitNames);

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
                               const kex::semantic::TypePtr& selfType)
        -> std::optional<kex::semantic::Signature> {
        if (fn.clauses.empty()) return std::nullopt;
        const auto& clause = fn.clauses.front();
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
                                 const kex::semantic::TypePtr& selfType)
        -> kex::semantic::Signature {
        kex::semantic::Signature sig;
        sig.name = fn.name;
        sig.isFoul = fn.isFoul;
        sig.params.push_back(selfType ? selfType
                                      : kex::semantic::Type::unknown());
        const size_t arity =
            fn.clauses.empty() ? 0 : fn.clauses.front().params.size();
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
        ifaces.modules[mod].exports[sig.name].push_back(ifn);
        ifaces.receiverFunctions[sig.name].push_back(ifn);
    };

    for (auto& program : programs) {

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
                                if (auto sig = inlineSignature(**fn, selfType)) {
                                    std::vector<std::string> paramNames;
                                    for (const auto& param :
                                         (*fn)->clauses.front().params)
                                        paramNames.push_back(
                                            param.name ? *param.name : "");
                                    addReceiverSig(sourceModule, *sig,
                                                   std::move(paramNames));
                                }
                            } else if (!directBackendOwnership) {
                                addReceiverSig(sourceModule,
                                               shapeOnlySignature(**fn, selfType));
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

        auto collectTypeAlias = [&](const ast::TypeDef& td) {
            if (td.isDistinct && td.variants && td.variants->size() == 1) {
                std::unordered_map<std::string, kex::semantic::TypePtr> vars;
                auto backing = resolveSourceType(
                    *td.variants->front(), vars, &typeAliases);
                ifaces.distinctTypes[td.name] = {td.typeParams,
                                                 std::move(backing)};
                return;
            }
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
            adt.typeParamCount = td.typeParams.size();
            adt.typeParamNames = td.typeParams;
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
            const auto moduleName = parent.empty() ||
                    mod.name.starts_with(parent + ".")
                ? mod.name : parent + "." + mod.name;
            for (const auto& item : mod.body) {
                if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                    if (*ann) moduleAnnotated.insert(moduleName + "::" + (*ann)->name);
                if (const auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                    if (*nested) collectAnnotatedNames(**nested, moduleName);
            }
        };
        std::function<void(const ast::ModuleDef&, const std::string&)>
            collectModule =
            [&](const ast::ModuleDef& mod, const std::string& parent) {
            const auto moduleName = parent.empty() ||
                    mod.name.starts_with(parent + ".")
                ? mod.name : parent + "." + mod.name;
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
                        if (!moduleAnnotated.count(moduleName + "::" + (*fn)->name))
                            if (auto sig = inlineSignature(**fn, nullptr)) {
                                std::vector<std::string> paramNames;
                                for (const auto& param :
                                     (*fn)->clauses.front().params)
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
                        addModuleSig(moduleName, sig);
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
    // Importers need these to expand a field or signature spelled against a
    // defining module's alias, the same way the KexI path carries them.
    for (auto& [name, body] : typeAliases)
        ifaces.typeAliases.try_emplace(name, body);
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
                } else if (duplicate->paramNames.empty() &&
                           !function.paramNames.empty()) {
                    // Keep the compiled entry's backend ABI, but enrich it
                    // with source parameter labels needed by named arguments.
                    duplicate->paramNames = std::move(function.paramNames);
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
                    return existing.sourceName == function.sourceName &&
                        sameSignature(existing.signature, function.signature);
                });
            if (duplicate == destination.end())
                destination.push_back(std::move(function));
            else if (!function.backendModule.empty())
                *duplicate = std::move(function);
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
