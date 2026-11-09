#pragma once

// What both REPLs share (repl/input.cxx): classifying an input, the text
// helpers over it, line input, Tab completion, and `/with` replacements.

#include "../semantic/symbol_queries.hxx"
#include "../parser/parser.hxx"

#include <deque>
#include <functional>
#include <optional>
#include <map>
#include <string>
#include <vector>

// What Tab and `/complete` complete against: the program each REPL builds to
// type-check an input — its top-level definitions, then `main do` with the
// local bindings replayed — with the input being typed spliced in, so the
// receiver of `x.` is typed by the analyzer exactly as the editor types it.
struct ReplCompletionContext {
  std::string definitions;
  std::string bindings;
};
// What REPL completion lists names from besides the input it checks (whose
// own and imported declarations it reads off the check): the prelude's, and
// those of modules and `/load`ed units read for their members, by file.
struct ReplCompletionBase {
  std::vector<kex::semantic::SymbolInfo> prelude;
  std::map<std::string, std::vector<kex::semantic::SymbolInfo>> loaded;
  auto symbols() const -> std::vector<kex::semantic::SymbolInfo> {
    auto all = prelude;
    for (const auto &[_, symbols] : loaded) all.insert(all.end(), symbols.begin(), symbols.end());
    return all;
  }
};
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
// What a REPL step tells the input loop: read the next input, or stop (a
// script fed to the REPL has failed).
enum class ReplStep { Next, Stop };

// Classifying and splitting an input.
auto classifyReplInput(const std::string &source) -> ReplInput;
auto replInputIsDeclaration(const std::string &input) -> bool;
auto replHasOpenDelimiter(const std::string &source) -> bool;
auto replBlockDepth(const std::string &source) -> int;
auto replClauseHead(const std::string &source) -> std::optional<std::string>;
auto replDefinitionName(const std::string &source) -> std::string;
auto replTrimLeadingIndent(std::string source) -> std::string;
auto replTypeAnnotates(const std::string &definitions, const std::string &type) -> bool;
auto replBindingEqualsPos(const std::string &source, size_t from) -> size_t;
auto replLastStatement(const kex::ast::MainBlock &main) -> const kex::ast::Expr *;
auto collectPatternNames(const kex::ast::Pattern &pattern, std::vector<std::string> &names)
    -> void;
auto throwIfPartialPatternRebind(const std::vector<std::string> &bound,
                                 const std::vector<std::pair<std::string, std::string>> &binds)
    -> void;
auto throwOnParseErrors(const kex::Parser &parser) -> void;

// Line input: readline when built with it, `--eval` lines first.
auto readLine(const std::string &prompt) -> std::pair<std::string, bool>;
auto takeReplInputCancelled() -> bool;
auto bindReplCancelKeys() -> void;
// Completion and the cancel keys on the line editor; the history, kept.
auto installReplLineEditing() -> void;
auto saveReplHistory(const std::string &file) -> void;
#ifdef HAS_READLINE
auto loadReplHistory() -> std::string;
#endif
extern std::deque<std::string> g_replScript;
// Whether the line just read came from `g_replScript`, and whether one failed.
extern bool g_replLineFromScript;
extern bool g_replScriptFailed;

// Tab completion: what it checks and lists names from, set by the session.
auto printReplCompletions(const std::string &prefix) -> void;
extern std::function<ReplCompletionContext()> g_replCompletionContext;
extern ReplCompletionBase *g_replCompletionBase;
extern std::string g_replPendingInput;

// `/with` capability replacements kept for the session.
auto handleReplWithCommand(const std::string &input, std::vector<ReplSessionWith> &withs,
                           std::optional<ReplSessionWith> &pending) -> std::string;
// `except` leaves out the capability being replaced: its hidden binding is
// what the input rebinds.
auto replWithOpen(const std::vector<ReplSessionWith> &withs, const std::string &except = "")
    -> std::string;
auto replWithClose(const std::vector<ReplSessionWith> &withs, const std::string &except = "")
    -> std::string;

// Stored definitions hold one clause per entry, appended with newlines. A
// clause continues while its `do` blocks are unclosed, so splitting cannot
// simply break on every newline. `countBlocks` is the caller's own block
// counter (each REPL has one).
template <typename CountBlocks>
auto splitReplClauses(const std::string &source,
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
