#pragma once

#include "../ast/ast.hxx"
#include "environment.hxx"
#include "scheduler.hxx"
#include "value.hxx"
#include "../semantic/types.hxx"
#include <chrono>
#include <deque>
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

class TryException : public std::exception {
public:
    explicit TryException(ValuePtr error) : m_error(std::move(error)) {}
    auto error() const -> ValuePtr { return m_error; }

private:
    ValuePtr m_error;
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

class EvaluationTimeout : public std::runtime_error {
public:
    EvaluationTimeout()
        : std::runtime_error("compile-time evaluation timed out") {}
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
    // Loads source declarations without executing an explicit main, then
    // invokes one function under a cooperative wall-clock deadline. Used by
    // compile-time tagged-literal validators; no result is spliced into code.
    auto evaluateFunction(
        const ast::Program& program,
        const std::string& name,
        std::vector<ValuePtr> args,
        std::chrono::milliseconds timeout) -> ValuePtr;
    // Loads `program`'s declarations under the same cooperative deadline and
    // termination guard as evaluateFunction, then reads back the named global
    // bindings. Executing the declarations is what binds a `compiled do`
    // block's constants, so this is how src/compiled/ recovers their values
    // without running the program's `main`. A name that ends up unbound yields
    // a null entry rather than throwing, so the caller can report which one.
    auto evaluateGlobals(
        const ast::Program& program,
        const std::vector<std::string>& names,
        std::chrono::milliseconds timeout) -> std::vector<ValuePtr>;
    // Parse the sources selected by src/stdlib/prelude.kex (MainBlocks dropped)
    // once into a shared AST and
    // execute its declarations on this Evaluator, so the Kex-written stdlib
    // shadows the native builtins. No-op if no configured or embedded prelude
    // source root is available. Idempotent per Evaluator instance.
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
    auto execModule(const ast::ModuleDef& mod,
                    const std::string& parentModule = "") -> void;
    auto execFunctionDef(const ast::FunctionDef& def,
                         const std::string& typeScope = "",
                         bool hasImplicitReceiver = false) -> void;
    auto execMakeDef(const ast::MakeDef& def) -> void;
    auto execTypeDef(const ast::TypeDef& def,
                     const std::string& moduleScope = "") -> void;
    auto execRecordDef(const ast::RecordDef& def, const std::string& moduleScope = "") -> void;
    auto execTraitDef(const ast::TraitDef& def) -> void;
    auto execCompiledBlock(const ast::CompiledBlock& block) -> void;
    auto execVisibilityBlock(const ast::VisibilityBlock& block,
                             const std::string& typeScope = "",
                             bool hasImplicitReceiver = false) -> void;
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
    auto checkDeadline() const -> void;
    auto findNamedClause(const std::string& functionName,
                         const NamedArgs& namedArgs) const
        -> const ast::FunctionClause*;
    auto findImportedNamedOverload(const std::string& functionName,
                                   const NamedArgs& namedArgs) const
        -> std::optional<std::string>;
    auto receiverArgumentOffset(const std::string& functionName,
                                const std::vector<ValuePtr>& args) const -> size_t;
    auto resolveMethodName(const ValuePtr& receiver, const std::string& method,
                           const std::vector<ValuePtr>* args = nullptr) const
        -> std::string;
    auto registerRuntimeSignature(const ast::TypeAnnotation& annotation,
                                  const std::string& scope,
                                  bool implicitReceiver) -> void;
    auto runtimeTypeMatches(const ValuePtr& value,
                            const ast::TypeExpr& type) const -> bool;
    auto runtimeTypeKey(const ast::TypeExpr& type) const -> std::string;

    // Pattern matching
    auto matchPattern(const ast::Pattern& pattern, const ValuePtr& value) -> bool;

    // Try/rescue support
    auto evalRescue(const ast::RescueBlock& rescue, const ValuePtr& error,
                    SourceLocation loc) -> ValuePtr;

    // `let NAME = expr` at top level registers NAME as a zero-arg function
    // (constant) — auto-call it on lookup so `NAME` reads as its value, not
    // as the function itself. Shared by both Identifier (lowercase) and
    // UpperIdentifier (ALL_CAPS constants like `let MAX = 10`) lookup.
    auto autoCallZeroArgConstant(const std::string& name, const ValuePtr& val) -> ValuePtr;

    // Built-in functions — orchestrator defined in evaluator.cxx, domains
    // implemented in src/interpreter/stdlib/*.cxx (same access as before,
    // just split out of the core evaluator file by domain).
    auto defineIntrinsic(const std::string& name, NativeFunc fn) -> void;
    auto defineIntrinsic(const std::string& name, const ValuePtr& value) -> void;
    auto defineModule(const std::string& name) -> void;
    auto definePublic(const std::string& name, NativeFunc fn) -> void;
    auto defineDual(const std::string& name, NativeFunc fn) -> void;
    auto registerBuiltins() -> void;
    auto registerAdtConstructors() -> void;
    auto registerIOBuiltins() -> void;
    auto registerFileBuiltins() -> void;
    auto registerDirectoryBuiltins() -> void;
    auto registerMockBuiltins() -> void;
    auto registerListBuiltins() -> void;
    auto registerStringBuiltins() -> void;
    auto registerNumberBuiltins() -> void;
    auto registerRegexBuiltins() -> void;
    auto registerStreamBuiltins() -> void;
    auto registerEnvBuiltins() -> void;
    auto registerMapBuiltins() -> void;
    auto registerMathBuiltins() -> void;
    auto registerTimeBuiltins() -> void;
    auto registerTypeBuiltins() -> void;
    auto structuredTypeValue(const semantic::StructuredType& type) -> ValuePtr;
    auto knownModuleValue(const std::string& name) const -> bool;

public:
    // `Type.of(x)` sites the checker typed concretely (see
    // TypeChecker::staticTypeOfCalls). Optional: without analysis — `--no-check`
    // — the table is simply absent and every `Type.of` asks the value.
    auto setStaticTypeOfCalls(
        const std::unordered_map<const ast::MethodCall*,
                                 semantic::StaticTypeAnswer>* calls) -> void {
        m_staticTypeOfCalls = calls;
    }

private:
    const std::unordered_map<const ast::MethodCall*,
                             semantic::StaticTypeAnswer>* m_staticTypeOfCalls = nullptr;
    auto registerBitsBuiltins() -> void;
    auto registerConsoleBuiltins() -> void;
    auto registerTestBuiltins() -> void;
    auto registerProcessBuiltins() -> void;
    auto registerParserBuiltins() -> void;
    auto registerEvalBuiltins() -> void;
    auto registerHttpBuiltins() -> void;
    auto registerWebBuiltins() -> void;
    auto registerKexBuiltins() -> void;

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
    struct RuntimeSignature {
        std::string receiverType;
        std::vector<const ast::TypeExpr*> params;
    };
    std::unordered_map<std::string, std::vector<RuntimeSignature>>
        m_runtimeSignatures;
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
    std::optional<std::chrono::steady_clock::time_point> m_deadline;

    std::unordered_map<std::string, std::string> m_mockFiles;
    std::unordered_set<std::string> m_mockDirs;

    bool m_mockIO = false;
    std::string m_mockIOOutput;
    std::deque<std::string> m_mockIOInputLines;

    bool m_mockHttp = false;
    std::deque<ValuePtr> m_mockHttpResponses;

    // describe/it/assert (registerTestBuiltins) — nesting depth for
    // indentation, and pass/fail counters for the summary line printed
    // after the program finishes if any test ran.
    int m_testDepth = 0;
    int m_testsPassed = 0;
    int m_testsFailed = 0;
    struct TestHookScope {
        std::vector<ValuePtr> before;
        std::vector<ValuePtr> after;
        std::vector<ValuePtr> afterAll;
    };
    std::vector<TestHookScope> m_testHookScopes;
};

} // namespace kex::interpreter
