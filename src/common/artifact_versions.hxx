#pragma once

namespace kex {

// Increment when the private Kex.Intrinsic contract changes incompatibly.
inline constexpr int kIntrinsicAbiVersion = 1;

// Increment when emitted BEAM value representation or calling conventions
// change without requiring a KexI schema bump.
// 2: a record declared inside a `module` block is tagged with its qualified
//    identity ({'Parsing.Input', ...}), so tuples from an older artifact no
//    longer match this toolchain's patterns and accessors.
inline constexpr int kBeamRepresentationVersion = 2;

} // namespace kex
