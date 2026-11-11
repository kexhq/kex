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

auto findModuleRuleViolations(const ast::Program& program,
                              const ImportedInterfaces* interfaces,
                              const ModuleLocator* locate)
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
    // A `using`, a declaration (UsingBlock) or one written in a body
    // (UsingExpr), against what its module declares.
    const auto usingNames = [&](const ast::TypeName& written, const std::vector<std::string>& only,
                                const std::vector<std::string>& except, const SourceLocation& at,
                                const std::string& current) {
        const auto report = [&](const std::string& message) {
            found.push_back({Diagnostic::Level::Error, at, message});
        };
        const auto requested = written.dotted();
        const auto module = resolved(current, requested);
        if (modules.hasModule(module))
            checkUsing(modules, module, only, except, report);
        else if (const auto* interface = interfaceOf(requested))
            checkUsing(*interfaces, *interface, requested, only, report);
        // A module nothing here describes is the build's to find (or warn of).
    };
    const auto using_ = [&](const ast::UsingBlock& block, const std::string& current) {
        usingNames(block.module, block.onlyNames, block.exceptNames, block.location, current);
    };
    // The `using`s written in the bodies of `items`, which are in module `path`.
    const auto bodyUsings = [&](const auto& self, const auto& items, const std::string& path) -> void {
        const auto inExpr = [&](const auto& expression, const ast::Expr& expr) -> void {
            if (const auto* written = std::get_if<ast::UsingExpr>(&expr.kind))
                usingNames(written->module, written->onlyNames, written->exceptNames, expr.location, path);
            ast::forEachChild(expr, [&](const ast::ExprPtr& child) {
                if (child) expression(expression, *child);
            });
        };
        const auto inFunction = [&](const ast::FunctionDef& function) {
            for (const auto& clause : function.clauses)
                for (const auto& expr : clause.body)
                    if (expr) inExpr(inExpr, *expr);
        };
        for (const auto& item : items)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (node) inFunction(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                    if (node)
                        for (const auto& expr : node->body)
                            if (expr) inExpr(inExpr, *expr);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                    if (node) self(self, node->body, path);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (node) self(self, node->items, path);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) self(self, node->body, ast::nestedModulePath(path, node->name));
                }
            }, item);
    };
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
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>)
                    using_(*node, path);
                else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ExportDecl>>)
                    export_(*node, path);
                else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>)
                    self(self, *node, path);
            }, item);
    };
    for (const auto& item : program.items)
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if (!node) return;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>)
                using_(*node, "");
            else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>)
                module(module, *node, "");
        }, item);
    bodyUsings(bodyUsings, program.items, "");
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
