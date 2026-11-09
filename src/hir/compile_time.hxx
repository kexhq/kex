#pragma once

// Compile-time code, checked before it runs (docs/pass-restructure-plan.md,
// Stage 4): the sandbox that evaluates a `compiled do` block reads the
// checker's answers as every other walker run does.

#include "hir.hxx"
#include "../semantic/analyzer.hxx"

#include <memory>
#include <optional>

namespace kex::hir {

// A program checked for the walker that runs it: the answers, keyed by the
// program's own nodes, and the analysis they were read from (none when they
// were read back from the cache).
struct CheckedProgram {
    std::unique_ptr<semantic::Analyzer> analyzer;
    std::unique_ptr<Program> program;
};

// Checks `program` silently, its `compiled do` blocks lent to it for the
// check as the items they read as and given back afterwards: a top-level
// block's declarations as top-level declarations and each statement as a
// top-level statement; a module's block's declarations as the module's, and
// its statements, in order, as one function body of the module. The
// diagnostics are dropped: before generation, code outside the blocks may
// call what they have yet to generate. Expansion checks before running a
// block and before collapsing, which runs what generation produced.
auto checkCompileTime(ast::Program& program, const semantic::ImportedInterfaces* interfaces)
    -> std::unique_ptr<CheckedProgram>;

// Checks `program` silently as it stands, for a walker given a program no one
// checked (a test harness, an embedder): every program the walker runs is
// checked first.
auto checkSilently(const ast::Program& program, const semantic::ImportedInterfaces* interfaces)
    -> std::unique_ptr<CheckedProgram>;

// `program`'s check, as checkSilently gives it, read back from the cache when
// this toolchain checked the same program before: `key` names everything the
// program was made from (its source, the files its expansion read). A module
// the walker loads is checked once, not by every build that loads it.
auto cachedCheck(const ast::Program& program, const semantic::ImportedInterfaces* interfaces,
                 const std::string& key) -> std::unique_ptr<CheckedProgram>;

} // namespace kex::hir
