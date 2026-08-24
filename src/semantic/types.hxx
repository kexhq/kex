#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace kex::semantic {

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct PrimitiveType {
    // String is its OWN kind, not `[Char]`. Modelling it as a list of Char
    // made the two indistinguishable to the checker, so `contains? : String ->
    // Bool` silently accepted a Char and `[Char]` reported itself as "String".
    enum Kind { Integer, Char, String, Bool, Atom, Unit };  // Integer = arbitrary precision
    Kind kind;
    // For Atom only, and for DISPLAY only: the literal an atom type was
    // written as (`:macos`), empty for a plain `Atom`. Without it a union of
    // atom literals printed as "Atom | Atom | ...", one indistinguishable
    // member per variant. Every comparison ignores it (typesEqual looks at
    // `kind` alone), so `:macos` stays interchangeable with any other atom.
    std::string atomName;
};

struct SizedIntType {
    int bits;       // 8, 16, 32, 64
    bool isSigned;  // Byte == SizedIntType{8, false}
};

struct SizedFloatType {
    int bits;  // 32, 64
};

struct NamedType {
    std::string name;
    std::vector<TypePtr> typeArgs;
};

struct FuncType {
    std::vector<TypePtr> params;
    TypePtr result;
};

struct TupleType {
    std::vector<TypePtr> elements;
};

struct ListType {
    TypePtr element;
};

struct MapType {
    TypePtr key;
    TypePtr value;
};

struct OptionalType {
    TypePtr inner;
};

struct UnionType {
    std::vector<TypePtr> members;
};

struct IntersectionType {
    std::vector<TypePtr> members;
};

struct RecordType {
    std::vector<std::pair<std::string, TypePtr>> fields;
};

struct TypeVar {
    int id;
};

struct UnknownType {};

// Bottom type — for expressions that never return (infinite loop, panic, exit).
// Unifies with any type as a universal subtype: a Never-typed branch is compatible
// with any other branch type because it never actually produces a value at runtime.
struct VoidType {
    // Empty for ordinary non-returning expressions. Populated when Never is
    // the normalization of an explicitly uninhabited type, so diagnostics can
    // retain the reason after normalization.
    std::string reason;
};

// A signature-table placeholder meaning "any type satisfying `traitName`"
// (e.g. `even? : T -> Bool` where T is constrained to Integer). `varName`
// is for display only ("T"); the constraint itself is consulted via
// TraitRegistry::satisfies, not stored structurally on the Type.
struct ConstrainedType {
    std::string varName;
    std::string traitName;
    // Negative interface-generic id when this bound participates in a public
    // relationship such as `(A: Inspectable) -> A`; zero for ordinary
    // structural constraints that do not bind a result placeholder.
    int genericId = 0;
};

struct Type {
    std::variant<
        PrimitiveType,
        SizedIntType,
        SizedFloatType,
        NamedType,
        FuncType,
        TupleType,
        ListType,
        MapType,
        OptionalType,
        UnionType,
        IntersectionType,
        RecordType,
        TypeVar,
        UnknownType,
        VoidType,
        ConstrainedType
    > kind;

    static auto integer() -> TypePtr;  // arbitrary precision
    static auto charT() -> TypePtr;
    static auto string() -> TypePtr;   // alias for list(charT())
    static auto boolean() -> TypePtr;
    static auto atom(std::string atomName = "") -> TypePtr;
    static auto unit() -> TypePtr;
    static auto unknown() -> TypePtr;
    static auto voidType(std::string reason = {}) -> TypePtr;
    static auto named(const std::string& name, std::vector<TypePtr> args = {}) -> TypePtr;
    static auto func(std::vector<TypePtr> params, TypePtr result) -> TypePtr;
    static auto list(TypePtr element) -> TypePtr;
    static auto tuple(std::vector<TypePtr> elements) -> TypePtr;
    static auto map(TypePtr key, TypePtr value) -> TypePtr;
    static auto optional(TypePtr inner) -> TypePtr;
    static auto intersection(std::vector<TypePtr> members) -> TypePtr;
    static auto record(std::vector<std::pair<std::string, TypePtr>> fields)
        -> TypePtr;
    static auto typeVar(int id) -> TypePtr;
    static auto constrained(const std::string& varName,
                            const std::string& traitName,
                            int genericId = 0) -> TypePtr;

    // Sized integers — explicit opt-ins for fixed width.
    static auto byte() -> TypePtr;     // UInt8
    static auto int8() -> TypePtr;
    static auto int16() -> TypePtr;
    static auto int32() -> TypePtr;
    static auto int64() -> TypePtr;    // "Int" is a name alias for this
    static auto uint8() -> TypePtr;
    static auto uint16() -> TypePtr;
    static auto uint32() -> TypePtr;
    static auto uint64() -> TypePtr;

    // Sized floats — Float64 is the default for a plain float literal.
    // There is no arbitrary-precision float and no bare "Float" Type
    // (it exists only as a trait name in the TraitRegistry).
    static auto float32() -> TypePtr;
    static auto float64() -> TypePtr;
};

auto typeToString(const TypePtr& type) -> std::string;

// A type flattened to name + arguments, the shape `Type.of` hands back to Kex
// code. Backend-neutral on purpose: the interpreter builds a record from it
// and the BEAM lowering emits a literal, without either depending on the
// checker's type representation.
struct StructuredType {
    std::string name;
    std::vector<StructuredType> args;
    // Functions only: `Function` carries the parameters followed by the
    // result in `args`. `pure` is false when calling it is a side effect —
    // the question `foul` answers in source, phrased positively because
    // everything is pure by default.
    bool pure = true;
};

// A recorded answer for a `Type.of`/`Type.returnedBy` call site. The argument
// of `Type.of(someValue)` still runs (it may have effects); one that NAMES a
// function does not — `Date.parse` on its own is a call missing its argument.
struct StaticTypeAnswer {
    StructuredType type;
    bool evaluateArgument = true;
};

// The checked type in that shape, or nullopt when it is not fully determined
// (a gradual expression checks as `?`/`A`/`N`, where the VALUE is the better
// source and the runtime fallback should answer instead).
auto structuredTypeOf(const TypePtr& type) -> std::optional<StructuredType>;

// True for the language's built-in type names — the spellings typeToString
// produces for primitives, plus the structural types that have no source
// declaration. These are the type names that resolve without any `type`
// declaration behind them; everything else must be declared.
auto isPrimitiveTypeName(const std::string& name) -> bool;

// Whether two named-type identities denote the same type. Record and ADT
// identity is module-qualified (`Tey.Manifest.Dependency`), so two qualified
// names must match in full — that is what keeps same-named records in sibling
// modules apart. A BARE name is an identity the checker could not qualify
// (an interface signature it read back, a type it never saw declared), so it
// matches any qualified name ending in that segment rather than spuriously
// mismatching against the qualified spelling of the same type.
auto namedTypesMatch(const std::string& a, const std::string& b) -> bool;
auto typesEqual(const TypePtr& a, const TypePtr& b) -> bool;

// True when a type is fully determined — no Unknown, no type variable, and no
// trait-bound placeholder anywhere inside it. Two callers ask it for the same
// underlying reason, "does the checker actually KNOW this type?":
//   - display, where a gradual `?`/`A`/`N` means the VALUE is the better
//     source of a type name than the checked type;
//   - diagnostics, where a mismatch against a determined receiver is provable
//     and one against a gradual receiver is not.
auto isFullyConcrete(const TypePtr& type) -> bool;

class TypeEnv {
public:
    auto set(const std::string& name, TypePtr type) -> void;
    auto get(const std::string& name) const -> TypePtr;
    auto has(const std::string& name) const -> bool;

private:
    std::unordered_map<std::string, TypePtr> m_types;
};

} // namespace kex::semantic
