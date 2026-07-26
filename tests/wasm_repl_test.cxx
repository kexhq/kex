#include "test.hxx"
#include <string>

struct KexReplSession;

extern "C" {
auto kex_repl_create() -> KexReplSession*;
auto kex_repl_destroy(KexReplSession* session) -> void;
auto kex_repl_eval(KexReplSession* session, const char* source) -> void;
auto kex_repl_last_result(KexReplSession* session) -> const char*;
}

using namespace test;

namespace {

auto eval(KexReplSession* session, const std::string& source) -> std::string {
    kex_repl_eval(session, source.c_str());
    return kex_repl_last_result(session);
}

auto importsUnitsSi(KexReplSession* session) -> void {
    auto imported = eval(session, "using Units.SI");
    assertTrue(imported.find("error:") == std::string::npos, imported);

    auto measured = eval(session, "3.meter");
    assertTrue(measured.find("notation:") != std::string::npos &&
                   measured.find("\"m\"") != std::string::npos,
               measured);
}

} // namespace

int main() {
    describe("Wasm REPL session", []() {
        it("resolves embedded standard-library modules", []() {
            auto* session = kex_repl_create();
            importsUnitsSi(session);
            kex_repl_destroy(session);
        });

        it("restores standard-library module roots after reset", []() {
            auto* session = kex_repl_create();
            eval(session, "/reset");
            importsUnitsSi(session);
            kex_repl_destroy(session);
        });
    });

    return runAll();
}
