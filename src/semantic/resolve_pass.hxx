#pragma once

#include "../ast/ast.hxx"
#include "db.hxx"
#include "imported_interfaces.hxx"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kex::semantic {

// Pass 2: walk all expression bodies, look up every name reference against
// the collected symbol index and imported interfaces, emit an error for
// unknown names. Populates SymbolInfo::references for go-to-definition /
// find-references.
class ResolvePass {
public:
    explicit ResolvePass(const ImportedInterfaces* imports = nullptr);
    auto run(SemanticDB& db, const std::string& file) -> void;

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

    SemanticDB* m_db = nullptr;
    FileState* m_state = nullptr;
    const ImportedInterfaces* m_imports;

    std::vector<std::unordered_set<std::string>> m_scopes;
    struct ImportOrigin { std::string module; bool explicitImport = false; };
    std::vector<std::unordered_map<std::string, ImportOrigin>> m_importScopes;
    std::string m_currentModule;
    // name -> every parameter label its clauses declare, collected from the
    // whole unit before resolution so a call may precede its definition.
    std::unordered_map<std::string, std::unordered_set<std::string>>
        m_declaredLabels;
};

} // namespace kex::semantic
