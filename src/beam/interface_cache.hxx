#pragma once

// Serialization of the checked interface snapshot (semantic::ImportedInterfaces)
// so the prelude's ~360ms frontend work (ETF-decoding 77 KexI chunks + parsing
// ~41 opt-in stdlib sources) is paid once and then read from a cache.
//
// The format is versioned and self-describing; deserialize returns nullopt on
// any malformed input, so a caller can always fall back to re-derivation.

#include <cstdint>
#include <optional>
#include <vector>

namespace kex::semantic {
struct ImportedInterfaces;
}

namespace kex::beam {

auto serializeImportedInterfaces(const kex::semantic::ImportedInterfaces& ifaces)
    -> std::vector<uint8_t>;

auto deserializeImportedInterfaces(const std::vector<uint8_t>& bytes)
    -> std::optional<kex::semantic::ImportedInterfaces>;

} // namespace kex::beam
