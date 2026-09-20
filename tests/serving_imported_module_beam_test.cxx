// Regression test for kexhq/kex#377: a `serving` block declared in a plain
// IMPORTED module (not the entrypoint) failed its first slot call with
// `{'function not exported', ...}` the moment `Net.HTTP` was anywhere in the
// compiled program, even though neither the `serving` module nor the
// entrypoint actually called into `Net.HTTP`.
//
// Root cause: `kex_intrinsic_process:invoke_slot/2` decided whether the
// slot's plain arity or arity+1 (capability-context) form applies by asking
// `erlang:function_exported/3` — a BIF that answers about the CURRENTLY
// LOADED code only and, unlike an ordinary call, never triggers the
// autoloader. A `serving` block's own module is exactly a module nothing
// else in the program necessarily calls by name (`Process.spawn` only
// constructs the record and hands it to `kex_intrinsic_process`), so on the
// very first slot call the module was not yet loaded and the check reported
// `false` regardless of the real export list. That sent the call down the
// arity+1 branch, which — only once `Net.HTTP` (or anything else with a
// same-named, genuinely arity+1 capability-threaded method) was in the
// build — landed on that OTHER module's dispatcher instead, whose receiver
// guard never matches this slot's own type. Fixed by `code:ensure_loaded/1`
// before the check, matching the existing, already-correct pattern in
// `kex_compare.erl`'s own `function_exported` use.
//
// Two compilation units (the `serving` block in a sibling module, `Net.HTTP`
// pulled into the entrypoint) are the whole point, and this is runtime BEAM
// behavior, not a check — so, like multi_module_check_cli_test.cxx, this
// cannot be a `spec/*.kex`.
#include "test.hxx"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace test;
namespace fs = std::filesystem;

namespace {

auto writeFile(const fs::path& path, const std::string& contents) -> void {
    std::ofstream file(path);
    file << contents;
}

constexpr const char* kStoreLibrary = R"KEX(module Store

record Counter do
  n : Integer = 0
end

serving Counter do
  slot add(x: Integer) -> Reply<Integer> do
    new.n = @n + x
    return { new, reply: new.n }
  end
end
)KEX";

constexpr const char* kMainEntry = R"KEX(using Store
using Net.HTTP, only: [Headers]

main do
  let counter = Process.spawn(Store.Counter {})
  IO.printLine("${counter.add(5)}")
  IO.printLine("${counter.add(3)}")
end
)KEX";

auto popenCapture(const std::string& cmd) -> std::string {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        std::array<char, 4096> buf;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
            result.append(buf.data(), n);
        pclose(pipe);
    }
    return result;
}

// `--run` (compileRun) deliberately does not reproduce this bug: it loads
// every freshly emitted module with an explicit `code:load_binary/3` before
// calling `main/0` (see main.cxx's own comment on `loadExpr`, there
// specifically to stop autoload picking up a stale same-named module from
// an earlier compilation) — which, as a side effect, always has the
// `serving` module loaded before its first slot call, masking exactly the
// `function_exported`-before-load gap this test exists to catch. A
// persistent `--compile -o dir` build, then a SEPARATE `erl` invocation —
// the shape `tey`/a deployed release actually runs a compiled program in —
// loads modules lazily, the only way to observe it.
auto runKex(const fs::path& libRoot, const fs::path& entry, const fs::path& outDir)
    -> std::string {
    std::string compileCmd = std::string(KEX_BINARY_PATH) +
        " --compile --no-colors --source-root " + libRoot.string() +
        " -o " + outDir.string() + " " + entry.string() + " 2>&1";
    auto compileOutput = popenCapture(compileCmd);
    if (!fs::exists(outDir / "kex_main.beam"))
        return "compile failed:\n" + compileOutput;
    std::string runCmd = "erl -pa " + outDir.string() +
        " -noshell -eval 'kex_main:main(), halt().' 2>&1";
    return popenCapture(runCmd);
}

} // namespace

int main() {
    describe("serving slots across compilation units (kexhq/kex#377)", []() {
        it("dispatches a slot on its first call when Net.HTTP is also in the build", []() {
            char tmp[] = "/tmp/kex_serving_imported_XXXXXX";
            fs::path root = mkdtemp(tmp);
            fs::path lib = root / "lib";
            fs::create_directories(lib);
            writeFile(lib / "store.kex", kStoreLibrary);
            fs::path entry = root / "main.kex";
            writeFile(entry, kMainEntry);
            fs::path outDir = root / "ebin";
            fs::create_directories(outDir);

            auto output = runKex(lib, entry, outDir);
            assertEqual(output, std::string("5\n8\n"),
                        "the very first slot call must dispatch correctly, "
                        "not fall back to an unrelated same-named method "
                        "elsewhere in the build");

            std::error_code ec;
            fs::remove_all(root, ec);
        });
    });

    return runAll();
}
