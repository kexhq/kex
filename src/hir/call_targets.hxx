#pragma once
// Stage 2 of the pass restructure: what semantic analysis decided about each
// call site. The checker records an answer for some calls only — imported
// method calls get a full ResolvedCallTarget, overloaded ones a selected
// Signature — and lowering and the walker re-derive the rest, three times
// over. This lists every call site with what (if anything) was decided, so
// the gap is measured; HIR call nodes carry the same Target once sema decides
// every one.
#include "../ast/ast.hxx"

#include <string>
#include <string_view>
#include <vector>

namespace kex::semantic {
class Analyzer;
}

namespace kex::hir {

enum class TargetKind {
    // A ResolvedCallTarget: backend module, function, arity, placed
    // parameters, trait dictionaries — everything lowering needs.
    Imported,
    // The checker chose a signature, but not the function that has it.
    Selected,
    // A variant built by its constructor (`Just(x)`, `Shapes.Circle(r)`).
    Constructor,
    // A record's field read (`point.x`).
    FieldRead,
    // A module named as a value or a path segment (`FS.File`).
    ModuleReference,
    // A method a trait-typed receiver has through the trait.
    TraitMethod,
    // A function value bound in scope, called (`f(x)`, `block(x)`).
    ValueCall,
    // Looked up on the receiver at run time: its type is not known.
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
    // Nothing recorded: lowering and the walker resolve it themselves.
    Undecided,
};

auto targetKindName(TargetKind kind) -> std::string_view;

struct CallSite {
    const ast::Expr* expr = nullptr;
    std::string callee;
    TargetKind kind = TargetKind::Undecided;
};

inline constexpr TargetKind kTargetKinds[] = {
    TargetKind::Imported,  TargetKind::Selected,        TargetKind::Constructor,
    TargetKind::FieldRead, TargetKind::ModuleReference, TargetKind::TraitMethod,
    TargetKind::ValueCall, TargetKind::Dispatch,
    TargetKind::Conversion, TargetKind::TypeReference,
    TargetKind::Builtin, TargetKind::Operator, TargetKind::Undecided,
};

// Every function and method call in `program`'s function bodies and main
// blocks, in source order, with what `analysis` decided about it.
auto collectCallSites(const ast::Program& program,
                      const semantic::Analyzer& analysis)
    -> std::vector<CallSite>;

} // namespace kex::hir
