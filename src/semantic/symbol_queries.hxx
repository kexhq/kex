#pragma once
// What tooling asks of a set of indexed files' symbols: a definition by name,
// a module's or a type's member, the symbol at a position, completions by
// prefix and at a point. One implementation, over whatever holds the files:
// SemanticDB's documents, or the checker's name index (NameIndex).
#include "db.hxx"
#include "imported_interfaces.hxx"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kex::semantic {

// One file's symbols, the references recorded on them, and the names in
// scope at each point in it (none when the file was not resolved).
struct IndexedFile {
    std::string_view path;
    std::vector<SymbolInfo>* symbols;
    const std::vector<FileState::ScopeSnapshot>* scopes = nullptr;
};
using IndexedFiles = std::vector<IndexedFile>;

auto findSymbol(const IndexedFiles& files, const std::string& name,
                const std::string& preferFile = "") -> SymbolInfo*;
auto symbolInModule(const IndexedFiles& files, const std::string& moduleName,
                    const std::string& name) -> SymbolInfo*;
auto receiverSymbol(const IndexedFiles& files, const std::string& receiver,
                    const std::string& name) -> const SymbolInfo*;
auto symbolAt(const IndexedFiles& files, const std::string& file, uint32_t line,
              uint32_t col) -> const SymbolInfo*;
auto completionsFor(const IndexedFiles& files, const std::string& prefix,
                    const ImportedInterfaces* imports) -> std::vector<std::string>;
auto completionsAt(const IndexedFiles& files, const std::string& file, uint32_t line,
                   uint32_t col, const std::string& prefix,
                   const ImportedInterfaces* imports) -> std::vector<std::string>;

} // namespace kex::semantic
