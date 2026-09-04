#include "beam/beam_file.hxx"
#include "beam/collect_metadata.hxx"
#include "beam/etf.hxx"
#include "beam/kexi.hxx"
#include "beam/kexi_registry.hxx"
#include "beam/term_builder.hxx"
#include "ast/convert.hxx"
#include "common/artifact_versions.hxx"
#include "common/prelude_tiers.hxx"
#include "common/color.hxx"
#include "interpreter/evaluator.hxx"
#include "ir/emit_core.hxx"
#include "ir/lower.hxx"
#include "lexer/lexer.hxx"
#include "compiled/expand.hxx"
#include "module/resolver.hxx"
#include "parser/parser.hxx"
#include "semantic/analyzer.hxx"
#include "semantic/db.hxx"
#include "validation/tag_validator.hxx"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Pure string logic, no actual readline API dependency — needed
// unconditionally by `--complete`/`-K` (used by shell completion scripts,
// which shell out to `kex -K` rather than linking readline) and by the
// interactive REPL's `make X do` target tracking, which runs the same
// whether or not this build has readline (see the non-readline readLine()
// fallback below). Bundling this under #ifdef HAS_READLINE was an
// oversight that only ever surfaced when actually building without
// readline — every native dev machine so far has had it available via
// Homebrew.
#include "common/completion.hxx"
#ifdef KEX_HAS_LSP
#include "lsp/server.hxx"
#endif
#include "common/prelude_interfaces.hxx"
#include "common/prelude_loader.hxx"
#include "common/repl_commands.hxx"
// Set to the type name while the user is typing inside a `make X do` block,
// so the completer can infer parameter types from pattern signatures.
static std::string g_currentMakeTarget;

// Lexical incompleteness shared by both REPLs. Block (`do`/`end`) continuation
// is tracked separately because it is Kex grammar rather than delimiter state.
// This scanner deliberately ignores delimiters inside strings and comments.
static auto replHasOpenDelimiter(const std::string &source) -> bool {
  enum class Mode {
    Normal, DoubleString, Char, RawString, InterpolatedRawString, Comment
  };
  Mode mode = Mode::Normal;
  int parens = 0;
  int brackets = 0;
  int braces = 0;
  int interpolationBraces = 0;

  for (size_t i = 0; i < source.size(); i++) {
    char c = source[i];
    switch (mode) {
      case Mode::Comment:
        if (c == '\n') mode = Mode::Normal;
        break;
      case Mode::DoubleString:
        if (c == '\\' && i + 1 < source.size()) {
          i++;
        } else if (c == '"') {
          mode = Mode::Normal;
        }
        break;
      case Mode::Char:
        if (c == '\\' && i + 1 < source.size()) {
          i++;
        } else if (c == '\'') {
          mode = Mode::Normal;
        }
        break;
      case Mode::RawString:
        if (c == '`') {
          if (i + 1 < source.size() && source[i + 1] == '`')
            i++;
          else
            mode = Mode::Normal;
        }
        break;
      case Mode::InterpolatedRawString:
        if (c == '$' && i + 2 < source.size() &&
            source[i + 1] == '$' && source[i + 2] == '{') {
          i += 2;
        } else if (c == '$' && i + 1 < source.size() &&
                   source[i + 1] == '{') {
          interpolationBraces++;
          i++;
        } else if (interpolationBraces > 0 && c == '{') {
          interpolationBraces++;
        } else if (interpolationBraces > 0 && c == '}') {
          interpolationBraces--;
        } else if (c == '`') {
          if (i + 1 < source.size() && source[i + 1] == '`')
            i++;
          else
            mode = Mode::Normal;
        }
        break;
      case Mode::Normal:
        if (c == '#') mode = Mode::Comment;
        else if (c == '"') mode = Mode::DoubleString;
        else if (c == '\'') mode = Mode::Char;
        else if (c == '`')
          mode = i > 0 && source[i - 1] == '$'
                     ? Mode::InterpolatedRawString
                     : Mode::RawString;
        else if (c == '(') parens++;
        else if (c == ')' && parens > 0) parens--;
        else if (c == '[') brackets++;
        else if (c == ']' && brackets > 0) brackets--;
        else if (c == '{') braces++;
        else if (c == '}' && braces > 0) braces--;
        break;
    }
  }

  return mode == Mode::DoubleString || mode == Mode::RawString ||
         mode == Mode::InterpolatedRawString || interpolationBraces > 0 ||
         parens > 0 || brackets > 0 || braces > 0;
}

// Parser accumulates syntax errors instead of throwing, so every REPL path
// that parses user input has to ask for them. Without this the REPL evaluated
// whatever partial AST came back and printed its result: `1 + ,1` answered
// `None : Optional` rather than naming the stray comma, and the BEAM REPL
// printed nothing at all. Throwing routes the message through the same
// handler the REPL already uses for evaluation errors.
static auto throwOnParseErrors(const kex::Parser &parser) -> void {
  const auto &diagnostics = parser.diagnostics();
  if (diagnostics.empty()) return;
  std::string message;
  for (const auto &diagnostic : diagnostics) {
    // Each continuation line re-prints the caller's prefix so multiple
    // syntax errors read as a list rather than one run-on message.
    if (!message.empty()) message += "\n  error: ";
    message += diagnostic.message;
  }
  throw std::runtime_error(message);
}

// "3 type errors" / "1 type error" — the count is what makes a long
// fix-compile-repeat loop legible: it shows whether the last edit helped.
static auto errorCountPhrase(int count, const char *kind) -> std::string {
  return std::to_string(count) + " " + kind + (count == 1 ? " error" : " errors");
}

// The parameter patterns of a clause, with variable names flattened to `_`:
// `let fact(0)` → "(0)", `let fact(n)` and `let fact(k)` → "(_)". Two clauses
// with the same head match the same inputs, so entering one replaces the
// other; different heads accumulate. Returns nothing when the line is not a
// parameterised clause (a plain `let x = ...`, a record, a module).
static auto replClauseHead(const std::string &source)
    -> std::optional<std::string> {
  size_t offset = 0;
  if (source.rfind("foul ", 0) == 0) offset = 5;
  else if (source.rfind("let ", 0) == 0) offset = 4;
  else return std::nullopt;
  while (offset < source.size() &&
         std::isspace((unsigned char)source[offset])) offset++;
  if (offset < source.size() && std::isupper((unsigned char)source[offset]))
    return std::nullopt;
  size_t i = offset;
  while (i < source.size() &&
         (std::isalnum((unsigned char)source[i]) || source[i] == '_')) i++;
  if (i == offset) return std::nullopt;
  if (i < source.size() && (source[i] == '?' || source[i] == '!')) i++;
  if (i >= source.size() || source[i] != '(') return std::nullopt;

  std::string head;
  int depth = 0;
  bool inWord = false;
  std::string word;
  const auto flush = [&]() {
    if (!inWord) return;
    // A bare lowercase identifier is a binding, so any name stands for the
    // same set of inputs; a literal or constructor is matched as written.
    const bool binding =
        !word.empty() && (std::islower((unsigned char)word[0]) || word[0] == '_');
    head += binding ? "_" : word;
    word.clear();
    inWord = false;
  };
  for (; i < source.size(); i++) {
    const char c = source[i];
    if (std::isalnum((unsigned char)c) || c == '_') {
      word += c;
      inWord = true;
      continue;
    }
    flush();
    if (c == '(') depth++;
    if (std::isspace((unsigned char)c)) continue;
    // The type annotation on a parameter does not change what it matches.
    if (c == ':') {
      while (i + 1 < source.size() && source[i + 1] != ',' &&
             source[i + 1] != ')') i++;
      continue;
    }
    head += c;
    if (c == ')') {
      if (--depth == 0) return head;
    }
  }
  return std::nullopt;
}

// Stored definitions hold one clause per entry, appended with newlines. A
// clause continues while its `do` blocks are unclosed, so splitting cannot
// simply break on every newline. `countBlocks` is the caller's own block
// counter (each REPL has one).
template <typename CountBlocks>
static auto splitReplClauses(const std::string &source,
                             const CountBlocks &countBlocks)
    -> std::vector<std::string> {
  std::vector<std::string> clauses;
  std::string current;
  int blocks = 0;
  size_t start = 0;
  while (start <= source.size()) {
    const auto newline = source.find('\n', start);
    const auto line = source.substr(
        start, newline == std::string::npos ? std::string::npos : newline - start);
    if (!current.empty()) current += "\n";
    current += line;
    blocks += countBlocks(line);
    if (blocks <= 0 && !current.empty()) {
      clauses.push_back(current);
      current.clear();
      blocks = 0;
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  if (!current.empty()) clauses.push_back(current);
  return clauses;
}

static auto replDefinitionName(const std::string &source) -> std::string {
  size_t off = 0;
  if (source.rfind("foul module ", 0) == 0) off = 12;
  else if (source.rfind("foul ", 0) == 0) off = 5;
  else if (source.rfind("let ", 0) == 0) off = 4;
  else if (source.rfind("type ", 0) == 0) off = 5;
  else if (source.rfind("record ", 0) == 0) off = 7;
  else if (source.rfind("module ", 0) == 0) off = 7;
  else if (source.rfind("make ", 0) == 0) off = 5;
  else if (source.rfind("using ", 0) == 0) off = 6;
  while (off < source.size() && std::isspace((unsigned char)source[off])) off++;
  if (source.compare(off, 6, "final:") == 0) {
    off += 6;
    while (off < source.size() && std::isspace((unsigned char)source[off])) off++;
  }
  size_t end = off;
  while (end < source.size() &&
         (std::isalnum((unsigned char)source[end]) || source[end] == '_' ||
          source[end] == '.' || source[end] == '?' || source[end] == '!'))
    end++;
  return source.substr(off, end - off);
}

// `let UPPER = expr` is a zero-arg CONSTANT definition, the same rule the
// parser applies (parser.cxx, "UpperIdent after `let`"): an uppercase name is
// a constant unless `(` or `{` follows it, which makes it a constructor or
// named-record destructuring pattern instead. Both REPLs classify a line
// before parsing it, and lumping every uppercase name in with the patterns
// left `let BYTE_REGEX = ...` bound to the constant's own zero-arg function
// (interpreter) or rejected outright (BEAM).
static auto replIsUpperConstantBinding(const std::string &source, size_t off)
    -> bool {
  if (off == std::string::npos || off >= source.size()) return false;
  if (!std::isupper((unsigned char)source[off])) return false;
  size_t end = off;
  while (end < source.size() &&
         (std::isalnum((unsigned char)source[end]) || source[end] == '_'))
    end++;
  const auto next = source.find_first_not_of(" \t", end);
  if (next == std::string::npos) return false;
  return source[next] != '(' && source[next] != '{';
}

// Readline preserves whitespace typed or pasted before the first token. Kex's
// parser accepts that indentation, but the REPL classifies definitions before
// parsing them; comparing the raw line against "let ", "using ", etc. made an
// indented definition fall through to expression lowering. Only indentation
// outside the first token is removed—continuation-line and literal contents
// remain untouched.
static auto replTrimLeadingIndent(std::string source) -> std::string {
  const auto first = source.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  source.erase(0, first);
  return source;
}

// Wrap `s` as a single POSIX-shell single-quoted argument, escaping any
// embedded `'` via the standard close-quote/escaped-literal-quote/reopen
// idiom (`'\''`). Needed wherever an Erlang -eval string itself contains
// quoted atoms (e.g. a module name with a literal '.' in its stem, like
// "kex_json_parser.spec") — naively embedding those quotes directly inside
// an outer single-quoted shell argument is fragile: shell single-quote
// parsing is stateful (quote/unquote toggles as it scans), so whether a
// hand-placed escape sequence lands in "quoted" or "unquoted" context
// depends on the exact quote parity of everything BEFORE it, not just
// what's written at that one spot — verified the hard way (a `\''`
// sequence that worked correctly right after the argument's own opening
// quote broke once embedded deeper into a longer string, because an
// earlier quoted/unquoted flip had already changed the state at that
// point). Applying this uniformly across the whole string is what makes
// it position-independent and actually robust (spec/json_parser.spec.kex).
// `std::system` hands back a wait STATUS, not an exit code — returning it
// straight from main() truncates to the low 8 bits, which are zero for every
// normal exit: `System.exit(3)` reported 0 under `-R` where the walker
// reported 3, and `die` reported 0 where the walker reported 1.
#ifdef __EMSCRIPTEN__
// The browser build has no processes to fork, signal, or wait on — and the
// POSIX headers the implementation below needs are not included there either
// (see the guard at the top of this file). Keep the one entry point so every
// call site stays shared, and let `std::system` answer the way it did before
// the signal-forwarding version existed.
static auto runShellCommand(const std::string &command) -> int {
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

// Run a shell command and wait for it, the way `std::system` does — except
// that POSIX requires `system()` to IGNORE SIGINT and SIGQUIT in the calling
// process for the whole call, and gives back no pid. So `kex` could not
// observe Ctrl+C at all, had no child to forward it to, and left whatever
// `sh` started reparented to init: a `tey run` server kept serving and kept
// its port after its launcher was gone, and `eaddrinuse` met the next run.
//
// The child stays in this process group deliberately, so a terminal's
// Ctrl+C keeps reaching `erl` directly the way it already did. What is new
// is the second path: a signal delivered to the LAUNCHER alone now reaches
// the child too, and a launcher going down takes the child with it.
//
// Returns a wait status, as `std::system` does — pass it to `exitStatusOf`.
static auto runShellCommand(const std::string &command) -> int {
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

static auto exitStatusOf(int waitStatus) -> int {
  if (WIFEXITED(waitStatus))
    return WEXITSTATUS(waitStatus);
  if (WIFSIGNALED(waitStatus))
    return 128 + WTERMSIG(waitStatus);
  return waitStatus == 0 ? 0 : 1;
}

// `kex_test:configure(Mode, Filters), ` for the `-eval` the BEAM runner
// builds, or "" for an ordinary run (kexhq/kex#199). Like the Mock.* grant
// next to it, the reporting mode is the RUNNER's to set: an emitted .beam
// handed straight to `erl` prints the ✓/✗ prose a person reads.
static auto beamTestGate(
    kex::interpreter::Evaluator::TestReportMode mode,
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

static auto shellSingleQuote(const std::string &s) -> std::string {
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
// Persistent-VM driver for the BEAM REPL. Keeps one `erl -noshell` process
// alive for the whole REPL session, talking to runtime/src/kex_repl_driver.erl
// over stdin/stdout with a nonce-delimited protocol (see that module's header
// for the full rationale). This is what makes the BEAM REPL a real persistent
// VM — spawned processes, registered names, and ETS tables survive across
// inputs, and the recompiled session module is hot-loaded via
// code:load_binary rather than re-running a cold `erl` per line.
struct BeamVm {
  pid_t pid = -1;
  int inFd = -1;  // write end → child stdin
  int outFd = -1; // read end ← child stdout

  auto start(const std::vector<std::string> &argv) -> bool {
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

  void writeLine(const std::string &s) {
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
  auto readUntilSentinel(const std::string &sentinelPrefix, std::string &status)
      -> std::string {
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

  void close() {
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
};
#else
// Emscripten has no fork/exec/pipe, and the BEAM REPL is meaningless under
// wasm anyway (no erl in a browser/Node context). Stub so main.cxx compiles
// unchanged — start() fails and the beam-repl path reports unavailable.
struct BeamVm {
  int pid = -1;
  auto start(const std::vector<std::string> &) -> bool { return false; }
  void writeLine(const std::string &) {}
  auto readUntilSentinel(const std::string &, std::string &s) -> std::string {
    s = "eof";
    return {};
  }
  void close() {}
};
#endif

#ifdef HAS_READLINE
#include <readline/history.h>
#include <readline/readline.h>

// Completion state — populated before the REPL loop, read by the C callback.

static kex::semantic::SemanticDB *g_replDb = nullptr;
static std::vector<std::string> g_completionMatches;
static std::string g_completionWord;
static std::string g_completionStripPrefix;
static std::string g_completionRewriteTo;
static bool g_completionPreloaded = false;

extern "C" {
// Display hook: strip the shared "Qualifier." prefix from every entry so the
// list shows just member names like `map` instead of `[123,1,123,123].map`.
static void kexDisplayMatches(char **matches, int num_matches,
                              int /*max_length*/) {
  // matches[0] is readline's longest-common-prefix; find the last '.' in it
  // to determine how many characters to strip from every completion.
  size_t stripLen = 0;
  if (matches[0]) {
    std::string common(matches[0]);
    auto dot = common.rfind('.');
    if (dot != std::string::npos)
      stripLen = dot + 1;
  }

  if (stripLen == 0) {
    // No dot — let readline display normally via its own list formatter.
    rl_display_match_list(matches, num_matches, 0);
    rl_on_new_line();
    return;
  }

  // Build a temporary array of stripped names and call rl_display_match_list.
  std::vector<char *> stripped(num_matches + 2);
  stripped[0] = strdup(matches[0] + stripLen); // stripped common prefix
  int newMax = 0;
  for (int i = 1; i <= num_matches; i++) {
    const char *src = matches[i];
    size_t srcLen = std::strlen(src);
    const char *member = (srcLen > stripLen) ? src + stripLen : src;
    stripped[i] = strdup(member);
    int len = static_cast<int>(std::strlen(stripped[i]));
    if (len > newMax)
      newMax = len;
  }
  stripped[num_matches + 1] = nullptr;

  rl_display_match_list(stripped.data(), num_matches, newMax);

  for (int i = 0; i <= num_matches; i++)
    free(stripped[i]);
  rl_on_new_line();
}

static char *kexCompletionEntry(const char * /*text*/, int state) {
  if (state == 0) {
    if (!g_completionPreloaded) {
      g_completionMatches.clear();
      if (g_replDb) {
        auto raw = g_replDb->completionsFor(g_completionWord);
        g_completionMatches = kex::rewriteCompletions(
            std::move(raw), g_completionStripPrefix, g_completionRewriteTo);
      }
    }
    g_completionPreloaded = false;
  }
  if (state < static_cast<int>(g_completionMatches.size()))
    return strdup(g_completionMatches[state].c_str());
  return nullptr;
}
static char **kexCompletion(const char *text, int start, int end) {
  rl_attempted_completion_over = 1;
  rl_completion_suppress_append = 1; // no trailing space/quote after completion

  if (start == 0 && text[0] == '/') {
    g_completionMatches = kex::replCommandCompletions(text);
    g_completionPreloaded = true;
    return rl_completion_matches(text, kexCompletionEntry);
  }

  auto cq = kex::resolveCompletionQuery(rl_line_buffer, start, text);

  // If the DB query still has an unresolved lowercase qualifier (e.g. "x.")
  // and we're inside a `make X` block, try to resolve it via parameter
  // pattern inference (handles `@[x|xs]` head/tail and simple named params).
  if (!g_currentMakeTarget.empty()) {
    auto dotPos = cq.dbQuery.rfind('.');
    if (dotPos != std::string::npos) {
      std::string qualifier = cq.dbQuery.substr(0, dotPos);
      bool looksUnresolved =
          !qualifier.empty() && std::islower((unsigned char)qualifier[0]) &&
          std::all_of(qualifier.begin(), qualifier.end(), [](char c) {
            return std::isalnum((unsigned char)c) || c == '_';
          });
      if (looksUnresolved) {
        std::string inferred = kex::inferPatternParamType(
            rl_line_buffer, qualifier, g_currentMakeTarget);
        if (!inferred.empty()) {
          std::string memberPart = cq.dbQuery.substr(dotPos + 1);
          cq.dbQuery = inferred + "." + memberPart;
          cq.rewriteFrom = inferred + ".";
          // cq.rewriteTo keeps the original "x." so readline inserts correctly
        }
      }
    }
  }

  g_completionWord = cq.dbQuery;
  g_completionStripPrefix = cq.rewriteFrom;
  g_completionRewriteTo = cq.rewriteTo;

  static bool debugCompl = (std::getenv("KEX_DEBUG_COMPLETE") != nullptr);
  if (debugCompl) {
    fprintf(stderr,
            "\n[complete] linebuf=%s start=%d end=%d text=%s"
            " -> query=%s rewriteFrom=%s rewriteTo=%s\n",
            rl_line_buffer, start, end, text, cq.dbQuery.c_str(),
            cq.rewriteFrom.c_str(), cq.rewriteTo.c_str());
    auto preview = g_replDb ? g_replDb->completionsFor(cq.dbQuery)
                            : std::vector<std::string>{};
    auto rewritten =
        kex::rewriteCompletions(preview, cq.rewriteFrom, cq.rewriteTo);
    for (const auto &c : rewritten)
      fprintf(stderr, "  [match] %s\n", c.c_str());
  }

  return rl_completion_matches(text, kexCompletionEntry);
}
} // extern "C"
#endif

// Set when the user abandons the line being typed (Esc Esc, or Ctrl-G). The
// REPL loops check it after every read so a half-typed block can be thrown
// away without evaluating it — previously the only ways out of a `...>`
// continuation were completing the block or killing the process.
bool g_replInputCancelled = false;

auto takeReplInputCancelled() -> bool {
  const bool cancelled = g_replInputCancelled;
  g_replInputCancelled = false;
  return cancelled;
}

#ifdef HAS_READLINE
// Esc alone cannot be bound: readline uses it as the meta prefix, so binding
// it would break arrow keys and every other escape sequence. Esc Esc is the
// usual answer, and Ctrl-G is readline's traditional abort.
auto replCancelInput(int, int) -> int {
  g_replInputCancelled = true;
  rl_replace_line("", 0);
  rl_done = 1;
  return 0;
}

auto bindReplCancelKeys() -> void {
  rl_bind_keyseq("\\e\\e", replCancelInput);
  rl_bind_key(7, replCancelInput);  // Ctrl-G
}
#else
auto bindReplCancelKeys() -> void {}
#endif

auto readLine(const std::string &prompt) -> std::pair<std::string, bool> {
  g_replInputCancelled = false;
#ifdef HAS_READLINE
  char *input = readline(prompt.c_str());
  if (!input)
    return {"", false};
  std::string line(input);
  if (!line.empty() && !g_replInputCancelled)
    add_history(input);
  free(input);
  return {line, true};
#else
  std::cout << prompt;
  std::string line;
  if (!std::getline(std::cin, line))
    return {"", false};
  return {line, true};
#endif
}

auto readFile(const std::string &path) -> std::string {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "Error: could not open file: " << path << "\n";
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

namespace {

// Additional project roots supplied by repeatable `--source-root` options.
// Keeping this at the CLI boundary lets every existing consumer of
// moduleRootsFor (semantic checks, discovery, validation and both runtimes)
// observe exactly the same search path.
std::vector<std::string> cliSourceRoots;

auto isIdentChar(char c) -> bool {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// ===== TypeExpr pretty-printer =====

auto typeExprToString(const kex::ast::TypeExpr &te) -> std::string;

auto typeNameToString(const kex::ast::TypeName &tn) -> std::string {
  std::string r;
  for (size_t i = 0; i < tn.parts.size(); i++) {
    if (i)
      r += ".";
    r += tn.parts[i];
  }
  return r;
}

auto typeExprToString(const kex::ast::TypeExpr &te) -> std::string {
  return std::visit(
      [](const auto &node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, kex::ast::TypeName>) {
          return typeNameToString(node);
        } else if constexpr (std::is_same_v<T, kex::ast::GenericType>) {
          std::string r = typeNameToString(node.name) + "<";
          for (size_t i = 0; i < node.args.size(); i++) {
            if (i)
              r += ", ";
            if (node.args[i])
              r += typeExprToString(*node.args[i]);
          }
          return r + ">";
        } else if constexpr (std::is_same_v<T, kex::ast::FunctionType>) {
          std::string l = node.param ? typeExprToString(*node.param) : "?";
          std::string r = node.result ? typeExprToString(*node.result) : "?";
          return l + " -> " + r;
        } else if constexpr (std::is_same_v<T, kex::ast::TupleType>) {
          std::string r = "(";
          for (size_t i = 0; i < node.elements.size(); i++) {
            if (i)
              r += ", ";
            if (node.elements[i])
              r += typeExprToString(*node.elements[i]);
          }
          return r + ")";
        } else if constexpr (std::is_same_v<T, kex::ast::ListType>) {
          return "[" + (node.element ? typeExprToString(*node.element) : "?") +
                 "]";
        } else if constexpr (std::is_same_v<T, kex::ast::MapType>) {
          std::string k = node.key ? typeExprToString(*node.key) : "?";
          std::string v = node.value ? typeExprToString(*node.value) : "?";
          return "Map<" + k + ", " + v + ">";
        } else if constexpr (std::is_same_v<T, kex::ast::UnionType>) {
          std::string l = node.left ? typeExprToString(*node.left) : "?";
          std::string r = node.right ? typeExprToString(*node.right) : "?";
          return l + " | " + r;
        } else if constexpr (std::is_same_v<T, kex::ast::IntersectionType>) {
          std::string l = node.left ? typeExprToString(*node.left) : "?";
          std::string r = node.right ? typeExprToString(*node.right) : "?";
          return l + " & " + r;
        } else if constexpr (std::is_same_v<T, kex::ast::RecordType>) {
          std::string result = "{";
          for (size_t i = 0; i < node.fields.size(); i++) {
            if (i) result += ", ";
            result += node.fields[i].first + ": " +
                (node.fields[i].second
                     ? typeExprToString(*node.fields[i].second) : "?");
          }
          return result + "}";
        } else if constexpr (std::is_same_v<T, kex::ast::OptionalType>) {
          return (node.inner ? typeExprToString(*node.inner) : "?") + "?";
        } else if constexpr (std::is_same_v<T, kex::ast::BlockType>) {
          return "Block<" + (node.inner ? typeExprToString(*node.inner) : "?") +
                 ">";
        } else if constexpr (std::is_same_v<T, kex::ast::AtomType>) {
          return ":" + node.name;
        } else if constexpr (std::is_same_v<T, kex::ast::GenericVar>) {
          return node.name;
        }
        return "?";
      },
      te.kind);
}

// ===== JSON helpers (no external dep — hand-rolled) =====

auto jsonEscape(const std::string &s) -> std::string {
  std::string out;
  out.reserve(s.size() + 4);
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
    }
  }
  return out;
}

// Extract the "did you mean `X`?" portion from a diagnostic message, if any.
auto extractHint(const std::string &msg) -> std::string {
  const std::string needle = "did you mean `";
  auto pos = msg.find(needle);
  if (pos == std::string::npos)
    return "";
  pos += needle.size();
  auto end = msg.find('`', pos);
  if (end == std::string::npos)
    return "";
  return msg.substr(pos, end - pos);
}

// Syntax-highlights a diagnostic message body using a palette kept distinct
// from Value::inspect's literal coloring: type names / type vars become cyan
// (matching the REPL/IO.inspect type suffix), function names become bold
// (backtick spans and signature heads), arrows (->) become magenta, and
// backtick delimiters become gray. Cyan-vs-bold keeps functions and types
// distinguishable. Honors kex::color::enabled.
auto colorizeMessage(const std::string &msg) -> std::string {
  using namespace kex::color;
  std::string out;
  out.reserve(msg.size() * 2);
  const auto n = msg.size();
  bool atLineStart = true;
  for (size_t i = 0; i < n;) {
    char c = msg[i];

    if (c == '`') {
      size_t end = msg.find('`', i + 1);
      if (end == std::string::npos) {
        out += c;
        i++;
        continue;
      }
      out += apply(gray);
      out += '`';
      out += apply(reset);
      out += apply(bold);
      out.append(msg, i + 1, end - i - 1);
      out += apply(reset);
      out += apply(gray);
      out += '`';
      out += apply(reset);
      i = end + 1;
      atLineStart = false;
      continue;
    }

    if (atLineStart) {
      bool startsLower = std::isalpha(static_cast<unsigned char>(c)) &&
                         std::islower(static_cast<unsigned char>(c));
      bool identStart = std::isalpha(static_cast<unsigned char>(c)) || c == '_';
      if (startsLower || (c == '_' && identStart)) {
        size_t j = i + 1;
        while (j < n && isIdentChar(msg[j]))
          j++;
        if (j < n && (msg[j] == '?' || msg[j] == '!'))
          j++;
        size_t k = j;
        while (k < n && msg[k] == ' ')
          k++;
        if (k < n && msg[k] == ':') {
          out += apply(bold);
          out.append(msg, i, j - i);
          out += apply(reset);
          i = j;
          atLineStart = false;
          continue;
        }
      }
    }

    if (c == '-' && i + 1 < n && msg[i + 1] == '>') {
      out += apply(magenta);
      out += "->";
      out += apply(reset);
      i += 2;
      atLineStart = false;
      continue;
    }

    if (std::isupper(static_cast<unsigned char>(c)) &&
        (i == 0 || !isIdentChar(msg[i - 1]))) {
      size_t j = i + 1;
      while (j < n && isIdentChar(msg[j]))
        j++;
      out += apply(cyan);
      out.append(msg, i, j - i);
      out += apply(reset);
      i = j;
      atLineStart = false;
      continue;
    }

    if (c == '\n') {
      out += c;
      i++;
      atLineStart = true;
      continue;
    }
    out += c;
    i++;
    atLineStart = false;
  }
  return out;
}

// `--collapse-report`: what `compiled do` chain collapse folded away, and why
// the rest did not.
//
// Goes to stderr and says nothing at all for a program with no `compiled`
// block, so it composes with every other mode — you can ask about a run, a
// check or an emit without changing what any of them print.
auto printCollapseReport(const std::vector<kex::compiled::CollapseNote> &notes)
    -> void {
  if (notes.empty()) return;

  // Source order, not the order collapse worked in: a chain that failed and
  // was retried from the inside reports its outer attempt in a later round,
  // and reading the file top to bottom is what a user wants.
  auto ordered = notes;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const auto &a, const auto &b) {
                     if (a.location.file != b.location.file)
                       return a.location.file < b.location.file;
                     if (a.location.line != b.location.line)
                       return a.location.line < b.location.line;
                     return a.location.column < b.location.column;
                   });

  // Retrying a failed chain from the inside re-reports the parts it tries
  // again, and everything written inside one `"${...}"` shares that string's
  // position — so the same verdict can arrive several times over. Identical
  // lines say nothing extra. A location with BOTH verdicts keeps both: those
  // are genuinely different sub-expressions that happen to start together.
  ordered.erase(std::unique(ordered.begin(), ordered.end(),
                            [](const auto &a, const auto &b) {
                              return a.location.file == b.location.file &&
                                     a.location.line == b.location.line &&
                                     a.location.column == b.location.column &&
                                     a.collapsed == b.collapsed &&
                                     a.reason == b.reason;
                            }),
                ordered.end());

  std::size_t collapsed = 0;
  for (const auto &note : ordered)
    if (note.collapsed) collapsed++;

  std::cerr << kex::color::apply(kex::color::bold) << "collapse:"
            << kex::color::apply(kex::color::reset) << " " << collapsed << " of "
            << ordered.size() << " evaluated at compile time\n";
  for (const auto &note : ordered) {
    std::cerr << "  " << kex::color::apply(kex::color::gray)
              << note.location.file << ":" << note.location.line << ":"
              << note.location.column << kex::color::apply(kex::color::reset)
              << " ";
    if (note.collapsed) {
      std::cerr << kex::color::apply(kex::color::green) << "collapsed"
                << kex::color::apply(kex::color::reset) << "\n";
    } else {
      std::cerr << kex::color::apply(kex::color::magenta) << "kept"
                << kex::color::apply(kex::color::reset) << " — " << note.reason
                << "\n";
    }
  }
}

auto printSemanticDiagnostic(const kex::semantic::Diagnostic &diag) -> void {
  bool isError = diag.level == kex::semantic::Diagnostic::Level::Error;
  std::cerr << kex::color::apply(kex::color::gray) << diag.location.file
            << ":" << diag.location.line << ":" << diag.location.column;
  if (diag.endLocation) {
    std::cerr << "-" << diag.endLocation->line << ":"
              << diag.endLocation->column;
  }
  std::cerr << ":" << kex::color::apply(kex::color::reset) << " "
            << kex::color::apply(kex::color::bold)
            << (isError ? kex::color::apply(kex::color::red)
                        : kex::color::apply(kex::color::magenta))
            << (isError ? "error" : "warning") << ":"
            << kex::color::apply(kex::color::reset) << " "
            << colorizeMessage(diag.message) << "\n";

  if (diag.endLocation) {
    auto source = readFile(std::string(diag.location.file));
    if (!source.empty()) {
      std::istringstream lines{source};
      std::string line;
      for (int current = 1;
           current <= diag.location.line && std::getline(lines, line);
           ++current) {
        if (current != diag.location.line)
          continue;
        const auto start = std::max(1, diag.location.column);
        int finish = static_cast<int>(line.size()) + 1;
        if (diag.endLocation->line == diag.location.line)
          finish = std::max(start + 1, diag.endLocation->column);
        const auto width = std::max(1, finish - start);
        std::cerr << "  " << current << " | " << line << "\n"
                  << "    | "
                  << std::string(static_cast<size_t>(start - 1), ' ')
                  << kex::color::apply(
                         isError ? kex::color::red : kex::color::magenta)
                  << "^" << std::string(static_cast<size_t>(width - 1), '~')
                  << kex::color::apply(kex::color::reset) << "\n";
        if (diag.endLocation->line != diag.location.line)
          std::cerr << "    | ... through line " << diag.endLocation->line
                    << ", column " << diag.endLocation->column << "\n";
      }
    }
  }

  for (const auto &note : diag.notes) {
    std::cerr << kex::color::apply(kex::color::gray)
              << note.location.file << ":" << note.location.line << ":"
              << note.location.column << ":"
              << kex::color::apply(kex::color::reset) << " "
              << kex::color::apply(kex::color::bold)
              << kex::color::apply(kex::color::cyan) << "note:"
              << kex::color::apply(kex::color::reset) << " "
              << note.message << "\n";
  }
}

} // namespace

// Is this entry file a spec — the one kind of program allowed to install
// mocks without asking (issue #144)? Deliberately the FILE NAME and not, say,
// "the program called describe somewhere": the answer has to be known before
// a single line runs, and it has to be something a reader of the command line
// can check. Everything else needs --allow-mocks.
auto isSpecEntry(const std::string &filepath) -> bool {
  static const std::string suffix = ".spec.kex";
  return filepath.size() > suffix.size() &&
         filepath.compare(filepath.size() - suffix.size(), suffix.size(),
                          suffix) == 0;
}

// Convention: `<name>.spec.kex` is a spec for `<name>.kex` and doesn't need
// to redeclare its types/records/functions — running the spec auto-loads
// the base file's declarations (skipping its own `main` block(s), so its
// demo output/side effects don't run) into the same scope first. Looked up
// next to the spec file, and — since this project keeps specs in spec/
// alongside examples in a sibling examples/ — also under examples/ if the
// spec's directory is named "spec".
auto specBaseCandidates(const std::string &filepath)
    -> std::vector<std::string> {
  static const std::string suffix = ".spec.kex";
  // A `package.kex` manifest speaks a vocabulary the stdlib describes but no
  // program imports (`bundle`, `version`, `tey`, ...). It comes in the same
  // way a spec's base file does, so checking a manifest reports real
  // mistakes instead of "undefined function" on every declaration.
  if (std::filesystem::path(filepath).filename() == "package.kex") {
    if (auto vocabulary = kex::manifestVocabularyFile(); !vocabulary.empty())
      return {vocabulary};
    return {};
  }
  if (filepath.size() <= suffix.size())
    return {};
  if (filepath.compare(filepath.size() - suffix.size(), suffix.size(),
                       suffix) != 0)
    return {};

  std::string stem = filepath.substr(0, filepath.size() - suffix.size());
  auto slash = stem.find_last_of('/');
  std::string dir = slash == std::string::npos ? "" : stem.substr(0, slash);
  std::string name = slash == std::string::npos ? stem : stem.substr(slash + 1);

  std::vector<std::string> candidates = {stem + ".kex"};
  const std::string specSuffix = "spec";
  if (dir.size() >= specSuffix.size() &&
      dir.compare(dir.size() - specSuffix.size(), specSuffix.size(),
                  specSuffix) == 0) {
    std::string parent = dir.substr(0, dir.size() - specSuffix.size());
    candidates.push_back(parent + "examples/" + name + ".kex");
  }
  return candidates;
}

auto fileExists(const std::string &path) -> bool {
  std::ifstream probe(path);
  return probe.good();
}

namespace {

auto collectUsingModules(const kex::ast::Program &program) -> std::vector<std::string> {
  std::vector<std::string> result;
  auto addFrom = [&](const kex::ast::TypeName &tn) {
    std::string name;
    for (size_t i = 0; i < tn.parts.size(); i++) {
      if (i) name += ".";
      name += tn.parts[i];
    }
    if (!kex::module::Resolver::isForeignNamespace(name))
      result.push_back(std::move(name));
  };
  auto scanModuleBody = [&](auto &self,
                            const std::vector<kex::ast::ModuleItem> &body) -> void {
    for (const auto &item : body) {
      if (auto *ub = std::get_if<std::unique_ptr<kex::ast::UsingBlock>>(&item))
        if (*ub) addFrom((*ub)->module);
      if (auto *md = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
        if (*md) self(self, (*md)->body);
    }
  };
  for (const auto &item : program.items) {
    if (auto *ub = std::get_if<std::unique_ptr<kex::ast::UsingBlock>>(&item))
      if (*ub) addFrom((*ub)->module);
    if (auto *md = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
      if (*md) scanModuleBody(scanModuleBody, (*md)->body);
  }
  return result;
}

struct LoadedDep {
  std::unique_ptr<std::string> source;
  std::unique_ptr<std::string> path;
  std::unique_ptr<kex::ast::Program> program;
};

auto resolveBeamDeps(kex::ast::Program &program,
                     const std::vector<std::string> &roots,
                     const kex::semantic::ImportedInterfaces *interfaces,
                     const std::unordered_set<std::string> &qualifiedModules = {})
    -> std::vector<LoadedDep> {
  std::vector<LoadedDep> deps;
  std::unordered_set<std::string> loaded;
  kex::module::Resolver resolver(roots);

  std::function<void(const kex::ast::Program &,
                     const std::unordered_set<std::string> &)> resolve =
      [&](const kex::ast::Program &prog,
          const std::unordered_set<std::string> &seedModules) {
    auto modules = collectUsingModules(prog);
    modules.insert(modules.end(), seedModules.begin(), seedModules.end());

    // Each newly parsed source file can introduce qualified-only dependency
    // edges of its own. Analyze it here rather than only analyzing the entry
    // program; otherwise discovery stops after one qualified-reference hop.
    kex::semantic::Analyzer dependencyAnalysis(interfaces);
    (void)dependencyAnalysis.analyze(prog);
    // Sorted: `referencedModules` is a hash set, and this list decides what
    // gets compiled and in what order. Iterating it directly made the build
    // depend on the host's hash order (kexhq/kex#143).
    auto referenced = dependencyAnalysis.referencedModules();
    std::vector<std::string> referencedOrdered(referenced.begin(),
                                               referenced.end());
    std::sort(referencedOrdered.begin(), referencedOrdered.end());
    for (const auto &name : referencedOrdered) {
      const bool automatic = interfaces && [&] {
        const auto imported = interfaces->modules.find(name);
        return imported != interfaces->modules.end() &&
               imported->second.automaticImport;
      }();
      if (!automatic)
        modules.push_back(name);
    }
    for (const auto &modName : modules) {
      auto resolved = resolver.resolve(modName);
      if (!resolved) continue;
      if (!loaded.insert(resolved->path).second) continue;

      auto path = std::make_unique<std::string>(resolved->path);
      auto src = std::make_unique<std::string>(readFile(*path));
      kex::Lexer lexer(std::string(*src), *path);
      kex::Parser parser(lexer.tokenizeAll(), *path);
      auto depProg = std::make_unique<kex::ast::Program>(parser.parseProgram());
      // A dependency that does not parse is a build failure, not a module to
      // skip. Silently dropping it produced no error anywhere: the module
      // simply did not exist, the program compiled, and the first call into
      // it died at RUNTIME with `Undefined function: Tey.Resolver`.
      if (!parser.diagnostics().empty()) {
        for (const auto &diagnostic : parser.diagnostics())
          std::cerr << kex::color::apply(kex::color::gray)
                    << diagnostic.location.file << ":"
                    << diagnostic.location.line << ":"
                    << diagnostic.location.column << ":"
                    << kex::color::apply(kex::color::reset) << " "
                    << kex::color::apply(kex::color::bold)
                    << kex::color::apply(kex::color::red)
                    << "error:" << kex::color::apply(kex::color::reset) << " "
                    << diagnostic.message << "\n";
        std::cerr << "error: could not parse module " << modName << "\n";
        std::exit(1);
      }
      resolve(*depProg, {});
      deps.push_back({std::move(src), std::move(path), std::move(depProg)});
    }
  };
  resolve(program, qualifiedModules);

  if (!deps.empty()) {
    std::vector<kex::ast::TopLevelItem> merged;
    for (auto &dep : deps)
      for (auto &item : dep.program->items)
        if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(item))
          merged.push_back(std::move(item));
    for (auto &item : program.items)
      merged.push_back(std::move(item));
    program.items = std::move(merged);
  }
  return deps;
}

} // namespace

auto loadPrelude(kex::semantic::SemanticDB &db) -> void {
  kex::loadDiscoveredPrelude(db);
}

auto moduleRootsFor(const std::string &filepath) -> std::vector<std::string> {
  namespace fs = std::filesystem;
  const auto srcDir = fs::weakly_canonical(filepath).parent_path();
  std::vector<std::string> roots;
  for (const auto &configured : cliSourceRoots) {
    std::error_code ec;
    auto root = fs::weakly_canonical(configured, ec);
    const auto normalized = ec ? fs::path(configured).lexically_normal() : root;
    const auto value = normalized.string();
    if (std::find(roots.begin(), roots.end(), value) == roots.end())
      roots.push_back(value);
  }
  for (const auto &relative : {"lib", "src"}) {
    const auto candidate = srcDir / relative;
    std::error_code ec;
    if (fs::is_directory(candidate, ec) && !ec)
      roots.push_back(candidate.string());
  }
  if (roots.empty())
    roots.push_back(srcDir.string());
  for (auto &root : kex::standardLibraryModuleRoots())
    if (std::find(roots.begin(), roots.end(), root) == roots.end())
      roots.push_back(std::move(root));
  return roots;
}

auto prebuiltRuntimeBeamDir() -> std::string;

struct PreludeInterfaceNames {
  std::unordered_set<std::string> receiverFunctions;
};

// Source discovery remains the interface boundary for builds that cannot use
// BEAM artifacts (currently the browser/wasm build). Native builds use KexI.
auto sourcePreludeInterfaceNames() -> PreludeInterfaceNames {
  PreludeInterfaceNames names;
  for (const auto &filePath : kex::preludeSourceFiles()) {
    kex::Lexer lexer(readFile(filePath), filePath);
    kex::Parser parser(lexer.tokenizeAll(), filePath);
    auto program = parser.parseProgram();
    auto addReceiver = [&](const kex::ast::FunctionDef &fn) {
      names.receiverFunctions.insert(fn.name);
    };
    auto collectMake = [&](const kex::ast::MakeDef &make) {
      for (const auto &item : make.body) {
        if (const auto *fn =
                std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item)) {
          if (*fn) addReceiver(**fn);
        } else if (const auto *visibility =
                       std::get_if<std::unique_ptr<kex::ast::VisibilityBlock>>(
                           &item)) {
          if (!*visibility) continue;
          for (const auto &visible : (*visibility)->items)
            if (const auto *fn =
                    std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(
                        &visible); fn && *fn)
              addReceiver(**fn);
        }
      }
    };
    auto collectTrait = [&](const kex::ast::TraitDef &trait) {
      for (const auto &item : trait.body)
        if (const auto *fn =
                std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item);
            fn && *fn)
          addReceiver(**fn);
    };
    std::function<void(const kex::ast::ModuleDef &)> collect;
    collect = [&](const kex::ast::ModuleDef &module) {
      for (const auto &item : module.body) {
        if (const auto *make =
                std::get_if<std::unique_ptr<kex::ast::MakeDef>>(&item)) {
          if (*make) collectMake(**make);
        } else if (const auto *trait =
                       std::get_if<std::unique_ptr<kex::ast::TraitDef>>(&item)) {
          if (*trait) collectTrait(**trait);
        } else if (const auto *child =
                       std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item)) {
          collect(**child);
        }
      }
    };
    for (const auto &item : program.items) {
      if (const auto *make =
              std::get_if<std::unique_ptr<kex::ast::MakeDef>>(&item)) {
        if (*make) collectMake(**make);
      } else if (const auto *trait =
                     std::get_if<std::unique_ptr<kex::ast::TraitDef>>(&item)) {
        if (*trait) collectTrait(**trait);
      } else if (const auto *module =
                     std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item)) {
        collect(**module);
      } else if (const auto *fn =
                     std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(
                         &item)) {
        if (*fn && !(*fn)->clauses.empty() &&
            (*fn)->clauses[0].params.size() >= 2)
          names.receiverFunctions.insert((*fn)->name);
      }
    }
  }
  return names;
}

auto preludeInterfaceNames() -> const PreludeInterfaceNames & {
  static const PreludeInterfaceNames names = [] {
    auto runtimeDir = prebuiltRuntimeBeamDir();
    if (runtimeDir.empty()) return sourcePreludeInterfaceNames();
    auto path = std::filesystem::path{runtimeDir} / "kex_prelude.beam";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return sourcePreludeInterfaceNames();
    const auto &registry = kex::preludeRegistry(runtimeDir);
    PreludeInterfaceNames result;
    for (const auto *module : registry.allLoadedModules()) {
      for (const auto &receiver : module->chunk.typeInterface.methods)
        result.receiverFunctions.insert(receiver.name);
    }
    return result;
  }();
  return names;
}

auto preludeExternalModules() -> const kex::ir::ExternalModules & {
  static const auto modules = [] {
    auto runtimeDir = prebuiltRuntimeBeamDir();
    if (runtimeDir.empty()) return kex::ir::ExternalModules{};
    return kex::preludeRegistry(runtimeDir).buildExternalModules();
  }();
  return modules;
}

auto preludeSemanticInterfaces()
    -> const kex::semantic::ImportedInterfaces & {
  return kex::preludeSemanticInterfaces(prebuiltRuntimeBeamDir());
}

auto mergeExternalModules(const kex::ir::ExternalModules &base,
                          const kex::ir::ExternalModules &overrides)
    -> kex::ir::ExternalModules {
  auto result = base;
  for (const auto &[name, atom] : overrides.nameToAtom)
    result.nameToAtom[name] = atom;
  for (const auto &[name, function] : overrides.exportToBeamFn)
    result.exportToBeamFn[name] = function;
  for (const auto &[name, arity] : overrides.exportArity)
    result.exportArity[name] = arity;
  for (const auto &[name, names] : overrides.exportParamNames)
    result.exportParamNames[name] = names;
  for (const auto &name : overrides.foulExports)
    result.foulExports.insert(name);
  for (const auto &name : overrides.capabilityModules)
    result.capabilityModules.insert(name);
  for (const auto &[name, functions] : overrides.receiverFunctions)
    result.receiverFunctions[name] = functions;
  for (const auto &[name, methods] : overrides.traitMethods)
    result.traitMethods[name] = methods;
  return result;
}

struct PreludeBuildModule {
  kex::ir::EmitResult emitted;
  kex::beam::KexiChunk interface;
};

// Build the stdlib entry plus ordinary Kex module companions. The entry keeps
// receiver compatibility code until typed receiver ownership is carried into
// IR; public module calls already use companion interfaces.
auto compilePreludeCore(const std::string &dir,
                        std::vector<PreludeBuildModule> *builtModules) -> bool {
  kex::ast::Program merged;
  auto files = kex::preludeSourceFiles();
  if (files.empty()) {
    std::cerr << "error: no prelude source root is available\n";
    return false;
  }
  try {
    files = kex::orderPreludeSourcesByTier(files);
  } catch (const std::exception &e) {
    std::cerr << "error: invalid prelude tier manifest: " << e.what() << "\n";
    return false;
  }
  auto tierGroups = kex::groupPreludeSourcesByTier(files);
  const auto stdlibRoot =
      std::filesystem::path(files.front()).parent_path();
  auto optInFiles = kex::standardLibrarySourceFiles();
  optInFiles.erase(
      std::remove_if(
          optInFiles.begin(), optInFiles.end(),
          [&](const auto &file) {
            return std::filesystem::path(file).parent_path() !=
                   stdlibRoot;
          }),
      optInFiles.end());
  files.insert(files.end(), optInFiles.begin(), optInFiles.end());
  // Track item index boundaries per tier: tierBounds[t] = index of first
  // item belonging to tier t.  tierBounds[4] = total item count.
  std::array<size_t, 5> tierBounds{};
  for (size_t t = 0; t < 4; t++) {
    tierBounds[t] = merged.items.size();
    for (const auto &f : tierGroups[t]) {
      kex::Lexer lex(readFile(f), f);
      kex::Parser parser(lex.tokenizeAll(), f);
      auto prog = parser.parseProgram();
      for (auto &item : prog.items)
        if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(item))
          merged.items.push_back(std::move(item));
    }
  }
  for (const auto &f : optInFiles) {
    kex::Lexer lex(readFile(f), f);
    kex::Parser parser(lex.tokenizeAll(), f);
    auto prog = parser.parseProgram();
    for (auto &item : prog.items)
      if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(item))
        merged.items.push_back(std::move(item));
  }
  tierBounds[4] = merged.items.size();
  try {
    kex::semantic::Analyzer analyzer;
    if (!analyzer.analyze(merged)) {
      for (const auto &diag : analyzer.diagnostics())
        if (diag.level == kex::semantic::Diagnostic::Level::Error)
          std::cerr << "error: prelude interface: " << diag.location.file
                    << ":" << diag.location.line << ":"
                    << diag.location.column << ": " << diag.message << "\n";
      return false;
    }

    kex::ir::ExternalModules selfExt;
    auto selfNames = sourcePreludeInterfaceNames();
    for (const auto& name : selfNames.receiverFunctions)
      for (int arity = 1; arity <= 4; arity++)
        selfExt.receiverFunctions[name].push_back(
            {"kex_prelude", name, arity});

    std::vector<kex::ir::Module> modules;
    modules.push_back(kex::ir::lowerProgramTiered(
        merged, tierBounds, "prelude", "", &selfExt, nullptr, nullptr, true));
    auto splitModules = kex::ir::lowerModules(
        merged, "prelude", "", nullptr, &selfExt, nullptr, true);
    for (size_t i = 1; i < splitModules.size(); i++)
      modules.push_back(std::move(splitModules[i]));

    builtModules->clear();
    for (size_t i = 0; i < modules.size(); i++) {
      PreludeBuildModule built;
      built.emitted = kex::ir::emitCore(modules[i]);
      std::ofstream out(dir + "/" + built.emitted.moduleName + ".core");
      if (!out) return false;
      out << built.emitted.source;

      if (i == 0) {
        kex::beam::CollectOptions options;
        options.unitId = "kex_prelude";
        options.moduleAtom = built.emitted.moduleName;
        options.moduleName = std::string(kex::semantic::kFileLevelPreludeModule);
        options.collectTopLevel = true;
        options.flattenModules = true;
        options.role = kex::beam::KexiModuleRole::Entry;
        options.analysis = &analyzer;
        built.interface = kex::beam::collectMetadata(merged, options);
        built.interface.metadata.package.id = "kex.stdlib";
        built.interface.metadata.package.unitIds = {"kex_prelude"};
        built.interface.metadata.package.receiverProviders = {
            std::string(kex::semantic::kFileLevelPreludeModule)};
        built.interface.metadata.package.automaticImports = {
            std::string(kex::semantic::kFileLevelPreludeModule)};
        built.interface.sourceHash = kex::preludeSourceHash(files);
      } else {
        kex::beam::CollectOptions options;
        options.unitId = "kex_prelude";
        options.moduleAtom = built.emitted.moduleName;
        options.analysis = &analyzer;
        options.role = kex::beam::KexiModuleRole::Companion;
        options.entryBackPointer = "kex_prelude";
        options.moduleName = built.emitted.moduleName.rfind("Kex.", 0) == 0
            ? built.emitted.moduleName.substr(4) : built.emitted.moduleName;
        built.interface = kex::beam::collectMetadata(merged, options);
      }
      builtModules->push_back(std::move(built));
    }
    const auto automaticInterfaces =
        kex::sourcePreludeSemanticInterfaces();
    for (size_t i = 1; i < builtModules->size(); i++) {
      kex::beam::KexiCompanion companion;
      companion.beamAtom = (*builtModules)[i].emitted.moduleName;
      companion.relativePath = companion.beamAtom + ".beam";
      (*builtModules)[0].interface.metadata.companions.push_back(
          std::move(companion));
      const auto& sourceModule =
          (*builtModules)[i].interface.metadata.sourceModule;
      const auto automatic =
          automaticInterfaces.modules.find(sourceModule);
      if (!sourceModule.empty() &&
          automatic != automaticInterfaces.modules.end() &&
          automatic->second.automaticImport)
        (*builtModules)[0]
            .interface.metadata.package.automaticImports.push_back(
                sourceModule);
    }
  } catch (const kex::ir::LowerError &e) {
    std::cerr << "error: prelude: " << e.what() << "\n";
    return false;
  }
  return true;
}

// Load stdlib record layouts from the embedded interface of the explicitly
// built prelude artifact. Compiled metadata stays separate from the user's AST.
auto loadPreludeRecordLayouts() -> std::vector<kex::ir::ExternalRecordLayout> {
  std::vector<kex::ir::ExternalRecordLayout> layouts;
  auto runtimeDir = prebuiltRuntimeBeamDir();
  if (runtimeDir.empty()) return layouts;
  const auto &registry = kex::preludeRegistry(runtimeDir);
  for (const auto *module : registry.allLoadedModules())
    for (const auto &record : module->chunk.metadata.records) {
      kex::ir::ExternalRecordLayout layout;
      // The layout is keyed by the tuple tag the record's own module emits,
      // so a construction here agrees with a pattern match there.
      layout.name = record.tagAtom.empty() ? record.name : record.tagAtom;
      layout.moduleAtom = module->beamAtom;
      for (const auto &field : record.fields)
        layout.fields.push_back(field.name);
      layouts.push_back(std::move(layout));
    }
  return layouts;
}

// The checked type of a REPL expression, when it is worth showing. Both REPLs
// otherwise derive the type from the VALUE, which cannot recover what runtime
// representation erases (a typestate parameter, an empty list's element type,
// the unused half of a Result). A gradual/unannotated expression checks as
// `?`/`A`/`N`, and there the value is the better source — hence the
// concreteness gate rather than "always prefer static".
auto displayTypeOf(const kex::semantic::Analyzer &analyzer,
                   const kex::ast::Expr *expr)
    -> std::optional<std::string> {
  if (!expr) return std::nullopt;
  auto type = analyzer.displayTypeOf(expr);
  if (!type || !kex::semantic::isFullyConcrete(type)) return std::nullopt;
  return kex::semantic::typeToString(type);
}

// Prelude variant tags, so a compiled program can register their display info
// the same way it registers the prelude's record layouts.
auto loadPreludeVariantTags() -> std::vector<kex::ir::ExternalVariantTag> {
  std::vector<kex::ir::ExternalVariantTag> tags;
  auto runtimeDir = prebuiltRuntimeBeamDir();
  if (runtimeDir.empty()) return tags;
  const auto &registry = kex::preludeRegistry(runtimeDir);
  for (const auto *module : registry.allLoadedModules())
    for (const auto &adt : module->chunk.metadata.adts)
      for (const auto &constructor : adt.constructors)
        tags.push_back({constructor.tagAtom,
                        static_cast<int>(constructor.arity), adt.name});
  return tags;
}

// Display info for everything the PRELUDE declares, in the shape
// kex_io:register_display/2 expects. A compiled module registers its own
// records and variants (see lower.cxx's withDisplayInfo), and `/load`
// registers a loaded unit's — but the prelude's were registered by nobody, so
// the BEAM REPL printed every prelude ADT as raw data: `(:InvalidFormat, "x")`
// instead of `InvalidFormat("x")`.
auto preludeDisplayRegistration() -> std::string {
  auto runtimeDir = prebuiltRuntimeBeamDir();
  if (runtimeDir.empty()) return "";
  std::string records;
  std::string variants;
  const auto &registry = kex::preludeRegistry(runtimeDir);
  for (const auto *module : registry.allLoadedModules()) {
    for (const auto &record : module->chunk.metadata.records) {
      std::string fields;
      for (size_t i = 0; i < record.fields.size(); i++) {
        if (i) fields += ", ";
        fields += "'" + record.fields[i].name + "'";
      }
      // Keyed by the value's RUNTIME tag, which the metadata records: a
      // record declared inside a `module` block is tagged with its qualified
      // identity, one at a file's top level with its bare name.
      if (!records.empty()) records += ", ";
      records += "'" +
                 (record.tagAtom.empty() ? record.name : record.tagAtom) +
                 "' => [" + fields + "]";
    }
    for (const auto &adt : module->chunk.metadata.adts)
      for (const auto &constructor : adt.constructors) {
        if (!variants.empty()) variants += ", ";
        variants += "'" + constructor.tagAtom + "' => {" +
                    std::to_string(constructor.arity) + ", '" + adt.name +
                    "'}";
      }
  }
  if (records.empty() && variants.empty()) return "";
  return "kex_io:register_display(#{" + records + "}, #{" + variants + "})";
}

// Runs semantic analysis (undefined-name detection + type checking) and
// prints any diagnostics, same as plain `run` mode's pre-execution check.
// Shared by `run` and `compile`/`-R` (BEAM) so both backends catch the same
// errors before doing anything backend-specific — previously `-R` skipped
// this entirely and fell straight through to erlc, which reports things
// like an undefined function as a raw, un-Kex-like Core Erlang compile
// error ("unbound variable 'UndefinedFunctionCall' in main/0") instead of
// this backend-agnostic diagnostic. Returns false if there were any errors.
// `errorCount`, when given, receives the number of error-level diagnostics
// reported — the abort message quotes it so a long compile shows progress as
// the count comes down, rather than just repeating "fix type errors".
auto runSemanticCheck(const kex::ast::Program &program,
                      const std::string &filepath,
                      kex::semantic::Analyzer *retainedAnalyzer = nullptr,
                      const std::string &extraDeclFile = "",
                      int *errorCount = nullptr) -> bool {
  int errors = 0;
  auto countError = [&](const kex::semantic::Diagnostic &diag) {
    if (diag.level == kex::semantic::Diagnostic::Level::Error) errors++;
  };
  // Pass 1+2: SemanticDB undefined-name detection
  kex::semantic::SemanticDB runDb;
  runDb.setImportedInterfaces(&preludeSemanticInterfaces());
  runDb.setModuleRoots(moduleRootsFor(filepath));
  loadPrelude(runDb);
  // A `<name>.spec.kex`'s declarations come from its base `<name>.kex`, which
  // the Analyzer sees (they are merged into `program`) but the DB would not:
  // it re-reads `filepath` from disk. Pass the base as a companion so its
  // types and constructors are not reported as undefined names.
  std::vector<std::string> companions;
  if (!extraDeclFile.empty()) companions.push_back(extraDeclFile);
  runDb.updateFile(filepath, readFile(filepath), companions);
  bool dbOk = true;
  for (const auto &diag : runDb.diagnosticsFor(filepath)) {
    if (diag.level == kex::semantic::Diagnostic::Level::Error)
      dbOk = false;
    countError(diag);
    printSemanticDiagnostic(diag);
  }

  // Pass 3+: existing Analyzer (purity, type checking)
  kex::semantic::Analyzer localAnalyzer(&preludeSemanticInterfaces());
  auto &analyzer = retainedAnalyzer ? *retainedAnalyzer : localAnalyzer;
  bool ok = analyzer.analyze(program);
  for (const auto &diag : analyzer.diagnostics()) {
    countError(diag);
    printSemanticDiagnostic(diag);
  }

  bool validationOk = true;
  if (ok && dbOk) {
    for (const auto &diag :
         kex::validation::validateTaggedLiterals(program, analyzer, moduleRootsFor(filepath))) {
      if (diag.level == kex::semantic::Diagnostic::Level::Error)
        validationOk = false;
      countError(diag);
      printSemanticDiagnostic(diag);
    }
  }

  if (errorCount) *errorCount = errors;
  return ok && dbOk && validationOk;
}

auto printAst(const kex::ast::Program &program) -> void {
  std::cout << "Program (" << program.items.size() << " items)\n";
  for (const auto &item : program.items) {
    std::visit(
        [](const auto &node) {
          using T = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<T,
                                       std::unique_ptr<kex::ast::ModuleDef>>) {
            std::cout << "  Module: " << node->name << "\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::TypeDef>>) {
            std::cout << "  Type: " << node->name << "\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::RecordDef>>) {
            std::cout << "  Record: " << node->name << " ("
                      << node->fields.size() << " fields)\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::MakeDef>>) {
            std::cout << "  Make" << (node->isFinal ? " [final]" : "") << "\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::FunctionDef>>) {
            std::cout << "  Function: " << node->name
                      << (node->isFoul ? " [foul]" : "")
                      << (node->isPredicate ? " [?]" : "") << "\n";
          } else if constexpr (std::is_same_v<
                                   T,
                                   std::unique_ptr<kex::ast::CompiledBlock>>) {
            std::cout << "  Compiled block\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::UsingBlock>>) {
            std::cout << "  Using block\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::MainBlock>>) {
            std::cout << "  Main (" << node->body.size() << " expressions)\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::Pragma>>) {
            std::cout << "  Pragma: ";
            for (const auto &r : node->requirements)
              std::cout << r << " ";
            std::cout << "\n";
          }
        },
        item);
  }
}

// Runtime-configured or executable-relative Kex runtime beams. Using these
// avoids an erlc spawn (~0.15s per module) on every BEAM invocation.
auto prebuiltRuntimeBeamDir() -> std::string {
  namespace fs = std::filesystem;
  std::vector<fs::path> roots;
  if (const char *configured = std::getenv("KEX_RUNTIME_DIR");
      configured && *configured)
    roots.emplace_back(configured);
  if (const auto executableDir = kex::executableDirectory(); !executableDir.empty()) {
    roots.push_back((executableDir / "../share/kex/runtime").lexically_normal());
    roots.push_back((executableDir / "runtime/beam").lexically_normal());
  }
#ifdef KEX_RUNTIME_BEAM_DIR
  // Baked in for wasm, which has no executable path to search from. The CLI
  // build reads it straight off the host filesystem via NODERAWFS.
  roots.emplace_back(KEX_RUNTIME_BEAM_DIR);
#endif
  // Where the browser REPL's copy is mounted — see the --preload-file mapping
  // in CMakeLists.txt, next to /stdlib.
  roots.emplace_back("/runtime");
  for (const auto &root : roots) {
    std::error_code ec;
    if (fs::exists(root / "kex_io.beam", ec)) return root.string();
  }
  return {};
}

// The `erl` kex spawns for every BEAM mode (run -R, compile-run, the BEAM
// REPL). KEX_ERL overrides PATH, mirroring tey's TEY_ERL — the escape hatch
// for when the erl first on PATH is older than the runtime beams' compile
// OTP but a newer one is installed elsewhere.
auto erlExecutable() -> std::string {
  if (const char *override = std::getenv("KEX_ERL"); override && *override)
    return override;
  return "erl";
}

// The `erlc` kex shells out to for Core Erlang. It has to come from the same
// OTP install as erlExecutable() or KEX_ERL only half-works: pointing at a
// newer erl while PATH's erlc stays behind compiles session modules with one
// toolchain and loads them with another. erlc always sits beside erl, so an
// absolute KEX_ERL names its erlc too; KEX_ERLC overrides that directly.
auto erlcExecutable() -> std::string {
  if (const char *override = std::getenv("KEX_ERLC"); override && *override)
    return override;
  if (const char *erl = std::getenv("KEX_ERL"); erl && *erl) {
    std::filesystem::path p{erl};
    if (p.has_parent_path()) {
      auto sibling = p.parent_path() / "erlc";
      std::error_code ec;
      if (std::filesystem::exists(sibling, ec))
        return sibling.string();
    }
  }
  return "erlc";
}

#ifndef KEX_RUNTIME_OTP_FLOOR
#define KEX_RUNTIME_OTP_FLOOR 0
#endif

// A .beam cannot be loaded by an Erlang runtime older than the one whose
// erlc produced it (OTP 28 changed the atom table encoding: an OTP 27 `erl`
// rejects OTP 29 output with "corrupt atom table", and the run dies as an
// anonymous `undef`). KEX_RUNTIME_OTP_FLOOR — baked in by CMakeLists next to
// the runtime-beam compilation — is the OTP that compiled the bundled
// runtime beams; 0 means unknown and disables the check. Prepended to every
// eval kex hands to `erl`, so it costs one comparison per VM boot and fires
// before any module load can fail opaquely.
auto beamOtpFloorGuard() -> std::string {
  if (KEX_RUNTIME_OTP_FLOOR == 0)
    return {};
  return "case catch list_to_integer(erlang:system_info(otp_release)) of "
         "N when is_integer(N), N >= " +
         std::to_string(KEX_RUNTIME_OTP_FLOOR) +
         " -> ok; "
         "_ -> E=case os:find_executable(os:getenv(\"KEX_ERL\",\"erl\")) of "
         "false -> os:getenv(\"KEX_ERL\",\"erl\"); P -> P end, "
         "io:format(standard_error, \"error: the Kex runtime beams bundled "
         "with this kex were compiled with Erlang/OTP " +
         std::to_string(KEX_RUNTIME_OTP_FLOOR) +
         ", and ~ts (OTP ~ts) is too old to load them — without this check "
         "the run would die as 'corrupt atom table' or 'undef'.~nUpgrade "
         "Erlang, or point the KEX_ERL environment variable at an OTP " +
         std::to_string(KEX_RUNTIME_OTP_FLOOR) + "+ erl.~n\", "
         "[E, erlang:system_info(otp_release)]), halt(1) end, ";
}

// Every variable a pattern binds. `let x = ...` binds one, but
// `let Just(x) = ...` / `let (a, b) = ...` bind through the pattern, and the
// REPL has to remember those too or the names vanish on the next input.
auto collectPatternNames(const kex::ast::Pattern &pattern,
                         std::vector<std::string> &names) -> void {
  std::visit(
      [&](const auto &node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, kex::ast::VarPattern>) {
          if (node.name != "_") names.push_back(node.name);
        } else if constexpr (std::is_same_v<T, kex::ast::ConstructorPattern>) {
          for (const auto &arg : node.args)
            if (arg) collectPatternNames(*arg, names);
        } else if constexpr (std::is_same_v<T, kex::ast::TuplePattern>) {
          for (const auto &element : node.elements)
            if (element) collectPatternNames(*element, names);
        } else if constexpr (std::is_same_v<T, kex::ast::ListPattern>) {
          for (const auto &element : node.elements)
            if (element) collectPatternNames(*element, names);
          if (node.rest) collectPatternNames(**node.rest, names);
        } else if constexpr (std::is_same_v<T, kex::ast::RecordPattern>) {
          // `let { name, age } = user` — a field with no sub-pattern is the
          // shorthand binding, so the field name is what it binds.
          for (const auto &field : node.fields) {
            if (field.pattern) collectPatternNames(**field.pattern, names);
            else if (field.name != "_") names.push_back(field.name);
          }
        } else if constexpr (std::is_same_v<T, kex::ast::ThisPattern>) {
          if (node.inner) collectPatternNames(*node.inner, names);
        }
      },
      pattern.kind);
}

// Offset of the `=` separating a binding's pattern from its right-hand side,
// or npos. Bracket depth keeps a `=` inside the pattern from matching, and the
// comparison operators (`==`, `!=`, `<=`, `>=`) are skipped.
auto replBindingEqualsPos(const std::string &source, size_t from) -> size_t {
  int depth = 0;
  for (size_t i = from; i < source.size(); i++) {
    char c = source[i];
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') depth--;
    else if (c == '"' || c == '\'') { // skip a string/char literal
      char quote = c;
      for (i++; i < source.size() && source[i] != quote; i++)
        if (source[i] == '\\') i++;
    } else if (c == '=' && depth == 0) {
      if (i + 1 < source.size() && source[i + 1] == '=') { i++; continue; }
      if (i > from && (source[i - 1] == '!' || source[i - 1] == '<' ||
                       source[i - 1] == '>' || source[i - 1] == ':'))
        continue;
      return i;
    }
  }
  return std::string::npos;
}

// Names bound by a REPL input's trailing `let`, or empty if it is not one.
auto replLetBoundNames(const std::string &source) -> std::vector<std::string> {
  std::vector<std::string> names;
  kex::Lexer lexer("main do\n" + source + "\nend\n");
  kex::Parser parser(lexer.tokenizeAll());
  auto program = parser.parseProgram();
  if (!parser.diagnostics().empty()) return names;
  for (const auto &item : program.items) {
    const auto *main = std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&item);
    if (!main || !*main || (*main)->body.empty()) continue;
    const auto *binding =
        std::get_if<kex::ast::LetExpr>(&(*main)->body.back()->kind);
    if (binding && binding->pattern) collectPatternNames(*binding->pattern, names);
  }
  return names;
}

auto printUsage(const char *progName) -> void {
  std::cerr
      << "Usage: " << progName << " [options] <file.kex>\n"
      << "\n"
      << "Options:\n"
      << "  -r, --run         Interpret the program (default)\n"
      << "  -c, --compile     Compile to BEAM via Core Erlang\n"
      << "  -R, --run-beam    Run on BEAM (.kex or existing .beam; temp dir, "
         "auto-clean)\n"
      << "  -i, --interactive Interactive REPL on BEAM (also: kex -R with no "
         "file)\n"
      << "  -C, --check       Run semantic analysis only\n"
      << "  -n, --no-check    Skip semantic check when running\n"
      << "  -l, --lex         Print token stream\n"
      << "  -p, --parse       Print AST\n"
      << "  -j, --json        With --check: output diagnostics as JSON\n"
      << "      --lsp         Run the Kex language server over stdio\n"
      << "  -s, --summary     Print public API signatures (Kex syntax)\n"
      << "  -t, --types       With --check: dump inferred expression types\n"
      << "  -e, --emit-core   Emit Core Erlang (.core) — does not invoke erlc\n"
      << "      --emit-ast    Emit the parsed Kex AST as ETF\n"
      << "      --expand      Print the AST after `compiled do` expansion\n"
      << "      --collapse-report\n"
      << "                    Report what `compiled do` chain collapse folded "
         "away, and why\n"
      << "                    the rest did not — the feature is invisible at "
         "runtime\n"
      << "  -o <path>         Output directory for -c / --emit-core, or output "
         "file for --emit-ast\n"
      << "      --source-root <dir>\n"
      << "                    Add a module source root (repeatable)\n"
      << "      --allow-mocks Permit Mock.* outside a spec — it makes one part "
         "of a\n"
      << "                    program lie to another, so it is off unless the "
         "entry\n"
      << "                    file is a *.spec.kex or you ask for it here\n"
      << "      --test-json   Report describe/it as one JSON record per case "
         "(with\n"
      << "                    source locations), instead of the ✓/✗ prose\n"
      << "      --test-list   List the cases as JSON WITHOUT running any of "
         "them\n"
      << "      --test-only <name>\n"
      << "                    Run only this case, or this describe. `name` is "
         "the\n"
      << "                    full path — `outer > inner > case` — or a bare "
         "label.\n"
      << "                    Repeatable\n"
      << "  -h, --help        Show this help\n"
      << "  -v, --version     Show version\n"
      << "      --info        Print this build's details as JSON, for tools\n"
      << "  --no-colors       Disable ANSI color output\n";
}

auto printVersion() -> void {
  std::cout << "kex " << kex::versionStringWithBuild() << "\n";
}

// `--info`: everything a TOOL needs to know about this binary, as JSON on one
// line. `--version` is for people and its text may be reworded; this is a
// contract, and Tey depends on it — see docs/tey-resolver-plan.md.
//
// `runtime_otp_floor` is the reason this exists. The OTP release that compiled
// the bundled runtime beams is the oldest that can load them, it is baked in
// at build time (KEX_RUNTIME_OTP_FLOOR), and a package manager cannot read a
// C++ define. Without it, Tey can only guess at the requirement it is supposed
// to record and enforce.
//
// Fields may be ADDED; existing ones keep their name and type. A reader must
// ignore what it does not know, and treat a missing field as unknown rather
// than as an error — a Tey newer than the Kex it is asking will meet one.
// `null` for a value this build genuinely does not have, so the key's absence
// and its emptiness stay distinguishable.
auto printInfo() -> void {
  auto quoted = [](const std::string &value) {
    return value.empty() ? std::string("null") : "\"" + value + "\"";
  };
  std::cout << "{"
            << "\"version\":\"" << kex::versionNumber() << "\","
            << "\"revision\":" << quoted(kex::kGitRevision) << ","
            << "\"built\":" << quoted(kex::kBuildDate) << ","
            // 0 means "unknown", which disables the check — report it as null
            // rather than as a floor of zero that every runtime satisfies.
            << "\"runtime_otp_floor\":"
            << (KEX_RUNTIME_OTP_FLOOR == 0
                    ? std::string("null")
                    : std::to_string(KEX_RUNTIME_OTP_FLOOR))
            << ","
            // Where the prebuilt stdlib and runtime beams live. Empty when
            // this build has none, which is what makes every BEAM mode fail —
            // worth being able to ask about rather than infer.
            << "\"runtime_dir\":" << quoted(prebuiltRuntimeBeamDir()) << ","
            << "\"erl\":\"" << erlExecutable() << "\""
            << "}\n";
}

int main(int argc, char *argv[]) {
  static struct option longOptions[] = {
      {"run", no_argument, nullptr, 'r'},
      {"no-check", no_argument, nullptr, 'n'},
      {"lex", no_argument, nullptr, 'l'},
      {"parse", no_argument, nullptr, 'p'},
      {"check", no_argument, nullptr, 'C'},
      {"compile", no_argument, nullptr, 'c'},
      {"run-beam", no_argument, nullptr, 'R'},
      {"interactive", no_argument, nullptr, 'i'},
      {"json", no_argument, nullptr, 'j'},
      {"summary", no_argument, nullptr, 's'},
      {"types", no_argument, nullptr, 't'},
      {"emit-core", no_argument, nullptr, 'e'},
      {"emit-ast", no_argument, nullptr, 1011},
      {"ast-filename", required_argument, nullptr, 1012},
      {"complete", required_argument, nullptr, 'K'},
      {"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, 'v'},
      // Machine-readable counterpart to --version. Long-only: it is for
      // tools, and a short flag would invite it into shell prompts.
      {"info", no_argument, nullptr, 1009},
      {"no-colors", no_argument, nullptr, 'N'},
      {"no-prelude", no_argument, nullptr, 1003},
      {"source-root", required_argument, nullptr, 1008},
      // Mock.* is test-only (issue #144): a mock lets one part of a program
      // lie to another about the filesystem, environment, platform, network
      // or console, so the runtime denies the mock intrinsics unless the
      // entry file is a `*.spec.kex`, this is a REPL, or the user says so
      // here. Long-only and deliberately verbose — nobody should reach for
      // it by habit.
      {"allow-mocks", no_argument, nullptr, 1010},
      // The testing DSL, said in JSON rather than in prose (kexhq/kex#199).
      // `--test-json` replaces the ✓/✗ lines with one record per case —
      // including the FILE and LINE of the `it` and of the failure, which is
      // the part only the runner can know and the part an editor's inline
      // decoration needs. `--test-list` discovers the cases without running
      // any of them, and `--test-only` runs one (repeatable).
      {"test-json", no_argument, nullptr, 1013},
      {"test-list", no_argument, nullptr, 1014},
      {"test-only", required_argument, nullptr, 1015},
      {"lsp", no_argument, nullptr, 1007},
      // Print the AST AFTER compile-time expansion of `compiled do` blocks —
      // i.e. what the type checker and both backends actually see. `--parse`
      // shows the AST before it.
      {"expand", no_argument, nullptr, 1004},
      // Report every chain-collapse attempt and its outcome. Collapse changes
      // only the EMITTED code, so without this the only way to tell whether it
      // happened is to decode the Core Erlang by hand.
      {"collapse-report", no_argument, nullptr, 1005},
      // Compile the sources selected by src/stdlib/prelude.kex into kex_prelude.core +
      // kex_prelude.beam in the given dir. Used by the build to prebuild the
      // shared stdlib module alongside the runtime beams.
      {"build-prelude", required_argument, nullptr, 1001},
      {nullptr, 0, nullptr, 0}};

  std::string mode = "run";
  bool skipCheck = false;
  bool dumpTypes = false;
  bool jsonOutput = false;
  bool summaryMode = false;
  std::string completePrefix;
  std::string outputDir = ".";
  std::string astFilename;
  bool outputDirExplicit = false;
  int opt;

  bool compileRun = false;
  bool skipPrelude = false;
  bool collapseReport = false;
  bool allowMocks = false;
  auto testReportMode = kex::interpreter::Evaluator::TestReportMode::Pretty;
  std::vector<std::string> testFilters;
  while ((opt = getopt_long(argc, argv, "rnlcCiRjspethvK:o:", longOptions,
                            nullptr)) != -1) {
    switch (opt) {
    case 1003:
      skipPrelude = true;
      break;
    case 1004:
      mode = "expand";
      break;
    case 1005:
      collapseReport = true;
      break;
    case 1010:
      allowMocks = true;
      break;
    case 1013:
      testReportMode = kex::interpreter::Evaluator::TestReportMode::Json;
      break;
    case 1014:
      testReportMode = kex::interpreter::Evaluator::TestReportMode::List;
      break;
    case 1015:
      testFilters.emplace_back(optarg);
      break;
    case 1011:
      mode = "emit-ast";
      break;
    case 1012:
      astFilename = optarg;
      break;
    case 1007:
#ifdef KEX_HAS_LSP
      mode = "lsp";
#else
      std::cerr << "error: LSP support is unavailable in this build\n";
      return 1;
#endif
      break;
    case 1008:
      cliSourceRoots.emplace_back(optarg);
      break;
    case 1001: {
      std::string dir = optarg;
      std::vector<PreludeBuildModule> modules;
      if (!compilePreludeCore(dir, &modules))
        return 1;
      try {
        // The prelude build directory is also the installed-runtime staging
        // directory. Remove companions from older builds so deleting or
        // renaming a Kex module cannot leave a stale beam that gets installed
        // merely because it still matches `*.beam`.
        std::unordered_set<std::string> currentCompanions;
        for (auto &module : modules) {
          module.interface.intrinsicAbiVersion = kex::kIntrinsicAbiVersion;
          module.interface.backendRepresentationVersion =
              kex::kBeamRepresentationVersion;
        }
        for (size_t i = 1; i < modules.size(); i++)
          currentCompanions.insert(modules[i].emitted.moduleName);
        std::error_code cleanupError;
        for (const auto &entry : std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied,
                 cleanupError)) {
          if (cleanupError) break;
          const auto &path = entry.path();
          const auto stem = path.stem().string();
          const auto extension = path.extension().string();
          if (stem.rfind("Kex.", 0) == 0 &&
              (extension == ".beam" || extension == ".core") &&
              !currentCompanions.contains(stem))
            std::filesystem::remove(path, cleanupError);
          if (cleanupError) break;
        }
        if (cleanupError)
          throw std::runtime_error("could not remove stale stdlib artifact: " +
                                   cleanupError.message());

        for (const auto &module : modules) {
          // -W0: erlc's warnings analyze the GENERATED Core Erlang, not Kex —
          // they carry no source location (`no_file:`) and the emitter's dead
          // statement-value lets make them inevitable. Real errors still fail
          // the exit-code check below.
          std::string cmd = erlcExecutable() + " +from_core -W0 -pa " + dir +
                            " -o " + dir + " " + dir + "/" +
                            module.emitted.moduleName + ".core";
          if (runShellCommand(cmd) != 0) return 1;
        }

        for (size_t i = 1; i < modules.size(); i++)
          modules[i].interface.interfaceHash =
              kex::beam::computeInterfaceHash(modules[i].interface);
        for (auto &companion : modules[0].interface.metadata.companions)
          for (size_t i = 1; i < modules.size(); i++)
            if (modules[i].emitted.moduleName == companion.beamAtom)
              companion.expectedHash = modules[i].interface.interfaceHash;
        modules[0].interface.interfaceHash =
            kex::beam::computeInterfaceHash(modules[0].interface);

        for (auto &module : modules) {
          auto beamPath = dir + "/" + module.emitted.moduleName + ".beam";
          auto beam = kex::beam::readBeamFile(beamPath);
          module.interface.artifactHash = kex::beam::computeArtifactHash(beam);
          beam.setChunk(kex::beam::KEXI_CHUNK_ID,
                        kex::beam::serializeKexi(module.interface));
          kex::beam::writeBeamFile(beam, beamPath);
        }
      } catch (const std::exception &e) {
        std::cerr << "error: could not attach stdlib interface: " << e.what()
                  << "\n";
        return 1;
      }
      return 0;
    }
    case 'r':
      mode = "run";
      break;
    case 'n':
      skipCheck = true;
      break;
    case 'l':
      mode = "lex";
      break;
    case 'p':
      mode = "parse";
      break;
    case 'C':
      mode = "check";
      break;
    case 'c':
      mode = "compile";
      break;
    case 'R':
      mode = "compile";
      compileRun = true;
      break;
    case 'i':
      mode = "beam-repl";
      break;
    case 'j':
      jsonOutput = true;
      mode = "check";
      break;
    case 's':
      summaryMode = true;
      mode = "check";
      break;
    case 't':
      dumpTypes = true;
      break;
    case 'e':
      mode = "emit-core";
      break;
    case 'o':
      outputDir = optarg;
      outputDirExplicit = true;
      break;
    case 'K':
      completePrefix = optarg;
      mode = "complete";
      break;
    case 'h':
      printUsage(argv[0]);
      return 0;
    case 'v':
      printVersion();
      return 0;
    case 1009:
      printInfo();
      return 0;
    case 'N':
      kex::color::enabled = false;
      break;
    default:
      printUsage(argv[0]);
      return 1;
    }
  }

#ifndef __EMSCRIPTEN__
  // The BEAM runtime is a child process, so carry the CLI color choice over
  // explicitly. Console constants and the spec reporter share this setting.
  setenv("KEX_COLORS", kex::color::enabled ? "1" : "0", 1);
  // Runtime services that invoke the compiler (currently Kex.AST) must use
  // the toolchain that launched this process, not an unrelated `kex` found
  // earlier on PATH. Preserve an explicit override for embedding/tests.
  if (const char *configured = std::getenv("KEX");
      !configured || !*configured) {
    const auto currentExecutable =
        kex::executableDirectory() / std::filesystem::path(argv[0]).filename();
    setenv("KEX", currentExecutable.string().c_str(), 0);
  }
#endif

  if (mode == "lsp") {
#ifdef KEX_HAS_LSP
    return kex::lsp::run(std::cin, std::cout, prebuiltRuntimeBeamDir());
#endif
  }

  if (mode == "complete") {
    kex::semantic::SemanticDB db;
    db.setImportedInterfaces(&preludeSemanticInterfaces());
    if (optind < argc)
      db.setModuleRoots(moduleRootsFor(argv[optind]));
    loadPrelude(db);
    if (optind < argc)
      db.updateFile(argv[optind], readFile(argv[optind]));
    // Simulate readline not splitting: start=0, text=completePrefix
    auto cq = kex::resolveCompletionQuery(completePrefix.c_str(), 0,
                                          completePrefix.c_str());
    auto raw = db.completionsFor(cq.dbQuery);
    auto completions =
        kex::rewriteCompletions(std::move(raw), cq.rewriteFrom, cq.rewriteTo);
    for (const auto &c : completions)
      std::cout << c << "\n";
    return 0;
  }

  if (optind >= argc && mode != "repl") {
    // No file — enter REPL mode (BEAM REPL if -R was given, tree-walker
    // otherwise)
    if (mode == "compile" && compileRun)
      mode = "beam-repl";
    else if (mode != "beam-repl")
      mode = "repl";
  }

  if (mode == "beam-repl") {
    // ── Writable temp dir for per-eval user beams ─────────────────────
    char rtmpl[] = "/tmp/kex_irepl_XXXXXX";
    char *rtd = mkdtemp(rtmpl);
    if (!rtd) {
      std::cerr << "error: mkdtemp failed\n";
      return 1;
    }
    std::string beamDir = rtd;

    // Runtime artifacts are produced explicitly by the toolchain build.
    std::string rtPaDir = prebuiltRuntimeBeamDir();
    if (rtPaDir.empty()) {
      std::cerr << "error: prebuilt runtime artifacts are missing; "
                   "rebuild or reinstall the Kex toolchain\n";
      std::filesystem::remove_all(beamDir);
      return 1;
    }

    // Spawn ONE persistent erl VM for the whole session — driven by
    // runtime/src/kex_repl_driver.erl over stdin/stdout. This is what
    // makes spawned processes / registered names survive across REPL
    // inputs and turns session recompiles into hot-loads instead of
    // cold re-runs.
    BeamVm vm;
    {
      std::vector<std::string> erlArgs = {erlExecutable(), "-noshell", "-pa",
                                          beamDir};
      if (!rtPaDir.empty()) {
        erlArgs.push_back("-pa");
        erlArgs.push_back(rtPaDir);
      }
      erlArgs.push_back("-eval");
      // allow_mocks/0 before the loop, in the same process the driver
      // evaluates input in — the BEAM half of "a REPL may mock" (issue #144).
      // The flag lives in the process dictionary, so this grants it for the
      // session and for nothing else.
      erlArgs.push_back(beamOtpFloorGuard() +
                        "kex_test:allow_mocks(), kex_repl_driver:loop()");
      if (!vm.start(erlArgs)) {
        std::cerr << "error: could not start erl VM for BEAM REPL\n";
        std::filesystem::remove_all(beamDir);
        return 1;
      }
    }

    // readline history — same file as the tree-walker REPL, so a session
    // in one is recalled in the other.
#ifdef HAS_READLINE
    std::string historyFile;
    if (const char *home = std::getenv("HOME")) {
      std::filesystem::path histDir =
          std::filesystem::path(home) / ".config" / "kex";
      std::error_code ec;
      std::filesystem::create_directories(histDir, ec);
      historyFile = (histDir / "history").string();
      read_history(historyFile.c_str());
    }
#endif

    {
      std::string nonce = "info_boot";
      std::string sentinel = "KEX_REPL_DONE " + nonce + " ";
      vm.writeLine("info " + nonce);
      std::string status;
      std::string otpInfo = vm.readUntilSentinel(sentinel, status);
      // The VM died before answering the first handshake — an OTP too old for
      // the bundled beams (the floor guard has already said so on stderr), a
      // missing runtime module, a broken erl. Without this the REPL prints
      // its banner over the corpse and answers every input with "failed to
      // load session module".
      if (status == "eof") {
        vm.close();
        std::cerr
            << "error: the BEAM runtime did not start; cannot run the REPL\n";
        std::filesystem::remove_all(beamDir);
        return 1;
      }
      while (!otpInfo.empty() && otpInfo.back() == '\n')
        otpInfo.pop_back();
      if (!otpInfo.empty())
        std::cout << kex::color::apply(kex::color::gray) << otpInfo
                  << kex::color::apply(kex::color::reset) << "\n";
    }
    kex::printReplBanner(std::cout, "BEAM");

    if (!skipPrelude) {
      const auto preludeDisplay = preludeDisplayRegistration();
      if (!preludeDisplay.empty()) {
        std::string nonce = "display_boot";
        vm.writeLine("exec " + nonce + " " + preludeDisplay);
        std::string status;
        vm.readUntilSentinel("KEX_REPL_DONE " + nonce + " ", status);
      }
    }

    kex::semantic::SemanticDB beamReplDb;
    if (!skipPrelude) {
      beamReplDb.setImportedInterfaces(&preludeSemanticInterfaces());
      loadPrelude(beamReplDb);
    }
#ifdef HAS_READLINE
    g_replDb = &beamReplDb;
    rl_attempted_completion_function = kexCompletion;
    rl_completion_display_matches_hook = kexDisplayMatches;
    rl_completer_word_break_characters = (char *)" \t\n\\@$><=;|&{(";
    rl_completer_quote_characters = (char *)"";
    bindReplCancelKeys();
#endif

    // Top-level definitions, tracked by name so redefining a function
    // REPLACES its earlier clauses rather than appending duplicates (a
    // stale first clause would otherwise shadow the new one — the
    // "this clause cannot match" BEAM warning). Insertion order kept so
    // independent defs stay in source order.
    std::vector<std::pair<std::string, std::string>> topDefs;
    std::string localBinds; // let x = ... — re-emitted inside main do each eval
    // Names bound with `var`. They are replayed as `var` (not `let`) so a
    // later line may reassign or `!`-mutate them, and every evaluation stores
    // them back into the process dictionary so those mutations survive.
    std::vector<std::string> mutableBinds;
    auto isMutableBind = [&](const std::string &name) {
      return std::find(mutableBinds.begin(), mutableBinds.end(), name) !=
             mutableBinds.end();
    };
    // Original local binding sources retained for semantic replay. Runtime
    // replay uses process-dictionary lookups, which intentionally erase the
    // source type and therefore cannot enforce typestate by itself.
    std::vector<std::pair<std::string, std::string>> beamSemanticBinds;
    std::optional<std::string> pendingLine; // read-ahead during clause chaining
    int iteration = 0;
    std::vector<std::string> loadedBeamFiles; // .kex paths loaded via /load
    kex::beam::KexiRegistry kexiRegistry;

    auto topDefsStr = [&]() -> std::string {
      std::string s;
      for (auto &[n, src] : topDefs)
        s += src + "\n";
      return s;
    };

    // Counts unmatched do/end block keywords (word-delimited) to decide
    // whether more lines are needed to close a multi-line block.
    auto countBlocks = [](const std::string &s) -> int {
      int count = 0;
      for (size_t i = 0; i < s.size(); i++) {
        if (i + 2 <= s.size() && s.substr(i, 2) == "do") {
          bool wb = (i > 0 && std::isalnum((unsigned char)s[i - 1]));
          bool wa = (i + 2 < s.size() && std::isalnum((unsigned char)s[i + 2]));
          if (!wb && !wa)
            count++;
        }
        if (i + 3 <= s.size() && s.substr(i, 3) == "end") {
          bool wb = (i > 0 && std::isalnum((unsigned char)s[i - 1]));
          bool wa = (i + 3 < s.size() && std::isalnum((unsigned char)s[i + 3]));
          if (!wb && !wa)
            count--;
        }
      }
      return count;
    };

    // If `s` is a function-clause header (`let name(` / `foul name(`),
    // return the function name — used to chain consecutive clauses of the
    // same function into one definition (mirrors the tree-walker REPL).
    auto clauseFuncName =
        [](const std::string &s) -> std::optional<std::string> {
      size_t offset;
      if (s.rfind("foul ", 0) == 0)
        offset = 5;
      else if (s.rfind("let ", 0) == 0)
        offset = 4;
      else
        return std::nullopt;
      // `let Just(x) = ...` destructures a constructor pattern; it is not a
      // function definition. Function names are lowercase, so an uppercase
      // initial rules the clause reading out — otherwise the REPL treated the
      // binding as defining a function named `Just`, waited for further
      // clauses, and re-evaluated it later in a scope where the right-hand
      // side's locals no longer existed ("Undefined identifier: a").
      if (offset < s.size() && std::isupper((unsigned char)s[offset]))
        return std::nullopt;

      size_t i = offset;
      while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_'))
        i++;
      if (i == offset)
        return std::nullopt;
      std::string name = s.substr(offset, i - offset);
      if (i < s.size() && s[i] == '?') {
        name += '?';
        i++;
      }
      if (i < s.size() && s[i] == '(')
        return name;
      return std::nullopt;
    };

    while (true) {
      std::string input;
      if (pendingLine) {
        input = std::move(*pendingLine);
        pendingLine.reset();
      } else {
        auto [l, ok] = readLine("kex(beam)> ");
        if (!ok)
          break;
        input = l;
      }
      input = replTrimLeadingIndent(std::move(input));
      if (input.empty())
        continue;
      if (kex::isReplExit(input))
        break;
      if (input == "/help" || input == "/h") {
        kex::printReplHelp(std::cout);
        continue;
      }
      if (input == "/clear") {
        std::cout << kex::replClearScreenSequence() << std::flush;
        continue;
      }
      if (input == "/reset") {
        topDefs.clear();
        localBinds.clear();
        mutableBinds.clear();
        beamSemanticBinds.clear();
        iteration = 0;
        std::cout << "  (bindings cleared)\n";
        continue;
      }
      if (input == "/set" || input.substr(0, 5) == "/set " ||
          input.substr(0, 7) == "/unset ") {
        std::cerr << "  not yet available in the BEAM REPL\n";
        continue;
      }
      if (input.substr(0, 10) == "/complete ") {
        auto prefix = input.substr(10);
        auto results = beamReplDb.completionsFor(prefix);
        if (results.empty())
          std::cout << "  (no completions for \"" << prefix << "\")\n";
        else
          for (const auto& r : results) std::cout << "  " << r << "\n";
        continue;
      }
      if (input.substr(0, 6) == "/load ") {
        std::string filePath = input.substr(6);
        size_t start = filePath.find_first_not_of(" \t");
        if (start != std::string::npos) filePath = filePath.substr(start);
        size_t end = filePath.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) filePath = filePath.substr(0, end + 1);
        if (!fileExists(filePath)) {
          std::cerr << "  /load: file not found: " << filePath << "\n";
          continue;
        }

        // Compiled .beam/.kx.beam: load via KexI registry + hot-load into VM
        bool isBeam = (filePath.size() > 5 &&
                       filePath.substr(filePath.size() - 5) == ".beam");
        if (isBeam) {
          auto absPath = std::filesystem::weakly_canonical(filePath).string();
          auto errors = kexiRegistry.loadUnit(absPath);
          if (!errors.empty()) {
            for (const auto& err : errors)
              std::cerr << "  /load: " << err.message << "\n";
            continue;
          }
          auto* unit = kexiRegistry.getUnit(
              kexiRegistry.lastLoadedEntryAtom());
          if (!unit) {
            std::cerr << "  /load: internal error\n";
            continue;
          }
          // Hot-load each module into the running BEAM VM using the
          // VM's "load" protocol command.
          bool loadOk = true;
          for (const auto& mod : unit->modules) {
            std::string nonce = std::to_string(++iteration);
            vm.writeLine("load " + nonce + " " + mod.beamAtom +
                         " " + mod.beamPath);
            std::string status;
            vm.readUntilSentinel("KEX_REPL_DONE " + nonce + " ", status);
            if (status != "ok") {
              std::cerr << "  /load: failed to hot-load " << mod.beamAtom
                        << "\n";
              loadOk = false;
              break;
            }
          }
          if (!loadOk) continue;

          auto displayExpr = kexiRegistry.generateDisplayRegistration(*unit);
          if (!displayExpr.empty()) {
            std::string dn = std::to_string(++iteration);
            vm.writeLine("exec " + dn + " " + displayExpr);
            std::string ds;
            vm.readUntilSentinel("KEX_REPL_DONE " + dn + " ", ds);
          }

          auto stubs = kexiRegistry.generateCompletionStubs(*unit);
          if (!stubs.empty()) {
            try {
              beamReplDb.updateFile("<kexi:" +
                  kexiRegistry.lastLoadedEntryAtom() + ">", stubs);
            } catch (...) {}
          }

          std::cout << "  loaded " << filePath;
          if (unit->modules.size() > 1)
            std::cout << " (" << unit->modules.size() << " modules)";
          std::cout << "\n";
          continue;
        }

        // Source .kex file: validate syntax and register for session build
        try {
          auto src = readFile(filePath);
          kex::Lexer lex(std::move(src), filePath);
          kex::Parser parser(lex.tokenizeAll(), filePath);
          parser.parseProgram();
          throwOnParseErrors(parser);
          loadedBeamFiles.push_back(filePath);
          std::cout << "  loaded " << filePath << "\n";
        } catch (const std::exception& e) {
          std::cerr << "  /load parse error: " << e.what() << "\n";
        }
        continue;
      }
      if (input.substr(0, 8) == "/unload ") {
        std::string modName = input.substr(8);
        size_t start = modName.find_first_not_of(" \t");
        if (start != std::string::npos) modName = modName.substr(start);
        size_t end = modName.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) modName = modName.substr(0, end + 1);

        std::string entryAtom;
        if (kexiRegistry.isLoaded(modName))
          entryAtom = modName;
        else
          entryAtom = kexiRegistry.findEntryByShortName(modName);

        if (entryAtom.empty()) {
          std::cerr << "  /unload: module '" << modName << "' is not loaded\n";
          continue;
        }

        auto* unit = kexiRegistry.getUnit(entryAtom);
        if (unit) {
          for (const auto& mod : unit->modules) {
            std::string dn = std::to_string(++iteration);
            vm.writeLine("exec " + dn + " code:purge('" + mod.beamAtom +
                         "'), code:delete('" + mod.beamAtom + "')");
            std::string ds;
            vm.readUntilSentinel("KEX_REPL_DONE " + dn + " ", ds);
          }
        }

        beamReplDb.removeFile("<kexi:" + entryAtom + ">");
        kexiRegistry.unloadUnit(entryAtom);
        std::cout << "  unloaded " << modName << "\n";
        continue;
      }
      if (input == "/reload") {
        // Loaded files are re-read on every session build — no explicit
        // reload needed. /reset clears topDefs + localBinds but keeps
        // loaded paths; use /reset then /load each file again to refresh.
        std::cout << "  (loaded files are re-evaluated on each input)\n";
        continue;
      }

      // Accumulate do/end blocks, then chain consecutive clauses of the
      // same function so `let f(0) = ...` / `let f(n) = ...` combine,
      // and a bare `let f(` header keeps reading instead of being
      // accepted as a broken standalone clause. Matches the tree-walker.
      std::string source = input;
      int dc = countBlocks(source);
      bool cancelled = false;
      while (dc > 0 || replHasOpenDelimiter(source)) {
        auto [cont, contOk] = readLine("  ...> ");
        if (takeReplInputCancelled()) {
          cancelled = true;
          break;
        }
        if (!contOk)
          break;
        source += "\n" + cont;
        dc = countBlocks(source);
      }
      if (cancelled) {
        std::cout << kex::color::apply(kex::color::gray) << "   (cancelled)"
                  << kex::color::apply(kex::color::reset) << "\n";
        continue;
      }
      // Clauses of one function are NOT gathered by reading ahead here. That
      // is what made a complete line hang on `...>`: after `let fact(0) = 1`
      // the REPL blocked waiting to see whether the next line was another
      // clause, with nothing on screen to say so. Each line is now accepted
      // on its own and clauses accumulate at definition time instead.

      // Classify: function def vs local let vs expression
      bool isFuncDef = false;
      bool isLocalLet = false;
      bool isMutableLet = false;
      std::string letVarName;
      std::vector<std::string> patternLetNames;
      {
        size_t off = std::string::npos;
        if (source.rfind("let ", 0) == 0)
          off = 4;
        else if (source.rfind("foul ", 0) == 0)
          off = 5;
        // `var x = v` is a binding, never a definition — a mutable one, so it
        // takes the local-binding path rather than being wrapped as an
        // expression (which lowered `IO.inspect(var x = v)` and failed).
        else if (source.rfind("var ", 0) == 0)
          isMutableLet = true;
        // A destructuring `let` binds the pattern's variables; it does not
        // define a function. `let Just(x) = ...` must not define `Just`, and
        // `let (a, b) = ...` must not be read as the `name(params)` shape —
        // without the pattern openers here it matched "paren before =" and the
        // REPL announced "defined", binding nothing. Function names are
        // lowercase identifiers, so an uppercase initial or a `(`/`[`/`{`
        // opener rules the definition reading out. Getting this wrong also
        // kept the binding as a top-level definition that was re-evaluated
        // later in a scope where the right-hand side's locals were gone
        // ("Undefined identifier: a").
        const char patternLead =
            (off != std::string::npos && off < source.size()) ? source[off] : '\0';
        const bool upperConstant = replIsUpperConstantBinding(source, off);
        const bool destructuringPattern =
            !upperConstant &&
            (std::isupper((unsigned char)patternLead) || patternLead == '(' ||
             patternLead == '[' || patternLead == '{');
        if (upperConstant) {
          isFuncDef = true;
        } else if (destructuringPattern) {
          // `let Just(x) = ...` binds through the pattern; persist each name.
          patternLetNames = replLetBoundNames(source);
          isLocalLet = !patternLetNames.empty();
          if (!isLocalLet) isFuncDef = true;
        } else if (off != std::string::npos) {
          auto parenPos = source.find('(', off);
          auto eqPos = source.find('=', off);
          bool hasParenBeforeEq =
              parenPos != std::string::npos &&
              (eqPos == std::string::npos || parenPos < eqPos);
          if (hasParenBeforeEq) {
            isFuncDef = true;
          } else if (eqPos != std::string::npos) {
            isLocalLet = true;
            size_t i = off;
            while (i < source.size() &&
                   (std::isalnum((unsigned char)source[i]) || source[i] == '_'))
              i++;
            letVarName = source.substr(off, i - off);
          } else {
            isFuncDef = true; // 0-arity function def
          }
        }
        if (isMutableLet) {
          size_t i = 4;
          while (i < source.size() &&
                 (std::isalnum((unsigned char)source[i]) || source[i] == '_'))
            i++;
          letVarName = source.substr(4, i - 4);
          isLocalLet = !letVarName.empty() &&
                       source.find('=', i) != std::string::npos;
          isMutableLet = isLocalLet;
        }
      }
      if (source.rfind("module ", 0) == 0 || source.rfind("type ", 0) == 0 ||
          source.rfind("record ", 0) == 0 || source.rfind("make ", 0) == 0 ||
          source.rfind("using ", 0) == 0)
        isFuncDef = true;

      try {
        if (isFuncDef) {
          // Validate by parsing against accumulated defs.
          std::string check = topDefsStr() + source + "\n";
          kex::Lexer lexer(check);
          auto tokens = lexer.tokenizeAll();
          kex::Parser parser(std::move(tokens));
          parser.parseProgram();
          throwOnParseErrors(parser);

          std::string fname = replDefinitionName(source);

          // A new clause of a function already defined EXTENDS it; anything
          // else replaces what was there. Redefinition still takes effect
          // (last def wins) because a clause whose parameter patterns match
          // an existing one overwrites that clause rather than being added
          // beside it — otherwise the stale clause would shadow the new one
          // and BEAM would warn that it cannot match.
          auto existing =
              std::find_if(topDefs.begin(), topDefs.end(),
                           [&](const auto &p) { return p.first == fname; });
          const auto head = replClauseHead(source);
          if (existing != topDefs.end() && head) {
            // Substituted IN PLACE, never moved to the end: clause order is
            // match order, so hoisting a catch-all above a base case turns
            // `fact` into an infinite recursion.
            std::string rebuilt;
            bool substituted = false;
            for (const auto &clause :
                 splitReplClauses(existing->second, countBlocks)) {
              if (!rebuilt.empty()) rebuilt += "\n";
              if (replClauseHead(clause) == head) {
                rebuilt += source;
                substituted = true;
              } else {
                rebuilt += clause;
              }
            }
            if (!substituted) {
              if (!rebuilt.empty()) rebuilt += "\n";
              rebuilt += source;
            }
            existing->second = rebuilt;
          } else {
            topDefs.erase(
                std::remove_if(topDefs.begin(), topDefs.end(),
                               [&](const auto &p) { return p.first == fname; }),
                topDefs.end());
            topDefs.push_back({fname, source});
          }

          // `using M` is an import, not a definition — it is kept in topDefs
          // so it persists across inputs, but saying "defined M" is wrong.
          const bool isImport = source.rfind("using ", 0) == 0;
          std::cout << kex::color::apply(kex::color::gray) << "=> "
                    << kex::color::apply(kex::color::reset)
                    << (isImport ? "using " : "defined ") << fname << "\n";
        } else {
          // Expression or local let — compile and run on BEAM.
          // For local lets, the current binding is evaluated fresh and
          // stashed in the process dictionary so subsequent evals
          // retrieve it (avoiding re-evaluation of side-effectful
          // expressions like Ets.new).
          std::string semanticBindSource;
          for (const auto &[name, binding] : beamSemanticBinds)
            if (!isLocalLet || name != letVarName)
              semanticBindSource += "  " + binding + "\n";
          auto semanticSource = topDefsStr() + "main do\n" +
                                semanticBindSource + "  " + source +
                                "\nend\n";
          kex::Lexer semanticLexer(semanticSource, "<repl>");
          kex::Parser semanticParser(semanticLexer.tokenizeAll(), "<repl>");
          auto semanticProgram = semanticParser.parseProgram();
          // Loaded declarations go BEFORE the replayed main block: the
          // checker registers a module's functions as it checks them, so a
          // module appended after the call site is registered too late and
          // `Loaded.values()` came back untyped — the walker REPL, which
          // prepends the same source, reported `[Integer]` for it while this
          // one fell back to the runtime `[Int]`.
          std::vector<kex::ast::TopLevelItem> loadedItems;
          for (const auto& f : loadedBeamFiles) {
            auto fs = readFile(f);
            kex::Lexer fl(std::move(fs), f);
            kex::Parser fp(fl.tokenizeAll(), f);
            auto fprog = fp.parseProgram();
            for (auto& item : fprog.items)
              if (!std::holds_alternative<
                      std::unique_ptr<kex::ast::MainBlock>>(item))
                loadedItems.push_back(std::move(item));
          }
          if (!loadedItems.empty()) {
            loadedItems.reserve(loadedItems.size() + semanticProgram.items.size());
            for (auto& item : semanticProgram.items)
              loadedItems.push_back(std::move(item));
            semanticProgram.items = std::move(loadedItems);
          }
          // Installed stdlib modules already have a typed source interface.
          // Keep them external here: merging their source into the replay
          // would make the local module shell shadow that interface and erase
          // refined results such as FileHandle<CannotRead, CanWrite>.
          kex::semantic::Analyzer semanticAnalyzer(
              &preludeSemanticInterfaces());
          if (!semanticAnalyzer.analyze(semanticProgram)) {
            for (const auto &diagnostic : semanticAnalyzer.diagnostics())
              if (diagnostic.level ==
                  kex::semantic::Diagnostic::Level::Error)
                throw std::runtime_error(diagnostic.message);
            throw std::runtime_error("semantic analysis failed");
          }
          std::optional<std::string> beamSemanticType;
          for (auto item = semanticProgram.items.rbegin();
               item != semanticProgram.items.rend(); ++item) {
            auto *main =
                std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&*item);
            if (!main || !*main || (*main)->body.empty())
              continue;
            const auto *last = (*main)->body.back().get();
            const kex::ast::Expr *typedExpr = last;
            if (const auto *binding =
                    std::get_if<kex::ast::LetExpr>(&last->kind);
                binding && binding->value)
              typedExpr = binding->value.get();
            beamSemanticType = displayTypeOf(semanticAnalyzer, typedExpr);
            break;
          }
          const auto inspectCall = [&](const std::string &expression) {
            if (!beamSemanticType)
              return "Kex.Intrinsic.IO.inspectRepl(" + expression + ")";
            return "Kex.Intrinsic.IO.inspectTyped(" + expression + ", \"" +
                   *beamSemanticType + "\")";
          };

          // Validate the input on its own before it is wrapped. Wrapping it as
          // `IO.inspect(<source>)` can turn a syntax error into a well-formed
          // argument list — `1,2` became `IO.inspect(1,2)`, which parses as a
          // two-argument call and only failed later on BEAM as an opaque
          // `undef`. Checking it inside a bare `main` is what the walker REPL
          // effectively does, so both report the same syntax error.
          {
            kex::Lexer checkLexer("main do\n" + source + "\nend\n");
            kex::Parser checkParser(checkLexer.tokenizeAll());
            checkParser.parseProgram();
            throwOnParseErrors(checkParser);
          }

          const auto putBack = [](const std::string &name) {
            return "  Erlang.Erlang.put(:kexrepl" + name + ", " + name + ")\n";
          };
          // Any line can mutate a `var` from an earlier line — by reassigning
          // it or through a `!` method — so every tracked mutable name is
          // written back, not only the one this line binds.
          std::string mutablePuts;
          for (const auto &name : mutableBinds)
            mutablePuts += putBack(name);

          // `x = v` and `x.foo!(v)` are statements: lowering rejects a `!`
          // receiver that is not a plain binding, and neither node lowers as
          // an expression, so they cannot go inside `IO.inspect(...)`. Run the
          // line as a statement and inspect the variable afterwards.
          std::string mutatedName;
          if (!isLocalLet && !isFuncDef) {
            size_t i = 0;
            while (i < source.size() &&
                   (std::isalnum((unsigned char)source[i]) || source[i] == '_'))
              i++;
            const auto head = source.substr(0, i);
            const auto rest = source.find_first_not_of(" \t", i);
            const bool reassigns = rest != std::string::npos &&
                                   source[rest] == '=' &&
                                   rest + 1 < source.size() &&
                                   source[rest + 1] != '=';
            const bool mutates = rest != std::string::npos &&
                                 source[rest] == '.' &&
                                 source.find('!', rest) != std::string::npos;
            if (!head.empty() && isMutableBind(head) && (reassigns || mutates))
              mutatedName = head;
          }

          std::string kexSource;
          if (isLocalLet) {
            std::string puts;
            std::string shown = letVarName;
            std::string bindLine = "  " + source + "\n";
            if (!patternLetNames.empty()) {
              for (const auto &name : patternLetNames)
                puts += putBack(name);
              // Echo the whole MATCHED value, not the first name bound out of
              // it: the type comes from analysing the binding as a whole, so
              // showing `a` against it printed `1 : (Integer, Integer)`. The
              // right-hand side is bound to a temporary first so it is still
              // evaluated exactly once.
              constexpr const char *matched = "__replMatched";
              auto eq = replBindingEqualsPos(source, 0);
              if (eq != std::string::npos) {
                bindLine = "  let " + std::string(matched) + " =" +
                           source.substr(eq + 1) + "\n" +
                           "  " + source.substr(0, eq + 1) + " " + matched + "\n";
                shown = matched;
              } else {
                shown = patternLetNames.front();
              }
            } else {
              puts = putBack(letVarName);
            }
            kexSource = topDefsStr() + "main do\n" + localBinds + bindLine +
                        puts + mutablePuts +
                        "  " + inspectCall(shown) + "\nend\n";
          } else if (!mutatedName.empty()) {
            kexSource = topDefsStr() + "main do\n" + localBinds + "  " +
                        source + "\n" + mutablePuts +
                        "  " + inspectCall(mutatedName) + "\nend\n";
          } else {
            kexSource = topDefsStr() + "main do\n" + localBinds +
                        "  " + inspectCall(source) + "\n" + mutablePuts +
                        "end\n";
          }

          kex::Lexer lexer(kexSource);
          auto tokens = lexer.tokenizeAll();
          kex::Parser parser(std::move(tokens));
          auto program = parser.parseProgram();
          throwOnParseErrors(parser);
          // Merge non-MainBlock items from files loaded via /load so their
          // definitions (function defs, make blocks, records, types) are
          // visible to every subsequent REPL input. Re-read on each session
          // build so the current on-disk version is always used.
          for (const auto& f : loadedBeamFiles) {
            try {
              auto fs = readFile(f);
              kex::Lexer fl(std::move(fs), f);
              kex::Parser fp(fl.tokenizeAll(), f);
              auto fprog = fp.parseProgram();
              for (auto& item : fprog.items)
                if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(item))
                  program.items.push_back(std::move(item));
            } catch (...) {
              // Syntax errors in a loaded file were caught by /load; if the
              // file changed underneath us, just skip it this round.
            }
          }
          // Source-module imports in a REPL expression need the same
          // dependency expansion as `kex -R file.kex`, for both `using` and
          // qualified access such as `Units.SI.Meter`.
          kex::semantic::Analyzer replDependencyAnalysis(
              &preludeSemanticInterfaces());
          (void)replDependencyAnalysis.analyze(program);
          auto replQualifiedModules =
              replDependencyAnalysis.referencedModules();
          for (auto it = replQualifiedModules.begin();
               it != replQualifiedModules.end();) {
            const auto imported =
                preludeSemanticInterfaces().modules.find(*it);
            if (imported !=
                    preludeSemanticInterfaces().modules.end() &&
                imported->second.automaticImport)
              it = replQualifiedModules.erase(it);
            else
              ++it;
          }
          auto replDeps = resolveBeamDeps(
              program, kex::standardLibraryModuleRoots(),
              &preludeSemanticInterfaces(),
              replQualifiedModules);
          (void)replDeps;
          auto extMods = mergeExternalModules(
              preludeExternalModules(), kexiRegistry.buildExternalModules());
          kex::semantic::Analyzer replAnalyzer(&preludeSemanticInterfaces());
          replAnalyzer.analyze(program);
          auto replRecordLayouts = loadPreludeRecordLayouts();
          auto replVariantTags = loadPreludeVariantTags();
          auto irModules = kex::ir::lowerModules(
              program, "kex_repl_session", "", &replRecordLayouts,
              extMods.nameToAtom.empty() ? nullptr : &extMods,
              &replAnalyzer.resolvedCalls(),
              /*preferExternalReceivers=*/false, &replVariantTags,
              // Without this the REPL never saw the checker's `Type.of`
              // answers: `Type.of(someFunction)` fell back to the runtime,
              // which lowered the bare function name to an unbound Core
              // Erlang variable and failed to compile.
              &replAnalyzer.staticTypeOfCalls(),
              &replAnalyzer.typeMap());
          std::vector<kex::ir::EmitResult> results;
          for (const auto& irModule : irModules)
            results.push_back(kex::ir::emitCore(irModule));
          const auto& result = results.front();

          bool compiled = true;
          for (const auto& emitted : results) {
            std::string corePath = beamDir + "/" + emitted.moduleName + ".core";
            std::ofstream cf(corePath);
            if (!cf) {
              std::cerr << "  error: cannot write " << corePath << "\n";
              compiled = false;
              break;
            }
            cf << emitted.source;
            cf.close();

            std::string erlCmd = erlcExecutable() + " +from_core -W0 -pa " +
                                 beamDir + " -o " + beamDir + " " +
                                 corePath + " 2>&1";
            int erlcRet = runShellCommand(erlCmd);
            std::filesystem::remove(corePath);
            if (erlcRet != 0) {
              compiled = false;
              break;
            }
          }
          if (!compiled) {
            std::cerr << "  error: compilation failed\n";
            continue;
          }

          // Hot-load the freshly compiled session module into the
          // persistent VM, then evaluate it. The stable module name
          // means each reload is a new version superseding the
          // previous — code:load_binary's native code-upgrade path.
          bool loaded = true;
          // Load companion modules first; the session entry can then call
          // imported module functions immediately after it is hot-loaded.
          for (auto it = results.rbegin(); it != results.rend(); ++it) {
            std::string beamPath = beamDir + "/" + it->moduleName + ".beam";
            std::string loadNonce = std::to_string(++iteration);
            vm.writeLine("load " + loadNonce + " " + it->moduleName + " " +
                         beamPath);
            std::string loadStatus;
            vm.readUntilSentinel("KEX_REPL_DONE " + loadNonce + " ", loadStatus);
            if (loadStatus != "ok") {
              loaded = false;
              break;
            }
          }
          if (!loaded) {
            std::cerr << "  " << kex::color::apply(kex::color::red)
                      << "error:" << kex::color::apply(kex::color::reset)
                      << " failed to load session module\n";
            continue;
          }
          std::string evalNonce = std::to_string(++iteration);
          vm.writeLine("eval " + evalNonce + " " + result.moduleName);
          std::string evalStatus;
          std::string output = vm.readUntilSentinel(
              "KEX_REPL_DONE " + evalNonce + " ", evalStatus);
          if (evalStatus == "ok") {
            std::cout << output;
          } else {
            std::cerr << "  " << kex::color::apply(kex::color::red)
                      << "error:" << kex::color::apply(kex::color::reset) << " "
                      << output;
          }

          if (isLocalLet && evalStatus == "ok") {
            const auto equals = source.find('=');
            const auto rhs = equals == std::string::npos
                ? std::string::npos
                : source.find_first_not_of(" \t", equals + 1);
            // An anonymous fun belongs to the currently loaded session
            // module. Keeping it in the process dictionary turns it into a
            // badfun after that module is hot-reloaded, so retain its source
            // and recreate it on every REPL evaluation instead.
            const bool isLambda = rhs != std::string::npos &&
                (source[rhs] == '&' ||
                 (source[rhs] == '{' &&
                  source.find_first_not_of(" \t", rhs + 1) != std::string::npos &&
                  source[source.find_first_not_of(" \t", rhs + 1)] == '|'));
            // A destructuring let binds through its pattern, so every name it
            // introduced has to be replayed, not just a single variable —
            // otherwise `let Just(x) = ...` stored x but later inputs could
            // not see it.
            std::vector<std::string> replayNames =
                patternLetNames.empty() ? std::vector<std::string>{letVarName}
                                        : patternLetNames;
            if (isLambda)
              localBinds += "  " + source + "\n";
            else
              for (const auto &name : replayNames) {
                // A `var` replays as `var`: replaying it as `let` would make
                // every later line see an immutable binding and reject both
                // `name = v` and `name.foo!(v)`.
                localBinds += std::string("  ") +
                              (isMutableLet ? "var " : "let ") + name +
                              " = Erlang.Erlang.get(:kexrepl" + name + ")\n";
                if (isMutableLet && !isMutableBind(name))
                  mutableBinds.push_back(name);
              }
            beamSemanticBinds.erase(
                std::remove_if(
                    beamSemanticBinds.begin(), beamSemanticBinds.end(),
                    [&](const auto &binding) {
                      return std::find(replayNames.begin(), replayNames.end(),
                                       binding.first) != replayNames.end();
                    }),
                beamSemanticBinds.end());
            beamSemanticBinds.emplace_back(replayNames.front(), source);
          }
        }
      } catch (const std::exception &e) {
        std::cerr << "  " << kex::color::apply(kex::color::red)
                  << "error:" << kex::color::apply(kex::color::reset) << " "
                  << e.what() << "\n";
      }
    }

#ifdef HAS_READLINE
    if (!historyFile.empty())
      write_history(historyFile.c_str());
#endif

    vm.close();
    std::filesystem::remove_all(beamDir);
    return 0;
  }

  if (mode == "repl") {
    kex::printReplBanner(std::cout, "");

#ifdef HAS_READLINE
    std::string historyFile;
    if (const char *home = std::getenv("HOME")) {
      std::filesystem::path histDir =
          std::filesystem::path(home) / ".config" / "kex";
      std::error_code ec;
      std::filesystem::create_directories(histDir, ec);
      historyFile = (histDir / "history").string();
      read_history(historyFile.c_str());
    }
#endif

    // SemanticDB for REPL: prelude loaded once, updated on each input.
    kex::semantic::SemanticDB replDb;
    if (!skipPrelude) {
      replDb.setImportedInterfaces(&preludeSemanticInterfaces());
      loadPrelude(replDb);
    }
#ifdef HAS_READLINE
    g_replDb = &replDb;
    rl_attempted_completion_function = kexCompletion;
    rl_completion_display_matches_hook = kexDisplayMatches;
    // Exclude '.' so "IO.pr<TAB>" is one token; exclude '"' so readline
    // never treats string literals as quoted words (which would cause it to
    // call a NULL dequoting function → segfault and add a stray close-quote).
    rl_completer_word_break_characters = (char *)" \t\n\\@$><=;|&{(";
    // No quote characters: our completions can contain '"' and we don't want
    // readline's quoting machinery to touch them.
    rl_completer_quote_characters = (char *)"";
    bindReplCancelKeys();
#endif

    kex::interpreter::Evaluator evaluator;
    evaluator.setReplMode(true);
    // A REPL session is the user experimenting with their own machine, and
    // there is no other code present for a mock to deceive — the reason
    // Mock.* is gated elsewhere (issue #144) does not exist here.
    evaluator.setMocksAllowed(true);
    // Without this the evaluator keeps its default relative {"lib", "src"}
    // roots, so `using Regex` (or any opt-in stdlib module) failed with
    // "Unknown module" in the interpreter REPL while working both in scripts
    // and in the BEAM REPL. There is no file being run, so resolution is
    // anchored at the working directory plus the standard library roots.
    evaluator.setModuleRoots(
        moduleRootsFor((std::filesystem::current_path() / "<repl>").string()));
    std::string line;
    // Accumulated source of all top-level definitions typed in the REPL so
    // far, used to keep the SemanticDB index complete across multiple inputs.
    std::string replAccumSource;
    // Local bindings are evaluated one input at a time, but their source is
    // retained here so semantic analysis can infer the type of later inputs.
    std::vector<std::pair<std::string, std::string>> replBindings;

    // Keep parsed programs alive so function closures can reference AST nodes
    std::vector<kex::ast::Program *> replPrograms;
    // A line read ahead while chaining function clauses that turned out
    // to belong to the next statement; replayed on the next iteration.
    std::optional<std::string> pendingLine;

    // If `s` looks like the start of a function clause definition
    // (`let name(...` or `foul name(...`), return the function name.
    auto clauseFuncName =
        [](const std::string &s) -> std::optional<std::string> {
      size_t offset;
      if (s.rfind("foul ", 0) == 0)
        offset = 5;
      else if (s.rfind("let ", 0) == 0)
        offset = 4;
      else
        return std::nullopt;

      size_t i = offset;
      while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_'))
        i++;
      if (i == offset)
        return std::nullopt;
      std::string name = s.substr(offset, i - offset);
      if (i < s.size() && s[i] == '?') {
        name += '?';
        i++;
      }
      if (i < s.size() && s[i] == '(')
        return name;
      return std::nullopt;
    };

    // REPL settings
    bool showTypes = true;
    bool showAst = false;
    bool showTokens = false;

    static const std::string setOptionsHelp =
        "  Options for /set / /unset:\n"
        "    types         Show type of each result\n"
        "    ast           Show AST for each input\n"
        "    tokens        Show token stream for each input\n";

    auto handleSet = [&](const std::string &arg, bool enable) {
      if (arg == "types") {
        showTypes = enable;
        std::cout << "  types: " << (enable ? "on" : "off") << "\n";
      } else if (arg == "ast") {
        showAst = enable;
        std::cout << "  ast: " << (enable ? "on" : "off") << "\n";
      } else if (arg == "tokens") {
        showTokens = enable;
        std::cout << "  tokens: " << (enable ? "on" : "off") << "\n";
      } else {
        std::cerr << "  Unknown option: " << arg << "\n";
        std::cerr << "  Available: types, ast, tokens\n";
      }
    };

    while (true) {
      std::string input;
      bool ok;
      if (pendingLine) {
        input = *pendingLine;
        pendingLine.reset();
        ok = true;
      } else {
        auto result = readLine("kex> ");
        input = result.first;
        ok = result.second;
      }
      if (!ok)
        break;
      input = replTrimLeadingIndent(std::move(input));
      line = input;
      if (kex::isReplExit(line))
        break;
      if (line.empty())
        continue;

      // REPL commands
      if (line == "/help" || line == "/h") {
        kex::printReplHelp(std::cout, setOptionsHelp);
        continue;
      }
      if (line == "/clear") {
        std::cout << kex::replClearScreenSequence() << std::flush;
        continue;
      }
      if (line == "/set") {
        std::cout << "  types:  " << (showTypes ? "on" : "off") << "\n"
                  << "  ast:    " << (showAst ? "on" : "off") << "\n"
                  << "  tokens: " << (showTokens ? "on" : "off") << "\n";
        continue;
      }
      if (line.substr(0, 5) == "/set ") {
        handleSet(line.substr(5), true);
        continue;
      }
      if (line.substr(0, 7) == "/unset ") {
        handleSet(line.substr(7), false);
        continue;
      }
      if (line.substr(0, 10) == "/complete ") {
        auto prefix = line.substr(10);
        auto results = replDb.completionsFor(prefix);
        if (results.empty()) {
          std::cout << "  (no completions for \"" << prefix << "\")\n";
        } else {
          for (const auto &c : results)
            std::cout << "  " << c << "\n";
        }
        continue;
      }
      if (line.substr(0, 6) == "/load ") {
        auto filePath = line.substr(6);
        size_t start = filePath.find_first_not_of(" \t");
        if (start != std::string::npos) filePath = filePath.substr(start);
        if (!fileExists(filePath)) {
          std::cerr << "  /load: file not found: " << filePath << "\n";
          continue;
        }
        try {
          auto src = readFile(filePath);
          kex::Lexer lex(std::move(src), filePath);
          kex::Parser parser(lex.tokenizeAll(), filePath);
          // Loaded definitions retain pointers into their parsed AST (function
          // bodies, make clauses, module members). Keep that Program alive for
          // the rest of the REPL session, just like definitions entered at the
          // prompt; a stack-local Program leaves those pointers dangling as
          // soon as /load returns.
          auto *prog = new kex::ast::Program(parser.parseProgram());
          throwOnParseErrors(parser);
          replPrograms.push_back(prog);
          evaluator.execute(*prog);
          replAccumSource += readFile(filePath) + "\n";
          replDb.updateFile("<repl>", replAccumSource);
          std::cout << "  loaded " << filePath << "\n";
        } catch (const std::exception& e) {
          std::cerr << "  /load error: " << e.what() << "\n";
        }
        continue;
      }
      if (line == "/reload") {
        std::cerr << "  /reload not yet implemented (use /reset then /load to rebuild)\n";
        continue;
      }

      // Multi-line: accumulate if there are unmatched do/end blocks
      std::string source = line;

      // Returns true if `s` starts with `make <TypeName>` but has no
      // `do` keyword on the same line (implicit-do block opener).
      auto isMakeWithoutDo = [](const std::string &s) -> bool {
        if (s.rfind("make ", 0) != 0)
          return false;
        // Check that the rest is a type name (no explicit `do`)
        auto doPos = s.find(" do");
        return doPos == std::string::npos;
      };

      auto countBlocks = [](const std::string &s) -> int {
        int count = 0;
        for (size_t i = 0; i < s.size(); i++) {
          if (i + 2 <= s.size() && s.substr(i, 2) == "do") {
            bool wordBefore = (i > 0 && std::isalnum(s[i - 1]));
            bool wordAfter = (i + 2 < s.size() && std::isalnum(s[i + 2]));
            if (!wordBefore && !wordAfter)
              count++;
          }
          if (i + 3 <= s.size() && s.substr(i, 3) == "end") {
            bool wordBefore = (i > 0 && std::isalnum(s[i - 1]));
            bool wordAfter = (i + 3 < s.size() && std::isalnum(s[i + 3]));
            if (!wordBefore && !wordAfter)
              count--;
          }
        }
        return count;
      };

      // `make TypeName` (no explicit `do`) implicitly opens a block.
      bool implicitDo = isMakeWithoutDo(source);
      int doCount = implicitDo ? 1 : countBlocks(source);

      // Track which make block we're inside so the completer can infer
      // parameter types from `@[x|xs]` patterns, etc.
      if (doCount > 0 && source.rfind("make ", 0) == 0) {
        std::string rest = source.substr(5);
        auto sp = rest.find_first_of(" \n\t");
        g_currentMakeTarget =
            (sp != std::string::npos) ? rest.substr(0, sp) : rest;
      }
      bool cancelled = false;
      while (doCount > 0 || replHasOpenDelimiter(source)) {
        auto [contLine, contOk] = readLine("...> ");
        if (takeReplInputCancelled()) {
          cancelled = true;
          break;
        }
        if (!contOk)
          break;
        line = contLine;
        source += "\n" + line;
        doCount = implicitDo ? 1 + countBlocks(source) : countBlocks(source);
      }
      if (cancelled) {
        std::cout << kex::color::apply(kex::color::gray) << "   (cancelled)"
                  << kex::color::apply(kex::color::reset) << "\n";
        continue;
      }
      // Clauses are not gathered by reading ahead — see the BEAM REPL above.
      // A complete line is accepted as soon as it is entered, and clauses of
      // one function accumulate where the definition is stored.

      // Show tokens if enabled
      if (showTokens) {
        kex::Lexer debugLexer(source);
        auto debugTokens = debugLexer.tokenizeAll();
        std::cout << "  tokens: ";
        for (const auto &t : debugTokens) {
          if (t.type == kex::TokenType::Eof ||
              t.type == kex::TokenType::Newline)
            continue;
          std::cout << kex::tokenTypeName(t.type);
          if (!t.value.empty())
            std::cout << "[" << t.value << "]";
          std::cout << " ";
        }
        std::cout << "\n";
      }

      auto execProgram =
          [&](kex::ast::Program *program) -> kex::interpreter::ValuePtr {
        if (showAst)
          printAst(*program);
        return evaluator.execute(*program);
      };

      auto showResult = [&](const kex::interpreter::ValuePtr &result,
                            const std::optional<std::string> &semanticType =
                                std::nullopt) {
        if (result && !std::holds_alternative<kex::interpreter::UnitValue>(
                          result->data)) {
          std::cout << kex::color::apply(kex::color::gray) << "=> "
                    << kex::color::apply(kex::color::reset)
                    << result->inspect();
          if (showTypes) {
            std::cout << " " << kex::color::apply(kex::color::gray) << ":"
                      << kex::color::apply(kex::color::reset) << " "
                      << kex::color::apply(kex::color::cyan)
                      << (semanticType ? *semanticType : result->typeName())
                      << kex::color::apply(kex::color::reset);
          }
          std::cout << "\n";
        }
      };

      // Detect if this is a top-level definition (not an expression)
      bool isFuncDef = false;
      size_t defOffset = std::string::npos;
      if (source.substr(0, 4) == "let ")
        defOffset = 4;
      else if (source.substr(0, 5) == "foul ")
        defOffset = 5;
      // A destructuring `let` binds the pattern's names; it is not a function
      // definition. An uppercase name is a constructor pattern
      // (`let Just(x) = ...`) and `(`/`[`/`{` open a tuple, list or record
      // one — without the openers, `let (a, b) = ...` matched the
      // `name(params)` shape below and was echoed as "defined". See the
      // matching guard in the BEAM branch above.
      const bool upperConstantDef =
          replIsUpperConstantBinding(source, defOffset);
      if (defOffset != std::string::npos && defOffset < source.size() &&
          !upperConstantDef &&
          (std::isupper((unsigned char)source[defOffset]) ||
           source[defOffset] == '(' || source[defOffset] == '[' ||
           source[defOffset] == '{'))
        defOffset = std::string::npos;
      if (upperConstantDef) {
        isFuncDef = true;
      } else if (defOffset != std::string::npos) {
        // It's a function def if: let/foul name( ... )  with parens before =
        auto parenPos = source.find('(', defOffset);
        auto eqPos = source.find('=', defOffset);
        auto doPos = source.find(" do", defOffset);
        isFuncDef = (parenPos != std::string::npos &&
                     (eqPos == std::string::npos || parenPos < eqPos) &&
                     (doPos == std::string::npos || parenPos < doPos));
      }
      if (source.substr(0, 7) == "module " || source.substr(0, 5) == "type " ||
          source.substr(0, 7) == "record " || source.substr(0, 5) == "make " ||
          source.substr(0, 12) == "foul module " ||
          source.substr(0, 6) == "using ") {
        isFuncDef = true;
      }

      try {
        if (isFuncDef) {
          // If the user wrote `make TypeName` without `do`, insert it
          // before parsing so the grammar's `expect(Do)` is satisfied.
          if (implicitDo) {
            auto nl = source.find('\n');
            if (nl != std::string::npos)
              source.insert(nl, " do");
            else
              source += " do";
          }
          // Parse as top-level definition
          kex::Lexer lexer(source);
          auto tokens = lexer.tokenizeAll();
          kex::Parser parser(std::move(tokens));
          auto *program = new kex::ast::Program(parser.parseProgram());
          throwOnParseErrors(parser);
          replPrograms.push_back(program);
          execProgram(program);
          // Accumulate and re-index so all prior definitions stay
          // visible for tab completion, not just the latest one.
          replAccumSource += source + "\n";
          replDb.updateFile("<repl>", replAccumSource);
          g_currentMakeTarget.clear(); // block is complete
          const bool isImport = source.rfind("using ", 0) == 0;
          std::cout << kex::color::apply(kex::color::gray) << "=> "
                    << kex::color::apply(kex::color::reset)
                    << (isImport ? "using " : "defined ")
                    << replDefinitionName(source) << "\n";
        } else {
          // Wrap in main for expression evaluation
          auto wrapped = "main do\n" + source + "\nend\n";
          kex::Lexer lexer(wrapped);
          auto tokens = lexer.tokenizeAll();
          kex::Parser parser(std::move(tokens));
          auto *program = new kex::ast::Program(parser.parseProgram());
          throwOnParseErrors(parser);
          replPrograms.push_back(program);

          // A REPL `let` may intentionally shadow a previous binding. Keep
          // its name so the semantic replay can replace, rather than
          // redeclare, the old source.
          std::optional<std::string> bindingName;
          std::vector<std::string> boundNames;
          for (const auto &item : program->items) {
            const auto *main =
                std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&item);
            if (!main || !*main || (*main)->body.empty())
              continue;
            const auto &lastKind = (*main)->body.back()->kind;
            // `var x = v` binds just as much as `let` does. The runtime value
            // lives in the evaluator's persistent REPL env either way; what
            // this record drives is the semantic replay below, and without it
            // the NEXT line's analysis reports `x` undefined.
            if (const auto *mutableBinding =
                    std::get_if<kex::ast::VarExpr>(&lastKind)) {
              if (!mutableBinding->name.empty()) {
                bindingName = mutableBinding->name;
                boundNames = {mutableBinding->name};
              }
              continue;
            }
            const auto *binding = std::get_if<kex::ast::LetExpr>(&lastKind);
            if (!binding || !binding->pattern)
              continue;
            std::vector<std::string> names;
            collectPatternNames(*binding->pattern, names);
            if (!names.empty()) {
              bindingName = names.front();
              boundNames = std::move(names);
            }
          }

          // Runtime values intentionally erase phantom typestate parameters.
          // Analyze a parallel source containing prior REPL bindings so the
          // displayed type is the source-level type rather than FileHandle.
          std::optional<std::string> semanticType;
          std::optional<std::string> semanticError;
          try {
            std::string bindingSource;
            for (const auto &[name, binding] : replBindings)
              if (!bindingName || name != *bindingName)
                bindingSource += binding + "\n";
            auto semanticSource = replAccumSource + "main do\n" +
                                  bindingSource + source + "\nend\n";
            kex::Lexer semanticLexer(semanticSource, "<repl>");
            kex::Parser semanticParser(semanticLexer.tokenizeAll(), "<repl>");
            auto semanticProgram = semanticParser.parseProgram();
            kex::semantic::Analyzer replAnalyzer(&preludeSemanticInterfaces());
            if (replAnalyzer.analyze(semanticProgram)) {
              for (auto item = semanticProgram.items.rbegin();
                   item != semanticProgram.items.rend(); ++item) {
                auto *main = std::get_if<
                    std::unique_ptr<kex::ast::MainBlock>>(&*item);
                if (!main || !*main || (*main)->body.empty())
                  continue;
                const auto *last = (*main)->body.back().get();
                const kex::ast::Expr *typedExpr = last;
                if (const auto *binding =
                        std::get_if<kex::ast::LetExpr>(&last->kind);
                    binding && binding->value)
                  typedExpr = binding->value.get();
                semanticType = displayTypeOf(replAnalyzer, typedExpr);
                break;
              }
            } else {
              for (const auto &diagnostic : replAnalyzer.diagnostics()) {
                if (diagnostic.level !=
                    kex::semantic::Diagnostic::Level::Error)
                  continue;
                semanticError = diagnostic.message;
                break;
              }
            }
          } catch (...) {
            // Evaluation still works if an incomplete semantic history (for
            // example a loaded file with a main block) cannot be re-parsed.
          }

          if (semanticError)
            throw std::runtime_error(*semanticError);

          auto result = execProgram(program);
          showResult(result, semanticType);
          if (bindingName) {
            replBindings.erase(
                std::remove_if(
                    replBindings.begin(), replBindings.end(),
                    [&](const auto &binding) {
                      return std::find(boundNames.begin(), boundNames.end(),
                                       binding.first) != boundNames.end();
                    }),
                replBindings.end());
            replBindings.emplace_back(*bindingName, source);
          }
        }
      } catch (const std::exception &e) {
        std::cerr << "  " << kex::color::apply(kex::color::red)
                  << "error:" << kex::color::apply(kex::color::reset) << " "
                  << e.what() << "\n";
      }
    }
#ifdef HAS_READLINE
    if (!historyFile.empty())
      write_history(historyFile.c_str());
#endif
    return 0;
  }

  std::string filepath = argv[optind];

  // `kex file.kx.beam [args]` or `kex file.beam [args]` — run a compiled BEAM
  // module.
  if (filepath.size() > 5 &&
      filepath.compare(filepath.size() - 5, 5, ".beam") == 0) {
    std::vector<std::string> beamArgs;
    for (int i = optind + 1; i < argc; i++)
      beamArgs.push_back(argv[i]);

    namespace fs = std::filesystem;
    std::string absBeamPath = fs::weakly_canonical(filepath).string();
    std::string absBeamDir = fs::path(absBeamPath).parent_path().string();
    std::string modFile = fs::path(absBeamPath).filename().string();

    // Derive module name from filename convention:
    //   <stem>.kx.beam → kex_<stem>   (our compiled output)
    //   kex_<x>.beam   → kex_<x>      (backward compat)
    //   anything.beam  → anything      (external module)
    std::string moduleName;
    if (modFile.size() > 8 &&
        modFile.compare(modFile.size() - 8, 8, ".kx.beam") == 0) {
      moduleName = "kex_" + modFile.substr(0, modFile.size() - 8);
    } else {
      moduleName = modFile.substr(0, modFile.size() - 5);
    }

    // Put explicitly built Kex runtime beams on the code path.
    std::string rtBeamDir = prebuiltRuntimeBeamDir();
    if (rtBeamDir.empty()) {
      std::cerr << "error: prebuilt runtime artifacts are missing; "
                   "rebuild or reinstall the Kex toolchain\n";
      return 1;
    }

    // Load explicitly — code:load_abs rejects when filename != module name,
    // so use code:load_binary which skips that check.
    // Force UTF-8 on the node's I/O instead of inheriting it from the locale.
    // An `erl` started under a non-UTF-8 LANG (the Linux CI runner) writes
    // `\x{2717}` for `✗` and mangles every non-ASCII byte, so a Kex program's
    // output depended on the environment: identical source printed `[h, é, é]`
    // here and `[h, \ufffd, \ufffd]` there.
    // A prebuilt .beam carries no evidence of being a spec — the file name it
    // was compiled from is gone — so mocks here are strictly opt-in
    // (issue #144).
    std::string mainCall =
        beamOtpFloorGuard() +
        "io:setopts(standard_io, [{encoding, unicode}]), "
        "io:setopts(standard_error, [{encoding, unicode}]), " +
        (allowMocks ? "kex_test:allow_mocks(), " : "") +
        beamTestGate(testReportMode, testFilters) +
        "try {ok,_Bin}=file:read_file(\"" + absBeamPath +
        "\"), "
        "code:load_binary('" +
        moduleName + "',\"" + absBeamPath +
        "\",_Bin), "
        "case lists:member({main,1},erlang:get_module_info('" +
        moduleName +
        "',exports)) of "
        "true -> '" +
        moduleName +
        "':main([unicode:characters_to_binary(A) || A <- "
        "init:get_plain_arguments()]); "
        "false -> '" +
        moduleName +
        "':main() end of "
        "Result -> halt() "
        "catch _:Reason:_ -> io:format(standard_error, \"Internal error: "
        "runtime error: ~p~n\", [Reason]), halt(1) end";
    // Erlang prepends each -pa directory, so add the toolchain first and the
    // compiled unit last. A source module intentionally shadows a stdlib
    // module with the same public name (for example a user-defined `Math`).
    // `+Bi` is the other half of forwardSignalToChild's SIGINT→SIGTERM
    // substitution, and neither works alone. A terminal's Ctrl+C signals the
    // whole process group, so the VM takes a SIGINT DIRECTLY as well as this
    // launcher's forward. Without `+Bi` that direct one drives the VM into its
    // break handler, where the forwarded SIGTERM no longer lands and nothing
    // exits at all; with `+Bi` the VM ignores SIGINT entirely and the SIGTERM
    // does the work — `erl_signal_server` runs, `kex_child_guard` kills the
    // port's OS child, and the run ends clean.
    //
    // Deliberately NOT applied to the REPL's persistent VM (the `BeamVm` block
    // above), where Ctrl+C should interrupt evaluation rather than end the
    // session.
    std::string runCmd = erlExecutable() + " +Bi -noshell";
    if (!rtBeamDir.empty())
      runCmd += " -pa " + rtBeamDir;
    runCmd += " -pa " + absBeamDir;
    // shellSingleQuote (see its own comment) wraps the whole -eval
    // text as one shell argument, so quote characters embedded in it
    // (e.g. around a module name with a literal '.' in its stem)
    // survive into erl correctly regardless of where they land.
    runCmd += " -eval " + shellSingleQuote(mainCall);
    if (!beamArgs.empty()) {
      runCmd += " -extra";
      for (const auto &a : beamArgs)
        runCmd += " " + a;
    }
    int rc = runShellCommand(runCmd);
    return exitStatusOf(rc);
  }

  // Reject non-.kex files before trying to parse them.
  if (filepath.size() < 4 ||
      filepath.compare(filepath.size() - 4, 4, ".kex") != 0) {
    std::cerr << "error: " << filepath << ": expected a .kex source file\n";
    return 1;
  }

  auto source = readFile(filepath);
  if (source.empty())
    return 1;
  auto astDocs = kex::ast::extractDocComments(source);

  // Honour `# kex: no-check` pragma in the first few lines — any file that
  // contains it is treated as if --no-check was passed on the command line.
  {
    std::istringstream ss(source);
    std::string line;
    for (int ln = 0; ln < 10 && std::getline(ss, line); ++ln) {
      if (line.find("# kex: no-check") != std::string::npos) {
        skipCheck = true;
        break;
      }
    }
  }

  // Everything after the script path is the script's own argument list,
  // exposed to Kex code via `main(args) do ... end`.
  std::vector<std::string> scriptArgs;
  for (int i = optind + 1; i < argc; i++) {
    scriptArgs.push_back(argv[i]);
  }

  kex::Lexer lexer(std::move(source), filepath);
  auto tokens = lexer.tokenizeAll();

  if (mode == "lex") {
    for (const auto &token : tokens) {
      std::cout << token.location.line << ":" << token.location.column << "  "
                << kex::tokenTypeName(token.type);
      if (!token.value.empty()) {
        std::cout << "  [" << token.value << "]";
      }
      std::cout << "\n";
    }
    return 0;
  }

  try {
    kex::Parser parser(std::move(tokens), filepath);
    auto program = parser.parseProgram();

    // In --run and --compile/-R modes, print parse errors and abort —
    // SemanticDB is not invoked yet at this point so we're the only
    // place that can report them cleanly. Without this, --compile/-R
    // fell through to runSemanticCheck, which re-parses via SemanticDB
    // and reports the exact same underlying parse error but mislabeled
    // as "fix type errors" — confusing for something that's actually a
    // syntax error (see spec/error_if_with_do.kex, a real regression
    // case). In --check mode the SemanticDB re-parses and reports them
    // itself; printing here would duplicate every message.
    if (!parser.diagnostics().empty() &&
        (mode == "run" || mode == "compile" || mode == "emit-ast" ||
         mode == "parse" || mode == "expand" || mode == "emit-core")) {
      if (mode == "emit-ast") {
        const auto &diagnostic = parser.diagnostics().front();
        const auto logicalFilename =
            astFilename.empty() ? filepath : astFilename;
        kex::beam::TermBuilder builder;
        auto location = builder.record("Location", {
            {"file", builder.string(logicalFilename)},
            {"line", builder.integer(int64_t{diagnostic.location.line})},
            {"column", builder.integer(int64_t{diagnostic.location.column})},
            {"startOffset", builder.integer(
                int64_t{diagnostic.location.startOffset})},
            {"endOffset", builder.integer(
                int64_t{diagnostic.location.endOffset})},
        });
        auto error = builder.record("ParseError", {
            {"message", builder.string(diagnostic.message)},
            {"location", builder.just(std::move(location))},
        });
        auto result = builder.variant("Error", "Result", {std::move(error)});
        auto bytes = kex::beam::encodeEtf(result);
        std::cout.write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
        return 0;
      }
      for (const auto &pd : parser.diagnostics()) {
        std::cerr << kex::color::apply(kex::color::gray) << pd.location.file
                  << ":" << pd.location.line << ":" << pd.location.column << ":"
                  << kex::color::apply(kex::color::reset) << " "
                  << kex::color::apply(kex::color::bold)
                  << kex::color::apply(kex::color::red)
                  << "error:" << kex::color::apply(kex::color::reset) << " "
                  << colorizeMessage(pd.message) << "\n";
      }
      std::cerr << kex::color::apply(kex::color::bold)
                << kex::color::apply(kex::color::magenta)
                << "Aborted:" << kex::color::apply(kex::color::reset) << " "
                << errorCountPhrase(static_cast<int>(parser.diagnostics().size()),
                                    "syntax")
                << " — fix before "
                << (mode == "run" || compileRun
                        ? "running"
                        : (mode == "parse" || mode == "expand"
                               ? "parsing"
                               : "compiling"))
                << ".\n";
      return 1;
    }

    if (mode == "parse") {
      printAst(program);
      return 0;
    }

    if (mode == "emit-ast") {
      kex::beam::TermBuilder builder;
      kex::ast::Converter converter(builder);
      auto bytes = kex::beam::encodeEtf(
          converter.program(program,
                            astFilename.empty() ? filepath : astFilename,
                            astDocs));
      if (outputDirExplicit) {
        std::ofstream output(outputDir, std::ios::binary);
        if (!output) {
          std::cerr << "error: cannot open AST output file: " << outputDir
                    << "\n";
          return 1;
        }
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
      } else {
        std::cout.write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
      }
      return 0;
    }

    // Compile-time expansion of `compiled do` blocks. Deliberately the ONE
    // place every mode passes through, right after parsing and before any
    // semantic pass — so --check, --run, --compile, --emit-core and -R can
    // never disagree about what the program contains.
    //
    // Note for when this stops being an identity pass: the BEAM path also
    // merges a `.spec.kex` base file's declarations into program.items further
    // down, and resolveBeamDeps pulls in dependency modules. Those items are
    // parsed AFTER this point, so their own `compiled` blocks would not be
    // expanded here — they need either their own expand() call at load time or
    // a second pass once the program is fully assembled.
    auto expandParsedProgram = [&](kex::ast::Program &target,
                                   bool reportCollapse) -> bool {
      std::vector<kex::semantic::Diagnostic> expandDiagnostics;
      std::vector<kex::compiled::CollapseNote> collapseNotes;
      kex::compiled::ExpandOptions expandOptions;
      if (reportCollapse) expandOptions.report = &collapseNotes;
      const bool expanded =
          kex::compiled::expand(target, expandDiagnostics, expandOptions);
      if (reportCollapse) printCollapseReport(collapseNotes);
      for (const auto &diagnostic : expandDiagnostics)
        printSemanticDiagnostic(diagnostic);
      if (!expanded) {
        std::cerr << kex::color::apply(kex::color::bold)
                  << kex::color::apply(kex::color::magenta)
                  << "Aborted:" << kex::color::apply(kex::color::reset)
                  << " compile-time expansion failed.\n";
        return false;
      }
      return true;
    };
    if (!expandParsedProgram(program, collapseReport)) return 1;

    if (mode == "expand") {
      printAst(program);
      return 0;
    }

  std::unique_ptr<kex::semantic::Analyzer> compileAnalysis;
  std::vector<LoadedDep> beamDeps;
  // The `<name>.spec.kex` base actually merged in below, kept so the gating
  // semantic check can pass it to SemanticDB as a companion — see its use at
  // the `runSemanticCheck` call.
  std::string beamSpecBaseFile;
  if (mode == "compile" || mode == "emit-core") {
      // For `-R file.kex` without explicit `-o`, use a temp dir and clean up
      // after.
      std::string tempDir;
      if (compileRun && !outputDirExplicit) {
        char tmpl[] = "/tmp/kex_XXXXXX";
        char *td = mkdtemp(tmpl);
        if (!td) {
          std::cerr << "error: mkdtemp failed\n";
          return 1;
        }
        tempDir = td;
        outputDir = tempDir;
      }

      // Derive module stem from filename (e.g. "hello" from "hello.kex")
      std::string stem = filepath;
      auto slash = stem.rfind('/');
      if (slash != std::string::npos)
        stem = stem.substr(slash + 1);
      auto dot = stem.rfind('.');
      if (dot != std::string::npos)
        stem = stem.substr(0, dot);

      // `<name>.spec.kex` auto-loads `<name>.kex`'s declarations —
      // see specBaseCandidates's own doc comment. `mode == "run"`
      // (the tree-walker) already does this via a separate
      // Evaluator::execute call below; the BEAM codegen path emits
      // everything as ONE Core Erlang module from a single Program,
      // so the equivalent here is prepending the base file's
      // declarations (everything but its own `main` block) directly
      // into `program.items` before emitting — a real, reproduced
      // gap otherwise: spec/json_parser.spec.kex's `Parser.parse`
      // (declared in examples/json_parser.kex) compiled to a bare
      // `undef` under BEAM, since nothing here ever loaded that
      // file's declarations at all.
      for (const auto &candidate : specBaseCandidates(filepath)) {
        if (!fileExists(candidate))
          continue;
        beamSpecBaseFile = candidate;

        auto baseSource = readFile(candidate);
        kex::Lexer baseLexer(std::move(baseSource), candidate);
        auto baseTokens = baseLexer.tokenizeAll();
        kex::Parser baseParser(std::move(baseTokens), candidate);
        auto baseProgram = baseParser.parseProgram();

        std::vector<kex::ast::TopLevelItem> merged;
        merged.reserve(baseProgram.items.size() + program.items.size());
        for (auto &item : baseProgram.items)
          if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(
                  item))
            merged.push_back(std::move(item));
        for (auto &item : program.items)
          merged.push_back(std::move(item));
        program.items = std::move(merged);
        break;
      }

      // Cross-file dependency resolution: walk `using` statements,
      // and metadata-resolved qualified module references, parse their source
      // files, and merge them into the program so IR lowering sees every
      // definition without making qualified members lexically imported.
      {
        kex::semantic::Analyzer dependencyAnalysis(
            &preludeSemanticInterfaces());
        (void)dependencyAnalysis.analyze(program);
        auto qualifiedModules =
            dependencyAnalysis.referencedModules();
        for (auto it = qualifiedModules.begin();
             it != qualifiedModules.end();) {
          const auto imported =
              preludeSemanticInterfaces().modules.find(*it);
          if (imported !=
                  preludeSemanticInterfaces().modules.end() &&
              imported->second.automaticImport)
            it = qualifiedModules.erase(it);
          else
            ++it;
        }
        beamDeps = resolveBeamDeps(
            program, moduleRootsFor(filepath),
            &preludeSemanticInterfaces(),
            qualifiedModules);
      }

      // An opt-in module merged in above may claim a prelude trait
      // (`implement: Enumerable`) purely through inherited defaults, with no
      // local override — lowering needs that trait's default-method bodies,
      // which live in the prelude's own source, not in anything just merged
      // in (kexhq/kex#235). See backfillExternalTraitDefaults's doc comment.
      kex::backfillExternalTraitDefaults(program);

      // Base files and dependency modules are parsed after the initial source
      // expansion. Expand the fully assembled compilation unit as well; the
      // pass is idempotent because an expanded block is removed.
      if (!expandParsedProgram(program, false)) return 1;

      // Type-check the same dependency-expanded program that lowering will
      // compile.  In particular, `using Units.SI` must make its ordinary
      // functions visible before names such as `times` are resolved.
      if (mode == "compile" || mode == "emit-core") {
        compileAnalysis = std::make_unique<kex::semantic::Analyzer>(
            &preludeSemanticInterfaces());
        // `--no-check` suppresses diagnostics and lets invalid programs
        // continue, but overload resolution still has to run: lowering needs
        // the exact selected target even when errors are non-gating.
        const bool gatingAnalysis = mode == "compile" && !skipCheck;
        int typeErrors = 0;
        // `beamSpecBaseFile` matters here for the same reason it does on the
        // walker path: the Analyzer sees the base file's declarations because
        // they were merged into `program`, but SemanticDB re-reads `filepath`
        // from disk and would report every constructor defined in the base as
        // an undefined name. Passing "" made `-c`/`-R` reject
        // spec/json_parser.spec.kex with 21 phantom errors while `--emit-core`
        // — which skips the gating check — compiled it fine.
        const bool analysisOk = gatingAnalysis
            ? runSemanticCheck(program, filepath, compileAnalysis.get(),
                               beamSpecBaseFile, &typeErrors)
            : compileAnalysis->analyze(program);
        if (gatingAnalysis && !analysisOk) {
          std::cerr << kex::color::apply(kex::color::bold)
                    << kex::color::apply(kex::color::magenta)
                    << "Aborted:" << kex::color::apply(kex::color::reset) << " "
                    << errorCountPhrase(typeErrors, "type") << " — fix before "
                    << (compileRun ? "running" : "compiling")
                    << " (use --no-check to skip).\n";
          return 1;
        }
      }

      // An explicit `main do ... end` is not required: IR lowering
      // synthesizes one from trailing bare top-level expressions, matching
      // the tree-walker's implicit top-level execution behavior.
      // Prelude record layouts come from the embedded compiled interface, not
      // from reparsing stdlib source or injecting declarations into the user AST.
      std::vector<kex::ir::ExternalRecordLayout> preludeRecordLayouts;
      try {
        preludeRecordLayouts = loadPreludeRecordLayouts();
      } catch (const std::exception &e) {
        std::cerr << "error: invalid prebuilt standard library: " << e.what()
                  << "\n";
        if (compileRun && !outputDirExplicit && !tempDir.empty())
          std::filesystem::remove_all(tempDir);
        return 1;
      }
      kex::ir::EmitResult result;
      std::vector<kex::ir::EmitResult> moduleResults;
      try {
        auto preludeVariantTags = loadPreludeVariantTags();
        auto irModules = kex::ir::lowerModules(program, stem,
                                               filepath,
                                               &preludeRecordLayouts,
                                               &preludeExternalModules(),
                                               compileAnalysis
                                                   ? &compileAnalysis->resolvedCalls()
                                                   : nullptr,
                                               /*preferExternalReceivers=*/false,
                                               &preludeVariantTags,
                                               compileAnalysis
                                                   ? &compileAnalysis->staticTypeOfCalls()
                                                   : nullptr,
                                               compileAnalysis
                                                   ? &compileAnalysis->typeMap()
                                                   : nullptr);
        for (const auto &irMod : irModules)
          moduleResults.push_back(kex::ir::emitCore(irMod));
        result = moduleResults.front();
      } catch (const kex::ir::LowerError &e) {
        std::cerr << "error: " << e.what() << "\n";
        if (compileRun && !outputDirExplicit && !tempDir.empty())
          std::filesystem::remove_all(tempDir);
        return 1;
      }

      std::vector<std::string> corePaths;
      for (const auto &emitted : moduleResults) {
        std::string path = outputDir + "/" + emitted.moduleName + ".core";
        std::ofstream outFile(path);
        if (!outFile) {
          std::cerr << "error: cannot write " << path << "\n";
          return 1;
        }
        outFile << emitted.source;
        corePaths.push_back(std::move(path));
      }
      const std::string &outPath = corePaths.front();
      if (mode == "emit-core") {
        for (const auto &path : corePaths) std::cerr << "wrote " << path << "\n";
        return 0;
      }

      // --compile: also invoke erlc to produce a .beam file.
      // Place explicitly built Kex runtime beams into the output directory.
      {
        namespace fs = std::filesystem;
        std::string prebuilt = prebuiltRuntimeBeamDir();
        if (prebuilt.empty()) {
          std::cerr << "error: prebuilt runtime artifacts are missing; "
                       "rebuild or reinstall the Kex toolchain\n";
          if (compileRun && !outputDirExplicit && !tempDir.empty())
            fs::remove_all(tempDir);
          return 1;
        }
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(prebuilt))
          if (e.path().extension() == ".beam")
            fs::copy_file(e.path(), fs::path{outputDir} / e.path().filename(),
                          fs::copy_options::overwrite_existing, ec);
      }

      // User compilation consumes the explicitly built stdlib artifact. A
      // missing installed/development artifact is a toolchain error; never
      // rebuild stdlib source as an implicit side effect of compiling a user
      // program.
      if (!skipPrelude &&
          !std::filesystem::exists(std::filesystem::path{outputDir} /
                                   "kex_prelude.beam")) {
        std::cerr << "error: prebuilt standard library is missing; "
                     "rebuild or reinstall the Kex toolchain\n";
        if (compileRun && !outputDirExplicit && !tempDir.empty())
          std::filesystem::remove_all(tempDir);
        return 1;
      }

      int erlcRet = 0;
      for (size_t moduleIndex = 0; moduleIndex < corePaths.size(); ++moduleIndex) {
        // -W0: these warnings analyze the generated Core Erlang — locationless
        // (`no_file:`) compiler-analysis noise the emitter's dead statement-
        // value lets make inevitable ("a term is constructed, but never used",
        // "this clause cannot match"). They cannot point back at Kex source,
        // and the interpreter path below has always silenced them for exactly
        // that reason. Real erlc failures still fail the exit-code check and
        // are re-run loudly below.
        std::string coreCmd = erlcExecutable() + " +from_core -W0 -pa " +
                              outputDir + " -o " + outputDir + " " +
                              corePaths[moduleIndex];
        if (!tempDir.empty()) {
        // Suppress erlc noise in temp-dir (interpreter/-R) mode —
        // was `2>&1` (merging stderr into stdout), the OPPOSITE of
        // what this comment always said it should do. erlc prints
        // its own warnings (e.g. "this clause cannot match because
        // a previous clause always matches" — confirmed harmless
        // compiler-analysis noise, not a real logic bug: reproduced
        // on spec/functions.kex, whose actual computed values are
        // all correct) to its OWN STDOUT, not stderr — so both
        // streams need silencing, not just stderr, to keep them out
        // of the running program's own visible output. erlc
        // failures are still caught via the exit-code check below
        // regardless of where its diagnostic text went.
          coreCmd += " > /dev/null 2>&1";
        } else {
          std::cerr << "  Compile: " << moduleResults[moduleIndex].moduleName << "\n";
        }
        erlcRet = runShellCommand(coreCmd);
        if (erlcRet != 0) {
          // The quiet mode above swallows erlc's own diagnostic — which is the
          // only thing that says WHY. Re-run it loudly on failure so the
          // message reaches the terminal (and CI) instead of a bare
          // "erlc failed", which is all a failing spec could report.
          if (!tempDir.empty())
            runShellCommand(erlcExecutable() + " +from_core -pa " + outputDir +
                            " -o " + outputDir + " " + corePaths[moduleIndex]);
          std::cerr << "error: erlc failed\n";
          if (!tempDir.empty()) std::filesystem::remove_all(tempDir);
          return 1;
        }

        // Attach KexI chunk to the freshly compiled .beam file.
        if (!compileRun) {
          std::string beamPath = outputDir + "/" +
                                 moduleResults[moduleIndex].moduleName + ".beam";
          try {
            kex::beam::CollectOptions copts;
            copts.unitId = moduleResults[0].moduleName;
            copts.moduleAtom = moduleResults[moduleIndex].moduleName;
            copts.fileStem = stem;
            copts.noCheck = skipCheck;
            copts.analysis = compileAnalysis.get();
            copts.role = moduleIndex == 0
                ? kex::beam::KexiModuleRole::Entry
                : kex::beam::KexiModuleRole::Companion;
            if (copts.role == kex::beam::KexiModuleRole::Companion) {
              copts.entryBackPointer = moduleResults[0].moduleName;
              auto irName = moduleResults[moduleIndex].moduleName;
              if (irName.rfind("Kex.", 0) == 0)
                copts.moduleName = irName.substr(4);
            }
            auto chunk = kex::beam::collectMetadata(program, copts);
            chunk.intrinsicAbiVersion = kex::kIntrinsicAbiVersion;
            chunk.backendRepresentationVersion =
                kex::kBeamRepresentationVersion;
            if (moduleIndex == 0 && moduleResults.size() > 1) {
              for (size_t ci = 1; ci < moduleResults.size(); ci++) {
                kex::beam::KexiCompanion comp;
                comp.beamAtom = moduleResults[ci].moduleName;
                comp.relativePath = moduleResults[ci].moduleName + ".beam";
                chunk.metadata.companions.push_back(std::move(comp));
              }
            }
            chunk.interfaceHash = kex::beam::computeInterfaceHash(chunk);
            auto bf = kex::beam::readBeamFile(beamPath);
            chunk.artifactHash = kex::beam::computeArtifactHash(bf);
            auto payload = kex::beam::serializeKexi(chunk);
            bf.setChunk(kex::beam::KEXI_CHUNK_ID, std::move(payload));
            kex::beam::writeBeamFile(bf, beamPath);
          } catch (const std::exception& e) {
            std::cerr << "warning: could not attach KexI chunk to "
                      << beamPath << ": " << e.what() << "\n";
          }
        }
      }

      // Second pass: backfill companion hashes into the entry module's
      // KexI companion manifest now that all companions have been written.
      if (!compileRun && moduleResults.size() > 1) {
        try {
          std::string entryBeam = outputDir + "/" +
                                  moduleResults[0].moduleName + ".beam";
          auto entryBf = kex::beam::readBeamFile(entryBeam);
          auto* kexiChk = entryBf.findChunk(kex::beam::KEXI_CHUNK_ID);
          if (kexiChk) {
            auto entryChunk = kex::beam::deserializeKexi(kexiChk->data);
            for (auto& comp : entryChunk.metadata.companions) {
              std::string compBeam = outputDir + "/" + comp.beamAtom + ".beam";
              auto compBf = kex::beam::readBeamFile(compBeam);
              auto* compChk = compBf.findChunk(kex::beam::KEXI_CHUNK_ID);
              if (compChk) {
                auto compChunk = kex::beam::deserializeKexi(compChk->data);
                comp.expectedHash = compChunk.interfaceHash;
              }
            }
            entryChunk.interfaceHash =
                kex::beam::computeInterfaceHash(entryChunk);
            auto payload = kex::beam::serializeKexi(entryChunk);
            entryBf.setChunk(kex::beam::KEXI_CHUNK_ID, std::move(payload));
            kex::beam::writeBeamFile(entryBf, entryBeam);
          }
        } catch (const std::exception& e) {
          std::cerr << "warning: could not backfill companion hashes: "
                    << e.what() << "\n";
        }
      }

      // The emitted file is named after the module inside it. Kex used to
      // rename `kex_<stem>.beam` to `<stem>.kx.beam` as a friendlier name,
      // but BEAM's code server finds a module by FILENAME: a directory of
      // `.kx.beam` files could not be put on a code path, so anything
      // booting a built program straight from the emulator (`erl -pa ebin`,
      // a package launcher) needed a renamed copy alongside it.
      std::string kxBeam = outputDir + "/" + result.moduleName + ".beam";
      if (tempDir.empty()) std::cerr << "  done  " << kxBeam << "\n";

      if (compileRun) {
        // Load every freshly emitted module explicitly.  Relying on the code
        // path here lets BEAM autoload a same-named module from an earlier
        // compilation, which is particularly easy to hit for imported
        // standard-library modules such as Kex.Units.Data.
        namespace fs = std::filesystem;
        std::string loadExpr;
        for (size_t moduleIndex = 0; moduleIndex < moduleResults.size();
             ++moduleIndex) {
          const auto &emitted = moduleResults[moduleIndex];
          std::string beam = outputDir + "/" + emitted.moduleName + ".beam";
          std::string absBeam = fs::weakly_canonical(beam).string();
          std::string binaryVar = "_B" + std::to_string(moduleIndex);
          loadExpr += "{ok," + binaryVar + "}=file:read_file(\"" +
                      absBeam + "\"), "
                      "code:load_binary('" + emitted.moduleName + "',\"" +
                      absBeam + "\"," + binaryVar + "), ";
        }
        // Kex runtime errors carry a String (binary/charlist) reason
        // printed verbatim; anything else (raw BEAM errors like
        // badarg tuples) falls back to ~p.
        std::string reasonFmt =
            "case Reason of _R when is_binary(_R); is_list(_R) -> "
            "io:format(standard_error, \"Internal error: ~ts~n\", [_R]); "
            "_R -> io:format(standard_error, \"Internal error: ~p~n\", [_R]) "
            "end";
        // Same UTF-8 pinning as the `.beam` runner above.
        const std::string utf8Io =
            "io:setopts(standard_io, [{encoding, unicode}]), "
            "io:setopts(standard_error, [{encoding, unicode}]), ";
        // Mock.* gate, BEAM side (issue #144). The grant is made by the
        // RUNNER, not baked into the emitted module: the same .beam handed to
        // `erl -pa ebin` — or to `kex prog.beam` — starts with mocks denied,
        // which is what makes a compiled program un-hijackable. main/0 runs in
        // this process, so the process-dictionary flag covers exactly it.
        const std::string mockGate =
            ((allowMocks || isSpecEntry(filepath)) ? "kex_test:allow_mocks(), "
                                                   : "") +
            beamTestGate(testReportMode, testFilters);
        std::string mainCall =
            result.mainArity == 1
                ? beamOtpFloorGuard() + utf8Io + mockGate + "try " + loadExpr + "'" +
                      result.moduleName +
                      "':main([unicode:characters_to_binary(A) || A <- "
                      "init:get_plain_arguments()]) of Result -> halt() catch "
                      "_:Reason:_ -> " +
                      reasonFmt + ", halt(1) end"
                : beamOtpFloorGuard() + utf8Io + mockGate + "try " + loadExpr + "'" +
                      result.moduleName +
                      "':main() of Result -> halt() catch _:Reason:_ -> " +
                      reasonFmt + ", halt(1) end";
        // shellSingleQuote (see its own comment) wraps the whole
        // -eval text as one shell argument, so quote characters
        // embedded in it survive into erl correctly regardless of
        // where they land (spec/json_parser.spec.kex).
        std::string runCmd =
            // `+Bi` for the same reason as the other run path above: it is
            // half of the Ctrl+C fix, with forwardSignalToChild's
            // SIGINT→SIGTERM the other half.
            erlExecutable() + " +Bi -noshell -pa " + outputDir + " -eval " +
            shellSingleQuote(mainCall);
        if (result.mainArity == 1 && !scriptArgs.empty()) {
          runCmd += " -extra";
          for (const auto &a : scriptArgs)
            runCmd += " " + a;
        }
        int ret = runShellCommand(runCmd);
        if (!tempDir.empty())
          std::filesystem::remove_all(tempDir);
        return exitStatusOf(ret);
      }
      return 0;
    }

    // `<name>.spec.kex` auto-loads `<name>.kex`'s declarations — merge them
    // into `program` BEFORE the semantic check, exactly as the BEAM path
    // above does. Loading them only into the Evaluator (as this path used to)
    // leaves the checker blind to the base file's types: spec/
    // json_parser.spec.kex's `JsonNull`/`JsonBool` come from examples/
    // json_parser.kex's `type Json`, and the checker rejected every one of
    // them as an undefined constructor before the program ever ran.
    std::string specBaseFile;
    if (mode == "run") {
      for (const auto &candidate : specBaseCandidates(filepath)) {
        if (!fileExists(candidate))
          continue;
        specBaseFile = candidate;

        auto baseSource = readFile(candidate);
        kex::Lexer baseLexer(std::move(baseSource), candidate);
        auto baseTokens = baseLexer.tokenizeAll();
        kex::Parser baseParser(std::move(baseTokens), candidate);
        auto baseProgram = baseParser.parseProgram();

        std::vector<kex::ast::TopLevelItem> merged;
        merged.reserve(baseProgram.items.size() + program.items.size());
        for (auto &item : baseProgram.items)
          if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(
                  item))
            merged.push_back(std::move(item));
        for (auto &item : program.items)
          merged.push_back(std::move(item));
        program.items = std::move(merged);
        break;
      }

      // Cross-file dependency resolution: walk `using` statements and
      // qualified module references, parse their source files, and merge
      // them into `program` so the checker AND the evaluator see every
      // declaration a project-local module makes — mirrors what `mode ==
      // "compile"` below already does for BEAM (see its own comment).
      // Without this, a call into a module declared in a different file
      // took the checker's lenient fallback instead of being verified
      // against that function's real signature: a qualified call typechecked
      // with any argument shape and only failed at runtime, and a bare call
      // typechecked only by accident, breaking outright the moment a
      // same-named prelude function also existed (docs/known-gaps.md,
      // "Cross-file `using` imports are barely type-checked").
      {
        kex::semantic::Analyzer dependencyAnalysis(&preludeSemanticInterfaces());
        (void)dependencyAnalysis.analyze(program);
        auto qualifiedModules = dependencyAnalysis.referencedModules();
        for (auto it = qualifiedModules.begin(); it != qualifiedModules.end();) {
          const auto imported = preludeSemanticInterfaces().modules.find(*it);
          if (imported != preludeSemanticInterfaces().modules.end() &&
              imported->second.automaticImport)
            it = qualifiedModules.erase(it);
          else
            ++it;
        }
        resolveBeamDeps(program, moduleRootsFor(filepath),
                        &preludeSemanticInterfaces(), qualifiedModules);
      }

      if (!expandParsedProgram(program, false)) return 1;
    }

    // Retained so the evaluator can use what the checker learned — today the
    // `Type.of(x)` sites it typed concretely. Declared out here so it outlives
    // the evaluator that borrows from it.
    kex::semantic::Analyzer runAnalyzer(&preludeSemanticInterfaces());
    bool runAnalyzed = false;
    if (mode == "run" && !skipCheck) {
      int typeErrors = 0;
      runAnalyzed = true;
      if (!runSemanticCheck(program, filepath, &runAnalyzer, specBaseFile,
                            &typeErrors)) {
        std::cerr
            << kex::color::apply(kex::color::bold)
            << kex::color::apply(kex::color::magenta)
            << "Aborted:" << kex::color::apply(kex::color::reset) << " "
            << errorCountPhrase(typeErrors, "type")
            << " — fix before running (use --no-check to skip).\n";
        return 1;
      }
    }

    if (mode == "run") {
      kex::interpreter::Evaluator evaluator;
      evaluator.setArgs(scriptArgs);
      evaluator.setModuleRoots(moduleRootsFor(filepath));
      // Mock.* is denied by default — a spec earns it by being a spec, and
      // anything else has to say --allow-mocks (issue #144).
      evaluator.setMocksAllowed(allowMocks || isSpecEntry(filepath));
      evaluator.setTestReportMode(testReportMode);
      evaluator.setTestFilters(testFilters);
      if (runAnalyzed) {
        evaluator.setStaticTypeOfCalls(&runAnalyzer.staticTypeOfCalls());
        evaluator.setExpressionTypes(&runAnalyzer.typeMap());
      }

      // The Kex-written stdlib is loaded by the Evaluator's constructor
      // (loadPrelude), so no explicit load is needed here.

      // The spec base file's declarations were merged into `program` above
      // (before the semantic check), so there is nothing extra to load here —
      // and `program` owns those nodes for as long as the evaluator needs the
      // raw `const ast::FunctionDef*` pointers it keeps into them.
      auto result = evaluator.execute(program);
      // A failing spec is a failing run. Checked before the program's own
      // return value because a spec file has none — it ends on the summary
      // line — and without this `kex some.spec.kex` exited 0 no matter how
      // many `it` blocks failed, which is what made every downstream `tey
      // test` and CI job green on a red suite.
      if (evaluator.testsFailed() > 0)
        return 1;
      if (auto *i = std::get_if<kex::interpreter::IntValue>(&result->data))
        return static_cast<int>(i->value);
      return 0;
    }

    // mode == "check"
    // A companion file's declarations are part of what this file means: a
    // `.spec.kex`'s base, and a `package.kex`'s manifest vocabulary. The run
    // and compile paths merge them before checking; do the same here, or
    // `kex -C` reports names the other two accept.
    std::string checkCompanion;
    for (const auto &candidate : specBaseCandidates(filepath)) {
      if (!fileExists(candidate))
        continue;
      checkCompanion = candidate;

      auto baseSource = readFile(candidate);
      kex::Lexer baseLexer(std::move(baseSource), candidate);
      auto baseTokens = baseLexer.tokenizeAll();
      kex::Parser baseParser(std::move(baseTokens), candidate);
      auto baseProgram = baseParser.parseProgram();

      std::vector<kex::ast::TopLevelItem> merged;
      merged.reserve(baseProgram.items.size() + program.items.size());
      for (auto &item : baseProgram.items)
        if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(item))
          merged.push_back(std::move(item));
      for (auto &item : program.items)
        merged.push_back(std::move(item));
      program.items = std::move(merged);
      break;
    }

    // ...and so are the project's OTHER modules. `mode == "run"` and
    // `mode == "compile"` both merge every module a `using` or a qualified
    // reference reaches before checking; this path did not, so it built its
    // checker from the prelude interface alone. A two-module program then
    // reported errors no other mode saw — a record declared in a sibling
    // module was an unknown name here, so a union declared alongside it
    // refused its own members and any annotation naming it went unverified.
    // The editor sees this too: the LSP checks the way `-C` does.
    {
      kex::semantic::Analyzer dependencyAnalysis(&preludeSemanticInterfaces());
      (void)dependencyAnalysis.analyze(program);
      auto qualifiedModules = dependencyAnalysis.referencedModules();
      for (auto it = qualifiedModules.begin(); it != qualifiedModules.end();) {
        const auto imported = preludeSemanticInterfaces().modules.find(*it);
        if (imported != preludeSemanticInterfaces().modules.end() &&
            imported->second.automaticImport)
          it = qualifiedModules.erase(it);
        else
          ++it;
      }
      // PROJECT roots only. A stdlib module reached by `using FS` already
      // has a compiled interface, and merging its SOURCE on top of that adds
      // a second, unannotated candidate for every method it defines — enough
      // to switch off a mismatch report that the interface alone gets right
      // (spec/typestate_survives_standin.kex: writing through a read-only
      // FileHandle stopped being an error). The run and compile paths merge
      // the stdlib too, but they consume it for CODEGEN, where having the
      // source is the point.
      auto projectRoots = moduleRootsFor(filepath);
      const auto stdlibRoots = kex::standardLibraryModuleRoots();
      projectRoots.erase(
          std::remove_if(projectRoots.begin(), projectRoots.end(),
                         [&](const std::string &root) {
                           return std::find(stdlibRoots.begin(),
                                            stdlibRoots.end(),
                                            root) != stdlibRoots.end();
                         }),
          projectRoots.end());
      resolveBeamDeps(program, projectRoots, &preludeSemanticInterfaces(),
                      qualifiedModules);
    }

    // Pass 1+2: collect symbols and resolve names via SemanticDB
    kex::semantic::SemanticDB db;
    db.setImportedInterfaces(&preludeSemanticInterfaces());
    db.setModuleRoots(moduleRootsFor(filepath));
    loadPrelude(db);
    std::vector<std::string> checkCompanions;
    if (!checkCompanion.empty()) checkCompanions.push_back(checkCompanion);
    db.updateFile(filepath, readFile(filepath), checkCompanions);

    // Pass 3+: existing Analyzer (purity, type checking)
    kex::semantic::Analyzer analyzer(&preludeSemanticInterfaces());
    bool ok = analyzer.analyze(program);

    // Collect all diagnostics
    std::vector<kex::semantic::Diagnostic> allDiags;
    bool dbOk = true;
    for (const auto &d : db.diagnosticsFor(filepath)) {
      if (d.level == kex::semantic::Diagnostic::Level::Error)
        dbOk = false;
      allDiags.push_back(d);
    }
    for (const auto &d : analyzer.diagnostics())
      allDiags.push_back(d);

    bool validationOk = true;
    if (ok && dbOk) {
      for (const auto &d :
           kex::validation::validateTaggedLiterals(program, analyzer, moduleRootsFor(filepath))) {
        if (d.level == kex::semantic::Diagnostic::Level::Error)
          validationOk = false;
        allDiags.push_back(d);
      }
    }

    bool allOk = ok && dbOk && validationOk;

    if (jsonOutput) {
      // Machine-readable JSON — one object per diagnostic
      std::cout << "[\n";
      for (size_t i = 0; i < allDiags.size(); i++) {
        const auto &d = allDiags[i];
        bool isErr = d.level == kex::semantic::Diagnostic::Level::Error;
        std::string hint = extractHint(d.message);
        std::cout << "  {\n"
                  << "    \"file\": \""
                  << jsonEscape(std::string(d.location.file)) << "\",\n"
                  << "    \"line\": " << d.location.line << ",\n"
                  << "    \"column\": " << d.location.column << ",\n"
                  << "    \"severity\": \"" << (isErr ? "error" : "warning")
                  << "\",\n"
                  << "    \"message\": \"" << jsonEscape(d.message) << "\"";
        if (d.endLocation)
          std::cout << ",\n"
                    << "    \"end_line\": " << d.endLocation->line << ",\n"
                    << "    \"end_column\": " << d.endLocation->column;
        if (!hint.empty())
          std::cout << ",\n    \"hint\": \"" << jsonEscape(hint) << "\"";
        if (!d.notes.empty()) {
          std::cout << ",\n    \"notes\": [\n";
          for (size_t noteIndex = 0; noteIndex < d.notes.size();
               ++noteIndex) {
            const auto &note = d.notes[noteIndex];
            std::cout << "      {\n"
                      << "        \"file\": \""
                      << jsonEscape(std::string(note.location.file))
                      << "\",\n"
                      << "        \"line\": " << note.location.line << ",\n"
                      << "        \"column\": " << note.location.column
                      << ",\n"
                      << "        \"message\": \""
                      << jsonEscape(note.message) << "\"\n"
                      << "      }"
                      << (noteIndex + 1 < d.notes.size() ? "," : "")
                      << "\n";
          }
          std::cout << "    ]";
        }
        std::cout << "\n  }" << (i + 1 < allDiags.size() ? "," : "") << "\n";
      }
      std::cout << "]\n";
      return allOk ? 0 : 1;
    }

    if (summaryMode) {
      // Output the public API signatures of the file in Kex syntax.
      // Preserve nested namespaces so FS.File is not misrepresented as a
      // separate top-level File module.
      auto printAnnotation =
          [&](const kex::ast::TypeAnnotation &annotation, size_t depth) {
            std::cout << std::string(depth * 2, ' ')
                      << (annotation.isFoul ? "foul " : "")
                      << annotation.name << " : ";
            if (annotation.type)
              std::cout << typeExprToString(*annotation.type);
            std::cout << "\n";
          };
      std::function<void(const kex::ast::ModuleDef &, size_t)> printModule;
      printModule = [&](const kex::ast::ModuleDef &module, size_t depth) {
        const auto indent = std::string(depth * 2, ' ');
        const auto separator = module.name.rfind('.');
        const auto displayName =
            depth > 0 && separator != std::string::npos
                ? module.name.substr(separator + 1)
                : module.name;
        std::cout << indent << "module "
                  << displayName << " do\n";
        for (const auto &item : module.body) {
          std::visit(
              [&](const auto &ptr) {
                using T = std::decay_t<decltype(*ptr)>;
                if constexpr (std::is_same_v<T,
                                             kex::ast::TypeAnnotation>)
                  printAnnotation(*ptr, depth + 1);
                else if constexpr (std::is_same_v<T,
                                                  kex::ast::ModuleDef>)
                  printModule(*ptr, depth + 1);
              },
              item);
        }
        std::cout << indent << "end\n";
      };

      for (const auto &item : program.items) {
        std::visit(
            [&](const auto &ptr) {
              using T = std::decay_t<decltype(*ptr)>;
              if constexpr (std::is_same_v<T, kex::ast::TypeAnnotation>) {
                printAnnotation(*ptr, 0);
              } else if constexpr (std::is_same_v<T, kex::ast::ModuleDef>) {
                printModule(*ptr, 0);
              }
            },
            item);
      }
      return allOk ? 0 : 1;
    }

    // Normal colored output
    for (const auto &d : allDiags)
      printSemanticDiagnostic(d);

    if (dumpTypes) {
      // Collect and sort by source location so output is readable
      // top-to-bottom.
      const auto &tmap = analyzer.typeMap();
      std::vector<std::pair<const kex::ast::Expr *, kex::semantic::TypePtr>>
          entries(tmap.begin(), tmap.end());
      std::sort(entries.begin(), entries.end(),
                [](const auto &a, const auto &b) {
                  const auto &la = a.first->location;
                  const auto &lb = b.first->location;
                  if (la.line != lb.line)
                    return la.line < lb.line;
                  if (la.column != lb.column)
                    return la.column < lb.column;
                  // Nested expressions share a start position — `n * f(x)`
                  // and its own left operand both begin at `n`. Without a
                  // tiebreak their order comes from an unordered map, so two
                  // runs of `-t` over one unchanged file could disagree.
                  return kex::semantic::typeToString(a.second) <
                         kex::semantic::typeToString(b.second);
                });
      for (const auto &[expr, type] : entries) {
        if (!type)
          continue;
        std::cout << expr->location.line << ":" << expr->location.column << "  "
                  << kex::semantic::typeToString(type) << "\n";
      }
    }

    if (allOk && !dumpTypes) {
      std::cout << "No errors found.\n";
    } else if (allOk) {
      std::cerr << "No errors found.\n";
    }

    return allOk ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << kex::color::apply(kex::color::bold)
              << kex::color::apply(kex::color::red)
              << "Internal error:" << kex::color::apply(kex::color::reset)
              << " " << e.what() << "\n";
    return 1;
  }

  return 0;
}
