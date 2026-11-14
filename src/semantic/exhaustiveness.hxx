#pragma once
// Which values a `match` leaves unmatched, from its arms' patterns: the
// usefulness algorithm over a pattern matrix (Maranget, "Warnings for
// pattern matching", 2007), reduced to what Kex patterns need. A pattern is
// either anything (a variable, `_`) or a constructor with argument patterns;
// a constructor belongs to a set of siblings that together cover its type,
// or to a type no finite set covers (a literal's).
//
// The checker translates its patterns into this (TypeChecker::
// checkPatternMatrix) and reports what comes back.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kex::semantic::exhaustiveness {

struct Space {
    // Empty for "anything".
    std::string constructor;
    std::vector<Space> arguments;
};

// The constructors that together cover the type `constructor` builds, each
// with its arity; none when no finite set does (an Integer literal's).
using Siblings =
    std::function<std::optional<std::vector<std::pair<std::string, std::size_t>>>(const std::string&)>;

// Values no row matches, as rows of `width` spaces, at most `limit` of them;
// empty when the rows cover every value.
auto uncovered(const std::vector<std::vector<Space>>& rows, std::size_t width,
               const Siblings& siblings, std::size_t limit) -> std::vector<std::vector<Space>>;

} // namespace kex::semantic::exhaustiveness
