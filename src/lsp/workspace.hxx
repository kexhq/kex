#pragma once
// The language server's documents: each file parsed and expanded, with the
// symbols it declares, and the modules it names loaded from the source roots.
// It analyzes nothing itself: a file's references and scopes are the
// checker's (setResolved, resolveTogether), and the queries are
// semantic/symbol_queries.hxx's over these files.
#include "../ast/ast.hxx"
#include "../semantic/analyzer.hxx"
#include "../semantic/imported_interfaces.hxx"
#include "../semantic/symbol_info.hxx"
#include "../semantic/symbol_queries.hxx"
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kex::lsp {

using semantic::Diagnostic;
using semantic::ImportedInterfaces;
using semantic::IndexedFiles;
using semantic::ScopeSnapshot;
using semantic::SymbolInfo;
using semantic::TypeChecker;

struct FileState {
    std::string path;
    std::string source;
    uint64_t version = 0;
    ast::Program ast;
    std::vector<Diagnostic> diagnostics;
    // The parser's and compile-time expansion's, the part of `diagnostics`
    // no sema produces: the language server adds the one sema's to them.
    std::vector<Diagnostic> sourceDiagnostics;
    std::vector<SymbolInfo> symbols;
    std::vector<ScopeSnapshot> completionScopes;
};

class Workspace {
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
    //
    // It indexes: parse, compile-time expansion, collected symbols and module
    // exports. Names are resolved by the checker (setResolved, or
    // resolveTogether for files resolved as one program).
    auto updateFile(const std::string& path, std::string source,
                    const std::vector<std::string>& companionDeclFiles = {}) -> void;
    auto removeFile(const std::string& path) -> void;
    // Indexes each of the files at `paths` it can read.
    auto indexFiles(const std::vector<std::string>& paths) -> void;
    // `path`'s symbols, with their references and the names in scope at each
    // point, as the checker resolved them (Analyzer::nameIndex), in place of
    // the index's own pass: an open document is resolved once.
    auto setResolved(const std::string& path, std::vector<SymbolInfo> symbols,
                     std::vector<ScopeSnapshot> scopes) -> void;
    // The references several files make, recorded on every indexed file's
    // symbols at once: a reference in one file to a declaration in another
    // lands on it (a workspace's references). Each of `checked` is a file's
    // path and the checker that checked its AST with its names recorded.
    auto resolveTogether(
        const std::vector<std::pair<std::string, const TypeChecker*>>& checked) -> void;
    auto setModuleRoots(std::vector<std::string> roots) -> void;
    auto ensureModule(const std::string& moduleName,
                      const std::string& currentModule = "") -> std::optional<std::string>;

    auto diagnosticsFor(const std::string& file) const -> const std::vector<Diagnostic>&;
    auto symbolsFor(const std::string& file) const -> const std::vector<SymbolInfo>&;
    auto hasModule(const std::string& moduleName) const -> bool;
    auto symbolInModule(const std::string& moduleName,
                        const std::string& name) -> SymbolInfo*;
    auto receiverSymbol(const std::string& receiver,
                        const std::string& name) const -> const SymbolInfo*;

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

private:
    // Its documents as the symbol queries read them (symbol_queries.hxx).
    auto indexed() const -> IndexedFiles;

    std::unordered_map<std::string, FileState> m_files;
    std::vector<std::string> m_moduleRoots{"lib", "src"};
    std::unordered_set<std::string> m_loadingModules;
    const ImportedInterfaces* m_imports = nullptr;

    static const std::vector<Diagnostic> s_emptyDiagnostics;
    static const std::vector<SymbolInfo> s_emptySymbols;
};

} // namespace kex::lsp
