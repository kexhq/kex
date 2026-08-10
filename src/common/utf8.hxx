#pragma once
// UTF-8 <-> codepoint helpers, plus Unicode simple case mapping.
//
// A Kex String is text, not bytes: `"école".length` is 5, `chars` yields five
// Chars, and `capitalize` reaches the `é`. The BEAM backend gets that for free
// (strings are binaries and `string:uppercase/1` is Unicode-aware); the
// tree-walk interpreter needs these helpers to agree with it.
//
// Case mapping covers all of Unicode, from the generated tables in
// unicode_case.hxx, and comes in the two flavours the language needs: a Char
// maps 1:1 (`Char -> Char` cannot expand), while a String uses the full
// mapping and may grow — `"straße".upperCase` is `"STRASSE"`. Both are
// locale-independent and applied per codepoint, which is what BEAM's
// `string:uppercase/1` does; `tools/unicode_case_gen.py` regenerates them.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "unicode_case.hxx"

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

// ---- Case mapping ---------------------------------------------------------
namespace detail {

template <std::size_t N>
auto findRun(const tables::CaseRun (&runs)[N], char32_t c)
    -> const tables::CaseRun* {
    std::size_t low = 0, high = N;
    while (low < high) {
        auto mid = low + (high - low) / 2;
        if (c < runs[mid].start) high = mid;
        else if (c > runs[mid].end) low = mid + 1;
        else return &runs[mid];
    }
    return nullptr;
}

template <std::size_t N>
auto mapSimple(const tables::CaseRun (&runs)[N], char32_t c) -> char32_t {
    const auto* run = findRun(runs, c);
    // A stride of 2 covers alternating upper/lower pairs, so landing inside
    // the range is not enough — the codepoint must be ON the step.
    if (!run || (c - run->start) % run->stride != 0) return c;
    return static_cast<char32_t>(static_cast<std::int32_t>(c) + run->delta);
}

template <std::size_t N>
auto findExpansion(const tables::FullCase (&entries)[N], char32_t c)
    -> const tables::FullCase* {
    std::size_t low = 0, high = N;
    while (low < high) {
        auto mid = low + (high - low) / 2;
        if (c < entries[mid].from) high = mid;
        else if (c > entries[mid].from) low = mid + 1;
        else return &entries[mid];
    }
    return nullptr;
}

} // namespace detail

// Simple (1:1) mapping — what a Char maps to, since a Char must stay one Char.
inline auto toUpper(char32_t c) -> char32_t {
    return detail::mapSimple(tables::simpleUpper, c);
}
inline auto toLower(char32_t c) -> char32_t {
    return detail::mapSimple(tables::simpleLower, c);
}

// Full mapping — a String may grow ("straße" -> "STRASSE"). Applied per
// codepoint, so the context-sensitive rules (Greek final sigma) do NOT fire;
// that matches BEAM's string:uppercase/1 and string:lowercase/1.
inline auto toUpper(std::string_view s) -> std::string {
    std::string out;
    for (auto cp : decode(s)) {
        if (const auto* wide = detail::findExpansion(tables::expandUpper, cp)) {
            for (std::uint8_t i = 0; i < wide->length; i++)
                out += encode(wide->to[i]);
        } else {
            out += encode(toUpper(cp));
        }
    }
    return out;
}

inline auto toLower(std::string_view s) -> std::string {
    std::string out;
    for (auto cp : decode(s)) {
        if (const auto* wide = detail::findExpansion(tables::expandLower, cp)) {
            for (std::uint8_t i = 0; i < wide->length; i++)
                out += encode(wide->to[i]);
        } else {
            out += encode(toLower(cp));
        }
    }
    return out;
}

} // namespace kex::utf8
