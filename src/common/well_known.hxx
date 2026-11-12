#pragma once

// Names the compiler itself depends on, spelled once. A comparison against
// one of these is a comparison against the language's own vocabulary, not a
// magic string: renaming the runtime prelude module, say, is one edit here.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kex::names {

// The runtime module the prelude compiles to; the fallback provider of every
// receiver method a program does not define itself.
inline const std::string kPreludeModule = "kex_prelude";

// The root every Kex module compiles under: `Net.HTTP` is the BEAM module
// `Kex.Net.HTTP`.
inline const std::string kRuntimeRoot = "Kex";
inline auto moduleAtom(const std::string& path) -> std::string { return kRuntimeRoot + "." + path; }

// The prelude's finite iterations, whose callback lowering may turn into a
// fold when it reassigns an outer variable (ir/lowering_body.cxx).
inline const std::string kEachFunction = "each";
inline const std::string kTimesFunction = "times";

// `Kex.embed("file")`: the file's contents, read while compiling
// (compiled/expand.cxx folds it before checking).
inline const std::string kEmbedFunction = "embed";

// The interop root: `BEAM.lists.reverse(xs)` calls Erlang's `lists:reverse`.
inline const std::string kForeignRoot = "BEAM";

// The runtime's own modules: `Kex.Intrinsic.List.reverse(x)` calls
// `kex_intrinsic_list:reverse`, the primitive the prelude wraps.
inline const std::string kIntrinsicRoot = kRuntimeRoot + ".Intrinsic";
inline auto isIntrinsicPath(const std::string& path) -> bool {
    return path.starts_with(kIntrinsicRoot + ".");
}

// The reflection record a type is as a value (`String.name`), and the static
// type of a type named where a value goes: `Type<String>`.
inline const std::string kTypeRecord = "Type";

// The structural collection types' names: `[X]` is `List<X>` and `{K: V}`
// is `Map<K, V>` wherever a type is named.
inline const std::string kListType = "List";
inline const std::string kMapType = "Map";

// The unit type's name: named as a value it is the unit value, not a `Type`.
inline const std::string kVoidType = "Void";
// A process as its bare identifier, and as a handle typed by what it serves.
inline const std::string kPidType = "Pid";
inline const std::string kProcessType = "Process";
inline const std::string kResultType = "Result";
inline const std::string kOptionalType = "Optional";

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

// A measure (units.kex): `Measure { canonical, value, unit: UnitDefinition {
// conversionFactor, dimension, notation } }`. "A measure is its quantity": the
// runtimes' equality compares two by dimension and canonical value, wherever
// they are, as it compares numbers across the numeric tower.
inline const std::string kMeasureRecord = "Measure";
inline const std::string kMeasureCanonical = "canonical";
inline const std::string kMeasureUnit = "unit";
inline const std::string kUnitDimension = "dimension";

// The testing DSL (stdlib/test.kex declares them, with no body): runtime
// builtins the walker and `kex_test` implement. The checker decides that a
// bare call is one (a Builtin whose owner is `Kex.Intrinsic.Test.<name>`),
// so a program's own `assert` is its own; both backends read that decision.
enum class TestBuiltin { Describe, It, Before, After, Assert };
inline constexpr std::array<std::pair<std::string_view, TestBuiltin>, 5> kTestBuiltins{{
    {"describe", TestBuiltin::Describe},
    {"it", TestBuiltin::It},
    {"before", TestBuiltin::Before},
    {"after", TestBuiltin::After},
    {"assert", TestBuiltin::Assert},
}};
inline const std::string kTestIntrinsics = kIntrinsicRoot + ".Test";
inline auto testBuiltinNamed(std::string_view name) -> std::optional<TestBuiltin> {
    for (const auto& [spelling, builtin] : kTestBuiltins)
        if (spelling == name) return builtin;
    return std::nullopt;
}
inline auto testBuiltinName(TestBuiltin builtin) -> std::string_view {
    for (const auto& [spelling, entry] : kTestBuiltins)
        if (entry == builtin) return spelling;
    return {};
}
// The intrinsic a decided call names (`Kex.Intrinsic.Test.assert`), and back.
inline auto testIntrinsicPath(TestBuiltin builtin) -> std::string {
    return kTestIntrinsics + "." + std::string(testBuiltinName(builtin));
}
inline auto testBuiltinOf(const std::string& owner) -> std::optional<TestBuiltin> {
    if (!owner.starts_with(kTestIntrinsics + ".")) return std::nullopt;
    return testBuiltinNamed(std::string_view(owner).substr(kTestIntrinsics.size() + 1));
}

} // namespace kex::names
