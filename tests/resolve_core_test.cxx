#include "test.hxx"
#include "../src/semantic/analyzer.hxx"
#include "../src/semantic/db.hxx"
#include "../src/semantic/resolve_core.hxx"

using namespace test;
using namespace kex::semantic;
using namespace kex::semantic::resolve_core;

static auto originOf(const std::vector<ImportScope>& scopes,
                     const std::string& name) -> const ImportOrigin* {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
        if (auto found = it->find(name); found != it->end())
            return &found->second;
    return nullptr;
}

static auto receiverFn(const std::string& name, const std::string& module,
                       int arity, TypePtr receiver,
                       std::vector<TypePtr> rest = {}) -> ImportedFunction {
    ImportedFunction fn;
    fn.sourceName = name;
    fn.backendFunction = name;
    fn.backendModule = module;
    fn.backendArity = arity;
    fn.sourceModule = module;
    fn.isReceiverMethod = true;
    std::vector<TypePtr> params = {std::move(receiver)};
    for (auto& type : rest) params.push_back(std::move(type));
    fn.signature = {name, std::move(params), Type::unknown(), false};
    return fn;
}

int main() {
    describe("resolve_core::mergeImportOrigin", []() {
        it("inserts a first import into the current scope", []() {
            std::vector<ImportScope> scopes(1);
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Headers", false, false});
            assertTrue(merged.proceed);
            assertFalse(merged.ambiguity.has_value());
            const auto* origin = originOf(scopes, "get");
            assertTrue(origin != nullptr);
            assertEqual(origin->module, std::string("Headers"));
            assertFalse(origin->explicitImport);
            assertFalse(origin->receiverMethod);
        });
        it("creates the current scope when the stack is empty", []() {
            std::vector<ImportScope> scopes;
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Headers", false, false});
            assertTrue(merged.proceed);
            assertEqual(scopes.size(), size_t(1));
            assertTrue(originOf(scopes, "get") != nullptr);
        });
        it("leaves a same-module re-import unchanged", []() {
            std::vector<ImportScope> scopes(1);
            mergeImportOrigin(scopes, "get", {"Headers", false, false});
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Headers", true, false});
            assertTrue(merged.proceed);
            assertFalse(merged.ambiguity.has_value());
            assertFalse(originOf(scopes, "get")->explicitImport);
        });
        it("lets a receiver method stand beside a bare owner", []() {
            std::vector<ImportScope> scopes(1);
            mergeImportOrigin(scopes, "get", {"Headers", false, false});
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Dsl", false, true});
            assertTrue(merged.proceed);
            assertFalse(merged.ambiguity.has_value());
            assertEqual(originOf(scopes, "get")->module,
                        std::string("Headers"));
        });
        it("lets a bare import claim a receiver-method name", []() {
            std::vector<ImportScope> scopes(1);
            mergeImportOrigin(scopes, "get", {"Dsl", false, true});
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Headers", false, false});
            assertTrue(merged.proceed);
            const auto* origin = originOf(scopes, "get");
            assertEqual(origin->module, std::string("Headers"));
            assertFalse(origin->receiverMethod);
        });
        it("prefers an explicit import over a whole-module one", []() {
            std::vector<ImportScope> scopes(1);
            mergeImportOrigin(scopes, "get", {"Headers", false, false});
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Dsl", true, false});
            assertTrue(merged.proceed);
            const auto* origin = originOf(scopes, "get");
            assertEqual(origin->module, std::string("Dsl"));
            assertTrue(origin->explicitImport);
        });
        it("keeps a standing explicit import over a whole-module one", []() {
            std::vector<ImportScope> scopes(1);
            mergeImportOrigin(scopes, "get", {"Headers", true, false});
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Dsl", false, false});
            assertFalse(merged.proceed);
            assertFalse(merged.ambiguity.has_value());
            assertEqual(originOf(scopes, "get")->module,
                        std::string("Headers"));
        });
        it("reports two whole-module bare claims as ambiguous", []() {
            std::vector<ImportScope> scopes(1);
            mergeImportOrigin(scopes, "get", {"Headers", false, false});
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Dsl", false, false});
            assertFalse(merged.proceed);
            assertTrue(merged.ambiguity.has_value());
            assertEqual(*merged.ambiguity,
                        std::string("ambiguous name `get`, imported from "
                                    "both `Headers` and `Dsl`"));
        });
        it("reports two explicit bare claims as ambiguous", []() {
            std::vector<ImportScope> scopes(1);
            mergeImportOrigin(scopes, "get", {"Headers", true, false});
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Dsl", true, false});
            assertFalse(merged.proceed);
            assertTrue(merged.ambiguity.has_value());
        });
        it("overwrites the standing origin in its own scope", []() {
            std::vector<ImportScope> scopes(2);
            mergeImportOrigin(scopes, "get", {"Headers", false, false});
            scopes.emplace_back();
            const auto merged = mergeImportOrigin(
                scopes, "get", {"Dsl", true, false});
            assertTrue(merged.proceed);
            assertEqual(scopes[1].at("get").module, std::string("Dsl"));
            assertTrue(scopes[2].find("get") == scopes[2].end());
        });
    });

    describe("resolve_core::usingImportsNames", []() {
        it("imports names unless it only binds an alias", []() {
            assertTrue(usingImportsNames(std::nullopt, {}));
            assertTrue(usingImportsNames(std::nullopt, {"get"}));
            assertFalse(usingImportsNames(std::optional<std::string>("H"), {}));
            assertTrue(usingImportsNames(std::optional<std::string>("H"),
                                         {"get"}));
        });
    });

    describe("resolve_core::usingSelectsName", []() {
        it("picks with only:, removes with except:", []() {
            assertTrue(usingSelectsName("get", {}, {}));
            assertTrue(usingSelectsName("get", {"get"}, {}));
            assertFalse(usingSelectsName("put", {"get"}, {}));
            assertFalse(usingSelectsName("get", {}, {"get"}));
            assertTrue(usingSelectsName("put", {}, {"get"}));
            assertFalse(usingSelectsName("get", {"get"}, {"get"}));
        });
    });

    describe("resolve_core::resolveSourceModulePath", []() {
        it("resolves as written, relatively, and leaves misses alone", []() {
            SemanticDB db;
            db.updateFile("<test>", "module Outer do\n"
                                    "  module Inner do\n"
                                    "    let helper(x) = x\n"
                                    "  end\n"
                                    "end\n"
                                    "module Top do\n"
                                    "  let top(x) = x\n"
                                    "end\n");
            assertEqual(resolveSourceModulePath(&db, "", "Outer.Inner"),
                        std::string("Outer.Inner"));
            assertEqual(resolveSourceModulePath(&db, "Outer", "Inner"),
                        std::string("Outer.Inner"));
            assertEqual(resolveSourceModulePath(&db, "", "Nope"),
                        std::string("Nope"));
            assertEqual(resolveSourceModulePath(nullptr, "", "Outer"),
                        std::string("Outer"));
        });
    });

    describe("resolve_core::resolveInScopes", []() {
        it("walks innermost first and strips one segment at a time", []() {
            std::vector<std::string> probed;
            auto exists = [&](const std::string& candidate) {
                probed.push_back(candidate);
                return candidate == "Outer.Reply";
            };
            const auto hit =
                resolveInScopes("Outer.Inner", "Reply", exists);
            assertTrue(hit.has_value());
            assertEqual(*hit, std::string("Outer.Reply"));
            assertEqual(probed.size(), size_t(2));
            assertEqual(probed[0], std::string("Outer.Inner.Reply"));
            assertEqual(probed[1], std::string("Outer.Reply"));
        });
        it("returns nullopt on a miss or an empty module", []() {
            auto never = [](const std::string&) { return false; };
            assertFalse(
                resolveInScopes("Outer", "Nope", never).has_value());
            assertFalse(resolveInScopes("", "Nope", never).has_value());
        });
    });

    describe("resolve_core::matchSuffix", []() {
        it("wins only on exactly one longer match", []() {
            const std::unordered_map<std::string, int> one{
                {"Data.Set", 1}, {"Other", 2}};
            const auto unique = matchSuffix(one, ".Set");
            assertTrue(unique.kind == SuffixMatchKind::Unique);
            assertEqual(unique.name, std::string("Data.Set"));
            const std::unordered_map<std::string, int> none{
                {"Set", 1}, {"Other", 2}};
            assertTrue(matchSuffix(none, ".Set").kind ==
                       SuffixMatchKind::None);
            const std::unordered_map<std::string, int> two{
                {"Data.Set", 1}, {"Units.Set", 2}};
            const auto ambiguous = matchSuffix(two, ".Set");
            assertTrue(ambiguous.kind == SuffixMatchKind::Ambiguous);
            assertTrue(ambiguous.name.empty());
        });
    });

    describe("resolve_core::writtenTargetHead", []() {
        it("spells primitives by canonical name", []() {
            const auto integer = writtenTargetHead(Type::integer());
            assertTrue(integer.has_value());
            assertEqual(integer->spelling, std::string("Integer"));
            assertFalse(integer->nominal);
            assertEqual(writtenTargetHead(Type::charT())->spelling,
                        std::string("Char"));
            assertEqual(writtenTargetHead(Type::string())->spelling,
                        std::string("String"));
            assertEqual(writtenTargetHead(Type::boolean())->spelling,
                        std::string("Bool"));
            assertEqual(writtenTargetHead(Type::atom())->spelling,
                        std::string("Atom"));
        });
        it("rejects atom literals, unit, and sized numerics", []() {
            assertFalse(writtenTargetHead(Type::atom("ok")).has_value());
            assertFalse(writtenTargetHead(Type::unit()).has_value());
            assertFalse(writtenTargetHead(Type::int64()).has_value());
            assertFalse(writtenTargetHead(Type::uint8()).has_value());
            assertFalse(writtenTargetHead(Type::float64()).has_value());
        });
        it("spells nominals as written except Block and Optional", []() {
            const auto point =
                writtenTargetHead(Type::named("Point"));
            assertTrue(point.has_value());
            assertEqual(point->spelling, std::string("Point"));
            assertTrue(point->nominal);
            const auto dotted =
                writtenTargetHead(Type::named("M.Point"));
            assertTrue(dotted.has_value());
            assertTrue(dotted->nominal);
            assertFalse(
                writtenTargetHead(Type::named("Block")).has_value());
            assertFalse(
                writtenTargetHead(Type::named("Optional")).has_value());
        });
        it("spells List and Map kinds, nothing else", []() {
            assertEqual(
                writtenTargetHead(Type::list(Type::integer()))->spelling,
                std::string("List"));
            assertFalse(
                writtenTargetHead(Type::list(Type::integer()))->nominal);
            assertEqual(writtenTargetHead(Type::map(Type::string(),
                                                    Type::integer()))
                            ->spelling,
                        std::string("Map"));
            assertFalse(writtenTargetHead(Type::optional(Type::integer()))
                            .has_value());
            assertFalse(writtenTargetHead(Type::tuple({})).has_value());
            assertFalse(
                writtenTargetHead(Type::func({}, Type::integer()))
                    .has_value());
            assertFalse(writtenTargetHead(Type::record({})).has_value());
            assertFalse(writtenTargetHead(Type::typeVar(-1)).has_value());
            assertFalse(writtenTargetHead(Type::unknown()).has_value());
            assertFalse(
                writtenTargetHead(Type::constrained("T", "Showable"))
                    .has_value());
        });
    });

    describe("resolve_core::receiverCoversHead", []() {
        it("matches primitives exactly", []() {
            const ImportedInterfaces ifaces;
            assertTrue(receiverCoversHead(Type::integer(), "Integer",
                                          ifaces));
            assertFalse(receiverCoversHead(Type::integer(), "String",
                                           ifaces));
            assertTrue(receiverCoversHead(Type::atom("ok"), "Atom",
                                          ifaces));
            assertFalse(receiverCoversHead(Type::unit(), "Void", ifaces));
            assertFalse(
                receiverCoversHead(Type::int64(), "Int64", ifaces));
        });
        it("matches nominals exactly or by last segment", []() {
            const ImportedInterfaces ifaces;
            assertTrue(receiverCoversHead(Type::named("M.Point"),
                                          "M.Point", ifaces));
            assertTrue(receiverCoversHead(Type::named("M.Point"), "Point",
                                          ifaces));
            assertTrue(receiverCoversHead(Type::named("Point"),
                                          "M.Point", ifaces));
            assertFalse(receiverCoversHead(Type::named("Point"), "Line",
                                           ifaces));
        });
        it("matches List and Map by head only", []() {
            const ImportedInterfaces ifaces;
            assertTrue(receiverCoversHead(
                Type::list(Type::integer()), "List", ifaces));
            assertTrue(receiverCoversHead(
                Type::map(Type::string(), Type::integer()), "Map",
                ifaces));
            assertFalse(receiverCoversHead(Type::list(Type::integer()),
                                           "Map", ifaces));
        });
        it("matches structural receivers by record membership", []() {
            ImportedInterfaces ifaces;
            ifaces.recordFields["M.Point"]["x"] = Type::integer();
            assertTrue(receiverCoversHead(
                Type::record({{"x", Type::integer()}}), "M.Point",
                ifaces));
            assertTrue(receiverCoversHead(
                Type::record({{"x", Type::integer()}}), "Point",
                ifaces));
            assertFalse(receiverCoversHead(
                Type::record({{"x", Type::integer()}}), "Line",
                ifaces));
            assertFalse(receiverCoversHead(
                Type::record({{"x", Type::integer()}}), "List",
                ifaces));
        });
        it("covers nothing for unions, vars, and the unknown", []() {
            const ImportedInterfaces ifaces;
            assertFalse(receiverCoversHead(Type::typeVar(3), "List",
                                           ifaces));
            assertFalse(receiverCoversHead(Type::unknown(), "List",
                                           ifaces));
        });
    });

    describe("resolve_core::importDispatchSpelling", []() {
        // Which head a value reaches through a dispatcher (shadowed owners,
        // wider guards, definitions merged ahead) is the emitter's record now
        // (`implementation`), tested in codegen_test's "dispatcher routes".
        it("honors KEX_NO_DEVIRT", []() {
            const auto winner =
                receiverFn("first", "kex_prelude", 1,
                           Type::list(Type::typeVar(-1)));
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("first", "kex_prelude", 1, Type::integer()),
            };
            const ImportedInterfaces ifaces;
            setenv("KEX_NO_DEVIRT", "1", 1);
            const auto spelling = importDispatchSpelling(
                winner, Type::list(Type::integer()),
                ifaces, {});
            unsetenv("KEX_NO_DEVIRT");
            assertFalse(spelling.has_value());
        });
        it("fires for a sole covering head in a collision", []() {
            auto winner =
                receiverFn("first", "kex_prelude", 1,
                           Type::list(Type::typeVar(-1)));
            // Its module emitted `first/List` (recordImplementations).
            winner.implementation = "first/List";
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("first", "kex_prelude", 1, Type::integer()),
            };
            const ImportedInterfaces ifaces;
            const auto spelling = importDispatchSpelling(
                winner, Type::list(Type::integer()),
                ifaces, {});
            assertTrue(spelling.has_value());
            assertEqual(*spelling, std::string("first/List"));
        });
        it("skips an undeclared nominal head (1b)", []() {
            const auto winner = receiverFn("area", "kex_prelude", 1,
                                           Type::named("Elm"));
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("area", "kex_prelude", 1, Type::integer()),
            };
            const ImportedInterfaces ifaces;
            assertFalse(importDispatchSpelling(winner,
                                               Type::named("Elm"),
                                               ifaces, {})
                            .has_value());
        });
        it("fires for a qualified record head", []() {
            auto winner = receiverFn("area", "kex_prelude", 1,
                                           Type::named("M.Point"));
            // Its module emitted `area/M.Point` (recordImplementations).
            winner.implementation = "area/M.Point";
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("area", "kex_prelude", 1, Type::integer()),
            };
            ImportedInterfaces ifaces;
            ifaces.recordFields["M.Point"]["x"] = Type::integer();
            const auto spelling = importDispatchSpelling(
                winner,
                Type::record({{"x", Type::integer()}}), ifaces, {});
            assertTrue(spelling.has_value());
            assertEqual(*spelling, std::string("area/M.Point"));
        });
        it("fires for a bare ADT head", []() {
            auto winner = receiverFn("parse", "kex_prelude", 1,
                                           Type::named("Status"));
            // Its module emitted `parse/Status` (recordImplementations).
            winner.implementation = "parse/Status";
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("parse", "kex_prelude", 1, Type::integer()),
            };
            ImportedInterfaces ifaces;
            ImportedADT adt;
            adt.name = "Status";
            ifaces.adts.push_back(std::move(adt));
            const auto spelling = importDispatchSpelling(
                winner, Type::named("Status"), ifaces,
                {});
            assertTrue(spelling.has_value());
            assertEqual(*spelling, std::string("parse/Status"));
        });
        it("skips module functions and remote slots (0)", []() {
            auto moduleFn = receiverFn("text", "kex_prelude", 1,
                                       Type::string());
            moduleFn.isReceiverMethod = false;
            auto slot = receiverFn("close", "kex_prelude", 1,
                                   Type::named("Handle"));
            slot.remoteSlot = true;
            const std::vector<ImportedFunction> candidates = {
                moduleFn, slot,
                receiverFn("text", "kex_prelude", 1, Type::integer()),
            };
            const ImportedInterfaces ifaces;
            assertFalse(importDispatchSpelling(moduleFn,
                                               Type::string(), ifaces,
                                               {})
                            .has_value());
            assertFalse(importDispatchSpelling(slot,
                                               Type::named("Handle"),
                                               ifaces, {})
                            .has_value());
        });
        it("skips gradual receivers (0b)", []() {
            const auto winner =
                receiverFn("first", "kex_prelude", 1,
                           Type::list(Type::typeVar(-1)));
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("first", "kex_prelude", 1, Type::integer()),
            };
            const ImportedInterfaces ifaces;
            assertFalse(importDispatchSpelling(
                winner, Type::list(Type::unknown()),
                ifaces, {})
                            .has_value());
        });
        it("expands call-site aliases before covering", []() {
            auto winner = receiverFn("trim", "kex_prelude", 1,
                                           Type::string());
            // Its module emitted `trim/String` (recordImplementations).
            winner.implementation = "trim/String";
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("trim", "kex_prelude", 1, Type::integer()),
            };
            const ImportedInterfaces ifaces;
            AliasExpander expand = [](const TypePtr& type) {
                if (const auto* named =
                        std::get_if<NamedType>(&type->kind);
                    named && named->name == "FilePath")
                    return Type::string();
                return type;
            };
            const auto spelling = importDispatchSpelling(
                winner, Type::named("FilePath"), ifaces,
                expand);
            assertTrue(spelling.has_value());
            assertEqual(*spelling, std::string("trim/String"));
        });
        it("skips foul winners (context registry misses)", []() {
            auto winner = receiverFn("get", "kex_prelude", 2,
                                     Type::named("Client"),
                                     {Type::string()});
            winner.signature.isFoul = true;
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("get", "kex_prelude", 2,
                           Type::named("Headers"), {Type::string()}),
            };
            ImportedInterfaces ifaces;
            ifaces.recordFields["Client"]["base"] = Type::string();
            ifaces.recordFields["Headers"]["list"] = Type::string();
            assertFalse(importDispatchSpelling(winner,
                                               Type::named("Client"),
                                               ifaces, {})
                            .has_value());
        });
        it("fires when the winner's module emitted name/Head", []() {
            auto winner =
                receiverFn("first", "kex_prelude", 1,
                           Type::list(Type::typeVar(-1)));
            // Its module emitted `first/List` (recordImplementations).
            winner.implementation = "first/List";
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("first", "kex_prelude", 2, Type::integer(),
                           {Type::integer()}),
            };
            const ImportedInterfaces ifaces;
            const auto spelling = importDispatchSpelling(
                winner, Type::list(Type::integer()),
                ifaces, {});
            assertTrue(spelling.has_value());
            assertEqual(*spelling, std::string("first/List"));
        });
        it("keeps the dispatcher when no name/Head was emitted", []() {
            const auto winner = receiverFn("first", "mod_a", 1,
                                           Type::list(Type::typeVar(-1)));
            const std::vector<ImportedFunction> candidates = {
                winner,
                receiverFn("first", "mod_b", 1, Type::integer()),
            };
            const ImportedInterfaces ifaces;
            assertFalse(importDispatchSpelling(
                winner, Type::list(Type::integer()),
                ifaces, {})
                            .has_value());
        });
    });

    return runAll();
}
