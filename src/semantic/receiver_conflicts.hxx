#pragma once

#include "../ast/ast.hxx"
#include "analyzer.hxx"

#include <vector>

namespace kex::semantic {

// Two definitions of the same method for the SAME receiver type are an
// equally-specific tie, which docs/ufcs-dispatch-plan.md "Decisions (locked)"
// makes a compile error naming both candidates. Nothing else can: declaration
// form carries no dispatch priority, so there is no rule that picks a winner —
// and each backend was picking a different one.
//
//   module A do make String do let shout -> String = "A:" + this end end
//   module B do make String do let shout -> String = "B:" + this end end
//   "x".shout    # walker: "A:x" (first declaration, silently). BEAM: undef.
//
//   tag : String -> String
//   let tag(s) = "free/String"
//   make String do let tag -> String = "make/String" end
//   "x".tag      # walker: "make/String". BEAM: "free/String".
//
// Clauses of ONE method are not a conflict: several `let combine(@Equal, …)` /
// `let combine(@Less, …)` in a single `make` block are one definition, and so
// are arity overloads, which dispatch resolves on argument count.
//
// Only definitions in the SAME FILE are compared. Dependency modules are merged
// into the program on the `-c`/`-R` paths and absent on `-C`/run, so widening
// this would make the two disagree about which programs compile.
auto findReceiverConflicts(const ast::Program& program)
    -> std::vector<Diagnostic>;

// A `make` method and a plain function in the same scope that land on ONE
// BEAM symbol. A method reaches its receiver as an extra argument, so
// `make Env do let formFields = ... end` emits `formFields/1` — exactly what a
// module-level `let formFields(text)` emits, private or not. erlc used to
// report this as an internal crash in `beam_ssa_throw`
// (`{key_exists,{b_local,{b_literal,formFields},1}}`) with nothing naming
// either declaration.
//
// Same-receiver ties are findReceiverConflicts' business and are left to it.
auto findMethodFunctionCollisions(const ast::Program& program)
    -> std::vector<Diagnostic>;

} // namespace kex::semantic
