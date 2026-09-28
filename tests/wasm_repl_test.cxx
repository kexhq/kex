#include "test.hxx"
#include <string>

struct KexReplSession;

extern "C" {
auto kex_repl_create() -> KexReplSession*;
auto kex_repl_destroy(KexReplSession* session) -> void;
auto kex_repl_eval(KexReplSession* session, const char* source) -> void;
auto kex_repl_last_result(KexReplSession* session) -> const char*;
auto kex_repl_complete(KexReplSession* session, const char* line, int start,
                       const char* text) -> const char*;
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

        it("clears the screen with an ANSI escape sequence", []() {
            auto* session = kex_repl_create();
            auto cleared = eval(session, "/clear");
            // xterm.js renders the same escape as a real terminal; the bare
            // \x1b[2J (ED 2) is the part that actually wipes the screen.
            assertTrue(cleared.find("\x1b[2J") != std::string::npos, cleared);
            kex_repl_destroy(session);
        });

        // The same analysis as the native REPLs and the editor: a binding's
        // type comes from replaying it, not from reading its name as a type.
        it("completes a binding's members by its type", []() {
            auto* session = kex_repl_create();
            // A session record: the embedded prelude is absent natively.
            eval(session, "record Box do\n  size : Integer\nend");
            eval(session, "let b = Box { size: 1 }");
            const std::string completed = kex_repl_complete(session, "b.si", 0, "b.si");
            assertTrue(completed.find("b.size\n") != std::string::npos, completed);
            kex_repl_destroy(session);
        });
    });

    return runAll();
}
