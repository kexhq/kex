#pragma once

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kex {

inline constexpr std::array<std::string_view, 12> kPreludeTier0 = {
    "algebra.kex", "console.kex", "errorable.kex", "io.kex",
    "kex.kex", "math.kex", "optional.kex", "process.kex",
    "range.kex", "stream.kex", "system.kex", "test.kex",
};
inline constexpr std::array<std::string_view, 8> kPreludeTier1 = {
    "blankable.kex", "env.kex", "http.kex", "number.kex",
    "parser.kex", "string.kex", "truthyable.kex", "web_server.kex",
};
inline constexpr std::array<std::string_view, 3> kPreludeTier2 = {
    "enumerable.kex", "evaluator.kex", "file.kex",
};
inline constexpr std::array<std::string_view, 2> kPreludeTier3 = {
    "list.kex", "map.kex",
};

inline auto orderPreludeSourcesByTier(const std::vector<std::string>& files)
    -> std::vector<std::string> {
    std::unordered_map<std::string, std::string> discovered;
    for (const auto& file : files) {
        auto name = std::filesystem::path(file).filename().string();
        if (!discovered.emplace(name, file).second)
            throw std::runtime_error("duplicate stdlib source filename: " + name);
    }

    std::vector<std::string> ordered;
    std::unordered_set<std::string> declared;
    auto append = [&](const auto& tier) {
        for (const auto nameView : tier) {
            std::string name{nameView};
            if (!declared.insert(name).second)
                throw std::runtime_error("stdlib source is declared in multiple tiers: " +
                                         name);
            auto found = discovered.find(name);
            if (found == discovered.end())
                throw std::runtime_error("declared stdlib source is missing: " + name);
            ordered.push_back(found->second);
        }
    };
    append(kPreludeTier0);
    append(kPreludeTier1);
    append(kPreludeTier2);
    append(kPreludeTier3);

    for (const auto& [name, _] : discovered)
        if (!declared.contains(name))
            throw std::runtime_error("stdlib source has no compilation tier: " + name);
    return ordered;
}

inline auto groupPreludeSourcesByTier(const std::vector<std::string>& files)
    -> std::array<std::vector<std::string>, 4> {
    const auto ordered = orderPreludeSourcesByTier(files);
    std::array<std::vector<std::string>, 4> groups;
    size_t offset = 0;
    auto take = [&](size_t tier, size_t count) {
        groups[tier].insert(groups[tier].end(), ordered.begin() + offset,
                            ordered.begin() + offset + count);
        offset += count;
    };
    take(0, kPreludeTier0.size());
    take(1, kPreludeTier1.size());
    take(2, kPreludeTier2.size());
    take(3, kPreludeTier3.size());
    return groups;
}

} // namespace kex
