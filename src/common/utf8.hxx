#pragma once
// UTF-8 <-> codepoint helpers, plus Unicode simple case mapping.
//
// A Kex String is text, not bytes: `"école".length` is 5, `chars` yields five
// Chars, and `capitalize` reaches the `é`. The BEAM backend gets that for free
// (strings are binaries and `string:uppercase/1` is Unicode-aware); the
// tree-walk interpreter needs these helpers to agree with it.
//
// The case tables are the SIMPLE (1:1, locale-independent) mappings, which is
// what both backends apply — `ß` therefore stays `ß` rather than expanding to
// `SS`. Coverage is the alphabetic ranges that have regular pair structure:
// ASCII, Latin-1 Supplement, Latin Extended-A, Greek, and Cyrillic. Anything
// else maps to itself, which is also correct for scripts without case (`日本語`).
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kex::utf8 {

// Encode one codepoint. A value outside Unicode's range, or a surrogate,
// becomes U+FFFD rather than an invalid byte sequence.
inline auto encode(char32_t cp) -> std::string {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// Byte length of the sequence starting at `s[i]`, or 1 for a malformed lead.
inline auto sequenceLength(std::string_view s, std::size_t i) -> std::size_t {
    auto lead = static_cast<unsigned char>(s[i]);
    std::size_t width = lead < 0x80 ? 1
                      : (lead & 0xE0) == 0xC0 ? 2
                      : (lead & 0xF0) == 0xE0 ? 3
                      : (lead & 0xF8) == 0xF0 ? 4
                      : 1;
    if (i + width > s.size()) return 1;
    for (std::size_t k = 1; k < width; k++)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 1;
    return width;
}

// Decode to codepoints. Malformed bytes pass through as their own value, so a
// round trip through decode/encode never loses data outright — it degrades to
// U+FFFD for exactly the bytes that were already not text.
inline auto decode(std::string_view s) -> std::vector<char32_t> {
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        auto width = sequenceLength(s, i);
        auto lead = static_cast<unsigned char>(s[i]);
        char32_t cp = width == 1 ? lead
                    : width == 2 ? (lead & 0x1Fu)
                    : width == 3 ? (lead & 0x0Fu)
                                 : (lead & 0x07u);
        for (std::size_t k = 1; k < width; k++)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        out.push_back(cp);
        i += width;
    }
    return out;
}

// Codepoint count — a String's `length`/`count`.
inline auto length(std::string_view s) -> std::size_t {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); i += sequenceLength(s, i)) n++;
    return n;
}

inline auto encodeAll(const std::vector<char32_t>& cps) -> std::string {
    std::string out;
    for (auto cp : cps) out += encode(cp);
    return out;
}

// ---- Simple case mapping --------------------------------------------------
// Latin Extended-A is built from alternating upper/lower pairs, but in two
// different phases, and with a handful of codepoints that break the pattern.
namespace detail {
inline auto latinExtendedAUpper(char32_t c) -> char32_t {
    if (c == 0x0131) return 0x0049;              // dotless i -> I
    if (c == 0x017F) return 0x0053;              // long s -> S
    if (c == 0x00FF) return 0x0178;              // ÿ -> Ÿ
    if (c >= 0x0100 && c <= 0x0137) return (c % 2 == 1) ? c - 1 : c;
    if (c >= 0x0139 && c <= 0x0148) return (c % 2 == 0) ? c - 1 : c;
    if (c >= 0x014A && c <= 0x0177) return (c % 2 == 1) ? c - 1 : c;
    if (c >= 0x0179 && c <= 0x017E) return (c % 2 == 0) ? c - 1 : c;
    return c;
}
inline auto latinExtendedALower(char32_t c) -> char32_t {
    if (c == 0x0178) return 0x00FF;              // Ÿ -> ÿ
    if (c == 0x0130) return 0x0069;              // İ -> i
    if (c >= 0x0100 && c <= 0x0137) return (c % 2 == 0) ? c + 1 : c;
    if (c >= 0x0139 && c <= 0x0148) return (c % 2 == 1) ? c + 1 : c;
    if (c >= 0x014A && c <= 0x0177) return (c % 2 == 0) ? c + 1 : c;
    if (c >= 0x0179 && c <= 0x017E) return (c % 2 == 1) ? c + 1 : c;
    return c;
}
} // namespace detail

inline auto toUpper(char32_t c) -> char32_t {
    if (c >= U'a' && c <= U'z') return c - 32;
    // ÷ (U+00F7) sits inside the Latin-1 lowercase block but is a math sign.
    if (c >= 0x00E0 && c <= 0x00FE && c != 0x00F7) return c - 32;
    if (c == 0x00FF || (c >= 0x0100 && c <= 0x017F))
        return detail::latinExtendedAUpper(c);
    if (c == 0x03C2) return 0x03A3;              // final sigma -> Σ
    if (c >= 0x03B1 && c <= 0x03C9) return c - 32;
    if (c >= 0x0430 && c <= 0x044F) return c - 32;
    if (c >= 0x0450 && c <= 0x045F) return c - 80;
    return c;
}

inline auto toLower(char32_t c) -> char32_t {
    if (c >= U'A' && c <= U'Z') return c + 32;
    // × (U+00D7) is the multiplication sign, not a letter.
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) return c + 32;
    if (c == 0x0178 || (c >= 0x0100 && c <= 0x017F))
        return detail::latinExtendedALower(c);
    if (c >= 0x0391 && c <= 0x03A9) return c + 32;
    if (c >= 0x0410 && c <= 0x042F) return c + 32;
    if (c >= 0x0400 && c <= 0x040F) return c + 80;
    return c;
}

inline auto toUpper(std::string_view s) -> std::string {
    std::string out;
    for (auto cp : decode(s)) out += encode(toUpper(cp));
    return out;
}

inline auto toLower(std::string_view s) -> std::string {
    std::string out;
    for (auto cp : decode(s)) out += encode(toLower(cp));
    return out;
}

} // namespace kex::utf8
