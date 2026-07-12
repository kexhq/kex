#include "db.hxx"
#include "analyzer.hxx"
#include "collect_pass.hxx"
#include "resolve_pass.hxx"
#include "../lexer/lexer.hxx"
#include "../module/resolver.hxx"
#include <algorithm>
#include <fstream>
#include "../parser/parser.hxx"
#include <stdexcept>

namespace kex::semantic {

const std::vector<Diagnostic> SemanticDB::s_emptyDiagnostics;
const std::vector<SymbolInfo> SemanticDB::s_emptySymbols;
const std::vector<std::string> SemanticDB::s_emptyPaths;

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
        Lexer lexer(std::move(source), state.path);
        auto tokens = lexer.tokenizeAll();
        bool noTokens = tokens.empty();
        Parser parser(std::move(tokens), state.path);
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
    m_resolvingFiles.insert(path);
    ResolvePass resolve;
    resolve.run(*this, path);
    m_resolvingFiles.erase(path);
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

auto SemanticDB::setModuleRoots(std::vector<std::string> roots) -> void {
    m_moduleRoots = std::move(roots);
}

auto SemanticDB::ensureModule(const std::string& moduleName,
                              const std::string& currentModule) -> std::optional<std::string> {
    module::Resolver resolver(m_moduleRoots);
    auto resolution = resolver.resolve(moduleName, currentModule);
    if (!resolution) return std::nullopt;
    m_shadowedModulePaths[resolution->moduleName] = resolution->shadowedPaths;
    if (hasModule(resolution->moduleName)) return resolution->moduleName;
    if (!m_loadingModules.insert(resolution->moduleName).second)
        return resolution->moduleName;

    std::ifstream input(resolution->path);
    std::string source((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    updateFile(resolution->path, std::move(source));
    m_loadingModules.erase(resolution->moduleName);
    if (!hasModule(resolution->moduleName)) return std::nullopt;
    return resolution->moduleName;
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

auto SemanticDB::hasModule(const std::string& moduleName) const -> bool {
    for (const auto& [_, state] : m_files)
        for (const auto& symbol : state.symbols)
            if (symbol.kind == SymbolKind::Module && symbol.name == moduleName) return true;
    return false;
}

auto SemanticDB::symbolInModule(const std::string& moduleName,
                                const std::string& name) -> SymbolInfo* {
    for (auto& [_, state] : m_files)
        for (auto& symbol : state.symbols)
            if (symbol.module == moduleName && symbol.name == name) return &symbol;
    return nullptr;
}

auto SemanticDB::isModuleLoading(const std::string& moduleName,
                                 const std::string& fromFile) const -> bool {
    if (m_loadingModules.count(moduleName)) return true;
    for (const auto& path : m_resolvingFiles) {
        if (path == fromFile) continue;
        auto it = m_files.find(path);
        if (it == m_files.end()) continue;
        for (const auto& sym : it->second.symbols)
            if (sym.kind == SymbolKind::Module && sym.name == moduleName)
                return true;
    }
    return false;
}

auto SemanticDB::shadowedModulePaths(const std::string& moduleName) const
    -> const std::vector<std::string>& {
    const auto found = m_shadowedModulePaths.find(moduleName);
    return found == m_shadowedModulePaths.end() ? s_emptyPaths : found->second;
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

auto SemanticDB::completionsFor(const std::string& prefix) const -> std::vector<std::string> {
    std::vector<std::string> results;

    auto dotPos = prefix.rfind('.');
    if (dotPos != std::string::npos) {
        // "Module.mem" or "Type.mem" — complete members by module name or make target
        std::string qualifier = prefix.substr(0, dotPos);
        std::string memberPrefix = prefix.substr(dotPos + 1);
        for (const auto& [path, state] : m_files) {
            for (const auto& sym : state.symbols) {
                bool matchesMod = (sym.module == qualifier)
                    || (sym.module.size() > qualifier.size()
                        && sym.module.compare(sym.module.size() - qualifier.size(),
                                              qualifier.size(), qualifier) == 0
                        && sym.module[sym.module.size() - qualifier.size() - 1] == '.');
                bool matchesMake = (!sym.makeTarget.empty() && sym.makeTarget == qualifier);
                if ((matchesMod || matchesMake) && sym.isExported
                    && sym.name.rfind(memberPrefix, 0) == 0) {
                    results.push_back(qualifier + "." + sym.name);
                }
            }
        }
    } else {
        // Top-level names only — module-scoped and make-scoped symbols
        // require a dot qualifier (e.g. Math.sin, List.map)
        for (const auto& [path, state] : m_files) {
            for (const auto& sym : state.symbols) {
                if (!sym.module.empty() || !sym.makeTarget.empty()) continue;
                if (sym.name.rfind(prefix, 0) == 0) {
                    results.push_back(sym.name);
                }
            }
        }
    }

    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    return results;
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
