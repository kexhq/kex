#pragma once

#include "../ast/ast.hxx"
#include "../semantic/analyzer.hxx"
#include <chrono>
#include <cstddef>
#include <vector>

// Compile-time expansion of `compiled do ... end` blocks.
//
// This runs as an AST->AST pass BEFORE semantic analysis: it replaces
// CompiledBlock nodes with the ordinary declarations they generate, so
// everything downstream — SemanticDB, the type checker, the tree-walk
// interpreter, IR lowering, the Core Erlang emitter, --summary — sees only
// plain FunctionDef/TypeDef/RecordDef/MakeDef and needs no changes. It is also
// what keeps the interpreter and the BEAM backend from drifting: neither knows
// the feature exists.
//
// Deliberately NOT under src/semantic/: this pass MUTATES the AST, which the
// semantic passes are careful never to do.
//
// Currently an identity pass — `compiled` blocks are still unwrapped verbatim
// by both backends, exactly as before. See docs/compiled-meta-plan.md for the
// staged plan; this is the plumbing step that lets the real work land behind a
// stable interface.
namespace kex::compiled {

// What became of one expression chain collapse tried to fold away.
//
// The feature's entire value is invisible at runtime — a program that fails to
// collapse still prints the right answer — so without a report the only way to
// know is to decode the emitted Core Erlang. This is that report.
struct CollapseNote {
    SourceLocation location;
    bool collapsed = false;
    // Why not, when it did not: "the value of `userId` is not known at compile
    // time, and converting it to text needs it". Empty when it collapsed.
    std::string reason;
};

struct ExpandOptions {
    // Wall-clock budget for all compile-time evaluation in one program. The
    // backstop for pathological cases; the step limit is what gives
    // reproducible errors across machines.
    std::chrono::milliseconds timeout{2000};
    std::size_t stepLimit = 5'000'000;
    // Report errors inside a template's own body as well as at the use site.
    bool verbose = false;
    // When non-null, receives one note per collapse attempt. Costs nothing to
    // leave null, which every path but `--collapse-report` does.
    std::vector<CollapseNote>* report = nullptr;
    // The path of the file being compiled. `Kex.embed("relative/path")`
    // (kexhq/kex#171) resolves against this file's directory, the way
    // `Kex.embed` is Rust's `include_str!`: the path is relative to the
    // source that wrote it, not to the process's working directory. Left
    // empty by a caller that has no file (a REPL line, an in-memory eval)
    // makes every `Kex.embed` there fail to resolve rather than guessing a
    // directory.
    std::string sourcePath;
};

// Expands every `compiled` block in `program` in place, and folds every
// `Kex.embed("path")` call into the literal contents of that file.
//
// The embed fold runs unconditionally — it is not gated behind the program
// having any `compiled do` block, since embedding a template's source has
// nothing to do with that feature; it just happens to share this pass
// because both are AST rewrites that must run before semantic analysis sees
// the program.
//
// Returns false and appends to `diagnostics` if expansion failed; the program
// is left in an unspecified state in that case and must not be compiled.
// Diagnostics are anchored at the USER's source location, not inside whatever
// template generated the code.
auto expand(ast::Program& program,
            std::vector<semantic::Diagnostic>& diagnostics,
            const ExpandOptions& options = {}) -> bool;

} // namespace kex::compiled
