#include "db.hxx"
#include "analyzer.hxx"
#include "collect_pass.hxx"
#include "resolve_pass.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <stdexcept>

namespace kex::semantic {

const std::vector<Diagnostic> SemanticDB::s_emptyDiagnostics;
const std::vector<SymbolInfo> SemanticDB::s_emptySymbols;

auto SemanticDB::updateFile(const std::string& path, std::string source) -> void {
    FileState& state = m_files[path];
    state.path = path;
    state.version++;
    state.diagnostics.clear();
    state.symbols.clear();

    // Parse
    try {
        Lexer lexer(std::move(source), path);
        auto tokens = lexer.tokenizeAll();
        Parser parser(std::move(tokens), path);
        state.ast = parser.parseProgram();
    } catch (const std::exception& e) {
        state.diagnostics.push_back(Diagnostic{
            Diagnostic::Level::Error,
            SourceLocation{path, 1, 1},
            std::string("Parse error: ") + e.what()
        });
        return;
    }

    // Pass 1: collect all top-level names
    CollectPass collect;
    collect.run(*this, path);

    // Update cross-file module export index
    rebuildModuleExports(path);

    // Pass 2: resolve name references, report undefined names
    ResolvePass resolve;
    resolve.run(*this, path);
}

auto SemanticDB::removeFile(const std::string& path) -> void {
    m_files.erase(path);
    // Remove any module exports that pointed into this file
    for (auto& [mod, ptrs] : m_moduleExports) {
        ptrs.erase(
            std::remove_if(ptrs.begin(), ptrs.end(), [&](const SymbolInfo* s) {
                return s->definition.file == path;
            }),
            ptrs.end());
    }
}

auto SemanticDB::diagnosticsFor(const std::string& file) const -> const std::vector<Diagnostic>& {
    auto it = m_files.find(file);
    return it != m_files.end() ? it->second.diagnostics : s_emptyDiagnostics;
}

auto SemanticDB::symbolsFor(const std::string& file) const -> const std::vector<SymbolInfo>& {
    auto it = m_files.find(file);
    return it != m_files.end() ? it->second.symbols : s_emptySymbols;
}

auto SemanticDB::exportsFor(const std::string& moduleName) const -> std::vector<SymbolInfo*> {
    auto it = m_moduleExports.find(moduleName);
    return it != m_moduleExports.end() ? it->second : std::vector<SymbolInfo*>{};
}

auto SemanticDB::fileState(const std::string& path) -> FileState* {
    auto it = m_files.find(path);
    return it != m_files.end() ? &it->second : nullptr;
}

auto SemanticDB::fileState(const std::string& path) const -> const FileState* {
    auto it = m_files.find(path);
    return it != m_files.end() ? &it->second : nullptr;
}

auto SemanticDB::rebuildModuleExports(const std::string& path) -> void {
    auto it = m_files.find(path);
    if (it == m_files.end()) return;

    // Remove stale pointers from this file
    for (auto& [mod, ptrs] : m_moduleExports) {
        ptrs.erase(
            std::remove_if(ptrs.begin(), ptrs.end(), [&](const SymbolInfo* s) {
                return s->definition.file == path;
            }),
            ptrs.end());
    }

    // Re-insert exported symbols from this file
    for (auto& sym : it->second.symbols) {
        if (!sym.module.empty() && sym.isExported) {
            m_moduleExports[sym.module].push_back(&sym);
        }
    }
}

} // namespace kex::semantic
