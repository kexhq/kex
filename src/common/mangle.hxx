#pragma once

// The single source of truth for how a name is qualified with its scope (a
// module path, a type name, a trait, or an intrinsic category). Every backend
// builds and splits these the same way through these helpers, so the separator
// — and, later, the whole convention — changes in one place.
//
// The separator is `.`, Kex's own member syntax: `Net.HTTP.get` is the
// function `get` of `Net.HTTP`, spelled as a program spells it. A name is
// split at its LAST separator, the scope itself being a dotted path.

#include <string>
#include <string_view>

namespace kex::mangle {

inline constexpr std::string_view kSeparator = ".";

// `scope <sep> name`, or `name` alone when `scope` is empty.
inline auto qualify(const std::string &scope, const std::string &name)
    -> std::string {
  if (scope.empty())
    return name;
  std::string out;
  out.reserve(scope.size() + kSeparator.size() + name.size());
  out += scope;
  out += kSeparator;
  out += name;
  return out;
}

// Everything before the LAST separator, or "" when there is none.
inline auto scopeOf(const std::string& name) -> std::string {
    const auto pos = name.rfind(kSeparator);
    return pos == std::string::npos ? std::string{} : name.substr(0, pos);
}

// Everything after the LAST separator, or `name` when there is none.
inline auto memberOf(const std::string &name) -> std::string {
  const auto pos = name.rfind(kSeparator);
  return pos == std::string::npos ? name : name.substr(pos + kSeparator.size());
}

// Whether `name` carries a scope.
inline auto hasScope(const std::string& name) -> bool {
    return name.find(kSeparator) != std::string::npos;
}

} // namespace kex::mangle
