#include "test.hxx"
#include "../src/common/prelude_loader.hxx"
#include "../src/module/resolver.hxx"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

using namespace test;

int main() {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / "kex-module-resolver-test";
    fs::remove_all(base);
    fs::create_directories(base / "lib/http");
    fs::create_directories(base / "src/http");
    fs::create_directories(base / "prelude");
    {
        std::ofstream libFile(base / "lib/http/router.kex");
        libFile << "module Http.Router\n";
        libFile.close();
        std::ofstream srcFile(base / "src/http/router.kex");
        srcFile << "module Http.Router\n";
        srcFile.close();
        std::ofstream containerFile(base / "lib/shop.kex");
        containerFile << "module Shop do\n  module Cart do\n  end\nend\n";
        containerFile.close();
        std::ofstream(base / "prelude/z.kex") << "module Z\n";
        std::ofstream(base / "prelude/a.kex") << "module A\n";
        std::ofstream(base / "prelude/ignored.txt") << "not kex\n";
    }

    describe("Module resolver", [&]() {
        it("maps dotted module names and keeps source-root precedence", [&]() {
            kex::module::Resolver resolver({(base / "lib").string(), (base / "src").string()});
            auto resolved = resolver.resolve("Http.Router");
            assertTrue(resolved.has_value());
            assertEqual(resolved->moduleName, std::string("Http.Router"));
            assertEqual(resolved->path, (base / "lib/http/router.kex").string());
            assertEqual(resolved->shadowedPaths.size(), size_t{1});
            assertEqual(resolved->shadowedPaths[0],
                        (base / "src/http/router.kex").string());
        });

        it("resolves a relative module against its enclosing module", [&]() {
            kex::module::Resolver resolver({(base / "lib").string()});
            auto resolved = resolver.resolve("Router", "Http");
            assertTrue(resolved.has_value());
            assertEqual(resolved->moduleName, std::string("Http.Router"));
        });

        it("falls back to a containing module file", [&]() {
            kex::module::Resolver resolver({(base / "lib").string()});
            auto resolved = resolver.resolve("Shop.Cart");
            assertTrue(resolved.has_value());
            assertEqual(resolved->moduleName, std::string("Shop.Cart"));
            assertEqual(resolved->path, (base / "lib/shop.kex").string());
        });

        it("does not source-resolve foreign module namespaces", [&]() {
            kex::module::Resolver resolver({(base / "lib").string()});
            assertTrue(!resolver.resolve("Erlang.GenServer").has_value());
            assertTrue(!resolver.resolve("Elixir.Phoenix.Router").has_value());
            assertTrue(!resolver.resolve("Gleam.Http").has_value());
        });
    });

    describe("Prelude source discovery", [&]() {
        it("prefers KEX_STDLIB_DIR and returns sorted Kex sources", [&]() {
            std::optional<std::string> previous;
            if (const char* value = std::getenv("KEX_STDLIB_DIR")) previous = value;
            setenv("KEX_STDLIB_DIR", (base / "prelude").c_str(), 1);

            const auto files = kex::preludeSourceFiles();

            if (previous) setenv("KEX_STDLIB_DIR", previous->c_str(), 1);
            else unsetenv("KEX_STDLIB_DIR");

            assertEqual(files.size(), size_t{2});
            assertEqual(files[0], (base / "prelude/a.kex").string());
            assertEqual(files[1], (base / "prelude/z.kex").string());
        });
    });

    const int result = test::runAll();
    fs::remove_all(base);
    return result;
}
