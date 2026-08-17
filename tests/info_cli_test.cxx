// `kex --info` is a CONTRACT, not a display. Tey reads it to learn the OTP
// release that compiled this build's runtime beams — the oldest that can load
// them — and enforces the resulting floor (see docs/tey-resolver-plan.md).
// `--version` is prose and may be reworded freely; this must not drift.
//
// Driving the real binary rather than calling printInfo(): the point is what a
// separate process sees on stdout, which is what a package manager will parse.
#include "test.hxx"
#include "../src/common/version.hxx"
#include <array>
#include <cstdio>
#include <string>

using namespace test;

namespace {

auto runKex(const std::string& args) -> std::string {
    std::string cmd = std::string(KEX_BINARY_PATH) + " " + args + " 2>/dev/null";
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    std::array<char, 4096> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0)
        result.append(buf.data(), n);
    pclose(pipe);
    return result;
}

// Enough of a JSON reader to assert on a flat object of strings, integers and
// nulls, which is all --info emits. A real parser would be a dependency this
// test does not need; what matters is that the bytes are shaped as promised.
auto valueOf(const std::string& json, const std::string& key) -> std::string {
    const auto at = json.find("\"" + key + "\":");
    if (at == std::string::npos) return "<missing>";
    auto start = at + key.size() + 3;
    if (start >= json.size()) return "<missing>";
    if (json[start] == '"') {
        const auto end = json.find('"', start + 1);
        if (end == std::string::npos) return "<unterminated>";
        return json.substr(start + 1, end - start - 1);
    }
    const auto end = json.find_first_of(",}", start);
    return json.substr(start, end - start);
}

auto isInteger(const std::string& text) -> bool {
    if (text.empty()) return false;
    for (char c : text)
        if (c < '0' || c > '9') return false;
    return true;
}

} // namespace

int main() {
    const std::string info = runKex("--info");

    describe("kex --info", [&]() {
        it("emits one line of JSON", [&]() {
            assertTrue(!info.empty());
            assertEqual(info.front(), '{');
            // One line: a tool reading this with a line-oriented pipe must get
            // the whole object, and a trailing newline is the only break.
            assertEqual(info.find('\n'), info.size() - 1);
            assertEqual(info[info.size() - 2], '}');
        });

        it("reports the version this binary actually is", [&]() {
            assertEqual(valueOf(info, "version"), kex::versionNumber());
        });

        it("reports runtime_otp_floor as an integer or null", [&]() {
            const auto floor = valueOf(info, "runtime_otp_floor");
            assertTrue(floor != "<missing>");
            // null when erlc was absent at build time, which disables the
            // check. Never 0 — a floor everything satisfies would read as a
            // real answer.
            assertTrue(floor == "null" || isInteger(floor));
            assertTrue(floor != "0");
        });

        it("carries every key a reader is promised", [&]() {
            for (const auto* key : {"version", "revision", "built",
                                    "runtime_otp_floor", "runtime_dir", "erl"})
                assertTrue(valueOf(info, key) != "<missing>");
        });

        it("quotes absent strings as null, not as empty strings", [&]() {
            // The distinction is load-bearing: "" would be a path that exists
            // and is empty, null is "this build has no such thing".
            const auto revision = valueOf(info, "revision");
            assertTrue(!revision.empty());
        });

        it("says nothing on stdout that is not the object", [&]() {
            // A banner, a warning, or a colour escape here would break every
            // parser. --info is machine output and must stay silent otherwise.
            assertEqual(info.find('\x1b'), std::string::npos);
        });
    });

    describe("kex --version", [&]() {
        it("stays human prose, and is not the same thing as --info", [&]() {
            const auto version = runKex("--version");
            assertTrue(version.rfind("kex ", 0) == 0);
            assertTrue(version.find('{') == std::string::npos);
        });
    });

    return runAll();
}
