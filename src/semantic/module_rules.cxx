#include "module_rules.hxx"

#include "collect_pass.hxx"
#include "resolve_core.hxx"
#include "../ast/walk.hxx"

#include <algorithm>

namespace kex::semantic {

ProgramModules::ProgramModules(std::vector<SymbolInfo> symbols)
    : m_symbols(std::move(symbols)) {
    for (auto& symbol : m_symbols) {
        m_members[symbol.module].try_emplace(symbol.name, &symbol);
        m_byName[symbol.name].push_back(&symbol);
    }
}

auto ProgramModules::hasModule(const std::string& module) const -> bool {
    return std::any_of(m_symbols.begin(), m_symbols.end(), [&](const SymbolInfo& symbol) {
        return symbol.kind == SymbolKind::Module &&
               (symbol.module.empty() ? symbol.name
                                      : symbol.module + "." + symbol.name) == module;
    });
}

auto ProgramModules::member(const std::string& module, const std::string& name) const
    -> const SymbolInfo* {
    const auto members = m_members.find(module);
    if (members == m_members.end()) return nullptr;
    const auto found = members->second.find(name);
    return found == members->second.end() ? nullptr : found->second;
}

auto ProgramModules::publicMembers(const std::string& module) const
    -> std::vector<const SymbolInfo*> {
    std::vector<const SymbolInfo*> exports;
    if (module.empty()) return exports;
    for (const auto& symbol : m_symbols)
        if (symbol.module == module && symbol.isExported) exports.push_back(&symbol);
    return exports;
}

auto checkExport(const ModuleSymbols& modules, const std::string& current,
                 const std::string& target,
                 const std::vector<std::string>& onlyNames,
                 const std::vector<std::string>& exceptNames,
                 const RuleViolation& report) -> void {
    if (target == current) return report("module cannot export itself");
    if (!modules.hasModule(target))
        return report("exported module not found: " + target);
    for (const auto& name : onlyNames) {
        const auto* symbol = modules.member(target, name);
        if (!symbol)
            report("module " + target + " has no name `" + name + "`");
        else if (!symbol->isExported)
            report("cannot export private name `" + name + "` from " + target);
    }
    for (const auto& name : exceptNames) {
        const auto* symbol = modules.member(target, name);
        if (!symbol)
            report("module " + target + " has no name `" + name + "`");
        else if (!symbol->isExported)
            report("cannot reference private name `" + name + "` in except list");
    }
    if (onlyNames.empty() && !modules.hasExports(target))
        report("module " + target + " has no public names to export");
}

auto checkUsing(const ModuleSymbols& modules, const std::string& module,
                const std::vector<std::string>& onlyNames,
                const std::vector<std::string>& exceptNames,
                const RuleViolation& report) -> void {
    for (const auto& name : onlyNames) {
        const auto* symbol = modules.member(module, name);
        if (!symbol)
            report("module " + module + " has no name `" + name + "`");
        else if (!symbol->isExported)
            report("cannot import private name `" + name + "` from " + module);
    }
    for (const auto& name : exceptNames)
        if (const auto* symbol = modules.member(module, name);
            symbol && !symbol->isExported)
            report("cannot reference private name `" + name + "` from " + module);
}

auto checkUsing(const ImportedInterfaces& interfaces,
                const ImportedModuleInterface& module, const std::string& requested,
                const std::vector<std::string>& onlyNames,
                const RuleViolation& report) -> void {
    // An interface does not list every member a module declares (an opt-in
    // module's `make` methods are its receiver functions only once a package
    // approves them), so a name counts when the interfaces know it at all: a
    // refusal is then never wrong, and a typo is still caught.
    const auto& owner = module.sourceModule.empty() ? requested : module.sourceModule;
    const auto declares = [&](const std::string& name) {
        const auto qualified = owner + "." + name;
        return module.exports.count(name) ||
               interfaces.receiverFunctions.count(name) ||
               interfaces.typeNames.count(qualified) ||
               interfaces.recordArities.count(qualified) ||
               interfaces.distinctTypes.count(qualified) ||
               interfaces.typeAliases.count(qualified) ||
               interfaces.modules.count(qualified) ||
               std::any_of(interfaces.traits.begin(), interfaces.traits.end(),
                           [&](const TraitDef& trait) {
                               return trait.name == qualified || trait.name == name;
                           });
    };
    for (const auto& name : onlyNames)
        if (!declares(name))
            report("module " + requested + " has no name `" + name + "`");
}

auto forEachImportedName(
    const ModuleSymbols& modules, const ImportedInterfaces* imports, const std::string& within,
    const std::string& written, const std::optional<std::string>& alias,
    const std::vector<std::string>& only, const std::vector<std::string>& except,
    const std::function<void(const std::string&, const std::string&, bool)>& visit) -> void {
    if (!resolve_core::usingImportsNames(alias, only)) return;
    const auto module = resolve_core::resolveModulePathIn(
        within, written, [&](const std::string& candidate) { return modules.hasModule(candidate); });
    if (modules.hasModule(module)) {
        for (const auto* symbol : modules.publicMembers(module))
            if (resolve_core::usingSelectsName(symbol->name, only, except))
                visit(symbol->name, module, !symbol->makeTarget.empty());
        return;
    }
    if (!imports) return;
    const auto interface = imports->modules.find(written);
    if (interface == imports->modules.end()) return;
    for (const auto& [name, overloads] : interface->second.exports)
        if (resolve_core::usingSelectsName(name, only, except))
            visit(name, written,
                  !overloads.empty() &&
                      std::all_of(overloads.begin(), overloads.end(),
                                  [](const ImportedFunction& function) {
                                      return function.isReceiverMethod;
                                  }));
}

auto forEachUsing(const ast::Program& program,
                  const std::function<void(const WrittenUsing&)>& visit) -> void {
    const auto inExpr = [&](const auto& self, const ast::Expr& expr, const std::string& path) -> void {
        if (const auto* written = std::get_if<ast::UsingExpr>(&expr.kind))
            visit({written->module, written->alias, written->onlyNames, written->exceptNames,
                   expr.location, path});
        ast::forEachChild(expr, [&](const ast::ExprPtr& child) {
            if (child) self(self, *child, path);
        });
    };
    const auto inBody = [&](const std::vector<ast::ExprPtr>& body, const std::string& path) {
        for (const auto& expr : body)
            if (expr) inExpr(inExpr, *expr, path);
    };
    const auto inItems = [&](const auto& self, const auto& items, const std::string& path) -> void {
        for (const auto& item : items)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if (!node) return;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                    visit({node->module, node->alias, node->onlyNames, node->exceptNames,
                           node->location, path});
                    inBody(node->body, path);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    for (const auto& clause : node->clauses) inBody(clause.body, path);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                    inBody(node->body, path);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                    self(self, node->body, path);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    self(self, node->items, path);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    self(self, node->body, ast::nestedModulePath(path, node->name));
                }
            }, item);
    };
    inItems(inItems, program.items, "");
}

auto findModuleRuleViolations(const ast::Program& program,
                              const ImportedInterfaces* interfaces,
                              const ModuleLocator* locate,
                              const std::vector<std::string>& warnedFiles)
    -> std::vector<Diagnostic> {
    const ProgramModules modules(collectSymbols(program));
    std::vector<Diagnostic> found;
    const auto resolved = [&](const std::string& current, const std::string& written) {
        return resolve_core::resolveModulePathIn(
            current, written,
            [&](const std::string& module) { return modules.hasModule(module); });
    };
    // A module only an interface describes: its members are not collected.
    const auto interfaceOf = [&](const std::string& module) -> const ImportedModuleInterface* {
        if (!interfaces) return nullptr;
        const auto found = interfaces->modules.find(module);
        return found == interfaces->modules.end() ? nullptr : &found->second;
    };
    // Each `using` against what its module declares.
    forEachUsing(program, [&](const WrittenUsing& written) {
        const auto report = [&](const std::string& message) {
            found.push_back({Diagnostic::Level::Error, written.location, message});
        };
        const auto requested = written.module.dotted();
        const auto module = resolved(written.within, requested);
        if (modules.hasModule(module))
            checkUsing(modules, module, written.onlyNames, written.exceptNames, report);
        else if (const auto* interface = interfaceOf(requested))
            checkUsing(*interfaces, *interface, requested, written.onlyNames, report);
        // A module nothing here describes is the build's to find.
        else if (std::find(warnedFiles.begin(), warnedFiles.end(), written.location.file) !=
                 warnedFiles.end())
            found.push_back({Diagnostic::Level::Warning, written.location,
                             "module not found in source roots: " + requested});
    });
    const auto export_ = [&](const ast::ExportDecl& decl, const std::string& current) {
        const auto target = resolved(current, decl.module.dotted());
        // Not one of the program's: described by an interface, or on disk
        // (an `export` does not pull its module into the program), its
        // members are not known here.
        if (!modules.hasModule(target) &&
            (interfaceOf(target) || !locate || (*locate)(target, current)))
            return;
        checkExport(modules, current, target, decl.onlyNames, decl.exceptNames,
                    [&](const std::string& message) {
                        found.push_back({Diagnostic::Level::Error, decl.location, message});
                    });
    };
    const auto module = [&](const auto& self, const ast::ModuleDef& def,
                            const std::string& parent) -> void {
        const auto path = ast::nestedModulePath(parent, def.name);
        for (const auto& item : def.body)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if (!node) return;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::ExportDecl>>)
                    export_(*node, path);
                else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>)
                    self(self, *node, path);
            }, item);
    };
    for (const auto& item : program.items)
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if (!node) return;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>)
                module(module, *node, "");
        }, item);
    return found;
}

} // namespace kex::semantic

namespace kex::semantic {

auto ProgramModules::referenced(const std::string& module, const std::string& name,
                                const std::string& preferFile) -> SymbolInfo* {
    if (!module.empty()) return const_cast<SymbolInfo*>(member(module, name));
    const auto named = m_byName.find(name);
    if (named == m_byName.end()) return nullptr;
    for (auto* symbol : named->second)
        if (symbol->definition.file == preferFile) return symbol;
    return named->second.front();
}

} // namespace kex::semantic
