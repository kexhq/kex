#pragma once

// Serialization of the checked interface snapshot (semantic::ImportedInterfaces)
// so the prelude's ~360ms frontend work (ETF-decoding 77 KexI chunks + parsing
// ~41 opt-in stdlib sources) is paid once and then read from a cache.
//
// The format is versioned and self-describing; deserialize returns nullopt on
// any malformed input, so a caller can always fall back to re-derivation.

#include "etf.hxx"
#include "../semantic/traits.hxx"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kex::semantic {
struct ImportedInterfaces;
}

namespace kex::beam {

// Why a cache load missed, for KEX_TIMINGS reporting. `absent` (no file at
// all) is reported by the caller, which is the only side that can observe
// it; deserialize reports the three below.
enum class CacheMissReason { BadMagic, BadVersion, Corrupt };

auto cacheMissReasonName(CacheMissReason reason) -> const char*;

auto serializeImportedInterfaces(const kex::semantic::ImportedInterfaces& ifaces)
    -> std::vector<uint8_t>;

auto deserializeImportedInterfaces(const std::vector<uint8_t>& bytes,
                                   CacheMissReason* reason = nullptr)
    -> std::optional<kex::semantic::ImportedInterfaces>;

// The checker's types and signatures as this cache writes them, for another
// cache (hir/persist) to read and write them the same way.
auto encodeSemanticType(const kex::semantic::TypePtr& type) -> TermPtr;
auto decodeSemanticType(SaxReader& sax) -> kex::semantic::TypePtr;
auto encodeSignature(const kex::semantic::Signature& signature) -> TermPtr;
auto decodeSignature(SaxReader& sax) -> kex::semantic::Signature;

// Drop all but the newest `keepNewest` *.bin entries in `dir`, never `keep`
// itself. Best-effort: every failure is silently ignored, the cache stays
// correct (just briefly unpruned) when pruning loses a race.
auto pruneInterfaceCacheDirectory(const std::filesystem::path& dir,
                                  const std::filesystem::path& keep,
                                  size_t keepNewest = 5) -> void;

} // namespace kex::beam
