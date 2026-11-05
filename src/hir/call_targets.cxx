#include "call_targets.hxx"

#include "../ast/walk.hxx"
#include "../semantic/analyzer.hxx"

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
    case TargetKind::Undecided: return "undecided";
    }
    return "undecided";
}

namespace {

auto classify(const ast::Expr& expr, const semantic::Analyzer& analysis)
    -> TargetKind {
    if (const auto* call = std::get_if<ast::MethodCall>(&expr.kind);
        call && analysis.resolvedCalls().contains(call))
        return TargetKind::Imported;
    if (analysis.selectedCallSignatures().contains(&expr))
        return TargetKind::Selected;
    if (const auto decided = analysis.decidedCalls().find(&expr);
        decided != analysis.decidedCalls().end()) {
        using Kind = semantic::CallKind;
        switch (decided->second.kind) {
        case Kind::Constructor: return TargetKind::Constructor;
        case Kind::FieldRead: return TargetKind::FieldRead;
        case Kind::ModuleReference: return TargetKind::ModuleReference;
        case Kind::TraitMethod: return TargetKind::TraitMethod;
        case Kind::ValueCall: return TargetKind::ValueCall;
        case Kind::Dispatch: return TargetKind::Dispatch;
        case Kind::Conversion: return TargetKind::Conversion;
        case Kind::TypeReference: return TargetKind::TypeReference;
        case Kind::Builtin: return TargetKind::Builtin;
        case Kind::Operator: return TargetKind::Operator;
        }
    }
    return TargetKind::Undecided;
}

auto collectCallSitesIn(const ast::Expr& expr, const semantic::Analyzer& analysis,
             std::vector<CallSite>& sites) -> void {
    if (const auto* call = std::get_if<ast::MethodCall>(&expr.kind))
        sites.push_back({&expr, call->method, classify(expr, analysis)});
    else if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind))
        sites.push_back({&expr, call->name, classify(expr, analysis)});
    ast::forEachChild(expr, [&](const ast::ExprPtr& child) {
        collectCallSitesIn(*child, analysis, sites);
    });
}

} // namespace

auto collectCallSites(const ast::Program& program,
                      const semantic::Analyzer& analysis)
    -> std::vector<CallSite> {
    std::vector<CallSite> sites;
    ast::forEachBodyExpr(program, [&](const ast::Expr& expr) {
        collectCallSitesIn(expr, analysis, sites);
    });
    return sites;
}

} // namespace kex::hir
