#include "evaluator.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"
#include <iostream>

namespace kex::interpreter {

Evaluator::Evaluator() {
    m_globalEnv = std::make_shared<Environment>();
    m_env = m_globalEnv;
    registerBuiltins();
}

auto Evaluator::execute(const ast::Program& program) -> ValuePtr {
    ValuePtr lastResult = Value::none();

    for (const auto& item : program.items) {
        execTopLevel(item);
    }

    for (const auto& item : program.items) {
        if (auto* main = std::get_if<std::unique_ptr<ast::MainBlock>>(&item)) {
            lastResult = execMainBlock(**main);
        }
    }

    // describe/it/assert summary — only printed if any `it` ran, so
    // programs that don't use the testing DSL see no extra output.
    if (m_testsPassed + m_testsFailed > 0) {
        std::string summary = "\n" + std::to_string(m_testsPassed) + " passed, "
            + std::to_string(m_testsFailed) + " failed\n";
        m_output += summary;
        std::cout << summary;
    }

    return lastResult;
}

auto Evaluator::setReplMode(bool enabled) -> void {
    m_replMode = enabled;
}

auto Evaluator::setArgs(std::vector<std::string> args) -> void {
    m_scriptArgs = std::move(args);
}

auto Evaluator::output() const -> const std::string& {
    return m_output;
}

auto Evaluator::execTopLevel(const ast::TopLevelItem& item) -> void {
    std::visit([this](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
            execModule(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
            execFunctionDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
            execMakeDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
            execRecordDef(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
            execTypeDef(*node);
        }
    }, item);
}

auto Evaluator::execModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                execModule(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                execTypeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                execRecordDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                execMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                execVisibilityBlock(*node);
            }
            // CompiledBlock/UsingBlock: not implemented yet (separate,
            // larger features — metaprogramming and module imports).
        }, item);
    }
}

auto Evaluator::execTypeDef(const ast::TypeDef& def) -> void {
    if (def.staticBlock) {
        // Static constructors/constants are namespaced under the type
        // (Vector2D.Polar(...), not bare Polar(...)) — see docs/functions.md
        // "Static Functions (Constructors)".
        for (const auto& func : def.staticBlock->functions) {
            execFunctionDef(*func, def.name);
        }
    }
    // Register sum-type variant constructors with args (Just(A), Ok(A),
    // Error(E), Number(Int), ...) as callable functions that build a
    // RecordValue with positional fields "0", "1", .... Zero-arg variants
    // (Fizz, Nothing, ...) need no constructor registration — they already
    // work as values and as patterns via the UpperIdentifier-as-Atom
    // fallback in eval()/matchPattern(). Both kinds still need an entry in
    // m_variantParent so `make TypeName do ... end` method dispatch can map
    // the variant tag back to the declaring type.
    if (def.variants) {
        for (const auto& variant : *def.variants) {
            if (!variant) continue;
            std::string variantName;
            size_t arity = 0;
            if (auto* generic = std::get_if<ast::GenericType>(&variant->kind)) {
                if (generic->name.parts.empty()) continue;
                variantName = generic->name.parts.back();
                arity = generic->args.size();
            } else if (auto* plain = std::get_if<ast::TypeName>(&variant->kind)) {
                if (plain->parts.empty()) continue;
                variantName = plain->parts.back();
            } else {
                continue;
            }

            m_variantParent[variantName] = def.name;

            if (arity == 0) continue;
            auto val = std::make_shared<Value>();
            val->data = FunctionValue{variantName,
                [variantName, arity](std::vector<ValuePtr> args) -> ValuePtr {
                    std::unordered_map<std::string, ValuePtr> fields;
                    for (size_t i = 0; i < arity; i++) {
                        fields[std::to_string(i)] = i < args.size() ? args[i] : Value::none();
                    }
                    return Value::record(variantName, std::move(fields));
                }};
            m_env->define(variantName, val);
        }
    }
}

auto Evaluator::execRecordDef(const ast::RecordDef& def) -> void {
    // Register record name as a namespace for static access (e.g. Vector2D.Polar)
    m_env->define(def.name, Value::record(def.name, {}));
    m_recordDefs[def.name] = &def;
    if (def.staticBlock) {
        // Static constructors/constants are namespaced under the record
        // (Vector2D.Polar(...), not bare Polar(...)) — see docs/functions.md
        // "Static Functions (Constructors)".
        for (const auto& func : def.staticBlock->functions) {
            execFunctionDef(*func, def.name);
        }
    }
}

auto Evaluator::execVisibilityBlock(const ast::VisibilityBlock& block, const std::string& typeScope) -> void {
    // Visibility (public/private) isn't enforced anywhere yet (see
    // semantic/analyzer.cxx's "TODO: handle visibility") — both kinds are
    // registered identically; only their accessibility differs, which
    // isn't checked at this stage.
    for (const auto& item : block.items) {
        std::visit([this, &typeScope](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node, typeScope);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                execMakeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                execTypeDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                execRecordDef(*node);
            }
            // TypeAnnotation: semantic-only, nothing to execute.
        }, item);
    }
}

auto Evaluator::execFunctionDef(const ast::FunctionDef& def, const std::string& typeScope) -> void {
    // Collect clauses: if there's already a function with this name, merge
    auto existing = m_env->get(def.name);
    std::vector<const ast::FunctionClause*> allClauses;

    // Get clauses from existing definition if it's the same function
    if (existing) {
        if (auto* fv = std::get_if<FunctionValue>(&existing->data)) {
            if (fv->name == def.name && fv->native) {
                // It's a previous definition with clauses stored in the closure
                // We'll rebuild with all clauses
            }
        }
    }

    // We store pointers to all known clauses for this function
    // Since the AST is stable, we collect them via a vector in the closure
    // For multi-def functions, each call to execFunctionDef appends
    struct ClauseStore {
        std::vector<const ast::FunctionDef*> defs;
    };
    auto store = std::make_shared<ClauseStore>();

    // If existing, retrieve its store
    if (existing) {
        // Can't easily retrieve — just rebuild from this def
    }

    store->defs.push_back(&def);

    // Check if already defined — merge
    if (existing) {
        if (auto* fv = std::get_if<FunctionValue>(&existing->data)) {
            // Unwrap and rebuild — for simplicity, use a global registry
        }
    }

    // Register under mangled name if in a type scope
    std::string regName = def.name;
    if (!typeScope.empty()) {
        regName = typeScope + "::" + def.name;
    }

    auto& funcDefs = m_functionDefs[regName];
    funcDefs.push_back(&def);

    auto funcValue = std::make_shared<Value>();
    auto* defsPtr = &funcDefs;
    funcValue->data = FunctionValue{def.name, [this, defsPtr](std::vector<ValuePtr> args) -> ValuePtr {
        for (const auto* funcDef : *defsPtr) {
            for (const auto& clause : funcDef->clauses) {
                pushEnv();
                bool matched = true;

                // If more args than params, first arg is 'this' (UFCS method call)
                size_t argOffset = 0;
                if (args.size() > clause.params.size()) {
                    m_env->define("this", args[0]);
                    argOffset = 1;
                }

                for (size_t i = 0; i < clause.params.size() && (i + argOffset) < args.size(); i++) {
                    const auto& param = clause.params[i];
                    if (param.pattern && *param.pattern) {
                        if (!matchPattern(**param.pattern, args[i + argOffset])) {
                            matched = false;
                            break;
                        }
                    } else if (param.name.has_value()) {
                        m_env->define(*param.name, args[i + argOffset]);
                    }
                }

                if (matched) {
                    // catch(...) (not just ReturnException) so a RuntimeError
                    // — e.g. a failed `assert` caught higher up by `it` — still
                    // pops this scope before propagating; otherwise m_env
                    // leaks one level deep for the rest of the program (see
                    // the identical guard on the MatchExpr clause loop above).
                    try {
                        auto result = evalBody(clause.body);
                        popEnv();
                        return result;
                    } catch (ReturnException& ret) {
                        popEnv();
                        return ret.value();
                    } catch (...) {
                        popEnv();
                        throw;
                    }
                }

                popEnv();
            }
        }

        return Value::none();
    }};

    m_env->define(regName, funcValue);
}

auto Evaluator::execMakeDef(const ast::MakeDef& def) -> void {
    // Extract type name from make target
    std::string typeName;
    if (def.target) {
        if (auto* named = std::get_if<ast::TypeName>(&def.target->kind)) {
            if (!named->parts.empty()) typeName = named->parts[0];
        } else if (auto* generic = std::get_if<ast::GenericType>(&def.target->kind)) {
            if (!generic->name.parts.empty()) typeName = generic->name.parts[0];
        } else if (std::holds_alternative<ast::ListType>(def.target->kind)) {
            typeName = "List";
        }
    }

    for (const auto& item : def.body) {
        std::visit([this, &typeName](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                execFunctionDef(*node, typeName);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                // `private do ... end` / `public do ... end` inside a make
                // block — methods defined there still belong to typeName.
                execVisibilityBlock(*node, typeName);
            }
            // TypeAnnotation: semantic-only, nothing to execute.
        }, item);
    }
}

auto Evaluator::execMainBlock(const ast::MainBlock& block) -> ValuePtr {
    if (!m_replMode) pushEnv();
    // main(args) do ... end — bind the script's command-line arguments
    // ([String], set via setArgs()) to the declared parameter, if any.
    if (!block.params.empty()) {
        std::vector<ValuePtr> elems;
        for (const auto& arg : m_scriptArgs) elems.push_back(Value::string(arg));
        auto argsValue = Value::list(std::move(elems));
        const auto& param = block.params[0];
        if (param.pattern && *param.pattern) {
            matchPattern(**param.pattern, argsValue);
        } else if (param.name) {
            m_env->define(*param.name, argsValue);
        }
    }
    ValuePtr result;
    try {
        result = evalBody(block.body);
    } catch (ReturnException& ret) {
        result = ret.value();
    }
    if (!m_replMode) popEnv();
    return result;
}

auto Evaluator::evalBody(const std::vector<ast::ExprPtr>& body) -> ValuePtr {
    ValuePtr last = Value::none();
    for (const auto& expr : body) {
        if (expr) last = eval(*expr);
    }
    return last;
}

auto Evaluator::eval(const ast::Expr& expr) -> ValuePtr {
    return std::visit([this, &expr](const auto& node) -> ValuePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::IntLiteral>) {
            return Value::integer(std::stoll(node.value));
        }
        else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
            return Value::floating(std::stod(node.value));
        }
        else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            // Handle interpolation: find ${...} and evaluate in current scope
            std::string result;
            const auto& s = node.value;
            size_t i = 0;
            while (i < s.size()) {
                if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '{') {
                    i += 2;
                    std::string inner;
                    int depth = 1;
                    while (i < s.size() && depth > 0) {
                        if (s[i] == '{') depth++;
                        else if (s[i] == '}') { depth--; if (depth == 0) break; }
                        inner += s[i];
                        i++;
                    }
                    if (i < s.size()) i++; // skip }
                    // Parse the expression and evaluate directly in current env
                    try {
                        kex::Lexer interpLexer(inner);
                        auto interpTokens = interpLexer.tokenizeAll();
                        // Parse as a single expression
                        kex::Parser interpParser(std::move(interpTokens));
                        auto interpExpr = interpParser.parseExpr();
                        if (interpExpr) {
                            auto val = eval(*interpExpr);
                            result += val->toString();
                        }
                    } catch (...) {
                        // Fallback: try as variable lookup
                        auto val = m_env->get(inner);
                        result += val ? val->toString() : inner;
                    }
                } else {
                    result += s[i];
                    i++;
                }
            }
            return Value::string(result);
        }
        else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
            return Value::character(node.value);
        }
        else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
            return Value::boolean(node.value);
        }
        else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
            return Value::atom(node.name);
        }
        else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
            auto val = m_env->get("this");
            if (!val) {
                throw RuntimeError("'this' used outside of a method context", expr.location);
            }
            return val;
        }
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
            auto val = m_env->get(node.name);
            if (!val) {
                throw RuntimeError("Undefined variable: " + node.name, expr.location);
            }
            return autoCallZeroArgConstant(node.name, val);
        }
        else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            auto value = node.value ? eval(*node.value) : Value::none();
            if (auto* varPat = std::get_if<ast::VarPattern>(&node.pattern->kind)) {
                m_env->define(varPat->name, value);
            } else if (auto* tuplePat = std::get_if<ast::TuplePattern>(&node.pattern->kind)) {
                if (auto* tupleVal = std::get_if<TupleValue>(&value->data)) {
                    // Delegate each element to the general matchPattern() so
                    // nested patterns (e.g. `let (JsonString(key), rest) =
                    // ...`, a ConstructorPattern element) bind correctly —
                    // not just bare variable names.
                    for (size_t i = 0; i < tuplePat->elements.size() && i < tupleVal->elements.size(); i++) {
                        matchPattern(*tuplePat->elements[i], tupleVal->elements[i]);
                    }
                }
            } else if (auto* recPat = std::get_if<ast::RecordPattern>(&node.pattern->kind)) {
                // `field.pattern` holds the rename/sub-pattern for
                // `{ "key": shortName }` or `{ field: subPattern }` — must
                // bind/recurse through it when present; only fall back to
                // `field.name` (the key itself) for the shorthand `{ name }`
                // form with no explicit pattern.
                if (auto* recVal = std::get_if<RecordValue>(&value->data)) {
                    for (const auto& field : recPat->fields) {
                        if (auto it = recVal->fields.find(field.name); it != recVal->fields.end()) {
                            if (field.pattern && *field.pattern) {
                                matchPattern(**field.pattern, it->second);
                            } else {
                                m_env->define(field.name, it->second);
                            }
                        }
                    }
                } else if (auto* mapVal = std::get_if<MapValue>(&value->data)) {
                    for (const auto& field : recPat->fields) {
                        for (const auto& [k, v] : mapVal->entries) {
                            if (auto* sk = std::get_if<StringValue>(&k->data)) {
                                if (sk->value == field.name) {
                                    if (field.pattern && *field.pattern) {
                                        matchPattern(**field.pattern, v);
                                    } else {
                                        m_env->define(field.name, v);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            return value;
        }
        else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            auto value = node.value ? eval(*node.value) : Value::none();
            m_env->define(node.name, value, /*isMutable=*/true);
            return value;
        }
        else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            auto value = node.value ? eval(*node.value) : Value::none();
            if (!m_env->has(node.name)) {
                throw RuntimeError("Undefined variable: " + node.name, expr.location);
            }
            if (!m_env->isMutable(node.name)) {
                throw RuntimeError("Cannot assign to immutable binding: " + node.name, expr.location);
            }
            m_env->set(node.name, value);
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            auto left = node.left ? eval(*node.left) : Value::none();
            auto right = node.right ? eval(*node.right) : Value::none();
            return evalBinaryOp(node.op, left, right, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            auto operand = node.operand ? eval(*node.operand) : Value::none();
            return evalUnaryOp(node.op, operand, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            std::vector<ValuePtr> args;
            for (const auto& arg : node.args) {
                args.push_back(arg ? eval(*arg) : Value::none());
            }
            NamedArgs namedArgs;
            for (const auto& [name, val] : node.namedArgs) {
                namedArgs.push_back({name, val ? eval(*val) : Value::none()});
            }
            // Handle block as last arg (lambda)
            if (node.block) {
                args.push_back(eval(**node.block));
            }
            return callFunction(node.name, std::move(args), std::move(namedArgs), expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            auto receiver = node.receiver ? eval(*node.receiver) : Value::none();

            // Namespace access: empty-record namespace placeholders (File, IO,
            // record-static namespaces like Vector2D) OR a bare UpperIdentifier
            // receiver that isn't bound to anything (Stream, Math, etc.).
            //
            // The UpperIdentifier case matters: without it, an unresolved
            // namespace identifier evaluates (via ast::UpperIdentifier's eval
            // fallback) to an Atom, which would otherwise fall through to the
            // generic UFCS path below and get silently prepended as args[0],
            // corrupting calls like Stream.Sequence(from: 0) { ... }.take(3)
            // ("Cannot add Atom and Int"). Treating it as a namespace call
            // instead means it either dispatches correctly (if a matching
            // mangled or plain function exists) or fails with a clear
            // "Undefined function" error — never silent corruption.
            std::string namespaceName;
            bool isNamespaceCall = false;
            if (auto* rec = std::get_if<RecordValue>(&receiver->data)) {
                if (rec->fields.empty()) {
                    isNamespaceCall = true;
                    namespaceName = rec->typeName;
                }
            }
            if (!isNamespaceCall) {
                if (auto* upperIdent = std::get_if<ast::UpperIdentifier>(&node.receiver->kind)) {
                    // A known zero-arg ADT variant tag (Nothing, Fizz, ...)
                    // is a value being used as a UFCS receiver, not a
                    // namespace — let it fall through to the generic UFCS
                    // path below (as an Atom) so receiverType resolution
                    // there can map it to its declaring type.
                    bool isKnownVariant = m_variantParent.count(upperIdent->name) > 0;
                    if (!isKnownVariant && !m_env->get(upperIdent->name)) {
                        isNamespaceCall = true;
                        namespaceName = upperIdent->name;
                    }
                }
            }
            if (isNamespaceCall) {
                // Namespace call: Stream.Sequence(...), Math.PI, File.read(...), etc.
                std::vector<ValuePtr> args;
                for (const auto& arg : node.args) {
                    args.push_back(arg ? eval(*arg) : Value::none());
                }
                NamedArgs namedArgs;
                for (const auto& [name, val] : node.namedArgs) {
                    namedArgs.push_back({name, val ? eval(*val) : Value::none()});
                }
                if (node.block) {
                    args.push_back(eval(**node.block));
                }
                // Prefer the mangled "Namespace::method" name (e.g. "IO::putLine")
                // so namespaced builtins can't collide with unrelated plain-name
                // globals. Falls back to the plain name for namespaces that were
                // registered without a mangled prefix (e.g. Stream.Sequence).
                std::string dispatchName = node.method;
                auto mangled = namespaceName + "::" + node.method;
                if (m_env->get(mangled)) dispatchName = mangled;
                return callFunction(dispatchName, std::move(args), std::move(namedArgs), expr.location);
            }

            // Field access on records: receiver.field (no args, no parens)
            if (node.args.empty() && !node.block && !node.mutating) {
                if (auto* rec = std::get_if<RecordValue>(&receiver->data)) {
                    auto it = rec->fields.find(node.method);
                    if (it != rec->fields.end()) {
                        return it->second;
                    }
                }
            }

            std::vector<ValuePtr> args;
            args.push_back(receiver); // UFCS: receiver is first arg
            for (const auto& arg : node.args) {
                args.push_back(arg ? eval(*arg) : Value::none());
            }
            if (node.block) {
                args.push_back(eval(**node.block));
            }

            NamedArgs namedArgs;
            for (const auto& [name, val] : node.namedArgs) {
                namedArgs.push_back({name, val ? eval(*val) : Value::none()});
            }

            // Type-based dispatch: try TypeName::method first
            std::string mangledName = node.method;
            std::string receiverType;
            if (auto* rec = std::get_if<RecordValue>(&receiver->data)) {
                receiverType = rec->typeName;
            } else if (auto* atom = std::get_if<AtomValue>(&receiver->data)) {
                // Zero-arg ADT variant tags (Nothing, Fizz, ...) are Atoms —
                // needed so `make TypeName do ... end` methods registered
                // under "TypeName::method" can be found via m_variantParent
                // below even though the value itself carries no type name.
                receiverType = atom->name;
            } else if (std::holds_alternative<ListValue>(receiver->data)) {
                receiverType = "List";
            } else if (std::holds_alternative<MapValue>(receiver->data)) {
                receiverType = "Map";
            } else if (std::holds_alternative<IntValue>(receiver->data)) {
                receiverType = "Integer";
            } else if (std::holds_alternative<FloatValue>(receiver->data)) {
                receiverType = "Float";
            } else if (std::holds_alternative<StringValue>(receiver->data)) {
                receiverType = "String";
            }
            if (!receiverType.empty()) {
                auto typed = receiverType + "::" + node.method;
                if (m_env->get(typed)) {
                    mangledName = typed;
                } else {
                    // receiverType may be a variant tag (Just, Ok, Nothing,
                    // ...) rather than the type that declared it (Option,
                    // Result, ...) — methods from `make TypeName do ... end`
                    // are registered under the declared type's name, so
                    // fall back to that mapping before giving up.
                    auto parentIt = m_variantParent.find(receiverType);
                    if (parentIt != m_variantParent.end()) {
                        auto typedByParent = parentIt->second + "::" + node.method;
                        if (m_env->get(typedByParent)) {
                            mangledName = typedByParent;
                        }
                    }
                }
            }

            // For mutating calls, reassign back
            if (node.mutating) {
                auto* ident = std::get_if<ast::Identifier>(&node.receiver->kind);
                if (!ident) {
                    throw RuntimeError("'!' requires a variable binding as the receiver", expr.location);
                }
                if (!m_env->has(ident->name)) {
                    throw RuntimeError("Undefined variable: " + ident->name, expr.location);
                }
                if (!m_env->isMutable(ident->name)) {
                    throw RuntimeError("Cannot use '!' on immutable binding: " + ident->name, expr.location);
                }
                auto result = callFunction(mangledName, args, namedArgs, expr.location);
                m_env->set(ident->name, result);
                return result;
            }

            return callFunction(mangledName, std::move(args), std::move(namedArgs), expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            std::vector<ValuePtr> elements;
            for (const auto& elem : node.elements) {
                elements.push_back(elem ? eval(*elem) : Value::none());
            }
            return Value::list(std::move(elements));
        }
        else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
            std::vector<ValuePtr> elements;
            for (const auto& elem : node.elements) {
                elements.push_back(elem ? eval(*elem) : Value::none());
            }
            return Value::tuple(std::move(elements));
        }
        else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            auto map = std::make_shared<Value>();
            std::vector<std::pair<ValuePtr, ValuePtr>> entries;
            for (const auto& entry : node.entries) {
                auto key = entry.key ? eval(*entry.key) : Value::none();
                auto val = entry.value ? eval(*entry.value) : Value::none();
                entries.push_back({key, val});
            }
            map->data = MapValue{std::move(entries)};
            return map;
        }
        else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            auto start = node.start ? eval(*node.start) : Value::integer(0);
            auto end = node.end ? eval(*node.end) : Value::integer(0);
            auto* s = std::get_if<IntValue>(&start->data);
            auto* e = std::get_if<IntValue>(&end->data);
            if (s && e) {
                auto range = std::make_shared<Value>();
                range->data = RangeValue{s->value, e->value};
                return range;
            }
            auto range = std::make_shared<Value>();
            range->data = RangeValue{0, 0};
            return range;
        }
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            auto cond = node.condition ? eval(*node.condition) : Value::boolean(false);
            if (cond->isTrue()) {
                return evalBody(node.thenBody);
            }
            for (const auto& [elifCond, elifBody] : node.elifs) {
                auto ec = elifCond ? eval(*elifCond) : Value::boolean(false);
                if (ec->isTrue()) {
                    return evalBody(elifBody);
                }
            }
            if (node.elseBody) {
                return evalBody(*node.elseBody);
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            auto subject = node.subject ? eval(*node.subject) : Value::none();
            for (const auto& clause : node.clauses) {
                pushEnv();
                if (node.subjectBinding) {
                    m_env->define(*node.subjectBinding, subject);
                }
                // Everything below must pop this scope before returning OR
                // propagating an exception. Without the try/catch, a clause
                // body containing `return` (extremely common — e.g. `_ ->
                // return p`) would throw past the popEnv() below, leaking
                // this scope permanently: m_env would stay one level too
                // deep for the rest of the enclosing function call, and
                // anything that function defined locally (e.g. `var p =
                // this` in a helper called from a caller's loop) would
                // become invisible/shadowed to the caller afterward —
                // silently corrupting unrelated variables with the same
                // name in the caller, or causing infinite loops.
                try {
                    bool matched = false;
                    for (const auto& pat : clause.patterns) {
                        if (matchPattern(*pat, subject)) {
                            matched = true;
                            break;
                        }
                    }
                    if (matched) {
                        if (clause.guard && *clause.guard) {
                            auto guardVal = eval(**clause.guard);
                            if (!guardVal->isTrue()) {
                                popEnv();
                                continue;
                            }
                        }
                        auto result = clause.body ? eval(*clause.body) : Value::none();
                        popEnv();
                        return result;
                    }
                } catch (...) {
                    popEnv();
                    throw;
                }
                popEnv();
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            // `return EXPR if COND` parses as ReturnExpr(TrailingIf(EXPR,
            // COND)) — i.e. the `if` must gate whether the return happens at
            // all, not just what value it carries. Without this special
            // case, ReturnExpr unconditionally throws even when COND is
            // false (just throwing None), short-circuiting the function
            // unconditionally — breaking idioms like
            // `return Error(...) if invalid?` used throughout Result-style
            // error handling.
            if (node.value) {
                if (auto* trailing = std::get_if<ast::TrailingIf>(&node.value->kind)) {
                    auto cond = trailing->condition ? eval(*trailing->condition) : Value::boolean(false);
                    if (!cond->isTrue()) {
                        return Value::none();
                    }
                    auto value = trailing->expr ? eval(*trailing->expr) : Value::none();
                    throw ReturnException(value);
                }
            }
            auto value = node.value ? eval(*node.value) : Value::none();
            throw ReturnException(value);
        }
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            auto lambda = std::make_shared<Value>();
            auto capturedEnv = m_env;
            const auto* bodyPtr = &node.body;
            std::vector<std::string> paramNames;
            for (const auto& p : node.params) {
                paramNames.push_back(p.name);
            }

            lambda->data = FunctionValue{"<lambda>",
                [this, bodyPtr, paramNames, capturedEnv](std::vector<ValuePtr> args) -> ValuePtr {
                    auto prevEnv = m_env;
                    m_env = std::make_shared<Environment>(capturedEnv);
                    for (size_t i = 0; i < paramNames.size() && i < args.size(); i++) {
                        m_env->define(paramNames[i], args[i]);
                    }
                    ValuePtr result;
                    // catch(...) so a RuntimeError propagating through this
                    // lambda (e.g. a failed `assert` caught higher up by
                    // `it`) still restores m_env before unwinding further —
                    // same reasoning as the MatchExpr/function-clause guards.
                    try {
                        result = evalBody(*bodyPtr);
                    } catch (ReturnException& ret) {
                        result = ret.value();
                    } catch (...) {
                        m_env = prevEnv;
                        throw;
                    }
                    m_env = prevEnv;
                    return result;
                }};
            return lambda;
        }
        else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            auto cond = node.condition ? eval(*node.condition) : Value::boolean(false);
            if (cond->isTrue()) {
                return node.expr ? eval(*node.expr) : Value::none();
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            auto cond = node.condition ? eval(*node.condition) : Value::boolean(false);
            if (cond->isTrue()) {
                return node.thenExpr ? eval(*node.thenExpr) : Value::none();
            }
            return node.elseExpr ? eval(*node.elseExpr) : Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            std::unordered_map<std::string, ValuePtr> fields;
            for (const auto& [name, val] : node.fields) {
                fields[name] = val ? eval(*val) : Value::none();
            }
            // Apply declared field defaults (e.g. `pos : Int = 0`) for any
            // field this construction didn't specify explicitly.
            auto defIt = m_recordDefs.find(node.typeName);
            if (defIt != m_recordDefs.end()) {
                for (const auto& field : defIt->second->fields) {
                    if (fields.count(field.name)) continue;
                    if (field.defaultValue && *field.defaultValue) {
                        fields[field.name] = eval(**field.defaultValue);
                    }
                }
            }
            return Value::record(node.typeName, std::move(fields));
        }
        else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
            if (node.kind == ast::ShorthandLambda::Kind::Function) {
                // &function — look up the function and return it
                auto val = m_env->get(node.name);
                if (val) return val;
                throw RuntimeError("Undefined function: " + node.name, expr.location);
            }
            if (node.kind == ast::ShorthandLambda::Kind::Method) {
                // &.method — create a lambda that calls method on its arg
                auto method = node.name;
                auto lambda = std::make_shared<Value>();
                lambda->data = FunctionValue{"&." + method,
                    [this, method](std::vector<ValuePtr> args) -> ValuePtr {
                        if (args.empty()) return Value::none();
                        // Call method on the arg via UFCS
                        return callFunction(method, std::move(args), {}, {});
                    }};
                return lambda;
            }
            if (node.kind == ast::ShorthandLambda::Kind::MethodWithArgs) {
                // &.method(args) — create a lambda that calls method with extra args
                auto method = node.name;
                std::vector<ValuePtr> extraArgs;
                for (const auto& arg : node.args) {
                    extraArgs.push_back(arg ? eval(*arg) : Value::none());
                }
                auto lambda = std::make_shared<Value>();
                lambda->data = FunctionValue{"&." + method,
                    [this, method, extraArgs](std::vector<ValuePtr> args) -> ValuePtr {
                        auto allArgs = args;
                        for (const auto& a : extraArgs) allArgs.push_back(a);
                        return callFunction(method, std::move(allArgs), {}, {});
                    }};
                return lambda;
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            return evalBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            // `loop do ... end` runs forever — Kex has no break/continue,
            // so the only way out is `return` (ReturnException, which
            // unwinds to the enclosing function's call site and is caught
            // there) or an uncaught error. Each iteration gets its own
            // scope so `var`s declared inside the loop body don't leak
            // across iterations (mirrors how other block bodies push/pop).
            while (true) {
                pushEnv();
                try {
                    evalBody(node.body);
                } catch (...) {
                    popEnv();
                    throw;
                }
                popEnv();
            }
        }
        else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            while (true) {
                auto cond = node.condition ? eval(*node.condition) : Value::boolean(false);
                if (!cond->isTrue()) break;
                pushEnv();
                try {
                    evalBody(node.body);
                } catch (...) {
                    popEnv();
                    throw;
                }
                popEnv();
            }
            return Value::none();
        }
        else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
            // Look up in environment first (records, namespaces, ALL_CAPS
            // constants like `let MAX_RETRIES = 3`)
            auto val = m_env->get(node.name);
            if (val) return autoCallZeroArgConstant(node.name, val);
            // Otherwise return as type tag (atom) for pattern matching
            return Value::atom(node.name);
        }
        else if constexpr (std::is_same_v<T, ast::ErrorPropagate>) {
            // expr? — unwrap Ok(x)/Just(x) to x; short-circuit the enclosing
            // function on Error(e)/None by reusing the same ReturnException
            // mechanism `return` already uses. Any other value passes
            // through unchanged (matches the type checker, which just
            // forwards the inner expression's type).
            auto inner = node.inner ? eval(*node.inner) : Value::none();
            if (std::holds_alternative<NoneValue>(inner->data)) {
                throw ReturnException(inner);
            }
            if (auto* rec = std::get_if<RecordValue>(&inner->data)) {
                if ((rec->typeName == "Ok" || rec->typeName == "Just") && rec->fields.size() == 1) {
                    auto it = rec->fields.find("0");
                    if (it != rec->fields.end()) return it->second;
                }
                if (rec->typeName == "Error") {
                    throw ReturnException(inner);
                }
            }
            return inner;
        }
        else {
            return Value::none();
        }
    }, expr.kind);
}

auto Evaluator::evalBinaryOp(TokenType op, const ValuePtr& left, const ValuePtr& right,
                             SourceLocation loc) -> ValuePtr {
    // Operator overloading: `make Type do let +(other) -> Type ... end`
    // registers "Type::+", dispatched here the same way method calls
    // dispatch on receiver type, before any built-in handling.
    if (auto* rec = std::get_if<RecordValue>(&left->data)) {
        std::string opSymbol;
        switch (op) {
            case TokenType::Plus:       opSymbol = "+";  break;
            case TokenType::Minus:      opSymbol = "-";  break;
            case TokenType::Star:       opSymbol = "*";  break;
            case TokenType::Slash:      opSymbol = "/";  break;
            case TokenType::Percent:    opSymbol = "%";  break;
            case TokenType::EqEq:       opSymbol = "=="; break;
            case TokenType::NotEq:      opSymbol = "!="; break;
            case TokenType::LessThan:   opSymbol = "<";  break;
            case TokenType::GreaterThan: opSymbol = ">"; break;
            case TokenType::LessEq:     opSymbol = "<="; break;
            case TokenType::GreaterEq:  opSymbol = ">="; break;
            default: break;
        }
        if (!opSymbol.empty()) {
            auto mangled = rec->typeName + "::" + opSymbol;
            if (m_env->get(mangled)) {
                return callFunction(mangled, {left, right}, {}, loc);
            }
        }
    }

    auto* li = std::get_if<IntValue>(&left->data);
    auto* ri = std::get_if<IntValue>(&right->data);
    auto* lf = std::get_if<FloatValue>(&left->data);
    auto* rf = std::get_if<FloatValue>(&right->data);
    auto* ls = std::get_if<StringValue>(&left->data);
    auto* rs = std::get_if<StringValue>(&right->data);
    auto* lc = std::get_if<CharValue>(&left->data);
    auto* rc = std::get_if<CharValue>(&right->data);
    auto* lb = std::get_if<BoolValue>(&left->data);
    auto* rb = std::get_if<BoolValue>(&right->data);

    switch (op) {
        case TokenType::Plus:
            if (li && ri) return Value::integer(li->value + ri->value);
            if (lf && rf) return Value::floating(lf->value + rf->value);
            if (li && rf) return Value::floating(li->value + rf->value);
            if (lf && ri) return Value::floating(lf->value + ri->value);
            // String/Char/[Char] concatenate as text — e.g. 'a' + 'b' ==
            // "ab", "ab" + 'c' == "abc". This is broader than the Char/
            // String *equality* rule (Char isn't a String for ==) — here
            // we just want "what text does this contribute", which a bare
            // Char answers fine; see textContent vs. stringOrCharListText
            // in value.cxx.
            if (auto lt = textContent(left)) {
                if (auto rt = textContent(right)) return Value::string(*lt + *rt);
            }
            throw RuntimeError("Cannot add " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Minus:
            if (li && ri) return Value::integer(li->value - ri->value);
            if (lf && rf) return Value::floating(lf->value - rf->value);
            if (li && rf) return Value::floating(li->value - rf->value);
            if (lf && ri) return Value::floating(lf->value - ri->value);
            throw RuntimeError("Cannot subtract " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Star:
            if (li && ri) return Value::integer(li->value * ri->value);
            if (lf && rf) return Value::floating(lf->value * rf->value);
            if (li && rf) return Value::floating(li->value * rf->value);
            if (lf && ri) return Value::floating(lf->value * ri->value);
            throw RuntimeError("Cannot multiply " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Slash:
            if (ri && ri->value == 0) throw RuntimeError("Division by zero", loc);
            if (rf && rf->value == 0.0) throw RuntimeError("Division by zero", loc);
            if (li && ri) return Value::integer(li->value / ri->value);
            if (lf && rf) return Value::floating(lf->value / rf->value);
            if (li && rf) return Value::floating(li->value / rf->value);
            if (lf && ri) return Value::floating(lf->value / ri->value);
            throw RuntimeError("Cannot divide " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::Percent:
            if (li && ri) {
                if (ri->value == 0) throw RuntimeError("Modulo by zero", loc);
                return Value::integer(li->value % ri->value);
            }
            throw RuntimeError("Modulo requires integers", loc);

        case TokenType::EqEq: return Value::boolean(valuesEqual(left, right));
        case TokenType::NotEq: return Value::boolean(!valuesEqual(left, right));

        case TokenType::LessThan:
            if (li && ri) return Value::boolean(li->value < ri->value);
            if (lf && rf) return Value::boolean(lf->value < rf->value);
            if (ls && rs) return Value::boolean(ls->value < rs->value);
            if (lc && rc) return Value::boolean(lc->value < rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::GreaterThan:
            if (li && ri) return Value::boolean(li->value > ri->value);
            if (lf && rf) return Value::boolean(lf->value > rf->value);
            if (ls && rs) return Value::boolean(ls->value > rs->value);
            if (lc && rc) return Value::boolean(lc->value > rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::LessEq:
            if (li && ri) return Value::boolean(li->value <= ri->value);
            if (lf && rf) return Value::boolean(lf->value <= rf->value);
            if (ls && rs) return Value::boolean(ls->value <= rs->value);
            if (lc && rc) return Value::boolean(lc->value <= rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::GreaterEq:
            if (li && ri) return Value::boolean(li->value >= ri->value);
            if (lf && rf) return Value::boolean(lf->value >= rf->value);
            if (ls && rs) return Value::boolean(ls->value >= rs->value);
            if (lc && rc) return Value::boolean(lc->value >= rc->value);
            throw RuntimeError("Cannot compare " + left->typeName() + " and " + right->typeName(), loc);

        case TokenType::AmpAmp:
            return Value::boolean(left->isTrue() && right->isTrue());

        case TokenType::PipePipe:
            return Value::boolean(left->isTrue() || right->isTrue());

        default:
            throw RuntimeError("Unknown operator", loc);
    }
}

auto Evaluator::evalUnaryOp(TokenType op, const ValuePtr& operand,
                            SourceLocation loc) -> ValuePtr {
    switch (op) {
        case TokenType::Minus:
            if (auto* i = std::get_if<IntValue>(&operand->data))
                return Value::integer(-i->value);
            if (auto* f = std::get_if<FloatValue>(&operand->data))
                return Value::floating(-f->value);
            throw RuntimeError("Cannot negate " + operand->typeName(), loc);

        case TokenType::Bang:
            return Value::boolean(!operand->isTrue());

        default:
            throw RuntimeError("Unknown unary operator", loc);
    }
}

auto Evaluator::callFunction(const std::string& name, std::vector<ValuePtr> args,
                             NamedArgs namedArgs, SourceLocation loc) -> ValuePtr {
    auto val = m_env->get(name);
    if (!val) {
        throw RuntimeError("Undefined function: " + name, loc);
    }

    if (auto* func = std::get_if<FunctionValue>(&val->data)) {
        if (func->native) {
            // Reorder: place named args into correct positions based on param names
            if (!namedArgs.empty()) {
                auto it = m_functionDefs.find(name);
                if (it != m_functionDefs.end() && !it->second.empty()) {
                    const auto& firstClause = it->second[0]->clauses[0];
                    // Build full arg list: start with positional, fill in named
                    size_t totalParams = firstClause.params.size();
                    std::vector<ValuePtr> fullArgs(totalParams, nullptr);

                    // Place positional args first
                    for (size_t i = 0; i < args.size() && i < totalParams; i++) {
                        fullArgs[i] = std::move(args[i]);
                    }

                    // Place named args by matching param names
                    for (auto& [argName, argVal] : namedArgs) {
                        for (size_t i = 0; i < firstClause.params.size(); i++) {
                            if (firstClause.params[i].name.has_value() &&
                                *firstClause.params[i].name == argName) {
                                fullArgs[i] = std::move(argVal);
                                break;
                            }
                        }
                    }

                    // Fill any remaining nulls with None
                    for (auto& a : fullArgs) {
                        if (!a) a = Value::none();
                    }

                    return func->native(std::move(fullArgs));
                } else {
                    // No def info — just append named args
                    for (auto& [_, v] : namedArgs) {
                        args.push_back(std::move(v));
                    }
                }
            }
            return func->native(std::move(args));
        }
    }

    throw RuntimeError("'" + name + "' is not callable", loc);
}

auto Evaluator::autoCallZeroArgConstant(const std::string& name, const ValuePtr& val) -> ValuePtr {
    // Only acts on user-defined Kex functions (tracked in m_functionDefs);
    // native builtins, namespace placeholders (RecordValue), and ADT
    // constructors (registered directly via m_env->define, never through
    // execFunctionDef) are absent from m_functionDefs and pass through
    // unchanged here.
    auto* func = std::get_if<FunctionValue>(&val->data);
    if (!func || !func->native) return val;

    auto defIt = m_functionDefs.find(name);
    if (defIt == m_functionDefs.end() || defIt->second.empty()) return val;
    if (!defIt->second[0]->clauses[0].params.empty()) return val;

    auto savedEnv = m_env;
    try {
        auto result = func->native({});
        if (result && !std::holds_alternative<NoneValue>(result->data)) {
            return result;
        }
    } catch (...) {
        m_env = savedEnv;
    }
    return val;
}

auto Evaluator::matchPattern(const ast::Pattern& pattern, const ValuePtr& value) -> bool {
    return std::visit([this, &value](const auto& pat) -> bool {
        using T = std::decay_t<decltype(pat)>;

        if constexpr (std::is_same_v<T, ast::WildcardPattern>) {
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            // @pattern — match the inner pattern against 'this' (the value)
            if (pat.inner) {
                return matchPattern(*pat.inner, value);
            }
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::VarPattern>) {
            m_env->define(pat.name, value);
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
            if (pat.literal.type == TokenType::Integer) {
                auto* iv = std::get_if<IntValue>(&value->data);
                return iv && iv->value == std::stoll(pat.literal.value);
            }
            if (pat.literal.type == TokenType::String) {
                auto* sv = std::get_if<StringValue>(&value->data);
                return sv && sv->value == pat.literal.value;
            }
            if (pat.literal.type == TokenType::Char) {
                // Char is its own type, not a 1-character String — a
                // char-literal pattern only matches a Char value.
                auto* cv = std::get_if<CharValue>(&value->data);
                return cv && cv->value == (pat.literal.value.empty() ? '\0' : pat.literal.value[0]);
            }
            if (pat.literal.type == TokenType::True) {
                auto* bv = std::get_if<BoolValue>(&value->data);
                return bv && bv->value;
            }
            if (pat.literal.type == TokenType::False) {
                auto* bv = std::get_if<BoolValue>(&value->data);
                return bv && !bv->value;
            }
            if (pat.literal.type == TokenType::None) {
                return std::holds_alternative<NoneValue>(value->data);
            }
            if (pat.literal.type == TokenType::Atom) {
                auto* av = std::get_if<AtomValue>(&value->data);
                return av && av->name == pat.literal.value;
            }
            return false;
        }
        else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            auto* tv = std::get_if<TupleValue>(&value->data);
            if (!tv || tv->elements.size() != pat.elements.size()) return false;
            for (size_t i = 0; i < pat.elements.size(); i++) {
                if (!matchPattern(*pat.elements[i], tv->elements[i])) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            auto* lv = std::get_if<ListValue>(&value->data);
            if (!lv) return false;
            if (pat.elements.empty() && !pat.rest) {
                return lv->elements.empty();
            }
            if (lv->elements.size() < pat.elements.size()) return false;
            for (size_t i = 0; i < pat.elements.size(); i++) {
                if (!matchPattern(*pat.elements[i], lv->elements[i])) return false;
            }
            if (pat.rest) {
                std::vector<ValuePtr> rest(lv->elements.begin() + pat.elements.size(), lv->elements.end());
                if (!matchPattern(**pat.rest, Value::list(std::move(rest)))) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            // Match None
            if (pat.name == "None") return std::holds_alternative<NoneValue>(value->data);

            if (pat.args.empty()) {
                // Match type tag atoms: to(String) passes atom("String")
                if (auto* atom = std::get_if<AtomValue>(&value->data)) {
                    if (atom->name == pat.name) return true;
                }

                // Match type names as type patterns (for runtime type checking)
                if (pat.name == "String") return std::holds_alternative<StringValue>(value->data);
                if (pat.name == "Int" || pat.name == "Integer") return std::holds_alternative<IntValue>(value->data);
                if (pat.name == "Float") return std::holds_alternative<FloatValue>(value->data);
                if (pat.name == "Bool") return std::holds_alternative<BoolValue>(value->data);
                if (pat.name == "Atom") return std::holds_alternative<AtomValue>(value->data);
                if (pat.name == "List") return std::holds_alternative<ListValue>(value->data);
                if (pat.name == "Map") return std::holds_alternative<MapValue>(value->data);
                if (pat.name == "Tuple") return std::holds_alternative<TupleValue>(value->data);
                if (pat.name == "Range") return std::holds_alternative<RangeValue>(value->data);
                if (pat.name == "Stream") return std::holds_alternative<StreamValue>(value->data);
                // Match record type name
                if (auto* rec = std::get_if<RecordValue>(&value->data)) {
                    if (rec->typeName == pat.name) return true;
                }
                // Match True/False as literal patterns
                if (pat.name == "True") {
                    auto* b = std::get_if<BoolValue>(&value->data);
                    return b && b->value;
                }
                if (pat.name == "False") {
                    auto* b = std::get_if<BoolValue>(&value->data);
                    return b && !b->value;
                }
            }

            // Constructor with args: Just(x), Ok(x), Error(e), Number(n), etc.
            // Constructor calls build a RecordValue with positional fields
            // keyed "0", "1", ... (see the variant-constructor registration
            // in registerBuiltins / execTopLevel's TypeDef handling).
            if (auto* rec = std::get_if<RecordValue>(&value->data)) {
                if (rec->typeName != pat.name) return false;
                if (rec->fields.size() != pat.args.size()) return false;
                for (size_t i = 0; i < pat.args.size(); i++) {
                    auto it = rec->fields.find(std::to_string(i));
                    if (it == rec->fields.end()) return false;
                    if (!matchPattern(*pat.args[i], it->second)) return false;
                }
                return true;
            }
            return false;
        }
        else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            if (auto* rv = std::get_if<RecordValue>(&value->data)) {
                for (const auto& field : pat.fields) {
                    auto it = rv->fields.find(field.name);
                    if (it == rv->fields.end()) return false;
                    if (field.pattern && !matchPattern(**field.pattern, it->second)) return false;
                    if (!field.pattern) m_env->define(field.name, it->second);
                }
                return true;
            }
            return false;
        }
        else {
            return false;
        }
    }, pattern.kind);
}

auto Evaluator::registerBuiltins() -> void {
    // Orchestrator only — each domain is implemented in its own file under
    // src/interpreter/stdlib/. Order matters: registerStreamBuiltins()
    // wraps the plain-list `take` registered by registerListBuiltins().
    registerAdtConstructors();
    registerIOBuiltins();
    registerFileBuiltins();
    registerListBuiltins();
    registerStringBuiltins();
    registerIntegerBuiltins();
    registerStreamBuiltins();
    registerMapBuiltins();
    registerEnvBuiltins();
    registerMathBuiltins();
    registerTestBuiltins();
}

auto Evaluator::pushEnv() -> void {
    m_env = std::make_shared<Environment>(m_env);
}

auto Evaluator::popEnv() -> void {
    if (m_env->parent()) {
        m_env = m_env->parent();
    }
}

} // namespace kex::interpreter
