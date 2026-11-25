// The standard library's interface, as the compiler hands it to a program:
// the checked description `kex` installs (registerCheckedStdlibInterfaces),
// not the source-derived fallback.
#include "test.hxx"
#include "../src/common/prelude_interfaces.hxx"
#include "../src/driver/stdlib_interfaces.hxx"
#include "../src/semantic/types.hxx"
#include <algorithm>
#include <string>
#include <vector>

using namespace kex;
using namespace test;

namespace {

auto unknownIn(const semantic::TypePtr& type) -> bool {
    return !type || semantic::anyType(type, [](const semantic::TypePtr& node) {
        const auto* unknown = std::get_if<semantic::UnknownType>(&node->kind);
        return unknown && !unknown->written;
    });
}

auto shown(const semantic::ImportedFunction& function) -> std::string {
    std::string text = function.sourceModule + " " + function.signature.name + " : ";
    for (const auto& param : function.signature.params) {
        text += semantic::typeToString(param) + " -> ";
    }
    return text + semantic::typeToString(function.signature.result);
}

} // namespace

int main() {
    describe("Stdlib interface", []() {
        // An Unknown nobody wrote is a contract the checker never learned:
        // every call through it goes unchecked.
        it("has no unwritten Unknown in any signature", []() {
            driver::registerCheckedStdlibInterfaces();
            const auto& interfaces = preludeSemanticInterfaces(KEX_RUNTIME_BEAM_DIR);
            // The opt-in modules are described too, not only the prelude.
            for (const auto* module : {"JSON", "Regex", "Evaluator", "Kex.AST", "Units.SI"}) {
                assertTrue(interfaces.modules.contains(module), std::string("missing ") + module);
            }
            std::vector<std::string> untyped;
            const auto collect = [&](const semantic::ImportedFunction& function) {
                bool bad = unknownIn(function.signature.result);
                for (const auto& param : function.signature.params) {
                    bad = bad || unknownIn(param);
                }
                if (bad) {
                    untyped.push_back(shown(function));
                }
            };
            for (const auto& [_, functions] : interfaces.receiverFunctions) {
                for (const auto& function : functions) {
                    collect(function);
                }
            }
            for (const auto& [_, module] : interfaces.modules) {
                for (const auto& [__, functions] : module.exports) {
                    for (const auto& function : functions) {
                        if (!function.isReceiverMethod) {
                            collect(function);
                        }
                    }
                }
            }
            std::sort(untyped.begin(), untyped.end());
            untyped.erase(std::unique(untyped.begin(), untyped.end()), untyped.end());
            std::string listed;
            for (const auto& line : untyped) {
                listed += "\n  " + line;
            }
            assertTrue(untyped.empty(), "stdlib signatures with an Unknown:" + listed);
        });
    });

    return runAll();
}
