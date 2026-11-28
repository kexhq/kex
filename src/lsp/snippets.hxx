#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace kex::lsp {

// A completion that inserts a whole construct (`if … end`, `make … do … end`)
// rather than a name. `body` is LSP snippet syntax (`${1:name}`, `$0`),
// already indented for the line it is inserted on.
struct Snippet {
    std::string label;
    std::string filterText;
    std::string detail;
    std::string body;
    // Replaces the cursor's line up to the cursor rather than the word, so
    // the line can move left: `elif` and `else` sit level with their `if`.
    bool fromLineStart = false;
};

// Where the word being typed at a cursor sits, as far as which constructs
// can be written there.
enum class SnippetContext {
    None,         // inside a string or comment, after `.`, mid-expression
    TopLevel,     // a file's own items: declarations and `main`
    Declarations, // inside `module`, `capability`, `compiled`: declarations
    Functions,    // inside `make` / `serving`: function declarations only
    Statements,   // a function, `main` or block body
    Expression,   // after `=`: the constructs that are values
    BlockArgument // after a call or a block header on the same line: `do`
};

// The context of the word ending at byte `cursor` of `text`, read from the
// tokens before it, so it holds while the code after the cursor is still
// unfinished.
auto snippetContextAt(const std::string& text, size_t cursor) -> SnippetContext;

// The snippets that can be written where the word ending at byte `cursor`
// of `text` starts and that the word is a prefix of. Empty when no word is
// being typed.
// `blockParameters`, when the checker knows them, are the types the call
// before the cursor passes its block: `do |…|` then names one for each.
auto snippetCompletions(
    const std::string& text, size_t cursor, const std::vector<std::string>& blockParameters = {})
    -> std::vector<Snippet>;

// A word that writes `match` arms: the `do` after `match subject` (`header`),
// or a line of its own inside a `match … do` block. `matchOffset` is where
// that `match` keyword is, so the caller can ask the checker about it.
struct MatchSite {
    size_t matchOffset = 0;
    bool header = false;
};
auto matchSiteAt(const std::string& text, size_t cursor) -> std::optional<MatchSite>;

// An arm the checker says a match still owes (TypeChecker::OwedArm).
struct Arm {
    std::string name;
    size_t arity = 0;
    bool type = false; // a union member taken by a type pattern, `s: String`
};
// The snippets that write `arms` at a match site: after the header, a `do`
// with every arm and its `end`; inside the block, each arm, and all of them
// at once.
auto armSnippets(
    const std::string& text, size_t cursor, const MatchSite& site, const std::vector<Arm>& arms)
    -> std::vector<Snippet>;

// A word that writes the functions a `make … implement:` block owes: the `do`
// after its header (`header`), or a line of its own inside a `make` block.
// `makeOffset` is where that `make` keyword is.
struct MakeSite {
    size_t makeOffset = 0;
    bool header = false;
};
auto makeSiteAt(const std::string& text, size_t cursor) -> std::optional<MakeSite>;

// A function the checker says a block owes a trait (TypeChecker::OwedFunction).
struct Stub {
    std::string name;
    bool foul = false;
    std::vector<std::string> params; // their types, the receiver left out
    std::string result;              // "" when not determined
};
// The snippets that write `stubs` at a make site: after the header, a `do`
// with every stub and its `end`; inside the block, each stub, and all of
// them at once.
auto stubSnippets(
    const std::string& text, size_t cursor, const MakeSite& site, const std::vector<Stub>& stubs)
    -> std::vector<Snippet>;

// What pressing Enter after a line that opens a block (`if c`, `xs.each do`,
// `make Box do`) adds when the block has no `end`: the new line indented one
// step past the opener (`indent` replaces bytes [lineStart, lineEnd)), and
// `closing`, the `end` level with the opener, inserted at `closingAt`.
struct EnterEdit {
    size_t lineStart = 0;
    size_t lineEnd = 0;
    std::string indent;
    size_t closingAt = 0;
    std::string closing;
};
// The edit for an Enter that left the cursor at byte `cursor` of `text`, on
// a line holding nothing but its indentation. Nothing when the line before
// opens no block, or every block in the file already has its `end`.
// `unit` is one step inward, from the client's formatting options.
auto endOnEnter(const std::string& text, size_t cursor, const std::string& unit)
    -> std::optional<EnterEdit>;

// Where the `end` closing the block whose keyword starts at `openerOffset`
// (`match` in `match x do`) begins, when it is the first thing on its line;
// nothing when the block is unclosed or its `end` shares a line.
auto closingEndOf(const std::string& text, size_t openerOffset) -> std::optional<size_t>;

// `arms` as source a code action inserts: one line each at `indent`, each
// body a `die` that names its case, so the program checks until it is filled in.
auto armSource(const std::vector<Arm>& arms, const std::string& indent) -> std::string;

// The indentation of the line holding byte `offset`, and the file's step in:
// the one most blocks in it are indented by, two spaces when none is.
auto lineIndentation(const std::string& text, size_t offset) -> std::string;
auto indentStep(const std::string& text) -> std::string;

} // namespace kex::lsp
