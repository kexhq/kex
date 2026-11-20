#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Stable codes for the compiler's diagnostics (kexhq/kex#656).
//
// Every kind of problem the compiler reports has one entry in the registry,
// the files under src/common/diagnostic_codes/. An entry names the kind of
// problem, written once by hand:
//
//   KEX_DIAGNOSTIC(TypeMismatch, "KEXET-0001", "Type mismatch: expected {expected}, got {actual}")
//
// - The code is `KEX`, a severity letter (E error, W warning), a stage letter
//   and a number. It is spelled out in the entry, never computed, so moving
//   or deleting entries cannot change another entry's code.
// - The template is the English message. A `{slot}` is filled with a value
//   from the call site: a name, a type, a token, a number. Slots never carry
//   English words, so a catalog for another language can replace the whole
//   sentence and keep the code.
// - A code is never reused. A retired one stays listed with
//   KEX_RETIRED_DIAGNOSTIC so tests can tell.
//
// Stages:
//   L lexical          P parse             M modules (using, export)
//   F purity (foul)    S scope, structure  T types
//   C compile-time evaluation and tagged literals
//   B lowering to the backend
namespace kex::diag {

enum class Code : std::uint16_t {
    None = 0,
#define KEX_DIAGNOSTIC(name, code, english) name,
#define KEX_RETIRED_DIAGNOSTIC(code)
#include "diagnostic_codes/all.def"
#undef KEX_DIAGNOSTIC
#undef KEX_RETIRED_DIAGNOSTIC
};

struct Entry {
    Code code;
    std::string_view name;
    std::string_view id;      // "KEXET-0001"
    std::string_view english; // the template
};

// One value for a template's `{slot}`.
using Arg = std::pair<std::string, std::string>;
using Args = std::vector<Arg>;

// Every entry, in registry order.
auto entries() -> const std::vector<Entry>&;
// Every retired code.
auto retired() -> const std::vector<std::string_view>&;
// The entry of a code; None has none.
auto entry(Code code) -> const Entry*;
// The entry with this id ("KEXET-0001"), case-insensitively.
auto find(std::string_view id) -> const Entry*;

// The message for a code with its slots filled. A slot the call site did
// not give stays as written, `{slot}`, so a missing value shows instead of
// vanishing.
auto render(Code code, const Args& args) -> std::string;
// The same for a template given directly.
auto fill(std::string_view text, const Args& args) -> std::string;
// The names of the slots a template uses, in order of first use.
auto slots(std::string_view text) -> std::vector<std::string>;

// An error of a registered kind thrown where no location is at hand
// (lowering): what() is its English message, and code and args keep what a
// printer or a translation needs.
struct CodedError : std::runtime_error {
    explicit CodedError(Code code, Args args = {})
        : std::runtime_error(render(code, args)), code(code), args(std::move(args)) {}
    Code code;
    Args args;
};

// "KEXET-0001", or "" for None.
auto idOf(Code code) -> std::string_view;
// Where a code is explained.
auto urlOf(Code code) -> std::string;

// "error" or "warning", from the code's severity letter.
auto severityOf(const Entry& entry) -> std::string_view;
// The stage named by the code's stage letter: "types", "parse", ...
auto stageOf(const Entry& entry) -> std::string_view;

// The registry as JSON, for tools outside the compiler (the site that
// explains each code): `kex --explain all --json`. One object with the
// compiler's version, every entry, and the retired codes:
//
//   {"version": "0.4.0", "entries": [{"code": "KEXET-0001",
//     "name": "TypeMismatch", "severity": "error", "stage": "types",
//     "template": "...", "slots": ["expected", "actual"],
//     "url": "https://oops.kex.run/KEXET-0001"}, ...],
//    "retired": ["KEXET-0099", ...]}
auto catalogJson() -> std::string;
// One entry as the object catalogJson lists: `kex --explain CODE --json`.
auto entryJson(const Entry& entry) -> std::string;

} // namespace kex::diag
