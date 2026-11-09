#pragma once

// Names the compiler itself depends on, spelled once. A comparison against
// one of these is a comparison against the language's own vocabulary, not a
// magic string: renaming the runtime prelude module, say, is one edit here.

#include <string>

namespace kex::names {

// The runtime module the prelude compiles to; the fallback provider of every
// receiver method a program does not define itself.
inline const std::string kPreludeModule = "kex_prelude";

// The BEAM module a Kex module compiles to: `Net.HTTP` is `Kex.Net.HTTP`.
inline auto moduleAtom(const std::string& path) -> std::string { return "Kex." + path; }

// The reflection record a type is as a value (`String.name`), and the static
// type of a type named where a value goes: `Type<String>`.
inline const std::string kTypeRecord = "Type";

// The structural collection types' names: `[X]` is `List<X>` and `{K: V}`
// is `Map<K, V>` wherever a type is named.
inline const std::string kListType = "List";
inline const std::string kMapType = "Map";

// The unit type's name: named as a value it is the unit value, not a `Type`.
inline const std::string kVoidType = "Void";

// A trait's self type: in its signatures and default bodies, whatever type
// claims the trait.
inline const std::string kThisType = "This";

// The two Bool values, spelled as constructors in a pattern.
inline const std::string kTrue = "True";
inline const std::string kFalse = "False";

// The name that binds nothing: a `_` parameter, pattern, or loop counter.
inline const std::string kWildcard = "_";
inline auto isWildcard(const std::string& name) -> bool { return name == kWildcard; }

// The atom a caller passes for an argument it leaves out: the callee puts
// its own default in its place on entry, checked and lowered in its own
// scope. A unit compiled apart needs nothing of the default but this.
inline const std::string kDefaultArgument = "$kex_default";

// What every record-declaring module answers a field's declared default
// with, as `__kex_record_default(Tag, Field)`, and its answer for a field
// without one. The runtime's hot-reload migration calls it too.
inline const std::string kRecordDefaultFunction = "__kex_record_default";
inline const std::string kNoRecordDefault = "kex_no_default";

// The trait whose `showValue` makes a value text (interpolation, printing).
inline const std::string kShowableTrait = "Showable";

} // namespace kex::names
