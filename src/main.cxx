#include "beam/beam_file.hxx"
#include "beam/collect_metadata.hxx"
#include "beam/etf.hxx"
#include "beam/kexi.hxx"
#include "beam/kexi_registry.hxx"
#include "cache/cache.hxx"
#include "cache/separate_unit.hxx"
#include "beam/term_builder.hxx"
#include "ast/clone.hxx"
#include "ast/convert.hxx"
#include "ast/syntax.hxx"
#include "common/artifact_versions.hxx"
#include "common/prelude_tiers.hxx"
#include "common/color.hxx"
#include "interpreter/evaluator.hxx"
#include "hir/hir.hxx"
#include "hir/repl_input.hxx"
#include "ir/emit_core.hxx"
#include "ir/pass_manager.hxx"
#include "driver/pipeline.hxx"
#include "ir/lower.hxx"
#include "lexer/lexer.hxx"
#include "compiled/expand.hxx"
#include "module/discovery.hxx"
#include "module/resolver.hxx"
#include "parser/parser.hxx"
#include "semantic/analyzer.hxx"
#include "semantic/db.hxx"
#include "validation/tag_validator.hxx"
#include <algorithm>
#include <cctype>
#include <deque>
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
#include "common/diagnostics.hxx"
#include "common/digest.hxx"
#include "common/file_io.hxx"
#ifdef KEX_HAS_LSP
#include "lsp/server.hxx"
#endif
#include "common/prelude_interfaces.hxx"
#include "common/prelude_loader.hxx"
#include "common/process.hxx"
#include "common/repl_commands.hxx"
#include "common/string_utils.hxx"
#include "common/timings.hxx"
#include "common/well_known.hxx"
#include "semantic/completion_at.hxx"
#include <functional>
#include <thread>

using kex::process::BeamVm;
using kex::process::beamTestGate;
using kex::process::exitStatusOf;
using kex::process::runShellCommand;
using kex::process::runShellCommandsTogether;
using kex::process::shellSingleQuote;
using kex::readFile;
using kex::diagnostics::colorizeMessage;
using kex::diagnostics::errorCountPhrase;
using kex::diagnostics::extractHint;
using kex::diagnostics::jsonEscape;
using kex::diagnostics::printCollapseReport;
using kex::diagnostics::printSemanticDiagnostic;
using kex::diagnostics::typeExprToString;
using kex::module::collectUsingModules;
using kex::module::declaredModuleNames;
using kex::module::fileDeclaringModule;
using kex::module::moduleHeaderOf;
using kex::module::moduleNameForPath;
using kex::module::shownPath;
using kex::module::skippedByModuleWalk;
using kex::module::standardModuleRoots;
using kex::snakeCased;

// What Tab and `/complete` complete against: the program each REPL builds to
// type-check an input — its top-level definitions, then `main do` with the
// local bindings replayed — with the input being typed spliced in, so the
// receiver of `x.` is typed by the analyzer exactly as the editor types it.
struct ReplCompletionContext {
  std::string definitions;
  std::string bindings;
};
static std::function<ReplCompletionContext()> g_replCompletionContext;
// The index members are listed from (prelude + the session's definitions),
// and the one the spliced program is analyzed through.
static kex::semantic::SemanticDB *g_replCompletionDb = nullptr;
static kex::semantic::SemanticDB *g_replAnalysisDb = nullptr;
// Earlier lines of a multi-line input still being typed (`make X do` …), so a
// line inside a block completes with the block around it.
static std::string g_replPendingInput;

auto preludeSemanticInterfaces() -> const kex::semantic::ImportedInterfaces &;

// Whether an input is a top-level declaration rather than code for `main`.
static auto replInputIsDeclaration(const std::string &input) -> bool {
  size_t i = input.find_first_not_of(" \t\n");
  if (i == std::string::npos) return false;
  size_t end = i;
  while (end < input.size() &&
         (std::isalnum((unsigned char)input[end]) || input[end] == '_'))
    ++end;
  const auto word = input.substr(i, end - i);
  for (const char *keyword : {"module", "capability", "type", "distinct",
                              "record", "trait", "make", "serving",
                              "compiled", "using"})
    if (word == keyword) return true;
  if (word != "let" && word != "foul") return false;
  // `let f(x) = …` defines a function; `let x = …` binds a local.
  i = input.find_first_not_of(" \t", end);
  if (i == std::string::npos || !std::islower((unsigned char)input[i]))
    return false;
  while (i < input.size() &&
         (std::isalnum((unsigned char)input[i]) || input[i] == '_' ||
          input[i] == '?' || input[i] == '!'))
    ++i;
  i = input.find_first_not_of(" \t", i);
  return i != std::string::npos && input[i] != '=';
}

auto moduleRootsFor(const std::string &filepath) -> std::vector<std::string>;

// The modules under the REPL's source roots, by the name each file declares:
// what a dotted name typed at the prompt can lead to. Read once a session —
// the roots are fixed when it starts — and only each file's header, so `DB.`
// costs no parsing.
static auto replRootModules() -> const std::vector<std::string> & {
  static const std::vector<std::string> modules = [] {
    namespace fs = std::filesystem;
    std::set<std::string> found;
    const auto standard = standardModuleRoots();
    const auto roots =
        moduleRootsFor((fs::current_path() / "<repl>").string());
    for (const auto &root : roots) {
      std::error_code ec;
      if (standard.contains(fs::weakly_canonical(root, ec).string())) continue;
      fs::recursive_directory_iterator walk(
          root, fs::directory_options::skip_permission_denied, ec);
      for (; !ec && walk != fs::recursive_directory_iterator(); walk.increment(ec)) {
        const auto &entry = *walk;
        const auto name = entry.path().filename().string();
        if (entry.is_directory(ec)) {
          if (skippedByModuleWalk(name)) walk.disable_recursion_pending();
          continue;
        }
        if (entry.path().extension() != ".kex" || name.ends_with(".spec.kex"))
          continue;
        if (const auto header = moduleHeaderOf(entry.path());
            header && !header->name.empty())
          found.insert(header->name);
      }
    }
    return std::vector<std::string>(found.begin(), found.end());
  }();
  return modules;
}

// Completions for a dotted name the REPL's own index cannot know: a module
// under a source root. `qualified` is the path up to the word being typed
// (`DB.Repositories.Users.`, or "" for a bare word). The module it names is
// loaded into the index the first time, so its members complete as the
// prelude's do; and the next part of every module whose name it begins
// (`DB.` offers `Repositories`, `Schema`, `Store`) is offered too.
static auto replRootCompletions(const std::string &qualified,
                                const std::string &word)
    -> std::vector<std::string> {
  std::vector<std::string> matches;
  if (!g_replCompletionDb) return matches;
  if (!qualified.empty()) {
    static std::set<std::string> loaded;
    const auto module = qualified.substr(0, qualified.size() - 1);
    if (loaded.insert(module).second) {
      kex::module::Resolver resolver(
          moduleRootsFor((std::filesystem::current_path() / "<repl>").string()));
      if (const auto resolved = resolver.resolve(module)) {
        std::ifstream input(resolved->path);
        std::stringstream text;
        text << input.rdbuf();
        // As SemanticDB::ensureModule loads one: for its interface, without
        // resolving its own names (kexhq/kex#323).
        g_replCompletionDb->updateFile(resolved->path, text.str(), {},
                                       /*resolveNames=*/false);
      }
    }
    for (auto &match : g_replCompletionDb->completionsFor(qualified + word))
      if (match.rfind(qualified, 0) == 0)
        matches.push_back(match.substr(qualified.size()));
  }
  for (const auto &module : replRootModules()) {
    if (module.rfind(qualified + word, 0) != 0) continue;
    const auto rest = module.substr(qualified.size());
    matches.push_back(rest.substr(0, rest.find('.')));
  }
  return matches;
}

// Completions for `line` at byte offset `point`. `replaceFrom` is where, in
// `line`, the word each match replaces starts.
static auto replCompleteAt(const std::string &line, size_t point)
    -> kex::semantic::CompletionAt {
  if (!g_replCompletionDb || !g_replAnalysisDb) return {};
  const auto context = g_replCompletionContext ? g_replCompletionContext()
                                               : ReplCompletionContext{};
  const auto input = g_replPendingInput.empty()
                         ? line
                         : g_replPendingInput + "\n" + line;
  const bool declaration = replInputIsDeclaration(input);
  auto completion = kex::semantic::completeReplInput(
      *g_replCompletionDb, *g_replAnalysisDb, context.definitions,
      context.bindings, input, line, point, declaration,
      &preludeSemanticInterfaces());
  // A name that is not an expression's member — `DB.`, `DB.Repositories.Us`,
  // `Conf` — may be a module under a source root the index has not loaded.
  if (completion.qualifier.empty()) {
    const auto isWord = [](unsigned char c) {
      return std::isalnum(c) || c == '_' || c == '?' || c == '!';
    };
    const auto end = std::min(point, line.size());
    size_t wordStart = end;
    while (wordStart > 0 && isWord(static_cast<unsigned char>(line[wordStart - 1])))
      --wordStart;
    size_t pathStart = wordStart;
    while (pathStart > 0 &&
           (isWord(static_cast<unsigned char>(line[pathStart - 1])) ||
            line[pathStart - 1] == '.'))
      --pathStart;
    const auto qualified = line.substr(pathStart, wordStart - pathStart);
    const auto word = line.substr(wordStart, end - wordStart);
    // Only a module path: a lower-case part before the word (`users.`) is a
    // value, which the analyzer has already answered for.
    const bool modulePath = qualified.empty()
        ? !word.empty() && std::isupper(static_cast<unsigned char>(word[0]))
        : std::isupper(static_cast<unsigned char>(qualified[0]));
    if (modulePath) {
      for (auto &match : replRootCompletions(qualified, word))
        completion.matches.push_back(std::move(match));
      std::sort(completion.matches.begin(), completion.matches.end());
      completion.matches.erase(
          std::unique(completion.matches.begin(), completion.matches.end()),
          completion.matches.end());
    }
  }
  return completion;
}

// `/complete <prefix>`: each completed word, in full.
static auto printReplCompletions(const std::string &prefix) -> void {
  const auto completion = replCompleteAt(prefix, prefix.size());
  if (completion.matches.empty()) {
    std::cout << "  (no completions for \"" << prefix << "\")\n";
    return;
  }
  for (const auto &match : completion.matches)
    std::cout << "  " << prefix.substr(0, completion.replaceFrom) << match
              << "\n";
}

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

auto preludeSemanticInterfaces() -> const kex::semantic::ImportedInterfaces &;

// Whether `type` — as the REPL displays it — is usable as the annotation of a
// replayed binding: it must parse and check as `let x : <type> = <untyped>`
// against the session's definitions. Displayed types are not always source
// (`Unknown`, free type variables), and a replay line that failed to check
// would break every later input, so anything doubtful is refused.
static auto replTypeAnnotates(const std::string &definitions,
                              const std::string &type) -> bool {
  if (type.empty() || type.find("Unknown") != std::string::npos) return false;
  try {
    const auto source = definitions + "main do\n  let __replTyped : " + type +
                        " = BEAM.erlang.get(:kexreplprobe)\n" +
                        "  __replTyped\nend\n";
    kex::Lexer lexer(source, "<repl>");
    kex::Parser parser(lexer.tokenizeAll(), "<repl>");
    auto program = parser.parseProgram();
    if (!parser.diagnostics().empty()) return false;
    kex::semantic::Analyzer analyzer(&preludeSemanticInterfaces());
    return analyzer.analyze(program);
  } catch (...) {
    return false;
  }
}

// `compiled do … end` declares what its body generates, so a REPL input that
// is one is a definition, not an expression to wrap in `main`.
static auto isReplCompiledBlock(const std::string &source) -> bool {
  if (source.rfind("compiled", 0) != 0) return false;
  auto after = source.find_first_not_of(" \t", 8);
  return after != 8 && after != std::string::npos &&
         source.compare(after, 2, "do") == 0 &&
         (after + 2 == source.size() ||
          !std::isalnum(static_cast<unsigned char>(source[after + 2])));
}

static auto replDefinitionName(const std::string &source) -> std::string {
  // A `compiled do` block names nothing itself, but the BEAM REPL keys its
  // session definitions by name, so each block gets a number of its own.
  if (isReplCompiledBlock(source)) {
    static int blocks = 0;
    return "compiled block " + std::to_string(++blocks);
  }
  size_t off = 0;
  if (source.rfind("foul module ", 0) == 0) off = 12;
  else if (source.rfind("foul ", 0) == 0) off = 5;
  else if (source.rfind("let ", 0) == 0) off = 4;
  else if (source.rfind("type ", 0) == 0) off = 5;
  else if (source.rfind("record ", 0) == 0) off = 7;
  else if (source.rfind("module ", 0) == 0) off = 7;
  else if (source.rfind("make ", 0) == 0) off = 5;
  else if (source.rfind("using ", 0) == 0) off = 6;
  else if (source.rfind("serving ", 0) == 0) {
    // Keyed apart from the record it serves: both would otherwise be
    // "Entries", and defining the protocol would drop the record itself.
    // A second `serving Entries` still replaces the first.
    off = 8;
    while (off < source.size() && std::isspace((unsigned char)source[off])) off++;
    size_t end = off;
    while (end < source.size() &&
           (std::isalnum((unsigned char)source[end]) || source[end] == '_' ||
            source[end] == '.'))
      end++;
    return "serving " + source.substr(off, end - off);
  }
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

// Whether `source[at, at + length)` is a whole word: no letter or digit on
// either side.
static auto replWholeWordAt(const std::string &source, size_t at,
                            size_t length) -> bool {
  const auto wordChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
  };
  return (at == 0 || !wordChar(source[at - 1])) &&
         (at + length >= source.size() || !wordChar(source[at + length]));
}

// Unmatched `do` minus `end` keywords (whole words) in `source`: how many
// blocks an input still has open, so a REPL keeps reading while it is
// positive. Shared by the BEAM and the tree-walk REPL.
static auto replBlockDepth(const std::string &source) -> int {
  int depth = 0;
  for (size_t i = 0; i < source.size(); i++) {
    if (source.compare(i, 2, "do") == 0 && replWholeWordAt(source, i, 2))
      depth++;
    if (source.compare(i, 3, "end") == 0 && replWholeWordAt(source, i, 3))
      depth--;
  }
  return depth;
}

#ifdef HAS_READLINE
#include <readline/history.h>
#include <readline/readline.h>

// Completion state — populated before the REPL loop, read by the C callback.

static std::vector<std::string> g_completionMatches;

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
  if (state < static_cast<int>(g_completionMatches.size()))
    return strdup(g_completionMatches[state].c_str());
  return nullptr;
}
static char **kexCompletion(const char *text, int start, int end) {
  rl_attempted_completion_over = 1;
  rl_completion_suppress_append = 1; // no trailing space/quote after completion

  if (start == 0 && text[0] == '/') {
    g_completionMatches = kex::replCommandCompletions(text);
    return rl_completion_matches(text, kexCompletionEntry);
  }

  // readline's word starts at `start`; the completed word at `replaceFrom`
  // (after the dot of a member). Readline inserts whole words, so carry the
  // text between the two — `pa.` — in front of every match.
  const std::string line(rl_line_buffer);
  const auto completion = replCompleteAt(line, static_cast<size_t>(end));
  const auto from = std::max(completion.replaceFrom, static_cast<size_t>(start));
  const auto lead = line.substr(static_cast<size_t>(start),
                                from - static_cast<size_t>(start));
  g_completionMatches.clear();
  for (const auto &match : completion.matches)
    if (from == completion.replaceFrom)
      g_completionMatches.push_back(lead + match);
    else if (match.size() >= from - completion.replaceFrom)
      g_completionMatches.push_back(
          match.substr(from - completion.replaceFrom));

  static bool debugCompl = (std::getenv("KEX_DEBUG_COMPLETE") != nullptr);
  if (debugCompl) {
    fprintf(stderr, "\n[complete] linebuf=%s start=%d end=%d text=%s"
                    " -> qualifier=%s\n",
            rl_line_buffer, start, end, text, completion.qualifier.c_str());
    for (const auto &c : g_completionMatches)
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

// `--eval` source, one line at a time: the REPL reads these before it asks
// the terminal for anything, so a boot script is entered exactly as if it had
// been typed — multi-line blocks included (kexhq/kex#549).
std::deque<std::string> g_replScript;
// Whether the line just read came from `g_replScript`. An input that fails
// while booting ends the session instead of leaving a half-booted REPL.
bool g_replLineFromScript = false;
bool g_replScriptFailed = false;

auto readLine(const std::string &prompt) -> std::pair<std::string, bool> {
  g_replInputCancelled = false;
  g_replLineFromScript = !g_replScript.empty();
  if (g_replLineFromScript) {
    auto line = std::move(g_replScript.front());
    g_replScript.pop_front();
    return {line, true};
  }
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

namespace {

// Additional project roots supplied by repeatable `--source-root` options.
// Keeping this at the CLI boundary lets every existing consumer of
// moduleRootsFor (semantic checks, discovery, validation and both runtimes)
// observe exactly the same search path.
std::vector<std::string> cliSourceRoots;
// `--quiet` and `--progress`: whether a build shows its steps (BuildProgress).
bool quietBuild = false;
bool progressRequested = false;

// `--sname` / `--name` / `--cookie`: start the BEAM VM as a distributed node,
// so the program can connect to others (`Node.connect`) and they to it. They
// become erl's own `-sname`/`-name`/`-setcookie` on every VM kex launches.
std::vector<std::string> cliDistributionArgs;

auto distributionArgsForShell() -> std::string {
  std::string out;
  for (const auto &arg : cliDistributionArgs)
    out += " " + shellSingleQuote(arg);
  return out;
}

// What erl would reject as a node name, said before the VM is launched: left
// to erl, a bad name kills the kernel during boot and the user gets a crash
// dump instead of a reason. Empty when +name+ is acceptable.
auto nodeNameProblem(const std::string &name, bool shortName) -> std::string {
  const auto at = name.find('@');
  const std::string local = name.substr(0, at);
  const std::string host = at == std::string::npos ? "" : name.substr(at + 1);
  const bool localOk =
      !local.empty() && std::all_of(local.begin(), local.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
               c == '-';
      });
  if (!localOk) {
    std::string why = "the part before '@' may only contain letters, digits, "
                      "'_' and '-'";
    if (shortName && at == std::string::npos &&
        name.find('.') != std::string::npos)
      why += " (for a short name, drop the domain: '" +
             name.substr(0, name.find('.')) + "')";
    return why;
  }
  if (shortName && host.find('.') != std::string::npos)
    return "a short name's host cannot contain '.'; use --name for a "
           "fully qualified host";
  return "";
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

// `context` with the checked program `hir` filled in — what each call is and
// what type each expression has, everything lowering reads off a checked
// program. Unchanged without one.
auto withCheckerAnswers(kex::ir::LowerContext context,
                        const kex::hir::Program *hir) -> kex::ir::LowerContext {
  if (hir) {
    const auto &analysis = hir->analysis();
    context.hir = hir;
    context.resolvedCalls = &analysis.resolvedCalls();
    context.staticTypeOfCalls = &analysis.staticTypeOfCalls();
    context.expressionTypes = &analysis.typeMap();
    context.resolvedTraits = &analysis.resolvedTraits();
  }
  return context;
}

auto fileExists(const std::string &path) -> bool {
  std::ifstream probe(path);
  return probe.good();
}

// `file`'s declarations as a companion contributes them: everything but its
// own `main` block(s). `text` is the file's source when the caller already
// has it. Parse errors are left to whatever checks `file` itself.
auto declarationsOf(const std::string &file, std::string text)
    -> std::vector<kex::ast::TopLevelItem> {
  kex::Lexer lexer(std::move(text), file);
  kex::Parser parser(lexer.tokenizeAll(), file);
  auto parsed = parser.parseProgram();
  std::vector<kex::ast::TopLevelItem> declarations;
  for (auto &item : parsed.items)
    if (kex::ast::isMergedDeclaration(item))
      declarations.push_back(std::move(item));
  return declarations;
}

auto declarationsOf(const std::string &file)
    -> std::vector<kex::ast::TopLevelItem> {
  return declarationsOf(file, readFile(file));
}

// Puts `items` ahead of `program`'s own, in order — where every merged-in
// declaration goes, so the entry's own items stay last.
auto prependItems(kex::ast::Program &program,
                  std::vector<kex::ast::TopLevelItem> items) -> void {
  items.reserve(items.size() + program.items.size());
  for (auto &item : program.items) items.push_back(std::move(item));
  program.items = std::move(items);
}

// Prepends each file's declarations, the first file's first.
auto prependDeclarationsOf(kex::ast::Program &program,
                           const std::vector<std::string> &files) -> void {
  std::vector<kex::ast::TopLevelItem> merged;
  for (const auto &file : files)
    for (auto &item : declarationsOf(file)) merged.push_back(std::move(item));
  prependItems(program, std::move(merged));
}

// Merges the entry's companion — a spec's base file, a manifest's vocabulary
// (specBaseCandidates) — into `program`. Returns the file merged, or "" when
// there is none; `rawSource`, when given, receives its text.
auto mergeSpecBase(kex::ast::Program &program, const std::string &entry,
                   std::string *rawSource = nullptr) -> std::string {
  for (const auto &candidate : specBaseCandidates(entry)) {
    if (!fileExists(candidate)) continue;
    auto text = readFile(candidate);
    if (rawSource) *rawSource = text;
    prependItems(program, declarationsOf(candidate, std::move(text)));
    return candidate;
  }
  return "";
}

// Defined with the prelude's other loaders below; lowering a unit on its own
// (lowerUnitSeparately) needs them first.
auto preludeExternalModules() -> const kex::ir::ExternalModules &;
auto mergeExternalModules(const kex::ir::ExternalModules &base,
                          const kex::ir::ExternalModules &overrides)
    -> kex::ir::ExternalModules;
auto loadPreludeRecordLayouts() -> std::vector<kex::ir::ExternalRecordLayout>;
auto loadPreludeVariantTags() -> std::vector<kex::ir::ExternalVariantTag>;
auto prebuiltRuntimeBeamDir() -> std::string;
auto sourceMentionsCompiledBlock(const std::string &src) -> bool;
auto sourceMentionsEmbed(const std::string &src) -> bool;

namespace {

struct LoadedDep {
  std::unique_ptr<std::string> source;
  std::unique_ptr<std::string> path;
  std::unique_ptr<kex::ast::Program> program;
};

// An error about a module's file, at its header.
// Whether a dependency that cannot be loaded ends the process (a build) or
// is thrown as an error (a REPL, where `/load` and each input catch it). An
// interactive session used to die with the build's `exit(1)` the moment a
// `/load`-ed file used a module with a syntax error.
bool g_dependencyFailureThrows = false;

[[noreturn]] auto dependencyFailure(const std::string &what) -> void {
  if (g_dependencyFailureThrows) throw std::runtime_error(what);
  std::exit(1);
}

auto reportModuleFileError(const std::filesystem::path &file, int line,
                           const std::string &message) -> void {
  std::cerr << kex::color::apply(kex::color::gray) << shownPath(file) << ":"
            << line << ":1:" << kex::color::apply(kex::color::reset) << " "
            << kex::color::apply(kex::color::bold)
            << kex::color::apply(kex::color::red) << "error:"
            << kex::color::apply(kex::color::reset) << " " << message << "\n";
}

// ── Dependency-discovery cache (kexhq/kex#323) ──────────────────────────────
//
// Which modules a dependency file refers to is found by analyzing that file on
// its own, against the toolchain's prelude and stdlib interfaces; nothing else
// feeds the answer. So it is a pure function of the file's bytes and the
// toolchain, and a warm build reads it back instead of re-analyzing every
// dependency — 0.4 s of a 2.3 s warm build of Tey. The toolchain part covers
// the stdlib sources as well, which a checkout can change without a new
// revision.
//
// The toolchain part (kex::cache::toolchainFingerprint) covers the compiler
// binary itself as well (kexhq/kex#372).

// Where an expansion keeps template scans between builds, and for which
// toolchain (kexhq/kex#323); left empty when every cache is off.
auto setScanCache(kex::compiled::ExpandOptions &options) -> void {
  if (const auto base = kex::cache::base()) {
    options.scanCache = *base;
    options.toolchainKey = kex::cache::toolchainFingerprint();
  }
}

// The same for every expansion that builds its own options: a module the
// semantic pass or a compile-time sandbox expands.
auto setDefaultScanCache() -> void {
  if (const auto base = kex::cache::base())
    kex::compiled::setDefaultScanCache(*base, kex::cache::toolchainFingerprint());
}

auto discoveryCacheEntry(const std::string &source)
    -> std::optional<std::filesystem::path> {
  const auto base = kex::cache::base();
  if (!base) return std::nullopt;
  const std::string material =
      "kex-deps-cache-v1\n" + kex::cache::toolchainFingerprint() + "\n" + source;
  const auto key = kex::sha256Hex(material);
  return *base / "deps" / key.substr(0, 2) / (key + ".txt");
}

// A cached module list, or nullopt when there is no complete entry. The first
// line marks a finished write, so no partial file ever reads as "refers to
// nothing".
auto readDiscoveryCache(const std::filesystem::path &entry)
    -> std::optional<std::vector<std::string>> {
  std::ifstream input(entry);
  if (!input) return std::nullopt;
  std::string line;
  if (!std::getline(input, line) || line != "kex-deps") return std::nullopt;
  std::vector<std::string> modules;
  while (std::getline(input, line))
    if (!line.empty()) modules.push_back(line);
  return modules;
}

// Written beside the final name and renamed into place, so two builds sharing
// the cache never see half an entry; a failed write only means the next build
// analyzes that file again.
auto writeDiscoveryCache(const std::filesystem::path &entry,
                         const std::vector<std::string> &modules) -> void {
  std::string text = "kex-deps\n";
  for (const auto &module : modules) text += module + "\n";
  kex::cache::atomicWrite(entry, text);
}

// ── Build progress (kexhq/kex#586) ───────────────────────────────────────────
//
// What a long build is doing, as it does it: Slick's `tey build` printed
// nothing for 17 minutes, and KEX_TIMINGS could only say afterwards which
// phase had taken them. One stderr line per step, written BEFORE the step
// starts, so the last line on screen is what is running now: a stuck build
// names the file it is stuck on. Lines, not a status line redrawn in place —
// Tey reads this through a pipe, where `\r` redraws nothing.
//
//   [  0.0s]    Resolving main.kex
//   [  1.5s]    Expanding app/live/conversations.kex
//   [ 74.9s]     Checking 160 modules
//   [ 96.6s]     Lowering 160 modules
//   [101.0s]    Compiling Kex.App.Live.Conversations
//   [154.6s]     Finished ebin/kex_main.beam in 2m 34.6s
//
// On by default for `kex -c`; `--progress` turns it on elsewhere (a spec run,
// `kex file.kex`), and `--quiet` off, together with erlc's own lines.
struct BuildProgress {
  using Clock = std::chrono::steady_clock;
  bool enabled = false;
  Clock::time_point start = Clock::now();
  // A dependency is resolved by the gating pre-check and again for the real
  // build; the second pass is quick and would only repeat the first's lines.
  std::unordered_set<std::string> expanded;

  auto elapsed() const -> double {
    return std::chrono::duration<double>(Clock::now() - start).count();
  }

  auto step(const char *verb, const char *colour, const std::string &what) const
      -> void {
    if (!enabled) return;
    namespace c = kex::color;
    std::fprintf(stderr, "%s[%6.1fs]%s %s%s%12s%s %s\n", c::apply(c::gray),
                 elapsed(), c::apply(c::reset), c::apply(c::bold),
                 c::apply(colour), verb, c::apply(c::reset), what.c_str());
    std::fflush(stderr);
  }

  // Once per dependency file, however many passes resolve it. A standard
  // library file is named from the library, not by the `../../..` climb from
  // the working directory to wherever the toolchain is installed.
  auto expanding(const std::string &path) -> void {
    if (!enabled || !expanded.insert(path).second) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto absolute = fs::weakly_canonical(path, ec);
    for (const auto &root : kex::standardLibraryModuleRoots()) {
      const auto relative =
          fs::weakly_canonical(root, ec).lexically_normal();
      const auto inside = absolute.lexically_relative(relative);
      if (!ec && !inside.empty() && *inside.begin() != "..") {
        step("Expanding", kex::color::magenta, "stdlib/" + inside.string());
        return;
      }
    }
    step("Expanding", kex::color::magenta, path);
  }

  auto finished(const std::string &what) const -> void {
    if (!enabled) return;
    const auto total = elapsed();
    char took[32];
    const int minutes = static_cast<int>(total) / 60;
    if (minutes > 0)
      std::snprintf(took, sizeof took, "%dm %.1fs", minutes,
                    total - 60.0 * minutes);
    else
      std::snprintf(took, sizeof took, "%.1fs", total);
    namespace c = kex::color;
    step("Finished", kex::color::green,
         what + " " + c::apply(c::gray) + "in " + took + c::apply(c::reset));
  }
};

BuildProgress buildProgress;

// "1 module", "160 modules": what BuildProgress counts.
auto moduleCount(std::size_t count, const char *kind = "") -> std::string {
  return std::to_string(count) + " " + kind + (count == 1 ? "module" : "modules");
}

// The program as units (docs/plan_separate_compilation.md): its files
// grouped into the strongly connected components of what each one uses, so
// that a cycle of modules is one unit and every other module one of its
// own. In build order — a unit comes after every unit it uses — with the
// entry program, written as the empty path, last.
struct UnitGraph {
  std::map<std::string, std::set<std::string>> uses;
  std::vector<std::vector<std::string>> units;
};

auto unitsOf(const std::map<std::string, std::set<std::string>> &uses)
    -> std::vector<std::vector<std::string>> {
  // Tarjan's algorithm: a component is complete only after every component
  // it reaches, which is the order a build needs.
  std::vector<std::vector<std::string>> units;
  std::map<std::string, int> index, low;
  std::set<std::string> onStack;
  std::vector<std::string> stack;
  int next = 0;
  std::function<void(const std::string &)> visit = [&](const std::string &file) {
    index[file] = low[file] = next++;
    stack.push_back(file);
    onStack.insert(file);
    if (const auto found = uses.find(file); found != uses.end())
      for (const auto &used : found->second) {
        if (!index.contains(used)) {
          visit(used);
          low[file] = std::min(low[file], low[used]);
        } else if (onStack.contains(used)) {
          low[file] = std::min(low[file], index[used]);
        }
      }
    if (low[file] != index[file]) return;
    std::vector<std::string> unit;
    std::string member;
    do {
      member = stack.back();
      stack.pop_back();
      onStack.erase(member);
      unit.push_back(member);
    } while (member != file);
    std::sort(unit.begin(), unit.end());
    units.push_back(std::move(unit));
  };
  // From the entry: every file is reached from it, and it is the last unit.
  visit("");
  for (const auto &[file, _] : uses)
    if (!index.contains(file)) visit(file);
  return units;
}

// KEX_SEPARATE=1, phase 2 of docs/plan_separate_compilation.md: every unit
// checked on its own, in build order, against the interfaces of the units
// below it — the KexI its modules would carry once compiled, read back the
// way a compiled unit's are. The merged check still decides the build; this
// one prints what it found, `separate:` before each line, so the two can be
// compared until they agree.
auto separateCheckRequested() -> bool {
  const char *value = std::getenv("KEX_SEPARATE");
  return value && *value && std::string(value) != "0";
}

// An emitted module's routed implementations, as recordImplementations asks.
auto routedIn(const std::set<std::pair<std::string, int>> &routed)
    -> std::function<bool(const std::string &, int)> {
  return [&routed](const std::string &function, int arity) {
    return routed.count({function, arity}) > 0;
  };
}

// KEX_SEPARATE_BUILD=1 with KEX_SEPARATE=1, phase 3: the build's modules are
// its units' own, each lowered apart, whenever every unit checked and lowered;
// otherwise the merged build's, as without it.
auto separateBuildRequested() -> bool {
  const char *value = std::getenv("KEX_SEPARATE_BUILD");
  return separateCheckRequested() && value && *value && std::string(value) != "0";
}

// The entry file being built, which names the entry unit's file-local module
// as the merged build names it.
std::string g_separateEntryPath;

// Every unit's modules, set when each unit was lowered apart and a separate
// build was asked for.
std::optional<std::vector<kex::ir::EmitResult>> g_separateModules;
std::vector<std::string> g_separateDiagnostics;

// Phase 3: a checked unit lowered on its own, with its own analysis, against
// the prelude and the KexI of the units below it (`registry`, which holds only
// those yet). Its modules' Core Erlang, or nothing when lowering failed; with
// KEX_SEPARATE_CORE=<dir> also written to `dir`, to be compared with the
// merged build's.
auto lowerUnitSeparately(const kex::ast::Program &program, const std::string &path,
                         const kex::semantic::Analyzer &analyzer,
                         const kex::beam::KexiRegistry &registry,
                         const std::string &dir)
    -> std::optional<std::vector<kex::ir::EmitResult>> {
  // A module this build compiles is described by its own interface. The
  // prebuilt runtime has its own copy of an opt-in standard library module,
  // laid out as the prelude build lays it out (`make Router` filed under
  // `Kex.Net.HTTP.Router`, not `Kex.Net.HTTP`), and left in, it wins.
  std::set<std::string> builtModules, builtAtoms;
  for (const auto *module : registry.allLoadedModules()) {
    builtModules.insert(module->chunk.metadata.sourceModule);
    builtAtoms.insert(module->beamAtom);
  }
  auto prelude = preludeExternalModules();
  const auto builtKey = [&](const std::string &key) {
    const auto dot = key.rfind('.');
    return dot != std::string::npos && builtModules.contains(key.substr(0, dot));
  };
  std::erase_if(prelude.exportToBeamFn, [&](const auto &entry) { return builtKey(entry.first); });
  std::erase_if(prelude.exportArity, [&](const auto &entry) { return builtKey(entry.first); });
  std::erase_if(prelude.exportParamNames, [&](const auto &entry) { return builtKey(entry.first); });
  std::erase_if(prelude.foulExports, [&](const auto &key) { return builtKey(key); });
  for (auto &[_, functions] : prelude.receiverFunctions)
    std::erase_if(functions, [&](const auto &function) {
      return builtAtoms.contains(function.moduleAtom);
    });
  auto unitExternals = registry.buildExternalModules();
  for (auto &[_, functions] : unitExternals.receiverFunctions)
    for (auto &function : functions) function.compiledApart = true;
  for (const auto &[name, _] : unitExternals.nameToAtom) unitExternals.compiledApartModules.insert(name);
  auto externals = mergeExternalModules(prelude, unitExternals);
  // The merge lets a unit's providers of a name replace the prelude's, as a
  // module loaded into the REPL does; here both stand, or `handle.feed` lost
  // the prelude's foul FileHandle provider to FS.File's, and its context.
  for (const auto &[name, providers] : prelude.receiverFunctions) {
    auto &merged = externals.receiverFunctions[name];
    for (const auto &provider : providers)
      if (std::none_of(merged.begin(), merged.end(), [&](const auto &existing) {
            return existing.moduleAtom == provider.moduleAtom &&
                   existing.beamFunction == provider.beamFunction &&
                   existing.beamArity == provider.beamArity &&
                   existing.receiverType == provider.receiverType &&
                   existing.paramNames == provider.paramNames;
          }))
        merged.push_back(provider);
  }
  auto layouts = loadPreludeRecordLayouts();
  auto variants = loadPreludeVariantTags();
  // Where each type's `make` functions are, for the runtime's dispatch on a
  // value: its declaring module may have none, and another unit may.
  std::map<std::string, std::string> methodOwners;
  for (const auto *module : registry.allLoadedModules())
    for (const auto &method : module->chunk.typeInterface.methods)
      if (!method.moduleFunction && !method.typeOnly && method.receiverType &&
          method.receiverType->kind == kex::beam::KexiType::Named)
        methodOwners.try_emplace(method.receiverType->name, module->beamAtom);
  const auto ownerOf = [&](const std::string &type) {
    const auto found = methodOwners.find(type);
    return found == methodOwners.end() ? std::string{} : found->second;
  };
  // A type of a unit below that claims a trait has the trait's defaults
  // (`Foldable`'s `any?` on `Data.Set`) in the module holding its `make`
  // functions, which forwards them to where its unit emitted them. The merged
  // build had them in its own dispatcher; here they are receiver functions
  // for that type, at the arities of the prelude's generic ones.
  std::map<std::string, std::vector<std::string>> traitDefaults;
  for (const auto &trait : preludeSemanticInterfaces().traits)
    traitDefaults[trait.name] = trait.defaultMethods;
  for (const auto &trait : registry.buildSemanticInterfaces().traits)
    traitDefaults[trait.name] = trait.defaultMethods;
  for (const auto *module : registry.allLoadedModules())
    for (const auto &conformance : module->chunk.metadata.traitConformances) {
      const auto owner = ownerOf(conformance.typeName);
      const auto defaults = traitDefaults.find(conformance.traitName);
      if (owner.empty() || defaults == traitDefaults.end()) continue;
      for (const auto &name : defaults->second) {
        auto &providers = externals.receiverFunctions[name];
        std::set<int> arities;
        for (const auto &provider : providers)
          if (provider.moduleAtom == kex::names::kPreludeModule && provider.receiverType.empty())
            arities.insert(provider.beamArity);
        for (const auto arity : arities)
          if (std::none_of(providers.begin(), providers.end(), [&](const auto &provider) {
                return provider.moduleAtom == owner && provider.beamArity == arity &&
                       provider.receiverType == conformance.typeName;
              }))
            providers.push_back({owner, name, arity, {}, conformance.typeName, false, true});
      }
    }
  for (const auto *module : registry.allLoadedModules()) {
    for (const auto &record : module->chunk.metadata.records) {
      kex::ir::ExternalRecordLayout layout;
      layout.name = record.tagAtom.empty() ? record.name : record.tagAtom;
      layout.moduleAtom = module->beamAtom;
      layout.methodOwner = ownerOf(layout.name);
      layout.declaringModule = module->chunk.metadata.sourceModule;
      for (const auto &field : record.fields) {
        layout.fields.push_back(field.name);
        layout.fieldDefaults.push_back(field.defaultSource);
      }
      layouts.push_back(std::move(layout));
    }
    for (const auto &adt : module->chunk.metadata.adts)
      for (const auto &constructor : adt.constructors)
        variants.push_back(
            {constructor.tagAtom, constructor.arity, adt.name, ownerOf(adt.name)});
  }
  // Several units can have the same basename (`app/models/conversation.kex`
  // and `app/views/chat/conversation.kex`). Their file modules must not
  // overwrite one another. Keep the entry's traditional `kex_main` name.
  const auto baseStem = std::filesystem::path(path).stem().string();
  auto stem = baseStem;
  if (path != g_separateEntryPath) {
    stem += "_" + kex::sha256Hex(path).substr(0, 16);
  }
  try {
    const kex::hir::Program hir(program, analyzer);
    auto modules = kex::ir::lowerModules(
        program, stem,
        withCheckerAnswers({.sourcePath = path,
                            .externals = &externals,
                            .externalRecords = &layouts,
                            .externalVariants = &variants},
                           &hir));
    if (!dir.empty()) std::filesystem::create_directories(dir);
    // The unit's own module registers its records' layouts as it loads, but
    // only the entry's `main` used to run, and a call goes to a companion
    // (`Kex.RecordValue.Sessions`): its loading loads that module first, or
    // `Type.of(Session)` met a record nothing had registered.
    const auto fileModule = "kex_" + stem;
    const auto registers = std::any_of(modules.begin(), modules.end(), [&](const auto &module) {
      return module.name == fileModule && !module.onLoad.empty();
    });
    for (auto &module : modules) {
      if (!registers || module.name == fileModule || !module.onLoad.empty()) continue;
      auto load = std::make_unique<kex::ir::Expr>();
      std::vector<kex::ir::ExprPtr> args;
      args.push_back(std::make_unique<kex::ir::Expr>());
      args.back()->node = kex::ir::Lit{kex::ir::LitKind::Atom, fileModule};
      load->node = kex::ir::Call{"code", "ensure_loaded", 1, std::move(args), false};
      auto ok = std::make_unique<kex::ir::Expr>();
      ok->node = kex::ir::Lit{kex::ir::LitKind::Atom, "ok"};
      auto body = std::make_unique<kex::ir::Expr>();
      body->node = kex::ir::Let{"_Loaded", std::move(load), std::move(ok)};
      kex::ir::FunDef onLoad;
      onLoad.name = "__kex_on_load";
      onLoad.exported = false;
      kex::ir::FunClause clause;
      clause.body = std::move(body);
      onLoad.clauses.push_back(std::move(clause));
      module.functions.push_back(std::move(onLoad));
      module.onLoad = "__kex_on_load";
    }
    // What the unit's interface names in a module's companion and lowering
    // emitted in the unit's own module, the companion forwards there:
    //   * a free function sharing its name with a `make` function of the same
    //     module (`Kex.Vault:get` is `kex_vault:'Vault.get$free'`);
    //   * the functions of a `make` block written outside the module for one
    //     of its types (`make Type.Field` after `module Type`);
    //   * a trait's default functions (`Foldable`'s `any?`), dispatching over
    //     the unit's types that claim it, from every companion: the runtime
    //     asks the module holding a type's `make` functions (`Kex.Data` for
    //     `Data.Set`).
    std::set<std::string> traitDefaults;
    for (const auto &item : program.items)
      if (const auto *trait = std::get_if<std::unique_ptr<kex::ast::TraitDef>>(&item);
          trait && *trait)
        for (const auto &member : (*trait)->body)
          if (const auto *function = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&member);
              function && *function)
            traitDefaults.insert((*function)->name);
    std::map<std::string, std::set<std::string>> outsideMakes;
    for (const auto &item : program.items) {
      const auto *make = std::get_if<std::unique_ptr<kex::ast::MakeDef>>(&item);
      if (!make || !*make || !(*make)->target) continue;
      const auto *target = std::get_if<kex::ast::TypeName>(&(*make)->target->kind);
      if (!target || target->parts.size() < 2) continue;
      const auto owner =
          kex::join(target->parts, ".", target->parts.size() - 1);
      const auto note = [&](const kex::ast::FunctionDef &function) {
        outsideMakes["Kex." + owner].insert(function.name);
      };
      for (const auto &member : (*make)->body)
        if (const auto *function = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&member);
            function && *function)
          note(**function);
        else if (const auto *block = std::get_if<std::unique_ptr<kex::ast::VisibilityBlock>>(&member);
                 block && *block && (*block)->isPublic)
          for (const auto &inner : (*block)->items)
            if (const auto *function = std::get_if<std::unique_ptr<kex::ast::FunctionDef>>(&inner);
                function && *function)
              note(**function);
    }
    std::vector<std::tuple<std::string, std::string, std::string, int>> forwards;
    for (const auto &module : modules) {
      if (module.name != fileModule) continue;
      for (const auto &function : module.functions) {
        for (const auto &[companion, names] : outsideMakes)
          if (names.contains(function.name) && function.exported)
            forwards.emplace_back(companion, function.name, function.name, function.arity);
        if (traitDefaults.contains(function.name) && function.exported)
          for (const auto &companion : modules)
            if (companion.name != fileModule)
              forwards.emplace_back(companion.name, function.name, function.name,
                                    function.arity);
        const auto dot = function.name.rfind('.');
        if (dot == std::string::npos || !function.name.ends_with("$free")) continue;
        forwards.emplace_back("Kex." + function.name.substr(0, dot),
                              function.name.substr(dot + 1, function.name.size() - dot - 6),
                              function.name, function.arity);
      }
    }
    for (const auto &[companion, name, target, arity] : forwards)
      for (auto &module : modules) {
        if (module.name != companion ||
            std::any_of(module.functions.begin(), module.functions.end(),
                        [&](const auto &function) {
                          return function.name == name && function.arity == arity;
                        }))
          continue;
        kex::ir::FunClause clause;
        std::vector<kex::ir::ExprPtr> args;
        for (int i = 0; i < arity; i++) {
          const auto param = "_Arg" + std::to_string(i);
          auto pattern = std::make_unique<kex::ir::Pattern>();
          pattern->kind = kex::ir::PatKind::Var;
          pattern->name = param;
          clause.params.push_back(std::move(pattern));
          args.push_back(std::make_unique<kex::ir::Expr>());
          args.back()->node = kex::ir::Var{param};
        }
        clause.body = std::make_unique<kex::ir::Expr>();
        clause.body->node = kex::ir::Call{fileModule, target, arity, std::move(args), true};
        kex::ir::FunDef forward;
        forward.name = name;
        forward.arity = arity;
        forward.clauses.push_back(std::move(clause));
        module.functions.push_back(std::move(forward));
      }
    // KexI advertises a receiver method at its plain name. A unit can emit
    // only the receiver-qualified implementation when an imported field or
    // method shares that name (`Grant.teamId`, `OrganizationId.empty?`).
    // Give the companion the advertised entry point when it has one local
    // owner and no dispatcher of its own.
    std::map<std::string, std::set<std::string>> ownTypes;
    std::function<void(const kex::ast::ModuleDef &)> noteTypes =
        [&](const kex::ast::ModuleDef &definition) {
          auto &types = ownTypes["Kex." + definition.name];
          for (const auto &item : definition.body) {
            std::string name;
            if (const auto *record = std::get_if<std::unique_ptr<kex::ast::RecordDef>>(&item);
                record && *record) name = (*record)->name;
            if (const auto *type = std::get_if<std::unique_ptr<kex::ast::TypeDef>>(&item);
                type && *type) name = (*type)->name;
            if (!name.empty()) {
              types.insert(name);
              types.insert(definition.name + "." + name);
            }
            if (const auto *nested = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
                nested && *nested) noteTypes(**nested);
          }
        };
    for (const auto &item : program.items)
      if (const auto *definition = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
          definition && *definition) noteTypes(**definition);
    for (auto &module : modules) {
      std::map<std::pair<std::string, int>, std::vector<std::string>> implementations;
      for (const auto &function : module.functions) {
        if (!function.exported) continue;
        const auto slash = function.name.find('/');
        if (slash == std::string::npos) continue;
        const auto owner = function.name.substr(slash + 1);
        if (!ownTypes[module.name].contains(owner)) continue;
        implementations[{function.name.substr(0, slash), function.arity}]
            .push_back(function.name);
      }
      for (const auto &[key, targets] : implementations) {
        if (targets.size() != 1 ||
            std::any_of(module.functions.begin(), module.functions.end(),
                        [&](const auto &function) {
                          return function.name == key.first && function.arity == key.second;
                        })) continue;
        kex::ir::FunDef forward;
        forward.name = key.first;
        forward.arity = key.second;
        kex::ir::FunClause clause;
        std::vector<kex::ir::ExprPtr> args;
        for (int i = 0; i < key.second; ++i) {
          const auto param = "_Arg" + std::to_string(i);
          auto pattern = std::make_unique<kex::ir::Pattern>();
          pattern->kind = kex::ir::PatKind::Var;
          pattern->name = param;
          clause.params.push_back(std::move(pattern));
          args.push_back(std::make_unique<kex::ir::Expr>());
          args.back()->node = kex::ir::Var{param};
        }
        clause.body = std::make_unique<kex::ir::Expr>();
        clause.body->node = kex::ir::Call{"", targets.front(), key.second,
                                          std::move(args), true};
        forward.clauses.push_back(std::move(clause));
        module.functions.push_back(std::move(forward));
      }
    }
    auto emitted = kex::ir::emitModules(modules);
    if (!dir.empty())
      for (const auto &module : emitted)
        std::ofstream(dir + "/" + module.moduleName + ".core") << module.source;
    return emitted;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "separate: %s: lowering failed: %s\n", path.c_str(), error.what());
  }
  return std::nullopt;
}

auto checkUnitsSeparately(kex::ast::Program &entry, std::vector<LoadedDep> &deps,
                          const std::vector<std::vector<std::string>> &units,
                          const std::map<std::string, std::set<std::string>> &uses,
                          const kex::semantic::ImportedInterfaces *interfaces) -> void {
  g_separateModules.reset();
  g_separateDiagnostics.clear();
  std::map<std::string, LoadedDep *> byPath;
  for (auto &dep : deps) byPath[*dep.path] = &dep;
  std::size_t errors = 0;
  // Every unit's modules, for a separate build; it stands only if each unit
  // checked and lowered.
  std::vector<kex::ir::EmitResult> separateModules;
  bool allLowered = true;
  // What each unit checked so far says of itself, with its files and the
  // modules it provides.
  struct Described {
    std::set<std::string> files;
    std::vector<std::string> names;
    kex::beam::LoadedUnit unit;
  };
  std::vector<Described> describedUnits;
  std::optional<kex::cache::SeparateUnitCache> unitCache;
  if (auto root = kex::cache::base(); root && separateBuildRequested())
    unitCache.emplace(*root);
  for (const auto &unit : units) {
    const bool isEntry = unit.size() == 1 && unit.front().empty();
    // A unit is checked and lowered against the units its files reach, and
    // no others: one it does not use (`Harmat` from `Parsing`) offers it
    // nothing, not even an operator on an `Unknown` value.
    std::set<std::string> reached;
    std::vector<std::string> pending(unit.begin(), unit.end());
    while (!pending.empty()) {
      const auto file = pending.back();
      pending.pop_back();
      if (const auto used = uses.find(file); used != uses.end())
        for (const auto &next : used->second)
          if (reached.insert(next).second) pending.push_back(next);
    }
    kex::beam::KexiRegistry registry;
    kex::module::PackageMetadata build;
    build.id = "kex.build";
    for (const auto &below : describedUnits) {
      if (std::none_of(below.files.begin(), below.files.end(),
                       [&](const std::string &file) { return reached.contains(file); }))
        continue;
      build.unitIds.push_back(below.unit.modules.front().chunk.metadata.unitId);
      // Once each: a module can be split across files of different units.
      for (const auto &name : below.names)
        if (std::find(build.receiverProviders.begin(), build.receiverProviders.end(), name) ==
            build.receiverProviders.end())
          build.receiverProviders.push_back(name);
      registry.registerUnit(below.unit);
    }
    // The build's own units as one package, every module of them a
    // provider: a type's `make` functions reach the units above it only
    // through a package that says so, as the prelude's do.
    if (!build.unitIds.empty())
      for (const auto &error : registry.declarePackage(build))
        std::fprintf(stderr, "separate: package: %s\n", error.message.c_str());
    std::optional<std::string> cacheKey;
    if (!isEntry && unitCache) {
      std::vector<kex::cache::SeparateUnitCache::Source> sources;
      bool safe = true;
      for (const auto &path : unit) {
        const auto found = byPath.find(path);
        if (found == byPath.end()) { safe = false; break; }
        const auto &source = *found->second->source;
        if (sourceMentionsCompiledBlock(source) || sourceMentionsEmbed(source) ||
            source.find("Template") != std::string::npos) {
          safe = false;
          break;
        }
        sources.push_back({path, source});
      }
      std::vector<const kex::beam::LoadedUnit*> dependencies;
      for (const auto &below : describedUnits) {
        if (std::none_of(below.files.begin(), below.files.end(),
                         [&](const std::string &file) { return reached.contains(file); }))
          continue;
        dependencies.push_back(&below.unit);
      }
      if (safe) {
        const auto runtime = prebuiltRuntimeBeamDir();
        cacheKey = unitCache->keyFor(
            kex::cache::toolchainFingerprint(),
            "runtime:" + runtime + ":" +
                kex::compiled::fileStamp(runtime + "/kex_prelude.beam"),
            std::move(sources), dependencies);
        if (auto cached = unitCache->load(*cacheKey)) {
          if (const char *coreDir = std::getenv("KEX_SEPARATE_CORE");
              coreDir && *coreDir) {
            std::filesystem::create_directories(coreDir);
            for (const auto &module : cached->modules)
              std::ofstream(std::string(coreDir) + "/" + module.moduleName + ".core")
                  << module.source;
          }
          for (auto &module : cached->modules)
            separateModules.push_back(std::move(module));
          std::vector<std::string> names;
          for (const auto &module : cached->interface.modules)
            names.push_back(module.chunk.metadata.sourceModule);
          describedUnits.push_back({std::set<std::string>(unit.begin(), unit.end()),
                                    std::move(names), std::move(cached->interface)});
          if (kex::timings::enabled())
            std::fprintf(stderr, "  unit cache hit: %s\n", unit.front().c_str());
          continue;
        }
      }
    }
    const auto emittedBefore = separateModules.size();
    const auto describedBefore = describedUnits.size();
    // The unit's files, as one program for the length of its check; their
    // items go back where they came from afterwards.
    kex::ast::Program program;
    std::vector<std::pair<LoadedDep *, std::size_t>> taken;
    if (isEntry) {
      program.items = std::move(entry.items);
    } else {
      for (const auto &path : unit) {
        const auto found = byPath.find(path);
        if (found == byPath.end()) continue;
        auto &items = found->second->program->items;
        taken.push_back({found->second, items.size()});
        for (auto &item : items) program.items.push_back(std::move(item));
        items.clear();
      }
    }
    auto below = registry.buildSemanticInterfaces();
    for (auto &[_, module] : below.modules) module.compiledApart = true;
    // A `make` function travels with its receiver unless it was declared for a
    // type its module does not declare (KexiMethod::foreignTarget).
    const auto travelsWithReceiver = [](const kex::semantic::ImportedFunction &function) {
      return function.isReceiverMethod && !function.foreignTarget;
    };
    for (auto &[_, functions] : below.receiverFunctions)
      for (auto &function : functions)
        if (travelsWithReceiver(function)) function.visibleByReceiver = true;
    for (auto &[_, module] : below.modules)
      for (auto &[__, functions] : module.exports)
        for (auto &function : functions)
          if (travelsWithReceiver(function)) function.visibleByReceiver = true;
    // A module this build compiles is described by its own interface, not by
    // the one the prelude derives from the standard library's source for an
    // opt-in module: that one names where the module's functions would be
    // in a merged program (`make Router` in `Net.HTTP`, filed under
    // `Kex.Net.HTTP.Router`), and the merge keeps the first description.
    auto preludeView = [&]() -> kex::semantic::ImportedInterfaces {
      auto view = *interfaces;
      // A type's `make` functions are filed under the type (`Regex.Match`),
      // inside the module this build compiles (`Regex`).
      const auto builtHere = [&](std::string module) {
        for (;;) {
          if (below.modules.contains(module)) return true;
          const auto dot = module.rfind('.');
          if (dot == std::string::npos) return false;
          module.resize(dot);
        }
      };
      std::erase_if(view.modules, [&](const auto &module) { return builtHere(module.first); });
      // The prelude files its own copies under `Prelude`; a receiver of a
      // type declared in such a module (`Regex.Match`) gives them away.
      const auto onBuiltType = [&](const kex::semantic::ImportedFunction &function) {
        if (function.signature.params.empty()) return false;
        const auto *named =
            std::get_if<kex::semantic::NamedType>(&function.signature.params.front()->kind);
        return named && named->name.find('.') != std::string::npos && builtHere(named->name);
      };
      for (auto &[__, functions] : view.receiverFunctions)
        std::erase_if(functions, [&](const auto &function) {
          return builtHere(function.sourceModule) || onBuiltType(function);
        });
      return view;
    };
    // Taken before `below` is moved into the merge, which reads it.
    auto prelude = interfaces ? preludeView() : kex::semantic::ImportedInterfaces{};
    const auto available =
        interfaces ? kex::mergeSemanticInterfaces(std::move(prelude), std::move(below))
                   : std::move(below);
    // A prelude trait the unit claims gives it its default bodies, from the
    // trait's source, before the check, as the merged build backfills them:
    // the checker decides calls to them (`clock.inspected`). Taken out again
    // once lowered, since the unit's items go back to their files below.
    const auto ownItems = program.items.size();
    kex::backfillExternalTraitDefaults(program);
    kex::semantic::Analyzer analyzer(&available);
    (void)analyzer.analyze(program);
    for (const auto &diagnostic : analyzer.diagnostics()) {
      if (diagnostic.level != kex::semantic::Diagnostic::Level::Error) continue;
      errors++;
      if (separateBuildRequested()) {
        g_separateDiagnostics.push_back(
            std::string(diagnostic.location.file) + ":" +
            std::to_string(diagnostic.location.line) + ":" +
            std::to_string(diagnostic.location.column) + ": error: " +
            diagnostic.message);
        continue;
      }
      std::fprintf(stderr, "separate: %s:%d:%d: error: %s\n",
                   std::string(diagnostic.location.file).c_str(),
                   diagnostic.location.line, diagnostic.location.column,
                   diagnostic.message.c_str());
    }
    // Phase 3: the unit lowered on its own, against the prelude and the KexI
    // of the units below it, as the REPL lowers code against a loaded unit.
    const bool unitChecked =
        std::none_of(analyzer.diagnostics().begin(), analyzer.diagnostics().end(),
                     [](const auto &diagnostic) {
                       return diagnostic.level == kex::semantic::Diagnostic::Level::Error;
                     });
    const char *coreDir = std::getenv("KEX_SEPARATE_CORE");
    const bool lowerApart = (coreDir && *coreDir) || separateBuildRequested();
    bool unitLowered = false;
    std::map<std::string, std::string> unitCoreExports;
    std::map<std::string, std::set<std::pair<std::string, int>>> unitRoutes;
    if (lowerApart && !unitChecked) allLowered = false;
    if (lowerApart && unitChecked) {
      auto lowered = lowerUnitSeparately(program, isEntry ? g_separateEntryPath : unit.front(),
                                         analyzer, registry, coreDir ? coreDir : "");
      if (!lowered) allLowered = false;
      else {
        unitLowered = true;
        for (auto &module : *lowered) {
          unitCoreExports[module.moduleName] =
              module.source.substr(0, module.source.find('\n'));
          unitRoutes[module.moduleName] = module.routedImplementations;
          separateModules.push_back(std::move(module));
        }
      }
    }
    program.items.erase(program.items.begin() + static_cast<std::ptrdiff_t>(ownItems),
                        program.items.end());
    if (!isEntry) {
      // What the unit's modules would say of themselves once compiled.
      kex::beam::LoadedUnit described;
      std::vector<std::string> names;
      std::vector<const kex::ast::ModuleDef *> modules;
      // Nested ones too (`capability Ticker` inside `module Tickers` is
      // `Tickers.Ticker`), once each: a module can come in more than one part.
      std::function<void(const kex::ast::ModuleDef &)> describe =
          [&](const kex::ast::ModuleDef &module) {
            if (std::find(names.begin(), names.end(), module.name) == names.end()) {
              names.push_back(module.name);
              modules.push_back(&module);
            }
            for (const auto &item : module.body)
              if (const auto *nested = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
                if (*nested) describe(**nested);
          };
      for (const auto &item : program.items)
        if (const auto *module = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
          if (*module) describe(**module);
      // The types a bare name in a module's signatures can mean: its own,
      // then those of the modules it uses, as the checker resolves them.
      std::set<std::string> qualifiedTypes(available.typeNames.begin(),
                                           available.typeNames.end());
      for (const auto &[name, _] : available.recordArities) qualifiedTypes.insert(name);
      for (const auto &[name, _] : available.typeAliases) qualifiedTypes.insert(name);
      for (const auto &[name, _] : available.distinctTypes) qualifiedTypes.insert(name);
      for (const auto *module : modules)
        for (const auto &item : module->body) {
          if (const auto *record = std::get_if<std::unique_ptr<kex::ast::RecordDef>>(&item))
            if (*record) qualifiedTypes.insert(module->name + "." + (*record)->name);
          if (const auto *type = std::get_if<std::unique_ptr<kex::ast::TypeDef>>(&item))
            if (*type) qualifiedTypes.insert(module->name + "." + (*type)->name);
        }
      auto qualifierFor = [&](const kex::ast::ModuleDef &module) {
        return [&](const std::string &name) -> std::string {
          if (qualifiedTypes.contains(module.name + "." + name))
            return module.name + "." + name;
          for (const auto &item : module.body) {
            const auto *use = std::get_if<std::unique_ptr<kex::ast::UsingBlock>>(&item);
            if (!use || !*use || (*use)->alias) continue;
            const auto &only = (*use)->onlyNames;
            const auto &except = (*use)->exceptNames;
            if (!only.empty() && std::find(only.begin(), only.end(), name) == only.end())
              continue;
            if (std::find(except.begin(), except.end(), name) != except.end()) continue;
            std::string used = (*use)->module.dotted();
            if (qualifiedTypes.contains(used + "." + name)) return used + "." + name;
          }
          return name;
        };
      };
      for (std::size_t i = 0; i < names.size(); i++) {
        kex::beam::CollectOptions options;
        options.moduleAtom = "Kex." + names[i];
        options.unitId = "Kex." + names.front();
        options.moduleName = names[i];
        options.role = i == 0 ? kex::beam::KexiModuleRole::Entry
                              : kex::beam::KexiModuleRole::Companion;
        if (i > 0) options.entryBackPointer = "Kex." + names.front();
        options.analysis = &analyzer;
        options.unitCompiledApart = true;
        auto chunk = kex::beam::collectMetadata(program, options);
        kex::beam::qualifyTypeNames(chunk, qualifierFor(*modules[i]));
        // A plain receiver dispatcher may only accept tagged records. A
        // distinct value is erased to its backing value, so another unit
        // must call the receiver-specific implementation when one was
        // emitted (`empty?/ThingId`, not `empty?/1` on a String).
        if (auto core = unitCoreExports.find(options.moduleAtom);
            core != unitCoreExports.end()) {
          kex::beam::recordImplementations(
              chunk, routedIn(unitRoutes[options.moduleAtom]));
          const auto emitted = kex::beam::coreExports(core->second);
          for (auto &method : chunk.typeInterface.methods)
            if (method.receiverType &&
                method.receiverType->kind == kex::beam::KexiType::Named)
              if (auto own = kex::beam::emittedImplementationOf(method, emitted);
                  !own.empty())
                method.beamFunction = std::move(own);
        }
        described.modules.push_back({options.moduleAtom, "", std::move(chunk)});
      }
      if (!described.modules.empty()) {
        described.entryBeamAtom = described.modules.front().beamAtom;
        describedUnits.push_back({std::set<std::string>(unit.begin(), unit.end()), names,
                                  std::move(described)});
      }
    }
    if (cacheKey && unitChecked && unitLowered &&
        describedUnits.size() > describedBefore) {
      kex::cache::CachedSeparateUnit cached;
      cached.interface = describedUnits.back().unit;
      cached.modules.assign(separateModules.begin() +
                                static_cast<std::ptrdiff_t>(emittedBefore),
                            separateModules.end());
      unitCache->store(*cacheKey, cached);
    }
    if (isEntry) {
      entry.items = std::move(program.items);
    } else {
      std::size_t at = 0;
      for (auto &[dep, count] : taken)
        for (std::size_t i = 0; i < count; i++)
          dep->program->items.push_back(std::move(program.items[at++]));
    }
  }
  // A separate build runs the program, whose output this would be part of.
  if (!separateBuildRequested())
    std::fprintf(stderr, "separate: %zu units checked, %zu errors\n", units.size(), errors);
  if (separateBuildRequested() && allLowered) g_separateModules = std::move(separateModules);
}

// Every dependency file this build has parsed and expanded, by path. The
// gating pre-check and the build's own dependency load read the same files,
// and expanding each twice cost Slick's no-op build a second pass over all
// of them. Only for one build (`kex -c` / `--run` of a file, set in main):
// within it the sources stay as they are, while across REPL inputs a file's
// compile-time reads may change under the same bytes. Handed out as clones,
// keyed by the bytes and roots they were expanded from.
struct ExpandedDependency {
  std::string source;
  std::vector<std::string> roots; // effective (kex::compiled::effectiveModuleRoots)
  kex::ast::Program program;
};
static bool g_memoizeDependencyExpansion = false;
static std::unordered_map<std::string, ExpandedDependency> g_expandedDependencies;

// Whether the prelude imports `module` into every program: its interface is
// already known, so its source is never merged in.
auto isAutomaticImport(const kex::semantic::ImportedInterfaces *interfaces,
                       const std::string &module) -> bool {
  if (!interfaces) return false;
  const auto imported = interfaces->modules.find(module);
  return imported != interfaces->modules.end() &&
         imported->second.automaticImport;
}

// The modules `program` names — through `using` or a qualified reference —
// whose source has to be merged in: all but the automatic imports.
auto sourceModulesNamedBy(const kex::ast::Program &program)
    -> std::unordered_set<std::string> {
  kex::semantic::Analyzer analysis(&preludeSemanticInterfaces());
  (void)analysis.analyze(program);
  auto modules = analysis.referencedModules();
  std::erase_if(modules, [](const std::string &module) {
    return isAutomaticImport(&preludeSemanticInterfaces(), module);
  });
  return modules;
}

// `qualifiedModules` is the caller's own fresh discovery result for `program`
// (referencedModules, automatic-import filtered), or nullopt when the caller
// did not analyze: the entry program is then analyzed here instead. A
// PROVIDED set — even an empty one — is authoritative and skips that
// re-analyze (3b); the recursion into dependency files always passes nullopt
// and uses the discovery cache as before.
auto resolveBeamDeps(kex::ast::Program &program,
                     const std::vector<std::string> &roots,
                     const kex::semantic::ImportedInterfaces *interfaces,
                     const std::optional<std::unordered_set<std::string>>
                         &qualifiedModules = std::nullopt,
                     UnitGraph *graph = nullptr)
    -> std::vector<LoadedDep> {
  std::map<std::string, std::set<std::string>> uses{{"", {}}};
  std::vector<LoadedDep> deps;
  std::unordered_set<std::string> loaded;
  kex::module::Resolver resolver(roots);
  // KEX_TIMINGS=1 splits dependency loading into its two costs (kexhq/kex#323).
  std::chrono::steady_clock::duration parseTime{};
  std::chrono::steady_clock::duration analyzeTime{};
  std::chrono::steady_clock::duration expandTime{};
  const auto sandboxesBefore = kex::compiled::sandboxTimings();

  size_t discoveryHits = 0;
  // Names a `using` or a qualified reference asked for that no file under the
  // source roots answers to. Most are declared inline or come from an
  // interface; the rest are checked once everything is loaded.
  std::set<std::string> unresolved;

  // `source` is a dependency file's own text, which keys the discovery cache;
  // the entry program passes nullptr, since its AST is already expanded and
  // may carry a merged `.spec.kex` base, so its bytes are not the whole input.
  std::function<void(const kex::ast::Program &,
                     const std::optional<std::unordered_set<std::string>> &,
                     const std::string *, const std::string &)> resolve =
      [&](const kex::ast::Program &prog,
          const std::optional<std::unordered_set<std::string>> &seedModules,
          const std::string *source, const std::string &from) {
    auto modules = collectUsingModules(prog);
    if (seedModules)
      modules.insert(modules.end(), seedModules->begin(), seedModules->end());

    // Each newly parsed source file can introduce qualified-only dependency
    // edges of its own. Analyze it here rather than only analyzing the entry
    // program; otherwise discovery stops after one qualified-reference hop.
    // Sorted: `referencedModules` is a hash set, and this list decides what
    // gets compiled and in what order. Iterating it directly made the build
    // depend on the host's hash order (kexhq/kex#143).
    const auto analyzeStart = std::chrono::steady_clock::now();
    const auto cacheEntry =
        source ? discoveryCacheEntry(*source) : std::nullopt;
    std::vector<std::string> referencedOrdered;
    if (seedModules) {
      // The caller analyzed this exact program with these exact
      // interfaces already: the seed IS the discovery result. Sorting
      // keeps `referencedOrdered` identical to what the analyze below
      // would have produced (it sorts too), so resolution order — and
      // hence merge order — is unchanged while the re-analyze is gone.
      referencedOrdered.assign(seedModules->begin(), seedModules->end());
      std::sort(referencedOrdered.begin(), referencedOrdered.end());
    } else if (auto cached =
                   cacheEntry ? readDiscoveryCache(*cacheEntry)
                              : std::nullopt) {
      referencedOrdered = std::move(*cached);
      ++discoveryHits;
    } else {
      kex::semantic::Analyzer dependencyAnalysis(interfaces);
      (void)dependencyAnalysis.analyze(prog);
      const auto &referenced = dependencyAnalysis.referencedModules();
      referencedOrdered.assign(referenced.begin(), referenced.end());
      std::sort(referencedOrdered.begin(), referencedOrdered.end());
      if (cacheEntry) writeDiscoveryCache(*cacheEntry, referencedOrdered);
    }
    analyzeTime += std::chrono::steady_clock::now() - analyzeStart;
    for (const auto &name : referencedOrdered)
      if (!isAutomaticImport(interfaces, name)) modules.push_back(name);
    for (const auto &modName : modules) {
      auto resolved = resolver.resolve(modName);
      if (!resolved) {
        unresolved.insert(modName);
        continue;
      }
      // Before the already-loaded check: a file uses what it uses, whoever
      // loaded it first.
      if (resolved->path != from) uses[from].insert(resolved->path);
      uses.try_emplace(resolved->path);
      if (!loaded.insert(resolved->path).second) continue;

      auto path = std::make_unique<std::string>(resolved->path);
      const auto parseStart = std::chrono::steady_clock::now();
      auto src = std::make_unique<std::string>(readFile(*path));
      std::unique_ptr<kex::ast::Program> depProg;
      if (g_memoizeDependencyExpansion)
        if (const auto memo = g_expandedDependencies.find(*path);
            memo != g_expandedDependencies.end() &&
            memo->second.source == *src &&
            memo->second.roots == kex::compiled::effectiveModuleRoots(roots))
          if (auto copy = kex::ast::clone(memo->second.program))
            depProg = std::make_unique<kex::ast::Program>(std::move(*copy));
      const bool memoized = depProg != nullptr;
      std::optional<kex::Parser> parser;
      if (!memoized) {
        kex::Lexer lexer(std::string(*src), *path);
        parser.emplace(lexer.tokenizeAll(), *path);
        depProg = std::make_unique<kex::ast::Program>(parser->parseProgram());
      }
      parseTime += std::chrono::steady_clock::now() - parseStart;
      // A dependency that does not parse is a build failure, not a module to
      // skip. Silently dropping it produced no error anywhere: the module
      // simply did not exist, the program compiled, and the first call into
      // it died at RUNTIME with `Undefined function: Tey.Resolver`.
      if (parser && !parser->diagnostics().empty()) {
        for (const auto &diagnostic : parser->diagnostics())
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
        dependencyFailure("could not parse module " + modName);
      }
      // Expanded HERE, against THIS module's own path, while it is still its
      // own separate AST — not later, once `resolveBeamDeps` has merged its
      // items into the entry program's `items` alongside every other
      // dependency's. `expandParsedProgram` in main() runs exactly once, on
      // the entry file, with `sourcePath` fixed to the entry file's own
      // path — so a `Kex.embed(...)` or builder-chain collapse candidate
      // that a merged-in dependency module owns was resolving relative to
      // the ENTRY file's directory, or (for a chain requiring `compiled`
      // expansion) not being visited by that pass at all, since dependency
      // items are parsed after it already ran. Real per-declaration source
      // provenance would let one pass over the fully-merged program handle
      // this uniformly; short of that, each dependency gets its own
      // self-contained expansion, against its own path, before merging
      // (kexhq/kex#385).
      std::vector<kex::semantic::Diagnostic> depExpandDiagnostics;
      kex::compiled::ExpandOptions depExpandOptions;
      depExpandOptions.sourcePath = *path;
      depExpandOptions.moduleRoots = roots;
      setScanCache(depExpandOptions);
      if (!memoized) buildProgress.expanding(shownPath(*path));
      const auto expandStart = std::chrono::steady_clock::now();
      const bool expanded =
          memoized || kex::compiled::expand(*depProg, depExpandDiagnostics,
                                            depExpandOptions);
      const auto expandSpent = std::chrono::steady_clock::now() - expandStart;
      expandTime += expandSpent;
      // The total alone says expansion is slow, not where: Slick spent 930s
      // here across 146 files with nothing naming any of them (kexhq/kex#586).
      if (kex::timings::enabled() &&
          expandSpent > std::chrono::milliseconds(250))
        std::fprintf(stderr, "      expand %9.1f ms  %s\n",
                     std::chrono::duration<double, std::milli>(expandSpent).count(),
                     shownPath(*path).c_str());
      if (!expanded) {
        for (const auto &diagnostic : depExpandDiagnostics)
          printSemanticDiagnostic(diagnostic);
        std::cerr << "error: could not expand module " << modName << "\n";
        dependencyFailure("could not expand module " + modName);
      }
      // Found by its path, but declaring another module: `using
      // App.Services.Authority` reaching app/services/authority.kex whose
      // header says `module App.Authority`. The walker refuses that file;
      // the build compiled it and the first call failed at run time. A name
      // found through its enclosing module's file is not checked: the type
      // `DB.Store.Store` lives in db/store.kex, whose stem only happens to
      // match.
      const auto lastPart =
          resolved->moduleName.substr(resolved->moduleName.rfind('.') + 1);
      const auto stem = std::filesystem::path(*path).stem().string();
      const auto lowered = kex::lowered(lastPart);
      const bool ownFile = !resolved->enclosing &&
                           (stem == snakeCased(lastPart) || stem == lowered);
      const kex::ast::ModuleDef *header = nullptr;
      for (const auto &item : depProg->items)
        if (auto *module = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
            module && *module) {
          header = module->get();
          break;
        }
      if (header && ownFile &&
          !declaredModuleNames(*depProg).contains(resolved->moduleName)) {
        reportModuleFileError(
            *path, header->location.line,
            "this file is `" + resolved->moduleName + "` by its path, but "
            "declares `" + header->name + "`; name it `module " +
            resolved->moduleName + "`");
        dependencyFailure("module file " + shownPath(*path) + " declares another module");
      }
      if (g_memoizeDependencyExpansion && !memoized)
        if (auto copy = kex::ast::clone(*depProg))
          g_expandedDependencies[*path] = {
              *src, kex::compiled::effectiveModuleRoots(roots), std::move(*copy)};
      resolve(*depProg, std::nullopt, src.get(), *path);
      deps.push_back({std::move(src), std::move(path), std::move(depProg)});
    }
  };
  resolve(program, qualifiedModules, nullptr, std::string{});
  // Files declaring the same module (`module Data` atop both data/set.kex
  // and data/stack.kex) build that one module together: one unit, as if
  // each used the next.
  std::map<std::string, std::vector<std::string>> declaringFiles;
  for (const auto &dep : deps)
    for (const auto &item : dep.program->items)
      if (const auto *module = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item);
          module && *module)
        declaringFiles[(*module)->name].push_back(*dep.path);
  for (auto &[_, paths] : declaringFiles) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (std::size_t i = 0; paths.size() > 1 && i < paths.size(); i++)
      uses[paths[i]].insert(paths[(i + 1) % paths.size()]);
  }
  const auto units = unitsOf(uses);
  // Before the dependencies are merged in, while each is still its own.
  if (graph && separateCheckRequested())
    checkUnitsSeparately(program, deps, units, uses, interfaces);

  if (!deps.empty()) {
    std::vector<kex::ast::TopLevelItem> merged;
    for (auto &dep : deps)
      for (auto &item : dep.program->items)
        if (kex::ast::isMergedDeclaration(item))
          merged.push_back(std::move(item));
    prependItems(program, std::move(merged));
  }
  // A module whose file sits where another name belongs is reached by
  // neither: `module App.Authority` in app/services/authority.kex. The build
  // used to succeed without it and the first call died at run time with
  // "Unknown module" or "Undefined function".
  if (!unresolved.empty()) {
    const auto declared = declaredModuleNames(program);
    for (const auto &name : unresolved) {
      if (declared.contains(name)) continue;
      if (interfaces && (interfaces->modules.contains(name) ||
                         interfaces->modules.contains("Kex." + name)))
        continue;
      const auto found = fileDeclaringModule(name, roots);
      if (!found) continue;
      if (loaded.contains(found->file.string())) continue;
      const auto byPath =
          moduleNameForPath(found->file.lexically_relative(found->root));
      auto expected = found->root;
      std::stringstream parts(name);
      for (std::string part; std::getline(parts, part, '.');)
        expected /= snakeCased(part);
      expected += ".kex";
      reportModuleFileError(
          found->file, found->line,
          "module `" + name + "` is declared in a file whose path names `" +
              byPath + "`, so `" + name + "` is not found; name it `module " +
              byPath + "`, or move it to " + shownPath(expected));
      dependencyFailure("module `" + name + "` is declared in a file named for another module");
    }
  }
  if (kex::timings::enabled()) {
    const auto ms = kex::timings::milliseconds;
    std::fprintf(stderr,
                 "    %zu dependency files: parse %.1f ms, expand %.1f ms, "
                 "analyze %.1f ms (%zu from the discovery cache)\n",
                 deps.size(), ms(parseTime), ms(expandTime), ms(analyzeTime),
                 discoveryHits);
    const auto &sandboxes = kex::compiled::sandboxTimings();
    if (sandboxes.started > sandboxesBefore.started)
      std::fprintf(stderr,
                   "    %zu compile-time sandboxes: start-up %.1f ms, evaluation %.1f ms\n",
                   sandboxes.started - sandboxesBefore.started,
                   ms(sandboxes.startup - sandboxesBefore.startup),
                   ms(sandboxes.evaluation - sandboxesBefore.evaluation));
    std::size_t largest = 0, inCycles = 0;
    for (const auto &unit : units) {
      largest = std::max(largest, unit.size());
      if (unit.size() > 1) inCycles += unit.size();
    }
    std::fprintf(stderr, "    %zu units: largest %zu files, %zu files in cycles\n",
                 units.size(), largest, inCycles);
  }
  if (graph) {
    graph->uses = std::move(uses);
    graph->units = units;
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

// The compile-time expansion a REPL input needs before it is checked or run:
// a file run goes through `expandParsedProgram`, and without this the REPL
// left `Kex.embed`, `Template.text`/`html` and `compiled do` for run time,
// where none of them exists (kexhq/kex#543). An input has no file of its own,
// so it is expanded as if it were one in the working directory: that is
// where `Kex.embed`'s relative paths and the session's modules are found.
//
// Each call gets a name of its own: generated template functions are named
// after the path, and the walker REPL keeps every input's definitions, so a
// second input reusing `<repl>` redefined the first one's templates.
auto expandReplProgram(kex::ast::Program &program) -> void {
  static int inputs = 0;
  const auto here = (std::filesystem::current_path() /
                     ("<repl-" + std::to_string(++inputs) + ">"))
                        .string();
  std::vector<kex::semantic::Diagnostic> diagnostics;
  kex::compiled::ExpandOptions options;
  options.sourcePath = here;
  options.moduleRoots = moduleRootsFor(here);
  setScanCache(options);
  if (kex::compiled::expand(program, diagnostics, options)) return;
  for (const auto &diagnostic : diagnostics)
    if (diagnostic.level == kex::semantic::Diagnostic::Level::Error)
      throw std::runtime_error(diagnostic.message);
  throw std::runtime_error("compile-time expansion failed");
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
  for (const auto &[name, defaults] : overrides.exportDefaults)
    result.exportDefaults[name] = defaults;
  for (const auto &name : overrides.foulExports)
    result.foulExports.insert(name);
  for (const auto &name : overrides.capabilityModules)
    result.capabilityModules.insert(name);
  for (const auto &[name, members] : overrides.capabilityMembers)
    result.capabilityMembers[name] = members;
  for (const auto &[type, serving] : overrides.servingSlots)
    result.servingSlots[type] = serving;
  result.compiledApartModules.insert(overrides.compiledApartModules.begin(),
                                     overrides.compiledApartModules.end());
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
  auto optInFiles = kex::optInStdlibSources(stdlibRoot);
  files.insert(files.end(), optInFiles.begin(), optInFiles.end());
  // Track item index boundaries per tier: tierBounds[t] = index of first
  // item belonging to tier t.  tierBounds[4] = total item count.
  std::array<size_t, 5> tierBounds{};
  // A stdlib file that does not parse is reported as itself, and stops the
  // build before analysis. Parser recovery can drop the rest of an enclosing
  // block, and analyzing what survived produced a wall of type errors in
  // OTHER files (every String method missing after a truncated `make String`)
  // with nothing pointing at the file that was actually wrong (kexhq/kex#165).
  bool parsed = true;
  auto mergeFile = [&](const std::string &f) {
    kex::Lexer lex(readFile(f), f);
    kex::Parser parser(lex.tokenizeAll(), f);
    auto prog = parser.parseProgram();
    for (const auto &diag : parser.diagnostics()) {
      std::cerr << "error: prelude source: " << diag.location.file << ":"
                << diag.location.line << ":" << diag.location.column << ": "
                << diag.message << "\n";
      parsed = false;
    }
    for (auto &item : prog.items)
      if (kex::ast::isMergedDeclaration(item))
        merged.items.push_back(std::move(item));
  };
  for (size_t t = 0; t < 4; t++) {
    tierBounds[t] = merged.items.size();
    for (const auto &f : tierGroups[t]) mergeFile(f);
  }
  for (const auto &f : optInFiles) mergeFile(f);
  if (!parsed) return false;
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
            {kex::names::kPreludeModule, name, arity});

    // The prelude lowers against its own names, receivers preferred, with
    // what its own check decided — as every other program does.
    const kex::hir::Program hir(merged, analyzer);
    const auto selfContext = withCheckerAnswers(
        {.externals = &selfExt, .preferExternalReceivers = true}, &hir);
    std::vector<kex::ir::Module> modules;
    modules.push_back(kex::ir::lowerProgramTiered(merged, tierBounds,
                                                  "prelude", selfContext));
    auto splitModules = kex::ir::lowerModules(merged, "prelude", selfContext);
    for (size_t i = 1; i < splitModules.size(); i++)
      modules.push_back(std::move(splitModules[i]));

    auto passes = kex::ir::standardPasses();
    builtModules->clear();
    for (size_t i = 0; i < modules.size(); i++) {
      PreludeBuildModule built;
      passes.run(modules[i]);
      built.emitted = kex::ir::emitCore(modules[i]);
      std::ofstream out(dir + "/" + built.emitted.moduleName + ".core");
      if (!out) return false;
      out << built.emitted.source;

      if (i == 0) {
        kex::beam::CollectOptions options;
        options.unitId = kex::names::kPreludeModule;
        options.moduleAtom = built.emitted.moduleName;
        options.moduleName = std::string(kex::semantic::kFileLevelPreludeModule);
        options.collectTopLevel = true;
        options.flattenModules = true;
        options.role = kex::beam::KexiModuleRole::Entry;
        options.analysis = &analyzer;
        built.interface = kex::beam::collectMetadata(merged, options);
        built.interface.metadata.package.id = "kex.stdlib";
        built.interface.metadata.package.unitIds = {kex::names::kPreludeModule};
        built.interface.metadata.package.receiverProviders = {
            std::string(kex::semantic::kFileLevelPreludeModule)};
        built.interface.metadata.package.automaticImports = {
            std::string(kex::semantic::kFileLevelPreludeModule)};
        built.interface.sourceHash = kex::preludeSourceHash(files);
      } else {
        kex::beam::CollectOptions options;
        options.unitId = kex::names::kPreludeModule;
        options.moduleAtom = built.emitted.moduleName;
        options.analysis = &analyzer;
        options.role = kex::beam::KexiModuleRole::Companion;
        options.entryBackPointer = kex::names::kPreludeModule;
        options.moduleName = built.emitted.moduleName.rfind("Kex.", 0) == 0
            ? built.emitted.moduleName.substr(4) : built.emitted.moduleName;
        built.interface = kex::beam::collectMetadata(merged, options);
      }
      kex::beam::recordImplementations(
          built.interface, routedIn(built.emitted.routedImplementations));
      builtModules->push_back(std::move(built));
    }
    passes.report();
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

// `/with Harmat.Connection = conn` keeps a capability replaced for the rest
// of the REPL session (kexhq/kex#550): a `with ... do ... end` closes at its
// `end`, so in an app console every query needed its own wrapper. The value
// is evaluated once, into a hidden session binding, and every later input is
// entered inside `with Capability = <binding> do ... end`.
struct ReplSessionWith {
  std::string capability;
  std::string binding;
  std::string value; // as typed, for `/with` to list
};

auto replWithBindingName(const std::string &capability) -> std::string {
  std::string name = "__replWith_";
  for (char c : capability)
    name += c == '.' ? '_' : c;
  return name;
}

// A capability name as `/with` and `/without` take it: dotted, each part
// capitalized (`FS.File`).
auto isReplCapabilityName(const std::string &name) -> bool {
  if (name.empty()) return false;
  bool partStart = true;
  for (char c : name) {
    if (partStart) {
      if (!std::isupper((unsigned char)c)) return false;
      partStart = false;
    } else if (c == '.') {
      partStart = true;
    } else if (!std::isalnum((unsigned char)c) && c != '_') {
      return false;
    }
  }
  return !partStart;
}

auto trimReplWords(const std::string &s) -> std::string {
  const auto start = s.find_first_not_of(" \t");
  if (start == std::string::npos) return "";
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// Handles `/with` and `/without`. Returns the input to go on with: empty when
// the command was handled here, or — for `/with Capability = value` — the
// hidden binding to evaluate, with `pending` set to the `with` to keep once it
// has been evaluated and checked.
auto handleReplWithCommand(const std::string &input,
                           std::vector<ReplSessionWith> &withs,
                           std::optional<ReplSessionWith> &pending)
    -> std::string {
  if (input == "/with") {
    if (withs.empty())
      std::cout << "  (no session with)\n";
    for (const auto &with : withs)
      std::cout << "  " << with.capability << " = " << with.value << "\n";
    return "";
  }
  if (input.rfind("/without ", 0) == 0) {
    const auto capability = trimReplWords(input.substr(9));
    auto found = std::find_if(withs.begin(), withs.end(), [&](const auto &w) {
      return w.capability == capability;
    });
    if (found == withs.end())
      throw std::runtime_error("/without: no session with for `" +
                               capability + "`");
    withs.erase(found);
    std::cout << "  (without " << capability << ")\n";
    return "";
  }
  const auto rest = input.substr(6);
  const auto equals = rest.find('=');
  const auto capability =
      trimReplWords(equals == std::string::npos ? rest : rest.substr(0, equals));
  const auto value = equals == std::string::npos
                         ? std::string()
                         : trimReplWords(rest.substr(equals + 1));
  if (!isReplCapabilityName(capability) || value.empty())
    throw std::runtime_error(
        "/with takes `Capability = value`, as `with` does");
  pending = ReplSessionWith{capability, replWithBindingName(capability), value};
  return "let " + pending->binding + " = " + value;
}

// The `with` lines the session's inputs are entered inside, and their `end`s.
// `except` leaves out the capability being replaced: its hidden binding is
// what the input rebinds.
auto replWithOpen(const std::vector<ReplSessionWith> &withs,
                  const std::string &except = "") -> std::string {
  std::string open;
  for (const auto &with : withs)
    if (with.capability != except)
      open += "  with " + with.capability + " = " + with.binding + " do\n";
  return open;
}

auto replWithClose(const std::vector<ReplSessionWith> &withs,
                   const std::string &except = "") -> std::string {
  std::string close;
  for (const auto &with : withs)
    if (with.capability != except) close += "  end\n";
  return close;
}

// The input's own last statement in a REPL main block: inside the session's
// `with` wrappers, when there are any.
auto replLastStatement(const kex::ast::MainBlock &main)
    -> const kex::ast::Expr * {
  if (main.body.empty()) return nullptr;
  const kex::ast::Expr *last = main.body.back().get();
  while (last) {
    const auto *with = std::get_if<kex::ast::WithExpr>(&last->kind);
    if (!with || with->body.empty()) break;
    last = with->body.back().get();
  }
  return last;
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
  // KEX_TIMINGS=1 splits the type check into its passes (kexhq/kex#323).
  kex::timings::PhaseTimer passTimer(4, false);
  auto phase = [&](const char *name) { passTimer.mark(name); };
  // One sema: names (the resolver SemanticDB indexes files with, over the
  // checked program, its dependencies and a spec's base merged in; reported
  // for the entry file and the base), module rules, purity and types.
  std::vector<std::string> files{filepath};
  if (!extraDeclFile.empty()) files.push_back(extraDeclFile);
  kex::semantic::Analyzer localAnalyzer(&preludeSemanticInterfaces());
  auto &analyzer = retainedAnalyzer ? *retainedAnalyzer : localAnalyzer;
  // An exported module the program does not merge is looked for where the
  // build finds modules.
  analyzer.setModuleLocator(
      [resolver = kex::module::Resolver(moduleRootsFor(filepath))](
          const std::string &module, const std::string &current) {
        return resolver.resolve(module, current).has_value();
      });
  analyzer.setNameCheckFiles(std::move(files));
  bool ok = analyzer.analyze(program);
  for (const auto &diag : analyzer.diagnostics()) {
    countError(diag);
    printSemanticDiagnostic(diag);
  }
  phase("analyzer (whole program)");

  bool validationOk = true;
  if (ok) {
    for (const auto &diag :
         kex::validation::validateTaggedLiterals(program, analyzer, moduleRootsFor(filepath))) {
      if (diag.level == kex::semantic::Diagnostic::Level::Error)
        validationOk = false;
      countError(diag);
      printSemanticDiagnostic(diag);
    }
    phase("tagged literal validation");
  }

  if (errorCount) *errorCount = errors;
  return ok && validationOk;
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

// ── Compiled-module cache (kexhq/kex#323) ───────────────────────────────────
//
// erlc is most of a build: every `--compile` re-ran it over every reachable
// module, including opt-in stdlib modules that never change. A module's .beam
// is a pure function of its Core Erlang text and the erlc that compiled it, so
// the cache is content-addressed on exactly that and needs no dependency
// tracking to stay correct: anything upstream that changes what a module
// compiles to changes its .core text, and so its key. The KexI chunk is not
// cached — it is attached afresh after every build from the program's own
// analysis, exactly as for a freshly compiled module.

// Where cached .beam files live: $KEX_CACHE, else $XDG_CACHE_HOME/kex, else
// ~/.cache/kex. `KEX_CACHE=off` (or `0`, or empty) turns the cache off.
auto beamCacheRoot() -> std::optional<std::filesystem::path> {
  if (auto base = kex::cache::base()) return *base / "beam";
  return std::nullopt;
}

// The erlc actually run, as a string that changes whenever it does: its
// resolved path (an OTP upgrade lands in a new versioned directory) plus the
// file's size and modification time. Asking erlc for its OTP release would
// start a VM — the very cost the cache exists to avoid. `erlc` is only a
// launcher, though: what compiles is the installation's compiler
// application, which an update in place can replace without touching it. So
// the release (`releases/<n>/OTP_VERSION`) and the compiler application's
// versioned directory are part of it too, read from the files, and so is
// `ERL_COMPILER_OPTIONS`, which erlc applies to everything it compiles.
auto erlcIdentity() -> std::string {
  namespace fs = std::filesystem;
  std::string command = erlcExecutable();
  fs::path located{command};
  if (!located.has_parent_path())
    if (const char *path = std::getenv("PATH")) {
      std::string directories = path;
      size_t start = 0;
      while (start <= directories.size()) {
        const auto end = directories.find(':', start);
        const auto directory = directories.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::error_code ec;
        if (!directory.empty() && fs::exists(fs::path{directory} / command, ec)) {
          located = fs::path{directory} / command;
          break;
        }
        if (end == std::string::npos) break;
        start = end + 1;
      }
    }
  std::error_code ec;
  const auto canonical = fs::canonical(located, ec);
  if (ec) return command;
  const auto size = fs::file_size(canonical, ec);
  const auto modified = fs::last_write_time(canonical, ec).time_since_epoch().count();
  auto identity = canonical.string() + "|" +
                  std::to_string(static_cast<unsigned long long>(size)) + "|" +
                  std::to_string(static_cast<long long>(modified));
  // bin/erlc sits in the installation's root.
  const auto root = canonical.parent_path().parent_path();
  std::vector<std::string> parts;
  for (const auto &release : fs::directory_iterator(root / "releases", ec)) {
    std::ifstream input(release.path() / "OTP_VERSION");
    std::string version;
    if (input && std::getline(input, version))
      parts.push_back("otp:" + release.path().filename().string() + "=" + version);
  }
  for (const auto &application : fs::directory_iterator(root / "lib", ec))
    if (application.path().filename().string().rfind("compiler-", 0) == 0)
      parts.push_back(application.path().filename().string());
  std::sort(parts.begin(), parts.end());
  for (const auto &part : parts) identity += "|" + part;
  if (const char *options = std::getenv("ERL_COMPILER_OPTIONS"))
    identity += "|options:" + std::string(options);
  return identity;
}

// The cache key for one module: SHA-256 over everything its .beam depends on,
// and nothing else. The compiler's own version is not part of it: whatever a
// new kex emits differently is in the Core Erlang text, so its key moves
// anyway. Keyed on the git revision as well, every rebuild of kex threw the
// whole cache away, and Tey's 82 modules went back through erlc (45s of a 76s
// build) after any commit at all.
//
// So the cache holds exactly what erlc wrote, and nothing of kex's own: the
// KexI chunk is attached to the copy in the build's output, after it leaves
// the cache, every build. Anything that changes what an entry holds — a beam
// post-processed before it is stored, another layout — must bump the
// `kex-beam-cache-v2` tag, or a newer kex reads an older entry as its own.
auto beamCacheKey(const std::string &coreSource, const std::string &erlc,
                  const std::string &flags) -> std::string {
  std::string material = "kex-beam-cache-v2\n" + erlc + "\n" + flags + "\n" +
                         coreSource;
  return kex::sha256Hex(material);
}

// Stores a freshly compiled .beam under its key. Written beside the final name
// and renamed into place, so two builds sharing the cache never read half a
// file; any failure only means the next build compiles that module again.
auto storeCachedBeam(const std::filesystem::path &entry,
                     const std::filesystem::path &beam) -> void {
  kex::cache::atomicCopy(beam, entry);
}

// ── Run-result cache (kexhq/kex#323) ────────────────────────────────────────
//
// The compiled-module cache above skips erlc for a module whose Core Erlang
// is unchanged, but kex's own frontend (parse the dependency closure,
// type-check it, lower it, emit Core Erlang) still reruns from scratch on
// every invocation — the cost `tey/spec/*.spec.kex` pays 18 times over for
// the ~15-26 Tey/stdlib modules every file depends on identically. Measured
// in isolation (KEX_TIMINGS=1, warm .beam cache): a single file's frontend
// plus erlc costs ~2.1s even with nothing to recompile.
//
// True per-module reuse (skip typechecking a dependency at all, only that
// dependency, only when IT changed) needs real separate compilation — the
// whole program is still typechecked and lowered as one merged AST, so a
// dependency's own cost isn't separable from the entry file's. That is
// "module system and packaging" work, tracked in docs/module-sys-plan.md
// rather than attempted here.
//
// What IS safe to cache without that: for `kex --run` (Mode::Compile,
// compileRun — a throwaway compile-then-execute with no persistent .beam
// artifact to keep in sync), the entire frontend+erlc result is a pure
// function of the entry file's bytes, every resolved dependency's bytes, and
// the toolchain — exactly like the caches above, just drawn one level
// higher, and content-addressed the same way so a wrong hit is not possible.
// A hit skips straight to running `erl` against the cached .beam files.
//
// The one input this can't see is `Kex.embed(path)` — inside a `compiled do`
// block or, via `Template.text`/`Template.html`, standing alone at any call
// site — which reads a file at expansion time that isn't any module's own
// source (kexhq/kex#335 wants that to be inspectable at run time too, which
// would make this worse, not better). Rather than track embed reads, this
// cache simply refuses to apply — falls back to the normal, always-correct
// path — whenever "compiled" or "Kex.embed" appears anywhere in the entry
// file, the merged `.spec.kex` base, or any resolved dependency's source
// text. A false positive (the word appears in a comment or string) only
// costs a cache miss; it can never cause a stale hit.
auto runCacheRoot() -> std::optional<std::filesystem::path> {
  if (auto base = kex::cache::base()) return *base / "run";
  return std::nullopt;
}

// A conservative, lexer-free scan for the actual `compiled do ... end` block
// syntax — not just the English word "compiled", which shows up constantly
// in ordinary comments and doc text ("a compiled Kex module", "gets compiled
// into a .beam"). Requires the KEYWORD "compiled" (word-bounded, so it
// doesn't fire inside `precompiled` either) followed, after whitespace only,
// by the keyword "do". A false positive here only costs a cache miss; a
// false negative would let a stale hit through, so this stays deliberately
// narrow rather than trying to also rule out e.g. a same-named identifier.
auto sourceMentionsCompiledBlock(const std::string &src) -> bool {
  auto isWordChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '?' || c == '!';
  };
  size_t pos = 0;
  while ((pos = src.find("compiled", pos)) != std::string::npos) {
    const bool leftBoundary = pos == 0 || !isWordChar(src[pos - 1]);
    size_t after = pos + 8; // strlen("compiled")
    const bool rightBoundary = after >= src.size() || !isWordChar(src[after]);
    if (leftBoundary && rightBoundary) {
      while (after < src.size() && std::isspace(static_cast<unsigned char>(src[after])))
        ++after;
      if (src.compare(after, 2, "do") == 0 &&
          (after + 2 >= src.size() || !isWordChar(src[after + 2])))
        return true;
    }
    pos += 8;
  }
  return false;
}

// A conservative, lexer-free scan for `Kex.embed(...)` — the call form
// `EmbedFolder::claim` (src/compiled/expand.cxx) recognizes, requiring the
// literal `Kex` receiver (no alias or bare `embed(...)` is matched by that
// macro, so none needs matching here either). Reachable outside any
// `compiled do` block — `Template.text(Kex.embed(path))` and
// `Template.html(Kex.embed(path))` call it directly — so
// `sourceMentionsCompiledBlock` alone missed it: a file changed between two
// runs of the exact same entry+deps was served the FIRST run's embedded
// text, silently, forever (kexhq/kex#reported-as-run-cache-staleness). Same
// false-positive-tolerant, false-negative-intolerant contract as that scan.
auto sourceMentionsEmbed(const std::string &src) -> bool {
  auto isWordChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '?' || c == '!';
  };
  size_t pos = 0;
  while ((pos = src.find("Kex", pos)) != std::string::npos) {
    const bool leftBoundary = pos == 0 || !isWordChar(src[pos - 1]);
    size_t after = pos + 3; // strlen("Kex")
    if (leftBoundary && src.compare(after, 1, ".") == 0 &&
        src.compare(after + 1, 5, "embed") == 0) {
      const size_t nameEnd = after + 1 + 5;
      if (nameEnd >= src.size() || !isWordChar(src[nameEnd]))
        return true;
    }
    pos += 3;
  }
  return false;
}

// nullopt means "don't even try": the cache is off, or something in the
// closure makes it unsafe (see the block comment above).
// KEX_RUN_CACHE_DEBUG=1 says which of the three provably-safe checks kept
// this run off the cache — same spirit as KEX_TIMINGS, just for "did it
// apply" instead of "how long did it take".
auto runCacheMiss(const char *why) -> std::optional<std::string> {
  if (std::getenv("KEX_RUN_CACHE_DEBUG"))
    std::fprintf(stderr, "run cache: miss (%s)\n", why);
  return std::nullopt;
}

auto runCacheKeyFor(const std::string &filepath, bool skipCheck,
                    const std::string &entryRawSource,
                    const std::string &specBaseFile,
                    const std::string &specBaseRawSource,
                    const std::vector<LoadedDep> &deps)
    -> std::optional<std::string> {
  if (!runCacheRoot()) return runCacheMiss("KEX_CACHE is off");
  if (sourceMentionsCompiledBlock(entryRawSource))
    return runCacheMiss("entry file has a `compiled do` block");
  if (sourceMentionsEmbed(entryRawSource))
    return runCacheMiss("entry file calls `Kex.embed`");
  if (!specBaseFile.empty()) {
    if (sourceMentionsCompiledBlock(specBaseRawSource))
      return runCacheMiss("its .spec.kex base has a `compiled do` block");
    if (sourceMentionsEmbed(specBaseRawSource))
      return runCacheMiss("its .spec.kex base calls `Kex.embed`");
  }
  std::vector<const LoadedDep *> sorted;
  for (const auto &dep : deps) sorted.push_back(&dep);
  std::sort(sorted.begin(), sorted.end(), [](const auto *a, const auto *b) {
    return *a->path < *b->path;
  });
  std::string material = "kex-run-cache-v2\n" + kex::cache::toolchainFingerprint() +
                        "\n" + (separateBuildRequested() ? "separate\n" : "merged\n") +
                        filepath + "\n" +
                        (skipCheck ? "nocheck\n" : "check\n");
  material += "entry:" + kex::sha256Hex(entryRawSource) + "\n";
  if (!specBaseFile.empty())
    material += "specbase:" + specBaseFile + ":" +
                kex::sha256Hex(specBaseRawSource) + "\n";
  for (const auto *dep : sorted) {
    if (sourceMentionsCompiledBlock(*dep->source))
      return runCacheMiss(("a dependency (" + *dep->path +
                           ") has a `compiled do` block")
                              .c_str());
    if (sourceMentionsEmbed(*dep->source))
      return runCacheMiss(
          ("a dependency (" + *dep->path + ") calls `Kex.embed`").c_str());
    material += "dep:" + *dep->path + ":" + kex::sha256Hex(*dep->source) + "\n";
  }
  return kex::sha256Hex(material);
}

auto runCacheDir(const std::string &key) -> std::filesystem::path {
  return *runCacheRoot() / key.substr(0, 2) / key;
}

// A cache hit: every module the previous matching run produced, in the same
// order (index 0 is the entry module — order matters for the BEAM load
// expression and for `mainArity`, which is only meaningful on that one).
auto tryLoadRunCacheFrom(const std::filesystem::path &dir, const std::string &outputDir)
    -> std::optional<std::vector<kex::ir::EmitResult>> {
  namespace fs = std::filesystem;
  std::ifstream manifest(dir / "manifest.txt");
  if (!manifest) return std::nullopt;
  std::string line;
  if (!std::getline(manifest, line) || line != "kex-run-cache-v1")
    return std::nullopt;
  std::vector<kex::ir::EmitResult> results;
  while (std::getline(manifest, line)) {
    if (line.empty()) continue;
    const auto tab = line.find('\t');
    if (tab == std::string::npos) return std::nullopt;
    kex::ir::EmitResult result;
    result.moduleName = line.substr(0, tab);
    try {
      result.mainArity = std::stoi(line.substr(tab + 1));
    } catch (const std::exception &) {
      return std::nullopt;
    }
    results.push_back(std::move(result));
  }
  if (results.empty()) return std::nullopt;
  std::error_code ec;
  for (const auto &result : results) {
    const auto cached = dir / (result.moduleName + ".beam");
    const auto target =
        fs::path{outputDir} / (result.moduleName + ".beam");
    if (!fs::is_regular_file(cached, ec)) return std::nullopt;
    fs::copy_file(cached, target, fs::copy_options::overwrite_existing, ec);
    if (ec) return std::nullopt;
  }
  return results;
}

auto tryLoadRunCache(const std::string &key, const std::string &outputDir)
    -> std::optional<std::vector<kex::ir::EmitResult>> {
  return tryLoadRunCacheFrom(runCacheDir(key), outputDir);
}

// Written beside the final name and renamed into place, matching every other
// cache in this file — a build reading it mid-write never sees a half-copied
// entry, and a failed write only costs the next run a cache miss.
auto storeRunCacheIn(const std::filesystem::path &dir, const std::string &outputDir,
                     const std::vector<kex::ir::EmitResult> &results) -> bool {
  const std::filesystem::path output{outputDir};
  for (const auto &result : results)
    if (!kex::cache::atomicCopy(output / (result.moduleName + ".beam"),
                                dir / (result.moduleName + ".beam")))
      return false;
  std::string manifest = "kex-run-cache-v1\n";
  for (const auto &result : results)
    manifest += result.moduleName + "\t" + std::to_string(result.mainArity) + "\n";
  return kex::cache::atomicWrite(dir / "manifest.txt", manifest);
}

auto storeRunCache(const std::string &key, const std::string &outputDir,
                   const std::vector<kex::ir::EmitResult> &results) -> void {
  (void)storeRunCacheIn(runCacheDir(key), outputDir, results);
}

// ── Run cache by entry (kexhq/kex#323) ──────────────────────────────────────
//
// The cache above is keyed by the whole closure's content, so it is only
// known once every dependency has been found — after the gating pre-check
// and dependency loading, most of a large program's build — and it refuses
// any program with a `compiled do` block or `Kex.embed` in it, whose result
// depends on files no module's source names. This one is keyed by the entry
// and the flags alone, and holds, beside the modules, every file the build
// that made them read, with its stamp (compiled::fileStamp): the entry, its
// `.spec.kex` base, every dependency, everything compile-time evaluation
// loaded or embedded (the build's DependencyRecorder), and each place a new
// file could take a module's name from it — the same relative path under
// every other root, and every base a spec could have. When every stamp still
// holds, the run starts from these modules at once; any one changed, and the
// build runs as it would have.
//
// Not kept when a compile-time evaluation reached outside the program
// (compiled::reachedOutside), nor when a file it read changed while it ran.

// The process's own recorder, for the whole of a build that reuses
// collapses (main): what the entry run cache lists.
std::optional<kex::compiled::DependencyRecorder> buildRecorder;

// When this process started building: a file modified since may have changed
// after it was read, and its stamp would vouch for what was not compiled.
const auto buildStarted = std::filesystem::file_time_type::clock::now();

auto runManifestKey(const std::string &flags, const std::string &filepath)
    -> std::optional<std::string> {
  if (!runCacheRoot()) return std::nullopt;
  std::error_code ec;
  const auto entry = std::filesystem::weakly_canonical(filepath, ec);
  if (ec) return std::nullopt;
  // The compiler that made the kept modules too: the .beam cache below keys
  // on it, but a run started from this entry never reaches that cache.
  return kex::sha256Hex("kex-run-by-entry-v2\n" + kex::cache::toolchainFingerprint() + "\n" +
                   erlcIdentity() + "\n" +
                   (separateBuildRequested() ? "separate\n" : "merged\n") +
                   entry.string() + "\n" + flags);
}

auto runManifestDir(const std::string &key) -> std::filesystem::path {
  return *runCacheRoot() / "entry" / key.substr(0, 2) / key;
}

// Whether the build an entry holds was made from files exactly as they are.
auto runManifestCurrent(const std::string &key) -> bool {
  std::ifstream in(runManifestDir(key) / "files.txt");
  std::string line;
  if (!in || !std::getline(in, line) || line != "kex-run-by-entry-v1") return false;
  bool any = false;
  while (std::getline(in, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos) return false;
    if (kex::compiled::fileStamp(line.substr(tab + 1)) != line.substr(0, tab))
      return false;
    any = true;
  }
  return any;
}

// What an entry's build was made from (see above), canonically named.
auto runManifestFiles(const std::string &filepath, const std::string &specBase,
                      const std::vector<LoadedDep> &deps) -> std::set<std::string> {
  namespace fs = std::filesystem;
  std::set<std::string> files;
  auto add = [&](const std::string &path) {
    if (path.empty()) return;
    std::error_code ec;
    auto canonical = fs::weakly_canonical(path, ec);
    files.insert(ec ? path : canonical.string());
  };
  if (buildRecorder) files = buildRecorder->files();
  add(filepath);
  add(specBase);
  for (const auto &candidate : specBaseCandidates(filepath)) add(candidate);
  for (const auto &dep : deps) add(*dep.path);
  auto roots = moduleRootsFor(filepath);
  for (auto &root : kex::standardLibraryModuleRoots()) roots.push_back(std::move(root));
  const auto shadows = kex::compiled::shadowsOf(files, roots);
  files.insert(shadows.begin(), shadows.end());
  return files;
}

auto storeRunManifest(const std::string &key, const std::string &outputDir,
                      const std::vector<kex::ir::EmitResult> &results,
                      const std::set<std::string> &files) -> void {
  namespace fs = std::filesystem;
  std::string stamps;
  for (const auto &file : files) {
    std::error_code ec;
    if (fs::exists(file, ec) && fs::last_write_time(file, ec) >= buildStarted)
      return;
    stamps += kex::compiled::fileStamp(file) + "\t" + file + "\n";
  }
  const auto dir = runManifestDir(key);
  std::error_code ec;
  // The file list last: until it is in place, the entry is not current.
  fs::remove(dir / "files.txt", ec);
  if (!storeRunCacheIn(dir, outputDir, results)) return;
  kex::cache::atomicWrite(dir / "files.txt", "kex-run-by-entry-v1\n" + stamps);
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
// comparison operators (`==`, `!=`, `<=`, `>=`) and the arm arrow `=>` are
// skipped.
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
      if (i + 1 < source.size() &&
          (source[i + 1] == '=' || source[i + 1] == '>')) { i++; continue; }
      if (i > from && (source[i - 1] == '!' || source[i - 1] == '<' ||
                       source[i - 1] == '>' || source[i - 1] == ':'))
        continue;
      return i;
    }
  }
  return std::string::npos;
}

// Names bound by a REPL input's trailing `let`/`var`, or empty if it is
// not one.
auto replBoundNames(const std::string &source) -> std::vector<std::string> {
  std::vector<std::string> names;
  kex::Lexer lexer("main do\n" + source + "\nend\n");
  kex::Parser parser(lexer.tokenizeAll());
  auto program = parser.parseProgram();
  if (!parser.diagnostics().empty()) return names;
  for (const auto &item : program.items) {
    const auto *main = std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&item);
    if (!main || !*main || (*main)->body.empty()) continue;
    const auto &kind = (*main)->body.back()->kind;
    if (const auto *binding = std::get_if<kex::ast::LetExpr>(&kind)) {
      if (binding->pattern) collectPatternNames(*binding->pattern, names);
    } else if (const auto *mutableBinding =
                   std::get_if<kex::ast::VarExpr>(&kind)) {
      if (mutableBinding->pattern)
        collectPatternNames(*mutableBinding->pattern, names);
    }
  }
  return names;
}

// A line rebinding only SOME names of an earlier pattern (`var (i, j)`
// then `var j = 5`) cannot replay: the earlier line is atomic, so replaying
// it redeclares `j` while dropping it loses `i`. Report what to do instead
// before analysis fails later with a redeclare (PR #607 review).
auto throwIfPartialPatternRebind(
    const std::vector<std::string> &bound,
    const std::vector<std::pair<std::string, std::string>> &binds) -> void {
  if (bound.empty()) return;
  for (const auto &[_, line] : binds) {
    const auto prior = replBoundNames(line);
    if (prior.size() < 2) continue;
    std::string first;
    bool all = true;
    for (const auto &p : prior) {
      const bool rebound =
          std::find(bound.begin(), bound.end(), p) != bound.end();
      if (rebound && first.empty()) first = p;
      all = all && rebound;
    }
    if (!first.empty() && !all)
      throw std::runtime_error(
          "Cannot rebind '" + first +
          "' alone: re-enter the whole pattern to rebind a destructured name");
  }
}

auto printUsage(const char *progName) -> void {
  std::cerr
      << "Usage: " << progName << " [options] <file.kex>\n"
      << "\n"
      << "Options:\n"
      << "  -r, --run         Run on BEAM (default; .kex or existing .beam — "
         "temp dir,\n"
      << "                    auto-clean). With no file: the BEAM REPL, also "
         "the default\n"
      << "  -c, --compile     Compile to BEAM via Core Erlang\n"
      << "  -R, --run-walker  Run with the tree-walk interpreter\n"
      << "  -i, --interactive Interactive tree-walk REPL\n"
      << "  -C, --check       Run semantic analysis only\n"
      << "  -n, --no-check    Skip semantic check when running\n"
      << "  -l, --lex         Print token stream\n"
      << "  -p, --parse       Print AST\n"
      << "  -j, --json        With --check: output diagnostics as JSON\n"
      << "      --lsp         Run the Kex language server over stdio\n"
      << "  -s, --summary     Print public API signatures (Kex syntax)\n"
      << "  -t, --types       With --check: dump inferred expression types\n"
      << "  -e, --eval <code> Enter <code> in the REPL before its first prompt, as\n"
      << "                    if typed (repeatable). A failing line ends the\n"
      << "                    session with status 1. Takes no file; -i picks the\n"
      << "                    tree-walk REPL\n"
      << "  -E, --emit-core   Emit Core Erlang (.core) — does not invoke erlc\n"
      << "      --emit-ast    Emit the parsed Kex AST as ETF\n"
      << "      --dump-hir    With -C: list each call site and what the\n"
         "                    checker decided about it\n"
      << "      --verify-hir  With -C: fail when a call site is undecided\n"
      << "      --print-passes List the compile pipeline and IR passes\n"
      << "      --emit-syntax Emit the lossless syntax tree as ETF\n"
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
      << "      --progress    Show each build step as it starts (on for -c)\n"
      << "  -q, --quiet       Show no build steps, not even erlc's\n"
      << "      --sname <name>, --name <name@host>\n"
      << "                    Run the BEAM VM as a distributed node (short or\n"
      << "                    fully-qualified host name), reachable by `Node.connect`\n"
      << "      --cookie <cookie>\n"
      << "                    The shared secret nodes need to connect\n"
      << "      --credits\n"
      << "                    Roll credits\n"
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
      << "  --colors          Use ANSI colors even when output is not a terminal\n"
      << "                    (also: FORCE_COLOR=1)\n"
      << "  --no-colors       Disable ANSI color output (also: NO_COLOR=1)\n";
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

// getopt_long reports a long-only option (one with no short flag) by number.
// One name per option, shared by the table and the switch below. Only the
// first value is fixed: short options come back as their character, so the
// long-only markers start past the char range and auto-increment from
// there. The values are never persisted — only their uniqueness matters —
// so new options just append a name.
namespace Option {
enum Value {
  BuildPrelude = UCHAR_MAX + 1,
  NoPrelude,
  Expand,
  CollapseReport,
  Lsp,
  SourceRoot,
  Info,
  AllowMocks,
  EmitAst,
  AstFilename,
  TestJson,
  TestList,
  TestOnly,
  EmitSyntax,
  Sname,
  Name,
  Cookie,
  Colors,
  Credits,
  Progress,
  DumpHir,
  VerifyHir,
  PrintPasses,
};
} // namespace Option

// What the CLI was asked to do. One enumerator per mode, chosen by the
// flags below and defaulted after option parsing; `compileRun` refines
// `Compile` (emit files vs. compile-and-run on the BEAM).
enum class Mode {
  Unset,
  Lex,
  Parse,
  Check,
  Compile,
  Run,
  Repl,
  BeamRepl,
  EmitCore,
  EmitAst,
  EmitSyntax,
  Expand,
  Complete,
  Lsp,
};

#ifdef HAS_READLINE
// Loads the REPL history both REPLs share (~/.config/kex/history), so a
// session in one is recalled in the other; returns the file to save it to,
// "" when there is no home directory.
static auto loadReplHistory() -> std::string {
  const char *home = std::getenv("HOME");
  if (!home) return "";
  const auto directory = std::filesystem::path(home) / ".config" / "kex";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  const auto file = (directory / "history").string();
  read_history(file.c_str());
  return file;
}
#endif

// What a line typed at a REPL is: a definition the session keeps (function,
// constant, type, module, ...), a local binding it replays on every later
// input (`let x = v`, `var x = v`, `let (a, b) = v`), or else an expression
// to evaluate. Shared by both REPLs, which used to classify separately and
// drifted: only one knew that the `=>` of a match arm is not a binding's `=`.
struct ReplInput {
  bool definition = false;
  bool localBinding = false;
  bool mutableBinding = false;          // `var`
  std::string bindingName;              // `let x` / `var x`
  std::vector<std::string> patternNames; // `let (a, b)` / `var (i, j)`

  // Every name the line binds: one for `let x`/`var x`, the whole pattern
  // for `let (a, b)`/`var (i, j)`.
  auto boundNames() const -> std::vector<std::string> {
    if (!localBinding) return {};
    return patternNames.empty() ? std::vector<std::string>{bindingName}
                                : patternNames;
  }
};

static auto classifyReplInput(const std::string &source) -> ReplInput {
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
      patternLetNames = replBoundNames(source);
      isLocalLet = !patternLetNames.empty();
      if (!isLocalLet) isFuncDef = true;
    } else if (off != std::string::npos) {
      auto parenPos = source.find('(', off);
      // Not a bare find('='): the `=>` of a receive or match arm in a
      // `let name do ... end` body read as a binding, so the 0-arity
      // definition became a local holding a lambda — which turned into a
      // badfun once the session module had been reloaded twice.
      auto eqPos = replBindingEqualsPos(source, off);
      size_t nameEnd = off;
      while (nameEnd < source.size() &&
             (std::isalnum((unsigned char)source[nameEnd]) ||
              source[nameEnd] == '_' || source[nameEnd] == '?' ||
              source[nameEnd] == '!'))
        nameEnd++;
      const auto afterName = source.find_first_not_of(" \t", nameEnd);
      const bool doBody = afterName != std::string::npos &&
                          source.compare(afterName, 2, "do") == 0 &&
                          (afterName + 2 == source.size() ||
                           !std::isalnum((unsigned char)source[afterName + 2]));
      if (doBody) eqPos = std::string::npos;
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
      const char lead = 4 < source.size() ? source[4] : '\0';
      const bool patternForm =
          lead == '(' || lead == '[' || lead == '{' ||
          std::isupper((unsigned char)lead);
      if (patternForm) {
        // `var (i, j) = ...` binds through its pattern like `let` does;
        // replayed per-name as `var` below (kexhq/kex#602).
        patternLetNames = replBoundNames(source);
        isLocalLet = !patternLetNames.empty();
      } else {
        size_t i = 4;
        while (i < source.size() &&
               (std::isalnum((unsigned char)source[i]) || source[i] == '_'))
          i++;
        letVarName = source.substr(4, i - 4);
        isLocalLet = !letVarName.empty() &&
                     source.find('=', i) != std::string::npos;
      }
      isMutableLet = isLocalLet;
    }
  }
  if (source.rfind("module ", 0) == 0 || source.rfind("type ", 0) == 0 ||
      source.rfind("record ", 0) == 0 || source.rfind("make ", 0) == 0 ||
      source.rfind("serving ", 0) == 0 || source.rfind("using ", 0) == 0 ||
      isReplCompiledBlock(source))
    isFuncDef = true;

  if (source.rfind("foul module ", 0) == 0) isFuncDef = true;
  return {isFuncDef, isLocalLet, isMutableLet, letVarName, patternLetNames};
}

// What a REPL step tells the input loop: read the next input, or stop (a
// script fed to the REPL has failed).
enum class ReplStep { Next, Stop };

// The BEAM REPL (`kex -i`, and `kex -c --run` without a file): every input is
// lowered and evaluated in a long-lived BEAM VM. One session: that VM, and
// everything defined in it so far.
class BeamReplSession {
public:
  explicit BeamReplSession(bool skipPrelude) : m_skipPrelude(skipPrelude) {}
  auto run() -> int;

private:
  bool m_skipPrelude;
  // Writable directory for each evaluation's beams, and the prebuilt runtime.
  std::string m_beamDir;
  std::string m_rtPaDir;
  // ONE persistent erl VM for the whole session — driven by
  // runtime/src/kex_repl_driver.erl over stdin/stdout. This is what makes
  // spawned processes and registered names survive across inputs, and turns
  // session recompiles into hot-loads instead of cold re-runs.
  BeamVm m_vm;
  // Completion's index (prelude + session) and the one inputs are checked in.
  kex::semantic::SemanticDB m_beamReplDb;
  kex::semantic::SemanticDB m_beamAnalysisDb;
  // Top-level definitions, tracked by name so redefining a function
  // REPLACES its earlier clauses rather than appending duplicates (a
  // stale first clause would otherwise shadow the new one — the
  // "this clause cannot match" BEAM warning). Insertion order kept so
  // independent defs stay in source order.
  std::vector<std::pair<std::string, std::string>> m_topDefs;
  std::string m_localBinds; // let x = ... — re-emitted inside main do each eval
  // Names bound with `var`. They are replayed as `var` (not `let`) so a
  // later line may reassign or `!`-mutate them, and every evaluation stores
  // them back into the process dictionary so those mutations survive.
  std::vector<std::string> m_mutableBinds;
  // Original local binding sources retained for semantic replay. Runtime
  // replay uses process-dictionary lookups, which intentionally erase the
  // source type and therefore cannot enforce typestate by itself.
  std::vector<std::pair<std::string, std::string>> m_beamSemanticBinds;
  std::vector<ReplSessionWith> m_sessionWiths;
  // Each input's session module gets a name of its own. Reloading one stable
  // name on every input meant the BEAM purged the version two inputs back —
  // killing any process spawned there that was still blocked in `receive`,
  // and turning every fun created there into a badfun.
  int m_sessionVersion = 0;
  // The Core Erlang last loaded for each companion module (a `module`
  // defined in the session). One is reloaded only when its code changed,
  // so its processes are purged only by redefinition, as in Erlang.
  std::map<std::string, std::string> m_loadedCompanionCore;
  std::vector<std::string> m_loadedBeamFiles; // .kex paths loaded via /load
  // Where a module's `using`s are found (see `run`), and those roots minus
  // the standard library's.
  std::vector<std::string> m_replRoots;
  std::vector<std::string> m_replProjectRoots;
  kex::beam::KexiRegistry m_kexiRegistry;

  auto isMutableBind(const std::string &name) const -> bool {
    return std::find(m_mutableBinds.begin(), m_mutableBinds.end(), name) !=
           m_mutableBinds.end();
  }
  // Prepends the loaded files' declarations (and `extra`, a file being
  // `/load`-ed) to a checker program, then the project modules they and the
  // input use. Installed stdlib modules stay external: their source would
  // shadow their typed interface (see the expression check in `evaluate`).
  auto mergeLoadedForCheck(kex::ast::Program &program,
                           const std::string &extra = {}) -> void {
    auto files = m_loadedBeamFiles;
    if (!extra.empty()) files.push_back(extra);
    prependDeclarationsOf(program, files);
    resolveBeamDeps(program, m_replProjectRoots, &preludeSemanticInterfaces(),
                    sourceModulesNamedBy(program));
  }
  auto topDefsStr() const -> std::string {
    std::string s;
    for (const auto &[name, source] : m_topDefs) s += source + "\n";
    return s;
  }
  // `/load file`: a compiled `.beam` joins through its KexI, a `.kex` file
  // once it parses and checks with the session.
  auto load(const std::string &filePath) -> ReplStep;
  // Evaluates one complete input (`source`, read as `shape`) in the session,
  // `pendingWith` being a `/with` waiting for its value to evaluate.
  auto evaluate(const std::string &source, const ReplInput &shape,
                const std::optional<ReplSessionWith> &pendingWith) -> ReplStep;
  auto define(const std::string &source) -> void;
  auto loadModules(const std::vector<kex::ir::EmitResult> &results) -> bool;
};

auto runBeamRepl(bool skipPrelude) -> int {
  BeamReplSession session(skipPrelude);
  return session.run();
}

auto BeamReplSession::load(const std::string &filePath) -> ReplStep {
  if (!fileExists(filePath)) {
    std::cerr << "  /load: file not found: " << filePath << "\n";
    return ReplStep::Next;
  }

  // Compiled .beam/.kx.beam: load via KexI registry + hot-load into VM
  bool isBeam = (filePath.size() > 5 &&
                 filePath.substr(filePath.size() - 5) == ".beam");
  if (isBeam) {
    auto absPath = std::filesystem::weakly_canonical(filePath).string();
    auto errors = m_kexiRegistry.loadUnit(absPath);
    if (!errors.empty()) {
      for (const auto& err : errors)
        std::cerr << "  /load: " << err.message << "\n";
      return ReplStep::Next;
    }
    auto* unit = m_kexiRegistry.getUnit(
        m_kexiRegistry.lastLoadedEntryAtom());
    if (!unit) {
      std::cerr << "  /load: internal error\n";
      return ReplStep::Next;
    }
    // Hot-load each module into the running BEAM VM using the
    // VM's "load" protocol command.
    bool loadOk = true;
    for (const auto& mod : unit->modules) {
      if (!vmRequest(m_vm, "load", mod.beamAtom + " " + mod.beamPath).ok()) {
        std::cerr << "  /load: failed to hot-load " << mod.beamAtom
                  << "\n";
        loadOk = false;
        break;
      }
    }
    if (!loadOk) return ReplStep::Next;

    auto displayExpr = m_kexiRegistry.generateDisplayRegistration(*unit);
    if (!displayExpr.empty()) vmRequest(m_vm, "exec", displayExpr);

    auto stubs = m_kexiRegistry.generateCompletionStubs(*unit);
    if (!stubs.empty()) {
      try {
        m_beamReplDb.updateFile("<kexi:" +
            m_kexiRegistry.lastLoadedEntryAtom() + ">", stubs);
      } catch (...) {}
    }

    std::cout << "  loaded " << filePath;
    if (unit->modules.size() > 1)
      std::cout << " (" << unit->modules.size() << " modules)";
    std::cout << "\n";
    return ReplStep::Next;
  }

  // Source .kex file: validate syntax and register for session build
  try {
    auto src = readFile(filePath);
    kex::Lexer lex(std::move(src), filePath);
    kex::Parser parser(lex.tokenizeAll(), filePath);
    parser.parseProgram();
    throwOnParseErrors(parser);
  } catch (const std::exception& e) {
    std::cerr << "  /load: " << filePath << " was not loaded: "
              << e.what() << "\n";
    return ReplStep::Next;
  }
  // Checked with the session and the modules it uses before it joins:
  // a file that does not check is refused here, rather than kept and
  // replayed into every later input, each of which then failed with
  // its error.
  try {
    kex::Lexer checkLexer(topDefsStr() + "main do\nend\n", "<repl>");
    kex::Parser checkParser(checkLexer.tokenizeAll(), "<repl>");
    auto checkProgram = checkParser.parseProgram();
    expandReplProgram(checkProgram);
    mergeLoadedForCheck(checkProgram, filePath);
    kex::semantic::Analyzer checkAnalyzer(&preludeSemanticInterfaces());
    if (!checkAnalyzer.analyze(checkProgram)) {
      std::string message = "semantic analysis failed";
      for (const auto &diagnostic : checkAnalyzer.diagnostics())
        if (diagnostic.level == kex::semantic::Diagnostic::Level::Error) {
          message = diagnostic.message;
          break;
        }
      throw std::runtime_error(message);
    }
  } catch (const std::exception& e) {
    std::cerr << "  /load: " << filePath << " was not loaded: "
              << e.what() << "\n";
    if (g_replLineFromScript) {
      g_replScriptFailed = true;
      return ReplStep::Stop;
    }
    return ReplStep::Next;
  }
  m_loadedBeamFiles.push_back(filePath);
  std::cout << "  loaded " << filePath << "\n";
  return ReplStep::Next;
}

// A definition (`let f(...)`, a record, a module, ...): checked against the
// session's definitions, then kept, replacing an earlier one of that name.
auto BeamReplSession::define(const std::string &source) -> void {
  // Validate by parsing against accumulated defs.
  std::string check = topDefsStr() + source + "\n";
  kex::Lexer lexer(check);
  auto tokens = lexer.tokenizeAll();
  kex::Parser parser(std::move(tokens));
  parser.parseProgram();
  throwOnParseErrors(parser);

  std::string fname = replDefinitionName(source);
  const auto previousTopDefs = m_topDefs;

  // A new clause of a function already defined EXTENDS it; anything
  // else replaces what was there. Redefinition still takes effect
  // (last def wins) because a clause whose parameter patterns match
  // an existing one overwrites that clause rather than being added
  // beside it — otherwise the stale clause would shadow the new one
  // and BEAM would warn that it cannot match.
  auto existing =
      std::find_if(m_topDefs.begin(), m_topDefs.end(),
                   [&](const auto &p) { return p.first == fname; });
  const auto head = replClauseHead(source);
  if (existing != m_topDefs.end() && head) {
    // Substituted IN PLACE, never moved to the end: clause order is
    // match order, so hoisting a catch-all above a base case turns
    // `fact` into an infinite recursion.
    std::string rebuilt;
    bool substituted = false;
    for (const auto &clause :
         splitReplClauses(existing->second, replBlockDepth)) {
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
    m_topDefs.erase(
        std::remove_if(m_topDefs.begin(), m_topDefs.end(),
                       [&](const auto &p) { return p.first == fname; }),
        m_topDefs.end());
    m_topDefs.push_back({fname, source});
  }

  // A parse is not enough: a definition the checker rejects (a pure
  // `let` that receives, say) was kept anyway, and every later input
  // — which replays all definitions — then failed with its error.
  // Any failure here restores the earlier definitions, including a
  // dependency that cannot be loaded (thrown from mergeLoadedForCheck).
  try {
    const auto checkSource = topDefsStr() + "main do\nend\n";
    kex::Lexer checkLexer(checkSource, "<repl>");
    kex::Parser checkParser(checkLexer.tokenizeAll(), "<repl>");
    auto checkProgram = checkParser.parseProgram();
    expandReplProgram(checkProgram);
    mergeLoadedForCheck(checkProgram);
    kex::semantic::Analyzer checkAnalyzer(&preludeSemanticInterfaces());
    if (!checkAnalyzer.analyze(checkProgram)) {
      for (const auto &diagnostic : checkAnalyzer.diagnostics())
        if (diagnostic.level == kex::semantic::Diagnostic::Level::Error)
          throw std::runtime_error(diagnostic.message);
      throw std::runtime_error("semantic analysis failed");
    }
  } catch (...) {
    m_topDefs = previousTopDefs;
    throw;
  }

  // `using M` is an import, not a definition — it is kept in m_topDefs
  // so it persists across inputs, but saying "defined M" is wrong.
  const bool isImport = source.rfind("using ", 0) == 0;
  // Completion reads the session's own definitions from here: without
  // it the BEAM REPL completed nothing it defined itself — no `make`
  // methods, no serving slots after `pa.` (the tree-walker REPL
  // already indexes its `<repl>` source the same way).
  m_beamReplDb.updateFile("<repl>", topDefsStr());
  std::cout << kex::color::apply(kex::color::gray) << "=> "
            << kex::color::apply(kex::color::reset)
            << (isImport ? "using " : "defined ") << fname << "\n";
}

// Compiles and hot-loads each module in the persistent VM, which is the VM
// that will run the code anyway. This used to fork an `erlc` per module and
// load the .beam it wrote: a whole BEAM starting up to compile one small
// module, ~150 ms a line, which a sampling profile put at 93% of this REPL's
// wall time (kexhq/kex#318). Companion modules go first, in reverse: the
// session entry can then call their functions the moment it is loaded
// itself. False when one did not build, having said why.
auto BeamReplSession::loadModules(
    const std::vector<kex::ir::EmitResult> &results) -> bool {
  bool loaded = true;
  std::vector<std::pair<std::string, std::string>> newlyLoaded;
  for (auto it = results.rbegin(); it != results.rend(); ++it) {
    if (it != std::prev(results.rend())) {
      auto previous = m_loadedCompanionCore.find(it->moduleName);
      if (previous != m_loadedCompanionCore.end() &&
          previous->second == it->source)
        continue;
    }
    std::string corePath = m_beamDir + "/" + it->moduleName + ".core";
    std::ofstream cf(corePath);
    if (!cf) {
      std::cerr << "  error: cannot write " << corePath << "\n";
      loaded = false;
      break;
    }
    cf << it->source;
    cf.close();


    // Whatever the compiler had to say arrives above the sentinel;
    // it is the only account of WHY a module did not build, so it
    // reaches the terminal rather than being dropped.
    auto compiled = vmRequest(m_vm, "compile", corePath);
    const std::string &diagnostics = compiled.output;
    std::filesystem::remove(corePath);
    // A successful compile prints nothing, so what arrived here is
    // output a spawned process wrote after the previous input's
    // sentinel — it was dropped, losing a child's first printLine.
    if (compiled.ok()) std::cout << diagnostics;
    if (!compiled.ok()) {
      if (!diagnostics.empty()) std::cerr << diagnostics;
      loaded = false;
      break;
    }
    if (it != std::prev(results.rend()))
      newlyLoaded.emplace_back(it->moduleName, it->source);
  }
  for (auto &[name, core] : newlyLoaded)
    m_loadedCompanionCore[name] = std::move(core);
  return loaded;
}

auto BeamReplSession::evaluate(
    const std::string &source, const ReplInput &shape,
    const std::optional<ReplSessionWith> &pendingWith) -> ReplStep {
  const bool isFuncDef = shape.definition;
  const bool isLocalLet = shape.localBinding;
  const bool isMutableLet = shape.mutableBinding;
  const std::string &letVarName = shape.bindingName;
  const auto &patternLetNames = shape.patternNames;
  const std::vector<std::string> lineBoundNames = shape.boundNames();
  if (isFuncDef) {
    define(source);
    return ReplStep::Next;
  }
  // Expression or local let — compile and run on BEAM.
  // For local lets, the current binding is evaluated fresh and
  // stashed in the process dictionary so subsequent evals
  // retrieve it (avoiding re-evaluation of side-effectful
  // expressions like Ets.new).
  throwIfPartialPatternRebind(lineBoundNames, m_beamSemanticBinds);
  std::string semanticBindSource;
  for (const auto &[name, binding] : m_beamSemanticBinds)
    if (std::find(lineBoundNames.begin(), lineBoundNames.end(),
                   name) == lineBoundNames.end())
      semanticBindSource += "  " + binding + "\n";
  const auto withExcept =
      pendingWith ? pendingWith->capability : std::string();
  const auto withOpen = replWithOpen(m_sessionWiths, withExcept);
  const auto withClose = replWithClose(m_sessionWiths, withExcept);
  auto semanticSource = topDefsStr() + "main do\n" +
                        semanticBindSource + withOpen + "  " + source +
                        "\n" + withClose + "end\n";
  kex::Lexer semanticLexer(semanticSource, "<repl>");
  kex::Parser semanticParser(semanticLexer.tokenizeAll(), "<repl>");
  auto semanticProgram = semanticParser.parseProgram();
  expandReplProgram(semanticProgram);
  // Loaded declarations go BEFORE the replayed main block: the
  // checker registers a module's functions as it checks them, so a
  // module appended after the call site is registered too late and
  // `Loaded.values()` came back untyped — the walker REPL, which
  // prepends the same source, reported `[Integer]` for it while this
  // one fell back to the runtime `[Int]`.
  mergeLoadedForCheck(semanticProgram);
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
  if (pendingWith) {
    // The value has to be something `with` accepts for the
    // capability; checked here, before it is kept, so a bad one does
    // not break every input after it.
    const auto withCheck =
        topDefsStr() + "main do\n" + semanticBindSource + withOpen +
        "  " + source + "\n  with " + pendingWith->capability + " = " +
        pendingWith->binding + " do\n    0\n  end\n" + withClose +
        "end\n";
    kex::Lexer withLexer(withCheck, "<repl>");
    kex::Parser withParser(withLexer.tokenizeAll(), "<repl>");
    auto withProgram = withParser.parseProgram();
    expandReplProgram(withProgram);
    mergeLoadedForCheck(withProgram);
    kex::semantic::Analyzer withAnalyzer(&preludeSemanticInterfaces());
    if (!withAnalyzer.analyze(withProgram)) {
      for (const auto &diagnostic : withAnalyzer.diagnostics())
        if (diagnostic.level ==
            kex::semantic::Diagnostic::Level::Error)
          throw std::runtime_error(diagnostic.message);
      throw std::runtime_error("semantic analysis failed");
    }
  }
  std::optional<std::string> beamSemanticType;
  for (auto item = semanticProgram.items.rbegin();
       item != semanticProgram.items.rend(); ++item) {
    auto *main =
        std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&*item);
    if (!main || !*main || (*main)->body.empty())
      continue;
    const auto *last = replLastStatement(**main);
    const kex::ast::Expr *typedExpr = last;
    if (const auto *binding =
            std::get_if<kex::ast::LetExpr>(&last->kind);
        binding && binding->value)
      typedExpr = binding->value.get();
    else if (const auto *mutableBinding =
                 std::get_if<kex::ast::VarExpr>(&last->kind);
             mutableBinding && mutableBinding->value)
      typedExpr = mutableBinding->value.get();
    else if (const auto *assigned =
                 std::get_if<kex::ast::AssignExpr>(&last->kind);
             assigned && assigned->value)
      typedExpr = assigned->value.get();
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
    return "  BEAM.erlang.put(:kexrepl" + name + ", " + name + ")\n";
  };
  // Any line can mutate a `var` from an earlier line — by reassigning
  // it or through a `!` method — so every tracked mutable name is
  // written back, not only the one this line binds.
  std::string mutablePuts;
  for (const auto &name : m_mutableBinds)
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
    const auto headRest = source.find_first_not_of(" \t", i);
    // `.field` segments after the head: `p.x = v` writes through the
    // head binding, so the operator scan below starts past them.
    // Segments may sit apart (`p . x = 1` lexes the same).
    size_t seg = i;
    while (true) {
      const auto dot = source.find_first_not_of(" \t", seg);
      if (dot == std::string::npos || source[dot] != '.') break;
      auto k = source.find_first_not_of(" \t", dot + 1);
      if (k == std::string::npos ||
          !(std::isalnum((unsigned char)source[k]) ||
            source[k] == '_'))
        break;
      while (k < source.size() &&
             (std::isalnum((unsigned char)source[k]) ||
              source[k] == '_'))
        k++;
      seg = k;
    }
    const auto opPos = source.find_first_not_of(" \t", seg);
    // `n = v` reassigns; so do the compound forms (`n += v`, `n -=
    // v`, `n *= v`, `n /= v`, `n %= v`, `n &&= v`, `n ||= v` — the
    // same set parseAssignment desugars). A compound operator is one
    // token, so its `=` sits directly against it: `n * 2` stays an
    // expression, and `n == v` stays a comparison (kexhq/kex#604).
    const bool reassigns = [&] {
      if (opPos == std::string::npos) return false;
      if (source[opPos] == '=') {
        return opPos + 1 < source.size() &&
               source[opPos + 1] != '=';
      }
      const char op = source[opPos];
      if (op == '+' || op == '-' || op == '*' || op == '/' ||
          op == '%') {
        return opPos + 1 < source.size() &&
               source[opPos + 1] == '=';
      }
      if (op == '&' || op == '|') {
        return opPos + 2 < source.size() &&
               source[opPos + 1] == op && source[opPos + 2] == '=';
      }
      return false;
    }();
    const bool mutates = headRest != std::string::npos &&
                         source[headRest] == '.' &&
                         source.find('!', headRest) != std::string::npos;
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
    // A `/with` value is not echoed: the command answers for itself.
    kexSource = topDefsStr() + "main do\n" + m_localBinds + withOpen +
                bindLine + puts + mutablePuts +
                (pendingWith ? "" : "  " + inspectCall(shown) + "\n") +
                withClose + "end\n";
  } else if (!mutatedName.empty()) {
    kexSource = topDefsStr() + "main do\n" + m_localBinds + withOpen +
                "  " + source + "\n" + mutablePuts +
                "  " + inspectCall(mutatedName) + "\n" + withClose +
                "end\n";
  } else {
    kexSource = topDefsStr() + "main do\n" + m_localBinds + withOpen +
                "  " + inspectCall(source) + "\n" + mutablePuts +
                withClose + "end\n";
  }

  kex::Lexer lexer(kexSource);
  auto tokens = lexer.tokenizeAll();
  kex::Parser parser(std::move(tokens));
  auto program = parser.parseProgram();
  throwOnParseErrors(parser);
  expandReplProgram(program);
  // Merge non-MainBlock items from files loaded via /load so their
  // definitions (function defs, make blocks, records, types) are
  // visible to every subsequent REPL input. Re-read on each session
  // build so the current on-disk version is always used.
  for (const auto& f : m_loadedBeamFiles) {
    try {
      for (auto &item : declarationsOf(f))
        program.items.push_back(std::move(item));
    } catch (...) {
      // Syntax errors in a loaded file were caught by /load; if the
      // file changed underneath us, just skip it this round.
    }
  }
  // Source-module imports in a REPL expression need the same
  // dependency expansion as `kex -R file.kex`, for both `using` and
  // qualified access such as `Units.SI.Meter`.
  resolveBeamDeps(program, m_replRoots, &preludeSemanticInterfaces(),
                  sourceModulesNamedBy(program));
  auto extMods = mergeExternalModules(
      preludeExternalModules(), m_kexiRegistry.buildExternalModules());
  kex::semantic::Analyzer replAnalyzer(&preludeSemanticInterfaces());
  replAnalyzer.analyze(program);
  auto replRecordLayouts = loadPreludeRecordLayouts();
  auto replVariantTags = loadPreludeVariantTags();
  // All of the checker's answers, `Type.of` included: without them
  // `Type.of(someFunction)` fell back to the runtime, which lowered
  // the bare function name to an unbound Core Erlang variable and
  // failed to compile.
  const kex::hir::Program hir(program, replAnalyzer);
  auto irModules = kex::ir::lowerModules(
      program, "kex_repl_session_" + std::to_string(++m_sessionVersion),
      withCheckerAnswers(
          {.externals = extMods.nameToAtom.empty() ? nullptr : &extMods,
           .externalRecords = &replRecordLayouts,
           .externalVariants = &replVariantTags},
          &hir));
  auto results = kex::ir::emitModules(irModules);
  const auto& result = results.front();

  const bool loaded = loadModules(results);
  if (!loaded) {
    std::cerr << "  " << kex::color::apply(kex::color::red)
              << "error:" << kex::color::apply(kex::color::reset)
              << " failed to load session module\n";
    if (g_replLineFromScript) {
      g_replScriptFailed = true;
      return ReplStep::Stop;
    }
    return ReplStep::Next;
  }
  auto evaluated = vmRequest(m_vm, "eval", result.moduleName);
  const std::string &output = evaluated.output;
  if (evaluated.ok()) {
    std::cout << output;
  } else {
    std::cerr << "  " << kex::color::apply(kex::color::red)
              << "error:" << kex::color::apply(kex::color::reset) << " "
              << output;
    // A boot line that fails at run time must not leave a
    // half-booted session behind its prompt (kexhq/kex#549).
    if (g_replLineFromScript) {
      g_replScriptFailed = true;
      return ReplStep::Stop;
    }
  }

  if (isLocalLet && evaluated.ok()) {
    const auto equals = source.find('=');
    const auto rhs = equals == std::string::npos
        ? std::string::npos
        : source.find_first_not_of(" \t", equals + 1);
    // A lambda literal is replayed from its source rather than read
    // back from the process dictionary. This used to be what kept it
    // from turning into a badfun when the session module reloaded;
    // each input now has a session module of its own, so a stored
    // fun stays valid either way.
    const bool isLambda = rhs != std::string::npos &&
        (source[rhs] == '&' ||
         (source[rhs] == '{' &&
          source.find_first_not_of(" \t", rhs + 1) != std::string::npos &&
          source[source.find_first_not_of(" \t", rhs + 1)] == '|'));
    // A destructuring let binds through its pattern, so every name it
    // introduced has to be replayed, not just a single variable —
    // otherwise `let Just(x) = ...` stored x but later inputs could
    // not see it.
    const std::vector<std::string> &replayNames = lineBoundNames;
    if (isLambda)
      m_localBinds += "  " + source + "\n";
    else
      for (const auto &name : replayNames) {
        // The replayed value comes back from the process dictionary
        // untyped, so lowering could not see what `pa` holds and
        // dispatched `pa.get(k)` dynamically by name — where a slot
        // called `get` lost to every other `get` ("Undefined method:
        // get for Server") while the checker, which replays the
        // original source, had accepted it. Carrying the type the
        // binding was checked at restores the static dispatch a file
        // gets. Only a type that checks as an annotation is carried;
        // anything else (type variables, Unknown) replays untyped.
        std::string annotation;
        if (patternLetNames.empty() && beamSemanticType &&
            replTypeAnnotates(topDefsStr(), *beamSemanticType))
          annotation = " : " + *beamSemanticType;
        // A `var` replays as `var`: replaying it as `let` would make
        // every later line see an immutable binding and reject both
        // `name = v` and `name.foo!(v)`.
        m_localBinds += std::string("  ") +
                      (isMutableLet ? "var " : "let ") + name +
                      annotation +
                      " = BEAM.erlang.get(:kexrepl" + name + ")\n";
        if (isMutableLet && !isMutableBind(name))
          m_mutableBinds.push_back(name);
      }
    m_beamSemanticBinds.erase(
        std::remove_if(
            m_beamSemanticBinds.begin(), m_beamSemanticBinds.end(),
            [&](const auto &binding) {
              return std::find(replayNames.begin(), replayNames.end(),
                               binding.first) != replayNames.end();
            }),
        m_beamSemanticBinds.end());
    m_beamSemanticBinds.emplace_back(replayNames.front(), source);
    if (pendingWith) {
      std::erase_if(m_sessionWiths, [&](const auto &with) {
        return with.capability == pendingWith->capability;
      });
      m_sessionWiths.push_back(*pendingWith);
      std::cout << "  (with " << pendingWith->capability << ")\n";
    }
  }
  return ReplStep::Next;
}

auto BeamReplSession::run() -> int {
  // ── Writable temp dir for per-eval user beams ─────────────────────
  char rtmpl[] = "/tmp/kex_irepl_XXXXXX";
  char *rtd = mkdtemp(rtmpl);
  if (!rtd) {
    std::cerr << "error: mkdtemp failed\n";
    return 1;
  }
  m_beamDir = rtd;

  // Runtime artifacts are produced explicitly by the toolchain build.
  m_rtPaDir = prebuiltRuntimeBeamDir();
  if (m_rtPaDir.empty()) {
    std::cerr << "error: prebuilt runtime artifacts are missing; "
                 "rebuild or reinstall the Kex toolchain\n";
    std::filesystem::remove_all(m_beamDir);
    return 1;
  }

  // Spawn ONE persistent erl VM for the whole session — driven by
  // runtime/src/kex_repl_driver.erl over stdin/stdout. This is what
  // makes spawned processes / registered names survive across REPL
  // inputs and turns session recompiles into hot-loads instead of
  // cold re-runs.
  {
    std::vector<std::string> erlArgs = {erlExecutable(), "-noshell", "-pa",
                                        m_beamDir};
    erlArgs.insert(erlArgs.end(), cliDistributionArgs.begin(),
                   cliDistributionArgs.end());
    if (!m_rtPaDir.empty()) {
      erlArgs.push_back("-pa");
      erlArgs.push_back(m_rtPaDir);
    }
    erlArgs.push_back("-eval");
    // allow_mocks/0 before the loop, in the same process the driver
    // evaluates input in — the BEAM half of "a REPL may mock" (issue #144).
    // The flag lives in the process dictionary, so this grants it for the
    // session and for nothing else.
    erlArgs.push_back(beamOtpFloorGuard() +
                      "kex_test:allow_mocks(), kex_repl_driver:loop()");
    if (!m_vm.start(erlArgs)) {
      std::cerr << "error: could not start erl VM for BEAM REPL\n";
      std::filesystem::remove_all(m_beamDir);
      return 1;
    }
  }

  // readline history — same file as the tree-walker REPL, so a session
  // in one is recalled in the other.
#ifdef HAS_READLINE
  const std::string historyFile = loadReplHistory();
#endif

  {
    auto info = vmRequest(m_vm, "info");
    std::string otpInfo = std::move(info.output);
    // The VM died before answering the first handshake — an OTP too old for
    // the bundled beams (the floor guard has already said so on stderr), a
    // missing runtime module, a broken erl. Without this the REPL prints
    // its banner over the corpse and answers every input with "failed to
    // load session module".
    if (info.status == "eof") {
      m_vm.close();
      std::cerr
          << "error: the BEAM runtime did not start; cannot run the REPL\n";
      std::filesystem::remove_all(m_beamDir);
      return 1;
    }
    while (!otpInfo.empty() && otpInfo.back() == '\n')
      otpInfo.pop_back();
    if (!otpInfo.empty())
      std::cout << kex::color::apply(kex::color::gray) << otpInfo
                << kex::color::apply(kex::color::reset) << "\n";
  }
  kex::printReplBanner(std::cout, "BEAM");

  if (!m_skipPrelude) {
    const auto preludeDisplay = preludeDisplayRegistration();
    if (!preludeDisplay.empty()) vmRequest(m_vm, "exec", preludeDisplay);
  }

  if (!m_skipPrelude) {
    m_beamReplDb.setImportedInterfaces(&preludeSemanticInterfaces());
    loadPrelude(m_beamReplDb);
  }
  m_beamAnalysisDb.setImportedInterfaces(&preludeSemanticInterfaces());
  m_beamAnalysisDb.setModuleRoots(
      moduleRootsFor((std::filesystem::current_path() / "<repl>").string()));
  g_replCompletionDb = &m_beamReplDb;
  g_replAnalysisDb = &m_beamAnalysisDb;
#ifdef HAS_READLINE
  rl_attempted_completion_function = kexCompletion;
  rl_completion_display_matches_hook = kexDisplayMatches;
  rl_completer_word_break_characters = (char *)" \t\n\\@$><=;|&{(";
  rl_completer_quote_characters = (char *)"";
  bindReplCancelKeys();
#endif

  // Where a module's `using`s are found: the session's source roots
  // (`--source-root`, then lib/ and src/ under the working directory), as a
  // build of a file there would search them. A `/load`-ed module used to
  // bring in only its own declarations, so `Store`'s `using Wire` left
  // `Statement` unknown, and every later input failed with that error.
  // A dependency that cannot be loaded is a refused `/load` or a failed
  // input here, not the end of the session.
  g_dependencyFailureThrows = true;
  m_replRoots = moduleRootsFor(
      (std::filesystem::current_path() / "repl.kex").string());
  m_replProjectRoots = kex::withoutStandardLibraryRoots(m_replRoots);

  g_replCompletionContext = [&]() {
    ReplCompletionContext context{topDefsStr(), {}};
    for (const auto &[name, binding] : m_beamSemanticBinds)
      context.bindings += "  " + binding + "\n";
    return context;
  };



  while (true) {
    auto [read, ok] = readLine("kex(beam)> ");
    if (!ok)
      break;
    std::string input = replTrimLeadingIndent(std::move(read));
    if (input.empty())
      continue;
    if (kex::isReplExit(input))
      break;
    const auto command = kex::parseReplCommand(input);
    using Command = kex::ReplCommandKind;
    if (command.kind == Command::Help) {
      kex::printReplHelp(std::cout);
      continue;
    }
    if (command.kind == Command::Clear) {
      std::cout << kex::replClearScreenSequence() << std::flush;
      continue;
    }
    if (command.kind == Command::Reset) {
      m_topDefs.clear();
      m_localBinds.clear();
      m_mutableBinds.clear();
      m_beamSemanticBinds.clear();
      m_sessionWiths.clear();
      m_beamReplDb.removeFile("<repl>");
      std::cout << "  (bindings cleared)\n";
      continue;
    }
    std::optional<ReplSessionWith> pendingWith;
    if (command.kind == Command::With || command.kind == Command::Without) {
      try {
        input = handleReplWithCommand(input, m_sessionWiths, pendingWith);
      } catch (const std::exception &e) {
        std::cerr << "  " << kex::color::apply(kex::color::red)
                  << "error:" << kex::color::apply(kex::color::reset) << " "
                  << e.what() << "\n";
        if (g_replLineFromScript) {
          g_replScriptFailed = true;
          break;
        }
        continue;
      }
      if (input.empty()) continue;
    }
    if (command.kind == Command::Set || command.kind == Command::Unset) {
      std::cerr << "  not yet available in the BEAM REPL\n";
      continue;
    }
    if (command.kind == Command::Complete) {
      printReplCompletions(command.argument);
      continue;
    }
    if (command.kind == Command::Load) {
      if (load(command.argument) == ReplStep::Stop) break;
      continue;
    }
    if (command.kind == Command::Unload) {
      std::string modName = command.argument;

      std::string entryAtom;
      if (m_kexiRegistry.isLoaded(modName))
        entryAtom = modName;
      else
        entryAtom = m_kexiRegistry.findEntryByShortName(modName);

      if (entryAtom.empty()) {
        std::cerr << "  /unload: module '" << modName << "' is not loaded\n";
        continue;
      }

      auto* unit = m_kexiRegistry.getUnit(entryAtom);
      if (unit) {
        for (const auto& mod : unit->modules) {
          vmRequest(m_vm, "exec", "code:purge('" + mod.beamAtom +
                                    "'), code:delete('" + mod.beamAtom + "')");
        }
      }

      m_beamReplDb.removeFile("<kexi:" + entryAtom + ">");
      m_kexiRegistry.unloadUnit(entryAtom);
      std::cout << "  unloaded " << modName << "\n";
      continue;
    }
    if (command.kind == Command::Reload) {
      // Loaded files are re-read on every session build — no explicit
      // reload needed. /reset clears m_topDefs + m_localBinds but keeps
      // loaded paths; use /reset then /load each file again to refresh.
      std::cout << "  (loaded files are re-evaluated on each input)\n";
      continue;
    }

    // Accumulate do/end blocks, then chain consecutive clauses of the
    // same function so `let f(0) = ...` / `let f(n) = ...` combine,
    // and a bare `let f(` header keeps reading instead of being
    // accepted as a broken standalone clause. Matches the tree-walker.
    std::string source = input;
    int dc = replBlockDepth(source);
    bool cancelled = false;
    while (dc > 0 || replHasOpenDelimiter(source)) {
      g_replPendingInput = source;
      auto [cont, contOk] = readLine("  ...> ");
      if (takeReplInputCancelled()) {
        cancelled = true;
        break;
      }
      if (!contOk)
        break;
      source += "\n" + cont;
      dc = replBlockDepth(source);
    }
    g_replPendingInput.clear();
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

    try {
      if (evaluate(source, classifyReplInput(source), pendingWith) ==
          ReplStep::Stop)
        break;
    } catch (const std::exception &e) {
      std::cerr << "  " << kex::color::apply(kex::color::red)
                << "error:" << kex::color::apply(kex::color::reset) << " "
                << e.what() << "\n";
      if (g_replLineFromScript) {
        g_replScriptFailed = true;
        break;
      }
    }
  }

#ifdef HAS_READLINE
  if (!historyFile.empty())
    write_history(historyFile.c_str());
#endif

  m_vm.close();
  std::filesystem::remove_all(m_beamDir);
  return g_replScriptFailed ? 1 : 0;
}

// The tree-walking REPL (`kex` with no file, or a non-BEAM mode). One
// session: the evaluator every input runs in, and what has been defined.
class WalkerReplSession {
public:
  explicit WalkerReplSession(bool skipPrelude) : m_skipPrelude(skipPrelude) {}
  auto run() -> int;

private:
  bool m_skipPrelude;
  // Completion's index (prelude loaded once, updated on each input) and the
  // one inputs are checked in.
  kex::semantic::SemanticDB m_replDb;
  kex::semantic::SemanticDB m_analysisDb;
  // Made in `run`, after the banner: making one loads the prelude.
  std::optional<kex::interpreter::Evaluator> m_evaluator;
  // Accumulated source of all top-level definitions typed in the REPL so
  // far, used to keep the SemanticDB index complete across multiple inputs.
  std::string m_replAccumSource;
  // Local bindings are evaluated one input at a time, but their source is
  // retained here so semantic analysis can infer the type of later inputs.
  std::vector<std::pair<std::string, std::string>> m_replBindings;
  std::vector<ReplSessionWith> m_sessionWiths;
  // Keep parsed programs alive so function closures can reference AST nodes
  std::vector<kex::ast::Program *> m_replPrograms;
  // Every input's checked program, kept for the session: a later input
  // calls an earlier one's definitions, and the evaluator reads them all.
  std::vector<kex::hir::CheckedInput> m_checkedInputs;

  // The program an input is checked in: the session's definitions, then a
  // `main` replaying `bindingSource` (the earlier bindings).
  auto sessionProgram(const std::string &bindingSource)
      -> std::unique_ptr<kex::ast::Program> {
    kex::Lexer lexer(m_replAccumSource + "main do\n" + bindingSource + "end\n",
                     "<repl>");
    kex::Parser parser(lexer.tokenizeAll(), "<repl>");
    auto session = std::make_unique<kex::ast::Program>(parser.parseProgram());
    expandReplProgram(*session);
    return session;
  }
  // What the session reads from disk, merged in before the check.
  auto prepareSession(kex::ast::Program &semantic) -> void {
    prependDeclarationsOf(semantic, m_replLoadedDepFiles);
    // A module the session names (`using Clocks`, `Clocks.stamp()`) is
    // checked from its source, as the BEAM REPL does. Without it the checker
    // knew the module's functions only from what the runtime had loaded, and
    // none of its capabilities: `with Clocks.Ticker = ...` was rejected as
    // "not a capability".
    resolveBeamDeps(semantic, m_walkerProjectRoots, &preludeSemanticInterfaces(),
                    sourceModulesNamedBy(semantic));
  }
  // Keeps `checked` for the session and has the evaluator read it.
  auto keepChecked(kex::hir::CheckedInput checked) -> void {
    if (!checked.program) return;
    m_evaluator->addProgram(checked.program.get());
    m_checkedInputs.push_back(std::move(checked));
  }
  std::vector<LoadedDep> m_replLoadedDeps;
  // Their files, which the input check parses in as declarations: a file
  // with a `module Wire` header cannot be glued onto the session source.
  std::vector<std::string> m_replLoadedDepFiles;
  // Where a `/load`-ed file's `using`s are found: the project's roots.
  std::vector<std::string> m_walkerProjectRoots;
  // `/set` and `/unset` options.
  bool m_showTypes = true;
  bool m_showAst = false;
  bool m_showTokens = false;

  auto handleSet(const std::string &arg, bool enable) -> void {
    if (arg == "types") {
      m_showTypes = enable;
      std::cout << "  types: " << (enable ? "on" : "off") << "\n";
    } else if (arg == "ast") {
      m_showAst = enable;
      std::cout << "  ast: " << (enable ? "on" : "off") << "\n";
    } else if (arg == "tokens") {
      m_showTokens = enable;
      std::cout << "  tokens: " << (enable ? "on" : "off") << "\n";
    } else {
      std::cerr << "  Unknown option: " << arg << "\n";
      std::cerr << "  Available: types, ast, tokens\n";
    }
  }
  auto execProgram(kex::ast::Program *program, bool expanded = false)
      -> kex::interpreter::ValuePtr {
    if (!expanded) expandReplProgram(*program);
    if (m_showAst)
      printAst(*program);
    return m_evaluator->execute(*program);
  }
  auto showResult(const kex::interpreter::ValuePtr &result,
                  const std::optional<std::string> &semanticType = std::nullopt)
      -> void {
    if (result && !std::holds_alternative<kex::interpreter::UnitValue>(
                      result->data)) {
      std::cout << kex::color::apply(kex::color::gray) << "=> "
                << kex::color::apply(kex::color::reset)
                << result->inspect();
      if (m_showTypes) {
        std::cout << " " << kex::color::apply(kex::color::gray) << ":"
                  << kex::color::apply(kex::color::reset) << " "
                  << kex::color::apply(kex::color::cyan)
                  << (semanticType ? *semanticType : result->typeName())
                  << kex::color::apply(kex::color::reset);
      }
      std::cout << "\n";
    }
  }
  // `/load file`: runs a `.kex` file's declarations into the session once it
  // checks with it.
  auto load(const std::string &filePath) -> ReplStep;
  // Evaluates one complete input; `implicitDo` when it is a `make X` whose
  // `do` the user left off, `pendingWith` a `/with` waiting for its value.
  auto evaluate(std::string source, bool implicitDo,
                const std::optional<ReplSessionWith> &pendingWith) -> void;
};

auto runWalkerRepl(bool skipPrelude) -> int {
  WalkerReplSession session(skipPrelude);
  return session.run();
}

auto WalkerReplSession::load(const std::string &filePath) -> ReplStep {
  if (!fileExists(filePath)) {
    std::cerr << "  /load: file not found: " << filePath << "\n";
    return ReplStep::Next;
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
    auto prog = std::make_unique<kex::ast::Program>(parser.parseProgram());
    throwOnParseErrors(parser);
    auto deps = resolveBeamDeps(*prog, m_walkerProjectRoots,
                                &preludeSemanticInterfaces());
    // Checked with the session before anything runs, as the definitions
    // that run (hir::checkReplInput): a file that does not check is refused,
    // rather than kept in the session source that every later input is
    // checked against, each failing with its error.
    expandReplProgram(*prog);
    {
      std::string bindingSource;
      for (const auto &[_, binding] : m_replBindings) bindingSource += binding + "\n";
      auto checked = kex::hir::checkReplInput(
          sessionProgram(bindingSource), *prog, {},
          [this](kex::ast::Program &semantic) { prepareSession(semantic); },
          &preludeSemanticInterfaces());
      if (!checked.program) {
        std::cerr << "  /load: " << filePath << " was not loaded: "
                  << checked.error.value_or("semantic analysis failed") << "\n";
        return ReplStep::Next;
      }
      keepChecked(std::move(checked));
    }
    m_replPrograms.push_back(prog.release());
    m_evaluator->execute(*m_replPrograms.back());
    for (auto &dep : deps) {
      if (dep.path &&
          std::find(m_replLoadedDepFiles.begin(), m_replLoadedDepFiles.end(),
                    *dep.path) == m_replLoadedDepFiles.end())
        m_replLoadedDepFiles.push_back(*dep.path);
      m_replLoadedDeps.push_back(std::move(dep));
    }
    m_replAccumSource += readFile(filePath) + "\n";
    m_replDb.updateFile("<repl>", m_replAccumSource);
    std::cout << "  loaded " << filePath << "\n";
  } catch (const std::exception& e) {
    std::cerr << "  /load: " << filePath << " was not loaded: " << e.what() << "\n";
  }
  return ReplStep::Next;
}

auto WalkerReplSession::evaluate(
    std::string source, bool implicitDo,
    const std::optional<ReplSessionWith> &pendingWith) -> void {
  const bool isFuncDef = classifyReplInput(source).definition;
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
    m_replPrograms.push_back(program);
    // Checked as the definitions that run, so a later call into them reads
    // the checker's decisions. Definitions were never refused here, so one
    // the check rejects still runs, on the values alone.
    expandReplProgram(*program);
    try {
      std::string bindingSource;
      for (const auto &[_, binding] : m_replBindings) bindingSource += binding + "\n";
      keepChecked(kex::hir::checkReplInput(
          sessionProgram(bindingSource), *program, {},
          [this](kex::ast::Program &semantic) { prepareSession(semantic); },
          &preludeSemanticInterfaces()));
    } catch (...) {
      // A session history that does not re-parse: run on the values.
    }
    execProgram(program, true);
    // Accumulate and re-index so all prior definitions stay
    // visible for tab completion, not just the latest one.
    m_replAccumSource += source + "\n";
    m_replDb.updateFile("<repl>", m_replAccumSource);
    const bool isImport = source.rfind("using ", 0) == 0;
    std::cout << kex::color::apply(kex::color::gray) << "=> "
              << kex::color::apply(kex::color::reset)
              << (isImport ? "using " : "defined ")
              << replDefinitionName(source) << "\n";
  } else {
    // Wrap in main for expression evaluation
    const auto withExcept =
        pendingWith ? pendingWith->capability : std::string();
    const auto withOpen = replWithOpen(m_sessionWiths, withExcept);
    const auto withClose = replWithClose(m_sessionWiths, withExcept);
    auto wrapped =
        "main do\n" + withOpen + source + "\n" + withClose + "end\n";
    kex::Lexer lexer(wrapped);
    auto tokens = lexer.tokenizeAll();
    kex::Parser parser(std::move(tokens));
    auto *program = new kex::ast::Program(parser.parseProgram());
    throwOnParseErrors(parser);
    m_replPrograms.push_back(program);

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
      const auto &lastKind = replLastStatement(**main)->kind;
      // `var x = v` binds just as much as `let` does. The runtime value
      // lives in the m_evaluator's persistent REPL env either way; what
      // this record drives is the semantic replay below, and without it
      // the NEXT line's analysis reports `x` undefined.
      if (const auto *mutableBinding =
              std::get_if<kex::ast::VarExpr>(&lastKind)) {
        if (mutableBinding->pattern) {
          std::vector<std::string> names;
          collectPatternNames(*mutableBinding->pattern, names);
          if (!names.empty()) {
            bindingName = names.front();
            boundNames = std::move(names);
          }
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
    // Before the try: its catch-all swallows exceptions so a stale
    // semantic history cannot break evaluation — this error must
    // surface instead.
    throwIfPartialPatternRebind(boundNames, m_replBindings);
    // The input is checked as the very statements that run: they are lent
    // to the checked program (the session's definitions and bindings) and
    // returned, so the checked program the evaluator reads (`hir`) answers
    // for the nodes it evaluates, as for a file.
    expandReplProgram(*program);
    kex::ast::MainBlock *inputMain = nullptr;
    for (auto &item : program->items)
      if (auto *main = std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&item);
          main && *main)
        inputMain = main->get();
    // Its last statement (inside any `with` wrappers), found before the
    // statements are lent: the node, not its slot, is what is typed.
    const kex::ast::Expr *inputLast =
        inputMain && !inputMain->body.empty() ? replLastStatement(*inputMain)
                                              : nullptr;
    // The input checked as the statements that run (hir::checkReplInput).
    std::optional<kex::hir::CheckedInput> checked;
    try {
      std::string bindingSource;
      for (const auto &[name, binding] : m_replBindings)
        if (!bindingName ||
            std::find(boundNames.begin(), boundNames.end(), name) ==
                boundNames.end())
          bindingSource += binding + "\n";
      auto session = sessionProgram(bindingSource);
      // A `/with` value is checked against its capability after the input;
      // its type is not shown.
      std::vector<kex::ast::ExprPtr> after;
      if (pendingWith) {
        kex::Lexer withLexer("main do\nwith " + pendingWith->capability +
                                 " = " + pendingWith->binding +
                                 " do\n0\nend\nend\n",
                             "<repl>");
        kex::Parser withParser(withLexer.tokenizeAll(), "<repl>");
        auto withProgram = withParser.parseProgram();
        for (auto &item : withProgram.items)
          if (auto *main = std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&item);
              main && *main)
            for (auto &statement : (*main)->body)
              after.push_back(std::move(statement));
      }
      checked = kex::hir::checkReplInput(
          std::move(session), *program, std::move(after),
          [this](kex::ast::Program &semantic) { prepareSession(semantic); },
          &preludeSemanticInterfaces());
      semanticError = checked->error;
      // The input's last statement, lent with the rest: the same node.
      if (const auto *last = inputLast; last && checked->program) {
        const kex::ast::Expr *typedExpr = last;
        if (const auto *binding = std::get_if<kex::ast::LetExpr>(&last->kind);
            binding && binding->value)
          typedExpr = binding->value.get();
        else if (const auto *mutableBinding =
                     std::get_if<kex::ast::VarExpr>(&last->kind);
                 mutableBinding && mutableBinding->value)
          typedExpr = mutableBinding->value.get();
        else if (const auto *assigned =
                     std::get_if<kex::ast::AssignExpr>(&last->kind);
                 assigned && assigned->value)
          typedExpr = assigned->value.get();
        semanticType = displayTypeOf(*checked->analysis, typedExpr);
      }
    } catch (...) {
      // Evaluation still works if an incomplete semantic history (for
      // example a loaded file with a main block) cannot be re-parsed.
    }

    if (semanticError)
      throw std::runtime_error(*semanticError);

    if (checked) keepChecked(std::move(*checked));
    auto result = execProgram(program, true);
    if (pendingWith) {
      std::erase_if(m_sessionWiths, [&](const auto &with) {
        return with.capability == pendingWith->capability;
      });
      m_sessionWiths.push_back(*pendingWith);
      std::cout << "  (with " << pendingWith->capability << ")\n";
    } else {
      showResult(result, semanticType);
    }
    if (bindingName) {
      m_replBindings.erase(
          std::remove_if(
              m_replBindings.begin(), m_replBindings.end(),
              [&](const auto &binding) {
                return std::find(boundNames.begin(), boundNames.end(),
                                 binding.first) != boundNames.end();
              }),
          m_replBindings.end());
      m_replBindings.emplace_back(*bindingName, source);
    }
  }
}

auto WalkerReplSession::run() -> int {
  kex::printReplBanner(std::cout, "");

#ifdef HAS_READLINE
  const std::string historyFile = loadReplHistory();
#endif

  // SemanticDB for REPL: prelude loaded once, updated on each input.
  if (!m_skipPrelude) {
    m_replDb.setImportedInterfaces(&preludeSemanticInterfaces());
    loadPrelude(m_replDb);
  }
  m_analysisDb.setImportedInterfaces(&preludeSemanticInterfaces());
  m_analysisDb.setModuleRoots(
      moduleRootsFor((std::filesystem::current_path() / "<repl>").string()));
  g_replCompletionDb = &m_replDb;
  g_replAnalysisDb = &m_analysisDb;
#ifdef HAS_READLINE
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

  m_evaluator.emplace();
  m_evaluator->setReplMode(true);
  // A REPL session is the user experimenting with their own machine, and
  // there is no other code present for a mock to deceive — the reason
  // Mock.* is gated elsewhere (issue #144) does not exist here.
  m_evaluator->setMocksAllowed(true);
  // Without this the m_evaluator keeps its default relative {"lib", "src"}
  // roots, so `using Regex` (or any opt-in stdlib module) failed with
  // "Unknown module" in the interpreter REPL while working both in scripts
  // and in the BEAM REPL. There is no file being run, so resolution is
  // anchored at the working directory plus the standard library roots.
  m_evaluator->setModuleRoots(
      moduleRootsFor((std::filesystem::current_path() / "<repl>").string()));
  std::string line;
  g_replCompletionContext = [&]() {
    ReplCompletionContext context{m_replAccumSource, {}};
    for (const auto &[name, binding] : m_replBindings)
      context.bindings += binding + "\n";
    return context;
  };

  // The project modules a `/load`-ed file uses, found as a build of a file
  // in the working directory would find them, and kept alive for the same
  // reason. Without them `Store`'s `using Wire` left `Statement` unknown.
  g_dependencyFailureThrows = true; // as in the BEAM REPL
  m_walkerProjectRoots = kex::withoutStandardLibraryRoots(
      moduleRootsFor((std::filesystem::current_path() / "repl.kex").string()));


  static const std::string setOptionsHelp =
      "  Options for /set / /unset:\n"
      "    types         Show type of each result\n"
      "    ast           Show AST for each input\n"
      "    tokens        Show token stream for each input\n";
  while (true) {
    auto [read, ok] = readLine("kex> ");
    if (!ok)
      break;
    std::string input = replTrimLeadingIndent(std::move(read));
    line = input;
    if (kex::isReplExit(line))
      break;
    const auto command = kex::parseReplCommand(line);
    using Command = kex::ReplCommandKind;
    if (line.empty())
      continue;

    // REPL commands
    if (command.kind == Command::Help) {
      kex::printReplHelp(std::cout, setOptionsHelp);
      continue;
    }
    if (command.kind == Command::Clear) {
      std::cout << kex::replClearScreenSequence() << std::flush;
      continue;
    }
    if (command.kind == Command::Set && command.argument.empty()) {
      std::cout << "  types:  " << (m_showTypes ? "on" : "off") << "\n"
                << "  ast:    " << (m_showAst ? "on" : "off") << "\n"
                << "  tokens: " << (m_showTokens ? "on" : "off") << "\n";
      continue;
    }
    if (command.kind == Command::Set) {
      handleSet(command.argument, true);
      continue;
    }
    if (command.kind == Command::Unset) {
      handleSet(command.argument, false);
      continue;
    }
    if (command.kind == Command::Complete) {
      printReplCompletions(command.argument);
      continue;
    }
    std::optional<ReplSessionWith> pendingWith;
    if (command.kind == Command::With || command.kind == Command::Without) {
      try {
        line = handleReplWithCommand(line, m_sessionWiths, pendingWith);
      } catch (const std::exception &e) {
        std::cerr << "  " << kex::color::apply(kex::color::red)
                  << "error:" << kex::color::apply(kex::color::reset) << " "
                  << e.what() << "\n";
        if (g_replLineFromScript) {
          g_replScriptFailed = true;
          break;
        }
        continue;
      }
      if (line.empty()) continue;
    }
    if (command.kind == Command::Load) {
      if (load(command.argument) == ReplStep::Stop) break;
      continue;
    }
    if (command.kind == Command::Reload) {
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


    // `make TypeName` (no explicit `do`) implicitly opens a block.
    bool implicitDo = isMakeWithoutDo(source);
    int doCount = implicitDo ? 1 : replBlockDepth(source);

    bool cancelled = false;
    while (doCount > 0 || replHasOpenDelimiter(source)) {
      g_replPendingInput = source;
      auto [contLine, contOk] = readLine("...> ");
      if (takeReplInputCancelled()) {
        cancelled = true;
        break;
      }
      if (!contOk)
        break;
      line = contLine;
      source += "\n" + line;
      doCount = implicitDo ? 1 + replBlockDepth(source) : replBlockDepth(source);
    }
    g_replPendingInput.clear();
    if (cancelled) {
      std::cout << kex::color::apply(kex::color::gray) << "   (cancelled)"
                << kex::color::apply(kex::color::reset) << "\n";
      continue;
    }
    // Clauses are not gathered by reading ahead — see the BEAM REPL above.
    // A complete line is accepted as soon as it is entered, and clauses of
    // one function accumulate where the definition is stored.

    // Show tokens if enabled
    if (m_showTokens) {
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
    try {
      evaluate(std::move(source), implicitDo, pendingWith);
    } catch (const std::exception &e) {
      std::cerr << "  " << kex::color::apply(kex::color::red)
                << "error:" << kex::color::apply(kex::color::reset) << " "
                << e.what() << "\n";
      if (g_replLineFromScript) {
        g_replScriptFailed = true;
        break;
      }
    }
  }
#ifdef HAS_READLINE
  if (!historyFile.empty())
    write_history(historyFile.c_str());
#endif
  return g_replScriptFailed ? 1 : 0;
}

// `--build-prelude DIR`: compile the standard library into DIR, attach each
// module's KexI interface, and drop artifacts of modules that no longer exist.
auto buildPrelude(const std::string &dir) -> int {
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

// `--complete PREFIX [FILE]`: print the completions for PREFIX, one per line,
// against the prelude and (when given) FILE.
auto printCompletions(const std::string &prefix, const char *file) -> int {
  kex::semantic::SemanticDB db;
  db.setImportedInterfaces(&preludeSemanticInterfaces());
  if (file)
    db.setModuleRoots(moduleRootsFor(file));
  loadPrelude(db);
  if (file)
    db.updateFile(file, readFile(file));
  // Simulate readline not splitting: start=0, text=prefix
  auto cq = kex::resolveCompletionQuery(prefix.c_str(), 0, prefix.c_str());
  auto raw = db.completionsFor(cq.dbQuery);
  auto completions =
      kex::rewriteCompletions(std::move(raw), cq.rewriteFrom, cq.rewriteTo);
  for (const auto &c : completions)
    std::cout << c << "\n";
  return 0;
}

// The command line, read once by parseCommandLine and handed to the mode
// that runs. `exitNow` is set when an option has already finished the run
// (`--help`, `--version`, `--build-prelude`, a bad flag) with that status.
struct Options {
  // Unset until a flag picks a mode; whatever is still unset after option
  // parsing gets the default, which is BEAM (Compile + compileRun). The
  // tree-walk interpreter is opt-in: -R for a script, -i for a REPL.
  Mode mode = Mode::Unset;
  bool skipCheck = false;
  bool dumpTypes = false;
  bool jsonOutput = false;
  bool summaryMode = false;
  // `--dump-hir` / `--verify-hir`: list every call site with what the
  // checker decided about it / fail when any is undecided (src/hir/).
  bool dumpHir = false;
  bool verifyHir = false;
  // `--print-passes`: list the compile pipeline and the IR passes, then exit.
  bool printPasses = false;
  std::string completePrefix;
  std::string outputDir = ".";
  std::string astFilename;
  bool outputDirExplicit = false;
  // `--eval` was given: its lines are queued in g_replScript.
  bool replEval = false;
  bool compileRun = false;
  bool skipPrelude = false;
  bool collapseReport = false;
  bool allowMocks = false;
  kex::interpreter::Evaluator::TestReportMode testReportMode =
      kex::interpreter::Evaluator::TestReportMode::Pretty;
  std::vector<std::string> testFilters;
  std::optional<int> exitNow;
};

auto parseCommandLine(int argc, char *argv[]) -> Options {
  Options options;
  static struct option longOptions[] = {
      {"run", no_argument, nullptr, 'r'},
      {"no-check", no_argument, nullptr, 'n'},
      {"lex", no_argument, nullptr, 'l'},
      {"parse", no_argument, nullptr, 'p'},
      {"check", no_argument, nullptr, 'C'},
      {"compile", no_argument, nullptr, 'c'},
      {"run-walker", no_argument, nullptr, 'R'},
      // A second name for the same thing. `--run-beam` is deliberately NOT
      // accepted any more: it used to mean this flag's opposite, and a name
      // that silently reverses is worse than one that errors.
      {"run-interpreter", no_argument, nullptr, 'R'},
      {"interactive", no_argument, nullptr, 'i'},
      {"json", no_argument, nullptr, 'j'},
      {"summary", no_argument, nullptr, 's'},
      {"types", no_argument, nullptr, 't'},
      {"dump-hir", no_argument, nullptr, Option::DumpHir},
      {"verify-hir", no_argument, nullptr, Option::VerifyHir},
      {"print-passes", no_argument, nullptr, Option::PrintPasses},
      {"emit-core", no_argument, nullptr, 'E'},
      {"eval", required_argument, nullptr, 'e'},
      {"emit-ast", no_argument, nullptr, Option::EmitAst},
      {"ast-filename", required_argument, nullptr, Option::AstFilename},
      {"complete", required_argument, nullptr, 'K'},
      {"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, 'v'},
      // Machine-readable counterpart to --version. Long-only: it is for
      // tools, and a short flag would invite it into shell prompts.
      {"info", no_argument, nullptr, Option::Info},
      {"no-colors", no_argument, nullptr, 'N'},
      {"colors", no_argument, nullptr, Option::Colors},
      {"no-prelude", no_argument, nullptr, Option::NoPrelude},
      {"source-root", required_argument, nullptr, Option::SourceRoot},
      {"quiet", no_argument, nullptr, 'q'},
      {"progress", no_argument, nullptr, Option::Progress},
      {"sname", required_argument, nullptr, Option::Sname},
      {"name", required_argument, nullptr, Option::Name},
      {"cookie", required_argument, nullptr, Option::Cookie},
      {"credits", no_argument, nullptr, Option::Credits},
      // Mock.* is test-only (issue #144): a mock lets one part of a program
      // lie to another about the filesystem, environment, platform, network
      // or console, so the runtime denies the mock intrinsics unless the
      // entry file is a `*.spec.kex`, this is a REPL, or the user says so
      // here. Long-only and deliberately verbose — nobody should reach for
      // it by habit.
      {"allow-mocks", no_argument, nullptr, Option::AllowMocks},
      // The testing DSL, said in JSON rather than in prose (kexhq/kex#199).
      // `--test-json` replaces the ✓/✗ lines with one record per case —
      // including the FILE and LINE of the `it` and of the failure, which is
      // the part only the runner can know and the part an editor's inline
      // decoration needs. `--test-list` discovers the cases without running
      // any of them, and `--test-only` runs one (repeatable).
      {"test-json", no_argument, nullptr, Option::TestJson},
      {"test-list", no_argument, nullptr, Option::TestList},
      {"test-only", required_argument, nullptr, Option::TestOnly},
      {"emit-syntax", no_argument, nullptr, Option::EmitSyntax},
      {"lsp", no_argument, nullptr, Option::Lsp},
      // Print the AST AFTER compile-time expansion of `compiled do` blocks —
      // i.e. what the type checker and both backends actually see. `--parse`
      // shows the AST before it.
      {"expand", no_argument, nullptr, Option::Expand},
      // Report every chain-collapse attempt and its outcome. Collapse changes
      // only the EMITTED code, so without this the only way to tell whether it
      // happened is to decode the Core Erlang by hand.
      {"collapse-report", no_argument, nullptr, Option::CollapseReport},
      // Compile the sources selected by src/stdlib/prelude.kex into kex_prelude.core +
      // kex_prelude.beam in the given dir. Used by the build to prebuild the
      // shared stdlib module alongside the runtime beams.
      {"build-prelude", required_argument, nullptr, Option::BuildPrelude},
      {nullptr, 0, nullptr, 0}};

  int opt;
#ifndef __EMSCRIPTEN__
  // Styling is for a terminal: escape codes in a pipe, a log or a CI capture
  // are noise, and NO_COLOR (https://no-color.org) asks for none at all.
  // `--colors` / `--no-colors` below override it either way. (The wasm
  // build's output is the web REPL's own terminal, so it keeps colors.)
  // FORCE_COLOR is the same convention's way back on, for output that is
  // piped but still ends up on a screen — a parent that relays it (Tey runs
  // `kex` behind a pipe), or a CI log viewer.
  if (!isatty(STDOUT_FILENO))
    kex::color::enabled = false;
  if (const char *force = std::getenv("FORCE_COLOR");
      force && *force && std::string(force) != "0")
    kex::color::enabled = true;
  if (const char *noColor = std::getenv("NO_COLOR"); noColor && *noColor)
    kex::color::enabled = false;
#endif
  while ((opt = getopt_long(argc, argv, "rnlcCiRjspqEe:thvK:o:", longOptions,
                            nullptr)) != -1) {
    switch (opt) {
    case Option::NoPrelude:
      options.skipPrelude = true;
      break;
    case Option::Expand:
      options.mode = Mode::Expand;
      break;
    case Option::CollapseReport:
      options.collapseReport = true;
      break;
    case Option::AllowMocks:
      options.allowMocks = true;
      break;
    case Option::TestJson:
      options.testReportMode = kex::interpreter::Evaluator::TestReportMode::Json;
      break;
    case Option::TestList:
      options.testReportMode = kex::interpreter::Evaluator::TestReportMode::List;
      break;
    case Option::TestOnly:
      options.testFilters.emplace_back(optarg);
      break;
    case Option::Sname:
    case Option::Name: {
      const bool alreadyNamed =
          std::find_if(cliDistributionArgs.begin(), cliDistributionArgs.end(),
                       [](const std::string &a) {
                         return a == "-sname" || a == "-name";
                       }) != cliDistributionArgs.end();
      if (alreadyNamed) {
        std::cerr << "error: give a node one name: --sname or --name, once\n";
        { options.exitNow = 1; return options; }
      }
      if (const auto problem = nodeNameProblem(optarg, opt == Option::Sname);
          !problem.empty()) {
        std::cerr << "error: invalid node name '" << optarg << "': " << problem
                  << "\n";
        { options.exitNow = 1; return options; }
      }
      cliDistributionArgs.push_back(opt == Option::Sname ? "-sname" : "-name");
      cliDistributionArgs.emplace_back(optarg);
      break;
    }
    case Option::Cookie:
      cliDistributionArgs.push_back("-setcookie");
      cliDistributionArgs.emplace_back(optarg);
      break;
    case Option::Credits: // --credits
      std::cout << "Copyright (c) 2026 Akos Kovacs." << std::endl;
      { options.exitNow = 0; return options; }
    case Option::EmitAst:
      options.mode = Mode::EmitAst;
      break;
    case Option::DumpHir:
      options.dumpHir = true;
      options.mode = Mode::Check;
      break;
    case Option::VerifyHir:
      options.verifyHir = true;
      options.mode = Mode::Check;
      break;
    case Option::PrintPasses:
      options.printPasses = true;
      break;
    case Option::EmitSyntax:
      options.mode = Mode::EmitSyntax;
      break;
    case Option::AstFilename:
      options.astFilename = optarg;
      break;
    case Option::Lsp:
#ifdef KEX_HAS_LSP
      options.mode = Mode::Lsp;
#else
      std::cerr << "error: LSP support is unavailable in this build\n";
      { options.exitNow = 1; return options; }
#endif
      break;
    case Option::SourceRoot:
      cliSourceRoots.emplace_back(optarg);
      break;
    case 'q':
      quietBuild = true;
      break;
    case Option::Progress:
      progressRequested = true;
      break;
    case Option::BuildPrelude:
      options.exitNow = buildPrelude(optarg);
      return options;
    case 'r':
      options.mode = Mode::Compile;
      options.compileRun = true;
      break;
    case 'n':
      options.skipCheck = true;
      break;
    case 'l':
      options.mode = Mode::Lex;
      break;
    case 'p':
      options.mode = Mode::Parse;
      break;
    case 'C':
      options.mode = Mode::Check;
      break;
    case 'c':
      options.mode = Mode::Compile;
      break;
    case 'R':
      options.mode = Mode::Run;
      options.compileRun = false;
      break;
    case 'i':
      options.mode = Mode::Repl;
      break;
    case 'j':
      options.jsonOutput = true;
      options.mode = Mode::Check;
      break;
    case 's':
      options.summaryMode = true;
      options.mode = Mode::Check;
      break;
    case 't':
      options.dumpTypes = true;
      break;
    case 'E':
      options.mode = Mode::EmitCore;
      break;
    case 'e': {
      // Repeatable; each one is entered as its own lines, in order.
      std::istringstream source(optarg);
      for (std::string line; std::getline(source, line);)
        g_replScript.push_back(line);
      options.replEval = true;
      break;
    }
    case 'o':
      options.outputDir = optarg;
      options.outputDirExplicit = true;
      break;
    case 'K':
      options.completePrefix = optarg;
      options.mode = Mode::Complete;
      break;
    case 'h':
      printUsage(argv[0]);
      { options.exitNow = 0; return options; }
    case 'v':
      printVersion();
      { options.exitNow = 0; return options; }
    case Option::Info:
      printInfo();
      { options.exitNow = 0; return options; }
    case 'N':
      kex::color::enabled = false;
      break;
    case Option::Colors:
      kex::color::enabled = true;
      break;
    default:
      printUsage(argv[0]);
      { options.exitNow = 1; return options; }
    }
  }

  // No mode flag: BEAM. A file runs on the BEAM, and no file opens the BEAM
  // REPL (the `mode == Mode::Compile && compileRun` branch below). A wasm build
  // has no BEAM to run on — no erl, no runtime beams — so there the default
  // stays the tree-walk interpreter.
  if (options.mode == Mode::Unset) {
#ifdef __EMSCRIPTEN__
    options.mode = Mode::Run;
#else
    options.mode = Mode::Compile;
    options.compileRun = true;
#endif
  }
  return options;
}

// Attaches each emitted module's KexI chunk to its `.beam` in `outputDir`.
// Companions go first, so the entry's chunk is written once, already naming
// every companion's interface hash — the entry used to be written, then read
// back with every companion and written again to fill those hashes in.
auto attachInterfaces(const kex::ast::Program &program,
                      const std::vector<kex::ir::EmitResult> &modules,
                      const std::string &outputDir, const std::string &stem,
                      bool noCheck, const kex::semantic::Analyzer *analysis)
    -> void {
  std::map<std::string, kex::beam::Hash128> companionHashes;
  auto attach = [&](size_t index) {
    const auto &module = modules[index];
    const std::string beamPath = outputDir + "/" + module.moduleName + ".beam";
    try {
      kex::beam::CollectOptions options;
      options.unitId = modules[0].moduleName;
      options.moduleAtom = module.moduleName;
      options.fileStem = stem;
      options.noCheck = noCheck;
      options.analysis = analysis;
      options.role = index == 0 ? kex::beam::KexiModuleRole::Entry
                                : kex::beam::KexiModuleRole::Companion;
      if (index != 0) {
        options.entryBackPointer = modules[0].moduleName;
        if (module.moduleName.rfind("Kex.", 0) == 0)
          options.moduleName = module.moduleName.substr(4);
      }
      auto chunk = kex::beam::collectMetadata(program, options);
      kex::beam::recordImplementations(chunk,
                                       routedIn(module.routedImplementations));
      chunk.intrinsicAbiVersion = kex::kIntrinsicAbiVersion;
      chunk.backendRepresentationVersion = kex::kBeamRepresentationVersion;
      if (index == 0)
        for (size_t c = 1; c < modules.size(); c++) {
          kex::beam::KexiCompanion companion;
          companion.beamAtom = modules[c].moduleName;
          companion.relativePath = modules[c].moduleName + ".beam";
          if (const auto hash = companionHashes.find(companion.beamAtom);
              hash != companionHashes.end())
            companion.expectedHash = hash->second;
          chunk.metadata.companions.push_back(std::move(companion));
        }
      chunk.interfaceHash = kex::beam::computeInterfaceHash(chunk);
      if (index != 0) companionHashes[module.moduleName] = chunk.interfaceHash;
      auto beam = kex::beam::readBeamFile(beamPath);
      chunk.artifactHash = kex::beam::computeArtifactHash(beam);
      beam.setChunk(kex::beam::KEXI_CHUNK_ID, kex::beam::serializeKexi(chunk));
      kex::beam::writeBeamFile(beam, beamPath);
    } catch (const std::exception &e) {
      std::cerr << "warning: could not attach KexI chunk to " << beamPath
                << ": " << e.what() << "\n";
    }
  };
  for (size_t index = 1; index < modules.size(); index++) attach(index);
  if (!modules.empty()) attach(0);
}

// Runs an already compiled `.beam` entry module with the bundled runtime;
// `beamArgs` are the program's own arguments.
auto runCompiledBeam(const std::string &filepath, const Options &options,
                     const std::vector<std::string> &beamArgs) -> int {

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
      (options.allowMocks ? "kex_test:allow_mocks(), " : "") +
      beamTestGate(options.testReportMode, options.testFilters) +
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
  std::string runCmd =
      erlExecutable() + " +Bi -noshell" + distributionArgsForShell();
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

// Compiles each emitted module's Core Erlang (`corePaths`, in
// `moduleResults` order) to a `.beam` in `outputDir`. A `quiet` build — a
// run from a temporary directory — keeps erlc's output out of the program's
// own. False once erlc has failed and said why.
//
// ONE erlc for every module, not one per module. Each invocation
// starts a whole BEAM just to compile, ~90 ms before it reads a byte,
// so a spec file's three modules paid that three times over
// (kexhq/kex#318). erlc takes any number of files in a single run.
//
// -W0: these warnings analyze the generated Core Erlang — locationless
// (`no_file:`) compiler-analysis noise the emitter's dead statement-
// value lets make inevitable ("a term is constructed, but never used",
// "this clause cannot match"). They cannot point back at Kex source,
// and the interpreter path below has always silenced them for exactly
// that reason. Real erlc failures still fail the exit-code check and
// are re-run loudly below.
//
// Modules whose Core Erlang an earlier build already compiled with this
// erlc come straight from the cache (kexhq/kex#323); only the rest go
// to erlc, and those are stored for next time once it succeeds.
auto compileCoreModules(const std::vector<kex::ir::EmitResult> &moduleResults,
                        const std::vector<std::string> &corePaths,
                        const std::string &outputDir, bool quiet) -> bool {
  const auto cacheRoot = beamCacheRoot();
  const std::string erlcFlags = "+from_core -W0";
  const std::string erlc = cacheRoot ? erlcIdentity() : std::string{};
  std::vector<size_t> toCompile;
  std::vector<std::filesystem::path> cacheEntries(moduleResults.size());
  for (size_t moduleIndex = 0; moduleIndex < moduleResults.size(); ++moduleIndex) {
    if (cacheRoot) {
      const auto key =
          beamCacheKey(moduleResults[moduleIndex].source, erlc, erlcFlags);
      cacheEntries[moduleIndex] = *cacheRoot / key.substr(0, 2) / (key + ".beam");
      std::error_code ec;
      const auto target = std::filesystem::path{outputDir} /
                          (moduleResults[moduleIndex].moduleName + ".beam");
      if (std::filesystem::is_regular_file(cacheEntries[moduleIndex], ec) &&
          std::filesystem::copy_file(cacheEntries[moduleIndex], target,
                                     std::filesystem::copy_options::overwrite_existing,
                                     ec) &&
          !ec)
        continue;
    }
    toCompile.push_back(moduleIndex);
  }

  // erlc is one BEAM per run, and one run took every module in turn: Tey's
  // 82 modules spent 45s in a single process on a machine with cores to
  // spare. They go out in batches, one erlc each, side by side — at most
  // one per core (KEX_ERLC_JOBS to say otherwise), and at least four
  // modules apiece, since every run first starts a VM. Batches are
  // balanced by the size of their Core Erlang, the best guess at how long
  // each module takes. A batch reports when it ENDS, so what the build
  // shows is how far the compiling has actually got.
  std::size_t jobs = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
  if (const char *configured = std::getenv("KEX_ERLC_JOBS"))
    if (const auto requested = std::atoi(configured); requested > 0)
      jobs = static_cast<std::size_t>(requested);
  jobs = std::min(jobs, std::max<std::size_t>(1, toCompile.size() / 4));
  std::vector<std::vector<std::size_t>> batches(jobs);
  {
    auto bySize = toCompile;
    std::sort(bySize.begin(), bySize.end(), [&](std::size_t a, std::size_t b) {
      return moduleResults[a].source.size() > moduleResults[b].source.size();
    });
    std::vector<std::size_t> load(jobs, 0);
    for (const auto moduleIndex : bySize) {
      const auto lightest = static_cast<std::size_t>(
          std::min_element(load.begin(), load.end()) - load.begin());
      batches[lightest].push_back(moduleIndex);
      load[lightest] += moduleResults[moduleIndex].source.size();
    }
  }
  batches.erase(std::remove_if(batches.begin(), batches.end(),
                               [](const auto &batch) { return batch.empty(); }),
                batches.end());
  const auto erlcCommand = [&](const std::vector<std::size_t> &batch,
                               bool loud) {
    std::string command = erlcExecutable() + " " +
                          (loud ? std::string("+from_core") : erlcFlags) +
                          " -pa " + outputDir + " -o " + outputDir;
    for (const auto moduleIndex : batch) command += " " + corePaths[moduleIndex];
    // Suppress erlc noise in temp-dir (interpreter/-R) mode. erlc prints
    // its own warnings (e.g. "this clause cannot match because a
    // previous clause always matches" — harmless analysis of the
    // generated code) to its OWN STDOUT, not stderr, so both streams are
    // silenced to keep them out of the running program's own output.
    // Failures are still caught by the exit status, and re-run loudly.
    if (!loud && quiet) command += " > /dev/null 2>&1";
    return command;
  };
  std::vector<std::string> commands;
  for (const auto &batch : batches)
    commands.push_back(erlcCommand(batch, false));
  if (!quiet && buildProgress.enabled) {
    if (const auto cached = moduleResults.size() - toCompile.size(); cached > 0)
      buildProgress.step("Cached", kex::color::gray,
                         moduleCount(cached, "unchanged "));
    if (!toCompile.empty())
      buildProgress.step(
          "Compiling", kex::color::green,
          moduleCount(toCompile.size()) +
              (commands.size() > 1
                   ? std::string(kex::color::apply(kex::color::gray)) +
                         " in " + std::to_string(commands.size()) +
                         " erlc runs" + kex::color::apply(kex::color::reset)
                   : std::string{}));
  } else if (!quiet && !quietBuild) {
    for (const auto moduleIndex : toCompile)
      std::cerr << "  Compile: " << moduleResults[moduleIndex].moduleName << "\n";
  }
  std::vector<std::size_t> failed;
  std::size_t compiled = 0;
  runShellCommandsTogether(commands, [&](std::size_t index, int status) {
    const auto &batch = batches[index];
    if (status != 0) {
      failed.push_back(index);
      return;
    }
    if (cacheRoot)
      for (const auto moduleIndex : batch)
        storeCachedBeam(cacheEntries[moduleIndex],
                        std::filesystem::path{outputDir} /
                            (moduleResults[moduleIndex].moduleName + ".beam"));
    compiled += batch.size();
    if (!quiet && commands.size() > 1)
      buildProgress.step("Compiled", kex::color::green,
                         std::to_string(compiled) + "/" +
                             moduleCount(toCompile.size()));
  });
  if (!failed.empty()) {
    // The quiet mode above swallows erlc's own diagnostic — which is the
    // only thing that says WHY. Re-run the batches that failed loudly so
    // the message reaches the terminal (and CI) instead of a bare "erlc
    // failed", which is all a failing spec could report. erlc names the
    // file it choked on.
    if (quiet)
      for (const auto index : failed)
        runShellCommand(erlcCommand(batches[index], true));
    std::cerr << "error: erlc failed\n";
    return false;
  }
  return true;
}

// Runs the program `-c --run` just built into `outputDir`: loads every
// emitted module explicitly, then calls the entry's (the first module's)
// `main`, handing it `scriptArgs` when it takes them. Removes `tempDir`, the
// build's temporary output, when there is one. Returns the exit status.
auto runBuiltProgram(const Options &options, const std::string &filepath,
                     const std::vector<kex::ir::EmitResult> &moduleResults,
                     const std::string &tempDir,
                     const std::vector<std::string> &scriptArgs) -> int {
  const auto &outputDir = options.outputDir;
  const auto &result = moduleResults.front();
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
      ((options.allowMocks || isSpecEntry(filepath)) ? "kex_test:allow_mocks(), "
                                             : "") +
      beamTestGate(options.testReportMode, options.testFilters);
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
      erlExecutable() + " +Bi -noshell" + distributionArgsForShell() +
      " -pa " + outputDir + " -eval " +
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

// What the gating pre-check found: whether the entry, checked together with
// the project modules it uses, failed, and the Analyzer's diagnostics.
struct PreCheck {
  bool failed = false;
  std::vector<kex::semantic::Diagnostic> diagnostics;
  int typeErrors = 0;
};

// Checks `program` the way the project's other modules will see it: with its
// spec base and every PROJECT module it names merged in. `program` gets its
// own items back afterwards, unchanged; only the verdict is kept.
auto preCheckWithDependencies(kex::ast::Program &program,
                              const std::string &filepath) -> PreCheck {
  PreCheck result;
  kex::ast::Program checkProgram;
  checkProgram.items = std::move(program.items);
  const size_t originalCount = checkProgram.items.size();

  mergeSpecBase(checkProgram, filepath);

  resolveBeamDeps(checkProgram,
                  kex::withoutStandardLibraryRoots(moduleRootsFor(filepath)),
                  &preludeSemanticInterfaces(),
                  sourceModulesNamedBy(checkProgram));

  // Every item merged in above was PREPENDED (resolveBeamDeps's own
  // convention), so the entry's own (already-expanded) items are
  // still exactly the last `originalCount` of them — restorable by
  // index regardless of how many dependency files contributed.
  const size_t prependedCount = checkProgram.items.size() - originalCount;

  // `Analyzer::analyze` directly, not `runSemanticCheck`: that helper
  // also drives a `SemanticDB` pass for undefined-identifier
  // diagnostics, which re-reads AND RE-PROCESSES `filepath` from disk
  // (`SemanticDB::updateFile`) — including, for a file whose entry
  // calls `Kex.embed`/`Template.text`, running the same expensive
  // sandboxed scan a THIRD time (once in the shared expansion pass
  // above, once in the real pipeline's own gating check below, and
  // this would be a third — kexhq/kex#379 measured that scan at
  // multiple seconds for an ordinary template). This pre-check's own
  // job is the #378 overload-resolution class of error, which is the
  // Analyzer's, not SemanticDB's; undefined identifiers are still
  // caught by the real, unchanged gating check further down.
  kex::semantic::Analyzer preCheckAnalyzer(&preludeSemanticInterfaces());
  result.failed = !preCheckAnalyzer.analyze(checkProgram);
  result.diagnostics = preCheckAnalyzer.diagnostics();
  for (const auto &diag : result.diagnostics)
    if (diag.level == kex::semantic::Diagnostic::Level::Error)
      result.typeErrors++;

  // Hand the entry's own items back to `program` either way: this
  // pre-check's own verdict is consulted, deferred, further down —
  // the real pipeline continues exactly as if it had never run.
  program.items.clear();
  program.items.reserve(originalCount);
  for (size_t i = prependedCount; i < prependedCount + originalCount; i++)
    program.items.push_back(std::move(checkProgram.items[i]));
  return result;
}

// Copies the toolchain's prebuilt runtime `.beam` files into `outputDir`,
// where every compiled program loads them from. False, having said why, when
// they are missing — or the prelude is, and `needsPrelude`.
auto copyRuntimeBeams(const std::string &outputDir, bool needsPrelude)
    -> bool {
  namespace fs = std::filesystem;
  const std::string prebuilt = prebuiltRuntimeBeamDir();
  if (prebuilt.empty()) {
    std::cerr << "error: prebuilt runtime artifacts are missing; "
                 "rebuild or reinstall the Kex toolchain\n";
    return false;
  }
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(prebuilt))
    if (entry.path().extension() == ".beam")
      fs::copy_file(entry.path(), fs::path{outputDir} / entry.path().filename(),
                    fs::copy_options::overwrite_existing, ec);
  if (needsPrelude &&
      !fs::exists(fs::path{outputDir} / (kex::names::kPreludeModule + ".beam"))) {
    std::cerr << "error: prebuilt standard library is missing; "
                 "rebuild or reinstall the Kex toolchain\n";
    return false;
  }
  return true;
}

// How `main` hands its entry-file expansion step to the compile pipeline:
// expand `compiled` blocks in a program, optionally printing the collapse
// report; false once expansion has failed and said why.
using ExpandProgram = std::function<bool(kex::ast::Program &, bool)>;

// `-c` / `--emit-core` for one entry file, already parsed and expanded:
// merge the spec base and dependencies, check, lower, emit Core Erlang,
// compile it to `.beam` with the KexI chunks attached, and — for `-c --run`
// — run the result. Returns the process exit status.
// One `kex -c` / `--emit-core` build of an entry file: what its passes
// (compilePipeline) read and leave for the next one.
struct CompileUnit {
  Options &options;
  const std::string &filepath;
  kex::ast::Program &program;
  const std::vector<std::string> &scriptArgs;
  const std::string &entryRawSourceForCache;
  const std::string &buildFlags;
  const ExpandProgram &expandParsedProgram;

  // A `-c --run` whose inputs a recorded build already covers: everything
  // from `expand` to `store-caches` is skipped and its output reused.
  std::optional<std::string> manifestKey;
  bool manifestHit = false;
  PreCheck preCheck;
  // The temporary output directory of a run without `-o`, removed after.
  std::string tempDir;
  std::string stem;
  // The `<name>.spec.kex` base merged in, for the check and the run cache.
  std::string beamSpecBaseFile;
  std::string beamSpecBaseRawSource;
  std::vector<LoadedDep> beamDeps;
  std::optional<std::string> runCacheKeyValue;
  bool haveCachedResult = false;
  std::unique_ptr<kex::semantic::Analyzer> compileAnalysis;
  std::vector<kex::ir::EmitResult> moduleResults;
  std::vector<std::string> corePaths;

  auto mode() const -> Mode { return options.mode; }
  auto compileRun() const -> bool { return options.compileRun; }
  // Ends a failed build, taking its temporary output directory with it.
  auto abandon() const -> int {
    if (!tempDir.empty()) std::filesystem::remove_all(tempDir);
    return 1;
  }
  // The passes a cached build does not run again.
  auto rebuilding() const -> bool { return !haveCachedResult; }
};

using kex::driver::kContinue;
using kex::driver::PassOutcome;

auto passPreludeInterfaces(CompileUnit &unit) -> PassOutcome {
  // Loaded once per process and used by every pass below; timed on its own
  // so it does not hide inside whichever pass touches it first.
  buildProgress.step("Resolving", kex::color::cyan, shownPath(unit.filepath));
  (void)preludeSemanticInterfaces();
  return kContinue;
}

auto passRunManifest(CompileUnit &unit) -> PassOutcome {
  if (unit.mode() != Mode::Compile || !unit.compileRun()) return kContinue;
  unit.manifestKey = runManifestKey(unit.buildFlags, unit.filepath);
  if (unit.manifestKey && runManifestCurrent(*unit.manifestKey)) {
    unit.manifestHit = true;
    buildProgress.step("Cached", kex::color::gray,
                       "build of " + shownPath(unit.filepath));
  }
  return kContinue;
}

auto passPreCheck(CompileUnit &unit) -> PassOutcome {
  if (unit.mode() == Mode::Compile && !unit.options.skipCheck &&
      !unit.manifestHit && !separateBuildRequested())
    unit.preCheck = preCheckWithDependencies(unit.program, unit.filepath);
  return kContinue;
}

auto passOutputDirectory(CompileUnit &unit) -> PassOutcome {
  // For `-R file.kex` without explicit `-o`, use a temp dir and clean up
  // after.
  if (unit.compileRun() && !unit.options.outputDirExplicit &&
      unit.tempDir.empty()) {
    char tmpl[] = "/tmp/kex_XXXXXX";
    char *td = mkdtemp(tmpl);
    if (!td) {
      std::cerr << "error: mkdtemp failed\n";
      return 1;
    }
    unit.tempDir = td;
    unit.options.outputDir = unit.tempDir;
  }
  unit.stem = std::filesystem::path(unit.filepath).stem().string();
  return kContinue;
}

auto passSpecBase(CompileUnit &unit) -> PassOutcome {
  // `<name>.spec.kex` auto-loads `<name>.kex`'s declarations (see
  // specBaseCandidates): prepended so the one Core Erlang module emitted
  // from this program defines them.
  if (!unit.manifestHit)
    unit.beamSpecBaseFile = mergeSpecBase(unit.program, unit.filepath,
                                          &unit.beamSpecBaseRawSource);
  return kContinue;
}

auto passDependencies(CompileUnit &unit) -> PassOutcome {
  // Every module a `using` or a qualified reference reaches, merged in so
  // lowering sees every definition.
  if (unit.manifestHit) return kContinue;
  auto qualifiedModules = sourceModulesNamedBy(unit.program);
  // The separate check (and, with KEX_SEPARATE_CORE, its lowering) runs on
  // the build's own dependencies too.
  UnitGraph separateUnits;
  g_separateEntryPath = unit.filepath;
  unit.beamDeps = resolveBeamDeps(
      unit.program, moduleRootsFor(unit.filepath), &preludeSemanticInterfaces(),
      qualifiedModules, separateCheckRequested() ? &separateUnits : nullptr);
  return kContinue;
}

auto passRuntimeBeams(CompileUnit &unit) -> PassOutcome {
  // Before the run-cache lookup: a cache hit restores its saved `.beam`
  // files over these, so a stale prebuilt copy of a module the program
  // merges from source cannot win (kexhq/kex#27).
  if (unit.mode() == Mode::Compile &&
      !copyRuntimeBeams(unit.options.outputDir, !unit.options.skipPrelude))
    return unit.abandon();
  return kContinue;
}

auto passRunCache(CompileUnit &unit) -> PassOutcome {
  std::vector<kex::ir::EmitResult> cached;
  if (unit.manifestHit) {
    auto hit = tryLoadRunCacheFrom(runManifestDir(*unit.manifestKey),
                                   unit.options.outputDir);
    if (!hit) {
      std::cerr << "error: the cached build of " << unit.filepath
                << " changed while starting; run it again\n";
      return unit.abandon();
    }
    cached = std::move(*hit);
    unit.haveCachedResult = true;
  } else if (unit.mode() == Mode::Compile && unit.compileRun()) {
    unit.runCacheKeyValue = runCacheKeyFor(
        unit.filepath, unit.options.skipCheck, unit.entryRawSourceForCache,
        unit.beamSpecBaseFile, unit.beamSpecBaseRawSource, unit.beamDeps);
    if (unit.runCacheKeyValue)
      if (auto hit = tryLoadRunCache(*unit.runCacheKeyValue,
                                     unit.options.outputDir)) {
        cached = std::move(*hit);
        unit.haveCachedResult = true;
      }
  }
  if (unit.haveCachedResult) unit.moduleResults = std::move(cached);
  return kContinue;
}

auto passExpand(CompileUnit &unit) -> PassOutcome {
  if (!unit.rebuilding()) return kContinue;
  // Trait defaults a merged dependency inherits from a trait declared in
  // the prelude's source (kexhq/kex#235); see backfillExternalTraitDefaults.
  kex::backfillExternalTraitDefaults(unit.program);
  // Dependency modules arrive after the entry's own expansion, so their
  // `compiled` blocks are expanded here, once the program is assembled.
  if (!unit.expandParsedProgram(unit.program, false)) return 1;
  return kContinue;
}

// "Aborted: N type errors — fix before running/compiling".
auto reportAborted(const CompileUnit &unit, int errors) -> void {
  std::cerr << kex::color::apply(kex::color::bold)
            << kex::color::apply(kex::color::magenta)
            << "Aborted:" << kex::color::apply(kex::color::reset) << " "
            << errorCountPhrase(errors, "type") << " — fix before "
            << (unit.compileRun() ? "running" : "compiling")
            << " (use --no-check to skip).\n";
}

auto passCheck(CompileUnit &unit) -> PassOutcome {
  if (!unit.rebuilding()) return kContinue;
  buildProgress.step("Checking", kex::color::blue,
                     moduleCount(unit.beamDeps.size() + 1));
  if (unit.mode() != Mode::Compile && unit.mode() != Mode::EmitCore)
    return kContinue;
  unit.compileAnalysis =
      std::make_unique<kex::semantic::Analyzer>(&preludeSemanticInterfaces());
  // Gating: a type error stops the build. Otherwise (`--no-check`,
  // `--emit-core`) the analysis still runs for the answers lowering reads.
  const bool gating = unit.mode() == Mode::Compile && !unit.options.skipCheck;
  int typeErrors = 0;
  const bool ok = gating
      ? runSemanticCheck(unit.program, unit.filepath, unit.compileAnalysis.get(),
                         unit.beamSpecBaseFile, &typeErrors)
      : unit.compileAnalysis->analyze(unit.program);
  if (!gating) return kContinue;
  if (!ok) {
    reportAborted(unit, typeErrors);
    return 1;
  }
  // The pre-check's verdict, deferred to here (see preCheckWithDependencies).
  if (unit.preCheck.failed) {
    for (const auto &diag : unit.preCheck.diagnostics)
      printSemanticDiagnostic(diag);
    reportAborted(unit, unit.preCheck.typeErrors);
    return 1;
  }
  if (!g_separateDiagnostics.empty()) {
    for (const auto &diagnostic : g_separateDiagnostics)
      std::cerr << diagnostic << '\n';
    reportAborted(unit, static_cast<int>(g_separateDiagnostics.size()));
    return 1;
  }
  return kContinue;
}

auto passLower(CompileUnit &unit) -> PassOutcome {
  if (!unit.rebuilding()) return kContinue;
  buildProgress.step("Lowering", kex::color::yellow,
                     moduleCount(unit.beamDeps.size() + 1));
  // Modules a separate build already lowered, the entry's own first.
  if (g_separateModules) {
    unit.moduleResults = std::move(*g_separateModules);
    g_separateModules.reset();
    std::stable_partition(unit.moduleResults.begin(), unit.moduleResults.end(),
                          [&](const auto &module) {
                            return module.moduleName == "kex_" + unit.stem;
                          });
    return kContinue;
  }
  // Prelude record layouts come from the embedded compiled interface, not
  // from reparsing stdlib source. `emitModules` runs the IR passes.
  std::vector<kex::ir::ExternalRecordLayout> preludeRecordLayouts;
  try {
    preludeRecordLayouts = loadPreludeRecordLayouts();
  } catch (const std::exception &e) {
    std::cerr << "error: invalid prebuilt standard library: " << e.what()
              << "\n";
    return unit.abandon();
  }
  try {
    auto preludeVariantTags = loadPreludeVariantTags();
    std::optional<kex::hir::Program> hir;
    if (unit.compileAnalysis) hir.emplace(unit.program, *unit.compileAnalysis);
    auto irModules = kex::ir::lowerModules(
        unit.program, unit.stem,
        withCheckerAnswers({.sourcePath = unit.filepath,
                            .externals = &preludeExternalModules(),
                            .externalRecords = &preludeRecordLayouts,
                            .externalVariants = &preludeVariantTags},
                           hir ? &*hir : nullptr));
    unit.moduleResults = kex::ir::emitModules(irModules);
  } catch (const kex::ir::LowerError &e) {
    std::cerr << "error: " << e.what() << "\n";
    return unit.abandon();
  }
  return kContinue;
}

auto passWriteCore(CompileUnit &unit) -> PassOutcome {
  if (!unit.rebuilding()) return kContinue;
  for (const auto &emitted : unit.moduleResults) {
    std::string path = unit.options.outputDir + "/" + emitted.moduleName + ".core";
    std::ofstream outFile(path);
    if (!outFile) {
      std::cerr << "error: cannot write " << path << "\n";
      return 1;
    }
    outFile << emitted.source;
    unit.corePaths.push_back(std::move(path));
  }
  if (unit.mode() == Mode::EmitCore) {
    for (const auto &path : unit.corePaths)
      std::cerr << "wrote " << path << "\n";
    return 0;
  }
  return kContinue;
}

auto passErlc(CompileUnit &unit) -> PassOutcome {
  if (!unit.rebuilding()) return kContinue;
  // The runtime and prelude beams were copied before the run-cache lookup.
  if (!compileCoreModules(unit.moduleResults, unit.corePaths,
                          unit.options.outputDir, !unit.tempDir.empty()))
    return unit.abandon();
  return kContinue;
}

auto passStoreCaches(CompileUnit &unit) -> PassOutcome {
  if (!unit.rebuilding()) return kContinue;
  // What the next matching invocation reuses (kexhq/kex#323).
  if (unit.runCacheKeyValue)
    storeRunCache(*unit.runCacheKeyValue, unit.options.outputDir,
                  unit.moduleResults);
  if (unit.manifestKey && !kex::compiled::reachedOutside())
    storeRunManifest(*unit.manifestKey, unit.options.outputDir,
                     unit.moduleResults,
                     runManifestFiles(unit.filepath, unit.beamSpecBaseFile,
                                      unit.beamDeps));
  return kContinue;
}

auto passAttachInterfaces(CompileUnit &unit) -> PassOutcome {
  if (!unit.compileRun() && !unit.corePaths.empty())
    attachInterfaces(unit.program, unit.moduleResults, unit.options.outputDir,
                     unit.stem, unit.options.skipCheck,
                     unit.compileAnalysis.get());
  return kContinue;
}

auto passFinish(CompileUnit &unit) -> PassOutcome {
  // The emitted file is named after the module inside it, so a directory of
  // them can go straight on a BEAM code path (`erl -pa ebin`).
  const std::string kxBeam =
      unit.options.outputDir + "/" + unit.moduleResults.front().moduleName +
      ".beam";
  if (unit.tempDir.empty() && buildProgress.enabled)
    buildProgress.finished(shownPath(kxBeam));
  else if (unit.tempDir.empty() && !quietBuild)
    std::cerr << "  done  " << kxBeam << "\n";
  if (!unit.compileRun()) return 0;
  buildProgress.step("Running", kex::color::cyan, shownPath(unit.filepath));
  return runBuiltProgram(unit.options, unit.filepath, unit.moduleResults,
                         unit.tempDir, unit.scriptArgs);
}

// The passes of a `kex -c` / `--emit-core` build, in order.
auto compilePipeline() -> const kex::driver::Pipeline<CompileUnit> & {
  static const auto pipeline = [] {
    kex::driver::Pipeline<CompileUnit> passes("compile (kex -c, --emit-core)");
    passes
        .add("prelude-interfaces", "load the stdlib's compiled interfaces",
             passPreludeInterfaces)
        .add("run-manifest", "-c --run: is a recorded build still current?",
             passRunManifest)
        .add("pre-check", "check the entry with its project modules",
             passPreCheck)
        .add("output-dir", "choose the output (a temp dir for a run)",
             passOutputDirectory)
        .add("spec-base", "merge a spec's base file", passSpecBase)
        .add("dependencies", "merge every module the program reaches",
             passDependencies)
        .add("runtime-beams", "copy the prebuilt runtime", passRuntimeBeams)
        .add("run-cache", "reuse an earlier identical build", passRunCache)
        .add("expand", "expand `compiled` blocks of merged modules",
             passExpand)
        .add("check", "type-check; record the answers lowering reads",
             passCheck)
        .add("lower", "lower to IR, run the IR passes, emit Core Erlang",
             passLower)
        .add("write-core", "write .core files (--emit-core stops here)",
             passWriteCore)
        .add("erlc", "compile Core Erlang to .beam", passErlc)
        .add("store-caches", "record this build for the next one",
             passStoreCaches)
        .add("attach-kexi", "attach each module's KexI interface",
             passAttachInterfaces)
        .add("finish", "report, and run it for -c --run", passFinish);
    return passes;
  }();
  return pipeline;
}

auto compileToBeam(Options &options, const std::string &filepath,
                   kex::ast::Program &program,
                   const std::vector<std::string> &scriptArgs,
                   const std::string &entryRawSourceForCache,
                   const std::string &buildFlags,
                   const ExpandProgram &expandParsedProgram) -> int {
  CompileUnit unit{options, filepath, program, scriptArgs,
                   entryRawSourceForCache, buildFlags, expandParsedProgram};
  return compilePipeline().run(unit);
}

// `kex -C`: checks the entry file the way the run and compile paths see it —
// with its companion and the project modules it uses merged in — and prints
// the diagnostics (as JSON, a summary, or one per line), then the types with
// `--dump-types`. Returns 0 when there were no errors.
auto checkEntry(const Options &options, const std::string &filepath,
                kex::ast::Program &program) -> int {
  const auto &dumpTypes = options.dumpTypes;
  const auto &jsonOutput = options.jsonOutput;
  const auto &summaryMode = options.summaryMode;
  // A companion file's declarations are part of what this file means: a
  // `.spec.kex`'s base, and a `package.kex`'s manifest vocabulary. The run
  // and compile paths merge them before checking; do the same here, or
  // `kex -C` reports names the other two accept.
  const auto checkCompanion = mergeSpecBase(program, filepath);

  // ...and so are the project's OTHER modules. `mode == Mode::Run` and
  // `mode == Mode::Compile` both merge every module a `using` or a qualified
  // reference reaches before checking; this path did not, so it built its
  // checker from the prelude interface alone. A two-module program then
  // reported errors no other mode saw — a record declared in a sibling
  // module was an unknown name here, so a union declared alongside it
  // refused its own members and any annotation naming it went unverified.
  // The editor sees this too: the LSP checks the way `-C` does.
  {
    auto qualifiedModules = sourceModulesNamedBy(program);
    // PROJECT roots only. A stdlib module reached by `using FS` already
    // has a compiled interface, and merging its SOURCE on top of that adds
    // a second, unannotated candidate for every method it defines — enough
    // to switch off a mismatch report that the interface alone gets right
    // (spec/typestate_survives_standin.kex: writing through a read-only
    // FileHandle stopped being an error). The run and compile paths merge
    // the stdlib too, but they consume it for CODEGEN, where having the
    // source is the point.
    const auto projectRoots =
        kex::withoutStandardLibraryRoots(moduleRootsFor(filepath));
    UnitGraph units;
    resolveBeamDeps(program, projectRoots, &preludeSemanticInterfaces(),
                    qualifiedModules, &units);
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

  if (options.dumpHir || options.verifyHir) {
    const kex::hir::Program hir(program, analyzer);
    const auto &sites = hir.callSites();
    std::map<kex::hir::TargetKind, std::size_t> counts;
    for (const auto &site : sites) {
      counts[site.kind]++;
      if (options.dumpHir)
        std::cout << site.expr->location.file << ":"
                  << site.expr->location.line << ":"
                  << site.expr->location.column << "  "
                  << kex::hir::targetKindName(site.kind) << "  "
                  << site.callee
                  << (site.target.empty() ? "" : "  -> " + site.target) << "\n";
    }
    std::cerr << "call sites: " << sites.size() << " (";
    for (const auto kind : kex::hir::kTargetKinds)
      std::cerr << (kind == kex::hir::kTargetKinds[0] ? "" : ", ")
                << kex::hir::targetKindName(kind) << " " << counts[kind];
    std::cerr << ")\n";
    if (options.verifyHir) {
      const auto problems = kex::hir::verify(hir);
      for (const auto &problem : problems) std::cerr << problem << "\n";
      if (!problems.empty()) allOk = false;
    }
  }

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
}

// `kex -R`: runs the entry on the tree-walking interpreter, with its spec
// base and the project modules it uses merged in, after the same check
// `-c` runs (unless --no-check). Returns the program's exit status; a spec
// with a failing example exits 1.
auto runOnWalker(const Options &options, const std::string &filepath,
                 kex::ast::Program &program,
                 const std::vector<std::string> &scriptArgs,
                 const ExpandProgram &expandParsedProgram) -> int {
  const auto &skipCheck = options.skipCheck;
  const auto &allowMocks = options.allowMocks;
  const auto &testReportMode = options.testReportMode;
  const auto &testFilters = options.testFilters;
  // `<name>.spec.kex` auto-loads `<name>.kex`'s declarations — merge them
  // into `program` BEFORE the semantic check, exactly as the BEAM path
  // above does. Loading them only into the Evaluator (as this path used to)
  // leaves the checker blind to the base file's types: spec/
  // json_parser.spec.kex's `JsonNull`/`JsonBool` come from examples/
  // json_parser.kex's `type Json`, and the checker rejected every one of
  // them as an undefined constructor before the program ever ran.
  const auto specBaseFile = mergeSpecBase(program, filepath);

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
  resolveBeamDeps(program, moduleRootsFor(filepath),
                  &preludeSemanticInterfaces(),
                  sourceModulesNamedBy(program));

  if (!expandParsedProgram(program, false)) return 1;

  // As the BEAM build does: a type error stops the run, and under
  // `--no-check` the analysis still runs, silently, for the decisions the
  // evaluator reads (the same ones lowering reads).
  kex::semantic::Analyzer runAnalyzer(&preludeSemanticInterfaces());
  if (skipCheck) {
    runAnalyzer.analyze(program);
  } else {
    int typeErrors = 0;
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

  kex::interpreter::Evaluator evaluator;
  evaluator.setArgs(scriptArgs);
  evaluator.setModuleRoots(moduleRootsFor(filepath));
  // Mock.* is denied by default — a spec earns it by being a spec, and
  // anything else has to say --allow-mocks (issue #144).
  evaluator.setMocksAllowed(allowMocks || isSpecEntry(filepath));
  evaluator.setTestReportMode(testReportMode);
  evaluator.setTestFilters(testFilters);
  // The checked program, read by the evaluator as lowering reads it.
  // Declared out here so it outlives the evaluator that borrows from it.
  const kex::hir::Program hir(program, runAnalyzer);
  evaluator.setProgram(&hir);

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

int main(int argc, char *argv[]) {
  auto options = parseCommandLine(argc, argv);
  if (options.exitNow) return *options.exitNow;
  if (options.printPasses) {
    compilePipeline().print(std::cout);
    std::cout << "IR passes (every emitted module):\n";
    const auto irPasses = kex::ir::standardPasses();
    for (const auto &pass : irPasses.passes())
      std::cout << "  " << pass.name << "\n";
    return 0;
  }
  auto &mode = options.mode;
  auto &skipCheck = options.skipCheck;
  auto &completePrefix = options.completePrefix;
  auto &outputDir = options.outputDir;
  auto &astFilename = options.astFilename;
  auto &outputDirExplicit = options.outputDirExplicit;
  auto &replEval = options.replEval;
  auto &compileRun = options.compileRun;
  auto &skipPrelude = options.skipPrelude;
  auto &collapseReport = options.collapseReport;

  // A persistent `kex -c` build shows its steps; a run shows them on request,
  // since its stderr is the program's own.
  buildProgress.enabled =
      !quietBuild &&
      (progressRequested || (mode == Mode::Compile && !compileRun));

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

  setDefaultScanCache();
  // One build or check, over files that stay as they are while it runs. Not
  // the REPL or the language server, which see files change.
  const bool oneBuild =
      !replEval && optind < argc &&
      (mode == Mode::Check || mode == Mode::Compile || mode == Mode::Run ||
       mode == Mode::Unset || mode == Mode::EmitCore || mode == Mode::Expand);
  kex::compiled::setReuseCollapses(oneBuild);
  g_memoizeDependencyExpansion = oneBuild;
  kex::module::Resolver::setCaching(oneBuild);
  // Everything this build reads, for the run cache by entry.
  if (oneBuild) buildRecorder.emplace();
  // What the build was asked for, as written: part of that cache's key.
  // getopt has moved every option in front of the file by now.
  std::string buildFlags;
  for (int i = 1; i < optind; i++) buildFlags += std::string(argv[i]) + "\n";

  if (mode == Mode::Lsp) {
#ifdef KEX_HAS_LSP
    return kex::lsp::run(std::cin, std::cout, prebuiltRuntimeBeamDir());
#endif
  }

  if (mode == Mode::Complete)
    return printCompletions(completePrefix, optind < argc ? argv[optind] : nullptr);

  if (replEval && (optind < argc || !((mode == Mode::Compile && compileRun) ||
                                      mode == Mode::Repl ||
                                      mode == Mode::BeamRepl))) {
    std::cerr << "error: --eval enters code in the REPL, so it takes no file "
                 "and no other mode (use -i for the tree-walk REPL)\n";
    return 1;
  }

  if (optind >= argc && mode != Mode::Repl) {
    // No file — enter REPL mode (the BEAM REPL by default, the tree-walk
    // REPL for -i and for the other non-BEAM modes)
    if (mode == Mode::Compile && compileRun)
      mode = Mode::BeamRepl;
    else if (mode != Mode::BeamRepl)
      mode = Mode::Repl;
  }

  if (mode == Mode::BeamRepl) return runBeamRepl(skipPrelude);
  if (mode == Mode::Repl) return runWalkerRepl(skipPrelude);

  std::string filepath = argv[optind];
  kex::module::Resolver::setEntryFile(filepath);
#ifndef __EMSCRIPTEN__
  // A spec says it is one to what it runs, the BEAM child included, so a
  // framework can pick its test settings without being told: Metro's
  // environment is `test` under it unless METRO_ENV says otherwise.
  if (isSpecEntry(filepath)) setenv("KEX_TEST", "1", 0);
#endif

  const bool compiledInput =
      filepath.size() > 5 &&
      filepath.compare(filepath.size() - 5, 5, ".beam") == 0;
  // Every other mode reads Kex SOURCE. Handed a compiled module, `-c` used to
  // fall into the run path below and execute it, and the source-reading modes
  // parsed BEAM bytes as program text (kexhq/kex#85).
  if (compiledInput && !(mode == Mode::Compile && compileRun)) {
    std::cerr << "error: " << filepath
              << " is a compiled BEAM module, not Kex source — run it with "
                 "`kex "
              << filepath << "`, or pass the .kex file it was built from\n";
    return 1;
  }

  // `kex file.kx.beam [args]` or `kex file.beam [args]` — run a compiled BEAM
  // module.
  if (compiledInput)
    return runCompiledBeam(filepath, options,
                           std::vector<std::string>(argv + optind + 1, argv + argc));

  // Reject non-.kex files before trying to parse them.
  if (filepath.size() < 4 ||
      filepath.compare(filepath.size() - 4, 4, ".kex") != 0) {
    std::cerr << "error: " << filepath << ": expected a .kex source file\n";
    return 1;
  }

  auto source = readFile(filepath);
  // Kept around (the lexer below consumes `source` with std::move) so the
  // BEAM `--run` path can use the entry file's ORIGINAL bytes as part of a
  // run-result cache key (kexhq/kex#323) — see `runCacheKeyFor`.
  const std::string entryRawSourceForCache = source;
  // An empty file is an empty program to the AST tools. The interpreter answers
  // `Kex.AST.parse("")` and `parseSyntax("")` with an empty tree, and BEAM
  // reaches both through a temporary file, so refusing it here made the two
  // backends disagree. A file that could not be opened still stops here —
  // readFile has already said why.
  if (source.empty() &&
      !((mode == Mode::EmitAst || mode == Mode::EmitSyntax) &&
        std::filesystem::is_regular_file(filepath)))
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

  // --emit-syntax reprints the source from its tokens, and --emit-ast reads
  // doc comments out of their trivia, so both keep the text (and
  // --emit-syntax the token list) that the lexer and parser below consume.
  const std::string syntaxSource =
      mode == Mode::EmitSyntax || mode == Mode::EmitAst ? source : std::string{};
  kex::Lexer lexer(std::move(source), filepath);
  auto tokens = lexer.tokenizeAll();
  const auto syntaxTokens =
      mode == Mode::EmitSyntax ? tokens : std::vector<kex::Token>{};
  const auto astDocs = mode == Mode::EmitAst
      ? kex::ast::extractDocComments(tokens, syntaxSource)
      : std::unordered_map<int, std::string>{};

  if (mode == Mode::Lex) {
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
        (mode == Mode::Run || mode == Mode::Compile || mode == Mode::EmitAst ||
         mode == Mode::EmitSyntax ||
         mode == Mode::Parse || mode == Mode::Expand || mode == Mode::EmitCore)) {
      if (mode == Mode::EmitAst || mode == Mode::EmitSyntax) {
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
                << (mode == Mode::Run || compileRun
                        ? "running"
                        : (mode == Mode::Parse || mode == Mode::Expand
                               ? "parsing"
                               : "compiling"))
                << ".\n";
      return 1;
    }

    if (mode == Mode::Parse) {
      printAst(program);
      return 0;
    }

    if (mode == Mode::EmitSyntax) {
      kex::beam::TermBuilder builder;
      auto tree = kex::ast::buildSyntaxTree(builder, syntaxTokens, syntaxSource,
                                            parser.syntaxSpans());
      if (!tree) {
        std::cerr << "error: " << filepath
                  << ": the source could not be divided among its tokens\n";
        return 1;
      }
      auto bytes = kex::beam::encodeEtf(*tree);
      std::cout.write(reinterpret_cast<const char *>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
      return 0;
    }
    if (mode == Mode::EmitAst) {
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
      // Kex.embed(...) resolves relative to the ENTRY file, even on the
      // later calls below where dependency modules have been merged into
      // `target` — a Kex.embed inside a dependency module's own source
      // resolves against the wrong directory until per-declaration source
      // provenance exists (the same gap this function's own comment notes
      // for compiled blocks in merged-in modules).
      expandOptions.sourcePath = filepath;
      expandOptions.moduleRoots = moduleRootsFor(filepath);
      setScanCache(expandOptions);
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

    if (mode == Mode::Expand) {
      printAst(program);
      return 0;
    }

    if (mode == Mode::Compile || mode == Mode::EmitCore)
      return compileToBeam(options, filepath, program, scriptArgs,
                           entryRawSourceForCache, buildFlags,
                           expandParsedProgram);

    if (mode == Mode::Run)
      return runOnWalker(options, filepath, program, scriptArgs,
                         expandParsedProgram);
    return checkEntry(options, filepath, program);
  } catch (const std::exception &e) {
    std::cerr << kex::color::apply(kex::color::bold)
              << kex::color::apply(kex::color::red)
              << "Internal error:" << kex::color::apply(kex::color::reset)
              << " " << e.what() << "\n";
    return 1;
  }

  return 0;
}
