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

class BreakException : public std::exception {};
class NextException : public std::exception {};

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
    // Script arguments (everything after the script path on the command
    // line), exposed to Kex code via Args.all/Args.get/Args.count.
    auto setArgs(std::vector<std::string> args) -> void;

private:
    // Top-level
    auto execTopLevel(const ast::TopLevelItem& item) -> void;
    auto execModule(const ast::ModuleDef& mod) -> void;
    auto execFunctionDef(const ast::FunctionDef& def, const std::string& typeScope = "") -> void;
    auto execMakeDef(const ast::MakeDef& def) -> void;
    auto execTypeDef(const ast::TypeDef& def) -> void;
    auto execRecordDef(const ast::RecordDef& def) -> void;
    auto execTraitDef(const ast::TraitDef& def) -> void;
    auto execVisibilityBlock(const ast::VisibilityBlock& block, const std::string& typeScope = "") -> void;
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

    // `let NAME = expr` at top level registers NAME as a zero-arg function
    // (constant) — auto-call it on lookup so `NAME` reads as its value, not
    // as the function itself. Shared by both Identifier (lowercase) and
    // UpperIdentifier (ALL_CAPS constants like `let MAX = 10`) lookup.
    auto autoCallZeroArgConstant(const std::string& name, const ValuePtr& val) -> ValuePtr;

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
    auto registerEnvBuiltins() -> void;
    auto registerMapBuiltins() -> void;
    auto registerMathBuiltins() -> void;
    auto registerTestBuiltins() -> void;

    // Environment
    auto pushEnv() -> void;
    auto popEnv() -> void;

    std::shared_ptr<Environment> m_env;
    std::shared_ptr<Environment> m_globalEnv;
    std::string m_output;
    std::unordered_map<std::string, std::vector<const ast::FunctionDef*>> m_functionDefs;
    // Maps a sum-type variant name (e.g. "Just", "Ok", "Fizz") to the type
    // that declared it (e.g. "Option", "Result", "FizzBuzz"). Populated in
    // execTopLevel's TypeDef handling. Lets method dispatch resolve
    // `make Option<A> do let map(@Just(x), f) = ... end` (registered under
    // "Option::map") when called on a `Just(...)` value (tagged "Just").
    std::unordered_map<std::string, std::string> m_variantParent;
    // Record definitions, keyed by name, so RecordConstruction can apply
    // declared field defaults (e.g. `pos : Int = 0`) for fields the
    // constructor call doesn't specify explicitly.
    std::unordered_map<std::string, const ast::RecordDef*> m_recordDefs;
    std::vector<std::string> m_scriptArgs;
    bool m_replMode = false;

    // describe/it/assert (registerTestBuiltins) — nesting depth for
    // indentation, and pass/fail counters for the summary line printed
    // after the program finishes if any test ran.
    int m_testDepth = 0;
    int m_testsPassed = 0;
    int m_testsFailed = 0;
};

} // namespace kex::interpreter
