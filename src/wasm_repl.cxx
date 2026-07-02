// Minimal C API for embedding the interpreter's REPL semantics in a
// browser via a handful of extern "C" functions Emscripten can export
// directly — deliberately NOT reusing main.cxx's interactive REPL loop,
// which is built around a real blocking stdin (readline or a plain-stdin
// fallback), neither of which makes sense in a browser. One persistent
// Evaluator per session, one chunk of source evaluated per call, same
// cross-call state persistence already exercised by
// tests/interpreter_test.cxx's "a process spawned on one execute() call is
// reachable from a later call" test — a `let`/`spawn` on one call stays
// visible/alive on the next, matching what a real REPL user expects.
#include "common/color.hxx"
#include "lexer/lexer.hxx"
#include "parser/parser.hxx"
#include "interpreter/evaluator.hxx"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#include <memory>
#include <string>
#include <vector>

namespace {

// Every parsed Program must outlive anything that might still reference
// its AST — a `spawn`'d process captures raw pointers into the AST it was
// spawned from (see docs/fiber-process-plan.md's fiber-lifetime notes) and
// can outlive the call that spawned it (e.g. a `loop { receive ... }`
// server, matching examples/proc_ping.kex). Kept alive for the whole
// session, same convention main.cxx's REPL uses (`replPrograms`, `new
// kex::ast::Program(...)`, never deleted).
std::vector<std::unique_ptr<kex::ast::Program>> g_programs;

} // namespace

struct KexReplSession {
    kex::interpreter::Evaluator evaluator;
    std::string lastResult;
};

extern "C" {

EMSCRIPTEN_KEEPALIVE
KexReplSession* kex_repl_create() {
    // web/index.html renders this through a real terminal emulator
    // (xterm.js), so leave ANSI colors on (the default) — output matches
    // the native REPL's coloring exactly, rendered by the terminal instead
    // of reimplemented in JS.
    auto* session = new KexReplSession();
    // Matches the real REPL's own mode — without it, each chunk's
    // top-level `main`-block wrapping pushes its own scope (see
    // execMainBlock's `!m_replMode && !block.synthetic` check), which
    // would make a `let` on one call invisible on the next.
    session->evaluator.setReplMode(true);
    return session;
}

EMSCRIPTEN_KEEPALIVE
void kex_repl_destroy(KexReplSession* session) {
    delete session;
}

namespace {

// Same "is this a top-level definition, or a bare expression that needs
// wrapping in main do...end" heuristic main.cxx's real REPL uses —
// definitions (foul/type/record/module/make, or a `let`/`foul` that looks
// like a function def: name( before any =/do) parse as top-level items
// directly; everything else (a plain `let x = 5`, or any bare expression
// like `1 + 2`) gets wrapped so the grammar accepts it as a `main` body.
auto looksLikeTopLevelDef(const std::string& source) -> bool {
    auto startsWith = [&](const char* prefix) {
        return source.rfind(prefix, 0) == 0;
    };
    if (startsWith("module ") || startsWith("type ") || startsWith("record ") ||
        startsWith("make ") || startsWith("foul module ")) {
        return true;
    }
    size_t defOffset = std::string::npos;
    if (startsWith("let ")) defOffset = 4;
    else if (startsWith("foul ")) defOffset = 5;
    if (defOffset == std::string::npos) return false;

    auto parenPos = source.find('(', defOffset);
    auto eqPos = source.find('=', defOffset);
    auto doPos = source.find(" do", defOffset);
    return parenPos != std::string::npos &&
           (eqPos == std::string::npos || parenPos < eqPos) &&
           (doPos == std::string::npos || parenPos < doPos);
}

} // namespace

// Evaluates one chunk of Kex source against this session's persistent
// Evaluator, storing the result in the session (see kex_repl_last_result)
// rather than returning it directly. Deliberately split this way: this
// function's call chain runs through execute()'s Asyncify-based fiber
// machinery (see docs/fiber-process-plan.md), and a direct return value
// from an Asyncify-instrumented export was not reliably reaching the JS
// caller through ccall's async plumbing (confirmed with a hardcoded
// static-string return — still came back as a null pointer on the JS
// side, ruling out a memory-ownership bug specifically). Fetching the
// result via a separate, ordinary synchronous call sidesteps the issue
// entirely, since that call never touches Asyncify at all.
EMSCRIPTEN_KEEPALIVE
void kex_repl_eval(KexReplSession* session, const char* sourceIn) {
    using namespace kex;
    using namespace kex::interpreter;

    std::string source(sourceIn ? sourceIn : "");
    bool isDef = looksLikeTopLevelDef(source);
    std::string toParse = isDef ? source : ("main do\n" + source + "\nend\n");
    std::string result;

    try {
        Lexer lexer(toParse);
        auto tokens = lexer.tokenizeAll();
        Parser parser(std::move(tokens));
        g_programs.push_back(std::make_unique<ast::Program>(parser.parseProgram()));
        ast::Program* program = g_programs.back().get();

        size_t outputBefore = session->evaluator.output().size();
        auto value = session->evaluator.execute(*program);
        result = session->evaluator.output().substr(outputBefore);

        // Matches main.cxx's real REPL showResult lambda exactly (gray "=>"
        // and ":", plain value, cyan type — showTypes defaults to on there
        // too), so the two look identical once rendered through a real
        // terminal instead of a plain-text diff.
        if (!isDef && value && !std::holds_alternative<UnitValue>(value->data)) {
            result += std::string(color::apply(color::gray)) + "=> " +
                      color::apply(color::reset) + value->inspect() + " " +
                      color::apply(color::gray) + ":" + color::apply(color::reset) +
                      " " + color::apply(color::cyan) + value->typeName() +
                      color::apply(color::reset) + "\n";
        }
    } catch (const std::exception& e) {
        // Matches main.cxx's real REPL error formatting exactly (see its
        // `catch (const std::exception &e)` blocks) so the two look
        // identical once rendered through a real terminal.
        result = std::string("  ") + color::apply(color::red) + "error:" +
                 color::apply(color::reset) + " " + e.what() + "\n";
    } catch (...) {
        result = std::string("  ") + color::apply(color::red) + "error:" +
                 color::apply(color::reset) + " unknown failure\n";
    }

    session->lastResult = std::move(result);
}

// Ordinary synchronous call, no Asyncify involvement — fetches whatever
// kex_repl_eval most recently produced. The returned pointer is owned by
// the session and only valid until the next kex_repl_eval call (or
// kex_repl_destroy); callers must copy it out immediately, matching the
// usual UTF8ToString(ptr)-right-after-the-call usage pattern.
EMSCRIPTEN_KEEPALIVE
const char* kex_repl_last_result(KexReplSession* session) {
    return session->lastResult.c_str();
}

} // extern "C"
