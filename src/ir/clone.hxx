#pragma once

// Copying lowering IR, and the variables an expression reads from outside
// it: what a pass needs to move code from one function into another.

#include "ir.hxx"

#include <functional>
#include <set>
#include <string>

namespace kex::ir {

// How a copy names a variable: every binder and every use of the name goes
// through it, so a consistent renaming keeps the copy's scoping intact.
using Rename = std::function<std::string(const std::string&)>;

// A deep copy of `e` (and of a pattern), every variable name renamed by
// `rename` (kept when it is null).
auto cloneExpr(const Expr& e, const Rename& rename = {}) -> ExprPtr;
auto clonePattern(const Pattern& p, const Rename& rename = {}) -> PatternPtr;

// The names a pattern binds.
auto bindPattern(const Pattern& p, std::set<std::string>& bound) -> void;

// The variables `e` reads that are bound outside it (not in `bound`).
auto collectFree(const Expr& e, std::set<std::string> bound,
                 std::set<std::string>& out) -> void;

// Whether an early `return` in `e` leaves the function it is written in (one
// inside a nested lambda leaves that lambda). Code holding one cannot move
// into another function: the emitter catches a `return` per function.
auto returnsEarly(const Expr& e) -> bool;

} // namespace kex::ir
