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

// The type of an arbitrary receiver expression, by re-parsing `source` with
// the half-typed member removed and asking the analyzer. `analysisDb` parses
// the recovered buffer (and resolves its `using` modules); keep it between
// calls so module sources are parsed once rather than per keystroke.
auto recoveredReceiverQualifier(SemanticDB& analysisDb,
                                const std::string& source,
                                const std::string& path,
                                const DotReceiver& receiver, size_t cursor,
                                const ImportedInterfaces* interfaces)
    -> std::string;

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

// Completions at byte offset `cursor` in `source`. Members are looked up in
// `db` (the index holding the prelude and the user's definitions); `source`
// itself is analyzed through `analysisDb` under `path`.
auto completeAt(const SemanticDB& db, SemanticDB& analysisDb,
                const std::string& path, const std::string& source,
                size_t cursor, const ImportedInterfaces* interfaces)
    -> CompletionAt;

} // namespace kex::semantic
