#pragma once

#include <cstdint>
#include <fstream>
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

struct VariantValue {
    std::string tag;         // "Ok", "Less", "Nothing"
    std::string parentType;  // "Result", "Ordering", "Option", "" if unknown
    std::vector<ValuePtr> args;  // payload — empty for zero-arg

    // Declared type params of parentType, e.g. ["X"] for Optional<X> or
    // ["T", "E"] for Result<T, E> — empty if parentType is non-generic or
    // unknown. Lets typeName() render "Optional<Integer>" instead of just
    // "Optional" by pairing each param with the runtime type of the args[]
    // entry that instantiates it (see argParamIndex).
    std::vector<std::string> typeParams;
    // argParamIndex[i] is the index into typeParams that args[i] instantiates,
    // or -1 if args[i]'s declared type wasn't a bare type param (e.g. nested
    // generics like List<X>) and so can't be resolved to a single param.
    std::vector<int> argParamIndex;
};

struct ModuleValue { std::string name; };  // "Math", "IO", "File"

// A process handle — the runtime value of `spawn do ... end` and
// `Process.self`. Deliberately thin: the real Fiber, mailbox, and links
// live in the Scheduler's own process table (src/interpreter/scheduler.hxx),
// not here, so copying a pid around never risks fiber/mailbox lifetime
// issues. `scheduler` is a non-owning back-pointer — it outlives every
// ProcessValue it produces, since it's owned by the Evaluator for the
// program's whole run.
struct ProcessValue { uint64_t pid; class Scheduler* scheduler; };

// `Task.start { block }`'s handle — a Task *is* spawn+monitor underneath
// (same as runtime/src/kex_task.erl on the BEAM backend), but kept as its
// own variant rather than reusing ProcessValue so typeName() says "Task"
// and `.await` dispatches unambiguously without a tag check.
struct TaskValue { uint64_t pid; class Scheduler* scheduler; };

struct ListValue { std::vector<ValuePtr> elements; };
struct TupleValue { std::vector<ValuePtr> elements; };
struct MapValue { std::vector<std::pair<ValuePtr, ValuePtr>> entries; };
struct RangeValue { int64_t start; int64_t end; bool isChar = false; };

using StreamGenerator = std::function<std::shared_ptr<struct Value>(int64_t index)>;
struct StreamValue {
    StreamGenerator generator;
    int64_t offset = 0;
};

struct FileHandleValue {
    std::shared_ptr<std::fstream> stream;
    std::string path;
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
        VariantValue,
        ModuleValue,
        ProcessValue,
        TaskValue,
        ListValue,
        TupleValue,
        MapValue,
        RangeValue,
        StreamValue,
        FileHandleValue,
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
    static auto variant(std::string tag, std::string parentType = "", std::vector<ValuePtr> args = {},
                         std::vector<std::string> typeParams = {}, std::vector<int> argParamIndex = {}) -> ValuePtr;
    static auto module(std::string name) -> ValuePtr;
    static auto process(uint64_t pid, class Scheduler* scheduler) -> ValuePtr;
    static auto task(uint64_t pid, class Scheduler* scheduler) -> ValuePtr;
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
