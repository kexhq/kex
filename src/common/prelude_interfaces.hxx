#pragma once

#include "../beam/kexi_registry.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include "../semantic/imported_interfaces.hxx"
#include "prelude_loader.hxx"
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
        const auto files = preludeSourceFiles();
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
                              std::unordered_map<std::string, kex::semantic::TypePtr>& vars)
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
            if (name == "Void" || name == "Unit") return Type::unit();
            if (name == "Any") return Type::unknown();
            if (name.size() == 1 && std::isupper(static_cast<unsigned char>(name[0]))) {
                auto it = vars.find(name);
                if (it != vars.end()) return it->second;
                auto tv = Type::typeVar(-(static_cast<int>(vars.size()) + 1));
                vars[name] = tv;
                return tv;
            }
            return Type::named(name);
        }
        else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::string name = node.name.parts.empty() ? "" : node.name.parts.back();
            std::vector<kex::semantic::TypePtr> args;
            for (const auto& a : node.args)
                if (a) args.push_back(resolveSourceType(*a, vars));
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
            auto p = node.param ? resolveSourceType(*node.param, vars) : Type::unknown();
            auto r = node.result ? resolveSourceType(*node.result, vars) : Type::unknown();
            return Type::func({p}, r);
        }
        else if constexpr (std::is_same_v<T, ast::TupleType>) {
            std::vector<kex::semantic::TypePtr> elems;
            for (const auto& e : node.elements)
                if (e) elems.push_back(resolveSourceType(*e, vars));
            return Type::tuple(std::move(elems));
        }
        else if constexpr (std::is_same_v<T, ast::ListType>) {
            return Type::list(node.element ? resolveSourceType(*node.element, vars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::MapType>) {
            return Type::map(
                node.key ? resolveSourceType(*node.key, vars) : Type::unknown(),
                node.value ? resolveSourceType(*node.value, vars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return Type::optional(node.inner ? resolveSourceType(*node.inner, vars) : Type::unknown());
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
inline auto sourcePreludeSemanticInterfaces()
    -> kex::semantic::ImportedInterfaces {
    kex::semantic::ImportedInterfaces ifaces;

    auto annotationToSignature = [](const ast::TypeAnnotation& ann,
                                    kex::semantic::TypePtr selfType,
                                    std::unordered_map<std::string, kex::semantic::TypePtr> vars = {})
        -> kex::semantic::Signature {
        kex::semantic::Signature sig;
        sig.name = ann.name;
        sig.isFoul = ann.isFoul;
        if (!ann.type) { sig.result = kex::semantic::Type::unknown(); return sig; }
        auto resolved = resolveSourceType(*ann.type, vars);
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
        ifaces.modules[mod].exports[sig.name].push_back(ifn);
    };
    auto addReceiverSig = [&](const std::string& mod,
                              const kex::semantic::Signature& sig) {
        kex::semantic::ImportedFunction ifn;
        ifn.sourceName = sig.name;
        ifn.signature = sig;
        ifaces.modules[mod].exports[sig.name].push_back(ifn);
        ifaces.receiverFunctions[sig.name].push_back(ifn);
    };

    for (const auto& filePath : preludeSourceFiles()) {
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

        auto collectMakeAnnotations = [&](const ast::MakeDef& make) {
            auto typeName = makeTargetName(make);
            kex::semantic::TypePtr selfType;
            std::unordered_map<std::string, kex::semantic::TypePtr> tvars;
            if (make.target)
                selfType = resolveSourceType(*make.target, tvars);
            for (const auto& item : make.body) {
                if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                    if (*ann) addReceiverSig(typeName, annotationToSignature(**ann, selfType, tvars));
                if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
                    if (*fn) ifaces.modules[typeName].exports.try_emplace((*fn)->name);
                if (const auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item))
                    if (*vb) for (const auto& vi : (*vb)->items) {
                        if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&vi))
                            if (*ann) addReceiverSig(typeName, annotationToSignature(**ann, selfType, tvars));
                        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                            if (*fn) ifaces.modules[typeName].exports.try_emplace((*fn)->name);
                    }
            }
        };

        auto collectModule = [&](const ast::ModuleDef& mod) {
            ifaces.modules[mod.name].sourceModule = mod.name;
            ifaces.modules[mod.name].automaticImport = true;
            ifaces.modules[mod.name].isFoul = mod.isFoul;
            for (const auto& item : mod.body) {
                if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
                    if (*fn) ifaces.modules[mod.name].exports.try_emplace((*fn)->name);
                if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                    if (*ann) addModuleSig(mod.name, annotationToSignature(**ann, nullptr));
                if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&item))
                    if (*make) collectMakeAnnotations(**make);
            }
        };

        for (const auto& item : program.items) {
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
    return ifaces;
}

// Build the ImportedInterfaces snapshot from the prebuilt prelude beam in
// `runtimeDir`. Cached per process; safe to call from any thread after
// first construction. Falls back to source-based extraction when no
// prebuilt BEAM artifacts are available (e.g. the wasm build).
inline auto preludeSemanticInterfaces(const std::string& runtimeDir)
    -> const kex::semantic::ImportedInterfaces& {
    static const auto cached = [&]() -> kex::semantic::ImportedInterfaces {
        if (runtimeDir.empty()) return sourcePreludeSemanticInterfaces();
        return preludeRegistry(runtimeDir).buildSemanticInterfaces();
    }();
    return cached;
}

} // namespace kex
