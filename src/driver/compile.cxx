// `kex -c`, `--emit-core` and `-c --run`: the driver around the build's pass
// pipeline (build_ir.hxx). It chooses the output, copies the runtime, lets
// the run caches answer, writes `.core`, runs erlc, attaches KexI and runs the
// program; the program pipeline does the rest.
#include "../beam/beam_file.hxx"
#include "../beam/kexi.hxx"
#include "../common/artifact_versions.hxx"
#include "../common/color.hxx"
#include "../ir/emit_core.hxx"
#include "build_ir.hxx"
#include "../ir/module_passes.hxx"
#include "../pass/options.hxx"
#include "../compiled/expand.hxx"
#include "../module/discovery.hxx"
#include "../semantic/analyzer.hxx"
#include "../semantic/db.hxx"
#include "../common/runtime_dir.hxx"
#include "dependencies.hxx"
#include "build_support.hxx"
#include "compile.hxx"
#include "options.hxx"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif
#include "../common/diagnostics.hxx"
#include "../common/process.hxx"
#include "../common/well_known.hxx"
#include <functional>
#include <thread>

using kex::prebuiltRuntimeBeamDir;
using kex::driver::buildProgress;
using kex::driver::LoadedDep;
using kex::driver::moduleCount;
using kex::process::beamTestGate;
using kex::process::exitStatusOf;
using kex::process::runShellCommand;
using kex::process::runShellCommandsTogether;
using kex::process::shellSingleQuote;
using kex::diagnostics::errorCountPhrase;
using kex::diagnostics::printSemanticDiagnostic;
using kex::module::shownPath;

// Attaches each emitted module's KexI chunk to its `.beam` in `outputDir`.
// Companions go first, so the entry's chunk is written once, already naming
// every companion's interface hash — the entry used to be written, then read
// back with every companion and written again to fill those hashes in.
// Each module's chunk is the one its own unit described (`chunks`), stamped
// here with the build's layout: the first module the entry, the rest its
// companions.
auto attachInterfaces(const std::vector<kex::ir::EmitResult> &modules,
                      const std::string &outputDir,
                      const std::map<std::string, kex::beam::KexiChunk> &chunks) -> void {
  std::map<std::string, kex::beam::Hash128> companionHashes;
  auto attach = [&](size_t index) {
    const auto &module = modules[index];
    const std::string beamPath = outputDir + "/" + module.moduleName + ".beam";
    try {
      const auto described = chunks.find(module.moduleName);
      if (described == chunks.end())
        throw std::runtime_error("no unit described " + module.moduleName);
      auto chunk = described->second;
      chunk.metadata.unitId = modules[0].moduleName;
      chunk.metadata.moduleAtom = module.moduleName;
      chunk.metadata.role = index == 0 ? kex::beam::KexiModuleRole::Entry
                                       : kex::beam::KexiModuleRole::Companion;
      chunk.metadata.entryBackPointer = index == 0 ? "" : modules[0].moduleName;
      chunk.metadata.companions.clear();
      chunk.intrinsicAbiVersion = kex::kIntrinsicAbiVersion;
      chunk.backendRepresentationVersion = kex::kBeamRepresentationVersion;
      if (index == 0)
        for (size_t c = 1; c < modules.size(); c++) {
          kex::beam::KexiCompanion companion;
          companion.beamAtom = modules[c].moduleName;
          companion.relativePath = modules[c].moduleName + ".beam";
          if (const auto hash = companionHashes.find(companion.beamAtom);
              hash != companionHashes.end())
            companion.expectedHash = hash->second;
          chunk.metadata.companions.push_back(std::move(companion));
        }
      chunk.interfaceHash = kex::beam::computeInterfaceHash(chunk);
      if (index != 0) companionHashes[module.moduleName] = chunk.interfaceHash;
      auto beam = kex::beam::readBeamFile(beamPath);
      chunk.artifactHash = kex::beam::computeArtifactHash(beam);
      beam.setChunk(kex::beam::KEXI_CHUNK_ID, kex::beam::serializeKexi(chunk));
      kex::beam::writeBeamFile(beam, beamPath);
    } catch (const std::exception &e) {
      std::cerr << "warning: could not attach KexI chunk to " << beamPath
                << ": " << e.what() << "\n";
    }
  };
  for (size_t index = 1; index < modules.size(); index++) attach(index);
  if (!modules.empty()) attach(0);
}

// Runs an already compiled `.beam` entry module with the bundled runtime;
// `beamArgs` are the program's own arguments.
auto runCompiledBeam(const std::string &filepath, const Options &options,
                     const std::vector<std::string> &beamArgs) -> int {

  namespace fs = std::filesystem;
  std::string absBeamPath = fs::weakly_canonical(filepath).string();
  std::string absBeamDir = fs::path(absBeamPath).parent_path().string();
  std::string modFile = fs::path(absBeamPath).filename().string();

  // Derive module name from filename convention:
  //   <stem>.kx.beam → kex_<stem>   (our compiled output)
  //   kex_<x>.beam   → kex_<x>      (backward compat)
  //   anything.beam  → anything      (external module)
  std::string moduleName;
  if (modFile.size() > 8 &&
      modFile.compare(modFile.size() - 8, 8, ".kx.beam") == 0) {
    moduleName = "kex_" + modFile.substr(0, modFile.size() - 8);
  } else {
    moduleName = modFile.substr(0, modFile.size() - 5);
  }

  // Put explicitly built Kex runtime beams on the code path.
  std::string rtBeamDir = prebuiltRuntimeBeamDir();
  if (rtBeamDir.empty()) {
    std::cerr << "error: prebuilt runtime artifacts are missing; "
                 "rebuild or reinstall the Kex toolchain\n";
    return 1;
  }

  // Load explicitly — code:load_abs rejects when filename != module name,
  // so use code:load_binary which skips that check.
  // Force UTF-8 on the node's I/O instead of inheriting it from the locale.
  // An `erl` started under a non-UTF-8 LANG (the Linux CI runner) writes
  // `\x{2717}` for `✗` and mangles every non-ASCII byte, so a Kex program's
  // output depended on the environment: identical source printed `[h, é, é]`
  // here and `[h, \ufffd, \ufffd]` there.
  // A prebuilt .beam carries no evidence of being a spec — the file name it
  // was compiled from is gone — so mocks here are strictly opt-in
  // (issue #144).
  std::string mainCall =
      beamOtpFloorGuard() +
      "io:setopts(standard_io, [{encoding, unicode}]), "
      "io:setopts(standard_error, [{encoding, unicode}]), " +
      (options.allowMocks ? "kex_test:allow_mocks(), " : "") +
      beamTestGate(options.testReportMode, options.testFilters) +
      "try {ok,_Bin}=file:read_file(\"" + absBeamPath +
      "\"), "
      "code:load_binary('" +
      moduleName + "',\"" + absBeamPath +
      "\",_Bin), "
      "case lists:member({main,1},erlang:get_module_info('" +
      moduleName +
      "',exports)) of "
      "true -> '" +
      moduleName +
      "':main([unicode:characters_to_binary(A) || A <- "
      "init:get_plain_arguments()]); "
      "false -> '" +
      moduleName +
      "':main() end of "
      "Result -> halt() "
      "catch _:Reason:_ -> io:format(standard_error, \"Internal error: "
      "runtime error: ~p~n\", [Reason]), halt(1) end";
  // Erlang prepends each -pa directory, so add the toolchain first and the
  // compiled unit last. A source module intentionally shadows a stdlib
  // module with the same public name (for example a user-defined `Math`).
  // `+Bi` is the other half of forwardSignalToChild's SIGINT→SIGTERM
  // substitution, and neither works alone. A terminal's Ctrl+C signals the
  // whole process group, so the VM takes a SIGINT DIRECTLY as well as this
  // launcher's forward. Without `+Bi` that direct one drives the VM into its
  // break handler, where the forwarded SIGTERM no longer lands and nothing
  // exits at all; with `+Bi` the VM ignores SIGINT entirely and the SIGTERM
  // does the work — `erl_signal_server` runs, `kex_child_guard` kills the
  // port's OS child, and the run ends clean.
  //
  // Deliberately NOT applied to the REPL's persistent VM (the `BeamVm` block
  // above), where Ctrl+C should interrupt evaluation rather than end the
  // session.
  std::string runCmd =
      erlExecutable() + " +Bi -noshell" + distributionArgsForShell();
  if (!rtBeamDir.empty())
    runCmd += " -pa " + rtBeamDir;
  runCmd += " -pa " + absBeamDir;
  // shellSingleQuote (see its own comment) wraps the whole -eval
  // text as one shell argument, so quote characters embedded in it
  // (e.g. around a module name with a literal '.' in its stem)
  // survive into erl correctly regardless of where they land.
  runCmd += " -eval " + shellSingleQuote(mainCall);
  if (!beamArgs.empty()) {
    runCmd += " -extra";
    for (const auto &a : beamArgs)
      runCmd += " " + a;
  }
  int rc = runShellCommand(runCmd);
  return exitStatusOf(rc);
}

// Compiles each emitted module's Core Erlang (`corePaths`, in
// `moduleResults` order) to a `.beam` in `outputDir`. A `quiet` build — a
// run from a temporary directory — keeps erlc's output out of the program's
// own. False once erlc has failed and said why.
//
// ONE erlc for every module, not one per module. Each invocation
// starts a whole BEAM just to compile, ~90 ms before it reads a byte,
// so a spec file's three modules paid that three times over
// (kexhq/kex#318). erlc takes any number of files in a single run.
//
// -W0: these warnings analyze the generated Core Erlang — locationless
// (`no_file:`) compiler-analysis noise the emitter's dead statement-
// value lets make inevitable ("a term is constructed, but never used",
// "this clause cannot match"). They cannot point back at Kex source,
// and the interpreter path below has always silenced them for exactly
// that reason. Real erlc failures still fail the exit-code check and
// are re-run loudly below.
//
// Modules whose Core Erlang an earlier build already compiled with this
// erlc come straight from the cache (kexhq/kex#323); only the rest go
// to erlc, and those are stored for next time once it succeeds.
auto compileCoreModules(const std::vector<kex::ir::EmitResult> &moduleResults,
                        const std::vector<std::string> &corePaths,
                        const std::string &outputDir, bool quiet) -> bool {
  const auto cacheRoot = beamCacheRoot();
  const std::string erlcFlags = "+from_core -W0";
  const std::string erlc = cacheRoot ? erlcIdentity() : std::string{};
  std::vector<size_t> toCompile;
  std::vector<std::filesystem::path> cacheEntries(moduleResults.size());
  for (size_t moduleIndex = 0; moduleIndex < moduleResults.size(); ++moduleIndex) {
    if (cacheRoot) {
      const auto key =
          beamCacheKey(moduleResults[moduleIndex].source, erlc, erlcFlags);
      cacheEntries[moduleIndex] = *cacheRoot / key.substr(0, 2) / (key + ".beam");
      std::error_code ec;
      const auto target = std::filesystem::path{outputDir} /
                          (moduleResults[moduleIndex].moduleName + ".beam");
      if (std::filesystem::is_regular_file(cacheEntries[moduleIndex], ec) &&
          std::filesystem::copy_file(cacheEntries[moduleIndex], target,
                                     std::filesystem::copy_options::overwrite_existing,
                                     ec) &&
          !ec)
        continue;
    }
    toCompile.push_back(moduleIndex);
  }

  // erlc is one BEAM per run, and one run took every module in turn: Tey's
  // 82 modules spent 45s in a single process on a machine with cores to
  // spare. They go out in batches, one erlc each, side by side — at most
  // one per core (KEX_ERLC_JOBS to say otherwise), and at least four
  // modules apiece, since every run first starts a VM. Batches are
  // balanced by the size of their Core Erlang, the best guess at how long
  // each module takes. A batch reports when it ENDS, so what the build
  // shows is how far the compiling has actually got.
  std::size_t jobs = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
  if (const char *configured = std::getenv("KEX_ERLC_JOBS"))
    if (const auto requested = std::atoi(configured); requested > 0)
      jobs = static_cast<std::size_t>(requested);
  jobs = std::min(jobs, std::max<std::size_t>(1, toCompile.size() / 4));
  std::vector<std::vector<std::size_t>> batches(jobs);
  {
    auto bySize = toCompile;
    std::sort(bySize.begin(), bySize.end(), [&](std::size_t a, std::size_t b) {
      return moduleResults[a].source.size() > moduleResults[b].source.size();
    });
    std::vector<std::size_t> load(jobs, 0);
    for (const auto moduleIndex : bySize) {
      const auto lightest = static_cast<std::size_t>(
          std::min_element(load.begin(), load.end()) - load.begin());
      batches[lightest].push_back(moduleIndex);
      load[lightest] += moduleResults[moduleIndex].source.size();
    }
  }
  batches.erase(std::remove_if(batches.begin(), batches.end(),
                               [](const auto &batch) { return batch.empty(); }),
                batches.end());
  const auto erlcCommand = [&](const std::vector<std::size_t> &batch,
                               bool loud) {
    std::string command = erlcExecutable() + " " +
                          (loud ? std::string("+from_core") : erlcFlags) +
                          " -pa " + outputDir + " -o " + outputDir;
    for (const auto moduleIndex : batch) command += " " + corePaths[moduleIndex];
    // Suppress erlc noise in temp-dir (interpreter/-R) mode. erlc prints
    // its own warnings (e.g. "this clause cannot match because a
    // previous clause always matches" — harmless analysis of the
    // generated code) to its OWN STDOUT, not stderr, so both streams are
    // silenced to keep them out of the running program's own output.
    // Failures are still caught by the exit status, and re-run loudly.
    if (!loud && quiet) command += " > /dev/null 2>&1";
    return command;
  };
  std::vector<std::string> commands;
  for (const auto &batch : batches)
    commands.push_back(erlcCommand(batch, false));
  if (!quiet && buildProgress.enabled) {
    if (const auto cached = moduleResults.size() - toCompile.size(); cached > 0)
      buildProgress.step("Cached", kex::color::gray,
                         moduleCount(cached, "unchanged "));
    if (!toCompile.empty())
      buildProgress.step(
          "Compiling", kex::color::green,
          moduleCount(toCompile.size()) +
              (commands.size() > 1
                   ? std::string(kex::color::apply(kex::color::gray)) +
                         " in " + std::to_string(commands.size()) +
                         " erlc runs" + kex::color::apply(kex::color::reset)
                   : std::string{}));
  } else if (!quiet && !quietBuild) {
    for (const auto moduleIndex : toCompile)
      std::cerr << "  Compile: " << moduleResults[moduleIndex].moduleName << "\n";
  }
  std::vector<std::size_t> failed;
  std::size_t compiled = 0;
  runShellCommandsTogether(commands, [&](std::size_t index, int status) {
    const auto &batch = batches[index];
    if (status != 0) {
      failed.push_back(index);
      return;
    }
    if (cacheRoot)
      for (const auto moduleIndex : batch)
        storeCachedBeam(cacheEntries[moduleIndex],
                        std::filesystem::path{outputDir} /
                            (moduleResults[moduleIndex].moduleName + ".beam"));
    compiled += batch.size();
    if (!quiet && commands.size() > 1)
      buildProgress.step("Compiled", kex::color::green,
                         std::to_string(compiled) + "/" +
                             moduleCount(toCompile.size()));
  });
  if (!failed.empty()) {
    // The quiet mode above swallows erlc's own diagnostic — which is the
    // only thing that says WHY. Re-run the batches that failed loudly so
    // the message reaches the terminal (and CI) instead of a bare "erlc
    // failed", which is all a failing spec could report. erlc names the
    // file it choked on.
    if (quiet)
      for (const auto index : failed)
        runShellCommand(erlcCommand(batches[index], true));
    std::cerr << "error: erlc failed\n";
    return false;
  }
  return true;
}

// Runs the program `-c --run` just built into `outputDir`: loads every
// emitted module explicitly, then calls the entry's (the first module's)
// `main`, handing it `scriptArgs` when it takes them. Removes `tempDir`, the
// build's temporary output, when there is one. Returns the exit status.
auto runBuiltProgram(const Options &options, const std::string &filepath,
                     const std::vector<kex::ir::EmitResult> &moduleResults,
                     const std::string &tempDir,
                     const std::vector<std::string> &scriptArgs) -> int {
  const auto &outputDir = options.outputDir;
  const auto &result = moduleResults.front();
  // Load every freshly emitted module explicitly.  Relying on the code
  // path here lets BEAM autoload a same-named module from an earlier
  // compilation, which is particularly easy to hit for imported
  // standard-library modules such as Kex.Units.Data.
  namespace fs = std::filesystem;
  std::string loadExpr;
  for (size_t moduleIndex = 0; moduleIndex < moduleResults.size();
       ++moduleIndex) {
    const auto &emitted = moduleResults[moduleIndex];
    std::string beam = outputDir + "/" + emitted.moduleName + ".beam";
    std::string absBeam = fs::weakly_canonical(beam).string();
    std::string binaryVar = "_B" + std::to_string(moduleIndex);
    loadExpr += "{ok," + binaryVar + "}=file:read_file(\"" +
                absBeam + "\"), "
                "code:load_binary('" + emitted.moduleName + "',\"" +
                absBeam + "\"," + binaryVar + "), ";
  }
  // Kex runtime errors carry a String (binary/charlist) reason
  // printed verbatim; anything else (raw BEAM errors like
  // badarg tuples) falls back to ~p.
  std::string reasonFmt =
      "case Reason of _R when is_binary(_R); is_list(_R) -> "
      "io:format(standard_error, \"Internal error: ~ts~n\", [_R]); "
      "_R -> io:format(standard_error, \"Internal error: ~p~n\", [_R]) "
      "end";
  // Same UTF-8 pinning as the `.beam` runner above.
  const std::string utf8Io =
      "io:setopts(standard_io, [{encoding, unicode}]), "
      "io:setopts(standard_error, [{encoding, unicode}]), ";
  // Mock.* gate, BEAM side (issue #144). The grant is made by the
  // RUNNER, not baked into the emitted module: the same .beam handed to
  // `erl -pa ebin` — or to `kex prog.beam` — starts with mocks denied,
  // which is what makes a compiled program un-hijackable. main/0 runs in
  // this process, so the process-dictionary flag covers exactly it.
  const std::string mockGate =
      ((options.allowMocks || isSpecEntry(filepath)) ? "kex_test:allow_mocks(), "
                                             : "") +
      beamTestGate(options.testReportMode, options.testFilters);
  std::string mainCall =
      result.mainArity == 1
          ? beamOtpFloorGuard() + utf8Io + mockGate + "try " + loadExpr + "'" +
                result.moduleName +
                "':main([unicode:characters_to_binary(A) || A <- "
                "init:get_plain_arguments()]) of Result -> halt() catch "
                "_:Reason:_ -> " +
                reasonFmt + ", halt(1) end"
          : beamOtpFloorGuard() + utf8Io + mockGate + "try " + loadExpr + "'" +
                result.moduleName +
                "':main() of Result -> halt() catch _:Reason:_ -> " +
                reasonFmt + ", halt(1) end";
  // shellSingleQuote (see its own comment) wraps the whole
  // -eval text as one shell argument, so quote characters
  // embedded in it survive into erl correctly regardless of
  // where they land (spec/json_parser.spec.kex).
  std::string runCmd =
      // `+Bi` for the same reason as the other run path above: it is
      // half of the Ctrl+C fix, with forwardSignalToChild's
      // SIGINT→SIGTERM the other half.
      erlExecutable() + " +Bi -noshell" + distributionArgsForShell() +
      " -pa " + outputDir + " -eval " +
      shellSingleQuote(mainCall);
  if (result.mainArity == 1 && !scriptArgs.empty()) {
    runCmd += " -extra";
    for (const auto &a : scriptArgs)
      runCmd += " " + a;
  }
  int ret = runShellCommand(runCmd);
  if (!tempDir.empty())
    std::filesystem::remove_all(tempDir);
  return exitStatusOf(ret);
}

// Copies the toolchain's prebuilt runtime `.beam` files into `outputDir`,
// where every compiled program loads them from. False, having said why, when
// they are missing — or the prelude is, and `needsPrelude`.
auto copyRuntimeBeams(const std::string &outputDir, bool needsPrelude)
    -> bool {
  namespace fs = std::filesystem;
  const std::string prebuilt = prebuiltRuntimeBeamDir();
  if (prebuilt.empty()) {
    std::cerr << "error: prebuilt runtime artifacts are missing; "
                 "rebuild or reinstall the Kex toolchain\n";
    return false;
  }
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(prebuilt))
    if (entry.path().extension() == ".beam")
      fs::copy_file(entry.path(), fs::path{outputDir} / entry.path().filename(),
                    fs::copy_options::overwrite_existing, ec);
  if (needsPrelude &&
      !fs::exists(fs::path{outputDir} / (kex::names::kPreludeModule + ".beam"))) {
    std::cerr << "error: prebuilt standard library is missing; "
                 "rebuild or reinstall the Kex toolchain\n";
    return false;
  }
  return true;
}

// "Aborted: N type errors — fix before running/compiling".
auto reportAborted(bool compileRun, int errors) -> void {
  std::cerr << kex::color::apply(kex::color::bold)
            << kex::color::apply(kex::color::magenta)
            << "Aborted:" << kex::color::apply(kex::color::reset) << " "
            << errorCountPhrase(errors, "type") << " — fix before "
            << (compileRun ? "running" : "compiling")
            << " (use --no-check to skip).\n";
}

// The temporary output directory of a run without `-o`, removed when the
// build is abandoned.
struct OutputDirectory {
  std::string temporary;
  auto abandon() const -> int {
    if (!temporary.empty()) std::filesystem::remove_all(temporary);
    return 1;
  }
};

// The build of one entry: the program pipeline (build_ir.hxx), or a cache
// that answers for it. The modules it emitted, or nullopt when it stopped,
// having said why.
auto buildModules(Options &options, const std::string &filepath, kex::ast::Program &program,
                  const std::string &entryRawSourceForCache,
                  std::optional<std::string> &runCacheKey,
                  std::map<std::string, kex::beam::KexiChunk> &chunks,
                  std::vector<LoadedDep> &dependencies, std::string &specBaseFile,
                  bool &cached) -> std::optional<std::vector<kex::ir::EmitResult>> {
  const bool compileRun = options.compileRun;
  const bool gates = options.mode == Mode::Compile && !options.skipCheck;
  kex::build::Program build(program, {filepath, moduleRootsFor(filepath),
                                      &preludeSemanticInterfaces(), gates});
  std::vector<kex::ir::EmitResult> fromRunCache;
  // `-c --run`: a run cache keyed by the whole closure's content answers for
  // the build once its files are loaded.
  build.answerFromRunCache = [&](kex::build::Program &loaded) {
    if (options.mode != Mode::Compile || !compileRun) return false;
    runCacheKey = runCacheKeyFor(filepath, options.skipCheck, entryRawSourceForCache,
                                 loaded.specBaseFile, loaded.specBaseRawSource,
                                 loaded.dependencies.files);
    if (!runCacheKey) return false;
    auto hit = tryLoadRunCache(*runCacheKey, options.outputDir);
    if (!hit) return false;
    fromRunCache = std::move(*hit);
    return true;
  };
  auto callbacks = kex::pass::standardCallbacks();
  callbacks.registerBeforePass([&](std::string_view pass, const kex::pass::UnitRef &unit) {
    if (unit.level == kex::pass::IRUnitLevel<kex::build::Program>::kName &&
        pass == kex::pass::IRUnitLevel<kex::build::Unit>::kName)
      buildProgress.step("Checking", kex::color::blue,
                         moduleCount(build.dependencies.files.size() + 1));
  });
  auto pipeline = kex::build::buildProgramPipeline();
  const bool ran = kex::build::runBuild(build, pipeline, callbacks);
  dependencies = std::move(build.dependencies.files);
  specBaseFile = build.specBaseFile;
  if (!ran) return std::nullopt;
  if (build.answered) {
    cached = true;
    return fromRunCache;
  }
  // In a gating build the units' diagnostics are reported and an error
  // stops it; `--no-check` and `--emit-core` lower whatever the check found.
  if (gates) {
    const auto diagnostics = build.diagnostics();
    const auto errors = std::count_if(diagnostics.begin(), diagnostics.end(),
                                      [](const auto &diagnostic) {
                                        return diagnostic.level ==
                                               kex::semantic::Diagnostic::Level::Error;
                                      });
    for (const auto &diagnostic : diagnostics) printSemanticDiagnostic(diagnostic);
    if (errors > 0) {
      reportAborted(compileRun, static_cast<int>(errors));
      return std::nullopt;
    }
  }
  // A unit that did not lower said why.
  if (!build.allLowered()) return std::nullopt;
  chunks = build.chunks();
  return build.takeModules();
}

auto compileToBeam(Options &options, const std::string &filepath,
                   kex::ast::Program &program,
                   const std::vector<std::string> &scriptArgs,
                   const std::string &entryRawSourceForCache,
                   const std::string &buildFlags) -> int {
  const bool compileRun = options.compileRun;
  // Loaded once per process and used by every pass; timed on its own so it
  // does not hide inside whichever pass touches it first.
  buildProgress.step("Resolving", kex::color::cyan, shownPath(filepath));
  (void)preludeSemanticInterfaces();

  // A `-c --run` whose inputs a recorded build already covers: the build is
  // skipped and its output reused.
  std::optional<std::string> manifestKey;
  bool manifestHit = false;
  if (options.mode == Mode::Compile && compileRun) {
    manifestKey = runManifestKey(buildFlags, filepath);
    if (manifestKey && runManifestCurrent(*manifestKey)) {
      manifestHit = true;
      buildProgress.step("Cached", kex::color::gray, "build of " + shownPath(filepath));
    }
  }

  // `-c --run` without `-o` builds in a temporary directory, removed after.
  OutputDirectory output;
  if (compileRun && !options.outputDirExplicit) {
    char tmpl[] = "/tmp/kex_XXXXXX";
    char *td = mkdtemp(tmpl);
    if (!td) {
      std::cerr << "error: mkdtemp failed\n";
      return 1;
    }
    output.temporary = td;
    options.outputDir = output.temporary;
  }
  const auto stem = std::filesystem::path(filepath).stem().string();

  // Before the run cache: a hit restores its saved `.beam` files over these,
  // so a stale prebuilt copy of a module the program merges from source
  // cannot win (kexhq/kex#27).
  if (options.mode == Mode::Compile &&
      !copyRuntimeBeams(options.outputDir, !options.skipPrelude))
    return output.abandon();

  std::vector<kex::ir::EmitResult> modules;
  std::optional<std::string> runCacheKey;
  std::map<std::string, kex::beam::KexiChunk> chunks;
  std::vector<LoadedDep> dependencies;
  std::string specBaseFile;
  bool cached = false;
  if (manifestHit) {
    auto hit = tryLoadRunCacheFrom(runManifestDir(*manifestKey), options.outputDir);
    if (!hit) {
      std::cerr << "error: the cached build of " << filepath
                << " changed while starting; run it again\n";
      return output.abandon();
    }
    modules = std::move(*hit);
    cached = true;
  } else {
    auto built = buildModules(options, filepath, program, entryRawSourceForCache, runCacheKey,
                              chunks, dependencies, specBaseFile, cached);
    if (!built) return output.abandon();
    modules = std::move(*built);
    // The entry's own modules first.
    std::stable_partition(modules.begin(), modules.end(), [&](const auto &module) {
      return module.moduleName == "kex_" + stem;
    });
  }

  std::vector<std::string> corePaths;
  if (!cached) {
    for (const auto &emitted : modules) {
      std::string path = options.outputDir + "/" + emitted.moduleName + ".core";
      std::ofstream outFile(path);
      if (!outFile) {
        std::cerr << "error: cannot write " << path << "\n";
        return 1;
      }
      outFile << emitted.source;
      corePaths.push_back(std::move(path));
    }
    if (options.mode == Mode::EmitCore) {
      for (const auto &path : corePaths) std::cerr << "wrote " << path << "\n";
      return 0;
    }
    // The runtime and prelude beams were copied before the run-cache lookup.
    if (!compileCoreModules(modules, corePaths, options.outputDir, !output.temporary.empty()))
      return output.abandon();
    // What the next matching invocation reuses (kexhq/kex#323).
    if (runCacheKey) storeRunCache(*runCacheKey, options.outputDir, modules);
    if (manifestKey && !kex::compiled::reachedOutside())
      storeRunManifest(*manifestKey, options.outputDir, modules,
                       runManifestFiles(filepath, specBaseFile, dependencies));
  }
  if (!compileRun && !corePaths.empty())
    attachInterfaces(modules, options.outputDir, chunks);

  // The emitted file is named after the module inside it, so a directory of
  // them can go straight on a BEAM code path (`erl -pa ebin`).
  const std::string kxBeam = options.outputDir + "/" + modules.front().moduleName + ".beam";
  if (output.temporary.empty() && buildProgress.enabled)
    buildProgress.finished(shownPath(kxBeam));
  else if (output.temporary.empty() && !quietBuild)
    std::cerr << "  done  " << kxBeam << "\n";
  if (!compileRun) return 0;
  buildProgress.step("Running", kex::color::cyan, shownPath(filepath));
  return runBuiltProgram(options, filepath, modules, output.temporary, scriptArgs);
}

auto printCompilePipeline(std::ostream &out) -> void { kex::build::printPasses(out); }

auto checkPassPipeline() -> std::optional<std::string> {
  try {
    (void)kex::build::buildProgramPipeline();
    (void)kex::ir::buildModulePipeline();
  } catch (const std::invalid_argument &problem) {
    return problem.what();
  }
  return std::nullopt;
}

auto buildPipelineText() -> std::string {
  std::ostringstream text;
  kex::build::buildProgramPipeline().printPipeline(text);
  return text.str();
}
