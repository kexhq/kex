#pragma once

#include "ir.hxx"

#include <string>
#include <vector>

// What a lowered module must hold before it is emitted (kexhq/kex#709). A
// broken one is a bug in lowering or in a pass after it, said here by name
// rather than by `erlc` refusing the Core Erlang, or by the module failing
// when it runs:
//
// - every clause takes as many parameters as its function's arity;
// - no variable is read that nothing bound (`This` in a clause matching its
//   receiver by shape, kexhq/kex#803);
// - a call passes as many arguments as its arity says, and a local one names
//   a function the module defines at that arity;
// - no two functions share a name and an arity.
namespace kex::ir {

// The problems, one line each, naming the function; none when it holds.
auto verify(const Module& module) -> std::vector<std::string>;

} // namespace kex::ir
