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
        it("keeps same-named aliases local to their modules", []() {
            Package package;
            package.write("lib/library/first.kex", R"KEX(module Library.First
compiled do
  let VALUE = 41
end
)KEX");
            package.write("lib/library/second.kex", R"KEX(module Library.Second
compiled do
  let VALUE = 99
end
)KEX");
            package.write("app/main.kex", R"KEX(module First do
  using Library.First, as: Config
  let answer() -> Integer = Config.VALUE
end
module Second do
  using Library.Second, as: Config
  let answer() -> Integer = Config.VALUE
end
main do
  IO.printLine(First.answer())
  IO.printLine(Second.answer())
end
)KEX");
            const auto output = package.root / "alias-output";
            const auto command = std::string(KEX_BINARY_PATH) +
                " -R --no-colors --source-root " +
                (package.root / "lib").string() + " --source-root " +
                (package.root / "app").string() + " " +
                (package.root / "app/main.kex").string() + " > " +
                output.string() + " 2>&1";
            const int status = std::system(command.c_str());
            std::ifstream file(output);
            std::stringstream buffer;
            buffer << file.rdbuf();
            const auto result = buffer.str();
            assertTrue(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                       "alias fixture failed: " + result);
            assertTrue(result == "41\n99\n",
                       "aliases leaked across module scopes: " + result);
        });
        it("lets a nested module see its parent's alias", []() {
            Package package;
            package.write("lib/library/first.kex", R"KEX(module Library.First
compiled do
  let VALUE = 41
end
)KEX");
            package.write("app/main.kex", R"KEX(module Outer do
  using Library.First, as: Config
  module Inner do
    let answer() -> Integer = Config.VALUE
  end
end
main do
  IO.printLine(Outer.Inner.answer())
end
)KEX");
            const auto output = package.root / "nested-output";
            const auto command = std::string(KEX_BINARY_PATH) +
                " -R --no-colors --source-root " +
                (package.root / "lib").string() + " --source-root " +
                (package.root / "app").string() + " " +
                (package.root / "app/main.kex").string() + " > " +
                output.string() + " 2>&1";
            const int status = std::system(command.c_str());
            std::ifstream file(output);
            std::stringstream buffer;
            buffer << file.rdbuf();
            const auto result = buffer.str();
            assertTrue(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                       "nested alias fixture failed: " + result);
            assertTrue(result == "41\n",
                       "the nested module lost its parent's alias: " + result);
        });
        it("falls back to the absolute owner when the relative one is no module", []() {
            // `Mid.Leaf` inside `module Chain` is first read as `Chain.Mid.Leaf`,
            // which the container heuristic matches to chain/mid.kex — a file
            // that does not declare it. The top-level `Mid.Leaf` is meant.
            Package package;
            std::filesystem::create_directories(package.root / "lib/chain");
            std::filesystem::create_directories(package.root / "lib/mid");
            package.write("lib/mid/leaf.kex", R"KEX(module Mid.Leaf
compiled do
  let VALUE = 7
end
)KEX");
            package.write("lib/chain/mid.kex", R"KEX(module Chain.Mid
let other() -> Integer = 1
)KEX");
            package.write("lib/chain.kex", R"KEX(module Chain
using Mid.Leaf, as: Base
let aliased() -> Integer = Base.VALUE
let qualified() -> Integer = Mid.Leaf.VALUE
)KEX");
            package.write("app/main.kex", R"KEX(using Chain
main do
  IO.printLine(Chain.aliased())
  IO.printLine(Chain.qualified())
end
)KEX");
            const auto output = package.root / "absolute-output";
            const auto command = std::string(KEX_BINARY_PATH) +
                " -R --no-colors --source-root " +
                (package.root / "lib").string() + " --source-root " +
                (package.root / "app").string() + " " +
                (package.root / "app/main.kex").string() + " > " +
                output.string() + " 2>&1";
            const int status = std::system(command.c_str());
            std::ifstream file(output);
            std::stringstream buffer;
            buffer << file.rdbuf();
            const auto result = buffer.str();
            assertTrue(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                       "absolute owner fixture failed: " + result);
            assertTrue(result == "7\n7\n",
                       "the phantom relative owner shadowed the real one: " + result);
        });
        it("collapses a compiled record containing an optional default", []() {
            Package package;
            package.write("lib/library/defaults.kex", R"KEX(module Library.Defaults
record Shape do
  fallback : String? = None
  params : [Any] = []
end
compiled do
  let SHAPE = Shape { fallback: Just("") }
  let withValue(shape: Shape, value: String, count: Integer) -> Shape do
    let params : [Any] = [value]
    Shape { ...shape, fallback: Just(value), params: params.push(count) }
  end
end
)KEX");
            package.write("app/default.kex", R"KEX(using Library.Defaults
let query(value: String, count: Integer) -> Library.Defaults.Shape do
  Library.Defaults.withValue(Library.Defaults.SHAPE, value, count)
end
)KEX");
            std::filesystem::create_directories(package.root / "core");
            const auto output = package.root / "default-output";
            const auto command = std::string(KEX_BINARY_PATH) +
                " -E --collapse-report --no-colors --source-root " +
                (package.root / "lib").string() + " -o " +
                (package.root / "core").string() + " " +
                (package.root / "app/default.kex").string() + " > " +
                output.string() + " 2>&1";
            const int status = std::system(command.c_str());
            std::ifstream reportFile(output);
            std::stringstream report;
            report << reportFile.rdbuf();
            assertTrue(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                       "optional default fixture failed: " + report.str());
            assertTrue(report.str().find("collapse: 1 of 1 evaluated at compile time") !=
                           std::string::npos,
                       "optional default prevented collapse: " + report.str());
            std::ifstream coreFile(package.root / "core/kex_default.core");
            std::stringstream core;
            core << coreFile.rdbuf();
            assertTrue(coreFile.good() || !core.str().empty(),
                       "optional default entry Core missing");
            assertTrue(core.str().find("call 'Kex.Library.Defaults':'withValue'") ==
                           std::string::npos,
                       "optional default still calls compiled builder at runtime");
        });
    });
    return runAll();
}
