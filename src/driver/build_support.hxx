#pragma once

// The build services the commands share: the REPLs (repl/session.cxx) use
// them as `kex -c` and `kex -R` do. Each is defined in the driver file its
// section names.

#include "dependencies.hxx"
#include "../ast/ast.hxx"
#include "../ir/lower.hxx"
#include "../semantic/analyzer.hxx"
#include "../semantic/imported_interfaces.hxx"
#include "../semantic/symbol_queries.hxx"
#include "../semantic/completion_at.hxx"
#include "../beam/kexi.hxx"
#include "../ir/emit_core.hxx"
#include "../compiled/expand.hxx"

#include <filesystem>
#include <functional>
#include <map>

#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

// The prelude's interfaces, from the prebuilt runtime.
auto preludeSemanticInterfaces() -> const kex::semantic::ImportedInterfaces &;
// What lowering knows of the prelude and its compiled units.
auto preludeExternalModules() -> const kex::ir::ExternalModules &;
auto loadPreludeRecordLayouts() -> std::vector<kex::ir::ExternalRecordLayout>;
auto loadPreludeVariantTags() -> std::vector<kex::ir::ExternalVariantTag>;
// The Core Erlang that registers the prelude's types for display.
auto preludeDisplayRegistration() -> std::string;
// The prelude's names, read once from its sources, for completion.
auto preludeCompletionSymbols() -> const std::vector<kex::semantic::SymbolInfo> &;

// Where `filepath`'s modules are found: its source roots, then the stdlib.
auto moduleRootsFor(const std::string &filepath) -> std::vector<std::string>;
// The modules `program` uses, loaded and merged in (kex::driver), for a REPL,
// a walker run or a check. A build does it unit by unit (build_ir.hxx).
auto resolveBeamDeps(kex::ast::Program &program, const std::vector<std::string> &roots,
                     const kex::semantic::ImportedInterfaces *interfaces,
                     const std::optional<std::unordered_set<std::string>> &qualifiedModules =
                         std::nullopt)
    -> std::vector<kex::driver::LoadedDep>;
// The modules a unit's lowering may call as receiver-function providers.
auto providerModules(const kex::ir::ExternalModules &externals) -> std::set<std::string>;

// A file's declarations, parsed; merged ahead of a program's own.
auto fileExists(const std::string &path) -> bool;
auto declarationsOf(const std::string &file) -> std::vector<kex::ast::TopLevelItem>;
auto prependDeclarationsOf(kex::ast::Program &program, const std::vector<std::string> &files)
    -> void;
// A REPL program's `compiled do` blocks expanded.
auto expandReplProgram(kex::ast::Program &program) -> void;

// The full check: names reported for `files`, tagged literals validated.
auto configureFullCheck(kex::semantic::Analyzer &analyzer, std::vector<std::string> files,
                        const std::string &entryPath) -> void;
auto taggedLiteralDiagnostics(const kex::ast::Program &program,
                              const kex::semantic::Analyzer &analyzer,
                              const std::string &entryPath)
    -> std::vector<kex::semantic::Diagnostic>;
// A buffer checked for completion, as the REPL checks an input it runs.
auto checkForCompletion(const std::string &path, const std::string &source)
    -> std::optional<kex::semantic::CheckedSource>;
// An expression's type as the REPL shows it.
auto displayTypeOf(const kex::semantic::Analyzer &analyzer, const kex::ast::Expr *expr)
    -> std::optional<std::string>;
auto printAst(const kex::ast::Program &program) -> void;

// The Erlang toolchain: `erl` and `erlc` (KEX_ERL / PATH), and the guard that
// refuses an OTP older than the runtime was compiled for.
auto erlExecutable() -> std::string;
auto erlcExecutable() -> std::string;
auto beamOtpFloorGuard() -> std::string;

// `--sname` / `--name` / `--cookie`, as erl's own flags, for every VM kex starts.
extern std::vector<std::string> cliDistributionArgs;

// An emitted module's routed implementations, as recordImplementations asks.
auto routedIn(const std::set<std::pair<std::string, int>> &routed)
    -> std::function<bool(const std::string &, int)>;

// Whether a source may hold `compiled do` code or a `Kex.embed`: such a file
// reads other files while it compiles, so a run cache cannot key it by its
// bytes alone.
auto sourceMentionsCompiledBlock(const std::string &src) -> bool;
auto sourceMentionsEmbed(const std::string &src) -> bool;

// The compiled-module cache (driver/toolchain.cxx): a module's .beam by its
// Core Erlang and the erlc that compiles it.
auto beamCacheRoot() -> std::optional<std::filesystem::path>;
auto erlcIdentity() -> std::string;
auto beamCacheKey(const std::string &coreSource, const std::string &erlc,
                  const std::string &flags) -> std::string;
auto storeCachedBeam(const std::filesystem::path &entry, const std::filesystem::path &beam)
    -> void;

// The run cache (driver/toolchain.cxx): a `-c --run`'s modules, by the whole
// closure's content, or by the entry and flags with every file the build read.
auto runCacheKeyFor(const std::string &filepath, bool skipCheck,
                    const std::string &entryRawSource, const std::string &specBaseFile,
                    const std::string &specBaseRawSource,
                    const std::vector<kex::driver::LoadedDep> &deps)
    -> std::optional<std::string>;
auto tryLoadRunCache(const std::string &key, const std::string &outputDir)
    -> std::optional<std::vector<kex::ir::EmitResult>>;
auto storeRunCache(const std::string &key, const std::string &outputDir,
                   const std::vector<kex::ir::EmitResult> &results) -> void;
auto runManifestKey(const std::string &flags, const std::string &filepath)
    -> std::optional<std::string>;
auto runManifestDir(const std::string &key) -> std::filesystem::path;
auto runManifestCurrent(const std::string &key) -> bool;
auto runManifestFiles(const std::string &filepath, const std::string &specBase,
                      const std::vector<kex::driver::LoadedDep> &deps) -> std::set<std::string>;
// The process's own recorder, for the whole of a build that reuses collapses.
auto tryLoadRunCacheFrom(const std::filesystem::path &dir, const std::string &outputDir)
    -> std::optional<std::vector<kex::ir::EmitResult>>;
auto storeRunManifest(const std::string &key, const std::string &outputDir,
                      const std::vector<kex::ir::EmitResult> &results,
                      const std::set<std::string> &files) -> void;
extern std::optional<kex::compiled::DependencyRecorder> buildRecorder;

// `--build-prelude DIR` (driver/prelude_build.cxx).
auto buildPrelude(const std::string &dir, bool verifyHir) -> int;
// The files a spec's or manifest's base could be (driver/program_support.cxx).
auto specBaseCandidates(const std::string &filepath) -> std::vector<std::string>;

// The CLI's own state and entry helpers (driver/program_support.cxx).
// `--source-root` directories, searched before the stdlib by moduleRootsFor.
extern std::vector<std::string> cliSourceRoots;
extern bool quietBuild;
auto distributionArgsForShell() -> std::string;
auto isSpecEntry(const std::string &filepath) -> bool;
// Merges the entry's companion (a spec's base, a manifest's vocabulary) into
// `program`; the file merged, or "". `rawSource` receives its text.
auto mergeSpecBase(kex::ast::Program &program, const std::string &entry,
                   std::string *rawSource = nullptr) -> std::string;
// The full check of a program, its diagnostics printed: whether it passed.
auto runSemanticCheck(const kex::ast::Program &program, const std::string &filepath,
                      kex::semantic::Analyzer *retainedAnalyzer = nullptr,
                      const std::string &extraDeclFile = "", int *errorCount = nullptr)
    -> bool;
