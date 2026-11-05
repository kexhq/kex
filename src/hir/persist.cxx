#include "persist.hxx"

#include "../ast/walk.hxx"
#include "../beam/etf.hxx"
#include "../beam/interface_cache.hxx"

namespace kex::hir {

namespace {

using beam::SaxReader;
using beam::Term;
using beam::TermPtr;

constexpr int kMagic = 0x4b584852;  // "KXHR"
// Bump with any change to the shape below.
constexpr int kVersion = 1;

auto none() -> TermPtr { return Term::atom("none"); }

auto structuredToTerm(const semantic::StructuredType& type) -> TermPtr {
    std::vector<TermPtr> args;
    for (const auto& arg : type.args) args.push_back(structuredToTerm(arg));
    return Term::tuple({Term::binary(type.name), Term::list(std::move(args)),
                        Term::boolean(type.pure)});
}

auto structuredFromSax(SaxReader& sax) -> semantic::StructuredType {
    sax.beginTuple();
    semantic::StructuredType type;
    type.name = sax.readBinary();
    const auto count = sax.beginList();
    for (size_t i = 0; i < count; ++i) type.args.push_back(structuredFromSax(sax));
    sax.endList();
    type.pure = sax.readBool();
    return type;
}

auto targetToTerm(const Target& target) -> TermPtr {
    return Term::tuple({Term::integer(static_cast<int64_t>(target.kind)),
                        Term::binary(target.module), Term::binary(target.function),
                        Term::integer(target.arity), Term::binary(target.owner),
                        Term::boolean(target.servingCast)});
}

auto targetFromSax(SaxReader& sax) -> Target {
    sax.beginTuple();
    Target target;
    target.kind = static_cast<TargetKind>(sax.readInt());
    target.module = sax.readBinary();
    target.function = sax.readBinary();
    target.arity = static_cast<int>(sax.readInt());
    target.owner = sax.readBinary();
    target.servingCast = sax.readBool();
    return target;
}

// The MethodCall a `Type.of` answer is keyed by, inside its expression.
auto methodCallOf(const ast::Expr& expr) -> const ast::MethodCall* {
    return std::get_if<ast::MethodCall>(&expr.kind);
}

} // namespace

auto nodeOrder(const ast::Program& program) -> std::vector<const ast::Expr*> {
    std::vector<const ast::Expr*> order;
    const auto visit = [&](const auto& self, const ast::Expr& expr) -> void {
        order.push_back(&expr);
        ast::forEachChild(expr, [&](const ast::ExprPtr& child) {
            if (child) self(self, *child);
        });
    };
    ast::forEachBodyExpr(program, [&](const ast::Expr& expr) { visit(visit, expr); });
    return order;
}

auto save(const Program& program, const std::vector<const ast::Expr*>& order)
    -> std::vector<uint8_t> {
    std::vector<TermPtr> entries;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& expr = *order[i];
        const auto& target = program.targetOf(expr);
        const auto type = program.typeOf(expr);
        const auto* call = methodCallOf(expr);
        const auto* answer = call ? program.typeOfAnswer(*call) : nullptr;
        const auto* selected = program.selectedSignatureOf(expr);
        if (target.kind == TargetKind::Undecided && !type && !answer && !selected)
            continue;
        entries.push_back(Term::tuple({
            Term::integer(static_cast<int64_t>(i)),
            target.kind == TargetKind::Undecided ? none() : targetToTerm(target),
            beam::encodeSemanticType(type),
            answer ? Term::tuple({structuredToTerm(answer->type),
                                  Term::boolean(answer->evaluateArgument)})
                   : none(),
            selected ? beam::encodeSignature(*selected) : none(),
        }));
    }
    return beam::encodeEtf(Term::tuple({
        Term::atom("hir"), Term::integer(kMagic), Term::integer(kVersion),
        Term::integer(static_cast<int64_t>(order.size())),
        Term::list(std::move(entries)),
    }));
}

auto load(const ast::Program& ast, const std::vector<const ast::Expr*>& order,
          const std::vector<uint8_t>& bytes) -> std::unique_ptr<Program> {
    try {
        SaxReader sax(bytes.data(), bytes.size());
        if (sax.beginTuple() != 5 || sax.readAtom() != "hir" ||
            sax.readInt() != kMagic || sax.readInt() != kVersion ||
            sax.readInt() != static_cast<int64_t>(order.size()))
            return nullptr;
        std::unordered_map<const ast::Expr*, Target> targets;
        auto answers = std::make_unique<Answers>();
        const auto count = sax.beginList();
        for (size_t entry = 0; entry < count; ++entry) {
            sax.beginTuple();
            const auto index = static_cast<std::size_t>(sax.readInt());
            if (index >= order.size()) return nullptr;
            const auto* expr = order[index];
            if (sax.nextIsAtom()) sax.readAtom();
            else targets.emplace(expr, targetFromSax(sax));
            if (auto type = beam::decodeSemanticType(sax)) answers->types.emplace(expr, type);
            if (sax.nextIsAtom()) {
                sax.readAtom();
            } else {
                sax.beginTuple();
                semantic::StaticTypeAnswer answer;
                answer.type = structuredFromSax(sax);
                answer.evaluateArgument = sax.readBool();
                if (const auto* call = methodCallOf(*expr))
                    answers->typeOfAnswers.emplace(call, std::move(answer));
            }
            if (sax.nextIsAtom()) sax.readAtom();
            else answers->selected.emplace(expr, beam::decodeSignature(sax));
        }
        sax.endList();
        return std::make_unique<Program>(ast, targets, std::move(answers));
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace kex::hir
