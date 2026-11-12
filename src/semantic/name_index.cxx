#include "name_index.hxx"

#include "collect_pass.hxx"
#include "module_rules.hxx"
#include "resolve_core.hxx"
#include "typechecker.hxx"

#include <algorithm>
#include <optional>
#include <tuple>
#include <unordered_map>

namespace kex::semantic {

namespace {

// The name an expression the checker recorded as a use spells.
auto usedName(const ast::Expr& expr) -> const std::string* {
    if (const auto* identifier = std::get_if<ast::Identifier>(&expr.kind)) return &identifier->name;
    if (const auto* upper = std::get_if<ast::UpperIdentifier>(&expr.kind)) return &upper->name;
    if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind)) return &call->name;
    if (const auto* tagged = std::get_if<ast::TaggedLiteral>(&expr.kind)) return &tagged->tag;
    if (const auto* capture = std::get_if<ast::CurryExpr>(&expr.kind)) return &capture->name;
    return nullptr;
}

// `Units.SI` of `Units.SI.kilo`.
auto owningModule(const std::string& qualified) -> std::string {
    const auto dot = qualified.rfind('.');
    return dot == std::string::npos ? std::string{} : qualified.substr(0, dot);
}

auto before(const SourceLocation& left, const SourceLocation& right) -> bool {
    return std::tie(left.file, left.line, left.column) <
           std::tie(right.file, right.line, right.column);
}

} // namespace

auto indexNames(const ast::Program& program, const std::string& path,
                const TypeChecker& checker, const ImportedInterfaces* imports,
                ProgramModules& modules, bool fileLocalTopLevel)
    -> std::vector<ScopeSnapshot> {
    const auto eachImported = [&](const std::string& within, const std::string& written,
                                  const std::optional<std::string>& alias,
                                  const std::vector<std::string>& only,
                                  const std::vector<std::string>& except, const auto& visit) {
        forEachImportedName(modules, imports, within, written, alias, only, except, visit);
    };

    // The names each context's `using`s bring in, and where each is from.
    struct Imported {
        std::vector<std::string> names;
        std::vector<resolve_core::ImportScope> origins{1};
    };
    const auto& contexts = checker.scopeContexts();
    std::vector<std::optional<Imported>> importedByContext(contexts.size());
    const auto importedIn = [&](std::size_t id) -> const Imported& {
        auto& imported = importedByContext[id];
        if (imported) return *imported;
        imported.emplace();
        for (const auto& import : contexts[id].imports) {
            if (import.alias) imported->names.push_back(*import.alias);
            eachImported(contexts[id].module, import.module, import.alias, import.onlyNames,
                         import.exceptNames,
                         [&](const std::string& name, const std::string& module, bool receiver) {
                             imported->names.push_back(name);
                             resolve_core::mergeImportOrigin(
                                 imported->origins, name,
                                 {module, !import.onlyNames.empty(), receiver});
                         });
        }
        return *imported;
    };

    std::vector<std::pair<SourceLocation, SymbolInfo*>> references;
    for (const auto& [expr, context] : checker.nameUses()) {
        const auto* name = usedName(*expr);
        if (!name) continue;
        // The declaration the checker bound it to, where it recorded one.
        std::string module;
        if (const auto called = checker.calledFunctions().find(expr);
            called != checker.calledFunctions().end())
            module = owningModule(called->second);
        else if (const auto constant = checker.referencedConstants().find(expr);
                 constant != checker.referencedConstants().end())
            module = owningModule(constant->second);
        auto* symbol = module.empty() ? nullptr : modules.referenced(module, *name, path);
        if (!symbol) {
            const auto& origins = importedIn(context).origins.front();
            if (const auto origin = origins.find(*name); origin != origins.end()) {
                // A name of a module known only by its interface has no
                // declaration here.
                symbol = modules.referenced(origin->second.module, *name, path);
            } else if ((symbol = modules.referenced({}, *name, path))) {
                // Declared at file level or in a module around the use, and
                // in a workspace, at file level of this file only.
                const auto& within = contexts[context].module;
                const bool encloses = symbol->module.empty() || symbol->module == within ||
                                      within.starts_with(symbol->module + ".");
                if (!encloses || (fileLocalTopLevel && symbol->module.empty() &&
                                  symbol->definition.file != path))
                    symbol = nullptr;
            }
        }
        if (symbol) references.emplace_back(expr->location, symbol);
    }
    // A `using` is a reference to each name it brings in.
    forEachUsing(program, [&](const WrittenUsing& written) {
        eachImported(written.within, written.module.dotted(), written.alias, written.onlyNames,
                     written.exceptNames,
                     [&](const std::string& name, const std::string& module, bool) {
                         if (auto* symbol = modules.referenced(module, name, path))
                             references.emplace_back(written.location, symbol);
                     });
    });
    std::stable_sort(references.begin(), references.end(),
                     [](const auto& left, const auto& right) {
                         return before(left.first, right.first);
                     });
    for (const auto& [location, symbol] : references) symbol->references.push_back(location);

    std::vector<ScopeSnapshot> scopes;
    for (const auto& [expr, bound] : checker.scopeNames()) {
        if (!path.empty() && expr->location.file != path) continue;
        auto names = bound.bound;
        const auto& imported = importedIn(bound.context).names;
        names.insert(names.end(), imported.begin(), imported.end());
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        scopes.push_back({expr->location, std::move(names)});
    }
    // In source order; of two at one place the one that sees more last.
    std::sort(scopes.begin(), scopes.end(), [](const auto& left, const auto& right) {
        if (before(left.location, right.location)) return true;
        if (before(right.location, left.location)) return false;
        if (left.names.size() != right.names.size()) return left.names.size() < right.names.size();
        return left.names < right.names;
    });
    return scopes;
}

auto buildNameIndex(const ast::Program& program, const std::string& path,
                    const TypeChecker& checker, const ImportedInterfaces* imports) -> NameIndex {
    ProgramModules modules(collectSymbols(program, path));
    NameIndex index;
    index.completionScopes = indexNames(program, path, checker, imports, modules);
    index.symbols = std::move(modules.symbols());
    return index;
}

} // namespace kex::semantic
