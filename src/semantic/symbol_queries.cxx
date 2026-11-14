#include "symbol_queries.hxx"

#include <algorithm>

namespace kex::semantic {

auto symbolInModule(const IndexedFiles& files, const std::string& moduleName,
                    const std::string& name) -> SymbolInfo* {
    for (const auto& file : files)
        for (auto& symbol : *file.symbols)
            if (symbol.module == moduleName && symbol.name == name) return &symbol;
    return nullptr;
}

auto receiverSymbol(const IndexedFiles& files, const std::string& receiver,
                    const std::string& name)
    -> const SymbolInfo* {
    const SymbolInfo* best = nullptr;
    for (const auto& file : files)
        for (const auto& symbol : *file.symbols)
            if (symbol.name == name && symbol.makeTarget == receiver) {
                if (!best || (best->detail.empty() && !symbol.detail.empty()) ||
                    (best->definition.line <= 0 && symbol.definition.line > 0))
                    best = &symbol;
            }
    return best;
}

auto findSymbol(const IndexedFiles& files, const std::string& name,
                const std::string& preferFile) -> SymbolInfo* {
    // The preferred file first, then every other in order.
    for (const bool preferred : {true, false})
        for (const auto& file : files) {
            if ((file.path == preferFile) != preferred) continue;
            for (auto& symbol : *file.symbols)
                if (symbol.name == name) return &symbol;
        }
    return nullptr;
}

auto symbolAt(const IndexedFiles& files, const std::string& file,
              uint32_t line, uint32_t col) -> const SymbolInfo* {
    auto contains = [line, col](const SourceLocation& location,
                                std::string_view name) {
        return location.line == static_cast<int>(line) &&
               col >= static_cast<uint32_t>(location.column) &&
               col < static_cast<uint32_t>(location.column) + name.size();
    };
    for (const auto& indexed : files) {
        for (const auto& sym : *indexed.symbols) {
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

auto completionsFor(const IndexedFiles& files, const std::string& prefix,
                    const ImportedInterfaces* imports) -> std::vector<std::string> {
    std::vector<std::string> results;

    auto dotPos = prefix.rfind('.');
    if (dotPos != std::string::npos) {
        // "Module.mem" or "Type.mem" — complete members by module name or make target
        std::string qualifier = prefix.substr(0, dotPos);
        std::string memberPrefix = prefix.substr(dotPos + 1);
        // A typed server exposes the declared slots of its STATE in addition
        // to Server's own lifecycle methods. Slot identity is indexed
        // explicitly so ordinary `make Counter` helpers never leak into the
        // remote protocol completion list.
        std::string servingState;
        if (qualifier.rfind("Server<", 0) == 0 && qualifier.ends_with('>'))
            servingState = qualifier.substr(7, qualifier.size() - 8);
        for (const auto& file : files) {
            for (const auto& sym : *file.symbols) {
                // A record's fields and a make block's methods carry the
                // module they were DECLARED in as well as the type they belong
                // to. They are members of the type, not of the module, so
                // `Kex.` must not offer `major`/`number`/`tuple` —
                // those complete under `Version.`, which is what makeTarget
                // matches below.
                bool matchesMod = sym.module == qualifier && sym.makeTarget.empty();
                // A make target declared inside a module is written bare
                // there (`make Circle` in `module Shapes`), while a receiver
                // reached from ANOTHER file is typed by its qualified
                // identity (`Shapes.Circle`) — so `circle.` offered nothing
                // across a file boundary while working within one file.
                bool matchesMake = !sym.makeTarget.empty() &&
                    (sym.makeTarget == qualifier ||
                     (!sym.module.empty() &&
                      sym.module + "." + sym.makeTarget == qualifier));
                if ((matchesMod || matchesMake) && sym.isExported
                    && sym.name.rfind(memberPrefix, 0) == 0) {
                    results.push_back(qualifier + "." + sym.name);
                }
                if (!servingState.empty() && sym.isServingSlot &&
                    sym.makeTarget == servingState && sym.isExported &&
                    sym.name.rfind(memberPrefix, 0) == 0)
                    results.push_back(qualifier + "." + sym.name);
            }
        }
        if (imports) {
            // Installed/compiled standard-library modules are represented by
            // ImportedInterfaces rather than FileState entries.  Include both
            // their exports (`FS.File.read`) and immediate nested modules
            // (`FS.` -> `FS.File`, `FS.Directory`) in editor completion.
            if (auto imported = imports->modules.find(qualifier);
                imported != imports->modules.end()) {
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
            for (const auto& [name, overloads] : imports->receiverFunctions) {
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
                    const auto generic = tail.find('<');
                    const auto base = generic == std::string::npos
                        ? tail : tail.substr(0, generic);
                    const auto receiverGeneric = receiver.find('<');
                    const auto receiverBase = receiverGeneric == std::string::npos
                        ? receiver : receiver.substr(0, receiverGeneric);
                    const bool matches = receiver == qualifier || receiver == tail ||
                        (generic != std::string::npos &&
                         receiverGeneric != std::string::npos &&
                         receiverBase == base);
                    if (!matches) continue;
                    results.push_back(qualifier + "." + name);
                    break;
                }
            }
            const auto nestedPrefix = qualifier + ".";
            for (const auto& [moduleName, interface] : imports->modules) {
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
        for (const auto& file : files) {
            for (const auto& sym : *file.symbols) {
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

auto completionsAt(const IndexedFiles& files, const std::string& file, uint32_t line,
                   uint32_t col, const std::string& prefix, const ImportedInterfaces* imports)
    -> std::vector<std::string> {
    auto results = completionsFor(files, prefix, imports);
    // Qualified lookup is handled by module/make-target exports above. Scope
    // snapshots contain bare lexical names and must not be offered after '.'.
    if (prefix.find('.') != std::string::npos) return results;

    const auto state = std::find_if(files.begin(), files.end(),
                                    [&](const IndexedFile& indexed) { return indexed.path == file; });
    if (state == files.end() || !state->scopes) return results;
    const ScopeSnapshot* nearest = nullptr;
    for (const auto& snapshot : *state->scopes) {
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

} // namespace kex::semantic
