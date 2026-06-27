#pragma once

#include "../ast/ast.hxx"
#include "symbol.hxx"
#include "types.hxx"
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

struct Diagnostic;

struct SymbolInfo {
    std::string name;
    SymbolKind kind;
    TypePtr type;                   // resolved type (Unknown before pass 3)
    SourceLocation definition;
    std::vector<SourceLocation> references;
    std::string module;             // owning module, "" for top-level
    bool isFoul = false;
    bool isExported = true;
    std::vector<std::pair<std::string, TypePtr>> params;
    int clauseCount = 1;
};

struct FileState {
    std::string path;
    uint64_t version = 0;
    ast::Program ast;
    std::vector<Diagnostic> diagnostics;
    std::vector<SymbolInfo> symbols;
};

class SemanticDB {
public:
    auto updateFile(const std::string& path, std::string source) -> void;
    auto removeFile(const std::string& path) -> void;

    auto diagnosticsFor(const std::string& file) const -> const std::vector<Diagnostic>&;
    auto symbolsFor(const std::string& file) const -> const std::vector<SymbolInfo>&;
    auto exportsFor(const std::string& moduleName) const -> std::vector<SymbolInfo*>;

    // Returns true if `name` appears as a top-level symbol in ANY indexed file.
    // Used by ResolvePass to check prelude-sourced names without a per-call
    // stdlib table lookup.
    auto isGloballyKnown(const std::string& name) const -> bool;

    // Finds the SymbolInfo for `name`, preferring symbols from `preferFile`.
    // Returns null if not found in any indexed file.
    auto findSymbol(const std::string& name,
                    const std::string& preferFile = "") -> SymbolInfo*;
    auto findSymbol(const std::string& name,
                    const std::string& preferFile = "") const -> const SymbolInfo*;

    // Returns the symbol whose definition or a reference is at (file, line, col).
    // Used for go-to-definition and hover. Returns null if nothing found there.
    auto symbolAt(const std::string& file,
                  uint32_t line, uint32_t col) const -> const SymbolInfo*;

    // Access to raw file state for passes
    auto fileState(const std::string& path) -> FileState*;
    auto fileState(const std::string& path) const -> const FileState*;

private:
    auto rebuildModuleExports(const std::string& path) -> void;

    std::unordered_map<std::string, FileState> m_files;
    std::unordered_map<std::string, std::vector<SymbolInfo*>> m_moduleExports;

    static const std::vector<Diagnostic> s_emptyDiagnostics;
    static const std::vector<SymbolInfo> s_emptySymbols;
};

} // namespace kex::semantic
