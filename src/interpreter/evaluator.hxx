#pragma once

#include "../ast/ast.hxx"
#include "environment.hxx"
#include "scheduler.hxx"
#include "value.hxx"
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    // Scheduler needs direct access to eval/evalBody/matchPattern/
    // pushEnv/popEnv/m_env to run process bodies and implement
    // blockingReceive's clause matching — see scheduler.cxx. Tightly
    // coupled by design, not worth a larger public surface just to
    // avoid friendship.
    friend class Scheduler;

public:
    Evaluator();

    auto execute(const ast::Program& program) -> ValuePtr;
    // Parse src/prelude/*.kex (MainBlocks dropped) once into a shared AST and
    // execute its declarations on this Evaluator, so the Kex-written stdlib
    // shadows the native builtins. No-op if KEX_PRELUDE_DIR is unset or the
    // directory can't be read. Idempotent per Evaluator instance.
    auto loadPrelude() -> void;
    auto setReplMode(bool enabled) -> void;
    auto output() const -> const std::string&;
    // Script arguments (everything after the script path on the command
    // line), exposed to Kex code via Args.all/Args.get/Args.count.
    auto setArgs(std::vector<std::string> args) -> void;
    auto setModuleRoots(std::vector<std::string> roots) -> void;

private:
    // Top-level
    auto execTopLevel(const ast::TopLevelItem& item) -> void;
    auto execModule(const ast::ModuleDef& mod) -> void;
    auto execFunctionDef(const ast::FunctionDef& def, const std::string& typeScope = "") -> void;
    auto execMakeDef(const ast::MakeDef& def) -> void;
    auto execTypeDef(const ast::TypeDef& def) -> void;
    auto execRecordDef(const ast::RecordDef& def, const std::string& moduleScope = "") -> void;
    auto execTraitDef(const ast::TraitDef& def) -> void;
    auto execCompiledBlock(const ast::CompiledBlock& block) -> void;
    auto execVisibilityBlock(const ast::VisibilityBlock& block, const std::string& typeScope = "") -> void;
    auto execUsingBlock(const ast::UsingBlock& block, const std::string& moduleScope = "") -> void;
    auto execMainBlock(const ast::MainBlock& block) -> ValuePtr;
    auto ensureModuleLoaded(const std::string& moduleName, SourceLocation loc,
                            const std::string& currentModule = "") -> std::string;
    auto resolvePendingExports() -> void;
    auto defineImported(const std::string& bindingName, const std::string& logicalName,
                        const std::string& sourceModule, bool explicitImport,
                        const std::string& moduleScope, ValuePtr value,
                        SourceLocation loc) -> void;

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
    auto registerDirectoryBuiltins() -> void;
    auto registerMockBuiltins() -> void;
    auto registerListBuiltins() -> void;
    auto registerStringBuiltins() -> void;
    auto registerNumberBuiltins() -> void;
    auto registerStreamBuiltins() -> void;
    auto registerEnvBuiltins() -> void;
    auto registerMapBuiltins() -> void;
    auto registerMathBuiltins() -> void;
    auto registerConsoleBuiltins() -> void;
    auto registerTestBuiltins() -> void;
    auto registerProcessBuiltins() -> void;

    // Environment
    auto pushEnv() -> void;
    auto popEnv() -> void;

    struct ModuleEntry {
        std::unordered_map<std::string, ValuePtr> exports;
        std::unordered_set<std::string> privateNames;
        std::unordered_map<std::string, std::string> submodules;
        bool isFoul = false;
    };
    struct PendingExport { std::string owner; const ast::ExportDecl* decl; };

    std::shared_ptr<Environment> m_env;
    std::shared_ptr<Environment> m_globalEnv;
    std::shared_ptr<Environment> m_intrinsicEnv;
    // Owns every process (including "process 0", the top-level program
    // itself — see Scheduler::runToCompletion) for this Evaluator's whole
    // lifetime, so processes spawned on one execute() call (e.g. one REPL
    // line) remain reachable via `send` from a later call.
    std::unique_ptr<Scheduler> m_scheduler;
    std::string m_output;
    std::unordered_map<std::string, std::vector<const ast::FunctionDef*>> m_functionDefs;
    std::unordered_map<std::string, ModuleEntry> m_moduleRegistry;
    std::vector<PendingExport> m_pendingExports;
    std::unordered_set<std::string> m_loadingModules;
    std::vector<std::string> m_moduleRoots{"lib", "src"};
    std::vector<std::unique_ptr<std::string>> m_loadedModulePaths;
    std::vector<std::unique_ptr<ast::Program>> m_loadedModulePrograms;
    std::string m_currentModule;
    struct ImportOrigin { std::string module; bool explicitImport = false; };
    std::vector<std::unordered_map<std::string, ImportOrigin>> m_importScopes{{}};
    std::unordered_map<std::string, ImportOrigin> m_moduleImportOrigins;
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
    bool m_preludeLoaded = false;
    // Method names defined by the prelude (sealed stdlib). Users may add new
    // methods to builtin types but not redefine these.
    std::unordered_set<std::string> m_sealedMethods;
    std::unordered_map<std::string, std::string> m_mockFiles;
    std::unordered_set<std::string> m_mockDirs;

    // describe/it/assert (registerTestBuiltins) — nesting depth for
    // indentation, and pass/fail counters for the summary line printed
    // after the program finishes if any test ran.
    int m_testDepth = 0;
    int m_testsPassed = 0;
    int m_testsFailed = 0;
};

} // namespace kex::interpreter
