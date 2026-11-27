#include "analyzer.hxx"
#include "../common/name_case.hxx"
#include "../ast/walk.hxx"
#include "module_rules.hxx"
#include "receiver_conflicts.hxx"
#include "resolve_core.hxx"
#include "../lexer/lexer.hxx"

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <set>

namespace kex::semantic {

namespace {

// The module path a qualified call names, or "" when the receiver is a value
// rather than a module path: `IO.printLine` -> "IO", `FS.File.read` ->
// "FS.File". Nested modules carry their parent in the name (parseModuleDef
// builds `parent + "." + name`), so the dotted path is what both the local
// collection pass and imported interfaces are keyed by.
auto receiverModuleName(const ast::Expr& receiver) -> std::string {
    if (auto* lower = std::get_if<ast::Identifier>(&receiver.kind)) {
        return lower->name;
    }
    if (auto* upper = std::get_if<ast::UpperIdentifier>(&receiver.kind)) {
        return upper->name;
    }
    // `FS.File` parses as an argument-less method call on `FS`. Treat it as a
    // module path only when the segment is itself capitalized, so a field read
    // such as `config.io` never looks like one.
    if (auto* call = std::get_if<ast::MethodCall>(&receiver.kind)) {
        if (call->args.empty() && call->namedArgs.empty() && !call->block &&
            !call->method.empty() && kex::startsUppercase(call->method) && call->receiver) {
            auto parent = receiverModuleName(*call->receiver);
            if (!parent.empty()) {
                return parent + "." + call->method;
            }
        }
    }
    return {};
}

} // namespace

auto Analyzer::analyze(const ast::Program& program) -> bool {
    m_analyzed = true;
    // A file with nothing in it is a valid program that does nothing, but
    // seldom the one meant. Blank and still without items: a spec base or the
    // project's modules merged in give a blank file code to run.
    if (program.blank && program.items.empty() && !m_entryFiles.empty()) {
        m_diagnostics.push_back(makeDiagnostic(
            Diagnostic::Level::Warning, diag::Code::EmptyProgram,
            SourceLocation{internFilename(m_entryFiles.front()), 1, 1}));
    }
    // Which names one module may take from another (`using`, `export`).
    for (auto& violation : findModuleRuleViolations(
             program, m_importedInterfaces, m_moduleLocator ? &m_moduleLocator : nullptr,
             m_entryFiles, m_missingModuleIsError)) {
        m_diagnostics.push_back(std::move(violation));
    }

    // Type checking: the checker decides what every name and call means.
    const auto checkedFrom = m_diagnostics.size();
    m_checker.check(program, m_diagnostics, m_entryFiles);
    if (m_indexesNames) {
        m_nameIndex = buildNameIndex(
            program, m_entryFiles.empty() ? std::string{} : m_entryFiles.front(), m_checker,
            m_importedInterfaces);
    }
    // Two definitions of one method for one receiver type, the receivers as
    // the checker resolved them. Reported ahead of the type errors so the
    // message is not buried under those an ambiguous method tends to cause.
    auto conflicts = findReceiverConflicts(program, m_checker);
    m_diagnostics.insert(
        m_diagnostics.begin() + static_cast<std::ptrdiff_t>(checkedFrom),
        std::make_move_iterator(conflicts.begin()), std::make_move_iterator(conflicts.end()));

    // Purity, over the functions the checker says each call runs. Reported
    // ahead of the checker's diagnostics: an effect in the wrong place is
    // usually the first thing to fix.
    std::vector<Diagnostic> checked(
        std::make_move_iterator(m_diagnostics.begin() + static_cast<std::ptrdiff_t>(checkedFrom)),
        std::make_move_iterator(m_diagnostics.end()));
    m_diagnostics.resize(checkedFrom);
    // Per-module member effects, so a qualified call can be checked against
    // a module declared anywhere in the unit, including below it.
    collectModuleMemberEffects(program);
    m_usings.clear();
    for (const auto& item : program.items) {
        if (const auto* using_ = std::get_if<std::unique_ptr<ast::UsingBlock>>(&item)) {
            if (*using_) {
                m_usings.push_back(using_->get());
            }
        }
    }
    reportDuplicateRecords(program.items);
    for (const auto& item : program.items) {
        analyzeItem(item);
    }
    // Foulness through the call graph, so a guard rejects a transitively
    // foul call.
    computeTransitiveEffects(program);
    computeCapabilityRequirements(program);
    checkEffectFlow(program);
    m_diagnostics.insert(
        m_diagnostics.end(), std::make_move_iterator(checked.begin()),
        std::make_move_iterator(checked.end()));

    // Foul calls through a value receiver or a trait, which only the
    // checker's resolved targets show.
    enrichEffectsFromResolvedCalls(program);
    recordPureBlocks(program);

    return std::none_of(m_diagnostics.begin(), m_diagnostics.end(), [](const Diagnostic& d) {
        return d.level == Diagnostic::Level::Error;
    });
}

auto Analyzer::diagnostics() const -> const std::vector<Diagnostic>& {
    return m_diagnostics;
}

auto Analyzer::displayTypeOf(const ast::Expr* expr) const -> TypePtr {
    return m_checker.displayTypeOf(expr);
}

auto Analyzer::typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>& {
    return m_checker.typeMap();
}

auto Analyzer::assignmentTargetTypeOf(const ast::Expr* expr) const -> TypePtr {
    return m_checker.assignmentTargetTypeOf(expr);
}

auto Analyzer::functionSignatures(const ast::FunctionDef* function) const
    -> const std::vector<Signature>* {
    return m_checker.functionSignatures(function);
}

auto Analyzer::resolvedCalls() const
    -> const std::unordered_map<const ast::MethodCall*, ResolvedCallTarget>& {
    return m_checker.resolvedCalls();
}

auto Analyzer::callsFoul(const ast::Expr& call) const -> bool {
    const auto& selected = m_checker.selectedCallSignatures();
    if (const auto found = selected.find(&call); found != selected.end() && found->second.isFoul) {
        return true;
    }
    // A module's function, by its module's record of its effects.
    std::string named;
    if (const auto found = m_checker.calledFunctions().find(&call);
        found != m_checker.calledFunctions().end()) {
        named = found->second;
    } else if (const auto constant = m_checker.referencedConstants().find(&call);
               constant != m_checker.referencedConstants().end()) {
        named = constant->second;
    }
    const auto dot = named.rfind('.');
    return dot != std::string::npos &&
           isQualifiedCallFoul(named.substr(0, dot), named.substr(dot + 1));
}

template <typename Items>
auto Analyzer::reportDuplicateRecords(const Items& items) -> void {
    // A type may be reopened; a record may not, nor share a type's name.
    std::set<std::string> declared;
    auto declare = [&](const auto& item) {
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if (!node) {
                    return;
                }
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                    declared.insert(node->name);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                    if (!declared.insert(node->name).second) {
                        error(diag::Code::DuplicateRecord, node->location, {{"name", node->name}});
                    }
                }
            },
            item);
    };
    for (const auto& item : items) {
        declare(item);
        std::visit(
            [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (node) {
                        for (const auto& inner : node->items) {
                            declare(inner);
                        }
                    }
                }
            },
            item);
    }
}

template <typename Item>
auto Analyzer::analyzeItem(const Item& item) -> void {
    std::visit(
        [this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if (!node) {
                return;
            }
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                analyzeModule(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                // A `make` block's `private do` members are not walked (yet).
                for (const auto& member : node->body) {
                    if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&member)) {
                        if (*fn) {
                            analyzeFunctionDef(**fn);
                        }
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                analyzeFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                // A main block may do anything. A top-level `let` (the
                // parser's synthetic wrapper) is a `let` like any other: pure.
                // Letting it be foul laundered effects into pure functions
                // that read it, and BEAM re-ran them at every read.
                const auto outer = std::exchange(m_inFoulContext, !node->synthetic);
                analyzeBody(node->body);
                m_inFoulContext = outer;
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                // Visibility decides who may CALL a function, not whether its
                // body is checked: a `private do` member is purity-checked too.
                for (const auto& inner : node->items) {
                    analyzeItem(inner);
                }
            }
        },
        item);
}

auto Analyzer::analyzeModule(const ast::ModuleDef& mod) -> void {
    // A module never carries an effect of its own: entering one leaves the
    // surrounding purity exactly as it was, and each member declares its own.
    // Its own `using`s apply inside it, and only there.
    const auto outerUsings = m_usings.size();
    for (const auto& item : mod.body) {
        if (const auto* using_ = std::get_if<std::unique_ptr<ast::UsingBlock>>(&item)) {
            if (*using_) {
                m_usings.push_back(using_->get());
            }
        }
    }
    reportDuplicateRecords(mod.body);
    for (const auto& item : mod.body) {
        analyzeItem(item);
    }
    m_usings.resize(outerUsings);
}

auto Analyzer::analyzeFunctionDef(const ast::FunctionDef& def) -> void {
    const auto outer = m_inFoulContext;
    if (def.isFoul) {
        m_inFoulContext = true;
    }
    for (const auto& clause : def.clauses) {
        analyzeBody(clause.body);
    }
    m_inFoulContext = outer;
}

auto Analyzer::analyzeBody(const std::vector<ast::ExprPtr>& body) -> void {
    for (const auto& expr : body) {
        if (expr) {
            analyzeExpr(*expr);
        }
    }
}

auto Analyzer::analyzeExpr(const ast::Expr& expr) -> void {
    auto foulCall = [&](diag::Code code, const std::string& name) {
        if (!m_inFoulContext) {
            error(code, expr.location, {{"name", name}});
        }
    };
    if (const auto* ident = std::get_if<ast::Identifier>(&expr.kind)) {
        // A zero-parameter function named bare is called.
        if (callsFoul(expr)) {
            foulCall(diag::Code::FoulCallFromPure, ident->name);
        }
    } else if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind)) {
        if (callsFoul(expr)) {
            foulCall(diag::Code::FoulCallFromPure, call->name);
        }
    } else if (const auto* tagged = std::get_if<ast::TaggedLiteral>(&expr.kind)) {
        if (callsFoul(expr)) {
            foulCall(diag::Code::FoulTagFromPure, tagged->tag);
        }
    } else if (const auto* method = std::get_if<ast::MethodCall>(&expr.kind)) {
        // `IO.inspect` is the documented purity-exempt escape hatch.
        const auto module =
            method->receiver ? receiverModuleName(*method->receiver) : std::string{};
        if (method->method != "inspect" && !module.empty() &&
            isQualifiedCallFoul(module, method->method)) {
            foulCall(diag::Code::FoulCallFromPure, module + "." + method->method);
        }
    } else if (const auto* capture = std::get_if<ast::CurryExpr>(&expr.kind)) {
        // `~shout` names a foul function as a call of it would: pure code may
        // hand it only where a foul function is expected (a route's
        // `Handler`, stored and run by the server), not to `map`, which runs
        // it there and then (#640).
        if (const auto captured = m_checker.captureSignatures().find(&expr);
            captured != m_checker.captureSignatures().end() && captured->second.isFoul &&
            !m_inFoulContext && !m_checker.capturesIntoFoul().contains(&expr)) {
            error(
                diag::Code::FoulCaptureFromPure, expr.location,
                {{"name", capture->module.empty() ? capture->name
                                                  : capture->module + "." + capture->name}});
        }
    } else if (
        std::holds_alternative<ast::SpawnExpr>(expr.kind) ||
        std::holds_alternative<ast::ReceiveExpr>(expr.kind)) {
        if (!m_inFoulContext) {
            error(
                diag::Code::FoulKeywordInPure, expr.location,
                {{"keyword",
                  std::holds_alternative<ast::SpawnExpr>(expr.kind) ? "spawn" : "receive"}});
        }
    } else if (std::holds_alternative<ast::BreakExpr>(expr.kind)) {
        checkLoopControl(expr.location, "break");
    } else if (std::holds_alternative<ast::NextExpr>(expr.kind)) {
        checkLoopControl(expr.location, "next");
    }

    // `break` and `next` bind to the nearest loop, and not across a block.
    const bool loop = std::holds_alternative<ast::LoopExpr>(expr.kind) ||
                      std::holds_alternative<ast::WhileExpr>(expr.kind);
    const bool block = std::holds_alternative<ast::Lambda>(expr.kind);
    if (loop || block) {
        m_loopScopes.push_back(loop ? LoopScope::Loop : LoopScope::Closure);
    }
    ast::forEachChild(expr, [&](const ast::ExprPtr& child) { analyzeExpr(*child); });
    if (loop || block) {
        m_loopScopes.pop_back();
    }
}

auto Analyzer::collectModuleMemberEffects(const ast::Program& program) -> void {
    m_localModuleFoulMembers.clear();
    m_topLevelFoul.clear();

    std::function<void(const ast::ModuleDef&)> collect = [&](const ast::ModuleDef& mod) {
        auto& foulMembers = m_localModuleFoulMembers[mod.name];
        for (const auto& item : mod.body) {
            if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn && (*fn)->isFoul) {
                    foulMembers.insert((*fn)->name);
                }
            } else if (const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
                // `foul name : T` declares a foul value binding; the `let`
                // clause below it carries no marker of its own.
                if (*ann && (*ann)->isFoul) {
                    foulMembers.insert((*ann)->name);
                }
            } else if (const auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*nested) {
                    collect(**nested);
                }
            } else if (
                const auto* visibility =
                    std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
                // A `private do` member is still a member: its effect has to
                // be registered, or a caller inside the same module sees it
                // as pure.
                if (*visibility) {
                    for (const auto& inner : (*visibility)->items) {
                        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner);
                            fn && *fn && (*fn)->isFoul) {
                            foulMembers.insert((*fn)->name);
                        }
                    }
                }
            }
        }
    };

    for (const auto& item : program.items) {
        if (const auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
            if (*mod) {
                collect(**mod);
            }
        } else if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fn && (*fn)->isFoul) {
                m_topLevelFoul.insert((*fn)->name);
            }
        }
    }
}

auto Analyzer::isQualifiedCallFoul(const std::string& module, const std::string& member) const
    -> bool {
    auto known = [&](const std::string& name) {
        if (auto local = m_localModuleFoulMembers.find(name);
            local != m_localModuleFoulMembers.end() && local->second.count(member)) {
            return true;
        }
        return m_importedInterfaces && isExportFoul(*m_importedInterfaces, name, member);
    };
    // Only the receiver exactly as written is looked up. Resolving a
    // shortened path (`File.read` under `using FS`) would mean matching any
    // module whose name ends in that segment, and `Mock.System.OS` would then
    // make every `System.OS` read foul. Missing a rejection is recoverable;
    // rejecting pure code because an unrelated module shares a segment is not.
    if (known(module)) {
        return true;
    }
    // An alias is not a guess: `using Store.Repo, as: Repo` names exactly
    // one module, and `Repo.fetch` is its `fetch`. Read as written, every
    // foul call through an alias passed as pure (kexhq/kex#584).
    const auto dot = module.find('.');
    const auto head = module.substr(0, dot);
    for (auto it = m_usings.rbegin(); it != m_usings.rend(); ++it) {
        const auto& using_ = **it;
        if (!using_.alias || *using_.alias != head) {
            continue;
        }
        std::string target = using_.module.dotted();
        if (dot != std::string::npos) {
            target += module.substr(dot);
        }
        return known(target);
    }
    return false;
}

auto Analyzer::computeTransitiveEffects(const ast::Program& program) -> void {
    // Build a call graph over top-level and module functions.
    // A function is "directly foul" if:
    //   - its AST isFoul flag is set, or
    //   - it contains spawn/receive, or
    //   - it calls a method on a foul imported module, or
    //   - it calls a symbol known to be foul.
    // Propagation: any function calling a (directly or transitively) foul
    // function is itself transitively foul.

    std::unordered_set<std::string> allFns;
    std::unordered_set<std::string> directlyFoul;
    // The call graph is keyed by bare name, so two same-named functions in
    // different modules share an entry. Foulness only ever widens, so that is
    // safe there; a capability requirement would be REPORTED against a
    // function that does not have it, so an ambiguous name is dropped rather
    // than guessed at (kexhq/kex#143).
    std::unordered_map<std::string, int> definitionCount;
    m_callGraph.clear();

    // Which modules are capabilities has to be known before the walk, so a
    // call into one can be recorded as a requirement (kexhq/kex#143).
    m_capabilityModules.clear();

    {
        std::function<void(const ast::ModuleDef&, const std::string&)> collectCapabilities =
            [&](const ast::ModuleDef& module, const std::string& parent) {
                const auto qualified = ast::nestedModulePath(parent, module.name);
                if (module.isCapability) {
                    m_capabilityModules.insert(qualified);
                }
                for (const auto& mi : module.body) {
                    std::visit(
                        [&](const auto& mn) {
                            using MT = std::decay_t<decltype(mn)>;
                            if constexpr (std::is_same_v<MT, std::unique_ptr<ast::ModuleDef>>) {
                                if (mn) {
                                    collectCapabilities(*mn, qualified);
                                }
                            }
                        },
                        mi);
                }
            };
        for (const auto& item : program.items) {
            std::visit(
                [&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        if (n) {
                            collectCapabilities(*n, "");
                        }
                    }
                },
                item);
        }
    }

    // Walk an expression tree, collecting called function names and detecting
    // direct foul operations (spawn, receive, foul module method calls).
    std::function<void(const ast::Expr&, std::set<std::string>&, bool&)> walkExpr =
        [&](const ast::Expr& e, std::set<std::string>& calls, bool& hasFoulOp) {
            if (const auto* call = std::get_if<ast::FunctionCall>(&e.kind)) {
                if (allFns.count(call->name)) {
                    calls.insert(call->name);
                }
                if (callsFoul(e)) {
                    hasFoulOp = true;
                }
            } else if (const auto* tagged = std::get_if<ast::TaggedLiteral>(&e.kind)) {
                if (allFns.count(tagged->tag)) {
                    calls.insert(tagged->tag);
                }
                if (callsFoul(e)) {
                    hasFoulOp = true;
                }
            } else if (const auto* method = std::get_if<ast::MethodCall>(&e.kind)) {
                if (allFns.count(method->method)) {
                    calls.insert(method->method);
                }
                if (method->receiver) {
                    auto receiverName = receiverModuleName(*method->receiver);
                    if (!receiverName.empty() &&
                        isQualifiedCallFoul(receiverName, method->method)) {
                        hasFoulOp = true;
                    }
                }
            } else if (
                std::holds_alternative<ast::SpawnExpr>(e.kind) ||
                std::holds_alternative<ast::ReceiveExpr>(e.kind)) {
                hasFoulOp = true;
            }
            ast::forEachChild(
                e, [&](const ast::ExprPtr& child) { walkExpr(*child, calls, hasFoulOp); });
        };

    // Pass 1: collect all function names.
    auto collectFns = [&](const auto& items) {
        for (const auto& item : items) {
            std::visit(
                [&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        if (n) {
                            allFns.insert(n->name);
                            definitionCount[n->name]++;
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        if (n) {
                            for (const auto& mi : n->body) {
                                std::visit(
                                    [&](const auto& mn) {
                                        using MT = std::decay_t<decltype(mn)>;
                                        if constexpr (std::is_same_v<
                                                          MT, std::unique_ptr<ast::FunctionDef>>) {
                                            if (mn) {
                                                allFns.insert(mn->name);
                                                definitionCount[mn->name]++;
                                            }
                                        }
                                    },
                                    mi);
                            }
                        }
                    }
                },
                item);
        }
    };
    collectFns(program.items);

    // Pass 2: build call graph and seed directly foul.
    auto processFn = [&](const ast::FunctionDef& def) {
        std::set<std::string> calls;
        bool hasFoulOp = def.isFoul;
        for (const auto& clause : def.clauses) {
            for (const auto& ex : clause.body) {
                if (ex) {
                    walkExpr(*ex, calls, hasFoulOp);
                }
            }
        }
        calls.erase(def.name);
        m_callGraph[def.name] = std::move(calls);
        if (hasFoulOp) {
            directlyFoul.insert(def.name);
        }
    };

    for (const auto& item : program.items) {
        std::visit(
            [&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n) {
                        processFn(*n);
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (n) {
                        for (const auto& mi : n->body) {
                            std::visit(
                                [&](const auto& mn) {
                                    using MT = std::decay_t<decltype(mn)>;
                                    if constexpr (std::is_same_v<
                                                      MT, std::unique_ptr<ast::FunctionDef>>) {
                                        if (mn) {
                                            processFn(*mn);
                                        }
                                    }
                                },
                                mi);
                        }
                    }
                }
            },
            item);
    }

    // Propagate foulness through the call graph (fixpoint).
    m_transitivelyFoul = directlyFoul;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [fn, callees] : m_callGraph) {
            if (m_transitivelyFoul.count(fn)) {
                continue;
            }
            for (const auto& callee : callees) {
                if (m_transitivelyFoul.count(callee)) {
                    m_transitivelyFoul.insert(fn);
                    changed = true;
                    break;
                }
            }
        }
    }

    // Re-walk guard expressions to reject foul calls, direct and transitive.
    std::function<void(const ast::Expr&)> checkGuard = [&](const ast::Expr& e) {
        std::visit(
            [&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                    if (callsFoul(e)) {
                        error(diag::Code::FoulCallInGuard, e.location, {{"name", n.name}});
                    } else if (m_transitivelyFoul.count(n.name)) {
                        error(
                            diag::Code::TransitivelyFoulCallInGuard, e.location,
                            {{"name", n.name}});
                    }
                    for (const auto& a : n.args) {
                        if (a) {
                            checkGuard(*a);
                        }
                    }
                    for (const auto& [_, a] : n.namedArgs) {
                        if (a) {
                            checkGuard(*a);
                        }
                    }
                    // A block handed over in a guard may be run there: its effects
                    // are the guard's (#142). `holds? { IO.printLine(x); true }`
                    // printed while the arms were being matched.
                    if (n.block && *n.block) {
                        checkGuard(**n.block);
                    }
                } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                    for (const auto& value : n.values) {
                        if (value) {
                            checkGuard(*value);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
                    if (callsFoul(e)) {
                        error(diag::Code::FoulTagInGuard, e.location, {{"name", n.tag}});
                    } else if (m_transitivelyFoul.count(n.tag)) {
                        error(
                            diag::Code::TransitivelyFoulTagInGuard, e.location, {{"name", n.tag}});
                    }
                    for (const auto& value : n.values) {
                        if (value) {
                            checkGuard(*value);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                    if (n.receiver) {
                        auto receiverName = receiverModuleName(*n.receiver);
                        bool isInspect = (n.method == "inspect");
                        if (!isInspect && !receiverName.empty() &&
                            isQualifiedCallFoul(receiverName, n.method)) {
                            error(
                                diag::Code::FoulCallInGuard, e.location,
                                {{"name", receiverName + "." + n.method}});
                        }
                        checkGuard(*n.receiver);
                    }
                    if (m_topLevelFoul.contains(n.method)) {
                        error(diag::Code::FoulCallInGuard, e.location, {{"name", n.method}});
                    } else if (m_transitivelyFoul.count(n.method)) {
                        error(
                            diag::Code::TransitivelyFoulCallInGuard, e.location,
                            {{"name", n.method}});
                    }
                    for (const auto& a : n.args) {
                        if (a) {
                            checkGuard(*a);
                        }
                    }
                    for (const auto& [_, a] : n.namedArgs) {
                        if (a) {
                            checkGuard(*a);
                        }
                    }
                    if (n.block && *n.block) {
                        checkGuard(**n.block);
                    }
                } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                    if (n.left) {
                        checkGuard(*n.left);
                    }
                    if (n.right) {
                        checkGuard(*n.right);
                    }
                } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                    if (n.operand) {
                        checkGuard(*n.operand);
                    }
                } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                    // Its body runs if what it is handed to calls it.
                    for (const auto& expr : n.body) {
                        if (expr) {
                            checkGuard(*expr);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
                    // `xs.any?(~greet)`: a captured foul function, run the same way.
                    if (m_topLevelFoul.contains(n.name) || m_transitivelyFoul.count(n.name)) {
                        error(diag::Code::FoulCaptureInGuard, e.location, {{"name", n.name}});
                    }
                }
            },
            e.kind);
    };

    // Every guard in every body that runs.
    ast::forEachBodyExpr(
        program, [&](const ast::Expr& body) { ast::forEachGuard(body, checkGuard); });
}

// The blocks whose body has no effect a fused loop could reorder: no foul call
// (by the checker's decisions, through a value receiver too), no `inspect`
// (exempt from purity, but it prints), no spawn or receive, and no call whose
// effect is decided only when it runs: through a function value, a run-time
// dispatch, a trait, a builtin (`send`) or another language. Conservative: a
// block left out only keeps its own order.
auto Analyzer::recordPureBlocks(const ast::Program& program) -> void {
    m_pureBlocks.clear();
    const auto& resolved = m_checker.resolvedCalls();
    const auto& decided = m_checker.decidedCalls();
    std::function<bool(const ast::Expr&)> effectful = [&](const ast::Expr& e) -> bool {
        if (std::holds_alternative<ast::SpawnExpr>(e.kind) ||
            std::holds_alternative<ast::ReceiveExpr>(e.kind)) {
            return true;
        }
        if (callsFoul(e)) {
            return true;
        }
        if (const auto* call = std::get_if<ast::FunctionCall>(&e.kind);
            call && m_transitivelyFoul.contains(call->name)) {
            return true;
        }
        if (const auto* ident = std::get_if<ast::Identifier>(&e.kind);
            ident && m_transitivelyFoul.contains(ident->name)) {
            return true;
        }
        if (const auto* method = std::get_if<ast::MethodCall>(&e.kind)) {
            if (method->method == "inspect" || m_transitivelyFoul.contains(method->method)) {
                return true;
            }
            if (const auto found = resolved.find(method);
                found != resolved.end() && found->second.isFoul) {
                return true;
            }
            const auto module =
                method->receiver ? receiverModuleName(*method->receiver) : std::string{};
            if (!module.empty() && isQualifiedCallFoul(module, method->method)) {
                return true;
            }
        }
        // An operator's decision is how it computes, not what it calls.
        const bool isCall = std::holds_alternative<ast::MethodCall>(e.kind) ||
                            std::holds_alternative<ast::FunctionCall>(e.kind) ||
                            std::holds_alternative<ast::Identifier>(e.kind);
        if (const auto found = decided.find(&e); isCall && found != decided.end()) {
            switch (found->second.kind) {
                case CallKind::ValueCall:
                case CallKind::Dispatch:
                case CallKind::TraitMethod:
                case CallKind::Builtin:
                case CallKind::Foreign:
                case CallKind::Hidden: return true;
                default: break;
            }
        }
        bool found = false;
        ast::forEachChild(
            e, [&](const ast::ExprPtr& child) { found = found || (child && effectful(*child)); });
        return found;
    };
    std::function<void(const ast::Expr&)> visit = [&](const ast::Expr& e) {
        if (const auto* lambda = std::get_if<ast::Lambda>(&e.kind)) {
            const bool pure =
                std::none_of(lambda->body.begin(), lambda->body.end(), [&](const ast::ExprPtr& s) {
                    return s && effectful(*s);
                });
            if (pure) {
                m_pureBlocks.insert(lambda);
            }
        }
        ast::forEachChild(e, [&](const ast::ExprPtr& child) {
            if (child) {
                visit(*child);
            }
        });
    };
    ast::forEachBodyExpr(program, visit);
}
auto Analyzer::enrichEffectsFromResolvedCalls(const ast::Program& program) -> void {
    const auto& resolved = m_checker.resolvedCalls();
    // No early return on an empty `resolved` map: the trait-receiver check
    // below does not consult it, and a program whose only foul call goes
    // through a trait has no resolved targets at all (kexhq/kex#172).

    // A foul method reached through a VALUE receiver (`b.bang()`, `c.now()`)
    // is invisible to the purity walk, which checks a method call by its
    // module-qualified receiver only. The resolved call target knows the
    // answer, so record the first such
    // call per function and report it below (kexhq/kex#172).
    const ast::Expr* foulValueCall = nullptr; // location lives on the Expr
    std::string foulValueMethod;

    // Walk expression trees looking for MethodCall nodes with foul resolved targets.
    std::function<bool(const ast::Expr&)> hasFoulResolved = [&](const ast::Expr& e) -> bool {
        if (const auto* call = std::get_if<ast::MethodCall>(&e.kind)) {
            const auto& n = *call;
            auto it = resolved.find(&n);
            if (it != resolved.end() && it->second.isFoul) {
                // `IO.inspect` is the documented purity-exempt hatch.
                // A call the purity walk already reported must not be reported
                // twice — that is exactly the qualified-and-known-foul
                // case, which `receiverModuleName` alone cannot identify
                // (it returns the name of any lowercase identifier, so a
                // plain value receiver `b` looks module-qualified).
                bool alreadyReported =
                    n.receiver && isQualifiedCallFoul(receiverModuleName(*n.receiver), n.method);
                if (!foulValueCall && !alreadyReported && n.method != "inspect") {
                    foulValueCall = &e;
                    foulValueMethod = n.method;
                }
                return true;
            }
            // A trait-typed receiver dispatches dynamically, so the call
            // has no single resolved target and the branch above never
            // fires. The trait declaration states the effect, so consult
            // it directly (kexhq/kex#172).
            if (n.receiver) {
                if (auto recvType = m_checker.typeOf(n.receiver.get())) {
                    // A trait-typed parameter is a ConstrainedType (a type
                    // variable bound to the trait), not a NamedType — but
                    // a value annotated with the trait directly is named.
                    std::string traitName;
                    if (auto* con = std::get_if<ConstrainedType>(&recvType->kind)) {
                        traitName = con->traitName;
                    } else if (auto* named = std::get_if<NamedType>(&recvType->kind)) {
                        traitName = named->name;
                    }
                    if (!traitName.empty() && m_checker.isTrait(traitName) &&
                        m_checker.traitMethodIsFoul(traitName, n.method)) {
                        if (!foulValueCall && n.method != "inspect") {
                            foulValueCall = &e;
                            foulValueMethod = n.method;
                        }
                        return true;
                    }
                }
            }
        }
        bool found = false;
        ast::forEachChild(
            e, [&](const ast::ExprPtr& child) { found = found || hasFoulResolved(*child); });
        return found;
    };

    // Check each function for newly discovered foul resolved calls.
    bool newFoul = false;
    auto checkFn = [&](const ast::FunctionDef& def) {
        bool known = m_transitivelyFoul.count(def.name) > 0;
        foulValueCall = nullptr;
        bool found = false;
        for (const auto& clause : def.clauses) {
            for (const auto& ex : clause.body) {
                if (ex && hasFoulResolved(*ex)) {
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        if (found && !known) {
            m_transitivelyFoul.insert(def.name);
            newFoul = true;
        }
        if (!def.isFoul && foulValueCall) {
            error(
                diag::Code::FoulCallFromPure, foulValueCall->location, {{"name", foulValueMethod}});
        }
    };

    for (const auto& item : program.items) {
        std::visit(
            [&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n) {
                        checkFn(*n);
                    }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (n) {
                        for (const auto& mi : n->body) {
                            std::visit(
                                [&](const auto& mn) {
                                    using MT = std::decay_t<decltype(mn)>;
                                    if constexpr (std::is_same_v<
                                                      MT, std::unique_ptr<ast::FunctionDef>>) {
                                        if (mn) {
                                            checkFn(*mn);
                                        }
                                    }
                                },
                                mi);
                        }
                    }
                }
            },
            item);
    }

    if (!newFoul) {
        return;
    }

    // Snapshot before re-propagation so guard re-check only flags NEW entries.
    auto priorFoul = m_transitivelyFoul;

    // Re-propagate through the call graph.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [fn, callees] : m_callGraph) {
            if (m_transitivelyFoul.count(fn)) {
                continue;
            }
            for (const auto& callee : callees) {
                if (m_transitivelyFoul.count(callee)) {
                    m_transitivelyFoul.insert(fn);
                    changed = true;
                    break;
                }
            }
        }
    }

    // Re-check guards for newly transitively foul calls only.
    std::function<void(const ast::Expr&)> checkGuard = [&](const ast::Expr& e) {
        std::visit(
            [&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                    bool symbolFoul = callsFoul(e);
                    bool newlyFoul = m_transitivelyFoul.count(n.name) && !priorFoul.count(n.name);
                    if (newlyFoul && !symbolFoul) {
                        error(
                            diag::Code::TransitivelyFoulCallInGuard, e.location,
                            {{"name", n.name}});
                    }
                    for (const auto& a : n.args) {
                        if (a) {
                            checkGuard(*a);
                        }
                    }
                    for (const auto& [_, a] : n.namedArgs) {
                        if (a) {
                            checkGuard(*a);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                    for (const auto& value : n.values) {
                        if (value) {
                            checkGuard(*value);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
                    bool symbolFoul = callsFoul(e);
                    bool newlyFoul = m_transitivelyFoul.count(n.tag) && !priorFoul.count(n.tag);
                    if (newlyFoul && !symbolFoul) {
                        error(
                            diag::Code::TransitivelyFoulTagInGuard, e.location, {{"name", n.tag}});
                    }
                    for (const auto& value : n.values) {
                        if (value) {
                            checkGuard(*value);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                    auto it = resolved.find(&n);
                    if (it != resolved.end() && it->second.isFoul) {
                        error(diag::Code::FoulCallInGuard, e.location, {{"name", n.method}});
                    }
                    bool symbolFoul = m_topLevelFoul.contains(n.method);
                    bool newlyFoulMethod =
                        m_transitivelyFoul.count(n.method) && !priorFoul.count(n.method);
                    if (newlyFoulMethod && !symbolFoul) {
                        error(
                            diag::Code::TransitivelyFoulCallInGuard, e.location,
                            {{"name", n.method}});
                    }
                    if (n.receiver) {
                        checkGuard(*n.receiver);
                    }
                    for (const auto& a : n.args) {
                        if (a) {
                            checkGuard(*a);
                        }
                    }
                    for (const auto& [_, a] : n.namedArgs) {
                        if (a) {
                            checkGuard(*a);
                        }
                    }
                } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                    if (n.left) {
                        checkGuard(*n.left);
                    }
                    if (n.right) {
                        checkGuard(*n.right);
                    }
                } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                    if (n.operand) {
                        checkGuard(*n.operand);
                    }
                }
            },
            e.kind);
    };

    // Every guard in every body that runs.
    ast::forEachBodyExpr(
        program, [&](const ast::Expr& body) { ast::forEachGuard(body, checkGuard); });
}

auto Analyzer::checkLoopControl(SourceLocation loc, const std::string& keyword) -> void {
    for (auto it = m_loopScopes.rbegin(); it != m_loopScopes.rend(); ++it) {
        if (*it == LoopScope::Closure) {
            error(diag::Code::LoopControlInClosure, loc, {{"keyword", keyword}});
            return;
        }
        if (*it == LoopScope::Loop) {
            return;
        }
    }
    error(diag::Code::LoopControlOutsideLoop, loc, {{"keyword", keyword}});
}

auto Analyzer::error(diag::Code code, SourceLocation loc, diag::Args args) -> void {
    m_diagnostics.push_back(makeDiagnostic(Diagnostic::Level::Error, code, loc, std::move(args)));
}

// Which capabilities each function needs, from the checker's decisions
// (docs/typechecker-hm-plan.md §9). Every function counts, a `make` block's
// too, and a call is followed to the clause the checker selected for it: the
// `stamp` a `Stamper {…}.stamp` reaches, the overload a call chose, either of
// two functions of one name in different modules. A function of a
// `capability` module is one a caller needs that capability for. The
// syntactic call graph this replaced keyed functions by bare name, never
// looked inside `make` blocks, and dropped any name defined twice.
auto Analyzer::computeCapabilityRequirements(const ast::Program& program) -> void {
    m_requiredCapabilities.clear();
    // Every function, with the capability module it is a member of ("" if none).
    std::unordered_map<const ast::FunctionDef*, std::string> functions;
    std::unordered_map<const ast::FunctionClause*, const ast::FunctionDef*> ofClause;
    const auto addFunction = [&](const ast::FunctionDef& def, const std::string& capability) {
        functions[&def] = capability;
        for (const auto& clause : def.clauses) {
            ofClause[&clause] = &def;
        }
    };
    const auto addMake = [&](const ast::MakeDef& make) {
        for (const auto& item : make.body) {
            if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item); fn && *fn) {
                addFunction(**fn, "");
            } else if (const auto* block =
                           std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                       block && *block) {
                for (const auto& inner : (*block)->items) {
                    if (const auto* innerFn =
                            std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner);
                        innerFn && *innerFn) {
                        addFunction(**innerFn, "");
                    }
                }
            }
        }
    };
    const auto walkItems = [&](auto& self, const auto& items, const std::string& capability,
                               const std::string& path) -> void {
        for (const auto& item : items) {
            std::visit(
                [&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        if (node) {
                            addFunction(*node, capability);
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                        if (node) {
                            addMake(*node);
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        if (node) {
                            const auto qualified = ast::nestedModulePath(path, node->name);
                            self(
                                self, node->body, node->isCapability ? qualified : capability,
                                qualified);
                        }
                    }
                },
                item);
        }
    };
    walkItems(walkItems, program.items, "", "");

    // Every function has an entry before any is filled: references into the
    // map must survive the walk below.
    m_capabilityOf.clear();
    for (const auto& [def, capability] : functions) {
        m_requiredCapabilities[def];
        if (!capability.empty()) {
            m_capabilityOf[def] = capability;
        }
    }
    const auto& selected = m_checker.selectedCallSignatures();
    std::unordered_map<const ast::FunctionDef*, std::set<const ast::FunctionDef*>> callees;
    for (const auto& [def, ownCapability] : functions) {
        auto& needs = m_requiredCapabilities[def];
        auto& calls = callees[def];
        std::function<void(const ast::Expr&)> visit = [&](const ast::Expr& e) {
            if (const auto found = selected.find(&e); found != selected.end()) {
                const auto& signature = found->second;
                const auto clause =
                    signature.clause ? ofClause.find(signature.clause) : ofClause.end();
                if (clause != ofClause.end()) {
                    const auto* callee = clause->second;
                    if (const auto& capability = functions.at(callee);
                        !capability.empty() && capability != ownCapability) {
                        needs.emplace(capability, CapabilityReason{});
                    } else if (callee != def) {
                        calls.insert(callee);
                    }
                } else {
                    // Another unit's function: its module, when a capability.
                    for (const auto* module : {&signature.declaringModule, &signature.makeModule}) {
                        if (!module->empty() && m_capabilityModules.count(*module)) {
                            needs.emplace(*module, CapabilityReason{});
                        }
                    }
                    // and what its interface says it needs, carried in by it.
                    for (const auto& capability : signature.requiredCapabilities) {
                        if (capability != ownCapability) {
                            needs.emplace(capability, CapabilityReason{signature.name, nullptr});
                        }
                    }
                }
            }
            // A call into a capability by its name (`Clock.now()`), as written.
            if (const auto* method = std::get_if<ast::MethodCall>(&e.kind);
                method && method->receiver) {
                const auto receiver = receiverModuleName(*method->receiver);
                if (!receiver.empty() && receiver != ownCapability &&
                    m_capabilityModules.count(receiver)) {
                    needs.emplace(receiver, CapabilityReason{});
                }
            }
            ast::forEachChild(e, [&](const ast::ExprPtr& child) {
                if (child) {
                    visit(*child);
                }
            });
        };
        for (const auto& clause : def->clauses) {
            for (const auto& expr : clause.body) {
                if (expr) {
                    visit(*expr);
                }
            }
        }
    }
    // A requirement travels from callee to caller, each keeping the callee
    // that brought it, so hover can print the chain.
    for (bool spread = true; spread;) {
        spread = false;
        for (const auto& [def, called] : callees) {
            auto& mine = m_requiredCapabilities.at(def);
            for (const auto* callee : called) {
                for (const auto& [capability, _] : m_requiredCapabilities.at(callee)) {
                    if (!mine.count(capability)) {
                        mine.emplace(capability, CapabilityReason{callee->name, callee});
                        spread = true;
                    }
                }
            }
        }
    }

    // A block's own requirements (docs/typechecker-hm-plan.md §8.3, decided
    // 2026-11-24: inferred, shown only): what the calls in its body require,
    // written onto its function type, which hover shows. Its definer requires
    // the same already, so this adds no obligation.
    const auto& types = m_checker.typeMap();
    for (const auto& [def, ownCapability] : functions) {
        const auto direct = [&](const ast::Expr& e, std::set<std::string>& into) {
            if (const auto found = selected.find(&e); found != selected.end()) {
                const auto& signature = found->second;
                const auto clause =
                    signature.clause ? ofClause.find(signature.clause) : ofClause.end();
                if (clause != ofClause.end()) {
                    const auto* callee = clause->second;
                    if (const auto& capability = functions.at(callee);
                        !capability.empty() && capability != ownCapability) {
                        into.insert(capability);
                    }
                    for (const auto& [capability, _] : m_requiredCapabilities.at(callee)) {
                        into.insert(capability);
                    }
                } else {
                    for (const auto* module : {&signature.declaringModule, &signature.makeModule}) {
                        if (!module->empty() && m_capabilityModules.count(*module)) {
                            into.insert(*module);
                        }
                    }
                    for (const auto& capability : signature.requiredCapabilities) {
                        if (capability != ownCapability) {
                            into.insert(capability);
                        }
                    }
                }
            }
            if (const auto* method = std::get_if<ast::MethodCall>(&e.kind);
                method && method->receiver) {
                const auto receiver = receiverModuleName(*method->receiver);
                if (!receiver.empty() && receiver != ownCapability &&
                    m_capabilityModules.count(receiver)) {
                    into.insert(receiver);
                }
            }
        };
        std::function<void(const ast::Expr&, std::set<std::string>*)> walk =
            [&](const ast::Expr& e, std::set<std::string>* outer) {
                const bool block = std::holds_alternative<ast::Lambda>(e.kind);
                std::set<std::string> own;
                auto* into = block ? &own : outer;
                if (into) {
                    direct(e, *into);
                }
                ast::forEachChild(e, [&](const ast::ExprPtr& child) {
                    if (child) {
                        walk(*child, into);
                    }
                });
                if (!block) {
                    return;
                }
                if (outer) {
                    outer->insert(own.begin(), own.end());
                }
                const auto type = types.find(&e);
                if (type != types.end() && type->second) {
                    if (auto* function = std::get_if<FuncType>(&type->second->kind)) {
                        function->capabilities.assign(own.begin(), own.end());
                    }
                }
            };
        // `let task = { … }`: the binding's recorded type is a copy made when
        // the check finished, before this pass; it carries the block's set too.
        std::function<void(const ast::Expr&)> bindings = [&](const ast::Expr& e) {
            if (const auto* let = std::get_if<ast::LetExpr>(&e.kind);
                let && let->pattern && let->value) {
                const auto* name = std::get_if<ast::VarPattern>(&let->pattern->kind);
                const auto block = types.find(let->value.get());
                const auto* function = block != types.end() && block->second
                                           ? std::get_if<FuncType>(&block->second->kind)
                                           : nullptr;
                if (name && function && !function->capabilities.empty()) {
                    for (const auto& binding : m_checker.patternBindings()) {
                        if (binding.name != name->name || !binding.type ||
                            binding.location.line != let->pattern->location.line ||
                            binding.location.column != let->pattern->location.column) {
                            continue;
                        }
                        if (auto* bound = std::get_if<FuncType>(&binding.type->kind)) {
                            bound->capabilities = function->capabilities;
                        }
                    }
                }
            }
            ast::forEachChild(e, [&](const ast::ExprPtr& child) {
                if (child) {
                    bindings(*child);
                }
            });
        };
        for (const auto& clause : def->clauses) {
            for (const auto& expr : clause.body) {
                if (expr) {
                    walk(*expr, nullptr);
                }
            }
        }
        for (const auto& clause : def->clauses) {
            for (const auto& expr : clause.body) {
                if (expr) {
                    bindings(*expr);
                }
            }
        }
    }
}

} // namespace kex::semantic

namespace kex::semantic {

// Effects carried by data and passed through parameters (#142, decision 6 of
// docs/typechecker-hm-plan.md; the hidden effect parameter, option a). A block
// that runs something foul is a foul value, and so is a capture of a foul
// function, and a record, list or tuple holding one: the record's hidden
// effect is instantiated by what it was built with. A function that calls one
// of its parameters, or a block read out of one (`let f = h.run; f()`), is
// effect-transparent in it: calling it is as foul as what it is handed. A
// pure function only ever holds such a value through a parameter, so the place
// this decides something is a guard, which must not run an effect however the
// effect gets there.
auto Analyzer::checkEffectFlow(const ast::Program& program) -> void {
    // Whether running `e` may perform an effect: a foul call anywhere in it.
    std::function<bool(const ast::Expr&)> runsFoul = [&](const ast::Expr& e) -> bool {
        if (callsFoul(e)) {
            return true;
        }
        if (const auto* call = std::get_if<ast::FunctionCall>(&e.kind);
            call && (m_topLevelFoul.contains(call->name) || m_transitivelyFoul.count(call->name))) {
            return true;
        }
        if (const auto* method = std::get_if<ast::MethodCall>(&e.kind)) {
            if (m_topLevelFoul.contains(method->method) ||
                m_transitivelyFoul.count(method->method)) {
                return true;
            }
            const auto module =
                method->receiver ? receiverModuleName(*method->receiver) : std::string{};
            if (!module.empty() && isQualifiedCallFoul(module, method->method)) {
                return true;
            }
        }
        if (std::holds_alternative<ast::SpawnExpr>(e.kind) ||
            std::holds_alternative<ast::ReceiveExpr>(e.kind)) {
            return true;
        }
        bool found = false;
        ast::forEachChild(
            e, [&](const ast::ExprPtr& child) { found = found || (child && runsFoul(*child)); });
        return found;
    };

    auto& transparent = m_transparentParams;
    transparent.clear();
    std::unordered_set<std::string> returnsFoulValue;

    // Whether `e`, given the local names known to carry one, is a value that
    // carries an effect.
    const auto carries = [&](const auto& self, const ast::Expr& e,
                             const std::unordered_set<std::string>& carriers) -> bool {
        if (const auto* lambda = std::get_if<ast::Lambda>(&e.kind)) {
            return std::any_of(lambda->body.begin(), lambda->body.end(), [&](const auto& body) {
                return body && runsFoul(*body);
            });
        }
        if (const auto* capture = std::get_if<ast::CurryExpr>(&e.kind)) {
            return m_topLevelFoul.contains(capture->name) ||
                   m_transitivelyFoul.count(capture->name) > 0;
        }
        if (const auto* name = std::get_if<ast::Identifier>(&e.kind)) {
            // A local that holds one, or a function called by its bare name
            // (`let made = loud`) whose result is one.
            return carriers.contains(name->name) || returnsFoulValue.contains(name->name);
        }
        if (const auto* call = std::get_if<ast::FunctionCall>(&e.kind)) {
            if (returnsFoulValue.contains(call->name)) {
                return true;
            }
        }
        if (std::holds_alternative<ast::RecordConstruction>(e.kind) ||
            std::holds_alternative<ast::ListExpr>(e.kind) ||
            std::holds_alternative<ast::TupleExpr>(e.kind) ||
            std::holds_alternative<ast::MapExpr>(e.kind)) {
            bool found = false;
            ast::forEachChild(e, [&](const ast::ExprPtr& child) {
                found = found || (child && self(self, *child, carriers));
            });
            return found;
        }
        return false;
    };

    // One function's names: which carry an effect, which hold a parameter or
    // a value read out of one (by the parameter's position).
    struct Scope {
        std::unordered_set<std::string> carriers;
        std::unordered_map<std::string, std::size_t> fromParam;
    };
    const auto scopeOf = [&](const ast::FunctionClause& clause) {
        Scope scope;
        for (std::size_t i = 0; i < clause.params.size(); ++i) {
            if (clause.params[i].name) {
                scope.fromParam[*clause.params[i].name] = i;
            }
        }
        std::function<void(const ast::Expr&)> bind = [&](const ast::Expr& e) {
            if (const auto* let = std::get_if<ast::LetExpr>(&e.kind); let && let->value) {
                const auto* var =
                    let->pattern ? std::get_if<ast::VarPattern>(&let->pattern->kind) : nullptr;
                if (var) {
                    if (carries(carries, *let->value, scope.carriers)) {
                        scope.carriers.insert(var->name);
                    }
                    // `let f = h` or `let f = h.run`: still the parameter's.
                    const ast::Expr* source = let->value.get();
                    if (const auto* read = std::get_if<ast::MethodCall>(&source->kind);
                        read && read->receiver && read->args.empty() && !read->block) {
                        source = read->receiver.get();
                    }
                    if (const auto* name = std::get_if<ast::Identifier>(&source->kind)) {
                        if (const auto param = scope.fromParam.find(name->name);
                            param != scope.fromParam.end()) {
                            scope.fromParam[var->name] = param->second;
                        }
                    }
                }
            }
            ast::forEachChild(e, [&](const ast::ExprPtr& child) {
                if (child) {
                    bind(*child);
                }
            });
        };
        for (const auto& expr : clause.body) {
            if (expr) {
                bind(*expr);
            }
        }
        return scope;
    };

    // Transparency and foul results, to a fixpoint: each pass may let another
    // function see through a callee decided in the last.
    for (bool changed = true; changed;) {
        changed = false;
        ast::forEachFunction(program, [&](const ast::FunctionDef& function) {
            for (const auto& clause : function.clauses) {
                const auto scope = scopeOf(clause);
                auto& mine = transparent[function.name];
                std::function<void(const ast::Expr&)> visit = [&](const ast::Expr& e) {
                    if (const auto* call = std::get_if<ast::FunctionCall>(&e.kind)) {
                        // Calling the parameter, or a block read from it.
                        if (const auto param = scope.fromParam.find(call->name);
                            param != scope.fromParam.end() && mine.insert(param->second).second) {
                            changed = true;
                        }
                        // Handing it on to a parameter another function calls.
                        if (const auto callee = transparent.find(call->name);
                            callee != transparent.end()) {
                            for (std::size_t i = 0; i < call->args.size(); ++i) {
                                const auto* name =
                                    call->args[i]
                                        ? std::get_if<ast::Identifier>(&call->args[i]->kind)
                                        : nullptr;
                                if (!name || !callee->second.contains(i)) {
                                    continue;
                                }
                                if (const auto param = scope.fromParam.find(name->name);
                                    param != scope.fromParam.end() &&
                                    mine.insert(param->second).second) {
                                    changed = true;
                                }
                            }
                        }
                    }
                    ast::forEachChild(e, [&](const ast::ExprPtr& child) {
                        if (child) {
                            visit(*child);
                        }
                    });
                };
                for (const auto& expr : clause.body) {
                    if (expr) {
                        visit(*expr);
                    }
                }
                if (!clause.body.empty() && clause.body.back() &&
                    carries(carries, *clause.body.back(), scope.carriers) &&
                    returnsFoulValue.insert(function.name).second) {
                    changed = true;
                }
            }
        });
    }

    // A guard handing an effect to a parameter its callee runs.
    ast::forEachFunction(program, [&](const ast::FunctionDef& function) {
        for (const auto& clause : function.clauses) {
            const auto scope = scopeOf(clause);
            std::function<void(const ast::Expr&)> inGuard = [&](const ast::Expr& e) {
                if (const auto* call = std::get_if<ast::FunctionCall>(&e.kind)) {
                    if (const auto callee = transparent.find(call->name);
                        callee != transparent.end()) {
                        for (std::size_t i = 0; i < call->args.size(); ++i) {
                            if (call->args[i] && callee->second.contains(i) &&
                                !std::holds_alternative<ast::Lambda>(call->args[i]->kind) &&
                                !std::holds_alternative<ast::CurryExpr>(call->args[i]->kind) &&
                                carries(carries, *call->args[i], scope.carriers)) {
                                error(
                                    diag::Code::FoulValueInGuard, call->args[i]->location,
                                    {{"name", call->name}});
                            }
                        }
                    }
                }
                ast::forEachChild(e, [&](const ast::ExprPtr& child) {
                    if (child) {
                        inGuard(*child);
                    }
                });
            };
            for (const auto& expr : clause.body) {
                if (expr) {
                    ast::forEachGuard(*expr, inGuard);
                }
            }
        }
    });
}

} // namespace kex::semantic
