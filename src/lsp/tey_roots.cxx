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

// A package directory's manifest declares a workspace (`workspace do`) —
// Tey.Workspace.workspaceAt, read the way the editor can afford to: textually.
auto declaresWorkspace(const fs::path& directory) -> bool {
    const auto manifest = readFile(directory / "package.kex");
    size_t lineStart = 0;
    while (lineStart < manifest.size()) {
        auto lineEnd = manifest.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = manifest.size();
        auto first = manifest.find_first_not_of(" \t", lineStart);
        if (first != std::string::npos && first < lineEnd &&
            manifest.compare(first, 9, "workspace") == 0) {
            auto after = manifest.find_first_not_of(" \t", first + 9);
            if (after != std::string::npos && after < lineEnd &&
                manifest.compare(after, 2, "do") == 0)
                return true;
        }
        lineStart = lineEnd + 1;
    }
    return false;
}

// The directory whose `tey.lock` governs `packageDirectory`: the OUTERMOST
// enclosing package that declares a workspace, or the package itself — the
// root Tey.Workspace.discover picks. A workspace member has no lockfile of
// its own; the one at the workspace root lists every member's dependencies.
auto lockDirectoryFor(const fs::path& packageDirectory) -> fs::path {
    fs::path chosen = packageDirectory;
    std::error_code ec;
    for (auto directory = packageDirectory; !directory.empty();
         directory = directory.parent_path()) {
        ec.clear();
        if (fs::is_regular_file(directory / "package.kex", ec) && !ec &&
            declaresWorkspace(directory))
            chosen = directory;
        if (!directory.has_relative_path()) break;
    }
    return chosen;
}

auto canonicalRoot(const fs::path& path) -> std::string {
    std::error_code ec;
    auto resolved = fs::weakly_canonical(path, ec);
    return (ec ? path.lexically_normal() : resolved).string();
}

// `local("name", path: "../checkout")` lines of `package.local.kex` — the
// developer's own overrides, which win over the lockfile (Tey.Local).
auto localOverrides(const fs::path& workspaceRoot) -> std::map<std::string, fs::path> {
    std::map<std::string, fs::path> overrides;
    const auto text = readFile(workspaceRoot / "package.local.kex");
    size_t at = 0;
    while ((at = text.find("local(", at)) != std::string::npos) {
        at += 6;
        const auto nameOpen = text.find('"', at);
        const auto nameClose = nameOpen == std::string::npos ? nameOpen : text.find('"', nameOpen + 1);
        const auto pathKey = nameClose == std::string::npos ? nameClose : text.find("path:", nameClose);
        const auto pathOpen = pathKey == std::string::npos ? pathKey : text.find('"', pathKey);
        const auto pathClose = pathOpen == std::string::npos ? pathOpen : text.find('"', pathOpen + 1);
        if (pathClose == std::string::npos) break;
        fs::path path = text.substr(pathOpen + 1, pathClose - pathOpen - 1);
        if (path.is_relative()) path = workspaceRoot / path;
        overrides[text.substr(nameOpen + 1, nameClose - nameOpen - 1)] = path;
        at = pathClose;
    }
    return overrides;
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

// Where one locked package's sources live — Tey.Commands.dependencyRoots:
// a local override first, then by source kind. `workspace` is a member of
// this workspace; `path` is snapshotted into the cache by content hash, but
// the directory it was taken from is what the developer is editing, so it
// wins while it exists; `git` is the cache checkout at its commit, inside its
// `subdir` when the package is not at the repository root.
auto packageRootFor(const ::lsp::json::Object& entry, const fs::path& workspaceRoot)
    -> std::vector<fs::path> {
    const auto source = stringField(entry, "source");
    const auto path = stringField(entry, "path");
    if (source == "workspace") return {workspaceRoot / path};
    if (source == "path") {
        std::vector<fs::path> candidates;
        if (!path.empty())
            candidates.push_back(fs::path(path).is_relative() ? workspaceRoot / path : fs::path(path));
        if (const auto digest = stringField(entry, "sha256"); !digest.empty())
            candidates.push_back(fs::path(cacheRoot()) / "snapshots" / digest);
        return candidates;
    }
    const auto git = stringField(entry, "git");
    const auto commit = stringField(entry, "commit");
    if (git.empty() || commit.empty()) return {};
    auto checkout = fs::path(teyCachePackagePath(git, commit));
    if (const auto subdir = stringField(entry, "subdir"); !subdir.empty() && subdir != ".")
        checkout /= subdir;
    return {checkout};
}

auto rootsFromLock(const std::string& text, const fs::path& workspaceRoot) -> LockRoots {
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

    // `packages` is the lockfile tey writes today; `deps`, git-only, is the
    // shape it wrote first and an old checkout may still carry.
    const auto* packages = root.find("packages");
    if (!packages || !packages->isObject()) packages = root.find("deps");
    if (!packages || !packages->isObject()) return {};

    const auto overrides = localOverrides(workspaceRoot);
    LockRoots result;
    std::map<std::string, std::string> byName;
    for (const auto& [name, entry] : packages->object().keyValueMap()) {
        if (!entry.isObject()) continue;
        std::vector<fs::path> candidates;
        if (const auto local = overrides.find(std::string(name)); local != overrides.end())
            candidates.push_back(local->second);
        for (auto& candidate : packageRootFor(entry.object(), workspaceRoot))
            candidates.push_back(std::move(candidate));
        if (candidates.empty()) continue;
        bool found = false;
        for (const auto& candidate : candidates) {
            std::error_code ec;
            const auto source = candidate / "src";
            if (fs::is_directory(source, ec) && !ec) {
                byName.emplace(std::string(name), canonicalRoot(source));
                found = true;
                break;
            }
        }
        // Not yet fetched: the answer is `tey install`, and dropping the root
        // leaves the resolver to say the module is missing — which is true.
        if (!found) result.complete = false;
    }

    result.roots.reserve(byName.size());
    for (auto& [name, path] : byName) result.roots.push_back(std::move(path));
    return result;
}

struct CacheEntry {
    fs::file_time_type modified{};
    std::uintmax_t size = 0;
    fs::file_time_type localModified{};
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
    const auto workspaceRoot = lockDirectoryFor(packageDirectory);
    const auto lockPath = workspaceRoot / "tey.lock";

    std::error_code ec;
    const auto modified = fs::last_write_time(lockPath, ec);
    if (ec) return {};
    ec.clear();
    const auto size = fs::file_size(lockPath, ec);
    if (ec) return {};

    // `tey install` rewrites the lockfile and populates the cache behind the
    // server's back, so the cached answer is only good while the file it was
    // read from is unchanged.
    // `package.local.kex` changes the answer as well.
    ec.clear();
    auto localModified = fs::last_write_time(workspaceRoot / "package.local.kex", ec);
    if (ec) localModified = {};
    static std::unordered_map<std::string, CacheEntry> cache;
    const auto key = lockPath.string();
    if (const auto cached = cache.find(key);
        cached != cache.end() && cached->second.modified == modified &&
        cached->second.size == size && cached->second.localModified == localModified)
        return cached->second.roots;

    auto derived = rootsFromLock(readFile(lockPath), workspaceRoot);
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
    entry.localModified = localModified;
    entry.roots = std::move(derived.roots);
    return entry.roots;
}

} // namespace kex::lsp
