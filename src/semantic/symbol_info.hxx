#pragma once
// What tooling knows of one declaration (a function, type, record, trait,
// `make` function, module), and of the names in scope at a point: collected
// from the AST (collectSymbols) and filled in from the checker's record
// (name_index.hxx).
#include "../ast/ast.hxx"
#include "symbol.hxx"
#include "types.hxx"

#include <string>
#include <utility>
#include <vector>

namespace kex::semantic {

struct SymbolInfo {
    std::string name;
    SymbolKind kind;
    TypePtr type;                   // resolved type (Unknown until checked)
    SourceLocation definition;
    std::vector<SourceLocation> references;
    std::string module;             // owning module, "" for top-level
    bool isFoul = false;
    bool isCapability = false;
    bool isServingSlot = false;
    bool isExported = true;
    std::vector<std::pair<std::string, TypePtr>> params;
    // A record's fields in declaration order, each with its type: as written
    // until the file is checked, then as the checker resolved it.
    std::vector<std::pair<std::string, std::string>> fields;
    int clauseCount = 1;
    std::string makeTarget; // set for functions inside a `make TypeName do` block
    // Kex source-shaped declaration text used by editor hover. This is
    // populated when syntax alone carries useful structure (notably records)
    // and enriched with inferred function signatures after type checking.
    std::string detail;
};

// The names in scope at an expression, as the checker recorded them
// (indexNames), for completion at a position.
struct ScopeSnapshot {
    SourceLocation location;
    std::vector<std::string> names;
};

} // namespace kex::semantic
