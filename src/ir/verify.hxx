#pragma once
// The IR verifier, as an IR pass: after lowering and the other passes, every
// call names as many arguments as its arity says, and every local call names a
// function its module defines at that arity. A violation used to surface only
// as erlc's error on generated Core Erlang, or as `undef` at run time.
#include "ir.hxx"

#include <string>
#include <vector>

namespace kex::ir {

// What is wrong with `module`, one line each; empty when it verifies.
auto verifyModule(const Module& module) -> std::vector<std::string>;

} // namespace kex::ir
