#pragma once
// What the name check found (resolveProgramNames): its diagnostics, and the
// program's symbols with the references to them and the names in scope at
// each point, which tooling answers definitions, references and completions
// from.
#include "db.hxx"

#include <vector>

namespace kex::semantic {

struct NameIndex {
    std::vector<Diagnostic> diagnostics;
    std::vector<SymbolInfo> symbols;
    std::vector<FileState::ScopeSnapshot> completionScopes;
};

} // namespace kex::semantic
