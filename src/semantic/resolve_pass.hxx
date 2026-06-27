#pragma once

#include "../ast/ast.hxx"
#include "db.hxx"
#include "stdlib_signatures.hxx"
#include <string>
#include <unordered_set>
#include <vector>

namespace kex::semantic {

// Pass 2: walk all expression bodies, look up every name reference against
// the collected symbol index and stdlib, emit an error for unknown names.
// Populates SymbolInfo::references for go-to-definition / find-references.
class ResolvePass {
public:
    explicit ResolvePass();
    auto run(SemanticDB& db, const std::string& file) -> void;

private:
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

    SemanticDB* m_db = nullptr;
    FileState* m_state = nullptr;
    SignatureTable m_stdlib;

    std::vector<std::unordered_set<std::string>> m_scopes;
};

} // namespace kex::semantic
