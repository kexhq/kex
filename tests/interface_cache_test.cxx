#include "test.hxx"
#include "../src/beam/interface_cache.hxx"
#include "../src/semantic/imported_interfaces.hxx"
#include "../src/semantic/traits.hxx"
#include "../src/semantic/types.hxx"

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

    return runAll();
}
