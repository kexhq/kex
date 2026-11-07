#pragma once

// Names the compiler itself depends on, spelled once. A comparison against
// one of these is a comparison against the language's own vocabulary, not a
// magic string: renaming the runtime prelude module, say, is one edit here.

#include <string>

namespace kex::names {

// The runtime module the prelude compiles to; the fallback provider of every
// receiver method a program does not define itself.
inline const std::string kPreludeModule = "kex_prelude";

// The reflection record a type is as a value (`String.name`), and the static
// type of a type named where a value goes: `Type<String>`.
inline const std::string kTypeRecord = "Type";

// The unit type's name: named as a value it is the unit value, not a `Type`.
inline const std::string kVoidType = "Void";

// The two Bool values, spelled as constructors in a pattern.
inline const std::string kTrue = "True";
inline const std::string kFalse = "False";

} // namespace kex::names
