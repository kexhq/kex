#pragma once

// Where the prebuilt runtime lives, and the prelude's interfaces read from it.

#include "../semantic/imported_interfaces.hxx"

#include <string>

namespace kex {

// The directory holding the prebuilt runtime BEAMs (`kex_io.beam`, the
// compiled stdlib unit): KEX_RUNTIME_DIR, else beside the executable, else
// the wasm build's baked-in or mounted copy. "" when none is found.
auto prebuiltRuntimeBeamDir() -> std::string;

// The prelude's interfaces, read from that runtime (from the stdlib sources
// when there is none). Cached per process.
auto defaultPreludeInterfaces() -> const semantic::ImportedInterfaces&;

} // namespace kex
