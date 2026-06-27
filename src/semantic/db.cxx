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

    // Parse — errors are recovered from internally; the partial AST is still
    // analyzable for the well-formed portions of the file.
    bool fatalParseError = false;
    {
        Lexer lexer(std::move(source), path);
        auto tokens = lexer.tokenizeAll();
        bool noTokens = tokens.empty();
        Parser parser(std::move(tokens), path);
        state.ast = parser.parseProgram();
        for (const auto& pd : parser.diagnostics()) {
            state.diagnostics.push_back(Diagnostic{
                Diagnostic::Level::Error,
                pd.location,
                pd.message
            });
        }
        // Empty token stream (e.g. lexer completely failed) means the AST
        // is also empty — no point running passes.
        fatalParseError = noTokens && !parser.diagnostics().empty();
    }
    if (fatalParseError) return;

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

auto SemanticDB::isGloballyKnown(const std::string& name) const -> bool {
    for (const auto& [path, state] : m_files) {
        for (const auto& sym : state.symbols) {
            if (sym.name == name) return true;
        }
    }
    return false;
}

auto SemanticDB::findSymbol(const std::string& name,
                             const std::string& preferFile) -> SymbolInfo* {
    // Search preferred file first
    if (!preferFile.empty()) {
        auto it = m_files.find(preferFile);
        if (it != m_files.end()) {
            for (auto& sym : it->second.symbols) {
                if (sym.name == name) return &sym;
            }
        }
    }
    for (auto& [path, state] : m_files) {
        if (path == preferFile) continue;
        for (auto& sym : state.symbols) {
            if (sym.name == name) return &sym;
        }
    }
    return nullptr;
}

auto SemanticDB::findSymbol(const std::string& name,
                             const std::string& preferFile) const -> const SymbolInfo* {
    if (!preferFile.empty()) {
        auto it = m_files.find(preferFile);
        if (it != m_files.end()) {
            for (const auto& sym : it->second.symbols) {
                if (sym.name == name) return &sym;
            }
        }
    }
    for (const auto& [path, state] : m_files) {
        if (path == preferFile) continue;
        for (const auto& sym : state.symbols) {
            if (sym.name == name) return &sym;
        }
    }
    return nullptr;
}

auto SemanticDB::symbolAt(const std::string& file,
                           uint32_t line, uint32_t col) const -> const SymbolInfo* {
    for (const auto& [path, state] : m_files) {
        for (const auto& sym : state.symbols) {
            // Check definition site
            if (sym.definition.file == file
                    && sym.definition.line == line
                    && sym.definition.column == col) {
                return &sym;
            }
            // Check reference sites
            for (const auto& ref : sym.references) {
                if (ref.file == file && ref.line == line && ref.column == col) {
                    return &sym;
                }
            }
        }
    }
    return nullptr;
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
