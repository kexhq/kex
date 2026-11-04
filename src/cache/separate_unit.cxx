#include "separate_unit.hxx"
#include "cache.hxx"
#include "../beam/kexi.hxx"
#include "../common/digest.hxx"
#include "../common/file_io.hxx"
#include <algorithm>
#include <fstream>
#include <limits>

namespace kex::cache {

namespace {

auto writeFile(const std::filesystem::path& path, const std::string& bytes)
    -> bool {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return output.good();
}

auto readCount(std::istream& input) -> std::optional<std::size_t> {
    std::size_t count = 0;
    if (!(input >> count) || count > 1000) return std::nullopt;
    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    return count;
}

} // namespace

SeparateUnitCache::SeparateUnitCache(std::filesystem::path root)
    : m_root(std::move(root)) {}

auto SeparateUnitCache::pathFor(const std::string& key) const
    -> std::filesystem::path {
    return m_root / "units" / key.substr(0, 2) / key;
}

auto SeparateUnitCache::digest(const std::string& bytes) -> std::string {
    return sha256Hex(bytes);
}

auto SeparateUnitCache::keyFor(
    const std::string& toolchain, const std::string& runtimeStamp,
    std::vector<Source> sources,
    const std::vector<const beam::LoadedUnit*>& dependencies) -> std::string {
    std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) {
        return a.path < b.path;
    });
    std::string material = "kex-unit-v1\n" + toolchain + "\n" + runtimeStamp + "\n";
    for (const auto& source : sources)
        material += source.path + ":" + digest(std::string(source.text)) + "\n";
    for (const auto* unit : dependencies)
        for (const auto& module : unit->modules) {
            const auto bytes = beam::serializeKexi(module.chunk);
            material += module.beamAtom + ":" +
                        digest(std::string(bytes.begin(), bytes.end())) + "\n";
        }
    return digest(material);
}

auto SeparateUnitCache::load(const std::string& key) const
    -> std::optional<CachedSeparateUnit> {
    const auto dir = pathFor(key);
    std::ifstream manifest(dir / "manifest.txt");
    std::string version;
    if (!std::getline(manifest, version) || version != "kex-unit-v1")
        return std::nullopt;

    CachedSeparateUnit cached;
    if (!std::getline(manifest, cached.interface.entryBeamAtom))
        return std::nullopt;
    const auto interfaceCount = readCount(manifest);
    if (!interfaceCount || *interfaceCount == 0) return std::nullopt;
    for (std::size_t i = 0; i < *interfaceCount; ++i) {
        std::string atom, expected;
        if (!std::getline(manifest, atom) || !std::getline(manifest, expected))
            return std::nullopt;
        const auto bytes = readFileText(dir / (std::to_string(i) + ".kexi"));
        if (!bytes || digest(*bytes) != expected) return std::nullopt;
        try {
            cached.interface.modules.push_back({atom, "", beam::deserializeKexi(
                std::vector<uint8_t>(bytes->begin(), bytes->end()))});
        } catch (...) {
            return std::nullopt;
        }
    }
    if (cached.interface.entryBeamAtom != cached.interface.modules.front().beamAtom)
        return std::nullopt;

    const auto moduleCount = readCount(manifest);
    if (!moduleCount || *moduleCount == 0) return std::nullopt;
    for (std::size_t i = 0; i < *moduleCount; ++i) {
        std::string name, expected;
        int arity = 0;
        if (!std::getline(manifest, name) || !(manifest >> arity))
            return std::nullopt;
        manifest.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (!std::getline(manifest, expected)) return std::nullopt;
        auto source = readFileText(dir / (std::to_string(i) + ".core"));
        if (!source || digest(*source) != expected) return std::nullopt;
        cached.modules.push_back({std::move(*source), std::move(name), arity});
    }
    return cached;
}

auto SeparateUnitCache::store(const std::string& key,
                              const CachedSeparateUnit& unit) const -> void {
    namespace fs = std::filesystem;
    const auto target = pathFor(key);
    std::error_code error;
    if (fs::exists(target / "manifest.txt", error)) return;
    auto temporary = target;
    temporary += stagingSuffix();
    fs::create_directories(temporary, error);
    if (error) return;

    std::string manifest = "kex-unit-v1\n" + unit.interface.entryBeamAtom + "\n" +
                           std::to_string(unit.interface.modules.size()) + "\n";
    for (std::size_t i = 0; i < unit.interface.modules.size(); ++i) {
        const auto& module = unit.interface.modules[i];
        const auto data = beam::serializeKexi(module.chunk);
        const std::string bytes(data.begin(), data.end());
        if (!writeFile(temporary / (std::to_string(i) + ".kexi"), bytes)) {
            fs::remove_all(temporary, error);
            return;
        }
        manifest += module.beamAtom + "\n" + digest(bytes) + "\n";
    }
    manifest += std::to_string(unit.modules.size()) + "\n";
    for (std::size_t i = 0; i < unit.modules.size(); ++i) {
        const auto& module = unit.modules[i];
        if (!writeFile(temporary / (std::to_string(i) + ".core"), module.source)) {
            fs::remove_all(temporary, error);
            return;
        }
        manifest += module.moduleName + "\n" +
                    std::to_string(module.mainArity) + "\n" +
                    digest(module.source) + "\n";
    }
    if (!writeFile(temporary / "manifest.txt", manifest)) {
        fs::remove_all(temporary, error);
        return;
    }
    fs::create_directories(target.parent_path(), error);
    if (!error) fs::rename(temporary, target, error);
    if (error) fs::remove_all(temporary, error);
}

} // namespace kex::cache
