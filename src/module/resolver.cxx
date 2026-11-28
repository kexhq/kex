#include "resolver.hxx"
#include "../common/diagnostics.hxx"
#include "../common/name_case.hxx"
#include "../common/prelude_loader.hxx"
#include "../semantic/analyzer.hxx"
#include "discovery.hxx"

#include "../common/string_utils.hxx"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
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
    if (entryFile().empty()) {
        return false;
    }
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
    if (part.size() < 2 || !kex::startsUppercase(part) ||
        !std::isdigit(static_cast<unsigned char>(part[1]))) {
        return {};
    }
    auto result = snakeCased(part);
    result[0] = part[0];
    return result;
}

// Every file spelling a module name can take, most specific first. Each name
// part contributes its own spellings, so `Web.MockData` reaches
// `web/mock_data.kex` as readily as `web/mockdata.kex`.
auto sourcePaths(const std::string& moduleName) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> paths{{}};
    std::stringstream parts(moduleName);
    std::string part;
    while (std::getline(parts, part, '.')) {
        std::vector<std::string> spellings{lowered(part)};
        if (auto snake = snakeCased(part); snake != spellings.front()) {
            spellings.push_back(std::move(snake));
        }
        if (auto numbered = numberedSnakeCased(part); !numbered.empty()) {
            spellings.push_back(std::move(numbered));
        }

        std::vector<std::filesystem::path> extended;
        extended.reserve(paths.size() * spellings.size());
        for (const auto& prefix : paths) {
            for (const auto& spelling : spellings) {
                extended.push_back(prefix / spelling);
            }
        }
        paths = std::move(extended);
    }
    for (auto& path : paths) {
        path += ".kex";
    }
    return paths;
}

auto candidates(const std::string& moduleName, std::string currentModule)
    -> std::vector<std::string> {
    std::vector<std::string> result;
    while (!currentModule.empty()) {
        result.push_back(currentModule + "." + moduleName);
        const auto dot = currentModule.rfind('.');
        if (dot == std::string::npos) {
            break;
        }
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

// Read afresh, not through the cached standardLibraryModuleRoots: a resolve
// can come before whatever sets KEX_STDLIB_DIR, and must not fix the roots.
auto inStandardLibrary(const std::string& path) -> bool {
    std::error_code ec;
    const auto file = std::filesystem::weakly_canonical(path, ec);
    for (const auto& root : kex::standardLibraryRootCandidates()) {
        const auto relative = file.lexically_relative(std::filesystem::weakly_canonical(root, ec));
        if (!ec && !relative.empty() && *relative.begin() != "..") {
            return true;
        }
    }
    return false;
}

// A program's own file found ahead of a standard-library module's file (a
// stray units.kex beside the entry, for `using Units.SI`). The stdlib's
// wins: the runtimes used to load the program's file while the checker read
// the stdlib's interface, so a program checked clean and failed when run, or
// failed to parse a file it never meant to use. Every mode resolves here, so
// they agree; the file is named once, as a warning.
auto warnShadowed(const std::string& path, const std::string& moduleName) -> void {
    static std::mutex warnedMutex;
    static std::set<std::string> warned;
    {
        std::lock_guard lock(warnedMutex);
        if (!warned.insert(path + '\x1f' + moduleName).second) {
            return;
        }
    }
    kex::diagnostics::printSemanticDiagnostic(
        kex::semantic::makeDiagnostic(
            kex::semantic::Diagnostic::Level::Warning,
            kex::diag::Code::ModuleFileShadowsStandardLibrary,
            SourceLocation{.file = shownPath(path), .line = 1, .column = 1},
            {{"module", moduleName}}));
}

auto standardLibraryWins(const Resolution& program, Resolution stdlib) -> Resolution {
    warnShadowed(program.path, stdlib.moduleName);
    stdlib.shadowedPaths.push_back(program.path);
    return stdlib;
}

// Whether the standard library has a file of `moduleName`'s own under a root
// that `roots` leaves out.
auto standardLibraryHasFile(const std::string& moduleName, const std::vector<std::string>& roots)
    -> bool {
    for (const auto& candidate : kex::standardLibraryRootCandidates()) {
        const auto root = candidate.string();
        if (std::find(roots.begin(), roots.end(), root) != roots.end()) {
            continue;
        }
        for (const auto& relative : sourcePaths(moduleName)) {
            if (std::filesystem::is_regular_file(std::filesystem::path(root) / relative)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

auto Resolver::setCaching(bool on) -> void {
    std::lock_guard lock(g_cacheMutex);
    g_caching = on;
    if (!on) {
        g_resolutions.clear();
    }
}

auto Resolver::resolve(const std::string& moduleName, const std::string& currentModule) const
    -> std::optional<Resolution> {
    {
        std::lock_guard lock(g_cacheMutex);
        if (!g_caching) {
            return resolveUncached(moduleName, currentModule);
        }
    }
    std::string key = moduleName + '\x1f' + currentModule + '\x1f' + entryFile().string();
    for (const auto& root : m_roots) {
        key += '\x1f' + root;
    }
    {
        std::lock_guard lock(g_cacheMutex);
        if (const auto found = g_resolutions.find(key); found != g_resolutions.end()) {
            return found->second;
        }
    }
    auto resolution = resolveUncached(moduleName, currentModule);
    std::lock_guard lock(g_cacheMutex);
    g_resolutions.emplace(std::move(key), resolution);
    return resolution;
}

auto Resolver::resolveUncached(const std::string& moduleName, const std::string& currentModule)
    const -> std::optional<Resolution> {
    if (isForeignNamespace(moduleName)) {
        return std::nullopt;
    }
    // Relative module identities take precedence over absolute ones. Once an
    // identity has a winning source-root match, record any later matches so
    // callers can diagnose the shadowing without changing resolution order.
    for (const auto& candidateName : candidates(moduleName, currentModule)) {
        std::optional<Resolution> result;
        std::optional<Resolution> stdlibMatch;
        for (const auto& root : m_roots) {
            std::optional<std::string> matchedPath;
            bool enclosing = false;
            for (const auto& relative : sourcePaths(candidateName)) {
                auto direct = std::filesystem::path(root) / relative;
                if (std::filesystem::is_regular_file(direct) && !isEntryFile(direct)) {
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
                if (container.size() <= prefixLength) {
                    break;
                }
                for (const auto& relative : sourcePaths(container)) {
                    auto path = std::filesystem::path(root) / relative;
                    if (std::filesystem::is_regular_file(path) && !isEntryFile(path)) {
                        matchedPath = path.string();
                        enclosing = true;
                        break;
                    }
                }
            }

            if (!matchedPath) {
                continue;
            }
            if (!result) {
                result = Resolution{candidateName, *matchedPath, {}, enclosing};
            } else if (*matchedPath != result->path) {
                result->shadowedPaths.push_back(*matchedPath);
                // The standard library's own file for the name, behind one of
                // the program's: the stdlib's wins (standardLibraryWins).
                if (!stdlibMatch && !enclosing && inStandardLibrary(*matchedPath) &&
                    !inStandardLibrary(result->path)) {
                    stdlibMatch = Resolution{candidateName, *matchedPath, {}, enclosing};
                }
            }
        }
        if (result && stdlibMatch) {
            return standardLibraryWins(*result, std::move(*stdlibMatch));
        }
        // Roots without the stdlib's (a check or a walker run, which read a
        // stdlib module through its interface): its file is not ours to return,
        // so the program's is not found and the caller takes the interface.
        if (result && !inStandardLibrary(result->path) &&
            standardLibraryHasFile(candidateName, m_roots)) {
            warnShadowed(result->path, candidateName);
            return std::nullopt;
        }
        if (result) {
            return result;
        }
    }
    return std::nullopt;
}

} // namespace kex::module
