#pragma once
// A REPL input checked as the very statements that run. A REPL runs each
// input as a program of its own but checks it against the whole session (its
// definitions and earlier bindings). Re-parsing the input into that session
// program gave the checker other nodes than the ones the evaluator runs, so a
// checked program keyed by node (hir::Program) could not answer for them.
// Here the input's `main` statements are lent to the session program, checked
// there, and given back: nodes do not move, so the HIR answers for them.
#include "hir.hxx"
#include "../semantic/analyzer.hxx"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kex::hir {

struct CheckedInput {
    // The session program the input was checked in; the lent statements are
    // back in the input, but the HIR keeps pointing into both.
    std::unique_ptr<ast::Program> session;
    // The statements checked after the input's, kept alive: the HIR is keyed
    // by node address, and a freed node's address may be reused.
    std::vector<ast::ExprPtr> after;
    std::unique_ptr<semantic::Analyzer> analysis;
    // Null when the check found an error.
    std::unique_ptr<Program> program;
    // The first error the check found.
    std::optional<std::string> error;
};

// Checks `input` in `session` (the session's definitions and bindings,
// already parsed and expanded by the caller): its definitions beside the
// session's, its `main` statements in the session's `main`, with `after`
// checked behind them. `prepare` runs on the session program before
// the analysis: a REPL that reads modules from disk merges them there.
auto checkReplInput(std::unique_ptr<ast::Program> session, ast::Program& input,
                    std::vector<ast::ExprPtr> after,
                    const std::function<void(ast::Program&)>& prepare,
                    const semantic::ImportedInterfaces* interfaces)
    -> CheckedInput;

} // namespace kex::hir
