// Regression test for the "--parse silently drops declarations on a syntax
// error" bug (kexhq/kex#248). `Parser::parseProgram` recovers from a syntax
// error by resyncing to the next top-level keyword and recording a
// diagnostic in `parser.diagnostics()`, but `--parse` used to print
// `printAst(program)` unconditionally, ignoring those diagnostics — so a
// malformed declaration (and everything after it, until the resync point)
// vanished from the printed AST with no error at all. This drives the real
// binary, since the bug is specifically in main.cxx's CLI dispatch, not in
// the Parser class the in-process parser_test exercises directly.
#include "test.hxx"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace test;

namespace {

struct Result {
    std::string out;
    std::string err;
    int exitCode = -1;
};

auto writeTempSource(const std::string& source) -> std::string {
    char tmp[] = "/tmp/kex_parse_diag_src_XXXXXX";
    int fd = mkstemp(tmp);
    close(fd);
    std::string path = std::string(tmp) + ".kex";
    std::rename(tmp, path.c_str());
    std::ofstream f(path);
    f << source;
    return path;
}

auto runKexParse(const std::string& sourcePath) -> Result {
    char outPath[] = "/tmp/kex_parse_diag_out_XXXXXX";
    char errPath[] = "/tmp/kex_parse_diag_err_XXXXXX";
    int outFd = mkstemp(outPath);
    int errFd = mkstemp(errPath);
    close(outFd);
    close(errFd);

    std::string cmd = std::string(KEX_BINARY_PATH) + " --parse " + sourcePath +
                       " > " + outPath + " 2> " + errPath;
    int status = std::system(cmd.c_str());

    auto read = [](const char* path) {
        std::ifstream file(path);
        return std::string(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
    };
    Result result{read(outPath), read(errPath),
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1};
    std::remove(outPath);
    std::remove(errPath);
    return result;
}

} // namespace

int main() {
    describe("kex --parse diagnostics", [&]() {
        it("reports a syntax error instead of silently dropping declarations", [&]() {
            // The `&&` condition spans a newline without the outer parens
            // grammar.ebnf requires, which is a syntax error. Before the fix,
            // `--parse` printed an AST missing both `unquote` and `after`
            // with no diagnostic at all.
            const std::string path = writeTempSource(
                "let unquote(text: String) -> String do\n"
                "  let quoted = text.count >= 2 &&\n"
                "    (text.startsWith?(\"x\"))\n"
                "  quoted then text else text\n"
                "end\n"
                "\n"
                "let later -> Integer do\n"
                "  2\n"
                "end\n");
            const auto result = runKexParse(path);
            std::remove(path.c_str());

            assertTrue(result.exitCode != 0,
                       "a malformed multi-line condition must fail --parse, not exit 0");
            assertTrue(result.err.find("error") != std::string::npos,
                       "stderr should report the syntax error");
            assertTrue(result.out.find("Program (0 items)") == std::string::npos,
                       "must not silently print an empty/partial AST as if nothing were wrong");
        });

        it("still parses cleanly when the multi-line condition is parenthesized", [&]() {
            const std::string path = writeTempSource(
                "let quoted = (1 >= 2 &&\n"
                "    (1 == 1))\n"
                "\n"
                "let later -> Integer do\n"
                "  2\n"
                "end\n");
            const auto result = runKexParse(path);
            std::remove(path.c_str());

            assertEqual(result.exitCode, 0);
            assertTrue(result.out.find("later") != std::string::npos,
                       "the declaration after the parenthesized condition must survive");
        });
    });

    return runAll();
}
