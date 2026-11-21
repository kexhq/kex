// A `Block<[A]>` function a `using` brings in from another source root
// collects its block when called by its bare name. Compiled apart, the bare
// call found none of the module's signatures, so `group "/g" do ... end`
// handed `group` only its block's last statement: a router's
// `scope "/api" do ... end` under `using Rodolfo` kept only its last route.
#include "test.hxx"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace test;

namespace {

const char* const EXPECTED =
    "[Nest.Leaf { name: a }, Nest.Group { children: [Nest.Leaf { name: b }, Nest.Leaf "
    "{ name: c }], prefix: /g }]\n"
    "[Nest.Group { children: [Nest.Leaf { name: d }, Nest.Leaf { name: e }], prefix: /h }]\n"
    "[Nest.Leaf { name: f }]\n"
    "[1, 2]\n";

struct Package {
    std::filesystem::path root;

    Package() {
        char path[] = "/tmp/kex_using_bare_collection_XXXXXX";
        root = mkdtemp(path);
        std::filesystem::create_directories(root / "app");
        std::filesystem::create_directories(root / "lib");
        write("lib/nest.kex", R"KEX(module Nest

record Leaf do
  name : String
end

record Group do
  prefix : String
  children : [Item]
end

type Item = Leaf | Group

leaf : String -> Item
let leaf(name) = Leaf { name: name }

group : String -> Block<[Item]> -> Item
let group(prefix, items) = Group { prefix: prefix, children: items() }

tree : Block<[Item]> -> [Item]
let tree(items) = items()

pair : Block<[Integer]> -> [Integer]
let pair(items) = items()
)KEX");
        write("app/main.kex", R"KEX(using Nest

main do
  # Bare, and bare inside bare.
  IO.printLine(tree do
    leaf("a")
    group "/g" do
      leaf("b")
      leaf("c")
    end
  end)
  # Bare inside qualified, the shape `Rodolfo.router do scope ... end` has.
  IO.printLine(Nest.tree do
    group "/h" do
      leaf("d")
      leaf("e")
    end
  end)
  # One statement still makes a list.
  IO.printLine(tree do
    leaf("f")
  end)
  IO.printLine(pair do
    1
    2
  end)
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

auto check(const std::string& mode) -> void {
    Package package;
    const auto app = (package.root / "app").string();
    const auto lib = (package.root / "lib").string();
    const auto output = package.root / "output";
    const auto command = std::string(KEX_BINARY_PATH) + " " + mode + " --no-colors --source-root " +
                         app + " --source-root " + lib + " " + app + "/main.kex > " +
                         output.string() + " 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream file(output);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto text = buffer.str();
    assertTrue(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the program failed: " + text);
    assertTrue(text == EXPECTED, "a bare `using` call did not collect its block: " + text);
}

} // namespace

int main() {
    describe("a bare call to a `using`-imported collection block", []() {
        it("collects on BEAM", []() { check(""); });
        it("collects in the walker", []() { check("-R"); });
    });
    return runAll();
}
