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

    registerTypeAliases(program);

    // Register built-in types
    // (done before registerDeclaredSignatures so annotation TypeExprs can resolve these names)
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

    registerDeclaredSignatures(program);
    preRegisterFunctionSigs(program);

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

auto TypeChecker::typeDefToType(const ast::TypeDef& def) -> TypePtr {
    if (!def.variants || def.variants->empty()) return Type::unknown();
    // Build variant types, then fold into a union if more than one.
    std::unordered_map<std::string, TypePtr> noGenerics;
    std::vector<TypePtr> parts;
    for (const auto& v : *def.variants) {
        if (v) parts.push_back(resolveTypeExpr(*v, noGenerics));
    }
    if (parts.empty()) return Type::unknown();
    if (parts.size() == 1) return parts[0];
    return std::make_shared<Type>(Type{UnionType{std::move(parts)}});
}

auto TypeChecker::registerTypeAliases(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                // Only register as alias if no variant is constructor-shaped.
                for (const auto& v : *node->variants) {
                    if (extractConstructorName(v)) return; // ADT, not an alias
                }
                m_typeAliases[node->name] = typeDefToType(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerTypeAliasesInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::registerTypeAliasesInModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                for (const auto& v : *node->variants) {
                    if (extractConstructorName(v)) return;
                }
                m_typeAliases[node->name] = typeDefToType(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerTypeAliasesInModule(*node);
            }
        }, item);
    }
}

auto TypeChecker::annotationToSignature(const ast::TypeAnnotation& ann) -> std::optional<Signature> {
    if (!ann.type) return std::nullopt;
    // Unroll `A -> B -> C` (right-nested FunctionType) into params=[A,B], result=C.
    std::unordered_map<std::string, TypePtr> genericVars;
    std::vector<TypePtr> params;
    const ast::TypeExpr* cur = ann.type.get();
    while (cur) {
        if (auto* ft = std::get_if<ast::FunctionType>(&cur->kind)) {
            params.push_back(ft->param ? resolveTypeExpr(*ft->param, genericVars) : Type::unknown());
            cur = ft->result.get();
        } else {
            break;
        }
    }
    TypePtr result = cur ? resolveTypeExpr(*cur, genericVars) : Type::unknown();
    if (params.empty()) {
        // Non-function annotation (e.g. `x : Int`) — treat as a zero-param
        // constant whose type IS the annotated type.
        return Signature{ann.name, {}, result};
    }
    return Signature{ann.name, std::move(params), result};
}

auto TypeChecker::registerDeclaredSignatures(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
            if (!*ann) continue;
            auto sig = annotationToSignature(**ann);
            if (!sig) continue;
            // Declared annotation wins — stored first so checkFunctionDef
            // can find it and verify the body against the declared type.
            m_annotationDeclared.insert((*ann)->name);
            auto& sigs = m_userSignatures[(*ann)->name];
            // Insert declared sig at front, replacing any same-arity inferred
            // one that was somehow already there (shouldn't happen in pre-pass
            // ordering, but guard for safety).
            sigs.insert(sigs.begin(), std::move(*sig));
        }
    }
}

auto TypeChecker::preRegisterFunctionSigs(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) preRegisterFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (!node) return;
                for (const auto& modItem : node->body) {
                    std::visit([this](const auto& mn) {
                        using MT = std::decay_t<decltype(mn)>;
                        if constexpr (std::is_same_v<MT, std::unique_ptr<ast::FunctionDef>>) {
                            if (mn) preRegisterFunctionDef(*mn);
                        }
                    }, modItem);
                }
            }
        }, item);
    }
}

auto TypeChecker::preRegisterFunctionDef(const ast::FunctionDef& def) -> void {
    // Skip make-block methods (they have an implicit `this` that mis-counts
    // arity in checkCall's UFCS desugaring, same exclusion as checkFunctionDef).
    if (m_inMakeBlock) return;
    // Skip annotation-declared functions — registerDeclaredSignatures already
    // populated m_userSignatures for these and checkFunctionDef will use the
    // annotation as ground truth.
    if (m_annotationDeclared.count(def.name)) return;
    // Skip if already pre-registered (e.g. multi-clause function — all clauses
    // share the same def.name and the first call handles them all).
    if (m_userSignatures.count(def.name)) return;

    std::vector<Signature> provisional;
    for (const auto& clause : def.clauses) {
        std::unordered_map<std::string, TypePtr> genericVars;
        std::vector<TypePtr> paramTypes;
        for (const auto& param : clause.params) {
            paramTypes.push_back(
                param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar());
        }
        provisional.push_back(Signature{def.name, std::move(paramTypes), freshTypeVar()});
    }
    m_userSignatures[def.name] = std::move(provisional);
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
            std::string ctorName;
            if (auto* ctor = std::get_if<ast::ConstructorPattern>(&pat->kind)) {
                ctorName = ctor->name;
            } else if (auto* lit = std::get_if<ast::LiteralPattern>(&pat->kind);
                       lit && lit->literal.type == TokenType::None) {
                // `None` lexes as its own TokenType::None (lexer.cxx), so
                // it parses as a LiteralPattern, not ConstructorPattern{
                // "None"} — treat it as the Option constructor it is.
                ctorName = "None";
            } else {
                continue;  // literal/list/tuple/record/range patterns don't drive ADT exhaustiveness
            }

            auto it = m_adtOfConstructor.find(ctorName);
            if (it == m_adtOfConstructor.end()) {
                inconclusive = true;  // unregistered constructor — can't prove the closed set
                continue;
            }
            if (adtName.empty()) adtName = it->second;
            else if (adtName != it->second) inconclusive = true;  // patterns span more than one ADT
            if (!guarded) covered.insert(ctorName);
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
            // Trait-only names (Float, Number, Comparable, ...) have no
            // concrete Type — m_globals deliberately has no entry for them
            // (see check()'s comment) — so a param annotated `Float` means
            // "any T satisfying Float", same as an explicit constraint.
            if (m_traits.get(last)) return Type::constrained(last, last);
            // User type alias (e.g. `type Level = :debug | :info | ...`)
            auto aliasIt = m_typeAliases.find(last);
            if (aliasIt != m_typeAliases.end()) return aliasIt->second;
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
    // A "declared" signature is one from a standalone TypeAnnotation
    // (`fact : Integer -> Integer`) — tracked in m_annotationDeclared.
    // Pre-registered provisional sigs (for forward-reference/recursion) are
    // in m_userSignatures but NOT in m_annotationDeclared, so they don't
    // affect param-type selection or return-type verification here.
    auto* declared = [&]() -> const Signature* {
        if (!m_annotationDeclared.count(def.name)) return nullptr;
        auto it = m_userSignatures.find(def.name);
        if (it != m_userSignatures.end() && !it->second.empty()) return &it->second[0];
        return nullptr;
    }();

    auto returnType = declared ? declared->result : freshTypeVar();
    defineVar(def.name, returnType);

    std::vector<Signature> signatures;
    for (size_t ci = 0; ci < def.clauses.size(); ci++) {
        const auto& clause = def.clauses[ci];
        pushScope();
        std::unordered_map<std::string, TypePtr> genericVars;
        std::vector<TypePtr> paramTypes;
        for (size_t pi = 0; pi < clause.params.size(); pi++) {
            const auto& param = clause.params[pi];
            // Use declared param type if available and the annotation covers
            // this position; fall back to inline annotation or fresh TypeVar.
            TypePtr paramType;
            if (declared && pi < declared->params.size() && !param.type) {
                paramType = declared->params[pi];
            } else {
                paramType = param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar();
            }
            paramTypes.push_back(paramType);
            if (param.name.has_value() && *param.name != "_") {
                defineVar(*param.name, paramType);
            }
            if (param.pattern) {
                bindPatternVars(**param.pattern);
            }
        }
        auto bodyType = inferBody(clause.body);
        // Verify body matches declared return type (if declared and concrete).
        // Use argMatchesParam (not typesEqual) to apply the same trait-family
        // relaxations that call-site checking uses — e.g. Int and Integer are
        // compatible, so `add : Int -> Int -> Int` with a body returning
        // Integer (inferred from literal arithmetic) isn't a mismatch.
        if (declared && !std::holds_alternative<TypeVar>(declared->result->kind) &&
            !std::holds_alternative<UnknownType>(bodyType->kind) &&
            !std::holds_alternative<TypeVar>(bodyType->kind) &&
            !argMatchesParam(bodyType, declared->result)) {
            error(def.location,
                  "`" + def.name + "` declared to return " + typeToString(declared->result) +
                  " but body returns " + typeToString(bodyType));
        }
        popScope();
        signatures.push_back(Signature{def.name, std::move(paramTypes), bodyType});
    }

    // make-block methods have an implicit `this` receiver, not a regular
    // param — checkCall's UFCS desugaring (receiver as argument 0) would
    // mis-count their arity, so they're checked (body inference still
    // runs above) but not registered for call-site checking.
    if (!m_inMakeBlock) {
        // If a declared signature already exists, update its result type with
        // the inferred one (keeping declared params) and keep one entry.
        if (declared) {
            auto& sigs = m_userSignatures[def.name];
            // Replace the placeholder declared sig with the fully-checked one.
            if (!signatures.empty()) sigs[0] = signatures[0];
        } else {
            m_userSignatures[def.name] = std::move(signatures);
        }
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
    if (!block.synthetic) pushScope();
    for (const auto& param : block.params) {
        if (param.name.has_value() && *param.name != "_") {
            defineVar(*param.name, freshTypeVar());
        }
        if (param.pattern) {
            bindPatternVars(**param.pattern);
        }
    }
    inferBody(block.body);
    if (!block.synthetic) popScope();
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
            } else if (node.pattern) {
                // Other destructuring shapes (`let { host, port } = ...`,
                // `let (a, b) = ...`) don't have a single value type to
                // propagate per binding — fresh type vars, same fallback
                // bindPatternVars uses everywhere else.
                bindPatternVars(*node.pattern);
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
                if (!argMatchesParam(valueType, varType) &&
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
            if (node.block) {
                inferExpr(**node.block);
                // A trailing `do...end`/`{ }` block fills the callee's last
                // param slot (e.g. `times(3) do ... end` is 2 arguments to
                // `times(n, block)`, not 1) — Block<T> isn't structurally
                // modeled yet (resolveTypeExpr's BlockType case), so this is
                // a permissive placeholder, not the block's real type.
                argTypes.push_back(Type::unknown());
            }
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
            if (node.block) {
                inferExpr(**node.block);
                argTypes.push_back(Type::unknown());
            }
            return checkCall(node.method, argTypes, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            TypePtr elemType = Type::unknown();
            for (const auto& elem : node.elements) {
                if (elem) {
                    auto t = inferExpr(*elem);
                    bool elemPermissive = std::holds_alternative<UnknownType>(elemType->kind) ||
                                         std::holds_alternative<TypeVar>(elemType->kind);
                    bool tPermissive = std::holds_alternative<UnknownType>(t->kind) ||
                                       std::holds_alternative<TypeVar>(t->kind);
                    if (elemPermissive) {
                        elemType = t; // adopt the concrete type if we have one
                    } else if (!tPermissive && !typesEqual(elemType, t)) {
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

    // Recurse into compound types structurally rather than requiring exact
    // equality — e.g. `[Int]` vs `[Integer]` (the relaxation above, but
    // inside a list) and `String` (= `[Char]`) vs a generic `[A]` param
    // both need this, not just bare params.
    if (auto* paramList = std::get_if<ListType>(&paramType->kind)) {
        auto* argList = std::get_if<ListType>(&argType->kind);
        return argList && argMatchesParam(argList->element, paramList->element);
    }
    if (auto* paramTuple = std::get_if<TupleType>(&paramType->kind)) {
        auto* argTuple = std::get_if<TupleType>(&argType->kind);
        if (!argTuple || argTuple->elements.size() != paramTuple->elements.size()) return false;
        for (size_t i = 0; i < paramTuple->elements.size(); i++) {
            if (!argMatchesParam(argTuple->elements[i], paramTuple->elements[i])) return false;
        }
        return true;
    }
    if (auto* paramMap = std::get_if<MapType>(&paramType->kind)) {
        auto* argMap = std::get_if<MapType>(&argType->kind);
        return argMap && argMatchesParam(argMap->key, paramMap->key) &&
               argMatchesParam(argMap->value, paramMap->value);
    }
    if (auto* paramOpt = std::get_if<OptionalType>(&paramType->kind)) {
        auto* argOpt = std::get_if<OptionalType>(&argType->kind);
        return argOpt && argMatchesParam(argOpt->inner, paramOpt->inner);
    }
    // FunctionType param — e.g. `(T-1) -> Bool` vs `(T81) -> Bool`. Without
    // this branch both sides fall to typesEqual which fails on mismatched
    // TypeVar ids even though both are permissive. Recurse into params and
    // result so lambda arguments to map/filter/each pass the checker.
    if (auto* paramFn = std::get_if<FuncType>(&paramType->kind)) {
        auto* argFn = std::get_if<FuncType>(&argType->kind);
        if (!argFn) return isPermissive(argType);
        if (argFn->params.size() == paramFn->params.size()) {
            for (size_t i = 0; i < paramFn->params.size(); i++) {
                if (!argMatchesParam(argFn->params[i], paramFn->params[i])) return false;
            }
            return argMatchesParam(argFn->result, paramFn->result);
        }
        // Curried arg: `(A) -> (B) -> C` matches `(A, B) -> C`.
        // Haskell-style annotations write multi-arg callbacks as `B -> A -> B`;
        // the stdlib signatures use multi-param FuncType.
        if (argFn->params.size() == 1 && paramFn->params.size() > 1) {
            if (auto* inner = std::get_if<FuncType>(&argFn->result->kind)) {
                if (inner->params.size() == paramFn->params.size() - 1) {
                    if (!argMatchesParam(argFn->params[0], paramFn->params[0])) return false;
                    for (size_t i = 0; i < inner->params.size(); i++) {
                        if (!argMatchesParam(inner->params[i], paramFn->params[i + 1])) return false;
                    }
                    return argMatchesParam(inner->result, paramFn->result);
                }
            }
        }
        return false;
    }
    // UnionType param (e.g. type alias `Level = :a | :b | :c`) — arg
    // matches if it matches any branch of the union.
    if (auto* paramUnion = std::get_if<UnionType>(&paramType->kind)) {
        for (const auto& member : paramUnion->members) {
            if (member && argMatchesParam(argType, member)) return true;
        }
        return false;
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
    const std::vector<Signature>* sigs = m_stdlib.lookup(name);
    if (!sigs) {
        auto it = m_userSignatures.find(name);
        if (it != m_userSignatures.end()) sigs = &it->second;
    }
    if (!sigs) return Type::unknown();  // unknown name, or not yet registered (forward/recursive ref)

    // `let hello = makeGreeter("Hello")` is a top-level zero-arg binding
    // (every top-level `let NAME = EXPR` is a 0-param function — see
    // Parser::parseFunctionDef), and referencing it as a bare identifier
    // auto-calls it (Evaluator::autoCallZeroArgConstant). `hello("Alice")`
    // is the same idiom through a call: auto-call `hello` to get the
    // closure `makeGreeter` returned, then apply "Alice" to *that* — not
    // "call hello with 1 argument," which is what zero arity would
    // otherwise mean. Only a single, unambiguous 0-param signature
    // triggers this (an overload set with a 0-param AND non-0-param
    // signature is a real arity question, not this idiom).
    if (sigs->size() == 1 && (*sigs)[0].params.empty() && !argTypes.empty()) {
        return Type::unknown();
    }

    // `This` (the trait placeholder — see Surface syntax for declaring a
    // trait in the plan) has no substitution mechanism implemented yet, so
    // it can leak through as a literal NamedType("This") from a `param :
    // This` annotation inside a `make` block. A call involving it can't be
    // meaningfully checked against any signature here — reporting a
    // mismatch against the wrong (stdlib or unrelated) candidate would be
    // actively misleading, so bail out rather than guess.
    for (const auto& argType : argTypes) {
        if (auto* named = std::get_if<NamedType>(&argType->kind); named && named->name == "This") {
            return Type::unknown();
        }
    }

    // Namespace call heuristic: `BuiltIn.foo(x)` or any call where the
    // receiver resolves to Unknown (an undefined namespace sentinel like
    // `BuiltIn`). The UFCS desugaring adds the receiver as argTypes[0], but
    // if the receiver is Unknown and no overload matches the full arity while
    // overloads DO match with the receiver dropped, treat it as a plain
    // function call through a namespace — drop argTypes[0] and re-check.
    if (!argTypes.empty() &&
        (std::holds_alternative<UnknownType>(argTypes[0]->kind) ||
         std::holds_alternative<TypeVar>(argTypes[0]->kind))) {
        bool anyFullArityMatch = false;
        bool anyDroppedArityMatch = false;
        for (const auto& sig : *sigs) {
            if (sig.params.size() == argTypes.size()) anyFullArityMatch = true;
            if (sig.params.size() == argTypes.size() - 1) anyDroppedArityMatch = true;
        }
        if (!anyFullArityMatch && anyDroppedArityMatch) {
            return checkCall(name, {argTypes.begin() + 1, argTypes.end()}, loc);
        }
    }

    // Name collision guard: a make-block method isn't registered here at
    // all (see checkFunctionDef), so `this.modulo(...)` inside a
    // user-defined `make CustomType do let modulo(...) ... end` would
    // otherwise get checked against the *stdlib* `modulo`'s signature
    // purely because the names match — a different, unrelated function.
    // Only kick in when the receiver/first argument is itself a NamedType
    // (a record/ADT/custom-type value) — a builtin-primitive mismatch
    // (e.g. `even?('c')`) must still error normally; this only suppresses
    // the case where nothing in the known overload set could ever apply.
    if (!argTypes.empty() && std::holds_alternative<NamedType>(argTypes[0]->kind)) {
        bool anyFirstParamPlausible = false;
        for (const auto& sig : *sigs) {
            if (!sig.params.empty() && argMatchesParam(argTypes[0], sig.params[0])) {
                anyFirstParamPlausible = true;
                break;
            }
        }
        if (!anyFirstParamPlausible) return Type::unknown();
    }

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
