#pragma once

#include "ast.hxx"

#include <optional>

// Deep copy for AST subtrees.
//
// The AST is built from unique_ptr, so it is move-only by construction — which
// is right for parsing (one source span, one node) but not for compile-time
// metaprogramming, where ONE template declaration produces N independent
// copies. `["a","b"].each do |n| let %n(...) ... end` needs a separate body per
// iteration: they get different spliced names and different compile-time values
// substituted into them, so they cannot share nodes.
//
// A cloned node keeps its original SourceLocation. Callers that want a
// generated node to blame a different place must overwrite it themselves —
// see the generation trail described in docs/compiled-meta-plan.md.
namespace kex::ast {

auto clone(const ExprPtr& expr) -> ExprPtr;
auto clone(const PatternPtr& pattern) -> PatternPtr;
auto clone(const TypeExprPtr& type) -> TypeExprPtr;
auto clone(const Param& param) -> Param;
auto clone(const FunctionClause& clause) -> FunctionClause;
auto clone(const MatchClause& clause) -> MatchClause;
auto clone(const RescueBlock& rescue) -> RescueBlock;
auto clone(const FunctionDef& function) -> std::unique_ptr<FunctionDef>;
auto clone(const AbstractFunction& fn) -> AbstractFunction;
auto clone(const TypeDef& type) -> std::unique_ptr<TypeDef>;
auto clone(const RecordDef& record) -> std::unique_ptr<RecordDef>;
// A `make` block, methods and all. Needed because `make %name` generation
// instantiates one template into N independent blocks, each with its own bodies
// for hygiene substitution to rewrite.
auto clone(const MakeDef& make) -> std::unique_ptr<MakeDef>;
auto clone(const TypeAnnotation& annotation) -> std::unique_ptr<TypeAnnotation>;
auto clone(const UsingBlock& block) -> std::unique_ptr<UsingBlock>;
auto clone(const TraitDef& trait) -> std::unique_ptr<TraitDef>;
auto clone(const ExportDecl& decl) -> std::unique_ptr<ExportDecl>;
auto clone(const MainBlock& main) -> std::unique_ptr<MainBlock>;
auto clone(const Pragma& pragma) -> std::unique_ptr<Pragma>;
// Null when the block (or a module inside it) still holds a CompiledBlock:
// unexpanded compile-time code has no copy.
auto clone(const VisibilityBlock& block) -> std::unique_ptr<VisibilityBlock>;
auto clone(const ModuleDef& module) -> std::unique_ptr<ModuleDef>;
// A whole program, or nullopt if any CompiledBlock remains in it.
auto clone(const Program& program) -> std::optional<Program>;

} // namespace kex::ast
