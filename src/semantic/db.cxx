#include "db.hxx"
#include "analyzer.hxx"
#include "collect_pass.hxx"
#include "resolve_pass.hxx"
#include "symbol_queries.hxx"
#include "../compiled/expand.hxx"
#include "../lexer/lexer.hxx"
#include "../module/resolver.hxx"
#include <algorithm>
#include <fstream>
#include <map>
#include <iterator>
#include "../parser/parser.hxx"
#include <stdexcept>

namespace kex::semantic {

const std::vector<Diagnostic> SemanticDB::s_emptyDiagnostics;
const std::vector<SymbolInfo> SemanticDB::s_emptySymbols;

auto SemanticDB::updateFile(const std::string& path, std::string source,
                            const std::vector<std::string>& companionDeclFiles)
    -> void {
    FileState& state = m_files[path];
    state.path = path;
    state.source = source;
    state.version++;
    state.diagnostics.clear();
    state.symbols.clear();
    state.completionScopes.clear();

    // Parse — errors are recovered from internally; the partial AST is still
    // analyzable for the well-formed portions of the file.
    bool fatalParseError = false;
    {
        Lexer lexer(source, state.path);
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
    if (fatalParseError) {
        state.sourceDiagnostics = state.diagnostics;
        return;
    }

    // The DB re-reads and re-parses the file itself, so it would otherwise see
    // the program BEFORE compile-time expansion and report every generated
    // declaration as an undefined name — even though codegen is fine, because
    // main.cxx expands the AST it compiles. Run the same pass here so both
    // views agree. Keep expansion diagnostics for non-CLI consumers such as
    // the language server. The CLI exits while expanding its primary AST, so
    // these do not create duplicate terminal diagnostics.
    //
    // This does mean a file with `compiled` blocks is expanded twice per
    // build. Acceptable while expansion is cheap; the alternative is threading
    // the already-expanded declarations in through updateFile's signature.
    {
        std::vector<Diagnostic> expansionDiagnostics;
        compiled::ExpandOptions expandOptions;
        expandOptions.sourcePath = state.path;
        expandOptions.moduleRoots = m_moduleRoots;
        compiled::expand(state.ast, expansionDiagnostics, expandOptions);
        state.diagnostics.insert(
            state.diagnostics.end(),
            std::make_move_iterator(expansionDiagnostics.begin()),
            std::make_move_iterator(expansionDiagnostics.end()));
    }
    state.sourceDiagnostics = state.diagnostics;

    // Companion declarations (see the header): parse each and prepend
    // everything except its main code, so the collect/resolve passes below
    // treat them as part of this file's unit. Parse diagnostics from a
    // companion are dropped — it is checked on its own when run directly.
    for (const auto& companion : companionDeclFiles) {
        std::ifstream input(companion, std::ios::binary);
        if (!input) continue;
        std::string companionSource((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
        Lexer companionLexer(std::move(companionSource), companion);
        Parser companionParser(companionLexer.tokenizeAll(), companion);
        auto companionAst = companionParser.parseProgram();

        std::vector<ast::TopLevelItem> merged;
        merged.reserve(companionAst.items.size() + state.ast.items.size());
        for (auto& item : companionAst.items)
            if (ast::isMergedDeclaration(item))
                merged.push_back(std::move(item));
        for (auto& item : state.ast.items) merged.push_back(std::move(item));
        state.ast.items = std::move(merged);
    }

    // Every name it declares, top-level and in its modules.
    state.symbols = collectSymbols(state.ast, state.path);
}

auto SemanticDB::setResolved(const std::string& path, std::vector<SymbolInfo> symbols,
                             std::vector<FileState::ScopeSnapshot> scopes) -> void {
    auto found = m_files.find(path);
    if (found == m_files.end()) return;
    found->second.symbols = std::move(symbols);
    found->second.completionScopes = std::move(scopes);
}

auto SemanticDB::resolveTogether(const std::vector<std::string>& paths) -> void {
    std::vector<SymbolInfo> all;
    std::vector<std::string> owners;  // the file each symbol is indexed under
    for (const auto& [path, state] : m_files)
        for (const auto& symbol : state.symbols) {
            all.push_back(symbol);
            owners.push_back(path);
        }
    ProgramModules shared(std::move(all));
    std::map<std::string, std::vector<FileState::ScopeSnapshot>> scopes;
    for (const auto& path : paths)
        if (auto* state = fileState(path))
            for (auto& diagnostic :
                 ResolvePass(m_imports).run(state->ast, path, shared,
                                            {.scopes = &scopes[path],
                                             .checksModuleRules = true,
                                             .fileLocalTopLevel = true}))
                state->diagnostics.push_back(std::move(diagnostic));
    // Each file's symbols back to it, with the references now on them.
    std::map<std::string, std::vector<SymbolInfo>> byFile;
    auto& resolved = shared.symbols();
    for (size_t i = 0; i < resolved.size(); ++i)
        byFile[owners[i]].push_back(std::move(resolved[i]));
    for (const auto& path : paths) byFile.try_emplace(path);
    for (auto& [path, symbols] : byFile) {
        auto fileScopes = scopes.contains(path) ? std::move(scopes[path])
                                                : m_files[path].completionScopes;
        setResolved(path, std::move(symbols), std::move(fileScopes));
    }
}

auto SemanticDB::removeFile(const std::string& path) -> void {
    m_files.erase(path);
}

auto SemanticDB::setModuleRoots(std::vector<std::string> roots) -> void {
    m_moduleRoots = std::move(roots);
}

auto SemanticDB::ensureModule(const std::string& moduleName,
                              const std::string& currentModule) -> std::optional<std::string> {
    module::Resolver resolver(m_moduleRoots);
    auto resolution = resolver.resolve(moduleName, currentModule);
    if (!resolution) return std::nullopt;
    if (hasModule(resolution->moduleName)) return resolution->moduleName;
    if (!m_loadingModules.insert(resolution->moduleName).second)
        return resolution->moduleName;

    std::ifstream input(resolution->path);
    std::string source((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    updateFile(resolution->path, std::move(source));
    m_loadingModules.erase(resolution->moduleName);
    if (hasModule(resolution->moduleName)) return resolution->moduleName;

    // The relative candidate did not pan out. `Mid.Leaf` can match a FILE —
    // the container heuristic in Resolver::resolve accepts `mid.kex` for a
    // nested `Mid.Leaf`, because that is how `module Shop do module Cart`
    // works — without that file actually defining the nested module. When it
    // does not, the absolute name is still owed a look: `using Leaf` inside
    // `module Mid` means the top-level `Leaf` whenever `Mid.Leaf` is not a
    // real module.
    //
    // Without this, a library whose own module imports another module
    // resolved to a name nothing defines, and reported the module it was
    // standing in as missing.
    if (resolution->moduleName != moduleName) {
        module::Resolver absolute(m_moduleRoots);
        if (auto plain = absolute.resolve(moduleName, "")) {
            if (hasModule(plain->moduleName)) return plain->moduleName;
            if (m_loadingModules.insert(plain->moduleName).second) {
                std::ifstream plainInput(plain->path);
                std::string plainSource((std::istreambuf_iterator<char>(plainInput)),
                                        std::istreambuf_iterator<char>());
                updateFile(plain->path, std::move(plainSource), {});
                m_loadingModules.erase(plain->moduleName);
            }
            if (hasModule(plain->moduleName)) return plain->moduleName;
        }
    }
    return std::nullopt;
}

auto SemanticDB::diagnosticsFor(const std::string& file) const -> const std::vector<Diagnostic>& {
    auto it = m_files.find(file);
    return it != m_files.end() ? it->second.diagnostics : s_emptyDiagnostics;
}

auto SemanticDB::symbolsFor(const std::string& file) const -> const std::vector<SymbolInfo>& {
    auto it = m_files.find(file);
    return it != m_files.end() ? it->second.symbols : s_emptySymbols;
}

auto SemanticDB::hasModule(const std::string& moduleName) const -> bool {
    for (const auto& [_, state] : m_files)
        for (const auto& symbol : state.symbols)
            if (symbol.kind == SymbolKind::Module) {
                const auto qualified =
                    symbol.module.empty()
                        ? symbol.name
                        : symbol.module + "." + symbol.name;
                if (qualified == moduleName) return true;
            }
    return false;
}

auto SemanticDB::indexed() const -> IndexedFiles {
    IndexedFiles files;
    files.reserve(m_files.size());
    // The queries hand back symbols the resolve pass records references on.
    for (auto& [path, state] : const_cast<std::unordered_map<std::string, FileState>&>(m_files))
        files.push_back({path, &state.symbols, &state.completionScopes});
    return files;
}

auto SemanticDB::symbolInModule(const std::string& moduleName,
                                const std::string& name) -> SymbolInfo* {
    return semantic::symbolInModule(indexed(), moduleName, name);
}

auto SemanticDB::receiverSymbol(const std::string& receiver,
                                const std::string& name) const -> const SymbolInfo* {
    return semantic::receiverSymbol(indexed(), receiver, name);
}

auto SemanticDB::findSymbol(const std::string& name,
                            const std::string& preferFile) -> SymbolInfo* {
    return semantic::findSymbol(indexed(), name, preferFile);
}

auto SemanticDB::findSymbol(const std::string& name,
                            const std::string& preferFile) const -> const SymbolInfo* {
    return semantic::findSymbol(indexed(), name, preferFile);
}

auto SemanticDB::symbolAt(const std::string& file, uint32_t line, uint32_t col) const
    -> const SymbolInfo* {
    return semantic::symbolAt(indexed(), file, line, col);
}

auto SemanticDB::completionsFor(const std::string& prefix) const -> std::vector<std::string> {
    return semantic::completionsFor(indexed(), prefix, m_imports);
}

auto SemanticDB::completionsAt(const std::string& file, uint32_t line, uint32_t col,
                               const std::string& prefix) const -> std::vector<std::string> {
    return semantic::completionsAt(indexed(), file, line, col, prefix, m_imports);
}

auto SemanticDB::fileState(const std::string& path) -> FileState* {
    auto it = m_files.find(path);
    return it != m_files.end() ? &it->second : nullptr;
}

} // namespace kex::semantic
