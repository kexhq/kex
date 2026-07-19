#pragma once

#include "../beam/kexi_registry.hxx"
#include "../semantic/imported_interfaces.hxx"
#include <filesystem>
#include <stdexcept>
#include <string>

namespace kex {

// Load the prebuilt `kex_prelude.beam` from `runtimeDir` and return its
// compiled-interface registry. The beam must have been produced by a
// matching `kex --build-prelude` run; throws if the artifact is missing
// or its KexI chunk is malformed.
inline auto loadPreludeRegistry(const std::string& runtimeDir)
    -> kex::beam::KexiRegistry {
    kex::beam::KexiRegistry registry;
    auto path = (std::filesystem::path{runtimeDir} / "kex_prelude.beam").string();
    auto errors = registry.loadUnit(path);
    if (!errors.empty())
        throw std::runtime_error("invalid prebuilt standard library: " +
                                 errors.front().message);
    return registry;
}

// Build the ImportedInterfaces snapshot from the prebuilt prelude beam in
// `runtimeDir`. Cached per process; safe to call from any thread after
// first construction. Returns an empty snapshot when `runtimeDir` is
// empty (e.g. on the wasm build with no runtime beams).
inline auto preludeSemanticInterfaces(const std::string& runtimeDir)
    -> const kex::semantic::ImportedInterfaces& {
    static const auto cached = [&]() -> kex::semantic::ImportedInterfaces {
        if (runtimeDir.empty()) return {};
        return loadPreludeRegistry(runtimeDir).buildSemanticInterfaces();
    }();
    return cached;
}

} // namespace kex
