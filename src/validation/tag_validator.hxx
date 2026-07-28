#pragma once

#include "../ast/ast.hxx"
#include "../semantic/analyzer.hxx"
#include <chrono>
#include <vector>

namespace kex::validation {

// Runs optional validateTag companions for raw tagged literals. Semantic
// analysis must already have completed so resolved function signatures and
// purity diagnostics are available.
// `moduleRoots` lets a tag defined in a `using`-imported module be validated
// too: without it only tags defined in `program` itself are checked, so an
// opt-in module's tag (`regex`, and any third-party `sql`/`html`) silently
// skips validation. Pass the same roots the compiler resolves imports with.
auto validateTaggedLiterals(
    const ast::Program& program,
    const semantic::Analyzer& analyzer,
    const std::vector<std::string>& moduleRoots = {},
    std::chrono::milliseconds timeout = std::chrono::seconds(1))
    -> std::vector<semantic::Diagnostic>;

} // namespace kex::validation
