#pragma once
// Compile-time intrinsics: calls the type checker answers itself, because
// what they mean is known only from the checked program (`Type.of(x)` is
// the STATIC type of `x`; `Type.returnedBy(f)` reads a signature a function
// value does not carry at run time). Each is one entry here — its handlers
// are TypeChecker members — and the checker dispatches through this table
// at a single point, rather than testing the callee's name in place. What
// a fold records (staticTypeOfCalls) is all either backend sees of it.
#include "types.hxx"

#include <cstddef>
#include <string_view>
#include <vector>

namespace kex::ast {
struct Expr;
struct MethodCall;
} // namespace kex::ast

namespace kex::semantic {

class TypeChecker;

struct CompileTimeIntrinsic {
    // As the checker names the call: `Type::returnedBy`.
    std::string_view name;
    // How many arguments the source passes.
    std::size_t arity;
    // Types the whole call, arguments included, when they are not ordinary
    // values (a function's NAME). Returns null to leave the call to its stdlib
    // signature (and `fold`).
    TypePtr (TypeChecker::*check)(const ast::Expr& expr,
                                  const ast::MethodCall& call) = nullptr;
    // Once the arguments are typed: records the answer the call folds to.
    void (TypeChecker::*fold)(const ast::MethodCall& call,
                              const std::vector<TypePtr>& argTypes) = nullptr;
};

// The intrinsic a call named `name` with `arity` arguments is, if any.
auto findCompileTimeIntrinsic(std::string_view name, std::size_t arity)
    -> const CompileTimeIntrinsic*;

} // namespace kex::semantic
