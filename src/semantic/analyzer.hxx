#pragma once

#include "../ast/ast.hxx"
#include "symbol.hxx"
#include "typechecker.hxx"
#include <string>
#include <vector>

namespace kex::semantic {

struct Diagnostic {
    enum class Level { Error, Warning };
    Level level;
    SourceLocation location;
    std::string message;
};

class Analyzer {
public:
    auto analyze(const ast::Program& program) -> bool;
    auto diagnostics() const -> const std::vector<Diagnostic>&;

    // Query the inferred type of an expression node after analyze() has run.
    auto typeOf(const ast::Expr* expr) const -> TypePtr;
    auto typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>&;
    auto functionSignatures(const ast::FunctionDef* function) const
        -> const std::vector<Signature>*;

private:
    // Top-level declarations
    auto analyzeTopLevel(const ast::TopLevelItem& item) -> void;
    auto analyzeModule(const ast::ModuleDef& mod) -> void;
    auto analyzeTypeDef(const ast::TypeDef& def) -> void;
    auto analyzeRecordDef(const ast::RecordDef& def) -> void;
    auto analyzeMakeDef(const ast::MakeDef& def) -> void;
    auto analyzeFunctionDef(const ast::FunctionDef& def) -> void;
    auto analyzeMainBlock(const ast::MainBlock& block) -> void;

    // Expressions
    auto analyzeExpr(const ast::Expr& expr) -> void;
    auto analyzeBody(const std::vector<ast::ExprPtr>& body) -> void;

    // Defines every variable a pattern introduces (VarPattern, shorthand
    // record fields, nested constructor/list/tuple args) as a Symbol in
    // the current scope — mirrors TypeChecker::bindPatternVars, just
    // populating SymbolTable instead of TypeEnv.
    auto bindPatternVars(const ast::Pattern& pat, SourceLocation loc) -> void;

    // Purity
    auto isInFoulContext() const -> bool;
    auto checkPurity(const std::string& callee, SourceLocation loc) -> void;

    // Loop control
    auto checkLoopControl(SourceLocation loc, const std::string& keyword) -> void;

    // Helpers
    auto error(SourceLocation loc, const std::string& msg) -> void;
    auto warning(SourceLocation loc, const std::string& msg) -> void;

    SymbolTable m_symbols;
    std::vector<Diagnostic> m_diagnostics;
    TypeChecker m_checker;
    bool m_inFoulContext = false;

    // Stdlib module names whose methods are inherently foul (I/O, network, etc.).
    // IO.inspect is the one documented exception — always allowed even in pure fns.
    static constexpr std::string_view kFoulModules[] = {"IO", "System", "Net", "File", "Directory", "Mock", "Http", "Database", "Supervisor"};

    // break/next bind to the nearest enclosing Loop marker, but a Closure
    // marker in between makes them illegal — they don't cross into a
    // do-block passed to another function. `match`/`receive` clauses don't
    // push a marker, so break/next see through them to the loop.
    enum class LoopScope { Loop, Closure };
    std::vector<LoopScope> m_loopScopes;
};

} // namespace kex::semantic
