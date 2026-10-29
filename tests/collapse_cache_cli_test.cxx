// A builder chain collapsed by one build is kept for the next
// (compiled::setReuseCollapses), and must be decided again as soon as
// anything it was made from changes — including a module only its sandbox
// loaded — and never kept at all when its evaluation reached outside the
// program.
#include "test.hxx"
#include <chrono>
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
        char path[] = "/tmp/kex_collapse_cache_XXXXXX";
        root = mkdtemp(path);
        std::filesystem::create_directories(root / "app");
        std::filesystem::create_directories(root / "lib/words");
        std::filesystem::create_directories(root / "cache");
        write("lib/words/text.kex", R"KEX(module Words.Text

greeting : String
let greeting() = "first"
)KEX");
        write("lib/words/build.kex", R"KEX(module Words.Build

using Words.Text, only: [greeting]

compiled do
  let label(name: String) -> String = "${greeting()} ${name}"

  foul stamp(name: String) -> String = "${ENV.get("KEX_COLLAPSE_PROBE", "unset")} ${name}"

  # Pure as written, but the intrinsic reads a file the program is not made of.
  let noted(name: String) -> String = "${Kex.Intrinsic.File.read(")KEX" +
              (root / "note.txt").string() + R"KEX(").or("none")} ${name}"
end
)KEX");
        write("note.txt", "one");
        // Each in a file of its own: a round that reads the environment is
        // not kept, and with it everything else it decided.
        write("app/main.kex", R"KEX(using Words.Build, only: [label]

main do
  IO.printLine(label("kex"))
end
)KEX");
        write("app/stamped.kex", R"KEX(using Words.Build, only: [stamp]

main do
  IO.printLine(stamp("kex"))
end
)KEX");
        write("app/noted.kex", R"KEX(using Words.Build, only: [noted]

main do
  IO.printLine(noted("kex"))
end
)KEX");
    }

    ~Package() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    auto write(const std::string& path, const std::string& contents) -> void {
        std::filesystem::create_directories((root / path).parent_path());
        {
            std::ofstream file(root / path);
            file << contents;
        }
        // A rewrite within the same tick as the last build must still look
        // changed, and every file must look older than the build that reads
        // it (a file changed while a build runs keeps it from being cached):
        // each write is dated in the past, each a little later than the one
        // before.
        static int writes = 0;
        std::error_code error;
        std::filesystem::last_write_time(
            root / path,
            std::filesystem::file_time_type::clock::now() - std::chrono::hours(1) +
                std::chrono::seconds(2 * ++writes),
            error);
    }

    auto run(const std::string& probe, const std::string& entry = "main.kex",
             const std::string& flags = "-q") -> std::string {
        const auto output = root / "output";
        const auto command = "KEX_CACHE=" + (root / "cache").string() +
            " KEX_COLLAPSE_PROBE=" + probe + " " + std::string(KEX_BINARY_PATH) +
            " --no-colors " + flags + " --source-root " + (root / "app").string() +
            " --source-root " + (root / "lib").string() + " " +
            (root / "app" / entry).string() + " > " + output.string() + " 2>&1";
        const int status = std::system(command.c_str());
        std::ifstream file(output);
        std::stringstream buffer;
        buffer << file.rdbuf();
        assertTrue(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                   "build failed: " + buffer.str());
        return buffer.str();
    }

    auto keptRounds() const -> int {
        int count = 0;
        std::error_code error;
        const auto kept = root / "cache" / "collapse";
        if (!std::filesystem::is_directory(kept, error)) return 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(kept, error))
            if (entry.path().extension() == ".round") count++;
        return count;
    }
};

} // namespace

int main() {
    describe("collapses kept between builds", []() {
        it("are kept, and decided again when a module the sandbox loaded changes", []() {
            Package package;
            auto text = package.run("one");
            assertTrue(text.find("first kex\n") != std::string::npos,
                       "unexpected first build: " + text);
            assertTrue(package.keptRounds() > 0, "the first build kept no collapse");
            text = package.run("one");
            assertTrue(text.find("first kex\n") != std::string::npos,
                       "unexpected second build: " + text);
            package.write("lib/words/text.kex", R"KEX(module Words.Text

greeting : String
let greeting() = "second"
)KEX");
            text = package.run("one");
            assertTrue(text.find("second kex\n") != std::string::npos,
                       "a kept collapse outlived the module it was made from: " + text);
        });
        it("never keep one that read the environment", []() {
            Package package;
            auto text = package.run("one", "stamped.kex");
            assertTrue(text.find("one kex\n") != std::string::npos,
                       "unexpected first build: " + text);
            text = package.run("two", "stamped.kex");
            assertTrue(text.find("two kex\n") != std::string::npos,
                       "a collapse that read ENV was kept for the next build: " + text);
        });
        it("never keep one that read a file, even through a pure function", []() {
            Package package;
            auto text = package.run("one", "noted.kex");
            assertTrue(text.find("one kex\n") != std::string::npos,
                       "unexpected first build: " + text);
            package.write("note.txt", "two");
            text = package.run("one", "noted.kex");
            assertTrue(text.find("two kex\n") != std::string::npos,
                       "a collapse that read a file was kept for the next build: " + text);
        });
    });
    describe("runs kept by their entry", []() {
        it("start from the modules of an unchanged build", []() {
            Package package;
            auto text = package.run("one", "main.kex", "--progress");
            assertTrue(text.find("Cached build") == std::string::npos,
                       "the first build was cached: " + text);
            text = package.run("one", "main.kex", "--progress");
            assertTrue(text.find("Cached build") != std::string::npos &&
                           text.find("first kex\n") != std::string::npos,
                       "an unchanged build was compiled again: " + text);
        });
        it("build again when a new file takes a module's name", []() {
            Package package;
            package.run("one");
            auto text = package.run("one");
            assertTrue(text.find("first kex\n") != std::string::npos,
                       "unexpected second build: " + text);
            // The application's root comes first, so this is Words.Text now.
            package.write("app/words/text.kex", R"KEX(module Words.Text

greeting : String
let greeting() = "shadowing"
)KEX");
            text = package.run("one");
            assertTrue(text.find("shadowing kex\n") != std::string::npos,
                       "a kept build outlived a module taken over by a new file: " + text);
        });
    });
    return runAll();
}
