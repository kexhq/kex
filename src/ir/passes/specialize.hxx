#pragma once
// Block specialisation (IR → IR).
//
// A function that takes a block and only ever calls it — or hands it to
// itself unchanged — pays a closure call per use: `eachPair(items) { |a, b|
// ... }` applies the block once per pair. Where a call passes a block written
// in place, this pass emits a copy of the callee with the block's body in
// place of each call of it, and the call goes to the copy. The block's
// captured variables become trailing parameters of the copy; its recursive
// calls go to the copy too, so a callee that recurses over its input is a
// plain local loop.
//
// Within one module, and only where nothing else can observe the block: the
// callee reads its parameter nowhere but as the function it applies (at the
// block's own arity) or as the same argument of a call to itself, and the
// block has no early `return` (which would leave the copy instead).
#include "../ir.hxx"

namespace kex::ir {

auto specializeBlocks(Module& mod) -> void;

} // namespace kex::ir
