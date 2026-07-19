#include "kexi_registry.hxx"
#include "beam_file.hxx"
#include <filesystem>
#include <optional>
#include <unordered_set>

namespace kex::beam {

namespace {

auto typeNameStr(const KexiTypePtr& t) -> std::string {
    if (!t) return "";
    switch (t->kind) {
    case KexiType::Primitive: return t->name;
    case KexiType::Named:     return t->name;
    default:                  return "";
    }
}

// Single-letter generic names in KexI signatures (`X`, `K`, `V`) map to
// sequential negative TypeVar ids (-1, -2, ...) in order of first appearance
// within one signature — the same convention the hand-written stdlib table
// uses, so diagnostics render them as A, B, ... and per-signature
// substitution keeps working.
using TypeVarMap = std::unordered_map<std::string, int>;

auto semanticType(const KexiTypePtr& type, TypeVarMap& vars) -> kex::semantic::TypePtr {
    using kex::semantic::Type;
    if (!type) return Type::unknown();
    auto convertAll = [&vars](const std::vector<KexiTypePtr>& types) {
        std::vector<kex::semantic::TypePtr> converted;
        for (const auto& item : types) converted.push_back(semanticType(item, vars));
        return converted;
    };
    switch (type->kind) {
    case KexiType::Primitive:
        if (type->name == "Integer") return Type::integer();
        if (type->name == "Int") return Type::int64();
        if (type->name == "Byte" || type->name == "UInt8") return Type::uint8();
        if (type->name == "Int8") return Type::int8();
        if (type->name == "Int16") return Type::int16();
        if (type->name == "Int32") return Type::int32();
        if (type->name == "Int64") return Type::int64();
        if (type->name == "UInt16") return Type::uint16();
        if (type->name == "UInt32") return Type::uint32();
        if (type->name == "UInt64") return Type::uint64();
        if (type->name == "Char") return Type::charT();
        if (type->name == "String") return Type::string();
        if (type->name == "Bool") return Type::boolean();
        if (type->name == "Atom") return Type::atom();
        if (type->name == "Unit") return Type::unit();
        if (type->name == "Number") return Type::constrained("Number", "Number");
        if (type->name == "Float") return Type::constrained("Float", "Float");
        if (type->name == "Float32") return Type::float32();
        if (type->name == "Float64") return Type::float64();
        return Type::named(type->name);
    case KexiType::Named:
        if (type->typeArgs.empty() && type->name.size() == 1 &&
            type->name[0] >= 'A' && type->name[0] <= 'Z') {
            auto [it, _] = vars.try_emplace(
                type->name, -static_cast<int>(vars.size() + 1));
            return Type::typeVar(it->second);
        }
        // Prelude make targets name these compound types (`make Map<K, V>`,
        // `make Optional<X>`); values of them carry the compound semantic
        // types, so normalize or receiver matching never succeeds.
        if (type->name == "Optional" && type->typeArgs.size() == 1)
            return Type::optional(semanticType(type->typeArgs[0], vars));
        if (type->name == "Map" && type->typeArgs.size() == 2)
            return Type::map(semanticType(type->typeArgs[0], vars),
                             semanticType(type->typeArgs[1], vars));
        if (type->name == "List" && type->typeArgs.size() == 1)
            return Type::list(semanticType(type->typeArgs[0], vars));
        return Type::named(type->name, convertAll(type->typeArgs));
    case KexiType::Func:
        return Type::func(convertAll(type->typeArgs), semanticType(type->result, vars));
    case KexiType::Tuple:
        return Type::tuple(convertAll(type->typeArgs));
    case KexiType::List:
        return Type::list(type->typeArgs.empty()
            ? Type::unknown() : semanticType(type->typeArgs[0], vars));
    case KexiType::Map:
        return Type::map(type->typeArgs.size() > 0
                             ? semanticType(type->typeArgs[0], vars) : Type::unknown(),
                         type->typeArgs.size() > 1
                             ? semanticType(type->typeArgs[1], vars) : Type::unknown());
    case KexiType::Optional:
        return Type::optional(type->typeArgs.empty()
            ? Type::unknown() : semanticType(type->typeArgs[0], vars));
    case KexiType::Union:
        return std::make_shared<kex::semantic::Type>(
            kex::semantic::Type{kex::semantic::UnionType{convertAll(type->typeArgs)}});
    case KexiType::Constrained:
        return Type::constrained(type->name, type->traitName);
    case KexiType::Never:
        return Type::voidType();
    case KexiType::Unknown:
        return Type::unknown();
    }
    return Type::unknown();
}

auto importedFunction(const KexiExport& exported,
                      const std::string& backendModule)
    -> kex::semantic::ImportedFunction {
    kex::semantic::ImportedFunction result;
    result.sourceName = exported.name;
    result.backendFunction = exported.beamFunction;
    result.backendModule = backendModule;
    result.backendArity = exported.beamArity;
    TypeVarMap vars;
    std::vector<kex::semantic::TypePtr> params;
    for (const auto& param : exported.paramTypes)
        params.push_back(semanticType(param, vars));
    result.signature = {exported.name, std::move(params),
                        semanticType(exported.returnType, vars), exported.isFoul};
    return result;
}

auto importedReceiverFunction(const KexiMethod& receiver,
                              const std::string& backendModule)
    -> kex::semantic::ImportedFunction {
    kex::semantic::ImportedFunction result;
    result.sourceName = receiver.name;
    result.backendFunction = receiver.beamFunction;
    result.backendModule = backendModule;
    result.backendArity = receiver.beamArity;
    TypeVarMap vars;
    auto receiverSemType = semanticType(receiver.receiverType, vars);
    std::vector<kex::semantic::TypePtr> params = {
        std::move(receiverSemType),
    };
    for (const auto& param : receiver.paramTypes)
        params.push_back(semanticType(param, vars));
    result.signature = {receiver.name, std::move(params),
                        semanticType(receiver.returnType, vars), receiver.isFoul};
    return result;
}

} // namespace

auto KexiRegistry::loadUnit(const std::string& beamPath)
    -> std::vector<LoadError> {
    namespace fs = std::filesystem;
    std::vector<LoadError> errors;

    if (!fs::exists(beamPath)) {
        errors.push_back({"file not found: " + beamPath});
        return errors;
    }

    BeamFile bf;
    try {
        bf = readBeamFile(beamPath);
    } catch (const std::exception& e) {
        errors.push_back({"cannot read BEAM file: " + std::string(e.what())});
        return errors;
    }

    auto* kexiChk = bf.findChunk(KEXI_CHUNK_ID);
    if (!kexiChk) {
        errors.push_back({"no KexI chunk found — recompile with current kex"});
        return errors;
    }

    KexiChunk entryChunk;
    try {
        entryChunk = deserializeKexi(kexiChk->data);
    } catch (const std::exception& e) {
        errors.push_back({"corrupt KexI chunk: " + std::string(e.what())});
        return errors;
    }

    if (entryChunk.metadata.role == KexiModuleRole::Companion) {
        errors.push_back({
            "this is a companion module of '" +
            entryChunk.metadata.entryBackPointer +
            "'; load the entry module instead"});
        return errors;
    }

    LoadedUnit unit;
    unit.entryBeamAtom = entryChunk.metadata.moduleAtom;
    unit.entryPath = beamPath;

    auto entryDir = fs::path(beamPath).parent_path();

    // Validate and load companions
    for (const auto& comp : entryChunk.metadata.companions) {
        auto compPath = (entryDir / comp.relativePath).string();

        if (comp.relativePath.find("..") != std::string::npos ||
            fs::path(comp.relativePath).is_absolute()) {
            errors.push_back({
                "companion path escapes entry directory: " + comp.relativePath});
            return errors;
        }

        if (!fs::exists(compPath)) {
            errors.push_back({
                "companion not found: " + comp.relativePath});
            return errors;
        }

        BeamFile compBf;
        try {
            compBf = readBeamFile(compPath);
        } catch (const std::exception& e) {
            errors.push_back({
                "cannot read companion " + comp.beamAtom + ": " + e.what()});
            return errors;
        }

        auto* compKexiChk = compBf.findChunk(KEXI_CHUNK_ID);
        if (!compKexiChk) {
            errors.push_back({
                "companion " + comp.beamAtom +
                " has no KexI chunk — recompile with current kex"});
            return errors;
        }

        KexiChunk compChunk;
        try {
            compChunk = deserializeKexi(compKexiChk->data);
        } catch (const std::exception& e) {
            errors.push_back({
                "companion " + comp.beamAtom +
                " has corrupt KexI chunk: " + e.what()});
            return errors;
        }

        if (compChunk.metadata.moduleAtom != comp.beamAtom) {
            errors.push_back({
                "companion identity mismatch: expected '" + comp.beamAtom +
                "' but found '" + compChunk.metadata.moduleAtom + "'"});
            return errors;
        }

        if (compChunk.metadata.unitId != entryChunk.metadata.unitId ||
            compChunk.metadata.entryBackPointer != entryChunk.metadata.moduleAtom) {
            errors.push_back({
                "companion ownership mismatch: '" + comp.beamAtom +
                "' does not belong to unit '" + entryChunk.metadata.unitId + "'"});
            return errors;
        }

        if (!compChunk.metadata.package.id.empty()) {
            errors.push_back({
                "companion '" + comp.beamAtom +
                "' carries package policy; only an entry module may declare it"});
            return errors;
        }

        if (compChunk.version >= 2 && compChunk.metadata.sourceModule.empty()) {
            errors.push_back({
                "companion '" + comp.beamAtom +
                "' has no source module identity"});
            return errors;
        }

        if (compChunk.interfaceHash != comp.expectedHash) {
            errors.push_back({
                "companion '" + comp.beamAtom +
                "' was recompiled independently — recompile the entry module"});
            return errors;
        }

        LoadedModule mod;
        mod.beamAtom = comp.beamAtom;
        mod.beamPath = compPath;
        mod.chunk = std::move(compChunk);
        unit.modules.push_back(std::move(mod));
    }

    // Add entry module last (companions first in the modules list for
    // load-order: companions should be loaded before the entry)
    LoadedModule entryMod;
    entryMod.beamAtom = entryChunk.metadata.moduleAtom;
    entryMod.beamPath = beamPath;
    entryMod.chunk = std::move(entryChunk);
    unit.modules.push_back(std::move(entryMod));

    auto key = unit.entryBeamAtom;
    auto package = unit.modules.back().chunk.metadata.package;

    // Make the complete candidate unit visible to package validation. Preserve
    // the previous unit until its replacement and embedded policy both pass.
    std::optional<LoadedUnit> previous;
    if (auto existing = m_units.find(key); existing != m_units.end()) {
        previous.emplace(std::move(existing->second));
        m_units.erase(existing);
    }
    m_units.emplace(key, std::move(unit));

    if (!package.id.empty()) {
        auto packageErrors = declarePackage(package);
        if (!packageErrors.empty()) {
            m_units.erase(key);
            if (previous) m_units.emplace(key, std::move(*previous));
            return packageErrors;
        }
    }

    m_lastLoaded = key;

    return errors;
}

auto KexiRegistry::isLoaded(const std::string& entryAtom) const -> bool {
    return m_units.count(entryAtom) > 0;
}

auto KexiRegistry::allLoadedModules() const -> std::vector<const LoadedModule*> {
    std::vector<const LoadedModule*> result;
    for (const auto& [_, unit] : m_units)
        for (const auto& mod : unit.modules)
            result.push_back(&mod);
    return result;
}

auto KexiRegistry::declarePackage(const kex::module::PackageMetadata& package)
    -> std::vector<LoadError> {
    std::vector<kex::module::PackageModuleIdentity> available;
    for (const auto& [_, unit] : m_units)
        for (const auto& mod : unit.modules)
            available.push_back({mod.chunk.metadata.unitId,
                                 mod.chunk.metadata.sourceModule});

    auto validation = kex::module::validatePackageMetadata(package, available);
    std::vector<LoadError> errors;
    for (auto& error : validation)
        errors.push_back({std::move(error.message)});
    for (const auto& [existingId, existing] : m_packages) {
        if (existingId == package.id) continue;
        std::unordered_set<std::string> existingUnits(existing.unitIds.begin(),
                                                      existing.unitIds.end());
        for (const auto& unitId : package.unitIds)
            if (existingUnits.count(unitId))
                errors.push_back({"compiled unit '" + unitId +
                                  "' is already owned by package '" +
                                  existingId + "'"});
    }
    if (errors.empty())
        m_packages[package.id] = package;
    return errors;
}

auto KexiRegistry::automaticImportModules() const
    -> std::vector<const LoadedModule*> {
    std::vector<const LoadedModule*> result;
    std::unordered_set<const LoadedModule*> seen;
    for (const auto& [_, package] : m_packages) {
        std::unordered_set<std::string> units(package.unitIds.begin(),
                                              package.unitIds.end());
        std::unordered_set<std::string> imports(package.automaticImports.begin(),
                                                package.automaticImports.end());
        for (const auto& [__, unit] : m_units)
            for (const auto& mod : unit.modules)
                if (units.count(mod.chunk.metadata.unitId) &&
                    imports.count(mod.chunk.metadata.sourceModule) &&
                    seen.insert(&mod).second)
                    result.push_back(&mod);
    }
    return result;
}

auto KexiRegistry::findExport(const std::string& moduleAtom,
                              const std::string& name) const
    -> const KexiExport* {
    for (const auto& [_, unit] : m_units)
        for (const auto& mod : unit.modules)
            if (mod.beamAtom == moduleAtom)
                for (const auto& exp : mod.chunk.typeInterface.exports)
                    if (exp.name == name)
                        return &exp;
    return nullptr;
}

auto KexiRegistry::findMethodsForReceiver(
    const std::string& methodName,
    const std::string& receiverTypeName) const
    -> std::vector<std::pair<std::string, const KexiMethod*>> {
    std::vector<std::pair<std::string, const KexiMethod*>> results;
    for (const auto& [_, package] : m_packages) {
        std::unordered_set<std::string> units(package.unitIds.begin(),
                                              package.unitIds.end());
        std::unordered_set<std::string> providers(package.receiverProviders.begin(),
                                                  package.receiverProviders.end());
        for (const auto& [__, unit] : m_units)
            for (const auto& mod : unit.modules) {
                if (!units.count(mod.chunk.metadata.unitId) ||
                    !providers.count(mod.chunk.metadata.sourceModule))
                    continue;
                for (const auto& method : mod.chunk.typeInterface.methods)
                    if (method.name == methodName &&
                        typeNameStr(method.receiverType) == receiverTypeName)
                        results.emplace_back(mod.beamAtom, &method);
            }
    }
    return results;
}

auto KexiRegistry::findType(const std::string& typeName) const
    -> std::pair<std::string, const KexiTypeExport*> {
    for (const auto& [_, unit] : m_units)
        for (const auto& mod : unit.modules)
            for (const auto& te : mod.chunk.typeInterface.types)
                if (te.name == typeName)
                    return {mod.beamAtom, &te};
    return {"", nullptr};
}

auto KexiRegistry::findADT(const std::string& adtName) const
    -> std::pair<std::string, const KexiADT*> {
    for (const auto& [_, unit] : m_units)
        for (const auto& mod : unit.modules)
            for (const auto& adt : mod.chunk.metadata.adts)
                if (adt.name == adtName)
                    return {mod.beamAtom, &adt};
    return {"", nullptr};
}

auto KexiRegistry::findRecord(const std::string& recordName) const
    -> std::pair<std::string, const KexiRecord*> {
    for (const auto& [_, unit] : m_units)
        for (const auto& mod : unit.modules)
            for (const auto& rec : mod.chunk.metadata.records)
                if (rec.name == recordName)
                    return {mod.beamAtom, &rec};
    return {"", nullptr};
}

auto KexiRegistry::getUnit(const std::string& entryAtom) const
    -> const LoadedUnit* {
    auto it = m_units.find(entryAtom);
    return it != m_units.end() ? &it->second : nullptr;
}

void KexiRegistry::unloadUnit(const std::string& entryAtom) {
    auto it = m_units.find(entryAtom);
    if (it == m_units.end()) return;
    std::unordered_set<std::string> removedIds;
    for (const auto& mod : it->second.modules)
        removedIds.insert(mod.chunk.metadata.unitId);
    m_units.erase(it);
    for (auto package = m_packages.begin(); package != m_packages.end(); ) {
        bool referencesRemoved = false;
        for (const auto& unitId : package->second.unitIds)
            if (removedIds.count(unitId)) { referencesRemoved = true; break; }
        if (referencesRemoved) package = m_packages.erase(package);
        else ++package;
    }
}

auto KexiRegistry::generateLoadErlang(const LoadedUnit& unit) const
    -> std::string {
    std::string expr;
    int idx = 0;
    for (const auto& mod : unit.modules) {
        auto var = "_KexLoad" + std::to_string(idx++);
        expr += "{ok," + var + "}=file:read_file(\"" +
                mod.beamPath + "\"), "
                "code:load_binary('" + mod.beamAtom + "',\"" +
                mod.beamPath + "\"," + var + "), ";
    }
    return expr;
}

auto KexiRegistry::buildExternalModules() const -> kex::ir::ExternalModules {
    kex::ir::ExternalModules ext;
    for (const auto& [_, unit] : m_units) {
        for (const auto& mod : unit.modules) {
            const auto& shortName = mod.chunk.metadata.sourceModule;
            if (shortName.empty()) continue;

            ext.nameToAtom[shortName] = mod.beamAtom;

            for (const auto& exp : mod.chunk.typeInterface.exports) {
                auto qualKey = shortName + "." + exp.name;
                ext.exportToBeamFn[qualKey] = exp.beamFunction;
                ext.exportArity[qualKey] = exp.beamArity;
                if (!exp.paramNames.empty())
                    ext.exportParamNames[qualKey] = exp.paramNames;
            }
        }
    }

    for (const auto& [_, package] : m_packages) {
        std::unordered_set<std::string> units(package.unitIds.begin(),
                                              package.unitIds.end());
        std::unordered_set<std::string> providers(package.receiverProviders.begin(),
                                                  package.receiverProviders.end());
        for (const auto& [__, unit] : m_units) {
            for (const auto& mod : unit.modules) {
                if (!units.count(mod.chunk.metadata.unitId) ||
                    !providers.count(mod.chunk.metadata.sourceModule))
                    continue;
                for (const auto& receiverFn : mod.chunk.typeInterface.methods) {
                    if (receiverFn.typeOnly) continue;
                    auto& vec = ext.receiverFunctions[receiverFn.name];
                    bool duplicate = false;
                    for (const auto& existing : vec)
                        if (existing.moduleAtom == mod.beamAtom &&
                            existing.beamFunction == receiverFn.beamFunction &&
                            existing.beamArity == receiverFn.beamArity) {
                            duplicate = true; break;
                        }
                    if (!duplicate)
                        vec.push_back({mod.beamAtom, receiverFn.beamFunction,
                                       receiverFn.beamArity});
                }
            }
        }
    }
    return ext;
}

auto KexiRegistry::buildSemanticInterfaces() const
    -> kex::semantic::ImportedInterfaces {
    kex::semantic::ImportedInterfaces interfaces;
    std::unordered_set<std::string> automaticModules;
    for (const auto& [_, package] : m_packages)
        automaticModules.insert(package.automaticImports.begin(),
                                package.automaticImports.end());

    for (const auto& [_, unit] : m_units)
        for (const auto& module : unit.modules) {
            const auto& sourceModule = module.chunk.metadata.sourceModule;
            if (sourceModule.empty()) continue;
            auto& imported = interfaces.modules[sourceModule];
            imported.sourceModule = sourceModule;
            imported.backendModule = module.beamAtom;
            imported.automaticImport = automaticModules.count(sourceModule) > 0;
            for (const auto& exported : module.chunk.typeInterface.exports)
                imported.exports[exported.name].push_back(
                    importedFunction(exported, module.beamAtom));
        }

    for (const auto& [_, package] : m_packages) {
        std::unordered_set<std::string> units(package.unitIds.begin(),
                                              package.unitIds.end());
        std::unordered_set<std::string> providers(package.receiverProviders.begin(),
                                                  package.receiverProviders.end());
        for (const auto& [__, unit] : m_units)
            for (const auto& module : unit.modules) {
                if (!units.count(module.chunk.metadata.unitId) ||
                    !providers.count(module.chunk.metadata.sourceModule))
                    continue;
                for (const auto& receiver : module.chunk.typeInterface.methods) {
                    if (receiver.typeOnly) continue;
                    interfaces.receiverFunctions[receiver.name].push_back(
                        importedReceiverFunction(receiver, module.beamAtom));
                }
            }
    }

    // Collect trait conformances and expand them to ADT constructors.
    // When `make Optional<X>, implement: Truthyable`, constructors
    // Just and None also satisfy Truthyable.
    std::unordered_map<std::string, std::vector<std::string>> adtConstructors;
    for (const auto& [_, unit] : m_units)
        for (const auto& module : unit.modules)
            for (const auto& adt : module.chunk.metadata.adts)
                for (const auto& ctor : adt.constructors)
                    adtConstructors[adt.name].push_back(ctor.name);

    for (const auto& [_, unit] : m_units)
        for (const auto& module : unit.modules)
            for (const auto& c : module.chunk.metadata.traitConformances) {
                interfaces.traitConformances.push_back({c.typeName, c.traitName});
                if (auto it = adtConstructors.find(c.typeName);
                    it != adtConstructors.end())
                    for (const auto& ctorName : it->second)
                        interfaces.traitConformances.push_back(
                            {ctorName, c.traitName});
            }

    return interfaces;
}

auto KexiRegistry::findEntryByShortName(const std::string& shortName) const
    -> std::string {
    for (const auto& [entryAtom, unit] : m_units)
        for (const auto& mod : unit.modules)
            if (mod.chunk.metadata.sourceModule == shortName)
                return entryAtom;
    return "";
}

auto KexiRegistry::generateDisplayRegistration(const LoadedUnit& unit) const
    -> std::string {
    std::string records;
    std::string variants;

    for (const auto& mod : unit.modules) {
        const auto& sn = mod.chunk.metadata.sourceModule;
        if (sn.empty()) continue;

        for (const auto& rec : mod.chunk.metadata.records) {
            if (!records.empty()) records += ", ";
            records += "'" + rec.name + "' => [";
            for (size_t i = 0; i < rec.fields.size(); i++) {
                if (i) records += ", ";
                records += "'" + rec.fields[i].name + "'";
            }
            records += "]";
        }

        for (const auto& adt : mod.chunk.metadata.adts) {
            for (const auto& ctor : adt.constructors) {
                if (!variants.empty()) variants += ", ";
                variants += "'" + ctor.tagAtom + "' => {" +
                    std::to_string(ctor.arity) + ", '" + adt.name + "'}";
            }
        }
    }

    if (records.empty() && variants.empty())
        return "";

    return "kex_io:register_display(#{" + records + "}, #{" + variants + "})";
}

auto KexiRegistry::generateCompletionStubs(const LoadedUnit& unit) const
    -> std::string {
    std::string stubs;

    for (const auto& mod : unit.modules) {
        const auto& sn = mod.chunk.metadata.sourceModule;
        if (sn.empty()) continue;

        stubs += "module " + sn + " do\n";

        for (const auto& te : mod.chunk.typeInterface.types) {
            stubs += "  type " + te.name;
            if (!te.constructors.empty()) {
                stubs += " = ";
                for (size_t i = 0; i < te.constructors.size(); i++) {
                    if (i) stubs += " | ";
                    stubs += te.constructors[i];
                }
            }
            stubs += "\n";
        }

        for (const auto& exp : mod.chunk.typeInterface.exports) {
            stubs += "  let " + exp.name + "(";
            for (int i = 0; i < exp.beamArity; i++) {
                if (i) stubs += ", ";
                stubs += "x" + std::to_string(i);
            }
            stubs += ") = ()\n";
        }

        // Methods as module-level functions for completion
        std::unordered_set<std::string> seen;
        for (const auto& method : mod.chunk.typeInterface.methods) {
            if (!seen.insert(method.name).second) continue;
            stubs += "  let " + method.name + "(";
            for (int i = 0; i < method.beamArity; i++) {
                if (i) stubs += ", ";
                stubs += "x" + std::to_string(i);
            }
            stubs += ") = ()\n";
        }

        stubs += "end\n";
    }

    return stubs;
}

} // namespace kex::beam
