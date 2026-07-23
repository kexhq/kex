#pragma once

#include "../semantic/db.hxx"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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

// Returns the first available prelude source set in deterministic order.
// Native builds search beside the executable for installed and development
// layouts; wasm embeds the same files at /prelude. Keeping discovery here
// prevents consumers from baking fallback and ordering rules into the compiler.
inline auto preludeSourceFiles() -> std::vector<std::string> {
    std::vector<std::string> roots;
    if (const char* configured = std::getenv("KEX_STDLIB_DIR");
        configured && *configured)
        roots.emplace_back(configured);
    if (const auto executableDir = executableDirectory(); !executableDir.empty()) {
        roots.push_back((executableDir / "../share/kex/prelude").lexically_normal().string());
        roots.push_back((executableDir / "../src/prelude").lexically_normal().string());
    }
#ifdef KEX_PRELUDE_DIR
    roots.emplace_back(KEX_PRELUDE_DIR);
#endif
    roots.emplace_back("/prelude");

    for (const auto& root : roots) {
        std::error_code ec;
        std::vector<std::string> files;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
            if (entry.path().extension() == ".kex")
                files.push_back(entry.path().string());
        if (!ec && !files.empty()) {
            std::sort(files.begin(), files.end());
            return files;
        }
    }
    return {};
}

inline auto isPreludeSourceFile(const std::string& filePath) -> bool {
    const auto candidate = std::filesystem::path(filePath).lexically_normal();
    const auto files = preludeSourceFiles();
    return std::any_of(files.begin(), files.end(), [&](const auto& source) {
        return std::filesystem::path(source).lexically_normal() == candidate;
    });
}

// Indexes every `.kex` file directly inside an explicit directory into `db`.
// The wasm REPL uses this form for its embedded /prelude tree; native callers
// use loadDiscoveredPrelude below so they share the standard discovery order.
inline auto loadPrelude(kex::semantic::SemanticDB& db, const std::string& dir) -> void {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".kex") continue;
        std::ifstream file(entry.path());
        if (!file.is_open()) continue;
        std::ostringstream contents;
        contents << file.rdbuf();
        db.updateFile(entry.path().string(), contents.str());
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
