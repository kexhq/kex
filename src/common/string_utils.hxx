#pragma once

#include <cctype>
#include <string>

namespace kex {

// Lowercase a whole string.
inline auto lowered(const std::string &s) -> std::string {
  std::string result = s;
  for (auto &c : result)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

// A CamelCase identifier as its conventional snake_case file spelling:
// `MockData` -> `mock_data`, `HTTPServer` -> `http_server`, `MockData2` ->
// `mock_data2`. A boundary is inserted before an upper-case letter that
// follows a lower-case letter, a digit, or an upper-case letter that is itself
// followed by a lower-case one — so an acronym run stays together except at
// its trailing boundary.
inline auto snakeCased(const std::string &part) -> std::string {
  std::string result;
  for (std::size_t i = 0; i < part.size(); ++i) {
    const auto c = static_cast<unsigned char>(part[i]);
    const bool boundary = i > 0 && std::isupper(c) &&
        (std::islower(static_cast<unsigned char>(part[i - 1])) ||
         std::isdigit(static_cast<unsigned char>(part[i - 1])) ||
         (i + 1 < part.size() &&
          std::islower(static_cast<unsigned char>(part[i + 1]))));
    if (boundary && !result.empty() && result.back() != '_') result += '_';
    result += static_cast<char>(std::tolower(c));
  }
  return result;
}

} // namespace kex
