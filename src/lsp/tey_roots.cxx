#include "tey_roots.hxx"

#include "../beam/kexi.hxx"

#include <lsp/json/json.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_map>

namespace kex::lsp {
namespace {

namespace fs = std::filesystem;

auto environmentValue(const char* name) -> std::string {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

// Tey.Cache.root: `$TEY_CACHE`, else `$HOME/.cache/tey`, else a relative
// directory — the same three-way fallback, in the same order.
auto cacheRoot() -> std::string {
    if (const auto configured = environmentValue("TEY_CACHE"); !configured.empty())
        return configured;
    if (const auto home = environmentValue("HOME"); !home.empty())
        return home + "/.cache/tey";
    return ".tey-cache";
}

auto sha256Hex(std::string_view text) -> std::string {
    const auto digest = beam::computeSha256(
        std::vector<uint8_t>(text.begin(), text.end()));
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest) out << std::setw(2) << static_cast<int>(byte);
    return out.str();
}

auto readFile(const fs::path& path) -> std::string {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// The directory of the nearest enclosing `package.kex`, walking up from
// `start`. A file outside any package has none, which is how a scratch buffer
// in /tmp keeps today's behaviour.
auto packageDirectoryFor(const std::string& start) -> fs::path {
    std::error_code ec;
    auto directory = fs::weakly_canonical(start, ec);
    if (ec) directory = fs::path(start).lexically_normal();
    if (!fs::is_directory(directory, ec) || ec) directory = directory.parent_path();
    for (; !directory.empty(); directory = directory.parent_path()) {
        ec.clear();
        if (fs::is_regular_file(directory / "package.kex", ec) && !ec)
            return directory;
        if (!directory.has_relative_path()) break;
    }
    return {};
}

auto stringField(const ::lsp::json::Object& object, std::string_view key)
    -> std::string {
    const auto* value = object.find(key);
    return value && value->isString() ? value->string() : std::string();
}

// The dependency source roots a `tey.lock` names, ordered by dependency name
// so the same lockfile always produces the same search order.
struct LockRoots {
    std::vector<std::string> roots;
    // False when the lockfile names a dependency the cache does not hold. The
    // answer is `tey install`, which may fetch it without rewriting the
    // lockfile — so an incomplete answer must not be remembered.
    bool complete = true;
};

auto rootsFromLock(const std::string& text) -> LockRoots {
    ::lsp::json::Value document;
    try {
        document = ::lsp::json::parse(text);
    } catch (const ::lsp::json::Error&) {
        return {};
    }
    if (!document.isObject()) return {};
    const auto& root = document.object();

    // Version 1 is the only shape this derivation knows. A future lockfile is
    // left to `tey` rather than guessed at.
    const auto* version = root.find("version");
    if (!version || !version->isNumber() || version->number() != 1) return {};

    const auto* deps = root.find("deps");
    if (!deps || !deps->isObject()) return {};

    LockRoots result;
    std::map<std::string, std::string> byName;
    for (const auto& [name, entry] : deps->object().keyValueMap()) {
        if (!entry.isObject()) continue;
        const auto& dependency = entry.object();
        const auto git = stringField(dependency, "git");
        const auto commit = stringField(dependency, "commit");
        if (git.empty() || commit.empty()) continue;
        const auto source =
            fs::path(teyCachePackagePath(git, commit)) / "src";
        std::error_code ec;
        // Not yet fetched: the answer is `tey install`, and dropping the root
        // leaves the resolver to say the module is missing — which is true.
        if (fs::is_directory(source, ec) && !ec)
            byName.emplace(std::string(name), source.lexically_normal().string());
        else
            result.complete = false;
    }

    result.roots.reserve(byName.size());
    for (auto& [name, path] : byName) result.roots.push_back(std::move(path));
    return result;
}

struct CacheEntry {
    fs::file_time_type modified{};
    std::uintmax_t size = 0;
    std::vector<std::string> roots;
};

} // namespace

auto teyPackageDirectory(const std::string& start) -> std::string {
    return packageDirectoryFor(start).string();
}

auto teyCachePackagePath(const std::string& gitUrl, const std::string& commit)
    -> std::string {
    // Content-addressed by source URL, then by commit: two packages sharing a
    // display name must never share a checkout (Tey.Cache.sourceRoot).
    return cacheRoot() + "/src/" + sha256Hex(gitUrl) + "/" + commit;
}

auto teyDependencyRoots(const std::string& start) -> std::vector<std::string> {
    const auto packageDirectory = packageDirectoryFor(start);
    if (packageDirectory.empty()) return {};
    const auto lockPath = packageDirectory / "tey.lock";

    std::error_code ec;
    const auto modified = fs::last_write_time(lockPath, ec);
    if (ec) return {};
    ec.clear();
    const auto size = fs::file_size(lockPath, ec);
    if (ec) return {};

    // `tey install` rewrites the lockfile and populates the cache behind the
    // server's back, so the cached answer is only good while the file it was
    // read from is unchanged.
    static std::unordered_map<std::string, CacheEntry> cache;
    const auto key = lockPath.string();
    if (const auto cached = cache.find(key);
        cached != cache.end() && cached->second.modified == modified &&
        cached->second.size == size)
        return cached->second.roots;

    auto derived = rootsFromLock(readFile(lockPath));
    // An incomplete answer is not remembered: the next request re-stats the
    // cache and picks up a `tey install` that fetched without touching the
    // lockfile.
    if (!derived.complete) {
        cache.erase(key);
        return derived.roots;
    }
    auto& entry = cache[key];
    entry.modified = modified;
    entry.size = size;
    entry.roots = std::move(derived.roots);
    return entry.roots;
}

} // namespace kex::lsp
