#pragma once
// The checked program — Stage 2's HIR. Built once from a parsed program and
// the type checker's analysis of it, it answers the two questions every later
// pass asks: what each call is (`targetOf`) and what type each expression has
// (`typeOf`). Lowering, `--dump-hir` and `--verify-hir` read it; the walker
// and the language server are to follow. It indexes the AST rather than
// copying it, so building it is one pass over the checker's tables.
#include "call_targets.hxx"
#include "../ast/ast.hxx"
#include "../semantic/types.hxx"

#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {
class Analyzer;
struct Signature;
}

namespace kex::hir {

// What a call is, as semantic analysis decided it.
struct Target {
    TargetKind kind = TargetKind::Undecided;
    // An imported target's backend module ("" for this unit's own) and
    // function, and the arity it is called at.
    std::string module;
    std::string function;
    int arity = 0;
    // The decided owner: the record a field read reads, the ADT a constructor
    // builds, the type whose operator applies, a builtin's subject type, ...
    std::string owner;
    // A `serving` slot called with no reply to wait for: a cast.
    bool servingCast = false;
};

class Program {
public:
    Program(const ast::Program& ast, const semantic::Analyzer& analysis);

    auto ast() const -> const ast::Program& { return *m_ast; }
    auto analysis() const -> const semantic::Analyzer& { return *m_analysis; }

    // What `call` is; a Target of kind Undecided when nothing decided it.
    auto targetOf(const ast::Expr& call) const -> const Target&;
    auto targetOf(const ast::MethodCall& call) const -> const Target&;
    auto targetOf(const ast::BinaryOp& op) const -> const Target&;
    // The nominal type a method call was decided on, without type arguments:
    // an OwnMethod's owner, else the receiver of the signature the checker
    // selected (`Process.Shared` for `site.get`); "" when neither names one.
    auto receiverTypeOf(const ast::Expr& call) const -> std::string;
    // The static type the checker gave `expr`, or null.
    auto typeOf(const ast::Expr& expr) const -> semantic::TypePtr;
    // The checker's compile-time answer to `Type.of(x)` at `call`, or null.
    auto typeOfAnswer(const ast::MethodCall& call) const
        -> const semantic::StaticTypeAnswer*;
    // The signature the checker selected for `call`, or null.
    auto selectedSignatureOf(const ast::Expr& call) const -> const semantic::Signature*;

    // Every call site, in source order, with its target.
    auto callSites() const -> const std::vector<CallSite>& { return m_sites; }

private:
    const ast::Program* m_ast;
    const semantic::Analyzer* m_analysis;
    std::vector<CallSite> m_sites;
    // Keyed by the call node (the Expr, and the MethodCall / BinaryOp inside
    // it), so a pass holding either finds the same target.
    std::unordered_map<const void*, Target> m_targets;
};

// Several checked programs read as one, newest first: a REPL session's
// inputs, each checked on its own, whose definitions later inputs call.
class ProgramSet {
public:
    auto clear() -> void { m_programs.clear(); }
    // `program` answers first from now on.
    auto push(const Program* program) -> void {
        if (program) m_programs.push_back(program);
    }
    auto empty() const -> bool { return m_programs.empty(); }

    auto targetOf(const ast::Expr& call) const -> const Target&;
    auto targetOf(const ast::MethodCall& call) const -> const Target&;
    auto targetOf(const ast::BinaryOp& op) const -> const Target&;
    auto typeOf(const ast::Expr& expr) const -> semantic::TypePtr;
    auto receiverTypeOf(const ast::Expr& call) const -> std::string;
    auto typeOfAnswer(const ast::MethodCall& call) const
        -> const semantic::StaticTypeAnswer*;
    auto selectedSignatureOf(const ast::Expr& call) const -> const semantic::Signature*;

private:
    std::vector<const Program*> m_programs;
};

// What is not yet decided: one line per undecided call site (`--verify-hir`).
auto verify(const Program& program) -> std::vector<std::string>;

} // namespace kex::hir
