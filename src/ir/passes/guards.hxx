#pragma once
// The `guards` IR pass: a match lowered from source (`GuardExpansion::Source`
// or `Rescue`) gets its `when` guards turned into nested matches, since BEAM
// guards cannot hold arbitrary calls, and a single-subject match its
// no-clause failure.
#include "../ir.hxx"

namespace kex::ir {

auto expandGuards(Module& module) -> void;

} // namespace kex::ir
