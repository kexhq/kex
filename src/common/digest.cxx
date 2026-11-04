#include "digest.hxx"

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <stdexcept>
#else
#include <openssl/sha.h>
#endif

namespace kex {

#ifdef __EMSCRIPTEN__
// Returns false when the host has no synchronous digest to offer, rather than
// producing a wrong answer — a bad digest would surface far away, as an
// unexplained "implementation digest mismatch" on a perfectly good artifact.
EM_JS(bool, hostSha256, (const uint8_t* data, size_t len, uint8_t* out), {
    try {
        // `require` is absent when this module is loaded as an ES module, so
        // its absence is a normal answer here, not an error.
        if (typeof require !== "function") return false;
        var digest = require("crypto")
                         .createHash("sha256")
                         .update(Buffer.from(HEAPU8.buffer, data, len))
                         .digest();
        HEAPU8.set(digest, out);
        return true;
    } catch (e) {
        return false;
    }
});
#endif

auto sha256(std::span<const uint8_t> bytes) -> Sha256 {
    Sha256 out{};
#ifdef __APPLE__
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), out.data());
#elif defined(__EMSCRIPTEN__)
    // Emscripten ships no crypto port, and these digests must agree bit for
    // bit with the ones a NATIVE build wrote into the compiled stdlib unit —
    // so hand the work to the host's own implementation rather than carrying
    // one of our own. Node is where this matters: it is the only environment
    // that loads compiled artifacts (the browser REPL ships no unit and takes
    // the source-derived path), and node:crypto's hashing is synchronous,
    // which the SubtleCrypto API in browsers is not.
    if (!hostSha256(bytes.data(), bytes.size(), out.data()))
        throw std::runtime_error(
            "cannot verify compiled artifacts here: this environment provides "
            "no synchronous SHA-256");
#else
    SHA256(bytes.data(), bytes.size(), out.data());
#endif
    return out;
}

auto sha256(std::string_view text) -> Sha256 {
    return sha256(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

auto hexEncode(std::span<const uint8_t> bytes) -> std::string {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out += digits[byte >> 4];
        out += digits[byte & 0xf];
    }
    return out;
}

auto sha256Hex(std::string_view text) -> std::string {
    return hexEncode(sha256(text));
}

auto sha256Hex(std::span<const uint8_t> bytes) -> std::string {
    return hexEncode(sha256(bytes));
}

} // namespace kex
