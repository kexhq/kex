#pragma once

#include "traits.hxx"
#include <functional>
#include <memory>
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
    // Declared in a `make T do` block, so this name is reached through a
    // typed receiver rather than as a bare call. `addReceiverSig` files such a
    // method under `exports` as well as `receiverFunctions`, and the import
    // scope must not let it compete for the bare name — see ResolvePass's
    // ambiguity check (kexhq/kex#272).
    bool isReceiverMethod = false;
    // ADT constructors are represented as ordinary module exports so
    // qualified access follows the same visibility and backend-routing rules
    // as constants and functions.
    bool isConstructor = false;
    // Owning ADT, retained when constructor spellings collide across modules.
    std::string constructorOwner;
};

struct ImportedModuleInterface {
    std::string sourceModule;
    std::string backendModule;
    bool automaticImport = false;
    // Declared `capability`, so an importer may replace it with `with` and
    // implement it with `make ..., implement:`. Without this crossing the
    // module boundary a stdlib capability is invisible to every consumer
    // (kexhq/kex#143).
    bool isCapability = false;
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
    // A serving slot with no Reply<T>: lower as asynchronous gen_server:cast.
    bool isServingCast = false;
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

struct ImportedDistinctType {
    std::vector<std::string> typeParams;
    TypePtr backingType;
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
    // Complete declared field types, used for open-record width/depth checks
    // across module boundaries. Keys include qualified runtime identities;
    // bare aliases are retained only where the existing record registry can
    // resolve them unambiguously.
    std::unordered_map<
        std::string, std::unordered_map<std::string, TypePtr>> recordFields;
    // Bodies of transparent aliases (`type FilePath = String`), keyed by the
    // bare name the way the local alias table is. Distinct types are NOT here
    // — those stay nominal and live in `distinctTypes`.
    std::unordered_map<std::string, TypePtr> typeAliases;
    // Every type NAME the imported sources declare: ADTs, records, and
    // constructor-less aliases such as `type List<X> = [X]`. `adts` only
    // carries types that have constructors, and it is indexed by those
    // constructors, so it cannot answer "is `List` a declared type?" — which
    // name resolution needs, because a bare type name is a legal value in a
    // type-directed call like `xs.to(List)`.
    std::unordered_set<std::string> typeNames;
    std::unordered_map<std::string, ImportedDistinctType> distinctTypes;
    std::vector<TraitDef> traits;
};

// Whether `module.member` names a foul export.
//
// Per-export purity is the only source of truth: Kex has no module-level
// foulness, so a module full of foul functions never taints the pure ones
// beside it (kexhq/kex#130). An overload set counts as foul when ANY overload
// is foul — this is consulted before overload resolution has picked a winner,
// so the conservative answer is the correct one, and the post-typecheck
// enrichment pass narrows it to the resolved target.
inline auto isExportFoul(const ImportedInterfaces& ifaces,
                         const std::string& module,
                         const std::string& member) -> bool {
    auto mod = ifaces.modules.find(module);
    if (mod == ifaces.modules.end()) return false;
    auto exported = mod->second.exports.find(member);
    if (exported == mod->second.exports.end()) return false;
    for (const auto& overload : exported->second)
        if (overload.signature.isFoul) return true;
    return false;
}

// Gives every module ADT its qualified identity inside the signatures that
// spell it. A signature is read from its spelling, so `BACKEND : Backend`
// inside `module Kex` carries a bare `Backend`, while the ADT, and the
// constructors it owns, are `Kex.Backend`. Left bare, that name could only be
// matched by spelling, and NetErrorKind has a constructor spelled `Backend`
// too. A bare name means the ADT declared by the innermost enclosing module
// of the signature's own module that declares one; anything else is left as
// written.
inline auto qualifyModuleAdtNames(ImportedInterfaces& ifaces) -> void {
    std::unordered_set<std::string> moduleAdts;
    for (const auto& adt : ifaces.adts)
        if (adt.name.find('.') != std::string::npos) moduleAdts.insert(adt.name);
    if (moduleAdts.empty()) return;
    auto adtIn = [&](std::string scope, const std::string& name) -> std::string {
        while (!scope.empty()) {
            if (auto candidate = scope + "." + name; moduleAdts.count(candidate))
                return candidate;
            const auto dot = scope.rfind('.');
            if (dot == std::string::npos) break;
            scope.resize(dot);
        }
        return "";
    };
    // Types are shared between signatures, so every rewrite is of a copy.
    std::function<void(TypePtr&, const std::string&)> qualify =
        [&](TypePtr& type, const std::string& scope) {
            if (!type) return;
            auto copy = std::make_shared<Type>(*type);
            bool changed = false;
            std::visit([&](auto& kind) {
                using K = std::decay_t<decltype(kind)>;
                auto visit = [&](TypePtr& inner) {
                    const auto before = inner;
                    qualify(inner, scope);
                    changed = changed || inner != before;
                };
                if constexpr (std::is_same_v<K, NamedType>) {
                    if (kind.name.find('.') == std::string::npos)
                        if (auto owned = adtIn(scope, kind.name); !owned.empty()) {
                            kind.name = owned;
                            changed = true;
                        }
                    for (auto& arg : kind.typeArgs) visit(arg);
                } else if constexpr (std::is_same_v<K, ListType>) {
                    visit(kind.element);
                } else if constexpr (std::is_same_v<K, OptionalType>) {
                    visit(kind.inner);
                } else if constexpr (std::is_same_v<K, MapType>) {
                    visit(kind.key);
                    visit(kind.value);
                } else if constexpr (std::is_same_v<K, TupleType>) {
                    for (auto& element : kind.elements) visit(element);
                } else if constexpr (std::is_same_v<K, FuncType>) {
                    for (auto& param : kind.params) visit(param);
                    visit(kind.result);
                } else if constexpr (std::is_same_v<K, UnionType>) {
                    for (auto& member : kind.members) visit(member);
                }
            }, copy->kind);
            if (changed) type = copy;
        };
    auto qualifySignature = [&](ImportedFunction& function) {
        if (function.sourceModule.empty()) return;
        for (auto& param : function.signature.params)
            qualify(param, function.sourceModule);
        qualify(function.signature.result, function.sourceModule);
    };
    for (auto& [_, module] : ifaces.modules)
        for (auto& [__, functions] : module.exports)
            for (auto& function : functions) qualifySignature(function);
    for (auto& [_, functions] : ifaces.receiverFunctions)
        for (auto& function : functions) qualifySignature(function);
    // A record's field types are spelled in the record's own module:
    // `operation : NetOperation` in `Net.NetError` is `Net.NetOperation`.
    for (auto& [record, fields] : ifaces.recordFields) {
        const auto dot = record.rfind('.');
        if (dot == std::string::npos) continue;
        const auto scope = record.substr(0, dot);
        for (auto& [__, fieldType] : fields) qualify(fieldType, scope);
    }
}

} // namespace kex::semantic
