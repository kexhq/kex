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
#include <set>
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
    // Loads `program`'s declarations ONCE under the same cooperative deadline
    // and termination guard as evaluateFunction, then forces each named
    // binding and returns its value. That is how src/compiled/ evaluates
    // `compiled do` constants without running the program's `main`.
    //
    // "Forces" because a parameterless `let MAX = 3` is a BINDING, but the AST
    // encodes it as a zero-parameter FunctionDef and the evaluator binds the
    // name to a function value — so the value only materialises once that is
    // invoked. Loading once lets a later constant reference an earlier one.
    // A name that cannot be forced yields a null entry rather than throwing,
    // so the caller can report which one.
    // A `let %name(...)` executed during compile-time evaluation. Recorded
    // rather than acted on: the expansion pass turns each into a real
    // declaration. `bindings` is the compile-time scope at the moment it ran —
    // the loop variables a generated body closes over, which must be baked in
    // as literals since they do not exist at runtime.
    struct GeneratedDeclaration {
        std::string name;
        ast::GeneratedTemplate function;
        std::unordered_map<std::string, ValuePtr> bindings;
        SourceLocation location;
        // Declarations produced by driver loops in a generated `make`'s own
        // body — `make %v do OPS.each do |op| let %op ... end end end`. They
        // are captured here rather than at top level because they belong INSIDE
        // this make, and because they only exist once its loop variable does.
        std::vector<GeneratedDeclaration> nested;
    };
    auto generatedDeclarations() const -> const std::vector<GeneratedDeclaration>& {
        return m_generatedDeclarations;
    }

    // Declared field order of every record this Evaluator knows, the prelude's
    // included, keyed by both the bare and the module-scoped name.
    //
    // A RecordValue stores its fields in an unordered_map, which loses the
    // order — and on BEAM a record IS a tuple, so field order is the ABI. Any
    // code turning a record value back into source (src/compiled/) has to
    // recover the declared order from here rather than invent one.
    auto recordFieldOrder() const
        -> std::unordered_map<std::string, std::vector<std::string>>;

    auto evaluateConstants(
        const ast::Program& program,
        const std::vector<std::string>& names,
        std::chrono::milliseconds timeout) -> std::vector<ValuePtr>;
    // Same sandbox, but for arbitrary expressions rather than named bindings:
    // loads `program`'s declarations once, then evaluates each expression in
    // the global scope. Used by `compiled` chain collapse, which has to
    // evaluate a call chain written at a use site, not a declaration.
    //
    // An expression that FAILS yields a null entry instead of aborting the
    // run: collapse is an optimization and falls back to building the value at
    // runtime, so an expression that turns out not to be compile-time
    // evaluable is an ordinary outcome, not an error. A timeout still
    // propagates — that is the sandbox's budget for the whole program, not one
    // expression's problem.
    // One expression to evaluate, plus the free names it is allowed to carry
    // without knowing. Each is bound to a PlaceholderValue whose `index` is
    // its position in `placeholders`, so the caller can map a placeholder that
    // survives into the result back to the expression it stands for.
    struct ExpressionRequest {
        const ast::Expr* expr = nullptr;
        std::vector<std::string> placeholders;
    };
    // `reasons`, when given, receives one entry per request: empty if the
    // expression evaluated, otherwise the exception's message. Callers use it
    // to explain WHY a collapse did not happen — the interesting half, since a
    // program that fails to collapse still runs and prints the right answer.
    auto evaluateExpressions(
        const ast::Program& program,
        const std::vector<ExpressionRequest>& requests,
        std::chrono::milliseconds timeout,
        std::vector<std::string>* reasons = nullptr) -> std::vector<ValuePtr>;
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
    // Mock.* is test-only: a mock lets one part of a program lie to another
    // about the filesystem, environment, platform, network or console
    // (issue #144), so the intrinsics refuse to run unless the runtime is
    // executing a spec suite (a *.spec.kex entry), an interactive REPL, or a
    // run the user explicitly opted into with --allow-mocks. Defaults to
    // denied.
    auto setMocksAllowed(bool allowed) -> void;
    auto mocksAllowed() const -> bool { return m_mocksAllowed; }

    // Every mock's state, captured so a test can be given it back at the end
    // of its `it` block. Mock state is global and lives until cleared, which
    // is what made a forgotten `Mock.FS.clear()` leak into the next test and
    // kept specs from running in parallel (kexhq/kex#143).
    struct MockState {
        std::unordered_map<std::string, std::string> files;
        std::unordered_set<std::string> dirs;
        ValuePtr fileReader;
        bool io = false;
        std::string ioOutput;
        std::deque<std::string> ioInput;
        bool http = false;
        std::deque<ValuePtr> httpResponses;
        std::unordered_map<std::string, std::string> env;
        std::unordered_set<std::string> envUnset;
    };
    auto captureMocks() const -> MockState {
        return MockState{m_mockFiles, m_mockDirs, m_mockFileReader, m_mockIO,
                         m_mockIOOutput, m_mockIOInputLines, m_mockHttp,
                         m_mockHttpResponses, m_mockEnv, m_mockEnvUnset};
    }
    auto restoreMocks(MockState saved) -> void {
        m_mockFiles = std::move(saved.files);
        m_mockDirs = std::move(saved.dirs);
        m_mockFileReader = std::move(saved.fileReader);
        m_mockIO = saved.io;
        m_mockIOOutput = std::move(saved.ioOutput);
        m_mockIOInputLines = std::move(saved.ioInput);
        m_mockHttp = saved.http;
        m_mockHttpResponses = std::move(saved.httpResponses);
        m_mockEnv = std::move(saved.env);
        m_mockEnvUnset = std::move(saved.envUnset);
        rebuildEnvMap();
    }

private:
    // Top-level
    auto execTopLevel(const ast::TopLevelItem& item) -> void;
    auto execModule(const ast::ModuleDef& mod,
                    const std::string& parentModule = "") -> void;
    // `enclosingModule` is the module a definition lexically sits in. For a
    // module-level function it equals `typeScope`; for a `make` body method it
    // is the only record of the module, since `typeScope` names the receiver
    // type. Methods need it to reach their module's `private do` helpers.
    auto execFunctionDef(const ast::FunctionDef& def,
                         const std::string& typeScope = "",
                         bool hasImplicitReceiver = false,
                         const std::string& enclosingModule = "") -> void;
    auto execMakeDef(const ast::MakeDef& def,
                     const std::string& enclosingModule = "") -> void;
    // One member of the make target — called once per name so a union target
    // (`make Float | Integer`) registers the block under each.
    auto execMakeDefFor(const ast::MakeDef& def,
                        const std::string& typeName,
                        const std::string& enclosingModule) -> void;
    auto execTypeDef(const ast::TypeDef& def,
                     const std::string& moduleScope = "") -> void;
    auto execRecordDef(const ast::RecordDef& def, const std::string& moduleScope = "") -> void;
    auto execTraitDef(const ast::TraitDef& def) -> void;
    auto execCompiledBlock(const ast::CompiledBlock& block,
                           const std::string& moduleScope = "") -> void;
    auto execVisibilityBlock(const ast::VisibilityBlock& block,
                             const std::string& typeScope = "",
                             bool hasImplicitReceiver = false,
                             const std::string& enclosingModule = "") -> void;
    auto execUsingBlock(const ast::UsingBlock& block, const std::string& moduleScope = "") -> void;
    auto execMainBlock(const ast::MainBlock& block) -> ValuePtr;
    auto ensureModuleLoaded(const std::string& moduleName, SourceLocation loc,
                            const std::string& currentModule = "") -> std::string;
    auto resolvePendingExports() -> void;
    auto runCompileTime(const ast::Program& program,
                        std::chrono::milliseconds timeout,
                        const std::function<void()>& produce) -> void;
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
    // The first label no clause of `functionName` declares, if any.
    auto unknownNamedArgument(const std::string& functionName,
                              const NamedArgs& namedArgs) const
        -> std::optional<std::string>;
    // A module defining a same-named function whose clause does accept the
    // labels — the `using` a call site is missing.
    auto moduleSupplyingNamedClause(const std::string& functionName,
                                    const NamedArgs& namedArgs) const
        -> std::optional<std::string>;
    auto findImportedNamedOverload(const std::string& functionName,
                                   const NamedArgs& namedArgs,
                                   const ValuePtr& receiver) const
        -> std::optional<std::string>;
    auto receiverArgumentOffset(const std::string& functionName,
                                const std::vector<ValuePtr>& args) const -> size_t;
    auto makeMethodInScope(const std::string& qualified) const -> bool;
    auto resolveMethodName(const ValuePtr& receiver, const std::string& method,
                           const std::vector<ValuePtr>* args = nullptr) const
        -> std::string;
    auto registerRuntimeSignature(const ast::TypeAnnotation& annotation,
                                  const std::string& scope,
                                  bool implicitReceiver) -> void;
    auto runtimeTypeMatches(const ValuePtr& value,
                            const ast::TypeExpr& type) const -> bool;
    auto runtimeTypeKey(const ast::TypeExpr& type) const -> std::string;
    auto resolveRecordTypeName(const std::string& name) const -> std::string;

    std::unordered_map<std::string, std::vector<std::string>> m_traitMethods;
    std::unordered_map<std::string, ValuePtr> m_functionValues;

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
    // Throws unless setMocksAllowed(true) — the runtime half of "Mock.* is
    // only callable from tests". `api` is the public Kex name of the entry
    // point being denied (e.g. "Mock.FS.File"), so the error names what the
    // program actually called rather than the private intrinsic behind it.
    auto requireMocksAllowed(const std::string& api) const -> void;
    auto registerBuiltins() -> void;
    auto registerAdtConstructors() -> void;
    auto registerIOBuiltins() -> void;
    auto registerFileBuiltins() -> void;
    auto registerDirectoryBuiltins() -> void;
    auto registerMockBuiltins() -> void;
    auto registerListBuiltins() -> void;
    auto registerStringBuiltins() -> void;
    auto registerBinaryBuiltins() -> void;
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
    // How many `it` blocks failed during the last execute(). The CLI turns a
    // non-zero count into a non-zero exit status: a spec suite that reports
    // failures and exits 0 makes every CI job built on it green regardless of
    // what the specs said. Kept as a count rather than a bool so a caller can
    // say how many without re-parsing the summary line.
    auto testsFailed() const -> int { return m_testsFailed; }

    // How the describe/it DSL reports itself (kexhq/kex#199). `Pretty` is the
    // ✓/✗ prose a person reads; `Json` is one JSON record per case on stdout,
    // for an editor's test tree; `List` discovers cases WITHOUT running any
    // body, which is what a test explorer needs before the first run.
    enum class TestReportMode { Pretty, Json, List };
    auto setTestReportMode(TestReportMode mode) -> void { m_testReportMode = mode; }
    auto testReportMode() const -> TestReportMode { return m_testReportMode; }
    // Run only the cases whose full name path (enclosing describes plus the
    // `it` label) equals one of these, or sits underneath one of them. Empty
    // means "run everything".
    auto setTestFilters(std::vector<std::string> filters) -> void {
        m_testFilters = std::move(filters);
    }

    // `Type.of(x)` sites the checker typed concretely (see
    // TypeChecker::staticTypeOfCalls). Optional: without analysis — `--no-check`
    // — the table is simply absent and every `Type.of` asks the value.
    auto setStaticTypeOfCalls(
        const std::unordered_map<const ast::MethodCall*,
                                 semantic::StaticTypeAnswer>* calls) -> void {
        m_staticTypeOfCalls = calls;
    }
    auto setExpressionTypes(
        const std::unordered_map<const ast::Expr*, semantic::TypePtr>* types)
        -> void {
        m_expressionTypes = types;
    }

private:
    const std::unordered_map<const ast::MethodCall*,
                             semantic::StaticTypeAnswer>* m_staticTypeOfCalls = nullptr;
    const std::unordered_map<const ast::Expr*, semantic::TypePtr>*
        m_expressionTypes = nullptr;
    // Current server caller/reference, saved per process by Scheduler.
    ValuePtr m_servingFrom;
    auto registerBitsBuiltins() -> void;
    auto registerConsoleBuiltins() -> void;
    auto registerTestBuiltins() -> void;
    auto registerProcessBuiltins() -> void;
    auto registerDigestBuiltins() -> void;
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
    // Capability substitutions installed by `with Name = value do ... end`,
    // innermost last. A stack rather than a map so nesting and restoration
    // are automatic, and so the same capability may be replaced again inside
    // an enclosing replacement (kexhq/kex#143).
    std::vector<std::pair<std::string, ValuePtr>> m_capabilityBindings;
    auto lookupCapability(const std::string& name) const -> ValuePtr {
        for (auto it = m_capabilityBindings.rbegin();
             it != m_capabilityBindings.rend(); ++it)
            if (it->first == name) return it->second;
        return nullptr;
    }
    std::vector<PendingExport> m_pendingExports;
    std::unordered_set<std::string> m_loadingModules;
    std::vector<std::string> m_moduleRoots{"lib", "src"};
    std::vector<std::unique_ptr<std::string>> m_loadedModulePaths;
    std::vector<std::unique_ptr<ast::Program>> m_loadedModulePrograms;
    std::string m_currentModule;
    // `Type::method` of every method a module-scoped `make` defines -> that
    // module. A module-scoped `make` is import-gated like every other module
    // member, so resolution skips one whose module is not in scope; a
    // top-level `make` never appears here and stays global.
    std::unordered_map<std::string, std::string> m_makeMethodModule;
    // Type names each module declares, for the rule above.
    std::unordered_map<std::string, std::set<std::string>> m_moduleDeclaredTypes;
    // Modules brought into scope by `using`, one set per environment scope so
    // the lexical `using M do ... end` form ends with its block.
    std::vector<std::set<std::string>> m_usingModules;
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
    // Declarations recorded by `let %name(...)` during compile-time
    // evaluation; drained by src/compiled/expand.cxx.
    std::vector<GeneratedDeclaration> m_generatedDeclarations;

    // `type CommandHandler = ParsedOptions -> Integer` and friends. A named
    // type the runtime cannot recognize would otherwise narrow method
    // dispatch to nothing: an annotated parameter simply never matched, and
    // the call fell through to None instead of reporting anything.
    std::unordered_map<std::string, const ast::TypeExpr*> m_typeAliases;

    std::unordered_map<std::string, std::string> m_mockFiles;
    std::unordered_set<std::string> m_mockDirs;

    // Mock.FS.onRead — answers by RULE rather than from a fixture: content
    // derived from the path, a failure on the third call, a record of what was
    // asked for. Consulted before the map, and its `None` means "no such
    // file", so a callback can model absence too (kexhq/kex#143).
    ValuePtr m_mockFileReader;
    auto mockFileContent(const std::string& path)
        -> std::optional<std::string>;

    // See setMocksAllowed. Denied by default: an Evaluator used for
    // compile-time evaluation (compiled do, tagged-literal validation) is
    // exactly the place a dependency must not be able to install a mock.
    bool m_mocksAllowed = false;

    bool m_mockIO = false;
    std::string m_mockIOOutput;
    std::deque<std::string> m_mockIOInputLines;

    bool m_mockHttp = false;
    std::deque<ValuePtr> m_mockHttpResponses;

    // Mock.ENV — an overlay on the real environment, so a test can say what
    // `ENV` holds without the process actually having it. `unset` is separate
    // from `set` because "absent" is an answer a program can depend on.
    std::unordered_map<std::string, std::string> m_mockEnv;
    std::unordered_set<std::string> m_mockEnvUnset;
    // The environment snapshot the `ENV` capability reads: the real
    // environment with the mock overlay applied, rebuilt whenever either
    // changes. Held here rather than bound as a global `ENV` — that binding
    // shadowed the capability declared in env.kex, so `ENV.get` answered
    // through Map dispatch and `ENV.set` had nowhere to go.
    ValuePtr m_envMap;
    auto rebuildEnvMap() -> void;

    // Mock.System — what the machine claims to be. Unset means "ask the real
    // one", which is why these are optionals rather than defaulted values.

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

    // Structured reporting (kexhq/kex#199). The path is the stack of enclosing
    // `describe` labels; a case's full name is that plus its own label.
    TestReportMode m_testReportMode = TestReportMode::Pretty;
    std::vector<std::string> m_testFilters;
    std::vector<std::string> m_testPath;
    // The `it`/`describe` call currently being dispatched — the DSL's natives
    // receive only their arguments, so the call site is handed to them here.
    SourceLocation m_lastCallLocation;
    // While an `it` body runs: whether we are inside one at all, and where the
    // most recent `assert` was written. That assert is where a failure happened
    // as far as the file's reader is concerned — but only if it is in the spec
    // file: `Assert.equal`'s own `assert` lives in the stdlib, and pointing an
    // editor's decoration there would be useless, so the `it` itself is the
    // fallback. kex_test:failure_location/1 makes the same choice on BEAM.
    bool m_inTestCase = false;
    SourceLocation m_lastTestFileLocation;
    auto reportTestCase(const std::vector<std::string>& path, const char* status,
                        double durationMs, const std::string& message,
                        SourceLocation caseLoc, SourceLocation failLoc) -> void;
    auto reportTestDiscovery(const std::vector<std::string>& path,
                             const char* kind, SourceLocation loc) -> void;
    auto reportTestSummary() -> void;
    auto testCaseSelected(const std::vector<std::string>& path, bool isGroup) const
        -> bool;
    auto emitTestRecord(const std::string& json) -> void;
};

} // namespace kex::interpreter
