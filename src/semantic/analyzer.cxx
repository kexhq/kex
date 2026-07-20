#include "analyzer.hxx"

#include <algorithm>

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

    // Phase 2: type checking
    m_checker.check(program, m_diagnostics);

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
            if (!m_inFoulContext && !isInspect && !receiverName.empty()) {
                for (std::string_view mod : kFoulModules) {
                    if (receiverName == mod) {
                        error(expr.location,
                            "Cannot call foul function '" + receiverName + "." +
                            node.method + "' from pure context");
                        break;
                    }
                }
            }
            if (m_inGuard && !isInspect && !receiverName.empty()) {
                for (std::string_view mod : kFoulModules) {
                    if (receiverName == mod) {
                        error(expr.location,
                            "Cannot call foul function '" + receiverName + "." +
                            node.method + "' in a guard");
                        break;
                    }
                }
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
