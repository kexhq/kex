#include "test.hxx"
#include "../src/common/named_arguments.hxx"

using namespace test;
using namespace kex;

static auto assertSlots(const std::vector<size_t>& actual,
                        const std::vector<size_t>& expected) -> void {
    assertEqual(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
        assertEqual(actual[i], expected[i]);
}

static auto clauseWith(std::vector<std::string> names,
                       std::vector<bool> defaulted) -> ast::FunctionClause {
    ast::FunctionClause clause;
    for (size_t i = 0; i < names.size(); ++i) {
        ast::Param param;
        param.name = names[i];
        if (i < defaulted.size() && defaulted[i])
            param.defaultValue = std::make_unique<ast::Expr>();
        clause.params.push_back(std::move(param));
    }
    return clause;
}

int main() {
    describe("selectClause", []() {
        it("returns the first clause the arguments fit", []() {
            auto narrow = clauseWith({"a"}, {false});
            auto wide = clauseWith({"a", "b"}, {false, true});
            const std::vector<const ast::FunctionClause*> clauses{&narrow,
                                                                  &wide};
            assertTrue(selectClause(clauses, {}, 1) == &narrow);
            assertTrue(selectClause(clauses, {"b"}, 1) == &wide);
        });
        it("returns nullptr when no clause fits", []() {
            auto only = clauseWith({"a"}, {false});
            const std::vector<const ast::FunctionClause*> clauses{&only};
            assertTrue(selectClause(clauses, {"zzz"}, 0) == nullptr);
            assertTrue(selectClause(clauses, {}, 5) == nullptr);
            const std::vector<const ast::FunctionClause*> empty;
            assertTrue(selectClause(empty, {}, 0) == nullptr);
        });
    });

    describe("placeArguments", []() {
        it("places labels by name and positionals in the slots left", []() {
            const auto placed = placeArguments({"a", "b", "c"}, {"c"}, 1);
            assertSlots(placed.labelSlots,
                        std::vector<size_t>{2});
            assertSlots(placed.positionalSlots,
                        std::vector<size_t>{0});
        });
        it("reports unknown labels and overflowing positionals as npos",
           []() {
               const auto placed = placeArguments({"a"}, {"zzz"}, 2);
               assertSlots(placed.labelSlots,
                           std::vector<size_t>{PlacedArguments::npos});
               assertSlots(placed.positionalSlots,
                           std::vector<size_t>{
                               0, PlacedArguments::npos});
           });
        it("reserves front slots for a pre-placed receiver", []() {
            const auto placed =
                placeArguments({"self", "a", "b"}, {"b"}, 1, 1);
            assertSlots(placed.labelSlots,
                        std::vector<size_t>{2});
            assertSlots(placed.positionalSlots,
                        std::vector<size_t>{1});
        });
        it("lands a repeated label where the first one did", []() {
            const auto placed =
                placeArguments({"a", "b"}, {"a", "a"}, 0);
            assertSlots(placed.labelSlots,
                        std::vector<size_t>{0, 0});
        });
        it("fills several positionals front to back around labels", []() {
            const auto placed =
                placeArguments({"a", "b", "c", "d"}, {"b"}, 3);
            assertSlots(placed.positionalSlots,
                        std::vector<size_t>{0, 2, 3});
        });
    });

    return runAll();
}
