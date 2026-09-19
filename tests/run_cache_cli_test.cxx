// Regression test for kexhq/kex#372: the `--run` run cache's fingerprint
// used to depend only on `kGitRevision` + the stdlib sources' size/mtime,
// never on the `kex` binary itself. Rebuilding the compiler from uncommitted
// changes without moving the git HEAD (the ordinary state while iterating on
// it) left the cache unable to tell the new binary apart from the one that
// populated a given entry, so a stale `.beam`/`mainArity` could be served
// forever until something else in the fingerprint happened to change. Drives
// the real `kex` binary (this bug lives entirely in main.cxx's cache-key
// material, unreachable from the in-process test harness) against an
// isolated `KEX_CACHE` directory, so it never touches — or is affected by —
// the real `~/.cache/kex` a developer or another concurrent process is using.
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

auto runKex(const std::string& cacheDir, const std::string& sourcePath) -> std::string {
    std::string cmd = "KEX_CACHE=" + cacheDir + " " + std::string(KEX_BINARY_PATH) +
                       " --run --no-colors " + sourcePath + " 2>&1";
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        std::array<char, 4096> buf;
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) result.append(buf.data(), n);
        pclose(pipe);
    }
    return result;
}

auto writeTempSource(const std::string& source) -> std::string {
    char tmp[] = "/tmp/kex_run_cache_src_XXXXXX";
    int fd = mkstemp(tmp);
    close(fd);
    std::string path = std::string(tmp) + ".kex";
    std::rename(tmp, path.c_str());
    { std::ofstream f(path); f << source; }
    return path;
}

// The number of distinct per-key subdirectories under `<cacheDir>/run`: a
// cache HIT reuses the same directory across calls, a MISS creates another.
auto runCacheEntryCount(const std::string& cacheDir) -> size_t {
    std::error_code ec;
    const auto runDir = fs::path(cacheDir) / "run";
    if (!fs::exists(runDir, ec)) return 0;
    size_t count = 0;
    for (const auto& shard : fs::directory_iterator(runDir, ec)) {
        if (!shard.is_directory()) continue;
        for (const auto& entry : fs::directory_iterator(shard.path(), ec)) {
            if (entry.is_directory()) ++count;
        }
    }
    return count;
}

// Backdates the binary's mtime rather than touching it forward: some
// filesystems/`touch` truncate mtime resolution, and a backdate is always
// observably different from "whatever it is right now" regardless of
// resolution or clock skew — the same property an actual rebuild has.
auto backdateFile(const std::string& path) -> void {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return;
    struct utimbuf times{};
    times.actime = st.st_atime;
    times.modtime = st.st_mtime - 3600;
    utime(path.c_str(), &times);
}

} // namespace

int main() {
    describe("Run cache — fingerprint reacts to the compiler binary itself", []() {
        it("misses the cache when the kex binary's mtime changes under an unchanged git HEAD", []() {
            char cacheTmp[] = "/tmp/kex_run_cache_test_XXXXXX";
            std::string cacheDir = mkdtemp(cacheTmp);
            auto path = writeTempSource(
                "main do\n"
                "  IO.printLine(\"cached\")\n"
                "end\n");

            auto first = runKex(cacheDir, path);
            assertEqual(first, std::string("cached\n"));
            auto afterFirst = runCacheEntryCount(cacheDir);
            assertTrue(afterFirst >= 1, "expected the first run to populate a cache entry");

            backdateFile(KEX_BINARY_PATH);

            auto second = runKex(cacheDir, path);
            assertEqual(second, std::string("cached\n"));
            auto afterSecond = runCacheEntryCount(cacheDir);
            assertTrue(afterSecond > afterFirst,
                       "a changed binary mtime should mint a new cache entry, not reuse "
                       "the old one (kexhq/kex#372)");

            std::remove(path.c_str());
            fs::remove_all(cacheDir);
        });
    });

    describe("Run cache — Kex.embed reads outside a `compiled do` block", []() {
        it("never caches, so the embedded file's current content always wins", []() {
            // `Template.text`/`Template.html` call `Kex.embed` directly, with
            // no `compiled do ... end` wrapper for `sourceMentionsCompiledBlock`
            // to see — the cache key used to be blind to this input entirely,
            // so the SECOND run silently kept serving the first run's embedded
            // text even after the file on disk changed. The fix opts an
            // embed-calling source out of the run cache entirely, the same way
            // an actual `compiled do` block already does: no entry is ever
            // written for it, so there is nothing stale to serve.
            char cacheTmp[] = "/tmp/kex_run_cache_embed_test_XXXXXX";
            std::string cacheDir = mkdtemp(cacheTmp);
            auto path = writeTempSource(
                "main do\n"
                "  let t = Template.text(Kex.embed(\"greeting.txt\"))\n"
                "  IO.print(t())\n"
                "end\n");
            auto dir = fs::path(path).parent_path();
            auto embedPath = dir / "greeting.txt";

            { std::ofstream f(embedPath); f << "version one"; }
            auto first = runKex(cacheDir, path);
            assertEqual(first, std::string("version one"));
            assertEqual(runCacheEntryCount(cacheDir), size_t{0},
                        "a `Kex.embed` call must opt the run out of the cache "
                        "entirely, exactly like a `compiled do` block does");

            { std::ofstream f(embedPath); f << "version two"; }
            auto second = runKex(cacheDir, path);
            assertEqual(second, std::string("version two"),
                        "changing the embedded file must not serve the first "
                        "run's stale text");

            std::remove(path.c_str());
            std::remove(embedPath.c_str());
            fs::remove_all(cacheDir);
        });
    });

    return runAll();
}
