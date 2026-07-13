#pragma once

#include "traits.hxx"
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

struct ImportedFunction {
    std::string sourceName;
    std::string backendFunction;
    std::string backendModule;
    int backendArity = 0;
    Signature signature;
};

struct ImportedModuleInterface {
    std::string sourceModule;
    std::string backendModule;
    bool automaticImport = false;
    std::unordered_map<std::string, std::vector<ImportedFunction>> exports;
};

// Backend-neutral checked interface snapshot. Ordinary module exports retain
// their owner; receiver functions are populated separately and only after
// package policy has approved their provider module.
struct ImportedInterfaces {
    std::unordered_map<std::string, ImportedModuleInterface> modules;
    std::unordered_map<std::string, std::vector<ImportedFunction>>
        receiverFunctions;
};

} // namespace kex::semantic
