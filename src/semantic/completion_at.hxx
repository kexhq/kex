#pragma once

#include "analyzer.hxx"  // completes db.hxx's forward-declared Diagnostic
#include "db.hxx"
#include "imported_interfaces.hxx"
#include "types.hxx"

#include <cstddef>
#include <string>
#include <vector>

namespace kex::semantic {

// Type-aware completion shared by the LSP and both REPLs. The receiver of a
// member completion (`x.`, `makeBox(3).`, a builder chain, a lambda
// parameter, a literal) is typed by analyzing the surrounding program rather
// than guessed from the text of one line.

// The completion qualifier for a type: collections are indexed by their
// stdlib names, everything else by the type as it is displayed.
auto completionQualifierForType(const TypePtr& type) -> std::string;

// Where the receiver of a member completion starts, and the dot it hangs off.
// Walks the real shape of the receiver — identifiers, balanced `(...)`/
// `[...]`, and quoted strings, joined by dots, across line breaks.
struct DotReceiver {
    size_t start = 0;  // first byte of the receiver expression
    size_t dot = 0;    // the '.' the member being typed hangs off
    // Where the AST records the OUTERMOST expression of the receiver. A call
    // is recorded at its argument list's `(`, not at the receiver it hangs
    // off: in a builder chain `Web.Server.new(0)\n  .get("/", ~h)` the last
    // `.get(…)` call sits at that `(`. Zero when the receiver ends in no call.
    size_t callOpen = 0;
    bool valid = false;
};

auto scanDotReceiver(const std::string& source, size_t cursor) -> DotReceiver;

// A buffer checked as a build checks it: parsed and expanded, its `using`
// modules loaded, and analyzed with its names checked (so the analyzer's
// name index holds its symbols and the names in scope at each point). Both
// are kept, since the analysis refers into the program.
struct CheckedSource {
    std::shared_ptr<ast::Program> program;
    std::shared_ptr<Analyzer> analyzer;
    // Whatever else the program refers into (merged dependencies' sources).
    std::shared_ptr<void> keepAlive;
};
// How a caller checks a buffer under a path, with its own module roots and
// interfaces: what completion asks of the REPLs and the language server.
using CompletionChecker =
    std::function<std::optional<CheckedSource>(const std::string& path, const std::string& source)>;

// The type of an arbitrary receiver expression, by re-checking `source` with
// the half-typed member removed and asking the analyzer.
auto recoveredReceiverQualifier(const CompletionChecker& check,
                                const std::string& source,
                                const std::string& path,
                                const DotReceiver& receiver, size_t cursor,
                                const ImportedInterfaces* interfaces)
    -> std::string;

// What the files at `paths` declare, each parsed and expanded (with `roots`
// for the modules an expansion reads): the base of the names completion
// offers, such as the prelude's sources, or a module loaded for its members.
auto symbolsOfFiles(const std::vector<std::string>& paths,
                    const std::vector<std::string>& roots) -> std::vector<SymbolInfo>;
// What `source`, read as the file `path`, declares.
auto symbolsOfSource(const std::string& path, std::string source,
                     const std::vector<std::string>& roots) -> std::vector<SymbolInfo>;

// The delimiters and `do` blocks still open at the end of `text`, closed in
// order: what a half-typed line needs appended so that it parses.
auto completionClosers(const std::string& text) -> std::string;

struct CompletionAt {
    // Byte offset in the source where the completed word starts: each match
    // replaces source[replaceFrom, cursor).
    size_t replaceFrom = 0;
    // The type the receiver resolved to, for a member completion; empty
    // otherwise.
    std::string qualifier;
    std::vector<std::string> matches;
};

// Completions at byte offset `cursor` in `source`, checked under `path`.
// Names are those the checked program declares (the user's definitions, its
// `using` modules) and `base`'s (the prelude's, modules loaded besides).
auto completeAt(const std::vector<SymbolInfo>& base, const CompletionChecker& check,
                const std::string& path, const std::string& source,
                size_t cursor, const ImportedInterfaces* interfaces)
    -> CompletionAt;

// Completions for a REPL input line, shared by the native REPL (main.cxx) and
// the wasm REPL (wasm_repl.cxx) so their program-splicing cannot drift.
// `definitions` are the session's top-level declarations and `bindings` its
// `main do` local bindings; `input` is the full text being typed (earlier
// continuation lines + the current `line`), `line` the current line, and
// `point` the cursor's byte offset within `line`. The input is spliced into a
// program — `definitions`, then `main do` with the bindings replayed — so the
// analyzer types the receiver exactly as it is being typed. `declaration`
// selects the definition splice (no `main do`/`end` wrapper). The result's
// `replaceFrom` is an offset within `line`.
auto completeReplInput(const std::vector<SymbolInfo>& base, const CompletionChecker& check,
                       const std::string& definitions,
                       const std::string& bindings,
                       const std::string& input, const std::string& line,
                       size_t point, bool declaration,
                       const ImportedInterfaces* interfaces) -> CompletionAt;

} // namespace kex::semantic
