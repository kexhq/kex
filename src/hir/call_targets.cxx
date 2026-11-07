#include "call_targets.hxx"

namespace kex::hir {

auto targetKindName(TargetKind kind) -> std::string_view {
    switch (kind) {
    case TargetKind::Imported: return "imported";
    case TargetKind::Selected: return "selected";
    case TargetKind::Constructor: return "constructor";
    case TargetKind::FieldRead: return "field";
    case TargetKind::ModuleReference: return "module";
    case TargetKind::TraitMethod: return "trait";
    case TargetKind::ValueCall: return "value";
    case TargetKind::Dispatch: return "dispatch";
    case TargetKind::Conversion: return "conversion";
    case TargetKind::TypeReference: return "type";
    case TargetKind::Builtin: return "builtin";
    case TargetKind::Operator: return "operator";
    case TargetKind::OwnMethod: return "own";
    case TargetKind::ModuleFunction: return "module-function";
    case TargetKind::Hidden: return "hidden";
    case TargetKind::Foreign: return "foreign";
    case TargetKind::Intrinsic: return "intrinsic";
    case TargetKind::FunctionReference: return "function";
    case TargetKind::Undecided: return "undecided";
    }
    return "undecided";
}

} // namespace kex::hir
