#pragma once

#include "../beam/kexi_registry.hxx"
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

// Build the ImportedInterfaces snapshot from the prebuilt prelude beam in
// `runtimeDir`. Cached per process; safe to call from any thread after
// first construction. Returns an empty snapshot when `runtimeDir` is
// empty (e.g. on the wasm build with no runtime beams).
inline auto preludeSemanticInterfaces(const std::string& runtimeDir)
    -> const kex::semantic::ImportedInterfaces& {
    static const auto cached = [&]() -> kex::semantic::ImportedInterfaces {
        if (runtimeDir.empty()) return {};
        return preludeRegistry(runtimeDir).buildSemanticInterfaces();
    }();
    return cached;
}

} // namespace kex
