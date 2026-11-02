#include "test.hxx"
#include "../src/ast/ast.hxx"
#include "../src/common/diagnostics.hxx"
#include "../src/common/color.hxx"

int main() {
    using namespace test;
    using namespace kex::diagnostics;

    describe("diagnostics::jsonEscape", []() {
        it("escapes quotes and backslashes", []() {
            assertEqual(jsonEscape("a\"b\\c"), std::string("a\\\"b\\\\c"));
        });
        it("escapes control characters", []() {
            assertEqual(jsonEscape("a\nb\rc\td"), std::string("a\\nb\\rc\\td"));
        });
        it("leaves plain text unchanged", []() {
            assertEqual(jsonEscape("hello"), std::string("hello"));
        });
    });

    describe("diagnostics::extractHint", []() {
        it("extracts the did-you-mean suggestion", []() {
            assertEqual(extractHint("Undefined `foo`; did you mean `bar`?"),
                        std::string("bar"));
        });
        it("returns empty when there is no hint", []() {
            assertEqual(extractHint("Undefined `foo`"), std::string(""));
        });
    });

    describe("diagnostics::errorCountPhrase", []() {
        it("pluralizes", []() {
            assertEqual(errorCountPhrase(3, "type"), std::string("3 type errors"));
        });
        it("singularizes", []() {
            assertEqual(errorCountPhrase(1, "type"), std::string("1 type error"));
        });
    });

    describe("diagnostics::typeNameToString", []() {
        it("joins parts with a dot", []() {
            kex::ast::TypeName tn{{"Foo", "Bar"}};
            assertEqual(typeNameToString(tn), std::string("Foo.Bar"));
        });
    });

    describe("diagnostics::typeExprToString", []() {
        it("renders a plain name", []() {
            kex::ast::TypeExpr te;
            te.kind = kex::ast::TypeName{{"List"}};
            assertEqual(typeExprToString(te), std::string("List"));
        });
        it("renders a qualified name", []() {
            kex::ast::TypeExpr te;
            te.kind = kex::ast::TypeName{{"Net", "HTTP"}};
            assertEqual(typeExprToString(te), std::string("Net.HTTP"));
        });
    });

    describe("diagnostics::colorizeMessage", []() {
        it("is the identity on plain text when color is disabled", []() {
            kex::color::enabled = false;
            assertEqual(colorizeMessage("plain message"), std::string("plain message"));
            assertEqual(colorizeMessage("a -> b"), std::string("a -> b"));
        });
        it("preserves backticks when color is disabled", []() {
            kex::color::enabled = false;
            assertEqual(colorizeMessage("`foo`"), std::string("`foo`"));
        });
    });

    return runAll();
}
