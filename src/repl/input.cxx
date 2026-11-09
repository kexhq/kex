// What both REPLs share: classifying an input, the text helpers that find a
// definition's name or a binding's pattern, line input (readline, history,
// `--eval` scripts), Tab completion, and `/with` capability replacements.
#include "../beam/beam_file.hxx"
#include "../beam/collect_metadata.hxx"
#include "../beam/etf.hxx"
#include "../beam/kexi.hxx"
#include "../beam/kexi_registry.hxx"
#include "../cache/cache.hxx"
#include "../cache/separate_unit.hxx"
#include "../beam/term_builder.hxx"
#include "../ast/clone.hxx"
#include "../ast/convert.hxx"
#include "../ast/syntax.hxx"
#include "../ast/type_text.hxx"
#include "../common/artifact_versions.hxx"
#include "../common/prelude_tiers.hxx"
#include "../common/color.hxx"
#include "../interpreter/evaluator.hxx"
#include "../hir/hir.hxx"
#include "../hir/repl_input.hxx"
#include "../ir/emit_core.hxx"
#include "../ir/pass_manager.hxx"
#include "../driver/pipeline.hxx"
#include "../ir/lower.hxx"
#include "../lexer/lexer.hxx"
#include "../compiled/expand.hxx"
#include "../module/discovery.hxx"
#include "../module/resolver.hxx"
#include "../parser/parser.hxx"
#include "../semantic/analyzer.hxx"
#include "../semantic/db.hxx"
#include "../validation/tag_validator.hxx"
#include "../common/runtime_dir.hxx"
#include "../driver/dependencies.hxx"
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
#include "../common/completion.hxx"
#include "../common/diagnostics.hxx"
#include "../common/digest.hxx"
#include "../common/file_io.hxx"
#include "../common/prelude_interfaces.hxx"
#include "../common/prelude_loader.hxx"
#include "../common/process.hxx"
#include "../common/repl_commands.hxx"
#include "../common/string_utils.hxx"
#include "../common/timings.hxx"
#include "../common/well_known.hxx"
#include "../semantic/completion_at.hxx"
#include "../semantic/symbol_queries.hxx"
#include <functional>
#include <thread>
#include "../driver/build_support.hxx"
#include "input.hxx"
#include "session.hxx"

using kex::prebuiltRuntimeBeamDir;
using kex::driver::buildProgress;
using kex::driver::BuildProgress;
using kex::driver::dependencyFailure;
using kex::driver::g_dependencyFailureThrows;
using kex::driver::g_memoizeDependencyExpansion;
using kex::driver::isAutomaticImport;
using kex::driver::LoadedDep;
using kex::driver::moduleCount;
using kex::driver::prependItems;
using kex::driver::reportModuleFileError;
using kex::driver::setDefaultScanCache;
using kex::driver::setScanCache;
using kex::driver::sourceModulesNamedBy;
using kex::driver::UnitGraph;
using kex::driver::unitsOf;
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
using kex::ast::typeText;
using kex::module::collectUsingModules;
using kex::module::declaredModuleNames;
using kex::module::fileDeclaringModule;
using kex::module::moduleHeaderOf;
using kex::module::moduleNameForPath;
using kex::module::shownPath;
using kex::module::skippedByModuleWalk;
using kex::module::standardModuleRoots;
using kex::snakeCased;

std::function<ReplCompletionContext()> g_replCompletionContext;
ReplCompletionBase *g_replCompletionBase = nullptr;
// Earlier lines of a multi-line input still being typed (`make X do` …), so a
// line inside a block completes with the block around it.
std::string g_replPendingInput;

// Whether an input is a top-level declaration rather than code for `main`.
auto replInputIsDeclaration(const std::string &input) -> bool {
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

// The modules under the REPL's source roots, by the name each file declares:
// what a dotted name typed at the prompt can lead to. Read once a session —
// the roots are fixed when it starts — and only each file's header, so `DB.`
// costs no parsing.
auto replRootModules() -> const std::vector<std::string> & {
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
auto replRootCompletions(const std::string &qualified,
                                const std::string &word)
    -> std::vector<std::string> {
  std::vector<std::string> matches;
  if (!g_replCompletionBase) return matches;
  if (!qualified.empty()) {
    const auto module = qualified.substr(0, qualified.size() - 1);
    const auto roots =
        moduleRootsFor((std::filesystem::current_path() / "<repl>").string());
    kex::module::Resolver resolver(roots);
    // Its declarations only, as a `using` loads it (kexhq/kex#323).
    if (const auto resolved = resolver.resolve(module);
        resolved && !g_replCompletionBase->loaded.contains(resolved->path))
      g_replCompletionBase->loaded[resolved->path] =
          kex::semantic::symbolsOfFiles({resolved->path}, roots);
    auto base = g_replCompletionBase->symbols();
    for (auto &match : kex::semantic::completionsFor({{"<base>", &base}}, qualified + word,
                                                     &preludeSemanticInterfaces()))
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
auto replCompleteAt(const std::string &line, size_t point)
    -> kex::semantic::CompletionAt {
  if (!g_replCompletionBase) return {};
  const auto context = g_replCompletionContext ? g_replCompletionContext()
                                               : ReplCompletionContext{};
  const auto input = g_replPendingInput.empty()
                         ? line
                         : g_replPendingInput + "\n" + line;
  const bool declaration = replInputIsDeclaration(input);
  auto completion = kex::semantic::completeReplInput(
      g_replCompletionBase->symbols(), checkForCompletion, context.definitions,
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
auto printReplCompletions(const std::string &prefix) -> void {
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
auto replHasOpenDelimiter(const std::string &source) -> bool {
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
auto throwOnParseErrors(const kex::Parser &parser) -> void {
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
auto replClauseHead(const std::string &source)
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


// Whether `type` — as the REPL displays it — is usable as the annotation of a
// replayed binding: it must parse and check as `let x : <type> = <untyped>`
// against the session's definitions. Displayed types are not always source
// (`Unknown`, free type variables), and a replay line that failed to check
// would break every later input, so anything doubtful is refused.
auto replTypeAnnotates(const std::string &definitions,
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
auto isReplCompiledBlock(const std::string &source) -> bool {
  if (source.rfind("compiled", 0) != 0) return false;
  auto after = source.find_first_not_of(" \t", 8);
  return after != 8 && after != std::string::npos &&
         source.compare(after, 2, "do") == 0 &&
         (after + 2 == source.size() ||
          !std::isalnum(static_cast<unsigned char>(source[after + 2])));
}

auto replDefinitionName(const std::string &source) -> std::string {
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
auto replIsUpperConstantBinding(const std::string &source, size_t off)
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
auto replTrimLeadingIndent(std::string source) -> std::string {
  const auto first = source.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  source.erase(0, first);
  return source;
}

// Whether `source[at, at + length)` is a whole word: no letter or digit on
// either side.
auto replWholeWordAt(const std::string &source, size_t at,
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
auto replBlockDepth(const std::string &source) -> int {
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

std::vector<std::string> g_completionMatches;

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
                  const std::string &except) -> std::string {
  std::string open;
  for (const auto &with : withs)
    if (with.capability != except)
      open += "  with " + with.capability + " = " + with.binding + " do\n";
  return open;
}

auto replWithClose(const std::vector<ReplSessionWith> &withs,
                   const std::string &except) -> std::string {
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

#ifdef HAS_READLINE
// Loads the REPL history both REPLs share (~/.config/kex/history), so a
// session in one is recalled in the other; returns the file to save it to,
// "" when there is no home directory.
auto loadReplHistory() -> std::string {
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


auto classifyReplInput(const std::string &source) -> ReplInput {
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


auto queueReplScriptLine(const std::string &line) -> void { g_replScript.push_back(line); }

// Tab completion and the cancel keys on readline's line editor, as both REPLs
// set them up.
auto installReplLineEditing() -> void {
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
}

auto saveReplHistory(const std::string &file) -> void {
#ifdef HAS_READLINE
  if (!file.empty()) write_history(file.c_str());
#else
  (void)file;
#endif
}
