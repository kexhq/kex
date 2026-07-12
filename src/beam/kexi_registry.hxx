#pragma once
// KexI registry: stores loaded module interfaces for the REPL.
// Supports loading .kx.beam files (entry + companions), querying
// exports/methods/types, and reload/replacement.
#include "kexi.hxx"
#include "../ir/lower.hxx"
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::beam {

struct LoadedModule {
    std::string beamAtom;
    std::string beamPath;
    KexiChunk chunk;
};

struct LoadedUnit {
    std::string entryBeamAtom;
    std::string entryPath;
    std::vector<LoadedModule> modules; // entry first, then companions
};

struct LoadError {
    std::string message;
};

class KexiRegistry {
public:
    auto loadUnit(const std::string& beamPath) -> std::vector<LoadError>;

    auto isLoaded(const std::string& entryAtom) const -> bool;

    auto allLoadedModules() const -> std::vector<const LoadedModule*>;

    auto findExport(const std::string& moduleAtom,
                    const std::string& name) const -> const KexiExport*;

    auto findMethodsForReceiver(const std::string& methodName,
                                const std::string& receiverTypeName) const
        -> std::vector<std::pair<std::string, const KexiMethod*>>;

    auto findType(const std::string& typeName) const
        -> std::pair<std::string, const KexiTypeExport*>;

    auto findADT(const std::string& adtName) const
        -> std::pair<std::string, const KexiADT*>;

    auto findRecord(const std::string& recordName) const
        -> std::pair<std::string, const KexiRecord*>;

    auto getUnit(const std::string& entryAtom) const -> const LoadedUnit*;

    void unloadUnit(const std::string& entryAtom);

    auto lastLoadedEntryAtom() const -> const std::string& { return m_lastLoaded; }

    auto generateLoadErlang(const LoadedUnit& unit) const -> std::string;

    auto buildExternalModules() const -> kex::ir::ExternalModules;

    auto generateDisplayRegistration(const LoadedUnit& unit) const -> std::string;

    auto generateCompletionStubs(const LoadedUnit& unit) const -> std::string;

    auto shortKexName(const std::string& beamAtom) const -> std::string;

    auto findEntryByShortName(const std::string& shortName) const -> std::string;

private:
    std::unordered_map<std::string, LoadedUnit> m_units; // keyed by entry atom
    std::string m_lastLoaded;
};

} // namespace kex::beam
