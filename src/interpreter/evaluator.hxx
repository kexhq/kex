#pragma once

#include "../ast/ast.hxx"
#include "environment.hxx"
#include "value.hxx"
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::interpreter {

class ReturnException : public std::exception {
public:
    explicit ReturnException(ValuePtr value) : m_value(std::move(value)) {}
    auto value() const -> ValuePtr { return m_value; }

private:
    ValuePtr m_value;
};

class RuntimeError : public std::runtime_error {
public:
    RuntimeError(const std::string& msg, SourceLocation loc)
        : std::runtime_error(
            std::string(loc.file) + ":" + std::to_string(loc.line) + ":"
            + std::to_string(loc.column) + ": runtime error: " + msg),
          m_location(loc) {}

    auto location() const -> SourceLocation { return m_location; }

private:
    SourceLocation m_location;
};

class Evaluator {
public:
    Evaluator();

    auto execute(const ast::Program& program) -> ValuePtr;
    auto setReplMode(bool enabled) -> void;
    auto output() const -> const std::string&;

private:
    // Top-level
    auto execTopLevel(const ast::TopLevelItem& item) -> void;
    auto execModule(const ast::ModuleDef& mod) -> void;
    auto execFunctionDef(const ast::FunctionDef& def, const std::string& typeScope = "") -> void;
    auto execMakeDef(const ast::MakeDef& def) -> void;
    auto execMainBlock(const ast::MainBlock& block) -> ValuePtr;

    // Expressions
    auto eval(const ast::Expr& expr) -> ValuePtr;
    auto evalBody(const std::vector<ast::ExprPtr>& body) -> ValuePtr;

    // Binary/unary ops
    auto evalBinaryOp(TokenType op, const ValuePtr& left, const ValuePtr& right,
                      SourceLocation loc) -> ValuePtr;
    auto evalUnaryOp(TokenType op, const ValuePtr& operand, SourceLocation loc) -> ValuePtr;

    // Function calling
    using NamedArgs = std::vector<std::pair<std::string, ValuePtr>>;
    auto callFunction(const std::string& name, std::vector<ValuePtr> args,
                      NamedArgs namedArgs, SourceLocation loc) -> ValuePtr;

    // Pattern matching
    auto matchPattern(const ast::Pattern& pattern, const ValuePtr& value) -> bool;

    // Built-in functions — orchestrator defined in evaluator.cxx, domains
    // implemented in src/interpreter/stdlib/*.cxx (same access as before,
    // just split out of the core evaluator file by domain).
    auto registerBuiltins() -> void;
    auto registerAdtConstructors() -> void;
    auto registerIOBuiltins() -> void;
    auto registerFileBuiltins() -> void;
    auto registerListBuiltins() -> void;
    auto registerStringBuiltins() -> void;
    auto registerIntegerBuiltins() -> void;
    auto registerStreamBuiltins() -> void;

    // Environment
    auto pushEnv() -> void;
    auto popEnv() -> void;

    std::shared_ptr<Environment> m_env;
    std::shared_ptr<Environment> m_globalEnv;
    std::string m_output;
    std::unordered_map<std::string, std::vector<const ast::FunctionDef*>> m_functionDefs;
    bool m_replMode = false;
};

} // namespace kex::interpreter
