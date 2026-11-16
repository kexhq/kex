#pragma once

// What a name's spelling says about it, decided as the lexer decides it.
//
// Kex tells values from types and constructors by the case of a name's first
// letter, and Unicode has that for every script that makes the distinction:
// `Φ` is uppercase exactly as `P` is, and lexes as an uppercase name. A test
// written as `std::isupper(name[0])` reads only the first BYTE, which for `Φ`
// is 0xCE, so everything after the lexer took `let Φ = 1.618` for a value
// binding and printed the function the constant compiles to.

#include "unicode_category.hxx"
#include "utf8.hxx"

#include <string_view>

namespace kex {

// Does `name` start with an uppercase letter (Unicode category Lu, the
// lexer's `upperRanges`)?
inline auto startsUppercase(std::string_view name) -> bool {
    if (name.empty()) {
        return false;
    }
    const auto lead = static_cast<unsigned char>(name.front());
    if (lead < 0x80) {
        return lead >= 'A' && lead <= 'Z';
    }
    const auto decoded = utf8::decode(name.substr(0, utf8::sequenceLength(name, 0)));
    return !decoded.empty() &&
           utf8::categories::inRanges(utf8::categories::upperRanges, decoded.front());
}

// Does `name` start as a value's name does: `_`, an ASCII lowercase letter,
// or a letter outside ASCII that is not uppercase? A script without case
// (Chinese, Arabic, …) has no uppercase form, so the lexer reads its names as
// values, and `árvíz` is one exactly as `arviz` is.
inline auto startsLowercase(std::string_view name) -> bool {
    if (name.empty()) {
        return false;
    }
    const auto lead = static_cast<unsigned char>(name.front());
    if (lead < 0x80) {
        return (lead >= 'a' && lead <= 'z') || lead == '_';
    }
    const auto decoded = utf8::decode(name.substr(0, utf8::sequenceLength(name, 0)));
    return !decoded.empty() &&
           utf8::categories::inRanges(utf8::categories::letterRanges, decoded.front()) &&
           !utf8::categories::inRanges(utf8::categories::upperRanges, decoded.front());
}

// Can this byte be part of a name? A letter, digit or `_` in ASCII, and any
// byte of a multi-byte sequence: the lexer has no operator outside ASCII
// (`a→b` stays three tokens), so a scan for where a name ends can take every
// such byte as the name's.
inline auto isNameByte(char c) -> bool {
    const auto byte = static_cast<unsigned char>(c);
    return byte >= 0x80 || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
}

} // namespace kex
