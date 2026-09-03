#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <gmpxx.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace kex::interpreter {

struct Value;
using ValuePtr = std::shared_ptr<Value>;

// Thrown when compile-time evaluation tries to USE a PlaceholderValue rather
// than just carry it around. Not a user-facing error: chain collapse catches
// it and keeps the runtime form, which is always correct — the builder simply
// turned out to need a value only the running program has.
class PlaceholderMisuse : public std::runtime_error {
public:
    explicit PlaceholderMisuse(const std::string& name, const std::string& what)
        : std::runtime_error("the value of `" + name + "` is not known at " +
                             "compile time, and " + what + " needs it"),
          m_name(name) {}
    auto name() const -> const std::string& { return m_name; }

private:
    std::string m_name;
};

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
struct BinaryValue { std::vector<uint8_t> bytes; };
// A Unicode codepoint, not a byte: `"école".chars` yields five of these.
struct CharValue { char32_t value; };
struct BoolValue { bool value; };
struct AtomValue { std::string name; };

struct VariantValue {
    std::string tag;         // "Ok", "Less", "None"
    std::string parentType;  // "Result", "Ordering", "Optional", "" if unknown
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

// Typed handle for a process-backed serving state machine.  The state itself
// lives in the server fiber; copying/configuring a handle never copies it.
struct ServerValue {
    uint64_t pid;
    class Scheduler* scheduler;
    int64_t timeoutMs = 5000;
};

struct ListValue { std::vector<ValuePtr> elements; };
struct TupleValue { std::vector<ValuePtr> elements; };
struct MapValue { std::vector<std::pair<ValuePtr, ValuePtr>> entries; };
struct RangeValue { int64_t start; int64_t end; bool isChar = false; };

// A stream is a chain of cells. Forcing one answers the element there and the
// cell after it, and MEMOISES both, so the element behind a given cell is
// computed once however often the stream is walked.
//
// The chain replaced an index-addressed generator (`answer element n`), which
// had to replay the stream from its seed on every access: `take(n)` cost n²
// steps, and `filter` — needing random access into a sequence whose n-th match
// is only findable by rescanning — bounded its rescan with a search cap and so
// silently ENDED a stream whose matches were merely sparse. Both problems are
// properties of index addressing, and neither survives it.
//
// A null tail marks the end. That sentinel is a null POINTER rather than a
// value on purpose: a finite stream (`FS.File.feed`) once signalled
// end-of-file with `None`, which is a perfectly good element of a
// `Stream<String?>`, so `take` could not tell a real element from the end and
// padded its answer with Nones (issue #215). Genuinely infinite streams
// (`Stream.Sequence`) never produce a null tail, and stay infinite.
struct StreamCell;
using StreamCellPtr = std::shared_ptr<StreamCell>;

struct StreamCell {
    // Answers this cell's element and the rest of the chain. Cleared once
    // forced, releasing whatever it captured.
    std::function<std::pair<std::shared_ptr<struct Value>, StreamCellPtr>()> thunk;
    bool forced = false;
    std::shared_ptr<struct Value> head;   // null once forced marks the end
    StreamCellPtr tail;

    // A forced chain is a linked list, so the default destructor would
    // release it by recursing one frame per cell and overflow the stack on a
    // stream of any real length. Unlink iteratively instead, and only through
    // cells nothing else still holds.
    ~StreamCell();
};

// A null cell IS the empty stream, which is why `Stream.empty` needs no
// special case anywhere downstream.
struct StreamValue { StreamCellPtr cell; };

// Forces `cell` to its memoised element, answering it. Answers null at the
// end of the stream — for an already-ended cell, and for a null cell, which
// is the end by construction.
auto forceStream(const StreamCellPtr& cell) -> StreamCell*;

// A feed is a one-shot source: pulling an element CONSUMES it. Where a stream
// memoises so that walking it twice costs one walk, a feed deliberately keeps
// nothing — that is precisely what lets it front a file or a socket bigger
// than memory, which a memoising stream cannot do while anything still holds
// its head. The state is shared, so every reference to a feed advances one
// cursor and a feed cannot be replayed.
struct FeedState {
    std::function<std::shared_ptr<struct Value>()> pull;  // null == spent
    bool spent = false;
};
struct FeedValue { std::shared_ptr<FeedState> state; };

// Pulls the next element, or null once the source is spent. Latches: a source
// that has ended once is never asked again.
auto pullFeed(const std::shared_ptr<FeedState>& state) -> std::shared_ptr<Value>;

struct FileHandleValue {
    std::shared_ptr<std::fstream> stream;
    std::string path;
};

// The three standard streams are ordinary FileHandle VALUES (kexhq/kex#139),
// so `IO.printLine(x)` and `IO.out.printLine(x)` are one call and a sink
// can be passed rather than switched globally. They carry no fstream: these
// sentinel paths mark them, and the FileHandle intrinsics route them through
// the IO ones so Mock.IO capture keeps covering them.
inline constexpr const char* kStdoutPath = "<stdout>";
inline constexpr const char* kStderrPath = "<stderr>";
inline constexpr const char* kStdinPath = "<stdin>";

inline auto isStandardHandle(const std::string& path) -> bool {
    return path == kStdoutPath || path == kStderrPath || path == kStdinPath;
}

struct RecordValue {
    std::string typeName;
    std::unordered_map<std::string, ValuePtr> fields;
};

using NativeFunc = std::function<ValuePtr(std::vector<ValuePtr>)>;

struct FunctionValue {
    std::string name;
    NativeFunc native; // for built-in functions
    // Declared parameter count for closures built from a Lambda; -1 when
    // unknown (builtins, function references). Mirrors what BEAM recovers via
    // erlang:fun_info(F, arity) — Kex.Intrinsic.Fun.applyIndexed needs it to
    // tell `|item, i|` from `|k, v, i|`.
    int arity = -1;
    // A caller whose parameter is Block<[A]> raises this for the duration of
    // the call. Lambda evaluation consults it to collect body expressions.
    std::shared_ptr<int> collectionDepth;
};

// A runtime value standing in for ITSELF during compile-time evaluation.
//
// `compiled` chain collapse binds each free variable of an otherwise-known
// chain to one of these, so a builder can carry the value through records and
// lists — the `params: [userId]` case — and the reifier can put the ORIGINAL
// expression back wherever it came to rest.
//
// Deliberately inert. Any operation that would need the actual value —
// arithmetic, comparison, stringification, a truthiness test — must FAIL
// rather than invent one, which is what PlaceholderMisuse is for. Silently
// producing something would bake a wrong answer into the emitted program,
// which is far worse than not collapsing: `"${userId}"` must never become the
// text "Placeholder".
struct PlaceholderValue {
    std::size_t index;   // into the collapse site's captured expressions
    std::string name;    // as written, for diagnostics
};

struct LambdaValue {
    std::vector<std::string> params;
    // body is stored by reference to AST — evaluated at call time
    const void* body = nullptr; // points to vector<ExprPtr>
    struct Environment* closure = nullptr;
};

struct Value {
    std::variant<
        UnitValue,
        IntValue,
        BigIntValue,
        FloatValue,
        StringValue,
        BinaryValue,
        CharValue,
        BoolValue,
        AtomValue,
        VariantValue,
        ModuleValue,
        ProcessValue,
        TaskValue,
        ServerValue,
        ListValue,
        TupleValue,
        MapValue,
        RangeValue,
        StreamValue,
        FeedValue,
        FileHandleValue,
        RecordValue,
        FunctionValue,
        LambdaValue,
        PlaceholderValue
    > data;

    static auto none() -> ValuePtr;
    static auto unit() -> ValuePtr;
    static auto integer(int64_t v) -> ValuePtr;
    static auto bigInteger(mpz_class v) -> ValuePtr;
    static auto floating(double v) -> ValuePtr;
    static auto string(std::string v) -> ValuePtr;
    static auto binary(std::vector<uint8_t> v) -> ValuePtr;
    static auto character(char32_t v) -> ValuePtr;
    static auto boolean(bool v) -> ValuePtr;
    static auto atom(std::string name) -> ValuePtr;
    static auto variant(std::string tag, std::string parentType = "", std::vector<ValuePtr> args = {},
                         std::vector<std::string> typeParams = {}, std::vector<int> argParamIndex = {}) -> ValuePtr;
    // Convenience wrappers over variant() for the prelude's builtin generic
    // ADTs (Optional<T> = Just(T) | None, Result<T, E> = Ok(T) | Error(E)) —
    // every stdlib call site that constructs one of these should go through
    // here rather than calling variant() directly with the tag/parentType
    // spelled out, so typeName() can always render "Optional<Integer>"/
    // "Result<String, Integer>" instead of just the bare "Optional"/"Result"
    // (which is what happens if typeParams/argParamIndex are left empty).
    static auto just(ValuePtr inner) -> ValuePtr;
    static auto ok(ValuePtr inner) -> ValuePtr;
    static auto error(ValuePtr inner) -> ValuePtr;
    static auto module(std::string name) -> ValuePtr;
    static auto process(uint64_t pid, class Scheduler* scheduler) -> ValuePtr;
    static auto task(uint64_t pid, class Scheduler* scheduler) -> ValuePtr;
    static auto server(uint64_t pid, class Scheduler* scheduler,
                       int64_t timeoutMs = 5000) -> ValuePtr;
    static auto list(std::vector<ValuePtr> elems) -> ValuePtr;
    static auto tuple(std::vector<ValuePtr> elems) -> ValuePtr;
    static auto record(std::string type, std::unordered_map<std::string, ValuePtr> fields) -> ValuePtr;

    auto isNone() const -> bool;
    auto isTrue() const -> bool;
    auto toString() const -> std::string;
    auto toRepr() const -> std::string;
    auto typeName() const -> std::string;
    // Pretty-printed inspect representation (value only, no type suffix).
    // Used by the REPL and IO.inspect. ANSI colors honor the global kex::color::enabled flag.
    auto inspect() const -> std::string;
};

auto valuesEqual(const ValuePtr& a, const ValuePtr& b) -> bool;

// The method-dispatch type name for a value — the name used for UFCS resolution
// (e.g. "Integer" for both IntValue/BigIntValue, "Pid" for ProcessValue).
// Separate from typeName() which is for display.
auto dispatchTypeName(const ValuePtr& v) -> std::string;

// Type names a receiver's methods may also be registered under, most- to
// least-specific (Integer/Float -> Number). Lets `make Number` cover both.
auto dispatchSupertypes(const std::string& typeName) -> std::vector<std::string>;

// Returns true when `name` is a valid type-pattern name for the value's
// runtime type (e.g. both "Int" and "Integer" match IntValue/BigIntValue).
auto matchesTypeName(const std::string& name, const ValuePtr& v) -> bool;

// The set of builtin type names that cannot have prelude methods overridden.
auto builtinTypeNames() -> const std::unordered_set<std::string>&;

// Default module allowlist for sandboxed Evaluator.run / .runExpression.
auto defaultEvalAllowList() -> const std::vector<std::string>&;

// Extracts the value as an mpz_class if it's IntValue or BigIntValue
// (the two runtime representations of `Integer`), else nullopt — the
// shared entry point for any integer-aware op (arithmetic, comparison,
// pattern matching) that needs to treat both representations uniformly.
auto asInteger(const ValuePtr& v) -> std::optional<mpz_class>;

// Demotes a computed mpz_class result back to the fast IntValue
// representation when it still fits in int64_t (e.g. a bignum result
// shrinking back down), otherwise keeps the bignum representation.
auto integerResult(mpz_class v) -> ValuePtr;

// Erlang floats cannot represent NaN or Infinity: every float operation that
// would produce one raises `badarith` instead, including plain overflow like
// `1.0e308 * 10.0`. Kex follows that rule so the walker and the BEAM backend
// agree, and because a NaN value would otherwise leak into comparisons,
// pattern matching, and Map keys, where `NaN != NaN` behaves surprisingly.
//
// Returns the error message for a non-finite result of the operation named by
// `what`, or nullopt when `v` is finite and the caller should proceed. The
// caller raises in whichever style its layer uses (RuntimeError with a source
// location in the evaluator, std::runtime_error in the stdlib intrinsics).
auto nonFiniteFloatError(double v, const std::string& what)
    -> std::optional<std::string>;

// Parses a whole string as an integer in `base` (2-36), with an optional
// leading sign — nullopt if any character is not a digit of that base, or if
// the base itself is out of range. Backs both `Integer.parse(s, radix: 16)`
// and `s.to(Integer, radix: 16)`.
auto parseIntegerInBase(const std::string& text, int base) -> std::optional<mpz_class>;

// Text content of a String, a Char, or a ListValue whose elements are all
// Char — nullopt for anything else. String/Char/[Char] are meant to be
// interchangeable from the language user's standpoint; this is the shared
// extraction point for that (see valuesEqual, toString(), `+`).
auto textContent(const ValuePtr& v) -> std::optional<std::string>;

} // namespace kex::interpreter
