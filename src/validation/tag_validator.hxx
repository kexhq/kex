#pragma once

#include "../ast/ast.hxx"
#include "../semantic/analyzer.hxx"
#include <chrono>
#include <vector>

namespace kex::validation {

// Runs optional validateTag companions for raw tagged literals. Semantic
// analysis must already have completed so resolved function signatures and
// purity diagnostics are available.
auto validateTaggedLiterals(
    const ast::Program& program,
    const semantic::Analyzer& analyzer,
    std::chrono::milliseconds timeout = std::chrono::seconds(1))
    -> std::vector<semantic::Diagnostic>;

} // namespace kex::validation
