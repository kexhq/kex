#pragma once

#include "../ast/ast.hxx"
#include "db.hxx"
#include "imported_interfaces.hxx"
#include "module_rules.hxx"
#include "name_index.hxx"
#include <memory>
#include "resolve_core.hxx"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kex::semantic {

// How a ResolvePass run reads the program it resolves.
struct ResolveOptions {
    // Gets the names in scope at each point, for completion.
    std::vector<FileState::ScopeSnapshot>* scopes = nullptr;
    // Reports the `using` and `export` rules, which a checked program
    // leaves to its checker.
    bool checksModuleRules = false;
    // A file's top-level declarations are its own: `modules` holds a
    // whole workspace's files, not one program's merged dependencies.
    bool fileLocalTopLevel = false;
};

// Pass 2: walk all expression bodies, look up every name reference against
// the collected symbol index and imported interfaces, emit an error for
// unknown names. Populates SymbolInfo::references for go-to-definition /
// find-references.
class ResolvePass {
public:
    explicit ResolvePass(const ImportedInterfaces* imports = nullptr);
    // A program, every dependency merged in (a build) or every indexed file at
    // once (the language server): its modules are its own (`modules`, whose
    // symbols get the references it finds), the prelude's come from the
    // interfaces.
    auto run(const ast::Program& program, const std::string& path, ProgramModules& modules,
             ResolveOptions options = {}) -> std::vector<Diagnostic>;

private:
    auto resolveModule(const ast::ModuleDef& module) -> void;
    auto resolveUsing(const ast::TypeName& module,
                      const std::optional<std::string>& alias,
                      const std::vector<std::string>& onlyNames,
                      const std::vector<std::string>& exceptNames,
                      SourceLocation loc) -> void;
    auto resolveUsingBlock(const ast::UsingBlock& block) -> void;
    auto resolveExportDecl(const ast::ExportDecl& decl) -> void;
    auto resolveFunctionDef(const ast::FunctionDef& def) -> void;
    auto resolveMakeFns(const decltype(ast::MakeDef::body)& body) -> void;

    auto resolveExpr(const ast::Expr& expr) -> void;
    auto resolveBody(const std::vector<ast::ExprPtr>& body) -> void;
    auto resolveRescue(const ast::RescueBlock& rescue) -> void;
    auto resolvePattern(const ast::Pattern& pat) -> void;
    auto bindParams(const std::vector<ast::Param>& params) -> void;

    auto isKnown(const std::string& name) const -> bool;
    // Parameter labels every clause of `name` declares, across this unit and
    // the imported interfaces. Empty optional when the name has no known
    // declaration to check against — an opaque or native callee, where the
    // runtime check stays the only answer.
    auto declaredLabels(const std::string& name) const
        -> std::optional<std::unordered_set<std::string>>;
    // Reports a named argument no clause of `name` declares. Without this the
    // label is only caught when the line RUNS, so a manifest — which is read
    // but never run — carried the typo silently (a `package.kex` written
    // against src/manifest/bundle.kex is exactly that case).
    auto checkNamedArguments(
        const std::string& name,
        const std::vector<std::pair<std::string, ast::ExprPtr>>& namedArgs,
        SourceLocation loc) -> void;
    // True when `owner` (a symbol's owning module path, "" for file level) is
    // the current module or lexically encloses it.
    auto enclosesCurrentModule(const std::string& owner) const -> bool;
    // If `name` resolves to a top-level symbol, record `loc` as a reference.
    auto recordRef(const std::string& name, SourceLocation loc) -> void;
    auto suggest(const std::string& name) const -> std::string;

    auto pushScope() -> void;
    auto popScope() -> void;
    auto defineLocal(const std::string& name) -> void;
    auto recordCompletionScope(SourceLocation location) -> void;

    auto error(SourceLocation loc, const std::string& msg) -> void;
    auto warning(SourceLocation loc, const std::string& msg) -> void;

    auto resolveItems(const ast::Program& program) -> void;

    // The checked program's own modules, when the pass resolves one.
    ProgramModules* m_program = nullptr;
    // Whether this pass reports the `using` and `export` rules
    // (module_rules.hxx), and whether top-level declarations are file-local:
    // see ResolveOptions.
    bool m_checksModuleRules = false;
    bool m_fileLocalTopLevel = false;
    const ModuleSymbols* m_modules = nullptr;
    const std::vector<SymbolInfo>* m_symbols = nullptr;
    std::string m_path;
    std::vector<Diagnostic>* m_diagnostics = nullptr;
    std::vector<FileState::ScopeSnapshot>* m_completionScopes = nullptr;
    const ImportedInterfaces* m_imports;

    std::vector<std::unordered_set<std::string>> m_scopes;
    std::vector<resolve_core::ImportScope> m_importScopes;
    std::string m_currentModule;
    // name -> every parameter label its clauses declare, collected from the
    // whole unit before resolution so a call may precede its definition.
    std::unordered_map<std::string, std::unordered_set<std::string>>
        m_declaredLabels;
};

// The build's name check: `program`, its dependencies merged in, resolved
// against its own modules (collectSymbols) and `imports` for the prelude's.
// What SemanticDB reports for the entry file, without indexing the prelude:
// only diagnostics in `files` (the entry file, a spec's base) are kept.
auto resolveProgramNames(const ast::Program& program,
                         const std::vector<std::string>& files,
                         const ImportedInterfaces* imports) -> NameIndex;

} // namespace kex::semantic
