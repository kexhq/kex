#pragma once

#include "../ast/ast.hxx"

namespace kex::semantic {

struct Diagnostic;
struct ImportedInterfaces;
class TraitRegistry;

// Validates type spellings in declaration ASTs after compiled expansion and
// before inference. Kept separate because name existence is a declaration
// invariant; resolveTypeExpr deliberately remains gradual for expressions.
auto validateDeclarations(const ast::Program& program,
                          const ImportedInterfaces* imported,
                          const TraitRegistry& traits,
                          std::vector<Diagnostic>& diagnostics) -> void;

} // namespace kex::semantic
