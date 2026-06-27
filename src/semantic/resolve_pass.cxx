#include "resolve_pass.hxx"
#include "analyzer.hxx"
#include <algorithm>
#include <variant>

namespace kex::semantic {

// Simple edit distance for "did you mean X?" suggestions.
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
    m_state = db.fileState(file);
    if (!m_state) return;

    for (const auto& item : m_state->ast.items) {
        std::visit([this](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, ast::FunctionDef>) {
                pushScope();
                for (const auto& clause : ptr->clauses) {
                    // Bind param names into local scope
                    for (const auto& p : clause.params) {
                        if (p.name) defineLocal(*p.name);
                        if (p.pattern) resolvePattern(**p.pattern);
                    }
                    resolveBody(clause.body);
                }
                popScope();
            } else if constexpr (std::is_same_v<T, ast::ModuleDef>) {
                for (const auto& mitem : ptr->body) {
                    std::visit([this](const auto& mptr) {
                        using MT = std::decay_t<decltype(*mptr)>;
                        if constexpr (std::is_same_v<MT, ast::FunctionDef>) {
                            pushScope();
                            for (const auto& clause : mptr->clauses) {
                                for (const auto& p : clause.params)
                                    if (p.name) defineLocal(*p.name);
                                resolveBody(clause.body);
                            }
                            popScope();
                        } else if constexpr (std::is_same_v<MT, ast::MakeDef>) {
                            for (const auto& bitem : mptr->body) {
                                std::visit([this](const auto& bptr) {
                                    using BT = std::decay_t<decltype(*bptr)>;
                                    if constexpr (std::is_same_v<BT, ast::FunctionDef>) {
                                        pushScope();
                                        for (const auto& clause : bptr->clauses) {
                                            for (const auto& p : clause.params)
                                                if (p.name) defineLocal(*p.name);
                                            resolveBody(clause.body);
                                        }
                                        popScope();
                                    }
                                }, bitem);
                            }
                        }
                    }, mitem);
                }
            } else if constexpr (std::is_same_v<T, ast::MakeDef>) {
                for (const auto& bitem : ptr->body) {
                    std::visit([this](const auto& bptr) {
                        using BT = std::decay_t<decltype(*bptr)>;
                        if constexpr (std::is_same_v<BT, ast::FunctionDef>) {
                            pushScope();
                            for (const auto& clause : bptr->clauses) {
                                for (const auto& p : clause.params)
                                    if (p.name) defineLocal(*p.name);
                                resolveBody(clause.body);
                            }
                            popScope();
                        }
                    }, bitem);
                }
            } else if constexpr (std::is_same_v<T, ast::MainBlock>) {
                pushScope();
                resolveBody(ptr->body);
                popScope();
            }
        }, item);
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
            if (node.name != "_" && !isKnown(node.name)) {
                auto hint = suggest(node.name);
                std::string msg = "Undefined name: `" + node.name + "`";
                if (!hint.empty()) msg += " — did you mean `" + hint + "`?";
                error(expr.location, msg);
            }
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            if (!isKnown(node.name)) {
                auto hint = suggest(node.name);
                std::string msg = "Undefined function: `" + node.name + "`";
                if (!hint.empty()) msg += " — did you mean `" + hint + "`?";
                error(expr.location, msg);
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
            if (!isKnown(node.name)) {
                auto hint = suggest(node.name);
                std::string msg = "Undefined function: `" + node.name + "`";
                if (!hint.empty()) msg += " — did you mean `" + hint + "`?";
                error(expr.location, msg);
            }
            for (const auto& group : node.argGroups)
                for (const auto& arg : group)
                    if (arg) resolveExpr(*arg);
        }
        // Literals, UpperIdentifier, ThisExpr, BreakExpr, NextExpr,
        // CurryPlaceholder, ShorthandLambda: nothing to resolve
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
                    defineLocal(field.name); // shorthand binding
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
        // LiteralPattern, WildcardPattern: no bindings
    }, pat.kind);
}

auto ResolvePass::isKnown(const std::string& name) const -> bool {
    // Check local scope stack (innermost first)
    for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
        if (it->count(name)) return true;
    }
    // Check file-level collected symbols
    if (m_state) {
        for (const auto& sym : m_state->symbols) {
            if (sym.name == name) return true;
        }
    }
    // Check stdlib
    if (m_stdlib.lookup(name)) return true;
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
