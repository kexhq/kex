#pragma once

#include "../ast/ast.hxx"

namespace kex::semantic {

struct Diagnostic;
class TraitRegistry;

// Checks declarations for mistakes no type resolves: a `make` contract
// nothing defines, and a function named `new`. Whether a written type name
// exists is the checker's to answer, where it resolves the name.
auto validateDeclarations(const ast::Program& program, const TraitRegistry& traits,
                          std::vector<Diagnostic>& diagnostics) -> void;

} // namespace kex::semantic
