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
    // Source parameter names excluding the receiver for receiver functions.
    std::vector<std::string> paramNames;
    Signature signature;
    // Source module that owns this export. Receiver functions are copied into
    // a name-indexed table, so retaining ownership here is what lets semantic
    // analysis apply lexical `using` policy instead of making every opt-in
    // module globally visible.
    std::string sourceModule;
    // ADT constructors are represented as ordinary module exports so
    // qualified access follows the same visibility and backend-routing rules
    // as constants and functions.
    bool isConstructor = false;
};

struct ImportedModuleInterface {
    std::string sourceModule;
    std::string backendModule;
    bool automaticImport = false;
    bool isFoul = false;
    std::unordered_map<std::string, std::vector<ImportedFunction>> exports;
};

// Exact backend ownership selected by semantic analysis for an imported call.
// The AST remains backend-neutral; lowering consumes this side table.
struct ResolvedCallTarget {
    std::string backendModule;
    std::string backendFunction;
    int backendArity = 0;
    bool passesReceiver = false;
    bool isFoul = false;
    // Source parameter names excluding the receiver when passesReceiver is
    // true; otherwise names all function parameters.
    std::vector<std::string> paramNames;
    // For a make-block winner compiled in the current unit, lowering chooses
    // the local implementation name. The full checked signature keeps
    // same-receiver, same-name/arity overloads distinct without teaching the
    // semantic layer about backend symbol spelling.
    std::vector<std::string> localDispatchTypes;
};

struct ImportedTraitConformance {
    std::string typeName;
    std::string traitName;
};

struct ImportedADT {
    std::string name;
    std::vector<std::string> constructors;
    std::unordered_map<std::string, int> constructorArities;
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
    // Field count per imported record, so a pattern that destructures the
    // wrong number of them is a compile error rather than an opaque runtime
    // failure (`if_clause` on BEAM).
    std::unordered_map<std::string, size_t> recordArities;
    std::vector<TraitDef> traits;
};

} // namespace kex::semantic
