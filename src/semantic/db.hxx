#pragma once

#include "../ast/ast.hxx"
#include "imported_interfaces.hxx"
#include "symbol.hxx"
#include "types.hxx"
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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
    std::string makeTarget; // set for functions inside a `make TypeName do` block
    // Kex source-shaped declaration text used by editor hover. This is
    // populated when syntax alone carries useful structure (notably records)
    // and enriched with inferred function signatures after type checking.
    std::string detail;
};

struct FileState {
    std::string path;
    std::string source;
    uint64_t version = 0;
    ast::Program ast;
    std::vector<Diagnostic> diagnostics;
    std::vector<SymbolInfo> symbols;
    // Names visible at expression boundaries, retained by ResolvePass for
    // position-aware editor completion after its transient scope stack is
    // gone. Entries are in source traversal order.
    struct ScopeSnapshot {
        SourceLocation location;
        std::vector<std::string> names;
    };
    std::vector<ScopeSnapshot> completionScopes;
};

class SemanticDB {
public:
    auto setImportedInterfaces(const ImportedInterfaces* ifaces) -> void {
        m_imports = ifaces;
    }
    // `companionDeclFiles` names files whose top-level declarations belong to
    // the same compilation unit as `path` — the `<name>.spec.kex` convention,
    // where the spec's types and functions are declared in `<name>.kex`.
    // Their declarations are merged into this file's AST before the passes
    // run; each node keeps its own source location, so diagnostics still
    // point into the file the code actually came from.
    auto updateFile(const std::string& path, std::string source,
                    const std::vector<std::string>& companionDeclFiles = {})
        -> void;
    auto removeFile(const std::string& path) -> void;
    auto setModuleRoots(std::vector<std::string> roots) -> void;
    auto ensureModule(const std::string& moduleName,
                      const std::string& currentModule = "") -> std::optional<std::string>;

    auto diagnosticsFor(const std::string& file) const -> const std::vector<Diagnostic>&;
    auto symbolsFor(const std::string& file) const -> const std::vector<SymbolInfo>&;
    auto exportsFor(const std::string& moduleName) const -> std::vector<SymbolInfo*>;
    auto hasModule(const std::string& moduleName) const -> bool;
    auto symbolInModule(const std::string& moduleName,
                        const std::string& name) -> SymbolInfo*;
    auto receiverSymbol(const std::string& receiver,
                        const std::string& name) const -> const SymbolInfo*;
    auto isModuleLoading(const std::string& moduleName,
                         const std::string& fromFile = "") const -> bool;
    auto shadowedModulePaths(const std::string& moduleName) const
        -> const std::vector<std::string>&;

    // Returns true if `name` appears as a file-level symbol in any indexed
    // file. This is an index query, not a name-resolution rule: top-level Kex
    // definitions remain file-local.
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

    // Returns all known symbol names (across all indexed files) whose name
    // starts with `prefix`. Used for REPL tab completion.
    auto completionsFor(const std::string& prefix) const -> std::vector<std::string>;
    auto completionsAt(const std::string& file, uint32_t line, uint32_t col,
                       const std::string& prefix) const
        -> std::vector<std::string>;

    // Access to raw file state for passes
    auto fileState(const std::string& path) -> FileState*;
    auto fileState(const std::string& path) const -> const FileState*;

private:
    auto rebuildModuleExports(const std::string& path) -> void;

    std::unordered_map<std::string, FileState> m_files;
    std::unordered_map<std::string, std::vector<SymbolInfo*>> m_moduleExports;
    std::vector<std::string> m_moduleRoots{"lib", "src"};
    std::unordered_set<std::string> m_loadingModules;
    const ImportedInterfaces* m_imports = nullptr;
    std::unordered_set<std::string> m_resolvingFiles;
    std::unordered_map<std::string, std::vector<std::string>> m_shadowedModulePaths;

    static const std::vector<Diagnostic> s_emptyDiagnostics;
    static const std::vector<SymbolInfo> s_emptySymbols;
    static const std::vector<std::string> s_emptyPaths;
};

} // namespace kex::semantic
