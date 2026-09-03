// `kex -C` must know the project's OTHER modules.
//
// The run and compile paths merge every module a `using` or a qualified
// reference reaches before checking; the check path did not, so it built its
// checker from the prelude interface alone. A record declared in a sibling
// module was then an unknown name, which made `-C` report errors no other
// mode saw: a union declared alongside that record refused its own members,
// and an annotation naming it went unverified.
//
// Two compilation units are the whole point here, so this drives the real
// binary with `--source-root` rather than exercising a class in process —
// and it is why this case cannot be a `spec/*.kex` (docs/rodolfo-findings.md).
#include "test.hxx"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <sys/wait.h>
#include <unistd.h>

using namespace test;

namespace {

struct Package {
    std::filesystem::path root;
    std::filesystem::path lib;
    std::filesystem::path main;

    ~Package() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

auto writeFile(const std::filesystem::path& path, const std::string& contents)
    -> void {
    std::ofstream file(path);
    file << contents;
}

// A package with its library module in `lib/` and its entry file beside it,
// so only `--source-root` can connect the two.
auto makePackage(const std::string& library, const std::string& entry)
    -> std::shared_ptr<Package> {
    auto package = std::make_shared<Package>();
    char tmp[] = "/tmp/kex_multi_module_XXXXXX";
    package->root = mkdtemp(tmp);
    package->lib = package->root / "lib";
    std::filesystem::create_directories(package->lib);
    writeFile(package->lib / "web.kex", library);
    package->main = package->root / "main.kex";
    writeFile(package->main, entry);
    return package;
}

struct Result {
    std::string output;
    int exitCode = -1;
};

auto runCheck(const Package& package) -> Result {
    const auto outPath = package.root / "check.out";
    std::string command = std::string(KEX_BINARY_PATH) +
        " -C --no-colors --source-root " + package.lib.string() + " " +
        package.main.string() + " > " + outPath.string() + " 2>&1";
    const int status = std::system(command.c_str());
    std::ifstream file(outPath);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return {buffer.str(), WIFEXITED(status) ? WEXITSTATUS(status) : -1};
}

// A record directly in the module, and one nested a level deeper — Rodolfo's
// `Response.JSON` shape. Both spellings must be accepted as members of a
// union the module declares.
constexpr const char* kLibrary = R"KEX(module Web

record Safe do
  markup : String
end

module Response do
  record Redirect do
    status : Integer
    location : String
  end
end

type Reply = Web.Safe | Web.Response.Redirect
)KEX";

// The same union with a BUILTIN alongside its records. Reaching a module
// through `--source-root` forces the qualified spelling, and a union mixing a
// bare name with a qualified one satisfied neither classifier — the alias pass
// saw `String` and called it an ADT, the ADT pass saw `Web.Safe` and could not
// name a constructor for it. `Reply` was then registered nowhere and refused
// every member, which is what kept Rodolfo's handlers at `Env -> Any`: a route
// must be free to answer with a response record OR a plain string.
constexpr const char* kLibraryWithBuiltin = R"KEX(module Web

record Safe do
  markup : String
end

module Response do
  record Redirect do
    status : Integer
    location : String
  end
end

type Reply = String | Web.Safe | Web.Response.Redirect
)KEX";

} // namespace

int main() {
    describe("kex -C across compilation units", []() {
        it("accepts a union member declared in the sibling module", []() {
            auto package = makePackage(kLibrary, R"KEX(using Web

let safely(markup: String) -> Web.Reply do
  Safe { markup: markup }
end

main do
  IO.printLine("ok")
end
)KEX");
            auto result = runCheck(*package);
            assertTrue(result.output.find("No errors found") !=
                           std::string::npos,
                       "expected a clean check, got: " + result.output);
            assertEqual(result.exitCode, 0);
        });

        it("accepts a member nested one module deeper", []() {
            auto package = makePackage(kLibrary, R"KEX(using Web

let seeOther(location: String) -> Web.Reply do
  Response.Redirect { status: 303, location: location }
end

main do
  IO.printLine("ok")
end
)KEX");
            auto result = runCheck(*package);
            assertTrue(result.output.find("No errors found") !=
                           std::string::npos,
                       "expected a clean check, got: " + result.output);
            assertEqual(result.exitCode, 0);
        });

        it("still rejects a value that is not a member", []() {
            auto package = makePackage(kLibrary, R"KEX(using Web

let wrong(location: String) -> Web.Reply do
  location
end

main do
  IO.printLine("ok")
end
)KEX");
            auto result = runCheck(*package);
            assertTrue(result.output.find("but body returns String") !=
                           std::string::npos,
                       "expected a member mismatch, got: " + result.output);
            assertEqual(result.exitCode, 1);
        });

        it("accepts a record member of a union that also names a builtin", []() {
            auto package = makePackage(kLibraryWithBuiltin, R"KEX(using Web

let seeOther(location: String) -> Web.Reply do
  Response.Redirect { status: 303, location: location }
end

let safely(markup: String) -> Web.Reply = Safe { markup: markup }

main do
  IO.printLine("ok")
end
)KEX");
            auto result = runCheck(*package);
            assertTrue(result.output.find("No errors found") !=
                           std::string::npos,
                       "a builtin in the union broke its record members: " +
                           result.output);
            assertEqual(result.exitCode, 0);
        });

        it("accepts the builtin member of that same union", []() {
            auto package = makePackage(kLibraryWithBuiltin, R"KEX(using Web

let plain(text: String) -> Web.Reply = text

main do
  IO.printLine("ok")
end
)KEX");
            auto result = runCheck(*package);
            assertTrue(result.output.find("No errors found") !=
                           std::string::npos,
                       "the union refused its own String: " + result.output);
            assertEqual(result.exitCode, 0);
        });

        it("still rejects a non-member of a union that names a builtin", []() {
            auto package = makePackage(kLibraryWithBuiltin, R"KEX(using Web

let wrong(status: Integer) -> Web.Reply = status

main do
  IO.printLine("ok")
end
)KEX");
            auto result = runCheck(*package);
            assertTrue(result.output.find("but body returns Integer") !=
                           std::string::npos,
                       "expected an Integer to be rejected, got: " +
                           result.output);
            assertEqual(result.exitCode, 1);
        });

        it("verifies a call into the sibling module's function", []() {
            auto package = makePackage(R"KEX(module Web

let render(markup: String) -> String = markup
)KEX", R"KEX(using Web

main do
  IO.printLine(Web.render(42))
end
)KEX");
            auto result = runCheck(*package);
            assertTrue(result.output.find("render") != std::string::npos,
                       "expected the sibling's signature to be checked, got: " +
                           result.output);
            assertEqual(result.exitCode, 1);
        });
    });
    return runAll();
}
