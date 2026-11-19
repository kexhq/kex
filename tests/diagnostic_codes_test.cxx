// The diagnostic registry (src/common/diagnostic_codes/, kexhq/kex#656):
// every code well formed and used once, no retired code back in use, and
// every template's slots written so a translation can fill them.
#include "test.hxx"
#include "../src/common/diagnostic_codes.hxx"
#include <cctype>
#include <set>
#include <string>

using namespace test;

namespace {

auto wellFormed(std::string_view id) -> bool {
    // KEX, E or W, a stage letter, a dash and four digits.
    const std::string_view stages = "LPMFSTCB";
    if (id.size() != 10 || id.substr(0, 3) != "KEX" || (id[3] != 'E' && id[3] != 'W') ||
        stages.find(id[4]) == std::string_view::npos || id[5] != '-') {
        return false;
    }
    for (std::size_t i = 6; i < 10; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(id[i]))) {
            return false;
        }
    }
    return id.substr(6) != "0000";
}

auto identifier(const std::string& name) -> bool {
    if (name.empty() || !std::isalpha(static_cast<unsigned char>(name[0]))) {
        return false;
    }
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    describe("the diagnostic registry", []() {
        it("has entries", []() { assertTrue(!kex::diag::entries().empty()); });

        it("writes every code as KEX, a severity, a stage and four digits", []() {
            for (const auto& entry : kex::diag::entries()) {
                assertTrue(wellFormed(entry.id), std::string(entry.id));
            }
            for (const auto id : kex::diag::retired()) {
                assertTrue(wellFormed(id), std::string(id));
            }
        });

        it("gives each code and each name to one entry", []() {
            std::set<std::string_view> ids;
            std::set<std::string_view> names;
            for (const auto& entry : kex::diag::entries()) {
                assertTrue(
                    ids.insert(entry.id).second, "code used twice: " + std::string(entry.id));
                assertTrue(
                    names.insert(entry.name).second, "name used twice: " + std::string(entry.name));
            }
        });

        it("never reuses a retired code", []() {
            std::set<std::string_view> retired;
            for (const auto id : kex::diag::retired()) {
                assertTrue(retired.insert(id).second, "retired twice: " + std::string(id));
            }
            for (const auto& entry : kex::diag::entries()) {
                assertFalse(
                    retired.contains(entry.id), "retired code reused: " + std::string(entry.id));
            }
        });

        it("writes templates with named slots, balanced braces and no em dash", []() {
            for (const auto& entry : kex::diag::entries()) {
                const auto text = std::string(entry.english);
                assertFalse(text.empty(), std::string(entry.id));
                assertTrue(text.find("—") == std::string::npos, "em dash in " + text);
                int depth = 0;
                for (char c : text) {
                    depth += c == '{' ? 1 : c == '}' ? -1 : 0;
                    assertTrue(depth == 0 || depth == 1, "unbalanced braces in " + text);
                }
                assertEqual(depth, 0, "unbalanced braces in " + text);
                for (const auto& slot : kex::diag::slots(text)) {
                    assertTrue(identifier(slot), "slot `" + slot + "` in " + std::string(entry.id));
                }
            }
        });

        it("finds an entry by its code, in any case", []() {
            const auto& first = kex::diag::entries().front();
            std::string lower(first.id);
            for (auto& c : lower) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const auto* found = kex::diag::find(lower);
            assertTrue(found != nullptr);
            assertEqual(std::string(found->id), std::string(first.id));
        });

        it("fills a template's slots and keeps a missing one visible", []() {
            const kex::diag::Args args = {{"expected", "Integer"}, {"actual", "String"}};
            assertEqual(
                kex::diag::fill("expected {expected}, got {actual}", args),
                std::string("expected Integer, got String"));
            assertEqual(kex::diag::fill("missing {gone}", {}), std::string("missing {gone}"));
        });
    });

    return runAll();
}
