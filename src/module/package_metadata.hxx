#pragma once

#include <string>
#include <vector>

namespace kex::module {

// Backend-independent package policy. Its source representation belongs to
// package.kex; compiled copies use the existing entry-module interface.
struct PackageMetadata {
    std::string id;
    std::vector<std::string> unitIds;
    std::vector<std::string> automaticImports;
    std::vector<std::string> receiverProviders;
};

struct PackageModuleIdentity {
    std::string unitId;
    std::string sourceModule;
};

struct PackageMetadataError {
    std::string message;
};

auto validatePackageMetadata(
    const PackageMetadata& package,
    const std::vector<PackageModuleIdentity>& availableModules)
    -> std::vector<PackageMetadataError>;

} // namespace kex::module
