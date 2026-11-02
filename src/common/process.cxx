#include "process.hxx"

#include <cstdlib>
#include <sys/wait.h>
#include <unordered_map>

#ifndef __EMSCRIPTEN__
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace kex::process {

// `std::system` hands back a wait STATUS, not an exit code — returning it
// straight from main() truncates to the low 8 bits, which are zero for every
// normal exit: `System.exit(3)` reported 0 under `-R` where the walker
// reported 3, and `die` reported 0 where the walker reported 1.
#ifdef __EMSCRIPTEN__
// The browser build has no processes to fork, signal, or wait on — and the
// POSIX headers the implementation below needs are not included there either.
// Keep the one entry point so every call site stays shared, and let
// `std::system` answer the way it did before the signal-forwarding version
// existed.
auto runShellCommand(const std::string &command) -> int {
  return std::system(command.c_str());
}
#else
// The pid of the program `runShellCommand` is currently waiting on, for the
// signal handler to forward to. A signal handler may only touch a
// `volatile sig_atomic_t`, so the pid lives here rather than in a closure.
static volatile sig_atomic_t g_childPid = -1;

// SIGINT is forwarded as SIGTERM, and that substitution is the whole reason a
// Ctrl+C stops leaking the port's OS child.
//
// SIGINT is the one signal Erlang cannot be handed: `os:set_signal(sigint,
// handle)` is a `badarg`, so it never reaches `erl_signal_server` and no Erlang
// code runs — `kex_child_guard`, which is what kills a port's child, is a
// gen_event handler there. It instead reaches the VM's BREAK handler, which
// takes TWO SIGINTs to abort: measured, the first leaves `beam.smp` running and
// the second kills it outright, cleaning up nothing. That is why Ctrl+C at a
// terminal left an HTTP server holding its port — the terminal signals the
// whole process group, so the VM took one SIGINT directly and one from this
// forward, hit the abort path, and the child outlived it.
//
// SIGTERM does reach `erl_signal_server`, and the guard on it is already proven
// clean end to end. So the child a `Process.stream` spawned is claimed on the
// way down instead of being orphaned. The other three signals pass through
// unchanged — only SIGINT has the break handler in the way.
extern "C" void forwardSignalToChild(int sig) {
  if (g_childPid > 0)
    kill(static_cast<pid_t>(g_childPid), sig == SIGINT ? SIGTERM : sig);
}

// The child stays in this process group deliberately, so a terminal's
// Ctrl+C keeps reaching `erl` directly the way it already did. What is new
// is the second path: a signal delivered to the LAUNCHER alone now reaches
// the child too, and a launcher going down takes the child with it.
auto runShellCommand(const std::string &command) -> int {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    // Whatever the parent chose to ignore, the program itself should not.
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);
  }
  g_childPid = pid;
  struct sigaction forward {};
  forward.sa_handler = forwardSignalToChild;
  sigemptyset(&forward.sa_mask);
  forward.sa_flags = 0; // no SA_RESTART: waitpid must return EINTR
  struct sigaction previousInt {}, previousTerm {}, previousHup {},
      previousQuit {};
  sigaction(SIGINT, &forward, &previousInt);
  sigaction(SIGTERM, &forward, &previousTerm);
  sigaction(SIGHUP, &forward, &previousHup);
  sigaction(SIGQUIT, &forward, &previousQuit);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR)
      continue; // a forwarded signal; keep waiting for the child to act on it
    // Waiting failed for a reason retrying cannot fix. Never leave the child
    // behind holding its port — that is the orphaned `beam.smp` this exists
    // to prevent.
    kill(pid, SIGTERM);
    status = -1;
    break;
  }

  sigaction(SIGINT, &previousInt, nullptr);
  sigaction(SIGTERM, &previousTerm, nullptr);
  sigaction(SIGHUP, &previousHup, nullptr);
  sigaction(SIGQUIT, &previousQuit, nullptr);
  g_childPid = -1;
  return status;
}
#endif

auto exitStatusOf(int waitStatus) -> int {
  if (WIFEXITED(waitStatus))
    return WEXITSTATUS(waitStatus);
  if (WIFSIGNALED(waitStatus))
    return 128 + WTERMSIG(waitStatus);
  return waitStatus == 0 ? 0 : 1;
}

// Runs `commands` side by side and calls `finished(index, status)` for each as
// it ends, in the order they end — which is what lets a build say how far its
// erlc runs have got while they run. Used for erlc only: these are short
// compiler runs in the build's own process group, so a Ctrl+C at the terminal
// reaches them directly and nothing needs forwarding (runShellCommand's
// reason to exist does not apply).
auto runShellCommandsTogether(
    const std::vector<std::string> &commands,
    const std::function<void(std::size_t, int)> &finished) -> void {
#ifdef __EMSCRIPTEN__
  for (std::size_t i = 0; i < commands.size(); ++i)
    finished(i, runShellCommand(commands[i]));
#else
  std::unordered_map<pid_t, std::size_t> running;
  for (std::size_t i = 0; i < commands.size(); ++i) {
    const pid_t pid = fork();
    if (pid < 0) {
      finished(i, -1);
      continue;
    }
    if (pid == 0) {
      execl("/bin/sh", "sh", "-c", commands[i].c_str(), nullptr);
      _exit(127);
    }
    running.emplace(pid, i);
  }
  while (!running.empty()) {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, 0);
    if (pid < 0) {
      if (errno == EINTR) continue;
      // Nothing left to wait for that we know of; report the rest failed.
      for (const auto &[_, index] : running) finished(index, -1);
      return;
    }
    if (const auto found = running.find(pid); found != running.end()) {
      const auto index = found->second;
      running.erase(found);
      finished(index, status);
    }
  }
#endif
}

// `kex_test:configure(Mode, Filters), ` for the `-eval` the BEAM runner
// builds, or "" for an ordinary run (kexhq/kex#199). Like the Mock.* grant
// next to it, the reporting mode is the RUNNER's to set: an emitted .beam
// handed straight to `erl` prints the ✓/✗ prose a person reads.
auto beamTestGate(kex::interpreter::Evaluator::TestReportMode mode,
                  const std::vector<std::string> &filters) -> std::string {
  using Mode = kex::interpreter::Evaluator::TestReportMode;
  if (mode == Mode::Pretty && filters.empty()) return "";
  std::string modeAtom = mode == Mode::Json    ? "json"
                         : mode == Mode::List  ? "list"
                                               : "pretty";
  std::string list;
  for (const auto &filter : filters) {
    if (!list.empty()) list += ",";
    // An Erlang binary literal inside an -eval string: only `"` and `\` can
    // end it early, and both survive as themselves once escaped.
    list += "<<\"";
    for (char c : filter) {
      if (c == '"' || c == '\\') list += '\\';
      list += c;
    }
    list += "\"/utf8>>";
  }
  return "kex_test:configure(" + modeAtom + ", [" + list + "]), ";
}

auto shellSingleQuote(const std::string &s) -> std::string {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

#ifndef __EMSCRIPTEN__
auto BeamVm::start(const std::vector<std::string> &argv) -> bool {
  signal(SIGPIPE, SIG_IGN); // a dead erl shouldn't SIGPIPE the REPL
  int inPipe[2], outPipe[2];
  if (pipe(inPipe) != 0 || pipe(outPipe) != 0)
    return false;
  pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    ::close(inPipe[0]);
    ::close(inPipe[1]);
    ::close(outPipe[0]);
    ::close(outPipe[1]);
    std::vector<char *> args;
    for (auto &a : argv)
      args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  ::close(inPipe[0]);
  ::close(outPipe[1]);
  inFd = inPipe[1];
  outFd = outPipe[0];
  return true;
}

void BeamVm::writeLine(const std::string &s) {
  if (inFd < 0)
    return;
  std::string line = s + "\n";
  const char *p = line.data();
  size_t remaining = line.size();
  while (remaining > 0) {
    ssize_t n = write(inFd, p, remaining);
    if (n <= 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    p += n;
    remaining -= n;
  }
}

// Read child stdout line-by-line until a line beginning with
// `sentinelPrefix` (e.g. "KEX_REPL_DONE <nonce> "). Returns everything
// read before that line (the command's program output); sets `status` to
// the token ("ok"/"error") following the nonce on the sentinel line.
auto BeamVm::readUntilSentinel(const std::string &sentinelPrefix,
                               std::string &status) -> std::string {
  std::string collected, line;
  char ch;
  while (true) {
    ssize_t r = read(outFd, &ch, 1);
    if (r <= 0) {
      status = "eof";
      return collected;
    }
    if (ch == '\n') {
      if (line.rfind(sentinelPrefix, 0) == 0) {
        status = line.substr(sentinelPrefix.size());
        return collected;
      }
      collected += line;
      collected += '\n';
      line.clear();
    } else {
      line += ch;
    }
  }
}

void BeamVm::close() {
  if (inFd >= 0) {
    ::close(inFd);
    inFd = -1;
  }
  if (outFd >= 0) {
    ::close(outFd);
    outFd = -1;
  }
  if (pid > 0) {
    kill(pid, SIGTERM);
    int st;
    waitpid(pid, &st, 0);
    pid = -1;
  }
}
#endif

} // namespace kex::process
