#include "package_metadata.hxx"

#include <unordered_map>
#include <unordered_set>

namespace kex::module {

auto validatePackageMetadata(
    const PackageMetadata& package,
    const std::vector<PackageModuleIdentity>& availableModules)
    -> std::vector<PackageMetadataError> {
    std::vector<PackageMetadataError> errors;
    if (package.id.empty())
        errors.push_back({"package identity must not be empty"});
    if (package.unitIds.empty())
        errors.push_back({"package '" + package.id + "' owns no compiled units"});

    std::unordered_set<std::string> availableUnits;
    std::unordered_map<std::string, std::vector<std::string>> moduleUnits;
    for (const auto& module : availableModules) {
        availableUnits.insert(module.unitId);
        if (!module.sourceModule.empty())
            moduleUnits[module.sourceModule].push_back(module.unitId);
    }

    std::unordered_set<std::string> ownedUnits;
    for (const auto& unit : package.unitIds) {
        if (!ownedUnits.insert(unit).second) {
            errors.push_back({"package '" + package.id +
                              "' repeats compiled unit '" + unit + "'"});
        } else if (!availableUnits.count(unit)) {
            errors.push_back({"package '" + package.id +
                              "' references unavailable compiled unit '" + unit + "'"});
        }
    }

    auto validateModules = [&](const std::vector<std::string>& modules,
                               const std::string& role) {
        std::unordered_set<std::string> seen;
        for (const auto& name : modules) {
            if (name.empty()) {
                errors.push_back({role + " module name must not be empty"});
                continue;
            }
            if (!seen.insert(name).second) {
                errors.push_back({"package '" + package.id + "' repeats " +
                                  role + " module '" + name + "'"});
                continue;
            }
            auto found = moduleUnits.find(name);
            if (found == moduleUnits.end()) {
                errors.push_back({"package '" + package.id + "' declares unknown " +
                                  role + " module '" + name + "'"});
                continue;
            }
            size_t ownedMatches = 0;
            for (const auto& unit : found->second)
                if (ownedUnits.count(unit)) ownedMatches++;
            if (ownedMatches == 0) {
                errors.push_back({"package '" + package.id + "' declares " + role +
                                  " module '" + name +
                                  "' from a unit it does not own"});
            } else if (ownedMatches > 1) {
                errors.push_back({"package '" + package.id + "' has ambiguous " + role +
                                  " module '" + name + "' in multiple owned units"});
            }
        }
    };

    validateModules(package.automaticImports, "automatic import");
    validateModules(package.receiverProviders, "receiver-function provider");
    return errors;
}

} // namespace kex::module
