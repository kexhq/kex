#include "analyzer.hxx"

#include <algorithm>
#include <functional>
#include <set>

namespace kex::semantic {

auto Analyzer::bindPatternVars(const ast::Pattern& pat, SourceLocation loc) -> void {
    std::visit([this, loc](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::VarPattern>) {
            if (node.name != "_") {
                auto* existing = m_symbols.currentScope().lookupLocal(node.name);
                if (existing && existing->kind == SymbolKind::Variable) {
                    if (existing->isMutable) {
                        error(loc, "Cannot redeclare '" + node.name + "': was declared with 'var'");
                    } else {
                        error(loc, "Cannot redeclare immutable binding: " + node.name);
                    }
                }
                m_symbols.define(Symbol{node.name, SymbolKind::Variable, false, false, true, loc});
            }
        }
        else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) bindPatternVars(*node.inner, loc);
        }
        else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            for (const auto& arg : node.args) {
                if (arg) bindPatternVars(*arg, loc);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            for (const auto& field : node.fields) {
                if (field.pattern) bindPatternVars(**field.pattern, loc);
                else if (!field.isStringKey) {
                    m_symbols.define(Symbol{field.name, SymbolKind::Variable, false, false, true, loc});
                }
            }
        }
        else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            for (const auto& elem : node.elements) {
                if (elem) bindPatternVars(*elem, loc);
            }
            if (node.rest) bindPatternVars(**node.rest, loc);
        }
        else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            for (const auto& elem : node.elements) {
                if (elem) bindPatternVars(*elem, loc);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RangePattern>) {
            if (node.start) bindPatternVars(*node.start, loc);
            if (node.end) bindPatternVars(*node.end, loc);
        }
        // LiteralPattern, WildcardPattern introduce nothing.
    }, pat.kind);
}

auto Analyzer::analyze(const ast::Program& program) -> bool {
    // Phase 1: scope resolution and purity checking
    for (const auto& item : program.items) {
        analyzeTopLevel(item);
    }

    // Phase 1.5: transitive effect computation — propagate foulness through
    // the call graph so guard checks reject transitively foul calls.
    computeTransitiveEffects(program);

    // Phase 2: type checking
    m_checker.check(program, m_diagnostics);

    // Phase 2.5: enrich transitive effects with resolved-call foulness
    // from imported interfaces (KexI isFoul on receiver functions).
    enrichEffectsFromResolvedCalls(program);

    return std::none_of(m_diagnostics.begin(), m_diagnostics.end(),
        [](const Diagnostic& d) { return d.level == Diagnostic::Level::Error; });
}

auto Analyzer::diagnostics() const -> const std::vector<Diagnostic>& {
    return m_diagnostics;
}

auto Analyzer::typeOf(const ast::Expr* expr) const -> TypePtr {
    return m_checker.typeOf(expr);
}

auto Analyzer::typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>& {
    return m_checker.typeMap();
}

auto Analyzer::functionSignatures(const ast::FunctionDef* function) const
    -> const std::vector<Signature>* {
    return m_checker.functionSignatures(function);
}

auto Analyzer::resolvedCalls() const
    -> const std::unordered_map<const ast::MethodCall*, ResolvedCallTarget>& {
    return m_checker.resolvedCalls();
}

auto Analyzer::analyzeTopLevel(const ast::TopLevelItem& item) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
            analyzeModule(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
            analyzeTypeDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
            analyzeRecordDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
            analyzeMakeDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
            analyzeFunctionDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
            analyzeMainBlock(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
            // Skip compiled blocks for now
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
            // Using blocks — would need module resolution
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::Pragma>>) {
            // Pragmas are metadata
        }
    }, item);
}

auto Analyzer::analyzeModule(const ast::ModuleDef& mod) -> void {
    m_symbols.define(Symbol{
        mod.name, SymbolKind::Module, mod.isFoul, false, true, mod.location});

    m_symbols.pushScope(mod.isFoul);
    bool prevFoul = m_inFoulContext;
    if (mod.isFoul) m_inFoulContext = true;

    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                analyzeModule(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                analyzeTypeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                analyzeRecordDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                analyzeMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                analyzeFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
                // skip
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                // TODO: handle visibility
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                // skip
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
                // Type annotations are handled with their function defs
            }
        }, item);
    }

    m_inFoulContext = prevFoul;
    m_symbols.popScope();
}

auto Analyzer::analyzeTypeDef(const ast::TypeDef& def) -> void {
    Symbol sym{def.name, SymbolKind::Type, false, false, true, def.location};
    sym.typeParams = def.typeParams;
    for (const auto& parent : def.parents) {
        if (!parent.parts.empty()) {
            sym.parents.push_back(parent.parts[0]);
        }
    }

    if (!m_symbols.define(std::move(sym))) {
        // Duplicate type names are allowed for reopening — skip error
    }
}

auto Analyzer::analyzeRecordDef(const ast::RecordDef& def) -> void {
    Symbol sym{def.name, SymbolKind::Record, false, false, true, def.location};
    sym.typeParams = def.typeParams;

    if (!m_symbols.define(std::move(sym))) {
        error(def.location, "Duplicate record definition: " + def.name);
    }
}

auto Analyzer::analyzeMakeDef(const ast::MakeDef& def) -> void {
    m_symbols.pushScope(m_inFoulContext);

    for (const auto& item : def.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                analyzeFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
                // handled with function defs
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                // TODO
            }
        }, item);
    }

    m_symbols.popScope();
}

auto Analyzer::analyzeFunctionDef(const ast::FunctionDef& def) -> void {
    Symbol sym{def.name, SymbolKind::Function, def.isFoul, false, true, def.location};
    sym.clauseCount = static_cast<int>(def.clauses.size());
    m_symbols.define(std::move(sym));

    // Analyze function bodies
    bool prevFoul = m_inFoulContext;
    if (def.isFoul) m_inFoulContext = true;

    for (const auto& clause : def.clauses) {
        m_symbols.pushScope(m_inFoulContext);

        // Register parameters as variables
        for (const auto& param : clause.params) {
            if (param.name.has_value() && *param.name != "_") {
                m_symbols.define(Symbol{
                    *param.name, SymbolKind::Variable, false, false, true, def.location});
            }
            if (param.pattern) {
                bindPatternVars(**param.pattern, def.location);
            }
        }

        analyzeBody(clause.body);
        m_symbols.popScope();
    }

    m_inFoulContext = prevFoul;
}

auto Analyzer::analyzeMainBlock(const ast::MainBlock& block) -> void {
    // Synthetic top-level binding wrappers run in the global scope so that
    // `let x = expr` at top level remains visible to subsequent items.
    if (!block.synthetic) m_symbols.pushScope(true); // main is implicitly foul
    bool prevFoul = m_inFoulContext;
    m_inFoulContext = true;

    // `main(args) do ... end` — args (and any pattern-shaped param) wasn't
    // registered at all before, so referencing it inside main's body
    // always failed with "Undefined identifier".
    for (const auto& param : block.params) {
        if (param.name.has_value() && *param.name != "_") {
            m_symbols.define(Symbol{
                *param.name, SymbolKind::Variable, false, false, true, block.location});
        }
        if (param.pattern) {
            bindPatternVars(**param.pattern, block.location);
        }
    }

    analyzeBody(block.body);

    m_inFoulContext = prevFoul;
    if (!block.synthetic) m_symbols.popScope();
}

auto Analyzer::analyzeBody(const std::vector<ast::ExprPtr>& body) -> void {
    for (const auto& expr : body) {
        if (expr) analyzeExpr(*expr);
    }
}

auto Analyzer::analyzeExpr(const ast::Expr& expr) -> void {
    std::visit([this, &expr](const auto& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::LetExpr>) {
            if (node.value) analyzeExpr(*node.value);
            // bindPatternVars generalizes the old VarPattern-only check to
            // every destructuring shape `let` accepts (e.g. `let { host,
            // port } = config`, `let (a, b) = pair`), which weren't
            // registering their bound names at all before.
            if (node.pattern) bindPatternVars(*node.pattern, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            if (node.value) analyzeExpr(*node.value);
            m_symbols.define(Symbol{
                node.name, SymbolKind::Variable, false, true, true, expr.location});
        }
        else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            if (node.value) analyzeExpr(*node.value);
            auto* sym = m_symbols.lookup(node.name);
            if (!sym) {
                error(expr.location, "Undefined variable: " + node.name);
            } else if (!sym->isMutable) {
                error(expr.location, "Cannot assign to immutable binding: " + node.name);
            }
        }
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
            auto* sym = m_symbols.lookup(node.name);
            if (!sym && node.name != "_") {
                error(expr.location, "Undefined identifier: " + node.name);
            }
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            auto* sym = m_symbols.lookup(node.name);
            if (sym && sym->isFoul && !m_inFoulContext) {
                error(expr.location,
                    "Cannot call foul function '" + node.name + "' from pure context");
            }
            if (sym && sym->isFoul && m_inGuard) {
                error(expr.location,
                    "Cannot call foul function '" + node.name + "' in a guard");
            }
            for (const auto& arg : node.args) {
                if (arg) analyzeExpr(*arg);
            }
            for (const auto& [_, arg] : node.namedArgs) {
                if (arg) analyzeExpr(*arg);
            }
            if (node.block) analyzeExpr(**node.block);
        }
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            if (node.receiver) analyzeExpr(*node.receiver);
            for (const auto& arg : node.args) {
                if (arg) analyzeExpr(*arg);
            }
            for (const auto& [_, arg] : node.namedArgs) {
                if (arg) analyzeExpr(*arg);
            }
            if (node.block) analyzeExpr(**node.block);

            // IO.inspect is the documented purity-exempt escape hatch.
            bool isInspect = (node.method == "inspect");
            auto receiverName = [&]() -> std::string {
                if (!node.receiver) return {};
                if (auto* lo = std::get_if<ast::Identifier>(&node.receiver->kind))
                    return lo->name;
                if (auto* up = std::get_if<ast::UpperIdentifier>(&node.receiver->kind))
                    return up->name;
                return {};
            }();
            auto isModuleFoul = [&](const std::string& name) -> bool {
                if (m_importedInterfaces) {
                    auto it = m_importedInterfaces->modules.find(name);
                    if (it != m_importedInterfaces->modules.end())
                        return it->second.isFoul;
                }
                for (std::string_view mod : kFoulModules)
                    if (name == mod) return true;
                return false;
            };
            // Kex.Intrinsic.* is the private ABI — its per-export isFoul
            // governs purity, not the blanket Kex module foulness.
            bool isIntrinsicCall = (node.method == "Intrinsic");
            if (!m_inFoulContext && !isInspect && !isIntrinsicCall
                && !receiverName.empty() && isModuleFoul(receiverName)) {
                error(expr.location,
                    "Cannot call foul function '" + receiverName + "." +
                    node.method + "' from pure context");
            }
            if (m_inGuard && !isInspect && !isIntrinsicCall
                && !receiverName.empty() && isModuleFoul(receiverName)) {
                error(expr.location,
                    "Cannot call foul function '" + receiverName + "." +
                    node.method + "' in a guard");
            }

            // Check mutating call on immutable
            if (node.mutating) {
                if (auto* ident = std::get_if<ast::Identifier>(&node.receiver->kind)) {
                    auto* sym = m_symbols.lookup(ident->name);
                    if (sym && !sym->isMutable) {
                        error(expr.location,
                            "Cannot use '!' on immutable binding: " + ident->name);
                    }
                }
            }
        }
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            if (node.left) analyzeExpr(*node.left);
            if (node.right) analyzeExpr(*node.right);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            if (node.operand) analyzeExpr(*node.operand);
        }
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            if (node.condition) analyzeExpr(*node.condition);
            analyzeBody(node.thenBody);
            for (const auto& [cond, body] : node.elifs) {
                if (cond) analyzeExpr(*cond);
                analyzeBody(body);
            }
            if (node.elseBody) analyzeBody(*node.elseBody);
        }
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            if (node.subject) analyzeExpr(*node.subject);
            for (const auto& clause : node.clauses) {
                m_symbols.pushScope(m_inFoulContext);
                if (node.subjectBinding) {
                    m_symbols.define(Symbol{
                        *node.subjectBinding, SymbolKind::Variable, false, false, true, expr.location});
                }
                for (const auto& pat : clause.patterns) {
                    if (pat) bindPatternVars(*pat, expr.location);
                }
                if (clause.guard && *clause.guard) {
                    m_inGuard = true;
                    analyzeExpr(**clause.guard);
                    m_inGuard = false;
                }
                if (clause.body) analyzeExpr(*clause.body);
                m_symbols.popScope();
            }
        }
        else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            m_loopScopes.push_back(LoopScope::Loop);
            analyzeBody(node.body);
            m_loopScopes.pop_back();
        }
        else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            if (node.condition) analyzeExpr(*node.condition);
            m_loopScopes.push_back(LoopScope::Loop);
            analyzeBody(node.body);
            m_loopScopes.pop_back();
        }
        else if constexpr (std::is_same_v<T, ast::BreakExpr>) {
            checkLoopControl(expr.location, "break");
        }
        else if constexpr (std::is_same_v<T, ast::NextExpr>) {
            checkLoopControl(expr.location, "next");
        }
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            if (node.value) analyzeExpr(*node.value);
        }
        else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            if (!m_inFoulContext) {
                error(expr.location, "Cannot use 'spawn' in pure context");
            }
            m_symbols.pushScope(true);
            analyzeBody(node.body);
            m_symbols.popScope();
        }
        else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            if (!m_inFoulContext) {
                error(expr.location, "Cannot use 'receive' in pure context");
            }
            for (const auto& clause : node.clauses) {
                m_symbols.pushScope(m_inFoulContext);
                // When a sender binding is present, every message is a
                // {Payload, Sender} tuple; the sender name is in scope for
                // every clause (alongside the pattern vars).
                if (node.senderBinding && *node.senderBinding != "_") {
                    m_symbols.define(Symbol{*node.senderBinding, SymbolKind::Variable,
                                            false, false, true, expr.location});
                }
                for (const auto& pat : clause.patterns) {
                    if (pat) bindPatternVars(*pat, expr.location);
                }
                if (clause.guard && *clause.guard) {
                    m_inGuard = true;
                    analyzeExpr(**clause.guard);
                    m_inGuard = false;
                }
                if (clause.body) analyzeExpr(*clause.body);
                m_symbols.popScope();
            }
        }
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            m_symbols.pushScope(m_inFoulContext);
            for (const auto& param : node.params) {
                if (param.name != "_") {
                    m_symbols.define(Symbol{
                        param.name, SymbolKind::Variable, false, false, true, expr.location});
                }
            }
            m_loopScopes.push_back(LoopScope::Closure);
            analyzeBody(node.body);
            m_loopScopes.pop_back();
            m_symbols.popScope();
        }
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            analyzeBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            for (const auto& elem : node.elements) {
                if (elem) analyzeExpr(*elem);
            }
            if (node.rest) analyzeExpr(**node.rest);
        }
        else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            for (const auto& entry : node.entries) {
                if (entry.key) analyzeExpr(*entry.key);
                if (entry.value) analyzeExpr(*entry.value);
            }
        }
        else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            for (const auto& elem : node.elements) {
                if (elem) analyzeExpr(*elem);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            if (node.start) analyzeExpr(*node.start);
            if (node.end) analyzeExpr(*node.end);
        }
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            for (const auto& [_, val] : node.fields) {
                if (val) analyzeExpr(*val);
            }
        }
        else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            if (node.expr) analyzeExpr(*node.expr);
            if (node.condition) analyzeExpr(*node.condition);
        }
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            if (node.condition) analyzeExpr(*node.condition);
            if (node.thenExpr) analyzeExpr(*node.thenExpr);
            if (node.elseExpr) analyzeExpr(*node.elseExpr);
        }
        else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
            if (node.inner) analyzeExpr(*node.inner);
        }
        else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
            for (const auto& group : node.argGroups)
                for (const auto& arg : group)
                    if (arg && !std::holds_alternative<ast::CurryPlaceholder>(arg->kind))
                        analyzeExpr(*arg);
        }
        // Literals and other simple nodes need no analysis
    }, expr.kind);
}

auto Analyzer::computeTransitiveEffects(const ast::Program& program) -> void {
    // Build a call graph over top-level and module functions.
    // A function is "directly foul" if:
    //   - its AST isFoul flag is set, or
    //   - it contains spawn/receive, or
    //   - it calls a method on a kFoulModules module, or
    //   - it calls a symbol known to be foul.
    // Propagation: any function calling a (directly or transitively) foul
    // function is itself transitively foul.

    std::unordered_set<std::string> allFns;
    std::unordered_set<std::string> directlyFoul;
    m_callGraph.clear();

    // Walk an expression tree, collecting called function names and detecting
    // direct foul operations (spawn, receive, foul module method calls).
    std::function<void(const ast::Expr&, std::set<std::string>&, bool&)> walkExpr =
        [&](const ast::Expr& e, std::set<std::string>& calls, bool& hasFoulOp) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                if (allFns.count(n.name)) calls.insert(n.name);
                auto* sym = m_symbols.lookup(n.name);
                if (sym && sym->isFoul) hasFoulOp = true;
                for (const auto& a : n.args) if (a) walkExpr(*a, calls, hasFoulOp);
                for (const auto& [_, a] : n.namedArgs) if (a) walkExpr(*a, calls, hasFoulOp);
                if (n.block) walkExpr(**n.block, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                if (allFns.count(n.method)) calls.insert(n.method);
                if (n.receiver) {
                    walkExpr(*n.receiver, calls, hasFoulOp);
                    auto receiverName = [&]() -> std::string {
                        if (auto* up = std::get_if<ast::UpperIdentifier>(&n.receiver->kind))
                            return up->name;
                        return {};
                    }();
                    if (!receiverName.empty() && n.method != "Intrinsic") {
                        bool modFoul = false;
                        if (m_importedInterfaces) {
                            auto it = m_importedInterfaces->modules.find(receiverName);
                            if (it != m_importedInterfaces->modules.end())
                                modFoul = it->second.isFoul;
                        }
                        if (!modFoul) {
                            for (std::string_view mod : kFoulModules)
                                if (receiverName == mod) { modFoul = true; break; }
                        }
                        if (modFoul) hasFoulOp = true;
                    }
                }
                for (const auto& a : n.args) if (a) walkExpr(*a, calls, hasFoulOp);
                for (const auto& [_, a] : n.namedArgs) if (a) walkExpr(*a, calls, hasFoulOp);
                if (n.block) walkExpr(**n.block, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
                hasFoulOp = true;
                for (const auto& ex : n.body) if (ex) walkExpr(*ex, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
                hasFoulOp = true;
                for (const auto& cl : n.clauses) {
                    if (cl.guard && *cl.guard) walkExpr(**cl.guard, calls, hasFoulOp);
                    if (cl.body) walkExpr(*cl.body, calls, hasFoulOp);
                }
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                if (n.left) walkExpr(*n.left, calls, hasFoulOp);
                if (n.right) walkExpr(*n.right, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                if (n.operand) walkExpr(*n.operand, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                if (n.condition) walkExpr(*n.condition, calls, hasFoulOp);
                for (const auto& ex : n.thenBody) if (ex) walkExpr(*ex, calls, hasFoulOp);
                for (const auto& [c, b] : n.elifs) {
                    if (c) walkExpr(*c, calls, hasFoulOp);
                    for (const auto& ex : b) if (ex) walkExpr(*ex, calls, hasFoulOp);
                }
                if (n.elseBody)
                    for (const auto& ex : *n.elseBody) if (ex) walkExpr(*ex, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                if (n.subject) walkExpr(*n.subject, calls, hasFoulOp);
                for (const auto& cl : n.clauses) {
                    if (cl.guard && *cl.guard) walkExpr(**cl.guard, calls, hasFoulOp);
                    if (cl.body) walkExpr(*cl.body, calls, hasFoulOp);
                }
            } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                for (const auto& ex : n.body) if (ex) walkExpr(*ex, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                for (const auto& ex : n.body) if (ex) walkExpr(*ex, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::LetExpr>) {
                if (n.value) walkExpr(*n.value, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
                if (n.value) walkExpr(*n.value, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
                if (n.value) walkExpr(*n.value, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                if (n.condition) walkExpr(*n.condition, calls, hasFoulOp);
                if (n.thenExpr) walkExpr(*n.thenExpr, calls, hasFoulOp);
                if (n.elseExpr) walkExpr(*n.elseExpr, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                if (n.expr) walkExpr(*n.expr, calls, hasFoulOp);
                if (n.condition) walkExpr(*n.condition, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                if (n.value) walkExpr(*n.value, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                for (const auto& ex : n.body) if (ex) walkExpr(*ex, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                if (n.condition) walkExpr(*n.condition, calls, hasFoulOp);
                for (const auto& ex : n.body) if (ex) walkExpr(*ex, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::ListExpr>) {
                for (const auto& el : n.elements) if (el) walkExpr(*el, calls, hasFoulOp);
                if (n.rest) walkExpr(**n.rest, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::MapExpr>) {
                for (const auto& en : n.entries) {
                    if (en.key) walkExpr(*en.key, calls, hasFoulOp);
                    if (en.value) walkExpr(*en.value, calls, hasFoulOp);
                }
            } else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
                for (const auto& el : n.elements) if (el) walkExpr(*el, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
                if (n.inner) walkExpr(*n.inner, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
                for (const auto& group : n.argGroups)
                    for (const auto& arg : group)
                        if (arg && !std::holds_alternative<ast::CurryPlaceholder>(arg->kind))
                            walkExpr(*arg, calls, hasFoulOp);
            } else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
                for (const auto& [_, v] : n.fields) if (v) walkExpr(*v, calls, hasFoulOp);
            }
        }, e.kind);
    };

    // Pass 1: collect all function names.
    auto collectFns = [&](const auto& items) {
        for (const auto& item : items) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n) allFns.insert(n->name);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (n) {
                        if (n->isFoul) directlyFoul.insert(n->name);
                        for (const auto& mi : n->body) {
                            std::visit([&](const auto& mn) {
                                using MT = std::decay_t<decltype(mn)>;
                                if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                                    if (mn) allFns.insert(mn->name);
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
        for (const auto& clause : def.clauses)
            for (const auto& ex : clause.body)
                if (ex) walkExpr(*ex, calls, hasFoulOp);
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
                                if (mn) {
                                    auto& def = *mn;
                                    std::set<std::string> calls;
                                    bool hasFoulOp = def.isFoul || n->isFoul;
                                    for (const auto& clause : def.clauses)
                                        for (const auto& ex : clause.body)
                                            if (ex) walkExpr(*ex, calls, hasFoulOp);
                                    calls.erase(def.name);
                                    m_callGraph[def.name] = std::move(calls);
                                    if (hasFoulOp) directlyFoul.insert(def.name);
                                }
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

    // Re-walk guard expressions to reject transitive foul calls.
    std::function<void(const ast::Expr&)> checkGuard = [&](const ast::Expr& e) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                // Phase 1 already rejects calls where the symbol itself is
                // marked foul; here we catch pure-declared functions that
                // transitively reach foul operations.
                auto* sym = m_symbols.lookup(n.name);
                bool symbolFoul = sym && sym->isFoul;
                if (m_transitivelyFoul.count(n.name) && !symbolFoul) {
                    error(e.location,
                        "Cannot call function '" + n.name +
                        "' in a guard: it transitively calls foul operations");
                }
                for (const auto& a : n.args) if (a) checkGuard(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) checkGuard(*a);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                auto* sym = m_symbols.lookup(n.method);
                bool symbolFoul = sym && sym->isFoul;
                if (m_transitivelyFoul.count(n.method) && !symbolFoul) {
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

    // Walk match and receive clauses to find guards.
    std::function<void(const ast::Expr&)> findGuards = [&](const ast::Expr& e) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                if (n.subject) findGuards(*n.subject);
                for (const auto& cl : n.clauses) {
                    if (cl.guard && *cl.guard) checkGuard(**cl.guard);
                    if (cl.body) findGuards(*cl.body);
                }
            } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
                for (const auto& cl : n.clauses) {
                    if (cl.guard && *cl.guard) checkGuard(**cl.guard);
                    if (cl.body) findGuards(*cl.body);
                }
            } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                for (const auto& a : n.args) if (a) findGuards(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) findGuards(*a);
                if (n.block) findGuards(**n.block);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                if (n.receiver) findGuards(*n.receiver);
                for (const auto& a : n.args) if (a) findGuards(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) findGuards(*a);
                if (n.block) findGuards(**n.block);
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                if (n.left) findGuards(*n.left);
                if (n.right) findGuards(*n.right);
            } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                if (n.operand) findGuards(*n.operand);
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                if (n.condition) findGuards(*n.condition);
                for (const auto& ex : n.thenBody) if (ex) findGuards(*ex);
                for (const auto& [c, b] : n.elifs) {
                    if (c) findGuards(*c);
                    for (const auto& ex : b) if (ex) findGuards(*ex);
                }
                if (n.elseBody)
                    for (const auto& ex : *n.elseBody) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                for (const auto& ex : n.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                for (const auto& ex : n.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::LetExpr>) {
                if (n.value) findGuards(*n.value);
            } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
                if (n.value) findGuards(*n.value);
            } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                for (const auto& ex : n.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                if (n.condition) findGuards(*n.condition);
                for (const auto& ex : n.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
                for (const auto& ex : n.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                if (n.condition) findGuards(*n.condition);
                if (n.thenExpr) findGuards(*n.thenExpr);
                if (n.elseExpr) findGuards(*n.elseExpr);
            } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                if (n.expr) findGuards(*n.expr);
                if (n.condition) findGuards(*n.condition);
            } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                if (n.value) findGuards(*n.value);
            }
        }, e.kind);
    };

    // Walk all function bodies and main blocks.
    for (const auto& item : program.items) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (n) {
                    for (const auto& clause : n->clauses)
                        for (const auto& ex : clause.body)
                            if (ex) findGuards(*ex);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (n) {
                    for (const auto& mi : n->body) {
                        std::visit([&](const auto& mn) {
                            using MT = std::decay_t<decltype(mn)>;
                            if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                                if (mn) {
                                    for (const auto& clause : mn->clauses)
                                        for (const auto& ex : clause.body)
                                            if (ex) findGuards(*ex);
                                }
                            }
                        }, mi);
                    }
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (n) {
                    for (const auto& ex : n->body)
                        if (ex) findGuards(*ex);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (n) {
                    for (const auto& mi : n->body) {
                        std::visit([&](const auto& mn) {
                            using MT = std::decay_t<decltype(mn)>;
                            if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                                if (mn) {
                                    for (const auto& clause : mn->clauses)
                                        for (const auto& ex : clause.body)
                                            if (ex) findGuards(*ex);
                                }
                            }
                        }, mi);
                    }
                }
            }
        }, item);
    }
}

auto Analyzer::enrichEffectsFromResolvedCalls(const ast::Program& program) -> void {
    const auto& resolved = m_checker.resolvedCalls();
    if (resolved.empty()) return;

    // Walk expression trees looking for MethodCall nodes with foul resolved targets.
    std::function<bool(const ast::Expr&)> hasFoulResolved =
        [&](const ast::Expr& e) -> bool {
        return std::visit([&](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::MethodCall>) {
                auto it = resolved.find(&n);
                if (it != resolved.end() && it->second.isFoul) return true;
                if (n.receiver && hasFoulResolved(*n.receiver)) return true;
                for (const auto& a : n.args) if (a && hasFoulResolved(*a)) return true;
                for (const auto& [_, a] : n.namedArgs) if (a && hasFoulResolved(*a)) return true;
                if (n.block && hasFoulResolved(**n.block)) return true;
            } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                for (const auto& a : n.args) if (a && hasFoulResolved(*a)) return true;
                for (const auto& [_, a] : n.namedArgs) if (a && hasFoulResolved(*a)) return true;
                if (n.block && hasFoulResolved(**n.block)) return true;
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                if (n.left && hasFoulResolved(*n.left)) return true;
                if (n.right && hasFoulResolved(*n.right)) return true;
            } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                if (n.operand && hasFoulResolved(*n.operand)) return true;
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                if (n.condition && hasFoulResolved(*n.condition)) return true;
                for (const auto& ex : n.thenBody) if (ex && hasFoulResolved(*ex)) return true;
                for (const auto& [c, b] : n.elifs) {
                    if (c && hasFoulResolved(*c)) return true;
                    for (const auto& ex : b) if (ex && hasFoulResolved(*ex)) return true;
                }
                if (n.elseBody)
                    for (const auto& ex : *n.elseBody) if (ex && hasFoulResolved(*ex)) return true;
            } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                if (n.subject && hasFoulResolved(*n.subject)) return true;
                for (const auto& cl : n.clauses) {
                    if (cl.guard && *cl.guard && hasFoulResolved(**cl.guard)) return true;
                    if (cl.body && hasFoulResolved(*cl.body)) return true;
                }
            } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                for (const auto& ex : n.body) if (ex && hasFoulResolved(*ex)) return true;
            } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                for (const auto& ex : n.body) if (ex && hasFoulResolved(*ex)) return true;
            } else if constexpr (std::is_same_v<T, ast::LetExpr>) {
                if (n.value && hasFoulResolved(*n.value)) return true;
            } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
                if (n.value && hasFoulResolved(*n.value)) return true;
            } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                if (n.value && hasFoulResolved(*n.value)) return true;
            } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                for (const auto& ex : n.body) if (ex && hasFoulResolved(*ex)) return true;
            } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                if (n.condition && hasFoulResolved(*n.condition)) return true;
                for (const auto& ex : n.body) if (ex && hasFoulResolved(*ex)) return true;
            } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                if (n.condition && hasFoulResolved(*n.condition)) return true;
                if (n.thenExpr && hasFoulResolved(*n.thenExpr)) return true;
                if (n.elseExpr && hasFoulResolved(*n.elseExpr)) return true;
            } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                if (n.expr && hasFoulResolved(*n.expr)) return true;
                if (n.condition && hasFoulResolved(*n.condition)) return true;
            }
            return false;
        }, e.kind);
    };

    // Check each function for newly discovered foul resolved calls.
    bool newFoul = false;
    auto checkFn = [&](const ast::FunctionDef& def) {
        if (m_transitivelyFoul.count(def.name)) return;
        for (const auto& clause : def.clauses)
            for (const auto& ex : clause.body)
                if (ex && hasFoulResolved(*ex)) {
                    m_transitivelyFoul.insert(def.name);
                    newFoul = true;
                    return;
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
                auto* sym = m_symbols.lookup(n.name);
                bool symbolFoul = sym && sym->isFoul;
                bool newlyFoul = m_transitivelyFoul.count(n.name) && !priorFoul.count(n.name);
                if (newlyFoul && !symbolFoul) {
                    error(e.location,
                        "Cannot call function '" + n.name +
                        "' in a guard: it transitively calls foul operations");
                }
                for (const auto& a : n.args) if (a) checkGuard(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) checkGuard(*a);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                auto it = resolved.find(&n);
                if (it != resolved.end() && it->second.isFoul) {
                    error(e.location,
                        "Cannot call foul function '" + n.method +
                        "' in a guard");
                }
                auto* sym = m_symbols.lookup(n.method);
                bool symbolFoul = sym && sym->isFoul;
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

    std::function<void(const ast::Expr&)> findGuards = [&](const ast::Expr& e) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                if (n.subject) findGuards(*n.subject);
                for (const auto& cl : n.clauses) {
                    if (cl.guard && *cl.guard) checkGuard(**cl.guard);
                    if (cl.body) findGuards(*cl.body);
                }
            } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
                for (const auto& cl : n.clauses) {
                    if (cl.guard && *cl.guard) checkGuard(**cl.guard);
                    if (cl.body) findGuards(*cl.body);
                }
            } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                for (const auto& a : n.args) if (a) findGuards(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) findGuards(*a);
                if (n.block) findGuards(**n.block);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                if (n.receiver) findGuards(*n.receiver);
                for (const auto& a : n.args) if (a) findGuards(*a);
                for (const auto& [_, a] : n.namedArgs) if (a) findGuards(*a);
                if (n.block) findGuards(**n.block);
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                if (n.left) findGuards(*n.left);
                if (n.right) findGuards(*n.right);
            } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                if (n.condition) findGuards(*n.condition);
                for (const auto& ex : n.thenBody) if (ex) findGuards(*ex);
                for (const auto& [c, b] : n.elifs) {
                    if (c) findGuards(*c);
                    for (const auto& ex : b) if (ex) findGuards(*ex);
                }
                if (n.elseBody)
                    for (const auto& ex : *n.elseBody) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                for (const auto& ex : n.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                for (const auto& ex : n.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::LetExpr>) {
                if (n.value) findGuards(*n.value);
            } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
                if (n.value) findGuards(*n.value);
            } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                for (const auto& ex : n.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                if (n.condition) findGuards(*n.condition);
                for (const auto& ex : n.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                if (n.value) findGuards(*n.value);
            } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                if (n.condition) findGuards(*n.condition);
                if (n.thenExpr) findGuards(*n.thenExpr);
                if (n.elseExpr) findGuards(*n.elseExpr);
            } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                if (n.expr) findGuards(*n.expr);
                if (n.condition) findGuards(*n.condition);
            }
        }, e.kind);
    };

    for (const auto& item : program.items) {
        std::visit([&](const auto& n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (n) for (const auto& cl : n->clauses)
                    for (const auto& ex : cl.body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (n) for (const auto& mi : n->body) {
                    std::visit([&](const auto& mn) {
                        using MT = std::decay_t<decltype(mn)>;
                        if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                            if (mn) for (const auto& cl : mn->clauses)
                                for (const auto& ex : cl.body) if (ex) findGuards(*ex);
                        }
                    }, mi);
                }
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (n) for (const auto& ex : n->body) if (ex) findGuards(*ex);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (n) for (const auto& mi : n->body) {
                    std::visit([&](const auto& mn) {
                        using MT = std::decay_t<decltype(mn)>;
                        if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                            if (mn) for (const auto& cl : mn->clauses)
                                for (const auto& ex : cl.body) if (ex) findGuards(*ex);
                        }
                    }, mi);
                }
            }
        }, item);
    }
}

auto Analyzer::isInFoulContext() const -> bool {
    return m_inFoulContext;
}

auto Analyzer::checkPurity(const std::string& callee, SourceLocation loc) -> void {
    if (m_inFoulContext) return;
    auto* sym = m_symbols.lookup(callee);
    if (sym && sym->isFoul) {
        error(loc, "Cannot call foul function '" + callee + "' from pure context");
    }
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

auto Analyzer::warning(SourceLocation loc, const std::string& msg) -> void {
    m_diagnostics.push_back({Diagnostic::Level::Warning, loc, msg});
}

} // namespace kex::semantic
