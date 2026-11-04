#include "clone.hxx"

namespace kex::ast {

namespace {

template <typename T>
auto cloneVec(const std::vector<T>& items) -> std::vector<T> {
    std::vector<T> out;
    out.reserve(items.size());
    for (const auto& item : items) out.push_back(clone(item));
    return out;
}

template <typename T>
auto cloneOpt(const std::optional<T>& item) -> std::optional<T> {
    if (!item) return std::nullopt;
    return clone(*item);
}

// name -> node pairs, as used by named arguments and record fields.
auto cloneNamed(const std::vector<std::pair<std::string, ExprPtr>>& items)
    -> std::vector<std::pair<std::string, ExprPtr>> {
    std::vector<std::pair<std::string, ExprPtr>> out;
    out.reserve(items.size());
    for (const auto& [name, value] : items)
        out.emplace_back(name, clone(value));
    return out;
}

} // namespace

auto clone(const TypeExprPtr& type) -> TypeExprPtr {
    if (!type) return nullptr;
    auto out = std::make_unique<TypeExpr>();
    out->location = type->location;
    std::visit(
        [&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, GenericType>) {
                out->kind = GenericType{node.name, cloneVec(node.args)};
            } else if constexpr (std::is_same_v<T, FunctionType>) {
                out->kind = FunctionType{clone(node.param), clone(node.result)};
            } else if constexpr (std::is_same_v<T, TupleType>) {
                out->kind = TupleType{cloneVec(node.elements)};
            } else if constexpr (std::is_same_v<T, ListType>) {
                out->kind = ListType{clone(node.element)};
            } else if constexpr (std::is_same_v<T, MapType>) {
                out->kind = MapType{clone(node.key), clone(node.value)};
            } else if constexpr (std::is_same_v<T, UnionType>) {
                out->kind = UnionType{clone(node.left), clone(node.right)};
            } else if constexpr (std::is_same_v<T, IntersectionType>) {
                out->kind =
                    IntersectionType{clone(node.left), clone(node.right)};
            } else if constexpr (std::is_same_v<T, RecordType>) {
                RecordType copy;
                for (const auto& [name, fieldType] : node.fields)
                    copy.fields.emplace_back(name, clone(fieldType));
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                out->kind = OptionalType{clone(node.inner)};
            } else if constexpr (std::is_same_v<T, BlockType>) {
                BlockType copy{clone(node.inner), {}};
                for (const auto& param : node.params)
                    copy.params.push_back(clone(param));
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, TypeQuery>) {
                out->kind = TypeQuery{node.query, clone(node.argument)};
            } else {
                // TypeName, AtomType, GenericVar — plain values.
                out->kind = node;
            }
        },
        type->kind);
    return out;
}

auto clone(const PatternPtr& pattern) -> PatternPtr {
    if (!pattern) return nullptr;
    auto out = std::make_unique<Pattern>();
    out->location = pattern->location;
    std::visit(
        [&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ThisPattern>) {
                out->kind = ThisPattern{clone(node.inner)};
            } else if constexpr (std::is_same_v<T, ConstructorPattern>) {
                out->kind = ConstructorPattern{node.name, cloneVec(node.args)};
            } else if constexpr (std::is_same_v<T, RecordPattern>) {
                RecordPattern copy;
                copy.typeName = node.typeName;
                for (const auto& field : node.fields)
                    copy.fields.push_back(FieldPattern{
                        field.name, cloneOpt(field.pattern), field.isStringKey});
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, ListPattern>) {
                out->kind =
                    ListPattern{cloneVec(node.elements), cloneOpt(node.rest)};
            } else if constexpr (std::is_same_v<T, TuplePattern>) {
                out->kind = TuplePattern{cloneVec(node.elements)};
            } else if constexpr (std::is_same_v<T, RangePattern>) {
                out->kind = RangePattern{clone(node.start), clone(node.end)};
            } else if constexpr (std::is_same_v<T, TypePattern>) {
                out->kind = TypePattern{node.name, clone(node.type)};
            } else {
                // LiteralPattern, VarPattern, WildcardPattern — plain values.
                out->kind = node;
            }
        },
        pattern->kind);
    return out;
}

auto clone(const MatchClause& clause) -> MatchClause {
    MatchClause out;
    out.patterns = cloneVec(clause.patterns);
    out.guard = cloneOpt(clause.guard);
    out.body = clone(clause.body);
    return out;
}

auto clone(const RescueBlock& rescue) -> RescueBlock {
    RescueBlock out;
    out.clauses = cloneVec(rescue.clauses);
    out.isCatchAll = rescue.isCatchAll;
    out.catchAllParam = rescue.catchAllParam;
    out.catchAllBody = cloneVec(rescue.catchAllBody);
    out.isInlineReturn = rescue.isInlineReturn;
    out.inlineReturnExpr = clone(rescue.inlineReturnExpr);
    return out;
}

auto clone(const Param& param) -> Param {
    Param out;
    out.pattern = cloneOpt(param.pattern);
    out.name = param.name;
    out.type = cloneOpt(param.type);
    out.defaultValue = cloneOpt(param.defaultValue);
    return out;
}

auto clone(const FunctionClause& clause) -> FunctionClause {
    FunctionClause out;
    out.params = cloneVec(clause.params);
    out.body = cloneVec(clause.body);
    out.returnAnnotation = cloneOpt(clause.returnAnnotation);
    out.rescue = cloneOpt(clause.rescue);
    out.hasParamList = clause.hasParamList;
    return out;
}

auto clone(const FunctionDef& function) -> std::unique_ptr<FunctionDef> {
    auto out = std::make_unique<FunctionDef>();
    out->location = function.location;
    out->name = function.name;
    out->isFoul = function.isFoul;
    out->isSlot = function.isSlot;
    out->isPredicate = function.isPredicate;
    out->computedName = clone(function.computedName);
    out->clauses = cloneVec(function.clauses);
    return out;
}

auto clone(const AbstractFunction& fn) -> AbstractFunction {
    return AbstractFunction{fn.name, clone(fn.type), fn.implicitThis};
}

auto clone(const TypeDef& type) -> std::unique_ptr<TypeDef> {
    auto out = std::make_unique<TypeDef>();
    out->location = type.location;
    out->name = type.name;
    out->isDistinct = type.isDistinct;
    out->leadingPipe = type.leadingPipe;
    out->typeParams = type.typeParams;
    out->parents = type.parents;
    if (type.variants) out->variants = cloneVec(*type.variants);
    if (type.abstractFunctions)
        out->abstractFunctions = cloneVec(*type.abstractFunctions);
    return out;
}

auto clone(const RecordDef& record) -> std::unique_ptr<RecordDef> {
    auto out = std::make_unique<RecordDef>();
    out->location = record.location;
    out->name = record.name;
    out->typeParams = record.typeParams;
    for (const auto& field : record.fields)
        out->fields.push_back(RecordField{
            field.name, clone(field.type), cloneOpt(field.defaultValue)});
    return out;
}

auto clone(const MakeDef& make) -> std::unique_ptr<MakeDef> {
    auto out = std::make_unique<MakeDef>();
    out->location = make.location;
    out->target = clone(make.target);
    out->isFinal = make.isFinal;
    out->isServing = make.isServing;
    out->implements = make.implements;
    for (const auto& item : make.body) {
        std::visit(
            [&](const auto& node) {
                using Ptr = std::decay_t<decltype(*node)>;
                if (!node) return;
                if constexpr (std::is_same_v<Ptr, FunctionDef>) {
                    out->body.push_back(clone(*node));
                } else if constexpr (std::is_same_v<Ptr, TypeAnnotation>) {
                    out->body.push_back(clone(*node));
                } else if constexpr (std::is_same_v<Ptr, VisibilityBlock>) {
                    if (auto copy = clone(*node)) out->body.push_back(std::move(copy));
                } else if constexpr (std::is_same_v<Ptr, Expr>) {
                    // A driver loop in a generated make's body. Expansion
                    // replaces it with the methods it declared, but a clone
                    // taken before that has to carry it.
                    out->body.push_back(clone(node));
                }
            },
            item);
    }
    return out;
}

auto clone(const ExprPtr& expr) -> ExprPtr {
    if (!expr) return nullptr;
    auto out = std::make_unique<Expr>();
    out->location = expr->location;
    std::visit(
        [&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, StringLiteral>) {
                StringLiteral copy;
                copy.value = node.value;
                copy.interpolating = node.interpolating;
                copy.parts = node.parts;
                copy.values = cloneVec(node.values);
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, TaggedLiteral>) {
                TaggedLiteral copy;
                copy.tag = node.tag;
                copy.parts = node.parts;
                copy.values = cloneVec(node.values);
                copy.interpolating = node.interpolating;
                copy.bodyStartOffset = node.bodyStartOffset;
                copy.bodyEndOffset = node.bodyEndOffset;
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, MethodCall>) {
                MethodCall copy;
                copy.receiver = clone(node.receiver);
                copy.method = node.method;
                copy.args = cloneVec(node.args);
                copy.namedArgs = cloneNamed(node.namedArgs);
                copy.block = cloneOpt(node.block);
                copy.mutating = node.mutating;
                copy.parenthesized = node.parenthesized;
                copy.targetType = clone(node.targetType);
                copy.tagged = node.tagged;
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, FunctionCall>) {
                FunctionCall copy;
                copy.name = node.name;
                copy.args = cloneVec(node.args);
                copy.namedArgs = cloneNamed(node.namedArgs);
                copy.block = cloneOpt(node.block);
                copy.reifiedConstructor = node.reifiedConstructor;
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, RecordConstruction>) {
                RecordConstruction copy;
                copy.typeName = node.typeName;
                for (const auto& field : node.fields)
                    copy.fields.push_back(
                        RecordEntry{field.name, clone(field.value), field.spread});
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, BinaryOp>) {
                out->kind = BinaryOp{clone(node.left), node.op, clone(node.right)};
            } else if constexpr (std::is_same_v<T, UnaryOp>) {
                out->kind = UnaryOp{node.op, clone(node.operand)};
            } else if constexpr (std::is_same_v<T, TupleExpr>) {
                out->kind = TupleExpr{cloneVec(node.elements)};
            } else if constexpr (std::is_same_v<T, ListExpr>) {
                out->kind = ListExpr{cloneVec(node.elements), cloneOpt(node.rest),
                                     node.anyElements};
            } else if constexpr (std::is_same_v<T, MapExpr>) {
                MapExpr copy;
                for (const auto& entry : node.entries)
                    copy.entries.push_back(
                        MapEntry{clone(entry.key), clone(entry.value)});
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, RangeExpr>) {
                out->kind = RangeExpr{clone(node.start), clone(node.end)};
            } else if constexpr (std::is_same_v<T, IfExpr>) {
                IfExpr copy;
                copy.condition = clone(node.condition);
                copy.thenBody = cloneVec(node.thenBody);
                for (const auto& [condition, body] : node.elifs)
                    copy.elifs.emplace_back(clone(condition), cloneVec(body));
                if (node.elseBody) copy.elseBody = cloneVec(*node.elseBody);
                copy.letPattern = clone(node.letPattern);
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, MatchExpr>) {
                MatchExpr copy;
                copy.subject = clone(node.subject);
                copy.subjectBinding = node.subjectBinding;
                copy.clauses = cloneVec(node.clauses);
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, ReceiveExpr>) {
                ReceiveExpr copy;
                copy.clauses = cloneVec(node.clauses);
                copy.timeout = cloneOpt(node.timeout);
                copy.afterBody = cloneOpt(node.afterBody);
                copy.senderBinding = node.senderBinding;
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, LoopExpr>) {
                out->kind = LoopExpr{cloneVec(node.body), node.counter};
            } else if constexpr (std::is_same_v<T, WhileExpr>) {
                out->kind = WhileExpr{clone(node.condition), cloneVec(node.body)};
            } else if constexpr (std::is_same_v<T, LetExpr>) {
                LetExpr copy;
                copy.pattern = clone(node.pattern);
                copy.value = clone(node.value);
                copy.type = cloneOpt(node.type);
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, VarExpr>) {
                VarExpr copy;
                copy.pattern = clone(node.pattern);
                copy.value = clone(node.value);
                copy.type = cloneOpt(node.type);
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, AssignExpr>) {
                out->kind = AssignExpr{node.name, node.path, clone(node.value)};
            } else if constexpr (std::is_same_v<T, ReturnExpr>) {
                out->kind = ReturnExpr{clone(node.value)};
            } else if constexpr (std::is_same_v<T, SpawnExpr>) {
                out->kind = SpawnExpr{cloneVec(node.body)};
            } else if constexpr (std::is_same_v<T, Lambda>) {
                Lambda copy;
                for (const auto& param : node.params)
                    copy.params.push_back(
                        LambdaParam{param.name, cloneOpt(param.type)});
                copy.body = cloneVec(node.body);
                copy.returnAnnotation = cloneOpt(node.returnAnnotation);
                copy.rescue = cloneOpt(node.rescue);
                copy.collection = node.collection;
                copy.namedFunction = node.namedFunction;
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, ShorthandLambda>) {
                out->kind =
                    ShorthandLambda{node.kind, node.name, cloneVec(node.args)};
            } else if constexpr (std::is_same_v<T, SpreadExpr>) {
                out->kind = SpreadExpr{clone(node.inner)};
            } else if constexpr (std::is_same_v<T, TrailingIf>) {
                out->kind = TrailingIf{clone(node.expr), clone(node.condition)};
            } else if constexpr (std::is_same_v<T, ThenElseExpr>) {
                out->kind = ThenElseExpr{clone(node.condition),
                                         clone(node.thenExpr),
                                         clone(node.elseExpr)};
            } else if constexpr (std::is_same_v<T, BlockExpr>) {
                out->kind = BlockExpr{cloneVec(node.body)};
            } else if constexpr (std::is_same_v<T, CurryExpr>) {
                CurryExpr copy;
                copy.name = node.name;
                copy.isOperator = node.isOperator;
                for (const auto& group : node.argGroups)
                    copy.argGroups.push_back(cloneVec(group));
                copy.module = node.module;
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, TryExpr>) {
                out->kind = TryExpr{clone(node.operand)};
            } else if constexpr (std::is_same_v<T, TryingExpr>) {
                out->kind = TryingExpr{cloneVec(node.body), clone(node.rescue)};
            } else if constexpr (std::is_same_v<T, GeneratedDecl>) {
                // The template is shared, not copied: instantiation clones it
                // per generated declaration, and it is never mutated in place.
                out->kind = GeneratedDecl{clone(node.name), node.function};
            } else if constexpr (std::is_same_v<T, WithExpr>) {
                WithExpr copy;
                copy.capability = node.capability;
                copy.value = clone(node.value);
                copy.body = cloneVec(node.body);
                out->kind = std::move(copy);
            } else if constexpr (std::is_same_v<T, UsingExpr>) {
                UsingExpr copy;
                copy.module = node.module;
                copy.alias = node.alias;
                copy.onlyNames = node.onlyNames;
                copy.exceptNames = node.exceptNames;
                copy.body = cloneVec(node.body);
                out->kind = std::move(copy);
            } else {
                // Leaves: the literals, Identifier, UpperIdentifier, ThisExpr,
                // BreakExpr, NextExpr, CurryPlaceholder, ErrorNode.
                out->kind = node;
            }
        },
        expr->kind);
    return out;
}

auto clone(const TypeAnnotation& annotation) -> std::unique_ptr<TypeAnnotation> {
    auto out = std::make_unique<TypeAnnotation>();
    out->location = annotation.location;
    out->name = annotation.name;
    out->type = clone(annotation.type);
    out->implicitThis = annotation.implicitThis;
    out->implicitFrom = annotation.implicitFrom;
    out->isFoul = annotation.isFoul;
    return out;
}

auto clone(const UsingBlock& block) -> std::unique_ptr<UsingBlock> {
    auto out = std::make_unique<UsingBlock>();
    out->location = block.location;
    out->module = block.module;
    out->alias = block.alias;
    out->onlyNames = block.onlyNames;
    out->exceptNames = block.exceptNames;
    for (const auto& expr : block.body) out->body.push_back(clone(expr));
    return out;
}

namespace {

// Copies of the items a block nests; a CompiledBlock (unexpanded
// compile-time code) has no copy, and makes the whole clone fail.
template <typename Items>
auto cloneItems(const Items& items, Items& out) -> bool {
    for (const auto& item : items) {
        bool ok = true;
        std::visit(
            [&](const auto& node) {
                using Ptr = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<Ptr, ExprPtr>) {
                    out.push_back(clone(node));
                } else {
                    using Node = typename Ptr::element_type;
                    if (!node) {
                        out.push_back(Ptr{});
                    } else if constexpr (std::is_same_v<Node, CompiledBlock>) {
                        ok = false;
                    } else if constexpr (std::is_same_v<Node, VisibilityBlock> ||
                                         std::is_same_v<Node, ModuleDef>) {
                        auto copy = clone(*node);
                        if (!copy) ok = false;
                        else out.push_back(std::move(copy));
                    } else {
                        out.push_back(clone(*node));
                    }
                }
            },
            item);
        if (!ok) return false;
    }
    return true;
}

} // namespace

auto clone(const VisibilityBlock& block) -> std::unique_ptr<VisibilityBlock> {
    auto out = std::make_unique<VisibilityBlock>();
    out->location = block.location;
    out->isPublic = block.isPublic;
    if (!cloneItems(block.items, out->items)) return nullptr;
    return out;
}

auto clone(const TraitDef& trait) -> std::unique_ptr<TraitDef> {
    auto out = std::make_unique<TraitDef>();
    out->location = trait.location;
    out->name = trait.name;
    out->typeParams = trait.typeParams;
    cloneItems(trait.body, out->body);
    return out;
}

auto clone(const ExportDecl& decl) -> std::unique_ptr<ExportDecl> {
    return std::make_unique<ExportDecl>(ExportDecl{
        decl.location, decl.module, decl.alias, decl.onlyNames, decl.exceptNames});
}

auto clone(const MainBlock& main) -> std::unique_ptr<MainBlock> {
    auto out = std::make_unique<MainBlock>();
    out->location = main.location;
    for (const auto& param : main.params) out->params.push_back(clone(param));
    for (const auto& expr : main.body) out->body.push_back(clone(expr));
    out->synthetic = main.synthetic;
    out->isExplicitMain = main.isExplicitMain;
    if (main.rescue) out->rescue = clone(*main.rescue);
    return out;
}

auto clone(const Pragma& pragma) -> std::unique_ptr<Pragma> {
    return std::make_unique<Pragma>(Pragma{pragma.location, pragma.requirements});
}

auto clone(const ModuleDef& module) -> std::unique_ptr<ModuleDef> {
    auto out = std::make_unique<ModuleDef>();
    out->location = module.location;
    out->name = module.name;
    out->isCapability = module.isCapability;
    if (!cloneItems(module.body, out->body)) return nullptr;
    return out;
}

auto clone(const Program& program) -> std::optional<Program> {
    Program out;
    if (!cloneItems(program.items, out.items)) return std::nullopt;
    return out;
}

} // namespace kex::ast
