#pragma once

#include "../ast/ast.hxx"
#include "../interpreter/value.hxx"
#include <string>

namespace kex::compiled {

// Turns a compile-time VALUE back into an AST expression that evaluates to it.
//
// This is what makes compile-time evaluation zero-cost: the work happens during
// compilation and only a literal survives into the program. Shared by
// compiled-block constants and (later) `.emit()` chain collapse.
//
// Produces AST literals, deliberately not IR: lowering then applies the
// backend's own representation invariants (on BEAM a String is a binary and a
// Char is {'Char', N}) exactly as it does for hand-written literals. Emitting
// IR directly here would mean re-implementing them.
//
// Returns nullptr and sets `error` for anything with no literal form — a
// closure, a process id, a resource handle. Integers round-trip through their
// decimal text so arbitrary-precision values survive.
auto valueToLiteral(const interpreter::ValuePtr& value,
                    const SourceLocation& location,
                    std::string& error) -> ast::ExprPtr;

} // namespace kex::compiled
