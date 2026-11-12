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
constexpr int kVersion = 7;

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
                        Term::boolean(target.servingCast),
                        Term::integer(static_cast<int64_t>(target.operands)),
                        Term::boolean(target.negated), Term::boolean(target.onThis)});
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
    target.operands = static_cast<semantic::BuiltinOperands>(sax.readInt());
    target.negated = sax.readBool();
    target.onThis = sax.readBool();
    return target;
}

// The MethodCall a `Type.of` answer is keyed by, inside its expression.
auto methodCallOf(const ast::Expr& expr) -> const ast::MethodCall* {
    return std::get_if<ast::MethodCall>(&expr.kind);
}

// Every `make` block of the program, in one fixed order: what persisted
// inherited defaults are keyed by.
template <typename Items>
auto collectMakes(const Items& items, std::vector<const ast::MakeDef*>& makes) -> void {
    for (const auto& item : items)
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) makes.push_back(node.get());
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectMakes(node->body, makes);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>> ||
                                 std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
                if (node) collectMakes(node->items, makes);
            }
        }, item);
}

auto makesOf(const ast::Program& program) -> std::vector<const ast::MakeDef*> {
    std::vector<const ast::MakeDef*> makes;
    collectMakes(program.items, makes);
    return makes;
}

// Every pattern of the program, nested ones included, in one fixed order:
// what persisted pattern answers are keyed by. Function and main-block
// parameters and function-level rescue arms first, then each expression's
// own, in `order`.
auto patternsOf(const ast::Program& program, const std::vector<const ast::Expr*>& order)
    -> std::vector<const ast::Pattern*> {
    std::vector<const ast::Pattern*> patterns;
    const auto add = [&](const ast::Pattern& pattern) {
        ast::forEachSubPattern(pattern, [&](const ast::Pattern& nested) {
            patterns.push_back(&nested);
        });
    };
    const auto params = [&](const std::vector<ast::Param>& list) {
        for (const auto& param : list)
            if (param.pattern && *param.pattern) add(**param.pattern);
    };
    ast::forEachFunction(program, [&](const ast::FunctionDef& function) {
        for (const auto& clause : function.clauses) {
            params(clause.params);
            if (clause.rescue)
                for (const auto& arm : clause.rescue->clauses)
                    for (const auto& pattern : arm.patterns)
                        if (pattern) add(*pattern);
        }
    });
    for (const auto& item : program.items)
        if (const auto* main = std::get_if<std::unique_ptr<ast::MainBlock>>(&item);
            main && *main)
            params((*main)->params);
    for (const auto* expr : order) ast::forEachOwnedPattern(*expr, add);
    return patterns;
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

auto save(const Program& program, const ast::Program& ast,
          const std::vector<const ast::Expr*>& order) -> std::vector<uint8_t> {
    std::vector<TermPtr> entries;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& expr = *order[i];
        const auto& target = program.targetOf(expr);
        const auto type = program.typeOf(expr);
        const auto* call = methodCallOf(expr);
        const auto* answer = call ? program.typeOfAnswer(*call) : nullptr;
        const auto* selected = program.selectedSignatureOf(expr);
        const auto* shown = program.shownThroughOf(expr);
        if (target.kind == TargetKind::Undecided && !type && !answer && !selected && !shown)
            continue;
        std::vector<TermPtr> shownTerms;
        if (shown)
            for (const auto& [position, typeName] : *shown)
                shownTerms.push_back(Term::tuple({Term::integer(static_cast<int64_t>(position)),
                                                  Term::binary(typeName)}));
        entries.push_back(Term::tuple({
            Term::integer(static_cast<int64_t>(i)),
            target.kind == TargetKind::Undecided ? none() : targetToTerm(target),
            beam::encodeSemanticType(type),
            answer ? Term::tuple({structuredToTerm(answer->type),
                                  Term::boolean(answer->evaluateArgument)})
                   : none(),
            selected ? beam::encodeSignature(*selected) : none(),
            Term::list(std::move(shownTerms)),
        }));
    }
    // Each `make` block's inherited trait defaults, by their positions.
    const auto makes = makesOf(ast);
    std::vector<TermPtr> inherited;
    for (std::size_t i = 0; i < makes.size(); ++i) {
        const auto* defaults = program.inheritedDefaultsOf(*makes[i]);
        if (!defaults) continue;
        std::vector<TermPtr> names;
        for (const auto& inheritedDefault : *defaults)
            names.push_back(Term::tuple({Term::binary(inheritedDefault.trait),
                                         Term::binary(inheritedDefault.method)}));
        inherited.push_back(Term::tuple({Term::integer(static_cast<int64_t>(i)),
                                         Term::list(std::move(names))}));
    }
    // The types each `make` block is for, by its position.
    std::vector<TermPtr> targets;
    for (std::size_t i = 0; i < makes.size(); ++i) {
        const auto* owners = program.makeTargetsOf(*makes[i]);
        if (!owners) continue;
        std::vector<TermPtr> names;
        for (const auto& owner : *owners) names.push_back(Term::binary(owner));
        targets.push_back(Term::tuple({Term::integer(static_cast<int64_t>(i)),
                                       Term::list(std::move(names))}));
    }
    // The list patterns that take a String apart, by their positions.
    const auto patterns = patternsOf(ast, order);
    std::vector<TermPtr> stringPatterns;
    for (std::size_t i = 0; i < patterns.size(); ++i)
        if (program.destructuresString(*patterns[i]))
            stringPatterns.push_back(Term::integer(static_cast<int64_t>(i)));
    return beam::encodeEtf(Term::tuple({
        Term::atom("hir"), Term::integer(kMagic), Term::integer(kVersion),
        Term::integer(static_cast<int64_t>(order.size())),
        Term::list(std::move(entries)),
        Term::list(std::move(inherited)),
        Term::list(std::move(stringPatterns)),
        Term::list(std::move(targets)),
    }));
}

auto load(const ast::Program& ast, const std::vector<const ast::Expr*>& order,
          const std::vector<uint8_t>& bytes) -> std::unique_ptr<Program> {
    try {
        SaxReader sax(bytes.data(), bytes.size());
        if (sax.beginTuple() != 8 || sax.readAtom() != "hir" ||
            sax.readInt() != kMagic || sax.readInt() != kVersion ||
            sax.readInt() != static_cast<int64_t>(order.size()))
            return nullptr;
        std::unordered_map<const ast::Expr*, Target> targets;
        auto answers = std::make_unique<Answers>();
        const auto count = sax.beginList();
        for (size_t entry = 0; entry < count; ++entry) {
            const auto fields = sax.beginTuple();
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
            // Distinct arguments shown through their own type: an entry
            // written before this field existed has five elements.
            if (fields > 5) {
                const auto shown = sax.beginList();
                for (size_t i = 0; i < shown; ++i) {
                    sax.beginTuple();
                    const auto position = static_cast<std::size_t>(sax.readInt());
                    answers->shownThroughDistinct[expr].emplace_back(position, sax.readBinary());
                }
                sax.endList();
            }
        }
        sax.endList();
        const auto makes = makesOf(ast);
        const auto blocks = sax.beginList();
        for (size_t block = 0; block < blocks; ++block) {
            sax.beginTuple();
            const auto index = static_cast<std::size_t>(sax.readInt());
            if (index >= makes.size()) return nullptr;
            auto& defaults = answers->inheritedDefaults[makes[index]];
            const auto count = sax.beginList();
            for (size_t i = 0; i < count; ++i) {
                sax.beginTuple();
                auto trait = sax.readBinary();
                defaults.push_back({std::move(trait), sax.readBinary()});
            }
            sax.endList();
        }
        sax.endList();
        const auto patterns = patternsOf(ast, order);
        const auto strings = sax.beginList();
        for (size_t i = 0; i < strings; ++i) {
            const auto index = static_cast<std::size_t>(sax.readInt());
            if (index >= patterns.size()) return nullptr;
            answers->stringPatterns.insert(patterns[index]);
        }
        sax.endList();
        const auto targetBlocks = sax.beginList();
        for (size_t block = 0; block < targetBlocks; ++block) {
            sax.beginTuple();
            const auto index = static_cast<std::size_t>(sax.readInt());
            if (index >= makes.size()) return nullptr;
            auto& owners = answers->makeTargets[makes[index]];
            const auto count = sax.beginList();
            for (size_t i = 0; i < count; ++i) owners.push_back(sax.readBinary());
            sax.endList();
        }
        sax.endList();
        return std::make_unique<Program>(ast, targets, std::move(answers));
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace kex::hir
