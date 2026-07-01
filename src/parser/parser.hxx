#pragma once

#include "../ast/ast.hxx"
#include "../lexer/lexer.hxx"
#include <functional>
#include <string>
#include <vector>

namespace kex {

struct ParseDiagnostic {
    SourceLocation location;
    std::string message;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens, std::string_view filename = "<stdin>");

    auto parseProgram() -> ast::Program;
    auto parseExpr() -> ast::ExprPtr;
    auto diagnostics() const -> const std::vector<ParseDiagnostic>&;

private:
    // Token navigation
    auto peek() const -> const Token&;
    auto peekNext() const -> const Token&;
    auto advance() -> const Token&;
    auto atEnd() const -> bool;
    auto check(TokenType type) const -> bool;
    auto match(TokenType type) -> bool;
    auto expect(TokenType type, const std::string& message) -> const Token&;
    auto skipNewlines() -> void;
    auto currentLocation() const -> SourceLocation;

    // Top-level
    auto parseTopLevelItem() -> ast::TopLevelItem;
    auto parseModuleDef() -> std::unique_ptr<ast::ModuleDef>;
    auto parseTypeDef() -> std::unique_ptr<ast::TypeDef>;
    auto parseRecordDef() -> std::unique_ptr<ast::RecordDef>;
    auto parseTraitDef() -> std::unique_ptr<ast::TraitDef>;
    auto parseMakeDef() -> std::unique_ptr<ast::MakeDef>;
    auto parseFunctionDef(bool isFoul = false) -> std::unique_ptr<ast::FunctionDef>;
    auto parseCompiledBlock() -> std::unique_ptr<ast::CompiledBlock>;
    auto parseUsingBlock() -> std::unique_ptr<ast::UsingBlock>;
    auto parseMainBlock() -> std::unique_ptr<ast::MainBlock>;
    auto parsePragma() -> std::unique_ptr<ast::Pragma>;
    auto parseVisibilityBlock() -> std::unique_ptr<ast::VisibilityBlock>;

    // Disambiguates `let` at a declaration-list position (top level, module
    // body, make body, visibility block — anywhere a bare `let` could mean
    // either a function definition or a plain value binding to a non-
    // callable value). Assumes the current token is `Let`. Mirrors
    // parseLetExpr's identical check for body-statement position.
    auto isLetFunctionDefAhead() -> bool;

    // Functions
    auto parseFunctionClause() -> ast::FunctionClause;
    auto parseParams() -> std::vector<ast::Param>;
    auto parseParam() -> ast::Param;
    auto parseTypeAnnotation() -> std::unique_ptr<ast::TypeAnnotation>;

    // Type expressions
    auto parseTypeExpr() -> ast::TypeExprPtr;
    auto parseTypeOr() -> ast::TypeExprPtr;
    auto parseTypeUnion() -> ast::TypeExprPtr;
    auto parseTypeFunction() -> ast::TypeExprPtr;
    auto parseTypePostfix() -> ast::TypeExprPtr;
    auto parseTypePrimary() -> ast::TypeExprPtr;
    auto parseTypeName() -> ast::TypeName;

    // Expressions
    auto parseAssignment() -> ast::ExprPtr;
    auto parseOr() -> ast::ExprPtr;
    auto parseAnd() -> ast::ExprPtr;
    auto parseEquality() -> ast::ExprPtr;
    auto parseComparison() -> ast::ExprPtr;
    auto parseAddition() -> ast::ExprPtr;
    auto parseMultiplication() -> ast::ExprPtr;
    auto parseUnary() -> ast::ExprPtr;
    auto parsePostfix() -> ast::ExprPtr;
    auto parsePrimary() -> ast::ExprPtr;
    auto parseBody() -> std::vector<ast::ExprPtr>;

    // Specific expressions
    auto parseIfExpr() -> ast::ExprPtr;
    auto parseMatchExpr() -> ast::ExprPtr;
    auto parseReceiveExpr() -> ast::ExprPtr;
    auto parseLoopExpr() -> ast::ExprPtr;
    auto parseWhileExpr() -> ast::ExprPtr;
    auto parseLetExpr() -> ast::ExprPtr;
    auto parseVarExpr() -> ast::ExprPtr;
    auto parseReturnExpr() -> ast::ExprPtr;
    auto parseSpawnExpr() -> ast::ExprPtr;
    auto parseLambda() -> ast::ExprPtr;
    auto parseShorthandLambda() -> ast::ExprPtr;
    auto parseCurryExpr() -> ast::ExprPtr;
    auto parseListExpr() -> ast::ExprPtr;
    auto parseMapOrBlock() -> ast::ExprPtr;
    auto parseTupleOrGrouped() -> ast::ExprPtr;

    // Patterns
    auto parsePattern() -> ast::PatternPtr;
    auto parsePatternPrimary() -> ast::PatternPtr;

    // Helpers
    auto parseBlock() -> std::optional<ast::ExprPtr>;
    auto parseMatchClause() -> ast::MatchClause;
    auto parseMatchClauseBody() -> ast::ExprPtr;
    auto isAtExprStart() const -> bool;
    auto hasDoBeforeNewline() const -> bool;
    // Returns true if current token (after skipNewlines) looks like the start
    // of a new match/receive clause (pattern -> body), used to terminate
    // multi-line clause body parsing without an explicit do...end.
    auto peekIsClauseStart() const -> bool;
    // Scans forward past balanced brackets from m_pos and returns true if a
    // '->' or 'when' follows the closing bracket (indicates a clause pattern).
    auto peekHasArrowAfterBalance() const -> bool;

    // Error handling and recovery
    [[noreturn]] auto error(const std::string& message) -> void;
    auto syncToTopLevel() -> void;
    auto syncToStatement() -> void;

    std::vector<Token> m_tokens;
    std::string_view m_filename;
    int m_pos = 0;
    bool m_noDoBlocks = false;
    bool m_noThenExpr = false; // suppress `then` as ternary inside if-conditions
    std::vector<ParseDiagnostic> m_diagnostics;
};

} // namespace kex
