#pragma once

#include "../ast/ast.hxx"
#include "../interpreter/value.hxx"
#include <string>
#include <unordered_map>
#include <vector>

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
//
// `layouts` maps a record type to its DECLARED field order (see
// Evaluator::recordFieldOrder). A record value's own fields are an unordered
// map, and on BEAM a record is a tuple — so field order is ABI, and a reified
// record must be written in declaration order rather than in whatever order
// happens to be convenient. A type missing from `layouts` falls back to
// alphabetical, which is at least reproducible across builds.
using RecordLayouts =
    std::unordered_map<std::string, std::vector<std::string>>;

auto valueToLiteral(const interpreter::ValuePtr& value,
                    const SourceLocation& location,
                    std::string& error,
                    const RecordLayouts* layouts = nullptr) -> ast::ExprPtr;

} // namespace kex::compiled
