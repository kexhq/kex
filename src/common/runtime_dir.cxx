#include "runtime_dir.hxx"

#include "prelude_interfaces.hxx"
#include "prelude_loader.hxx"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace kex {

auto prebuiltRuntimeBeamDir() -> std::string {
    namespace fs = std::filesystem;
    std::vector<fs::path> roots;
    if (const char* configured = std::getenv("KEX_RUNTIME_DIR"); configured && *configured)
        roots.emplace_back(configured);
    if (const auto executableDir = executableDirectory(); !executableDir.empty()) {
        roots.push_back((executableDir / "../share/kex/runtime").lexically_normal());
        roots.push_back((executableDir / "runtime/beam").lexically_normal());
    }
#ifdef KEX_RUNTIME_BEAM_DIR
    // Baked in for wasm, which has no executable path to search from. The CLI
    // build reads it straight off the host filesystem via NODERAWFS.
    roots.emplace_back(KEX_RUNTIME_BEAM_DIR);
#endif
    // Where the browser REPL's copy is mounted — see the --preload-file mapping
    // in CMakeLists.txt, next to /stdlib.
    roots.emplace_back("/runtime");
    for (const auto& root : roots) {
        std::error_code ec;
        if (fs::exists(root / "kex_io.beam", ec)) return root.string();
    }
    return {};
}

auto defaultPreludeInterfaces() -> const semantic::ImportedInterfaces& {
    return preludeSemanticInterfaces(prebuiltRuntimeBeamDir());
}

auto isStandardLibrarySource(std::string_view file) -> bool {
    static const std::vector<std::filesystem::path> roots = [] {
        std::vector<std::filesystem::path> paths;
        for (const auto& root : standardLibraryModuleRoots()) {
            std::error_code ec;
            if (auto canonical = std::filesystem::weakly_canonical(root, ec); !ec)
                paths.push_back(std::move(canonical));
        }
        return paths;
    }();
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(file), ec);
    if (ec) return false;
    return std::any_of(roots.begin(), roots.end(), [&](const std::filesystem::path& root) {
        const auto [rootEnd, _] =
            std::mismatch(root.begin(), root.end(), canonical.begin(), canonical.end());
        return rootEnd == root.end();
    });
}

} // namespace kex
