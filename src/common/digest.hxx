#pragma once

// SHA-256 and hex encoding: the one implementation every cache key, content
// hash and `Digest.sha256` goes through. The digest comes from the platform
// (CommonCrypto, OpenSSL, or the wasm host's node:crypto), never from code of
// our own, so every build agrees bit for bit with the digests a native build
// wrote into the compiled stdlib unit.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace kex {

using Sha256 = std::array<uint8_t, 32>;

auto sha256(std::span<const uint8_t> bytes) -> Sha256;
auto sha256(std::string_view text) -> Sha256;

// Lower-case hex of `bytes`, two digits per byte.
auto hexEncode(std::span<const uint8_t> bytes) -> std::string;

// sha256 written as 64 lower-case hex digits: the shape of every cache key.
auto sha256Hex(std::string_view text) -> std::string;
auto sha256Hex(std::span<const uint8_t> bytes) -> std::string;

} // namespace kex
