#pragma once

namespace kex {

// Increment when the private Kex.Intrinsic contract changes incompatibly.
inline constexpr int kIntrinsicAbiVersion = 1;

// Increment when emitted BEAM value representation or calling conventions
// change without requiring a KexI schema bump.
inline constexpr int kBeamRepresentationVersion = 1;

} // namespace kex
