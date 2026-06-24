#pragma once

#include "../ast/ast.hxx"
#include "stdlib_signatures.hxx"
#include "symbol.hxx"
#include "traits.hxx"
#include "types.hxx"
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

struct Diagnostic;

class TypeChecker {
public:
    auto check(const ast::Program& program, std::vector<Diagnostic>& diagnostics) -> void;

private:
    // Top-level
    auto checkTopLevel(const ast::TopLevelItem& item) -> void;
    auto checkModule(const ast::ModuleDef& mod) -> void;

    // ADT registry (sum types with a known, closed constructor set), for
    // match exhaustiveness checking. Populated in a pass over the whole
    // program before any function body is checked, so forward references
    // work the same way `m_globals` already does.
    auto registerAdt(const ast::TypeDef& def) -> void;
    auto registerAdtsInModule(const ast::ModuleDef& mod) -> void;
    auto checkMatchExhaustiveness(const ast::MatchExpr& node, SourceLocation loc) -> void;

    // Defines every variable a pattern introduces (VarPattern, shorthand
    // record fields, nested constructor/list/tuple args) in the current
    // scope as a fresh type var — mirrors how checkFunctionDef already
    // treats untyped params, just recursing into pattern structure.
    auto bindPatternVars(const ast::Pattern& pat) -> void;
    auto checkFunctionDef(const ast::FunctionDef& def) -> void;
    auto checkMakeDef(const ast::MakeDef& def) -> void;
    auto checkMainBlock(const ast::MainBlock& block) -> void;

    // Type inference for expressions
    auto inferExpr(const ast::Expr& expr) -> TypePtr;
    auto inferBody(const std::vector<ast::ExprPtr>& body) -> TypePtr;

    // Binary operator type resolution
    auto inferBinaryOp(TokenType op, const TypePtr& left, const TypePtr& right,
                       SourceLocation loc) -> TypePtr;

    // Call checking (FunctionCall and MethodCall, the latter desugared to
    // the same path with the receiver prepended as the first argument).
    // Resolves `name` against the stdlib signature table only — user-
    // defined functions don't have real Signatures yet (phase 5), so an
    // unrecognized name still falls back to Type::unknown() unchanged.
    auto checkCall(const std::string& name, const std::vector<TypePtr>& argTypes,
                   SourceLocation loc) -> TypePtr;
    auto argMatchesParam(const TypePtr& argType, const TypePtr& paramType) const -> bool;
    auto displaySignature(const std::string& name, const Signature& sig) const -> std::string;

    // Scope management
    auto pushScope() -> void;
    auto popScope() -> void;
    auto defineVar(const std::string& name, TypePtr type) -> void;
    auto lookupVar(const std::string& name) const -> TypePtr;

    // Error reporting
    auto error(SourceLocation loc, const std::string& msg) -> void;
    auto typeMismatch(SourceLocation loc, const TypePtr& expected, const TypePtr& actual) -> void;

    std::vector<Diagnostic>* m_diagnostics = nullptr;
    TypeEnv m_globals;
    std::vector<TypeEnv> m_scopeStack;
    int m_nextTypeVar = 0;
    TraitRegistry m_traits = TraitRegistry::withBuiltins();
    SignatureTable m_stdlib = SignatureTable::withStdlib();

    // typeName -> constructor names; constructorName -> owning typeName.
    std::unordered_map<std::string, std::vector<std::string>> m_adtVariants;
    std::unordered_map<std::string, std::string> m_adtOfConstructor;

    auto freshTypeVar() -> TypePtr;
};

} // namespace kex::semantic
