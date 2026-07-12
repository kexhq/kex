#include "test.hxx"
#include "../src/semantic/db.hxx"
#include "../src/semantic/analyzer.hxx"
#include "../src/common/completion.hxx"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

using namespace test;
using kex::semantic::SemanticDB;

static auto makePreludeDb() -> SemanticDB {
    SemanticDB db;
    std::filesystem::path dir = "src/prelude";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".kex") continue;
        std::ifstream f(entry.path());
        std::ostringstream ss; ss << f.rdbuf();
        db.updateFile(entry.path().string(), ss.str());
    }
    return db;
}

static auto has(const std::vector<std::string>& v, const std::string& s) -> bool {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Simulates the full completion pipeline: resolveCompletionQuery + DB lookup +
// rewriteCompletions — returning what readline would actually insert.
static auto simulate(SemanticDB& db, const std::string& linebuf,
                     int start, const char* text) -> std::vector<std::string> {
    auto cq  = kex::resolveCompletionQuery(linebuf.c_str(), start, text);
    auto raw = db.completionsFor(cq.dbQuery);
    return kex::rewriteCompletions(std::move(raw), cq.rewriteFrom, cq.rewriteTo);
}

int main() {
    // ── qualifierFromBefore ─────────────────────────────────────────────────
    describe("qualifierFromBefore", []() {
        it("identifier maps to itself", []() {
            assertEqual(kex::qualifierFromBefore("IO"),   std::string("IO"));
            assertEqual(kex::qualifierFromBefore("Math"), std::string("Math"));
        });
        it("list literal maps to List", []() {
            assertEqual(kex::qualifierFromBefore("[1,2,3]"), std::string("List"));
            assertEqual(kex::qualifierFromBefore("]"),       std::string("List"));
        });
        it("integer literal maps to Integer", []() {
            assertEqual(kex::qualifierFromBefore("42"),  std::string("Integer"));
            assertEqual(kex::qualifierFromBefore("0"),   std::string("Integer"));
        });
        it("char literal maps to Char", []() {
            assertEqual(kex::qualifierFromBefore("'a'"), std::string("Char"));
        });
        it("string literal maps to String", []() {
            assertEqual(kex::qualifierFromBefore("\"hi\""), std::string("String"));
        });
    });

    // ── resolveCompletionQuery ──────────────────────────────────────────────
    describe("resolveCompletionQuery", []() {
        it("no dot — passthrough, no rewrite", []() {
            auto cq = kex::resolveCompletionQuery("pri", 0, "pri");
            assertEqual(cq.dbQuery,     std::string("pri"));
            assertEqual(cq.rewriteFrom, std::string(""));
            assertEqual(cq.rewriteTo,   std::string(""));
        });

        // ── Case A: readline split on '.' ──────────────────────────────────
        it("A: identifier split — IO. text=''", []() {
            auto cq = kex::resolveCompletionQuery("IO.", 3, "");
            assertEqual(cq.dbQuery,     std::string("IO."));
            assertEqual(cq.rewriteFrom, std::string("IO."));
            assertEqual(cq.rewriteTo,   std::string(""));
        });
        it("A: identifier split — IO.pr text='pr'", []() {
            auto cq = kex::resolveCompletionQuery("IO.pr", 3, "pr");
            assertEqual(cq.dbQuery,     std::string("IO.pr"));
            assertEqual(cq.rewriteFrom, std::string("IO."));
            assertEqual(cq.rewriteTo,   std::string(""));
        });
        it("A: list literal split — [1,2,3]. text=''", []() {
            auto cq = kex::resolveCompletionQuery("[1,2,3].", 8, "");
            assertEqual(cq.dbQuery,     std::string("List."));
            assertEqual(cq.rewriteFrom, std::string("List."));
            assertEqual(cq.rewriteTo,   std::string(""));
        });
        it("A: list literal split — [1,2,3].ma text='ma'", []() {
            auto cq = kex::resolveCompletionQuery("[1,2,3].ma", 8, "ma");
            assertEqual(cq.dbQuery,     std::string("List.ma"));
            assertEqual(cq.rewriteFrom, std::string("List."));
            assertEqual(cq.rewriteTo,   std::string(""));
        });

        // ── Case B: readline did NOT split ─────────────────────────────────
        it("B: full IO.pr text='IO.pr' start=0 — no rewrite (identifier)", []() {
            auto cq = kex::resolveCompletionQuery("IO.pr", 0, "IO.pr");
            assertEqual(cq.dbQuery,     std::string("IO.pr"));
            assertEqual(cq.rewriteFrom, std::string("IO."));
            assertEqual(cq.rewriteTo,   std::string("IO."));  // identity rewrite
        });
        it("B: list literal [1,2,3]. text='[1,2,3].' start=0", []() {
            auto cq = kex::resolveCompletionQuery("[1,2,3].", 0, "[1,2,3].");
            assertEqual(cq.dbQuery,     std::string("List."));
            assertEqual(cq.rewriteFrom, std::string("List."));
            assertEqual(cq.rewriteTo,   std::string("[1,2,3]."));
        });
        it("B: list literal with member [1,2,3].ma text='[1,2,3].ma' start=0", []() {
            auto cq = kex::resolveCompletionQuery("[1,2,3].ma", 0, "[1,2,3].ma");
            assertEqual(cq.dbQuery,     std::string("List.ma"));
            assertEqual(cq.rewriteFrom, std::string("List."));
            assertEqual(cq.rewriteTo,   std::string("[1,2,3]."));
        });
        it("B: integer literal 23. text='23.' start=0", []() {
            auto cq = kex::resolveCompletionQuery("23.", 0, "23.");
            assertEqual(cq.dbQuery,     std::string("Integer."));
            assertEqual(cq.rewriteFrom, std::string("Integer."));
            assertEqual(cq.rewriteTo,   std::string("23."));
        });
        it("B: char literal 's'. text=\"'s'.\" start=0", []() {
            auto cq = kex::resolveCompletionQuery("'s'.", 0, "'s'.");
            assertEqual(cq.dbQuery,     std::string("Char."));
            assertEqual(cq.rewriteFrom, std::string("Char."));
            assertEqual(cq.rewriteTo,   std::string("'s'."));
        });
        it("B: identifier in expression — 'foo + Math.s' text='Math.s' start=6", []() {
            // readline did not split: text is "Math.s", start=6 (after space)
            auto cq = kex::resolveCompletionQuery("foo + Math.s", 6, "Math.s");
            assertEqual(cq.dbQuery,     std::string("Math.s"));
            assertEqual(cq.rewriteFrom, std::string("Math."));
            assertEqual(cq.rewriteTo,   std::string("Math."));
        });
    });

    // ── rewriteCompletions ──────────────────────────────────────────────────
    describe("rewriteCompletions", []() {
        it("strip only (rewriteTo empty)", []() {
            auto r = kex::rewriteCompletions({"IO.print", "IO.printLine"}, "IO.", "");
            assertTrue(has(r, "print"));
            assertTrue(has(r, "printLine"));
            assertTrue(!has(r, "IO.print"));
        });
        it("identity rewrite (from == to) — no change", []() {
            auto r = kex::rewriteCompletions({"IO.print"}, "IO.", "IO.");
            assertTrue(has(r, "IO.print"));
        });
        it("non-identifier prefix rewrite", []() {
            auto r = kex::rewriteCompletions({"List.map", "List.filter"}, "List.", "[1,2,3].");
            assertTrue(has(r, "[1,2,3].map"));
            assertTrue(has(r, "[1,2,3].filter"));
            assertTrue(!has(r, "List.map"));
        });
        it("empty rewriteFrom — passthrough", []() {
            auto r = kex::rewriteCompletions({"print", "printLine"}, "", "");
            assertTrue(has(r, "print"));
            assertTrue(has(r, "printLine"));
        });
    });

    // ── inferLambdaParamType ────────────────────────────────────────────────
    describe("inferLambdaParamType", []() {
        it("integer list → Integer", []() {
            assertEqual(kex::inferLambdaParamType("[23,324,23].filter { |x| x.", "x"),
                        std::string("Integer"));
        });
        it("float list → Float", []() {
            assertEqual(kex::inferLambdaParamType("[1.0, 2.5].map { |n| n.", "n"),
                        std::string("Float"));
        });
        it("mixed list → List (no elem type)", []() {
            std::string r = kex::inferLambdaParamType("[1, \"a\"].each { |e| e.", "e");
            assertTrue(r == "List" || r.empty());
        });
        it("string literal → Char", []() {
            assertEqual(kex::inferLambdaParamType("\"hello\".each { |c| c.", "c"),
                        std::string("Char"));
        });
        it("no pipe pattern → empty", []() {
            assertEqual(kex::inferLambdaParamType("IO.printLine(x)", "x"),
                        std::string(""));
        });
        it("receiver is plain ident → that ident", []() {
            // `xs.each { |v| v.` where xs is a named collection
            std::string r = kex::inferLambdaParamType("xs.each { |v| v.", "v");
            assertEqual(r, std::string("xs"));
        });
        it("integer receiver 234.times { |x| x. → Integer", []() {
            assertEqual(kex::inferLambdaParamType("234.times { |x| x.", "x"),
                        std::string("Integer"));
        });
    });

    // ── inferPatternParamType ───────────────────────────────────────────────
    describe("inferPatternParamType", []() {
        it("head of cons in String make block → Char", []() {
            assertEqual(kex::inferPatternParamType(
                "  let capitalize(@[x|xs]) = x.", "x", "String"),
                std::string("Char"));
        });
        it("tail of cons in String make block → String", []() {
            assertEqual(kex::inferPatternParamType(
                "  let capitalize(@[x|xs]) = xs.", "xs", "String"),
                std::string("String"));
        });
        it("simple named param in make block → receiver type", []() {
            assertEqual(kex::inferPatternParamType(
                "  let shout(s) = s.", "s", "String"),
                std::string("String"));
        });
        it("no make target → empty", []() {
            assertEqual(kex::inferPatternParamType(
                "  let shout(s) = s.", "s", ""),
                std::string(""));
        });
        it("unrelated param → empty", []() {
            assertEqual(kex::inferPatternParamType(
                "  let capitalize(@[x|xs]) = x.", "y", "String"),
                std::string(""));
        });
    });

    // ── resolveCompletionQuery — lambda param (Case B) ─────────────────────
    describe("resolveCompletionQuery — lambda param", []() {
        it("x. in integer list filter — Case B trailing ident", []() {
            const char* line = "[23,324,23].filter { |x| x.";
            auto cq = kex::resolveCompletionQuery(line, 0, line);
            assertEqual(cq.dbQuery,     std::string("Integer."));
            assertEqual(cq.rewriteFrom, std::string("Integer."));
            // rewriteTo: the full "[23,324,23].filter { |x| x."
            assertEqual(cq.rewriteTo,   std::string("[23,324,23].filter { |x| x."));
        });
        it("n. in float list map — Case B trailing ident", []() {
            const char* line = "[1.0, 2.5].map { |n| n.";
            auto cq = kex::resolveCompletionQuery(line, 0, line);
            assertEqual(cq.dbQuery,     std::string("Float."));
        });
        it("c. in string each — Case B char", []() {
            const char* line = "\"hello\".each { |c| c.";
            auto cq = kex::resolveCompletionQuery(line, 0, line);
            assertEqual(cq.dbQuery,     std::string("Char."));
        });
        // Case A (readline splits on last dot)
        it("x. in integer list filter — Case A split", []() {
            const char* linebuf = "[23,324,23].filter { |x| x.";
            // text="" start=27 (after the last dot)
            auto cq = kex::resolveCompletionQuery(linebuf, 27, "");
            assertEqual(cq.dbQuery,     std::string("Integer."));
            assertEqual(cq.rewriteFrom, std::string("Integer."));
            assertEqual(cq.rewriteTo,   std::string(""));
        });
    });

    // ── full pipeline (DB + query resolution + rewrite) ────────────────────
    describe("Completion pipeline — prelude", []() {
        it("IO. split at dot (A) — returns bare member names", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "IO.", 3, "");
            assertTrue(!r.empty());
            assertTrue(has(r, "printLine"));
            assertTrue(has(r, "print"));
            assertTrue(!has(r, "IO.printLine"));
        });
        it("IO.pr split at dot (A) — filtered bare member names", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "IO.pr", 3, "pr");
            assertTrue(has(r, "print"));
            assertTrue(has(r, "printLine"));
            assertTrue(!has(r, "inspect"));
        });
        it("IO.pr not split (B) — qualified completions returned as-is", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "IO.pr", 0, "IO.pr");
            assertTrue(has(r, "IO.print"));
            assertTrue(has(r, "IO.printLine"));
        });
        it("Math. not split (B) — math members with Math. prefix", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "Math.", 0, "Math.");
            assertTrue(has(r, "Math.cos"));
            assertTrue(has(r, "Math.sin"));
        });
        it("String. not split (B) — string members", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "String.", 0, "String.");
            assertTrue(has(r, "String.count"));
            assertTrue(has(r, "String.empty?"));
        });
        it("[1,2,3]. not split (B) — list members rewritten with literal prefix", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "[1,2,3].", 0, "[1,2,3].");
            assertTrue(!r.empty());
            assertTrue(has(r, "[1,2,3].map"));
            assertTrue(has(r, "[1,2,3].filter"));
            assertTrue(has(r, "[1,2,3].count"));
            assertTrue(!has(r, "List.map"));
            assertTrue(!has(r, "map"));
        });
        it("[1,2,3].ma not split (B) — filtered with literal prefix", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "[1,2,3].ma", 0, "[1,2,3].ma");
            assertTrue(has(r, "[1,2,3].map"));
            assertTrue(!has(r, "[1,2,3].filter"));
        });
        it("[1,2,3].ma split at dot (A) — bare member name", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "[1,2,3].ma", 8, "ma");
            assertTrue(has(r, "map"));
            assertTrue(!has(r, "[1,2,3].map"));
            assertTrue(!has(r, "List.map"));
        });
        it("File. — module functions", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "File.", 0, "File.");
            assertTrue(has(r, "File.read"));
            assertTrue(has(r, "File.write"));
            assertTrue(has(r, "File.exists?"));
        });
        it("23. not split (B) — integer methods rewritten with literal prefix", []() {
            auto db = makePreludeDb();
            auto r = simulate(db, "23.", 0, "23.");
            // Integer make-block functions, prefixed with "23."
            for (const auto& s : r)
                assertTrue(s.rfind("23.", 0) == 0);
        });
        it("top-level bare prefix — global names", []() {
            auto db = makePreludeDb();
            // Module-scoped and make-scoped names are not bare completions;
            // only truly top-level symbols (modules, types, traits) match.
            auto r = simulate(db, "fil", 0, "fil");
            assertTrue(!has(r, "filter"));
            auto r2 = simulate(db, "List.fil", 0, "List.fil");
            assertTrue(has(r2, "List.filter"));
        });
        it("user-defined function after updateFile", []() {
            auto db = makePreludeDb();
            db.updateFile("<repl>", "let myFunc(x) = x + 1\n");
            auto r = simulate(db, "myF", 0, "myF");
            assertTrue(has(r, "myFunc"));
        });
        it("make String user function shows as String member", []() {
            auto db = makePreludeDb();
            db.updateFile("<repl>",
                "make String do\n"
                "  let capitalize(s) = s\n"
                "end\n");
            auto r = simulate(db, "String.", 0, "String.");
            assertTrue(has(r, "String.capitalize"));
        });
        it("accumulated REPL updates keep all definitions", []() {
            auto db = makePreludeDb();
            // Simulate two separate REPL inputs accumulated
            std::string accum =
                "let foo(x) = x\n"
                "make String do\n"
                "  let shout(s) = s\n"
                "end\n";
            db.updateFile("<repl>", accum);
            auto r1 = simulate(db, "fo", 0, "fo");
            assertTrue(has(r1, "foo"));
            auto r2 = simulate(db, "String.sh", 0, "String.sh");
            assertTrue(has(r2, "String.shout"));
        });
    });

    return runAll();
}
