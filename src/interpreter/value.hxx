#pragma once

#include <functional>
#include <gmpxx.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace kex::interpreter {

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct NoneValue {};  // Kex `None` — the empty Optional
struct UnitValue {};  // Kex `()` — void return from IO/effectful operations
struct IntValue { int64_t value; };
// The arbitrary-precision case of `Integer` — only ever constructed once a
// value no longer fits in IntValue's int64_t (see integerResult/asInteger
// in value.cxx and the overflow-checked arithmetic in evaluator.cxx).
// IntValue stays the fast path for every value that fits; this is the
// fallback, not a parallel "the" representation of Integer.
struct BigIntValue { mpz_class value; };
struct FloatValue { double value; };
struct StringValue { std::string value; };
struct CharValue { char value; };
struct BoolValue { bool value; };
struct AtomValue { std::string name; };

struct ListValue { std::vector<ValuePtr> elements; };
struct TupleValue { std::vector<ValuePtr> elements; };
struct MapValue { std::vector<std::pair<ValuePtr, ValuePtr>> entries; };
struct RangeValue { int64_t start; int64_t end; bool isChar = false; };

using StreamGenerator = std::function<std::shared_ptr<struct Value>(int64_t index)>;
struct StreamValue {
    StreamGenerator generator;
    int64_t offset = 0;
};

struct RecordValue {
    std::string typeName;
    std::unordered_map<std::string, ValuePtr> fields;
};

using NativeFunc = std::function<ValuePtr(std::vector<ValuePtr>)>;

struct FunctionValue {
    std::string name;
    NativeFunc native; // for built-in functions
};

struct LambdaValue {
    std::vector<std::string> params;
    // body is stored by reference to AST — evaluated at call time
    const void* body = nullptr; // points to vector<ExprPtr>
    struct Environment* closure = nullptr;
};

struct Value {
    std::variant<
        NoneValue,
        UnitValue,
        IntValue,
        BigIntValue,
        FloatValue,
        StringValue,
        CharValue,
        BoolValue,
        AtomValue,
        ListValue,
        TupleValue,
        MapValue,
        RangeValue,
        StreamValue,
        RecordValue,
        FunctionValue,
        LambdaValue
    > data;

    static auto none() -> ValuePtr;
    static auto unit() -> ValuePtr;
    static auto integer(int64_t v) -> ValuePtr;
    static auto bigInteger(mpz_class v) -> ValuePtr;
    static auto floating(double v) -> ValuePtr;
    static auto string(std::string v) -> ValuePtr;
    static auto character(char v) -> ValuePtr;
    static auto boolean(bool v) -> ValuePtr;
    static auto atom(std::string name) -> ValuePtr;
    static auto list(std::vector<ValuePtr> elems) -> ValuePtr;
    static auto tuple(std::vector<ValuePtr> elems) -> ValuePtr;
    static auto record(std::string type, std::unordered_map<std::string, ValuePtr> fields) -> ValuePtr;

    auto isTrue() const -> bool;
    auto toString() const -> std::string;
    auto toRepr() const -> std::string;
    auto typeName() const -> std::string;
    // Pretty-printed inspect representation (value only, no type suffix).
    // Used by the REPL and IO.inspect. ANSI colors honor the global kex::color::enabled flag.
    auto inspect() const -> std::string;
};

auto valuesEqual(const ValuePtr& a, const ValuePtr& b) -> bool;

// Extracts the value as an mpz_class if it's IntValue or BigIntValue
// (the two runtime representations of `Integer`), else nullopt — the
// shared entry point for any integer-aware op (arithmetic, comparison,
// pattern matching) that needs to treat both representations uniformly.
auto asInteger(const ValuePtr& v) -> std::optional<mpz_class>;

// Demotes a computed mpz_class result back to the fast IntValue
// representation when it still fits in int64_t (e.g. a bignum result
// shrinking back down), otherwise keeps the bignum representation.
auto integerResult(mpz_class v) -> ValuePtr;

// Text content of a String, a Char, or a ListValue whose elements are all
// Char — nullopt for anything else. String/Char/[Char] are meant to be
// interchangeable from the language user's standpoint; this is the shared
// extraction point for that (see valuesEqual, toString(), `+`).
auto textContent(const ValuePtr& v) -> std::optional<std::string>;

} // namespace kex::interpreter
