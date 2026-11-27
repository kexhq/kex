#pragma once

#include <cstddef>
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

} // namespace kex::lsp
