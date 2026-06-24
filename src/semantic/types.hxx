#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace kex::semantic {

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct PrimitiveType {
    enum Kind { Integer, Char, Bool, Atom, Unit };  // Integer = arbitrary precision
    Kind kind;
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

struct TypeVar {
    int id;
};

struct UnknownType {};

// A signature-table placeholder meaning "any type satisfying `traitName`"
// (e.g. `even? : T -> Bool` where T is constrained to Integer). `varName`
// is for display only ("T"); the constraint itself is consulted via
// TraitRegistry::satisfies, not stored structurally on the Type.
struct ConstrainedType {
    std::string varName;
    std::string traitName;
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
        TypeVar,
        UnknownType,
        ConstrainedType
    > kind;

    static auto integer() -> TypePtr;  // arbitrary precision
    static auto charT() -> TypePtr;
    static auto string() -> TypePtr;   // alias for list(charT())
    static auto boolean() -> TypePtr;
    static auto atom() -> TypePtr;
    static auto unit() -> TypePtr;
    static auto unknown() -> TypePtr;
    static auto named(const std::string& name, std::vector<TypePtr> args = {}) -> TypePtr;
    static auto func(std::vector<TypePtr> params, TypePtr result) -> TypePtr;
    static auto list(TypePtr element) -> TypePtr;
    static auto tuple(std::vector<TypePtr> elements) -> TypePtr;
    static auto map(TypePtr key, TypePtr value) -> TypePtr;
    static auto optional(TypePtr inner) -> TypePtr;
    static auto typeVar(int id) -> TypePtr;
    static auto constrained(const std::string& varName, const std::string& traitName) -> TypePtr;

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
auto typesEqual(const TypePtr& a, const TypePtr& b) -> bool;

class TypeEnv {
public:
    auto set(const std::string& name, TypePtr type) -> void;
    auto get(const std::string& name) const -> TypePtr;
    auto has(const std::string& name) const -> bool;

private:
    std::unordered_map<std::string, TypePtr> m_types;
};

} // namespace kex::semantic
