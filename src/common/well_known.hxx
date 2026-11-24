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
inline auto moduleAtom(const std::string& path) -> std::string {
    return kRuntimeRoot + "." + path;
}

// `value.try`: a postfix keyword, not a function, offered after a dot where
// it applies (TypeChecker::memberCompletions).
inline const std::string kTryKeyword = "try";

// `value.is?(T)`: the checker's own receiver call
// (findCompileTimeIntrinsic), offered after a dot on every value.
inline const std::string kTypeTestCall = "is?";

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
inline const std::string kResultType = "Result";
inline const std::string kOptionalType = "Optional";
// The hole a type lambda leaves (`make Result<_, E>`, §8.2).
inline const std::string kTypeHole = "_";

// A trait's self type: in its signatures and default bodies, whatever type
// claims the trait.
inline const std::string kThisType = "This";

// Every other type, trait, constructor and module name the compiler decides
// something by. Each one is here so the decisions can be found, and each is
// a step of docs/typechecker-hm-plan.md §7: the syntax's own vocabulary stays
// (§7.2), and the rest should become declarations the stdlib makes.
//
// The syntax's vocabulary: literals, conditions and type sugar.
inline const std::string kBoolType = "Bool";
inline const std::string kStringType = "String";
inline const std::string kCharType = "Char";
inline const std::string kAtomType = "Atom";
inline const std::string kIntegerType = "Integer";
inline const std::string kFloatType = "Float";
inline const std::string kJustConstructor = "Just";
inline const std::string kNoneConstructor = "None";
inline const std::string kOkConstructor = "Ok";
inline const std::string kErrorConstructor = "Error";
// The type grammar's own forms, spelled as names.
inline const std::string kNeverType = "Never";
inline const std::string kTupleType = "Tuple";
inline const std::string kBlockType = "Block";
inline const std::string kAnyType = "Any";
inline const std::string kUnknownType = "Unknown";
// The numeric tower: §6, and decision 2 of the plan.
inline const std::string kNumberTrait = "Number";
inline const std::string kMonadTrait = "Monad";
inline const std::string kIntAlias = "Int";
inline const std::string kByteType = "Byte";
inline const std::string kFloat32Type = "Float32";
inline const std::string kFloat64Type = "Float64";
// Traits the compiler derives for every type: never a join's target.
inline const std::string kInspectableTrait = "Inspectable";
inline const std::string kEquatableTrait = "Equatable";
// Stdlib types the checker still knows by name: each is a special case to
// replace with a declaration (§7.1).
inline const std::string kRangeType = "Range";
inline const std::string kFilePathType = "FilePath";
inline const std::string kServerType = "Server";
inline const std::string kReplyType = "Reply";
inline const std::string kNewType = "New";
// Module names the checker treats specially.
inline const std::string kPreludeModuleName = "Prelude";
inline const std::string kErlangModule = "Erlang";
inline const std::string kElixirModule = "Elixir";
inline const std::string kGleamModule = "Gleam";

// The two Bool values, spelled as constructors in a pattern.
inline const std::string kTrue = "True";
inline const std::string kFalse = "False";

// The name that binds nothing: a `_` parameter, pattern, or loop counter.
inline const std::string kWildcard = "_";
inline auto isWildcard(const std::string& name) -> bool {
    return name == kWildcard;
}

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
    for (const auto& [spelling, builtin] : kTestBuiltins) {
        if (spelling == name) {
            return builtin;
        }
    }
    return std::nullopt;
}
inline auto testBuiltinName(TestBuiltin builtin) -> std::string_view {
    for (const auto& [spelling, entry] : kTestBuiltins) {
        if (entry == builtin) {
            return spelling;
        }
    }
    return {};
}
// The intrinsic a decided call names (`Kex.Intrinsic.Test.assert`), and back.
inline auto testIntrinsicPath(TestBuiltin builtin) -> std::string {
    return kTestIntrinsics + "." + std::string(testBuiltinName(builtin));
}
inline auto testBuiltinOf(const std::string& owner) -> std::optional<TestBuiltin> {
    if (!owner.starts_with(kTestIntrinsics + ".")) {
        return std::nullopt;
    }
    return testBuiltinNamed(std::string_view(owner).substr(kTestIntrinsics.size() + 1));
}

} // namespace kex::names
