#pragma once

// Names the compiler itself depends on, spelled once. A comparison against
// one of these is a comparison against the language's own vocabulary, not a
// magic string: renaming the runtime prelude module, say, is one edit here.

#include <string>

namespace kex::names {

// The runtime module the prelude compiles to; the fallback provider of every
// receiver method a program does not define itself.
inline const std::string kPreludeModule = "kex_prelude";

} // namespace kex::names
