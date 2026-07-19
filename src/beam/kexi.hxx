#pragma once
// KexI chunk: combined type interface + structural metadata for one Kex
// module, stored as a custom BEAM chunk in ETF format. See docs/kexm-plan.md.
#include "etf.hxx"
#include "../module/package_metadata.hxx"
#include <array>
#include <string>
#include <vector>

namespace kex::beam {

static constexpr const char* KEXI_CHUNK_ID = "KexI";
static constexpr int KEXI_SCHEMA_VERSION = 3;

using Hash128 = std::array<uint8_t, 16>;

// ── Type terms (structural, not strings) ─────────────────────────────

struct KexiType;
using KexiTypePtr = std::shared_ptr<KexiType>;

struct KexiType {
    enum Kind {
        Primitive,    // name: "Integer", "Bool", "Char", "String", "Unit", "Atom"
        Named,        // name + typeArgs
        Func,         // params + result
        Tuple,        // elements
        List,         // element
        Map,          // key + value
        Optional,     // inner
        Union,        // members
        Constrained,  // varName + traitName
        Never,
        Unknown,
    };
    Kind kind;
    std::string name;
    std::string traitName; // for Constrained
    std::vector<KexiTypePtr> typeArgs;
    KexiTypePtr result; // for Func
};

auto kexiPrimitive(const std::string& name) -> KexiTypePtr;
auto kexiNamed(const std::string& name, std::vector<KexiTypePtr> args = {}) -> KexiTypePtr;
auto kexiFunc(std::vector<KexiTypePtr> params, KexiTypePtr result) -> KexiTypePtr;
auto kexiTuple(std::vector<KexiTypePtr> elems) -> KexiTypePtr;
auto kexiList(KexiTypePtr element) -> KexiTypePtr;
auto kexiMap(KexiTypePtr key, KexiTypePtr value) -> KexiTypePtr;
auto kexiOptional(KexiTypePtr inner) -> KexiTypePtr;
auto kexiUnion(std::vector<KexiTypePtr> members) -> KexiTypePtr;
auto kexiConstrained(const std::string& var, const std::string& trait) -> KexiTypePtr;
auto kexiNever() -> KexiTypePtr;
auto kexiUnknown() -> KexiTypePtr;

// ── Type interface section ───────────────────────────────────────────

struct KexiExport {
    std::string name;
    std::string beamFunction; // emitted function name; v1 defaults to name
    int beamArity = 0;
    bool isFoul = false;
    std::vector<KexiTypePtr> paramTypes;
    std::vector<std::string> paramNames;
    KexiTypePtr returnType;
};

struct KexiConstant {
    std::string name;
    KexiTypePtr type;
};

struct KexiTypeExport {
    std::string name;
    std::vector<std::string> genericParams;
    std::vector<std::string> constructors;
};

struct KexiMethod {
    std::string name;
    KexiTypePtr receiverType;
    int beamArity = 0;
    bool isFoul = false;
    bool typeOnly = false; // annotation-only, no BEAM implementation
    std::vector<KexiTypePtr> paramTypes;
    KexiTypePtr returnType;
    std::string beamFunction; // emitted BEAM function name
};

struct KexiTypeInterface {
    std::vector<KexiExport> exports;
    std::vector<KexiConstant> constants;
    std::vector<KexiTypeExport> types;
    std::vector<KexiMethod> methods;
};

// ── Structural metadata section ──────────────────────────────────────

enum class KexiModuleRole { Entry, Companion };

struct KexiCompanion {
    std::string beamAtom;
    std::string relativePath;
    Hash128 expectedHash;
};

struct KexiRecordField {
    std::string name;
    KexiTypePtr type;
};

struct KexiRecord {
    std::string name;
    std::vector<KexiRecordField> fields;
};

struct KexiConstructor {
    std::string name;
    std::string tagAtom;
    std::vector<std::string> fieldNames; // empty = positional
    int arity = 0;
};

struct KexiADT {
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<KexiConstructor> constructors;
};

struct KexiMethodOwnership {
    std::string methodName;
    std::string beamFunction;
};

struct KexiTraitConformance {
    std::string typeName;
    std::string traitName;
};

struct KexiStructuralMetadata {
    // Durable ownership within a compiled package/unit. `unitId` identifies
    // the artifact group; `sourceModule` is the Kex-facing qualified module;
    // `moduleAtom` is its backend identity.
    std::string unitId;
    std::string sourceModule;
    std::string moduleAtom;
    bool isFoul = false;
    KexiModuleRole role = KexiModuleRole::Entry;
    std::string entryBackPointer; // for companions: the entry module's atom
    std::vector<KexiCompanion> companions; // for entries: the companion manifest
    std::vector<KexiRecord> records;
    std::vector<KexiADT> adts;
    std::vector<KexiMethodOwnership> methodOwnership;
    std::vector<KexiTraitConformance> traitConformances;
    std::vector<std::string> publicExports;
    // Present only on a package entry module. An empty id means that this
    // compilation unit does not carry package policy.
    kex::module::PackageMetadata package;
};

// ── KexI chunk (both sections) ───────────────────────────────────────

struct KexiChunk {
    int version = KEXI_SCHEMA_VERSION;
    Hash128 interfaceHash{};
    KexiTypeInterface typeInterface;
    KexiStructuralMetadata metadata;
};

// ── Serialization ────────────────────────────────────────────────────

auto serializeKexi(const KexiChunk& chunk) -> std::vector<uint8_t>;
auto deserializeKexi(const std::vector<uint8_t>& data) -> KexiChunk;

auto computeInterfaceHash(const KexiChunk& chunk) -> Hash128;

} // namespace kex::beam
