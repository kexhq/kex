#pragma once
// Checks files a Workspace indexes and records their references together,
// as the language server's workspace index does.
#include "../src/semantic/analyzer.hxx"
#include "../src/lsp/workspace.hxx"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace test {

inline auto resolveTogether(kex::lsp::Workspace& db, const std::vector<std::string>& paths)
    -> void {
    std::vector<std::unique_ptr<kex::semantic::Analyzer>> analyses;
    std::vector<std::pair<std::string, const kex::semantic::TypeChecker*>> checked;
    for (const auto& path : paths) {
        auto* state = db.fileState(path);
        if (!state) continue;
        auto& analyzer = *analyses.emplace_back(std::make_unique<kex::semantic::Analyzer>());
        analyzer.setIndexesNames(true);
        analyzer.analyze(state->ast);
        checked.emplace_back(path, &analyzer.checker());
    }
    db.resolveTogether(checked);
}

} // namespace test
