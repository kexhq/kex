// The prelude interface cache must key on the compiler binary itself, not
// just the stdlib sources + MAJOR.MINOR.PATCH — otherwise a rebuild from
// uncommitted compiler changes at an unchanged version + unchanged stdlib
// keeps serving the previous build's checked interfaces (risk #2, the same
// class kexhq/kex#372 fixed for the run cache). Drives the real `kex`
// binary against an isolated `XDG_CACHE_HOME`, mirroring run_cache_cli_test.
#include "test.hxx"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

using namespace test;
namespace fs = std::filesystem;

namespace {

auto runKex(const std::string& cacheDir, const std::string& sourcePath,
            const std::string& binary) -> std::string {
    std::string cmd = std::string("KEX_CACHE=") + cacheDir + " XDG_CACHE_HOME=" +
                      cacheDir + " " + binary + " --run --no-colors " +
                      sourcePath + " 2>&1";
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

auto writeTempSource(const std::string& source) -> std::string {
    char tmp[] = "/tmp/kex_iface_cache_src_XXXXXX";
    int fd = mkstemp(tmp);
    close(fd);
    std::string path = std::string(tmp) + ".kex";
    std::rename(tmp, path.c_str());
    { std::ofstream f(path); f << source; }
    return path;
}

// Distinct `prelude-interface/*.bin` keys: a HIT reuses the same file, a
// MISS (new key) mints another.
auto interfaceCacheEntryCount(const std::string& cacheDir) -> size_t {
    std::error_code ec;
    // runKex sets KEX_CACHE=cacheDir, the root every cache lives under.
    const auto dir = fs::path(cacheDir) / "prelude-interface";
    if (!fs::exists(dir, ec)) return 0;
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".bin") ++count;
    }
    return count;
}

// Backdates the binary's mtime rather than touching it forward: a backdate
// is observably different from "whatever it is right now" regardless of
// mtime resolution or clock skew — the same property an actual rebuild has.
auto backdateFile(const std::string& path) -> void {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return;
    struct utimbuf times{};
    times.actime = st.st_atime;
    times.modtime = st.st_mtime - 3600;
    utime(path.c_str(), &times);
}

// A private copy of the real binary to backdate. Mutating KEX_BINARY_PATH
// itself would invalidate every mtime-keyed cache entry of every test
// running in parallel (ctest -j), flaking cache-reuse assertions elsewhere.
// The copy sits alongside the original: stdlib/runtime discovery is
// executable-relative, so only a sibling directory resolves the same way.
auto copyBinaryForBackdate() -> std::string {
    const auto dest = fs::path(KEX_BINARY_PATH).parent_path() /
                      ("kex-backdate-probe-" + std::to_string(getpid()));
    std::error_code ec;
    fs::copy_file(KEX_BINARY_PATH, dest,
                  fs::copy_options::overwrite_existing, ec);
    chmod(dest.c_str(), 0755);
    return dest.string();
}

} // namespace

int main() {
    describe("Prelude interface cache — key reacts to the compiler binary itself",
             []() {
                 it("mints a new entry when the kex binary's mtime changes under "
                    "an unchanged version + stdlib",
                    []() {
                        char cacheTmp[] = "/tmp/kex_iface_cache_test_XXXXXX";
                        std::string cacheDir = mkdtemp(cacheTmp);
                        auto path = writeTempSource(
                            "main do\n"
                            "  IO.printLine(\"cached\")\n"
                            "end\n");
                        const auto binary = copyBinaryForBackdate();

                        auto first = runKex(cacheDir, path, binary);
                        assertEqual(first, std::string("cached\n"));
                        auto afterFirst = interfaceCacheEntryCount(cacheDir);
                        assertTrue(afterFirst >= 1,
                                   "expected the first run to populate a cache entry");

                        backdateFile(binary);

                        auto second = runKex(cacheDir, path, binary);
                        assertEqual(second, std::string("cached\n"));
                        auto afterSecond = interfaceCacheEntryCount(cacheDir);
                        assertTrue(
                            afterSecond > afterFirst,
                            "a changed binary mtime should mint a new cache entry, "
                            "not reuse the old build's interfaces: " +
                                std::to_string(afterFirst) + " -> " +
                                std::to_string(afterSecond));

                        std::remove(path.c_str());
                        std::remove(binary.c_str());
                        fs::remove_all(cacheDir);
                    });
             });

    return runAll();
}
