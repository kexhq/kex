// The build's program and unit passes and analyses (build_ir.hxx), and the
// registries a textual pipeline is built from.
#include "build_ir.hxx"
#include "../ir/module_passes.hxx"

#include "build_support.hxx"
#include "../beam/collect_metadata.hxx"
#include "../cache/cache.hxx"
#include "../common/digest.hxx"
#include "../common/prelude_interfaces.hxx"
#include "../common/runtime_dir.hxx"
#include "../common/string_utils.hxx"
#include "../common/timings.hxx"
#include "../common/well_known.hxx"
#include "../compiled/expand.hxx"
#include "../module/discovery.hxx"
#include "../pass/options.hxx"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <tuple>

namespace kex::build {

using driver::LoadedDep;

// ── The IR units ────────────────────────────────────────────────────────────

auto Unit::sourcePath() const -> const std::string& {
    return isEntry() ? m_program->settings().entryPath : m_files.front();
}

auto Program::nameCheckFiles() const -> std::vector<std::string> {
    std::vector<std::string> files{m_settings.entryPath};
    if (!specBaseFile.empty()) files.push_back(specBaseFile);
    return files;
}

auto Program::dependencyFile(const std::string& path) const -> LoadedDep* {
    for (auto& file : const_cast<std::vector<LoadedDep>&>(dependencies.files))
        if (*file.path == path) return &file;
    return nullptr;
}

auto Program::diagnostics() const -> std::vector<semantic::Diagnostic> {
    std::vector<semantic::Diagnostic> all;
    for (const auto& unit : units)
        all.insert(all.end(), unit->diagnostics.begin(), unit->diagnostics.end());
    return all;
}

auto Program::allLowered() const -> bool {
    return std::all_of(units.begin(), units.end(), [](const auto& unit) { return unit->lowered; });
}

auto Program::takeModules() -> std::vector<ir::EmitResult> {
    std::vector<ir::EmitResult> modules;
    for (auto& unit : units)
        for (auto& module : unit->emitted) modules.push_back(std::move(module));
    return modules;
}

auto Program::chunks() const -> std::map<std::string, beam::KexiChunk> {
    std::map<std::string, beam::KexiChunk> chunks;
    for (const auto& unit : units)
        for (std::size_t i = 0; i < unit->emitted.size() && i < unit->attachChunks.size(); ++i)
            chunks[unit->emitted[i].moduleName] = unit->attachChunks[i];
    return chunks;
}

auto unitName(const Program& program) -> std::string {
    return module::shownPath(program.settings().entryPath);
}
auto printUnit(std::ostream& out, const Program& program) -> void {
    out << program.units.size() << " units, " << program.dependencies.files.size()
        << " dependency files";
}
auto isAnswered(const Program& program) -> bool { return program.answered; }

auto unitName(const Unit& unit) -> std::string {
    return unit.isEntry() ? module::shownPath(unit.sourcePath()) + " (entry)"
                          : module::shownPath(unit.files().front());
}
auto printUnit(std::ostream& out, const Unit& unit) -> void {
    for (const auto& module : unit.emitted) out << module.source << "\n";
    if (unit.emitted.empty())
        for (const auto& module : unit.modules) ir::printUnit(out, module);
}
auto isAnswered(const Unit& unit) -> bool { return unit.answered; }

// ── Unit analyses ───────────────────────────────────────────────────────────

auto ReachAnalysis::run(Unit& unit, UnitAnalysisManager&) -> Result {
    const auto& uses = unit.program().dependencies.uses;
    Result reached;
    std::vector<std::string> pending(unit.files().begin(), unit.files().end());
    while (!pending.empty()) {
        const auto file = pending.back();
        pending.pop_back();
        if (const auto used = uses.find(file); used != uses.end())
            for (const auto& next : used->second)
                if (reached.insert(next).second) pending.push_back(next);
    }
    return reached;
}

auto BelowAnalysis::run(Unit& unit, UnitAnalysisManager& analyses) -> Result {
    const auto& reached = analyses.result<ReachAnalysis>(unit);
    Result below;
    module::PackageMetadata build;
    build.id = "kex.build";
    for (const auto& described : unit.program().described) {
        if (std::none_of(described.files.begin(), described.files.end(),
                         [&](const std::string& file) { return reached.contains(file); }))
            continue;
        build.unitIds.push_back(described.unit.modules.front().chunk.metadata.unitId);
        // Once each: a module can be split across files of different units.
        for (const auto& name : described.names)
            if (std::find(build.receiverProviders.begin(), build.receiverProviders.end(), name) ==
                build.receiverProviders.end())
                build.receiverProviders.push_back(name);
        below.registry.registerUnit(described.unit);
        below.digests.push_back(described.digest);
    }
    // The build's own units as one package, every module of them a provider:
    // a type's `make` functions reach the units above it only through a
    // package that says so, as the prelude's do.
    if (!build.unitIds.empty())
        for (const auto& error : below.registry.declarePackage(build))
            std::fprintf(stderr, "separate: package: %s\n", error.message.c_str());
    return below;
}

auto AvailableInterfacesAnalysis::run(Unit& unit, UnitAnalysisManager& analyses) -> Result {
    const auto* interfaces = unit.program().settings().interfaces;
    auto below = analyses.result<BelowAnalysis>(unit).registry.buildSemanticInterfaces();
    for (auto& [_, module] : below.modules) module.compiledApart = true;
    // A `make` function travels with its receiver unless it was declared for
    // a type its module does not declare (KexiMethod::foreignTarget).
    const auto travelsWithReceiver = [](const semantic::ImportedFunction& function) {
        return function.isReceiverMethod && !function.foreignTarget;
    };
    for (auto& [_, functions] : below.receiverFunctions)
        for (auto& function : functions)
            if (travelsWithReceiver(function)) function.visibleByReceiver = true;
    for (auto& [_, module] : below.modules)
        for (auto& [__, functions] : module.exports)
            for (auto& function : functions)
                if (travelsWithReceiver(function)) function.visibleByReceiver = true;
    if (!interfaces) return below;
    // A module this build compiles is described by its own interface, not by
    // the one the prelude derives from the standard library's source for an
    // opt-in module: that one names where the module's functions would be in
    // a merged program (`make Router` in `Net.HTTP`, filed under
    // `Kex.Net.HTTP.Router`), and the merge keeps the first description.
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
    std::erase_if(view.modules, [&](const auto& module) { return builtHere(module.first); });
    // The prelude files its own copies under `Prelude`; a receiver of a type
    // declared in such a module (`Regex.Match`) gives them away.
    const auto onBuiltType = [&](const semantic::ImportedFunction& function) {
        if (function.signature.params.empty()) return false;
        const auto* named =
            std::get_if<semantic::NamedType>(&function.signature.params.front()->kind);
        return named && named->name.find('.') != std::string::npos && builtHere(named->name);
    };
    for (auto& [__, functions] : view.receiverFunctions)
        std::erase_if(functions, [&](const auto& function) {
            return builtHere(function.sourceModule) || onBuiltType(function);
        });
    return mergeSemanticInterfaces(std::move(view), std::move(below));
}

auto unitExternalModules(const beam::KexiRegistry& registry) -> ir::ExternalModules {
    // A module this build compiles is described by its own interface. The
    // prebuilt runtime has its own copy of an opt-in standard library module,
    // laid out as the prelude build lays it out (`make Router` filed under
    // `Kex.Net.HTTP.Router`, not `Kex.Net.HTTP`), and left in, it wins.
    std::set<std::string> builtModules, builtAtoms;
    for (const auto* module : registry.allLoadedModules()) {
        builtModules.insert(module->chunk.metadata.sourceModule);
        builtAtoms.insert(module->beamAtom);
    }
    auto prelude = preludeExternalModules();
    const auto builtKey = [&](const std::string& key) {
        const auto dot = key.rfind('.');
        return dot != std::string::npos && builtModules.contains(key.substr(0, dot));
    };
    std::erase_if(prelude.exportToBeamFn, [&](const auto& entry) { return builtKey(entry.first); });
    std::erase_if(prelude.exportArity, [&](const auto& entry) { return builtKey(entry.first); });
    std::erase_if(prelude.exportParamNames, [&](const auto& entry) { return builtKey(entry.first); });
    std::erase_if(prelude.foulExports, [&](const auto& key) { return builtKey(key); });
    for (auto& [_, functions] : prelude.receiverFunctions)
        std::erase_if(functions, [&](const auto& function) {
            return builtAtoms.contains(function.moduleAtom);
        });
    auto unitExternals = registry.buildExternalModules();
    for (auto& [_, functions] : unitExternals.receiverFunctions)
        for (auto& function : functions) function.compiledApart = true;
    for (const auto& [name, _] : unitExternals.nameToAtom)
        unitExternals.compiledApartModules.insert(name);
    auto externals = ir::mergeExternalModules(prelude, unitExternals);
    // The merge lets a unit's providers of a name replace the prelude's, as a
    // module loaded into the REPL does; here both stand, or `handle.feed` lost
    // the prelude's foul FileHandle provider to FS.File's, and its context.
    for (const auto& [name, providers] : prelude.receiverFunctions) {
        auto& merged = externals.receiverFunctions[name];
        for (const auto& provider : providers)
            if (std::none_of(merged.begin(), merged.end(), [&](const auto& existing) {
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

auto LinkAnalysis::run(Unit& unit, UnitAnalysisManager& analyses) -> Result {
    const auto& registry = analyses.result<BelowAnalysis>(unit).registry;
    Result link{unitExternalModules(registry), loadPreludeRecordLayouts(),
                loadPreludeVariantTags()};
    // Where each type's `make` functions are, for the runtime's dispatch on a
    // value: its declaring module may have none, and another unit may. A
    // `make` over a union (`make TypeRef | PatternRef`) owns each member.
    std::map<std::string, std::string> methodOwners;
    for (const auto* module : registry.allLoadedModules())
        for (const auto& method : module->chunk.typeInterface.methods) {
            if (method.moduleFunction || method.typeOnly || !method.receiverType) continue;
            const auto& receiver = *method.receiverType;
            if (receiver.kind == beam::KexiType::Named)
                methodOwners.try_emplace(receiver.name, module->beamAtom);
            else if (receiver.kind == beam::KexiType::Union)
                for (const auto& member : receiver.typeArgs)
                    if (member && member->kind == beam::KexiType::Named)
                        methodOwners.try_emplace(member->name, module->beamAtom);
        }
    const auto ownerOf = [&](const std::string& type) {
        const auto found = methodOwners.find(type);
        return found == methodOwners.end() ? std::string{} : found->second;
    };
    // A type of a unit below that claims a trait has the trait's defaults
    // (`Foldable`'s `any?` on `Data.Set`) in the module holding its `make`
    // functions, which forwards them to where its unit emitted them: receiver
    // functions for that type, at the arities of the prelude's generic ones.
    std::map<std::string, std::vector<std::string>> traitDefaults;
    for (const auto& trait : ::preludeSemanticInterfaces().traits)
        traitDefaults[trait.name] = trait.defaultMethods;
    for (const auto& trait : registry.buildSemanticInterfaces().traits)
        traitDefaults[trait.name] = trait.defaultMethods;
    for (const auto* module : registry.allLoadedModules())
        for (const auto& conformance : module->chunk.metadata.traitConformances) {
            const auto owner = ownerOf(conformance.typeName);
            const auto defaults = traitDefaults.find(conformance.traitName);
            if (owner.empty() || defaults == traitDefaults.end()) continue;
            for (const auto& name : defaults->second) {
                auto& providers = link.externals.receiverFunctions[name];
                std::set<int> arities;
                for (const auto& provider : providers)
                    if (provider.moduleAtom == names::kPreludeModule && provider.receiverType.empty())
                        arities.insert(provider.beamArity);
                for (const auto arity : arities)
                    if (std::none_of(providers.begin(), providers.end(), [&](const auto& provider) {
                            return provider.moduleAtom == owner && provider.beamArity == arity &&
                                   provider.receiverType == conformance.typeName;
                        }))
                        providers.push_back(
                            {owner, name, arity, {}, conformance.typeName, false, true});
            }
        }
    for (const auto* module : registry.allLoadedModules()) {
        for (const auto& record : module->chunk.metadata.records) {
            ir::ExternalRecordLayout layout;
            layout.name = record.tagAtom.empty() ? record.name : record.tagAtom;
            layout.moduleAtom = module->beamAtom;
            layout.methodOwner = ownerOf(layout.name);
            layout.declaringModule = module->chunk.metadata.sourceModule;
            for (const auto& field : record.fields) {
                layout.fields.push_back(field.name);
                layout.fieldDefaults.push_back(field.defaultSource);
            }
            link.layouts.push_back(std::move(layout));
        }
        for (const auto& adt : module->chunk.metadata.adts)
            for (const auto& constructor : adt.constructors)
                link.variants.push_back(
                    {constructor.tagAtom, constructor.arity, adt.name, ownerOf(adt.name)});
    }
    return link;
}

auto UnitCacheKeyAnalysis::run(Unit& unit, UnitAnalysisManager& analyses) -> Result {
    auto& program = unit.program();
    if (unit.isEntry() || !program.unitCache) return std::nullopt;
    std::vector<cache::SeparateUnitCache::Source> sources;
    for (const auto& path : unit.files()) {
        const auto* file = program.dependencyFile(path);
        if (!file) return std::nullopt;
        // A file that reads others while it compiles is not keyed by its own
        // bytes alone.
        const auto& source = *file->source;
        if (sourceMentionsCompiledBlock(source) || sourceMentionsEmbed(source) ||
            source.find("Template") != std::string::npos)
            return std::nullopt;
        sources.push_back({path, source});
    }
    const auto runtime = prebuiltRuntimeBeamDir();
    return program.unitCache->keyFor(
        cache::toolchainFingerprint(),
        "runtime:" + runtime + ":" + compiled::fileStamp(runtime + "/kex_prelude.beam"),
        std::move(sources), analyses.result<BelowAnalysis>(unit).digests);
}

auto HirAnalysis::run(Unit& unit, UnitAnalysisManager& analyses) -> Result {
    auto& program = unit.program();
    const auto& available = analyses.result<AvailableInterfacesAnalysis>(unit);
    Result checked;
    checked.analyzer = std::make_unique<semantic::Analyzer>(&available);
    // The full check, as the merged program gets it; names are reported for
    // the entry unit alone.
    configureFullCheck(*checked.analyzer,
                       unit.isEntry() ? program.nameCheckFiles() : std::vector<std::string>{},
                       program.settings().entryPath);
    // The providers lowering will see, so the check counts a call's the same.
    checked.analyzer->setLinkedModules(
        providerModules(unitExternalModules(analyses.result<BelowAnalysis>(unit).registry)));
    checked.clean = checked.analyzer->analyze(unit.ast);
    checked.diagnostics = checked.analyzer->diagnostics();
    if (checked.clean)
        for (auto& diagnostic :
             taggedLiteralDiagnostics(unit.ast, *checked.analyzer, program.settings().entryPath))
            checked.diagnostics.push_back(std::move(diagnostic));
    checked.hir = std::make_unique<hir::Program>(unit.ast, *checked.analyzer);
    return checked;
}

// ── Unit passes ─────────────────────────────────────────────────────────────

namespace {

// Each module written to KEX_SEPARATE_CORE=<dir>, to compare builds.
auto writeSeparateCore(const std::vector<ir::EmitResult>& modules) -> void {
    const char* dir = std::getenv("KEX_SEPARATE_CORE");
    if (!dir || !*dir) return;
    std::filesystem::create_directories(dir);
    for (const auto& module : modules)
        std::ofstream(std::string(dir) + "/" + module.moduleName + ".core") << module.source;
}

auto preservesAll() -> pass::PreservedAnalyses { return pass::PreservedAnalyses::all(); }

// What a pass that changed only the unit's IR modules preserves: every unit
// analysis, since each is of the unit's source program and none reads them.
auto changedModulesOnly() -> pass::PreservedAnalyses { return pass::PreservedAnalyses::all(); }

// A forwarding function `name/arity` calling `target` in `module` with its
// arguments (`module` empty: a local call).
auto forwardingFunction(const std::string& name, int arity, const std::string& module,
                        const std::string& target) -> ir::FunDef {
    ir::FunClause clause;
    std::vector<ir::ExprPtr> args;
    for (int i = 0; i < arity; i++) {
        const auto param = "_Arg" + std::to_string(i);
        auto pattern = std::make_unique<ir::Pattern>();
        pattern->kind = ir::PatKind::Var;
        pattern->name = param;
        clause.params.push_back(std::move(pattern));
        args.push_back(std::make_unique<ir::Expr>());
        args.back()->node = ir::Var{param};
    }
    clause.body = std::make_unique<ir::Expr>();
    clause.body->node = ir::Call{module, target, arity, std::move(args), true};
    ir::FunDef forward;
    forward.name = name;
    forward.arity = arity;
    forward.clauses.push_back(std::move(clause));
    return forward;
}

auto definesFunction(const ir::Module& module, const std::string& name, int arity) -> bool {
    return std::any_of(module.functions.begin(), module.functions.end(), [&](const auto& function) {
        return function.name == name && function.arity == arity;
    });
}

} // namespace

auto UnitCachePass::run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses {
    const auto& key = analyses.result<UnitCacheKeyAnalysis>(unit);
    if (!key) return preservesAll();
    auto cached = unit.program().unitCache->load(*key);
    if (!cached) return preservesAll();
    writeSeparateCore(cached->modules);
    unit.emitted = std::move(cached->modules);
    unit.attachChunks = std::move(cached->attachChunks);
    DescribedUnit described{std::set<std::string>(unit.files().begin(), unit.files().end()), {},
                            std::move(cached->interface), {}};
    for (const auto& module : described.unit.modules)
        described.names.push_back(module.chunk.metadata.sourceModule);
    described.digest = cache::SeparateUnitCache::interfaceDigest(described.unit);
    unit.program().described.push_back(described);
    unit.described = std::move(described);
    unit.checked = unit.lowered = unit.answered = true;
    if (timings::enabled()) std::fprintf(stderr, "  unit cache hit: %s\n", unit.files().front().c_str());
    return preservesAll();
}

auto AssemblePass::run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses {
    auto& program = unit.program();
    if (unit.isEntry()) {
        unit.ast.items = std::move(program.entry().items);
    } else {
        for (const auto& path : unit.files()) {
            auto* file = program.dependencyFile(path);
            if (!file) continue;
            auto& items = file->program->items;
            unit.taken.push_back({file, items.size()});
            for (auto& item : items) unit.ast.items.push_back(std::move(item));
            items.clear();
        }
    }
    unit.ownItems = unit.ast.items.size();
    unit.assembled = true;
    // A prelude trait the unit claims gives it its default bodies, from the
    // trait's source, before the check, as the merged build backfills them:
    // the checker decides calls to them (`clock.inspected`). Taken out again
    // by `return-items`.
    std::vector<const ast::Program*> reachedPrograms;
    for (const auto& file : analyses.result<ReachAnalysis>(unit))
        if (const auto* loaded = program.dependencyFile(file); loaded && loaded->program)
            reachedPrograms.push_back(loaded->program.get());
    backfillExternalTraitDefaults(unit.ast, reachedPrograms);
    // The unit's own program changed: what was computed from it is not.
    return pass::PreservedAnalyses::none()
        .preserve<ReachAnalysis>()
        .preserve<BelowAnalysis>()
        .preserve<AvailableInterfacesAnalysis>()
        .preserve<LinkAnalysis>()
        .preserve<UnitCacheKeyAnalysis>();
}

auto CheckPass::run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses {
    const auto& checked = analyses.result<HirAnalysis>(unit);
    unit.diagnostics = checked.diagnostics;
    unit.checked = std::none_of(unit.diagnostics.begin(), unit.diagnostics.end(),
                                [](const auto& diagnostic) {
                                    return diagnostic.level == semantic::Diagnostic::Level::Error;
                                });
    return preservesAll();
}

auto LowerPass::run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses {
    auto& program = unit.program();
    // A unit with errors is lowered all the same when the check does not
    // gate this build; it is never cached.
    if (!unit.checked && program.settings().checkGates) return preservesAll();
    const auto& path = unit.sourcePath();
    // Several units can have the same basename (`app/models/conversation.kex`
    // and `app/views/chat/conversation.kex`). Their file modules must not
    // overwrite one another. Keep the entry's traditional `kex_main` name.
    auto stem = std::filesystem::path(path).stem().string();
    if (!unit.isEntry()) stem += "_" + sha256Hex(path).substr(0, 16);
    const auto& link = analyses.result<LinkAnalysis>(unit);
    const auto& checked = analyses.result<HirAnalysis>(unit);
    try {
        unit.modules = ir::lowerModules(unit.ast, stem,
                                        {.sourcePath = path,
                                         .externals = &link.externals,
                                         .externalRecords = &link.layouts,
                                         .externalVariants = &link.variants,
                                         .hir = checked.hir.get()});
        unit.fileModule = "kex_" + stem;
        unit.lowered = true;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s: %s\n", module::shownPath(path).c_str(), error.what());
    }
    return preservesAll();
}

// The unit's own module registers its records' layouts as it loads, but only
// the entry's `main` used to run, and a call goes to a companion
// (`Kex.RecordValue.Sessions`): its loading loads that module first, or
// `Type.of(Session)` met a record nothing had registered.
auto LoadOrderPass::run(Unit& unit, UnitAnalysisManager&) -> pass::PreservedAnalyses {
    auto& modules = unit.modules;
    const auto& fileModule = unit.fileModule;
    const auto registers = std::any_of(modules.begin(), modules.end(), [&](const auto& module) {
        return module.name == fileModule && !module.onLoad.empty();
    });
    if (!registers) return preservesAll();
    for (auto& module : modules) {
        if (module.name == fileModule || !module.onLoad.empty()) continue;
        auto load = std::make_unique<ir::Expr>();
        std::vector<ir::ExprPtr> args;
        args.push_back(std::make_unique<ir::Expr>());
        args.back()->node = ir::Lit{ir::LitKind::Atom, fileModule};
        load->node = ir::Call{"code", "ensure_loaded", 1, std::move(args), false};
        auto ok = std::make_unique<ir::Expr>();
        ok->node = ir::Lit{ir::LitKind::Atom, "ok"};
        auto body = std::make_unique<ir::Expr>();
        body->node = ir::Let{"_Loaded", std::move(load), std::move(ok)};
        ir::FunDef onLoad;
        onLoad.name = "__kex_on_load";
        onLoad.exported = false;
        ir::FunClause clause;
        clause.body = std::move(body);
        onLoad.clauses.push_back(std::move(clause));
        module.functions.push_back(std::move(onLoad));
        module.onLoad = "__kex_on_load";
    }
    return changedModulesOnly();
}

// What the unit's interface names in a module's companion and lowering
// emitted in the unit's own module, the companion forwards there:
//   * a free function sharing its name with a `make` function of the same
//     module (`Kex.Vault:get` is `kex_vault:'Vault.get$free'`);
//   * the functions of a `make` block written outside the module for one of
//     its types (`make Type.Field` after `module Type`);
//   * a trait's default functions (`Foldable`'s `any?`), dispatching over the
//     unit's types that claim it, from every companion: the runtime asks the
//     module holding a type's `make` functions (`Kex.Data` for `Data.Set`).
auto CompanionForwardsPass::run(Unit& unit, UnitAnalysisManager&) -> pass::PreservedAnalyses {
    auto& modules = unit.modules;
    const auto& fileModule = unit.fileModule;
    std::set<std::string> traitDefaults;
    for (const auto& item : unit.ast.items)
        if (const auto* trait = std::get_if<std::unique_ptr<ast::TraitDef>>(&item); trait && *trait)
            for (const auto& member : (*trait)->body)
                if (const auto* function = std::get_if<std::unique_ptr<ast::FunctionDef>>(&member);
                    function && *function)
                    traitDefaults.insert((*function)->name);
    std::map<std::string, std::set<std::string>> outsideMakes;
    for (const auto& item : unit.ast.items) {
        const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&item);
        if (!make || !*make || !(*make)->target) continue;
        const auto* target = std::get_if<ast::TypeName>(&(*make)->target->kind);
        if (!target || target->parts.size() < 2) continue;
        const auto owner = join(target->parts, ".", target->parts.size() - 1);
        const auto note = [&](const ast::FunctionDef& function) {
            outsideMakes[names::moduleAtom(owner)].insert(function.name);
        };
        for (const auto& member : (*make)->body)
            if (const auto* function = std::get_if<std::unique_ptr<ast::FunctionDef>>(&member);
                function && *function)
                note(**function);
            else if (const auto* block = std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&member);
                     block && *block && (*block)->isPublic)
                for (const auto& inner : (*block)->items)
                    if (const auto* function =
                            std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner);
                        function && *function)
                        note(**function);
    }
    std::vector<std::tuple<std::string, std::string, std::string, int>> forwards;
    for (const auto& module : modules) {
        if (module.name != fileModule) continue;
        for (const auto& function : module.functions) {
            for (const auto& [companion, functionNames] : outsideMakes)
                if (functionNames.contains(function.name) && function.exported)
                    forwards.emplace_back(companion, function.name, function.name, function.arity);
            if (traitDefaults.contains(function.name) && function.exported)
                for (const auto& companion : modules)
                    if (companion.name != fileModule)
                        forwards.emplace_back(companion.name, function.name, function.name,
                                              function.arity);
            const auto dot = function.name.rfind('.');
            if (dot == std::string::npos || !function.name.ends_with("$free")) continue;
            forwards.emplace_back(names::moduleAtom(function.name.substr(0, dot)),
                                  function.name.substr(dot + 1, function.name.size() - dot - 6),
                                  function.name, function.arity);
        }
    }
    for (const auto& [companion, name, target, arity] : forwards)
        for (auto& module : modules) {
            if (module.name != companion || definesFunction(module, name, arity)) continue;
            module.functions.push_back(forwardingFunction(name, arity, fileModule, target));
        }
    return changedModulesOnly();
}

// KexI advertises a receiver method at its plain name. A unit can emit only
// the receiver-qualified implementation when an imported field or method
// shares that name (`Grant.teamId`, `OrganizationId.empty?`). The companion
// gets the advertised entry point when it has one local owner and no
// dispatcher of its own.
auto ReceiverForwardsPass::run(Unit& unit, UnitAnalysisManager&) -> pass::PreservedAnalyses {
    std::map<std::string, std::set<std::string>> ownTypes;
    std::function<void(const ast::ModuleDef&)> noteTypes = [&](const ast::ModuleDef& definition) {
        auto& types = ownTypes[names::moduleAtom(definition.name)];
        for (const auto& item : definition.body) {
            std::string name;
            if (const auto* record = std::get_if<std::unique_ptr<ast::RecordDef>>(&item);
                record && *record)
                name = (*record)->name;
            if (const auto* type = std::get_if<std::unique_ptr<ast::TypeDef>>(&item); type && *type)
                name = (*type)->name;
            if (!name.empty()) {
                types.insert(name);
                types.insert(definition.name + "." + name);
            }
            if (const auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
                nested && *nested)
                noteTypes(**nested);
        }
    };
    for (const auto& item : unit.ast.items)
        if (const auto* definition = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
            definition && *definition)
            noteTypes(**definition);
    for (auto& module : unit.modules) {
        std::map<std::pair<std::string, int>, std::vector<std::string>> implementations;
        for (const auto& function : module.functions) {
            if (!function.exported) continue;
            const auto slash = function.name.find('/');
            if (slash == std::string::npos) continue;
            const auto owner = function.name.substr(slash + 1);
            if (!ownTypes[module.name].contains(owner)) continue;
            implementations[{function.name.substr(0, slash), function.arity}].push_back(
                function.name);
        }
        for (const auto& [key, targets] : implementations) {
            if (targets.size() != 1 || definesFunction(module, key.first, key.second)) continue;
            module.functions.push_back(
                forwardingFunction(key.first, key.second, "", targets.front()));
        }
    }
    return changedModulesOnly();
}

auto EmitCorePass::run(Unit& unit, UnitAnalysisManager&) -> pass::PreservedAnalyses {
    if (!unit.lowered) return preservesAll();
    unit.emitted.clear();
    for (const auto& module : unit.modules) unit.emitted.push_back(ir::emitCore(module));
    writeSeparateCore(unit.emitted);
    return preservesAll();
}

auto DescribePass::run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses {
    const auto& checked = analyses.result<HirAnalysis>(unit);
    const auto& available = analyses.result<AvailableInterfacesAnalysis>(unit);
    const auto& program = unit.ast;
    if (!unit.isEntry()) {
        // What the unit's modules would say of themselves once compiled.
        beam::LoadedUnit described;
        std::vector<std::string> moduleNames;
        // Nested ones too (`capability Ticker` inside `module Tickers` is
        // `Tickers.Ticker`), once each: a module can come in more than one part.
        std::function<void(const ast::ModuleDef&)> describe = [&](const ast::ModuleDef& module) {
            if (std::find(moduleNames.begin(), moduleNames.end(), module.name) == moduleNames.end())
                moduleNames.push_back(module.name);
            for (const auto& item : module.body)
                if (const auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                    if (*nested) describe(**nested);
        };
        for (const auto& item : program.items)
            if (const auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                if (*module) describe(**module);
        // The qualified types of the units below, which a bare name in a
        // module's signatures can mean through its `using`s.
        std::set<std::string> qualifiedTypes(available.typeNames.begin(), available.typeNames.end());
        for (const auto& [name, _] : available.recordArities) qualifiedTypes.insert(name);
        for (const auto& [name, _] : available.typeAliases) qualifiedTypes.insert(name);
        for (const auto& [name, _] : available.distinctTypes) qualifiedTypes.insert(name);
        std::map<std::string, const ir::EmitResult*> emittedByName;
        for (const auto& module : unit.emitted) emittedByName[module.moduleName] = &module;
        for (std::size_t i = 0; i < moduleNames.size(); i++) {
            beam::CollectOptions options;
            options.moduleAtom = names::moduleAtom(moduleNames[i]);
            options.unitId = names::moduleAtom(moduleNames.front());
            options.moduleName = moduleNames[i];
            options.role = i == 0 ? beam::KexiModuleRole::Entry : beam::KexiModuleRole::Companion;
            if (i > 0) options.entryBackPointer = names::moduleAtom(moduleNames.front());
            options.analysis = checked.analyzer.get();
            options.unitCompiledApart = true;
            auto chunk = beam::collectMetadata(program, options);
            beam::qualifyModuleTypeNames(chunk, program, moduleNames[i], qualifiedTypes);
            // A plain receiver dispatcher may only accept tagged records. A
            // distinct value is erased to its backing value, so another unit
            // must call the receiver-specific implementation when one was
            // emitted (`empty?/ThingId`, not `empty?/1` on a String).
            if (const auto emitted = emittedByName.find(options.moduleAtom);
                emitted != emittedByName.end()) {
                const auto& module = *emitted->second;
                beam::recordImplementations(chunk, routedIn(module.routedImplementations));
                // coreExports reads its source for as long as it is used.
                const auto header = module.source.substr(0, module.source.find('\n'));
                const auto exports = beam::coreExports(header);
                for (auto& method : chunk.typeInterface.methods)
                    if (method.receiverType && method.receiverType->kind == beam::KexiType::Named)
                        if (auto own = beam::emittedImplementationOf(method, exports); !own.empty())
                            method.beamFunction = std::move(own);
            }
            described.modules.push_back({options.moduleAtom, "", std::move(chunk)});
        }
        if (!described.modules.empty()) {
            described.entryBeamAtom = described.modules.front().beamAtom;
            DescribedUnit own{std::set<std::string>(unit.files().begin(), unit.files().end()),
                              moduleNames, std::move(described), {}};
            own.digest = cache::SeparateUnitCache::interfaceDigest(own.unit);
            unit.program().described.push_back(own);
            unit.described = std::move(own);
        }
    }
    // Each module the unit emitted, described by the unit: its own interface
    // where it described one (`Kex.Bags`), otherwise collected here with the
    // unit's analysis (its file-level module, the entry's modules).
    unit.attachChunks.clear();
    for (const auto& module : unit.emitted) {
        const beam::KexiChunk* own = nullptr;
        if (unit.described)
            for (const auto& described : unit.described->unit.modules)
                if (described.beamAtom == module.moduleName) own = &described.chunk;
        if (own) {
            unit.attachChunks.push_back(*own);
            continue;
        }
        beam::CollectOptions options;
        options.moduleAtom = module.moduleName;
        options.analysis = checked.analyzer.get();
        if (module.moduleName.rfind("Kex.", 0) == 0) options.moduleName = module.moduleName.substr(4);
        auto chunk = beam::collectMetadata(program, options);
        beam::recordImplementations(chunk, routedIn(module.routedImplementations));
        unit.attachChunks.push_back(std::move(chunk));
    }
    return preservesAll();
}

auto StoreUnitPass::run(Unit& unit, UnitAnalysisManager& analyses) -> pass::PreservedAnalyses {
    const auto& key = analyses.result<UnitCacheKeyAnalysis>(unit);
    if (!key || !unit.checked || !unit.lowered || !unit.described) return preservesAll();
    cache::CachedSeparateUnit cached;
    cached.interface = unit.described->unit;
    cached.modules = unit.emitted;
    cached.attachChunks = unit.attachChunks;
    unit.program().unitCache->store(*key, cached);
    return preservesAll();
}

auto ReturnItemsPass::run(Unit& unit, UnitAnalysisManager&) -> pass::PreservedAnalyses {
    if (!unit.assembled) return preservesAll();
    auto& items = unit.ast.items;
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(unit.ownItems), items.end());
    if (unit.isEntry()) {
        unit.program().entry().items = std::move(items);
    } else {
        std::size_t at = 0;
        for (auto& [file, count] : unit.taken)
            for (std::size_t i = 0; i < count; i++)
                file->program->items.push_back(std::move(items[at++]));
    }
    items.clear();
    unit.taken.clear();
    unit.assembled = false;
    return pass::PreservedAnalyses::none();
}

// ── Program passes ──────────────────────────────────────────────────────────

// `<name>.spec.kex` auto-loads `<name>.kex`'s declarations
// (specBaseCandidates): merged ahead of the entry's own.
auto SpecBasePass::run(Program& program, ProgramAnalysisManager&) -> pass::PreservedAnalyses {
    program.specBaseFile =
        mergeSpecBase(program.entry(), program.settings().entryPath, &program.specBaseRawSource);
    return program.specBaseFile.empty() ? preservesAll() : pass::PreservedAnalyses::none();
}

auto LoadDependenciesPass::run(Program& program, ProgramAnalysisManager&)
    -> pass::PreservedAnalyses {
    const auto& settings = program.settings();
    program.dependencies =
        driver::loadDependencies(program.entry(), settings.roots, settings.interfaces,
                                 driver::sourceModulesNamedBy(program.entry()));
    program.units.clear();
    for (const auto& files : program.dependencies.units)
        program.units.push_back(std::make_unique<Unit>(program, files));
    if (auto root = cache::base()) program.unitCache.emplace(*root);
    return pass::PreservedAnalyses::none();
}

auto RunCachePass::run(Program& program, ProgramAnalysisManager&) -> pass::PreservedAnalyses {
    if (program.answerFromRunCache && program.answerFromRunCache(program)) program.answered = true;
    return preservesAll();
}

auto MergeDependenciesPass::run(Program& program, ProgramAnalysisManager&)
    -> pass::PreservedAnalyses {
    const auto& settings = program.settings();
    driver::mergeDependencies(program.entry(), program.dependencies, settings.roots,
                              settings.interfaces);
    return pass::PreservedAnalyses::none();
}

// ── Registries and pipelines ────────────────────────────────────────────────

auto unitPassRegistry() -> const pass::PassRegistry<Unit>& {
    static const auto registry = [] {
        pass::PassRegistry<Unit> passes;
        passes.registerPass<UnitCachePass>();
        passes.registerPass<AssemblePass>();
        passes.registerPass<CheckPass>();
        passes.registerPass<LowerPass>();
        passes.registerPass<LoadOrderPass>();
        passes.registerPass<CompanionForwardsPass>();
        passes.registerPass<ReceiverForwardsPass>();
        passes.registerPass<EmitCorePass>();
        passes.registerPass<DescribePass>();
        passes.registerPass<StoreUnitPass>();
        passes.registerPass<ReturnItemsPass>();
        pass::registerNestedLevel(passes, ir::modulePassRegistry(),
                                  "a module pipeline over each module the unit lowered");
        return passes;
    }();
    return registry;
}

auto programPassRegistry() -> const pass::PassRegistry<Program>& {
    static const auto registry = [] {
        pass::PassRegistry<Program> passes;
        passes.registerPass<SpecBasePass>();
        passes.registerPass<LoadDependenciesPass>();
        passes.registerPass<RunCachePass>();
        passes.registerPass<MergeDependenciesPass>();
        pass::registerNestedLevel(passes, unitPassRegistry(),
                                  "a unit pipeline over each unit, in build order");
        return passes;
    }();
    return registry;
}

auto defaultProgramPipeline() -> std::string {
    const auto modules =
        pass::pipelineText(pass::IRUnitLevel<ir::Module>::kName, ir::kDefaultModulePipeline);
    return "spec-base,load-dependencies,run-cache,"
           "unit(unit-cache,assemble,check,lower,load-order,companion-forwards,receiver-forwards,"
           "module(" + modules + "),emit-core,describe,store-unit,return-items),"
           "merge-dependencies";
}

auto buildProgramPipeline() -> ProgramPassManager {
    ProgramPassManager pipeline;
    const auto text =
        pass::pipelineText(pass::IRUnitLevel<Program>::kName, defaultProgramPipeline());
    if (auto problem = programPassRegistry().build(pipeline, text))
        throw std::invalid_argument(*problem);
    return pipeline;
}

auto printPasses(std::ostream& out) -> void {
    out << "program passes (kex -c, --emit-core):\n";
    programPassRegistry().print(out);
    out << "unit passes (each unit, in build order):\n";
    unitPassRegistry().print(out);
    out << "module passes (each emitted module):\n";
    ir::modulePassRegistry().print(out);
    out << "default pipeline:\n  " << defaultProgramPipeline() << "\n";
}

auto runBuild(Program& program, ProgramPassManager& pipeline,
              const pass::PassInstrumentationCallbacks& callbacks) -> bool {
    ProgramAnalysisManager programAnalyses(&callbacks);
    UnitAnalysisManager unitAnalyses(&callbacks);
    ir::ModuleAnalysisManager moduleAnalyses(&callbacks);
    pass::crossRegisterProxies(programAnalyses, unitAnalyses);
    pass::crossRegisterProxies(unitAnalyses, moduleAnalyses);
    unitAnalyses.registerAnalysis<ReachAnalysis>();
    unitAnalyses.registerAnalysis<BelowAnalysis>();
    unitAnalyses.registerAnalysis<AvailableInterfacesAnalysis>();
    unitAnalyses.registerAnalysis<LinkAnalysis>();
    unitAnalyses.registerAnalysis<UnitCacheKeyAnalysis>();
    unitAnalyses.registerAnalysis<HirAnalysis>();
    try {
        pipeline.run(program, programAnalyses);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return false;
    }
    return true;
}

} // namespace kex::build
