#include "test.hxx"
#include "../src/beam/beam_file.hxx"
#include "../src/beam/collect_metadata.hxx"
#include "../src/beam/etf.hxx"
#include "../src/beam/kexi.hxx"
#include "../src/beam/kexi_registry.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include "../src/semantic/analyzer.hxx"
#include <filesystem>

using namespace kex::beam;

int main() {
    // ── ETF round-trip tests ──────────────────────────────────────────

    test::describe("ETF encoding", []() {
        test::it("round-trips atom", []() {
            auto t = Term::atom("hello");
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            test::assertTrue(decoded->isAtom("hello"));
        });

        test::it("round-trips small integer", []() {
            auto t = Term::integer(42);
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            test::assertEqual(decoded->asInt(), int64_t(42));
        });

        test::it("round-trips negative integer", []() {
            auto t = Term::integer(-1000);
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            test::assertEqual(decoded->asInt(), int64_t(-1000));
        });

        test::it("round-trips large integer via SMALL_BIG_EXT", []() {
            int64_t large = int64_t(1) << 40;
            auto t = Term::integer(large);
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            test::assertEqual(decoded->asInt(), large);
        });

        test::it("round-trips binary", []() {
            auto t = Term::binary("hello world");
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            test::assertEqual(decoded->asBinaryStr(), std::string("hello world"));
        });

        test::it("round-trips empty list", []() {
            auto t = Term::list({});
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            test::assertTrue(decoded->asList().empty());
        });

        test::it("round-trips list of atoms", []() {
            auto t = Term::list({Term::atom("a"), Term::atom("b")});
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            test::assertEqual(decoded->asList().size(), size_t(2));
            test::assertTrue(decoded->asList()[0]->isAtom("a"));
            test::assertTrue(decoded->asList()[1]->isAtom("b"));
        });

        test::it("round-trips tuple", []() {
            auto t = Term::tuple({Term::atom("ok"), Term::integer(1)});
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            auto& elems = decoded->asTuple();
            test::assertEqual(elems.size(), size_t(2));
            test::assertTrue(elems[0]->isAtom("ok"));
            test::assertEqual(elems[1]->asInt(), int64_t(1));
        });

        test::it("round-trips map", []() {
            auto t = Term::map({{Term::atom("key"), Term::binary("value")}});
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            auto val = decoded->mapGet("key");
            test::assertTrue(val != nullptr);
            test::assertEqual(val->asBinaryStr(), std::string("value"));
        });

        test::it("round-trips nested structure", []() {
            auto t = Term::tuple({
                Term::atom("kexi"),
                Term::integer(1),
                Term::list({Term::tuple({Term::binary("foo"), Term::integer(2)})}),
            });
            auto bytes = encodeEtf(t);
            auto decoded = decodeEtf(bytes);
            auto& top = decoded->asTuple();
            test::assertTrue(top[0]->isAtom("kexi"));
            test::assertEqual(top[1]->asInt(), int64_t(1));
            auto& list = top[2]->asList();
            test::assertEqual(list.size(), size_t(1));
            test::assertEqual(list[0]->asTuple()[0]->asBinaryStr(), std::string("foo"));
        });

        test::it("rejects unsupported ETF version", []() {
            std::vector<uint8_t> bad = {99, 0}; // version 99
            try {
                decodeEtf(bad);
                test::assertTrue(false, "should have thrown");
            } catch (const EtfError&) {}
        });
    });

    // ── BEAM file round-trip tests ────────────────────────────────────

    test::describe("BEAM file I/O", []() {
        test::it("round-trips a minimal BEAM file", []() {
            BeamFile bf;
            bf.chunks.push_back({"AtU8", {1, 2, 3}});
            bf.chunks.push_back({"Code", {4, 5, 6, 7}});
            auto bytes = writeBeamFile(bf);
            auto decoded = readBeamFile(bytes);
            test::assertEqual(decoded.chunks.size(), size_t(2));
            test::assertEqual(decoded.chunks[0].id, std::string("AtU8"));
            test::assertEqual(decoded.chunks[0].data.size(), size_t(3));
            test::assertEqual(decoded.chunks[1].id, std::string("Code"));
            test::assertEqual(decoded.chunks[1].data.size(), size_t(4));
        });

        test::it("setChunk replaces existing chunk", []() {
            BeamFile bf;
            bf.chunks.push_back({"KexI", {1}});
            bf.setChunk("KexI", {2, 3});
            test::assertEqual(bf.chunks.size(), size_t(1));
            test::assertEqual(bf.chunks[0].data.size(), size_t(2));
        });

        test::it("setChunk adds new chunk", []() {
            BeamFile bf;
            bf.chunks.push_back({"AtU8", {1}});
            bf.setChunk("KexI", {2});
            test::assertEqual(bf.chunks.size(), size_t(2));
            test::assertEqual(bf.findChunk("KexI")->data[0], uint8_t(2));
        });

        test::it("preserves padding on round-trip", []() {
            BeamFile bf;
            bf.chunks.push_back({"Test", {1, 2, 3}}); // 3 bytes → 1 byte padding
            bf.chunks.push_back({"Tes2", {4, 5}});    // 2 bytes → 2 bytes padding
            auto bytes = writeBeamFile(bf);
            auto decoded = readBeamFile(bytes);
            test::assertEqual(decoded.chunks[0].data.size(), size_t(3));
            test::assertEqual(decoded.chunks[0].data[0], uint8_t(1));
            test::assertEqual(decoded.chunks[1].data.size(), size_t(2));
            test::assertEqual(decoded.chunks[1].data[0], uint8_t(4));
        });

        test::it("rejects non-BEAM file", []() {
            std::vector<uint8_t> bad = {'N', 'O', 'P', 'E'};
            try {
                readBeamFile(bad);
                test::assertTrue(false, "should have thrown");
            } catch (const BeamError&) {}
        });
    });

    // ── KexI chunk round-trip tests ───────────────────────────────────

    test::describe("KexI serialization", []() {
        test::it("round-trips empty chunk", []() {
            KexiChunk chunk;
            chunk.metadata.unitId = "example/test";
            chunk.metadata.sourceModule = "Test";
            chunk.metadata.moduleAtom = "kex_test";
            chunk.metadata.role = KexiModuleRole::Entry;
            chunk.interfaceHash = computeInterfaceHash(chunk);

            auto bytes = serializeKexi(chunk);
            auto decoded = deserializeKexi(bytes);

            test::assertEqual(decoded.version, KEXI_SCHEMA_VERSION);
            test::assertEqual(decoded.metadata.unitId, std::string("example/test"));
            test::assertEqual(decoded.metadata.sourceModule, std::string("Test"));
            test::assertEqual(decoded.metadata.moduleAtom, std::string("kex_test"));
            test::assertTrue(decoded.typeInterface.exports.empty());
            test::assertTrue(decoded.metadata.companions.empty());
        });

        test::it("round-trips exports", []() {
            KexiChunk chunk;
            chunk.metadata.moduleAtom = "kex_test";
            KexiExport exp;
            exp.name = "greet";
            exp.beamFunction = "greet_impl";
            exp.beamArity = 1;
            exp.isFoul = false;
            exp.paramTypes = {kexiPrimitive("String")};
            exp.returnType = kexiPrimitive("String");
            chunk.typeInterface.exports.push_back(std::move(exp));
            chunk.interfaceHash = computeInterfaceHash(chunk);

            auto bytes = serializeKexi(chunk);
            auto decoded = deserializeKexi(bytes);

            test::assertEqual(decoded.typeInterface.exports.size(), size_t(1));
            test::assertEqual(decoded.typeInterface.exports[0].name, std::string("greet"));
            test::assertEqual(decoded.typeInterface.exports[0].beamFunction,
                              std::string("greet_impl"));
            test::assertEqual(decoded.typeInterface.exports[0].beamArity, 1);
            test::assertEqual(decoded.typeInterface.exports[0].paramTypes[0]->name,
                              std::string("String"));
        });

        test::it("round-trips ADTs with constructors", []() {
            KexiChunk chunk;
            chunk.metadata.moduleAtom = "Kex.BinaryTree";
            KexiADT adt;
            adt.name = "Tree";
            adt.typeParams = {"a"};
            adt.constructors.push_back({"Empty", "Empty", {}, 0});
            adt.constructors.push_back({"Node", "Node", {"value", "left", "right"}, 3});
            chunk.metadata.adts.push_back(std::move(adt));
            chunk.interfaceHash = computeInterfaceHash(chunk);

            auto bytes = serializeKexi(chunk);
            auto decoded = deserializeKexi(bytes);

            test::assertEqual(decoded.metadata.adts.size(), size_t(1));
            auto& tree = decoded.metadata.adts[0];
            test::assertEqual(tree.name, std::string("Tree"));
            test::assertEqual(tree.typeParams.size(), size_t(1));
            test::assertEqual(tree.constructors.size(), size_t(2));
            test::assertEqual(tree.constructors[0].name, std::string("Empty"));
            test::assertEqual(tree.constructors[0].arity, 0);
            test::assertEqual(tree.constructors[1].name, std::string("Node"));
            test::assertEqual(tree.constructors[1].arity, 3);
            test::assertEqual(tree.constructors[1].fieldNames.size(), size_t(3));
        });

        test::it("round-trips companion with back-pointer", []() {
            KexiChunk chunk;
            chunk.metadata.unitId = "example/binary-tree";
            chunk.metadata.sourceModule = "BinaryTree";
            chunk.metadata.moduleAtom = "Kex.BinaryTree";
            chunk.metadata.role = KexiModuleRole::Companion;
            chunk.metadata.entryBackPointer = "kex_binary_tree";
            chunk.interfaceHash = computeInterfaceHash(chunk);

            auto bytes = serializeKexi(chunk);
            auto decoded = deserializeKexi(bytes);

            test::assertTrue(decoded.metadata.role == KexiModuleRole::Companion);
            test::assertEqual(decoded.metadata.unitId,
                              std::string("example/binary-tree"));
            test::assertEqual(decoded.metadata.sourceModule,
                              std::string("BinaryTree"));
            test::assertEqual(decoded.metadata.entryBackPointer,
                              std::string("kex_binary_tree"));
        });

        test::it("reads version 1 ownership conservatively", []() {
            KexiChunk chunk;
            chunk.version = 1;
            chunk.metadata.moduleAtom = "Kex.Legacy";
            chunk.metadata.role = KexiModuleRole::Companion;
            chunk.metadata.entryBackPointer = "kex_legacy";
            KexiExport exp;
            exp.name = "run";
            exp.beamFunction = "ignored_in_v1";
            exp.returnType = kexiPrimitive("Unit");
            chunk.typeInterface.exports.push_back(std::move(exp));

            auto decoded = deserializeKexi(serializeKexi(chunk));
            test::assertEqual(decoded.version, 1);
            test::assertEqual(decoded.metadata.unitId, std::string("kex_legacy"));
            test::assertEqual(decoded.metadata.sourceModule, std::string("Legacy"));
            test::assertEqual(decoded.typeInterface.exports[0].beamFunction,
                              std::string("run"));
        });

        test::it("round-trips entry with companion manifest", []() {
            KexiChunk chunk;
            chunk.metadata.moduleAtom = "kex_test";
            chunk.metadata.role = KexiModuleRole::Entry;
            Hash128 hash = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
            chunk.metadata.companions.push_back({"Kex.Foo", "Kex.Foo.beam", hash});
            chunk.interfaceHash = computeInterfaceHash(chunk);

            auto bytes = serializeKexi(chunk);
            auto decoded = deserializeKexi(bytes);

            test::assertEqual(decoded.metadata.companions.size(), size_t(1));
            test::assertEqual(decoded.metadata.companions[0].beamAtom,
                              std::string("Kex.Foo"));
            test::assertEqual(decoded.metadata.companions[0].relativePath,
                              std::string("Kex.Foo.beam"));
            test::assertTrue(decoded.metadata.companions[0].expectedHash == hash,
                            "expected hash should match");
        });

        test::it("round-trips entry package policy", []() {
            KexiChunk chunk;
            chunk.metadata.unitId = "stdlib-artifact";
            chunk.metadata.sourceModule = "Core";
            chunk.metadata.moduleAtom = "kex_stdlib";
            chunk.metadata.package.id = "kex/stdlib";
            chunk.metadata.package.unitIds = {"stdlib-artifact"};
            chunk.metadata.package.automaticImports = {"Core"};
            chunk.metadata.package.receiverProviders = {"Core", "Collections"};

            auto decoded = deserializeKexi(serializeKexi(chunk));
            test::assertEqual(decoded.metadata.package.id,
                              std::string("kex/stdlib"));
            test::assertEqual(decoded.metadata.package.unitIds.size(), size_t(1));
            test::assertEqual(decoded.metadata.package.automaticImports[0],
                              std::string("Core"));
            test::assertEqual(decoded.metadata.package.receiverProviders.size(),
                              size_t(2));
        });

        test::it("hash is stable across identical payloads", []() {
            KexiChunk c1, c2;
            c1.metadata.moduleAtom = "kex_test";
            c2.metadata.moduleAtom = "kex_test";
            auto h1 = computeInterfaceHash(c1);
            auto h2 = computeInterfaceHash(c2);
            test::assertTrue(h1 == h2, "identical chunks should have same hash");
        });

        test::it("hash changes when content changes", []() {
            KexiChunk c1, c2;
            c1.metadata.moduleAtom = "kex_test";
            c2.metadata.moduleAtom = "kex_other";
            auto h1 = computeInterfaceHash(c1);
            auto h2 = computeInterfaceHash(c2);
            test::assertTrue(h1 != h2, "hashes should differ for different content");
        });

        test::it("rejects unsupported schema version", []() {
            KexiChunk chunk;
            chunk.version = 999;
            chunk.metadata.moduleAtom = "kex_test";
            auto bytes = serializeKexi(chunk);
            try {
                deserializeKexi(bytes);
                test::assertTrue(false, "should have thrown");
            } catch (const EtfError& e) {
                std::string msg = e.what();
                test::assertTrue(msg.find("unsupported KexI schema version") != std::string::npos);
            }
        });

        test::it("round-trips methods with receiver types", []() {
            KexiChunk chunk;
            chunk.metadata.moduleAtom = "Kex.BinaryTree";
            KexiMethod m;
            m.name = "insert";
            m.receiverType = kexiNamed("Tree", {kexiNamed("a")});
            m.beamArity = 2;
            m.isFoul = false;
            m.paramTypes = {kexiNamed("a")};
            m.returnType = kexiNamed("Tree", {kexiNamed("a")});
            m.beamFunction = "BinaryTree.insert";
            chunk.typeInterface.methods.push_back(std::move(m));
            chunk.interfaceHash = computeInterfaceHash(chunk);

            auto bytes = serializeKexi(chunk);
            auto decoded = deserializeKexi(bytes);

            test::assertEqual(decoded.typeInterface.methods.size(), size_t(1));
            auto& dm = decoded.typeInterface.methods[0];
            test::assertEqual(dm.name, std::string("insert"));
            test::assertEqual(dm.beamArity, 2);
            test::assertEqual(dm.beamFunction, std::string("BinaryTree.insert"));
            test::assertEqual(dm.receiverType->name, std::string("Tree"));
            test::assertEqual(dm.receiverType->typeArgs.size(), size_t(1));
        });

        test::it("round-trips records with field types", []() {
            KexiChunk chunk;
            chunk.metadata.moduleAtom = "kex_test";
            KexiRecord rec;
            rec.name = "Point";
            rec.fields.push_back({"x", kexiPrimitive("Integer")});
            rec.fields.push_back({"y", kexiPrimitive("Integer")});
            chunk.metadata.records.push_back(std::move(rec));
            chunk.interfaceHash = computeInterfaceHash(chunk);

            auto bytes = serializeKexi(chunk);
            auto decoded = deserializeKexi(bytes);

            test::assertEqual(decoded.metadata.records.size(), size_t(1));
            auto& r = decoded.metadata.records[0];
            test::assertEqual(r.name, std::string("Point"));
            test::assertEqual(r.fields.size(), size_t(2));
            test::assertEqual(r.fields[0].name, std::string("x"));
            test::assertEqual(r.fields[0].type->name, std::string("Integer"));
        });

        test::it("round-trips complex type terms", []() {
            KexiChunk chunk;
            chunk.metadata.moduleAtom = "kex_test";
            KexiExport exp;
            exp.name = "complex";
            exp.beamArity = 1;
            exp.paramTypes = {
                kexiFunc({kexiPrimitive("Integer")}, kexiPrimitive("Bool")),
            };
            exp.returnType = kexiOptional(kexiList(
                kexiMap(kexiPrimitive("String"), kexiUnion({
                    kexiPrimitive("Integer"),
                    kexiNamed("Tree", {kexiNamed("a")}),
                }))));
            chunk.typeInterface.exports.push_back(std::move(exp));
            chunk.interfaceHash = computeInterfaceHash(chunk);

            auto bytes = serializeKexi(chunk);
            auto decoded = deserializeKexi(bytes);

            auto& rt = decoded.typeInterface.exports[0].returnType;
            test::assertTrue(rt->kind == KexiType::Optional);
            test::assertTrue(rt->typeArgs[0]->kind == KexiType::List);
            test::assertTrue(rt->typeArgs[0]->typeArgs[0]->kind == KexiType::Map);
        });
    });

    test::describe("KexI analyzed interfaces", []() {
        test::it("collects flattened nested-module metadata", []() {
            kex::Lexer lexer(
                "module Web do\n"
                "  module Types do\n"
                "    let build(path: String) = path\n"
                "    record Request do\n"
                "      path: String\n"
                "    end\n"
                "  end\n"
                "end\n");
            kex::Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            CollectOptions options;
            options.moduleAtom = "kex_flattened";
            options.flattenModules = true;
            auto chunk = collectMetadata(program, options);
            test::assertEqual(chunk.metadata.records.size(), size_t(1));
            test::assertEqual(chunk.metadata.records[0].name,
                              std::string("Request"));
            test::assertEqual(chunk.metadata.records[0].fields[0].name,
                              std::string("path"));
            test::assertEqual(chunk.typeInterface.exports.size(), size_t(1));
            test::assertEqual(chunk.typeInterface.exports[0].name,
                              std::string("Web.Types.build"));
            test::assertEqual(chunk.typeInterface.exports[0].beamFunction,
                              std::string("Web__Types__build"));
        });

        test::it("uses inferred function results instead of syntax-only unknowns", []() {
            kex::Lexer lexer(
                "let double(n: Integer) = n * 2\n"
                "let answer() = 42\n");
            kex::Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            kex::semantic::Analyzer analyzer;
            test::assertTrue(analyzer.analyze(program),
                             "fixture should pass semantic analysis");

            CollectOptions options;
            options.moduleAtom = "kex_inferred";
            options.analysis = &analyzer;
            auto chunk = collectMetadata(program, options);
            test::assertEqual(chunk.typeInterface.exports.size(), size_t(2));

            const auto& doubleExport = chunk.typeInterface.exports[0];
            test::assertEqual(doubleExport.name, std::string("double"));
            test::assertEqual(doubleExport.paramTypes.size(), size_t(1));
            test::assertTrue(doubleExport.paramTypes[0]->kind == KexiType::Primitive);
            test::assertEqual(doubleExport.paramTypes[0]->name,
                              std::string("Integer"));
            test::assertTrue(doubleExport.returnType->kind == KexiType::Primitive);
            test::assertEqual(doubleExport.returnType->name,
                              std::string("Integer"));

            auto decoded = deserializeKexi(serializeKexi(chunk));
            test::assertEqual(decoded.typeInterface.exports[0].returnType->name,
                              std::string("Integer"));
            test::assertEqual(decoded.typeInterface.exports[1].returnType->name,
                              std::string("Integer"));
        });

        test::it("falls back to annotations when analysis is unavailable", []() {
            kex::Lexer lexer("let identity(value: String) -> String = value\n");
            kex::Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            CollectOptions options;
            options.moduleAtom = "kex_syntax_fallback";
            auto chunk = collectMetadata(program, options);
            test::assertEqual(chunk.typeInterface.exports.size(), size_t(1));
            test::assertEqual(chunk.typeInterface.exports[0].paramTypes[0]->name,
                              std::string("String"));
            test::assertEqual(chunk.typeInterface.exports[0].returnType->name,
                              std::string("String"));
        });

        test::it("keeps same-named module exports attached to their declarations", []() {
            kex::Lexer lexer(
                "module Numbers do\n"
                "  let value() = 42\n"
                "end\n"
                "module Flags do\n"
                "  let value() = true\n"
                "end\n");
            kex::Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            kex::semantic::Analyzer analyzer;
            test::assertTrue(analyzer.analyze(program),
                             "fixture should pass semantic analysis");

            CollectOptions numberOptions;
            numberOptions.unitId = "fixture/modules";
            numberOptions.moduleAtom = "kex_numbers";
            numberOptions.moduleName = "Numbers";
            numberOptions.analysis = &analyzer;
            auto numbers = collectMetadata(program, numberOptions);
            test::assertEqual(numbers.metadata.unitId,
                              std::string("fixture/modules"));
            test::assertEqual(numbers.metadata.sourceModule,
                              std::string("Numbers"));
            test::assertEqual(numbers.typeInterface.exports[0].beamFunction,
                              std::string("value"));
            test::assertEqual(numbers.typeInterface.exports[0].returnType->name,
                              std::string("Integer"));

            CollectOptions flagOptions;
            flagOptions.moduleAtom = "kex_flags";
            flagOptions.moduleName = "Flags";
            flagOptions.analysis = &analyzer;
            auto flags = collectMetadata(program, flagOptions);
            test::assertEqual(flags.typeInterface.exports[0].returnType->name,
                              std::string("Bool"));
        });

        test::it("uses analyzed receiver-function results", []() {
            kex::Lexer lexer(
                "make Integer do\n"
                "  let doubled = this * 2\n"
                "end\n");
            kex::Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            kex::semantic::Analyzer analyzer;
            test::assertTrue(analyzer.analyze(program),
                             "fixture should pass semantic analysis");

            CollectOptions options;
            options.moduleAtom = "kex_integer_extension";
            options.analysis = &analyzer;
            auto chunk = collectMetadata(program, options);
            test::assertEqual(chunk.typeInterface.methods.size(), size_t(1));
            test::assertEqual(chunk.typeInterface.methods[0].name,
                              std::string("doubled"));
            test::assertEqual(chunk.typeInterface.methods[0].beamFunction,
                              std::string("doubled"));
            test::assertEqual(chunk.typeInterface.methods[0].returnType->name,
                              std::string("Integer"));
            test::assertEqual(chunk.metadata.methodOwnership[0].beamFunction,
                              std::string("doubled"));
        });

        test::it("collects trait default receiver functions", []() {
            kex::Lexer lexer(
                "trait Enumerable do\n"
                "  let map(f) = ()\n"
                "end\n");
            kex::Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();

            CollectOptions options;
            options.moduleAtom = "kex_trait_defaults";
            options.flattenModules = true;
            auto chunk = collectMetadata(program, options);

            test::assertEqual(chunk.typeInterface.methods.size(), size_t(1));
            const auto& method = chunk.typeInterface.methods[0];
            test::assertEqual(method.name, std::string("map"));
            test::assertEqual(method.beamFunction, std::string("map"));
            test::assertEqual(method.beamArity, 2);
            test::assertTrue(method.receiverType->kind == KexiType::Constrained);
            test::assertEqual(method.receiverType->name, std::string("T"));
            test::assertEqual(method.receiverType->traitName,
                              std::string("Enumerable"));
            test::assertEqual(chunk.metadata.methodOwnership.size(), size_t(1));
            test::assertEqual(chunk.metadata.methodOwnership[0].methodName,
                              std::string("map"));
        });
    });

    test::describe("package interface policy", []() {
        test::it("validates package-owned automatic modules", []() {
            kex::module::PackageMetadata package;
            package.id = "example/stdlib";
            package.unitIds = {"stdlib-artifact"};
            package.automaticImports = {"Core"};
            package.receiverProviders = {"Collections"};
            std::vector<kex::module::PackageModuleIdentity> available = {
                {"stdlib-artifact", "Core"},
                {"stdlib-artifact", "Collections"},
                {"application-artifact", "Application"},
            };
            test::assertTrue(
                kex::module::validatePackageMetadata(package, available).empty());

            package.receiverProviders.push_back("Application");
            auto errors = kex::module::validatePackageMetadata(package, available);
            test::assertEqual(errors.size(), size_t(1));
            test::assertTrue(errors[0].message.find("unit it does not own") !=
                             std::string::npos);
        });

        test::it("only searches declared receiver-function providers", []() {
            namespace fs = std::filesystem;
            auto root = fs::temp_directory_path() / "kex-package-policy-test";
            fs::remove_all(root);
            fs::create_directories(root);
            auto beamPath = (root / "fixture.kx.beam").string();

            KexiChunk chunk;
            chunk.metadata.unitId = "fixture-artifact";
            chunk.metadata.sourceModule = "Numbers";
            chunk.metadata.moduleAtom = "kex_fixture";
            KexiMethod doubled;
            doubled.name = "doubled";
            doubled.beamFunction = "doubled";
            doubled.receiverType = kexiPrimitive("Integer");
            doubled.returnType = kexiPrimitive("Integer");
            chunk.typeInterface.methods.push_back(std::move(doubled));
            KexiExport answer;
            answer.name = "answer";
            answer.beamFunction = "answer";
            answer.returnType = kexiPrimitive("Integer");
            chunk.typeInterface.exports.push_back(std::move(answer));
            KexiExport narrow;
            narrow.name = "narrow";
            narrow.beamFunction = "narrow";
            narrow.paramTypes = {kexiPrimitive("Int32")};
            narrow.returnType = kexiPrimitive("Float");
            narrow.beamArity = 1;
            chunk.typeInterface.exports.push_back(std::move(narrow));

            BeamFile beam;
            beam.setChunk(KEXI_CHUNK_ID, serializeKexi(chunk));
            writeBeamFile(beam, beamPath);

            KexiRegistry registry;
            test::assertTrue(registry.loadUnit(beamPath).empty());
            test::assertTrue(
                registry.findMethodsForReceiver("doubled", "Integer").empty(),
                "loaded modules are not implicit receiver providers");
            test::assertTrue(
                registry.buildExternalModules().receiverFunctions.empty(),
                "IR receiver routing is not populated implicitly");
            test::assertTrue(
                registry.buildSemanticInterfaces().receiverFunctions.empty(),
                "semantic receiver lookup is not populated implicitly");

            kex::module::PackageMetadata package;
            package.id = "example/numbers";
            package.unitIds = {"fixture-artifact"};
            package.automaticImports = {"Numbers"};
            package.receiverProviders = {"Numbers"};
            test::assertTrue(registry.declarePackage(package).empty());
            test::assertEqual(registry.automaticImportModules().size(), size_t(1));
            auto matches = registry.findMethodsForReceiver("doubled", "Integer");
            test::assertEqual(matches.size(), size_t(1));
            test::assertEqual(matches[0].first, std::string("kex_fixture"));
            auto external = registry.buildExternalModules();
            test::assertEqual(external.receiverFunctions["doubled"].size(),
                              size_t(1));
            test::assertEqual(
                external.receiverFunctions["doubled"][0].moduleAtom,
                std::string("kex_fixture"));
            auto semantic = registry.buildSemanticInterfaces();
            test::assertTrue(semantic.modules.count("Numbers") > 0);
            test::assertTrue(semantic.modules["Numbers"].automaticImport);
            test::assertEqual(semantic.modules["Numbers"].backendModule,
                              std::string("kex_fixture"));
            test::assertEqual(
                semantic.modules["Numbers"].exports["answer"].size(), size_t(1));
            const auto& narrowSignature =
                semantic.modules["Numbers"].exports["narrow"][0].signature;
            test::assertEqual(
                kex::semantic::typeToString(narrowSignature.params[0]),
                std::string("Int32"));
            test::assertTrue(std::holds_alternative<
                kex::semantic::ConstrainedType>(narrowSignature.result->kind));
            test::assertEqual(semantic.receiverFunctions["doubled"].size(),
                              size_t(1));
            const auto& receiver = semantic.receiverFunctions["doubled"][0];
            test::assertEqual(receiver.backendModule,
                              std::string("kex_fixture"));
            test::assertEqual(receiver.signature.params.size(), size_t(1));
            test::assertTrue(std::holds_alternative<
                kex::semantic::PrimitiveType>(receiver.signature.params[0]->kind));
            test::assertEqual(
                kex::semantic::typeToString(receiver.signature.result),
                std::string("Integer"));

            auto competing = package;
            competing.id = "example/competing";
            auto conflict = registry.declarePackage(competing);
            test::assertEqual(conflict.size(), size_t(1));
            test::assertTrue(conflict[0].message.find("already owned") !=
                             std::string::npos);

            fs::remove_all(root);
        });

        test::it("loads package policy from an entry module", []() {
            namespace fs = std::filesystem;
            auto root = fs::temp_directory_path() / "kex-embedded-package-test";
            fs::remove_all(root);
            fs::create_directories(root);
            auto beamPath = (root / "stdlib.kx.beam").string();

            KexiChunk chunk;
            chunk.metadata.unitId = "stdlib-artifact";
            chunk.metadata.sourceModule = "Core";
            chunk.metadata.moduleAtom = "kex_stdlib";
            chunk.metadata.package.id = "kex/stdlib";
            chunk.metadata.package.unitIds = {"stdlib-artifact"};
            chunk.metadata.package.automaticImports = {"Core"};
            chunk.metadata.package.receiverProviders = {"Core"};
            KexiMethod identity;
            identity.name = "identity";
            identity.beamFunction = "identity";
            identity.receiverType = kexiPrimitive("Integer");
            identity.returnType = kexiPrimitive("Integer");
            chunk.typeInterface.methods.push_back(std::move(identity));

            BeamFile beam;
            beam.setChunk(KEXI_CHUNK_ID, serializeKexi(chunk));
            writeBeamFile(beam, beamPath);

            KexiRegistry registry;
            test::assertTrue(registry.loadUnit(beamPath).empty());
            test::assertEqual(registry.automaticImportModules().size(), size_t(1));
            test::assertEqual(
                registry.findMethodsForReceiver("identity", "Integer").size(),
                size_t(1));

            fs::remove_all(root);
        });
    });

    // ── Integration: real BEAM file ───────────────────────────────────

    test::describe("KexI in real BEAM files", []() {
        test::it("can attach and read back KexI from a BEAM file", []() {
            // Create a minimal BEAM file
            BeamFile bf;
            bf.chunks.push_back({"AtU8", {0, 0, 0, 1, 4, 't', 'e', 's', 't'}});
            bf.chunks.push_back({"Code", {0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 1, 1, 3, 153, 0}});

            KexiChunk chunk;
            chunk.metadata.moduleAtom = "test_mod";
            chunk.metadata.role = KexiModuleRole::Entry;
            KexiExport hello;
            hello.name = "hello";
            hello.beamFunction = "hello";
            hello.returnType = kexiPrimitive("String");
            chunk.typeInterface.exports.push_back(std::move(hello));
            chunk.interfaceHash = computeInterfaceHash(chunk);
            auto payload = serializeKexi(chunk);

            bf.setChunk(KEXI_CHUNK_ID, std::move(payload));
            auto bytes = writeBeamFile(bf);
            auto decoded = readBeamFile(bytes);

            auto* kexiChunk = decoded.findChunk(KEXI_CHUNK_ID);
            test::assertTrue(kexiChunk != nullptr, "KexI chunk should exist");

            auto kc = deserializeKexi(kexiChunk->data);
            test::assertEqual(kc.metadata.moduleAtom, std::string("test_mod"));
            test::assertEqual(kc.typeInterface.exports.size(), size_t(1));
            test::assertEqual(kc.typeInterface.exports[0].name, std::string("hello"));
        });
    });

    return test::runAll();
}
