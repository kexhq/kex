// #444: expansion of an imported library must inherit the application's
// source roots, including when its sandbox loads another compiled module.
#include "test.hxx"
#include "../src/compiled/expand.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace test;

namespace {

struct Package {
    std::filesystem::path root;

    Package() {
        char path[] = "/tmp/kex_compiled_dependency_XXXXXX";
        root = mkdtemp(path);
        std::filesystem::create_directories(root / "app");
        std::filesystem::create_directories(root / "lib/library");
        write("lib/library/schema.kex", R"KEX(module Library.Schema

compiled do
  let BASE = 40
end

value : Integer
let value() = BASE
)KEX");
        write("lib/library/query.kex", R"KEX(module Library.Query

using Library.Schema, only: [value]

compiled do
  let ANSWER = value() + 2

  let add(n: Integer) -> Integer = n + value()
end

answer : Integer
let answer() = ANSWER

result : Integer
let result() = add(2)
)KEX");
        write("app/main.kex", R"KEX(using Library.Query, only: [answer, result]

main do
  IO.printLine(answer())
  IO.printLine(result())
end
)KEX");
    }

    ~Package() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    auto write(const std::string& path, const std::string& contents) -> void {
        std::ofstream file(root / path);
        file << contents;
    }
};

auto check(const std::string& mode, bool libraryFirst) -> void {
    Package package;
    const auto app = (package.root / "app").string();
    const auto lib = (package.root / "lib").string();
    const auto output = package.root / "output";
    const auto command = std::string(KEX_BINARY_PATH) + " " + mode +
        " --no-colors --source-root " + (libraryFirst ? lib : app) +
        " --source-root " + (libraryFirst ? app : lib) + " " +
        app + "/main.kex > " + output.string() + " 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream file(output);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto text = buffer.str();
    assertTrue(WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "dependency expansion failed: " + text);
    assertTrue(text.find(mode == "-C" ? "No errors found" : "42\n42\n") !=
                   std::string::npos,
               "unexpected dependency result: " + text);
}

} // namespace

int main() {
    describe("compiled modules across package roots", []() {
        it("collapses a compiled call that imports a sibling module", []() {
            Package package;
            const std::string source = R"KEX(module Library.Query do
  using Library.Schema, only: [value]

  compiled do
    let add(n: Integer) -> Integer = n + value()
  end
end

main do
  Library.Query.add(2)
end
)KEX";
            kex::Lexer lexer(source);
            kex::Parser parser(lexer.tokenizeAll());
            auto program = parser.parseProgram();
            assertTrue(parser.diagnostics().empty(), "invalid regression fixture");
            std::vector<kex::semantic::Diagnostic> diagnostics;
            std::vector<kex::compiled::CollapseNote> report;
            kex::compiled::ExpandOptions options;
            options.moduleRoots = {(package.root / "lib").string()};
            options.report = &report;
            assertTrue(kex::compiled::expand(program, diagnostics, options),
                       "compiled function expansion failed");
            assertTrue(report.size() == 1 && report.front().collapsed,
                       "the sibling import silently prevented chain collapse: " +
                           (report.empty() ? "no collapse attempt" : report.front().reason));
        });
        it("runs on BEAM with the application root first", []() { check("", false); });
        it("runs on BEAM with the library root first", []() { check("", true); });
        it("runs in the walker with the application root first", []() { check("-R", false); });
        it("runs in the walker with the library root first", []() { check("-R", true); });
        it("checks with the application root first", []() { check("-C", false); });
        it("checks with the library root first", []() { check("-C", true); });
    });
    return runAll();
}
