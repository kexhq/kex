#include "evaluator.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace kex::interpreter {

Evaluator::Evaluator() {
    m_globalEnv = std::make_shared<Environment>();
    m_intrinsicEnv = std::make_shared<Environment>();
    m_env = m_globalEnv;
    // Owns every process for this Evaluator's whole lifetime — there is
    // no "outside of a process" execution mode, matching BEAM, so this
    // always exists rather than being created lazily on first
    // spawn/receive use.
    m_scheduler = std::make_unique<Scheduler>(*this);
    registerBuiltins();
    // Clone all builtins into m_intrinsicEnv so the Kex.Intrinsic.*
    // dispatch path can look them up without hitting prelude wrappers
    // (which may later shadow them in m_globalEnv).
    m_intrinsicEnv->importAll(*m_globalEnv);
    // The Kex-written stdlib shadows the native builtins on every Evaluator, so
    // there is a single source of truth for stdlib behaviour regardless of entry
    // point (CLI, REPL, tests). No-op when KEX_PRELUDE_DIR is unset/unreadable.
    loadPrelude();
    // The prelude's type declarations (Optional, Result) re-register variant
    // constructors (Just, Ok, Error) via execTypeDef — but the generic
    // constructor loses typeParams/argParamIndex metadata that the native
    // factories (Value::just/ok/error) provide. Re-register the native
    // factories so they win and typeName() renders correctly
    // (e.g. "Result<String, ?>" not bare "Result").
    registerAdtConstructors();
}

auto Evaluator::execute(const ast::Program& program) -> ValuePtr {
    for (const auto& item : program.items) {
        execTopLevel(item);
    }

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
    if (m_testsPassed + m_testsFailed > 0) {
        std::string summary = "\n" + std::to_string(m_testsPassed) + " passed, "
            + std::to_string(m_testsFailed) + " failed\n";
        m_output += summary;
        std::cout << summary;
    }

    return lastResult;
}

auto Evaluator::loadPrelude() -> void {
#ifdef KEX_PRELUDE_DIR
    if (m_preludeLoaded) return;
    // Parse the prelude once and cache the merged declarations. The AST must
    // outlive every Evaluator (m_functionDefs keeps raw pointers into it), so
    // it is a function-local static — shared across all instances.
    static const ast::Program* preludeProgram = []() -> const ast::Program* {
        auto* prog = new ast::Program();
        std::error_code ec;
        std::vector<std::string> files;
        // KEX_PRELUDE_DIR is the native source path; on wasm the same files
        // are embedded into MEMFS at "/prelude" instead (see CMakeLists.txt's
        // --preload-file and src/common/prelude_loader.hxx, which shares this
        // convention for the REPL's SemanticDB).
        for (const char* dir : {KEX_PRELUDE_DIR, "/prelude"}) {
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                if (e.path().extension() == ".kex")
                    files.push_back(e.path().string());
            if (!ec && !files.empty()) break;
            files.clear();
        }
        if (files.empty()) return prog; // no prelude available — run without
        std::sort(files.begin(), files.end());
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
    // Collect sealed method names from make blocks and trait defs.
    for (const auto& item : preludeProgram->items) {
        auto addFn = [this](const ast::FunctionDef* fd) { if (fd) m_sealedMethods.insert(fd->name); };
        if (auto* md = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
            if (*md) for (const auto& bi : (*md)->body) {
                if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi))
                    addFn(fd->get());
                else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi))
                    if (*vb) for (const auto& vi : (*vb)->items)
                        if (auto* vf = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                            addFn(vf->get());
            }
        } else if (auto* td = std::get_if<std::unique_ptr<ast::TraitDef>>(&item)) {
            if (*td) for (const auto& bi : (*td)->body)
                if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi))
                    addFn(fd->get());
        }
    }
    for (const auto& item : preludeProgram->items) execTopLevel(item);
    m_preludeLoaded = true;
#endif
}

auto Evaluator::setReplMode(bool enabled) -> void {
    m_replMode = enabled;
}

auto Evaluator::setArgs(std::vector<std::string> args) -> void {
    m_scriptArgs = std::move(args);
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
        }
    }, item);
}

auto Evaluator::execModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                execModule(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                execTypeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                execRecordDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                execMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                execVisibilityBlock(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                execTraitDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
                execCompiledBlock(*node);
            }
        }, item);
    }
}

auto Evaluator::execTraitDef(const ast::TraitDef& def) -> void {
    // Register default method implementations from the trait body under
    // the trait name so `make X, implement: Trait` can inherit them.
    for (const auto& item : def.body) {
        if (auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            execFunctionDef(**fn, def.name);
        }
    }
}

auto Evaluator::execCompiledBlock(const ast::CompiledBlock& block) -> void {
    // Execute compiled block items as if they were regular module items.
    // The interpreter doesn't distinguish compile-time vs runtime evaluation;
    // function defs and make blocks are simply registered in the environment.
    for (const auto& item : block.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                execMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                execRecordDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                execTypeDef(*node);
            } else if constexpr (std::is_same_v<T, ast::ExprPtr>) {
                if (node) eval(*node);
            }
        }, item);
    }
}

auto Evaluator::execTypeDef(const ast::TypeDef& def) -> void {
    if (def.staticBlock) {
        // Static constructors/constants are namespaced under the type
        // (Vector2D.Polar(...), not bare Polar(...)) — see docs/functions.md
        // "Static Functions (Constructors)".
        for (const auto& func : def.staticBlock->functions) {
            execFunctionDef(*func, def.name);
        }
    }
    // Register sum-type variant constructors. Zero-arg variants (Fizz,
    // Nothing, ...) are stored directly as VariantValue in the environment.
    // With-arg constructors (Just(A), Ok(A), ...) are registered as
    // callable functions that build a VariantValue with a positional args
    // list. Both kinds get an entry in m_variantParent so `make TypeName
    // do ... end` method dispatch can map the variant tag back to the
    // declaring type.
    if (def.variants) {
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
                m_env->define(variantName, Value::variant(variantName, def.name));
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
            m_env->define(variantName, val);
        }
    }
}

auto Evaluator::execRecordDef(const ast::RecordDef& def) -> void {
    // Register record name as a namespace for static access (e.g. Vector2D.Polar)
    m_env->define(def.name, Value::record(def.name, {}));
    m_recordDefs[def.name] = &def;
    if (def.staticBlock) {
        // Static constructors/constants are namespaced under the record
        // (Vector2D.Polar(...), not bare Polar(...)) — see docs/functions.md
        // "Static Functions (Constructors)".
        for (const auto& func : def.staticBlock->functions) {
            execFunctionDef(*func, def.name);
        }
    }
}

auto Evaluator::execVisibilityBlock(const ast::VisibilityBlock& block, const std::string& typeScope) -> void {
    // Visibility (public/private) isn't enforced anywhere yet (see
    // semantic/analyzer.cxx's "TODO: handle visibility") — both kinds are
    // registered identically; only their accessibility differs, which
    // isn't checked at this stage.
    for (const auto& item : block.items) {
        std::visit([this, &typeScope](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node, typeScope);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                execMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                execTypeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                execRecordDef(*node);
            }
            // TypeAnnotation: semantic-only, nothing to execute.
        }, item);
    }
}

auto Evaluator::execFunctionDef(const ast::FunctionDef& def, const std::string& typeScope) -> void {
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
    // A name containing "::" was registered with a type scope → it's a UFCS
    // method and the first arg is always the receiver ("this").
    bool isMethod = regName.find("::") != std::string::npos;
    funcValue->data = FunctionValue{def.name, [this, regName, isMethod](std::vector<ValuePtr> args) -> ValuePtr {
        for (const auto* funcDef : m_functionDefs.at(regName)) {
            for (const auto& clause : funcDef->clauses) {
                pushEnv();
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
                bool firstParamIsThisPattern = !clause.params.empty()
                    && clause.params[0].pattern
                    && *clause.params[0].pattern
                    && std::holds_alternative<ast::ThisPattern>((*clause.params[0].pattern)->kind);

                // Count required params (those without defaults)
                size_t requiredParams = 0;
                for (const auto& p : clause.params) {
                    if (!p.defaultValue) requiredParams++;
                }

                size_t argOffset = 0;
                if (firstParamIsThisPattern) {
                    // @Pat: receiver is pattern-matched as first param; bind this for @field access
                    if (!args.empty()) m_env->define("this", args[0]);
                    argOffset = 0;
                } else if (isMethod && !args.empty()
                           && args.size() == requiredParams + 1) {
                    // Type-scoped method called with exactly required-params + 1 args:
                    // the extra arg is the receiver.
                    m_env->define("this", args[0]);
                    argOffset = 1;
                } else if (args.size() > clause.params.size()) {
                    // Legacy fallback: more args than declared params → first is receiver.
                    m_env->define("this", args[0]);
                    argOffset = 1;
                }

                for (size_t i = 0; i < clause.params.size(); i++) {
                    const auto& param = clause.params[i];
                    if ((i + argOffset) < args.size()) {
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
}

auto Evaluator::execMakeDef(const ast::MakeDef& def) -> void {
    // Extract type name from make target
    std::string typeName;
    if (def.target) {
        if (auto* named = std::get_if<ast::TypeName>(&def.target->kind)) {
            if (!named->parts.empty()) typeName = named->parts[0];
        } else if (auto* generic = std::get_if<ast::GenericType>(&def.target->kind)) {
            if (!generic->name.parts.empty()) typeName = generic->name.parts[0];
        } else if (std::holds_alternative<ast::ListType>(def.target->kind)) {
            typeName = "List";
        } else if (std::holds_alternative<ast::MapType>(def.target->kind)) {
            typeName = "Map";
        }
    }

    // Sealed-stdlib enforcement: after the prelude has loaded, reject any
    // user make-block on a builtin type that redefines a prelude method.
    if (m_preludeLoaded && !m_sealedMethods.empty()) {
        static const std::unordered_set<std::string> builtins = {
            "Integer", "Float", "Char", "Bool", "Number", "String",
            "List", "Map", "Range", "Optional", "Result"};
        if (builtins.count(typeName)) {
            auto checkSeal = [&](const ast::FunctionDef* fd) {
                if (fd && m_sealedMethods.count(fd->name))
                    throw RuntimeError(
                        "cannot override sealed stdlib method '" + fd->name +
                        "' on builtin type '" + typeName + "'", def.location);
            };
            for (const auto& bi : def.body) {
                if (auto* fd = std::get_if<std::unique_ptr<ast::FunctionDef>>(&bi))
                    checkSeal(fd->get());
                else if (auto* vb = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&bi))
                    if (*vb) for (const auto& vi : (*vb)->items)
                        if (auto* vf = std::get_if<std::unique_ptr<ast::FunctionDef>>(&vi))
                            checkSeal(vf->get());
            }
        }
    }

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

    // Process the make block's own methods.
    for (const auto& item : def.body) {
        std::visit([this, &typeName](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node, typeName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                execVisibilityBlock(*node, typeName);
            }
        }, item);
    }

    // Inherit default methods from implemented traits.
    for (const auto& traitName : def.implements) {
        std::string prefix = traitName + "::";
        for (const auto& [key, fns] : m_functionDefs) {
            if (key.rfind(prefix, 0) != 0) continue; // doesn't start with prefix
            for (const auto* traitFn : fns) {
                if (!traitFn) continue;
                if (traitFn->clauses.empty() || traitFn->clauses[0].body.empty()) continue;
                if (ownMethods.count({traitFn->name, arityOf(traitFn)})) continue;
                execFunctionDef(*traitFn, typeName);
            }
        }
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
            return Value::floating(std::stod(node.value));
        }
        else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            // Handle interpolation: find ${...} and evaluate in current scope
            std::string result;
            const auto& s = node.value;
            size_t i = 0;
            while (i < s.size()) {
                if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '{') {
                    i += 2;
                    std::string inner;
                    int depth = 1;
                    while (i < s.size() && depth > 0) {
                        if (s[i] == '{') depth++;
                        else if (s[i] == '}') { depth--; if (depth == 0) break; }
                        inner += s[i];
                        i++;
                    }
                    if (i < s.size()) i++; // skip }
                    // Parse and evaluate the interpolated expression.
                    // Only catch parse failures here — let runtime errors
                    // propagate so bugs in ${expr} aren't silently hidden.
                    ast::ExprPtr interpExpr;
                    try {
                        kex::Lexer interpLexer(inner);
                        auto interpTokens = interpLexer.tokenizeAll();
                        kex::Parser interpParser(std::move(interpTokens));
                        interpExpr = interpParser.parseExpr();
                    } catch (...) {
                        // Parse failed (e.g. a keyword inside ${}) — try
                        // treating the whole inner string as a variable name.
                        auto val = m_env->get(inner);
                        result += val ? val->toString() : inner;
                        continue;
                    }
                    if (interpExpr) {
                        auto val = eval(*interpExpr);
                        result += val->toString();
                    }
                } else {
                    result += s[i];
                    i++;
                }
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
                    for (const auto& field : recPat->fields) {
                        if (auto it = recVal->fields.find(field.name); it != recVal->fields.end()) {
                            if (field.pattern && *field.pattern) {
                                matchPattern(**field.pattern, it->second);
                            } else {
                                m_env->define(field.name, it->second);
                            }
                        }
                    }
                } else if (auto* mapVal = std::get_if<MapValue>(&value->data)) {
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
            m_env->set(node.name, value);
            return Value::none();
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
            if (node.op == TokenType::QuestionQuestion) {
                auto left = node.left ? eval(*node.left) : Value::none();
                if (left && !std::holds_alternative<NoneValue>(left->data)) return left;
                return node.right ? eval(*node.right) : Value::none();
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
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
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
                                    auto val = m_intrinsicEnv->get(node.method);
                                    if (!val)
                                        throw RuntimeError("Undefined intrinsic: " + node.method, expr.location);
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
            if (node.receiver) {
                if (auto* upperIdent = std::get_if<ast::UpperIdentifier>(&node.receiver->kind)) {
                    bool isKnownVariant = m_variantParent.count(upperIdent->name) > 0;
                    if (!isKnownVariant && !m_env->get(upperIdent->name)) {
                        isNamespaceCall = true;
                        namespaceName = upperIdent->name;
                    }
                }
            }

            auto receiver = (!isNamespaceCall && node.receiver) ? eval(*node.receiver) : Value::none();

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
                // Prefer the mangled "Namespace::method" name (e.g. "IO::putLine")
                // so namespaced builtins can't collide with unrelated plain-name
                // globals. Falls back to the plain name for namespaces that were
                // registered without a mangled prefix (e.g. Stream.Sequence).
                std::string dispatchName = node.method;
                auto mangled = namespaceName + "::" + node.method;
                if (m_env->get(mangled)) dispatchName = mangled;
                return callFunction(dispatchName, std::move(args), std::move(namedArgs), expr.location);
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

            // Type-based dispatch: try TypeName::method first
            std::string mangledName = node.method;
            std::string receiverType;
            if (auto* rec = std::get_if<RecordValue>(&receiver->data)) {
                receiverType = rec->typeName;
            } else if (auto* var = std::get_if<VariantValue>(&receiver->data)) {
                receiverType = var->tag;
            } else if (std::holds_alternative<ListValue>(receiver->data)) {
                receiverType = "List";
            } else if (std::holds_alternative<MapValue>(receiver->data)) {
                receiverType = "Map";
            } else if (std::holds_alternative<FileHandleValue>(receiver->data)) {
                receiverType = "FileHandle";
            } else if (std::holds_alternative<IntValue>(receiver->data) ||
                       std::holds_alternative<BigIntValue>(receiver->data)) {
                receiverType = "Integer";
            } else if (std::holds_alternative<FloatValue>(receiver->data)) {
                receiverType = "Float";
            } else if (std::holds_alternative<StringValue>(receiver->data)) {
                receiverType = "String";
            } else if (std::holds_alternative<RangeValue>(receiver->data)) {
                receiverType = "Range";
            } else if (std::holds_alternative<StreamValue>(receiver->data)) {
                receiverType = "Stream";
            }
            if (!receiverType.empty()) {
                auto typed = receiverType + "::" + node.method;
                if (m_env->get(typed)) {
                    mangledName = typed;
                } else {
                    // receiverType may be a variant tag (Just, Ok, Nothing,
                    // ...) rather than the type that declared it (Option,
                    // Result, ...) — methods from `make TypeName do ... end`
                    // are registered under the declared type's name, so
                    // fall back to that mapping before giving up.
                    auto parentIt = m_variantParent.find(receiverType);
                    if (parentIt != m_variantParent.end()) {
                        auto typedByParent = parentIt->second + "::" + node.method;
                        if (m_env->get(typedByParent)) {
                            mangledName = typedByParent;
                        }
                    }
                }
            }

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

            return callFunction(mangledName, std::move(args), std::move(namedArgs), expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            std::vector<ValuePtr> elements;
            for (const auto& elem : node.elements) {
                elements.push_back(elem ? eval(*elem) : Value::none());
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
            for (const auto& entry : node.entries) {
                auto key = entry.key ? eval(*entry.key) : Value::none();
                auto val = entry.value ? eval(*entry.value) : Value::none();
                entries.push_back({key, val});
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
                range->data = RangeValue{
                    static_cast<int64_t>(static_cast<unsigned char>(sc->value)),
                    static_cast<int64_t>(static_cast<unsigned char>(ec->value)),
                    true};
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
            return Value::none();
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

            lambda->data = FunctionValue{"<lambda>",
                [this, bodyPtr, paramNames, capturedEnv](std::vector<ValuePtr> args) -> ValuePtr {
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
                    } catch (ReturnException& ret) {
                        result = ret.value();
                    } catch (...) {
                        m_env = prevEnv;
                        throw;
                    }
                    m_env = prevEnv;
                    return result;
                }};
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
            std::unordered_map<std::string, ValuePtr> fields;
            for (const auto& [name, val] : node.fields) {
                fields[name] = val ? eval(*val) : Value::none();
            }
            // Apply declared field defaults (e.g. `pos : Int = 0`) for any
            // field this construction didn't specify explicitly.
            auto defIt = m_recordDefs.find(node.typeName);
            if (defIt != m_recordDefs.end()) {
                for (const auto& field : defIt->second->fields) {
                    if (fields.count(field.name)) continue;
                    if (field.defaultValue && *field.defaultValue) {
                        fields[field.name] = eval(**field.defaultValue);
                    }
                }
            }
            return Value::record(node.typeName, std::move(fields));
        }
        else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
            if (node.kind == ast::ShorthandLambda::Kind::Function) {
                // &function — look up the function and return it
                auto val = m_env->get(node.name);
                if (val) return val;
                throw RuntimeError("Undefined function: " + node.name, expr.location);
            }
            if (node.kind == ast::ShorthandLambda::Kind::Method) {
                // &.method — create a lambda that calls method on its arg
                auto method = node.name;
                auto lambda = std::make_shared<Value>();
                lambda->data = FunctionValue{"&." + method,
                    [this, method](std::vector<ValuePtr> args) -> ValuePtr {
                        if (args.empty()) return Value::none();
                        // Call method on the arg via UFCS
                        return callFunction(method, std::move(args), {}, {});
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
                        auto allArgs = args;
                        for (const auto& a : extraArgs) allArgs.push_back(a);
                        return callFunction(method, std::move(allArgs), {}, {});
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

            // Determine arity: operators are always 2; user functions from defs.
            int arity = -1;
            if (node.isOperator) {
                arity = 2;
            } else {
                auto it = m_functionDefs.find(fnName);
                if (it != m_functionDefs.end() && !it->second.empty()) {
                    const auto& firstDef = *it->second[0];
                    if (!firstDef.clauses.empty())
                        arity = static_cast<int>(firstDef.clauses[0].params.size());
                }
            }

            int boundCount = static_cast<int>(slots.size());
            int openCount = 0;
            for (const auto& s : slots) if (s.isOpen) openCount++;

            // Fully applied: no open slots and we have at least arity args.
            bool fullyApplied = (openCount == 0) &&
                                (arity >= 0 ? boundCount >= arity : boundCount > 0);

            if (fullyApplied) {
                std::vector<ValuePtr> args;
                for (const auto& s : slots) args.push_back(s.value);
                if (node.isOperator && args.size() >= 2) {
                    static const std::unordered_map<std::string, TokenType> opToks = {
                        {"+", TokenType::Plus}, {"-", TokenType::Minus},
                        {"*", TokenType::Star}, {"/", TokenType::Slash},
                        {"%", TokenType::Percent}, {"==", TokenType::EqEq},
                        {"!=", TokenType::NotEq}, {"<", TokenType::LessThan},
                        {"<=", TokenType::LessEq}, {">", TokenType::GreaterThan},
                        {">=", TokenType::GreaterEq},
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
                {"%", TokenType::Percent}, {"==", TokenType::EqEq},
                {"!=", TokenType::NotEq}, {"<", TokenType::LessThan},
                {"<=", TokenType::LessEq}, {">", TokenType::GreaterThan},
                {">=", TokenType::GreaterEq},
            };
            auto opIt = opTokens.find(fnName);
            bool isOp = node.isOperator && opIt != opTokens.end();
            TokenType opToken = isOp ? opIt->second : TokenType::Plus;

            // Partial: return a lambda that fills open slots (or appends) then calls.
            auto lambda = std::make_shared<Value>();
            lambda->data = FunctionValue{"~" + fnName,
                [this, fnName, slots, isOp, opToken](std::vector<ValuePtr> fillArgs) mutable -> ValuePtr {
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
            while (true) {
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
            if (val) return autoCallZeroArgConstant(node.name, val);
            throw RuntimeError("Undefined identifier: " + node.name, expr.location);
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
        else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
            // `using Module` inside a body: execute the module's compiled
            // block functions in the current scope so their names become
            // available as plain identifiers (e.g. `html`, `body` after
            // `using Html.Language`). Currently a no-op because compiled
            // block functions are already registered globally by execModule.
            if (!node.body.empty()) {
                for (const auto& e : node.body) {
                    if (e) eval(*e);
                }
            }
            return Value::unit();
        }
        else {
            return Value::none();
        }
    }, expr.kind);
}

auto Evaluator::evalBinaryOp(TokenType op, const ValuePtr& left, const ValuePtr& right,
                             SourceLocation loc) -> ValuePtr {
    // Operator overloading: `make Type do let +(other) -> Type ... end`
    // registers "Type::+", dispatched here the same way method calls
    // dispatch on receiver type, before any built-in handling.
    if (auto* rec = std::get_if<RecordValue>(&left->data)) {
        std::string opSymbol;
        switch (op) {
            case TokenType::Plus:       opSymbol = "+";  break;
            case TokenType::Minus:      opSymbol = "-";  break;
            case TokenType::Star:       opSymbol = "*";  break;
            case TokenType::Slash:      opSymbol = "/";  break;
            case TokenType::Percent:    opSymbol = "%";  break;
            case TokenType::EqEq:       opSymbol = "=="; break;
            case TokenType::NotEq:      opSymbol = "!="; break;
            case TokenType::LessThan:   opSymbol = "<";  break;
            case TokenType::GreaterThan: opSymbol = ">"; break;
            case TokenType::LessEq:     opSymbol = "<="; break;
            case TokenType::GreaterEq:  opSymbol = ">="; break;
            default: break;
        }
        if (!opSymbol.empty()) {
            auto mangled = rec->typeName + "::" + opSymbol;
            if (m_env->get(mangled)) {
                return callFunction(mangled, {left, right}, {}, loc);
            }
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

    switch (op) {
        case TokenType::Plus:
            if (li && ri) {
                int64_t result;
                if (!__builtin_add_overflow(li->value, ri->value, &result)) return Value::integer(result);
            }
            if (leftInt && rightInt) return integerResult(*leftInt + *rightInt);
            if (lf && rf) return Value::floating(lf->value + rf->value);
            if (leftInt && rf) return Value::floating(intToDouble(li, *leftInt) + rf->value);
            if (lf && rightInt) return Value::floating(lf->value + intToDouble(ri, *rightInt));
            // String/Char/[Char] concatenate as text — e.g. 'a' + 'b' ==
            // "ab", "ab" + 'c' == "abc". This is broader than the Char/
            // String *equality* rule (Char isn't a String for ==) — here
            // we just want "what text does this contribute", which a bare
            // Char answers fine; see textContent vs. stringOrCharListText
            // in value.cxx.
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
            if (lf && rf) return Value::floating(lf->value - rf->value);
            if (leftInt && rf) return Value::floating(intToDouble(li, *leftInt) - rf->value);
            if (lf && rightInt) return Value::floating(lf->value - intToDouble(ri, *rightInt));
            throw RuntimeError("Cannot subtract " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Star:
            if (li && ri) {
                int64_t result;
                if (!__builtin_mul_overflow(li->value, ri->value, &result)) return Value::integer(result);
            }
            if (leftInt && rightInt) return integerResult(*leftInt * *rightInt);
            if (lf && rf) return Value::floating(lf->value * rf->value);
            if (leftInt && rf) return Value::floating(intToDouble(li, *leftInt) * rf->value);
            if (lf && rightInt) return Value::floating(lf->value * intToDouble(ri, *rightInt));
            throw RuntimeError("Cannot multiply " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Slash:
            if (rightInt && *rightInt == 0) throw RuntimeError("Division by zero", loc);
            if (rf && rf->value == 0.0) throw RuntimeError("Division by zero", loc);
            if (li && ri) return Value::integer(li->value / ri->value);
            if (leftInt && rightInt) return integerResult(*leftInt / *rightInt);
            if (lf && rf) return Value::floating(lf->value / rf->value);
            if (leftInt && rf) return Value::floating(intToDouble(li, *leftInt) / rf->value);
            if (lf && rightInt) return Value::floating(lf->value / intToDouble(ri, *rightInt));
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

        case TokenType::EqEq: return Value::boolean(valuesEqual(left, right));
        case TokenType::NotEq: return Value::boolean(!valuesEqual(left, right));

        case TokenType::LessThan:
            if (li && ri) return Value::boolean(li->value < ri->value);
            if (leftInt && rightInt) return Value::boolean(*leftInt < *rightInt);
            if (lf && rf) return Value::boolean(lf->value < rf->value);
            if (ls && rs) return Value::boolean(ls->value < rs->value);
            if (lc && rc) return Value::boolean(lc->value < rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::GreaterThan:
            if (li && ri) return Value::boolean(li->value > ri->value);
            if (leftInt && rightInt) return Value::boolean(*leftInt > *rightInt);
            if (lf && rf) return Value::boolean(lf->value > rf->value);
            if (ls && rs) return Value::boolean(ls->value > rs->value);
            if (lc && rc) return Value::boolean(lc->value > rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::LessEq:
            if (li && ri) return Value::boolean(li->value <= ri->value);
            if (leftInt && rightInt) return Value::boolean(*leftInt <= *rightInt);
            if (lf && rf) return Value::boolean(lf->value <= rf->value);
            if (ls && rs) return Value::boolean(ls->value <= rs->value);
            if (lc && rc) return Value::boolean(lc->value <= rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::GreaterEq:
            if (li && ri) return Value::boolean(li->value >= ri->value);
            if (leftInt && rightInt) return Value::boolean(*leftInt >= *rightInt);
            if (lf && rf) return Value::boolean(lf->value >= rf->value);
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

auto Evaluator::callFunction(const std::string& name, std::vector<ValuePtr> args,
                             NamedArgs namedArgs, SourceLocation loc) -> ValuePtr {
    // BEAM-style reduction-counted auto-yield: placed at function-call
    // boundaries, the same kind of safe point BEAM itself uses, so a compute-bound process that never calls
    // `receive` still gives other processes a turn periodically.
    m_scheduler->tickReduction();

    auto val = m_env->get(name);
    if (!val) val = m_intrinsicEnv->get(name);
    if (!val) {
        throw RuntimeError("Undefined function: " + name, loc);
    }

    if (auto* func = std::get_if<FunctionValue>(&val->data)) {
        if (func->native) {
            // Reorder: place named args into correct positions based on param names
            if (!namedArgs.empty()) {
                auto it = m_functionDefs.find(name);
                if (it != m_functionDefs.end() && !it->second.empty()) {
                    const auto& firstClause = it->second[0]->clauses[0];
                    // Build full arg list: place named args by matching
                    // param names first, then fill whatever slots remain
                    // (in order) from the positional args. Named-first
                    // matters because `args` may include a trailing
                    // do-block appended as an extra positional value (see
                    // "Handle block as last arg" above) — its destination
                    // param is often last, not at the front, so it must
                    // land in whichever slot is actually still open rather
                    // than wherever index 0 happens to be.
                    size_t totalParams = firstClause.params.size();
                    std::vector<ValuePtr> fullArgs(totalParams, nullptr);

                    for (auto& [argName, argVal] : namedArgs) {
                        for (size_t i = 0; i < firstClause.params.size(); i++) {
                            if (firstClause.params[i].name.has_value() &&
                                *firstClause.params[i].name == argName) {
                                fullArgs[i] = std::move(argVal);
                                break;
                            }
                        }
                    }

                    size_t nextSlot = 0;
                    for (auto& a : args) {
                        while (nextSlot < totalParams && fullArgs[nextSlot]) nextSlot++;
                        if (nextSlot >= totalParams) break;
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

auto Evaluator::autoCallZeroArgConstant(const std::string& name, const ValuePtr& val) -> ValuePtr {
    // Only acts on user-defined Kex functions (tracked in m_functionDefs);
    // native builtins, namespace placeholders (RecordValue), and ADT
    // constructors (registered directly via m_env->define, never through
    // execFunctionDef) are absent from m_functionDefs and pass through
    // unchanged here.
    auto* func = std::get_if<FunctionValue>(&val->data);
    if (!func || !func->native) return val;

    auto defIt = m_functionDefs.find(name);
    if (defIt == m_functionDefs.end() || defIt->second.empty()) return val;
    if (!defIt->second[0]->clauses[0].params.empty()) return val;

    auto savedEnv = m_env;
    try {
        auto result = func->native({});
        if (result && !std::holds_alternative<NoneValue>(result->data)) {
            return result;
        }
    } catch (...) {
        m_env = savedEnv;
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
                auto valueInt = asInteger(value);
                return valueInt && *valueInt == mpz_class(pat.literal.value);
            }
            if (pat.literal.type == TokenType::String) {
                auto* sv = std::get_if<StringValue>(&value->data);
                return sv && sv->value == pat.literal.value;
            }
            if (pat.literal.type == TokenType::Char) {
                // Char is its own type, not a 1-character String — a
                // char-literal pattern only matches a Char value.
                auto* cv = std::get_if<CharValue>(&value->data);
                return cv && cv->value == (pat.literal.value.empty() ? '\0' : pat.literal.value[0]);
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
                return std::holds_alternative<NoneValue>(value->data);
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
            auto startVal = rv->isChar ? Value::character(static_cast<char>(rv->start)) : Value::integer(rv->start);
            auto endVal = rv->isChar ? Value::character(static_cast<char>(rv->end)) : Value::integer(rv->end);
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
                chars.reserve(sv->value.size());
                for (unsigned char c : sv->value)
                    chars.push_back(Value::character(static_cast<char>(c)));
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
                            tail += cv->value;
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
            if (pat.name == "None") return std::holds_alternative<NoneValue>(value->data);

            if (pat.args.empty()) {
                // Match zero-arg variant constructors (Nothing, Less, Fizz, ...)
                if (auto* var = std::get_if<VariantValue>(&value->data)) {
                    if (var->tag == pat.name && var->args.empty()) return true;
                }

                // Match type names as type patterns (for runtime type checking).
                // These `if (holds_alternative) return true;` rather than
                // `return holds_alternative(...)` so that e.g. a bare `Integer`
                // expression — which resolves to the Integer::parse namespace
                // RecordValue, not an IntValue — still falls through to the
                // record-typeName check below instead of failing outright.
                if (pat.name == "String" && std::holds_alternative<StringValue>(value->data)) return true;
                if ((pat.name == "Int" || pat.name == "Integer") &&
                    (std::holds_alternative<IntValue>(value->data) ||
                     std::holds_alternative<BigIntValue>(value->data))) return true;
                if (pat.name == "Float" && std::holds_alternative<FloatValue>(value->data)) return true;
                if (pat.name == "Bool" && std::holds_alternative<BoolValue>(value->data)) return true;
                if (pat.name == "Atom" && std::holds_alternative<AtomValue>(value->data)) return true;
                if (pat.name == "List" && std::holds_alternative<ListValue>(value->data)) return true;
                if (pat.name == "Map" && std::holds_alternative<MapValue>(value->data)) return true;
                if (pat.name == "Tuple" && std::holds_alternative<TupleValue>(value->data)) return true;
                if (pat.name == "Range" && std::holds_alternative<RangeValue>(value->data)) return true;
                if (pat.name == "Stream" && std::holds_alternative<StreamValue>(value->data)) return true;
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
    // src/interpreter/stdlib/. Order matters: registerStreamBuiltins()
    // wraps the plain-list `take` registered by registerListBuiltins().
    registerAdtConstructors();
    registerIOBuiltins();
    registerFileBuiltins();
    registerDirectoryBuiltins();
    registerMockBuiltins();
    registerListBuiltins();
    registerStringBuiltins();
    registerNumberBuiltins();
    registerStreamBuiltins();
    registerMapBuiltins();
    registerEnvBuiltins();
    registerMathBuiltins();
    registerTestBuiltins();
    registerProcessBuiltins();

    // Kex.Intrinsic.Fun.applyItem(f, item) — auto-splat a pair into a
    // two-arg block. Backs all Enumerable HOFs (map/filter/each/etc.)
    // for Map enumeration where each item is a (K,V) tuple.
    {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{"applyItem", [this](std::vector<ValuePtr> args) -> ValuePtr {
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
        }};
        m_globalEnv->define("applyItem", val);
    }
}

auto Evaluator::pushEnv() -> void {
    m_env = std::make_shared<Environment>(m_env);
}

auto Evaluator::popEnv() -> void {
    if (m_env->parent()) {
        m_env = m_env->parent();
    }
}

} // namespace kex::interpreter
