// A unit whose expansion reads another file (`Kex.embed`, a template) is
// cached keyed by what it read (kexhq/kex#672): reused while that file is
// unchanged, and rebuilt, with the new contents, once it changes.
#include "test.hxx"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace test;

namespace {

struct Package {
    std::filesystem::path root;

    Package() {
        root = std::filesystem::temp_directory_path() /
               ("kex-unit-cache-embed-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        write("lib/page.kex", "module Page\n\ntext : String\nlet text = Kex.embed(\"page.txt\")\n");
        write("app/main.kex", "using Page\n\nmain do\n  IO.print(Page.text)\nend\n");
    }

    ~Package() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    auto write(const std::string& path, const std::string& contents) -> void {
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream file(root / path);
        file << contents;
    }

    static auto read(const std::filesystem::path& path) -> std::string {
        std::ifstream file(path);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // app/main.kex built unit by unit into a fresh directory: the build's own
    // report (with KEX_TIMINGS, which names each unit the cache answered).
    auto build() -> std::string {
        const auto out = root / "out";
        std::filesystem::remove_all(out);
        std::filesystem::create_directories(out);
        const auto report = root / "build.out";
        const auto command = "KEX_TIMINGS=1 KEX_CACHE=" + (root / "cache").string() + " " +
                             std::string(KEX_BINARY_PATH) + " --no-colors -c -o " + out.string() +
                             " --source-root " + (root / "app").string() + " --source-root " +
                             (root / "lib").string() + " " + (root / "app" / "main.kex").string() +
                             " > " + report.string() + " 2>&1";
        runShell(command);
        return read(report);
    }

    // What the program last built prints.
    auto run() -> std::string {
        const auto output = root / "run.out";
        const auto command = "cd " + (root / "out").string() +
                             " && erl -noshell -pa . -eval 'kex_main:main(), halt().' > " +
                             output.string() + " 2>&1";
        runShell(command);
        return read(output);
    }

    auto pageHit(const std::string& report) const -> bool {
        return report.find(
                   "unit cache hit: " +
                   std::filesystem::weakly_canonical(root / "lib" / "page.kex").string()) !=
               std::string::npos;
    }
};

} // namespace

int main() {
    describe("the unit cache and what a unit embeds", []() {
        it("reuses an embedding unit until the embedded file changes", []() {
            Package package;
            package.write("lib/page.txt", "first\n");
            const auto cold = package.build();
            assertEqual(package.run(), std::string("first\n"));
            assertTrue(!package.pageHit(cold), "a cold build answered from the cache: " + cold);

            const auto warm = package.build();
            assertTrue(package.pageHit(warm), "the unchanged unit was rebuilt: " + warm);
            assertEqual(package.run(), std::string("first\n"));

            // A different size, so the file's stamp differs even within the
            // same second.
            package.write("lib/page.txt", "second, longer\n");
            const auto changed = package.build();
            assertTrue(
                !package.pageHit(changed),
                "the unit was answered from the cache after what it embeds changed: " + changed);
            assertEqual(package.run(), std::string("second, longer\n"));
        });
    });
    return runAll();
}
