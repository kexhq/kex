#pragma once

#include "../beam/kexi_registry.hxx"
#include "../ir/emit_core.hxx"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kex::cache {

// The interface is needed by later units before erlc has assembled a BEAM.
// Core is kept beside it so a cache hit can skip checking and lowering.
struct CachedSeparateUnit {
    beam::LoadedUnit interface;
    std::vector<ir::EmitResult> modules;
};

class SeparateUnitCache {
public:
    struct Source {
        std::string path;
        std::string_view text;
    };

    explicit SeparateUnitCache(std::filesystem::path root);

    static auto keyFor(const std::string& toolchain,
                       const std::string& runtimeStamp,
                       std::vector<Source> sources,
                       const std::vector<const beam::LoadedUnit*>& dependencies)
        -> std::string;
    auto load(const std::string& key) const -> std::optional<CachedSeparateUnit>;
    auto store(const std::string& key, const CachedSeparateUnit& unit) const -> void;

private:
    static auto digest(const std::string& bytes) -> std::string;
    auto pathFor(const std::string& key) const -> std::filesystem::path;

    std::filesystem::path m_root;
};

} // namespace kex::cache
