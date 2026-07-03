#pragma once

#include "../semantic/db.hxx"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace kex {

// Indexes every `.kex` file directly inside `dir` into `db`, so REPL tab
// completion sees prelude-provided names (List.map, IO.printLine, ...) the
// same way it sees user-typed definitions. Shared by both the native CLI's
// REPL (dir = KEX_PRELUDE_DIR, a real filesystem path baked in by
// CMakeLists.txt) and the wasm REPL bindings (dir = "/prelude", a path
// Emscripten's --preload-file embeds into MEMFS at build time — see
// CMakeLists.txt's kex_repl_wasm target).
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

} // namespace kex
