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
//
// Across modules, the callee is the standard library's: its build records,
// beside `kex_prelude.beam`, the functions a copy may be made of
// (`specializableFunctions`), and a call of one of them is specialised as a
// local one is. Whatever else the copy calls in that module is called there.
// The copy pins the caller to the standard library it was built with, which
// is part of the toolchain fingerprint.
#include "../ir.hxx"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace kex::ir {

auto specializeBlocks(Module& mod) -> void;

// The functions of `mod` another module could copy with a block in place: a
// function parameter used only as such, and every other function they call
// exported. Taken as lowered, before any module pass; a `match` on a
// constant (the backend) is decided.
auto specializableFunctions(const Module& mod) -> std::vector<FunDef>;

// Where those functions come from, by the module that defines them: read the
// first time a call might use one.
using ForeignFunctions = std::map<std::string, std::vector<FunDef>>;
auto setForeignFunctions(std::function<ForeignFunctions()> load) -> void;

} // namespace kex::ir
