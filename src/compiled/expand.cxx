#include "expand.hxx"

namespace kex::compiled {

// Identity pass for now.
//
// The `compiled` blocks are left in place, which keeps today's behaviour
// exactly: src/interpreter/evaluator.cxx's execCompiledBlock and
// src/ir/lower.cxx both already unwrap them and register the contents as
// ordinary declarations. Once the sandbox and the reifier land, the walk over
// `program.items` below is where CompiledBlock nodes get replaced by their
// generated declarations instead.
auto expand(ast::Program& program,
            std::vector<semantic::Diagnostic>& diagnostics,
            const ExpandOptions& options) -> bool {
    (void)program;
    (void)diagnostics;
    (void)options;
    return true;
}

} // namespace kex::compiled
