#pragma once

// The single source of truth for how a name is qualified with its scope (a
// module path, a type name, a trait, or an intrinsic category). Every backend
// builds and splits these the same way through these helpers, so the separator
// — and, later, the whole convention — changes in one place.
//
// Kept as `::` for now so this centralization is purely behavior-preserving;
// it flips to `.` (matching Kex's own member syntax) once every backend reads
// it from here.
// TODO: use `.` for the separator once every backend reads it from here.

#include <string>
#include <string_view>

namespace kex::mangle {

inline constexpr std::string_view kSeparator = "::";

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

// The first segment of `name` (before the FIRST separator), or `name`.
inline auto headOf(const std::string &name) -> std::string {
  const auto pos = name.find(kSeparator);
  return pos == std::string::npos ? name : name.substr(0, pos);
}

// Everything after the FIRST separator, or "" when there is none.
inline auto tailOf(const std::string& name) -> std::string {
    const auto pos = name.find(kSeparator);
    return pos == std::string::npos ? std::string{}
                                    : name.substr(pos + kSeparator.size());
}

// Everything before the LAST separator, or "" when there is none.
inline auto scopeOf(const std::string& name) -> std::string {
    const auto pos = name.rfind(kSeparator);
    return pos == std::string::npos ? std::string{} : name.substr(0, pos);
}

// A dotted source path's module, everything before its LAST `.`
// (`Mock.Files` → `Mock`), or "" for a bare name.
inline auto pathScopeOf(const std::string& path) -> std::string {
    const auto pos = path.rfind('.');
    return pos == std::string::npos ? std::string{} : path.substr(0, pos);
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

// A source dotted path (`Net.HTTP`) in the internal mangled spelling
// (`Net::HTTP`): each `.` becomes the separator.
inline auto manglePath(const std::string& path) -> std::string {
    std::string out;
    out.reserve(path.size());
    for (char c : path)
        if (c == '.') out += kSeparator;
        else out += c;
    return out;
}

} // namespace kex::mangle
