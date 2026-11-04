#pragma once

// What every kex cache shares: where caches live, which toolchain wrote an
// entry, and how an entry is written so no reader ever sees half of it. The
// run, beam, discovery, template-scan, separate-unit and prelude-interface
// caches all go through these, so `KEX_CACHE=off` and a rebuilt compiler mean
// the same thing to every one of them.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace kex::cache {

// The root every cache lives under: $KEX_CACHE, else $XDG_CACHE_HOME/kex, else
// ~/.cache/kex. nullopt — every cache off — when KEX_CACHE is `off`, `0` or
// empty (kexhq/kex#323), or no location is known.
auto base() -> std::optional<std::filesystem::path>;

// `base() / subdirectory`, or nullopt when caching is off.
auto directory(std::string_view subdirectory)
    -> std::optional<std::filesystem::path>;

// Identifies the toolchain that would write an entry: version, git revision,
// the running compiler binary (path, size, mtime — kexhq/kex#372: a rebuild
// from uncommitted changes must not reuse the old build's entries), and every
// standard-library source, prelude and opt-in alike (size and mtime: editing a
// stdlib file rebuilds the prelude without relinking this binary). Computed
// once per process; stats files, never reads them.
auto toolchainFingerprint() -> const std::string&;

// A suffix no other writer of the same entry picks — a random per-process
// value plus a counter — for the staging file a write renames into place.
auto stagingSuffix() -> std::string;

// Writes `bytes` beside `path` and renames the result into place, creating
// parent directories. Readers see the old entry or the whole new one, never
// part of one; the staging file never outlives a failed write. False when
// anything failed — for a cache that only means the next build recomputes.
auto atomicWrite(const std::filesystem::path& path, std::string_view bytes)
    -> bool;

// Copies `source` to `path` the same way.
auto atomicCopy(const std::filesystem::path& source,
                const std::filesystem::path& path) -> bool;

} // namespace kex::cache
