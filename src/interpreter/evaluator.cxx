#include "evaluator.hxx"
#include "../common/utf8.hxx"
#include "../compiled/expand.hxx"
#include "../common/prelude_loader.hxx"
#include "../common/type_def_utils.hxx"
#include "../lexer/lexer.hxx"
#include "../module/resolver.hxx"
#include "../parser/parser.hxx"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace kex::interpreter {

Evaluator::Evaluator() {
    m_globalEnv = std::make_shared<Environment>();
    m_intrinsicEnv = std::make_shared<Environment>();
    m_env = m_globalEnv;
    for (auto& root : kex::standardLibraryModuleRoots())
        if (std::find(m_moduleRoots.begin(), m_moduleRoots.end(), root) ==
            m_moduleRoots.end())
            m_moduleRoots.push_back(std::move(root));
    // Owns every process for this Evaluator's whole lifetime — there is
    // no "outside of a process" execution mode, matching BEAM, so this
    // always exists rather than being created lazily on first
    // spawn/receive use.
    m_scheduler = std::make_unique<Scheduler>(*this);
    registerBuiltins();
    // The Kex-written stdlib shadows the native builtins on every Evaluator, so
    // there is a single source of truth for stdlib behaviour regardless of entry
    // point (CLI, REPL, tests). No-op when no prelude source root is available.
    loadPrelude();
    // The prelude's type declarations (Optional, Result) re-register variant
    // constructors (Just, Ok, Error) via execTypeDef — but the generic
    // constructor loses typeParams/argParamIndex metadata that the native
    // factories (Value::just/ok/error) provide. Re-register the native
    // factories so they win and typeName() renders correctly
    // (e.g. "Result<String, ?>" not bare "Result").
    registerAdtConstructors();
}

auto Evaluator::defineIntrinsic(const std::string& name, NativeFunc fn) -> void {
    if (name.find("::") == std::string::npos)
        throw std::logic_error("intrinsic identity must be category-qualified: " + name);
    auto value = std::make_shared<Value>();
    value->data = FunctionValue{name, std::move(fn)};
    m_intrinsicEnv->define(name, std::move(value));
}

auto Evaluator::defineIntrinsic(const std::string& name, const ValuePtr& value) -> void {
    auto* function = value ? std::get_if<FunctionValue>(&value->data) : nullptr;
    if (!function || !function->native) return;
    defineIntrinsic(name, function->native);
}

auto Evaluator::defineModule(const std::string& name) -> void {
    m_globalEnv->define(name, Value::module(name));
}

auto Evaluator::definePublic(const std::string& name, NativeFunc fn) -> void {
    auto val = std::make_shared<Value>();
    val->data = FunctionValue{name, std::move(fn)};
    m_globalEnv->define(name, val);
}

auto Evaluator::defineDual(const std::string& name, NativeFunc fn) -> void {
    auto val = std::make_shared<Value>();
    val->data = FunctionValue{name, fn};
    m_globalEnv->define(name, val);
    defineIntrinsic(name, std::move(fn));
}

auto Evaluator::execute(const ast::Program& program) -> ValuePtr {
    for (const auto& item : program.items) {
        execTopLevel(item);
    }
    resolvePendingExports();

    // The top-level program itself runs as one process (see
    // Scheduler::runToCompletion) — spawn/receive/Process.self at top level
    // go through the exact same path as inside a spawned process, with no
    // special-casing. Any processes spawned here that outlive this call
    // (e.g. a server loop that never explicitly terminates, matching
    // examples/proc_ping.kex) stay alive in the Scheduler's process table
    // for a later execute() call (e.g. the next REPL line) to `send` to.
    ValuePtr lastResult = m_scheduler->runToCompletion([this, &program]() -> ValuePtr {
        ValuePtr result = Value::none();
        for (const auto& item : program.items) {
            if (auto* main = std::get_if<std::unique_ptr<ast::MainBlock>>(&item)) {
                result = execMainBlock(**main);
            }
        }
        return result;
    }, m_globalEnv);

    // describe/it/assert summary — only printed if any `it` ran, so
    // programs that don't use the testing DSL see no extra output.
    if (m_testReportMode == TestReportMode::Json) {
        reportTestSummary();
    } else if (m_testsPassed + m_testsFailed > 0 &&
               m_testReportMode == TestReportMode::Pretty) {
        std::string summary = "\n" + std::to_string(m_testsPassed) + " passed, "
            + std::to_string(m_testsFailed) + " failed\n";
        m_output += summary;
        std::cout << summary;
    }

    return lastResult;
}

auto Evaluator::evaluateFunction(
    const ast::Program& program,
    const std::string& name,
    std::vector<ValuePtr> args,
    std::chrono::milliseconds timeout) -> ValuePtr {
    m_deadline = std::chrono::steady_clock::now() + timeout;
    try {
        for (const auto& item : program.items) {
            checkDeadline();
            execTopLevel(item);
        }
        resolvePendingExports();

        // Process termination is meaningful at runtime but must become an
        // ordinary validator crash during compilation, never terminate the
        // compiler process.
        auto blockedTermination = [](std::vector<ValuePtr>) -> ValuePtr {
            throw std::runtime_error(
                "process termination is not allowed during compile-time evaluation");
        };
        definePublic("die", blockedTermination);
        defineIntrinsic("System::die", blockedTermination);
        defineIntrinsic("System::exit", blockedTermination);

        auto result = m_scheduler->runToCompletion(
            [this, &name, &args]() mutable -> ValuePtr {
                return callFunction(
                    name, std::move(args), {}, SourceLocation{});
            },
            m_globalEnv);
        m_deadline.reset();
        return result;
    } catch (...) {
        m_deadline.reset();
        throw;
    }
}

// Shared body of the compile-time sandbox: arm the deadline, block process
// termination, load `program`'s declarations, then run `produce` on the
// scheduler — for the same reason evaluateFunction's call does, since
// compile-time code may touch anything the runtime offers and the fiber
// machinery has to be live.
auto Evaluator::runCompileTime(const ast::Program& program,
                               std::chrono::milliseconds timeout,
                               const std::function<void()>& produce) -> void {
    m_deadline = std::chrono::steady_clock::now() + timeout;
    try {
        auto blockedTermination = [](std::vector<ValuePtr>) -> ValuePtr {
            throw std::runtime_error(
                "process termination is not allowed during compile-time evaluation");
        };
        definePublic("die", blockedTermination);
        defineIntrinsic("System::die", blockedTermination);
        defineIntrinsic("System::exit", blockedTermination);

        m_scheduler->runToCompletion(
            [this, &program, &produce]() -> ValuePtr {
                for (const auto& item : program.items) {
                    checkDeadline();
                    execTopLevel(item);
                }
                resolvePendingExports();
                produce();
                return Value::unit();
            },
            m_globalEnv);
        m_deadline.reset();
    } catch (...) {
        m_deadline.reset();
        throw;
    }
}

auto Evaluator::recordFieldOrder() const
    -> std::unordered_map<std::string, std::vector<std::string>> {
    std::unordered_map<std::string, std::vector<std::string>> order;
    for (const auto& [name, def] : m_recordDefs) {
        if (!def) continue;
        auto& fields = order[name];
        fields.reserve(def->fields.size());
        for (const auto& field : def->fields) fields.push_back(field.name);
    }
    return order;
}

auto Evaluator::evaluateConstants(
    const ast::Program& program,
    const std::vector<std::string>& names,
    std::chrono::milliseconds timeout) -> std::vector<ValuePtr> {
    std::vector<ValuePtr> values;
    runCompileTime(program, timeout, [&] {
        values.reserve(names.size());
        for (const auto& name : names) {
            checkDeadline();
            values.push_back(callFunction(name, {}, {}, SourceLocation{}));
        }
    });
    return values;
}

auto Evaluator::evaluateExpressions(
    const ast::Program& program,
    const std::vector<ExpressionRequest>& requests,
    std::chrono::milliseconds timeout,
    std::vector<std::string>* reasons) -> std::vector<ValuePtr> {
    std::vector<ValuePtr> values;
    auto note = [&](std::string why) {
        if (reasons) reasons->push_back(std::move(why));
    };
    runCompileTime(program, timeout, [&] {
        values.reserve(requests.size());
        for (const auto& request : requests) {
            checkDeadline();
            if (!request.expr) {
                values.push_back(nullptr);
                note("there is no expression to evaluate");
                continue;
            }
            // Each request gets its own scope, so one expression's
            // placeholders are invisible to the next and a name that is also a
            // real global is shadowed only while this expression runs.
            pushEnv();
            try {
                for (std::size_t i = 0; i < request.placeholders.size(); i++) {
                    auto placeholder = std::make_shared<Value>();
                    placeholder->data =
                        PlaceholderValue{i, request.placeholders[i]};
                    m_env->define(request.placeholders[i],
                                  std::move(placeholder));
                }
                values.push_back(eval(*request.expr));
                note("");
            } catch (const EvaluationTimeout&) {
                popEnv();
                throw;
            } catch (const std::exception& why) {
                // Not compile-time evaluable after all — a PlaceholderMisuse
                // (the builder needed a value only the running program has) or
                // anything else. Either way the caller keeps the runtime form,
                // which is always correct.
                values.push_back(nullptr);
                note(why.what());
            }
            popEnv();
        }
    });
    return values;
}

auto Evaluator::checkDeadline() const -> void {
    if (m_deadline &&
        std::chrono::steady_clock::now() >= *m_deadline)
        throw EvaluationTimeout{};
}

auto Evaluator::resolvePendingExports() -> void {
    while (!m_pendingExports.empty()) {
        auto pendingExports = std::move(m_pendingExports);
        m_pendingExports.clear();
        for (const auto& pending : pendingExports) {
            std::string targetName;
            for (size_t i = 0; i < pending.decl->module.parts.size(); ++i) {
                if (i) targetName += ".";
                targetName += pending.decl->module.parts[i];
            }
            if (targetName == pending.owner)
                throw RuntimeError("module cannot export itself", pending.decl->location);

            auto target = m_moduleRegistry.find(targetName);
            if (target == m_moduleRegistry.end()) {
                targetName = ensureModuleLoaded(targetName, pending.decl->location, pending.owner);
                target = m_moduleRegistry.find(targetName);
            }
            if (target == m_moduleRegistry.end())
                throw RuntimeError("Unknown module exported by " + pending.owner + ": " + targetName,
                                   pending.decl->location);

            for (const auto& requested : pending.decl->onlyNames)
                if (target->second.privateNames.contains(requested))
                    throw RuntimeError("cannot export private name `" + requested + "` from " + targetName,
                                       pending.decl->location);
            for (const auto& requested : pending.decl->exceptNames)
                if (target->second.privateNames.contains(requested))
                    throw RuntimeError("cannot reference private name `" + requested + "` from " + targetName,
                                       pending.decl->location);

            const auto alias = pending.decl->alias.value_or(pending.decl->module.parts.back());
            const auto viewName = pending.owner + "." + alias;
            ModuleEntry view;
            for (const auto& [name, value] : target->second.exports) {
                if (!pending.decl->onlyNames.empty()
                    && std::find(pending.decl->onlyNames.begin(), pending.decl->onlyNames.end(), name)
                        == pending.decl->onlyNames.end()) continue;
                if (std::find(pending.decl->exceptNames.begin(), pending.decl->exceptNames.end(), name)
                    != pending.decl->exceptNames.end()) continue;
                view.exports.emplace(name, value);
                m_env->define(viewName + "::" + name, value);
            }
            if (view.exports.empty())
                throw RuntimeError("module export exposes no public names from " + targetName,
                                   pending.decl->location);

            auto& owner = m_moduleRegistry[pending.owner];
            owner.submodules[alias] = viewName;
            m_moduleRegistry[viewName] = std::move(view);
            m_env->define(pending.owner + "::" + alias, Value::module(viewName));
        }
    }
}

auto Evaluator::ensureModuleLoaded(const std::string& moduleName, SourceLocation loc,
                                   const std::string& currentModule) -> std::string {
    if (m_moduleRegistry.contains(moduleName)) return moduleName;

    module::Resolver resolver(m_moduleRoots);
    auto resolved = resolver.resolve(moduleName, currentModule);
    if (!resolved) {
        if (module::Resolver::isForeignNamespace(moduleName))
            throw RuntimeError("Foreign module interop is not implemented: " + moduleName, loc);
        // `using Leaf` written inside `module Mid` reaches here already
        // qualified as `Mid.Leaf`, because a nested module is the first
        // reading. When no such nested module exists, the name meant the
        // top-level `Leaf` all along — try it before giving up, or a module
        // can never import a sibling module.
        if (const auto dot = moduleName.rfind('.'); dot != std::string::npos) {
            const auto bare = moduleName.substr(dot + 1);
            if (auto sibling = resolver.resolve(bare, "")) {
                m_loadingModules.erase(moduleName);
                return ensureModuleLoaded(sibling->moduleName, loc, "");
            }
        }
        throw RuntimeError("Unknown module: " + moduleName, loc);
    }
    const auto canonicalName = resolved->moduleName;
    if (m_moduleRegistry.contains(canonicalName)) return canonicalName;
    if (!m_loadingModules.insert(canonicalName).second) {
        // Already being loaded. For a genuine cycle the name is right and the
        // half-built module is what the caller gets. But a RELATIVE candidate
        // reaching here means the container heuristic sent us back into the
        // file we are already executing — `using Leaf` inside `module Mid`
        // resolving `Mid.Leaf` to `mid.kex` — and returning that name hands
        // the caller a module nothing will ever define. The absolute name is
        // what was meant.
        if (canonicalName != moduleName)
            if (auto sibling = module::Resolver(m_moduleRoots).resolve(moduleName, ""))
                return ensureModuleLoaded(sibling->moduleName, loc, "");
        return canonicalName;
    }

    auto path = std::make_unique<std::string>(std::move(resolved->path));
    std::ifstream input(*path);
    std::string source((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
    Lexer lexer(std::move(source), *path);
    Parser parser(lexer.tokenizeAll(), *path);
    auto program = std::make_unique<ast::Program>(parser.parseProgram());
    if (!parser.diagnostics().empty()) {
        const auto& diagnostic = parser.diagnostics().front();
        m_loadingModules.erase(canonicalName);
        throw RuntimeError("Failed to parse module " + canonicalName + ": " + diagnostic.message,
                           diagnostic.location);
    }
    // A source root can supply a leading package namespace for leaf module
    // declarations. Qualify such a declaration with the resolver's canonical
    // identity before executing it, just as the compiled module loader does.
    for (auto& item : program->items) {
        auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
        if (!module || !*module || (*module)->name.find('.') != std::string::npos)
            continue;
        const auto dot = canonicalName.rfind('.');
        if (dot != std::string::npos &&
            canonicalName.substr(dot + 1) == (*module)->name) {
            (*module)->name = canonicalName;
            break;
        }
    }
    std::vector<semantic::Diagnostic> expandDiagnostics;
    if (!compiled::expand(*program, expandDiagnostics)) {
        const auto& diagnostic = expandDiagnostics.front();
        m_loadingModules.erase(canonicalName);
        throw RuntimeError("Failed to expand module " + canonicalName + ": " +
                               diagnostic.message,
                           diagnostic.location);
    }

    m_loadedModulePaths.push_back(std::move(path));
    auto* ownedProgram = program.get();
    m_loadedModulePrograms.push_back(std::move(program));
    // A module's declarations are global for the rest of the run. Loading is
    // lazy, so this can fire from inside a function body — executing the
    // items against the CURRENT m_env would define them into that call's
    // transient scope, and every binding would vanish when the call returned:
    // the first `FS.File.read` through a wrapper function registered
    // `FS.File::read` in the wrapper's local env and the next call reported
    // `Undefined function: FS.File.read` (issue #144's secondary bug). Swap
    // to the global environment for the duration of the load.
    {
        auto savedEnv = m_env;
        m_env = m_globalEnv;
        try {
            for (const auto& item : ownedProgram->items) execTopLevel(item);
        } catch (...) {
            m_env = std::move(savedEnv);
            throw;
        }
        m_env = std::move(savedEnv);
    }
    m_loadingModules.erase(canonicalName);

    if (!m_moduleRegistry.contains(canonicalName)) {
        // The relative candidate matched a FILE without that file defining the
        // nested module: `using Leaf` inside `module Mid` tries `Mid.Leaf`
        // first, and `mid.kex` answers it, because that is how a container
        // module (`module Shop do module Cart`) is found. When the nested
        // module turns out not to exist, the absolute name is still owed a
        // look — `Leaf` means the top-level `Leaf`.
        //
        // Mirrors the same fallback in SemanticDB::ensureModule. Without it a
        // module that imports another module fails at run time having already
        // type-checked.
        if (canonicalName != moduleName)
            if (auto plain = module::Resolver(m_moduleRoots).resolve(moduleName, "")) {
                m_loadingModules.erase(canonicalName);
                return ensureModuleLoaded(plain->moduleName, loc, "");
            }
        throw RuntimeError("Resolved file does not define module " + canonicalName, loc);
    }
    return canonicalName;
}

// See TypeChecker::resolveModulePath (kexhq/kex#229) for the precedence this
// mirrors: an active `using M` aliases bare `name` when it is `M`'s last
// segment or an immediate child `M.name`; failing that, a globally unique
// loaded module ending in `.name` is a last resort — which is what lets
// `UnorderedSet.from(…)` work from the same `using Data.Set` that only
// aliases `Set` under the first rule, since `Data.Set` and `Data.UnorderedSet`
// share a file but not a parent/child relationship.
auto Evaluator::resolveNamespaceAlias(const std::string& name, SourceLocation loc)
    -> std::optional<std::string> {
    if (m_moduleRegistry.contains(name)) return name;
    auto lastSegmentOf = [](const std::string& module) {
        const auto dot = module.rfind('.');
        return dot == std::string::npos ? module : module.substr(dot + 1);
    };
    std::vector<std::string> candidates;
    for (const auto& scope : m_usingModules) {
        for (const auto& imported : scope) {
            if (lastSegmentOf(imported) == name) candidates.push_back(imported);
            if (auto it = m_moduleRegistry.find(imported);
                it != m_moduleRegistry.end()) {
                if (auto sub = it->second.submodules.find(name);
                    sub != it->second.submodules.end())
                    candidates.push_back(sub->second);
            }
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.size() == 1) {
        try {
            return ensureModuleLoaded(candidates.front(), loc, m_currentModule);
        } catch (const RuntimeError&) {
            return std::nullopt;
        }
    }
    if (!candidates.empty()) return std::nullopt;

    std::optional<std::string> unique;
    const auto suffix = "." + name;
    for (const auto& [candidate, _] : m_moduleRegistry) {
        if (candidate.size() <= suffix.size() ||
            candidate.compare(candidate.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        if (unique && *unique != candidate) return std::nullopt;
        unique = candidate;
    }
    return unique;
}

auto Evaluator::defineImported(const std::string& bindingName, const std::string& logicalName,
                               const std::string& sourceModule, bool explicitImport,
                               const std::string& moduleScope, ValuePtr value,
                               SourceLocation loc) -> void {
    ImportOrigin* existing = nullptr;
    if (!moduleScope.empty()) {
        const auto key = moduleScope + "::" + logicalName;
        if (auto it = m_moduleImportOrigins.find(key); it != m_moduleImportOrigins.end())
            existing = &it->second;
        if (!existing) m_moduleImportOrigins[key] = {sourceModule, explicitImport};
    } else {
        for (auto it = m_importScopes.rbegin(); it != m_importScopes.rend(); ++it) {
            if (auto found = it->find(logicalName); found != it->end()) {
                existing = &found->second;
                break;
            }
        }
        if (!existing) m_importScopes.back()[logicalName] = {sourceModule, explicitImport};
    }

    if (existing && existing->module != sourceModule) {
        if (explicitImport && !existing->explicitImport) {
            *existing = {sourceModule, true};
        } else if (!explicitImport && existing->explicitImport) {
            return;
        } else {
            throw RuntimeError("ambiguous name `" + logicalName + "`, imported from both `"
                               + existing->module + "` and `" + sourceModule + "`", loc);
        }
    }
    // A receiver-typed import does not SHADOW an existing binding of the same
    // name — it joins the type-directed dispatch instead. `using Units.SI`
    // otherwise rebound the plain `to` to a function whose first parameter is
    // annotated `Measure`, and since a bare-name call cannot consult that
    // annotation, every conversion in the program went there and answered
    // None. The import stays reachable: resolveMethodName finds it through
    // m_importScopes when the receiver actually matches.
    if (m_env->get(bindingName)) {
        auto defs = m_functionDefs.find(sourceModule + "::" + logicalName);
        if (defs != m_functionDefs.end())
            for (const auto* def : defs->second)
                if (def && !def->clauses.empty() &&
                    !def->clauses.front().params.empty()) {
                    const auto& first = def->clauses.front().params.front();
                    if (first.type && *first.type &&
                        std::holds_alternative<ast::TypeName>((*first.type)->kind))
                        return;
                }
    }
    m_env->define(bindingName, std::move(value));
}

auto Evaluator::loadPrelude() -> void {
    if (m_preludeLoaded) return;
    // Parse the prelude once and cache the merged declarations. The AST must
    // outlive every Evaluator (m_functionDefs keeps raw pointers into it), so
    // it is a function-local static — shared across all instances.
    static const ast::Program* preludeProgram = []() -> const ast::Program* {
        auto* prog = new ast::Program();
        auto files = kex::preludeSourceFiles();
        if (files.empty()) return prog; // no prelude available — run without
        for (const auto& f : files) {
            std::ifstream in(f);
            std::string src((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
            Lexer lex(std::move(src), f);
            Parser parser(lex.tokenizeAll(), f);
            auto parsed = parser.parseProgram();
            for (auto& item : parsed.items)
                if (!std::holds_alternative<std::unique_ptr<ast::MainBlock>>(item))
                    prog->items.push_back(std::move(item));
        }
        return prog;
    }();
    for (const auto& item : preludeProgram->items) execTopLevel(item);
    m_preludeLoaded = true;
}

auto Evaluator::setReplMode(bool enabled) -> void {
    m_replMode = enabled;
}

auto Evaluator::setMocksAllowed(bool allowed) -> void {
    m_mocksAllowed = allowed;
}

// Wording is shared with the BEAM gate (kex_test:require_mocks_allowed/1 in
// runtime/src/kex_test.erl): both backends must deny with the same line, or
// the -R parity suites diff them apart.
auto Evaluator::requireMocksAllowed(const std::string& api) const -> void {
    if (m_mocksAllowed) return;
    throw std::runtime_error(
        api + " is test-only — Mock.* runs in spec files (*.spec.kex), the "
              "REPL, or with --allow-mocks");
}

auto Evaluator::setArgs(std::vector<std::string> args) -> void {
    m_scriptArgs = std::move(args);
}

auto Evaluator::setModuleRoots(std::vector<std::string> roots) -> void {
    m_moduleRoots = std::move(roots);
}

auto Evaluator::output() const -> const std::string& {
    return m_output;
}

auto Evaluator::execTopLevel(const ast::TopLevelItem& item) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
            execModule(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
            execFunctionDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
            execMakeDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
            execRecordDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
            execTypeDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
            execTraitDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
            execCompiledBlock(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
            execUsingBlock(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
            registerRuntimeSignature(*node, "", false);
        }
    }, item);
}

auto Evaluator::execUsingBlock(const ast::UsingBlock& block,
                               const std::string& moduleScope) -> void {
    std::string moduleName;
    for (size_t i = 0; i < block.module.parts.size(); ++i) {
        if (i) moduleName += ".";
        moduleName += block.module.parts[i];
    }
    auto imported = m_moduleRegistry.find(moduleName);
    if (imported == m_moduleRegistry.end()) {
        moduleName = ensureModuleLoaded(moduleName, block.location, moduleScope);
        imported = m_moduleRegistry.find(moduleName);
        if (imported == m_moduleRegistry.end())
            throw RuntimeError("Unknown module: " + moduleName, block.location);
    }

    const bool scoped = !block.body.empty();
    if (scoped) pushEnv();
    // Records the import for make-block visibility (see makeMethodInScope) —
    // after pushEnv, so a `using M do ... end` ends with its block.
    if (m_usingModules.empty()) m_usingModules.emplace_back();
    m_usingModules.back().insert(moduleName);
    try {
        for (const auto& requested : block.onlyNames)
            if (imported->second.privateNames.contains(requested))
                throw RuntimeError("cannot import private name `" + requested
                                   + "` from " + moduleName, block.location);
        for (const auto& requested : block.exceptNames)
            if (imported->second.privateNames.contains(requested))
                throw RuntimeError("cannot reference private name `" + requested
                                   + "` from " + moduleName, block.location);
        for (const auto& [name, value] : imported->second.exports) {
            if (!block.onlyNames.empty()
                && std::find(block.onlyNames.begin(), block.onlyNames.end(), name)
                    == block.onlyNames.end()) continue;
            if (std::find(block.exceptNames.begin(), block.exceptNames.end(), name)
                != block.exceptNames.end()) continue;
            const auto binding = scoped || moduleScope.empty() ? name : moduleScope + "::" + name;
            defineImported(binding, name, moduleName, !block.onlyNames.empty(),
                           scoped ? "" : moduleScope, value, block.location);
        }
        if (block.alias) {
            const auto binding = scoped || moduleScope.empty()
                ? *block.alias : moduleScope + "::" + *block.alias;
            defineImported(binding, *block.alias, moduleName, true,
                           scoped ? "" : moduleScope, Value::module(moduleName), block.location);
        }
        for (const auto& expression : block.body)
            if (expression) eval(*expression);
    } catch (...) {
        if (scoped) popEnv();
        throw;
    }
    if (scoped) popEnv();
}

auto Evaluator::execModule(const ast::ModuleDef& mod,
                           const std::string& parentModule) -> void {
    const bool alreadyQualified =
        !parentModule.empty() &&
        mod.name.rfind(parentModule + ".", 0) == 0;
    const auto moduleName =
        parentModule.empty() || alreadyQualified
            ? mod.name
            : parentModule + "." + mod.name;
    // Publish the module shell before its body so a dependency cycle can see
    // already-known module identity while definitions are still registering.
    m_moduleRegistry.try_emplace(moduleName, ModuleEntry{});
    // The types this module declares — a `make` for one of them is that type's
    // own interface, not a patch of someone else's (see execMakeDefFor).
    kex::collectDeclaredTypeNames(mod.body, m_moduleDeclaredTypes[moduleName]);
    std::unordered_set<std::string> publicNames;
    std::unordered_set<std::string> privateNames;
    for (const auto& item : mod.body) {
        std::visit([&publicNames, &privateNames, &moduleName](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                publicNames.insert(node->name);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                publicNames.insert(node->name);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                publicNames.insert(node->name);
                if (auto constructors = kex::typeConstructors(*node))
                    for (const auto& constructor : *constructors)
                        publicNames.insert(constructor.name);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                auto shortName = node->name;
                auto prefixLen = moduleName.size() + 1;
                if (shortName.size() > prefixLen &&
                    shortName.rfind(moduleName + ".", 0) == 0)
                    shortName = shortName.substr(prefixLen);
                publicNames.insert(shortName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                for (const auto& visible : node->items) {
                    if (const auto* function =
                            std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible))
                        (node->isPublic ? publicNames : privateNames).insert((*function)->name);
                    else if (const auto* record =
                            std::get_if<std::unique_ptr<ast::RecordDef>>(&visible))
                        (node->isPublic ? publicNames : privateNames).insert((*record)->name);
                    else if (const auto* typeDef =
                            std::get_if<std::unique_ptr<ast::TypeDef>>(&visible)) {
                        (node->isPublic ? publicNames : privateNames).insert((*typeDef)->name);
                        if (auto constructors =
                                kex::typeConstructors(**typeDef))
                            for (const auto& constructor : *constructors)
                                (node->isPublic ? publicNames : privateNames)
                                    .insert(constructor.name);
                    }
                }
            }
        }, item);
    }
    const auto hasPublicNative = [this](const std::string& name) {
        auto value = m_globalEnv->get(name);
        auto* function = value ? std::get_if<FunctionValue>(&value->data) : nullptr;
        return function && function->native && !m_functionDefs.contains(name);
    };
    for (const auto& item : mod.body) {
        std::visit([this, &moduleName, &hasPublicNative](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                auto nativeName = moduleName + "::" + node->name;
                // Only an explicit PUBLIC native binding may own a public
                // module slot. A same-named private intrinsic is the runtime
                // target of the Kex wrapper, not a reason to suppress it.
                // Public-only helpers (and intentionally native-wins module
                // functions such as variadic IO calls) remain preserved.
                bool hasNative = hasPublicNative(nativeName);
                if (!hasNative && nativeName.find('.') != std::string::npos) {
                    std::string alt;
                    for (char c : moduleName)
                        alt += (c == '.') ? "::" : std::string(1, c);
                    const auto altName = alt + "::" + node->name;
                    hasNative = hasPublicNative(altName);
                }
                if (!hasNative)
                    execFunctionDef(*node, moduleName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                execModule(*node, moduleName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                execTypeDef(*node, moduleName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                execRecordDef(*node, moduleName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                execMakeDef(*node, moduleName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                execVisibilityBlock(*node, moduleName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                execTraitDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
                execCompiledBlock(*node, moduleName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                execUsingBlock(*node, moduleName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ExportDecl>>) {
                m_pendingExports.push_back({moduleName, node.get()});
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
                registerRuntimeSignature(*node, moduleName, false);
            }
        }, item);
    }

    ModuleEntry entry;
    entry.privateNames = std::move(privateNames);
    entry.submodules = std::move(m_moduleRegistry[moduleName].submodules);
    const auto prefix = moduleName + "::";
    for (const auto& name : publicNames) {
        const auto qualified = prefix + name;
        if (auto value = m_env->get(qualified))
            entry.exports.emplace(name, std::move(value));
    }
    m_moduleRegistry[moduleName] = std::move(entry);

    // Make each segment of a qualified module path available to the existing
    // namespace dispatcher. For `Http.Router.get()`, `Http` resolves to a
    // ModuleValue and `Http::Router` resolves to the nested ModuleValue.
    size_t dot = moduleName.find('.');
    if (dot == std::string::npos) {
        // Preserve public constants that deliberately share a namespace name
        // (notably the ENV Map). This is public binding ownership, not an
        // intrinsic capability check.
        auto existing = m_env->get(moduleName);
        if (!existing || std::holds_alternative<ModuleValue>(existing->data))
            m_env->define(moduleName, Value::module(moduleName));
    } else {
        const auto parent = moduleName.substr(0, dot);
        m_env->define(parent, Value::module(parent));
        while (dot != std::string::npos) {
            const auto next = moduleName.find('.', dot + 1);
            const auto child = moduleName.substr(dot + 1, next - dot - 1);
            const auto qualified = moduleName.substr(0, next);
            const auto owner = moduleName.substr(0, dot);
            m_env->define(owner + "::" + child, Value::module(qualified));
            auto& parentEntry = m_moduleRegistry[owner];
            parentEntry.submodules[child] = qualified;
            dot = next;
        }
    }

    std::vector<std::string> moduleImports;
    for (const auto& [name, _] : m_moduleImportOrigins)
        if (name.rfind(prefix, 0) == 0) moduleImports.push_back(name);
    for (const auto& name : moduleImports) {
        m_env->erase(name);
        m_moduleImportOrigins.erase(name);
    }
}

auto Evaluator::execTraitDef(const ast::TraitDef& def) -> void {
    auto& required = m_traitMethods[def.name];
    required.clear();
    for (const auto& item : def.body)
        if (auto* annotation =
                std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item);
            annotation && *annotation)
            required.push_back((*annotation)->name);
    // Register default method implementations from the trait body under
    // the trait name so `make X, implement: Trait` can inherit them.
    for (const auto& item : def.body) {
        if (auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            execFunctionDef(**fn, def.name, true);
        }
    }
}

auto Evaluator::execCompiledBlock(const ast::CompiledBlock& block,
                                  const std::string& moduleScope) -> void {
    // Execute compiled block items as if they were regular module items.
    // The interpreter doesn't distinguish compile-time vs runtime evaluation;
    // function defs and make blocks are simply registered in the environment.
    for (const auto& item : block.items) {
        std::visit([this, &moduleScope](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node, moduleScope);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                execMakeDef(*node, moduleScope);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                execRecordDef(*node, moduleScope);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                execTypeDef(*node, moduleScope);
            } else if constexpr (std::is_same_v<T, ast::ExprPtr>) {
                if (node) {
                    const auto saved = m_currentModule;
                    m_currentModule = moduleScope;
                    try {
                        eval(*node);
                    } catch (...) {
                        m_currentModule = saved;
                        throw;
                    }
                    m_currentModule = saved;
                }
            }
        }, item);
    }
}

auto Evaluator::execTypeDef(const ast::TypeDef& def,
                            const std::string& moduleScope) -> void {
    // Register sum-type variant constructors. Zero-arg variants (Fizz,
    // None, ...) are stored directly as VariantValue in the environment.
    // With-arg constructors (Just(A), Ok(A), ...) are registered as
    // callable functions that build a VariantValue with a positional args
    // list. Both kinds get an entry in m_variantParent so `make TypeName
    // do ... end` method dispatch can map the variant tag back to the
    // declaring type.
    //
    // Skip transparent type aliases (single bare TypeName, e.g.
    // `type FilePath = String`) — they declare a name for an existing
    // type rather than introducing new variant constructors.
    // Remember what an alias stands for. Only single-target aliases — a bare
    // name (`type FilePath = String`) or a callable shape
    // (`type Handler = Request -> Response`) — since those are the ones a
    // parameter annotation names when the runtime has no such type of its
    // own to match against.
    if (def.variants && def.variants->size() == 1 && (*def.variants)[0]) {
        const auto& only = (*def.variants)[0];
        if (std::holds_alternative<ast::FunctionType>(only->kind) ||
            std::holds_alternative<ast::BlockType>(only->kind) ||
            kex::isTransparentTypeAlias(def))
            m_typeAliases[def.name] = only.get();
    }
    if (def.variants) {
        if (kex::isTransparentTypeAlias(def)) return;
        for (const auto& variant : *def.variants) {
            if (!variant) continue;
            std::string variantName;
            size_t arity = 0;
            if (auto* generic = std::get_if<ast::GenericType>(&variant->kind)) {
                if (generic->name.parts.empty()) continue;
                variantName = generic->name.parts.back();
                arity = generic->args.size();
            } else if (auto* plain = std::get_if<ast::TypeName>(&variant->kind)) {
                if (plain->parts.empty()) continue;
                variantName = plain->parts.back();
            } else {
                continue;
            }

            m_variantParent[variantName] = def.name;

            if (arity == 0) {
                const auto binding = moduleScope.empty()
                    ? variantName : moduleScope + "::" + variantName;
                m_env->define(binding,
                              Value::variant(variantName, def.name));
                continue;
            }
            auto val = std::make_shared<Value>();
            val->data = FunctionValue{variantName,
                [variantName, defName = def.name, arity](std::vector<ValuePtr> args) -> ValuePtr {
                    std::vector<ValuePtr> varArgs;
                    for (size_t i = 0; i < arity; i++) {
                        varArgs.push_back(i < args.size() ? args[i] : Value::none());
                    }
                    return Value::variant(variantName, defName, std::move(varArgs));
                }};
            const auto binding = moduleScope.empty()
                ? variantName : moduleScope + "::" + variantName;
            m_env->define(binding, val);
        }
    }
}

auto Evaluator::execRecordDef(const ast::RecordDef& def, const std::string& moduleScope) -> void {
    const auto typeName = moduleScope.empty()
        ? def.name : moduleScope + "." + def.name;
    if (moduleScope.empty()) {
        m_env->define(def.name, Value::record(typeName, {}));
        m_recordDefs[def.name] = &def;
    }
    if (!moduleScope.empty()) {
        const auto scoped = moduleScope + "::" + def.name;
        m_env->define(scoped, Value::record(typeName, {}));
        m_recordDefs[scoped] = &def;
        m_recordDefs[typeName] = &def;
    }
}

auto Evaluator::resolveRecordTypeName(const std::string& name) const
    -> std::string {
    if (m_recordDefs.count(name)) return name;
    if (!m_currentModule.empty()) {
        auto scope = m_currentModule;
        while (!scope.empty()) {
            const auto candidate = scope + "." + name;
            if (m_recordDefs.count(candidate)) return candidate;
            const auto dot = scope.rfind('.');
            if (dot == std::string::npos) break;
            scope.resize(dot);
        }
    }
    // An imported record may be written partially qualified: `Router.Config`
    // inside `using Http` is `Http.Router.Config`. Getting this wrong is not
    // a lookup miss but a wrong VALUE — the construction would carry an
    // unknown type name, skip its field defaults, and then look like a
    // namespace placeholder to method dispatch.
    for (auto scope = m_usingModules.rbegin(); scope != m_usingModules.rend();
         ++scope)
        for (const auto& imported : *scope)
            if (m_recordDefs.count(imported + "." + name))
                return imported + "." + name;
    if (auto value = m_env->get(name))
        if (const auto* record = std::get_if<RecordValue>(&value->data))
            if (record->fields.empty() && m_recordDefs.count(record->typeName))
                return record->typeName;
    // Last resort: an unambiguous suffix match. Ambiguity is the case that
    // module-qualified identity exists to keep apart, so it stays unresolved.
    const auto suffix = "." + name;
    std::string unique;
    for (const auto& [candidate, _] : m_recordDefs) {
        if (candidate.size() <= suffix.size() ||
            candidate.compare(candidate.size() - suffix.size(), suffix.size(),
                              suffix) != 0)
            continue;
        if (!unique.empty() && unique != candidate) return name;
        unique = candidate;
    }
    if (!unique.empty()) return unique;
    return name;
}

auto Evaluator::execVisibilityBlock(const ast::VisibilityBlock& block,
                                    const std::string& typeScope,
                                    bool hasImplicitReceiver,
                                    const std::string& enclosingModule) -> void {
    std::unordered_set<std::string> importsBefore;
    const auto prefix = typeScope.empty() ? std::string{} : typeScope + "::";
    for (const auto& [name, _] : m_moduleImportOrigins)
        if (!prefix.empty() && name.rfind(prefix, 0) == 0) importsBefore.insert(name);

    // A `private do` directly inside a module carries the module in typeScope;
    // one inside a `make` body is already handed the module explicitly.
    const auto& module =
        !enclosingModule.empty() || hasImplicitReceiver ? enclosingModule : typeScope;

    for (const auto& item : block.items) {
        std::visit([this, &typeScope, hasImplicitReceiver,
                    &module](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node, typeScope, hasImplicitReceiver, module);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                execMakeDef(*node, module);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                execTypeDef(*node, typeScope);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                execRecordDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                execUsingBlock(*node, typeScope);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
                registerRuntimeSignature(*node, typeScope, hasImplicitReceiver);
            }
        }, item);
    }
    if (!prefix.empty()) {
        std::vector<std::string> scopedImports;
        for (const auto& [name, _] : m_moduleImportOrigins)
            if (name.rfind(prefix, 0) == 0 && !importsBefore.contains(name))
                scopedImports.push_back(name);
        for (const auto& name : scopedImports) {
            m_env->erase(name);
            m_moduleImportOrigins.erase(name);
        }
    }
}

auto Evaluator::execFunctionDef(const ast::FunctionDef& def,
                                const std::string& typeScope,
                                bool hasImplicitReceiver,
                                const std::string& enclosingModule) -> void {
    // `let X = X` in a module aliases an outer X. Bind the value it names
    // right now rather than leaving a body to re-resolve at call time: after
    // `using M`, the bare name refers to THIS binding, so a lazy body would
    // read itself — unbounded recursion, and the interpreter has no call-depth
    // guard to turn that into a diagnostic. Folding it also makes the alias
    // genuinely the same value, so it matches the same patterns as the
    // original rather than merely evaluating to something equal.
    if (!hasImplicitReceiver && !typeScope.empty() && def.clauses.size() == 1) {
        const auto& clause = def.clauses[0];
        if (!clause.hasParamList && clause.params.empty()
            && clause.body.size() == 1 && clause.body[0]) {
            const auto& referenced = clause.body[0]->kind;
            const auto* upper = std::get_if<ast::UpperIdentifier>(&referenced);
            const auto* lower = std::get_if<ast::Identifier>(&referenced);
            const auto name = upper ? upper->name : lower ? lower->name : std::string{};
            if (name == def.name)
                if (auto aliased = m_env->get(def.name)) {
                    m_env->define(typeScope + "::" + def.name, std::move(aliased));
                    return;
                }
        }
    }

    // Collect clauses: if there's already a function with this name, merge
    auto existing = m_env->get(def.name);
    std::vector<const ast::FunctionClause*> allClauses;

    // Get clauses from existing definition if it's the same function
    if (existing) {
        if (auto* fv = std::get_if<FunctionValue>(&existing->data)) {
            if (fv->name == def.name && fv->native) {
                // It's a previous definition with clauses stored in the closure
                // We'll rebuild with all clauses
            }
        }
    }

    // We store pointers to all known clauses for this function
    // Since the AST is stable, we collect them via a vector in the closure
    // For multi-def functions, each call to execFunctionDef appends
    struct ClauseStore {
        std::vector<const ast::FunctionDef*> defs;
    };
    auto store = std::make_shared<ClauseStore>();

    // If existing, retrieve its store
    if (existing) {
        // Can't easily retrieve — just rebuild from this def
    }

    store->defs.push_back(&def);

    // Check if already defined — merge
    if (existing) {
        if (auto* fv = std::get_if<FunctionValue>(&existing->data)) {
            // Unwrap and rebuild — for simplicity, use a global registry
        }
    }

    // Register under mangled name if in a type scope
    std::string regName = def.name;
    if (!typeScope.empty()) {
        regName = typeScope + "::" + def.name;
    }

    m_functionDefs[regName].push_back(&def);

    // Capture regName by value so the closure always looks up the current
    // vector from the map — avoids a dangling pointer when unordered_map
    // rehashes after a new key is inserted for a different function.
    auto funcValue = std::make_shared<Value>();
    // Module functions are namespaced but have no implicit receiver. Only
    // functions scoped to a make/record type use the UFCS receiver slot, so
    // for those `typeScope` names the receiver and the module — the scope that
    // decides which `private do` helpers are reachable — arrives separately.
    const std::string moduleScope = hasImplicitReceiver ? enclosingModule : typeScope;
    bool isMethod = hasImplicitReceiver;
    std::vector<std::pair<std::string, ValuePtr>> capturedImports;
    if (!moduleScope.empty()) {
        const auto prefix = moduleScope + "::";
        for (const auto& [name, _] : m_moduleImportOrigins)
            if (name.rfind(prefix, 0) == 0)
                if (auto value = m_env->get(name)) capturedImports.push_back({name, value});
    }
    funcValue->data = FunctionValue{def.name, [this, regName, isMethod, moduleScope,
                                               capturedImports](std::vector<ValuePtr> args) -> ValuePtr {
        struct ModuleScopeGuard {
            std::string& current;
            std::string saved;
            ~ModuleScopeGuard() { current = std::move(saved); }
        } guard{m_currentModule, m_currentModule};
        if (!moduleScope.empty()) m_currentModule = moduleScope;
        std::set<std::string> typedSignatures;
        for (const auto* candidateDef : m_functionDefs.at(regName))
            for (const auto& candidateClause : candidateDef->clauses) {
                std::string signature;
                for (const auto& param : candidateClause.params) {
                    signature += "|";
                    signature += param.type && *param.type
                        ? runtimeTypeKey(**param.type) : "*";
                }
                typedSignatures.insert(std::move(signature));
            }
        const bool dispatchByParamType = typedSignatures.size() > 1;
        for (const auto* funcDef : m_functionDefs.at(regName)) {
            for (const auto& clause : funcDef->clauses) {
                const bool firstParamIsThisPattern = !clause.params.empty()
                    && clause.params[0].pattern
                    && *clause.params[0].pattern
                    && std::holds_alternative<ast::ThisPattern>(
                        (*clause.params[0].pattern)->kind);
                size_t requiredParams = 0;
                for (const auto& param : clause.params)
                    if (!param.defaultValue) requiredParams++;
                size_t argOffset = 0;
                if (!firstParamIsThisPattern && isMethod && !args.empty() &&
                    (args.size() == requiredParams + 1 ||
                     args.size() > clause.params.size()))
                    argOffset = 1;

                std::vector<std::pair<std::string, ValuePtr>> dictionaries;
                for (std::size_t i = 0; i < clause.params.size() &&
                                        i + argOffset < args.size(); ++i) {
                    const auto& param = clause.params[i];
                    if (!param.name || !param.type || !*param.type) continue;
                    const auto* type = std::get_if<ast::TypeName>(
                        &(*param.type)->kind);
                    if (!type || type->parts.size() != 1) continue;
                    auto trait = m_traitMethods.find(type->parts.front());
                    if (trait == m_traitMethods.end()) continue;
                    for (const auto& method : trait->second) {
                        auto methodArgs =
                            std::vector<ValuePtr>{args[i + argOffset]};
                        auto resolved = resolveMethodName(args[i + argOffset], method,
                                                          &methodArgs);
                        auto target = m_env->get(resolved);
                        if (!target)
                            if (auto global = m_functionValues.find(resolved);
                                global != m_functionValues.end())
                                target = global->second;
                        if (!target) {
                            const auto fallback = type->parts.front() + "::" + method;
                            target = m_env->get(fallback);
                            if (!target)
                                if (auto global = m_functionValues.find(fallback);
                                    global != m_functionValues.end())
                                    target = global->second;
                        }
                        if (!target) target = m_env->get(method);
                        if (!target) continue;
                        auto dictionary = std::make_shared<Value>();
                        dictionary->data = FunctionValue{
                            resolved,
                            [target](std::vector<ValuePtr> callArgs) {
                                auto* function = std::get_if<FunctionValue>(
                                    &target->data);
                                if (!function || !function->native)
                                    return Value::none();
                                return function->native(std::move(callArgs));
                            }};
                        dictionaries.push_back({
                            "__trait_dictionary__" + *param.name + "::" +
                                method,
                            std::move(dictionary)});
                    }
                }
                pushEnv();
                for (const auto& [name, value] : capturedImports) m_env->define(name, value);
                for (const auto& [name, value] : dictionaries)
                    m_env->define(name, value);
                if (funcDef->isSlot && m_servingFrom)
                    m_env->define("from", m_servingFrom);
                bool matched = true;

                // UFCS: if this function is a type-scoped method (name contains "::"),
                // the first arg is always "this" (the receiver). Fall back to the old
                // args.size() > params heuristic for free functions that might receive
                // extra args through some other path.
                // Decide whether args[0] is the implicit receiver ("this") or
                // an explicit first-param match.
                //
                // Rules:
                // 1. If the first param is a ThisPattern (@Pat), it explicitly
                //    matches the receiver — argOffset stays 0.
                // 2. If args has one MORE element than the (non-default) params
                //    and this is a type-scoped method, the extra arg is the
                //    receiver — argOffset = 1. This handles `from(table)` called
                //    as `q.from(:users)` (args=[q,:users], params=[table]).
                // 3. Legacy: any extra arg beyond params is the receiver (the
                //    old `args.size() > clause.params.size()` check). Handles
                //    pre-@ make-block functions like `let pub(b) = b.priv`.
                if (firstParamIsThisPattern) {
                    // @Pat: receiver is pattern-matched as first param; bind this for @field access
                    if (!args.empty()) {
                        m_env->define("this", args[0]);
                        m_env->define("new", args[0], /*isMutable=*/true);
                    }
                } else if (isMethod && !args.empty()
                           && args.size() == requiredParams + 1) {
                    // Type-scoped method called with exactly required-params + 1 args:
                    // the extra arg is the receiver.
                    m_env->define("this", args[0]);
                    m_env->define("new", args[0], /*isMutable=*/true);
                } else if (isMethod && args.size() > clause.params.size()) {
                    // Legacy fallback: more args than declared params → first is receiver.
                    m_env->define("this", args[0]);
                    m_env->define("new", args[0], /*isMutable=*/true);
                }

                for (size_t i = 0; i < clause.params.size(); i++) {
                    const auto& param = clause.params[i];
                    if ((i + argOffset) < args.size()) {
                        if (dispatchByParamType && param.type && *param.type &&
                            !runtimeTypeMatches(
                                args[i + argOffset], **param.type)) {
                            matched = false;
                            break;
                        }
                        if (param.pattern && *param.pattern) {
                            if (!matchPattern(**param.pattern, args[i + argOffset])) {
                                matched = false;
                                break;
                            }
                        } else if (param.name.has_value()) {
                            m_env->define(*param.name, args[i + argOffset]);
                        }
                    } else if (param.defaultValue && *param.defaultValue) {
                        // No arg provided — use the default value
                        if (param.name.has_value()) {
                            m_env->define(*param.name, eval(**param.defaultValue));
                        }
                    }
                    // No arg and no default: leave unbound (may cause runtime error if accessed)
                }

                // Reject a clause that can't consume all the (post-receiver)
                // args, so a lower-arity overload doesn't silently drop them
                // (e.g. `sort/1`/`count/1` swallowing `.sort(cmp)`/`.count(pred)`
                // instead of dispatching to the /2 form). Fewer args than params
                // is still fine (defaults / unbound).
                if (matched && args.size() > argOffset + clause.params.size())
                    matched = false;

                if (matched) {
                    // catch(...) (not just ReturnException) so a RuntimeError
                    // — e.g. a failed `assert` caught higher up by `it` — still
                    // pops this scope before propagating; otherwise m_env
                    // leaks one level deep for the rest of the program (see
                    // the identical guard on the MatchExpr clause loop above).
                    try {
                        auto result = evalBody(clause.body);
                        popEnv();
                        return result;
                    } catch (TryException& e) {
                        if (clause.rescue) {
                            try {
                                auto result = evalRescue(*clause.rescue, e.error(), funcDef->location);
                                popEnv();
                                return result;
                            } catch (ReturnException& ret) {
                                popEnv();
                                return ret.value();
                            }
                        }
                        popEnv();
                        // No rescue — this call's result IS Error(e). Return
                        // it; throwing a ReturnException here would escape
                        // THIS frame (a throw inside a catch handler skips
                        // that try's sibling handlers) and be caught by the
                        // caller instead — which silently ended `main` when a
                        // `.try` failure propagated out of a nested call.
                        auto errorVal = std::make_shared<Value>();
                        errorVal->data = VariantValue{"Error", "Result", {e.error()}, {}, {}};
                        return errorVal;
                    } catch (ReturnException& ret) {
                        popEnv();
                        return ret.value();
                    } catch (const BreakException&) {
                        popEnv();
                        throw RuntimeError("'break' used outside a loop", funcDef->location);
                    } catch (const NextException&) {
                        popEnv();
                        throw RuntimeError("'next' used outside a loop", funcDef->location);
                    } catch (...) {
                        popEnv();
                        throw;
                    }
                }

                popEnv();
            }
        }

        return Value::none();
    }};

    m_env->define(regName, funcValue);
    m_functionValues[regName] = funcValue;
}

auto Evaluator::execMakeDef(const ast::MakeDef& def,
                            const std::string& enclosingModule) -> void {
    // A union target (`make Float | Integer`) applies the block to every
    // member, so registration runs once per name. A block with no nameable
    // target still runs once with an empty name, as it always has.
    auto targetNames = kex::makeTargetNames(def.target);
    if (targetNames.empty()) targetNames.push_back("");
    for (const auto& typeName : targetNames)
        execMakeDefFor(def, typeName, enclosingModule);
}

auto Evaluator::execMakeDefFor(const ast::MakeDef& def,
                               const std::string& typeName,
                               const std::string& enclosingModule) -> void {

    // A method's call arity: AST param count, +1 for the implicit `this` unless
    // the first param IS the receiver (an `@`/record/range pattern). So
    // `count(@[])` is arity 1 but `count(pred)` is arity 2 — a type may define
    // both, and a trait default for one must NOT be blocked by the other.
    auto arityOf = [](const ast::FunctionDef* fd) -> size_t {
        if (!fd || fd->clauses.empty()) return 1;
        const auto& params = fd->clauses[0].params;
        if (params.empty()) return 1;
        const auto& p0 = params[0];
        bool recv = p0.pattern && *p0.pattern &&
            (std::holds_alternative<ast::ThisPattern>((*p0.pattern)->kind) ||
             std::holds_alternative<ast::RecordPattern>((*p0.pattern)->kind) ||
             std::holds_alternative<ast::RangePattern>((*p0.pattern)->kind));
        return recv ? params.size() : params.size() + 1;
    };

    // Collect the make block's own methods keyed by (name, arity).
    std::set<std::pair<std::string, size_t>> ownMethods;
    auto addOwn = [&](const ast::FunctionDef* fd) { if (fd) ownMethods.insert({fd->name, arityOf(fd)}); };
    for (const auto& item : def.body) {
        if (auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item))
            addOwn(fn->get());
        else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item))
            if (*vb) for (const auto& vi : (*vb)->items)
                if (auto* vf = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                    addOwn(vf->get());
    }

    // A module-scoped `make` is import-gated when it patches a FOREIGN type:
    // remember which module its methods came from, so resolveMethodName can
    // skip them where that module is not in scope. A `make` for a type the
    // same module declares is that type's own interface and travels with its
    // values (`SQL.select(:all).from(:users)`), so it stays reachable — as does
    // every top-level `make`.
    if (!enclosingModule.empty() && !typeName.empty() &&
        !m_moduleDeclaredTypes[enclosingModule].count(typeName))
        for (const auto& [method, arity] : ownMethods) {
            (void)arity;
            m_makeMethodModule[typeName + "::" + method] = enclosingModule;
        }

    // Process the make block's own methods.
    for (const auto& item : def.body) {
        std::visit([this, &typeName, &enclosingModule](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node, typeName, true, enclosingModule);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                execVisibilityBlock(*node, typeName, true, enclosingModule);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
                registerRuntimeSignature(*node, typeName, true);
            }
        }, item);
    }

    // Inherit default methods from implemented traits.
    for (const auto& traitName : def.implements) {
        std::string prefix = traitName + "::";
        std::vector<const ast::FunctionDef*> inheritedMethods;
        for (const auto& [key, fns] : m_functionDefs) {
            if (key.rfind(prefix, 0) != 0) continue; // doesn't start with prefix
            for (const auto* traitFn : fns) {
                if (!traitFn) continue;
                if (traitFn->clauses.empty() || traitFn->clauses[0].body.empty()) continue;
                if (ownMethods.count({traitFn->name, arityOf(traitFn)})) continue;
                inheritedMethods.push_back(traitFn);
            }
        }
        // execFunctionDef inserts into m_functionDefs. Do that only after
        // traversal: inserting while iterating an unordered_map can rehash it,
        // invalidating the iterator and silently dropping later trait methods.
        for (const auto* traitFn : inheritedMethods)
            execFunctionDef(*traitFn, typeName, true);
    }
}

auto Evaluator::execMainBlock(const ast::MainBlock& block) -> ValuePtr {
    if (!m_replMode && !block.synthetic) pushEnv();
    // main(args) or main(args, env) — bind script arguments and optionally
    // the ENV snapshot (Map<String, String>) to the declared parameters.
    if (!block.params.empty()) {
        std::vector<ValuePtr> elems;
        for (const auto& arg : m_scriptArgs) elems.push_back(Value::string(arg));
        auto argsValue = Value::list(std::move(elems));
        const auto& param = block.params[0];
        if (param.pattern && *param.pattern) {
            matchPattern(**param.pattern, argsValue);
        } else if (param.name) {
            m_env->define(*param.name, argsValue);
        }
        if (block.params.size() >= 2) {
            auto envValue = m_globalEnv->get("ENV");
            if (!envValue) { envValue = std::make_shared<Value>(); envValue->data = MapValue{}; }
            const auto& envParam = block.params[1];
            if (envParam.name) m_env->define(*envParam.name, envValue);
        }
    }
    ValuePtr result;
    try {
        result = evalBody(block.body);
    } catch (TryException& e) {
        if (block.rescue) {
            try {
                result = evalRescue(*block.rescue, e.error(), block.location);
            } catch (ReturnException& ret) {
                result = ret.value();
            }
        } else {
            if (!m_replMode && !block.synthetic) popEnv();
            throw RuntimeError(".try failed with no rescue handler: " + e.error()->toString(),
                               block.location);
        }
    } catch (ReturnException& ret) {
        result = ret.value();
    } catch (const BreakException&) {
        if (!m_replMode && !block.synthetic) popEnv();
        throw RuntimeError("'break' used outside a loop", block.location);
    } catch (const NextException&) {
        if (!m_replMode && !block.synthetic) popEnv();
        throw RuntimeError("'next' used outside a loop", block.location);
    }
    if (!m_replMode && !block.synthetic) popEnv();
    return result;
}

auto Evaluator::evalBody(const std::vector<ast::ExprPtr>& body) -> ValuePtr {
    ValuePtr last = Value::none();
    for (const auto& expr : body) {
        if (expr) last = eval(*expr);
    }
    return last;
}

auto Evaluator::eval(const ast::Expr& expr) -> ValuePtr {
    checkDeadline();
    // Inside an `it` body, remember where the last `assert` was written, so a
    // failure can be reported at a line an editor can decorate (kexhq/kex#199).
    // Costs one comparison outside tests, where m_inTestCase is false.
    //
    // Asserts only — not every subexpression, and not every call. The assert is
    // the place a reader would point at, rather than the comparison inside its
    // argument; and it is exactly what the BEAM side can report (see
    // kex_test:assert_at/2,3), so both backends name the same line for the same
    // failure. Every assert is recorded, wherever it lives; whether the one that
    // failed belongs to the SPEC FILE is decided when the case ends (see the
    // `it` builtin), exactly as kex_test:failure_location/1 decides it on BEAM.
    if (m_inTestCase && expr.location.line > 0) {
        if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind);
            call && call->name == "assert")
            m_lastTestFileLocation = expr.location;
    }
    return std::visit([this, &expr](const auto& node) -> ValuePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::IntLiteral>) {
            try {
                return Value::integer(std::stoll(node.value));
            } catch (const std::out_of_range&) {
                // Too big for int64_t — Integer is arbitrary precision by
                // default, so this is a normal value, not an error.
                return Value::bigInteger(mpz_class(node.value));
            }
        }
        else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
            // A literal too large for a double (1.0e999) rounds to Infinity,
            // which is not a Kex Float — report it as the out-of-range
            // literal it is rather than letting it through.
            double v = 0.0;
            try {
                v = std::stod(node.value);
            } catch (const std::out_of_range&) {
                throw RuntimeError("Float literal out of range: " + node.value, expr.location);
            }
            if (auto error = nonFiniteFloatError(v, "Float literal " + node.value))
                throw RuntimeError(*error, expr.location);
            return Value::floating(v);
        }
        else if constexpr (std::is_same_v<T, ast::GeneratedDecl>) {
            // `let %name(...)` — RECORD a declaration rather than binding
            // anything. Only meaningful during compile-time evaluation; at
            // runtime the expansion pass has already replaced it, so reaching
            // here means the pass did not run.
            std::string name;
            if (node.name) {
                auto resolved = eval(*node.name);
                if (auto* text = std::get_if<StringValue>(&resolved->data))
                    name = text->value;
                else if (auto* atom = std::get_if<AtomValue>(&resolved->data))
                    name = atom->name;
                else
                    name = resolved->toString();
            }
            // Snapshot the compile-time scope the body closes over. Innermost
            // binding wins, and the global scope is excluded: those names still
            // exist at runtime and must NOT be baked in as literals.
            std::unordered_map<std::string, ValuePtr> bindings;
            for (auto scope = m_env; scope && scope != m_globalEnv;
                 scope = scope->parent())
                for (const auto& bound : scope->names())
                    bindings.emplace(bound, scope->get(bound));
            // A generated `make` may hold driver loops of its own. Run them
            // HERE, in the scope that has this make's loop variable bound, and
            // take whatever they declared as this make's methods — they are
            // not top-level declarations and must not be recorded as such.
            std::vector<GeneratedDeclaration> nested;
            if (const auto* makeTemplate =
                    std::get_if<std::shared_ptr<ast::MakeDef>>(&node.function)) {
                if (*makeTemplate) {
                    const auto before = m_generatedDeclarations.size();
                    for (const auto& item : (*makeTemplate)->body)
                        if (const auto* driver =
                                std::get_if<ast::ExprPtr>(&item))
                            if (*driver) eval(**driver);
                    nested.assign(
                        std::make_move_iterator(
                            m_generatedDeclarations.begin() +
                            static_cast<long>(before)),
                        std::make_move_iterator(m_generatedDeclarations.end()));
                    m_generatedDeclarations.resize(before);
                }
            }
            m_generatedDeclarations.push_back(
                {std::move(name), node.function, std::move(bindings),
                 expr.location, std::move(nested)});
            return Value::unit();
        }
        else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            if (node.parts.empty()) return Value::string(node.value);
            // The parser already split the body and wrapped each interpolated
            // expression in its show-protocol call, so every value here
            // evaluates to a String the prelude produced. Concatenate.
            std::string result;
            for (size_t i = 0; i < node.parts.size(); i++) {
                result += node.parts[i];
                if (i < node.values.size() && node.values[i])
                    result += eval(*node.values[i])->toString();
            }
            return Value::string(result);
        }
        else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
            return Value::character(node.value);
        }
        else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
            return Value::boolean(node.value);
        }
        else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
            return Value::atom(node.name);
        }
        else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
            auto val = m_env->get("this");
            if (!val) {
                throw RuntimeError("'this' used outside of a method context", expr.location);
            }
            return val;
        }
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
            auto val = m_env->get(node.name);
            if (!val) {
                throw RuntimeError("Undefined variable: " + node.name, expr.location);
            }
            return autoCallZeroArgConstant(node.name, val);
        }
        else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            auto value = node.value ? eval(*node.value) : Value::none();
            if (auto* varPat = std::get_if<ast::VarPattern>(&node.pattern->kind)) {
                m_env->define(varPat->name, value);
            } else if (auto* tuplePat = std::get_if<ast::TuplePattern>(&node.pattern->kind)) {
                if (auto* tupleVal = std::get_if<TupleValue>(&value->data)) {
                    // Delegate each element to the general matchPattern() so
                    // nested patterns (e.g. `let (JsonString(key), rest) =
                    // ...`, a ConstructorPattern element) bind correctly —
                    // not just bare variable names.
                    for (size_t i = 0; i < tuplePat->elements.size() && i < tupleVal->elements.size(); i++) {
                        matchPattern(*tuplePat->elements[i], tupleVal->elements[i]);
                    }
                }
            } else if (auto* recPat = std::get_if<ast::RecordPattern>(&node.pattern->kind)) {
                // `field.pattern` holds the rename/sub-pattern for
                // `{ "key": shortName }` or `{ field: subPattern }` — must
                // bind/recurse through it when present; only fall back to
                // `field.name` (the key itself) for the shorthand `{ name }`
                // form with no explicit pattern.
                if (auto* recVal = std::get_if<RecordValue>(&value->data)) {
                    if (!recPat->typeName.empty() && recVal->typeName != recPat->typeName)
                        throw RuntimeError("pattern mismatch — expected record " +
                                           recPat->typeName, expr.location);
                    for (const auto& field : recPat->fields) {
                        if (auto it = recVal->fields.find(field.name); it != recVal->fields.end()) {
                            if (field.pattern && *field.pattern) {
                                matchPattern(**field.pattern, it->second);
                            } else {
                                m_env->define(field.name, it->second);
                            }
                        }
                    }
                } else if (recPat->typeName.empty()) {
                  // The anonymous `{ k }` form also destructures maps by key; a
                  // named `Foo { k }` is record-only (maps carry no type name).
                  if (auto* mapVal = std::get_if<MapValue>(&value->data)) {
                    for (const auto& field : recPat->fields) {
                        for (const auto& [k, v] : mapVal->entries) {
                            if (auto* sk = std::get_if<StringValue>(&k->data)) {
                                if (sk->value == field.name) {
                                    if (field.pattern && *field.pattern) {
                                        matchPattern(**field.pattern, v);
                                    } else {
                                        m_env->define(field.name, v);
                                    }
                                }
                            }
                        }
                    }
                  }
                }
            } else if (auto* constrPat = std::get_if<ast::ConstructorPattern>(&node.pattern->kind)) {
                if (!matchPattern(*node.pattern, value))
                    throw RuntimeError("pattern mismatch — expected " + constrPat->name, expr.location);
            } else if (std::get_if<ast::ListPattern>(&node.pattern->kind)) {
                if (!matchPattern(*node.pattern, value))
                    throw RuntimeError("pattern mismatch", expr.location);
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            auto value = node.value ? eval(*node.value) : Value::none();
            m_env->define(node.name, value, /*isMutable=*/true);
            return value;
        }
        else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            auto value = node.value ? eval(*node.value) : Value::none();
            if (!m_env->has(node.name)) {
                throw RuntimeError("Undefined variable: " + node.name, expr.location);
            }
            if (!m_env->isMutable(node.name)) {
                throw RuntimeError("Cannot assign to immutable binding: " + node.name, expr.location);
            }
            if (!node.path.empty()) {
                if (node.path.size() != 1)
                    throw RuntimeError(
                        "Nested record-field assignment is not supported",
                        expr.location);
                auto current = m_env->get(node.name);
                auto* record = current
                    ? std::get_if<RecordValue>(&current->data) : nullptr;
                if (!record)
                    throw RuntimeError("Field assignment requires a record binding",
                                       expr.location);
                if (!record->fields.count(node.path.front()))
                    throw RuntimeError("Record " + record->typeName +
                                           " has no field: " + node.path.front(),
                                       expr.location);
                auto fields = record->fields;
                fields[node.path.front()] = value;
                value = Value::record(record->typeName, std::move(fields));
            }
            m_env->set(node.name, value);
            // The assigned value, matching BEAM lowering (which yields the
            // new SSA binding). Returning None made the REPL print
            // "=> None : Optional" for `n = n + 5`.
            return value;
        }
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            // Short-circuit && and || before evaluating rhs.
            if (node.op == TokenType::AmpAmp) {
                auto left = node.left ? eval(*node.left) : Value::none();
                if (!left || !left->isTrue()) return Value::boolean(false);
                auto right = node.right ? eval(*node.right) : Value::none();
                return Value::boolean(right && right->isTrue());
            }
            if (node.op == TokenType::PipePipe) {
                auto left = node.left ? eval(*node.left) : Value::none();
                if (left && left->isTrue()) return Value::boolean(true);
                auto right = node.right ? eval(*node.right) : Value::none();
                return Value::boolean(right && right->isTrue());
            }
            auto left = node.left ? eval(*node.left) : Value::none();
            auto right = node.right ? eval(*node.right) : Value::none();
            return evalBinaryOp(node.op, left, right, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            auto operand = node.operand ? eval(*node.operand) : Value::none();
            return evalUnaryOp(node.op, operand, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            std::vector<ValuePtr> args;
            for (const auto& arg : node.args) {
                args.push_back(arg ? eval(*arg) : Value::none());
            }
            NamedArgs namedArgs;
            for (const auto& [name, val] : node.namedArgs) {
                namedArgs.push_back({name, val ? eval(*val) : Value::none()});
            }
            // Handle block as last arg (lambda)
            if (node.block) {
                args.push_back(eval(**node.block));
            }
            return callFunction(node.name, std::move(args), std::move(namedArgs), expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
            std::vector<ValuePtr> parts;
            parts.reserve(node.parts.size());
            for (const auto& part : node.parts)
                parts.push_back(Value::string(part));
            std::vector<ValuePtr> values;
            values.reserve(node.values.size());
            for (const auto& value : node.values)
                values.push_back(value ? eval(*value) : Value::none());
            return callFunction(
                node.tag,
                {Value::list(std::move(parts)), Value::list(std::move(values))},
                {}, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            // `Type.of(x)` where the checker typed the argument concretely:
            // build the answer from what it recorded. The value cannot know a
            // Result's unused half or an empty list's element type, so this
            // has to win over the runtime fallback — and it must sit ahead of
            // every dispatch path below, since a namespace call returns from
            // one of those.
            if (m_staticTypeOfCalls) {
                auto recorded = m_staticTypeOfCalls->find(&node);
                if (recorded != m_staticTypeOfCalls->end()) {
                    // A value argument still runs (it may have effects); one
                    // that NAMES a function does not — `Date.parse` on its own
                    // is a call missing its argument.
                    if (recorded->second.evaluateArgument)
                        for (const auto& arg : node.args)
                            if (arg) eval(*arg);
                    return structuredTypeValue(recorded->second.type);
                }
            }

            // Distinct conversions are representation-erased: checking has
            // already proved the retag is valid, so the runtime value is
            // unchanged. String is the one non-erased total conversion and
            // uses the same universal display representation as to(String).
            if (node.method == "as" && node.args.size() == 1 &&
                node.args[0] && node.receiver && node.namedArgs.empty() &&
                !node.block) {
                auto value = eval(*node.receiver);
                if (const auto* target =
                        std::get_if<ast::UpperIdentifier>(
                            &node.args[0]->kind);
                    target && target->name == "String")
                    return Value::string(value->toString());
                return value;
            }

            // `Kex.Intrinsic.<Category>.<fn>(args)` — the primitive boundary.
            // `Kex`, `Intrinsic`, `<Category>` are nested modules; dispatch the
            // function to its native C++ builtin (the walker's intrinsics stay
            // in C++). The typed stdlib in the Kex prelude sits on top of these.
            if (node.receiver) {
                if (auto* catMc = std::get_if<ast::MethodCall>(&node.receiver->kind))
                    if (auto* intrMc = std::get_if<ast::MethodCall>(&catMc->receiver->kind))
                        if (intrMc->method == "Intrinsic")
                            if (auto* kexId = std::get_if<ast::UpperIdentifier>(&intrMc->receiver->kind))
                                if (kexId->name == "Kex") {
                                    std::vector<ValuePtr> args;
                                    for (const auto& a : node.args)
                                        args.push_back(a ? eval(*a) : Value::none());
                                    if (node.block) args.push_back(eval(**node.block));
                                    // Look up in m_intrinsicEnv, not m_env —
                                    // the prelude's Kex.Intrinsic.* wrappers
                                    // live in m_globalEnv; the C++ native
                                    // implementations live in m_intrinsicEnv.
                                    // The category-qualified identity is
                                    // authoritative; ordinary/bare intrinsic
                                    // fallback would leak across categories.
                                    auto val = m_intrinsicEnv->get(
                                        catMc->method + "::" + node.method);
                                    if (!val)
                                        throw RuntimeError("Undefined intrinsic: " + catMc->method
                                                           + "." + node.method, expr.location);
                                    if (auto* func = std::get_if<FunctionValue>(&val->data))
                                        return func->native(std::move(args));
                                    throw RuntimeError("Intrinsic " + node.method + " is not a function", expr.location);
                                }
            }
            // Pre-check: if the receiver is a bare UpperIdentifier that isn't
            // in the environment and isn't a known variant, treat it as a
            // namespace call WITHOUT evaluating the receiver — so that an
            // unknown name like `Stream` or `NotANamespace` becomes a namespace
            // dispatch rather than throwing "Undefined identifier" here.
            // This must run before eval(*node.receiver) to avoid the throw.
            std::string namespaceName;
            bool isNamespaceCall = false;
            std::function<std::optional<std::string>(const ast::Expr&)>
                qualifiedPath;
            qualifiedPath = [&](const ast::Expr& receiver)
                -> std::optional<std::string> {
                if (auto* root =
                        std::get_if<ast::UpperIdentifier>(
                            &receiver.kind))
                    return root->name;
                auto* segment =
                    std::get_if<ast::MethodCall>(&receiver.kind);
                if (!segment || !segment->receiver ||
                    !segment->args.empty() ||
                    !segment->namedArgs.empty() || segment->block)
                    return std::nullopt;
                auto parent = qualifiedPath(*segment->receiver);
                return parent
                    ? std::optional<std::string>{
                          *parent + "." + segment->method}
                    : std::nullopt;
            };
            if (node.receiver) {
                if (auto path = qualifiedPath(*node.receiver);
                    path && path->find('.') != std::string::npos) {
                    if (m_moduleRegistry.contains(*path)) {
                        namespaceName = *path;
                        isNamespaceCall = true;
                    } else {
                        module::Resolver resolver(m_moduleRoots);
                        if (resolver.resolve(*path, m_currentModule)) {
                            try {
                                namespaceName = ensureModuleLoaded(
                                    *path, expr.location,
                                    m_currentModule);
                                isNamespaceCall = true;
                            } catch (const RuntimeError& error) {
                                if (std::string(error.what()).find(
                                        "Resolved file does not define module ")
                                    == std::string::npos)
                                    throw;
                            }
                        }
                    }
                }
            }
            if (!isNamespaceCall && node.receiver) {
                if (auto* upperIdent = std::get_if<ast::UpperIdentifier>(&node.receiver->kind)) {
                    bool isKnownVariant = m_variantParent.count(upperIdent->name) > 0;
                    auto existing = m_env->get(upperIdent->name);
                    // A constant declared inside a module is stored under its
                    // qualified lexical name (`Validators::TYPES`). Resolve it
                    // before deciding an uppercase receiver must be a module;
                    // the ordinary UpperIdentifier evaluator already follows
                    // this same enclosing-module search.
                    for (auto scope = m_currentModule;
                         !existing && !scope.empty();) {
                        existing = m_env->get(scope + "::" + upperIdent->name);
                        const auto dot = scope.rfind('.');
                        if (dot == std::string::npos) break;
                        scope.resize(dot);
                    }
                    module::Resolver resolver(m_moduleRoots);
                    const bool sourceModuleExists =
                        resolver.resolve(upperIdent->name,
                                         m_currentModule).has_value();
                    if (existing &&
                        std::holds_alternative<ModuleValue>(
                            existing->data)) {
                        namespaceName =
                            std::get<ModuleValue>(existing->data).name;
                        isNamespaceCall = true;
                    } else if (
                        (m_moduleRegistry.contains(upperIdent->name) ||
                         sourceModuleExists) &&
                        (!existing ||
                         std::holds_alternative<VariantValue>(
                             existing->data))) {
                        namespaceName =
                            m_moduleRegistry.contains(upperIdent->name)
                                ? upperIdent->name
                                : ensureModuleLoaded(
                                      upperIdent->name, expr.location,
                                      m_currentModule);
                        isNamespaceCall = true;
                    } else if (!isKnownVariant &&
                               !existing) {
                        auto resolved = (!m_currentModule.empty())
                            ? m_env->get(m_currentModule + "::" + upperIdent->name) : ValuePtr{};
                        if (resolved && std::holds_alternative<ModuleValue>(resolved->data)) {
                            namespaceName = std::get<ModuleValue>(resolved->data).name;
                        } else if (auto aliased =
                                       resolveNamespaceAlias(upperIdent->name, expr.location)) {
                            namespaceName = *aliased;
                        } else {
                            namespaceName = upperIdent->name;
                        }
                        isNamespaceCall = true;
                    }
                }
            }

            // `Clock.now()` inside `with Clock = FrozenClock { ... }` must
            // reach the replacement. Turning it into an ordinary value
            // receiver sends it through the same UFCS dispatch a method call
            // on that record would take (kexhq/kex#143).
            ValuePtr capabilityOverride;
            if (node.receiver)
                if (auto path = qualifiedPath(*node.receiver))
                    if (auto bound = lookupCapability(*path)) {
                        capabilityOverride = bound;
                        isNamespaceCall = false;
                        namespaceName.clear();
                    }
            auto receiver = capabilityOverride
                ? capabilityOverride
                : ((!isNamespaceCall && node.receiver) ? eval(*node.receiver)
                                                       : Value::none());

            // Serving calls cross the scheduler boundary.  The state is owned
            // by the server fiber; the handle only contributes its default
            // timeout, with `within:` overriding this one call.
            if (auto* server = std::get_if<ServerValue>(&receiver->data)) {
                if (node.method == "within") {
                    if (node.args.size() != 1) throw RuntimeError("Server.within expects one timeout", expr.location);
                    auto timeout = eval(*node.args[0]);
                    auto* milliseconds = std::get_if<IntValue>(&timeout->data);
                    if (!milliseconds) throw RuntimeError("Server.within expects Integer milliseconds", expr.location);
                    return Value::server(server->pid, server->scheduler, milliseconds->value);
                }
                if (node.method == "process")
                    return Value::process(server->pid, server->scheduler);
                if (node.method == "timeout")
                    return Value::integer(server->timeoutMs);
                if (node.method == "link") {
                    server->scheduler->link(server->pid);
                    return Value::unit();
                }
                if (node.method == "unlink") {
                    server->scheduler->unlink(server->pid);
                    return Value::unit();
                }
                // The walker has passive link bookkeeping but no DOWN-signal
                // model yet. Match Process.monitor's current inert fallback
                // without accidentally dispatching a serving slot named
                // `monitor`.
                if (node.method == "monitor") return Value::unit();
                if (node.method == "alive?") return Value::boolean(server->scheduler->isAlive(server->pid));
                std::vector<ValuePtr> slotArgs;
                for (const auto& arg : node.args) slotArgs.push_back(arg ? eval(*arg) : Value::none());
                bool isCast = false;
                if (m_expressionTypes)
                    if (auto found = m_expressionTypes->find(&expr);
                        found != m_expressionTypes->end() && found->second) {
                        const auto type = semantic::typeToString(found->second);
                        isCast = type == "Void" || type == "Unit";
                    }
                if (isCast) {
                    server->scheduler->castServer(server->pid, node.method,
                                                  std::move(slotArgs));
                    return Value::unit();
                }
                std::optional<int64_t> timeout = server->timeoutMs;
                for (const auto& [name, value] : node.namedArgs) if (name == "within") {
                    auto configured = value ? eval(*value) : Value::none();
                    if (auto* ms = std::get_if<IntValue>(&configured->data)) timeout = ms->value;
                    else if (auto* atom = std::get_if<AtomValue>(&configured->data); atom && atom->name == "infinity") timeout.reset();
                }
                auto result = server->scheduler->callServer(server->pid, node.method,
                                                            std::move(slotArgs), timeout);
                return result ? *result : Value::error(Value::atom("timeout"));
            }

            // Namespace access: ModuleValue (registered modules like IO, Math,
            // File, Integer) or empty-record placeholders for user record types
            // used as static-method namespaces (Vector2D, etc.).
            if (!isNamespaceCall) {
                if (auto* mod = std::get_if<ModuleValue>(&receiver->data)) {
                    isNamespaceCall = true;
                    namespaceName = mod->name;
                } else if (auto* rec = std::get_if<RecordValue>(&receiver->data)) {
                    if (rec->fields.empty()) {
                        isNamespaceCall = true;
                        namespaceName = rec->typeName;
                    }
                } else if (auto* var = std::get_if<VariantValue>(&receiver->data)) {
                    // An ADT variant whose tag names a registered module is
                    // the variant clobbering the module's env binding (e.g.
                    // `Http` is both a `Feature` variant and a module) —
                    // `Http.get(...)` must dispatch to the module, not UFCS
                    // on the variant value. Bare-value uses (pattern match,
                    // passing `Http` to a function) are unaffected: they never
                    // reach this method-call path.
                    if (var->args.empty() && m_moduleRegistry.count(var->tag)) {
                        isNamespaceCall = true;
                        namespaceName = var->tag;
                    }
                }
            }
            if (isNamespaceCall) {
                // Namespace call: Stream.Sequence(...), Math.PI, File.read(...), etc.
                std::vector<ValuePtr> args;
                for (const auto& arg : node.args) {
                    args.push_back(arg ? eval(*arg) : Value::none());
                }
                NamedArgs namedArgs;
                for (const auto& [name, val] : node.namedArgs) {
                    namedArgs.push_back({name, val ? eval(*val) : Value::none()});
                }
                if (node.block) {
                    args.push_back(eval(**node.block));
                }
                if (auto registered = m_moduleRegistry.find(namespaceName);
                    registered != m_moduleRegistry.end()
                    && registered->second.privateNames.contains(node.method)
                    && m_currentModule != namespaceName) {
                    throw RuntimeError("cannot access private name `" + node.method
                                       + "` from " + namespaceName, expr.location);
                }
                // Prefer the mangled "Namespace::method" name (e.g. "IO::putLine")
                // so namespaced builtins can't collide with unrelated plain-name
                // globals. Falls back to the plain name for namespaces that were
                // registered without a mangled prefix (e.g. Stream.Sequence).
                std::string dispatchName = node.method;
                auto mangled = namespaceName + "::" + node.method;
                if (namespaceName.find('.') != std::string::npos && !m_env->get(mangled)) {
                    std::string alt;
                    for (char c : namespaceName)
                        alt += (c == '.') ? "::" : std::string(1, c);
                    mangled = alt + "::" + node.method;
                }
                if (auto target = m_env->get(mangled)) {
                    if (node.args.empty() && node.namedArgs.empty() &&
                        !node.block &&
                        !std::holds_alternative<FunctionValue>(target->data))
                        return target;
                    if (node.args.empty() && node.namedArgs.empty() && !node.block
                        && std::holds_alternative<ModuleValue>(target->data))
                        return target;
                    if (std::holds_alternative<RecordValue>(target->data)) {
                        const auto& recName = std::get<RecordValue>(target->data).typeName;
                        std::unordered_map<std::string, ValuePtr> fields;
                        if (node.block) {
                            auto blockVal = eval(**node.block);
                            if (auto* mv = std::get_if<MapValue>(&blockVal->data))
                                for (const auto& [k, v] : mv->entries) {
                                    if (auto* sv = std::get_if<StringValue>(&k->data))
                                        fields[sv->value] = v;
                                    else if (auto* av = std::get_if<AtomValue>(&k->data))
                                        fields[av->name] = v;
                                }
                        }
                        for (const auto& [name, val] : node.namedArgs)
                            fields[name] = val ? eval(*val) : Value::none();
                        auto defIt = m_recordDefs.find(mangled);
                        if (defIt == m_recordDefs.end()) defIt = m_recordDefs.find(recName);
                        if (defIt != m_recordDefs.end()) {
                            for (const auto& field : defIt->second->fields) {
                                if (fields.count(field.name)) continue;
                                if (field.defaultValue && *field.defaultValue)
                                    fields[field.name] = eval(**field.defaultValue);
                            }
                        }
                        return Value::record(recName, std::move(fields));
                    }
                    dispatchName = mangled;
                }
                // An unknown namespace must not fall back to a global of the
                // same method name: `Maths.sqrt(4.0)` quietly became
                // `sqrt(4.0)` here and returned 2.0, while BEAM rejected it —
                // a typo that ran fine in one backend and broke in the other.
                if (dispatchName == node.method) {
                    const bool knownNamespace =
                        m_moduleRegistry.count(namespaceName) > 0 ||
                        m_recordDefs.count(namespaceName) > 0 ||
                        m_variantParent.count(namespaceName) > 0 ||
                        knownModuleValue(namespaceName);
                    if (!knownNamespace)
                        throw RuntimeError(
                            "Undefined function: " + namespaceName + "." + node.method,
                            expr.location);
                }
                // Report WHERE the name was looked for: `Missing.thing`, not
                // a bare `thing`, so a mistyped namespace says so. BEAM words
                // it the same way.
                try {
                    return callFunction(dispatchName, std::move(args),
                                        std::move(namedArgs), expr.location);
                } catch (const RuntimeError& error) {
                    const std::string undefined =
                        "Undefined function: " + dispatchName;
                    if (std::string(error.what()).find(undefined) ==
                        std::string::npos)
                        throw;
                    throw RuntimeError(
                        "Undefined function: " + namespaceName + "." + node.method,
                        expr.location);
                }
            }

            // Field access on records: receiver.field (no args, no parens)
            if (node.args.empty() && !node.block && !node.mutating) {
                if (auto* rec = std::get_if<RecordValue>(&receiver->data)) {
                    auto it = rec->fields.find(node.method);
                    if (it != rec->fields.end()) {
                        return it->second;
                    }
                }
            }

            std::vector<ValuePtr> args;
            args.push_back(receiver); // UFCS: receiver is first arg
            for (const auto& arg : node.args) {
                args.push_back(arg ? eval(*arg) : Value::none());
            }
            if (node.block) {
                args.push_back(eval(**node.block));
            }

            NamedArgs namedArgs;
            for (const auto& [name, val] : node.namedArgs) {
                namedArgs.push_back({name, val ? eval(*val) : Value::none()});
            }

            if (const auto* identifier =
                    std::get_if<ast::Identifier>(&node.receiver->kind)) {
                auto dictionary = m_env->get(
                    "__trait_dictionary__" + identifier->name + "::" +
                    node.method);
                if (dictionary) {
                    auto* function =
                        std::get_if<FunctionValue>(&dictionary->data);
                    if (function && function->native)
                        return function->native(std::move(args));
                }
            }

            // UFCS and free-call syntax share imported overloads. Named
            // overloads need to be considered before the receiver's generic
            // method so labels such as `in:` select the imported clause.
            if (auto imported = findImportedNamedOverload(
                    node.method, namedArgs, receiver))
                return callFunction(*imported, std::move(args),
                                    std::move(namedArgs), expr.location);
            auto specificMethod = resolveMethodName(receiver, node.method, &args);
            // Erased distinct values have the backing value's runtime name,
            // but their receiver methods were selected using the static
            // nominal type. Prefer that method when semantic analysis kept a
            // concrete named receiver type for this exact expression.
            if (m_expressionTypes) {
                if (auto found = m_expressionTypes->find(node.receiver.get());
                    found != m_expressionTypes->end() && found->second) {
                    if (const auto* named = std::get_if<semantic::NamedType>(
                            &found->second->kind)) {
                        const auto candidate = named->name + "::" + node.method;
                        const auto runtimeType = dispatchTypeName(receiver);
                        const auto runtimeDot = runtimeType.rfind('.');
                        const bool resolvedShortRecordMethod =
                            runtimeDot != std::string::npos &&
                            specificMethod == runtimeType.substr(runtimeDot + 1) +
                                "::" + node.method;
                        if (!resolvedShortRecordMethod &&
                            (m_functionValues.count(candidate) ||
                             m_env->get(candidate)))
                            specificMethod = candidate;
                    }
                }
            }

            auto mangledName = std::move(specificMethod);

            // For mutating calls, reassign back
            if (node.mutating) {
                auto* ident = std::get_if<ast::Identifier>(&node.receiver->kind);
                if (!ident) {
                    throw RuntimeError("'!' requires a variable binding as the receiver", expr.location);
                }
                if (!m_env->has(ident->name)) {
                    throw RuntimeError("Undefined variable: " + ident->name, expr.location);
                }
                if (!m_env->isMutable(ident->name)) {
                    throw RuntimeError("Cannot use '!' on immutable binding: " + ident->name, expr.location);
                }
                auto result = callFunction(mangledName, args, namedArgs, expr.location);
                m_env->set(ident->name, result);
                return result;
            }

            // A method call that resolves to nothing should say so as a
            // METHOD — `d.iso` where `d` is a Result reads as "no `iso` for a
            // Result", not as a missing global. The BEAM dispatcher's
            // fallback clause words it identically.
            // A record field may hold a function, and `d.fetch(url)` should
            // call it. The method keeps precedence — this is only consulted
            // when nothing answered the name — which matches the field/method
            // rule spec/record_field_method_collision.kex fixes for READS
            // (kexhq/kex#176). `args` is moved into callFunction below, so
            // copy it first, and only when such a field actually exists.
            const FunctionValue* callableField = nullptr;
            if (receiver)
                if (auto* record = std::get_if<RecordValue>(&receiver->data)) {
                    auto field = record->fields.find(node.method);
                    if (field != record->fields.end() && field->second)
                        callableField =
                            std::get_if<FunctionValue>(&field->second->data);
                }
            // UFCS put the receiver at args[0]; a field's function is a
            // plain value, not a method, so it takes only the explicit
            // arguments.
            std::vector<ValuePtr> fieldArgs;
            if (callableField && !args.empty())
                fieldArgs.assign(args.begin() + 1, args.end());

            try {
                return callFunction(mangledName, std::move(args),
                                    std::move(namedArgs), expr.location);
            } catch (const RuntimeError& error) {
                const std::string undefined = "Undefined function: " + mangledName;
                if (std::string(error.what()).find(undefined) == std::string::npos)
                    throw;
                // Say WHERE the name was looked for. A namespace call names
                // the namespace (`Missing.thing`, which is what BEAM
                // reports); a value receiver names its type instead.
                if (isNamespaceCall) {
                    std::string qualified;
                    if (node.receiver)
                        if (auto* uid = std::get_if<ast::UpperIdentifier>(
                                &node.receiver->kind))
                            qualified = uid->name + "." + node.method;
                    if (qualified.empty()) throw;
                    throw RuntimeError("Undefined function: " + qualified,
                                       expr.location);
                }
                if (callableField && callableField->native)
                    return callableField->native(std::move(fieldArgs));
                if (!receiver) throw;
                throw RuntimeError("Undefined method: " + mangledName + " for " +
                                       receiver->typeName(),
                                   expr.location);
            }
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            std::vector<ValuePtr> elements;
            for (const auto& elem : node.elements) {
                if (elem) {
                    if (auto* spread = std::get_if<ast::SpreadExpr>(&elem->kind)) {
                        auto val = eval(*spread->inner);
                        if (auto* list = std::get_if<ListValue>(&val->data)) {
                            elements.insert(elements.end(), list->elements.begin(), list->elements.end());
                        } else {
                            elements.push_back(val);
                        }
                    } else {
                        elements.push_back(eval(*elem));
                    }
                } else {
                    elements.push_back(Value::none());
                }
            }
            if (node.rest) {
                auto tailVal = eval(**node.rest);
                if (auto* tailList = std::get_if<ListValue>(&tailVal->data)) {
                    elements.insert(elements.end(),
                        tailList->elements.begin(), tailList->elements.end());
                } else {
                    elements.push_back(tailVal);
                }
            }
            return Value::list(std::move(elements));
        }
        else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            std::vector<ValuePtr> elements;
            for (const auto& elem : node.elements) {
                elements.push_back(elem ? eval(*elem) : Value::none());
            }
            return Value::tuple(std::move(elements));
        }
        else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            auto map = std::make_shared<Value>();
            std::vector<std::pair<ValuePtr, ValuePtr>> entries;
            // Later entries win, so a key written after a spread overrides the
            // one it brought in — and a spread overrides what came before it.
            auto put = [&entries](const ValuePtr& key, const ValuePtr& val) {
                for (auto& [existing, slot] : entries)
                    if (valuesEqual(existing, key)) { slot = val; return; }
                entries.push_back({key, val});
            };
            for (const auto& entry : node.entries) {
                if (entry.spread) {
                    auto source = entry.value ? eval(*entry.value) : Value::none();
                    if (auto* other = std::get_if<MapValue>(&source->data)) {
                        for (const auto& [key, val] : other->entries) put(key, val);
                    } else {
                        throw RuntimeError(
                            "Cannot spread " + source->typeName() + " into a map",
                            entry.value ? entry.value->location : expr.location);
                    }
                    continue;
                }
                auto key = entry.key ? eval(*entry.key) : Value::none();
                auto val = entry.value ? eval(*entry.value) : Value::none();
                put(key, val);
            }
            map->data = MapValue{std::move(entries)};
            return map;
        }
        else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            auto start = node.start ? eval(*node.start) : Value::integer(0);
            auto end = node.end ? eval(*node.end) : Value::integer(0);
            auto* s = std::get_if<IntValue>(&start->data);
            auto* e = std::get_if<IntValue>(&end->data);
            if (s && e) {
                auto range = std::make_shared<Value>();
                range->data = RangeValue{s->value, e->value, false};
                return range;
            }
            auto* sc = std::get_if<CharValue>(&start->data);
            auto* ec = std::get_if<CharValue>(&end->data);
            if (sc && ec) {
                auto range = std::make_shared<Value>();
                range->data = RangeValue{static_cast<int64_t>(sc->value),
                                         static_cast<int64_t>(ec->value), true};
                return range;
            }
            auto range = std::make_shared<Value>();
            range->data = RangeValue{0, 0, false};
            return range;
        }
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            if (node.letPattern) {
                // `if let Pattern = expr` — match the scrutinee against the
                // pattern; run thenBody with bindings in scope if it matches.
                auto scrutinee = node.condition ? eval(*node.condition) : Value::none();
                pushEnv();
                bool matched = matchPattern(*node.letPattern, scrutinee);
                ValuePtr result = Value::none();
                if (matched) {
                    result = evalBody(node.thenBody);
                } else if (node.elseBody) {
                    result = evalBody(*node.elseBody);
                }
                popEnv();
                return result;
            }
            auto cond = node.condition ? eval(*node.condition) : Value::boolean(false);
            if (cond->isTrue()) {
                return evalBody(node.thenBody);
            }
            for (const auto& [elifCond, elifBody] : node.elifs) {
                auto ec = elifCond ? eval(*elifCond) : Value::boolean(false);
                if (ec->isTrue()) {
                    return evalBody(elifBody);
                }
            }
            if (node.elseBody) {
                return evalBody(*node.elseBody);
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            auto subject = node.subject ? eval(*node.subject) : Value::none();
            for (const auto& clause : node.clauses) {
                pushEnv();
                if (node.subjectBinding) {
                    m_env->define(*node.subjectBinding, subject);
                }
                // Everything below must pop this scope before returning OR
                // propagating an exception. Without the try/catch, a clause
                // body containing `return` (extremely common — e.g. `_ ->
                // return p`) would throw past the popEnv() below, leaking
                // this scope permanently: m_env would stay one level too
                // deep for the rest of the enclosing function call, and
                // anything that function defined locally (e.g. `var p =
                // this` in a helper called from a caller's loop) would
                // become invisible/shadowed to the caller afterward —
                // silently corrupting unrelated variables with the same
                // name in the caller, or causing infinite loops.
                try {
                    bool matched = false;
                    for (const auto& pat : clause.patterns) {
                        if (matchPattern(*pat, subject)) {
                            matched = true;
                            break;
                        }
                    }
                    if (matched) {
                        if (clause.guard && *clause.guard) {
                            auto guardVal = eval(**clause.guard);
                            if (!guardVal->isTrue()) {
                                popEnv();
                                continue;
                            }
                        }
                        auto result = clause.body ? eval(*clause.body) : Value::none();
                        popEnv();
                        return result;
                    }
                } catch (...) {
                    popEnv();
                    throw;
                }
                popEnv();
            }
            // No clause matched: BEAM raises `{case_clause, subject}` here,
            // so the walker must raise too — a match with no matching
            // clause is a bug, not a value (it previously returned None).
            throw RuntimeError("no matching clause for " + subject->inspect(),
                               expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            // `return EXPR if COND` parses as ReturnExpr(TrailingIf(EXPR,
            // COND)) — i.e. the `if` must gate whether the return happens at
            // all, not just what value it carries. Without this special
            // case, ReturnExpr unconditionally throws even when COND is
            // false (just throwing None), short-circuiting the function
            // unconditionally — breaking idioms like
            // `return Error(...) if invalid?` used throughout Result-style
            // error handling.
            if (node.value) {
                if (auto* trailing = std::get_if<ast::TrailingIf>(&node.value->kind)) {
                    auto cond = trailing->condition ? eval(*trailing->condition) : Value::boolean(false);
                    if (!cond->isTrue()) {
                        return Value::none();
                    }
                    auto value = trailing->expr ? eval(*trailing->expr) : Value::none();
                    throw ReturnException(value);
                }
            }
            auto value = node.value ? eval(*node.value) : Value::none();
            throw ReturnException(value);
        }
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            auto lambda = std::make_shared<Value>();
            auto capturedEnv = m_env;
            const auto* bodyPtr = &node.body;
            std::vector<std::string> paramNames;
            for (const auto& p : node.params) {
                paramNames.push_back(p.name);
            }

            const ast::RescueBlock* rescuePtr = node.rescue ? &*node.rescue : nullptr;
            lambda->data = FunctionValue{"<lambda>",
                [this, bodyPtr, paramNames, capturedEnv, rescuePtr](std::vector<ValuePtr> args) -> ValuePtr {
                    auto prevEnv = m_env;
                    m_env = std::make_shared<Environment>(capturedEnv);
                    // If the lambda expects multiple params but receives a single
                    // tuple, auto-spread it so `list.each do |a, b|` works on
                    // a list of pairs without breaking `each do |pair|`.
                    if (paramNames.size() > 1 && args.size() == 1) {
                        if (auto* tv = std::get_if<TupleValue>(&args[0]->data)) {
                            if (tv->elements.size() == paramNames.size()) {
                                args = tv->elements;
                            }
                        }
                    }
                    for (size_t i = 0; i < paramNames.size() && i < args.size(); i++) {
                        m_env->define(paramNames[i], args[i]);
                    }
                    ValuePtr result;
                    // catch(...) so a RuntimeError propagating through this
                    // lambda (e.g. a failed `assert` caught higher up by
                    // `it`) still restores m_env before unwinding further —
                    // same reasoning as the MatchExpr/function-clause guards.
                    try {
                        result = evalBody(*bodyPtr);
                    } catch (TryException& e) {
                        if (rescuePtr) {
                            try {
                                result = evalRescue(*rescuePtr, e.error(), {});
                            } catch (ReturnException& ret) {
                                result = ret.value();
                            }
                        } else {
                            // A lambda IS a function, so the same rule applies
                            // as at a named function's boundary: an unrescued
                            // `.try` failure makes THIS call's result
                            // `Error(e)` rather than unwinding past it.
                            // Rethrowing let the error escape the HOF that
                            // called the block — `["x"].map { |v| f(v).try }`
                            // produced a bare `Error(...)` instead of a list
                            // containing one, and blew past `each` entirely.
                            m_env = prevEnv;
                            auto errorVal = std::make_shared<Value>();
                            errorVal->data =
                                VariantValue{"Error", "Result", {e.error()}, {}, {}};
                            return errorVal;
                        }
                    } catch (ReturnException& ret) {
                        result = ret.value();
                    } catch (...) {
                        m_env = prevEnv;
                        throw;
                    }
                    m_env = prevEnv;
                    return result;
                }, static_cast<int>(paramNames.size())};
            return lambda;
        }
        else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            auto cond = node.condition ? eval(*node.condition) : Value::boolean(false);
            if (cond->isTrue()) {
                return node.expr ? eval(*node.expr) : Value::none();
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            auto cond = node.condition ? eval(*node.condition) : Value::boolean(false);
            if (cond->isTrue()) {
                return node.thenExpr ? eval(*node.thenExpr) : Value::none();
            }
            return node.elseExpr ? eval(*node.elseExpr) : Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            std::string typeName;
            ValuePtr receiver;
            if (node.typeName == "This" || node.typeName == "New") {
                receiver = m_env->get("this");
                auto* record = receiver
                    ? std::get_if<RecordValue>(&receiver->data) : nullptr;
                if (!record)
                    throw RuntimeError("`" + node.typeName +
                                           "` requires a record receiver",
                                       expr.location);
                typeName = record->typeName;
            } else {
                typeName = resolveRecordTypeName(node.typeName);
            }
            std::unordered_map<std::string, ValuePtr> fields;
            if (node.typeName == "New")
                fields = std::get<RecordValue>(receiver->data).fields;
            for (const auto& entry : node.fields) {
                auto value = entry.value ? eval(*entry.value) : Value::none();
                if (entry.spread) {
                    auto* spread = std::get_if<RecordValue>(&value->data);
                    if (!spread || spread->typeName != typeName)
                        throw RuntimeError("Cannot spread " + value->typeName() +
                                               " into record " + typeName,
                                           expr.location);
                    for (const auto& [name, field] : spread->fields)
                        fields[name] = field;
                } else {
                    fields[entry.name] = value;
                }
            }
            // A qualified type from an opt-in module may not be loaded yet:
            // the declaration alone is enough to name it, and that came from
            // the interface. Its field DEFAULTS and its `make` block live in
            // the source, so without loading, `Parsing.Input { input: "ab" }`
            // came out missing `pos` and answered no methods at all — unless
            // the file happened to say `using Parsing` (kexhq/kex#143).
            if (auto dot = typeName.rfind('.');
                dot != std::string::npos && !m_recordDefs.count(typeName)) {
                const auto owner = typeName.substr(0, dot);
                if (!m_moduleRegistry.contains(owner) &&
                    !m_loadingModules.count(owner))
                    try {
                        ensureModuleLoaded(owner, expr.location, "");
                    } catch (const RuntimeError&) {
                        // Not a module that can be found — leave the
                        // construction to report whatever is actually wrong.
                    }
            }
            // Apply declared field defaults (e.g. `pos : Int = 0`) for any
            // field this construction didn't specify explicitly.
            auto defIt = m_recordDefs.find(typeName);
            if (defIt != m_recordDefs.end()) {
                for (const auto& field : defIt->second->fields) {
                    if (fields.count(field.name)) continue;
                    if (field.defaultValue && *field.defaultValue) {
                        fields[field.name] = eval(**field.defaultValue);
                    } else {
                        fields[field.name] = Value::none();
                    }
                }
            }
            return Value::record(typeName, std::move(fields));
        }
        else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
            if (node.kind == ast::ShorthandLambda::Kind::Method) {
                // &.method — create a lambda that calls method on its arg
                auto method = node.name;
                auto lambda = std::make_shared<Value>();
                lambda->data = FunctionValue{"&." + method,
                    [this, method](std::vector<ValuePtr> args) -> ValuePtr {
                        if (args.empty()) return Value::none();
                        // Field access wins over UFCS dispatch, the same order
                        // `receiver.field` uses — there is no callable
                        // `field/1` for a plain record field to dispatch to.
                        if (auto* rec = std::get_if<RecordValue>(&args[0]->data)) {
                            auto field = rec->fields.find(method);
                            if (field != rec->fields.end()) return field->second;
                        }
                        auto dispatchName = resolveMethodName(args[0], method);
                        return callFunction(dispatchName, std::move(args), {}, {});
                    }};
                return lambda;
            }
            if (node.kind == ast::ShorthandLambda::Kind::MethodWithArgs) {
                // &.method(args) — create a lambda that calls method with extra args
                auto method = node.name;
                std::vector<ValuePtr> extraArgs;
                for (const auto& arg : node.args) {
                    extraArgs.push_back(arg ? eval(*arg) : Value::none());
                }
                auto lambda = std::make_shared<Value>();
                lambda->data = FunctionValue{"&." + method,
                    [this, method, extraArgs](std::vector<ValuePtr> args) -> ValuePtr {
                        if (args.empty()) return Value::none();
                        auto dispatchName = resolveMethodName(args[0], method);
                        auto allArgs = args;
                        for (const auto& a : extraArgs) allArgs.push_back(a);
                        return callFunction(dispatchName, std::move(allArgs), {}, {});
                    }};
                return lambda;
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            return evalBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::CurryPlaceholder>) {
            throw RuntimeError("CurryPlaceholder evaluated outside curry context", expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
            struct Slot { bool isOpen; ValuePtr value; };
            std::vector<Slot> slots;
            for (const auto& group : node.argGroups)
                for (const auto& argExpr : group)
                    if (std::holds_alternative<ast::CurryPlaceholder>(argExpr->kind))
                        slots.push_back({true, nullptr});
                    else
                        slots.push_back({false, eval(*argExpr)});

            auto fnName = node.name;
            // `~Mod.fn` — resolve to the mangled "Mod::fn" binding that
            // namespace calls use, so callFunction dispatches to the module's
            // function rather than a same-named global.
            std::string defKey = fnName;
            if (!node.module.empty()) {
                auto ns = node.module;
                if (!m_moduleRegistry.contains(ns)) {
                    module::Resolver resolver(m_moduleRoots);
                    if (resolver.resolve(ns, m_currentModule))
                        ns = ensureModuleLoaded(ns, expr.location, m_currentModule);
                }
                if (auto registered = m_moduleRegistry.find(ns);
                    registered != m_moduleRegistry.end()
                    && registered->second.privateNames.contains(node.name)
                    && m_currentModule != ns) {
                    throw RuntimeError("cannot access private name `" + node.name
                                       + "` from " + ns, expr.location);
                }
                const auto mangled = ns + "::" + node.name;
                if (!m_env->get(mangled))
                    throw RuntimeError("`" + node.module + "." + node.name
                                       + "` is not a function", expr.location);
                fnName = mangled;
                defKey = mangled; // defs register under the same mangled name
            }

            // Determine arity: `!` is unary, other operators binary; user
            // functions from their defs.
            const bool isUnaryOp = node.isOperator && fnName == "!";
            int arity = -1;
            if (node.isOperator) {
                arity = isUnaryOp ? 1 : 2;
            } else {
                auto it = m_functionDefs.find(defKey);
                if (it != m_functionDefs.end() && !it->second.empty()) {
                    const auto& firstDef = *it->second[0];
                    if (!firstDef.clauses.empty())
                        arity = static_cast<int>(firstDef.clauses[0].params.size());
                }
            }

            int boundCount = static_cast<int>(slots.size());
            int openCount = 0;
            for (const auto& s : slots) if (s.isOpen) openCount++;

            // Fully applied: an argument group was actually written, no open
            // slots, and we have at least arity args. `~f` with no group is a
            // plain capture and must never be applied here — note that a
            // method's recorded arity can be 0 (the receiver is implicit
            // rather than a declared param), so arity alone can't decide this.
            bool fullyApplied = !node.argGroups.empty() && (openCount == 0) &&
                                (arity >= 0 ? boundCount >= arity : boundCount > 0);

            if (fullyApplied) {
                std::vector<ValuePtr> args;
                for (const auto& s : slots) args.push_back(s.value);
                if (isUnaryOp && args.size() >= 1)
                    return evalUnaryOp(TokenType::Bang, args[0], expr.location);
                if (node.isOperator && args.size() >= 2) {
                    static const std::unordered_map<std::string, TokenType> opToks = {
                        {"+", TokenType::Plus}, {"-", TokenType::Minus},
                        {"*", TokenType::Star}, {"/", TokenType::Slash},
                        {"%", TokenType::Percent}, {"^", TokenType::Caret}, {"==", TokenType::EqEq},
                        {"!=", TokenType::NotEq}, {"<", TokenType::LessThan},
                        {"<=", TokenType::LessEq}, {">", TokenType::GreaterThan},
                        {">=", TokenType::GreaterEq},
                        {"&&", TokenType::AmpAmp}, {"||", TokenType::PipePipe},
                    };
                    auto it2 = opToks.find(fnName);
                    if (it2 != opToks.end())
                        return evalBinaryOp(it2->second, args[0], args[1], expr.location);
                }
                return callFunction(fnName, std::move(args), {}, {});
            }

            // Map operator name to TokenType for evalBinaryOp dispatch.
            static const std::unordered_map<std::string, TokenType> opTokens = {
                {"+", TokenType::Plus}, {"-", TokenType::Minus},
                {"*", TokenType::Star}, {"/", TokenType::Slash},
                {"%", TokenType::Percent}, {"^", TokenType::Caret}, {"==", TokenType::EqEq},
                {"!=", TokenType::NotEq}, {"<", TokenType::LessThan},
                {"<=", TokenType::LessEq}, {">", TokenType::GreaterThan},
                {">=", TokenType::GreaterEq},
                {"&&", TokenType::AmpAmp}, {"||", TokenType::PipePipe},
            };
            auto opIt = opTokens.find(fnName);
            bool isOp = node.isOperator && opIt != opTokens.end();
            TokenType opToken = isOp ? opIt->second : TokenType::Plus;

            // Partial: return a lambda that fills open slots (or appends) then calls.
            auto lambda = std::make_shared<Value>();
            lambda->data = FunctionValue{"~" + fnName,
                [this, fnName, slots, isOp, isUnaryOp, opToken](std::vector<ValuePtr> fillArgs) mutable -> ValuePtr {
                    std::vector<ValuePtr> finalArgs;
                    size_t fillIdx = 0;
                    for (const auto& s : slots) {
                        if (s.isOpen && fillIdx < fillArgs.size())
                            finalArgs.push_back(fillArgs[fillIdx++]);
                        else if (!s.isOpen)
                            finalArgs.push_back(s.value);
                    }
                    while (fillIdx < fillArgs.size())
                        finalArgs.push_back(fillArgs[fillIdx++]);
                    if (isUnaryOp && finalArgs.size() >= 1)
                        return evalUnaryOp(TokenType::Bang, finalArgs[0], {});
                    if (isOp && finalArgs.size() >= 2)
                        return evalBinaryOp(opToken, finalArgs[0], finalArgs[1], {});
                    return callFunction(fnName, std::move(finalArgs), {}, {});
                }};
            return lambda;
        }
        else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            // `loop\n...end` runs forever — the only ways out are `break`
            // (BreakException), `return` (ReturnException, which unwinds to
            // the enclosing function's call site and is caught there), or
            // an uncaught error. Each iteration gets its own scope so
            // `var`s declared inside the loop body don't leak across
            // iterations (mirrors how other block bodies push/pop).
            // `loop do |i|` binds a 0-based iteration counter, fresh in each
            // iteration's scope (so the body can't leak a mutation into the
            // next round).
            long long iter = 0;
            while (true) {
                pushEnv();
                if (node.counter && *node.counter != "_")
                    m_env->define(*node.counter, Value::integer(iter));
                iter++;
                try {
                    evalBody(node.body);
                } catch (const BreakException&) {
                    popEnv();
                    break;
                } catch (const NextException&) {
                    popEnv();
                    continue;
                } catch (...) {
                    popEnv();
                    throw;
                }
                popEnv();
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            while (true) {
                auto cond = node.condition ? eval(*node.condition) : Value::boolean(false);
                if (!cond->isTrue()) break;
                pushEnv();
                try {
                    evalBody(node.body);
                } catch (const BreakException&) {
                    popEnv();
                    break;
                } catch (const NextException&) {
                    popEnv();
                    continue;
                } catch (...) {
                    popEnv();
                    throw;
                }
                popEnv();
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::BreakExpr>) {
            throw BreakException{};
        }
        else if constexpr (std::is_same_v<T, ast::NextExpr>) {
            throw NextException{};
        }
        else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
            // Look up in environment first (variants, modules, record namespaces,
            // ALL_CAPS constants like `let MAX_RETRIES = 3`).
            // All valid capitalized names (declared variants, stdlib modules,
            // user record types) are registered in the environment at
            // declaration time. An unknown name here is a real error.
            auto val = m_env->get(node.name);
            for (auto scope = m_currentModule; !val && !scope.empty();) {
                val = m_env->get(scope + "::" + node.name);
                const auto dot = scope.rfind('.');
                if (dot == std::string::npos) break;
                scope.resize(dot);
            }
            if (val) return autoCallZeroArgConstant(node.name, val);
            throw RuntimeError("Undefined identifier: " + node.name, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::TryExpr>) {
            auto value = eval(*node.operand);
            if (auto* var = std::get_if<VariantValue>(&value->data)) {
                if (var->tag == "Ok" && !var->args.empty()) {
                    return var->args[0];
                }
                if (var->tag == "Error" && !var->args.empty()) {
                    throw TryException(var->args[0]);
                }
                if (var->tag == "Just" && !var->args.empty()) {
                    return var->args[0];
                }
                if (var->tag == "None" || (var->tag == "Error" && var->args.empty())) {
                    throw TryException(Value::none());
                }
            }
            // Some native Optional-returning APIs erase Just on success and
            // return the payload directly, while retaining None on failure.
            // Semantic checking still rejects `.try` on a concrete non-Tryable
            // type; this identity fallback preserves that native ABI.
            return value;
        }
        else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
            try {
                return evalBody(node.body);
            } catch (TryException& e) {
                return evalRescue(node.rescue, e.error(), expr.location);
            }
        }
        else if constexpr (std::is_same_v<T, ast::ErrorNode>) {
            throw RuntimeError("Attempted to evaluate a parse error node: " + node.message,
                               expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            auto pid = m_scheduler->spawn(node.body, m_env);
            return Value::process(pid, m_scheduler.get());
        }
        else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            return m_scheduler->blockingReceive(node);
        }
        else if constexpr (std::is_same_v<T, ast::WithExpr>) {
            std::string capabilityName;
            for (size_t i = 0; i < node.capability.parts.size(); ++i) {
                if (i) capabilityName += ".";
                capabilityName += node.capability.parts[i];
            }
            auto replacement = node.value ? eval(*node.value) : Value::none();
            m_capabilityBindings.emplace_back(capabilityName, replacement);
            // Pop on the way out however the body leaves — a `return`, a
            // `break`, or a raised error must not leak the substitution into
            // the code that follows.
            struct Pop {
                std::vector<std::pair<std::string, ValuePtr>>& stack;
                ~Pop() { stack.pop_back(); }
            } pop{m_capabilityBindings};
            auto result = Value::none();
            for (const auto& item : node.body)
                if (item) result = eval(*item);
            return result;
        }
        else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
            std::string moduleName;
            for (size_t i = 0; i < node.module.parts.size(); ++i) {
                if (i) moduleName += ".";
                moduleName += node.module.parts[i];
            }
            auto it = m_moduleRegistry.find(moduleName);
            if (it == m_moduleRegistry.end()) {
                moduleName = ensureModuleLoaded(moduleName, expr.location);
                it = m_moduleRegistry.find(moduleName);
                if (it == m_moduleRegistry.end())
                    throw RuntimeError("Unknown module: " + moduleName, expr.location);
            }
            const bool scoped = !node.body.empty();
            if (scoped) pushEnv();
            try {
                for (const auto& requested : node.onlyNames)
                    if (it->second.privateNames.contains(requested))
                        throw RuntimeError("cannot import private name `" + requested
                                           + "` from " + moduleName, expr.location);
                for (const auto& requested : node.exceptNames)
                    if (it->second.privateNames.contains(requested))
                        throw RuntimeError("cannot reference private name `" + requested
                                           + "` from " + moduleName, expr.location);
                for (const auto& [name, value] : it->second.exports) {
                    if (!node.onlyNames.empty() &&
                        std::find(node.onlyNames.begin(), node.onlyNames.end(), name) == node.onlyNames.end()) continue;
                    if (std::find(node.exceptNames.begin(), node.exceptNames.end(), name) != node.exceptNames.end()) continue;
                    defineImported(name, name, moduleName, !node.onlyNames.empty(), "",
                                   value, expr.location);
                }
                if (m_usingModules.empty()) m_usingModules.emplace_back();
                m_usingModules.back().insert(moduleName);
                if (node.alias) m_env->define(*node.alias, Value::module(moduleName));
                for (const auto& e : node.body) {
                    if (e) eval(*e);
                }
            } catch (...) {
                if (scoped) popEnv();
                throw;
            }
            if (scoped) popEnv();
            return Value::unit();
        }
        else {
            return Value::none();
        }
    }, expr.kind);
}

auto Evaluator::evalBinaryOp(TokenType op, const ValuePtr& left, const ValuePtr& right,
                             SourceLocation loc) -> ValuePtr {
    // A placeholder can be CARRIED by compile-time evaluation but never
    // computed with — `x + 1` has no answer until the program runs. Checked
    // before operator overloading, which would otherwise dispatch on it.
    for (const auto* side : {&left, &right})
        if (const auto* ph = std::get_if<PlaceholderValue>(&(*side)->data))
            throw PlaceholderMisuse(ph->name, "this operator");

    // Operator overloading: `make Type do let +(other) -> Type ... end`
    // registers "Type::+", dispatched here through the same receiver-type
    // resolution as method calls (including ADT variants), before built-ins.
    std::string opSymbol;
    switch (op) {
        case TokenType::Plus:       opSymbol = "+";  break;
        case TokenType::Minus:      opSymbol = "-";  break;
        case TokenType::Star:       opSymbol = "*";  break;
        case TokenType::Slash:      opSymbol = "/";  break;
        case TokenType::Percent:    opSymbol = "%";  break;
        case TokenType::Caret:      opSymbol = "^";  break;
        case TokenType::EqEq:       opSymbol = "=="; break;
        case TokenType::NotEq:      opSymbol = "!="; break;
        case TokenType::LessThan:   opSymbol = "<";  break;
        case TokenType::GreaterThan: opSymbol = ">"; break;
        case TokenType::LessEq:     opSymbol = "<="; break;
        case TokenType::GreaterEq:  opSymbol = ">="; break;
        default: break;
    }
    if (!opSymbol.empty()) {
        std::vector<ValuePtr> operatorArgs{left, right};
        auto methodName = resolveMethodName(left, opSymbol, &operatorArgs);
        fprintf(stderr, "[DEBUG-OP] opSymbol=%s methodName=%s left=%s right=%s\n",
                opSymbol.c_str(), methodName.c_str(), left->typeName().c_str(), right->typeName().c_str());
        if (methodName != opSymbol) {
            auto r = callFunction(methodName, {left, right}, {}, loc);
            fprintf(stderr, "[DEBUG-OP] result=%s\n", r->typeName().c_str());
            return r;
        }
    }

    // li/ri stay the int64_t fast path for the overwhelmingly common case;
    // leftInt/rightInt (set for either IntValue or BigIntValue) are the
    // arbitrary-precision fallback — overflow promotes into it, and once
    // a value is already a BigIntValue every further op goes through it.
    auto* li = std::get_if<IntValue>(&left->data);
    auto* ri = std::get_if<IntValue>(&right->data);
    auto leftInt = asInteger(left);
    auto rightInt = asInteger(right);
    auto* lf = std::get_if<FloatValue>(&left->data);
    auto* rf = std::get_if<FloatValue>(&right->data);
    auto* ls = std::get_if<StringValue>(&left->data);
    auto* rs = std::get_if<StringValue>(&right->data);
    auto* lc = std::get_if<CharValue>(&left->data);
    auto* rc = std::get_if<CharValue>(&right->data);
    auto* lb = std::get_if<BoolValue>(&left->data);
    auto* rb = std::get_if<BoolValue>(&right->data);

    // Lossy (outside double's 53-bit exact-integer range) conversion of an
    // Integer-like value for mixed Integer/Float arithmetic — same
    // tradeoff every other Int->Float promotion here already accepts.
    auto intToDouble = [](IntValue* iv, const mpz_class& asMpz) -> double {
        return iv ? static_cast<double>(iv->value) : asMpz.get_d();
    };

    // Float arithmetic that would produce NaN or Infinity raises instead —
    // including overflow like `1.0e308 * 10.0`, which BEAM also rejects. See
    // nonFiniteFloatError.
    auto floatOp = [&loc](double v, const char* what) -> ValuePtr {
        if (auto error = nonFiniteFloatError(v, what)) throw RuntimeError(*error, loc);
        return Value::floating(v);
    };

    switch (op) {
        case TokenType::Plus:
            if (li && ri) {
                int64_t result;
                if (!__builtin_add_overflow(li->value, ri->value, &result)) return Value::integer(result);
            }
            if (leftInt && rightInt) return integerResult(*leftInt + *rightInt);
            if (lf && rf) return floatOp(lf->value + rf->value, "Float addition");
            if (leftInt && rf) return floatOp(intToDouble(li, *leftInt) + rf->value, "Float addition");
            if (lf && rightInt) return floatOp(lf->value + intToDouble(ri, *rightInt), "Float addition");
            // String and Char concatenate as text — e.g. 'a' + 'b' == "ab",
            // "ab" + 'c' == "abc". This is broader than the Char/String
            // *equality* rule (Char isn't a String for ==) — here we just
            // want "what text does this contribute", which a bare Char
            // answers fine; see textContent vs. stringText in value.cxx.
            // A [Char] is a list and concatenates as one, just above.
            {
                auto* ll = std::get_if<ListValue>(&left->data);
                auto* rl = std::get_if<ListValue>(&right->data);
                if (ll && rl) {
                    std::vector<ValuePtr> elems = ll->elements;
                    elems.insert(elems.end(), rl->elements.begin(), rl->elements.end());
                    return Value::list(std::move(elems));
                }
            }
            if (auto lt = textContent(left)) {
                if (auto rt = textContent(right)) return Value::string(*lt + *rt);
            }
            throw RuntimeError("Cannot add " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Minus:
            if (li && ri) {
                int64_t result;
                if (!__builtin_sub_overflow(li->value, ri->value, &result)) return Value::integer(result);
            }
            if (leftInt && rightInt) return integerResult(*leftInt - *rightInt);
            if (lf && rf) return floatOp(lf->value - rf->value, "Float subtraction");
            if (leftInt && rf) return floatOp(intToDouble(li, *leftInt) - rf->value, "Float subtraction");
            if (lf && rightInt) return floatOp(lf->value - intToDouble(ri, *rightInt), "Float subtraction");
            throw RuntimeError("Cannot subtract " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Star:
            if (li && ri) {
                int64_t result;
                if (!__builtin_mul_overflow(li->value, ri->value, &result)) return Value::integer(result);
            }
            if (leftInt && rightInt) return integerResult(*leftInt * *rightInt);
            if (lf && rf) return floatOp(lf->value * rf->value, "Float multiplication");
            if (leftInt && rf) return floatOp(intToDouble(li, *leftInt) * rf->value, "Float multiplication");
            if (lf && rightInt) return floatOp(lf->value * intToDouble(ri, *rightInt), "Float multiplication");
            throw RuntimeError("Cannot multiply " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Slash:
            if (rightInt && *rightInt == 0) throw RuntimeError("Division by zero", loc);
            if (rf && rf->value == 0.0) throw RuntimeError("Division by zero", loc);
            if (li && ri) return Value::integer(li->value / ri->value);
            if (leftInt && rightInt) return integerResult(*leftInt / *rightInt);
            if (lf && rf) return floatOp(lf->value / rf->value, "Float division");
            if (leftInt && rf) return floatOp(intToDouble(li, *leftInt) / rf->value, "Float division");
            if (lf && rightInt) return floatOp(lf->value / intToDouble(ri, *rightInt), "Float division");
            throw RuntimeError("Cannot divide " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Percent:
            if (li && ri) {
                if (ri->value == 0) throw RuntimeError("Modulo by zero", loc);
                return Value::integer(li->value % ri->value);
            }
            if (leftInt && rightInt) {
                if (*rightInt == 0) throw RuntimeError("Modulo by zero", loc);
                return integerResult(*leftInt % *rightInt);
            }
            throw RuntimeError("Modulo requires integers", loc);

        case TokenType::Caret:
            // Preserve integer arithmetic (including arbitrary-precision
            // results) for non-negative integer exponents; fractional and
            // negative powers follow the floating-point Math.pow behavior.
            if (leftInt && rightInt && *rightInt >= 0 && rightInt->fits_ulong_p()) {
                mpz_class result;
                mpz_pow_ui(result.get_mpz_t(), leftInt->get_mpz_t(), rightInt->get_ui());
                return integerResult(std::move(result));
            }
            if (leftInt && rightInt)
                return floatOp(std::pow(intToDouble(li, *leftInt), intToDouble(ri, *rightInt)), "Exponentiation");
            if (lf && rf) return floatOp(std::pow(lf->value, rf->value), "Exponentiation");
            if (leftInt && rf) return floatOp(std::pow(intToDouble(li, *leftInt), rf->value), "Exponentiation");
            if (lf && rightInt) return floatOp(std::pow(lf->value, intToDouble(ri, *rightInt)), "Exponentiation");
            throw RuntimeError("Exponentiation requires numbers", loc);

        case TokenType::EqEq: return Value::boolean(valuesEqual(left, right));
        case TokenType::NotEq: return Value::boolean(!valuesEqual(left, right));

        case TokenType::LessThan:
            if (li && ri) return Value::boolean(li->value < ri->value);
            if (leftInt && rightInt) return Value::boolean(*leftInt < *rightInt);
            if (lf && rf) return Value::boolean(lf->value < rf->value);
            // Mixed Integer/Float promotes, exactly as the arithmetic
            // operators above do — ordering must not be the one place where
            // `1 < 1.5` is an error.
            if (leftInt && rf) return Value::boolean(intToDouble(li, *leftInt) < rf->value);
            if (lf && rightInt) return Value::boolean(lf->value < intToDouble(ri, *rightInt));
            if (ls && rs) return Value::boolean(ls->value < rs->value);
            if (lc && rc) return Value::boolean(lc->value < rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::GreaterThan:
            if (li && ri) return Value::boolean(li->value > ri->value);
            if (leftInt && rightInt) return Value::boolean(*leftInt > *rightInt);
            if (lf && rf) return Value::boolean(lf->value > rf->value);
            if (leftInt && rf) return Value::boolean(intToDouble(li, *leftInt) > rf->value);
            if (lf && rightInt) return Value::boolean(lf->value > intToDouble(ri, *rightInt));
            if (ls && rs) return Value::boolean(ls->value > rs->value);
            if (lc && rc) return Value::boolean(lc->value > rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::LessEq:
            if (li && ri) return Value::boolean(li->value <= ri->value);
            if (leftInt && rightInt) return Value::boolean(*leftInt <= *rightInt);
            if (lf && rf) return Value::boolean(lf->value <= rf->value);
            if (leftInt && rf) return Value::boolean(intToDouble(li, *leftInt) <= rf->value);
            if (lf && rightInt) return Value::boolean(lf->value <= intToDouble(ri, *rightInt));
            if (ls && rs) return Value::boolean(ls->value <= rs->value);
            if (lc && rc) return Value::boolean(lc->value <= rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::GreaterEq:
            if (li && ri) return Value::boolean(li->value >= ri->value);
            if (leftInt && rightInt) return Value::boolean(*leftInt >= *rightInt);
            if (lf && rf) return Value::boolean(lf->value >= rf->value);
            if (leftInt && rf) return Value::boolean(intToDouble(li, *leftInt) >= rf->value);
            if (lf && rightInt) return Value::boolean(lf->value >= intToDouble(ri, *rightInt));
            if (ls && rs) return Value::boolean(ls->value >= rs->value);
            if (lc && rc) return Value::boolean(lc->value >= rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::AmpAmp:
            return Value::boolean(left->isTrue() && right->isTrue());

        case TokenType::PipePipe:
            return Value::boolean(left->isTrue() || right->isTrue());

        default:
            throw RuntimeError("Unknown operator", loc);
    }
}

auto Evaluator::evalUnaryOp(TokenType op, const ValuePtr& operand,
                            SourceLocation loc) -> ValuePtr {
    if (const auto* ph = std::get_if<PlaceholderValue>(&operand->data))
        throw PlaceholderMisuse(ph->name, "this operator");
    switch (op) {
        case TokenType::Minus:
            if (auto* i = std::get_if<IntValue>(&operand->data)) {
                // -INT64_MIN doesn't fit in int64_t — promote rather than
                // silently wrap/UB, same as the overflow-checked binary ops.
                // mpz_class built via decimal string, not
                // static_cast<long>: `long` is 32-bit on wasm32, unlike
                // every native (LP64) target this project has built on
                // before — see value.cxx's asInteger/integerResult.
                if (i->value == INT64_MIN) return Value::bigInteger(-mpz_class(std::to_string(i->value)));
                return Value::integer(-i->value);
            }
            if (auto* bi = std::get_if<BigIntValue>(&operand->data))
                return Value::bigInteger(-bi->value);
            if (auto* f = std::get_if<FloatValue>(&operand->data))
                return Value::floating(-f->value);
            throw RuntimeError("Cannot negate " + operand->typeName(), loc);

        case TokenType::Bang:
            return Value::boolean(!operand->isTrue());

        default:
            throw RuntimeError("Unknown unary operator", loc);
    }
}

// A semantic::StructuredType as the Kex `Type` record it stands for.
// Is this name bound to a module value (directly, or inside the module being
// evaluated)? Modules registered without a mangled prefix live here.
auto Evaluator::knownModuleValue(const std::string& name) const -> bool {
    auto binding = m_env->get(name);
    if (binding && std::holds_alternative<ModuleValue>(binding->data)) return true;
    if (m_currentModule.empty()) return false;
    auto scoped = m_env->get(m_currentModule + "::" + name);
    return scoped && std::holds_alternative<ModuleValue>(scoped->data);
}

auto Evaluator::structuredTypeValue(const semantic::StructuredType& type)
    -> ValuePtr {
    std::vector<ValuePtr> args;
    for (const auto& arg : type.args) args.push_back(structuredTypeValue(arg));
    return Value::record("Type", {
        {"name", Value::string(type.name)},
        {"args", Value::list(std::move(args))},
        {"pure", Value::boolean(type.pure)},
    });
}

auto Evaluator::callFunction(const std::string& name, std::vector<ValuePtr> args,
                             NamedArgs namedArgs, SourceLocation loc) -> ValuePtr {
    checkDeadline();
    // BEAM-style reduction-counted auto-yield: placed at function-call
    // boundaries, the same kind of safe point BEAM itself uses, so a compute-bound process that never calls
    // `receive` still gives other processes a turn periodically.
    m_scheduler->tickReduction();
    // The testing DSL's natives are handed only their arguments; `it` needs the
    // call site to tell an editor where the case is (kexhq/kex#199).
    if (loc.line > 0) m_lastCallLocation = loc;

    std::string lookupName = name;
    if (!m_currentModule.empty() && name.find("::") == std::string::npos) {
        // Innermost module first, then each enclosing one — the same lexical
        // rule the resolver applies, so `Outer.Inner` sees `Outer`'s names
        // and every module body sees the file's.
        for (auto scope = m_currentModule; !scope.empty();) {
            const auto scopedName = scope + "::" + name;
            if (m_env->get(scopedName)) { lookupName = scopedName; break; }
            const auto dot = scope.rfind('.');
            if (dot == std::string::npos) break;
            scope.resize(dot);
        }
    }
    auto val = m_env->get(lookupName);
    if (!val && !name.empty() && std::isupper(static_cast<unsigned char>(name[0]))) {
        for (const auto& [modName, entry] : m_moduleRegistry) {
            auto eit = entry.exports.find(name);
            if (eit != entry.exports.end()) {
                lookupName = modName + "::" + name;
                val = eit->second;
                break;
            }
        }
    }
    // Bare UFCS (`map(xs, f)`) is equivalent to receiver syntax
    // (`xs.map(f)`). Once a public native alias is removed, route the call to
    // the source-owned receiver method selected from argument zero.
    if (!val && !args.empty() && name.find("::") == std::string::npos) {
        auto methodName = resolveMethodName(args[0], name, &args);
        if (methodName != name) {
            lookupName = std::move(methodName);
            val = m_env->get(lookupName);
        }
    }
    if (!val) {
        throw RuntimeError("Undefined function: " + name, loc);
    }

    if (auto* func = std::get_if<FunctionValue>(&val->data)) {
        if (func->native) {
            // Reorder: place named args into correct positions based on param names
            if (!namedArgs.empty()) {
                auto it = m_functionDefs.find(lookupName);
                if (it != m_functionDefs.end() && !it->second.empty()) {
                    const auto* selected = findNamedClause(lookupName, namedArgs);
                    // No clause declares one of these labels. Falling back to
                    // clause 0 would drop it, turning `m.to(String, in: kWh)`
                    // into a plain `to(String)` whose answer looks fine and
                    // ignores what was asked for. Say so, and point at the
                    // module that does define a matching clause if there is
                    // one — that is nearly always a missing `using`.
                    if (!selected)
                        if (auto unknown = unknownNamedArgument(lookupName, namedArgs)) {
                            auto message = "unknown named argument `" + *unknown
                                + ":` for `" + lookupName + "`";
                            if (auto module =
                                    moduleSupplyingNamedClause(lookupName, namedArgs)) {
                                const auto separator = lookupName.rfind("::");
                                message += " — `" + *module + "` defines a matching `"
                                    + (separator == std::string::npos
                                        ? lookupName : lookupName.substr(separator + 2))
                                    + "`; add `using " + *module + "`";
                            }
                            throw RuntimeError(message, loc);
                        }
                    const auto& clause = selected
                        ? *selected : it->second[0]->clauses[0];
                    const auto receiverOffset =
                        receiverArgumentOffset(lookupName, args);
                    // Build full arg list: place named args by matching
                    // param names first, then fill whatever slots remain
                    // (in order) from the positional args. Named-first
                    // matters because `args` may include a trailing
                    // do-block appended as an extra positional value (see
                    // "Handle block as last arg" above) — its destination
                    // param is often last, not at the front, so it must
                    // land in whichever slot is actually still open rather
                    // than wherever index 0 happens to be.
                    size_t totalParams = clause.params.size();
                    std::vector<ValuePtr> fullArgs(totalParams + receiverOffset, nullptr);
                    if (receiverOffset) fullArgs[0] = std::move(args[0]);

                    for (auto& [argName, argVal] : namedArgs) {
                        for (size_t i = 0; i < clause.params.size(); i++) {
                            if (clause.params[i].name &&
                                *clause.params[i].name == argName) {
                                fullArgs[i + receiverOffset] = std::move(argVal);
                                break;
                            }
                        }
                    }

                    size_t nextSlot = receiverOffset;
                    for (size_t argIndex = receiverOffset; argIndex < args.size(); argIndex++) {
                        auto& a = args[argIndex];
                        while (nextSlot < fullArgs.size() && fullArgs[nextSlot])
                            nextSlot++;
                        if (nextSlot == fullArgs.size()) break;
                        fullArgs[nextSlot] = std::move(a);
                    }

                    // Fill any remaining nulls with None
                    for (auto& a : fullArgs) {
                        if (!a) a = Value::none();
                    }

                    return func->native(std::move(fullArgs));
                } else {
                    // No def info — just append named args
                    for (auto& [_, v] : namedArgs) {
                        args.push_back(std::move(v));
                    }
                }
            }
            return func->native(std::move(args));
        }
    }

    throw RuntimeError("'" + name + "' is not callable", loc);
}

auto Evaluator::findNamedClause(const std::string& functionName,
                                const NamedArgs& namedArgs) const
    -> const ast::FunctionClause* {
    auto definitions = m_functionDefs.find(functionName);
    if (definitions == m_functionDefs.end()) return nullptr;

    for (const auto* definition : definitions->second) {
        for (const auto& clause : definition->clauses) {
            bool acceptsAll = std::all_of(
                namedArgs.begin(), namedArgs.end(), [&](const auto& named) {
                    return std::any_of(
                        clause.params.begin(), clause.params.end(),
                        [&](const auto& param) {
                            return param.name && *param.name == named.first;
                        });
                });
            if (acceptsAll) return &clause;
        }
    }
    return nullptr;
}

auto Evaluator::unknownNamedArgument(const std::string& functionName,
                                     const NamedArgs& namedArgs) const
    -> std::optional<std::string> {
    auto definitions = m_functionDefs.find(functionName);
    if (definitions == m_functionDefs.end()) return std::nullopt;

    for (const auto& [label, _] : namedArgs) {
        bool declared = false;
        for (const auto* definition : definitions->second)
            for (const auto& clause : definition->clauses)
                for (const auto& param : clause.params)
                    if (param.name && *param.name == label) declared = true;
        if (!declared) return label;
    }
    return std::nullopt;
}

auto Evaluator::moduleSupplyingNamedClause(const std::string& functionName,
                                           const NamedArgs& namedArgs) const
    -> std::optional<std::string> {
    const auto separator = functionName.rfind("::");
    const auto shortName = separator == std::string::npos
        ? functionName : functionName.substr(separator + 2);

    for (const auto& [key, _] : m_functionDefs) {
        const auto keySeparator = key.rfind("::");
        if (keySeparator == std::string::npos) continue;
        if (key.substr(keySeparator + 2) != shortName) continue;
        auto scope = key.substr(0, keySeparator);
        if (key == functionName || !m_moduleRegistry.contains(scope)) continue;
        if (findNamedClause(key, namedArgs)) return scope;
    }
    return std::nullopt;
}

auto Evaluator::findImportedNamedOverload(const std::string& functionName,
                                          const NamedArgs& namedArgs,
                                          const ValuePtr& receiver) const
    -> std::optional<std::string> {
    if (namedArgs.empty()) return std::nullopt;

    for (auto scope = m_importScopes.rbegin(); scope != m_importScopes.rend(); ++scope) {
        auto imported = scope->find(functionName);
        if (imported == scope->end()) continue;

        auto qualified = imported->second.module + "::" + functionName;
        if (const auto* clause = findNamedClause(qualified, namedArgs)) {
            if (!clause->params.empty() && clause->params.front().type &&
                *clause->params.front().type &&
                !runtimeTypeMatches(receiver, **clause->params.front().type))
                return std::nullopt;
            return qualified;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

auto Evaluator::receiverArgumentOffset(const std::string& functionName,
                                       const std::vector<ValuePtr>& args) const -> size_t {
    if (args.empty()) return 0;

    auto separator = functionName.rfind("::");
    if (separator == std::string::npos) return 0;

    auto scope = functionName.substr(0, separator);
    auto receiverType = dispatchTypeName(args[0]);
    if (scope == receiverType) return 1;
    // Module-owned records retain their qualified runtime identity
    // (`CollisionWeb.Server`), while a make block written as `make Server`
    // registers `Server::method`. Method resolution applies this same
    // last-segment fallback; arity matching must count its implicit receiver
    // too, or the result depends on which same-named import is encountered.
    if (const auto dot = receiverType.rfind('.'); dot != std::string::npos &&
        scope == receiverType.substr(dot + 1))
        return 1;

    auto parent = m_variantParent.find(receiverType);
    return parent != m_variantParent.end() && scope == parent->second ? 1 : 0;
}

auto Evaluator::registerRuntimeSignature(
    const ast::TypeAnnotation& annotation,
    const std::string& scope,
    bool implicitReceiver) -> void {
    RuntimeSignature signature;
    if (implicitReceiver) signature.receiverType = scope;
    const ast::TypeExpr* current = annotation.type.get();
    while (current) {
        auto* function = std::get_if<ast::FunctionType>(&current->kind);
        if (!function) break;
        if (function->param) signature.params.push_back(function->param.get());
        current = function->result.get();
    }
    auto name = scope.empty()
        ? annotation.name
        : scope + "::" + annotation.name;
    m_runtimeSignatures[name].push_back(std::move(signature));
}

auto Evaluator::runtimeTypeMatches(const ValuePtr& value,
                                   const ast::TypeExpr& type) const -> bool {
    return std::visit([&](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::TypeName>) {
            if (node.parts.empty()) return true;
            const auto& expected = node.parts.back();
            if (expected == "Any" ||
                (expected.size() == 1 &&
                 std::isupper(static_cast<unsigned char>(expected[0]))))
                return true;
            auto actual = dispatchTypeName(value);
            if (actual == expected) return true;
            if (auto parent = m_variantParent.find(actual);
                parent != m_variantParent.end() && parent->second == expected)
                return true;
            // An alias is whatever it stands for: `handler: CommandHandler`
            // has to accept a closure, and `path: FilePath` a String.
            if (auto alias = m_typeAliases.find(expected);
                alias != m_typeAliases.end() && alias->second)
                return runtimeTypeMatches(value, *alias->second);
            return expected == "Int" && actual == "Integer";
        } else if constexpr (std::is_same_v<T, ast::GenericType>) {
            if (node.name.parts.empty()) return true;
            const auto& expected = node.name.parts.back();
            auto actual = dispatchTypeName(value);
            if (expected == "Optional")
                return actual == "None" || actual == "Just" ||
                       (m_variantParent.contains(actual) &&
                        m_variantParent.at(actual) == "Optional");
            return actual == expected ||
                (m_variantParent.contains(actual) &&
                 m_variantParent.at(actual) == expected);
        } else if constexpr (std::is_same_v<T, ast::FunctionType> ||
                             std::is_same_v<T, ast::BlockType>) {
            return value && std::holds_alternative<FunctionValue>(value->data);
        } else if constexpr (std::is_same_v<T, ast::ListType>) {
            if (!value) return false;
            if (std::holds_alternative<ListValue>(value->data)) return true;
            if (node.element) {
                if (auto* element =
                        std::get_if<ast::TypeName>(&node.element->kind);
                    element && !element->parts.empty() &&
                    element->parts.back() == "Char")
                    return std::holds_alternative<StringValue>(value->data);
            }
            return false;
        } else if constexpr (std::is_same_v<T, ast::TupleType>) {
            return value && std::holds_alternative<TupleValue>(value->data);
        } else if constexpr (std::is_same_v<T, ast::MapType>) {
            return value && std::holds_alternative<MapValue>(value->data);
        } else if constexpr (std::is_same_v<T, ast::UnionType>) {
            return (node.left && runtimeTypeMatches(value, *node.left)) ||
                   (node.right && runtimeTypeMatches(value, *node.right));
        } else if constexpr (std::is_same_v<T, ast::IntersectionType>) {
            return node.left && node.right &&
                runtimeTypeMatches(value, *node.left) &&
                runtimeTypeMatches(value, *node.right);
        } else if constexpr (std::is_same_v<T, ast::RecordType>) {
            if (!value) return false;
            const auto* record = std::get_if<RecordValue>(&value->data);
            if (!record) return false;
            for (const auto& [name, fieldType] : node.fields) {
                auto field = record->fields.find(name);
                if (field == record->fields.end() || !fieldType ||
                    !runtimeTypeMatches(field->second, *fieldType))
                    return false;
            }
            return true;
        } else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            auto actual = dispatchTypeName(value);
            return actual == "None" || actual == "Just" ||
                   (m_variantParent.contains(actual) &&
                    m_variantParent.at(actual) == "Optional");
        } else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return value && std::holds_alternative<AtomValue>(value->data);
        } else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            return true;
        } else {
            return true;
        }
    }, type.kind);
}

auto Evaluator::runtimeTypeKey(const ast::TypeExpr& type) const -> std::string {
    return std::visit([&](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::TypeName>) {
            return node.parts.empty() ? "*" : node.parts.back();
        } else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::string out =
                node.name.parts.empty() ? "*" : node.name.parts.back();
            out += "<";
            for (size_t i = 0; i < node.args.size(); i++) {
                if (i) out += ",";
                out += node.args[i] ? runtimeTypeKey(*node.args[i]) : "*";
            }
            return out + ">";
        } else if constexpr (std::is_same_v<T, ast::ListType>) {
            return "[" +
                (node.element ? runtimeTypeKey(*node.element) : "*") + "]";
        } else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return (node.inner ? runtimeTypeKey(*node.inner) : "*") + "?";
        } else if constexpr (std::is_same_v<T, ast::UnionType>) {
            return (node.left ? runtimeTypeKey(*node.left) : "*") + "|" +
                (node.right ? runtimeTypeKey(*node.right) : "*");
        } else if constexpr (std::is_same_v<T, ast::IntersectionType>) {
            return (node.left ? runtimeTypeKey(*node.left) : "*") + "&" +
                (node.right ? runtimeTypeKey(*node.right) : "*");
        } else if constexpr (std::is_same_v<T, ast::RecordType>) {
            std::string out = "{";
            for (std::size_t i = 0; i < node.fields.size(); i++) {
                if (i) out += ",";
                out += node.fields[i].first + ":";
                out += node.fields[i].second
                    ? runtimeTypeKey(*node.fields[i].second) : "*";
            }
            return out + "}";
        } else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            return node.name;
        } else {
            return std::to_string(type.kind.index());
        }
    }, type.kind);
}

// Is the module owning `Type::method` in scope here? A method from a top-level
// `make` (no owner) always is. Otherwise it takes a `using` — file-level or the
// lexical block form, both recorded in m_usingModules — or a call from inside
// the module itself. Before this, a `make` inside a module was the one module
// member visible everywhere with no import at all.
auto Evaluator::makeMethodInScope(const std::string& qualified) const -> bool {
    auto owner = m_makeMethodModule.find(qualified);
    if (owner == m_makeMethodModule.end()) return true;
    const auto& module = owner->second;
    if (module.empty()) return true;
    if (m_currentModule == module ||
        m_currentModule.rfind(module + ".", 0) == 0)
        return true;
    for (const auto& scope : m_usingModules)
        for (const auto& imported : scope)
            if (imported == module ||
                imported.rfind(module + ".", 0) == 0 ||
                module.rfind(imported + ".", 0) == 0)
                return true;
    return false;
}

auto Evaluator::resolveMethodName(const ValuePtr& receiver,
                                  const std::string& method,
                                  const std::vector<ValuePtr>* args) const
    -> std::string {
    auto receiverType = dispatchTypeName(receiver);
    // A record declared inside a module carries a module-qualified identity
    // (`Boxes.Box`) so same-named records in sibling modules stay distinct,
    // but a `make Box` written inside that module registers its methods under
    // the name as written. Prefer a qualified registration when one exists,
    // and otherwise dispatch on the last segment.
    if (const auto dot = receiverType.rfind('.'); dot != std::string::npos) {
        const auto qualified = receiverType + "::" + method;
        const auto shortType = receiverType.substr(dot + 1);
        const auto shortQualified = shortType + "::" + method;
        // A module function can have the same qualified name as its record
        // (`module Server; let new(...)`) while `make Server` registers the
        // actual receiver method under the short type. Presence of that
        // namespace function must not suppress receiver-method fallback.
        const bool shortMethod = m_functionDefs.contains(shortQualified) ||
            m_runtimeSignatures.contains(shortQualified);
        if ((!m_env->get(qualified) && !m_functionValues.count(qualified)) ||
            shortMethod)
            receiverType = std::move(shortType);
    }
    auto typed = receiverType + "::" + method;
    const bool protocolMethod = std::any_of(
        m_traitMethods.begin(), m_traitMethods.end(),
        [&](const auto& trait) {
            return std::find(trait.second.begin(), trait.second.end(), method) !=
                   trait.second.end();
        });
    if (protocolMethod && m_functionValues.count(typed) && !args) return typed;

    // A required protocol method may be supplied by the trait's generic make
    // block. Keep this fallback below a concrete implementation, but make it
    // available to direct calls as well as to calls through a trait-typed
    // parameter.
    std::string protocolFallback;
    if (protocolMethod) {
        for (const auto& [traitName, methods] : m_traitMethods) {
            if (std::find(methods.begin(), methods.end(), method) == methods.end())
                continue;
            const auto candidate = traitName + "::" + method;
            if (!m_functionValues.count(candidate)) continue;
            if (!protocolFallback.empty()) {
                protocolFallback.clear();
                break;
            }
            protocolFallback = candidate;
        }
    }

    // A value kind with no dispatch name of its own (a module, a function) has
    // nothing to key the name-based lookups below on — `"" + "::" + method`
    // matches nothing and the import-scope search would resolve the bare name
    // to some unrelated module's function. It can still be carried by a
    // trait's generic make block, so offer that and stop.
    if (receiverType.empty())
        return protocolFallback.empty() ? method : protocolFallback;

    if (args) {
        // A definition with inline parameter annotations and no separate
        // `name : T -> U` line registers no runtime signature, so the
        // signature-based test below cannot see it. Units.SI's
        // `let to(measure: Measure, String, ...)` is exactly that shape, and
        // without this it claimed EVERY receiver: `using Units.SI` made
        // `42.to(String)`, `42.to(Integer)` and `"7".to(Integer)` all answer
        // None. Fall back to the definition's own parameter annotations.
        auto matchesDefinition = [&](const std::string& candidate) {
            auto defs = m_functionDefs.find(candidate);
            if (defs == m_functionDefs.end()) return false;
            const auto receiverOffset = receiverArgumentOffset(candidate, *args);
            for (const auto* def : defs->second) {
                if (!def) continue;
                for (const auto& clause : def->clauses) {
                    if (clause.params.size() + receiverOffset != args->size())
                        continue;
                    bool allMatch = true;
                    for (size_t i = 0; i < clause.params.size(); ++i) {
                        const auto& param = clause.params[i];
                        if (!param.type || !*param.type) continue;
                        if (!runtimeTypeMatches((*args)[i + receiverOffset],
                                                **param.type)) {
                            allMatch = false;
                            break;
                        }
                    }
                    if (allMatch) return true;
                }
            }
            return false;
        };
        auto matches = [&](const std::string& candidate) {
            auto found = m_runtimeSignatures.find(candidate);
            if (found == m_runtimeSignatures.end())
                return matchesDefinition(candidate);
            for (const auto& signature : found->second) {
                const auto expectedSize =
                    signature.params.size() +
                    (signature.receiverType.empty() ? 0 : 1);
                if (expectedSize != args->size()) continue;
                size_t offset = 0;
                if (!signature.receiverType.empty()) {
                    if (receiverType != signature.receiverType) continue;
                    offset = 1;
                }
                bool allMatch = true;
                for (size_t i = 0; i < signature.params.size(); ++i)
                    if (!runtimeTypeMatches((*args)[i + offset],
                                            *signature.params[i])) {
                        allMatch = false;
                        break;
                    }
                if (allMatch) return true;
            }
            return false;
        };

        const bool typedMatches = matches(typed);
        const bool typedExists = m_env->get(typed) != nullptr ||
            m_functionValues.count(typed) > 0;
        const bool typedHasSignatures = m_runtimeSignatures.count(typed) > 0;
        std::string imported;
        for (auto scope = m_importScopes.rbegin();
             scope != m_importScopes.rend(); ++scope) {
            auto origin = scope->find(method);
            if (origin == scope->end()) continue;
            auto candidate = origin->second.module + "::" + method;
            if (matches(candidate)) imported = std::move(candidate);
            break;
        }
        if ((typedMatches ||
             (protocolMethod && typedExists && !typedHasSignatures)) &&
            makeMethodInScope(typed))
            return typed;
        if (!imported.empty()) return imported;
    }

    if (m_env->get(typed) && makeMethodInScope(typed)) return typed;

    // `make Number do ... end` covers Integer and Float. Below the concrete
    // lookup above, so a type defining the method itself still wins, and
    // above the trait fallback, since it is a real implementation.
    for (const auto& super : dispatchSupertypes(receiverType)) {
        auto typedBySuper = super + "::" + method;
        if (m_env->get(typedBySuper) && makeMethodInScope(typedBySuper))
            return typedBySuper;
    }

    // A variant value is tagged with its constructor while methods are
    // registered under the parent ADT's name. This has to be tried before the
    // trait's generic make block: `make Optional<Showable>, implement:
    // Showable` is a concrete implementation and must win over the structural
    // `make Showable` default, exactly as `Date::showValue` does.
    auto parentIt = m_variantParent.find(receiverType);
    if (parentIt != m_variantParent.end()) {
        auto typedByParent = parentIt->second + "::" + method;
        if (m_env->get(typedByParent)) return typedByParent;
    }

    if (!protocolFallback.empty()) return protocolFallback;

    return method;
}

auto Evaluator::autoCallZeroArgConstant(const std::string& name, const ValuePtr& val) -> ValuePtr {
    // Only acts on user-defined Kex functions (tracked in m_functionDefs);
    // native builtins, namespace placeholders (RecordValue), and ADT
    // constructors (registered directly via m_env->define, never through
    // execFunctionDef) are absent from m_functionDefs and pass through
    // unchanged here.
    auto* func = std::get_if<FunctionValue>(&val->data);
    if (!func || !func->native) return val;

    auto defIt = m_functionDefs.find(name);
    for (auto scope = m_currentModule;
         defIt == m_functionDefs.end() && !scope.empty();) {
        defIt = m_functionDefs.find(scope + "::" + name);
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) break;
        scope.resize(dot);
    }
    if (defIt == m_functionDefs.end() || defIt->second.empty()) return val;
    if (!defIt->second[0]->clauses[0].params.empty()) return val;

    auto savedEnv = m_env;
    try {
        auto result = func->native({});
        if (result && !result->isNone()) {
            return result;
        }
    } catch (...) {
        // The body already ran and failed — restore the environment the
        // partial call left behind, but let the error out. Swallowing it here
        // made `boom` (a bare reference that auto-calls) silently evaluate to
        // the function value, so a failing statement looked like it succeeded
        // and the program carried on to the next one.
        m_env = savedEnv;
        throw;
    }
    return val;
}

auto Evaluator::matchPattern(const ast::Pattern& pattern, const ValuePtr& value) -> bool {
    return std::visit([this, &value](const auto& pat) -> bool {
        using T = std::decay_t<decltype(pat)>;

        if constexpr (std::is_same_v<T, ast::WildcardPattern>) {
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            // @pattern — match the inner pattern against 'this' (the value)
            if (pat.inner) {
                return matchPattern(*pat.inner, value);
            }
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::VarPattern>) {
            m_env->define(pat.name, value);
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
            if (pat.literal.type == TokenType::Integer) {
                // mpz_class(string) handles literal patterns too big for
                // int64_t the same way IntLiteral evaluation does; asInteger
                // matches against either runtime representation of Integer.
                auto literal = mpz_class(pat.literal.value);
                if (auto valueInt = asInteger(value)) return *valueInt == literal;
                // A Float scrutinee matches numerically, so patterns agree
                // with `==` — `safeDiv(_, 0)` catches a 0.0 divisor.
                if (auto* fv = std::get_if<FloatValue>(&value->data))
                    return fv->value == literal.get_d();
                return false;
            }
            if (pat.literal.type == TokenType::Float) {
                const double literal = std::stod(pat.literal.value);
                if (auto* fv = std::get_if<FloatValue>(&value->data))
                    return fv->value == literal;
                if (auto valueInt = asInteger(value))
                    return valueInt->get_d() == literal;
                return false;
            }
            if (pat.literal.type == TokenType::String ||
                pat.literal.type == TokenType::RawString) {
                auto* sv = std::get_if<StringValue>(&value->data);
                return sv && sv->value == pat.literal.value;
            }
            if (pat.literal.type == TokenType::Char) {
                // Char is its own type, not a 1-character String — a
                // char-literal pattern only matches a Char value.
                auto* cv = std::get_if<CharValue>(&value->data);
                auto literal = utf8::decode(pat.literal.value);
                return cv && cv->value ==
                    (literal.empty() ? U'\0' : literal.front());
            }
            if (pat.literal.type == TokenType::True) {
                auto* bv = std::get_if<BoolValue>(&value->data);
                return bv && bv->value;
            }
            if (pat.literal.type == TokenType::False) {
                auto* bv = std::get_if<BoolValue>(&value->data);
                return bv && !bv->value;
            }
            if (pat.literal.type == TokenType::None) {
                return value->isNone();
            }
            if (pat.literal.type == TokenType::Atom) {
                auto* av = std::get_if<AtomValue>(&value->data);
                return av && av->name == pat.literal.value;
            }
            return false;
        }
        else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            auto* tv = std::get_if<TupleValue>(&value->data);
            if (!tv || tv->elements.size() != pat.elements.size()) return false;
            for (size_t i = 0; i < pat.elements.size(); i++) {
                if (!matchPattern(*pat.elements[i], tv->elements[i])) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::RangePattern>) {
            auto* rv = std::get_if<RangeValue>(&value->data);
            if (!rv) return false;
            auto startVal = rv->isChar
                ? Value::character(static_cast<char32_t>(rv->start))
                : Value::integer(rv->start);
            auto endVal = rv->isChar
                ? Value::character(static_cast<char32_t>(rv->end))
                : Value::integer(rv->end);
            if (!matchPattern(*pat.start, startVal)) return false;
            if (!matchPattern(*pat.end, endVal)) return false;
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            // Strings are stored as StringValue but are semantically [Char],
            // so a list pattern against a String treats it as a char sequence.
            std::vector<ValuePtr> chars;
            const std::vector<ValuePtr>* elements = nullptr;
            if (auto* sv = std::get_if<StringValue>(&value->data)) {
                for (auto cp : utf8::decode(sv->value))
                    chars.push_back(Value::character(cp));
                elements = &chars;
            } else if (auto* lv = std::get_if<ListValue>(&value->data)) {
                elements = &lv->elements;
            } else {
                return false;
            }
            if (pat.elements.empty() && !pat.rest) {
                return elements->empty();
            }
            if (pat.rest) {
                // [x | xs] — at least as many elements as the fixed part
                if (elements->size() < pat.elements.size()) return false;
            } else {
                // [x] or [x, y] — exact length match (no rest captures surplus)
                if (elements->size() != pat.elements.size()) return false;
            }
            for (size_t i = 0; i < pat.elements.size(); i++) {
                if (!matchPattern(*pat.elements[i], (*elements)[i])) return false;
            }
            if (pat.rest) {
                // Reconstruct tail: if original was a String, tail is also a String
                if (std::get_if<StringValue>(&value->data)) {
                    std::string tail;
                    for (size_t i = pat.elements.size(); i < elements->size(); i++) {
                        if (auto* cv = std::get_if<CharValue>(&(*elements)[i]->data))
                            tail += utf8::encode(cv->value);
                    }
                    if (!matchPattern(**pat.rest, Value::string(tail))) return false;
                } else {
                    std::vector<ValuePtr> rest(elements->begin() + pat.elements.size(), elements->end());
                    if (!matchPattern(**pat.rest, Value::list(std::move(rest)))) return false;
                }
            }
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            // Match None
            if (pat.name == "None") return value->isNone();

            if (pat.args.empty()) {
                // Match zero-arg variant constructors (None, Less, Fizz, ...)
                if (auto* var = std::get_if<VariantValue>(&value->data)) {
                    if (var->tag == pat.name && var->args.empty()) return true;
                }
                // A bare payload constructor used as a display-prefix marker
                // (for example `in: Mega`) is represented by its constructor
                // function until it receives a payload. Let a nullary pattern
                // select that marker without attempting to call it.
                if (auto* func = std::get_if<FunctionValue>(&value->data)) {
                    if (func->name == pat.name) return true;
                }

                // Match type names as type patterns (for runtime type checking).
                // These `if (holds_alternative) return true;` rather than
                // `return holds_alternative(...)` so that e.g. a bare `Integer`
                // expression — which resolves to the Integer::parse namespace
                // RecordValue, not an IntValue — still falls through to the
                // record-typeName check below instead of failing outright.
                if (matchesTypeName(pat.name, value)) return true;
                // Match record type name
                if (auto* rec = std::get_if<RecordValue>(&value->data)) {
                    if (rec->typeName == pat.name) return true;
                }
                // Match builtin type namespaces (String, Integer, Float, ...
                // registered as ModuleValue) — needed for the `to(String)`
                // conversion-protocol pattern and similar type-name matches.
                if (auto* mod = std::get_if<ModuleValue>(&value->data)) {
                    if (mod->name == pat.name) return true;
                }
                // Match True/False as literal patterns
                if (pat.name == "True") {
                    auto* b = std::get_if<BoolValue>(&value->data);
                    return b && b->value;
                }
                if (pat.name == "False") {
                    auto* b = std::get_if<BoolValue>(&value->data);
                    return b && !b->value;
                }
            }

            // Constructor with args: Just(x), Ok(x), Error(e), Number(n), etc.
            if (auto* var = std::get_if<VariantValue>(&value->data)) {
                if (var->tag != pat.name) return false;
                if (var->args.size() != pat.args.size()) return false;
                for (size_t i = 0; i < pat.args.size(); i++) {
                    if (!matchPattern(*pat.args[i], var->args[i])) return false;
                }
                return true;
            }
            // RecordValue constructors for user-defined records (not ADT variants)
            if (auto* rec = std::get_if<RecordValue>(&value->data)) {
                if (rec->typeName != pat.name) return false;
                if (rec->fields.size() != pat.args.size()) return false;
                for (size_t i = 0; i < pat.args.size(); i++) {
                    auto it = rec->fields.find(std::to_string(i));
                    if (it == rec->fields.end()) return false;
                    if (!matchPattern(*pat.args[i], it->second)) return false;
                }
                return true;
            }
            return false;
        }
        else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            if (auto* rv = std::get_if<RecordValue>(&value->data)) {
                // A named record pattern (`Foo { x }`) additionally asserts the
                // value's record type; the anonymous `{ x }` matches any record.
                if (!pat.typeName.empty() &&
                    rv->typeName != resolveRecordTypeName(pat.typeName))
                    return false;
                for (const auto& field : pat.fields) {
                    auto it = rv->fields.find(field.name);
                    if (it == rv->fields.end()) return false;
                    if (field.pattern && !matchPattern(**field.pattern, it->second)) return false;
                    if (!field.pattern) m_env->define(field.name, it->second);
                }
                return true;
            }
            return false;
        }
        else {
            return false;
        }
    }, pattern.kind);
}

auto Evaluator::registerBuiltins() -> void {
    // Orchestrator only — each domain is implemented in its own file under
    // src/interpreter/stdlib/. Collection domains run after List because they
    // reuse selected qualified List implementations.
    registerAdtConstructors();
    registerIOBuiltins();
    registerFileBuiltins();
    registerDirectoryBuiltins();
    registerMockBuiltins();
    registerListBuiltins();
    registerStringBuiltins();
    registerBinaryBuiltins();
    registerNumberBuiltins();
    registerRegexBuiltins();
    registerStreamBuiltins();
    registerMapBuiltins();
    registerEnvBuiltins();
    registerMathBuiltins();
    registerTimeBuiltins();
    registerTypeBuiltins();
    registerBitsBuiltins();
    registerConsoleBuiltins();
    registerTestBuiltins();
    registerProcessBuiltins();
    registerDigestBuiltins();
    registerParserBuiltins();
    registerEvalBuiltins();
    registerNetBuiltins();
    registerKexBuiltins();

    // Kex.Intrinsic.Fun.applyItem(f, item) — auto-splat a pair into a
    // two-arg block. Backs all Enumerable HOFs (map/filter/each/etc.)
    // for Map enumeration where each item is a (K,V) tuple.
    {
        defineIntrinsic("Fun::convertTo", [](std::vector<ValuePtr> args) -> ValuePtr {
            if (args.size() < 2) return Value::none();
            const auto& val = args[0];
            std::string targetName;
            if (auto* m = std::get_if<ModuleValue>(&args[1]->data))
                targetName = m->name;
            else if (auto* v = std::get_if<VariantValue>(&args[1]->data))
                targetName = v->tag;
            // A `Type` VALUE names its target too: `x.to(Type.of(y))`.
            else if (auto* r = std::get_if<RecordValue>(&args[1]->data);
                     r && r->typeName == "Type") {
                if (auto field = r->fields.find("name"); field != r->fields.end())
                    if (auto* n = std::get_if<StringValue>(&field->second->data))
                        targetName = n->value;
            }

            // Optional third argument: `.to(String, radix: 16)` /
            // `.to(Integer, radix: 2)`. Only Integer <-> String is base
            // dependent; anything else with a radix is a conversion that was
            // not meant, so it fails rather than silently ignoring the base.
            if (args.size() >= 3) {
                auto radix = asInteger(args[2]);
                if (!radix || *radix < 2 || *radix > 36) return Value::none();
                int base = static_cast<int>(radix->get_si());
                if (targetName == "String") {
                    auto i = asInteger(val);
                    if (!i) return Value::none();
                    return Value::just(Value::string(i->get_str(base)));
                }
                if (targetName == "Integer") {
                    auto* s = std::get_if<StringValue>(&val->data);
                    if (!s) return Value::none();
                    if (auto parsed = parseIntegerInBase(s->value, base))
                        return Value::just(integerResult(*parsed));
                    return Value::none();
                }
                return Value::none();
            }

            if (targetName == "Integer") {
                if (std::holds_alternative<IntValue>(val->data) ||
                    std::holds_alternative<BigIntValue>(val->data))
                    return Value::just(val);
                if (auto* f = std::get_if<FloatValue>(&val->data))
                    return Value::just(Value::integer(static_cast<int64_t>(f->value)));
                if (auto* ch = std::get_if<CharValue>(&val->data))
                    return Value::just(Value::integer(static_cast<int64_t>(ch->value)));
                if (auto* s = std::get_if<StringValue>(&val->data)) {
                    try {
                        size_t parsed = 0;
                        auto v = std::stoll(s->value, &parsed);
                        if (parsed == s->value.size()) return Value::just(Value::integer(v));
                    } catch (...) {}
                }
                return Value::none();
            }
            if (targetName == "Byte") {
                auto integer = asInteger(val);
                if (integer && *integer >= 0 && *integer <= 255)
                    return Value::just(integerResult(*integer));
                return Value::none();
            }
            if (targetName == "Float") {
                if (auto* f = std::get_if<FloatValue>(&val->data))
                    return Value::just(val);
                if (auto i = asInteger(val))
                    return Value::just(Value::floating(i->get_d()));
                if (auto* s = std::get_if<StringValue>(&val->data)) {
                    try {
                        size_t parsed = 0;
                        auto v = std::stod(s->value, &parsed);
                        // std::stod accepts "nan"/"inf", which are not Kex
                        // Float values — see nonFiniteFloatError.
                        if (parsed == s->value.size() && !nonFiniteFloatError(v, "to(Float)"))
                            return Value::just(Value::floating(v));
                    } catch (...) {}
                }
                return Value::none();
            }
            if (targetName == "String") {
                if (auto* binary = std::get_if<BinaryValue>(&val->data)) {
                    std::string text(binary->bytes.begin(), binary->bytes.end());
                    for (std::size_t i = 0; i < text.size();) {
                        const auto lead = static_cast<uint8_t>(text[i]);
                        const size_t width = lead < 0x80 ? 1
                            : lead >= 0xc2 && lead <= 0xdf ? 2
                            : lead >= 0xe0 && lead <= 0xef ? 3
                            : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
                        if (!width || i + width > text.size()) return Value::none();
                        for (size_t j = 1; j < width; ++j)
                            if ((static_cast<uint8_t>(text[i + j]) & 0xc0) != 0x80)
                                return Value::none();
                        if (width == 3) {
                            const auto next = static_cast<uint8_t>(text[i + 1]);
                            if ((lead == 0xe0 && next < 0xa0) ||
                                (lead == 0xed && next >= 0xa0)) return Value::none();
                        }
                        if (width == 4) {
                            const auto next = static_cast<uint8_t>(text[i + 1]);
                            if ((lead == 0xf0 && next < 0x90) ||
                                (lead == 0xf4 && next >= 0x90)) return Value::none();
                        }
                        i += width;
                    }
                    return Value::just(Value::string(std::move(text)));
                }
                return Value::just(Value::string(val->toString()));
            }
            if (targetName == "Binary") {
                if (auto* text = std::get_if<StringValue>(&val->data))
                    return Value::just(Value::binary(
                        std::vector<uint8_t>(text->value.begin(), text->value.end())));
                if (std::holds_alternative<BinaryValue>(val->data))
                    return Value::just(val);
                return Value::none();
            }
            if (targetName == "List") {
                if (auto* range = std::get_if<RangeValue>(&val->data)) {
                    std::vector<ValuePtr> elems;
                    int64_t step = range->start <= range->end ? 1 : -1;
                    for (int64_t i = range->start; step > 0 ? i <= range->end : i >= range->end; i += step)
                        elems.push_back(Value::integer(i));
                    return Value::just(Value::list(std::move(elems)));
                }
                if (std::holds_alternative<ListValue>(val->data))
                    return Value::just(val);
                return Value::none();
            }
            return Value::none();
        });

        defineIntrinsic("Fun::applyItem", [this](std::vector<ValuePtr> args) -> ValuePtr {
            if (args.size() < 2) return Value::none();
            auto& fn = *args[0];
            auto& item = *args[1];
            // Don't splat here — pass the item as-is. The lambda wrapper
            // (line ~1094) auto-spreads a tuple into its params when the
            // param count matches, mirroring BEAM's arity-check in
            // kex_intrinsic_fun:applyItem/2. Splatting here would
            // incorrectly spread a 2-tuple into a 1-param function.
            std::vector<ValuePtr> callArgs = {args[1]};
            // Native FunctionValue — fast path.
            if (auto* nf = std::get_if<FunctionValue>(&fn.data); nf && nf->native)
                return nf->native(callArgs);
            // Named Kex function passed by reference (no native callback) —
            // call through the evaluator's normal dispatch.
            if (auto* nf = std::get_if<FunctionValue>(&fn.data))
                return callFunction(nf->name, std::move(callArgs), {}, {});
            return Value::none();
        });

        // Kex.Intrinsic.Fun.applyIndexed(f, item, i) — the applyItem of the
        // indexed HOFs (eachIndexed/mapIndexed). The index is always the LAST
        // argument, so a 3-parameter block over a Map entry reads
        // `|k, v, i|` while a 2-parameter one reads `|entry, i|`. A
        // 1-parameter block simply ignores the extra argument, exactly as the
        // lambda wrapper does. Mirrors kex_intrinsic_fun:applyIndexed/3.
        defineIntrinsic("Fun::applyIndexed", [this](std::vector<ValuePtr> args) -> ValuePtr {
            if (args.size() < 3) return Value::none();
            auto& fn = *args[0];
            std::vector<ValuePtr> callArgs;
            auto* tuple = std::get_if<TupleValue>(&args[1]->data);
            int arity = -1;
            if (auto* nf = std::get_if<FunctionValue>(&fn.data)) arity = nf->arity;
            // Spread a tuple of ANY arity, not just a Map entry's two: the
            // block's parameters minus the trailing index have to line up with
            // the tuple's elements. A RecordValue is a distinct type here, so
            // unlike BEAM there is nothing to exclude — this is the one place
            // the walker has it easier.
            const int spreadWidth = arity - 1;
            if (spreadWidth > 1 && tuple &&
                static_cast<int>(tuple->elements.size()) == spreadWidth) {
                callArgs = tuple->elements;
                callArgs.push_back(args[2]);
            } else {
                callArgs = {args[1], args[2]};
            }
            if (auto* nf = std::get_if<FunctionValue>(&fn.data); nf && nf->native)
                return nf->native(callArgs);
            if (auto* nf = std::get_if<FunctionValue>(&fn.data))
                return callFunction(nf->name, std::move(callArgs), {}, {});
            return Value::none();
        });
    }

}

auto Evaluator::pushEnv() -> void {
    m_env = std::make_shared<Environment>(m_env);
    m_importScopes.emplace_back();
    m_usingModules.emplace_back();
}

auto Evaluator::popEnv() -> void {
    if (m_env->parent()) {
        m_env = m_env->parent();
        m_importScopes.pop_back();
        m_usingModules.pop_back();
    }
}

auto Evaluator::evalRescue(const ast::RescueBlock& rescue, const ValuePtr& error,
                           SourceLocation loc) -> ValuePtr {
    if (rescue.isInlineReturn) {
        throw ReturnException(eval(*rescue.inlineReturnExpr));
    }

    if (rescue.isCatchAll) {
        pushEnv();
        if (!rescue.catchAllParam.empty()) {
            m_env->define(rescue.catchAllParam, error);
        }
        auto result = evalBody(rescue.catchAllBody);
        popEnv();
        return result;
    }

    pushEnv();
    for (const auto& clause : rescue.clauses) {
        bool matched = false;
        for (const auto& pattern : clause.patterns) {
            if (matchPattern(*pattern, error)) {
                matched = true;
                break;
            }
        }
        if (matched) {
            if (clause.guard) {
                auto guardVal = eval(**clause.guard);
                if (auto* b = std::get_if<BoolValue>(&guardVal->data); b && !b->value) {
                    continue;
                }
            }
            auto result = eval(*clause.body);
            popEnv();
            return result;
        }
    }
    popEnv();

    // No clause matched — re-throw
    throw TryException(error);
}

} // namespace kex::interpreter
