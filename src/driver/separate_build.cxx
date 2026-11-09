// The build unit by unit (docs/plan_separate_compilation.md): each unit of the
// program checked and lowered on its own, in build order, against the
// interfaces of the units below it, as a compiled unit's are read back. Runs
// inside the `dependencies` pass, while each dependency is still its own
// program. Moved out of main.cxx.
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



// Every build and run (the compile pipeline, which sets g_building) checks and
// lowers each unit on its own, in build order, against the interfaces of the
// units below it, as a compiled unit's are read back.
bool g_building = false;
// Whether this build's check stops it on a type error: not under `--no-check`
// or `--emit-core`, which lower every unit whatever the check found.
bool g_checkGates = true;

// An emitted module's routed implementations, as recordImplementations asks.
auto routedIn(const std::set<std::pair<std::string, int>> &routed)
    -> std::function<bool(const std::string &, int)> {
  return [&routed](const std::string &function, int arity) {
    return routed.count({function, arity}) > 0;
  };
}

// The entry file being built, which names the entry unit's file-local module
// as the merged build names it.
std::string g_separateEntryPath;

// Every unit's modules, set when each unit was lowered apart and a separate
// build was asked for.
std::optional<std::vector<kex::ir::EmitResult>> g_separateModules;
// The files the entry unit's names are reported for, as the merged check
// reports them: the entry and a spec's base.
std::vector<std::string> g_separateNameCheckFiles;
// A separate build's diagnostics, every level, reported as the merged check
// reports its own.
std::vector<kex::semantic::Diagnostic> g_separateDiagnostics;
// Every module a separate build emitted, described by its own unit: what
// `kex -c` attaches as its KexI, so the merged program is never analysed.
std::map<std::string, kex::beam::KexiChunk> g_separateChunks;


// The modules providing receiver functions in `externals`: what the check
// counts a call's providers among (Analyzer::setLinkedModules).
auto providerModules(const kex::ir::ExternalModules &externals) -> std::set<std::string> {
  std::set<std::string> modules;
  for (const auto &[_, providers] : externals.receiverFunctions)
    for (const auto &provider : providers) modules.insert(provider.moduleAtom);
  return modules;
}

namespace {

// The providers a unit of this build lowers against: the prebuilt prelude's
// and those of the units below it. The check counts a call's providers among
// the same modules (Analyzer::setLinkedModules).
auto unitExternalModules(const kex::beam::KexiRegistry &registry) -> kex::ir::ExternalModules {
  // A module this build compiles is described by its own interface. The
  // prebuilt runtime has its own copy of an opt-in standard library module,
  // laid out as the prelude build lays it out (`make Router` filed under
  // `Kex.Net.HTTP.Router`, not `Kex.Net.HTTP`), and left in, it wins.
  std::set<std::string> builtModules, builtAtoms;
  for (const auto *module : registry.allLoadedModules()) {
    builtModules.insert(module->chunk.metadata.sourceModule);
    builtAtoms.insert(module->beamAtom);
  }
  auto prelude = preludeExternalModules();
  const auto builtKey = [&](const std::string &key) {
    const auto dot = key.rfind('.');
    return dot != std::string::npos && builtModules.contains(key.substr(0, dot));
  };
  std::erase_if(prelude.exportToBeamFn, [&](const auto &entry) { return builtKey(entry.first); });
  std::erase_if(prelude.exportArity, [&](const auto &entry) { return builtKey(entry.first); });
  std::erase_if(prelude.exportParamNames, [&](const auto &entry) { return builtKey(entry.first); });
  std::erase_if(prelude.foulExports, [&](const auto &key) { return builtKey(key); });
  for (auto &[_, functions] : prelude.receiverFunctions)
    std::erase_if(functions, [&](const auto &function) {
      return builtAtoms.contains(function.moduleAtom);
    });
  auto unitExternals = registry.buildExternalModules();
  for (auto &[_, functions] : unitExternals.receiverFunctions)
    for (auto &function : functions) function.compiledApart = true;
  for (const auto &[name, _] : unitExternals.nameToAtom) unitExternals.compiledApartModules.insert(name);
  auto externals = kex::ir::mergeExternalModules(prelude, unitExternals);
  // The merge lets a unit's providers of a name replace the prelude's, as a
  // module loaded into the REPL does; here both stand, or `handle.feed` lost
  // the prelude's foul FileHandle provider to FS.File's, and its context.
  for (const auto &[name, providers] : prelude.receiverFunctions) {
    auto &merged = externals.receiverFunctions[name];
    for (const auto &provider : providers)
      if (std::none_of(merged.begin(), merged.end(), [&](const auto &existing) {
            return existing.moduleAtom == provider.moduleAtom &&
                   existing.beamFunction == provider.beamFunction &&
                   existing.beamArity == provider.beamArity &&
                   existing.receiverType == provider.receiverType &&
                   existing.paramNames == provider.paramNames;
          }))
        merged.push_back(provider);
  }
  return externals;
}

// Phase 3: a checked unit lowered on its own, with its own analysis, against
// the prelude and the KexI of the units below it (`registry`, which holds only
// those yet). Its modules' Core Erlang, or nothing when lowering failed; with
// KEX_SEPARATE_CORE=<dir> also written to `dir`, to be compared with the
// merged build's.
auto lowerUnitSeparately(const kex::ast::Program &program, const std::string &path,
                         const kex::semantic::Analyzer &analyzer,
                         const kex::beam::KexiRegistry &registry,
                         const std::string &dir)
    -> std::optional<std::vector<kex::ir::EmitResult>> {
  auto externals = unitExternalModules(registry);
  auto layouts = loadPreludeRecordLayouts();
  auto variants = loadPreludeVariantTags();
  // Where each type's `make` functions are, for the runtime's dispatch on a
  // value: its declaring module may have none, and another unit may.
  std::map<std::string, std::string> methodOwners;
  // A `make` over a union (`make TypeRef | PatternRef`) owns each member.
  for (const auto *module : registry.allLoadedModules())
    for (const auto &method : module->chunk.typeInterface.methods) {
      if (method.moduleFunction || method.typeOnly || !method.receiverType) continue;
      const auto &receiver = *method.receiverType;
      if (receiver.kind == kex::beam::KexiType::Named)
        methodOwners.try_emplace(receiver.name, module->beamAtom);
      else if (receiver.kind == kex::beam::KexiType::Union)
        for (const auto &member : receiver.typeArgs)
          if (member && member->kind == kex::beam::KexiType::Named)
            methodOwners.try_emplace(member->name, module->beamAtom);
    }
  const auto ownerOf = [&](const std::string &type) {
    const auto found = methodOwners.find(type);
    return found == methodOwners.end() ? std::string{} : found->second;
  };
  // A type of a unit below that claims a trait has the trait's defaults
  // (`Foldable`'s `any?` on `Data.Set`) in the module holding its `make`
  // functions, which forwards them to where its unit emitted them. The merged
  // build had them in its own dispatcher; here they are receiver functions
  // for that type, at the arities of the prelude's generic ones.
  std::map<std::string, std::vector<std::string>> traitDefaults;
  for (const auto &trait : preludeSemanticInterfaces().traits)
    traitDefaults[trait.name] = trait.defaultMethods;
  for (const auto &trait : registry.buildSemanticInterfaces().traits)
    traitDefaults[trait.name] = trait.defaultMethods;
  for (const auto *module : registry.allLoadedModules())
    for (const auto &conformance : module->chunk.metadata.traitConformances) {
      const auto owner = ownerOf(conformance.typeName);
      const auto defaults = traitDefaults.find(conformance.traitName);
      if (owner.empty() || defaults == traitDefaults.end()) continue;
      for (const auto &name : defaults->second) {
        auto &providers = externals.receiverFunctions[name];
        std::set<int> arities;
        for (const auto &provider : providers)
          if (provider.moduleAtom == kex::names::kPreludeModule && provider.receiverType.empty())
            arities.insert(provider.beamArity);
        for (const auto arity : arities)
          if (std::none_of(providers.begin(), providers.end(), [&](const auto &provider) {
                return provider.moduleAtom == owner && provider.beamArity == arity &&
                       provider.receiverType == conformance.typeName;
              }))
            providers.push_back({owner, name, arity, {}, conformance.typeName, false, true});
      }
    }
  for (const auto *module : registry.allLoadedModules()) {
    for (const auto &record : module->chunk.metadata.records) {
      kex::ir::ExternalRecordLayout layout;
      layout.name = record.tagAtom.empty() ? record.name : record.tagAtom;
      layout.moduleAtom = module->beamAtom;
      layout.methodOwner = ownerOf(layout.name);
      layout.declaringModule = module->chunk.metadata.sourceModule;
      for (const auto &field : record.fields) {
        layout.fields.push_back(field.name);
        layout.fieldDefaults.push_back(field.defaultSource);
      }
      layouts.push_back(std::move(layout));
    }
    for (const auto &adt : module->chunk.metadata.adts)
      for (const auto &constructor : adt.constructors)
        variants.push_back(
            {constructor.tagAtom, constructor.arity, adt.name, ownerOf(adt.name)});
  }
  // Several units can have the same basename (`app/models/conversation.kex`
  // and `app/views/chat/conversation.kex`). Their file modules must not
  // overwrite one another. Keep the entry's traditional `kex_main` name.
  const auto baseStem = std::filesystem::path(path).stem().string();
  auto stem = baseStem;
  if (path != g_separateEntryPath) {
    stem += "_" + kex::sha256Hex(path).substr(0, 16);
  }
  try {
    const kex::hir::Program hir(program, analyzer);
    auto modules = kex::ir::lowerModules(
        program, stem,
        {.sourcePath = path,
         .externals = &externals,
         .externalRecords = &layouts,
         .externalVariants = &variants,
         .hir = &hir});
    if (!dir.empty()) std::filesystem::create_directories(dir);
    // The unit's own module registers its records' layouts as it loads, but
    // only the entry's `main` used to run, and a call goes to a companion
    // (`Kex.RecordValue.Sessions`): its loading loads that module first, or
    // `Type.of(Session)` met a record nothing had registered.
    const auto fileModule = "kex_" + stem;
    const auto registers = std::any_of(modules.begin(), modules.end(), [&](const auto &module) {
      return module.name == fileModule && !module.onLoad.empty();
    });
    for (auto &module : modules) {
      if (!registers || module.name == fileModule || !module.onLoad.empty()) continue;
      auto load = std::make_unique<kex::ir::Expr>();
      std::vector<kex::ir::ExprPtr> args;
      args.push_back(std::make_unique<kex::ir::Expr>());
      args.back()->node = kex::ir::Lit{kex::ir::LitKind::Atom, fileModule};
      load->node = kex::ir::Call{"code", "ensure_loaded", 1, std::move(args), false};
      auto ok = std::make_unique<kex::ir::Expr>();
      ok->node = kex::ir::Lit{kex::ir::LitKind::Atom, "ok"};
      auto body = std::make_unique<kex::ir::Expr>();
      body->node = kex::ir::Let{"_Loaded", std::move(load), std::move(ok)};
      kex::ir::FunDef onLoad;
      onLoad.name = "__kex_on_load";
      onLoad.exported = false;
      kex::ir::FunClause clause;
      clause.body = std::move(body);
      onLoad.clauses.push_back(std::move(clause));
      module.functions.push_back(std::move(onLoad));
      module.onLoad = "__kex_on_load";
    }
    // What the unit's interface names in a module's companion and lowering
    // emitted in the unit's own module, the companion forwards there:
    //   * a free function sharing its name with a `make` function of the same
    //     module (`Kex.Vault:get` is `kex_vault:'Vault.get$free'`);
    //   * the functions of a `make` block written outside the module for one
    //     of its types (`make Type.Field` after `module Type`);
    //   * a trait's default functions (`Foldable`'s `any?`), dispatching over
    //     the unit's types that claim it, from every companion: the runtime
    //     asks the module holding a type's `make` functions (`Kex.Data` for
    //     `Data.Set`).
    std::set<std::string> traitDefaults;
    for (const auto &item : program.items)
      if (const auto *trait = std::get_if<std::unique_ptr<kex::ast::TraitDef>>(&item);
          trait && *trait)
        for (const auto &member : (*trait)->body)
          if (const auto *function = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&member);
              function && *function)
            traitDefaults.insert((*function)->name);
    std::map<std::string, std::set<std::string>> outsideMakes;
    for (const auto &item : program.items) {
      const auto *make = std::get_if<std::unique_ptr<kex::ast::MakeDef>>(&item);
      if (!make || !*make || !(*make)->target) continue;
      const auto *target = std::get_if<kex::ast::TypeName>(&(*make)->target->kind);
      if (!target || target->parts.size() < 2) continue;
      const auto owner =
          kex::join(target->parts, ".", target->parts.size() - 1);
      const auto note = [&](const kex::ast::FunctionDef &function) {
        outsideMakes[kex::names::moduleAtom(owner)].insert(function.name);
      };
      for (const auto &member : (*make)->body)
        if (const auto *function = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&member);
            function && *function)
          note(**function);
        else if (const auto *block = std::get_if<std::unique_ptr<kex::ast::VisibilityBlock>>(&member);
                 block && *block && (*block)->isPublic)
          for (const auto &inner : (*block)->items)
            if (const auto *function = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&inner);
                function && *function)
              note(**function);
    }
    std::vector<std::tuple<std::string, std::string, std::string, int>> forwards;
    for (const auto &module : modules) {
      if (module.name != fileModule) continue;
      for (const auto &function : module.functions) {
        for (const auto &[companion, names] : outsideMakes)
          if (names.contains(function.name) && function.exported)
            forwards.emplace_back(companion, function.name, function.name, function.arity);
        if (traitDefaults.contains(function.name) && function.exported)
          for (const auto &companion : modules)
            if (companion.name != fileModule)
              forwards.emplace_back(companion.name, function.name, function.name,
                                    function.arity);
        const auto dot = function.name.rfind('.');
        if (dot == std::string::npos || !function.name.ends_with("$free")) continue;
        forwards.emplace_back(kex::names::moduleAtom(function.name.substr(0, dot)),
                              function.name.substr(dot + 1, function.name.size() - dot - 6),
                              function.name, function.arity);
      }
    }
    for (const auto &[companion, name, target, arity] : forwards)
      for (auto &module : modules) {
        if (module.name != companion ||
            std::any_of(module.functions.begin(), module.functions.end(),
                        [&](const auto &function) {
                          return function.name == name && function.arity == arity;
                        }))
          continue;
        kex::ir::FunClause clause;
        std::vector<kex::ir::ExprPtr> args;
        for (int i = 0; i < arity; i++) {
          const auto param = "_Arg" + std::to_string(i);
          auto pattern = std::make_unique<kex::ir::Pattern>();
          pattern->kind = kex::ir::PatKind::Var;
          pattern->name = param;
          clause.params.push_back(std::move(pattern));
          args.push_back(std::make_unique<kex::ir::Expr>());
          args.back()->node = kex::ir::Var{param};
        }
        clause.body = std::make_unique<kex::ir::Expr>();
        clause.body->node = kex::ir::Call{fileModule, target, arity, std::move(args), true};
        kex::ir::FunDef forward;
        forward.name = name;
        forward.arity = arity;
        forward.clauses.push_back(std::move(clause));
        module.functions.push_back(std::move(forward));
      }
    // KexI advertises a receiver method at its plain name. A unit can emit
    // only the receiver-qualified implementation when an imported field or
    // method shares that name (`Grant.teamId`, `OrganizationId.empty?`).
    // Give the companion the advertised entry point when it has one local
    // owner and no dispatcher of its own.
    std::map<std::string, std::set<std::string>> ownTypes;
    std::function<void(const kex::ast::ModuleDef &)> noteTypes =
        [&](const kex::ast::ModuleDef &definition) {
          auto &types = ownTypes[kex::names::moduleAtom(definition.name)];
          for (const auto &item : definition.body) {
            std::string name;
            if (const auto *record = std::get_if<std::unique_ptr<kex::ast::RecordDef>>(&item);
                record && *record) name = (*record)->name;
            if (const auto *type = std::get_if<std::unique_ptr<kex::ast::TypeDef>>(&item);
                type && *type) name = (*type)->name;
            if (!name.empty()) {
              types.insert(name);
              types.insert(definition.name + "." + name);
            }
            if (const auto *nested = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
                nested && *nested) noteTypes(**nested);
          }
        };
    for (const auto &item : program.items)
      if (const auto *definition = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
          definition && *definition) noteTypes(**definition);
    for (auto &module : modules) {
      std::map<std::pair<std::string, int>, std::vector<std::string>> implementations;
      for (const auto &function : module.functions) {
        if (!function.exported) continue;
        const auto slash = function.name.find('/');
        if (slash == std::string::npos) continue;
        const auto owner = function.name.substr(slash + 1);
        if (!ownTypes[module.name].contains(owner)) continue;
        implementations[{function.name.substr(0, slash), function.arity}]
            .push_back(function.name);
      }
      for (const auto &[key, targets] : implementations) {
        if (targets.size() != 1 ||
            std::any_of(module.functions.begin(), module.functions.end(),
                        [&](const auto &function) {
                          return function.name == key.first && function.arity == key.second;
                        })) continue;
        kex::ir::FunDef forward;
        forward.name = key.first;
        forward.arity = key.second;
        kex::ir::FunClause clause;
        std::vector<kex::ir::ExprPtr> args;
        for (int i = 0; i < key.second; ++i) {
          const auto param = "_Arg" + std::to_string(i);
          auto pattern = std::make_unique<kex::ir::Pattern>();
          pattern->kind = kex::ir::PatKind::Var;
          pattern->name = param;
          clause.params.push_back(std::move(pattern));
          args.push_back(std::make_unique<kex::ir::Expr>());
          args.back()->node = kex::ir::Var{param};
        }
        clause.body = std::make_unique<kex::ir::Expr>();
        clause.body->node = kex::ir::Call{"", targets.front(), key.second,
                                          std::move(args), true};
        forward.clauses.push_back(std::move(clause));
        module.functions.push_back(std::move(forward));
      }
    }
    auto emitted = kex::ir::emitModules(modules);
    if (!dir.empty())
      for (const auto &module : emitted)
        std::ofstream(dir + "/" + module.moduleName + ".core") << module.source;
    return emitted;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "error: %s: %s\n", shownPath(path).c_str(), error.what());
  }
  return std::nullopt;
}

auto checkUnitsSeparately(kex::ast::Program &entry, std::vector<LoadedDep> &deps,
                          const std::vector<std::vector<std::string>> &units,
                          const std::map<std::string, std::set<std::string>> &uses,
                          const kex::semantic::ImportedInterfaces *interfaces) -> void {
  g_separateModules.reset();
  g_separateDiagnostics.clear();
  g_separateChunks.clear();
  std::map<std::string, LoadedDep *> byPath;
  for (auto &dep : deps) byPath[*dep.path] = &dep;
  // Every unit's modules, for a separate build; it stands only if each unit
  // checked and lowered.
  std::vector<kex::ir::EmitResult> separateModules;
  bool allLowered = true;
  // What each unit checked so far says of itself, with its files and the
  // modules it provides.
  struct Described {
    std::set<std::string> files;
    std::vector<std::string> names;
    kex::beam::LoadedUnit unit;
    // SeparateUnitCache::interfaceDigest of `unit`, once.
    std::string digest;
  };
  std::vector<Described> describedUnits;
  std::optional<kex::cache::SeparateUnitCache> unitCache;
  if (auto root = kex::cache::base())
    unitCache.emplace(*root);
  // KEX_TIMINGS: where a separate build's time goes, summed over its units.
  std::map<std::string, double> unitPhaseMs;
  auto phaseStart = std::chrono::steady_clock::now();
  const auto chargeTo = [&](const char *phase) {
    const auto now = std::chrono::steady_clock::now();
    unitPhaseMs[phase] +=
        std::chrono::duration<double, std::milli>(now - phaseStart).count();
    phaseStart = now;
  };
  for (const auto &unit : units) {
    const bool isEntry = unit.size() == 1 && unit.front().empty();
    chargeTo("other");
    // A unit is checked and lowered against the units its files reach, and
    // no others: one it does not use (`Harmat` from `Parsing`) offers it
    // nothing, not even an operator on an `Unknown` value.
    std::set<std::string> reached;
    std::vector<std::string> pending(unit.begin(), unit.end());
    while (!pending.empty()) {
      const auto file = pending.back();
      pending.pop_back();
      if (const auto used = uses.find(file); used != uses.end())
        for (const auto &next : used->second)
          if (reached.insert(next).second) pending.push_back(next);
    }
    chargeTo("reach");
    kex::beam::KexiRegistry registry;
    kex::module::PackageMetadata build;
    build.id = "kex.build";
    for (const auto &below : describedUnits) {
      if (std::none_of(below.files.begin(), below.files.end(),
                       [&](const std::string &file) { return reached.contains(file); }))
        continue;
      build.unitIds.push_back(below.unit.modules.front().chunk.metadata.unitId);
      // Once each: a module can be split across files of different units.
      for (const auto &name : below.names)
        if (std::find(build.receiverProviders.begin(), build.receiverProviders.end(), name) ==
            build.receiverProviders.end())
          build.receiverProviders.push_back(name);
      registry.registerUnit(below.unit);
    }
    // The build's own units as one package, every module of them a
    // provider: a type's `make` functions reach the units above it only
    // through a package that says so, as the prelude's do.
    if (!build.unitIds.empty())
      for (const auto &error : registry.declarePackage(build))
        std::fprintf(stderr, "separate: package: %s\n", error.message.c_str());
    std::optional<std::string> cacheKey;
    if (!isEntry && unitCache) {
      std::vector<kex::cache::SeparateUnitCache::Source> sources;
      bool safe = true;
      for (const auto &path : unit) {
        const auto found = byPath.find(path);
        if (found == byPath.end()) { safe = false; break; }
        const auto &source = *found->second->source;
        if (sourceMentionsCompiledBlock(source) || sourceMentionsEmbed(source) ||
            source.find("Template") != std::string::npos) {
          safe = false;
          break;
        }
        sources.push_back({path, source});
      }
      std::vector<std::string> dependencies;
      for (const auto &below : describedUnits) {
        if (std::none_of(below.files.begin(), below.files.end(),
                         [&](const std::string &file) { return reached.contains(file); }))
          continue;
        dependencies.push_back(below.digest);
      }
      chargeTo("registry");
      if (safe) {
        const auto runtime = prebuiltRuntimeBeamDir();
        cacheKey = unitCache->keyFor(
            kex::cache::toolchainFingerprint(),
            "runtime:" + runtime + ":" +
                kex::compiled::fileStamp(runtime + "/kex_prelude.beam"),
            std::move(sources), dependencies);
        if (auto cached = unitCache->load(*cacheKey)) {
          if (const char *coreDir = std::getenv("KEX_SEPARATE_CORE");
              coreDir && *coreDir) {
            std::filesystem::create_directories(coreDir);
            for (const auto &module : cached->modules)
              std::ofstream(std::string(coreDir) + "/" + module.moduleName + ".core")
                  << module.source;
          }
          for (std::size_t i = 0; i < cached->modules.size(); ++i) {
            if (i < cached->attachChunks.size())
              g_separateChunks[cached->modules[i].moduleName] = cached->attachChunks[i];
            separateModules.push_back(std::move(cached->modules[i]));
          }
          std::vector<std::string> names;
          for (const auto &module : cached->interface.modules)
            names.push_back(module.chunk.metadata.sourceModule);
          describedUnits.push_back({std::set<std::string>(unit.begin(), unit.end()),
                                    std::move(names), std::move(cached->interface)});
          describedUnits.back().digest =
              kex::cache::SeparateUnitCache::interfaceDigest(describedUnits.back().unit);
          if (kex::timings::enabled())
            std::fprintf(stderr, "  unit cache hit: %s\n", unit.front().c_str());
          continue;
        }
      }
    }
    const auto emittedBefore = separateModules.size();
    const auto describedBefore = describedUnits.size();
    // The unit's files, as one program for the length of its check; their
    // items go back where they came from afterwards.
    kex::ast::Program program;
    std::vector<std::pair<LoadedDep *, std::size_t>> taken;
    if (isEntry) {
      program.items = std::move(entry.items);
    } else {
      for (const auto &path : unit) {
        const auto found = byPath.find(path);
        if (found == byPath.end()) continue;
        auto &items = found->second->program->items;
        taken.push_back({found->second, items.size()});
        for (auto &item : items) program.items.push_back(std::move(item));
        items.clear();
      }
    }
    chargeTo("cache key and load");
    auto below = registry.buildSemanticInterfaces();
    for (auto &[_, module] : below.modules) module.compiledApart = true;
    // A `make` function travels with its receiver unless it was declared for a
    // type its module does not declare (KexiMethod::foreignTarget).
    const auto travelsWithReceiver = [](const kex::semantic::ImportedFunction &function) {
      return function.isReceiverMethod && !function.foreignTarget;
    };
    for (auto &[_, functions] : below.receiverFunctions)
      for (auto &function : functions)
        if (travelsWithReceiver(function)) function.visibleByReceiver = true;
    for (auto &[_, module] : below.modules)
      for (auto &[__, functions] : module.exports)
        for (auto &function : functions)
          if (travelsWithReceiver(function)) function.visibleByReceiver = true;
    // A module this build compiles is described by its own interface, not by
    // the one the prelude derives from the standard library's source for an
    // opt-in module: that one names where the module's functions would be
    // in a merged program (`make Router` in `Net.HTTP`, filed under
    // `Kex.Net.HTTP.Router`), and the merge keeps the first description.
    auto preludeView = [&]() -> kex::semantic::ImportedInterfaces {
      auto view = *interfaces;
      // A type's `make` functions are filed under the type (`Regex.Match`),
      // inside the module this build compiles (`Regex`).
      const auto builtHere = [&](std::string module) {
        for (;;) {
          if (below.modules.contains(module)) return true;
          const auto dot = module.rfind('.');
          if (dot == std::string::npos) return false;
          module.resize(dot);
        }
      };
      std::erase_if(view.modules, [&](const auto &module) { return builtHere(module.first); });
      // The prelude files its own copies under `Prelude`; a receiver of a
      // type declared in such a module (`Regex.Match`) gives them away.
      const auto onBuiltType = [&](const kex::semantic::ImportedFunction &function) {
        if (function.signature.params.empty()) return false;
        const auto *named =
            std::get_if<kex::semantic::NamedType>(&function.signature.params.front()->kind);
        return named && named->name.find('.') != std::string::npos && builtHere(named->name);
      };
      for (auto &[__, functions] : view.receiverFunctions)
        std::erase_if(functions, [&](const auto &function) {
          return builtHere(function.sourceModule) || onBuiltType(function);
        });
      return view;
    };
    // Taken before `below` is moved into the merge, which reads it.
    auto prelude = interfaces ? preludeView() : kex::semantic::ImportedInterfaces{};
    const auto available =
        interfaces ? kex::mergeSemanticInterfaces(std::move(prelude), std::move(below))
                   : std::move(below);
    // A prelude trait the unit claims gives it its default bodies, from the
    // trait's source, before the check, as the merged build backfills them:
    // the checker decides calls to them (`clock.inspected`). Taken out again
    // once lowered, since the unit's items go back to their files below.
    const auto ownItems = program.items.size();
    std::vector<const kex::ast::Program *> reachedPrograms;
    for (const auto &file : reached)
      if (const auto found = byPath.find(file); found != byPath.end() && found->second->program)
        reachedPrograms.push_back(found->second->program.get());
    kex::backfillExternalTraitDefaults(program, reachedPrograms);
    // The full check, as the merged program gets it; names are reported for
    // the entry unit, as the merged check reports them for the entry alone.
    chargeTo("interfaces");
    kex::semantic::Analyzer analyzer(&available);
    configureFullCheck(analyzer, isEntry ? g_separateNameCheckFiles : std::vector<std::string>{},
                       g_separateEntryPath);
    // The providers lowering will see, so the check counts a call's the same.
    analyzer.setLinkedModules(providerModules(unitExternalModules(registry)));
    const bool clean = analyzer.analyze(program);
    auto unitDiagnostics = analyzer.diagnostics();
    if (clean)
      for (auto &diagnostic : taggedLiteralDiagnostics(program, analyzer, g_separateEntryPath))
        unitDiagnostics.push_back(std::move(diagnostic));
    for (const auto &diagnostic : unitDiagnostics) g_separateDiagnostics.push_back(diagnostic);
    // Phase 3: the unit lowered on its own, against the prelude and the KexI
    // of the units below it, as the REPL lowers code against a loaded unit.
    const bool unitChecked =
        std::none_of(unitDiagnostics.begin(), unitDiagnostics.end(),
                     [](const auto &diagnostic) {
                       return diagnostic.level == kex::semantic::Diagnostic::Level::Error;
                     });
    chargeTo("check");
    const char *coreDir = std::getenv("KEX_SEPARATE_CORE");
    bool unitLowered = false;
    std::map<std::string, std::string> unitCoreExports;
    std::map<std::string, std::set<std::pair<std::string, int>>> unitRoutes;
    // A unit with errors is lowered all the same when the check does not
    // gate this build; it is never cached.
    const bool unitLowerable = unitChecked || !g_checkGates;
    if (!unitLowerable) allLowered = false;
    if (unitLowerable) {
      auto lowered = lowerUnitSeparately(program, isEntry ? g_separateEntryPath : unit.front(),
                                         analyzer, registry, coreDir ? coreDir : "");
      if (!lowered) allLowered = false;
      else {
        unitLowered = true;
        for (auto &module : *lowered) {
          unitCoreExports[module.moduleName] =
              module.source.substr(0, module.source.find('\n'));
          unitRoutes[module.moduleName] = module.routedImplementations;
          separateModules.push_back(std::move(module));
        }
      }
    }
    chargeTo("lower");
    program.items.erase(program.items.begin() + static_cast<std::ptrdiff_t>(ownItems),
                        program.items.end());
    if (!isEntry) {
      // What the unit's modules would say of themselves once compiled.
      kex::beam::LoadedUnit described;
      std::vector<std::string> names;
      // Nested ones too (`capability Ticker` inside `module Tickers` is
      // `Tickers.Ticker`), once each: a module can come in more than one part.
      std::function<void(const kex::ast::ModuleDef &)> describe =
          [&](const kex::ast::ModuleDef &module) {
            if (std::find(names.begin(), names.end(), module.name) == names.end())
              names.push_back(module.name);
            for (const auto &item : module.body)
              if (const auto *nested = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
                if (*nested) describe(**nested);
          };
      for (const auto &item : program.items)
        if (const auto *module = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
          if (*module) describe(**module);
      // The qualified types of the units below, which a bare name in a
      // module's signatures can mean through its `using`s.
      std::set<std::string> qualifiedTypes(available.typeNames.begin(),
                                           available.typeNames.end());
      for (const auto &[name, _] : available.recordArities) qualifiedTypes.insert(name);
      for (const auto &[name, _] : available.typeAliases) qualifiedTypes.insert(name);
      for (const auto &[name, _] : available.distinctTypes) qualifiedTypes.insert(name);
      for (std::size_t i = 0; i < names.size(); i++) {
        kex::beam::CollectOptions options;
        options.moduleAtom = kex::names::moduleAtom(names[i]);
        options.unitId = kex::names::moduleAtom(names.front());
        options.moduleName = names[i];
        options.role = i == 0 ? kex::beam::KexiModuleRole::Entry
                              : kex::beam::KexiModuleRole::Companion;
        if (i > 0) options.entryBackPointer = kex::names::moduleAtom(names.front());
        options.analysis = &analyzer;
        options.unitCompiledApart = true;
        auto chunk = kex::beam::collectMetadata(program, options);
        kex::beam::qualifyModuleTypeNames(chunk, program, names[i], qualifiedTypes);
        // A plain receiver dispatcher may only accept tagged records. A
        // distinct value is erased to its backing value, so another unit
        // must call the receiver-specific implementation when one was
        // emitted (`empty?/ThingId`, not `empty?/1` on a String).
        if (auto core = unitCoreExports.find(options.moduleAtom);
            core != unitCoreExports.end()) {
          kex::beam::recordImplementations(
              chunk, routedIn(unitRoutes[options.moduleAtom]));
          const auto emitted = kex::beam::coreExports(core->second);
          for (auto &method : chunk.typeInterface.methods)
            if (method.receiverType &&
                method.receiverType->kind == kex::beam::KexiType::Named)
              if (auto own = kex::beam::emittedImplementationOf(method, emitted);
                  !own.empty())
                method.beamFunction = std::move(own);
        }
        described.modules.push_back({options.moduleAtom, "", std::move(chunk)});
      }
      if (!described.modules.empty()) {
        described.entryBeamAtom = described.modules.front().beamAtom;
        describedUnits.push_back({std::set<std::string>(unit.begin(), unit.end()), names,
                                  std::move(described)});
        describedUnits.back().digest =
            kex::cache::SeparateUnitCache::interfaceDigest(describedUnits.back().unit);
      }
    }
    // Each module the unit emitted, described by the unit: its own interface
    // where it described one (`Kex.Bags`), otherwise collected here with the
    // unit's analysis (its file-level module, the entry's modules).
    std::vector<kex::beam::KexiChunk> attachChunks;
    for (auto i = emittedBefore; unitLowered && i < separateModules.size(); ++i) {
      const auto &module = separateModules[i];
      const kex::beam::KexiChunk *described = nullptr;
      if (describedUnits.size() > describedBefore)
        for (const auto &own : describedUnits.back().unit.modules)
          if (own.beamAtom == module.moduleName) described = &own.chunk;
      if (described) {
        attachChunks.push_back(*described);
      } else {
        kex::beam::CollectOptions options;
        options.moduleAtom = module.moduleName;
        options.analysis = &analyzer;
        if (module.moduleName.rfind("Kex.", 0) == 0)
          options.moduleName = module.moduleName.substr(4);
        auto chunk = kex::beam::collectMetadata(program, options);
        kex::beam::recordImplementations(chunk, routedIn(module.routedImplementations));
        attachChunks.push_back(std::move(chunk));
      }
      g_separateChunks[module.moduleName] = attachChunks.back();
    }
    if (cacheKey && unitChecked && unitLowered &&
        describedUnits.size() > describedBefore) {
      kex::cache::CachedSeparateUnit cached;
      cached.interface = describedUnits.back().unit;
      cached.modules.assign(separateModules.begin() +
                                static_cast<std::ptrdiff_t>(emittedBefore),
                            separateModules.end());
      cached.attachChunks = std::move(attachChunks);
      unitCache->store(*cacheKey, cached);
    }
    if (isEntry) {
      entry.items = std::move(program.items);
    } else {
      std::size_t at = 0;
      for (auto &[dep, count] : taken)
        for (std::size_t i = 0; i < count; i++)
          dep->program->items.push_back(std::move(program.items[at++]));
    }
  }
  // A separate build runs the program, whose output this would be part of.
  chargeTo("describe and store");
  if (kex::timings::enabled())
    for (const auto &[phase, ms] : unitPhaseMs)
      std::fprintf(stderr, "    separate %-20s %9.1f ms\n", phase.c_str(), ms);
  if (allLowered) g_separateModules = std::move(separateModules);
}

} // namespace

// The modules `program` uses, loaded and merged in (kex::driver). A build
// checks and lowers each unit on its own first, while each dependency is
// still its own program; `graph`, when given, receives the units.
auto resolveBeamDeps(kex::ast::Program &program,
                     const std::vector<std::string> &roots,
                     const kex::semantic::ImportedInterfaces *interfaces,
                     const std::optional<std::unordered_set<std::string>>
                         &qualifiedModules,
                     UnitGraph *graph)
    -> std::vector<LoadedDep> {
  auto dependencies =
      kex::driver::loadDependencies(program, roots, interfaces, qualifiedModules);
  if (graph && g_building)
    checkUnitsSeparately(program, dependencies.files, dependencies.units,
                         dependencies.uses, interfaces);
  kex::driver::mergeDependencies(program, dependencies, roots, interfaces);
  if (graph) {
    graph->uses = std::move(dependencies.uses);
    graph->units = dependencies.units;
  }
  return std::move(dependencies.files);
}
