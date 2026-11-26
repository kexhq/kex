#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace kex {

// Lowercase a whole string.
inline auto lowered(std::string_view s) -> std::string {
    std::string result(s);
    for (auto& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

// `text` without the white space it starts or ends with.
inline auto trimmed(std::string_view text) -> std::string {
    const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

// `text` split at its line ends, a trailing one starting no empty line.
inline auto splitLines(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::string::size_type start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// A CamelCase identifier as its conventional snake_case file spelling:
// `MockData` -> `mock_data`, `HTTPServer` -> `http_server`, `MockData2` ->
// `mock_data2`. A boundary is inserted before an upper-case letter that
// follows a lower-case letter, a digit, or an upper-case letter that is itself
// followed by a lower-case one — so an acronym run stays together except at
// its trailing boundary.
inline auto snakeCased(const std::string& part) -> std::string {
    std::string result;
    for (std::size_t i = 0; i < part.size(); ++i) {
        const auto c = static_cast<unsigned char>(part[i]);
        const bool boundary =
            i > 0 && std::isupper(c) &&
            (std::islower(static_cast<unsigned char>(part[i - 1])) ||
             std::isdigit(static_cast<unsigned char>(part[i - 1])) ||
             (i + 1 < part.size() && std::islower(static_cast<unsigned char>(part[i + 1]))));
        if (boundary && !result.empty() && result.back() != '_') {
            result += '_';
        }
        result += static_cast<char>(std::tolower(c));
    }
    return result;
}

// `parts` joined by `separator`: `join({"Html", "Element"}, ".")` is
// `Html.Element`. Only the first `count` parts when it is given.
inline auto join(
    const std::vector<std::string>& parts,
    std::string_view separator,
    std::size_t count = std::string::npos) -> std::string {
    std::string result;
    const auto end = count < parts.size() ? count : parts.size();
    for (std::size_t i = 0; i < end; ++i) {
        if (i) {
            result += separator;
        }
        result += parts[i];
    }
    return result;
}

// `text` as the body of a Kex literal quoted by `quote` (`"` for a String,
// `'` for a Char), using the lexer's escape set (Lexer::lexEscape): the
// quote, a backslash and the control characters are escaped, and so is the
// `$` that would open an interpolation, so the result reads back as `text`.
// runtime/src/kex_io.erl's escape_literal/2 is the BEAM copy.
inline auto escapedLiteral(std::string_view text, char quote) -> std::string {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\x1b': result += "\\e"; break;
            case '\0': result += "\\0"; break;
            default:
                if (c == quote) {
                    result += '\\';
                    result += c;
                } else if (c == '$' && quote == '"' && i + 1 < text.size() && text[i + 1] == '{') {
                    result += "\\$";
                } else if (static_cast<unsigned char>(c) < 0x20 || c == '\x7f') {
                    const auto byte = static_cast<unsigned char>(c);
                    result += "\\x";
                    result += kHex[byte >> 4];
                    result += kHex[byte & 0xF];
                } else {
                    result += c;
                }
        }
    }
    return result;
}

} // namespace kex
