#include "test.hxx"
#include "../src/common/mangle.hxx"

int main() {
    using namespace test;
    using namespace kex::mangle;

    describe("mangle::qualify", []() {
        it("joins a scope and name with the separator", []() {
            assertEqual(qualify("Module", "name"), std::string("Module.name"));
        });
        it("returns the bare name for an empty scope", []() {
            assertEqual(qualify("", "name"), std::string("name"));
        });
        it("spells a member of a dotted scope as a program does", []() {
            assertEqual(qualify("Net.HTTP", "get"), std::string("Net.HTTP.get"));
        });
    });

    describe("mangle::scopeOf / memberOf", []() {
        it("splits at the last separator", []() {
            assertEqual(scopeOf("A.B.member"), std::string("A.B"));
            assertEqual(memberOf("A.B.member"), std::string("member"));
        });
        it("handles a name with no separator", []() {
            assertEqual(scopeOf("plain"), std::string(""));
            assertEqual(memberOf("plain"), std::string("plain"));
        });
        it("gives back what qualify joined", []() {
            const auto name = qualify("Net.HTTP", "get");
            assertEqual(scopeOf(name), std::string("Net.HTTP"));
            assertEqual(memberOf(name), std::string("get"));
        });
    });

    describe("mangle::hasScope", []() {
        it("detects the separator", []() {
            assertTrue(hasScope("A.b"));
            assertFalse(hasScope("plain"));
            assertFalse(hasScope(""));
        });
    });

    return runAll();
}
