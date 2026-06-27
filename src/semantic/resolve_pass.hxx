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
    auto resolveExpr(const ast::Expr& expr) -> void;
    auto resolveBody(const std::vector<ast::ExprPtr>& body) -> void;
    auto resolvePattern(const ast::Pattern& pat) -> void;

    // Returns true if `name` is known in the current scope / global index / stdlib.
    auto isKnown(const std::string& name) const -> bool;

    // Closest known name by edit distance, or "" if nothing close enough.
    auto suggest(const std::string& name) const -> std::string;

    auto pushScope() -> void;
    auto popScope() -> void;
    auto defineLocal(const std::string& name) -> void;

    auto error(SourceLocation loc, const std::string& msg) -> void;

    FileState* m_state = nullptr;
    SignatureTable m_stdlib;

    // Scope stack: each level is a set of locally-bound names
    std::vector<std::unordered_set<std::string>> m_scopes;
};

} // namespace kex::semantic
