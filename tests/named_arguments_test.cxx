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

int main() {
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
