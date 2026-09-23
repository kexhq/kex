#pragma once
// Hot-code-upgrade points (IR → IR).
//
// The BEAM keeps two versions of a module loaded. A process switches to the
// newest one only when it makes a fully-qualified call (`call 'mod':'f'`);
// a local call (`apply 'f'/N`) stays in whatever version the caller is
// running. A process that loops forever on local calls therefore never picks
// up reloaded code, and is killed outright when a second reload purges the
// version it is still executing.
//
// A long-running Kex process is a loop around `receive` — either a function
// that calls itself from its receive arms, or a `loop do receive ... end end`.
// This pass gives both an upgrade point:
//
//  - A `loop`/`while` whose body receives is lambda-lifted out of its
//    enclosing function into an exported module function, its captured
//    variables becoming trailing parameters.
//  - Inside any function (or lambda, or lifted loop) whose body receives,
//    every call to an exported function of the same module is emitted as a
//    remote call, so the next iteration after each message runs the newest
//    loaded code.
//
// Both are the ordinary Erlang idiom (`?MODULE:loop(State)`); functions that
// never receive keep their fast local calls.
#include "ir.hxx"

namespace kex::ir {

auto insertUpgradePoints(Module& mod) -> void;

} // namespace kex::ir
