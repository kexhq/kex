#include "test.hxx"
#include "../src/beam/etf.hxx"
#include "../src/beam/interface_cache.hxx"
#include "../src/cache/cache.hxx"
#include "../src/common/file_io.hxx"
#include "../src/common/prelude_loader.hxx"
#include "../src/semantic/imported_interfaces.hxx"
#include "../src/semantic/traits.hxx"
#include "../src/semantic/types.hxx"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

namespace {

using namespace kex::semantic;
namespace fs = std::filesystem;

// Strict structural equality for the round-trip agreement test. Unlike
// typesEqual (which fuzzes bare-vs-qualified names and ignores Atom's
// display name), every field the codec carries must survive exactly —
// including null, which the codec preserves as "null" rather than
// normalizing to UnknownType.
auto typesExactlyEqual(const TypePtr& a, const TypePtr& b) -> bool {
    if (!a || !b) return a == b;
    if (a.get() == b.get()) return true;
    return std::visit(
        [&](const auto& at) -> bool {
            using AT = std::decay_t<decltype(at)>;
            const auto* bt = std::get_if<AT>(&b->kind);
            if (!bt) return false;
            if constexpr (std::is_same_v<AT, PrimitiveType>) {
                return at.kind == bt->kind && at.atomName == bt->atomName;
            } else if constexpr (std::is_same_v<AT, SizedIntType>) {
                return at.bits == bt->bits && at.isSigned == bt->isSigned;
            } else if constexpr (std::is_same_v<AT, SizedFloatType>) {
                return at.bits == bt->bits;
            } else if constexpr (std::is_same_v<AT, NamedType>) {
                if (at.name != bt->name) return false;
                if (at.typeArgs.size() != bt->typeArgs.size()) return false;
                for (size_t i = 0; i < at.typeArgs.size(); i++)
                    if (!typesExactlyEqual(at.typeArgs[i], bt->typeArgs[i]))
                        return false;
                return true;
            } else if constexpr (std::is_same_v<AT, FuncType>) {
                if (at.block != bt->block) return false;
                if (at.params.size() != bt->params.size()) return false;
                for (size_t i = 0; i < at.params.size(); i++)
                    if (!typesExactlyEqual(at.params[i], bt->params[i]))
                        return false;
                return typesExactlyEqual(at.result, bt->result);
            } else if constexpr (std::is_same_v<AT, TupleType>) {
                if (at.elements.size() != bt->elements.size()) return false;
                for (size_t i = 0; i < at.elements.size(); i++)
                    if (!typesExactlyEqual(at.elements[i], bt->elements[i]))
                        return false;
                return true;
            } else if constexpr (std::is_same_v<AT, ListType>) {
                return typesExactlyEqual(at.element, bt->element);
            } else if constexpr (std::is_same_v<AT, MapType>) {
                return typesExactlyEqual(at.key, bt->key) &&
                       typesExactlyEqual(at.value, bt->value);
            } else if constexpr (std::is_same_v<AT, OptionalType>) {
                return typesExactlyEqual(at.inner, bt->inner);
            } else if constexpr (std::is_same_v<AT, UnionType>) {
                if (at.members.size() != bt->members.size()) return false;
                for (size_t i = 0; i < at.members.size(); i++)
                    if (!typesExactlyEqual(at.members[i], bt->members[i]))
                        return false;
                return true;
            } else if constexpr (std::is_same_v<AT, IntersectionType>) {
                if (at.members.size() != bt->members.size()) return false;
                for (size_t i = 0; i < at.members.size(); i++)
                    if (!typesExactlyEqual(at.members[i], bt->members[i]))
                        return false;
                return true;
            } else if constexpr (std::is_same_v<AT, RecordType>) {
                if (at.fields.size() != bt->fields.size()) return false;
                for (size_t i = 0; i < at.fields.size(); i++)
                    if (at.fields[i].first != bt->fields[i].first ||
                        !typesExactlyEqual(at.fields[i].second,
                                           bt->fields[i].second))
                        return false;
                return true;
            } else if constexpr (std::is_same_v<AT, TypeVar>) {
                return at.id == bt->id;
            } else if constexpr (std::is_same_v<AT, UnknownType>) {
                return true;
            } else if constexpr (std::is_same_v<AT, VoidType>) {
                return at.reason == bt->reason;
            } else if constexpr (std::is_same_v<AT, ConstrainedType>) {
                return at.varName == bt->varName &&
                       at.traitName == bt->traitName &&
                       at.genericId == bt->genericId;
            } else {
                static_assert(std::is_void_v<AT>, "unhandled Type alternative");
                return false;
            }
        },
        a->kind);
}

auto typeVecEqual(const std::vector<TypePtr>& a,
                  const std::vector<TypePtr>& b) -> bool {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!typesExactlyEqual(a[i], b[i])) return false;
    return true;
}

auto signaturesEqual(const Signature& a, const Signature& b) -> bool {
    return a.name == b.name && typeVecEqual(a.params, b.params) &&
           typesExactlyEqual(a.result, b.result) && a.isFoul == b.isFoul &&
           a.requiredParams == b.requiredParams &&
           a.paramNames == b.paramNames && a.makeModule == b.makeModule &&
           a.declaringModule == b.declaringModule;
}

auto functionsEqual(const ImportedFunction& a, const ImportedFunction& b)
    -> bool {
    return a.sourceName == b.sourceName &&
           a.backendFunction == b.backendFunction &&
           a.backendModule == b.backendModule &&
           a.backendArity == b.backendArity && a.paramNames == b.paramNames &&
           a.defaultSources == b.defaultSources &&
           signaturesEqual(a.signature, b.signature) &&
           a.sourceModule == b.sourceModule &&
           a.isReceiverMethod == b.isReceiverMethod &&
           a.visibleByReceiver == b.visibleByReceiver &&
           a.isConstructor == b.isConstructor &&
           a.remoteSlot == b.remoteSlot &&
           a.constructorOwner == b.constructorOwner &&
           a.implementation == b.implementation &&
           a.foreignTarget == b.foreignTarget;
}

auto fnVecEqual(const std::vector<ImportedFunction>& a,
                const std::vector<ImportedFunction>& b) -> bool {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!functionsEqual(a[i], b[i])) return false;
    return true;
}

auto traitDefsEqual(const TraitDef& a, const TraitDef& b) -> bool {
    if (a.name != b.name) return false;
    if (a.requiredMethods.size() != b.requiredMethods.size()) return false;
    for (size_t i = 0; i < a.requiredMethods.size(); i++)
        if (!signaturesEqual(a.requiredMethods[i], b.requiredMethods[i]))
            return false;
    return a.defaultMethods == b.defaultMethods && a.defaultBodies == b.defaultBodies;
}

auto adtsEqual(const ImportedADT& a, const ImportedADT& b) -> bool {
    if (a.name != b.name) return false;
    if (a.constructors != b.constructors) return false;
    if (a.constructorArities != b.constructorArities) return false;
    if (a.typeParamCount != b.typeParamCount) return false;
    if (a.constructorTypeParamSlots != b.constructorTypeParamSlots)
        return false;
    if (a.typeParamNames != b.typeParamNames) return false;
    if (a.constructorParamTypes.size() != b.constructorParamTypes.size())
        return false;
    for (const auto& [ctor, types] : a.constructorParamTypes) {
        const auto found = b.constructorParamTypes.find(ctor);
        if (found == b.constructorParamTypes.end()) return false;
        if (!typeVecEqual(types, found->second)) return false;
    }
    return true;
}

auto distinctEqual(const ImportedDistinctType& a,
                   const ImportedDistinctType& b) -> bool {
    return a.typeParams == b.typeParams &&
           typesExactlyEqual(a.backingType, b.backingType);
}

auto modulesEqual(const ImportedModuleInterface& a,
                  const ImportedModuleInterface& b) -> bool {
    if (a.sourceModule != b.sourceModule) return false;
    if (a.backendModule != b.backendModule) return false;
    if (a.automaticImport != b.automaticImport) return false;
    if (a.isCapability != b.isCapability) return false;
    if (a.compiledApart != b.compiledApart) return false;
    if (a.exports.size() != b.exports.size()) return false;
    for (const auto& [member, fns] : a.exports) {
        const auto found = b.exports.find(member);
        if (found == b.exports.end()) return false;
        if (!fnVecEqual(fns, found->second)) return false;
    }
    return true;
}

auto interfacesEqual(const ImportedInterfaces& a, const ImportedInterfaces& b)
    -> bool {
    if (a.modules.size() != b.modules.size()) return false;
    for (const auto& [name, module] : a.modules) {
        const auto found = b.modules.find(name);
        if (found == b.modules.end()) return false;
        if (!modulesEqual(module, found->second)) return false;
    }
    if (a.receiverFunctions.size() != b.receiverFunctions.size()) return false;
    for (const auto& [name, fns] : a.receiverFunctions) {
        const auto found = b.receiverFunctions.find(name);
        if (found == b.receiverFunctions.end()) return false;
        if (!fnVecEqual(fns, found->second)) return false;
    }
    if (a.traitConformances.size() != b.traitConformances.size()) return false;
    for (size_t i = 0; i < a.traitConformances.size(); i++)
        if (a.traitConformances[i].typeName != b.traitConformances[i].typeName ||
            a.traitConformances[i].traitName !=
                b.traitConformances[i].traitName)
            return false;
    if (a.adts.size() != b.adts.size()) return false;
    for (size_t i = 0; i < a.adts.size(); i++)
        if (!adtsEqual(a.adts[i], b.adts[i])) return false;
    if (a.recordArities != b.recordArities) return false;
    if (a.recordFieldNames != b.recordFieldNames) return false;
    if (a.recordFields.size() != b.recordFields.size()) return false;
    for (const auto& [record, fields] : a.recordFields) {
        const auto found = b.recordFields.find(record);
        if (found == b.recordFields.end()) return false;
        if (fields.size() != found->second.size()) return false;
        for (const auto& [field, type] : fields) {
            const auto inner = found->second.find(field);
            if (inner == found->second.end()) return false;
            if (!typesExactlyEqual(type, inner->second)) return false;
        }
    }
    if (a.typeAliases.size() != b.typeAliases.size()) return false;
    for (const auto& [name, type] : a.typeAliases) {
        const auto found = b.typeAliases.find(name);
        if (found == b.typeAliases.end()) return false;
        if (!typesExactlyEqual(type, found->second)) return false;
    }
    if (a.typeNames != b.typeNames) return false;
    if (a.distinctTypes.size() != b.distinctTypes.size()) return false;
    for (const auto& [name, distinct] : a.distinctTypes) {
        const auto found = b.distinctTypes.find(name);
        if (found == b.distinctTypes.end()) return false;
        if (!distinctEqual(distinct, found->second)) return false;
    }
    if (a.traits.size() != b.traits.size()) return false;
    for (size_t i = 0; i < a.traits.size(); i++)
        if (!traitDefsEqual(a.traits[i], b.traits[i])) return false;
    return true;
}

// Every member populated, every Type alternative exercised: any field the
// encoder writes but the decoder drops (or vice versa) fails interfacesEqual.
auto buildAgreementFixture() -> ImportedInterfaces {
    ImportedInterfaces ifaces;

    Signature sig;
    sig.name = "parse";
    sig.params = {
        Type::named("String"),
        Type::optional(Type::named("Integer")),
        Type::integer(),
        Type::charT(),
        Type::string(),
        Type::boolean(),
        Type::atom(":macos"),
        Type::unit(),
        Type::int32(),
        Type::uint8(),
        Type::float32(),
        Type::float64(),
        Type::named("Result", {Type::named("Integer"), Type::named("String")}),
        Type::func({Type::named("A")}, Type::named("B"), true),
        Type::tuple({Type::named("A"), Type::named("B")}),
        Type::list(Type::charT()),
        Type::map(Type::named("K"), Type::named("V")),
        std::make_shared<Type>(
            Type{UnionType{{Type::named("A"), Type::named("B")}}}),
        Type::intersection({Type::named("A"), Type::named("B")}),
        Type::record({{"x", Type::integer()}, {"y", Type::string()}}),
        Type::typeVar(-1),
        Type::unknown(),
        Type::voidType("uninhabited"),
        Type::constrained("T", "Number", -1),
    };
    sig.result = Type::named("Outcome");
    sig.isFoul = true;
    sig.requiredParams = 2;
    sig.paramNames = {"input", "radix"};
    sig.makeModule = "Maker";
    sig.declaringModule = "Decl";

    ImportedFunction fn;
    fn.sourceName = "parse";
    fn.backendFunction = "parse";
    fn.backendModule = "kex_test";
    fn.backendArity = 2;
    fn.paramNames = {"input", "radix"};
    fn.defaultSources = {"", "\"10\""};
    fn.signature = sig;
    fn.sourceModule = "Test";
    fn.isReceiverMethod = true;
    fn.visibleByReceiver = true;
    fn.isConstructor = false;
    fn.remoteSlot = true;
    fn.constructorOwner = "";
    fn.implementation = "parse/String";
    fn.foreignTarget = true;

    ImportedFunction ctor = fn;
    ctor.sourceName = "Some";
    ctor.isReceiverMethod = false;
    ctor.visibleByReceiver = false;
    ctor.isConstructor = true;
    ctor.remoteSlot = false;
    ctor.constructorOwner = "Optional";
    ctor.signature.result = nullptr; // null survives as null, not UnknownType

    ImportedModuleInterface module;
    module.sourceModule = "Test";
    module.backendModule = "kex_test";
    module.automaticImport = true;
    module.isCapability = true;
    module.compiledApart = true;
    module.exports["parse"] = {fn, ctor};
    ifaces.modules["Test"] = module;
    ifaces.receiverFunctions["parse"].push_back(fn);

    ifaces.traitConformances.push_back({"Integer", "Number"});
    ifaces.traitConformances.push_back({"String", "Showable"});

    ImportedADT adt;
    adt.name = "Optional";
    adt.constructors = {"Some", "None"};
    adt.constructorArities = {{"Some", 1}, {"None", 0}};
    adt.typeParamCount = 1;
    adt.constructorTypeParamSlots = {{"Some", {0}}, {"None", {-1}}};
    adt.typeParamNames = {"T"};
    adt.constructorParamTypes = {{"Some", {Type::named("T")}}, {"None", {}}};
    ifaces.adts.push_back(adt);

    ifaces.recordArities = {{"Point", 2}};
    ifaces.recordFieldNames = {{"Point", {"x", "y"}}};
    ifaces.recordFields = {
        {"Point", {{"x", Type::integer()}, {"y", Type::integer()}}}};
    ifaces.typeAliases = {{"FilePath", Type::named("String")}};
    ifaces.typeNames = {"Optional", "Point", "List"};

    ImportedDistinctType distinct;
    distinct.typeParams = {"T"};
    distinct.backingType = Type::named("Integer");
    ifaces.distinctTypes["UserId"] = distinct;

    TraitDef trait;
    trait.name = "Comparable";
    Signature method;
    method.name = "compare";
    method.params = {Type::typeVar(-1)};
    method.result = Type::named("Ordering");
    trait.requiredMethods = {method};
    trait.defaultMethods = {"equals"};
    trait.defaultBodies = {"equals"};
    ifaces.traits.push_back(trait);

    return ifaces;
}

// Re-encode valid cache bytes with root tuple element `index` replaced —
// index 1 is the magic, 2 the version.
auto withRootElem(size_t index, kex::beam::TermPtr replacement,
                  const std::vector<uint8_t>& bytes) -> std::vector<uint8_t> {
    auto root = kex::beam::decodeEtf(bytes);
    auto elems = root->asTuple();
    elems[index] = std::move(replacement);
    return kex::beam::encodeEtf(kex::beam::Term::tuple(std::move(elems)));
}

// The interface cache is written through the shared atomic writer.
auto writeCache(const fs::path& path, const std::vector<uint8_t>& bytes) -> bool {
    return kex::cache::atomicWrite(
        path, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                               bytes.size()));
}

} // namespace

int main() {
    using namespace test;
    using namespace kex::semantic;

    describe("interface cache round-trip", []() {
        it("preserves an empty interface", []() {
            ImportedInterfaces ifaces;
            auto bytes = kex::beam::serializeImportedInterfaces(ifaces);
            auto round = kex::beam::deserializeImportedInterfaces(bytes);
            assertTrue(round.has_value());
            assertEqual(round->modules.size(), size_t{0});
            assertEqual(round->traits.size(), size_t{0});
            assertEqual(round->receiverFunctions.size(), size_t{0});
        });

        it("preserves modules, functions, and their signatures", []() {
            ImportedInterfaces ifaces;

            ImportedFunction fn;
            fn.sourceName = "parse";
            fn.backendFunction = "parse";
            fn.backendModule = "kex_test";
            fn.backendArity = 2;
            fn.paramNames = {"input", "radix"};
            fn.signature.name = "parse";
            fn.signature.params = {
                Type::named("String"),
                Type::optional(Type::named("Integer")),
            };
            fn.signature.result =
                Type::named("Result", {Type::named("Integer"),
                                       Type::named("String")});
            fn.signature.isFoul = true;
            fn.isReceiverMethod = true;

            ImportedModuleInterface module;
            module.sourceModule = "Test";
            module.backendModule = "kex_test";
            module.automaticImport = true;
            module.exports["parse"] = {fn};
            ifaces.modules["Test"] = std::move(module);
            ifaces.receiverFunctions["parse"].push_back(fn);

            auto bytes = kex::beam::serializeImportedInterfaces(ifaces);
            auto round = kex::beam::deserializeImportedInterfaces(bytes);
            assertTrue(round.has_value());

            auto mod = round->modules.find("Test");
            assertTrue(mod != round->modules.end());
            assertEqual(mod->second.backendModule, std::string("kex_test"));
            assertTrue(mod->second.automaticImport);

            auto exported = mod->second.exports.find("parse");
            assertTrue(exported != mod->second.exports.end());
            assertEqual(exported->second.size(), size_t{1});
            const auto& parsed = exported->second.front();
            assertEqual(parsed.backendFunction, std::string("parse"));
            assertEqual(parsed.backendArity, 2);
            assertTrue(parsed.signature.isFoul);
            assertTrue(parsed.isReceiverMethod);
            assertEqual(parsed.paramNames.size(), size_t{2});
            assertEqual(parsed.paramNames[0], std::string("input"));
            assertEqual(parsed.signature.params.size(), size_t{2});

            // A nested NamedType survives with its type arguments intact.
            auto result = parsed.signature.result;
            auto* named = std::get_if<NamedType>(&result->kind);
            assertTrue(named != nullptr);
            assertEqual(named->name, std::string("Result"));
            assertEqual(named->typeArgs.size(), size_t{2});

            // receiverFunctions map survived too.
            assertEqual(round->receiverFunctions.size(), size_t{1});
        });

        it("preserves traits and their required methods", []() {
            ImportedInterfaces ifaces;
            TraitDef trait;
            trait.name = "Comparable";
            Signature method;
            method.name = "compare";
            method.params = {Type::typeVar(-1)};
            method.result = Type::named("Ordering");
            trait.requiredMethods = {method};
            trait.defaultMethods = {"equals"};
            ifaces.traits.push_back(trait);

            auto bytes = kex::beam::serializeImportedInterfaces(ifaces);
            auto round = kex::beam::deserializeImportedInterfaces(bytes);
            assertTrue(round.has_value());
            assertEqual(round->traits.size(), size_t{1});
            assertEqual(round->traits.front().name, std::string("Comparable"));
            assertEqual(round->traits.front().requiredMethods.size(), size_t{1});
            assertEqual(round->traits.front().defaultMethods.size(), size_t{1});
            assertEqual(round->traits.front().defaultMethods[0],
                        std::string("equals"));
        });

        it("rejects corrupted or truncated input", []() {
            assertFalse(kex::beam::deserializeImportedInterfaces({}).has_value());
            std::vector<uint8_t> garbage{0xde, 0xad, 0xbe, 0xef, 0x00};
            assertFalse(kex::beam::deserializeImportedInterfaces(garbage).has_value());
        });
    });

    describe("interface cache schema agreement", []() {
        it("round-trips every member and every Type alternative exactly", []() {
            const auto ifaces = buildAgreementFixture();
            const auto bytes =
                kex::beam::serializeImportedInterfaces(ifaces);
            const auto round =
                kex::beam::deserializeImportedInterfaces(bytes);
            assertTrue(round.has_value());
            assertTrue(interfacesEqual(ifaces, *round),
                       "encoder and decoder disagree on some field");
        });

        it("keeps a null signature result null, not UnknownType", []() {
            ImportedInterfaces ifaces;
            ImportedFunction fn;
            fn.sourceName = "f";
            fn.signature.result = nullptr;
            ImportedModuleInterface module;
            module.exports["f"] = {fn};
            ifaces.modules["M"] = module;
            const auto bytes = kex::beam::serializeImportedInterfaces(ifaces);
            const auto round =
                kex::beam::deserializeImportedInterfaces(bytes);
            assertTrue(round.has_value());
            const auto& back =
                round->modules.at("M").exports.at("f").front();
            assertTrue(back.signature.result == nullptr);
        });
    });

    describe("interface cache miss reasons", []() {
        it("reports bad-version for a version-skewed file", []() {
            const auto bytes = kex::beam::serializeImportedInterfaces(
                buildAgreementFixture());
            const auto skewed =
                withRootElem(2, kex::beam::Term::integer(9999), bytes);
            kex::beam::CacheMissReason reason =
                kex::beam::CacheMissReason::Corrupt;
            assertFalse(kex::beam::deserializeImportedInterfaces(skewed, &reason)
                            .has_value());
            assertTrue(reason == kex::beam::CacheMissReason::BadVersion);
            assertEqual(std::string(kex::beam::cacheMissReasonName(reason)),
                        std::string("bad-version"));
        });

        it("reports bad-magic for a foreign magic", []() {
            const auto bytes = kex::beam::serializeImportedInterfaces(
                buildAgreementFixture());
            const auto foreign =
                withRootElem(1, kex::beam::Term::integer(0x1234), bytes);
            kex::beam::CacheMissReason reason =
                kex::beam::CacheMissReason::BadVersion;
            assertFalse(kex::beam::deserializeImportedInterfaces(foreign, &reason)
                            .has_value());
            assertTrue(reason == kex::beam::CacheMissReason::BadMagic);
        });

        it("reports corrupt for garbage, empty, and truncated input", []() {
            for (const auto& bad :
                 {std::vector<uint8_t>{},
                  std::vector<uint8_t>{0xde, 0xad, 0xbe, 0xef, 0x00}}) {
                kex::beam::CacheMissReason reason =
                    kex::beam::CacheMissReason::BadVersion;
                assertFalse(
                    kex::beam::deserializeImportedInterfaces(bad, &reason)
                        .has_value());
                assertTrue(reason == kex::beam::CacheMissReason::Corrupt);
            }
            auto bytes = kex::beam::serializeImportedInterfaces(
                buildAgreementFixture());
            bytes.resize(bytes.size() > 10 ? bytes.size() - 10 : 0);
            kex::beam::CacheMissReason reason =
                kex::beam::CacheMissReason::BadVersion;
            assertFalse(kex::beam::deserializeImportedInterfaces(bytes, &reason)
                            .has_value());
            assertTrue(reason == kex::beam::CacheMissReason::Corrupt);
        });
    });

    describe("toolchain fingerprint (the interface cache key)", []() {
        it("is stable and names this binary", []() {
            const auto& first = kex::cache::toolchainFingerprint();
            assertEqual(first, kex::cache::toolchainFingerprint());
            assertTrue(first.find("self:") != std::string::npos,
                       "a rebuilt compiler must mint a new key");
        });

        it("covers opt-in stdlib modules in subdirectories", []() {
            // Editing src/stdlib/net/http.kex once kept serving the old
            // interface: the key hashed only the top-level artifact sources.
            const auto& fingerprint = kex::cache::toolchainFingerprint();
            bool nested = false;
            for (const auto& file : kex::standardLibrarySourceFiles())
                if (fs::path(file).parent_path().filename() != "stdlib") {
                    nested = true;
                    assertTrue(fingerprint.find(file + "|") != std::string::npos,
                               "missing from the fingerprint: " + file);
                }
            assertTrue(nested, "expected opt-in modules under subdirectories");
        });
    });

    describe("cache location", []() {
        it("honours KEX_CACHE, and KEX_CACHE=off turns caching off", []() {
            const char* saved = std::getenv("KEX_CACHE");
            const std::string restore = saved ? saved : "";
            setenv("KEX_CACHE", "/tmp/kex-cache-probe", 1);
            assertTrue(kex::cache::directory("prelude-interface") ==
                       fs::path("/tmp/kex-cache-probe/prelude-interface"));
            for (const char* off : {"off", "0", ""}) {
                setenv("KEX_CACHE", off, 1);
                assertFalse(kex::cache::base().has_value());
            }
            if (saved) setenv("KEX_CACHE", restore.c_str(), 1);
            else unsetenv("KEX_CACHE");
        });
    });

    describe("interface cache eviction", []() {
        it("prunes to the newest entries and never the one just written",
           []() {
               char tmp[] = "/tmp/kex_iface_prune_XXXXXX";
               std::string dir = mkdtemp(tmp);
               const auto base = fs::file_time_type::clock::now();
               // Seven stale keys with distinct mtimes, oldest first.
               for (int i = 0; i < 7; i++) {
                   auto path = fs::path(dir) / ("key" + std::to_string(i) +
                                                ".bin");
                   { std::ofstream f(path, std::ios::binary); f << "x"; }
                   std::error_code ec;
                   fs::last_write_time(
                       path, base + std::chrono::seconds(i), ec);
               }
               // Noise the pruner must ignore.
               { std::ofstream f(fs::path(dir) / "notes.txt"); f << "x"; }
               fs::create_directory(fs::path(dir) / "subdir");

               // `keep` is deliberately NOT among the newest: it must
               // survive anyway, holding one of the five slots.
               const auto keep = fs::path(dir) / "key0.bin";
               kex::beam::pruneInterfaceCacheDirectory(dir, keep, 5);

               assertTrue(fs::exists(keep), "the just-written key was pruned");
               size_t bins = 0;
               for (const auto& entry : fs::directory_iterator(dir))
                   if (entry.path().extension() == ".bin") bins++;
               assertEqual(bins, size_t{5});
               assertTrue(fs::exists(fs::path(dir) / "key6.bin"));
               assertTrue(fs::exists(fs::path(dir) / "notes.txt"));
               assertTrue(fs::exists(fs::path(dir) / "subdir"));
               fs::remove_all(dir);
           });

        it("leaves a below-budget directory alone", []() {
            char tmp[] = "/tmp/kex_iface_prune_small_XXXXXX";
            std::string dir = mkdtemp(tmp);
            const auto keep = fs::path(dir) / "only.bin";
            { std::ofstream f(keep, std::ios::binary); f << "x"; }
            kex::beam::pruneInterfaceCacheDirectory(dir, keep, 5);
            assertTrue(fs::exists(keep));
            fs::remove_all(dir);
        });
    });

    describe("interface cache concurrent writers", []() {
        it("writes atomically and never leaves a torn file", []() {
            char tmp[] = "/tmp/kex_iface_race_XXXXXX";
            std::string dir = mkdtemp(tmp);
            ImportedInterfaces a;
            a.typeNames.insert("A-payload");
            ImportedInterfaces b;
            b.typeNames.insert("B-payload");
            const auto bytesA = kex::beam::serializeImportedInterfaces(a);
            const auto bytesB = kex::beam::serializeImportedInterfaces(b);
            const auto path = fs::path(dir) / "race.bin";

            assertTrue(writeCache(path, bytesA));
            std::vector<std::thread> threads;
            for (int t = 0; t < 8; t++)
                threads.emplace_back([&, t]() {
                    for (int i = 0; i < 25; i++)
                        writeCache(
                            path, (t + i) % 2 ? bytesA : bytesB);
                });
            for (auto& th : threads) th.join();

            // Atomic rename means the survivor is exactly one payload —
            // never a mix of the two.
            const auto survived = kex::readFileBytes(path).value_or(std::vector<uint8_t>{});
            assertTrue(survived == bytesA || survived == bytesB,
                       "torn cache file under concurrent writers");
            assertTrue(kex::beam::deserializeImportedInterfaces(survived)
                           .has_value());
            for (const auto& entry : fs::directory_iterator(dir))
                assertTrue(entry.path().string().find(".tmp.") ==
                               std::string::npos,
                           "staging leftover: " + entry.path().string());
            fs::remove_all(dir);
        });
    });

    return runAll();
}
