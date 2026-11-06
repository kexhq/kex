#pragma once
// What the type checker decided a call-shaped expression is, when that is
// neither a call with a selected signature nor an imported method (those
// have tables of their own). Stage 2 of the pass restructure records every
// such decision, so no later pass resolves the call again (src/hir/); kept
// apart from typechecker.hxx so lowering can read it without the checker.
#include <string>

namespace kex::semantic {

enum class CallKind {
    Constructor,
    FieldRead,
    ModuleReference,
    // A method a trait-typed receiver has through that trait.
    TraitMethod,
    // A function value bound in scope (`f(x)`, a block parameter).
    ValueCall,
    // A method whose receiver's type is not known: found on the value
    // when the call runs.
    Dispatch,
    // `x.to(T)` / `x.as(T)`: a conversion to the named type.
    Conversion,
    // A record or sum type named as a namespace (`TCP.Endpoint`).
    TypeReference,
    // A runtime primitive the checker handles itself (`pid.send(msg)`).
    Builtin,
    // A binary operator a type overloads (`a + b` on a `make` block's `+`):
    // owner is the left operand's type, as the checker printed it.
    Operator,
    // A method the receiver's own type answers — its `make` block's, or a
    // trait default it inherits: owner is the type.
    OwnMethod,
    // A function of the receiver type's home module, called by UFCS
    // (`conn.close` is `TCP.close(conn)`): owner is its qualified name.
    ModuleFunction,
    // Answered only by a `make` for a type declared elsewhere, in a module
    // not in scope here (no `using` names it): owner is that module. The
    // call is an undefined method wherever it runs.
    Hidden,
    // A function of the backend, called through `BEAM.` (`BEAM.lists.reverse`):
    // owner is its path.
    Foreign,
    // A compile-time intrinsic folded to its answer (`Type.of(x)`): owner is
    // its name, as the checker spells it (`Type::of`).
    Intrinsic,
    // A function NAMED, not called (`Type.of(Date.parse)`): owner is its
    // qualified name.
    FunctionReference,
};

struct DecidedCall {
    CallKind kind;
    // The ADT a constructor builds, the record whose field is read, the
    // module referenced, the trait dispatched through, the binding called,
    // or the conversion's target type.
    std::string owner;
};

} // namespace kex::semantic
