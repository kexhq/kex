#pragma once

#include "ast.hxx"

#include <map>
#include <string>
#include <vector>

// The one rule for which same-name declarations are a single function
// (kexhq/kex#262). Kex writes a multi-clause function as several `let`s:
//
//   let repeat(0) = this.identity
//   let repeat(n: Integer) -> This do ... end
//
// `groupFunctionClauses` folds those into ONE FunctionDef whose `clauses`
// hold them in source order, right after parsing, so every later phase sees
// a function as one node instead of re-deriving the grouping on its own.
namespace kex::ast {

// How a parameter's declared type reads for dispatch: the last segment of a
// type name, generics and containers spelled out, `Any` where nothing is
// declared.
auto renderDispatchType(const TypeExpr& type) -> std::string;

// The dispatch types of a make-block method's clause: the receiver type,
// then one per parameter.
auto methodDispatchTypes(const FunctionClause& clause,
                         const std::string& receiverType)
    -> std::vector<std::string>;
auto methodDispatchTypes(const FunctionDef& function,
                         const std::string& receiverType)
    -> std::vector<std::string>;

// Whether several same-name method signatures actually SEPARATE the values
// they accept, which is what makes them an argument-type overload set (one
// BEAM function each) rather than ordinary clause dispatch.
auto separableOverloadSignatures(
    const std::map<std::string, std::vector<std::string>>& signatures) -> bool;

// A make-block method's BEAM arity: its receiver occupies position 0, either
// implicitly as `this` or explicitly as a receiver pattern.
auto methodArity(const FunctionClause& clause) -> size_t;
auto methodArity(const FunctionDef& function) -> size_t;

// A free function's arity: its parameters, with no receiver slot.
auto freeFunctionArity(const FunctionDef& function) -> size_t;

// The clauses of a function that each declare a signature of their own: the
// first, and every later one whose written parameter and result types differ
// from all before it. An untyped multi-clause function has one; typed
// overloads folded into one function (`close(connection: TCPConnection)`,
// `close(listener: TCPListener)`) have one each. Whatever describes a
// function's contract — an interface, an export, a published signature —
// reads these rather than only the first clause.
auto signatureClauses(const FunctionDef& function)
    -> std::vector<const FunctionClause*>;

// A function's parameter names by position, for placing named arguments:
// each position takes the first name any clause gives it, since a clause may
// match one position with a pattern (`to(measure, String, @Kilo)`) where a
// sibling names it (`to(measure, String, in: SIPrefix)`).
auto parameterNames(const FunctionDef& function) -> std::vector<std::string>;

// Each position's default value, from the first clause that gives one.
auto parameterDefaults(const FunctionDef& function)
    -> std::vector<const ExprPtr*>;

// Folds adjacent clauses of the same function into one FunctionDef, in
// every container of the program. Idempotent, so it is safe to run again
// after compile-time expansion has spliced in generated declarations.
auto groupFunctionClauses(Program& program) -> void;

} // namespace kex::ast
