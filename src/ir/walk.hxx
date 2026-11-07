#pragma once

// Structural traversal of the lowering IR: the one place that knows which
// fields of which node kinds hold sub-expressions. A pass spells out only the
// kinds it treats specially (a Lambda's own return scope, a TryCatch that
// catches its body) and hands every other node here. Hand-written copies of
// this list drifted apart — several skipped Receive, FieldGet or TryThrow, so
// a `return` or `.try` inside a receive clause went unseen.

#include "ir.hxx"

#include <type_traits>

namespace kex::ir {

// Calls `f` on every direct child of `e` (`ExprPtr&`, or `const ExprPtr&` for
// a const `e`), without interpreting scopes: a walker decides for itself
// which children to enter. Match and TryCatch clause guards are children;
// receive clauses carry none.
template <typename E, typename F>
auto forEachChild(E& e, F&& f) -> void {
    std::visit([&](auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, Intrinsic> || std::is_same_v<T, Call> ||
                      std::is_same_v<T, Construct>) {
            for (auto& a : n.args) f(a);
        } else if constexpr (std::is_same_v<T, CallIndirect>) {
            f(n.callee);
            for (auto& a : n.args) f(a);
        } else if constexpr (std::is_same_v<T, Let>) {
            f(n.value);
            f(n.body);
        } else if constexpr (std::is_same_v<T, Seq>) {
            for (auto& x : n.exprs) f(x);
        } else if constexpr (std::is_same_v<T, Match>) {
            for (auto& s : n.subjects) f(s);
            for (auto& c : n.clauses) {
                if (c.guard) f(*c.guard);
                f(c.body);
            }
        } else if constexpr (std::is_same_v<T, MakeTuple>) {
            for (auto& x : n.elements) f(x);
        } else if constexpr (std::is_same_v<T, MakeList>) {
            for (auto& x : n.elements) f(x);
            if (n.rest) f(*n.rest);
        } else if constexpr (std::is_same_v<T, FieldGet>) {
            f(n.record);
        } else if constexpr (std::is_same_v<T, Lambda>) {
            f(n.body);
        } else if constexpr (std::is_same_v<T, Return>) {
            f(n.value);
        } else if constexpr (std::is_same_v<T, TryThrow>) {
            f(n.error);
        } else if constexpr (std::is_same_v<T, TryCatch>) {
            f(n.body);
            for (auto& c : n.clauses) {
                if (c.guard) f(*c.guard);
                f(c.body);
            }
        } else if constexpr (std::is_same_v<T, LetRec>) {
            f(n.funBody);
            f(n.contBody);
        } else if constexpr (std::is_same_v<T, Receive>) {
            for (auto& c : n.clauses) f(c.body);
            if (n.timeout) f(*n.timeout);
            if (n.afterBody) f(*n.afterBody);
        } else {
            static_assert(std::is_same_v<T, Lit> || std::is_same_v<T, Var>,
                          "ir::forEachChild: a new node kind must say what it nests");
        }
    }, e.node);
}

// Whether any direct child of `e` satisfies `predicate` (null children never
// do). The usual tail of a recursive "does this subtree contain …" walk.
template <typename Predicate>
auto anyChild(const Expr& e, Predicate&& predicate) -> bool {
    bool found = false;
    forEachChild(e, [&](const ExprPtr& child) {
        found = found || (child && predicate(child));
    });
    return found;
}

} // namespace kex::ir
