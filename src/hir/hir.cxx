#include "hir.hxx"

#include "../ast/walk.hxx"
#include "../semantic/analyzer.hxx"

namespace kex::hir {

namespace {

auto kindOf(semantic::CallKind kind) -> TargetKind {
    using Kind = semantic::CallKind;
    switch (kind) {
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
    case Kind::OwnMethod: return TargetKind::OwnMethod;
    case Kind::ModuleFunction: return TargetKind::ModuleFunction;
    }
    return TargetKind::Undecided;
}

// The call node inside `expr` — the MethodCall, FunctionCall or BinaryOp a
// pass holding only the node looks a target up by — or null.
auto callNodeOf(const ast::Expr& expr) -> const void* {
    if (const auto* call = std::get_if<ast::MethodCall>(&expr.kind)) return call;
    if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind)) return call;
    if (const auto* op = std::get_if<ast::BinaryOp>(&expr.kind)) return op;
    return nullptr;
}

const Target kUndecided{};

} // namespace

Program::Program(const ast::Program& ast, const semantic::Analyzer& analysis)
    : m_ast(&ast), m_analysis(&analysis), m_types(&analysis.typeMap()),
      m_typeOfAnswers(&analysis.staticTypeOfCalls()),
      m_selected(&analysis.selectedCallSignatures()) {
    // An imported target names the function; what else the checker decided
    // fills in the rest. Keyed by node and by expression alike.
    const auto record = [&](const ast::Expr& expr, Target target) {
        if (const auto* node = callNodeOf(expr)) m_targets.emplace(node, target);
        m_targets.emplace(&expr, std::move(target));
    };
    for (const auto& [expr, decided] : analysis.decidedCalls())
        record(*expr, {kindOf(decided.kind), {}, {}, 0, decided.owner});
    for (const auto& [expr, signature] : analysis.selectedCallSignatures()) {
        (void)signature;
        if (!m_targets.contains(expr)) record(*expr, {TargetKind::Selected});
    }
    // An imported target names the function. A call can also carry a more
    // specific decision (`Process.spawn`: a Builtin whose owner is the
    // state's type); that kind and owner stay, with the function added.
    for (const auto& [call, resolved] : analysis.resolvedCalls()) {
        auto& target = m_targets[call];
        if (target.kind == TargetKind::Undecided ||
            target.kind == TargetKind::Selected)
            target.kind = TargetKind::Imported;
        target.module = resolved.backendModule;
        target.function = resolved.backendFunction;
        target.arity = resolved.backendArity;
        target.servingCast = resolved.isServingCast;
    }
    collectSites();
}

Program::Program(const ast::Program& ast,
                 const std::unordered_map<const ast::Expr*, Target>& targets,
                 std::unique_ptr<Answers> answers)
    : m_ast(&ast), m_owned(std::move(answers)) {
    m_types = &m_owned->types;
    m_typeOfAnswers = &m_owned->typeOfAnswers;
    m_selected = &m_owned->selected;
    for (const auto& [expr, target] : targets) {
        if (const auto* node = callNodeOf(*expr)) m_targets.emplace(node, target);
        m_targets.emplace(expr, target);
    }
    collectSites();
}

auto Program::collectSites() -> void {
    const auto& ast = *m_ast;
    // Every call site, in source order.
    const auto visit = [&](const auto& self, const ast::Expr& expr) -> void {
        std::string callee;
        if (const auto* call = std::get_if<ast::MethodCall>(&expr.kind))
            callee = call->method;
        else if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind))
            callee = call->name;
        if (!callee.empty()) {
            const auto& target = targetOf(expr);
            std::string shown = target.owner;
            if (!target.function.empty())
                shown = (target.module.empty() ? std::string("(local)") : target.module) +
                        ":" + target.function + "/" + std::to_string(target.arity);
            m_sites.push_back({&expr, std::move(callee), target.kind, std::move(shown)});
        }
        ast::forEachChild(expr, [&](const ast::ExprPtr& child) {
            if (child) self(self, *child);
        });
    };
    ast::forEachBodyExpr(ast, [&](const ast::Expr& expr) { visit(visit, expr); });
}

auto Program::targetOf(const ast::Expr& call) const -> const Target& {
    if (const auto* node = callNodeOf(call))
        if (const auto found = m_targets.find(node); found != m_targets.end())
            return found->second;
    const auto found = m_targets.find(&call);
    return found == m_targets.end() ? kUndecided : found->second;
}

auto Program::targetOf(const ast::MethodCall& call) const -> const Target& {
    const auto found = m_targets.find(&call);
    return found == m_targets.end() ? kUndecided : found->second;
}

auto Program::targetOf(const ast::BinaryOp& op) const -> const Target& {
    const auto found = m_targets.find(&op);
    return found == m_targets.end() ? kUndecided : found->second;
}

auto Program::receiverTypeOf(const ast::Expr& call) const -> std::string {
    const auto& target = targetOf(call);
    if (target.kind == TargetKind::OwnMethod)
        return target.owner.substr(0, target.owner.find('<'));
    const auto signature = m_selected->find(&call);
    if (signature == m_selected->end() || signature->second.params.empty())
        return {};
    const auto* named =
        std::get_if<semantic::NamedType>(&signature->second.params.front()->kind);
    if (!named) return {};
    // Only where the call's receiver is of that type too: a signature
    // selected for a receiver that is not (a generic `"${x}"` choosing
    // Binary's `showValue` for a String) names no type the call is on.
    const auto* method = std::get_if<ast::MethodCall>(&call.kind);
    const auto receiver = method && method->receiver ? typeOf(*method->receiver) : nullptr;
    const auto* receiverNamed =
        receiver ? std::get_if<semantic::NamedType>(&receiver->kind) : nullptr;
    // One type, however its module spells it (`Shared` and `Process.Shared`):
    // by its last segment, as receiverCoversHead compares heads.
    const auto last = [](const std::string& name) {
        return std::string_view(name).substr(name.rfind('.') + 1);
    };
    return receiverNamed && last(receiverNamed->name) == last(named->name)
        ? named->name
        : std::string{};
}

auto Program::typeOfAnswer(const ast::MethodCall& call) const
    -> const semantic::StaticTypeAnswer* {
    const auto found = m_typeOfAnswers->find(&call);
    return found == m_typeOfAnswers->end() ? nullptr : &found->second;
}

auto Program::selectedSignatureOf(const ast::Expr& call) const
    -> const semantic::Signature* {
    const auto found = m_selected->find(&call);
    return found == m_selected->end() ? nullptr : &found->second;
}

namespace {

// The first program's answer that `answered` accepts, newest first.
template <typename Answer, typename Ask, typename Answered>
auto newestAnswer(const std::vector<const Program*>& programs, const Program* base,
                  Answer none, Ask ask, Answered answered) -> Answer {
    for (auto program = programs.rbegin(); program != programs.rend(); ++program)
        if (auto answer = ask(**program); answered(answer)) return answer;
    if (base)
        if (auto answer = ask(*base); answered(answer)) return answer;
    return none;
}

} // namespace

auto ProgramSet::targetOf(const ast::MethodCall& call) const -> const Target& {
    return *newestAnswer<const Target*>(
        m_programs, m_base, &kUndecided, [&](const Program& p) { return &p.targetOf(call); },
        [](const Target* t) { return t->kind != TargetKind::Undecided; });
}

auto ProgramSet::targetOf(const ast::BinaryOp& op) const -> const Target& {
    return *newestAnswer<const Target*>(
        m_programs, m_base, &kUndecided, [&](const Program& p) { return &p.targetOf(op); },
        [](const Target* t) { return t->kind != TargetKind::Undecided; });
}

auto ProgramSet::typeOf(const ast::Expr& expr) const -> semantic::TypePtr {
    return newestAnswer<semantic::TypePtr>(
        m_programs, m_base, nullptr, [&](const Program& p) { return p.typeOf(expr); },
        [](const semantic::TypePtr& t) { return t != nullptr; });
}

auto ProgramSet::receiverTypeOf(const ast::Expr& call) const -> std::string {
    return newestAnswer<std::string>(
        m_programs, m_base, {}, [&](const Program& p) { return p.receiverTypeOf(call); },
        [](const std::string& t) { return !t.empty(); });
}

auto ProgramSet::typeOfAnswer(const ast::MethodCall& call) const
    -> const semantic::StaticTypeAnswer* {
    return newestAnswer<const semantic::StaticTypeAnswer*>(
        m_programs, m_base, nullptr, [&](const Program& p) { return p.typeOfAnswer(call); },
        [](const auto* a) { return a != nullptr; });
}

auto ProgramSet::selectedSignatureOf(const ast::Expr& call) const
    -> const semantic::Signature* {
    return newestAnswer<const semantic::Signature*>(
        m_programs, m_base, nullptr,
        [&](const Program& p) { return p.selectedSignatureOf(call); },
        [](const auto* s) { return s != nullptr; });
}

auto Program::typeOf(const ast::Expr& expr) const -> semantic::TypePtr {
    const auto found = m_types->find(&expr);
    return found == m_types->end() ? nullptr : found->second;
}

auto verify(const Program& program) -> std::vector<std::string> {
    std::vector<std::string> problems;
    for (const auto& site : program.callSites())
        if (site.kind == TargetKind::Undecided)
            problems.push_back(std::string(site.expr->location.file) + ":" +
                               std::to_string(site.expr->location.line) + ":" +
                               std::to_string(site.expr->location.column) +
                               ": no target for `" + site.callee + "`");
    return problems;
}

} // namespace kex::hir
