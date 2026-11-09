// The standard library as the build consumes it: the prelude's compiled
// interfaces and display tables read back from the prebuilt runtime, and
// `--build-prelude`, which compiles it. Moved out of main.cxx.
#include "../beam/beam_file.hxx"
#include "../beam/collect_metadata.hxx"
#include "../beam/etf.hxx"
#include "../beam/kexi.hxx"
#include "../beam/kexi_registry.hxx"
#include "../cache/cache.hxx"
#include "../cache/separate_unit.hxx"
#include "../beam/term_builder.hxx"
#include "../ast/clone.hxx"
#include "../ast/convert.hxx"
#include "../ast/syntax.hxx"
#include "../ast/type_text.hxx"
#include "../common/artifact_versions.hxx"
#include "../common/prelude_tiers.hxx"
#include "../common/color.hxx"
#include "../interpreter/evaluator.hxx"
#include "../hir/hir.hxx"
#include "../hir/repl_input.hxx"
#include "../ir/emit_core.hxx"
#include "../ir/pass_manager.hxx"
#include "pipeline.hxx"
#include "../ir/lower.hxx"
#include "../lexer/lexer.hxx"
#include "../compiled/expand.hxx"
#include "../module/discovery.hxx"
#include "../module/resolver.hxx"
#include "../parser/parser.hxx"
#include "../semantic/analyzer.hxx"
#include "../semantic/db.hxx"
#include "../validation/tag_validator.hxx"
#include "../common/runtime_dir.hxx"
#include "dependencies.hxx"
#include "build_support.hxx"
#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "../common/completion.hxx"
#include "../common/diagnostics.hxx"
#include "../common/digest.hxx"
#include "../common/file_io.hxx"
#include "../common/prelude_interfaces.hxx"
#include "../common/prelude_loader.hxx"
#include "../common/process.hxx"
#include "../common/repl_commands.hxx"
#include "../common/string_utils.hxx"
#include "../common/timings.hxx"
#include "../common/well_known.hxx"
#include "../semantic/completion_at.hxx"
#include "../semantic/symbol_queries.hxx"
#include <functional>
#include <thread>

using kex::prebuiltRuntimeBeamDir;
using kex::driver::buildProgress;
using kex::driver::BuildProgress;
using kex::driver::dependencyFailure;
using kex::driver::g_dependencyFailureThrows;
using kex::driver::g_memoizeDependencyExpansion;
using kex::driver::isAutomaticImport;
using kex::driver::LoadedDep;
using kex::driver::moduleCount;
using kex::driver::prependItems;
using kex::driver::reportModuleFileError;
using kex::driver::setDefaultScanCache;
using kex::driver::setScanCache;
using kex::driver::sourceModulesNamedBy;
using kex::driver::UnitGraph;
using kex::driver::unitsOf;
using kex::process::BeamVm;
using kex::process::beamTestGate;
using kex::process::exitStatusOf;
using kex::process::runShellCommand;
using kex::process::runShellCommandsTogether;
using kex::process::shellSingleQuote;
using kex::readFile;
using kex::diagnostics::colorizeMessage;
using kex::diagnostics::errorCountPhrase;
using kex::diagnostics::extractHint;
using kex::diagnostics::jsonEscape;
using kex::diagnostics::printCollapseReport;
using kex::diagnostics::printSemanticDiagnostic;
using kex::ast::typeText;
using kex::module::collectUsingModules;
using kex::module::declaredModuleNames;
using kex::module::fileDeclaringModule;
using kex::module::moduleHeaderOf;
using kex::module::moduleNameForPath;
using kex::module::shownPath;
using kex::module::skippedByModuleWalk;
using kex::module::standardModuleRoots;
using kex::snakeCased;

auto preludeExternalModules() -> const kex::ir::ExternalModules & {
  static const auto modules = [] {
    auto runtimeDir = prebuiltRuntimeBeamDir();
    if (runtimeDir.empty()) return kex::ir::ExternalModules{};
    return kex::preludeRegistry(runtimeDir).buildExternalModules();
  }();
  return modules;
}

auto preludeSemanticInterfaces()
    -> const kex::semantic::ImportedInterfaces & {
  return kex::defaultPreludeInterfaces();
}

struct PreludeBuildModule {
  kex::ir::EmitResult emitted;
  kex::beam::KexiChunk interface;
};

// Build the stdlib entry plus ordinary Kex module companions. The entry keeps
// receiver compatibility code until typed receiver ownership is carried into
// IR; public module calls already use companion interfaces.
auto compilePreludeCore(const std::string &dir,
                        std::vector<PreludeBuildModule> *builtModules,
                        bool verifyHir) -> bool {
  kex::ast::Program merged;
  auto files = kex::preludeSourceFiles();
  if (files.empty()) {
    std::cerr << "error: no prelude source root is available\n";
    return false;
  }
  try {
    files = kex::orderPreludeSourcesByTier(files);
  } catch (const std::exception &e) {
    std::cerr << "error: invalid prelude tier manifest: " << e.what() << "\n";
    return false;
  }
  auto tierGroups = kex::groupPreludeSourcesByTier(files);
  const auto stdlibRoot =
      std::filesystem::path(files.front()).parent_path();
  auto optInFiles = kex::optInStdlibSources(stdlibRoot);
  files.insert(files.end(), optInFiles.begin(), optInFiles.end());
  // Track item index boundaries per tier: tierBounds[t] = index of first
  // item belonging to tier t.  tierBounds[4] = total item count.
  std::array<size_t, 5> tierBounds{};
  // A stdlib file that does not parse is reported as itself, and stops the
  // build before analysis. Parser recovery can drop the rest of an enclosing
  // block, and analyzing what survived produced a wall of type errors in
  // OTHER files (every String method missing after a truncated `make String`)
  // with nothing pointing at the file that was actually wrong (kexhq/kex#165).
  bool parsed = true;
  auto mergeFile = [&](const std::string &f) {
    kex::Lexer lex(readFile(f), f);
    kex::Parser parser(lex.tokenizeAll(), f);
    auto prog = parser.parseProgram();
    for (const auto &diag : parser.diagnostics()) {
      std::cerr << "error: prelude source: " << diag.location.file << ":"
                << diag.location.line << ":" << diag.location.column << ": "
                << diag.message << "\n";
      parsed = false;
    }
    for (auto &item : prog.items)
      if (kex::ast::isMergedDeclaration(item))
        merged.items.push_back(std::move(item));
  };
  for (size_t t = 0; t < 4; t++) {
    tierBounds[t] = merged.items.size();
    for (const auto &f : tierGroups[t]) mergeFile(f);
  }
  for (const auto &f : optInFiles) mergeFile(f);
  if (!parsed) return false;
  tierBounds[4] = merged.items.size();
  try {
    kex::semantic::Analyzer analyzer;
    if (!analyzer.analyze(merged)) {
      for (const auto &diag : analyzer.diagnostics())
        if (diag.level == kex::semantic::Diagnostic::Level::Error)
          std::cerr << "error: prelude interface: " << diag.location.file
                    << ":" << diag.location.line << ":"
                    << diag.location.column << ": " << diag.message << "\n";
      return false;
    }

    // The prelude lowers from what its own check decided, as every other
    // program does.
    const kex::hir::Program hir(merged, analyzer);
    // `--verify-hir --build-prelude`: the stdlib decides every call too.
    if (verifyHir) {
      const auto problems = kex::hir::verify(hir);
      for (const auto &problem : problems) std::cerr << problem << "\n";
      std::cerr << "prelude: " << problems.size() << " undecided call sites\n";
      if (!problems.empty()) return false;
    }
    const kex::ir::LowerContext selfContext{.lowersPrelude = true, .hir = &hir};
    std::vector<kex::ir::Module> modules;
    modules.push_back(kex::ir::lowerProgramTiered(merged, tierBounds,
                                                  "prelude", selfContext));
    auto splitModules = kex::ir::lowerModules(merged, "prelude", selfContext);
    for (size_t i = 1; i < splitModules.size(); i++)
      modules.push_back(std::move(splitModules[i]));

    auto passes = kex::ir::standardPasses();
    builtModules->clear();
    for (size_t i = 0; i < modules.size(); i++) {
      PreludeBuildModule built;
      passes.run(modules[i]);
      built.emitted = kex::ir::emitCore(modules[i]);
      std::ofstream out(dir + "/" + built.emitted.moduleName + ".core");
      if (!out) return false;
      out << built.emitted.source;

      if (i == 0) {
        kex::beam::CollectOptions options;
        options.unitId = kex::names::kPreludeModule;
        options.moduleAtom = built.emitted.moduleName;
        options.moduleName = std::string(kex::semantic::kFileLevelPreludeModule);
        options.collectTopLevel = true;
        options.flattenModules = true;
        options.role = kex::beam::KexiModuleRole::Entry;
        options.analysis = &analyzer;
        built.interface = kex::beam::collectMetadata(merged, options);
        built.interface.metadata.package.id = "kex.stdlib";
        built.interface.metadata.package.unitIds = {kex::names::kPreludeModule};
        built.interface.metadata.package.receiverProviders = {
            std::string(kex::semantic::kFileLevelPreludeModule)};
        built.interface.metadata.package.automaticImports = {
            std::string(kex::semantic::kFileLevelPreludeModule)};
        built.interface.sourceHash = kex::preludeSourceHash(files);
      } else {
        kex::beam::CollectOptions options;
        options.unitId = kex::names::kPreludeModule;
        options.moduleAtom = built.emitted.moduleName;
        options.analysis = &analyzer;
        options.role = kex::beam::KexiModuleRole::Companion;
        options.entryBackPointer = kex::names::kPreludeModule;
        options.moduleName = built.emitted.moduleName.rfind("Kex.", 0) == 0
            ? built.emitted.moduleName.substr(4) : built.emitted.moduleName;
        built.interface = kex::beam::collectMetadata(merged, options);
        kex::beam::qualifyModuleTypeNames(built.interface, merged, options.moduleName, {});
      }
      kex::beam::recordImplementations(
          built.interface, routedIn(built.emitted.routedImplementations));
      builtModules->push_back(std::move(built));
    }
    passes.report();
    const auto automaticInterfaces =
        kex::sourcePreludeSemanticInterfaces();
    for (size_t i = 1; i < builtModules->size(); i++) {
      kex::beam::KexiCompanion companion;
      companion.beamAtom = (*builtModules)[i].emitted.moduleName;
      companion.relativePath = companion.beamAtom + ".beam";
      (*builtModules)[0].interface.metadata.companions.push_back(
          std::move(companion));
      const auto& sourceModule =
          (*builtModules)[i].interface.metadata.sourceModule;
      const auto automatic =
          automaticInterfaces.modules.find(sourceModule);
      if (!sourceModule.empty() &&
          automatic != automaticInterfaces.modules.end() &&
          automatic->second.automaticImport)
        (*builtModules)[0]
            .interface.metadata.package.automaticImports.push_back(
                sourceModule);
    }
  } catch (const kex::ir::LowerError &e) {
    std::cerr << "error: prelude: " << e.what() << "\n";
    return false;
  }
  return true;
}

// Load stdlib record layouts from the embedded interface of the explicitly
// built prelude artifact. Compiled metadata stays separate from the user's AST.
auto loadPreludeRecordLayouts() -> std::vector<kex::ir::ExternalRecordLayout> {
  std::vector<kex::ir::ExternalRecordLayout> layouts;
  auto runtimeDir = prebuiltRuntimeBeamDir();
  if (runtimeDir.empty()) return layouts;
  const auto &registry = kex::preludeRegistry(runtimeDir);
  for (const auto *module : registry.allLoadedModules())
    for (const auto &record : module->chunk.metadata.records) {
      kex::ir::ExternalRecordLayout layout;
      // The layout is keyed by the tuple tag the record's own module emits,
      // so a construction here agrees with a pattern match there.
      layout.name = record.tagAtom.empty() ? record.name : record.tagAtom;
      layout.moduleAtom = module->beamAtom;
      for (const auto &field : record.fields)
        layout.fields.push_back(field.name);
      layouts.push_back(std::move(layout));
    }
  return layouts;
}


// Prelude variant tags, so a compiled program can register their display info
// the same way it registers the prelude's record layouts.
auto loadPreludeVariantTags() -> std::vector<kex::ir::ExternalVariantTag> {
  std::vector<kex::ir::ExternalVariantTag> tags;
  auto runtimeDir = prebuiltRuntimeBeamDir();
  if (runtimeDir.empty()) return tags;
  const auto &registry = kex::preludeRegistry(runtimeDir);
  for (const auto *module : registry.allLoadedModules())
    for (const auto &adt : module->chunk.metadata.adts)
      for (const auto &constructor : adt.constructors)
        tags.push_back({constructor.tagAtom,
                        static_cast<int>(constructor.arity), adt.name});
  return tags;
}

// Display info for everything the PRELUDE declares, in the shape
// kex_io:register_display/2 expects. A compiled module registers its own
// records and variants (see lower.cxx's withDisplayInfo), and `/load`
// registers a loaded unit's — but the prelude's were registered by nobody, so
// the BEAM REPL printed every prelude ADT as raw data: `(:InvalidFormat, "x")`
// instead of `InvalidFormat("x")`.
auto preludeDisplayRegistration() -> std::string {
  auto runtimeDir = prebuiltRuntimeBeamDir();
  if (runtimeDir.empty()) return "";
  std::string records;
  std::string variants;
  const auto &registry = kex::preludeRegistry(runtimeDir);
  for (const auto *module : registry.allLoadedModules()) {
    for (const auto &record : module->chunk.metadata.records) {
      std::string fields;
      for (size_t i = 0; i < record.fields.size(); i++) {
        if (i) fields += ", ";
        fields += "'" + record.fields[i].name + "'";
      }
      // Keyed by the value's RUNTIME tag, which the metadata records: a
      // record declared inside a `module` block is tagged with its qualified
      // identity, one at a file's top level with its bare name.
      if (!records.empty()) records += ", ";
      records += "'" +
                 (record.tagAtom.empty() ? record.name : record.tagAtom) +
                 "' => [" + fields + "]";
    }
    for (const auto &adt : module->chunk.metadata.adts)
      for (const auto &constructor : adt.constructors) {
        if (!variants.empty()) variants += ", ";
        variants += "'" + constructor.tagAtom + "' => {" +
                    std::to_string(constructor.arity) + ", '" + adt.name +
                    "'}";
      }
  }
  if (records.empty() && variants.empty()) return "";
  return "kex_io:register_display(#{" + records + "}, #{" + variants + "})";
}

// `--build-prelude DIR`: compile the standard library into DIR, attach each
// module's KexI interface, and drop artifacts of modules that no longer exist.
auto buildPrelude(const std::string &dir, bool verifyHir) -> int {
  std::vector<PreludeBuildModule> modules;
  if (!compilePreludeCore(dir, &modules, verifyHir))
    return 1;
  try {
    // The prelude build directory is also the installed-runtime staging
    // directory. Remove companions from older builds so deleting or
    // renaming a Kex module cannot leave a stale beam that gets installed
    // merely because it still matches `*.beam`.
    std::unordered_set<std::string> currentCompanions;
    for (auto &module : modules) {
      module.interface.intrinsicAbiVersion = kex::kIntrinsicAbiVersion;
      module.interface.backendRepresentationVersion =
          kex::kBeamRepresentationVersion;
    }
    for (size_t i = 1; i < modules.size(); i++)
      currentCompanions.insert(modules[i].emitted.moduleName);
    std::error_code cleanupError;
    for (const auto &entry : std::filesystem::directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied,
             cleanupError)) {
      if (cleanupError) break;
      const auto &path = entry.path();
      const auto stem = path.stem().string();
      const auto extension = path.extension().string();
      if (stem.rfind("Kex.", 0) == 0 &&
          (extension == ".beam" || extension == ".core") &&
          !currentCompanions.contains(stem))
        std::filesystem::remove(path, cleanupError);
      if (cleanupError) break;
    }
    if (cleanupError)
      throw std::runtime_error("could not remove stale stdlib artifact: " +
                               cleanupError.message());

    for (const auto &module : modules) {
      // -W0: erlc's warnings analyze the GENERATED Core Erlang, not Kex —
      // they carry no source location (`no_file:`) and the emitter's dead
      // statement-value lets make them inevitable. Real errors still fail
      // the exit-code check below.
      std::string cmd = erlcExecutable() + " +from_core -W0 -pa " + dir +
                        " -o " + dir + " " + dir + "/" +
                        module.emitted.moduleName + ".core";
      if (runShellCommand(cmd) != 0) return 1;
    }

    for (size_t i = 1; i < modules.size(); i++)
      modules[i].interface.interfaceHash =
          kex::beam::computeInterfaceHash(modules[i].interface);
    for (auto &companion : modules[0].interface.metadata.companions)
      for (size_t i = 1; i < modules.size(); i++)
        if (modules[i].emitted.moduleName == companion.beamAtom)
          companion.expectedHash = modules[i].interface.interfaceHash;
    modules[0].interface.interfaceHash =
        kex::beam::computeInterfaceHash(modules[0].interface);

    for (auto &module : modules) {
      auto beamPath = dir + "/" + module.emitted.moduleName + ".beam";
      auto beam = kex::beam::readBeamFile(beamPath);
      module.interface.artifactHash = kex::beam::computeArtifactHash(beam);
      beam.setChunk(kex::beam::KEXI_CHUNK_ID,
                    kex::beam::serializeKexi(module.interface));
      kex::beam::writeBeamFile(beam, beamPath);
    }
  } catch (const std::exception &e) {
    std::cerr << "error: could not attach stdlib interface: " << e.what()
              << "\n";
    return 1;
  }
  return 0;
}
