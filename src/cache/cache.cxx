#include "cache.hxx"

#include "../common/prelude_loader.hxx"
#include "../common/version.hxx"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <random>

namespace kex::cache {

namespace fs = std::filesystem;

auto base() -> std::optional<fs::path> {
    if (const char* configured = std::getenv("KEX_CACHE")) {
        const std::string_view value = configured;
        if (value.empty() || value == "off" || value == "0") return std::nullopt;
        return fs::path{value};
    }
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return fs::path{xdg} / "kex";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path{home} / ".cache" / "kex";
    return std::nullopt;
}

auto directory(std::string_view subdirectory) -> std::optional<fs::path> {
    if (auto root = base()) return *root / subdirectory;
    return std::nullopt;
}

namespace {

// "path|size|mtime" of one file; a missing file stats as zeros, which is
// still a stable answer.
auto fileStamp(const fs::path& path) -> std::string {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    const auto modified = fs::last_write_time(path, ec).time_since_epoch().count();
    return path.string() + "|" +
           std::to_string(static_cast<unsigned long long>(ec ? 0 : size)) + "|" +
           std::to_string(static_cast<long long>(ec ? 0 : modified));
}

} // namespace

auto toolchainFingerprint() -> const std::string& {
    static const std::string fingerprint = [] {
        std::string text = kex::versionNumber() + "\n" + kex::kGitRevision + "\n";
        if (const auto exe = kex::executablePath(); !exe.empty())
            text += "self:" + fileStamp(exe) + "\n";
        for (const auto& file : kex::preludeSourceFiles())
            text += fileStamp(file) + "\n";
        for (const auto& file : kex::standardLibrarySourceFiles())
            text += fileStamp(file) + "\n";
        return text;
    }();
    return fingerprint;
}

auto stagingSuffix() -> std::string {
    // `getpid()` is unavailable under emscripten; a random per-process value
    // serves the same purpose everywhere.
    static const auto process = [] {
        std::random_device device;
        return (static_cast<unsigned long long>(device()) << 32) ^ device();
    }();
    static std::atomic<unsigned long long> counter{0};
    return ".tmp." + std::to_string(process) + "." +
           std::to_string(counter.fetch_add(1));
}

namespace {

// Creates `path`'s directory, lets `fill` write the staging file, then
// renames it into place; removes the staging file on any failure.
template <typename Fill>
auto publish(const fs::path& path, Fill&& fill) -> bool {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    auto staging = path;
    staging += stagingSuffix();
    if (!fill(staging)) {
        fs::remove(staging, ec);
        return false;
    }
    fs::rename(staging, path, ec);
    if (ec) {
        fs::remove(staging, ec);
        return false;
    }
    return true;
}

} // namespace

auto atomicWrite(const fs::path& path, std::string_view bytes) -> bool {
    return publish(path, [&](const fs::path& staging) {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        return static_cast<bool>(output);
    });
}

auto atomicCopy(const fs::path& source, const fs::path& path) -> bool {
    return publish(path, [&](const fs::path& staging) {
        std::error_code ec;
        fs::copy_file(source, staging, fs::copy_options::overwrite_existing, ec);
        return !ec;
    });
}

} // namespace kex::cache
