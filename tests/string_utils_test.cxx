#include "test.hxx"
#include "../src/common/repl_commands.hxx"
#include "../src/common/string_utils.hxx"

int main() {
    using namespace test;
    using namespace kex;

    describe("kex::lowered", []() {
        it("lowercases a mixed-case string", []() {
            assertEqual(lowered("HTTP"), std::string("http"));
            assertEqual(lowered("MiXeD"), std::string("mixed"));
        });
        it("keeps lowercase unchanged", []() {
            assertEqual(lowered("http"), std::string("http"));
        });
    });

    describe("kex::snakeCased", []() {
        it("converts CamelCase to snake_case", []() {
            assertEqual(snakeCased("MockData"), std::string("mock_data"));
        });
        it("keeps an acronym run together except the trailing boundary", []() {
            assertEqual(snakeCased("HTTPServer"), std::string("http_server"));
        });
        it("inserts a boundary before a digit", []() {
            assertEqual(snakeCased("MockData2"), std::string("mock_data2"));
        });
        it("keeps lowercase unchanged", []() {
            assertEqual(snakeCased("atom"), std::string("atom"));
        });
    });

    describe("kex::parseReplCommand", []() {
        using K = ReplCommandKind;
        it("reads the command word, its alias, and a trimmed argument", []() {
            assertTrue(parseReplCommand("/help").kind == K::Help);
            assertTrue(parseReplCommand("/h").kind == K::Help);
            const auto load = parseReplCommand("/load   lib/a.kex  ");
            assertTrue(load.kind == K::Load);
            assertEqual(load.argument, std::string("lib/a.kex"));
            assertTrue(parseReplCommand("/set").kind == K::Set);
            assertTrue(parseReplCommand("/set").argument.empty());
        });
        it("leaves ordinary input and unknown commands alone", []() {
            assertTrue(parseReplCommand("1 + 2").kind == K::None);
            assertTrue(parseReplCommand("/nope").kind == K::None);
            assertTrue(parseReplCommand("/loader").kind == K::None);
        });
        it("knows every exit spelling", []() {
            for (const auto* exit : {"exit", "/exit", "/quit", "/q"})
                assertTrue(parseReplCommand(exit).kind == K::Exit);
        });
    });

    describe("kex::join", []() {
        it("joins with the separator", []() {
            assertEqual(join({"Html", "Element"}, "."), std::string("Html.Element"));
            assertEqual(join({}, "."), std::string());
        });
        it("joins only the first count parts", []() {
            assertEqual(join({"A", "B", "C"}, ".", 2), std::string("A.B"));
            assertEqual(join({"A"}, ".", 5), std::string("A"));
        });
    });

    return runAll();
}
