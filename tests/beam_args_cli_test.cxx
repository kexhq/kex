// Regression test for kexhq/kex#837: `kex -r prog.kex <args>` used to hand the
// program's arguments to `sh -c` unquoted. `"a b"` reached `main` as two
// arguments, an argument with a `"` in it aborted with a shell syntax error,
// and `$(...)` or backticks in one RAN. The BEAM runners now start erl with an
// argument vector, so every argument arrives exactly as given. Drives the real
// `kex` binary, the only place this lives, and starts it with an argument
// vector itself so the test's own launch adds no quoting of its own.
#include "test.hxx"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace test;
namespace fs = std::filesystem;

namespace {

struct Run {
    std::string output; // stdout and stderr together
    int status = -1;
};

auto runKex(const std::vector<std::string>& args) -> Run {
    std::vector<std::string> command{KEX_BINARY_PATH};
    command.insert(command.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : command) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    int out[2];
    if (pipe(out) != 0) {
        return {};
    }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        close(out[0]);
        close(out[1]);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(out[1]);
    Run run;
    std::array<char, 4096> buf;
    ssize_t n;
    while ((n = read(out[0], buf.data(), buf.size())) > 0) {
        run.output.append(buf.data(), static_cast<size_t>(n));
    }
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    run.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return run;
}

// Prints how many arguments it got, then each one between `<` and `>`.
const std::string kProgram = "main(args) do\n"
                             "  IO.printLine(args.count.to(String))\n"
                             "  args.each { |arg| IO.printLine(\"<\" + arg + \">\") }\n"
                             "end\n";

// Every one of these broke, split, or ran under `sh -c`.
const std::vector<std::string> kArgs = {
    "a b",
    "say \"hi\"",
    "it's",
    "$(echo INJECTED)",
    "`echo INJECTED`",
    "",
    "$HOME",
    "; echo INJECTED",
    "back\\slash",
    "*",
};

auto expectedOutput() -> std::string {
    std::string expected = std::to_string(kArgs.size()) + "\n";
    for (const auto& arg : kArgs) {
        expected += "<" + arg + ">\n";
    }
    return expected;
}

} // namespace

int main() {
    // A directory of its own, with a quote and a space in its name: the paths
    // kex hands erl (and splices into its -eval) must survive them too.
    char tmp[] = "/tmp/kex_beam_args_test_XXXXXX";
    const fs::path root = mkdtemp(tmp);
    const fs::path dir = root / "it's a \"dir\"";
    fs::create_directories(dir);
    const auto source = (dir / "args.kex").string();
    {
        std::ofstream f(source);
        f << kProgram;
    }

    describe("BEAM program arguments (kexhq/kex#837)", [&]() {
        it("reach main verbatim from `kex -r prog.kex`", [&]() {
            std::vector<std::string> args{"-r", "--no-colors", source};
            args.insert(args.end(), kArgs.begin(), kArgs.end());
            const auto run = runKex(args);
            assertEqual(run.output, expectedOutput());
            assertEqual(run.status, 0);
        });

        it("reach main verbatim from `kex -r prog.beam`", [&]() {
            const auto out = (dir / "out").string();
            const auto built = runKex({"-c", "--quiet", "--no-colors", "-o", out, source});
            assertEqual(built.status, 0, built.output);
            // The entry module is `kex_<stem>`, beside the stdlib's beams.
            const fs::path beam = fs::path(out) / "kex_args.beam";
            assertTrue(fs::exists(beam), "expected `kex -c` to write kex_args.beam");
            std::vector<std::string> args{"-r", "--no-colors", beam.string()};
            args.insert(args.end(), kArgs.begin(), kArgs.end());
            const auto run = runKex(args);
            assertEqual(run.output, expectedOutput());
            assertEqual(run.status, 0);
        });

        it("still passes the program's exit status through", [&]() {
            const auto exiting = (dir / "exiting.kex").string();
            {
                std::ofstream f(exiting);
                f << "main(args) do\n  System.exit(args.count + 2)\nend\n";
            }
            const auto run = runKex({"-r", "--no-colors", exiting, "x"});
            assertEqual(run.status, 3, run.output);
        });
    });

    const int result = runAll();
    fs::remove_all(root);
    return result;
}
