#pragma once

#include "../value.hxx"
#include <optional>
#include <string>
#include <vector>

// Lets String's own operations dispatch to the regex engine when handed a
// `Regex` instead of a String separator, so `str.split(re)` works through the
// same UFCS name as `str.split(",")`. Kept to the few entry points String
// needs rather than exposing the whole engine.
//
// This does not leak the Regex module into the prelude: a `Regex` value can
// only be constructed through `using Regex`, so the dispatch below is
// unreachable without it.
namespace kex::interpreter::regexsupport {

// True when `value` is a compiled `Regex` (the record carrying its source).
auto isRegex(const ValuePtr& value) -> bool;

// Splits `subject` on a Regex, with Ruby's limit semantics. Returns nullopt
// when `regex` isn't a Regex or the build has no PCRE2.
auto split(const std::string& subject, const ValuePtr& regex, int64_t limit)
    -> std::optional<std::vector<ValuePtr>>;

} // namespace kex::interpreter::regexsupport
