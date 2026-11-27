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
auto snippetCompletions(const std::string& text, size_t cursor) -> std::vector<Snippet>;

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

} // namespace kex::lsp
