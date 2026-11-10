// The Erlang toolchain kex drives and the caches around it: `erl` and `erlc`
// (KEX_ERL / PATH), the compiled-module cache (Core Erlang → .beam, by the
// erlc that compiled it), the run cache and its manifest (a `-c --run` whose
// inputs an earlier build already covers), and the guard that refuses an OTP
// older than the runtime. Moved out of main.cxx.
#include "../cache/cache.hxx"
#include "../ir/emit_core.hxx"
#include "../compiled/expand.hxx"
#include "dependencies.hxx"
#include "build_support.hxx"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <cerrno>
#include <csignal>
#endif
#include "../common/digest.hxx"
#include "../common/prelude_loader.hxx"

using kex::driver::LoadedDep;

// The `erl` kex spawns for every BEAM mode (run -R, compile-run, the BEAM
// REPL). KEX_ERL overrides PATH, mirroring tey's TEY_ERL — the escape hatch
// for when the erl first on PATH is older than the runtime beams' compile
// OTP but a newer one is installed elsewhere.
auto erlExecutable() -> std::string {
  if (const char *override = std::getenv("KEX_ERL"); override && *override)
    return override;
  return "erl";
}

// The `erlc` kex shells out to for Core Erlang. It has to come from the same
// OTP install as erlExecutable() or KEX_ERL only half-works: pointing at a
// newer erl while PATH's erlc stays behind compiles session modules with one
// toolchain and loads them with another. erlc always sits beside erl, so an
// absolute KEX_ERL names its erlc too; KEX_ERLC overrides that directly.
auto erlcExecutable() -> std::string {
  if (const char *override = std::getenv("KEX_ERLC"); override && *override)
    return override;
  if (const char *erl = std::getenv("KEX_ERL"); erl && *erl) {
    std::filesystem::path p{erl};
    if (p.has_parent_path()) {
      auto sibling = p.parent_path() / "erlc";
      std::error_code ec;
      if (std::filesystem::exists(sibling, ec))
        return sibling.string();
    }
  }
  return "erlc";
}

// ── Compiled-module cache (kexhq/kex#323) ───────────────────────────────────
//
// erlc is most of a build: every `--compile` re-ran it over every reachable
// module, including opt-in stdlib modules that never change. A module's .beam
// is a pure function of its Core Erlang text and the erlc that compiled it, so
// the cache is content-addressed on exactly that and needs no dependency
// tracking to stay correct: anything upstream that changes what a module
// compiles to changes its .core text, and so its key. The KexI chunk is not
// cached — it is attached afresh after every build from the program's own
// analysis, exactly as for a freshly compiled module.

// Where cached .beam files live: $KEX_CACHE, else $XDG_CACHE_HOME/kex, else
// ~/.cache/kex. `KEX_CACHE=off` (or `0`, or empty) turns the cache off.
auto beamCacheRoot() -> std::optional<std::filesystem::path> {
  if (auto base = kex::cache::base()) return *base / "beam";
  return std::nullopt;
}

// The erlc actually run, as a string that changes whenever it does: its
// resolved path (an OTP upgrade lands in a new versioned directory) plus the
// file's size and modification time. Asking erlc for its OTP release would
// start a VM — the very cost the cache exists to avoid. `erlc` is only a
// launcher, though: what compiles is the installation's compiler
// application, which an update in place can replace without touching it. So
// the release (`releases/<n>/OTP_VERSION`) and the compiler application's
// versioned directory are part of it too, read from the files, and so is
// `ERL_COMPILER_OPTIONS`, which erlc applies to everything it compiles.
auto erlcIdentity() -> std::string {
  namespace fs = std::filesystem;
  std::string command = erlcExecutable();
  fs::path located{command};
  if (!located.has_parent_path())
    if (const char *path = std::getenv("PATH")) {
      std::string directories = path;
      size_t start = 0;
      while (start <= directories.size()) {
        const auto end = directories.find(':', start);
        const auto directory = directories.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::error_code ec;
        if (!directory.empty() && fs::exists(fs::path{directory} / command, ec)) {
          located = fs::path{directory} / command;
          break;
        }
        if (end == std::string::npos) break;
        start = end + 1;
      }
    }
  std::error_code ec;
  const auto canonical = fs::canonical(located, ec);
  if (ec) return command;
  const auto size = fs::file_size(canonical, ec);
  const auto modified = fs::last_write_time(canonical, ec).time_since_epoch().count();
  auto identity = canonical.string() + "|" +
                  std::to_string(static_cast<unsigned long long>(size)) + "|" +
                  std::to_string(static_cast<long long>(modified));
  // bin/erlc sits in the installation's root.
  const auto root = canonical.parent_path().parent_path();
  std::vector<std::string> parts;
  for (const auto &release : fs::directory_iterator(root / "releases", ec)) {
    std::ifstream input(release.path() / "OTP_VERSION");
    std::string version;
    if (input && std::getline(input, version))
      parts.push_back("otp:" + release.path().filename().string() + "=" + version);
  }
  for (const auto &application : fs::directory_iterator(root / "lib", ec))
    if (application.path().filename().string().rfind("compiler-", 0) == 0)
      parts.push_back(application.path().filename().string());
  std::sort(parts.begin(), parts.end());
  for (const auto &part : parts) identity += "|" + part;
  if (const char *options = std::getenv("ERL_COMPILER_OPTIONS"))
    identity += "|options:" + std::string(options);
  return identity;
}

// The cache key for one module: SHA-256 over everything its .beam depends on,
// and nothing else. The compiler's own version is not part of it: whatever a
// new kex emits differently is in the Core Erlang text, so its key moves
// anyway. Keyed on the git revision as well, every rebuild of kex threw the
// whole cache away, and Tey's 82 modules went back through erlc (45s of a 76s
// build) after any commit at all.
//
// So the cache holds exactly what erlc wrote, and nothing of kex's own: the
// KexI chunk is attached to the copy in the build's output, after it leaves
// the cache, every build. Anything that changes what an entry holds — a beam
// post-processed before it is stored, another layout — must bump the
// `kex-beam-cache-v2` tag, or a newer kex reads an older entry as its own.
auto beamCacheKey(const std::string &coreSource, const std::string &erlc,
                  const std::string &flags) -> std::string {
  std::string material = "kex-beam-cache-v2\n" + erlc + "\n" + flags + "\n" +
                         coreSource;
  return kex::sha256Hex(material);
}

// Stores a freshly compiled .beam under its key. Written beside the final name
// and renamed into place, so two builds sharing the cache never read half a
// file; any failure only means the next build compiles that module again.
auto storeCachedBeam(const std::filesystem::path &entry,
                     const std::filesystem::path &beam) -> void {
  kex::cache::atomicCopy(beam, entry);
}

// ── Run-result cache (kexhq/kex#323) ────────────────────────────────────────
//
// The compiled-module cache above skips erlc for a module whose Core Erlang
// is unchanged, but kex's own frontend (parse the dependency closure,
// type-check it, lower it, emit Core Erlang) still reruns from scratch on
// every invocation — the cost `tey/spec/*.spec.kex` pays 18 times over for
// the ~15-26 Tey/stdlib modules every file depends on identically. Measured
// in isolation (KEX_TIMINGS=1, warm .beam cache): a single file's frontend
// plus erlc costs ~2.1s even with nothing to recompile.
//
// True per-module reuse (skip typechecking a dependency at all, only that
// dependency, only when IT changed) needs real separate compilation — the
// whole program is still typechecked and lowered as one merged AST, so a
// dependency's own cost isn't separable from the entry file's. That is
// "module system and packaging" work, tracked in docs/module-sys-plan.md
// rather than attempted here.
//
// What IS safe to cache without that: for `kex --run` (Mode::Compile,
// compileRun — a throwaway compile-then-execute with no persistent .beam
// artifact to keep in sync), the entire frontend+erlc result is a pure
// function of the entry file's bytes, every resolved dependency's bytes, and
// the toolchain — exactly like the caches above, just drawn one level
// higher, and content-addressed the same way so a wrong hit is not possible.
// A hit skips straight to running `erl` against the cached .beam files.
//
// The one input this can't see is `Kex.embed(path)` — inside a `compiled do`
// block or, via `Template.text`/`Template.html`, standing alone at any call
// site — which reads a file at expansion time that isn't any module's own
// source (kexhq/kex#335 wants that to be inspectable at run time too, which
// would make this worse, not better). Rather than track embed reads, this
// cache simply refuses to apply — falls back to the normal, always-correct
// path — whenever "compiled" or "Kex.embed" appears anywhere in the entry
// file, the merged `.spec.kex` base, or any resolved dependency's source
// text. A false positive (the word appears in a comment or string) only
// costs a cache miss; it can never cause a stale hit.
auto runCacheRoot() -> std::optional<std::filesystem::path> {
  if (auto base = kex::cache::base()) return *base / "run";
  return std::nullopt;
}

// A conservative, lexer-free scan for the actual `compiled do ... end` block
// syntax — not just the English word "compiled", which shows up constantly
// in ordinary comments and doc text ("a compiled Kex module", "gets compiled
// into a .beam"). Requires the KEYWORD "compiled" (word-bounded, so it
// doesn't fire inside `precompiled` either) followed, after whitespace only,
// by the keyword "do". A false positive here only costs a cache miss; a
// false negative would let a stale hit through, so this stays deliberately
// narrow rather than trying to also rule out e.g. a same-named identifier.
auto sourceMentionsCompiledBlock(const std::string &src) -> bool {
  auto isWordChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '?' || c == '!';
  };
  size_t pos = 0;
  while ((pos = src.find("compiled", pos)) != std::string::npos) {
    const bool leftBoundary = pos == 0 || !isWordChar(src[pos - 1]);
    size_t after = pos + 8; // strlen("compiled")
    const bool rightBoundary = after >= src.size() || !isWordChar(src[after]);
    if (leftBoundary && rightBoundary) {
      while (after < src.size() && std::isspace(static_cast<unsigned char>(src[after])))
        ++after;
      if (src.compare(after, 2, "do") == 0 &&
          (after + 2 >= src.size() || !isWordChar(src[after + 2])))
        return true;
    }
    pos += 8;
  }
  return false;
}

// A conservative, lexer-free scan for `Kex.embed(...)` — the call form
// `EmbedFolder::claim` (src/compiled/expand.cxx) recognizes, requiring the
// literal `Kex` receiver (no alias or bare `embed(...)` is matched by that
// macro, so none needs matching here either). Reachable outside any
// `compiled do` block — `Template.text(Kex.embed(path))` and
// `Template.html(Kex.embed(path))` call it directly — so
// `sourceMentionsCompiledBlock` alone missed it: a file changed between two
// runs of the exact same entry+deps was served the FIRST run's embedded
// text, silently, forever (kexhq/kex#reported-as-run-cache-staleness). Same
// false-positive-tolerant, false-negative-intolerant contract as that scan.
auto sourceMentionsEmbed(const std::string &src) -> bool {
  auto isWordChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '?' || c == '!';
  };
  size_t pos = 0;
  while ((pos = src.find("Kex", pos)) != std::string::npos) {
    const bool leftBoundary = pos == 0 || !isWordChar(src[pos - 1]);
    size_t after = pos + 3; // strlen("Kex")
    if (leftBoundary && src.compare(after, 1, ".") == 0 &&
        src.compare(after + 1, 5, "embed") == 0) {
      const size_t nameEnd = after + 1 + 5;
      if (nameEnd >= src.size() || !isWordChar(src[nameEnd]))
        return true;
    }
    pos += 3;
  }
  return false;
}

// nullopt means "don't even try": the cache is off, or something in the
// closure makes it unsafe (see the block comment above).
// KEX_RUN_CACHE_DEBUG=1 says which of the three provably-safe checks kept
// this run off the cache — same spirit as KEX_TIMINGS, just for "did it
// apply" instead of "how long did it take".
auto runCacheMiss(const char *why) -> std::optional<std::string> {
  if (std::getenv("KEX_RUN_CACHE_DEBUG"))
    std::fprintf(stderr, "run cache: miss (%s)\n", why);
  return std::nullopt;
}

auto runCacheKeyFor(const std::string &filepath, bool skipCheck,
                    const std::string &entryRawSource,
                    const std::string &specBaseFile,
                    const std::string &specBaseRawSource,
                    const std::vector<LoadedDep> &deps)
    -> std::optional<std::string> {
  if (!runCacheRoot()) return runCacheMiss("KEX_CACHE is off");
  if (sourceMentionsCompiledBlock(entryRawSource))
    return runCacheMiss("entry file has a `compiled do` block");
  if (sourceMentionsEmbed(entryRawSource))
    return runCacheMiss("entry file calls `Kex.embed`");
  if (!specBaseFile.empty()) {
    if (sourceMentionsCompiledBlock(specBaseRawSource))
      return runCacheMiss("its .spec.kex base has a `compiled do` block");
    if (sourceMentionsEmbed(specBaseRawSource))
      return runCacheMiss("its .spec.kex base calls `Kex.embed`");
  }
  std::vector<const LoadedDep *> sorted;
  for (const auto &dep : deps) sorted.push_back(&dep);
  std::sort(sorted.begin(), sorted.end(), [](const auto *a, const auto *b) {
    return *a->path < *b->path;
  });
  std::string material = "kex-run-cache-v2\n" + kex::cache::toolchainFingerprint() +
                        "\n" +
                        filepath + "\n" +
                        (skipCheck ? "nocheck\n" : "check\n");
  material += "entry:" + kex::sha256Hex(entryRawSource) + "\n";
  if (!specBaseFile.empty())
    material += "specbase:" + specBaseFile + ":" +
                kex::sha256Hex(specBaseRawSource) + "\n";
  for (const auto *dep : sorted) {
    if (sourceMentionsCompiledBlock(*dep->source))
      return runCacheMiss(("a dependency (" + *dep->path +
                           ") has a `compiled do` block")
                              .c_str());
    if (sourceMentionsEmbed(*dep->source))
      return runCacheMiss(
          ("a dependency (" + *dep->path + ") calls `Kex.embed`").c_str());
    material += "dep:" + *dep->path + ":" + kex::sha256Hex(*dep->source) + "\n";
  }
  return kex::sha256Hex(material);
}

auto runCacheDir(const std::string &key) -> std::filesystem::path {
  return *runCacheRoot() / key.substr(0, 2) / key;
}

// A cache hit: every module the previous matching run produced, in the same
// order (index 0 is the entry module — order matters for the BEAM load
// expression and for `mainArity`, which is only meaningful on that one).
auto tryLoadRunCacheFrom(const std::filesystem::path &dir, const std::string &outputDir)
    -> std::optional<std::vector<kex::ir::EmitResult>> {
  namespace fs = std::filesystem;
  std::ifstream manifest(dir / "manifest.txt");
  if (!manifest) return std::nullopt;
  std::string line;
  if (!std::getline(manifest, line) || line != "kex-run-cache-v1")
    return std::nullopt;
  std::vector<kex::ir::EmitResult> results;
  while (std::getline(manifest, line)) {
    if (line.empty()) continue;
    const auto tab = line.find('\t');
    if (tab == std::string::npos) return std::nullopt;
    kex::ir::EmitResult result;
    result.moduleName = line.substr(0, tab);
    try {
      result.mainArity = std::stoi(line.substr(tab + 1));
    } catch (const std::exception &) {
      return std::nullopt;
    }
    results.push_back(std::move(result));
  }
  if (results.empty()) return std::nullopt;
  std::error_code ec;
  for (const auto &result : results) {
    const auto cached = dir / (result.moduleName + ".beam");
    const auto target =
        fs::path{outputDir} / (result.moduleName + ".beam");
    if (!fs::is_regular_file(cached, ec)) return std::nullopt;
    fs::copy_file(cached, target, fs::copy_options::overwrite_existing, ec);
    if (ec) return std::nullopt;
  }
  return results;
}

auto tryLoadRunCache(const std::string &key, const std::string &outputDir)
    -> std::optional<std::vector<kex::ir::EmitResult>> {
  return tryLoadRunCacheFrom(runCacheDir(key), outputDir);
}

// Written beside the final name and renamed into place, matching every other
// cache in this file — a build reading it mid-write never sees a half-copied
// entry, and a failed write only costs the next run a cache miss.
auto storeRunCacheIn(const std::filesystem::path &dir, const std::string &outputDir,
                     const std::vector<kex::ir::EmitResult> &results) -> bool {
  const std::filesystem::path output{outputDir};
  for (const auto &result : results)
    if (!kex::cache::atomicCopy(output / (result.moduleName + ".beam"),
                                dir / (result.moduleName + ".beam")))
      return false;
  std::string manifest = "kex-run-cache-v1\n";
  for (const auto &result : results)
    manifest += result.moduleName + "\t" + std::to_string(result.mainArity) + "\n";
  return kex::cache::atomicWrite(dir / "manifest.txt", manifest);
}

auto storeRunCache(const std::string &key, const std::string &outputDir,
                   const std::vector<kex::ir::EmitResult> &results) -> void {
  (void)storeRunCacheIn(runCacheDir(key), outputDir, results);
}

// ── Run cache by entry (kexhq/kex#323) ──────────────────────────────────────
//
// The cache above is keyed by the whole closure's content, so it is only
// known once every dependency has been found — after the gating pre-check
// and dependency loading, most of a large program's build — and it refuses
// any program with a `compiled do` block or `Kex.embed` in it, whose result
// depends on files no module's source names. This one is keyed by the entry
// and the flags alone, and holds, beside the modules, every file the build
// that made them read, with its stamp (compiled::fileStamp): the entry, its
// `.spec.kex` base, every dependency, everything compile-time evaluation
// loaded or embedded (the build's DependencyRecorder), and each place a new
// file could take a module's name from it — the same relative path under
// every other root, and every base a spec could have. When every stamp still
// holds, the run starts from these modules at once; any one changed, and the
// build runs as it would have.
//
// Not kept when a compile-time evaluation reached outside the program
// (compiled::reachedOutside), nor when a file it read changed while it ran.

// The process's own recorder, for the whole of a build that reuses
// collapses (main): what the entry run cache lists.
std::optional<kex::compiled::DependencyRecorder> buildRecorder;

// When this process started building: a file modified since may have changed
// after it was read, and its stamp would vouch for what was not compiled.
const auto buildStarted = std::filesystem::file_time_type::clock::now();

auto runManifestKey(const std::string &flags, const std::string &filepath)
    -> std::optional<std::string> {
  if (!runCacheRoot()) return std::nullopt;
  std::error_code ec;
  const auto entry = std::filesystem::weakly_canonical(filepath, ec);
  if (ec) return std::nullopt;
  // The compiler that made the kept modules too: the .beam cache below keys
  // on it, but a run started from this entry never reaches that cache.
  return kex::sha256Hex("kex-run-by-entry-v2\n" + kex::cache::toolchainFingerprint() + "\n" +
                   erlcIdentity() + "\n" + entry.string() + "\n" + flags);
}

auto runManifestDir(const std::string &key) -> std::filesystem::path {
  return *runCacheRoot() / "entry" / key.substr(0, 2) / key;
}

// Whether the build an entry holds was made from files exactly as they are.
auto runManifestCurrent(const std::string &key) -> bool {
  std::ifstream in(runManifestDir(key) / "files.txt");
  std::string line;
  if (!in || !std::getline(in, line) || line != "kex-run-by-entry-v1") return false;
  bool any = false;
  while (std::getline(in, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) return false;
    if (kex::compiled::fileStamp(line.substr(tab + 1)) != line.substr(0, tab))
      return false;
    any = true;
  }
  return any;
}

// What an entry's build was made from (see above), canonically named.
auto runManifestFiles(const std::string &filepath, const std::string &specBase,
                      const std::vector<LoadedDep> &deps) -> std::set<std::string> {
  namespace fs = std::filesystem;
  std::set<std::string> files;
  auto add = [&](const std::string &path) {
    if (path.empty()) return;
    std::error_code ec;
    auto canonical = fs::weakly_canonical(path, ec);
    files.insert(ec ? path : canonical.string());
  };
  if (buildRecorder) files = buildRecorder->files();
  add(filepath);
  add(specBase);
  for (const auto &candidate : specBaseCandidates(filepath)) add(candidate);
  for (const auto &dep : deps) add(*dep.path);
  auto roots = moduleRootsFor(filepath);
  for (auto &root : kex::standardLibraryModuleRoots()) roots.push_back(std::move(root));
  const auto shadows = kex::compiled::shadowsOf(files, roots);
  files.insert(shadows.begin(), shadows.end());
  return files;
}

auto storeRunManifest(const std::string &key, const std::string &outputDir,
                      const std::vector<kex::ir::EmitResult> &results,
                      const std::set<std::string> &files) -> void {
  namespace fs = std::filesystem;
  std::string stamps;
  for (const auto &file : files) {
    std::error_code ec;
    if (fs::exists(file, ec) && fs::last_write_time(file, ec) >= buildStarted)
      return;
    stamps += kex::compiled::fileStamp(file) + "\t" + file + "\n";
  }
  const auto dir = runManifestDir(key);
  std::error_code ec;
  // The file list last: until it is in place, the entry is not current.
  fs::remove(dir / "files.txt", ec);
  if (!storeRunCacheIn(dir, outputDir, results)) return;
  kex::cache::atomicWrite(dir / "files.txt", "kex-run-by-entry-v1\n" + stamps);
}

#ifndef KEX_RUNTIME_OTP_FLOOR
#define KEX_RUNTIME_OTP_FLOOR 0
#endif

// A .beam cannot be loaded by an Erlang runtime older than the one whose
// erlc produced it (OTP 28 changed the atom table encoding: an OTP 27 `erl`
// rejects OTP 29 output with "corrupt atom table", and the run dies as an
// anonymous `undef`). KEX_RUNTIME_OTP_FLOOR — baked in by CMakeLists next to
// the runtime-beam compilation — is the OTP that compiled the bundled
// runtime beams; 0 means unknown and disables the check. Prepended to every
// eval kex hands to `erl`, so it costs one comparison per VM boot and fires
// before any module load can fail opaquely.
auto beamOtpFloorGuard() -> std::string {
  if (KEX_RUNTIME_OTP_FLOOR == 0)
    return {};
  return "case catch list_to_integer(erlang:system_info(otp_release)) of "
         "N when is_integer(N), N >= " +
         std::to_string(KEX_RUNTIME_OTP_FLOOR) +
         " -> ok; "
         "_ -> E=case os:find_executable(os:getenv(\"KEX_ERL\",\"erl\")) of "
         "false -> os:getenv(\"KEX_ERL\",\"erl\"); P -> P end, "
         "io:format(standard_error, \"error: the Kex runtime beams bundled "
         "with this kex were compiled with Erlang/OTP " +
         std::to_string(KEX_RUNTIME_OTP_FLOOR) +
         ", and ~ts (OTP ~ts) is too old to load them — without this check "
         "the run would die as 'corrupt atom table' or 'undef'.~nUpgrade "
         "Erlang, or point the KEX_ERL environment variable at an OTP " +
         std::to_string(KEX_RUNTIME_OTP_FLOOR) + "+ erl.~n\", "
         "[E, erlang:system_info(otp_release)]), halt(1) end, ";
}
