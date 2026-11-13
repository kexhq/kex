#include "analyzer.hxx"
#include "../ast/walk.hxx"
#include "module_rules.hxx"
#include "receiver_conflicts.hxx"
#include "resolve_core.hxx"

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
    if (auto* lower = std::get_if<ast::Identifier>(&receiver.kind))
        return lower->name;
    if (auto* upper = std::get_if<ast::UpperIdentifier>(&receiver.kind))
        return upper->name;
    // `FS.File` parses as an argument-less method call on `FS`. Treat it as a
    // module path only when the segment is itself capitalized, so a field read
    // such as `config.io` never looks like one.
    if (auto* call = std::get_if<ast::MethodCall>(&receiver.kind))
        if (call->args.empty() && call->namedArgs.empty() && !call->block
            && !call->method.empty()
            && std::isupper(static_cast<unsigned char>(call->method[0]))
            && call->receiver) {
            auto parent = receiverModuleName(*call->receiver);
            if (!parent.empty()) return parent + "." + call->method;
        }
    return {};
}

} // namespace

auto Analyzer::analyze(const ast::Program& program) -> bool {
    // Which names one module may take from another (`using`, `export`).
    for (auto& violation : findModuleRuleViolations(
             program, m_importedInterfaces,
             m_moduleLocator ? &m_moduleLocator : nullptr, m_entryFiles))
        m_diagnostics.push_back(std::move(violation));

    // Type checking: the checker decides what every name and call means.
    const auto checkedFrom = m_diagnostics.size();
    m_checker.check(program, m_diagnostics, m_entryFiles);
    if (m_indexesNames)
        m_nameIndex = buildNameIndex(program,
                                     m_entryFiles.empty() ? std::string{} : m_entryFiles.front(),
                                     m_checker, m_importedInterfaces);
    // Two definitions of one method for one receiver type, the receivers as
    // the checker resolved them. Reported ahead of the type errors so the
    // message is not buried under those an ambiguous method tends to cause.
    auto conflicts = findReceiverConflicts(program, m_checker);
    m_diagnostics.insert(m_diagnostics.begin() + static_cast<std::ptrdiff_t>(checkedFrom),
                         std::make_move_iterator(conflicts.begin()),
                         std::make_move_iterator(conflicts.end()));

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
    for (const auto& item : program.items)
        if (const auto* using_ = std::get_if<std::unique_ptr<ast::UsingBlock>>(&item))
            if (*using_) m_usings.push_back(using_->get());
    reportDuplicateRecords(program.items);
    for (const auto& item : program.items) analyzeItem(item);
    // Foulness through the call graph, so a guard rejects a transitively
    // foul call.
    computeTransitiveEffects(program);
    m_diagnostics.insert(m_diagnostics.end(), std::make_move_iterator(checked.begin()),
                         std::make_move_iterator(checked.end()));

    // Foul calls through a value receiver or a trait, which only the
    // checker's resolved targets show.
    enrichEffectsFromResolvedCalls(program);

    return std::none_of(m_diagnostics.begin(), m_diagnostics.end(),
        [](const Diagnostic& d) { return d.level == Diagnostic::Level::Error; });
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
    if (const auto found = selected.find(&call);
        found != selected.end() && found->second.isFoul)
        return true;
    // A module's function, by its module's record of its effects.
    std::string named;
    if (const auto found = m_checker.calledFunctions().find(&call);
        found != m_checker.calledFunctions().end())
        named = found->second;
    else if (const auto constant = m_checker.referencedConstants().find(&call);
             constant != m_checker.referencedConstants().end())
        named = constant->second;
    const auto dot = named.rfind('.');
    return dot != std::string::npos &&
           isQualifiedCallFoul(named.substr(0, dot), named.substr(dot + 1));
}

template <typename Items>
auto Analyzer::reportDuplicateRecords(const Items& items) -> void {
    // A type may be reopened; a record may not, nor share a type's name.
    std::set<std::string> declared;
    auto declare = [&](const auto& item) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if (!node) return;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>)
                declared.insert(node->name);
            else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (!declared.insert(node->name).second)
                    error(node->location, "Duplicate record definition: " + node->name);
            }
        }, item);
    };
    for (const auto& item : items) {
        declare(item);
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>)
                if (node)
                    for (const auto& inner : node->items) declare(inner);
        }, item);
    }
}

template <typename Item>
auto Analyzer::analyzeItem(const Item& item) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if (!node) return;
        if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
            analyzeModule(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
            // A `make` block's `private do` members are not walked (yet).
            for (const auto& member : node->body)
                if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&member))
                    if (*fn) analyzeFunctionDef(**fn);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
            analyzeFunctionDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
            // A main block may do anything.
            const auto outer = std::exchange(m_inFoulContext, true);
            analyzeBody(node->body);
            m_inFoulContext = outer;
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
            // Visibility decides who may CALL a function, not whether its
            // body is checked: a `private do` member is purity-checked too.
            for (const auto& inner : node->items) analyzeItem(inner);
        }
    }, item);
}

auto Analyzer::analyzeModule(const ast::ModuleDef& mod) -> void {
    // A module never carries an effect of its own: entering one leaves the
    // surrounding purity exactly as it was, and each member declares its own.
    // Its own `using`s apply inside it, and only there.
    const auto outerUsings = m_usings.size();
    for (const auto& item : mod.body)
        if (const auto* using_ = std::get_if<std::unique_ptr<ast::UsingBlock>>(&item))
            if (*using_) m_usings.push_back(using_->get());
    reportDuplicateRecords(mod.body);
    for (const auto& item : mod.body) analyzeItem(item);
    m_usings.resize(outerUsings);
}

auto Analyzer::analyzeFunctionDef(const ast::FunctionDef& def) -> void {
    const auto outer = m_inFoulContext;
    if (def.isFoul) m_inFoulContext = true;
    for (const auto& clause : def.clauses) analyzeBody(clause.body);
    m_inFoulContext = outer;
}

auto Analyzer::analyzeBody(const std::vector<ast::ExprPtr>& body) -> void {
    for (const auto& expr : body)
        if (expr) analyzeExpr(*expr);
}

auto Analyzer::analyzeExpr(const ast::Expr& expr) -> void {
    auto foulCall = [&](const std::string& what, const std::string& name) {
        if (!m_inFoulContext)
            error(expr.location,
                  "Cannot call foul " + what + " '" + name + "' from pure context");
    };
    if (const auto* ident = std::get_if<ast::Identifier>(&expr.kind)) {
        // A zero-parameter function named bare is called.
        if (callsFoul(expr)) foulCall("function", ident->name);
    } else if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind)) {
        if (callsFoul(expr)) foulCall("function", call->name);
    } else if (const auto* tagged = std::get_if<ast::TaggedLiteral>(&expr.kind)) {
        if (callsFoul(expr)) foulCall("tag", tagged->tag);
    } else if (const auto* capture = std::get_if<ast::CurryExpr>(&expr.kind)) {
        // `~shout` hands the function on to be run, so a pure body may not
        // name a foul one any more than call it (kexhq/kex#640).
        const auto decided = m_checker.decidedCalls().find(&expr);
        if (!m_inFoulContext && decided != m_checker.decidedCalls().end() &&
            decided->second.kind == CallKind::FunctionReference) {
            const auto& owner = decided->second.owner;
            const auto dot = owner.rfind('.');
            const bool foul = dot == std::string::npos
                ? m_topLevelFoul.contains(owner)
                : isQualifiedCallFoul(owner.substr(0, dot), owner.substr(dot + 1));
            if (foul)
                error(expr.location, "Cannot capture foul function '" + capture->name +
                                         "' in a pure context");
        }
    } else if (const auto* method = std::get_if<ast::MethodCall>(&expr.kind)) {
        // `IO.inspect` is the documented purity-exempt escape hatch.
        const auto module = method->receiver ? receiverModuleName(*method->receiver)
                                              : std::string{};
        if (method->method != "inspect" && !module.empty() &&
            isQualifiedCallFoul(module, method->method))
            foulCall("function", module + "." + method->method);
    } else if (std::holds_alternative<ast::SpawnExpr>(expr.kind) ||
               std::holds_alternative<ast::ReceiveExpr>(expr.kind)) {
        if (!m_inFoulContext)
            error(expr.location,
                  std::string("Cannot use '") +
                      (std::holds_alternative<ast::SpawnExpr>(expr.kind) ? "spawn" : "receive") +
                      "' in pure context");
    } else if (std::holds_alternative<ast::BreakExpr>(expr.kind)) {
        checkLoopControl(expr.location, "break");
    } else if (std::holds_alternative<ast::NextExpr>(expr.kind)) {
        checkLoopControl(expr.location, "next");
    }

    // `break` and `next` bind to the nearest loop, and not across a block.
    const bool loop = std::holds_alternative<ast::LoopExpr>(expr.kind) ||
                      std::holds_alternative<ast::WhileExpr>(expr.kind);
    const bool block = std::holds_alternative<ast::Lambda>(expr.kind);
    if (loop || block) m_loopScopes.push_back(loop ? LoopScope::Loop : LoopScope::Closure);
    ast::forEachChild(expr, [&](const ast::ExprPtr& child) { analyzeExpr(*child); });
    if (loop || block) m_loopScopes.pop_back();
}

auto Analyzer::collectModuleMemberEffects(const ast::Program& program) -> void {
    m_localModuleFoulMembers.clear();
    m_topLevelFoul.clear();

    std::function<void(const ast::ModuleDef&)> collect =
        [&](const ast::ModuleDef& mod) {
        auto& foulMembers = m_localModuleFoulMembers[mod.name];
        for (const auto& item : mod.body) {
            if (const auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn && (*fn)->isFoul) foulMembers.insert((*fn)->name);
            } else if (const auto* ann =
                    std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
                // `foul name : T` declares a foul value binding; the `let`
                // clause below it carries no marker of its own.
                if (*ann && (*ann)->isFoul) foulMembers.insert((*ann)->name);
            } else if (const auto* nested =
                    std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*nested) collect(**nested);
            } else if (const auto* visibility =
                    std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
                // A `private do` member is still a member: its effect has to
                // be registered, or a caller inside the same module sees it
                // as pure.
                if (*visibility)
                    for (const auto& inner : (*visibility)->items)
                        if (const auto* fn = std::get_if<
                                std::unique_ptr<ast::FunctionDef>>(&inner);
                            fn && *fn && (*fn)->isFoul)
                            foulMembers.insert((*fn)->name);
            }
        }
    };

    for (const auto& item : program.items) {
        if (const auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
            if (*mod) collect(**mod);
        } else if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fn && (*fn)->isFoul) m_topLevelFoul.insert((*fn)->name);
        }
    }
}

auto Analyzer::isQualifiedCallFoul(const std::string& module,
                                   const std::string& member) const -> bool {
    auto known = [&](const std::string& name) {
        if (auto local = m_localModuleFoulMembers.find(name);
            local != m_localModuleFoulMembers.end() && local->second.count(member))
            return true;
        return m_importedInterfaces
            && isExportFoul(*m_importedInterfaces, name, member);
    };
    // Only the receiver exactly as written is looked up. Resolving a
    // shortened path (`File.read` under `using FS`) would mean matching any
    // module whose name ends in that segment, and `Mock.System.OS` would then
    // make every `System.OS` read foul. Missing a rejection is recoverable;
    // rejecting pure code because an unrelated module shares a segment is not.
    if (known(module)) return true;
    // An alias is not a guess: `using Store.Repo, as: Repo` names exactly
    // one module, and `Repo.fetch` is its `fetch`. Read as written, every
    // foul call through an alias passed as pure (kexhq/kex#584).
    const auto dot = module.find('.');
    const auto head = module.substr(0, dot);
    for (auto it = m_usings.rbegin(); it != m_usings.rend(); ++it) {
        const auto& using_ = **it;
        if (!using_.alias || *using_.alias != head) continue;
        std::string target = using_.module.dotted();
        if (dot != std::string::npos) target += module.substr(dot);
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
    m_requiredCapabilities.clear();
    {
        std::function<void(const ast::ModuleDef&)> collectCapabilities =
            [&](const ast::ModuleDef& module) {
            if (module.isCapability) m_capabilityModules.insert(module.name);
            for (const auto& mi : module.body)
                std::visit([&](const auto& mn) {
                    using MT = std::decay_t<decltype(mn)>;
                    if constexpr (std::is_same_v<MT,
                                                 std::unique_ptr<ast::ModuleDef>>)
                        if (mn) collectCapabilities(*mn);
                }, mi);
        };
        for (const auto& item : program.items)
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>)
                    if (n) collectCapabilities(*n);
            }, item);
    }

    // Walk an expression tree, collecting called function names and detecting
    // direct foul operations (spawn, receive, foul module method calls).
    std::function<void(const ast::Expr&, std::set<std::string>&, bool&)> walkExpr =
        [&](const ast::Expr& e, std::set<std::string>& calls, bool& hasFoulOp) {
        if (const auto* call = std::get_if<ast::FunctionCall>(&e.kind)) {
            if (allFns.count(call->name)) calls.insert(call->name);
            if (callsFoul(e)) hasFoulOp = true;
        } else if (const auto* tagged = std::get_if<ast::TaggedLiteral>(&e.kind)) {
            if (allFns.count(tagged->tag)) calls.insert(tagged->tag);
            if (callsFoul(e)) hasFoulOp = true;
        } else if (const auto* method = std::get_if<ast::MethodCall>(&e.kind)) {
            if (allFns.count(method->method)) calls.insert(method->method);
            if (method->receiver) {
                auto receiverName = receiverModuleName(*method->receiver);
                if (!receiverName.empty()
                    && isQualifiedCallFoul(receiverName, method->method))
                    hasFoulOp = true;
                // "" marks a requirement this function reaches itself,
                // as opposed to one inherited from a callee below.
                if (m_capabilityTarget && !receiverName.empty()
                    && m_capabilityModules.count(receiverName))
                    m_capabilityTarget->emplace(receiverName, "");
            }
        } else if (std::holds_alternative<ast::SpawnExpr>(e.kind) ||
                   std::holds_alternative<ast::ReceiveExpr>(e.kind)) {
            hasFoulOp = true;
        }
        ast::forEachChild(e, [&](const ast::ExprPtr& child) {
            walkExpr(*child, calls, hasFoulOp);
        });
    };

    // Pass 1: collect all function names.
    auto collectFns = [&](const auto& items) {
        for (const auto& item : items) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n) { allFns.insert(n->name); definitionCount[n->name]++; }
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (n) {
                        for (const auto& mi : n->body) {
                            std::visit([&](const auto& mn) {
                                using MT = std::decay_t<decltype(mn)>;
                                if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                                    if (mn) {
                                        allFns.insert(mn->name);
                                        definitionCount[mn->name]++;
                                    }
                                }
                            }, mi);
                        }
                    }
                }
            }, item);
        }
    };
    collectFns(program.items);

    // Pass 2: build call graph and seed directly foul.
    auto processFn = [&](const ast::FunctionDef& def) {
        std::set<std::string> calls;
        bool hasFoulOp = def.isFoul;
        m_capabilityTarget = &m_requiredCapabilities[def.name];
        for (const auto& clause : def.clauses)
            for (const auto& ex : clause.body)
                if (ex) walkExpr(*ex, calls, hasFoulOp);
        m_capabilityTarget = nullptr;
        calls.erase(def.name);
        m_callGraph[def.name] = std::move(calls);
        if (hasFoulOp) directlyFoul.insert(def.name);
    };

    for (const auto& item : program.items) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (n) processFn(*n);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (n) {
                    for (const auto& mi : n->body) {
                        std::visit([&](const auto& mn) {
                            using MT = std::decay_t<decltype(mn)>;
                            if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                                if (mn) processFn(*mn);
                            }
                        }, mi);
                    }
                }
            }
        }, item);
    }

    // Propagate foulness through the call graph (fixpoint).
    m_transitivelyFoul = directlyFoul;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [fn, callees] : m_callGraph) {
            if (m_transitivelyFoul.count(fn)) continue;
            for (const auto& callee : callees) {
                if (m_transitivelyFoul.count(callee)) {
                    m_transitivelyFoul.insert(fn);
                    changed = true;
                    break;
                }
            }
        }
    }

    // Requirements propagate the same way foulness does, but each one keeps
    // the callee that carried it in so the chain can be printed later.
    {
        bool spread = true;
        while (spread) {
            spread = false;
            for (const auto& [fn, callees] : m_callGraph) {
                for (const auto& callee : callees) {
                    auto found = m_requiredCapabilities.find(callee);
                    if (found == m_requiredCapabilities.end()) continue;
                    for (const auto& [capability, _] : found->second) {
                        auto& mine = m_requiredCapabilities[fn];
                        if (mine.count(capability)) continue;
                        mine.emplace(capability, callee);
                        spread = true;
                    }
                }
            }
        }
        // A name defined more than once in the unit cannot be attributed
        // precisely, and over-reporting a requirement is worse than staying
        // quiet about it.
        for (const auto& [name, count] : definitionCount)
            if (count > 1) m_requiredCapabilities.erase(name);
        // A function that needs nothing should not appear at all.
        for (auto it = m_requiredCapabilities.begin();
             it != m_requiredCapabilities.end();)
            it = it->second.empty() ? m_requiredCapabilities.erase(it)
                                    : std::next(it);
    }

    // Re-walk guard expressions to reject foul calls, direct and transitive.
    std::function<void(const ast::Expr&)> checkGuard = [&](const ast::Expr& e) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                if (callsFoul(e)) {
                    error(e.location,
                        "Cannot call foul function '" + n.name + "' in a guard");
                } else if (m_transitivelyFoul.count(n.name)) {
                    error(e.location,
                        "Cannot call function '" + n.name +
                        "' in a guard: it transitively calls foul operations");
                }
                for (const auto& a : n.args) if (a) checkGuard(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) checkGuard(*a);
            } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                for (const auto& value : n.values) if (value) checkGuard(*value);
            } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
                if (callsFoul(e)) {
                    error(e.location,
                        "Cannot call foul tag '" + n.tag + "' in a guard");
                } else if (m_transitivelyFoul.count(n.tag)) {
                    error(e.location,
                        "Cannot call tag '" + n.tag +
                        "' in a guard: it transitively calls foul operations");
                }
                for (const auto& value : n.values)
                    if (value) checkGuard(*value);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                if (n.receiver) {
                    auto receiverName = receiverModuleName(*n.receiver);
                    bool isInspect = (n.method == "inspect");
                    if (!isInspect && !receiverName.empty()
                        && isQualifiedCallFoul(receiverName, n.method)) {
                        error(e.location,
                            "Cannot call foul function '" + receiverName + "." +
                            n.method + "' in a guard");
                    }
                    checkGuard(*n.receiver);
                }
                if (m_topLevelFoul.contains(n.method)) {
                    error(e.location,
                        "Cannot call foul function '" + n.method + "' in a guard");
                } else if (m_transitivelyFoul.count(n.method)) {
                    error(e.location,
                        "Cannot call function '" + n.method +
                        "' in a guard: it transitively calls foul operations");
                }
                for (const auto& a : n.args) if (a) checkGuard(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) checkGuard(*a);
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                if (n.left) checkGuard(*n.left);
                if (n.right) checkGuard(*n.right);
            } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                if (n.operand) checkGuard(*n.operand);
            }
        }, e.kind);
    };

    // Every guard in every body that runs.
    ast::forEachBodyExpr(program, [&](const ast::Expr& body) {
        ast::forEachGuard(body, checkGuard);
    });
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
    const ast::Expr* foulValueCall = nullptr;   // location lives on the Expr
    std::string foulValueMethod;

    // Walk expression trees looking for MethodCall nodes with foul resolved targets.
    std::function<bool(const ast::Expr&)> hasFoulResolved =
        [&](const ast::Expr& e) -> bool {
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
                bool alreadyReported = n.receiver
                    && isQualifiedCallFoul(receiverModuleName(*n.receiver),
                                           n.method);
                if (!foulValueCall && !alreadyReported
                    && n.method != "inspect") {
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
                    if (auto* con =
                            std::get_if<ConstrainedType>(&recvType->kind))
                        traitName = con->traitName;
                    else if (auto* named =
                            std::get_if<NamedType>(&recvType->kind))
                        traitName = named->name;
                    if (!traitName.empty()
                        && m_checker.isTrait(traitName)
                        && m_checker.traitMethodIsFoul(traitName,
                                                       n.method)) {
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
        ast::forEachChild(e, [&](const ast::ExprPtr& child) {
            found = found || hasFoulResolved(*child);
        });
        return found;
    };

    // Check each function for newly discovered foul resolved calls.
    bool newFoul = false;
    auto checkFn = [&](const ast::FunctionDef& def) {
        bool known = m_transitivelyFoul.count(def.name) > 0;
        foulValueCall = nullptr;
        bool found = false;
        for (const auto& clause : def.clauses) {
            for (const auto& ex : clause.body)
                if (ex && hasFoulResolved(*ex)) { found = true; break; }
            if (found) break;
        }
        if (found && !known) {
            m_transitivelyFoul.insert(def.name);
            newFoul = true;
        }
        if (!def.isFoul && foulValueCall) {
            error(foulValueCall->location,
                "Cannot call foul function '" + foulValueMethod +
                "' from pure context");
        }
    };

    for (const auto& item : program.items) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (n) checkFn(*n);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (n) {
                    for (const auto& mi : n->body) {
                        std::visit([&](const auto& mn) {
                            using MT = std::decay_t<decltype(mn)>;
                            if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                                if (mn) checkFn(*mn);
                            }
                        }, mi);
                    }
                }
            }
        }, item);
    }

    if (!newFoul) return;

    // Snapshot before re-propagation so guard re-check only flags NEW entries.
    auto priorFoul = m_transitivelyFoul;

    // Re-propagate through the call graph.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [fn, callees] : m_callGraph) {
            if (m_transitivelyFoul.count(fn)) continue;
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
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                bool symbolFoul = callsFoul(e);
                bool newlyFoul = m_transitivelyFoul.count(n.name) && !priorFoul.count(n.name);
                if (newlyFoul && !symbolFoul) {
                    error(e.location,
                        "Cannot call function '" + n.name +
                        "' in a guard: it transitively calls foul operations");
                }
                for (const auto& a : n.args) if (a) checkGuard(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) checkGuard(*a);
            } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                for (const auto& value : n.values) if (value) checkGuard(*value);
            } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
                bool symbolFoul = callsFoul(e);
                bool newlyFoul = m_transitivelyFoul.count(n.tag) && !priorFoul.count(n.tag);
                if (newlyFoul && !symbolFoul) {
                    error(e.location,
                        "Cannot call tag '" + n.tag +
                        "' in a guard: it transitively calls foul operations");
                }
                for (const auto& value : n.values) if (value) checkGuard(*value);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                auto it = resolved.find(&n);
                if (it != resolved.end() && it->second.isFoul) {
                    error(e.location,
                        "Cannot call foul function '" + n.method +
                        "' in a guard");
                }
                bool symbolFoul = m_topLevelFoul.contains(n.method);
                bool newlyFoulMethod = m_transitivelyFoul.count(n.method) && !priorFoul.count(n.method);
                if (newlyFoulMethod && !symbolFoul) {
                    error(e.location,
                        "Cannot call function '" + n.method +
                        "' in a guard: it transitively calls foul operations");
                }
                if (n.receiver) checkGuard(*n.receiver);
                for (const auto& a : n.args) if (a) checkGuard(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) checkGuard(*a);
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                if (n.left) checkGuard(*n.left);
                if (n.right) checkGuard(*n.right);
            } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                if (n.operand) checkGuard(*n.operand);
            }
        }, e.kind);
    };

    // Every guard in every body that runs.
    ast::forEachBodyExpr(program, [&](const ast::Expr& body) {
        ast::forEachGuard(body, checkGuard);
    });
}

auto Analyzer::checkLoopControl(SourceLocation loc, const std::string& keyword) -> void {
    for (auto it = m_loopScopes.rbegin(); it != m_loopScopes.rend(); ++it) {
        if (*it == LoopScope::Closure) {
            error(loc, "'" + keyword + "' cannot be used inside a closure");
            return;
        }
        if (*it == LoopScope::Loop) {
            return;
        }
    }
    error(loc, "'" + keyword + "' used outside a loop");
}

auto Analyzer::error(SourceLocation loc, const std::string& msg) -> void {
    m_diagnostics.push_back({Diagnostic::Level::Error, loc, msg});
}

} // namespace kex::semantic
