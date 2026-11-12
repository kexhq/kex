#include "hir.hxx"

#include "../ast/walk.hxx"
#include "../semantic/analyzer.hxx"
#include "../common/well_known.hxx"

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
    case Kind::Hidden: return TargetKind::Hidden;
    case Kind::Foreign: return TargetKind::Foreign;
    case Kind::Intrinsic: return TargetKind::Intrinsic;
    case Kind::FunctionReference: return TargetKind::FunctionReference;
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
      m_selected(&analysis.selectedCallSignatures()),
      m_shownThrough(&analysis.shownThroughDistinct()),
      m_inheritedDefaults(&analysis.checker().inheritedDefaults()),
      m_stringPatterns(&analysis.checker().stringPatterns()),
      m_makeTargets(&analysis.checker().makeTargetIdentities()) {
    // An imported target names the function; what else the checker decided
    // fills in the rest. Keyed by node and by expression alike.
    const auto record = [&](const ast::Expr& expr, Target target) {
        if (const auto* node = callNodeOf(expr)) m_targets.emplace(node, target);
        m_targets.emplace(&expr, std::move(target));
    };
    for (const auto& [expr, decided] : analysis.decidedCalls())
        record(*expr, {kindOf(decided.kind), decided.backendModule, decided.function, 0,
                       decided.owner, false, decided.operands, decided.negated,
                       decided.onThis});
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
    m_shownThrough = &m_owned->shownThroughDistinct;
    m_inheritedDefaults = &m_owned->inheritedDefaults;
    m_stringPatterns = &m_owned->stringPatterns;
    m_makeTargets = &m_owned->makeTargets;
    for (const auto& [expr, target] : targets) {
        if (const auto* node = callNodeOf(*expr)) m_targets.emplace(node, target);
        m_targets.emplace(expr, target);
    }
    collectSites();
}

auto Program::collectSites() -> void {
    for (const auto& [expr, signature] : *m_selected)
        if (const auto* node = callNodeOf(*expr)) m_selectedByNode.emplace(node, &signature);
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

auto Program::targetOf(const ast::FunctionCall& call) const -> const Target& {
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

auto Program::resolvedTargetOf(const ast::MethodCall& call) const
    -> const semantic::ResolvedCallTarget* {
    if (!m_analysis) return nullptr;
    const auto& resolved = m_analysis->resolvedCalls();
    const auto found = resolved.find(&call);
    return found == resolved.end() ? nullptr : &found->second;
}

auto Program::dictionaryTypeOf(const ast::Expr& value) const -> std::string {
    return m_analysis ? m_analysis->dictionaryTypeOf(value) : std::string{};
}

auto Program::parameterTraitOf(const ast::Param& param) const -> std::string {
    return m_analysis ? m_analysis->parameterTraitOf(param) : std::string{};
}

auto Program::makeTargetTraitOf(const ast::FunctionDef& method) const -> std::string {
    return m_analysis ? m_analysis->makeTargetTraitOf(method) : std::string{};
}

auto Program::traitWrittenAs(const std::string& scope, const std::string& written) const
    -> std::string {
    return m_analysis ? m_analysis->traitWrittenAs(scope, written) : std::string{};
}

auto Program::calledFunctionOf(const ast::Expr& call) const -> std::string {
    return m_analysis ? m_analysis->calledFunctionOf(call) : std::string{};
}

auto Program::referencedConstantOf(const ast::Expr& name) const -> std::string {
    return m_analysis ? m_analysis->referencedConstantOf(name) : std::string{};
}

auto Program::recordOf(const ast::Expr& expr) const -> std::string {
    const auto type = typeOf(expr);
    const auto* named = type ? std::get_if<semantic::NamedType>(&type->kind) : nullptr;
    return named ? named->name : std::string{};
}

auto Program::declaredTypeOf(const ast::TypeExpr& type) const -> semantic::StructuredType {
    return m_analysis ? m_analysis->declaredTypeOf(type) : semantic::StructuredType{"Any", {}};
}

auto Program::expandDeclared(semantic::StructuredType type) const -> semantic::StructuredType {
    return m_analysis ? m_analysis->expandDeclared(std::move(type)) : type;
}

auto Program::annotationSpelling(const ast::TypeExpr& type) const -> std::string {
    return m_analysis ? m_analysis->annotationSpelling(type) : std::string{};
}

auto Program::typeTestOf(const std::string& scope, const std::string& written) const
    -> semantic::TypeTest {
    return m_analysis ? m_analysis->typeTestOf(scope, written) : semantic::TypeTest{};
}

auto Program::recordAnnotatedBy(const ast::TypeExpr& type) const -> std::string {
    return m_analysis ? m_analysis->recordAnnotatedBy(type) : std::string{};
}

auto Program::recordOfPattern(const ast::RecordPattern& pattern) const -> std::string {
    return m_analysis ? m_analysis->recordOfPattern(pattern) : std::string{};
}

auto Program::isTypeSelector(const ast::Pattern& pattern) const -> bool {
    return m_analysis && m_analysis->isTypeSelector(pattern);
}

auto Program::traitsOf(const ast::MakeDef& make) const -> const std::vector<std::string>* {
    if (!m_analysis) return nullptr;
    const auto& resolved = m_analysis->resolvedTraits();
    const auto found = resolved.find(&make);
    return found == resolved.end() ? nullptr : &found->second;
}

auto Program::inheritedDefaultsOf(const ast::MakeDef& make) const
    -> const std::vector<semantic::InheritedDefault>* {
    const auto found = m_inheritedDefaults->find(&make);
    return found == m_inheritedDefaults->end() ? nullptr : &found->second;
}

auto Program::makeTargetsOf(const ast::MakeDef& make) const -> const std::vector<std::string>* {
    const auto found = m_makeTargets->find(&make);
    return found == m_makeTargets->end() ? nullptr : &found->second;
}

auto Program::destructuresString(const ast::Pattern& pattern) const -> bool {
    return m_stringPatterns->contains(&pattern);
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

auto Program::shownThroughOf(const ast::Expr& call) const
    -> const std::vector<std::pair<std::size_t, std::string>>* {
    const auto found = m_shownThrough->find(&call);
    return found == m_shownThrough->end() ? nullptr : &found->second;
}

auto Program::shownThroughOf(const ast::Expr& call, std::size_t index) const
    -> const std::string* {
    const auto* shown = shownThroughOf(call);
    if (!shown) return nullptr;
    for (const auto& [position, type] : *shown)
        if (position == index) return &type;
    return nullptr;
}

auto Program::selectedSignatureOf(const ast::FunctionCall& call) const
    -> const semantic::Signature* {
    const auto found = m_selectedByNode.find(&call);
    return found == m_selectedByNode.end() ? nullptr : found->second;
}

auto Program::selectedSignatureOf(const ast::MethodCall& call) const
    -> const semantic::Signature* {
    const auto found = m_selectedByNode.find(&call);
    return found == m_selectedByNode.end() ? nullptr : found->second;
}

namespace {

// The answer of the newest program that answers for `node` (ProgramSet::push
// indexes them), else the base's, when `answered` accepts it.
template <typename Answer, typename Ask, typename Answered>
auto newestAnswer(const std::unordered_map<const void*, const Program*>& owners,
                  const void* node, const Program* base, Answer none, Ask ask,
                  Answered answered) -> Answer {
    if (const auto owner = owners.find(node); owner != owners.end())
        if (auto answer = ask(*owner->second); answered(answer)) return answer;
    if (base)
        if (auto answer = ask(*base); answered(answer)) return answer;
    return none;
}

} // namespace

auto ProgramSet::targetOf(const ast::MethodCall& call) const -> const Target& {
    return *newestAnswer<const Target*>(
        m_owners, &call, m_base, &kUndecided, [&](const Program& p) { return &p.targetOf(call); },
        [](const Target* t) { return t->kind != TargetKind::Undecided; });
}

auto ProgramSet::targetOf(const ast::FunctionCall& call) const -> const Target& {
    return *newestAnswer<const Target*>(
        m_owners, &call, m_base, &kUndecided, [&](const Program& p) { return &p.targetOf(call); },
        [](const Target* t) { return t->kind != TargetKind::Undecided; });
}

auto ProgramSet::targetOf(const ast::BinaryOp& op) const -> const Target& {
    return *newestAnswer<const Target*>(
        m_owners, &op, m_base, &kUndecided, [&](const Program& p) { return &p.targetOf(op); },
        [](const Target* t) { return t->kind != TargetKind::Undecided; });
}

auto ProgramSet::typeOf(const ast::Expr& expr) const -> semantic::TypePtr {
    return newestAnswer<semantic::TypePtr>(
        m_owners, &expr, m_base, nullptr, [&](const Program& p) { return p.typeOf(expr); },
        [](const semantic::TypePtr& t) { return t != nullptr; });
}

namespace {
auto mayNameType(const ast::Expr& expr) -> bool;
auto typeValueFrom(const ast::Expr& expr, const semantic::TypePtr& type)
    -> std::optional<semantic::StructuredType>;
}

auto ProgramSet::typeValueOf(const ast::Expr& expr) const
    -> std::optional<semantic::StructuredType> {
    return mayNameType(expr) ? typeValueFrom(expr, typeOf(expr)) : std::nullopt;
}

auto ProgramSet::receiverTypeOf(const ast::Expr& call) const -> std::string {
    return newestAnswer<std::string>(
        m_owners, &call, m_base, {}, [&](const Program& p) { return p.receiverTypeOf(call); },
        [](const std::string& t) { return !t.empty(); });
}

auto ProgramSet::typeOfAnswer(const ast::MethodCall& call) const
    -> const semantic::StaticTypeAnswer* {
    return newestAnswer<const semantic::StaticTypeAnswer*>(
        m_owners, &call, m_base, nullptr, [&](const Program& p) { return p.typeOfAnswer(call); },
        [](const auto* a) { return a != nullptr; });
}

auto ProgramSet::selectedSignatureOf(const ast::Expr& call) const
    -> const semantic::Signature* {
    return newestAnswer<const semantic::Signature*>(
        m_owners, &call, m_base, nullptr,
        [&](const Program& p) { return p.selectedSignatureOf(call); },
        [](const auto* s) { return s != nullptr; });
}

auto ProgramSet::shownThroughOf(const ast::Expr& call, std::size_t index) const
    -> const std::string* {
    return newestAnswer<const std::string*>(
        m_owners, &call, m_base, nullptr,
        [&](const Program& p) { return p.shownThroughOf(call, index); },
        [](const auto* s) { return s != nullptr; });
}

auto Program::forEachAnsweredNode(const std::function<void(const void*)>& visit) const -> void {
    for (const auto& [node, _] : m_targets) visit(node);
    for (const auto& [node, _] : m_selectedByNode) visit(node);
    for (const auto& [expr, _] : *m_types) visit(expr);
    for (const auto& [call, _] : *m_typeOfAnswers) visit(call);
    for (const auto& [expr, _] : *m_selected) visit(expr);
    for (const auto& [expr, _] : *m_shownThrough) visit(expr);
    for (const auto* pattern : *m_stringPatterns) visit(pattern);
}

auto Program::typeOf(const ast::Expr& expr) const -> semantic::TypePtr {
    const auto found = m_types->find(&expr);
    return found == m_types->end() ? nullptr : found->second;
}

namespace {

// Could `expr` name a type: `String`, or a path without arguments
// (`App.Models.User`)? Asked before the type map is.
auto mayNameType(const ast::Expr& expr) -> bool {
    const auto* call = std::get_if<ast::MethodCall>(&expr.kind);
    return std::holds_alternative<ast::UpperIdentifier>(expr.kind) ||
           (call && call->args.empty() && call->namedArgs.empty() && !call->block);
}

// A type name the checker typed `Type<X>`, as the `Type` record of X.
auto typeValueFrom(const ast::Expr& expr, const semantic::TypePtr& type)
    -> std::optional<semantic::StructuredType> {
    if (!type || !mayNameType(expr)) return std::nullopt;
    const auto* named = std::get_if<semantic::NamedType>(&type->kind);
    if (!named || named->name != kex::names::kTypeRecord || named->typeArgs.size() != 1)
        return std::nullopt;
    // The name written, which reflection on a VALUE cannot tell: a Byte is an
    // Integer at run time, but `to(Byte)` names the narrower type.
    const auto& written = named->typeArgs.front();
    if (std::holds_alternative<semantic::SizedIntType>(written->kind) ||
        std::holds_alternative<semantic::SizedFloatType>(written->kind))
        return semantic::StructuredType{semantic::typeToString(written), {}};
    return semantic::structuredTypeOf(written);
}

} // namespace

auto Program::typeValueOf(const ast::Expr& expr) const
    -> std::optional<semantic::StructuredType> {
    return mayNameType(expr) ? typeValueFrom(expr, typeOf(expr)) : std::nullopt;
}

auto verify(const Program& program) -> std::vector<std::string> {
    std::vector<std::string> problems;
    for (const auto& site : program.callSites()) {
        if (site.kind != TargetKind::Undecided) continue;
        auto problem = std::string(site.expr->location.file) + ":" +
                       std::to_string(site.expr->location.line) + ":" +
                       std::to_string(site.expr->location.column) +
                       ": no target for `" + site.callee + "`";
        // The receiver the checker saw is most of why a call went undecided.
        if (const auto* method = std::get_if<ast::MethodCall>(&site.expr->kind);
            method && method->receiver)
            if (const auto type = program.typeOf(*method->receiver))
                problem += " on " + semantic::typeToString(type);
        problems.push_back(std::move(problem));
    }
    return problems;
}

} // namespace kex::hir

namespace kex::hir {

// The newest program that checked the block answers; the base last.
auto ProgramSet::inheritedDefaultsOf(const ast::MakeDef& make) const
    -> const std::vector<semantic::InheritedDefault>* {
    for (auto program = m_programs.rbegin(); program != m_programs.rend(); ++program)
        if (const auto* inherited = (*program)->inheritedDefaultsOf(make)) return inherited;
    return m_base ? m_base->inheritedDefaultsOf(make) : nullptr;
}

auto ProgramSet::destructuresString(const ast::Pattern& pattern) const -> bool {
    for (auto program = m_programs.rbegin(); program != m_programs.rend(); ++program)
        if ((*program)->destructuresString(pattern)) return true;
    return m_base && m_base->destructuresString(pattern);
}

} // namespace kex::hir
