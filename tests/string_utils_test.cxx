#include "test.hxx"
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

    return runAll();
}
