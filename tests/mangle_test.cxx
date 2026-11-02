#include "test.hxx"
#include "../src/common/mangle.hxx"

int main() {
    using namespace test;
    using namespace kex::mangle;

    describe("mangle::qualify", []() {
        it("joins a scope and name with the separator", []() {
            assertEqual(qualify("Module", "name"), std::string("Module::name"));
        });
        it("returns the bare name for an empty scope", []() {
            assertEqual(qualify("", "name"), std::string("name"));
        });
        it("preserves a dotted scope", []() {
            assertEqual(qualify("Net.HTTP", "get"), std::string("Net.HTTP::get"));
        });
    });

    describe("mangle::headOf / tailOf", []() {
        it("splits at the first separator", []() {
            assertEqual(headOf("Module::name"), std::string("Module"));
            assertEqual(tailOf("Module::name"), std::string("name"));
        });
        it("returns the whole name when there is no separator", []() {
            assertEqual(headOf("plain"), std::string("plain"));
            assertEqual(tailOf("plain"), std::string(""));
        });
    });

    describe("mangle::scopeOf / memberOf", []() {
        it("splits at the last separator", []() {
            assertEqual(scopeOf("A::B::member"), std::string("A::B"));
            assertEqual(memberOf("A::B::member"), std::string("member"));
        });
        it("handles a name with no separator", []() {
            assertEqual(scopeOf("plain"), std::string(""));
            assertEqual(memberOf("plain"), std::string("plain"));
        });
    });

    describe("mangle::hasScope", []() {
        it("detects the separator", []() {
            assertTrue(hasScope("A::b"));
            assertFalse(hasScope("plain"));
            assertFalse(hasScope(""));
        });
    });

    describe("mangle::manglePath", []() {
        it("maps dots to the separator", []() {
            assertEqual(manglePath("Net.HTTP"), std::string("Net::HTTP"));
        });
        it("leaves an undotted name unchanged", []() {
            assertEqual(manglePath("Atom"), std::string("Atom"));
        });
    });

    return runAll();
}
