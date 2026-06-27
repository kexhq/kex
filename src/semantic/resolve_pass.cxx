#include "resolve_pass.hxx"
#include "analyzer.hxx"
#include <algorithm>
#include <cctype>
#include <variant>

namespace kex::semantic {

static auto editDistance(const std::string& a, const std::string& b) -> int {
    const int m = static_cast<int>(a.size());
    const int n = static_cast<int>(b.size());
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1));
    for (int i = 0; i <= m; i++) dp[i][0] = i;
    for (int j = 0; j <= n; j++) dp[0][j] = j;
    for (int i = 1; i <= m; i++)
        for (int j = 1; j <= n; j++)
            dp[i][j] = (a[i-1] == b[j-1])
                ? dp[i-1][j-1]
                : 1 + std::min({dp[i-1][j], dp[i][j-1], dp[i-1][j-1]});
    return dp[m][n];
}

ResolvePass::ResolvePass() : m_stdlib(SignatureTable::withStdlib()) {}

auto ResolvePass::run(SemanticDB& db, const std::string& file) -> void {
    m_db = &db;
    m_state = db.fileState(file);
    if (!m_state) return;

    // Persistent module scope: synthetic top-level let bindings land here so
    // they remain visible to all subsequent top-level items in the same file.
    pushScope();

    for (const auto& item : m_state->ast.items) {
        std::visit([this](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, ast::FunctionDef>) {
                resolveFunctionDef(*ptr);
            } else if constexpr (std::is_same_v<T, ast::ModuleDef>) {
                for (const auto& mitem : ptr->body) {
                    std::visit([this](const auto& mptr) {
                        using MT = std::decay_t<decltype(*mptr)>;
                        if constexpr (std::is_same_v<MT, ast::FunctionDef>) {
                            resolveFunctionDef(*mptr);
                        } else if constexpr (std::is_same_v<MT, ast::MakeDef>) {
                            resolveMakeFns(mptr->body);
                        } else if constexpr (std::is_same_v<MT, ast::VisibilityBlock>) {
                            for (const auto& vitem : mptr->items) {
                                std::visit([this](const auto& vptr) {
                                    using VT = std::decay_t<decltype(*vptr)>;
                                    if constexpr (std::is_same_v<VT, ast::FunctionDef>) {
                                        resolveFunctionDef(*vptr);
                                    } else if constexpr (std::is_same_v<VT, ast::MakeDef>) {
                                        resolveMakeFns(vptr->body);
                                    }
                                }, vitem);
                            }
                        }
                    }, mitem);
                }
            } else if constexpr (std::is_same_v<T, ast::MakeDef>) {
                resolveMakeFns(ptr->body);
            } else if constexpr (std::is_same_v<T, ast::MainBlock>) {
                if (ptr->synthetic) {
                    resolveBody(ptr->body);
                } else {
                    pushScope();
                    for (const auto& p : ptr->params)
                        if (p.name) defineLocal(*p.name);
                    resolveBody(ptr->body);
                    popScope();
                }
            }
        }, item);
    }

    popScope();
}

auto ResolvePass::resolveFunctionDef(const ast::FunctionDef& def) -> void {
    pushScope();
    for (const auto& clause : def.clauses) {
        bindParams(clause.params);
        resolveBody(clause.body);
    }
    popScope();
}

auto ResolvePass::resolveMakeFns(const std::vector<std::variant<
        std::unique_ptr<ast::FunctionDef>,
        std::unique_ptr<ast::TypeAnnotation>,
        std::unique_ptr<ast::VisibilityBlock>>>& body) -> void {
    for (const auto& item : body) {
        std::visit([this](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, ast::FunctionDef>) {
                resolveFunctionDef(*ptr);
            } else if constexpr (std::is_same_v<T, ast::VisibilityBlock>) {
                for (const auto& vitem : ptr->items) {
                    std::visit([this](const auto& vptr) {
                        using VT = std::decay_t<decltype(*vptr)>;
                        if constexpr (std::is_same_v<VT, ast::FunctionDef>) {
                            resolveFunctionDef(*vptr);
                        }
                    }, vitem);
                }
            }
        }, item);
    }
}

auto ResolvePass::bindParams(const std::vector<ast::Param>& params) -> void {
    for (const auto& p : params) {
        if (p.name) defineLocal(*p.name);
        if (p.pattern) resolvePattern(**p.pattern);
    }
}

auto ResolvePass::resolveBody(const std::vector<ast::ExprPtr>& body) -> void {
    for (const auto& expr : body) {
        if (expr) resolveExpr(*expr);
    }
}

auto ResolvePass::resolveExpr(const ast::Expr& expr) -> void {
    std::visit([this, &expr](const auto& node) {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::Identifier>) {
            if (node.name != "_") {
                if (!isKnown(node.name)) {
                    auto hint = suggest(node.name);
                    std::string msg = "Undefined name: `" + node.name + "`";
                    if (!hint.empty()) msg += " — did you mean `" + hint + "`?";
                    error(expr.location, msg);
                } else {
                    recordRef(node.name, expr.location);
                }
            }
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            // Uppercase names are constructors (Just, Ok, Error…) — the
            // TypeChecker validates them; skip undefined check here.
            if (!node.name.empty()
                    && std::islower(static_cast<unsigned char>(node.name[0]))) {
                if (!isKnown(node.name)) {
                    auto hint = suggest(node.name);
                    std::string msg = "Undefined function: `" + node.name + "`";
                    if (!hint.empty()) msg += " — did you mean `" + hint + "`?";
                    error(expr.location, msg);
                } else {
                    recordRef(node.name, expr.location);
                }
            }
            for (const auto& arg : node.args)
                if (arg) resolveExpr(*arg);
            for (const auto& [_, arg] : node.namedArgs)
                if (arg) resolveExpr(*arg);
            if (node.block && *node.block) resolveExpr(**node.block);
        }
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            if (node.receiver) resolveExpr(*node.receiver);
            for (const auto& arg : node.args)
                if (arg) resolveExpr(*arg);
            for (const auto& [_, arg] : node.namedArgs)
                if (arg) resolveExpr(*arg);
            if (node.block && *node.block) resolveExpr(**node.block);
        }
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            if (node.left) resolveExpr(*node.left);
            if (node.right) resolveExpr(*node.right);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            if (node.operand) resolveExpr(*node.operand);
        }
        else if constexpr (std::is_same_v<T, ast::ErrorPropagate>) {
            if (node.inner) resolveExpr(*node.inner);
        }
        else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            for (const auto& e : node.elements)
                if (e) resolveExpr(*e);
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            for (const auto& e : node.elements)
                if (e) resolveExpr(*e);
            if (node.rest) resolveExpr(**node.rest);
        }
        else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            for (const auto& entry : node.entries) {
                if (entry.key) resolveExpr(*entry.key);
                if (entry.value) resolveExpr(*entry.value);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            if (node.start) resolveExpr(*node.start);
            if (node.end) resolveExpr(*node.end);
        }
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            if (node.condition) resolveExpr(*node.condition);
            resolveBody(node.thenBody);
            for (const auto& [cond, body] : node.elifs) {
                if (cond) resolveExpr(*cond);
                resolveBody(body);
            }
            if (node.elseBody) resolveBody(*node.elseBody);
        }
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            if (node.subject) resolveExpr(*node.subject);
            for (const auto& clause : node.clauses) {
                pushScope();
                for (const auto& pat : clause.patterns)
                    if (pat) resolvePattern(*pat);
                if (clause.guard) resolveExpr(**clause.guard);
                if (clause.body) resolveExpr(*clause.body);
                popScope();
            }
        }
        else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            for (const auto& clause : node.clauses) {
                pushScope();
                for (const auto& pat : clause.patterns)
                    if (pat) resolvePattern(*pat);
                if (clause.guard) resolveExpr(**clause.guard);
                if (clause.body) resolveExpr(*clause.body);
                popScope();
            }
            if (node.timeout) resolveExpr(**node.timeout);
            if (node.afterBody) resolveExpr(**node.afterBody);
        }
        else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            resolveBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            if (node.condition) resolveExpr(*node.condition);
            resolveBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            // Pre-define name for local recursive lambdas so the body can
            // refer to itself (e.g. `let loop(s) do ... loop(s+1) end`).
            if (node.pattern && node.value) {
                if (const auto* vp = std::get_if<ast::VarPattern>(&node.pattern->kind)) {
                    bool isFunc = std::holds_alternative<ast::Lambda>(node.value->kind)
                               || std::holds_alternative<ast::BlockExpr>(node.value->kind);
                    if (isFunc) defineLocal(vp->name);
                }
            }
            if (node.value) resolveExpr(*node.value);
            if (node.pattern) resolvePattern(*node.pattern);
        }
        else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            if (node.value) resolveExpr(*node.value);
            defineLocal(node.name);
        }
        else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            if (node.value) resolveExpr(*node.value);
            if (!isKnown(node.name)) {
                error(expr.location, "Undefined variable: `" + node.name + "`");
            }
        }
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            if (node.value) resolveExpr(*node.value);
        }
        else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            resolveBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            pushScope();
            for (const auto& p : node.params) defineLocal(p.name);
            resolveBody(node.body);
            popScope();
        }
        else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
            if (node.inner) resolveExpr(*node.inner);
        }
        else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            if (node.expr) resolveExpr(*node.expr);
            if (node.condition) resolveExpr(*node.condition);
        }
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            if (node.condition) resolveExpr(*node.condition);
            if (node.thenExpr) resolveExpr(*node.thenExpr);
            if (node.elseExpr) resolveExpr(*node.elseExpr);
        }
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            resolveBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            for (const auto& [_, v] : node.fields)
                if (v) resolveExpr(*v);
        }
        else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
            if (!node.name.empty()
                    && std::islower(static_cast<unsigned char>(node.name[0]))) {
                if (!isKnown(node.name)) {
                    auto hint = suggest(node.name);
                    std::string msg = "Undefined function: `" + node.name + "`";
                    if (!hint.empty()) msg += " — did you mean `" + hint + "`?";
                    error(expr.location, msg);
                } else {
                    recordRef(node.name, expr.location);
                }
            }
            for (const auto& group : node.argGroups)
                for (const auto& arg : group)
                    if (arg) resolveExpr(*arg);
        }
        // Literals, UpperIdentifier, ThisExpr, BreakExpr, NextExpr,
        // CurryPlaceholder, ShorthandLambda, ErrorNode: nothing to resolve
    }, expr.kind);
}

auto ResolvePass::resolvePattern(const ast::Pattern& pat) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::VarPattern>) {
            if (node.name != "_") defineLocal(node.name);
        } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) resolvePattern(*node.inner);
        } else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            for (const auto& arg : node.args)
                if (arg) resolvePattern(*arg);
        } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            for (const auto& field : node.fields) {
                if (field.pattern)
                    resolvePattern(**field.pattern);
                else
                    defineLocal(field.name);
            }
        } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            for (const auto& e : node.elements)
                if (e) resolvePattern(*e);
            if (node.rest) resolvePattern(**node.rest);
        } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            for (const auto& e : node.elements)
                if (e) resolvePattern(*e);
        } else if constexpr (std::is_same_v<T, ast::RangePattern>) {
            if (node.start) resolvePattern(*node.start);
            if (node.end) resolvePattern(*node.end);
        }
    }, pat.kind);
}

auto ResolvePass::recordRef(const std::string& name, SourceLocation loc) -> void {
    if (!m_db || !m_state) return;
    // Only record references to top-level/module symbols, not local variables
    // (local vars live in m_scopes and have no SymbolInfo entry).
    for (const auto& scope : m_scopes)
        if (scope.count(name)) return;
    if (auto* sym = m_db->findSymbol(name, m_state->path))
        sym->references.push_back(loc);
}

auto ResolvePass::isKnown(const std::string& name) const -> bool {
    // Local scope stack (innermost first)
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        if (it->count(name)) return true;
    }
    // Current file's collected symbols
    if (m_state) {
        for (const auto& sym : m_state->symbols) {
            if (sym.name == name) return true;
        }
    }
    // Stdlib function signatures
    if (m_stdlib.lookup(name)) return true;
    // Prelude and other indexed files (e.g. src/prelude/*.kex loaded into DB)
    if (m_db && m_db->isGloballyKnown(name)) return true;
    return false;
}

auto ResolvePass::suggest(const std::string& name) const -> std::string {
    constexpr int kMaxDist = 3;
    std::string best;
    int bestDist = kMaxDist + 1;

    auto check = [&](const std::string& candidate) {
        if (candidate == name) return;
        int d = editDistance(name, candidate);
        if (d < bestDist) { bestDist = d; best = candidate; }
    };

    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it)
        for (const auto& s : *it) check(s);
    if (m_state)
        for (const auto& sym : m_state->symbols) check(sym.name);

    return best;
}

auto ResolvePass::pushScope() -> void {
    m_scopes.emplace_back();
}

auto ResolvePass::popScope() -> void {
    if (!m_scopes.empty()) m_scopes.pop_back();
}

auto ResolvePass::defineLocal(const std::string& name) -> void {
    if (m_scopes.empty()) m_scopes.emplace_back();
    m_scopes.back().insert(name);
}

auto ResolvePass::error(SourceLocation loc, const std::string& msg) -> void {
    if (!m_state) return;
    m_state->diagnostics.push_back(Diagnostic{Diagnostic::Level::Error, loc, msg});
}

} // namespace kex::semantic
