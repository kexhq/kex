#pragma once

#include "../ast/ast.hxx"
#include "db.hxx"
#include "imported_interfaces.hxx"
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
    auto resolveMakeFns(const std::vector<std::variant<
            std::unique_ptr<ast::FunctionDef>,
            std::unique_ptr<ast::TypeAnnotation>,
            std::unique_ptr<ast::VisibilityBlock>>>& body) -> void;

    auto resolveExpr(const ast::Expr& expr) -> void;
    auto resolveBody(const std::vector<ast::ExprPtr>& body) -> void;
    auto resolvePattern(const ast::Pattern& pat) -> void;
    auto bindParams(const std::vector<ast::Param>& params) -> void;

    auto isKnown(const std::string& name) const -> bool;
    // If `name` resolves to a top-level symbol, record `loc` as a reference.
    auto recordRef(const std::string& name, SourceLocation loc) -> void;
    auto suggest(const std::string& name) const -> std::string;

    auto pushScope() -> void;
    auto popScope() -> void;
    auto defineLocal(const std::string& name) -> void;

    auto error(SourceLocation loc, const std::string& msg) -> void;
    auto warning(SourceLocation loc, const std::string& msg) -> void;

    SemanticDB* m_db = nullptr;
    FileState* m_state = nullptr;
    const ImportedInterfaces* m_imports;

    std::vector<std::unordered_set<std::string>> m_scopes;
    struct ImportOrigin { std::string module; bool explicitImport = false; };
    std::vector<std::unordered_map<std::string, ImportOrigin>> m_importScopes;
    std::string m_currentModule;
};

} // namespace kex::semantic
