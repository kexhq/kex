#include "beam/beam_file.hxx"
#include "beam/collect_metadata.hxx"
#include "beam/kexi.hxx"
#include "beam/kexi_registry.hxx"
#include "codegen/core_erlang.hxx"
#include "common/color.hxx"
#include "interpreter/evaluator.hxx"
#include "ir/emit_core.hxx"
#include "ir/lower.hxx"
#include "lexer/lexer.hxx"
#include "module/resolver.hxx"
#include "parser/parser.hxx"
#include "semantic/analyzer.hxx"
#include "semantic/db.hxx"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <sstream>
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
#include "common/prelude_loader.hxx"
#include "common/repl_commands.hxx"
// Set to the type name while the user is typing inside a `make X do` block,
// so the completer can infer parameter types from pattern signatures.
static std::string g_currentMakeTarget;

static auto replDefinitionName(const std::string &source) -> std::string {
  size_t off = 0;
  if (source.rfind("foul module ", 0) == 0) off = 12;
  else if (source.rfind("foul ", 0) == 0) off = 5;
  else if (source.rfind("let ", 0) == 0) off = 4;
  else if (source.rfind("type ", 0) == 0) off = 5;
  else if (source.rfind("record ", 0) == 0) off = 7;
  else if (source.rfind("module ", 0) == 0) off = 7;
  else if (source.rfind("make ", 0) == 0) off = 5;
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

auto readLine(const std::string &prompt) -> std::pair<std::string, bool> {
#ifdef HAS_READLINE
  char *input = readline(prompt.c_str());
  if (!input)
    return {"", false};
  std::string line(input);
  if (!line.empty())
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

} // namespace

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
  std::unique_ptr<kex::ast::Program> program;
};

auto resolveBeamDeps(kex::ast::Program &program,
                     const std::vector<std::string> &roots)
    -> std::vector<LoadedDep> {
  std::vector<LoadedDep> deps;
  std::unordered_set<std::string> loaded;
  kex::module::Resolver resolver(roots);

  std::function<void(const kex::ast::Program &)> resolve =
      [&](const kex::ast::Program &prog) {
    for (const auto &modName : collectUsingModules(prog)) {
      auto resolved = resolver.resolve(modName);
      if (!resolved) continue;
      if (!loaded.insert(resolved->path).second) continue;

      auto src = std::make_unique<std::string>(readFile(resolved->path));
      kex::Lexer lexer(std::string(*src), resolved->path);
      kex::Parser parser(lexer.tokenizeAll(), resolved->path);
      auto depProg = std::make_unique<kex::ast::Program>(parser.parseProgram());
      if (parser.diagnostics().empty()) {
        resolve(*depProg);
        deps.push_back({std::move(src), std::move(depProg)});
      }
    }
  };
  resolve(program);

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
#ifdef KEX_PRELUDE_DIR
  kex::loadPrelude(db, KEX_PRELUDE_DIR);
#endif
}

// Stdlib functions in the Kex prelude — UFCS calls to these route to the
// shared `kex_prelude` BEAM module instead of the emitter's inline ladder.
static const std::unordered_set<std::string> &migratedPreludeFns() {
  static const std::unordered_set<std::string> fns = [] {
    std::unordered_set<std::string> names = {
      "reverse",     "sort",      "uniq",      "flatten", "take",
      "drop",        "zip",       "push",      "sum",     "product",
      "indexOf",     "at",        "min",       "max",     "count",
      "join",        "upperCase", "lowerCase", "trim",    "split",
      "startsWith?", "endsWith?", "digit?",    "alpha?",  "space?",
      "modulo",      "even?",     "odd?",      "keys",    "values",
      "entries",     "merge",     "has?",      "put",     "delete",
      "abs",         "sqrt",      "none?",     "some?",   "ok?",
      "error?",      "first",     "last",      "empty?",  "or",
      "in?",         "blank?",    "present?",  "truthy?", "falsy?",
      "second",      "third",     "floor",     "ceil",    "round",
      "toInteger",   "rest",      "toOptional",
      "chars",       "items",     "send",      "link",    "unlink",
      "monitor",     "alive?",    "demonitor", "await",
      "start"};
#ifdef KEX_PRELUDE_DIR
    // Discover both module functions and receiver-constrained extension
    // functions from Kex source. `value.foo(arg)` is UFCS for the resolved
    // `foo(value, arg)` extension; it is not an object-method dispatch. Keeping
    // this transitional routing source-driven prevents every new prelude
    // extension from requiring another compiler name entry while the stdlib is
    // moved to normal KexI ownership.
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(KEX_PRELUDE_DIR, ec)) {
      if (entry.path().extension() != ".kex") continue;
      kex::Lexer lexer(readFile(entry.path().string()), entry.path().string());
      kex::Parser parser(lexer.tokenizeAll(), entry.path().string());
      auto program = parser.parseProgram();
      auto collectMake = [&](const kex::ast::MakeDef &make) {
        for (const auto &item : make.body) {
          if (const auto *fn =
                  std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item)) {
            if (*fn) names.insert((*fn)->name);
          } else if (const auto *visibility =
                         std::get_if<std::unique_ptr<kex::ast::VisibilityBlock>>(
                             &item)) {
            if (!*visibility) continue;
            for (const auto &visible : (*visibility)->items)
              if (const auto *fn =
                      std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(
                          &visible); fn && *fn)
                names.insert((*fn)->name);
          }
        }
      };
      auto collectTrait = [&](const kex::ast::TraitDef &trait) {
        for (const auto &item : trait.body)
          if (const auto *fn =
                  std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item);
              fn && *fn)
            names.insert((*fn)->name);
      };
      std::function<void(const kex::ast::ModuleDef &)> collect;
      collect = [&](const kex::ast::ModuleDef &module) {
        for (const auto &item : module.body) {
          if (const auto *fn =
                  std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&item)) {
            if (*fn) names.insert(module.name + "." + (*fn)->name);
          } else if (const auto *make =
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
        }
      }
    }
#endif
    return names;
  }();
  return fns;
}

#ifdef KEX_PRELUDE_DIR
// Parse+merge the Kex prelude (src/prelude/*.kex, MainBlocks dropped) into one
// Program and lower it to the shared `kex_prelude` module's Core Erlang,
// written to <dir>/kex_prelude.core. Returns false (with a message) if the
// prelude can't be lowered yet.
auto compilePreludeCore(const std::string &dir) -> bool {
  namespace fs = std::filesystem;
  kex::ast::Program merged;
  std::error_code ec;
  std::vector<std::string> files;
  for (const auto &e : fs::directory_iterator(KEX_PRELUDE_DIR, ec))
    if (e.path().extension() == ".kex")
      files.push_back(e.path().string());
  std::sort(files.begin(), files.end());
  for (const auto &f : files) {
    kex::Lexer lex(readFile(f), f);
    kex::Parser parser(lex.tokenizeAll(), f);
    auto prog = parser.parseProgram();
    for (auto &item : prog.items)
      if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(item))
        merged.items.push_back(std::move(item));
  }
  try {
    auto mod = kex::ir::lowerProgram(merged, "prelude");
    auto res = kex::ir::emitCore(mod);
    std::ofstream out(dir + "/kex_prelude.core");
    if (!out)
      return false;
    out << res.source;
  } catch (const kex::ir::LowerError &e) {
    std::cerr << "error: prelude: " << e.what() << "\n";
    return false;
  }
  return true;
}
#endif

#ifdef KEX_PRELUDE_DIR
// Prelude record defs (e.g. ParseError in src/prelude/errorable.kex) aren't
// in the user's AST, but the BEAM codegen needs their field layout to emit
// field accessors (e.field -> element(N, e)). Merge just the RecordDef items
// (metadata-only — no runtime code) into the user program before codegen.
// Mirrors how the interpreter's Evaluator executes the prelude
// (evaluator.cxx's preludeProgram).
auto collectPreludeRecordDefs() -> std::vector<kex::ast::TopLevelItem> {
  std::vector<kex::ast::TopLevelItem> defs;
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<std::string> files;
  for (const auto& e : fs::directory_iterator(KEX_PRELUDE_DIR, ec))
    if (e.path().extension() == ".kex")
      files.push_back(e.path().string());
  std::sort(files.begin(), files.end());

  std::function<void(std::vector<kex::ast::ModuleItem>&)> extractFromModule;
  extractFromModule = [&](std::vector<kex::ast::ModuleItem>& body) {
    for (auto& item : body) {
      if (auto* rd = std::get_if<std::unique_ptr<kex::ast::RecordDef>>(&item)) {
        if (*rd) {
          auto copy = std::make_unique<kex::ast::RecordDef>();
          copy->name = (*rd)->name;
          copy->location = (*rd)->location;
          for (const auto& f : (*rd)->fields) {
            kex::ast::RecordField rf;
            rf.name = f.name;
            copy->fields.push_back(std::move(rf));
          }
          defs.push_back(std::move(copy));
        }
      } else if (auto* md = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item)) {
        if (*md) extractFromModule((*md)->body);
      }
    }
  };

  for (const auto& f : files) {
    kex::Lexer lex(readFile(f), f);
    kex::Parser parser(lex.tokenizeAll(), f);
    auto prog = parser.parseProgram();
    for (auto& item : prog.items) {
      if (std::holds_alternative<std::unique_ptr<kex::ast::RecordDef>>(item)) {
        defs.push_back(std::move(item));
      } else if (auto* md = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item)) {
        if (*md) extractFromModule((*md)->body);
      }
    }
  }
  return defs;
}
#endif

#ifdef KEX_PRELUDE_DIR
// Method names the sealed stdlib prelude provides — make-block methods across
// src/prelude/*.kex plus trait methods (Enumerable's map/filter/…). A user may
// ADD new methods to a builtin type but not REDEFINE one of these on it (they
// carry contracts like `first : X?`), so such a collision is a compile error.
auto preludeMethodNames() -> const std::unordered_set<std::string> & {
  static const std::unordered_set<std::string> names = [] {
    std::unordered_set<std::string> s;
    namespace fs = std::filesystem;
    std::error_code ec;
    auto addFn = [&](const kex::ast::FunctionDef *fd) {
      if (fd)
        s.insert(fd->name);
    };
    for (const auto &e : fs::directory_iterator(KEX_PRELUDE_DIR, ec)) {
      if (e.path().extension() != ".kex")
        continue;
      try {
        kex::Lexer lex(readFile(e.path().string()), e.path().string());
        kex::Parser parser(lex.tokenizeAll(), e.path().string());
        auto prog = parser.parseProgram();
        for (auto &item : prog.items) {
          if (auto *md =
                  std::get_if<std::unique_ptr<kex::ast::MakeDef>>(&item)) {
            if (*md)
              for (auto &bi : (*md)->body)
                if (auto *fd =
                        std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(
                            &bi))
                  addFn(fd->get());
          } else if (auto *td =
                         std::get_if<std::unique_ptr<kex::ast::TraitDef>>(
                             &item)) {
            if (*td)
              for (auto &bi : (*td)->body)
                if (auto *fd =
                        std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(
                            &bi))
                  addFn(fd->get());
          }
        }
      } catch (...) {
      }
    }
    return s;
  }();
  return names;
}

// The simple name of a `make` target, or "" — with "List"/"Map" for list/map
// types. Builtin types are sealed against stdlib-method redefinition.
auto makeTargetName(const kex::ast::TypeExprPtr &t) -> std::string {
  if (!t)
    return "";
  if (auto *tn = std::get_if<kex::ast::TypeName>(&t->kind))
    if (!tn->parts.empty())
      return tn->parts.back();
  if (auto *g = std::get_if<kex::ast::GenericType>(&t->kind))
    if (!g->name.parts.empty())
      return g->name.parts.back();
  if (std::holds_alternative<kex::ast::ListType>(t->kind))
    return "List";
  if (std::holds_alternative<kex::ast::MapType>(t->kind))
    return "Map";
  return "";
}

// Sealed-stdlib violations: a make block on a builtin type redefining a
// prelude-provided method. Returned as diagnostics so every check path (run,
// -R, and --check) reports them identically. Prelude files are exempt.
auto sealViolations(const kex::ast::Program &program,
                    const std::string &filepath)
    -> std::vector<kex::semantic::Diagnostic> {
  std::vector<kex::semantic::Diagnostic> diags;
  if (filepath.find(KEX_PRELUDE_DIR) != std::string::npos)
    return diags;
  // Keep in sync with the evaluator-side seal (execMakeDef in evaluator.cxx).
  static const std::unordered_set<std::string> builtins = {
      "Integer", "Float", "Char",  "Bool",     "Number", "String",
      "List",    "Map",   "Range", "Optional", "Result"};
  auto check = [&](const kex::ast::FunctionDef *fd, const std::string &ty,
                   const kex::SourceLocation &loc) {
    if (fd && preludeMethodNames().count(fd->name))
      diags.push_back({kex::semantic::Diagnostic::Level::Error, loc,
                       "cannot override sealed stdlib method '" + fd->name +
                           "' on builtin type '" + ty + "'"});
  };
  for (const auto &item : program.items)
    if (auto *md = std::get_if<std::unique_ptr<kex::ast::MakeDef>>(&item)) {
      if (!*md)
        continue;
      auto ty = makeTargetName((*md)->target);
      if (!builtins.count(ty))
        continue;
      for (const auto &bi : (*md)->body) {
        if (auto *fd = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&bi))
          check(fd->get(), ty, (*md)->location);
        else if (auto *vb =
                     std::get_if<std::unique_ptr<kex::ast::VisibilityBlock>>(
                         &bi))
          if (*vb)
            for (const auto &vi : (*vb)->items)
              if (auto *vf =
                      std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&vi))
                check(vf->get(), ty, (*md)->location);
      }
    }
  return diags;
}
#endif

// Runs semantic analysis (undefined-name detection + type checking) and
// prints any diagnostics, same as plain `run` mode's pre-execution check.
// Shared by `run` and `compile`/`-R` (BEAM) so both backends catch the same
// errors before doing anything backend-specific — previously `-R` skipped
// this entirely and fell straight through to erlc, which reports things
// like an undefined function as a raw, un-Kex-like Core Erlang compile
// error ("unbound variable 'UndefinedFunctionCall' in main/0") instead of
// this backend-agnostic diagnostic. Returns false if there were any errors.
auto runSemanticCheck(const kex::ast::Program &program,
                      const std::string &filepath) -> bool {
  auto printDiag = [&](const kex::semantic::Diagnostic &diag) {
    bool isError = diag.level == kex::semantic::Diagnostic::Level::Error;
    std::cerr << kex::color::apply(kex::color::gray) << diag.location.file
              << ":" << diag.location.line << ":" << diag.location.column << ":"
              << kex::color::apply(kex::color::reset) << " "
              << kex::color::apply(kex::color::bold)
              << (isError ? kex::color::apply(kex::color::red)
                          : kex::color::apply(kex::color::magenta))
              << (isError ? "error" : "warning") << ":"
              << kex::color::apply(kex::color::reset) << " "
              << colorizeMessage(diag.message) << "\n";
  };

  // Pass 1+2: SemanticDB undefined-name detection
  kex::semantic::SemanticDB runDb;
  loadPrelude(runDb);
  runDb.updateFile(filepath, readFile(filepath));
  bool dbOk = true;
  for (const auto &diag : runDb.diagnosticsFor(filepath)) {
    if (diag.level == kex::semantic::Diagnostic::Level::Error)
      dbOk = false;
    printDiag(diag);
  }

  // Pass 3+: existing Analyzer (purity, type checking)
  kex::semantic::Analyzer analyzer;
  bool ok = analyzer.analyze(program);
  for (const auto &diag : analyzer.diagnostics())
    printDiag(diag);

#ifdef KEX_PRELUDE_DIR
  for (const auto &d : sealViolations(program, filepath)) {
    printDiag(d);
    ok = false;
  }
#endif

  return ok && dbOk;
}

auto printAst(const kex::ast::Program &program) -> void {
  std::cout << "Program (" << program.items.size() << " items)\n";
  for (const auto &item : program.items) {
    std::visit(
        [](const auto &node) {
          using T = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<T,
                                       std::unique_ptr<kex::ast::ModuleDef>>) {
            std::cout << "  Module: " << node->name
                      << (node->isFoul ? " [foul]" : "") << "\n";
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

// Absolute path to cmake-pre-built Kex runtime beams (kex_io.beam, ...),
// or empty if none is available. Using these avoids an erlc spawn (~0.15s
// per module) on every BEAM invocation.
auto prebuiltRuntimeBeamDir() -> std::string {
#ifdef KEX_RUNTIME_BEAM_DIR
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::exists(fs::path{KEX_RUNTIME_BEAM_DIR} / "kex_io.beam", ec))
    return KEX_RUNTIME_BEAM_DIR;
#else
  (void)0;
#endif
  return {};
}

// Fallback: walk up from argv[0] to find runtime/src/*.erl and compile them
// into a fresh temp dir. Returns the dir (empty on failure); caller owns
// cleanup via fs::remove_all.
auto compileRuntimeBeamsToTemp(const char *argv0) -> std::string {
  namespace fs = std::filesystem;
  fs::path bin = fs::weakly_canonical(argv0);
  for (auto dir = bin.parent_path(); dir != dir.root_path();
       dir = dir.parent_path()) {
    auto rtSrc = dir / "runtime" / "src";
    if (!fs::exists(rtSrc))
      continue;
    char tmp[] = "/tmp/kex_rt_XXXXXX";
    if (!mkdtemp(tmp))
      return {};
    std::string out = tmp;
    for (const auto &e : fs::directory_iterator(rtSrc)) {
      if (e.path().extension() == ".erl") {
        std::string cmd =
            "erlc -o " + out + " " + e.path().string() + " > /dev/null 2>&1";
        std::system(cmd.c_str());
      }
    }
    return out;
  }
  return {};
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
      << "  -s, --summary     Print public API signatures (Kex syntax)\n"
      << "  -t, --types       With --check: dump inferred expression types\n"
      << "  -e, --emit-core   Emit Core Erlang (.core) — does not invoke erlc\n"
      << "  -o <dir>          Output directory for -c / --emit-core (default: "
         ".)\n"
      << "  -h, --help        Show this help\n"
      << "  -v, --version     Show version\n"
      << "  --no-colors       Disable ANSI color output\n"
      << "  --legacy-emitter  Use the legacy string-based Core Erlang "
         "emitter\n";
}

auto printVersion() -> void {
  std::cout << "kex " << kex::kVersion << "\n";
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
      {"complete", required_argument, nullptr, 'K'},
      {"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, 'v'},
      {"no-colors", no_argument, nullptr, 'N'},
      // The AST→IR→Core Erlang pipeline (src/ir/) is the default BEAM
      // backend; --ir is kept as a no-op for compatibility.
      {"ir", no_argument, nullptr, 1000},
      // Escape hatch: route BEAM codegen through the legacy string emitter
      // (src/codegen/core_erlang.cxx) instead of the IR pipeline.
      {"legacy-emitter", no_argument, nullptr, 1002},
      {"no-prelude", no_argument, nullptr, 1003},
      // Compile the Kex prelude (src/prelude/*.kex) into kex_prelude.core +
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
  bool outputDirExplicit = false;
  int opt;

  bool compileRun = false;
  bool useIr = true;
  bool skipPrelude = false;
  while ((opt = getopt_long(argc, argv, "rnlcCiRjspethvK:o:", longOptions,
                            nullptr)) != -1) {
    switch (opt) {
    case 1000:
      useIr = true; // already the default; kept for compatibility
      break;
    case 1002:
      useIr = false;
      break;
    case 1003:
      skipPrelude = true;
      break;
    case 1001: {
#ifdef KEX_PRELUDE_DIR
      std::string dir = optarg;
      if (!compilePreludeCore(dir))
        return 1;
      std::string cmd = "erlc +from_core -pa " + dir + " -o " + dir + " " +
                        dir + "/kex_prelude.core";
      return std::system(cmd.c_str()) == 0 ? 0 : 1;
#else
      std::cerr << "error: prelude dir not configured\n";
      return 1;
#endif
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
#endif

  if (mode == "complete") {
    kex::semantic::SemanticDB db;
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

    // Put runtime beams (kex_io, ...) on the path. Prefer the cmake
    // pre-built dir (added as an extra -pa at run time); fall back to
    // compiling them into beamDir so a single -pa suffices.
    std::string rtPaDir = prebuiltRuntimeBeamDir();
    if (rtPaDir.empty()) {
      namespace fs = std::filesystem;
      fs::path bin = fs::weakly_canonical(argv[0]);
      for (auto dir = bin.parent_path(); dir != dir.root_path();
           dir = dir.parent_path()) {
        auto rtDir = dir / "runtime" / "src";
        if (fs::exists(rtDir)) {
          for (const auto &entry : fs::directory_iterator(rtDir))
            if (entry.path().extension() == ".erl") {
              std::string cmd = "erlc -o " + beamDir + " " +
                                entry.path().string() + " > /dev/null 2>&1";
              std::system(cmd.c_str());
            }
          break;
        }
      }
    }

    // Spawn ONE persistent erl VM for the whole session — driven by
    // runtime/src/kex_repl_driver.erl over stdin/stdout. This is what
    // makes spawned processes / registered names survive across REPL
    // inputs and turns session recompiles into hot-loads instead of
    // cold re-runs.
    BeamVm vm;
    {
      std::vector<std::string> erlArgs = {"erl", "-noshell", "-pa", beamDir};
      if (!rtPaDir.empty()) {
        erlArgs.push_back("-pa");
        erlArgs.push_back(rtPaDir);
      }
      erlArgs.push_back("-eval");
      erlArgs.push_back("kex_repl_driver:loop()");
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
      while (!otpInfo.empty() && otpInfo.back() == '\n')
        otpInfo.pop_back();
      if (!otpInfo.empty())
        std::cout << kex::color::apply(kex::color::gray) << otpInfo
                  << kex::color::apply(kex::color::reset) << "\n";
    }
    kex::printReplBanner(std::cout, "BEAM");

    kex::semantic::SemanticDB beamReplDb;
    if (!skipPrelude)
      loadPrelude(beamReplDb);
#ifdef HAS_READLINE
    g_replDb = &beamReplDb;
    rl_attempted_completion_function = kexCompletion;
    rl_completion_display_matches_hook = kexDisplayMatches;
    rl_completer_word_break_characters = (char *)" \t\n\\@$><=;|&{(";
    rl_completer_quote_characters = (char *)"";
#endif

    // Top-level definitions, tracked by name so redefining a function
    // REPLACES its earlier clauses rather than appending duplicates (a
    // stale first clause would otherwise shadow the new one — the
    // "this clause cannot match" BEAM warning). Insertion order kept so
    // independent defs stay in source order.
    std::vector<std::pair<std::string, std::string>> topDefs;
    std::string localBinds; // let x = ... — re-emitted inside main do each eval
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
      if (input.empty())
        continue;
      if (kex::isReplExit(input))
        break;
      if (input == "/help" || input == "/h") {
        kex::printReplHelp(std::cout);
        continue;
      }
      if (input == "/reset") {
        topDefs.clear();
        localBinds.clear();
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
      while (dc > 0) {
        auto [cont, contOk] = readLine("  ...> ");
        if (!contOk)
          break;
        source += "\n" + cont;
        dc += countBlocks(cont);
      }
      if (dc == 0) {
        if (auto name = clauseFuncName(source)) {
          while (true) {
            auto [cont, contOk] = readLine("  ...> ");
            if (!contOk)
              break;
            auto nextName = clauseFuncName(cont);
            if (nextName && *nextName == *name) {
              source += "\n" + cont;
              int extra = countBlocks(cont);
              while (extra > 0) {
                auto [c2, ok2] = readLine("  ...> ");
                if (!ok2)
                  break;
                source += "\n" + c2;
                extra += countBlocks(c2);
              }
            } else {
              if (!cont.empty())
                pendingLine = cont;
              break;
            }
          }
        }
      }

      // Classify: function def vs local let vs expression
      bool isFuncDef = false;
      bool isLocalLet = false;
      std::string letVarName;
      {
        size_t off = std::string::npos;
        if (source.rfind("let ", 0) == 0)
          off = 4;
        else if (source.rfind("foul ", 0) == 0)
          off = 5;
        if (off != std::string::npos) {
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
      }
      if (source.rfind("module ", 0) == 0 || source.rfind("type ", 0) == 0 ||
          source.rfind("record ", 0) == 0 || source.rfind("make ", 0) == 0)
        isFuncDef = true;

      try {
        if (isFuncDef) {
          // Validate by parsing against accumulated defs.
          std::string check = topDefsStr() + source + "\n";
          kex::Lexer lexer(check);
          auto tokens = lexer.tokenizeAll();
          kex::Parser parser(std::move(tokens));
          parser.parseProgram(); // throws on syntax error

          std::string fname = replDefinitionName(source);

          // Replace any prior definition of the same function so a
          // redefinition takes effect (last def wins), rather than
          // appending a duplicate clause the stale one shadows.
          topDefs.erase(
              std::remove_if(topDefs.begin(), topDefs.end(),
                             [&](const auto &p) { return p.first == fname; }),
              topDefs.end());
          topDefs.push_back({fname, source});

          std::cout << kex::color::apply(kex::color::gray) << "=> "
                    << kex::color::apply(kex::color::reset) << "defined "
                    << fname << "\n";
        } else {
          // Expression or local let — compile and run on BEAM.
          // For local lets, the current binding is evaluated fresh and
          // stashed in the process dictionary so subsequent evals
          // retrieve it (avoiding re-evaluation of side-effectful
          // expressions like Ets.new).
          std::string kexSource;
          if (isLocalLet) {
            kexSource = topDefsStr() + "main do\n" + localBinds + "  " +
                        source + "\n" +
                        "  Erlang.Erlang.put(:kexrepl" + letVarName +
                        ", " + letVarName + ")\n" +
                        "  IO.inspect(" + letVarName +
                        ")\nend\n";
          } else {
            kexSource = topDefsStr() + "main do\n" + localBinds +
                        "  IO.inspect(" + source + ")\nend\n";
          }

          kex::Lexer lexer(kexSource);
          auto tokens = lexer.tokenizeAll();
          kex::Parser parser(std::move(tokens));
          auto program = parser.parseProgram();
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
          auto extMods = kexiRegistry.buildExternalModules();
          auto irMod = kex::ir::lowerProgram(program, "kex_repl_session",
                                             migratedPreludeFns(), "",
                                             extMods.nameToAtom.empty() ? nullptr : &extMods);
          auto result = kex::ir::emitCore(irMod);

          std::string corePath = beamDir + "/" + result.moduleName + ".core";
          std::ofstream cf(corePath);
          if (!cf) {
            std::cerr << "  error: cannot write " << corePath << "\n";
            continue;
          }
          cf << result.source;
          cf.close();

          std::string erlCmd = "erlc +from_core -W0 -pa " +
                               beamDir + " -o " + beamDir + " " +
                               corePath + " 2>&1";
          int erlcRet = std::system(erlCmd.c_str());
          std::filesystem::remove(corePath);
          if (erlcRet != 0) {
            std::cerr << "  error: compilation failed\n";
            continue;
          }

          // Hot-load the freshly compiled session module into the
          // persistent VM, then evaluate it. The stable module name
          // means each reload is a new version superseding the
          // previous — code:load_binary's native code-upgrade path.
          std::string beamPath = beamDir + "/" + result.moduleName + ".beam";
          std::string loadNonce = std::to_string(++iteration);
          vm.writeLine("load " + loadNonce + " " + result.moduleName + " " +
                       beamPath);
          std::string loadStatus;
          vm.readUntilSentinel("KEX_REPL_DONE " + loadNonce + " ", loadStatus);
          if (loadStatus != "ok") {
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

          if (isLocalLet)
            localBinds += "  let " + letVarName +
                          " = Erlang.Erlang.get(:kexrepl" +
                          letVarName + ")\n";
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
    if (!skipPrelude)
      loadPrelude(replDb);
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
#endif

    kex::interpreter::Evaluator evaluator;
    evaluator.setReplMode(true);
    std::string line;
    // Accumulated source of all top-level definitions typed in the REPL so
    // far, used to keep the SemanticDB index complete across multiple inputs.
    std::string replAccumSource;
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
      while (doCount > 0) {
        auto [contLine, contOk] = readLine("...> ");
        if (!contOk)
          break;
        line = contLine;
        source += "\n" + line;
        doCount += countBlocks(line);
      }

      // If this line starts a function clause definition, keep reading
      // additional clauses for the *same* function so pattern-matching
      // definitions like `let fact(1) = 1` / `let fact(n) = ...` are
      // combined into one function. The first line that isn't another
      // clause of the same function is replayed on the next iteration.
      if (doCount == 0) {
        if (auto name = clauseFuncName(source)) {
          while (true) {
            auto [contLine, contOk] = readLine("...> ");
            if (!contOk)
              break;
            auto nextName = clauseFuncName(contLine);
            if (nextName && *nextName == *name) {
              source += "\n" + contLine;
              int extra = countBlocks(contLine);
              while (extra > 0) {
                auto [contLine2, contOk2] = readLine("...> ");
                if (!contOk2)
                  break;
                source += "\n" + contLine2;
                extra += countBlocks(contLine2);
              }
            } else {
              if (!contLine.empty())
                pendingLine = contLine;
              break;
            }
          }
        }
      }

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

      auto showResult = [&](const kex::interpreter::ValuePtr &result) {
        if (result && !std::holds_alternative<kex::interpreter::UnitValue>(
                          result->data)) {
          std::cout << kex::color::apply(kex::color::gray) << "=> "
                    << kex::color::apply(kex::color::reset)
                    << result->inspect();
          if (showTypes) {
            std::cout << " " << kex::color::apply(kex::color::gray) << ":"
                      << kex::color::apply(kex::color::reset) << " "
                      << kex::color::apply(kex::color::cyan)
                      << result->typeName()
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
      if (defOffset != std::string::npos) {
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
          source.substr(0, 12) == "foul module ") {
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
          replPrograms.push_back(program);
          execProgram(program);
          // Accumulate and re-index so all prior definitions stay
          // visible for tab completion, not just the latest one.
          replAccumSource += source + "\n";
          replDb.updateFile("<repl>", replAccumSource);
          g_currentMakeTarget.clear(); // block is complete
          std::cout << kex::color::apply(kex::color::gray) << "=> "
                    << kex::color::apply(kex::color::reset) << "defined "
                    << replDefinitionName(source) << "\n";
        } else {
          // Wrap in main for expression evaluation
          auto wrapped = "main do\n" + source + "\nend\n";
          kex::Lexer lexer(wrapped);
          auto tokens = lexer.tokenizeAll();
          kex::Parser parser(std::move(tokens));
          auto *program = new kex::ast::Program(parser.parseProgram());
          replPrograms.push_back(program);
          auto result = execProgram(program);
          showResult(result);
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

    // Put Kex runtime beams (kex_io, kex_file, ...) on the code path.
    // Prefer cmake-pre-built beams (zero compile cost); fall back to
    // compiling them into a temp dir we clean up afterwards.
    std::string rtBeamDir = prebuiltRuntimeBeamDir();
    bool rtTemp = false;
    if (rtBeamDir.empty()) {
      rtBeamDir = compileRuntimeBeamsToTemp(argv[0]);
      rtTemp = !rtBeamDir.empty();
    }

    // Load explicitly — code:load_abs rejects when filename != module name,
    // so use code:load_binary which skips that check.
    std::string mainCall =
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
    std::string runCmd = "erl -noshell -pa " + absBeamDir;
    if (!rtBeamDir.empty())
      runCmd += " -pa " + rtBeamDir;
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
    int rc = std::system(runCmd.c_str());
    if (rtTemp)
      fs::remove_all(rtBeamDir);
    return rc;
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
    if (!parser.diagnostics().empty() && (mode == "run" || mode == "compile")) {
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
                << "Aborted:" << kex::color::apply(kex::color::reset)
                << " fix syntax errors before "
                << (mode == "run" || compileRun ? "running" : "compiling")
                << ".\n";
      return 1;
    }

    if (mode == "parse") {
      printAst(program);
      return 0;
    }

    if (mode == "compile" && !skipCheck) {
      // Same pre-execution check `run` mode does — see
      // runSemanticCheck's doc comment for why this matters: without
      // it, an error here only ever surfaced as a raw erlc failure.
      // Not applied to emit-core (a debug/inspection dump — you may
      // want to see the emitted Core Erlang for code that doesn't
      // type-check yet).
      if (!runSemanticCheck(program, filepath)) {
        // -R (run-beam) sets mode == "compile" internally too (see
        // compileRun above) — say "before running" there, matching
        // the tree-walker's identical message for the same failure,
        // since from the user's point of view they ran `kex -R
        // file.kex`, not `kex --compile file.kex`.
        std::cerr << kex::color::apply(kex::color::bold)
                  << kex::color::apply(kex::color::magenta)
                  << "Aborted:" << kex::color::apply(kex::color::reset)
                  << " fix type errors before "
                  << (compileRun ? "running" : "compiling")
                  << " (use --no-check to skip).\n";
        return 1;
      }
    }

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
      // resolve module files, parse them, and merge into the program
      // so the IR lowering sees all definitions.
      {
        namespace fs = std::filesystem;
        auto srcDir = fs::weakly_canonical(filepath).parent_path().string();
        std::vector<std::string> roots;
        for (const auto &r : {"lib", "src"}) {
          auto full = srcDir + "/" + r;
          if (fs::is_directory(full)) roots.push_back(full);
        }
        if (roots.empty()) roots.push_back(srcDir);
        auto deps = resolveBeamDeps(program, roots);
        (void)deps;
      }

      // An explicit `main do ... end` is no longer required here —
      // CoreErlangEmitter already synthesizes one from trailing bare
      // top-level expressions when there isn't one (see its
      // `bareExprs` handling), matching the tree-walker's own
      // implicit-top-level-execution behavior exactly. This used to
      // hard-require an explicit main block and reject anything else
      // outright, which was stricter than the emitter itself actually
      // needs — a real, reproduced case: spec/comparision.kex is
      // pure top-level `type`/`make`/`let` declarations plus bare
      // `comps.each { ... }` calls with no `main` block at all, and
      // runs identically under both backends once this guard is gone.
      // The AST→IR→Core Erlang pipeline (src/ir/) is the default BEAM
      // backend; `--legacy-emitter` opts back into the string emitter.
      // Both produce the same {source, moduleName, mainArity} shape, so
      // the downstream erlc/erl path is identical. A LowerError means
      // the IR path doesn't support that construct — reported cleanly,
      // never falling back (so gaps are visible, not masked).
#ifdef KEX_PRELUDE_DIR
      // Prelude record defs (e.g. ParseError) aren't in the user's AST; merge
      // them so the codegen can emit field accessors for prelude records.
      for (auto& item : collectPreludeRecordDefs())
        program.items.push_back(std::move(item));
#endif
      kex::codegen::CoreErlangEmitter::EmitResult result;
      std::vector<kex::codegen::CoreErlangEmitter::EmitResult> moduleResults;
      if (useIr) {
        try {
          auto irModules = kex::ir::lowerModules(program, stem,
                                                 migratedPreludeFns(), filepath);
          for (const auto &irMod : irModules) {
            auto irRes = kex::ir::emitCore(irMod);
            kex::codegen::CoreErlangEmitter::EmitResult emitted;
            emitted.source = std::move(irRes.source);
            emitted.moduleName = std::move(irRes.moduleName);
            emitted.mainArity = irRes.mainArity;
            moduleResults.push_back(std::move(emitted));
          }
          result = moduleResults.front();
        } catch (const kex::ir::LowerError &e) {
          std::cerr << "error: " << e.what() << "\n";
          if (compileRun && !outputDirExplicit && !tempDir.empty())
            std::filesystem::remove_all(tempDir);
          return 1;
        }
      } else {
        kex::codegen::CoreErlangEmitter emitter;
        result = emitter.emitProgram(program, stem);
        moduleResults.push_back(result);
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
      // Place Kex runtime beams (kex_io, kex_file, ...) into the output
      // dir so the compiled .beam is self-contained. Prefer copying the
      // cmake-pre-built beams; fall back to compiling from source.
      {
        namespace fs = std::filesystem;
        std::string prebuilt = prebuiltRuntimeBeamDir();
        if (!prebuilt.empty()) {
          std::error_code ec;
          for (const auto &e : fs::directory_iterator(prebuilt))
            if (e.path().extension() == ".beam")
              fs::copy_file(e.path(), fs::path{outputDir} / e.path().filename(),
                            fs::copy_options::overwrite_existing, ec);
        } else {
          fs::path bin = fs::weakly_canonical(argv[0]);
          for (auto dir = bin.parent_path(); dir != dir.root_path();
               dir = dir.parent_path()) {
            auto rtDir = dir / "runtime" / "src";
            if (fs::exists(rtDir)) {
              for (const auto &entry : fs::directory_iterator(rtDir)) {
                if (entry.path().extension() == ".erl") {
                  std::string rtCmd = "erlc -o " + outputDir + " " +
                                      entry.path().string() +
                                      " > /dev/null 2>&1";
                  std::system(rtCmd.c_str());
                }
              }
              break;
            }
          }
        }
      }

#ifdef KEX_PRELUDE_DIR
      // Shared stdlib: kex_prelude.beam is normally prebuilt into the
      // runtime beam dir (see CMakeLists.txt) and staged with the other
      // runtime beams above, so BEAM lazy-loads it. Only fall back to a
      // per-run compile when that prebuilt beam isn't present (e.g.
      // running outside the build tree).
      // Both backends emit `call 'kex_prelude':...` for migrated stdlib
      // methods, so the fallback compile isn't gated on the backend.
      if (!std::filesystem::exists(std::filesystem::path{outputDir} /
                                   "kex_prelude.beam") &&
          compilePreludeCore(outputDir)) {
        std::string preCmd = "erlc +from_core -pa " + outputDir + " -o " +
                             outputDir + " " + outputDir + "/kex_prelude.core";
        if (!tempDir.empty())
          preCmd += " > /dev/null 2>&1";
        std::system(preCmd.c_str());
      }
#endif

      int erlcRet = 0;
      for (size_t moduleIndex = 0; moduleIndex < corePaths.size(); ++moduleIndex) {
        std::string coreCmd = "erlc +from_core -pa " + outputDir + " -o " +
                              outputDir + " " + corePaths[moduleIndex];
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
        erlcRet = std::system(coreCmd.c_str());
        if (erlcRet != 0) {
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
            copts.moduleAtom = moduleResults[moduleIndex].moduleName;
            copts.fileStem = stem;
            copts.noCheck = skipCheck;
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
            if (moduleIndex == 0 && moduleResults.size() > 1) {
              for (size_t ci = 1; ci < moduleResults.size(); ci++) {
                kex::beam::KexiCompanion comp;
                comp.beamAtom = moduleResults[ci].moduleName;
                comp.relativePath = moduleResults[ci].moduleName + ".beam";
                chunk.metadata.companions.push_back(std::move(comp));
              }
            }
            chunk.interfaceHash = kex::beam::computeInterfaceHash(chunk);
            auto payload = kex::beam::serializeKexi(chunk);
            auto bf = kex::beam::readBeamFile(beamPath);
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

      // Rename kex_<stem>.beam → <stem>.kx.beam (user-facing name).
      // The internal Erlang module name stays kex_<stem> inside the file.
      std::string internalBeam = outputDir + "/" + result.moduleName + ".beam";
      std::string kxBeam = outputDir + "/" + stem + ".kx.beam";
      if (tempDir.empty()) {
        std::filesystem::rename(internalBeam, kxBeam);
        std::cerr << "  done  " << kxBeam << "\n";
      } else {
        // In temp-dir (interpreter/REPL) mode keep the internal name so
        // -pa <tempDir> auto-loads it by module name.
        kxBeam = internalBeam;
      }

      if (compileRun) {
        // Use code:load_binary for explicit load (filename != module name).
        namespace fs = std::filesystem;
        std::string absBeam = fs::weakly_canonical(kxBeam).string();
        std::string loadExpr = "{ok,_B}=file:read_file(\"" + absBeam +
                               "\"), "
                               "code:load_binary('" +
                               result.moduleName + "',\"" + absBeam +
                               "\",_B), ";
        // Kex runtime errors carry a String (binary/charlist) reason
        // printed verbatim; anything else (raw BEAM errors like
        // badarg tuples) falls back to ~p.
        std::string reasonFmt =
            "case Reason of _R when is_binary(_R); is_list(_R) -> "
            "io:format(standard_error, \"Internal error: ~ts~n\", [_R]); "
            "_R -> io:format(standard_error, \"Internal error: ~p~n\", [_R]) "
            "end";
        std::string mainCall =
            result.mainArity == 1
                ? "try " + loadExpr + "'" + result.moduleName +
                      "':main([unicode:characters_to_binary(A) || A <- "
                      "init:get_plain_arguments()]) of Result -> halt() catch "
                      "_:Reason:_ -> " +
                      reasonFmt + ", halt(1) end"
                : "try " + loadExpr + "'" + result.moduleName +
                      "':main() of Result -> halt() catch _:Reason:_ -> " +
                      reasonFmt + ", halt(1) end";
        // shellSingleQuote (see its own comment) wraps the whole
        // -eval text as one shell argument, so quote characters
        // embedded in it survive into erl correctly regardless of
        // where they land (spec/json_parser.spec.kex).
        std::string runCmd = "erl -noshell -pa " + outputDir + " -eval " +
                             shellSingleQuote(mainCall);
        if (result.mainArity == 1 && !scriptArgs.empty()) {
          runCmd += " -extra";
          for (const auto &a : scriptArgs)
            runCmd += " " + a;
        }
        int ret = std::system(runCmd.c_str());
        if (!tempDir.empty())
          std::filesystem::remove_all(tempDir);
        return ret;
      }
      return 0;
    }

    if (mode == "run" && !skipCheck) {
      if (!runSemanticCheck(program, filepath)) {
        std::cerr
            << kex::color::apply(kex::color::bold)
            << kex::color::apply(kex::color::magenta)
            << "Aborted:" << kex::color::apply(kex::color::reset)
            << " fix type errors before running (use --no-check to skip).\n";
        return 1;
      }
    }

    if (mode == "run") {
      kex::interpreter::Evaluator evaluator;
      evaluator.setArgs(scriptArgs);
      {
        namespace fs = std::filesystem;
        auto srcDir = fs::weakly_canonical(filepath).parent_path().string();
        std::vector<std::string> roots;
        for (const auto &r : {"lib", "src"}) {
          auto full = srcDir + "/" + r;
          if (fs::is_directory(full)) roots.push_back(full);
        }
        if (!roots.empty()) evaluator.setModuleRoots(std::move(roots));
      }

      // The Kex-written stdlib is loaded by the Evaluator's constructor
      // (loadPrelude), so no explicit load is needed here.

      // Must outlive `evaluator.execute(program)` below: the
      // evaluator keeps raw `const ast::FunctionDef*` pointers into
      // whatever Program owns these nodes (see m_functionDefs), so a
      // Program that goes out of scope before the evaluator is done
      // leaves those pointers dangling — declaring it here, not
      // inside the loop, keeps it alive for the rest of this block.
      kex::ast::Program declarationsOnly;
      for (const auto &candidate : specBaseCandidates(filepath)) {
        if (!fileExists(candidate))
          continue;

        auto baseSource = readFile(candidate);
        kex::Lexer baseLexer(std::move(baseSource), candidate);
        auto baseTokens = baseLexer.tokenizeAll();
        kex::Parser baseParser(std::move(baseTokens), candidate);
        auto baseProgram = baseParser.parseProgram();

        declarationsOnly.items.reserve(baseProgram.items.size());
        for (auto &item : baseProgram.items) {
          if (!std::holds_alternative<std::unique_ptr<kex::ast::MainBlock>>(
                  item)) {
            declarationsOnly.items.push_back(std::move(item));
          }
        }
        evaluator.execute(declarationsOnly);
        break;
      }

      auto result = evaluator.execute(program);
      if (auto *i = std::get_if<kex::interpreter::IntValue>(&result->data))
        return static_cast<int>(i->value);
      return 0;
    }

    // mode == "check"
    // Pass 1+2: collect symbols and resolve names via SemanticDB
    kex::semantic::SemanticDB db;
    loadPrelude(db);
    db.updateFile(filepath, readFile(filepath));

    // Pass 3+: existing Analyzer (purity, type checking)
    kex::semantic::Analyzer analyzer;
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

#ifdef KEX_PRELUDE_DIR
    for (const auto &d : sealViolations(program, filepath)) {
      allDiags.push_back(d);
      dbOk = false;
    }
#endif

    bool allOk = ok && dbOk;

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
        if (!hint.empty())
          std::cout << ",\n    \"hint\": \"" << jsonEscape(hint) << "\"";
        std::cout << "\n  }" << (i + 1 < allDiags.size() ? "," : "") << "\n";
      }
      std::cout << "]\n";
      return allOk ? 0 : 1;
    }

    if (summaryMode) {
      // Output the public API signatures of the file in Kex syntax.
      // Walk the AST for TypeAnnotation + FunctionDef nodes; group by module.
      std::string currentModule;
      bool inModule = false;
      auto closeModule = [&]() {
        if (inModule) {
          std::cout << "end\n";
          inModule = false;
        }
      };
      auto openModule = [&](const std::string &name, bool isFoul) {
        closeModule();
        std::cout << (isFoul ? "foul " : "") << "module " << name << " do\n";
        inModule = true;
        currentModule = name;
      };

      for (const auto &item : program.items) {
        std::visit(
            [&](const auto &ptr) {
              using T = std::decay_t<decltype(*ptr)>;
              if constexpr (std::is_same_v<T, kex::ast::TypeAnnotation>) {
                if (inModule)
                  std::cout << "  ";
                std::cout << (ptr->isFoul ? "foul " : "") << ptr->name << " : ";
                if (ptr->type)
                  std::cout << typeExprToString(*ptr->type);
                std::cout << "\n";
              } else if constexpr (std::is_same_v<T, kex::ast::ModuleDef>) {
                openModule(ptr->name, ptr->isFoul);
                for (const auto &mitem : ptr->body) {
                  std::visit(
                      [&](const auto &mptr) {
                        using MT = std::decay_t<decltype(*mptr)>;
                        if constexpr (std::is_same_v<
                                          MT, kex::ast::TypeAnnotation>) {
                          std::cout << "  " << (mptr->isFoul ? "foul " : "")
                                    << mptr->name << " : ";
                          if (mptr->type)
                            std::cout << typeExprToString(*mptr->type);
                          std::cout << "\n";
                        }
                      },
                      mitem);
                }
                closeModule();
              }
            },
            item);
      }
      closeModule();
      return allOk ? 0 : 1;
    }

    // Normal colored output
    auto printDiag = [&](const kex::semantic::Diagnostic &diag) {
      bool isError = diag.level == kex::semantic::Diagnostic::Level::Error;
      std::cerr << kex::color::apply(kex::color::gray) << diag.location.file
                << ":" << diag.location.line << ":" << diag.location.column
                << ":" << kex::color::apply(kex::color::reset) << " "
                << kex::color::apply(kex::color::bold)
                << (isError ? kex::color::apply(kex::color::red)
                            : kex::color::apply(kex::color::magenta))
                << (isError ? "error" : "warning") << ":"
                << kex::color::apply(kex::color::reset) << " "
                << colorizeMessage(diag.message) << "\n";
    };
    for (const auto &d : allDiags)
      printDiag(d);

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
                  return la.column < lb.column;
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
