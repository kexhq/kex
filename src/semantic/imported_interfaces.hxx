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

// Exact backend ownership selected by semantic analysis for an imported call.
// The AST remains backend-neutral; lowering consumes this side table.
struct ResolvedCallTarget {
    std::string backendModule;
    std::string backendFunction;
    int backendArity = 0;
    bool passesReceiver = false;
};

struct ImportedTraitConformance {
    std::string typeName;
    std::string traitName;
};

struct ImportedADT {
    std::string name;
    std::vector<std::string> constructors;
};

// Backend-neutral checked interface snapshot. Ordinary module exports retain
// their owner; receiver functions are populated separately and only after
// package policy has approved their provider module.
struct ImportedInterfaces {
    std::unordered_map<std::string, ImportedModuleInterface> modules;
    std::unordered_map<std::string, std::vector<ImportedFunction>>
        receiverFunctions;
    std::vector<ImportedTraitConformance> traitConformances;
    std::vector<ImportedADT> adts;
    std::vector<TraitDef> traits;
};

} // namespace kex::semantic
