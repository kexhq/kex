#pragma once
#include <cctype>
#include <optional>
#include <string>

// Structural traversal of the expression tree: the one place that knows which
// fields of which Expr kinds hold sub-expressions. A pass spells out only the
// kinds it treats specially and hands every other node here, so a new kind is
// one edit instead of one per walker (where a forgotten walker silently skips
// whatever the new kind contains).

#include "ast.hxx"

#include <type_traits>

namespace kex::ast {

namespace detail {

template <typename E>
using ExprPtrOf = std::conditional_t<std::is_const_v<E>, const ExprPtr, ExprPtr>;

} // namespace detail

// Calls `visit` on every expression directly nested in `expr`, in source
// order: operands, arguments, bodies, match/receive clause guards and bodies,
// rescue arms, block arguments. `visit` receives the owning pointer
// (`ExprPtr&`, or `const ExprPtr&` for a const `expr`), so a rewrite can
// replace a child in place. Null children are skipped. Type annotations,
// patterns and declarations are not expressions and are not visited.
template <typename E, typename Visit>
auto forEachChild(E& expr, Visit&& visit) -> void {
    using Ptr = detail::ExprPtrOf<E>;
    auto one = [&](Ptr& child) {
        if (child) visit(child);
    };
    auto maybe = [&](auto& child) {
        if (child) one(*child);
    };
    auto all = [&](auto& children) {
        for (auto& child : children) one(child);
    };
    auto clauses = [&](auto& list) {
        for (auto& clause : list) {
            maybe(clause.guard);
            one(clause.body);
        }
    };
    auto rescue = [&](auto& block) {
        clauses(block.clauses);
        all(block.catchAllBody);
        one(block.inlineReturnExpr);
    };
    std::visit(
        [&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, StringLiteral> ||
                          std::is_same_v<T, TaggedLiteral>) {
                all(node.values);
            } else if constexpr (std::is_same_v<T, MethodCall>) {
                one(node.receiver);
                all(node.args);
                for (auto& [_, value] : node.namedArgs) one(value);
                maybe(node.block);
            } else if constexpr (std::is_same_v<T, FunctionCall>) {
                all(node.args);
                for (auto& [_, value] : node.namedArgs) one(value);
                maybe(node.block);
            } else if constexpr (std::is_same_v<T, RecordConstruction>) {
                for (auto& field : node.fields) one(field.value);
            } else if constexpr (std::is_same_v<T, BinaryOp>) {
                one(node.left);
                one(node.right);
            } else if constexpr (std::is_same_v<T, UnaryOp>) {
                one(node.operand);
            } else if constexpr (std::is_same_v<T, TupleExpr>) {
                all(node.elements);
            } else if constexpr (std::is_same_v<T, ListExpr>) {
                all(node.elements);
                maybe(node.rest);
            } else if constexpr (std::is_same_v<T, MapExpr>) {
                for (auto& entry : node.entries) {
                    one(entry.key);
                    one(entry.value);
                }
            } else if constexpr (std::is_same_v<T, RangeExpr>) {
                one(node.start);
                one(node.end);
            } else if constexpr (std::is_same_v<T, IfExpr>) {
                one(node.condition);
                all(node.thenBody);
                for (auto& [condition, body] : node.elifs) {
                    one(condition);
                    all(body);
                }
                if (node.elseBody) all(*node.elseBody);
            } else if constexpr (std::is_same_v<T, MatchExpr>) {
                one(node.subject);
                clauses(node.clauses);
            } else if constexpr (std::is_same_v<T, ReceiveExpr>) {
                clauses(node.clauses);
                maybe(node.timeout);
                maybe(node.afterBody);
            } else if constexpr (std::is_same_v<T, LoopExpr> ||
                                 std::is_same_v<T, SpawnExpr> ||
                                 std::is_same_v<T, BlockExpr> ||
                                 std::is_same_v<T, UsingExpr>) {
                all(node.body);
            } else if constexpr (std::is_same_v<T, WhileExpr>) {
                one(node.condition);
                all(node.body);
            } else if constexpr (std::is_same_v<T, LetExpr> ||
                                 std::is_same_v<T, VarExpr> ||
                                 std::is_same_v<T, AssignExpr> ||
                                 std::is_same_v<T, ReturnExpr>) {
                one(node.value);
            } else if constexpr (std::is_same_v<T, Lambda>) {
                all(node.body);
                if (node.rescue) rescue(*node.rescue);
            } else if constexpr (std::is_same_v<T, ShorthandLambda>) {
                all(node.args);
            } else if constexpr (std::is_same_v<T, SpreadExpr>) {
                one(node.inner);
            } else if constexpr (std::is_same_v<T, TrailingIf>) {
                one(node.expr);
                one(node.condition);
            } else if constexpr (std::is_same_v<T, ThenElseExpr>) {
                one(node.condition);
                one(node.thenExpr);
                one(node.elseExpr);
            } else if constexpr (std::is_same_v<T, CurryExpr>) {
                for (auto& group : node.argGroups) all(group);
            } else if constexpr (std::is_same_v<T, TryExpr>) {
                one(node.operand);
            } else if constexpr (std::is_same_v<T, TryingExpr>) {
                all(node.body);
                rescue(node.rescue);
            } else if constexpr (std::is_same_v<T, GeneratedDecl>) {
                one(node.name);
            } else if constexpr (std::is_same_v<T, WithExpr>) {
                one(node.value);
                all(node.body);
            } else {
                // No sub-expressions: literals, identifiers, `this`,
                // break/next, the curry placeholder, error nodes. Listed so
                // a new kind that has some fails here, not silently.
                static_assert(
                    std::is_same_v<T, IntLiteral> || std::is_same_v<T, FloatLiteral> ||
                        std::is_same_v<T, CharLiteral> || std::is_same_v<T, BoolLiteral> ||
                        std::is_same_v<T, NoneLiteral> || std::is_same_v<T, AtomLiteral> ||
                        std::is_same_v<T, Identifier> || std::is_same_v<T, UpperIdentifier> ||
                        std::is_same_v<T, ThisExpr> || std::is_same_v<T, BreakExpr> ||
                        std::is_same_v<T, NextExpr> || std::is_same_v<T, CurryPlaceholder> ||
                        std::is_same_v<T, ErrorNode>,
                    "ast::forEachChild: a new Expr kind must say what it nests");
            }
        },
        expr.kind);
}

// Whether `predicate` holds for `expr` or any expression nested in it,
// pre-order; stops at the first hit.
template <typename Predicate>
auto anyExpr(const Expr& expr, Predicate&& predicate) -> bool {
    if (predicate(expr)) return true;
    bool found = false;
    forEachChild(expr, [&](const ExprPtr& child) {
        found = found || anyExpr(*child, predicate);
    });
    return found;
}

// Calls `onGuard(const Expr&)` on every match and receive clause guard in
// `expr`, at any depth. A guard's own sub-expressions are the callback's to
// walk: a guard nested inside a guard is not a separate one.
template <typename OnGuard>
auto forEachGuard(const Expr& expr, OnGuard&& onGuard) -> void {
    auto clauses = [&](const auto& list) {
        for (const auto& clause : list) {
            if (clause.guard && *clause.guard) onGuard(**clause.guard);
            if (clause.body) forEachGuard(*clause.body, onGuard);
        }
    };
    if (const auto* match = std::get_if<MatchExpr>(&expr.kind)) {
        if (match->subject) forEachGuard(*match->subject, onGuard);
        clauses(match->clauses);
        return;
    }
    if (const auto* receive = std::get_if<ReceiveExpr>(&expr.kind)) {
        clauses(receive->clauses);
        if (receive->timeout && *receive->timeout)
            forEachGuard(**receive->timeout, onGuard);
        if (receive->afterBody && *receive->afterBody)
            forEachGuard(**receive->afterBody, onGuard);
        return;
    }
    forEachChild(expr, [&](const ExprPtr& child) { forEachGuard(*child, onGuard); });
}

// Calls `visit(const FunctionDef&)` on every function `program` defines: at
// top level, in modules at any depth, and in `make` and visibility blocks
// inside either.
template <typename Visit>
auto forEachFunction(const Program& program, Visit&& visit) -> void {
    auto items = [&](auto& self, const auto& list) -> void {
        for (const auto& item : list)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<FunctionDef>>) {
                    if (node) visit(*node);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<ModuleDef>>) {
                    if (node) self(self, node->body);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<MakeDef>>) {
                    if (node) self(self, node->body);
                } else if constexpr (std::is_same_v<T, std::unique_ptr<VisibilityBlock>>) {
                    if (node) self(self, node->items);
                }
            }, item);
    };
    items(items, program.items);
}

// Calls `visit(const Expr&)` on each top-level expression of every body that
// runs: function clauses (wherever `forEachFunction` finds them) and main
// blocks.
template <typename Visit>
auto forEachBodyExpr(const Program& program, Visit&& visit) -> void {
    forEachFunction(program, [&](const FunctionDef& function) {
        for (const auto& clause : function.clauses)
            for (const auto& expr : clause.body)
                if (expr) visit(*expr);
    });
    for (const auto& item : program.items)
        if (const auto* main = std::get_if<std::unique_ptr<MainBlock>>(&item);
            main && *main)
            for (const auto& expr : (*main)->body)
                if (expr) visit(*expr);
}

} // namespace kex::ast

namespace kex::ast {

// The module path a receiver chain spells out — `App.Helper` for the
// receiver of `App.Helper.hint(...)` — or nullopt when it is not one: every
// segment a capitalized, argument-less name. A lowercase segment is a call:
// taken for a module, `App.Helper.hint.upcase` (and `"${App.Helper.hint}"`,
// which desugars the same way) recorded `App.Helper.hint` as the module to
// load, and `App.Helper` itself was never found.
inline auto writtenModulePath(const Expr& receiver) -> std::optional<std::string> {
    if (const auto* root = std::get_if<UpperIdentifier>(&receiver.kind))
        return root->name;
    const auto* segment = std::get_if<MethodCall>(&receiver.kind);
    if (!segment || !segment->receiver || !segment->args.empty() ||
        !segment->namedArgs.empty() || segment->block || segment->method.empty() ||
        !std::isupper(static_cast<unsigned char>(segment->method.front())))
        return std::nullopt;
    auto parent = writtenModulePath(*segment->receiver);
    return parent ? std::optional<std::string>{*parent + "." + segment->method}
                  : std::nullopt;
}

} // namespace kex::ast
