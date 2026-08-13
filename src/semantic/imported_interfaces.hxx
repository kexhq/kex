#pragma once

#include "traits.hxx"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kex::semantic {

// The synthetic module holding the standard library's FILE-LEVEL declarations
// — `describe`, `it`, `assert`, and friends — as opposed to the named modules
// (Math, Bits, IO, …) that the same sources also declare. Both are flagged
// automaticImport, but only these are callable without qualification, so the
// two have to be told apart wherever "is this bare name in scope?" is decided.
inline constexpr std::string_view kFileLevelPreludeModule = "Prelude";

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
    // Source owner selected by overload resolution (for example `Optional`
    // rather than an unrelated same-named `Bits` export). Tooling uses this
    // identity for documentation and navigation.
    std::string sourceModule;
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
    // Hidden trait dictionaries appended after the source arguments. Each
    // entry names the zero-based source argument whose concrete value chooses
    // the implementation and the trait contract the dictionary represents.
    std::vector<std::pair<std::size_t, std::string>> traitDictionaries;
};

struct ImportedTraitConformance {
    std::string typeName;
    std::string traitName;
};

struct ImportedADT {
    std::string name;
    std::vector<std::string> constructors;
    std::unordered_map<std::string, int> constructorArities;
    // Generic result reconstruction for source-derived interfaces. Each
    // constructor payload records the ADT type-parameter slot it fills, or -1
    // for a payload unrelated to a type parameter (`Just(X)` -> {0}).
    size_t typeParamCount = 0;
    std::unordered_map<std::string, std::vector<int>> constructorTypeParamSlots;
    // Source-derived interfaces retain spelling and complete payload types so
    // editor tooling can present the declaration rather than only its runtime
    // arity. Prebuilt interfaces may leave these empty; callers can still use
    // the generic-slot/arity information above as a faithful fallback.
    std::vector<std::string> typeParamNames;
    std::unordered_map<std::string, std::vector<TypePtr>> constructorParamTypes;
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
    // The field NAMES behind those counts. Without them a field read on an
    // imported record could not be told apart from an unrelated method of the
    // same spelling, and the method won: `moment.time.nanosecond` on the
    // prelude's Time resolved to units.kex's `Integer.nanosecond` Measure
    // constructor and failed to typecheck. Names alone settle that question —
    // the field's TYPE is not carried, so such a read stays gradual.
    std::unordered_map<std::string, std::unordered_set<std::string>>
        recordFieldNames;
    // Every type NAME the imported sources declare: ADTs, records, and
    // constructor-less aliases such as `type List<X> = [X]`. `adts` only
    // carries types that have constructors, and it is indexed by those
    // constructors, so it cannot answer "is `List` a declared type?" — which
    // name resolution needs, because a bare type name is a legal value in a
    // type-directed call like `xs.to(List)`.
    std::unordered_set<std::string> typeNames;
    std::vector<TraitDef> traits;
};

} // namespace kex::semantic
