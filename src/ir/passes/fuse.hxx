#pragma once
// List fusion (IR → IR).
//
// `xs.map { |x| x * 2 }.filter { |x| x > 3 }.reduce(0) { |a, x| a + x }` built
// two lists only to take them apart again: after specialize-blocks each stage
// is a copy that walks a list, and each but the last builds the next. Where
// one copy's result is only the list another walks, this pass makes one
// function of the two that walks the first list once, handing each element
// the first would have put in its list straight to the second.
//
// Only copies whose blocks are pure (FunDef::pureCopy): fused, the second
// stage's block runs between the first stage's, element by element, where it
// ran after all of them, and with nothing to observe the difference only the
// time it takes changes.
//
// What a copy is, it says by its shape, not by its name. It walks its first
// parameter, a list: `[]` in one clause, `[head | rest]` in another, and
// `rest` read only as the first argument of its own recursion.
// - A producer answers `[]` for the empty list and, for the rest, either
//   `[e | t]` with `t` its own recursion, or its recursion alone: one or no
//   element per element it walks (`map`, `filter`).
// - Any copy so shaped consumes, when its recursion is in tail position or
//   is a producer's (`reduce`, `each`, another `map`).
#include "../ir.hxx"

namespace kex::ir {

auto fuseLists(Module& mod) -> void;

} // namespace kex::ir
