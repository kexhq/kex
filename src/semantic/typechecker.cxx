#include "typechecker.hxx"
#include "analyzer.hxx"
#include <set>

namespace kex::semantic {

namespace {

// A sum-type variant is constructor-shaped (TypeName or GenericType with a
// single-part name) if it's a bare `Name` or `Name(Args...)` — anything
// else (tuple/list/function/union/atom type exprs) means the TypeDef is a
// type alias, not a real ADT, and the whole def is skipped for exhaustiveness.
auto extractConstructorName(const ast::TypeExprPtr& variant) -> std::optional<std::string> {
    if (!variant) return std::nullopt;
    if (auto* tn = std::get_if<ast::TypeName>(&variant->kind)) {
        if (tn->parts.size() == 1) return tn->parts[0];
    }
    if (auto* gt = std::get_if<ast::GenericType>(&variant->kind)) {
        if (gt->name.parts.size() == 1) return gt->name.parts[0];
    }
    return std::nullopt;
}

} // namespace

auto TypeChecker::check(const ast::Program& program,
                        std::vector<Diagnostic>& diagnostics) -> void {
    m_diagnostics = &diagnostics;
    m_scopeStack.clear();
    pushScope();

    // Built-in prelude ADTs (see src/interpreter/stdlib/adt.cxx) — Just/
    // None and Ok/Error are bare constructors, not declared via a user
    // `type ... = ...`, so they're registered here rather than discovered
    // by walking TypeDefs below.
    m_adtVariants["Option"] = {"Just", "None"};
    m_adtOfConstructor["Just"] = "Option";
    m_adtOfConstructor["None"] = "Option";
    m_adtVariants["Result"] = {"Ok", "Error"};
    m_adtOfConstructor["Ok"] = "Result";
    m_adtOfConstructor["Error"] = "Result";

    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                registerAdt(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerAdtsInModule(*node);
            }
        }, item);
    }

    // Register built-in types
    m_globals.set("Int", Type::int64());
    m_globals.set("Integer", Type::integer());
    m_globals.set("Char", Type::charT());
    m_globals.set("String", Type::string());
    m_globals.set("Bool", Type::boolean());
    m_globals.set("Byte", Type::byte());
    m_globals.set("Int8", Type::int8());
    m_globals.set("Int16", Type::int16());
    m_globals.set("Int32", Type::int32());
    m_globals.set("Int64", Type::int64());
    m_globals.set("UInt8", Type::uint8());
    m_globals.set("UInt16", Type::uint16());
    m_globals.set("UInt32", Type::uint32());
    m_globals.set("UInt64", Type::uint64());
    m_globals.set("Float32", Type::float32());
    m_globals.set("Float64", Type::float64());
    // Note: no "Float" entry — it's not a concrete Type, only a trait
    // name (TraitRegistry, phase 3), satisfied by Float32 and Float64.

    for (const auto& item : program.items) {
        checkTopLevel(item);
    }

    popScope();
}

auto TypeChecker::registerAdt(const ast::TypeDef& def) -> void {
    if (!def.variants) return;

    std::vector<std::string> names;
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) return;  // not constructor-shaped — a type alias, skip entirely
        names.push_back(*name);
    }
    if (names.empty()) return;

    m_adtVariants[def.name] = names;
    for (const auto& name : names) {
        m_adtOfConstructor[name] = def.name;
    }
}

auto TypeChecker::registerAdtsInModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                registerAdt(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerAdtsInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::checkMatchExhaustiveness(const ast::MatchExpr& node, SourceLocation loc) -> void {
    bool hasUnguardedCatchAll = false;
    std::set<std::string> covered;
    std::string adtName;
    bool inconclusive = false;

    for (const auto& clause : node.clauses) {
        bool guarded = clause.guard && *clause.guard;
        for (const auto& pat : clause.patterns) {
            if (!pat) continue;
            if (std::holds_alternative<ast::WildcardPattern>(pat->kind) ||
                std::holds_alternative<ast::VarPattern>(pat->kind)) {
                if (!guarded) hasUnguardedCatchAll = true;
                continue;
            }
            auto* ctor = std::get_if<ast::ConstructorPattern>(&pat->kind);
            if (!ctor) continue;  // literal/list/tuple/record/range patterns don't drive ADT exhaustiveness

            auto it = m_adtOfConstructor.find(ctor->name);
            if (it == m_adtOfConstructor.end()) {
                inconclusive = true;  // unregistered constructor — can't prove the closed set
                continue;
            }
            if (adtName.empty()) adtName = it->second;
            else if (adtName != it->second) inconclusive = true;  // patterns span more than one ADT
            if (!guarded) covered.insert(ctor->name);
        }
    }

    if (hasUnguardedCatchAll || inconclusive || adtName.empty()) return;

    std::vector<std::string> missing;
    for (const auto& ctorName : m_adtVariants[adtName]) {
        if (!covered.count(ctorName)) missing.push_back(ctorName);
    }
    if (missing.empty()) return;

    std::string list;
    for (size_t i = 0; i < missing.size(); i++) {
        if (i) list += ", ";
        list += missing[i];
    }
    error(loc, "Non-exhaustive match on " + adtName + ": missing case(s) " + list);
}

auto TypeChecker::resolveTypeExpr(const ast::TypeExpr& typeExpr,
                                   std::unordered_map<std::string, TypePtr>& genericVars) -> TypePtr {
    return std::visit([this, &genericVars](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::TypeName>) {
            if (node.parts.empty()) return Type::unknown();
            const std::string& last = node.parts.back();
            // Single-letter identifiers are generic type variables (docs/
            // types.md Generics) — reuse the same TypeVar for repeated
            // occurrences of the same letter within this clause.
            if (last.size() == 1 && node.parts.size() == 1) {
                auto it = genericVars.find(last);
                if (it != genericVars.end()) return it->second;
                auto var = freshTypeVar();
                genericVars[last] = var;
                return var;
            }
            if (auto known = m_globals.get(last)) return known;
            return Type::named(last);  // unregistered record/ADT name — nameable, not yet structurally known
        }
        else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::vector<TypePtr> args;
            for (const auto& arg : node.args) {
                args.push_back(arg ? resolveTypeExpr(*arg, genericVars) : Type::unknown());
            }
            std::string name = node.name.parts.empty() ? "" : node.name.parts.back();
            return Type::named(name, std::move(args));
        }
        else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            TypePtr param = node.param ? resolveTypeExpr(*node.param, genericVars) : Type::unknown();
            TypePtr result = node.result ? resolveTypeExpr(*node.result, genericVars) : Type::unknown();
            return Type::func({param}, result);
        }
        else if constexpr (std::is_same_v<T, ast::TupleType>) {
            std::vector<TypePtr> elements;
            for (const auto& elem : node.elements) {
                elements.push_back(elem ? resolveTypeExpr(*elem, genericVars) : Type::unknown());
            }
            return Type::tuple(std::move(elements));
        }
        else if constexpr (std::is_same_v<T, ast::ListType>) {
            return Type::list(node.element ? resolveTypeExpr(*node.element, genericVars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::MapType>) {
            return Type::map(node.key ? resolveTypeExpr(*node.key, genericVars) : Type::unknown(),
                              node.value ? resolveTypeExpr(*node.value, genericVars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return Type::optional(node.inner ? resolveTypeExpr(*node.inner, genericVars) : Type::unknown());
        }
        else if constexpr (std::is_same_v<T, ast::UnionType>) {
            std::vector<TypePtr> members;
            members.push_back(node.left ? resolveTypeExpr(*node.left, genericVars) : Type::unknown());
            members.push_back(node.right ? resolveTypeExpr(*node.right, genericVars) : Type::unknown());
            return std::make_shared<Type>(Type{UnionType{std::move(members)}});
        }
        else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return Type::atom();
        }
        else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            auto it = genericVars.find(node.name);
            if (it != genericVars.end()) return it->second;
            auto var = freshTypeVar();
            genericVars[node.name] = var;
            return var;
        }
        else {
            // BlockType — not modeled as a distinct semantic::Type yet.
            return Type::unknown();
        }
    }, typeExpr.kind);
}

auto TypeChecker::bindPatternVars(const ast::Pattern& pat) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::VarPattern>) {
            if (node.name != "_") defineVar(node.name, freshTypeVar());
        }
        else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) bindPatternVars(*node.inner);
        }
        else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            for (const auto& arg : node.args) {
                if (arg) bindPatternVars(*arg);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            for (const auto& field : node.fields) {
                if (field.pattern) bindPatternVars(**field.pattern);
                else if (!field.isStringKey) defineVar(field.name, freshTypeVar());
            }
        }
        else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            for (const auto& elem : node.elements) {
                if (elem) bindPatternVars(*elem);
            }
            if (node.rest) bindPatternVars(**node.rest);
        }
        else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            for (const auto& elem : node.elements) {
                if (elem) bindPatternVars(*elem);
            }
        }
        // LiteralPattern, WildcardPattern, RangePattern introduce nothing.
    }, pat.kind);
}

auto TypeChecker::checkTopLevel(const ast::TopLevelItem& item) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
            checkModule(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
            checkFunctionDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
            checkMakeDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
            checkMainBlock(*node);
        }
    }, item);
}

auto TypeChecker::checkModule(const ast::ModuleDef& mod) -> void {
    pushScope();
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                checkMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                checkModule(*node);
            }
        }, item);
    }
    popScope();
}

auto TypeChecker::checkFunctionDef(const ast::FunctionDef& def) -> void {
    auto returnType = freshTypeVar();
    defineVar(def.name, returnType);

    std::vector<Signature> signatures;
    for (const auto& clause : def.clauses) {
        pushScope();
        std::unordered_map<std::string, TypePtr> genericVars;
        std::vector<TypePtr> paramTypes;
        for (const auto& param : clause.params) {
            TypePtr paramType = param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar();
            paramTypes.push_back(paramType);
            if (param.name.has_value() && *param.name != "_") {
                defineVar(*param.name, paramType);
            }
            if (param.pattern) {
                bindPatternVars(**param.pattern);
            }
        }
        auto bodyType = inferBody(clause.body);
        popScope();
        signatures.push_back(Signature{def.name, std::move(paramTypes), bodyType});
    }

    // make-block methods have an implicit `this` receiver, not a regular
    // param — checkCall's UFCS desugaring (receiver as argument 0) would
    // mis-count their arity, so they're checked (body inference still
    // runs above) but not registered for call-site checking.
    if (!m_inMakeBlock) {
        m_userSignatures[def.name] = std::move(signatures);
    }
}

auto TypeChecker::checkMakeDef(const ast::MakeDef& def) -> void {
    pushScope();
    bool wasInMakeBlock = m_inMakeBlock;
    m_inMakeBlock = true;
    for (const auto& item : def.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            }
        }, item);
    }
    m_inMakeBlock = wasInMakeBlock;
    popScope();
}

auto TypeChecker::checkMainBlock(const ast::MainBlock& block) -> void {
    pushScope();
    inferBody(block.body);
    popScope();
}

auto TypeChecker::inferBody(const std::vector<ast::ExprPtr>& body) -> TypePtr {
    TypePtr lastType = Type::unit();
    for (const auto& expr : body) {
        if (expr) lastType = inferExpr(*expr);
    }
    return lastType;
}

auto TypeChecker::inferExpr(const ast::Expr& expr) -> TypePtr {
    return std::visit([this, &expr](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::IntLiteral>) {
            return Type::integer();
        }
        else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
            return Type::float64();
        }
        else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            return Type::string();
        }
        else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
            return Type::boolean();
        }
        else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
            return Type::charT();
        }
        else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
            return Type::named("None");
        }
        else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
            return Type::atom();
        }
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
            auto type = lookupVar(node.name);
            if (!type) {
                return Type::unknown();
            }
            return type;
        }
        else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            if (auto* varPat = std::get_if<ast::VarPattern>(&node.pattern->kind)) {
                defineVar(varPat->name, valueType);
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            defineVar(node.name, valueType);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            auto varType = lookupVar(node.name);
            if (varType && !std::holds_alternative<UnknownType>(varType->kind) &&
                !std::holds_alternative<TypeVar>(varType->kind)) {
                if (!typesEqual(varType, valueType) &&
                    !std::holds_alternative<UnknownType>(valueType->kind) &&
                    !std::holds_alternative<TypeVar>(valueType->kind)) {
                    typeMismatch(expr.location, varType, valueType);
                }
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            auto leftType = node.left ? inferExpr(*node.left) : Type::unknown();
            auto rightType = node.right ? inferExpr(*node.right) : Type::unknown();
            return inferBinaryOp(node.op, leftType, rightType, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            auto operandType = node.operand ? inferExpr(*node.operand) : Type::unknown();
            if (node.op == TokenType::Bang) {
                if (!std::holds_alternative<UnknownType>(operandType->kind) &&
                    !typesEqual(operandType, Type::boolean())) {
                    error(expr.location, "Logical not '!' requires Bool, got " +
                          typeToString(operandType));
                }
                return Type::boolean();
            }
            if (node.op == TokenType::Minus) {
                return operandType;
            }
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            std::vector<TypePtr> argTypes;
            for (const auto& arg : node.args) {
                argTypes.push_back(arg ? inferExpr(*arg) : Type::unknown());
            }
            for (const auto& [_, arg] : node.namedArgs) {
                if (arg) inferExpr(*arg);
            }
            if (node.block) inferExpr(**node.block);
            return checkCall(node.name, argTypes, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            std::vector<TypePtr> argTypes;
            argTypes.push_back(node.receiver ? inferExpr(*node.receiver) : Type::unknown());
            for (const auto& arg : node.args) {
                argTypes.push_back(arg ? inferExpr(*arg) : Type::unknown());
            }
            for (const auto& [_, arg] : node.namedArgs) {
                if (arg) inferExpr(*arg);
            }
            if (node.block) inferExpr(**node.block);
            return checkCall(node.method, argTypes, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            TypePtr elemType = Type::unknown();
            for (const auto& elem : node.elements) {
                if (elem) {
                    auto t = inferExpr(*elem);
                    if (std::holds_alternative<UnknownType>(elemType->kind)) {
                        elemType = t;
                    } else if (!typesEqual(elemType, t) &&
                               !std::holds_alternative<UnknownType>(t->kind) &&
                               !std::holds_alternative<TypeVar>(t->kind)) {
                        error(expr.location,
                              "List elements must be the same type. Expected " +
                              typeToString(elemType) + ", got " + typeToString(t));
                    }
                }
            }
            return Type::list(elemType);
        }
        else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            TypePtr keyType = Type::unknown();
            TypePtr valueType = Type::unknown();
            for (const auto& entry : node.entries) {
                if (entry.key) {
                    auto k = inferExpr(*entry.key);
                    if (std::holds_alternative<UnknownType>(keyType->kind)) keyType = k;
                }
                if (entry.value) {
                    auto v = inferExpr(*entry.value);
                    if (std::holds_alternative<UnknownType>(valueType->kind)) valueType = v;
                }
            }
            return Type::map(keyType, valueType);
        }
        else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            std::vector<TypePtr> types;
            for (const auto& elem : node.elements) {
                types.push_back(elem ? inferExpr(*elem) : Type::unknown());
            }
            return Type::tuple(std::move(types));
        }
        else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            if (node.start) inferExpr(*node.start);
            if (node.end) inferExpr(*node.end);
            return Type::named("Range", {Type::integer()});
        }
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                if (!std::holds_alternative<UnknownType>(condType->kind) &&
                    !std::holds_alternative<TypeVar>(condType->kind) &&
                    !typesEqual(condType, Type::boolean())) {
                    error(expr.location, "If condition must be Bool, got " +
                          typeToString(condType));
                }
            }
            auto thenType = inferBody(node.thenBody);
            for (const auto& [cond, body] : node.elifs) {
                if (cond) inferExpr(*cond);
                inferBody(body);
            }
            if (node.elseBody) inferBody(*node.elseBody);
            return thenType;
        }
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            TypePtr subjectType = node.subject ? inferExpr(*node.subject) : Type::unknown();
            TypePtr resultType = Type::unknown();
            for (const auto& clause : node.clauses) {
                pushScope();
                if (node.subjectBinding) {
                    defineVar(*node.subjectBinding, subjectType);
                }
                for (const auto& pat : clause.patterns) {
                    if (pat) bindPatternVars(*pat);
                }
                if (clause.guard && *clause.guard) inferExpr(**clause.guard);
                if (clause.body) {
                    auto t = inferExpr(*clause.body);
                    if (std::holds_alternative<UnknownType>(resultType->kind)) {
                        resultType = t;
                    }
                }
                popScope();
            }
            checkMatchExhaustiveness(node, expr.location);
            return resultType;
        }
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            return node.value ? inferExpr(*node.value) : Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            pushScope();
            for (const auto& param : node.params) {
                if (param.name != "_") {
                    defineVar(param.name, freshTypeVar());
                }
            }
            auto bodyType = inferBody(node.body);
            popScope();
            return bodyType;
        }
        else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            pushScope();
            inferBody(node.body);
            popScope();
            return Type::named("Process");
        }
        else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            inferBody(node.body);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                if (!std::holds_alternative<UnknownType>(condType->kind) &&
                    !typesEqual(condType, Type::boolean())) {
                    error(expr.location, "While condition must be Bool, got " +
                          typeToString(condType));
                }
            }
            inferBody(node.body);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::ErrorPropagate>) {
            if (node.inner) return inferExpr(*node.inner);
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            for (const auto& [_, val] : node.fields) {
                if (val) inferExpr(*val);
            }
            return Type::named(node.typeName);
        }
        else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                if (!std::holds_alternative<UnknownType>(condType->kind) &&
                    !std::holds_alternative<TypeVar>(condType->kind) &&
                    !typesEqual(condType, Type::boolean())) {
                    error(expr.location, "If condition must be Bool, got " +
                          typeToString(condType));
                }
            }
            if (node.expr) return inferExpr(*node.expr);
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            return inferBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                if (!std::holds_alternative<UnknownType>(condType->kind) &&
                    !std::holds_alternative<TypeVar>(condType->kind) &&
                    !typesEqual(condType, Type::boolean())) {
                    error(expr.location, "then/else condition must be Bool, got " +
                          typeToString(condType));
                }
            }
            auto thenType = node.thenExpr ? inferExpr(*node.thenExpr) : Type::unknown();
            if (node.elseExpr) inferExpr(*node.elseExpr);
            return thenType;
        }
        else {
            return Type::unknown();
        }
    }, expr.kind);
}

auto TypeChecker::inferBinaryOp(TokenType op, const TypePtr& left, const TypePtr& right,
                                SourceLocation loc) -> TypePtr {
    if (std::holds_alternative<UnknownType>(left->kind) ||
        std::holds_alternative<UnknownType>(right->kind) ||
        std::holds_alternative<TypeVar>(left->kind) ||
        std::holds_alternative<TypeVar>(right->kind)) {
        // Can't check if types are unknown
        switch (op) {
            case TokenType::EqEq: case TokenType::NotEq:
            case TokenType::LessThan: case TokenType::GreaterThan:
            case TokenType::LessEq: case TokenType::GreaterEq:
            case TokenType::AmpAmp: case TokenType::PipePipe:
                return Type::boolean();
            default:
                return left;
        }
    }

    auto isString = [](const TypePtr& t) {
        auto* list = std::get_if<ListType>(&t->kind);
        if (!list) return false;
        auto* elemPrim = std::get_if<PrimitiveType>(&list->element->kind);
        return elemPrim && elemPrim->kind == PrimitiveType::Char;
    };
    auto isFloat = [](const TypePtr& t) { return std::holds_alternative<SizedFloatType>(t->kind); };
    auto isIntegerLike = [](const TypePtr& t) {
        if (auto* prim = std::get_if<PrimitiveType>(&t->kind)) return prim->kind == PrimitiveType::Integer;
        return std::holds_alternative<SizedIntType>(t->kind);
    };
    auto isBool = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Bool;
    };

    bool leftIsString = isString(left), rightIsString = isString(right);
    bool leftIsFloat = isFloat(left), rightIsFloat = isFloat(right);
    bool leftIsInt = isIntegerLike(left), rightIsInt = isIntegerLike(right);

    switch (op) {
        case TokenType::Plus:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString && rightIsString)
                return Type::string();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot add " + typeToString(left) + " and " + typeToString(right));
                return Type::string();
            }
            if (!typesEqual(left, right)) {
                error(loc, "Operator '+' requires matching types, got " +
                      typeToString(left) + " and " + typeToString(right));
            }
            return left;

        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot use arithmetic operator on String");
                return Type::integer();
            }
            return left;

        case TokenType::EqEq:
        case TokenType::NotEq:
            return Type::boolean();

        case TokenType::LessThan:
        case TokenType::GreaterThan:
        case TokenType::LessEq:
        case TokenType::GreaterEq:
            if (isBool(left)) {
                error(loc, "Cannot compare Bool values with '<', '>', '<=', '>='");
            }
            return Type::boolean();

        case TokenType::AmpAmp:
        case TokenType::PipePipe:
            if (!isBool(left)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(left));
            }
            if (!isBool(right)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(right));
            }
            return Type::boolean();

        default:
            return Type::unknown();
    }
}

auto TypeChecker::argMatchesParam(const TypePtr& argType, const TypePtr& paramType) const -> bool {
    auto isPermissive = [](const TypePtr& t) {
        return std::holds_alternative<UnknownType>(t->kind) || std::holds_alternative<TypeVar>(t->kind);
    };
    if (isPermissive(argType) || isPermissive(paramType)) return true;
    if (auto* constrained = std::get_if<ConstrainedType>(&paramType->kind)) {
        return m_traits.satisfies(argType, constrained->traitName);
    }
    // Sized ints/floats and arbitrary-precision Integer aren't distinguished
    // at runtime yet (IntValue is one int64_t, FloatValue one double — see
    // the type-system plan's Runtime representation section), so don't
    // hard-error on a width/precision distinction the runtime doesn't keep:
    // any two Integer-trait members are compatible with each other, and
    // likewise for Float. Integer-vs-Float itself stays a real mismatch —
    // that distinction *is* runtime-backed today.
    if (m_traits.satisfies(argType, "Integer") && m_traits.satisfies(paramType, "Integer")) return true;
    if (m_traits.satisfies(argType, "Float") && m_traits.satisfies(paramType, "Float")) return true;
    return typesEqual(argType, paramType);
}

auto TypeChecker::displaySignature(const std::string& name, const Signature& sig) const -> std::string {
    // A ConstrainedType param displays as its trait name ("Integer"), not
    // its placeholder var name ("T") — readers want to know what's
    // required, not the table's internal variable naming.
    auto displayType = [](const TypePtr& t) -> std::string {
        if (auto* constrained = std::get_if<ConstrainedType>(&t->kind)) return constrained->traitName;
        return typeToString(t);
    };
    std::string result = name + " : ";
    for (const auto& param : sig.params) {
        result += displayType(param) + " -> ";
    }
    result += displayType(sig.result);
    return result;
}

auto TypeChecker::checkCall(const std::string& name, const std::vector<TypePtr>& argTypes,
                            SourceLocation loc) -> TypePtr {
    const std::vector<Signature>* sigs = m_stdlib.lookup(name);
    if (!sigs) {
        auto it = m_userSignatures.find(name);
        if (it != m_userSignatures.end()) sigs = &it->second;
    }
    if (!sigs) return Type::unknown();  // unknown name, or not yet registered (forward/recursive ref)

    std::vector<const Signature*> arityMatches;
    std::vector<const Signature*> fullMatches;
    for (const auto& sig : *sigs) {
        if (sig.params.size() != argTypes.size()) continue;
        arityMatches.push_back(&sig);

        bool allMatch = true;
        for (size_t i = 0; i < sig.params.size(); i++) {
            if (!argMatchesParam(argTypes[i], sig.params[i])) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) fullMatches.push_back(&sig);
    }

    if (fullMatches.size() == 1) return fullMatches[0]->result;
    if (fullMatches.size() > 1) return fullMatches[0]->result;  // no overlapping cases exist today

    if (arityMatches.empty()) {
        error(loc, "`" + name + "` expects " + std::to_string((*sigs)[0].params.size()) +
              " argument(s), got " + std::to_string(argTypes.size()));
        return (*sigs)[0].result;
    }

    // Zero matches with at least one arity match: find the first
    // mismatching argument against the first arity-matching candidate for
    // the headline message, then list every candidate signature tried,
    // Elm-style.
    const Signature& first = *arityMatches[0];
    std::string detail = "different arguments";
    for (size_t i = 0; i < first.params.size(); i++) {
        if (!argMatchesParam(argTypes[i], first.params[i])) {
            auto* constrained = std::get_if<ConstrainedType>(&first.params[i]->kind);
            std::string expected = constrained ? constrained->traitName : typeToString(first.params[i]);
            detail = "argument " + std::to_string(i + 1) + " to be " + expected +
                      ", but got " + typeToString(argTypes[i]);
            break;
        }
    }

    std::string message = "`" + name + "` expects " + detail;
    for (const auto& sig : *sigs) {
        message += "\n\n" + displaySignature(name, sig);
    }
    error(loc, message);
    return arityMatches[0]->result;
}

auto TypeChecker::pushScope() -> void {
    m_scopeStack.emplace_back();
}

auto TypeChecker::popScope() -> void {
    if (!m_scopeStack.empty()) {
        m_scopeStack.pop_back();
    }
}

auto TypeChecker::defineVar(const std::string& name, TypePtr type) -> void {
    if (!m_scopeStack.empty()) {
        m_scopeStack.back().set(name, std::move(type));
    } else {
        m_globals.set(name, std::move(type));
    }
}

auto TypeChecker::lookupVar(const std::string& name) const -> TypePtr {
    for (auto it = m_scopeStack.rbegin(); it != m_scopeStack.rend(); ++it) {
        if (auto type = it->get(name)) return type;
    }
    return m_globals.get(name);
}

auto TypeChecker::error(SourceLocation loc, const std::string& msg) -> void {
    if (m_diagnostics) {
        m_diagnostics->push_back({Diagnostic::Level::Error, loc, msg});
    }
}

auto TypeChecker::typeMismatch(SourceLocation loc, const TypePtr& expected,
                               const TypePtr& actual) -> void {
    error(loc, "Type mismatch: expected " + typeToString(expected) +
          ", got " + typeToString(actual));
}

auto TypeChecker::freshTypeVar() -> TypePtr {
    return Type::typeVar(m_nextTypeVar++);
}

} // namespace kex::semantic
