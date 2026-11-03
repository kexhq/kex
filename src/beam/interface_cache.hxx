#pragma once

// Serialization of the checked interface snapshot (semantic::ImportedInterfaces)
// so the prelude's ~360ms frontend work (ETF-decoding 77 KexI chunks + parsing
// ~41 opt-in stdlib sources) is paid once and then read from a cache.
//
// The format is versioned and self-describing; deserialize returns nullopt on
// any malformed input, so a caller can always fall back to re-derivation.

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
enum class CacheMissReason { badMagic, badVersion, corrupt };

auto cacheMissReasonName(CacheMissReason reason) -> const char*;

auto serializeImportedInterfaces(const kex::semantic::ImportedInterfaces& ifaces)
    -> std::vector<uint8_t>;

auto deserializeImportedInterfaces(const std::vector<uint8_t>& bytes,
                                   CacheMissReason* reason = nullptr)
    -> std::optional<kex::semantic::ImportedInterfaces>;

// Cache-file key: stdlib source hash + compiler version + compiler-binary
// stamp. The stamp (kex::compilerBinaryStamp, "" when unknown) is what keeps
// same-version compiler rebuilds from serving each other's interfaces.
auto interfaceCacheKey(const std::string& sourceHashHex,
                       const std::string& compilerStamp) -> std::string;

// Atomic write (unique staging file + rename into place), so concurrent
// builders never see — or clobber — a half-written cache.
auto writeInterfaceCacheFile(const std::filesystem::path& path,
                             const std::vector<uint8_t>& bytes) -> bool;

// Drop all but the newest `keepNewest` *.bin entries in `dir`, never `keep`
// itself. Best-effort: every failure is silently ignored, the cache stays
// correct (just briefly unpruned) when pruning loses a race.
auto pruneInterfaceCacheDirectory(const std::filesystem::path& dir,
                                  const std::filesystem::path& keep,
                                  size_t keepNewest = 5) -> void;

} // namespace kex::beam
