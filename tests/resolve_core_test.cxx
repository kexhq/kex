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
            assertTrue(unique.kind == SuffixMatchKind::unique);
            assertEqual(unique.name, std::string("Data.Set"));
            const std::unordered_map<std::string, int> none{
                {"Set", 1}, {"Other", 2}};
            assertTrue(matchSuffix(none, ".Set").kind ==
                       SuffixMatchKind::none);
            const std::unordered_map<std::string, int> two{
                {"Data.Set", 1}, {"Units.Set", 2}};
            const auto ambiguous = matchSuffix(two, ".Set");
            assertTrue(ambiguous.kind == SuffixMatchKind::ambiguous);
            assertTrue(ambiguous.name.empty());
        });
    });

    return runAll();
}
