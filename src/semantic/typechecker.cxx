#include "typechecker.hxx"
#include "analyzer.hxx"

namespace kex::semantic {

auto TypeChecker::check(const ast::Program& program,
                        std::vector<Diagnostic>& diagnostics) -> void {
    m_diagnostics = &diagnostics;
    m_scopeStack.clear();
    pushScope();

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

    for (const auto& clause : def.clauses) {
        pushScope();
        for (const auto& param : clause.params) {
            if (param.name.has_value() && *param.name != "_") {
                defineVar(*param.name, freshTypeVar());
            }
        }
        auto bodyType = inferBody(clause.body);
        popScope();
    }
}

auto TypeChecker::checkMakeDef(const ast::MakeDef& def) -> void {
    pushScope();
    for (const auto& item : def.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            }
        }, item);
    }
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
                if (clause.guard && *clause.guard) inferExpr(**clause.guard);
                if (clause.body) {
                    auto t = inferExpr(*clause.body);
                    if (std::holds_alternative<UnknownType>(resultType->kind)) {
                        resultType = t;
                    }
                }
                popScope();
            }
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
    auto* sigs = m_stdlib.lookup(name);
    if (!sigs) return Type::unknown();  // user-defined function: not checked yet (phase 5)

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
