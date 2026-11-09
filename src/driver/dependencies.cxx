#include "dependencies.hxx"

#include "../ast/clone.hxx"
#include "../cache/cache.hxx"
#include "../common/color.hxx"
#include "../common/diagnostics.hxx"
#include "../common/digest.hxx"
#include "../common/file_io.hxx"
#include "../common/prelude_loader.hxx"
#include "../common/runtime_dir.hxx"
#include "../common/string_utils.hxx"
#include "../common/timings.hxx"
#include "../common/well_known.hxx"
#include "../lexer/lexer.hxx"
#include "../module/discovery.hxx"
#include "../module/resolver.hxx"
#include "../parser/parser.hxx"
#include "../semantic/analyzer.hxx"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace kex::driver {

using diagnostics::printSemanticDiagnostic;
using module::collectUsingModules;
using module::declaredModuleNames;
using module::fileDeclaringModule;
using module::moduleNameForPath;
using module::shownPath;


// Whether a dependency that cannot be loaded ends the process (a build) or
// is thrown as an error (a REPL, where `/load` and each input catch it). An
// interactive session used to die with the build's `exit(1)` the moment a
// `/load`-ed file used a module with a syntax error.
bool g_dependencyFailureThrows = false;

[[noreturn]] auto dependencyFailure(const std::string &what) -> void {
  if (g_dependencyFailureThrows) throw std::runtime_error(what);
  std::exit(1);
}

auto reportModuleFileError(const std::filesystem::path &file, int line,
                           const std::string &message) -> void {
  std::cerr << kex::color::apply(kex::color::gray) << shownPath(file) << ":"
            << line << ":1:" << kex::color::apply(kex::color::reset) << " "
            << kex::color::apply(kex::color::bold)
            << kex::color::apply(kex::color::red) << "error:"
            << kex::color::apply(kex::color::reset) << " " << message << "\n";
}

// ── Dependency-discovery cache (kexhq/kex#323) ──────────────────────────────
//
// Which modules a dependency file refers to is found by analyzing that file on
// its own, against the toolchain's prelude and stdlib interfaces; nothing else
// feeds the answer. So it is a pure function of the file's bytes and the
// toolchain, and a warm build reads it back instead of re-analyzing every
// dependency — 0.4 s of a 2.3 s warm build of Tey. The toolchain part covers
// the stdlib sources as well, which a checkout can change without a new
// revision.
//
// The toolchain part (kex::cache::toolchainFingerprint) covers the compiler
// binary itself as well (kexhq/kex#372).

// Where an expansion keeps template scans between builds, and for which
// toolchain (kexhq/kex#323); left empty when every cache is off.
auto setScanCache(kex::compiled::ExpandOptions &options) -> void {
  if (const auto base = kex::cache::base()) {
    options.scanCache = *base;
    options.toolchainKey = kex::cache::toolchainFingerprint();
  }
}

// The same for every expansion that builds its own options: a module the
// semantic pass or a compile-time sandbox expands.
auto setDefaultScanCache() -> void {
  if (const auto base = kex::cache::base())
    kex::compiled::setDefaultScanCache(*base, kex::cache::toolchainFingerprint());
}

namespace {

auto discoveryCacheEntry(const std::string &source)
    -> std::optional<std::filesystem::path> {
  const auto base = kex::cache::base();
  if (!base) return std::nullopt;
  const std::string material =
      "kex-deps-cache-v1\n" + kex::cache::toolchainFingerprint() + "\n" + source;
  const auto key = kex::sha256Hex(material);
  return *base / "deps" / key.substr(0, 2) / (key + ".txt");
}

// A cached module list, or nullopt when there is no complete entry. The first
// line marks a finished write, so no partial file ever reads as "refers to
// nothing".
auto readDiscoveryCache(const std::filesystem::path &entry)
    -> std::optional<std::vector<std::string>> {
  std::ifstream input(entry);
  if (!input) return std::nullopt;
  std::string line;
  if (!std::getline(input, line) || line != "kex-deps") return std::nullopt;
  std::vector<std::string> modules;
  while (std::getline(input, line))
    if (!line.empty()) modules.push_back(line);
  return modules;
}

// Written beside the final name and renamed into place, so two builds sharing
// the cache never see half an entry; a failed write only means the next build
// analyzes that file again.
auto writeDiscoveryCache(const std::filesystem::path &entry,
                         const std::vector<std::string> &modules) -> void {
  std::string text = "kex-deps\n";
  for (const auto &module : modules) text += module + "\n";
  kex::cache::atomicWrite(entry, text);
}

} // namespace

// ── Build progress (kexhq/kex#586) ───────────────────────────────────────────
//
// What a long build is doing, as it does it: Slick's `tey build` printed
// nothing for 17 minutes, and KEX_TIMINGS could only say afterwards which
// phase had taken them. One stderr line per step, written BEFORE the step
// starts, so the last line on screen is what is running now: a stuck build
// names the file it is stuck on. Lines, not a status line redrawn in place —
// Tey reads this through a pipe, where `\r` redraws nothing.
//
//   [  0.0s]    Resolving main.kex
//   [  1.5s]    Expanding app/live/conversations.kex
//   [ 74.9s]     Checking 160 modules
//   [ 96.6s]     Lowering 160 modules
//   [101.0s]    Compiling Kex.App.Live.Conversations
//   [154.6s]     Finished ebin/kex_main.beam in 2m 34.6s
//
// On by default for `kex -c`; `--progress` turns it on elsewhere (a spec run,
// `kex file.kex`), and `--quiet` off, together with erlc's own lines.
auto BuildProgress::elapsed() const -> double {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

auto BuildProgress::step(const char *verb, const char *colour, const std::string &what) const
    -> void {
  if (!enabled) return;
  namespace c = kex::color;
  std::fprintf(stderr, "%s[%6.1fs]%s %s%s%12s%s %s\n", c::apply(c::gray),
               elapsed(), c::apply(c::reset), c::apply(c::bold),
               c::apply(colour), verb, c::apply(c::reset), what.c_str());
  std::fflush(stderr);
}

// A standard library file is named from the library, not by the `../../..`
// climb from the working directory to wherever the toolchain is installed.
auto BuildProgress::expanding(const std::string &path) -> void {
  if (!enabled || !expanded.insert(path).second) return;
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto absolute = fs::weakly_canonical(path, ec);
  for (const auto &root : kex::standardLibraryModuleRoots()) {
    const auto relative =
        fs::weakly_canonical(root, ec).lexically_normal();
    const auto inside = absolute.lexically_relative(relative);
    if (!ec && !inside.empty() && *inside.begin() != "..") {
      step("Expanding", kex::color::magenta, "stdlib/" + inside.string());
      return;
    }
  }
  step("Expanding", kex::color::magenta, path);
}

auto BuildProgress::finished(const std::string &what) const -> void {
  if (!enabled) return;
  const auto total = elapsed();
  char took[32];
  const int minutes = static_cast<int>(total) / 60;
  if (minutes > 0)
    std::snprintf(took, sizeof took, "%dm %.1fs", minutes,
                  total - 60.0 * minutes);
  else
    std::snprintf(took, sizeof took, "%.1fs", total);
  namespace c = kex::color;
  step("Finished", kex::color::green,
       what + " " + c::apply(c::gray) + "in " + took + c::apply(c::reset));
}

BuildProgress buildProgress;

auto moduleCount(std::size_t count, const char *kind) -> std::string {
  return std::to_string(count) + " " + kind + (count == 1 ? "module" : "modules");
}

auto unitsOf(const std::map<std::string, std::set<std::string>> &uses)
    -> std::vector<std::vector<std::string>> {
  // Tarjan's algorithm: a component is complete only after every component
  // it reaches, which is the order a build needs.
  std::vector<std::vector<std::string>> units;
  std::map<std::string, int> index, low;
  std::set<std::string> onStack;
  std::vector<std::string> stack;
  int next = 0;
  std::function<void(const std::string &)> visit = [&](const std::string &file) {
    index[file] = low[file] = next++;
    stack.push_back(file);
    onStack.insert(file);
    if (const auto found = uses.find(file); found != uses.end())
      for (const auto &used : found->second) {
        if (!index.contains(used)) {
          visit(used);
          low[file] = std::min(low[file], low[used]);
        } else if (onStack.contains(used)) {
          low[file] = std::min(low[file], index[used]);
        }
      }
    if (low[file] != index[file]) return;
    std::vector<std::string> unit;
    std::string member;
    do {
      member = stack.back();
      stack.pop_back();
      onStack.erase(member);
      unit.push_back(member);
    } while (member != file);
    std::sort(unit.begin(), unit.end());
    units.push_back(std::move(unit));
  };
  // From the entry: every file is reached from it, and it is the last unit.
  visit("");
  for (const auto &[file, _] : uses)
    if (!index.contains(file)) visit(file);
  return units;
}

// Every dependency file this build has parsed and expanded, by path. The
// gating pre-check and the build's own dependency load read the same files,
// and expanding each twice cost Slick's no-op build a second pass over all
// of them. Only for one build (`kex -c` / `--run` of a file, set in main):
// within it the sources stay as they are, while across REPL inputs a file's
// compile-time reads may change under the same bytes. Handed out as clones,
// keyed by the bytes and roots they were expanded from.
struct ExpandedDependency {
  std::string source;
  std::vector<std::string> roots; // effective (kex::compiled::effectiveModuleRoots)
  kex::ast::Program program;
};
bool g_memoizeDependencyExpansion = false;
static std::unordered_map<std::string, ExpandedDependency> g_expandedDependencies;

// Whether the prelude imports `module` into every program: its interface is
// already known, so its source is never merged in.
auto isAutomaticImport(const kex::semantic::ImportedInterfaces *interfaces,
                       const std::string &module) -> bool {
  if (!interfaces) return false;
  const auto imported = interfaces->modules.find(module);
  return imported != interfaces->modules.end() &&
         imported->second.automaticImport;
}

// The modules `program` names — through `using` or a qualified reference —
// whose source has to be merged in: all but the automatic imports.
auto sourceModulesNamedBy(const kex::ast::Program &program)
    -> std::unordered_set<std::string> {
  kex::semantic::Analyzer analysis(&kex::defaultPreludeInterfaces());
  (void)analysis.analyze(program);
  auto modules = analysis.referencedModules();
  std::erase_if(modules, [](const std::string &module) {
    return isAutomaticImport(&kex::defaultPreludeInterfaces(), module);
  });
  return modules;
}

auto prependItems(kex::ast::Program &program,
                  std::vector<kex::ast::TopLevelItem> items) -> void {
  items.reserve(items.size() + program.items.size());
  for (auto &item : program.items) items.push_back(std::move(item));
  program.items = std::move(items);
}

auto loadDependencies(const kex::ast::Program &program,
                      const std::vector<std::string> &roots,
                      const kex::semantic::ImportedInterfaces *interfaces,
                      const std::optional<std::unordered_set<std::string>>
                          &qualifiedModules) -> Dependencies {
  std::map<std::string, std::set<std::string>> uses{{"", {}}};
  std::vector<LoadedDep> deps;
  std::unordered_set<std::string> loaded;
  kex::module::Resolver resolver(roots);
  // KEX_TIMINGS=1 splits dependency loading into its two costs (kexhq/kex#323).
  std::chrono::steady_clock::duration parseTime{};
  std::chrono::steady_clock::duration analyzeTime{};
  std::chrono::steady_clock::duration expandTime{};
  const auto sandboxesBefore = kex::compiled::sandboxTimings();

  size_t discoveryHits = 0;
  // Names a `using` or a qualified reference asked for that no file under the
  // source roots answers to. Most are declared inline or come from an
  // interface; the rest are checked once everything is loaded.
  std::set<std::string> unresolved;

  // `source` is a dependency file's own text, which keys the discovery cache;
  // the entry program passes nullptr, since its AST is already expanded and
  // may carry a merged `.spec.kex` base, so its bytes are not the whole input.
  std::function<void(const kex::ast::Program &,
                     const std::optional<std::unordered_set<std::string>> &,
                     const std::string *, const std::string &)> resolve =
      [&](const kex::ast::Program &prog,
          const std::optional<std::unordered_set<std::string>> &seedModules,
          const std::string *source, const std::string &from) {
    auto modules = collectUsingModules(prog);
    if (seedModules)
      modules.insert(modules.end(), seedModules->begin(), seedModules->end());

    // Each newly parsed source file can introduce qualified-only dependency
    // edges of its own. Analyze it here rather than only analyzing the entry
    // program; otherwise discovery stops after one qualified-reference hop.
    // Sorted: `referencedModules` is a hash set, and this list decides what
    // gets compiled and in what order. Iterating it directly made the build
    // depend on the host's hash order (kexhq/kex#143).
    const auto analyzeStart = std::chrono::steady_clock::now();
    const auto cacheEntry =
        source ? discoveryCacheEntry(*source) : std::nullopt;
    std::vector<std::string> referencedOrdered;
    if (seedModules) {
      // The caller analyzed this exact program with these exact
      // interfaces already: the seed IS the discovery result. Sorting
      // keeps `referencedOrdered` identical to what the analyze below
      // would have produced (it sorts too), so resolution order — and
      // hence merge order — is unchanged while the re-analyze is gone.
      referencedOrdered.assign(seedModules->begin(), seedModules->end());
      std::sort(referencedOrdered.begin(), referencedOrdered.end());
    } else if (auto cached =
                   cacheEntry ? readDiscoveryCache(*cacheEntry)
                              : std::nullopt) {
      referencedOrdered = std::move(*cached);
      ++discoveryHits;
    } else {
      kex::semantic::Analyzer dependencyAnalysis(interfaces);
      (void)dependencyAnalysis.analyze(prog);
      const auto &referenced = dependencyAnalysis.referencedModules();
      referencedOrdered.assign(referenced.begin(), referenced.end());
      std::sort(referencedOrdered.begin(), referencedOrdered.end());
      if (cacheEntry) writeDiscoveryCache(*cacheEntry, referencedOrdered);
    }
    analyzeTime += std::chrono::steady_clock::now() - analyzeStart;
    for (const auto &name : referencedOrdered)
      if (!isAutomaticImport(interfaces, name)) modules.push_back(name);
    for (const auto &modName : modules) {
      auto resolved = resolver.resolve(modName);
      if (!resolved) {
        unresolved.insert(modName);
        continue;
      }
      // Before the already-loaded check: a file uses what it uses, whoever
      // loaded it first.
      if (resolved->path != from) uses[from].insert(resolved->path);
      uses.try_emplace(resolved->path);
      if (!loaded.insert(resolved->path).second) continue;

      auto path = std::make_unique<std::string>(resolved->path);
      const auto parseStart = std::chrono::steady_clock::now();
      auto src = std::make_unique<std::string>(readFile(*path));
      std::unique_ptr<kex::ast::Program> depProg;
      if (g_memoizeDependencyExpansion)
        if (const auto memo = g_expandedDependencies.find(*path);
            memo != g_expandedDependencies.end() &&
            memo->second.source == *src &&
            memo->second.roots == kex::compiled::effectiveModuleRoots(roots))
          if (auto copy = kex::ast::clone(memo->second.program))
            depProg = std::make_unique<kex::ast::Program>(std::move(*copy));
      const bool memoized = depProg != nullptr;
      std::optional<kex::Parser> parser;
      if (!memoized) {
        kex::Lexer lexer(std::string(*src), *path);
        parser.emplace(lexer.tokenizeAll(), *path);
        depProg = std::make_unique<kex::ast::Program>(parser->parseProgram());
      }
      parseTime += std::chrono::steady_clock::now() - parseStart;
      // A dependency that does not parse is a build failure, not a module to
      // skip. Silently dropping it produced no error anywhere: the module
      // simply did not exist, the program compiled, and the first call into
      // it died at RUNTIME with `Undefined function: Tey.Resolver`.
      if (parser && !parser->diagnostics().empty()) {
        for (const auto &diagnostic : parser->diagnostics())
          std::cerr << kex::color::apply(kex::color::gray)
                    << diagnostic.location.file << ":"
                    << diagnostic.location.line << ":"
                    << diagnostic.location.column << ":"
                    << kex::color::apply(kex::color::reset) << " "
                    << kex::color::apply(kex::color::bold)
                    << kex::color::apply(kex::color::red)
                    << "error:" << kex::color::apply(kex::color::reset) << " "
                    << diagnostic.message << "\n";
        std::cerr << "error: could not parse module " << modName << "\n";
        dependencyFailure("could not parse module " + modName);
      }
      // Expanded HERE, against THIS module's own path, while it is still its
      // own separate AST — not later, once `resolveBeamDeps` has merged its
      // items into the entry program's `items` alongside every other
      // dependency's. `expandParsedProgram` in main() runs exactly once, on
      // the entry file, with `sourcePath` fixed to the entry file's own
      // path — so a `Kex.embed(...)` or builder-chain collapse candidate
      // that a merged-in dependency module owns was resolving relative to
      // the ENTRY file's directory, or (for a chain requiring `compiled`
      // expansion) not being visited by that pass at all, since dependency
      // items are parsed after it already ran. Real per-declaration source
      // provenance would let one pass over the fully-merged program handle
      // this uniformly; short of that, each dependency gets its own
      // self-contained expansion, against its own path, before merging
      // (kexhq/kex#385).
      std::vector<kex::semantic::Diagnostic> depExpandDiagnostics;
      kex::compiled::ExpandOptions depExpandOptions;
      depExpandOptions.sourcePath = *path;
      depExpandOptions.moduleRoots = roots;
      depExpandOptions.interfaces = interfaces;
      setScanCache(depExpandOptions);
      if (!memoized) buildProgress.expanding(shownPath(*path));
      const auto expandStart = std::chrono::steady_clock::now();
      const bool expanded =
          memoized || kex::compiled::expand(*depProg, depExpandDiagnostics,
                                            depExpandOptions);
      const auto expandSpent = std::chrono::steady_clock::now() - expandStart;
      expandTime += expandSpent;
      // The total alone says expansion is slow, not where: Slick spent 930s
      // here across 146 files with nothing naming any of them (kexhq/kex#586).
      if (kex::timings::enabled() &&
          expandSpent > std::chrono::milliseconds(250))
        std::fprintf(stderr, "      expand %9.1f ms  %s\n",
                     std::chrono::duration<double, std::milli>(expandSpent).count(),
                     shownPath(*path).c_str());
      if (!expanded) {
        for (const auto &diagnostic : depExpandDiagnostics)
          printSemanticDiagnostic(diagnostic);
        std::cerr << "error: could not expand module " << modName << "\n";
        dependencyFailure("could not expand module " + modName);
      }
      // Found by its path, but declaring another module: `using
      // App.Services.Authority` reaching app/services/authority.kex whose
      // header says `module App.Authority`. The walker refuses that file;
      // the build compiled it and the first call failed at run time. A name
      // found through its enclosing module's file is not checked: the type
      // `DB.Store.Store` lives in db/store.kex, whose stem only happens to
      // match.
      const auto lastPart =
          resolved->moduleName.substr(resolved->moduleName.rfind('.') + 1);
      const auto stem = std::filesystem::path(*path).stem().string();
      const auto lowered = kex::lowered(lastPart);
      const bool ownFile = !resolved->enclosing &&
                           (stem == snakeCased(lastPart) || stem == lowered);
      const kex::ast::ModuleDef *header = nullptr;
      for (const auto &item : depProg->items)
        if (auto *module = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
            module && *module) {
          header = module->get();
          break;
        }
      if (header && ownFile &&
          !declaredModuleNames(*depProg).contains(resolved->moduleName)) {
        reportModuleFileError(
            *path, header->location.line,
            "this file is `" + resolved->moduleName + "` by its path, but "
            "declares `" + header->name + "`; name it `module " +
            resolved->moduleName + "`");
        dependencyFailure("module file " + shownPath(*path) + " declares another module");
      }
      if (g_memoizeDependencyExpansion && !memoized)
        if (auto copy = kex::ast::clone(*depProg))
          g_expandedDependencies[*path] = {
              *src, kex::compiled::effectiveModuleRoots(roots), std::move(*copy)};
      resolve(*depProg, std::nullopt, src.get(), *path);
      deps.push_back({std::move(src), std::move(path), std::move(depProg)});
    }
  };
  resolve(program, qualifiedModules, nullptr, std::string{});
  // Files declaring the same module (`module Data` atop both data/set.kex
  // and data/stack.kex) build that one module together: one unit, as if
  // each used the next.
  std::map<std::string, std::vector<std::string>> declaringFiles;
  for (const auto &dep : deps)
    for (const auto &item : dep.program->items)
      if (const auto *module = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
          module && *module)
        declaringFiles[(*module)->name].push_back(*dep.path);
  for (auto &[_, paths] : declaringFiles) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (std::size_t i = 0; paths.size() > 1 && i < paths.size(); i++)
      uses[paths[i]].insert(paths[(i + 1) % paths.size()]);
  }
  const auto units = unitsOf(uses);
  Dependencies loadedDependencies;
  loadedDependencies.files = std::move(deps);
  loadedDependencies.uses = std::move(uses);
  loadedDependencies.units = units;
  loadedDependencies.unresolved = std::move(unresolved);
  loadedDependencies.loaded = std::move(loaded);
  loadedDependencies.parseTime = parseTime;
  loadedDependencies.expandTime = expandTime;
  loadedDependencies.analyzeTime = analyzeTime;
  loadedDependencies.discoveryHits = discoveryHits;
  loadedDependencies.sandboxesBefore = sandboxesBefore;
  return loadedDependencies;
}

auto mergeDependencies(kex::ast::Program &program, Dependencies &dependencies,
                       const std::vector<std::string> &roots,
                       const kex::semantic::ImportedInterfaces *interfaces) -> void {
  auto &deps = dependencies.files;
  const auto &unresolved = dependencies.unresolved;
  const auto &loaded = dependencies.loaded;
  const auto &units = dependencies.units;
  const auto &parseTime = dependencies.parseTime;
  const auto &expandTime = dependencies.expandTime;
  const auto &analyzeTime = dependencies.analyzeTime;
  const auto discoveryHits = dependencies.discoveryHits;
  const auto &sandboxesBefore = dependencies.sandboxesBefore;
  if (!deps.empty()) {
    std::vector<kex::ast::TopLevelItem> merged;
    for (auto &dep : deps)
      for (auto &item : dep.program->items)
        if (kex::ast::isMergedDeclaration(item))
          merged.push_back(std::move(item));
    prependItems(program, std::move(merged));
  }
  // A module whose file sits where another name belongs is reached by
  // neither: `module App.Authority` in app/services/authority.kex. The build
  // used to succeed without it and the first call died at run time with
  // "Unknown module" or "Undefined function".
  if (!unresolved.empty()) {
    const auto declared = declaredModuleNames(program);
    for (const auto &name : unresolved) {
      if (declared.contains(name)) continue;
      if (interfaces && (interfaces->modules.contains(name) ||
                         interfaces->modules.contains(kex::names::moduleAtom(name))))
        continue;
      const auto found = fileDeclaringModule(name, roots);
      if (!found) continue;
      if (loaded.contains(found->file.string())) continue;
      const auto byPath =
          moduleNameForPath(found->file.lexically_relative(found->root));
      auto expected = found->root;
      std::stringstream parts(name);
      for (std::string part; std::getline(parts, part, '.');)
        expected /= snakeCased(part);
      expected += ".kex";
      reportModuleFileError(
          found->file, found->line,
          "module `" + name + "` is declared in a file whose path names `" +
              byPath + "`, so `" + name + "` is not found; name it `module " +
              byPath + "`, or move it to " + shownPath(expected));
      dependencyFailure("module `" + name + "` is declared in a file named for another module");
    }
  }
  if (kex::timings::enabled()) {
    const auto ms = kex::timings::milliseconds;
    std::fprintf(stderr,
                 "    %zu dependency files: parse %.1f ms, expand %.1f ms, "
                 "analyze %.1f ms (%zu from the discovery cache)\n",
                 deps.size(), ms(parseTime), ms(expandTime), ms(analyzeTime),
                 discoveryHits);
    const auto &sandboxes = kex::compiled::sandboxTimings();
    if (sandboxes.started > sandboxesBefore.started)
      std::fprintf(stderr,
                   "    %zu compile-time sandboxes: start-up %.1f ms, evaluation %.1f ms\n",
                   sandboxes.started - sandboxesBefore.started,
                   ms(sandboxes.startup - sandboxesBefore.startup),
                   ms(sandboxes.evaluation - sandboxesBefore.evaluation));
    std::size_t largest = 0, inCycles = 0;
    for (const auto &unit : units) {
      largest = std::max(largest, unit.size());
      if (unit.size() > 1) inCycles += unit.size();
    }
    std::fprintf(stderr, "    %zu units: largest %zu files, %zu files in cycles\n",
                 units.size(), largest, inCycles);
  }
}

auto prepareForWalker(kex::ast::Program &program, const std::vector<std::string> &roots,
                      const kex::semantic::ImportedInterfaces *interfaces,
                      const std::string &sourcePath)
    -> std::unique_ptr<hir::CheckedProgram> {
  // A harness or an embedder catches a module that cannot be loaded.
  struct Throwing {
    bool previous = g_dependencyFailureThrows;
    Throwing() { g_dependencyFailureThrows = true; }
    ~Throwing() { g_dependencyFailureThrows = previous; }
  } throwing;
  auto dependencies = loadDependencies(program, roots, interfaces);
  mergeDependencies(program, dependencies, roots, interfaces);
  std::vector<kex::semantic::Diagnostic> diagnostics;
  kex::compiled::ExpandOptions options;
  options.sourcePath = sourcePath;
  options.moduleRoots = roots;
  options.interfaces = interfaces;
  setScanCache(options);
  if (!kex::compiled::expand(program, diagnostics, options))
    throw std::runtime_error(diagnostics.empty() ? "compile-time expansion failed"
                                                 : diagnostics.front().message);
  return hir::checkSilently(program, interfaces);
}

} // namespace kex::driver
