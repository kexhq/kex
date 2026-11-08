#pragma once
// The `using` and `export` rules: which names one module may take from
// another. They are written once, over a ModuleSymbols view. SemanticDB answers
// that view from the files it indexes; the checker answers it from the program
// it checks (collectSymbols), so the build no longer needs SemanticDB for
// these checks.
#include "analyzer.hxx"
#include "db.hxx"
#include "imported_interfaces.hxx"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

// What the rules ask about the modules in scope.
class ModuleSymbols {
public:
    virtual ~ModuleSymbols() = default;
    virtual auto hasModule(const std::string& module) const -> bool = 0;
    // The member `name` of `module`, or null.
    virtual auto member(const std::string& module, const std::string& name) const
        -> const SymbolInfo* = 0;
    // The public members of `module`.
    virtual auto publicMembers(const std::string& module) const
        -> std::vector<const SymbolInfo*> = 0;
    auto hasExports(const std::string& module) const -> bool {
        return !publicMembers(module).empty();
    }
};

// One program's modules, from its collected symbols.
class ProgramModules final : public ModuleSymbols {
public:
    explicit ProgramModules(std::vector<SymbolInfo> symbols);
    auto symbols() const -> const std::vector<SymbolInfo>& { return m_symbols; }
    auto symbols() -> std::vector<SymbolInfo>& { return m_symbols; }
    // The symbol a reference to `name` reaches, to record it on: `module`'s
    // member, or with no module the first of that name, `preferFile`'s first.
    auto referenced(const std::string& module, const std::string& name,
                    const std::string& preferFile) -> SymbolInfo*;
    auto hasModule(const std::string& module) const -> bool override;
    auto member(const std::string& module, const std::string& name) const
        -> const SymbolInfo* override;
    auto publicMembers(const std::string& module) const
        -> std::vector<const SymbolInfo*> override;

private:
    std::vector<SymbolInfo> m_symbols;
    std::unordered_map<std::string, std::unordered_map<std::string, const SymbolInfo*>>
        m_members;
    // Every symbol by its name, in declaration order.
    std::unordered_map<std::string, std::vector<SymbolInfo*>> m_byName;
};

using RuleViolation = std::function<void(const std::string& message)>;

// `export target, only: [...] / except: [...]` written in module `current`,
// with `target` as the caller resolved it.
auto checkExport(const ModuleSymbols& modules, const std::string& current,
                 const std::string& target,
                 const std::vector<std::string>& onlyNames,
                 const std::vector<std::string>& exceptNames,
                 const RuleViolation& report) -> void;

// `using module, only: [...] / except: [...]` of a module of source.
auto checkUsing(const ModuleSymbols& modules, const std::string& module,
                const std::vector<std::string>& onlyNames,
                const std::vector<std::string>& exceptNames,
                const RuleViolation& report) -> void;

// `using module, only: [...]` of a module known only by its interface: a name
// counts when the interfaces know it (an export, a receiver method, a type,
// trait or module of the module).
auto checkUsing(const ImportedInterfaces& interfaces,
                const ImportedModuleInterface& module, const std::string& requested,
                const std::vector<std::string>& onlyNames,
                const RuleViolation& report) -> void;

// Whether a module exists outside the program and its interfaces — in the
// build's source roots — asked from inside `current`.
using ModuleLocator =
    std::function<bool(const std::string& module, const std::string& current)>;

// Every `using` and `export` of `program` checked against the program's own
// modules (collectSymbols) and, for a module known only by its interface,
// `interfaces`: the Analyzer's module-rules pass. An exported module neither
// describes is missing only when `locate` (the build's source roots) cannot
// find it either; without one, nothing is claimed missing.
auto findModuleRuleViolations(const ast::Program& program,
                              const ImportedInterfaces* interfaces,
                              const ModuleLocator* locate = nullptr)
    -> std::vector<Diagnostic>;

} // namespace kex::semantic
