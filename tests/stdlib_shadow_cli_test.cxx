// A program's own file cannot replace a standard-library module.
//
// A stray `units.kex` beside the entry answered `using Units.SI` ahead of the
// standard library's file. The runtimes loaded it while the checker read the
// stdlib's interface, so `-C` passed and the run failed; a stray file in an
// older syntax failed to parse with nothing pointing at the stdlib. Every
// mode now takes the stdlib's module and names the file it skipped.
//
// The case needs files beside the entry, so it drives the real binary in a
// directory of its own rather than living in spec/, where the stray file
// would shadow the module for every other spec.
#include "test.hxx"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace test;

namespace {

struct Project {
    std::filesystem::path root;
    std::filesystem::path main;

    ~Project() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

auto writeFile(const std::filesystem::path& path, const std::string& contents) -> void {
    std::ofstream file(path);
    file << contents;
}

// An entry file and one more file beside it.
auto makeProject(const std::string& name, const std::string& contents, const std::string& entry)
    -> std::shared_ptr<Project> {
    auto project = std::make_shared<Project>();
    char tmp[] = "/tmp/kex_stdlib_shadow_XXXXXX";
    project->root = mkdtemp(tmp);
    writeFile(project->root / name, contents);
    project->main = project->root / "main.kex";
    writeFile(project->main, entry);
    return project;
}

struct Result {
    std::string output;
    int exitCode = -1;
};

auto run(const Project& project, const std::string& mode) -> Result {
    const auto outPath = project.root / "run.out";
    std::string command = std::string(KEX_BINARY_PATH) + " " + mode + " --no-colors " +
                          project.main.string() + " > " + outPath.string() + " 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream file(outPath);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return {buffer.str(), WIFEXITED(status) ? WEXITSTATUS(status) : -1};
}

auto has(const std::string& text, const std::string& part) -> bool {
    return text.find(part) != std::string::npos;
}

constexpr const char* kEntry = R"KEX(using Units.SI

main do
  IO.printLine(5.kilometer.to(String))
end
)KEX";

// A valid module of the program's own, answering `using Units.SI` through
// its enclosing `Units`.
constexpr const char* kValidStray = R"KEX(module Units.SI

let meter = 1
)KEX";

// The same file in an older syntax: `->` in a match clause.
constexpr const char* kBrokenStray = R"KEX(module Units.SI

let f(x) = match x do
  1 -> 2
end
)KEX";

} // namespace

int main() {
    const auto valid = makeProject("units.kex", kValidStray, kEntry);
    const auto broken = makeProject("units.kex", kBrokenStray, kEntry);
    const auto own = makeProject(
        "geometry.kex", "module Geometry\n\nlet square(n: Integer) -> Integer = n * n\n",
        "using Geometry\n\nmain do\n  IO.printLine(square(4).to(String))\nend\n");

    describe("a file that takes a standard-library module's name", [&]() {
        for (const auto* mode : {"-C", "-R", "-r"}) {
            it(std::string("loses to the standard library in ") + mode, [&, mode]() {
                const auto result = run(*valid, mode);
                assertEqual(result.exitCode, 0, result.output);
                assertTrue(has(result.output, "KEXWM-0002"), result.output);
                assertTrue(has(result.output, "units.kex"), result.output);
                assertTrue(has(result.output, "not loaded for `Units.SI`"), result.output);
                assertFalse(has(result.output, "kilometer"), result.output);
            });
        }

        for (const auto* mode : {"-C", "-R", "-r"}) {
            it(std::string("is not parsed in ") + mode, [&, mode]() {
                const auto result = run(*broken, mode);
                assertEqual(result.exitCode, 0, result.output);
                assertTrue(has(result.output, "KEXWM-0002"), result.output);
                assertFalse(has(result.output, "expected '=>'"), result.output);
            });
        }
    });

    describe("a module of the program's own", [&]() {
        it("still loads when its name is not the standard library's", [&]() {
            const auto result = run(*own, "-R");
            assertEqual(result.exitCode, 0, result.output);
            assertEqual(result.output, std::string("16\n"));
        });
    });

    return runAll();
}
