#pragma once

#include <string>
#include <vector>

namespace kex::lsp {

// The directory of the nearest enclosing `package.kex`, walking up from
// `start`, or empty when the file belongs to no tey package. The package's
// own `src/` is the first `--source-root` a build of it passes (tey's
// `sourceRoots`), so the editor has to find the same directory or a nested
// module's `using` lines cannot resolve.
auto teyPackageDirectory(const std::string& start) -> std::string;

// The `--source-root` list `tey` would pass for a file inside a tey package:
// one entry per locked dependency, `${cache}/src/<source>/<commit>/src`.
//
// The editor and `tey build` have to agree about where modules live, or a
// dependency's every use is flagged undefined in the editor while the same
// file compiles. `tey` derives these roots from `tey.lock`
// (tey/src/tey/commands.kex, `dependencyRoots`); this is that derivation, in
// the process that answers the editor.
//
// Missing lockfile, unreadable JSON, a dependency not yet fetched: all
// degrade to a shorter list, never an error. `tey install` is the answer to a
// dependency the cache does not have, and the resolver's own
// "module not found in source roots" warning says so better than a thrown
// exception would.
//
// `start` may be a file or a directory; the nearest enclosing `package.kex`
// decides which lockfile is read. Results are cached per package directory
// and re-derived when `tey.lock` changes on disk, so `tey install` behind the
// server's back is picked up without a restart.
auto teyDependencyRoots(const std::string& start) -> std::vector<std::string>;

// Where a checkout of `gitUrl` at `commit` lives, by tey's cache layout
// (tey/src/tey/cache.kex). Exposed for tests.
auto teyCachePackagePath(const std::string& gitUrl, const std::string& commit)
    -> std::string;

} // namespace kex::lsp
