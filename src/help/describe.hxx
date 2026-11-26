#pragma once

// What the REPL's `/type <expr>` and `/help <name>` say about an input,
// answered from the checker's analysis of the session program the input was
// checked in. Every REPL — the BEAM one, the walker's, the browser's — builds
// that program its own way and asks this, so all of them answer alike.

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace kex::ast {
struct Expr;
}
namespace kex::semantic {
class Analyzer;
}

namespace kex::help {

struct Description {
    // `/type`'s answer, a line each: `name : signature` for every clause of a
    // function the input names, `name : Type` for a binding, and the type
    // alone for any other expression. Empty when nothing is known.
    std::vector<std::string> typeLines;
    // Where the help index documents what the input resolved to, as
    // (owner, member); the member is empty for a module or type itself.
    std::vector<std::pair<std::string, std::string>> documented;
    // The input names a module or a type rather than a value.
    bool namesType = false;
    // The input names a binding of the session.
    bool binding = false;
    // The input names functions of the program or its imports.
    bool function = false;
};

// The input `expr`, the last statement of the program `analyzer` checked.
auto describe(const semantic::Analyzer& analyzer, const ast::Expr& expr) -> Description;

// The name an input spells, when it is only a name: `f`, `List`, `List.first`;
// "" for anything else.
auto writtenName(const ast::Expr& expr) -> std::string;

class ApiIndex;

// `/type <input>`: the type of `expr` (the input, checked by `analyzer`
// without being run) to `out`, or why it has none to `err`. False on an
// error.
auto printReplType(
    const std::string& input,
    const semantic::Analyzer& analyzer,
    const ast::Expr& expr,
    std::ostream& out,
    std::ostream& err) -> bool;

// `/help <input>`: `/type`'s answer, then what `indexes` (the session's own
// declarations first, then the standard library's) document for what the
// checker resolved the input to, in `kex -s`'s format. False when there is
// nothing to say.
auto printReplNameHelp(
    const std::string& input,
    const semantic::Analyzer& analyzer,
    const ast::Expr& expr,
    const std::vector<const ApiIndex*>& indexes,
    std::ostream& out,
    std::ostream& err) -> bool;

// The standard library's help index (ApiIndex::fromStandardLibrary), read
// the first time it is asked for.
auto standardLibraryIndex() -> const ApiIndex&;

} // namespace kex::help
