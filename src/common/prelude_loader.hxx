#pragma once

#include "../semantic/db.hxx"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace kex {

inline auto executableDirectory() -> std::filesystem::path {
#if defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        buffer.resize(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return std::filesystem::weakly_canonical(buffer.data()).parent_path();
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size > 0) {
        buffer[static_cast<size_t>(size)] = '\0';
        return std::filesystem::weakly_canonical(buffer.data()).parent_path();
    }
#endif
    return {};
}

// All standard-library sources share one root. `prelude.kex` is the manifest
// that selects the automatically visible subset; every other module remains
// opt-in through `using`.
inline auto standardLibraryRootCandidates()
    -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> candidates;
    if (const char* configured = std::getenv("KEX_STDLIB_DIR");
        configured && *configured)
        candidates.emplace_back(configured);
    if (const char* configured = std::getenv("KEX_LIBRARY_DIR");
        configured && *configured)
        candidates.emplace_back(configured);
    if (const auto executableDir = executableDirectory(); !executableDir.empty()) {
        candidates.push_back(
            (executableDir / "../share/kex/stdlib").lexically_normal());
        candidates.push_back(
            (executableDir / "../src/stdlib").lexically_normal());
    }
#ifdef KEX_STDLIB_DIR
    candidates.emplace_back(KEX_STDLIB_DIR);
#endif
#ifdef KEX_STDLIB_MODULE_DIR
    candidates.emplace_back(KEX_STDLIB_MODULE_DIR);
#endif
    candidates.emplace_back("/stdlib");

    std::vector<std::filesystem::path> roots;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_directory(candidate, ec) || ec) continue;
        const auto normalized = candidate.lexically_normal();
        if (std::find(roots.begin(), roots.end(), normalized) == roots.end())
            roots.push_back(normalized);
    }
    return roots;
}

inline auto standardLibraryModuleRoots() -> std::vector<std::string> {
    std::vector<std::string> roots;
    for (const auto& root : standardLibraryRootCandidates())
        roots.push_back(root.string());
    return roots;
}

// The manifest vocabulary (`bundle`, `version`, `tey`, ...) that a
// `package.kex` is written in. Deliberately NOT under src/stdlib: everything
// there is compiled into the stdlib artifact, which would put these names in
// front of every program. They live on their own and are loaded only for the
// file that speaks them.
inline auto manifestVocabularyFile() -> std::string {
    std::vector<std::filesystem::path> candidates;
    if (const char* configured = std::getenv("KEX_MANIFEST_DIR");
        configured && *configured)
        candidates.emplace_back(configured);
    if (const auto executableDir = executableDirectory(); !executableDir.empty()) {
        candidates.push_back(
            (executableDir / "../share/kex/manifest").lexically_normal());
        candidates.push_back(
            (executableDir / "../src/manifest").lexically_normal());
    }
#ifdef KEX_MANIFEST_DIR
    candidates.emplace_back(KEX_MANIFEST_DIR);
#endif
    for (const auto& candidate : candidates) {
        const auto file = candidate / "bundle.kex";
        std::error_code ec;
        if (std::filesystem::is_regular_file(file, ec) && !ec)
            return file.string();
    }
    return {};
}

inline auto preludeManifestPath(const std::filesystem::path& root)
    -> std::optional<std::filesystem::path> {
    auto manifest = root / "prelude.kex";
    std::error_code ec;
    if (std::filesystem::is_regular_file(manifest, ec) && !ec) return manifest;
    return std::nullopt;
}

inline auto preludeFilesFromRoot(const std::filesystem::path& root)
    -> std::vector<std::string> {
    auto manifest = preludeManifestPath(root);
    if (!manifest) return {};

    std::ifstream input(*manifest);
    if (!input)
        throw std::runtime_error("cannot read prelude manifest: " +
                                 manifest->string());
    std::vector<std::string> files;
    std::unordered_set<std::string> importedModules;
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        constexpr std::string_view prefix = "using ";
        auto fail = [&](const std::string& message) {
            throw std::runtime_error(
                manifest->string() + ":" +
                std::to_string(lineNumber) + ": " + message);
        };
        if (line.compare(first, prefix.size(), prefix) != 0)
            fail("expected a bare `using Module.Name` entry");
        auto module = line.substr(first + prefix.size());
        const auto last = module.find_last_not_of(" \t\r");
        module.resize(last == std::string::npos ? 0 : last + 1);
        if (module.empty() || module.find_first_of(" ,") != std::string::npos)
            fail("expected exactly one module name after `using`");
        if (!importedModules.insert(module).second)
            fail("duplicate prelude import: " + module);

        std::filesystem::path relative;
        std::stringstream segments(module);
        std::string segment;
        while (std::getline(segments, segment, '.')) {
            if (segment.empty() ||
                !std::isupper(
                    static_cast<unsigned char>(segment.front())) ||
                !std::all_of(
                    segment.begin(), segment.end(),
                    [](unsigned char c) {
                        return std::isalnum(c) || c == '_';
                    }))
                fail("invalid module name: " + module);
            std::transform(segment.begin(), segment.end(), segment.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            relative /= segment;
        }
        relative += ".kex";
        auto source = root / relative;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec) || ec)
            throw std::runtime_error(
                "prelude imports missing standard-library source: " +
                module + " (" + source.string() + ")");
        files.push_back(source.string());
    }
    return files;
}

// Returns the sources imported by the first available `prelude.kex` manifest.
// Manifest order is preserved because it is the declaration order users see.
inline auto preludeSourceFiles() -> std::vector<std::string> {
    for (const auto& root : standardLibraryRootCandidates()) {
        if (preludeManifestPath(root))
            return preludeFilesFromRoot(root);
    }
    return {};
}

// Returns the first available opt-in standard-library source set. This mirrors
// preludeSourceFiles(), excluding both the manifest and everything it imports.
inline auto standardLibrarySourceFiles() -> std::vector<std::string> {
    for (const auto& rootString : standardLibraryModuleRoots()) {
        const auto root = std::filesystem::path(rootString);
        if (!preludeManifestPath(root)) continue;
        std::error_code ec;
        std::vector<std::string> files;
        std::unordered_set<std::string> automatic;
        for (const auto& path : preludeFilesFromRoot(root))
            automatic.insert(
                std::filesystem::path(path).lexically_normal().string());
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root, ec))
            if (entry.path().extension() == ".kex" &&
                entry.path().filename() != "prelude.kex" &&
                !automatic.contains(entry.path().lexically_normal().string()))
                files.push_back(entry.path().string());
        if (!ec) {
            std::sort(files.begin(), files.end());
            return files;
        }
    }
    return {};
}

// Sources compiled into the installed stdlib artifact: the manifest-selected
// prelude plus opt-in modules stored directly at the stdlib root. Nested
// library modules remain independently loadable source dependencies.
inline auto standardLibraryArtifactSourceFiles()
    -> std::vector<std::string> {
    auto files = preludeSourceFiles();
    if (files.empty()) return {};
    const auto root =
        std::filesystem::path(files.front()).parent_path();
    for (const auto& source : standardLibrarySourceFiles())
        if (std::filesystem::path(source).parent_path() == root)
            files.push_back(source);
    return files;
}

inline auto isPreludeSourceFile(const std::string& filePath) -> bool {
    const auto candidate = std::filesystem::path(filePath).lexically_normal();
    const auto files = preludeSourceFiles();
    return std::any_of(files.begin(), files.end(), [&](const auto& source) {
        return std::filesystem::path(source).lexically_normal() == candidate;
    });
}

// Indexes the prelude selected by an explicit stdlib root into `db`.
inline auto loadPrelude(kex::semantic::SemanticDB& db, const std::string& dir) -> void {
    for (const auto& filePath : preludeFilesFromRoot(dir)) {
        std::ifstream file(filePath);
        if (!file.is_open()) continue;
        std::ostringstream contents;
        contents << file.rdbuf();
        db.updateFile(filePath, contents.str());
    }
}

inline auto loadDiscoveredPrelude(kex::semantic::SemanticDB& db) -> void {
    for (const auto& filePath : preludeSourceFiles()) {
        std::ifstream file(filePath);
        if (!file.is_open()) continue;
        std::ostringstream contents;
        contents << file.rdbuf();
        db.updateFile(filePath, contents.str());
    }
}

} // namespace kex
