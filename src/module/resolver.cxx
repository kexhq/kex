#include "resolver.hxx"

#include "../common/string_utils.hxx"

#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <sstream>
#include <utility>

namespace kex::module {

Resolver::Resolver(std::vector<std::string> roots) : m_roots(std::move(roots)) {}

auto Resolver::isForeignNamespace(const std::string& name) -> bool {
    return name.rfind("BEAM.", 0) == 0;
}

namespace {

auto entryFile() -> std::filesystem::path& {
    static std::filesystem::path path;
    return path;
}

auto isEntryFile(const std::filesystem::path& candidate) -> bool {
    if (entryFile().empty()) return false;
    std::error_code ec;
    return std::filesystem::equivalent(candidate, entryFile(), ec) && !ec;
}

} // namespace

auto Resolver::setEntryFile(const std::string& path) -> void {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    entryFile() = ec ? std::filesystem::path(path) : canonical;
}

namespace {

// A part that opens with a letter and a number — `N0001CreateUsers`, the
// first of a directory of migrations — is a file somebody numbered so the
// directory sorts, and the letter is only there because a module name cannot
// start with a digit. Such a file is conventionally written with its capital,
// `N0001_create_users.kex`, which the lowered spellings above only find on a
// file system that ignores case. This is the snake_case spelling with that
// leading capital kept; empty for a part that is not numbered.
auto numberedSnakeCased(const std::string& part) -> std::string {
    if (part.size() < 2
        || !std::isupper(static_cast<unsigned char>(part[0]))
        || !std::isdigit(static_cast<unsigned char>(part[1])))
        return {};
    auto result = snakeCased(part);
    result[0] = part[0];
    return result;
}

// Every file spelling a module name can take, most specific first. Each name
// part contributes its own spellings, so `Web.MockData` reaches
// `web/mock_data.kex` as readily as `web/mockdata.kex`.
auto sourcePaths(const std::string& moduleName)
    -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> paths{{}};
    std::stringstream parts(moduleName);
    std::string part;
    while (std::getline(parts, part, '.')) {
        std::vector<std::string> spellings{lowered(part)};
        if (auto snake = snakeCased(part); snake != spellings.front())
            spellings.push_back(std::move(snake));
        if (auto numbered = numberedSnakeCased(part); !numbered.empty())
            spellings.push_back(std::move(numbered));

        std::vector<std::filesystem::path> extended;
        extended.reserve(paths.size() * spellings.size());
        for (const auto& prefix : paths)
            for (const auto& spelling : spellings)
                extended.push_back(prefix / spelling);
        paths = std::move(extended);
    }
    for (auto& path : paths) path += ".kex";
    return paths;
}

auto candidates(const std::string& moduleName, std::string currentModule)
    -> std::vector<std::string> {
    std::vector<std::string> result;
    while (!currentModule.empty()) {
        result.push_back(currentModule + "." + moduleName);
        const auto dot = currentModule.rfind('.');
        if (dot == std::string::npos) break;
        currentModule.resize(dot);
    }
    result.push_back(moduleName);
    return result;
}

} // namespace

namespace {

// Resolutions already made, by roots, name, enclosing module and entry file.
// Only while caching is on (setCaching): within one build no source file
// appears or disappears, while across REPL inputs one may.
bool g_caching = false;
std::mutex g_cacheMutex;
std::unordered_map<std::string, std::optional<Resolution>> g_resolutions;

} // namespace

auto Resolver::setCaching(bool on) -> void {
    std::lock_guard lock(g_cacheMutex);
    g_caching = on;
    if (!on) g_resolutions.clear();
}

auto Resolver::resolve(const std::string& moduleName,
                       const std::string& currentModule) const -> std::optional<Resolution> {
    {
        std::lock_guard lock(g_cacheMutex);
        if (!g_caching) return resolveUncached(moduleName, currentModule);
    }
    std::string key = moduleName + '\x1f' + currentModule + '\x1f' + entryFile().string();
    for (const auto& root : m_roots) key += '\x1f' + root;
    {
        std::lock_guard lock(g_cacheMutex);
        if (const auto found = g_resolutions.find(key); found != g_resolutions.end())
            return found->second;
    }
    auto resolution = resolveUncached(moduleName, currentModule);
    std::lock_guard lock(g_cacheMutex);
    g_resolutions.emplace(std::move(key), resolution);
    return resolution;
}

auto Resolver::resolveUncached(const std::string& moduleName,
                               const std::string& currentModule) const
    -> std::optional<Resolution> {
    if (isForeignNamespace(moduleName)) return std::nullopt;
    // Relative module identities take precedence over absolute ones. Once an
    // identity has a winning source-root match, record any later matches so
    // callers can diagnose the shadowing without changing resolution order.
    for (const auto& candidateName : candidates(moduleName, currentModule)) {
        std::optional<Resolution> result;
        for (const auto& root : m_roots) {
            std::optional<std::string> matchedPath;
            bool enclosing = false;
            for (const auto& relative : sourcePaths(candidateName)) {
                auto direct = std::filesystem::path(root) / relative;
                if (std::filesystem::is_regular_file(direct) &&
                    !isEntryFile(direct)) {
                    matchedPath = direct.string();
                    break;
                }
            }

            // No file of its own: a module nested in one that has a file
            // (`module Holdings do` inside db/schema.kex's `DB.Schema`). The
            // nearest enclosing module's file wins, so `DB.Schema.Holdings`
            // finds db/schema.kex before db.kex (kexhq/kex#454). The walk
            // stays within `moduleName`: climbing into a relative candidate's
            // `currentModule.` prefix would resolve `Library.Schema`, used in
            // `Library.Query`, to library/query.kex itself.
            const auto prefixLength = candidateName.size() - moduleName.size();
            for (auto container = candidateName;
                 !matchedPath && container.rfind('.') != std::string::npos;) {
                container.resize(container.rfind('.'));
                if (container.size() <= prefixLength) break;
                for (const auto& relative : sourcePaths(container)) {
                    auto path = std::filesystem::path(root) / relative;
                    if (std::filesystem::is_regular_file(path) &&
                        !isEntryFile(path)) {
                        matchedPath = path.string();
                        enclosing = true;
                        break;
                    }
                }
            }

            if (!matchedPath) continue;
            if (!result)
                result = Resolution{candidateName, *matchedPath, {}, enclosing};
            else if (*matchedPath != result->path)
                result->shadowedPaths.push_back(*matchedPath);
        }
        if (result) return result;
    }
    return std::nullopt;
}

} // namespace kex::module
