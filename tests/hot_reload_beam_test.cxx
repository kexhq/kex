// Hot code reload of compiled Kex on a live BEAM.
//
// The BEAM keeps two versions of a module. A process moves to the newest one
// only through a fully-qualified call, and a process still running the older
// one is KILLED when a second reload purges it. So each case here compiles
// three versions of one module (v1, v2, v3), starts a long-running process on
// v1, loads v2 and then v3 into the same VM, and checks that the process both
// runs the new code and survives the purge of v1:
//
//  - a function that recurses from its receive arms (src/ir/upgrade.cxx
//    makes those calls remote);
//  - a `loop do receive ... end end` (lifted out of its function by the same
//    pass, so it has a remote call to recurse through);
//  - a `serving` server whose state record gains a field, whose module gains
//    a slot, and which then declares an `upgrade` hook
//    (kex_intrinsic_process: migrate/2, upgrade/2, current slot lookup).
//
// Several module versions loaded into one VM is the whole point, so like
// serving_imported_module_beam_test.cxx this drives the real binary and erl
// rather than living in spec/*.kex.
#include "test.hxx"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace test;
namespace fs = std::filesystem;

namespace {

auto writeFile(const fs::path& path, const std::string& contents) -> void {
    std::ofstream file(path);
    file << contents;
}

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

auto replaceAll(std::string text, const std::string& from, const std::string& to)
    -> std::string {
    for (size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

// Compiles each version of `srv.kex` into its own directory, then runs
// `driver` (an Erlang module named `driver` exporting run/0) against them.
// Every version keeps the file name, so every version is the module kex_srv.
auto runReload(const std::vector<std::string>& versions, const std::string& driver)
    -> std::string {
    char tmp[] = "/tmp/kex_hot_reload_XXXXXX";
    fs::path root = mkdtemp(tmp);
    for (size_t i = 0; i < versions.size(); i++) {
        auto dir = root / ("v" + std::to_string(i + 1));
        fs::create_directories(dir);
        writeFile(dir / "srv.kex", versions[i]);
        auto output = popenCapture(std::string(KEX_BINARY_PATH) +
                                   " --compile --no-colors -o " + dir.string() +
                                   " " + (dir / "srv.kex").string() + " 2>&1");
        if (!fs::exists(dir / "kex_srv.beam")) {
            fs::remove_all(root);
            return "v" + std::to_string(i + 1) + " failed to compile:\n" + output;
        }
    }
    writeFile(root / "driver.erl", replaceAll(driver, "ROOT", root.string()));
    // No `cd`: erl and erlc resolve from the working directory ctest gives
    // (the source tree), so a version manager picks the same Erlang there as
    // for the rest of the build — not whatever a temp directory defaults to.
    auto erlc = popenCapture("erlc -o " + root.string() + " " +
                             (root / "driver.erl").string() + " 2>&1");
    // v1's directory also holds the runtime beams `--compile` copies next to
    // the program; the driver loads each kex_srv version explicitly.
    auto output = popenCapture("erl -noshell -pa " + (root / "v1").string() +
                               " -pa " + root.string() +
                               " -eval 'driver:run(), halt().' 2>&1");
    std::error_code ec;
    fs::remove_all(root, ec);
    return erlc + output;
}

// Loading helpers shared by every driver: `load(N)` makes vN the current
// version of kex_srv, purging the one before the previous.
constexpr const char* kDriverPrelude = R"ERL(-module(driver).
-export([run/0]).
load(N) ->
    code:purge(kex_srv),
    {module, kex_srv} = code:load_abs("ROOT/v" ++ integer_to_list(N) ++ "/kex_srv"),
    ok.
)ERL";

// ---- A function that recurses from its receive arms ----------------------

constexpr const char* kRecursiveLoop = R"KEX(type Msg = Inc | Get(Pid)

foul serve(count: Integer) do
  receive do
    Inc => serve(count + STEP)
    Get(sender) => do
      sender.send(count)
      serve(count)
    end
  end
end

foul start = spawn do serve(0) end
)KEX";

// ---- A `loop do receive` in a spawned block ------------------------------

constexpr const char* kImperativeLoop = R"KEX(type Msg = Inc | Get(Pid)

foul start = spawn do
  var count = 0
  loop do
    receive do
      Inc => count = count + STEP
      Get(sender) => sender.send(count)
    end
  end
end
)KEX";

// Each Inc is answered by whichever version is current once the previous
// message has been handled. The one message a process was already waiting
// for when a reload landed is still handled by the old code — that is where
// the process makes its remote call into the new version.
constexpr const char* kLoopDriver = R"ERL(
fetch(P) -> P ! {'Get', self()}, receive N -> N after 2000 -> timeout end.
run() ->
    load(1),
    P = kex_srv:start(#{}),
    P ! 'Inc',
    io:format("v1 ~p~n", [fetch(P)]),
    load(2),
    P ! 'Inc', P ! 'Inc',
    io:format("v2 ~p~n", [fetch(P)]),
    io:format("on old code ~p~n", [erlang:check_process_code(P, kex_srv)]),
    load(3),
    io:format("alive after purge ~p~n", [is_process_alive(P)]),
    P ! 'Inc', P ! 'Inc',
    io:format("v3 ~p~n", [fetch(P)]).
)ERL";

// ---- A serving server across a state-shape change ------------------------

constexpr const char* kServingV1 = R"KEX(record Counter do
  n : Integer
end

serving Counter do
  slot bump -> Reply<Integer> do
    new.n = @n + 1
    return { new, reply: new.n }
  end
end

foul start = Process.spawn(Counter { n: 0 })
)KEX";

// Gains a field with a default, and a slot.
constexpr const char* kServingV2 = R"KEX(record Counter do
  n : Integer
  step : Integer = 10
end

serving Counter do
  slot bump -> Reply<Integer> do
    new.n = @n + @step
    return { new, reply: new.n }
  end

  slot peek -> Reply<Integer> = { reply: @n }
end

foul start = Process.spawn(Counter { n: 0 })
)KEX";

// Same layout; declares an `upgrade` hook, which runs once on the live state.
constexpr const char* kServingV3 = R"KEX(record Counter do
  n : Integer
  step : Integer = 10
end

serving Counter do
  slot bump -> Reply<Integer> do
    new.n = @n + @step
    return { new, reply: new.n }
  end

  slot peek -> Reply<Integer> = { reply: @n }

  let upgrade -> Counter = Counter { n: @n, step: 100 }
end

foul start = Process.spawn(Counter { n: 0 })
)KEX";

constexpr const char* kServingDriver = R"ERL(
call(S, M) -> kex_intrinsic_process:server_call(S, M, [], default).
run() ->
    process_flag(trap_exit, true),
    load(1),
    S = kex_srv:start(#{}),
    {'Server', Pid, _} = S,
    io:format("v1 bump ~p~n", [call(S, bump)]),
    load(2),
    io:format("v2 bump ~p~n", [call(S, bump)]),
    io:format("v2 peek ~p~n", [call(S, peek)]),
    load(3),
    io:format("v3 bump ~p~n", [call(S, bump)]),
    io:format("v3 bump ~p~n", [call(S, bump)]),
    io:format("alive after purge ~p~n", [is_process_alive(Pid)]).
)ERL";

} // namespace

int main() {
    describe("hot code reload of a receive loop", []() {
        it("moves a self-recursive receive loop onto each new version", []() {
            std::vector<std::string> versions;
            for (const char* step : {"1", "10", "100"})
                versions.push_back(replaceAll(kRecursiveLoop, "STEP", step));
            auto output = runReload(versions,
                                    std::string(kDriverPrelude) + kLoopDriver);
            assertEqual(output,
                        std::string("v1 1\n"
                                    "v2 12\n"
                                    "on old code false\n"
                                    "alive after purge true\n"
                                    "v3 122\n"),
                        "the process must follow each reload and survive the purge");
        });

        it("moves a `loop do receive` onto each new version", []() {
            std::vector<std::string> versions;
            for (const char* step : {"1", "10", "100"})
                versions.push_back(replaceAll(kImperativeLoop, "STEP", step));
            auto output = runReload(versions,
                                    std::string(kDriverPrelude) + kLoopDriver);
            assertEqual(output,
                        std::string("v1 1\n"
                                    "v2 12\n"
                                    "on old code false\n"
                                    "alive after purge true\n"
                                    "v3 122\n"),
                        "the lifted loop must follow each reload and survive the purge");
        });
    });

    describe("hot code reload of a serving server", []() {
        it("migrates the state, finds new slots and runs the upgrade hook", []() {
            auto output = runReload({kServingV1, kServingV2, kServingV3},
                                    std::string(kDriverPrelude) + kServingDriver);
            assertEqual(output,
                        std::string("v1 bump {'Ok',1}\n"
                                    // the new field took its default, 10
                                    "v2 bump {'Ok',11}\n"
                                    "v2 peek {'Ok',11}\n"
                                    // the hook set step to 100, once
                                    "v3 bump {'Ok',111}\n"
                                    "v3 bump {'Ok',211}\n"
                                    "alive after purge true\n"),
                        "the server must keep its state across both reloads");
        });
    });

    return runAll();
}
