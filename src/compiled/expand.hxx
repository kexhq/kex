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

struct ExpandOptions {
    // Wall-clock budget for all compile-time evaluation in one program. The
    // backstop for pathological cases; the step limit is what gives
    // reproducible errors across machines.
    std::chrono::milliseconds timeout{2000};
    std::size_t stepLimit = 5'000'000;
    // Report errors inside a template's own body as well as at the use site.
    bool verbose = false;
};

// Expands every `compiled` block in `program` in place.
//
// Returns false and appends to `diagnostics` if expansion failed; the program
// is left in an unspecified state in that case and must not be compiled.
// Diagnostics are anchored at the USER's source location, not inside whatever
// template generated the code.
auto expand(ast::Program& program,
            std::vector<semantic::Diagnostic>& diagnostics,
            const ExpandOptions& options = {}) -> bool;

} // namespace kex::compiled
