#include "kexi_registry.hxx"
#include "beam_file.hxx"
#include <filesystem>
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
    m_units.erase(key);
    m_lastLoaded = key;
    m_units.emplace(key, std::move(unit));

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
    for (const auto& [_, unit] : m_units)
        for (const auto& mod : unit.modules)
            for (const auto& method : mod.chunk.typeInterface.methods)
                if (method.name == methodName &&
                    typeNameStr(method.receiverType) == receiverTypeName)
                    results.emplace_back(mod.beamAtom, &method);
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
    m_units.erase(entryAtom);
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
            // Derive the short Kex name from the BEAM atom.
            // "Kex.BinaryTree" → "BinaryTree", "kex_foo" → skip (entry)
            std::string shortName;
            if (mod.beamAtom.rfind("Kex.", 0) == 0)
                shortName = mod.beamAtom.substr(4);
            else
                continue; // entry modules don't have Kex-syntax names

            ext.nameToAtom[shortName] = mod.beamAtom;

            for (const auto& exp : mod.chunk.typeInterface.exports) {
                auto qualKey = shortName + "." + exp.name;
                ext.exportToBeamFn[qualKey] = exp.name;
                ext.exportArity[qualKey] = exp.beamArity;
            }
            for (const auto& method : mod.chunk.typeInterface.methods) {
                auto qualKey = shortName + "." + method.name;
                if (ext.exportToBeamFn.find(qualKey) == ext.exportToBeamFn.end()) {
                    ext.exportToBeamFn[qualKey] = method.name;
                    ext.exportArity[qualKey] = method.beamArity;
                }
            }
        }
    }
    return ext;
}

auto KexiRegistry::shortKexName(const std::string& beamAtom) const -> std::string {
    if (beamAtom.rfind("Kex.", 0) == 0)
        return beamAtom.substr(4);
    return "";
}

auto KexiRegistry::findEntryByShortName(const std::string& shortName) const
    -> std::string {
    std::string qualAtom = "Kex." + shortName;
    for (const auto& [entryAtom, unit] : m_units)
        for (const auto& mod : unit.modules)
            if (mod.beamAtom == qualAtom || shortKexName(mod.beamAtom) == shortName)
                return entryAtom;
    return "";
}

auto KexiRegistry::generateDisplayRegistration(const LoadedUnit& unit) const
    -> std::string {
    std::string records;
    std::string variants;

    for (const auto& mod : unit.modules) {
        auto sn = shortKexName(mod.beamAtom);
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
        auto sn = shortKexName(mod.beamAtom);
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
