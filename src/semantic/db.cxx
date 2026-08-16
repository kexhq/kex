#include "db.hxx"
#include "analyzer.hxx"
#include "collect_pass.hxx"
#include "resolve_pass.hxx"
#include "../compiled/expand.hxx"
#include "../lexer/lexer.hxx"
#include "../module/resolver.hxx"
#include <algorithm>
#include <fstream>
#include <iterator>
#include "../parser/parser.hxx"
#include <stdexcept>

namespace kex::semantic {

const std::vector<Diagnostic> SemanticDB::s_emptyDiagnostics;
const std::vector<SymbolInfo> SemanticDB::s_emptySymbols;
const std::vector<std::string> SemanticDB::s_emptyPaths;

auto SemanticDB::updateFile(const std::string& path, std::string source,
                            const std::vector<std::string>& companionDeclFiles)
    -> void {
    // Module export entries point directly into FileState::symbols. An editor
    // updates the same file repeatedly, so discard those pointers while the
    // old vector is still alive, before clear()/reallocation can invalidate
    // them. Batch compiler use rarely exercised this lifecycle; LSP use does
    // on every keystroke.
    if (m_files.contains(path)) {
        for (auto& [_, exports] : m_moduleExports) {
            exports.erase(
                std::remove_if(exports.begin(), exports.end(),
                               [&](const SymbolInfo* symbol) {
                                   return symbol->definition.file == path;
                               }),
                exports.end());
        }
    }
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
    if (fatalParseError) return;

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
        compiled::expand(state.ast, expansionDiagnostics);
        state.diagnostics.insert(
            state.diagnostics.end(),
            std::make_move_iterator(expansionDiagnostics.begin()),
            std::make_move_iterator(expansionDiagnostics.end()));
    }

    // Companion declarations (see the header): parse each and prepend
    // everything except its `main` block, so the collect/resolve passes below
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
            if (!std::holds_alternative<std::unique_ptr<ast::MainBlock>>(item))
                merged.push_back(std::move(item));
        for (auto& item : state.ast.items) merged.push_back(std::move(item));
        state.ast.items = std::move(merged);
    }

    // Pass 1: collect all top-level names
    CollectPass collect;
    collect.run(*this, path);

    // Update cross-file module export index
    rebuildModuleExports(path);

    // Pass 2: resolve name references, report undefined names
    m_resolvingFiles.insert(path);
    ResolvePass resolve(m_imports);
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
            if (symbol.kind == SymbolKind::Module) {
                const auto qualified =
                    symbol.module.empty()
                        ? symbol.name
                        : symbol.module + "." + symbol.name;
                if (qualified == moduleName) return true;
            }
    return false;
}

auto SemanticDB::symbolInModule(const std::string& moduleName,
                                const std::string& name) -> SymbolInfo* {
    for (auto& [_, state] : m_files)
        for (auto& symbol : state.symbols)
            if (symbol.module == moduleName && symbol.name == name) return &symbol;
    return nullptr;
}

auto SemanticDB::receiverSymbol(const std::string& receiver,
                                const std::string& name) const
    -> const SymbolInfo* {
    for (const auto& [_, state] : m_files)
        for (const auto& symbol : state.symbols)
            if (symbol.name == name && symbol.makeTarget == receiver)
                return &symbol;
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
            if (sym.kind == SymbolKind::Module) {
                const auto qualified =
                    sym.module.empty()
                        ? sym.name
                        : sym.module + "." + sym.name;
                if (qualified == moduleName) return true;
            }
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
    auto contains = [line, col](const SourceLocation& location,
                                std::string_view name) {
        return location.line == static_cast<int>(line) &&
               col >= static_cast<uint32_t>(location.column) &&
               col < static_cast<uint32_t>(location.column) + name.size();
    };
    for (const auto& [path, state] : m_files) {
        for (const auto& sym : state.symbols) {
            // Check definition site
            if (sym.definition.file == file && contains(sym.definition, sym.name)) {
                return &sym;
            }
            // Check reference sites
            for (const auto& ref : sym.references) {
                if (ref.file == file && contains(ref, sym.name)) {
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
                // A record's fields and a make block's methods carry the
                // module they were DECLARED in as well as the type they belong
                // to. They are members of the type, not of the module, so
                // `Kex.Kernel.` must not offer `major`/`number`/`tuple` —
                // those complete under `Version.`, which is what makeTarget
                // matches below.
                bool matchesMod = sym.module == qualifier && sym.makeTarget.empty();
                bool matchesMake = (!sym.makeTarget.empty() && sym.makeTarget == qualifier);
                if ((matchesMod || matchesMake) && sym.isExported
                    && sym.name.rfind(memberPrefix, 0) == 0) {
                    results.push_back(qualifier + "." + sym.name);
                }
            }
        }
        if (m_imports) {
            // Installed/compiled standard-library modules are represented by
            // ImportedInterfaces rather than FileState entries.  Include both
            // their exports (`FS.File.read`) and immediate nested modules
            // (`FS.` -> `FS.File`, `FS.Directory`) in editor completion.
            if (auto imported = m_imports->modules.find(qualifier);
                imported != m_imports->modules.end()) {
                for (const auto& [name, overloads] : imported->second.exports)
                    if (!overloads.empty() && name.rfind(memberPrefix, 0) == 0)
                        results.push_back(qualifier + "." + name);
            }
            // Instance methods of a type that came from a COMPILED interface.
            // The branch above offers a module's exports, which covers static
            // members (`Web.Server.new`), but a `make Server do` block inside
            // `module Web` lands in the name-indexed receiver table instead —
            // so `Web.Server.` offered nothing for `get`/`post`/`start`, and a
            // builder chain on such a type completed to nothing at all.
            for (const auto& [name, overloads] : m_imports->receiverFunctions) {
                if (name.rfind(memberPrefix, 0) != 0) continue;
                for (const auto& overload : overloads) {
                    if (overload.signature.params.empty()) continue;
                    const auto receiver =
                        typeToString(overload.signature.params.front());
                    // A value of this type reports it QUALIFIED
                    // (`Web.Server`), while the interface records the receiver
                    // as written inside its own module (`Server`). The owning
                    // module cannot be joined back on to compare them: every
                    // standard-library export carries sourceModule "Prelude",
                    // not "Web". So the last segment is what there is to match
                    // on. Two modules each declaring a `Server` would offer
                    // each other's methods here — acceptable for a completion
                    // list, and the alternative today is offering nothing at
                    // all for any module-qualified type.
                    const auto tail = qualifier.rfind('.') == std::string::npos
                        ? qualifier : qualifier.substr(qualifier.rfind('.') + 1);
                    const bool matches = receiver == qualifier || receiver == tail;
                    if (!matches) continue;
                    results.push_back(qualifier + "." + name);
                    break;
                }
            }
            const auto nestedPrefix = qualifier + ".";
            for (const auto& [moduleName, interface] : m_imports->modules) {
                if (moduleName.rfind(nestedPrefix, 0) != 0) continue;
                const auto remainder = moduleName.substr(nestedPrefix.size());
                const auto separator = remainder.find('.');
                const auto child = remainder.substr(0, separator);
                if (child.rfind(memberPrefix, 0) == 0)
                    results.push_back(nestedPrefix + child);
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

auto SemanticDB::completionsAt(const std::string& file, uint32_t line,
                               uint32_t col, const std::string& prefix) const
    -> std::vector<std::string> {
    auto results = completionsFor(prefix);
    // Qualified lookup is handled by module/make-target exports above. Scope
    // snapshots contain bare lexical names and must not be offered after '.'.
    if (prefix.find('.') != std::string::npos) return results;

    const auto state = m_files.find(file);
    if (state == m_files.end()) return results;
    const FileState::ScopeSnapshot* nearest = nullptr;
    for (const auto& snapshot : state->second.completionScopes) {
        const bool before = snapshot.location.line < static_cast<int>(line) ||
            (snapshot.location.line == static_cast<int>(line) &&
             snapshot.location.column <= static_cast<int>(col));
        if (!before) continue;
        if (!nearest || snapshot.location.line > nearest->location.line ||
            (snapshot.location.line == nearest->location.line &&
             snapshot.location.column >= nearest->location.column))
            nearest = &snapshot;
    }
    if (nearest)
        for (const auto& name : nearest->names)
            if (name.rfind(prefix, 0) == 0) results.push_back(name);

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
