#include "typechecker.hxx"
#include "analyzer.hxx"
#include "declaration_validator.hxx"
#include "../common/type_def_utils.hxx"
#include <functional>
#include <set>
#include <unordered_set>

namespace kex::semantic {

auto containsOpenType(const TypePtr& type) -> bool;

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

// The receiver without its type arguments — `Result<Box, ?>` becomes
// `Result`, which is the part that decides whether a candidate could accept
// it.
// Does a type contain an Unknown anywhere? On a signature PARAMETER this
// marks a contract the checker never learned — the shape-only entries the
// source-derived interface path records for un-annotated `make` methods,
// where compiled artifacts would supply real types. Type variables are not
// this: `[A]` is a known, genuinely generic contract.
auto mentionsUnknownType(const TypePtr& type) -> bool {
    if (!type) return false;
    if (std::holds_alternative<UnknownType>(type->kind)) return true;
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        for (const auto& arg : named->typeArgs)
            if (mentionsUnknownType(arg)) return true;
        return false;
    }
    if (auto* list = std::get_if<ListType>(&type->kind))
        return mentionsUnknownType(list->element);
    if (auto* optional = std::get_if<OptionalType>(&type->kind))
        return mentionsUnknownType(optional->inner);
    if (auto* map = std::get_if<MapType>(&type->kind))
        return mentionsUnknownType(map->key) || mentionsUnknownType(map->value);
    if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
        for (const auto& element : tuple->elements)
            if (mentionsUnknownType(element)) return true;
    }
    if (auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
        for (const auto& member : intersection->members)
            if (mentionsUnknownType(member)) return true;
    }
    if (auto* record = std::get_if<RecordType>(&type->kind)) {
        for (const auto& [_, fieldType] : record->fields)
            if (mentionsUnknownType(fieldType)) return true;
    }
    return false;
}

auto headShapeOf(const TypePtr& type) -> TypePtr {
    if (!type) return type;
    if (auto* named = std::get_if<NamedType>(&type->kind))
        return Type::named(named->name);
    return type;
}

auto substituteInterfaceGenerics(const TypePtr& type,
                                 const std::vector<TypePtr>& args) -> TypePtr {
    if (!type) return type;
    if (auto* var = std::get_if<TypeVar>(&type->kind)) {
        if (var->id < 0) {
            auto index = static_cast<size_t>(-var->id - 1);
            if (index < args.size()) return args[index];
        }
        return type;
    }
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        std::vector<TypePtr> nested;
        for (const auto& arg : named->typeArgs)
            nested.push_back(substituteInterfaceGenerics(arg, args));
        return Type::named(named->name, std::move(nested));
    }
    if (auto* list = std::get_if<ListType>(&type->kind))
        return Type::list(substituteInterfaceGenerics(list->element, args));
    if (auto* optional = std::get_if<OptionalType>(&type->kind))
        return Type::optional(
            substituteInterfaceGenerics(optional->inner, args));
    if (auto* map = std::get_if<MapType>(&type->kind))
        return Type::map(substituteInterfaceGenerics(map->key, args),
                         substituteInterfaceGenerics(map->value, args));
    if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
        std::vector<TypePtr> elements;
        for (const auto& element : tuple->elements)
            elements.push_back(substituteInterfaceGenerics(element, args));
        return Type::tuple(std::move(elements));
    }
    if (auto* fn = std::get_if<FuncType>(&type->kind)) {
        std::vector<TypePtr> params;
        for (const auto& param : fn->params)
            params.push_back(substituteInterfaceGenerics(param, args));
        return Type::func(std::move(params),
                          substituteInterfaceGenerics(fn->result, args));
    }
    if (auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
        std::vector<TypePtr> members;
        for (const auto& member : intersection->members)
            members.push_back(substituteInterfaceGenerics(member, args));
        return Type::intersection(std::move(members));
    }
    if (auto* record = std::get_if<RecordType>(&type->kind)) {
        std::vector<std::pair<std::string, TypePtr>> fields;
        for (const auto& [name, fieldType] : record->fields)
            fields.emplace_back(
                name, substituteInterfaceGenerics(fieldType, args));
        return Type::record(std::move(fields));
    }
    return type;
}

// How many type parameters a record has, read back off its registered field
// types. A record's parameters occupy the negative TypeVar slots -1, -2, …
// (the convention `substituteInterfaceGenerics` consumes), so the highest
// slot any field mentions is the arity. Deriving it this way keeps LOCAL and
// IMPORTED records on one code path: the KexI reader assigns the same
// negative ids when it rebuilds an imported record's fields, and neither
// KexiRecord nor ImportedInterfaces carries a parameter LIST to consult.
auto recordTypeParamCount(
    const std::unordered_map<std::string, TypePtr>& fields) -> size_t {
    size_t count = 0;
    std::function<void(const TypePtr&)> scan = [&](const TypePtr& type) {
        if (!type) return;
        if (auto* var = std::get_if<TypeVar>(&type->kind)) {
            if (var->id < 0)
                count = std::max(count, static_cast<size_t>(-var->id));
            return;
        }
        if (auto* named = std::get_if<NamedType>(&type->kind)) {
            for (const auto& arg : named->typeArgs) scan(arg);
            return;
        }
        if (auto* list = std::get_if<ListType>(&type->kind)) return scan(list->element);
        if (auto* optional = std::get_if<OptionalType>(&type->kind))
            return scan(optional->inner);
        if (auto* map = std::get_if<MapType>(&type->kind)) {
            scan(map->key);
            return scan(map->value);
        }
        if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
            for (const auto& element : tuple->elements) scan(element);
            return;
        }
        if (auto* fn = std::get_if<FuncType>(&type->kind)) {
            for (const auto& param : fn->params) scan(param);
            return scan(fn->result);
        }
        if (auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
            for (const auto& member : intersection->members) scan(member);
            return;
        }
        if (auto* record = std::get_if<RecordType>(&type->kind))
            for (const auto& [_, fieldType] : record->fields) scan(fieldType);
    };
    for (const auto& [_, fieldType] : fields) scan(fieldType);
    return count;
}

// Match a record field's DECLARED type against the type of the value supplied
// for it, recording what each parameter slot was filled with. First binding
// wins, so two fields disagreeing about `A` leave the first one's answer
// rather than silently taking the last.
auto bindRecordTypeParams(const TypePtr& declared, const TypePtr& actual,
                          std::unordered_map<int, TypePtr>& slots) -> void {
    if (!declared || !actual) return;
    if (auto* var = std::get_if<TypeVar>(&declared->kind)) {
        if (var->id >= 0) return;
        // An open actual says nothing about the slot — `Box { items: [] }`
        // must stay `Box<Any>` rather than pinning A to the empty list's
        // placeholder variable.
        if (std::holds_alternative<UnknownType>(actual->kind) ||
            std::holds_alternative<TypeVar>(actual->kind))
            return;
        slots.try_emplace(var->id, actual);
        return;
    }
    if (auto* named = std::get_if<NamedType>(&declared->kind)) {
        auto* other = std::get_if<NamedType>(&actual->kind);
        if (!other || other->typeArgs.size() != named->typeArgs.size()) return;
        for (size_t i = 0; i < named->typeArgs.size(); ++i)
            bindRecordTypeParams(named->typeArgs[i], other->typeArgs[i], slots);
        return;
    }
    if (auto* list = std::get_if<ListType>(&declared->kind)) {
        if (auto* other = std::get_if<ListType>(&actual->kind))
            bindRecordTypeParams(list->element, other->element, slots);
        return;
    }
    if (auto* optional = std::get_if<OptionalType>(&declared->kind)) {
        if (auto* other = std::get_if<OptionalType>(&actual->kind))
            bindRecordTypeParams(optional->inner, other->inner, slots);
        return;
    }
    if (auto* map = std::get_if<MapType>(&declared->kind)) {
        if (auto* other = std::get_if<MapType>(&actual->kind)) {
            bindRecordTypeParams(map->key, other->key, slots);
            bindRecordTypeParams(map->value, other->value, slots);
        }
        return;
    }
    if (auto* tuple = std::get_if<TupleType>(&declared->kind)) {
        auto* other = std::get_if<TupleType>(&actual->kind);
        if (!other || other->elements.size() != tuple->elements.size()) return;
        for (size_t i = 0; i < tuple->elements.size(); ++i)
            bindRecordTypeParams(tuple->elements[i], other->elements[i], slots);
        return;
    }
    if (auto* fn = std::get_if<FuncType>(&declared->kind)) {
        auto* other = std::get_if<FuncType>(&actual->kind);
        if (!other || other->params.size() != fn->params.size()) return;
        for (size_t i = 0; i < fn->params.size(); ++i)
            bindRecordTypeParams(fn->params[i], other->params[i], slots);
        bindRecordTypeParams(fn->result, other->result, slots);
        return;
    }
}

} // namespace

auto TypeChecker::check(const ast::Program& program,
                        std::vector<Diagnostic>& diagnostics) -> void {
    m_diagnostics = &diagnostics;
    m_patternBindings.clear();
    m_functionSignatures.clear();
    m_resolvedCalls.clear();
    m_selectedCallSignatures.clear();
    m_referencedModules.clear();
    m_localModules.clear();
    m_moduleConstructors.clear();
    m_adtVariants.clear();
    m_adtOfConstructor.clear();
    m_nullaryConstructors.clear();
    m_methodSignatures.clear();
    m_slotMethodNames.clear();
    m_makeMethodNames.clear();
    m_qualifiedPublished.clear();
    m_overloadPurity.clear();
    m_scopedDeclaredSignatures.clear();
    m_typeAliases.clear();
    m_distinctTypes.clear();
    m_recordFields.clear();
    m_requiredRecordFields.clear();
    if (m_importedInterfaces)
        m_distinctTypes = m_importedInterfaces->distinctTypes;
    if (m_importedInterfaces)
        m_recordFields = m_importedInterfaces->recordFields;
    m_currentModulePath.clear();
    m_scopeStack.clear();
    m_importScopeStack.clear();
    m_declarationImports.clear();
    m_functionImports.clear();
    m_makeImports.clear();
    m_mainImports.clear();
    pushScope();

    auto selectionFor = [](const ast::UsingBlock& block) {
        ImportSelection selection;
        for (size_t i = 0; i < block.module.parts.size(); ++i) {
            if (i) selection.module += ".";
            selection.module += block.module.parts[i];
        }
        selection.onlyNames = block.onlyNames;
        selection.exceptNames = block.exceptNames;
        return selection;
    };
    // A `private do ... end` / `visible do ... end` block is a plain
    // container: the declarations inside it see exactly the imports that are
    // active around it. Without this, annotations on those functions resolved
    // record names without the enclosing `using`, so the same record came out
    // qualified in one signature and bare in another.
    auto collectVisibilityImports =
        [&](const ast::VisibilityBlock& block,
            const std::vector<ImportSelection>& active) {
            for (const auto& visible : block.items) {
                if (const auto* fn =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible);
                    fn && *fn)
                    m_functionImports[fn->get()] = active;
                else if (const auto* make =
                             std::get_if<std::unique_ptr<ast::MakeDef>>(&visible);
                         make && *make)
                    m_makeImports[make->get()] = active;
            }
        };
    std::function<void(const ast::ModuleDef&, std::vector<ImportSelection>)>
        collectModuleImports;
    collectModuleImports = [&](const ast::ModuleDef& module,
                               std::vector<ImportSelection> active) {
        for (const auto& item : module.body) {
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                    if (node && node->body.empty())
                        active.push_back(selectionFor(*node));
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (node) m_functionImports[node.get()] = active;
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                    if (node) m_makeImports[node.get()] = active;
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (node) collectVisibilityImports(*node, active);
                } else if constexpr (
                    std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                    if (node) collectModuleImports(*node, active);
                }
            }, item);
        }
    };
    std::vector<ImportSelection> topLevelImports;
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                if (node && node->body.empty())
                    topLevelImports.push_back(selectionFor(*node));
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) m_functionImports[node.get()] = topLevelImports;
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) m_makeImports[node.get()] = topLevelImports;
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (node) m_mainImports[node.get()] = topLevelImports;
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (node) collectVisibilityImports(*node, topLevelImports);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectModuleImports(*node, topLevelImports);
            }
        }, item);
    }

    std::function<void(const ast::ModuleDef&)> collectLocalModule =
        [&](const ast::ModuleDef& mod) {
            m_localModules.insert(mod.name);
            for (const auto& item : mod.body) {
                std::visit([&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        if (node) collectLocalModule(*node);
                    }
                }, item);
            }
        };
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectLocalModule(*node);
            }
        }, item);
    }

    if (m_importedInterfaces) {
        // Constructor names are only unique per ADT: `Kilo` is a nullary
        // Units.Data prefix and a one-payload Units.SI prefix. These maps are
        // keyed by bare constructor name, so a name claimed by two different
        // ADTs is dropped rather than resolved last-writer-wins — which would
        // reject correct patterns against whichever ADT lost.
        std::unordered_set<std::string> ambiguous;
        for (const auto& adt : m_importedInterfaces->adts) {
            m_adtVariants[adt.name] = adt.constructors;
            for (const auto& ctor : adt.constructors) {
                auto [owner, fresh] = m_adtOfConstructor.try_emplace(ctor, adt.name);
                if (!fresh && owner->second != adt.name) {
                    ambiguous.insert(ctor);
                    continue;
                }
                auto arity = adt.constructorArities.find(ctor);
                if (arity != adt.constructorArities.end()) {
                    m_constructorArity[ctor] = arity->second;
                    if (arity->second == 0) m_nullaryConstructors.insert(ctor);
                }
                if (adt.typeParamCount ||
                    adt.constructorTypeParamSlots.contains(ctor)) {
                    ConstructorResult result;
                    result.adtName = adt.name;
                    result.typeParamCount = adt.typeParamCount;
                    if (auto slots = adt.constructorTypeParamSlots.find(ctor);
                        slots != adt.constructorTypeParamSlots.end())
                        result.slots = slots->second;
                    m_constructorResult[ctor] = std::move(result);
                }
            }
        }
        for (const auto& ctor : ambiguous) {
            m_adtOfConstructor.erase(ctor);
            m_constructorArity.erase(ctor);
            m_nullaryConstructors.erase(ctor);
            m_constructorResult.erase(ctor);
        }
    }

    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                registerAdt(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerAdtsInModule(*node);
            }
        }, item);
    }

    for (const auto& item : program.items)
        if (const auto* function =
                std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
            function && *function)
            m_functionDeclarations.emplace((*function)->name, function->get());
    registerRecordFields(program, /*namesOnly=*/true);

    // Register built-in types
    // (done before registerDeclaredSignatures so annotation TypeExprs can resolve these names)
    // Int is a user-facing alias for Integer (arbitrary precision).
    // Fixed-width integers are available as Int32, Int64, etc.
    m_globals.set("Int", Type::integer());
    m_globals.set("Integer", Type::integer());
    m_globals.set("Char", Type::charT());
    m_globals.set("String", Type::string());
    m_globals.set("Bool", Type::boolean());
    // Without this, the annotation `Atom` resolved to an unregistered
    // NamedType("Atom") while an atom literal infers PrimitiveType{Atom} —
    // same printed name, different kind, so they never matched. The symptom
    // was the self-contradictory "expects Atom | Integer, but got Atom".
    m_globals.set("Atom", Type::atom());
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

    m_globals.set("ENV", Type::map(Type::string(), Type::string()));

    // Imported aliases first; a locally declared name of the same spelling
    // then overwrites it, which is the precedence the rest of the checker uses.
    if (m_importedInterfaces)
        m_typeAliases = m_importedInterfaces->typeAliases;

    // Alias bodies must see the canonical built-in types. Resolving an open
    // row alias before this point made its `String` field a nominal name while
    // record declarations later used the real String type; both printed the
    // same but failed invariant row-field comparison.
    registerTypeAliases(program);

    // The imported field types seeded above were spelled against the defining
    // module's aliases and never went through resolveTypeExpr; expand them now
    // that the alias table is complete, so an imported record compares the same
    // way a locally declared one does.
    for (auto& [_, fields] : m_recordFields)
        for (auto& [__, fieldType] : fields)
            fieldType = expandTypeAliases(fieldType);

    if (m_importedInterfaces)
        for (const auto& trait : m_importedInterfaces->traits)
            m_traits.define(trait);
    registerTraits(program);
    registerCapabilityTraits(program);
    registerLocalConformances(program);
    if (m_importedInterfaces)
        for (const auto& c : m_importedInterfaces->traitConformances)
            m_traits.registerImplementation(c.typeName, c.traitName);
    registerRecordFields(program);
    for (const auto& [name, query] : m_computedAliases)
        m_typeAliases[name] = resolveTypeQuery(*query);
    m_computedAliases.clear();
    validateDeclarations(program, m_importedInterfaces, m_traits, diagnostics);
    registerDeclaredSignatures(program);
    registerMakeSignatures(program);
    preRegisterFunctionSigs(program);


    // Topological ordering: check functions whose callees are all known
    // before checking their callers, so forward-reference calls use real
    // (post-body-check) result types rather than provisional TypeVars.
    // SCCs (mutual recursion) are detected via back-edges in the DFS and
    // kept in their original relative order — pre-registration handles them.
    {
        // Collect names of all user-defined top-level functions.
        std::unordered_set<std::string> userFns;
        for (const auto& item : program.items) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n) userFns.insert(n->name);
                }
            }, item);
        }

        // Collect direct call-dependencies for each FunctionDef.
        // Only user-defined function names are tracked; imported package
        // functions are outside this compilation unit and need no ordering.
        std::function<void(const ast::Expr&, std::set<std::string>&)> collectCalls =
            [&](const ast::Expr& e, std::set<std::string>& out) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                    if (userFns.count(n.name)) out.insert(n.name);
                    for (const auto& a : n.args) if (a) collectCalls(*a, out);
                    if (n.block) collectCalls(**n.block, out);
                } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                    for (const auto& value : n.values)
                        if (value) collectCalls(*value, out);
                } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
                    if (userFns.count(n.tag)) out.insert(n.tag);
                    for (const auto& value : n.values)
                        if (value) collectCalls(*value, out);
                } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                    if (userFns.count(n.method)) out.insert(n.method);
                    if (n.receiver) collectCalls(*n.receiver, out);
                    for (const auto& a : n.args) if (a) collectCalls(*a, out);
                    if (n.block) collectCalls(**n.block, out);
                } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                    if (n.left) collectCalls(*n.left, out);
                    if (n.right) collectCalls(*n.right, out);
                } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                    if (n.operand) collectCalls(*n.operand, out);
                } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                    if (n.condition) collectCalls(*n.condition, out);
                    for (const auto& ex : n.thenBody) if (ex) collectCalls(*ex, out);
                    for (const auto& [c, b] : n.elifs) {
                        if (c) collectCalls(*c, out);
                        for (const auto& ex : b) if (ex) collectCalls(*ex, out);
                    }
                    if (n.elseBody)
                        for (const auto& ex : *n.elseBody) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                    if (n.subject) collectCalls(*n.subject, out);
                    for (const auto& cl : n.clauses) {
                        if (cl.guard && *cl.guard) collectCalls(**cl.guard, out);
                        if (cl.body) collectCalls(*cl.body, out);
                    }
                } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::LetExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::VarExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                    if (n.condition) collectCalls(*n.condition, out);
                    if (n.thenExpr) collectCalls(*n.thenExpr, out);
                    if (n.elseExpr) collectCalls(*n.elseExpr, out);
                } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                    if (n.expr) collectCalls(*n.expr, out);
                    if (n.condition) collectCalls(*n.condition, out);
                } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
                    if (n.value) collectCalls(*n.value, out);
                } else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                    if (n.condition) collectCalls(*n.condition, out);
                    for (const auto& ex : n.body) if (ex) collectCalls(*ex, out);
                } else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
                    if (n.inner) collectCalls(*n.inner, out);
                }
            }, e.kind);
        };

        std::unordered_map<std::string, std::set<std::string>> deps;
        // Keep a stable list of (name, item-index) for ordering.
        std::vector<std::pair<std::string, size_t>> fnOrder;
        for (size_t i = 0; i < program.items.size(); i++) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (!n) return;
                    std::set<std::string> d;
                    for (const auto& clause : n->clauses)
                        for (const auto& ex : clause.body) if (ex) collectCalls(*ex, d);
                    d.erase(n->name); // self-recursion: don't depend on self
                    deps[n->name] = std::move(d);
                    fnOrder.emplace_back(n->name, i);
                }
            }, program.items[i]);
        }

        // DFS post-order toposort (handles cycles via visited/onStack sets;
        // back-edge functions are left in original order — pre-registration
        // ensures they still type-check via shared TypeVar unification).
        std::unordered_set<std::string> visited, onStack;
        std::vector<std::string> sorted;
        std::function<void(const std::string&)> dfs = [&](const std::string& name) {
            if (visited.count(name)) return;
            if (onStack.count(name)) return; // back edge: cycle, skip
            onStack.insert(name);
            if (deps.count(name))
                for (const auto& dep : deps.at(name)) dfs(dep);
            onStack.erase(name);
            visited.insert(name);
            sorted.push_back(name);
        };
        for (const auto& [name, _] : fnOrder) dfs(name);

        // Build check order: sorted fn names first (deps before callers),
        // then any non-fn top-level items in their original positions.
        // We do two passes: first check all FunctionDefs in sorted order,
        // then check everything else (types, records, make blocks, main).
        std::unordered_set<std::string> fnChecked;
        for (const auto& name : sorted) {
            // Check ALL FunctionDefs with this name (handles overloaded functions
            // sharing a name — both overloads must be checked and appended to the
            // same overload set). Mark the name done only after the full sweep.
            for (const auto& item : program.items) {
                std::visit([&](const auto& n) {
                    using T = std::decay_t<decltype(n)>;
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        if (n && n->name == name) checkTopLevel(item);
                    }
                }, item);
            }
            fnChecked.insert(name);
        }
        // Non-function items and any unchecked functions (e.g. inside modules).
        for (const auto& item : program.items) {
            std::visit([&](const auto& n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                    if (n && !fnChecked.count(n->name)) checkTopLevel(item);
                } else {
                    checkTopLevel(item);
                }
            }, item);
        }
    }

    popScope();

    // `m_typeMap` records each expression's type as it was INFERRED, which for
    // anything inferred before its unification is a bare TypeVar; the concrete
    // type only lives in `m_subst`. Consumers outside this class (IR lowering,
    // via Analyzer::typeMap) have no access to the substitution, so they saw
    // `T1` where the checker itself would say `P` — and BEAM lowering silently
    // sent `let p = this; p.advance` to the prelude instead of the receiver's
    // own `make` block. Resolve once, here, so every consumer reads the answer
    // the checker actually reached. Unbound vars resolve to themselves, so a
    // genuinely un-inferred expression is unchanged.
    for (auto& [expr, type] : m_typeMap)
        type = resolve(type);
    // Same reasoning for pattern bindings: a binding's type is often a fresh
    // variable at the point the pattern is walked, and only a later use says
    // what it is.
    for (auto& binding : m_patternBindings)
        binding.type = resolve(binding.type);

    reportUnknownMethods();
}

// A method call whose NAME is not defined anywhere — the typo case. Both
// backends already fail on it at runtime ("Undefined method: shout for
// String"); saying so at check time is what makes an editor useful, and it is
// the single most common mistake a checker can catch.
//
// Deliberately name-based, not receiver-based: reporting "no such method FOR
// THIS TYPE" would need the receiver's type to be known, and it very often is
// not (a constructor-pattern binding carries none). A name that exists
// somewhere — including on a module-scoped `make` the call cannot see — is
// left to the runtime, which reports it with the receiver's real type.
auto TypeChecker::reportUnknownMethods() -> void {
    // With no imported interfaces the checker has never been told what the
    // standard library contains, so every prelude method would read as
    // undefined. Analyzers built that way (the tagged-literal validation
    // tests) are asking a different question entirely.
    if (!m_importedInterfaces) {
        m_unresolvedMethods.clear();
        return;
    }
    auto knownSomewhere = [&](const std::string& name) {
        if (m_methodSignatures.count(name) || m_userSignatures.count(name) ||
            m_annotatedMethods.count(name) || m_makeMethodNames.count(name))
            return true;
        for (const auto& [record, fields] : m_recordFields) {
            (void)record;
            if (fields.count(name)) return true;
        }
        for (const auto& [traitName, trait] : m_traits.all()) {
            (void)traitName;
            for (const auto& required : trait.requiredMethods)
                if (required.name == name) return true;
            for (const auto& defaulted : trait.defaultMethods)
                if (defaulted == name) return true;
        }
        if (m_importedInterfaces) {
            if (m_importedInterfaces->receiverFunctions.count(name)) return true;
            for (const auto& [moduleName, module] : m_importedInterfaces->modules) {
                (void)moduleName;
                if (module.exports.count(name)) return true;
            }
            for (const auto& [record, fields] :
                 m_importedInterfaces->recordFieldNames) {
                (void)record;
                if (fields.count(name)) return true;
            }
        }
        return false;
    };
    for (const auto& unresolved : m_unresolvedMethods) {
        if (knownSomewhere(unresolved.name)) continue;
        error(unresolved.location,
              "Undefined method `" + unresolved.name + "`" +
              (unresolved.receiver.empty() || unresolved.receiver == "Unknown"
                   ? std::string{}
                   : " for `" + unresolved.receiver + "`"));
    }
    m_unresolvedMethods.clear();
}

// `namesOnly` runs the same traversal for its record IDENTITIES alone. Type
// aliases are registered before field types can be resolved (a field may BE an
// alias), yet an alias body names records too — so the names have to exist
// first, and the field types are filled in on the second pass.
auto TypeChecker::registerRecordFields(const ast::Program& program,
                                       bool namesOnly) -> void {
    auto registerRecord = [&](const ast::RecordDef& record,
                              const std::string& owner = std::string{}) {
        const auto name = owner.empty()
            ? record.name : owner + "." + record.name;
        auto& fields = m_recordFields[name];
        if (namesOnly) return;
        const auto previousModule = m_currentModulePath;
        m_currentModulePath = owner;
        // `record Box<A>` binds A to the parameter slot -1 for the whole
        // declaration, the convention `substituteInterfaceGenerics` reads and
        // the one the KexI reader rebuilds imported records with. A map
        // shared across ALL records — which this used to be — did neither:
        // every record's `A` collapsed onto one fresh positive TypeVar, so a
        // field's element type could never be recovered from a receiver's
        // type arguments (kexhq/kex#203).
        std::unordered_map<std::string, TypePtr> generics;
        for (size_t i = 0; i < record.typeParams.size(); ++i)
            generics[record.typeParams[i]] =
                Type::typeVar(-static_cast<int>(i + 1));
        for (const auto& field : record.fields)
        {
            auto fieldType = field.type
                ? resolveTypeExpr(*field.type, generics)
                : Type::unknown();
            fields[field.name] = fieldType;
            if ((!field.defaultValue || !*field.defaultValue) &&
                !std::holds_alternative<OptionalType>(fieldType->kind))
                m_requiredRecordFields[name].insert(field.name);
        }
        m_currentModulePath = previousModule;
    };
    std::function<void(const ast::ModuleDef&)> registerModule;
    registerModule = [&](const ast::ModuleDef& module) {
      for (const auto& item : module.body) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (node) registerRecord(*node, module.name);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                // A `private do ... end` record is still the module's own
                // record, and its identity has to be the qualified one its
                // module constructs.
                if (!node) return;
                for (const auto& visible : node->items)
                    if (const auto* record =
                            std::get_if<std::unique_ptr<ast::RecordDef>>(&visible);
                        record && *record)
                        registerRecord(**record, module.name);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerModule(*node);
            }
        }, item);
      }
    };
    for (const auto& item : program.items)
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (node) registerRecord(*node);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (!node) return;
                for (const auto& visible : node->items)
                    if (const auto* record =
                            std::get_if<std::unique_ptr<ast::RecordDef>>(&visible);
                        record && *record)
                        registerRecord(**record);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerModule(*node);
            }
        }, item);
}

auto TypeChecker::resolveRecordName(const std::string& name) const
    -> std::string {
    auto recordExists = [&](const std::string& candidate) {
        return m_recordFields.count(candidate) ||
               (m_importedInterfaces &&
                m_importedInterfaces->recordArities.count(candidate));
    };
    if (!m_currentModulePath.empty()) {
        auto scope = m_currentModulePath;
        while (!scope.empty()) {
            const auto candidate = scope + "." + name;
            if (recordExists(candidate)) return candidate;
            const auto dot = scope.rfind('.');
            if (dot == std::string::npos) break;
            scope.resize(dot);
        }
    }
    auto importedRecord = [&](const ImportSelection& import)
        -> std::optional<std::string> {
        const auto candidate = import.module + "." + name;
        if (!recordExists(candidate)) return std::nullopt;
        if (!import.onlyNames.empty() &&
            std::find(import.onlyNames.begin(), import.onlyNames.end(), name) ==
                import.onlyNames.end())
            return std::nullopt;
        if (std::find(import.exceptNames.begin(), import.exceptNames.end(),
                      name) != import.exceptNames.end())
            return std::nullopt;
        return candidate;
    };
    for (auto scope = m_importScopeStack.rbegin();
         scope != m_importScopeStack.rend(); ++scope)
        for (auto import = scope->rbegin(); import != scope->rend(); ++import)
            if (auto found = importedRecord(*import)) return *found;
    for (auto import = m_declarationImports.rbegin();
         import != m_declarationImports.rend(); ++import)
        if (auto found = importedRecord(*import)) return *found;
    // A record declared in this file under the bare name owns that name. The
    // last-ditch suffix scan below guesses at a module nobody imported, and a
    // guess must never outrank a declaration sitting in front of the user:
    // stdlib records live in m_importedInterfaces whether or not they were
    // imported, so `record Request` in a plain script resolved to
    // `Net.HTTP.Request` and reported its fields instead.
    if (m_recordFields.count(name)) return name;
    if (m_importedInterfaces) {
        std::optional<std::string> unique;
        const auto suffix = "." + name;
        for (const auto& [candidate, _] :
             m_importedInterfaces->recordArities) {
            if (candidate.size() <= suffix.size() ||
                candidate.compare(candidate.size() - suffix.size(),
                                  suffix.size(), suffix) != 0)
                continue;
            if (unique && *unique != candidate) return name;
            unique = candidate;
        }
        if (unique) return *unique;
    }
    return name;
}

// Resolves a bare module segment against the modules `using` brought into
// scope, when `name` is not itself a registered module — `Set.from([…])`
// after `using Data.Set` writes the receiver as `Set`, but the module is
// registered as `Data.Set` (a file-level `module Data` header qualifies the
// nested `module Set do … end` constructor block that provides it).
//
// Precedence, most to least specific:
//   1. `name` is already a real key — never touched (this is what keeps
//      `Units.Data` and `Data.Set` unambiguous when a file imports both:
//      a literal `Data.Set.from(…)` matches this tier before any alias is
//      considered, so the `Data` → `Units.Data` alias below never gets a
//      chance to capture it).
//   2. An active `using M` whose last segment is `name` (`using Data.Set`
//      aliases bare `Set`), or whose immediate child `M.name` is a real
//      module (`using Net.HTTP` aliases bare `Status` to `Net.HTTP.Status`).
//      Two imports aliasing the same bare segment is an ambiguity — leave
//      `name` unresolved rather than guessing.
//   3. A globally unique module whose last segment is `name`, the same
//      last-resort `resolveRecordName` takes for record names — this is what
//      lets `UnorderedSet.from(…)` work from the same `using Data.Set` that
//      only aliases `Set` under rule 2, since `Data.Set` and `Data.UnorderedSet`
//      share a file but not a parent/child relationship.
auto TypeChecker::resolveModulePath(const std::string& name,
                                    const std::string& member) const
    -> std::string {
    if (!m_importedInterfaces) return name;
    // A `make X<A> do ... end` block unconditionally creates a placeholder
    // `ifaces.modules[typeName]` entry while scanning its own methods
    // (`prelude_interfaces.hxx`'s `collectMakeMember`), keyed by the type's
    // BARE written name regardless of the file's enclosing `module` header —
    // so `ifaces.modules["Set"]` exists even though `Set`'s static
    // constructors (`from`, `empty`) were published under the qualified
    // `Data.Set`. A mere key match is therefore not enough to trust `name`
    // literally; the literal module must actually export `member`.
    auto exportsMember = [&](const std::string& module) {
        auto found = m_importedInterfaces->modules.find(module);
        if (found == m_importedInterfaces->modules.end()) return false;
        auto exported = found->second.exports.find(member);
        return exported != found->second.exports.end() && !exported->second.empty();
    };
    if (exportsMember(name)) return name;
    auto lastSegmentOf = [](const std::string& module) {
        const auto dot = module.rfind('.');
        return dot == std::string::npos ? module : module.substr(dot + 1);
    };
    auto consider = [&](const std::string& module,
                        std::vector<std::string>& candidates) {
        if (lastSegmentOf(module) == name && exportsMember(module))
            candidates.push_back(module);
        const auto child = module + "." + name;
        if (exportsMember(child)) candidates.push_back(child);
    };
    std::vector<std::string> candidates;
    for (const auto& import : m_declarationImports) consider(import.module, candidates);
    for (const auto& scope : m_importScopeStack)
        for (const auto& import : scope) consider(import.module, candidates);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.size() == 1) return candidates.front();
    if (!candidates.empty()) return name;

    std::optional<std::string> unique;
    const auto suffix = "." + name;
    for (const auto& [candidate, _] : m_importedInterfaces->modules) {
        if (candidate.size() <= suffix.size() ||
            candidate.compare(candidate.size() - suffix.size(), suffix.size(),
                              suffix) != 0 ||
            !exportsMember(candidate))
            continue;
        if (unique && *unique != candidate) return name;
        unique = candidate;
    }
    return unique.value_or(name);
}

auto TypeChecker::registerAdt(const ast::TypeDef& def) -> void {
    if (!def.variants) return;

    if (def.isDistinct || kex::isTransparentTypeAlias(def)) return;

    std::vector<std::string> names;
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) return;  // not constructor-shaped — a type alias, skip entirely
        names.push_back(*name);
        if (std::holds_alternative<ast::TypeName>(variant->kind))
            m_nullaryConstructors.insert(*name);
        // Payload arity: `None` is 0, `Just(T)` is 1, `Between(A, B)` is 2.
        if (auto* gt = std::get_if<ast::GenericType>(&variant->kind))
            m_constructorArity[*name] = gt->args.size();
        else
            m_constructorArity[*name] = 0;
    }
    if (names.empty()) return;

    m_adtVariants[def.name] = names;
    for (const auto& name : names) {
        m_adtOfConstructor[name] = def.name;
    }

    // Record what each constructor produces, so `Just(1)` infers as an
    // Optional instead of `unknown` — without that, a mismatched pattern in
    // `if let Ok(x) = Just(1)` has no scrutinee type to be checked against.
    for (const auto& variant : *def.variants) {
        auto name = extractConstructorName(variant);
        if (!name) continue;
        ConstructorResult result;
        result.adtName = def.name;
        result.typeParamCount = def.typeParams.size();
        if (const auto* generic = std::get_if<ast::GenericType>(&variant->kind)) {
            for (const auto& payload : generic->args) {
                int slot = -1;
                if (payload) {
                    if (const auto* named =
                            std::get_if<ast::TypeName>(&payload->kind);
                        named && named->parts.size() == 1) {
                        auto found = std::find(def.typeParams.begin(),
                                               def.typeParams.end(),
                                               named->parts.front());
                        if (found != def.typeParams.end())
                            slot = static_cast<int>(
                                found - def.typeParams.begin());
                    } else if (const auto* var =
                                   std::get_if<ast::GenericVar>(&payload->kind)) {
                        auto found = std::find(def.typeParams.begin(),
                                               def.typeParams.end(), var->name);
                        if (found != def.typeParams.end())
                            slot = static_cast<int>(
                                found - def.typeParams.begin());
                    }
                }
                result.slots.push_back(slot);
                // Keep the declared type too, so a pattern binding can take
                // its type from the declaration rather than waiting for a use
                // to constrain it.
                TypePtr declared;
                if (payload) {
                    std::unordered_map<std::string, TypePtr> generics;
                    declared = resolveTypeExpr(*payload, generics);
                    if (declared &&
                        std::holds_alternative<UnknownType>(declared->kind))
                        declared = nullptr;
                }
                result.payloadTypes.push_back(std::move(declared));
            }
        }
        m_constructorResult[*name] = std::move(result);
    }
}

auto TypeChecker::constructorResultType(
    const std::string& name, const std::vector<TypePtr>& argTypes)
    -> TypePtr {
    auto found = m_constructorResult.find(name);
    if (found == m_constructorResult.end()) return nullptr;
    const auto& info = found->second;

    std::vector<TypePtr> typeArgs;
    typeArgs.reserve(info.typeParamCount);
    for (size_t i = 0; i < info.typeParamCount; ++i)
        typeArgs.push_back(freshTypeVar());
    for (std::size_t i = 0; i < info.slots.size() && i < argTypes.size(); ++i) {
        const auto slot = info.slots[i];
        if (slot >= 0 && static_cast<std::size_t>(slot) < typeArgs.size() &&
            argTypes[i])
            typeArgs[slot] = argTypes[i];
    }

    // `X?` is its own type kind everywhere else in the checker, so the
    // prelude's Optional has to produce that rather than a NamedType.
    if (info.adtName == "Optional")
        return Type::optional(typeArgs.empty() ? Type::unknown() : typeArgs[0]);
    return Type::named(info.adtName, std::move(typeArgs));
}

auto TypeChecker::registerAdtsInModule(const ast::ModuleDef& mod) -> void {
    for (const auto& item : mod.body) {
        std::visit([this, &mod](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node) return;
                if (auto constructors = kex::typeConstructors(*node))
                    for (const auto& constructor : *constructors)
                        m_moduleConstructors[mod.name][constructor.name] = {
                            node->name, constructor.arity, true};
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerAdtsInModule(*node);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (!node) return;
                for (const auto& visible : node->items)
                    if (const auto* type =
                            std::get_if<std::unique_ptr<ast::TypeDef>>(
                                &visible);
                        type && *type)
                        if (auto constructors =
                                kex::typeConstructors(**type))
                            for (const auto& constructor : *constructors)
                                m_moduleConstructors[mod.name]
                                                    [constructor.name] = {
                                    (*type)->name, constructor.arity,
                                    node->isPublic};
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

auto TypeChecker::resolveDistinctName(const std::string& name) const
    -> std::string {
    if (name.find('.') != std::string::npos && m_distinctTypes.count(name))
        return name;
    auto scope = m_currentModulePath;
    while (!scope.empty()) {
        const auto candidate = scope + "." + name;
        if (m_distinctTypes.count(candidate)) return candidate;
        const auto dot = scope.rfind('.');
        if (dot == std::string::npos) break;
        scope.resize(dot);
    }
    return name;
}

auto TypeChecker::distinctBacking(const TypePtr& type) const -> TypePtr {
    if (!type) return type;
    const auto* named = std::get_if<NamedType>(&type->kind);
    if (!named) return type;
    auto found = m_distinctTypes.find(named->name);
    if (found == m_distinctTypes.end()) {
        const auto resolvedName = resolveDistinctName(named->name);
        found = m_distinctTypes.find(resolvedName);
    }
    if (found == m_distinctTypes.end() || !found->second.backingType)
        return type;

    std::unordered_map<int, TypePtr> substitutions;
    for (size_t i = 0; i < named->typeArgs.size(); ++i)
        substitutions[-static_cast<int>(i + 1)] = named->typeArgs[i];
    std::function<TypePtr(const TypePtr&)> substitute =
        [&](const TypePtr& part) -> TypePtr {
            if (!part) return part;
            if (const auto* var = std::get_if<TypeVar>(&part->kind)) {
                if (auto value = substitutions.find(var->id);
                    value != substitutions.end())
                    return value->second;
                return part;
            }
            if (const auto* n = std::get_if<NamedType>(&part->kind)) {
                std::vector<TypePtr> args;
                for (const auto& arg : n->typeArgs)
                    args.push_back(substitute(arg));
                return Type::named(n->name, std::move(args));
            }
            if (const auto* list = std::get_if<ListType>(&part->kind))
                return Type::list(substitute(list->element));
            if (const auto* map = std::get_if<MapType>(&part->kind))
                return Type::map(substitute(map->key), substitute(map->value));
            if (const auto* optional = std::get_if<OptionalType>(&part->kind))
                return Type::optional(substitute(optional->inner));
            if (const auto* tuple = std::get_if<TupleType>(&part->kind)) {
                std::vector<TypePtr> elements;
                for (const auto& element : tuple->elements)
                    elements.push_back(substitute(element));
                return Type::tuple(std::move(elements));
            }
            if (const auto* fn = std::get_if<FuncType>(&part->kind)) {
                std::vector<TypePtr> params;
                for (const auto& param : fn->params)
                    params.push_back(substitute(param));
                return Type::func(std::move(params), substitute(fn->result));
            }
            if (const auto* intersection =
                    std::get_if<IntersectionType>(&part->kind)) {
                std::vector<TypePtr> members;
                for (const auto& member : intersection->members)
                    members.push_back(substitute(member));
                return Type::intersection(std::move(members));
            }
            if (const auto* record = std::get_if<RecordType>(&part->kind)) {
                std::vector<std::pair<std::string, TypePtr>> fields;
                for (const auto& [name, fieldType] : record->fields)
                    fields.emplace_back(name, substitute(fieldType));
                return Type::record(std::move(fields));
            }
            return part;
        };
    auto backing = substitute(found->second.backingType);
    return distinctBacking(backing);
}

auto TypeChecker::representationsCompatible(const TypePtr& from,
                                            const TypePtr& to) -> bool {
    const auto source = distinctBacking(resolve(from));
    const auto target = distinctBacking(resolve(to));
    if (typesEqual(source, target)) return true;
    return (m_traits.satisfies(source, "Integer") &&
            m_traits.satisfies(target, "Integer")) ||
           (m_traits.satisfies(source, "Float") &&
            m_traits.satisfies(target, "Float"));
}

// `check()` runs every FunctionDef before any make block (see its two-pass
// comment), so while a function body is being checked no LOCAL trait
// conformance has been registered yet — only imported ones and the builtins.
// A trait-typed default parameter value therefore had nothing to match
// against and was rejected (kexhq/kex#175). Registering the declared
// (type, trait) pairs up front fixes that; `checkTraitImplementation` still
// runs later and still reports a make block that fails to provide the
// trait's required methods.
// `capability Name do ... end` declares an interface AND its default
// implementation in one. The interface half is a trait synthesized from the
// module's own public functions, so `make Fake, implement: Name` works with
// the machinery that already exists, and the bodies remain the default the
// capability carries outside any `with` (kexhq/kex#143).
auto TypeChecker::registerCapabilityTraits(const ast::Program& program) -> void {
    std::function<void(const ast::ModuleDef&)> walk =
        [&](const ast::ModuleDef& module) {
        if (module.isCapability) {
            m_capabilities.insert(module.name);
            TraitDef td;
            td.name = module.name;
            for (const auto& mi : module.body)
                std::visit([&](const auto& mn) {
                    using MT = std::decay_t<decltype(mn)>;
                    if constexpr (std::is_same_v<MT,
                                                 std::unique_ptr<ast::FunctionDef>>) {
                        if (!mn || mn->clauses.empty()) return;
                        // A capability's INTERFACE is its `foul` members. A
                        // pure member performs no effect, so there is nothing
                        // for a stand-in to substitute and no reason to make
                        // one implement it — `IO.inspect` is deliberately pure
                        // (it is the debug hatch purity checking ignores) and
                        // requiring it would have kept `IO` from being a
                        // capability at all (kexhq/kex#143).
                        if (!mn->isFoul) return;
                        Signature sig;
                        sig.name = mn->name;
                        sig.isFoul = mn->isFoul;
                        // Arity matters for conformance; the parameter types
                        // stay permissive here because the capability's own
                        // bodies are checked like any other module's.
                        for (size_t i = 0; i < mn->clauses.front().params.size(); i++)
                            sig.params.push_back(Type::unknown());
                        sig.result = Type::unknown();
                        td.requiredMethods.push_back(std::move(sig));
                    }
                }, mi);
            m_traits.define(std::move(td));
        }
        for (const auto& mi : module.body)
            std::visit([&](const auto& mn) {
                using MT = std::decay_t<decltype(mn)>;
                if constexpr (std::is_same_v<MT, std::unique_ptr<ast::ModuleDef>>) {
                    if (mn) walk(*mn);
                }
            }, mi);
    };
    for (const auto& item : program.items)
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) walk(*node);
            }
        }, item);

    // A capability declared in an imported module is not in this AST, so its
    // interface is the only place its members are known. Registering it here
    // is what lets a consumer write `with FS.File = ...` and
    // `make Fake, implement: FS.File` against a capability it did not declare.
    if (m_importedInterfaces)
        for (const auto& [name, interface] : m_importedInterfaces->modules) {
            if (!interface.isCapability) continue;
            m_capabilities.insert(name);
            if (m_traits.get(name)) continue;
            TraitDef td;
            td.name = name;
            for (const auto& [member, overloads] : interface.exports) {
                if (overloads.empty()) continue;
                // Foul members only — see the local branch above.
                if (!overloads.front().signature.isFoul) continue;
                Signature sig = overloads.front().signature;
                sig.name = member;
                td.requiredMethods.push_back(std::move(sig));
            }
            // `exports` is a hash map, so the loop above yields its members in
            // an order that differs between standard-library implementations.
            // A trait's required methods are its DICTIONARY LAYOUT — slot i
            // means one method to whoever builds the dictionary and another to
            // whoever indexes it — so an unsorted list makes the emitted code
            // depend on the host's hash order. It built and passed on macOS
            // and died `badarg` on Linux, on both glibc and musl
            // (kexhq/kex#143).
            std::sort(td.requiredMethods.begin(), td.requiredMethods.end(),
                      [](const Signature& a, const Signature& b) {
                          if (a.name != b.name) return a.name < b.name;
                          return a.params.size() < b.params.size();
                      });
            m_traits.define(std::move(td));
        }
}

auto TypeChecker::registerLocalConformances(const ast::Program& program) -> void {
    // A record declared in a module is known by its QUALIFIED name, so a
    // `make` block inside that module has to register its conformance under
    // the same name. Registering the bare one left `make Files, implement:
    // FS.File` inside `module Mock` conforming a type called `Files` that
    // nothing is, and `with FS.File = Mock.Files { ... }` was rejected as not
    // implementing the capability (kexhq/kex#143).
    auto registerOne = [this](const ast::MakeDef& def,
                              const std::string& modulePath) {
        if (def.implements.empty() || !def.target) return;
        std::string typeName;
        if (auto* tn = std::get_if<ast::TypeName>(&def.target->kind))
            if (tn->parts.size() == 1) typeName = tn->parts[0];
        if (auto* gt = std::get_if<ast::GenericType>(&def.target->kind))
            if (gt->name.parts.size() == 1) typeName = gt->name.parts[0];
        if (typeName.empty()) return;
        for (const auto& traitName : def.implements) {
            m_traits.registerImplementation(typeName, traitName);
            if (!modulePath.empty())
                m_traits.registerImplementation(modulePath + "." + typeName,
                                                traitName);
        }
    };
    std::function<void(const ast::ModuleDef&, const std::string&)> walkModule =
        [&](const ast::ModuleDef& module, const std::string& prefix) {
        const std::string path =
            prefix.empty() ? module.name : prefix + "." + module.name;
        for (const auto& mi : module.body)
            std::visit([&](const auto& mn) {
                using MT = std::decay_t<decltype(mn)>;
                if constexpr (std::is_same_v<MT, std::unique_ptr<ast::MakeDef>>) {
                    if (mn) registerOne(*mn, path);
                } else if constexpr (std::is_same_v<MT,
                                                    std::unique_ptr<ast::ModuleDef>>) {
                    if (mn) walkModule(*mn, path);
                }
            }, mi);
    };
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) registerOne(*node, "");
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) walkModule(*node, "");
            }
        }, item);
    }
}

auto TypeChecker::registerTraits(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                if (!node) return;
                TraitDef td;
                td.name = node->name;
                for (const auto& bodyItem : node->body) {
                    std::visit([&](const auto& bi) {
                        using BT = std::decay_t<decltype(bi)>;
                        if constexpr (std::is_same_v<BT, std::unique_ptr<ast::TypeAnnotation>>) {
                            if (!bi) return;
                            auto sig = annotationToSignature(*bi);
                            if (sig) {
                                sig->isFoul = bi->isFoul;
                                td.requiredMethods.push_back(std::move(*sig));
                            }
                        }
                        // FunctionDef items are default implementations: no
                        // signature of their own, but the NAME matters —
                        // every implementing type inherits it.
                        else if constexpr (std::is_same_v<BT, std::unique_ptr<ast::FunctionDef>>) {
                            if (bi) td.defaultMethods.push_back(bi->name);
                        }
                    }, bodyItem);
                }
                m_traits.define(std::move(td));
            }
        }, item);
    }
}

auto TypeChecker::registerTypeAliases(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                if (node->isDistinct) {
                    std::unordered_map<std::string, TypePtr> vars;
                    for (size_t i = 0; i < node->typeParams.size(); ++i)
                        vars[node->typeParams[i]] =
                            Type::typeVar(-static_cast<int>(i + 1));
                    m_distinctTypes[node->name] = {
                        node->typeParams,
                        resolveTypeExpr(*node->variants->front(), vars)};
                    return;
                }
                // `type Row = Type.returnedBy(parseRow)` resolves against
                // function signatures, which are registered after this pass —
                // so it is deferred rather than resolved here.
                if (node->variants->size() == 1) {
                    if (auto* query = std::get_if<ast::TypeQuery>(
                            &(*node->variants)[0]->kind)) {
                        // Resolved after the builtin type names are
                        // registered — before them, `String` is just a name
                        // and the alias would bind NamedType("String")
                        // instead of the real string type.
                        m_computedAliases.push_back({node->name, query});
                        return;
                    }
                }
                // Transparent type alias: single bare TypeName — register
                // immediately, before falling through to the constructor check.
                // A leading `|` opts out, declaring a one-variant ADT instead.
                if (node->variants->size() == 1 && !node->leadingPipe) {
                    const auto& only = (*node->variants)[0]->kind;
                    // An APPLIED generic is transparent too. `type ReadOnlyFile
                    // = FileHandle<CanRead, CannotWrite>` looks
                    // constructor-shaped to the check below — `Name<args>`
                    // parses much like `Name(args)` — so it fell through and
                    // became an opaque type of its own, matching not even the
                    // type it aliases (kexhq/kex#173).
                    const auto* generic = std::get_if<ast::GenericType>(&only);
                    if (std::holds_alternative<ast::TypeName>(only) ||
                        (generic && generic->applied)) {
                        m_typeAliases[node->name] = typeDefToType(*node);
                        return;
                    }
                }
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
    // An alias body names types the way its own module sees them: `type
    // Handler = Request -> Response` inside `module Http` means Http's
    // records, not a same-named record some other module exports.
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = previousModule.empty() ||
                                  mod.name.rfind(previousModule + ".", 0) == 0
        ? mod.name
        : previousModule + "." + mod.name;
    for (const auto& item : mod.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (!node->variants) return;
                if (node->isDistinct) {
                    std::unordered_map<std::string, TypePtr> vars;
                    for (size_t i = 0; i < node->typeParams.size(); ++i)
                        vars[node->typeParams[i]] =
                            Type::typeVar(-static_cast<int>(i + 1));
                    ImportedDistinctType info{
                        node->typeParams,
                        resolveTypeExpr(*node->variants->front(), vars)};
                    m_distinctTypes[node->name] = info;
                    m_distinctTypes[m_currentModulePath + "." + node->name] =
                        std::move(info);
                    return;
                }
                // `type Row = Type.returnedBy(parseRow)` resolves against
                // function signatures, which are registered after this pass —
                // so it is deferred rather than resolved here.
                if (node->variants->size() == 1) {
                    if (auto* query = std::get_if<ast::TypeQuery>(
                            &(*node->variants)[0]->kind)) {
                        // Resolved after the builtin type names are
                        // registered — before them, `String` is just a name
                        // and the alias would bind NamedType("String")
                        // instead of the real string type.
                        m_computedAliases.push_back({node->name, query});
                        return;
                    }
                }
                // Transparent type alias: single bare TypeName — register
                // immediately, before falling through to the constructor check.
                // A leading `|` opts out, declaring a one-variant ADT instead.
                if (node->variants->size() == 1 && !node->leadingPipe) {
                    const auto& only = (*node->variants)[0]->kind;
                    // An APPLIED generic is transparent too. `type ReadOnlyFile
                    // = FileHandle<CanRead, CannotWrite>` looks
                    // constructor-shaped to the check below — `Name<args>`
                    // parses much like `Name(args)` — so it fell through and
                    // became an opaque type of its own, matching not even the
                    // type it aliases (kexhq/kex#173).
                    const auto* generic = std::get_if<ast::GenericType>(&only);
                    if (std::holds_alternative<ast::TypeName>(only) ||
                        (generic && generic->applied)) {
                        m_typeAliases[node->name] = typeDefToType(*node);
                        return;
                    }
                }
                for (const auto& v : *node->variants) {
                    if (extractConstructorName(v)) return;
                }
                m_typeAliases[node->name] = typeDefToType(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                registerTypeAliasesInModule(*node);
            }
        }, item);
    }
    m_currentModulePath = previousModule;
}

auto TypeChecker::annotationToSignature(
    const ast::TypeAnnotation& ann,
    std::unordered_map<std::string, TypePtr>* sharedGenericVars)
    -> std::optional<Signature> {
    if (!ann.type) return std::nullopt;
    // Unroll `A -> B -> C` (right-nested FunctionType) into params=[A,B], result=C.
    std::unordered_map<std::string, TypePtr> ownGenericVars;
    auto& genericVars = sharedGenericVars ? *sharedGenericVars : ownGenericVars;
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
    auto add = [&](const ast::TypeAnnotation& ann, const std::string& modulePath) {
        auto sig = annotationToSignature(ann);
        if (!sig) return;
        // Declared annotation wins — stored first so checkFunctionDef
        // can find it and verify the body against the declared type.
        m_annotationDeclared.insert(ann.name);
        const auto key = modulePath + "\n" + ann.name;
        m_annotationArities[key].insert(sig->params.size());
        m_scopedDeclaredSignatures[key].push_back(*sig);
        auto& sigs = m_userSignatures[ann.name];
        // Insert declared sig at front, replacing any same-arity inferred
        // one that was somehow already there (shouldn't happen in pre-pass
        // ordering, but guard for safety).
        sigs.insert(sigs.begin(), std::move(*sig));
    };
    for (const auto& item : program.items)
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item);
            ann && *ann)
            add(**ann, "");
        else if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
                 mod && *mod)
            registerDeclaredSignaturesInModule(**mod);
}

auto TypeChecker::registerDeclaredSignaturesInModule(
    const ast::ModuleDef& mod, const std::string& parentPath) -> void {
    const auto modulePath =
        parentPath.empty() || mod.name.starts_with(parentPath + ".")
            ? mod.name : parentPath + "." + mod.name;
    const auto previousModule = m_currentModulePath;
    m_currentModulePath = modulePath;
    auto importsFor = [&](const std::string& name)
        -> const std::vector<ImportSelection>* {
        for (const auto& item : mod.body) {
            if (const auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
                fn && *fn && (*fn)->name == name) {
                if (auto found = m_functionImports.find(fn->get());
                    found != m_functionImports.end())
                    return &found->second;
            }
            if (const auto* visibility =
                    std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                visibility && *visibility) {
                for (const auto& visible : (*visibility)->items)
                    if (const auto* fn = std::get_if<
                            std::unique_ptr<ast::FunctionDef>>(&visible);
                        fn && *fn && (*fn)->name == name)
                        if (auto found = m_functionImports.find(fn->get());
                            found != m_functionImports.end())
                            return &found->second;
            }
        }
        return nullptr;
    };
    auto add = [this, &modulePath, &importsFor](
                   const ast::TypeAnnotation& ann, bool exposeUnqualified) {
        const auto previousImports = m_declarationImports;
        if (const auto* imports = importsFor(ann.name))
            m_declarationImports = *imports;
        auto sig = annotationToSignature(ann);
        m_declarationImports = previousImports;
        if (!sig) return;
        m_annotationDeclared.insert(ann.name);
        const auto key = modulePath + "\n" + ann.name;
        m_annotationArities[key].insert(sig->params.size());
        m_scopedDeclaredSignatures[key].push_back(*sig);
        if (exposeUnqualified)
            m_userSignatures[ann.name].insert(
                m_userSignatures[ann.name].begin(), std::move(*sig));
    };
    for (const auto& item : mod.body) {
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item);
            ann && *ann) {
            add(**ann, true);
        } else if (auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items)
                if (auto* visibleAnn =
                        std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&visible);
                    visibleAnn && *visibleAnn)
                    add(**visibleAnn, false);
        } else if (auto* nested =
                       std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
                   nested && *nested) {
            registerDeclaredSignaturesInModule(**nested, modulePath);
        }
    }
    m_currentModulePath = previousModule;
}

auto TypeChecker::registerMakeSignature(const ast::MakeDef& def,
                                       const std::string& modulePath) -> void {
    // Every method name this block defines, private ones included. Only the
    // NAME is needed here: it answers "does this method exist anywhere?" for
    // the unknown-method report, which is name-based by design.
    auto noteName = [&](const ast::FunctionDef& fn) {
        m_makeMethodNames.insert(fn.name);
    };
    for (const auto& item : def.body) {
        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*fn) noteName(**fn);
        } else if (const auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
            if (!*visibility) continue;
            for (const auto& inner : (*visibility)->items)
                if (const auto* vfn =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner))
                    if (*vfn) noteName(**vfn);
        }
    }
    if (!def.target) return;
    // The target names a type the way source inside the module does, so it has
    // to resolve in that module's scope. Without this a `make Server` inside
    // `module CollisionWeb` resolved bare `Server` through the global
    // unique-suffix fallback and registered its methods against the prelude's
    // `Web.Server`, which checkMakeDef (which does set the path) then
    // registered a second time under the right name.
    const auto previousModulePath = m_currentModulePath;
    m_currentModulePath = modulePath;
    struct PathRestore {
        std::string& target;
        const std::string& saved;
        ~PathRestore() { target = saved; }
    } pathRestore{m_currentModulePath, previousModulePath};
    std::unordered_map<std::string, TypePtr> targetVars;
    auto receiver = resolveTypeExpr(*def.target, targetVars);
    std::unordered_set<std::string> slotNames;
    if (def.isServing)
        for (const auto& item : def.body)
            if (const auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&item);
                fn && *fn && (*fn)->isSlot)
                slotNames.insert((*fn)->name);
    m_slotMethodNames.insert(slotNames.begin(), slotNames.end());

    auto remoteSlotSignature = [&](const Signature& handler,
                                   bool hasFrom) -> Signature {
        Signature remote = handler;
        remote.params.front() = Type::named("Server", {receiver});
        if (hasFrom) {
            TypePtr reply = Type::unknown();
            if (auto* named = std::get_if<NamedType>(&handler.result->kind);
                named && named->name == "Reply" && named->typeArgs.size() == 1)
                reply = named->typeArgs.front();
            remote.result = Type::named(
                "Result", {reply, Type::named("CallError")});
            remote.isFoul = true;
        } else {
            remote.result = Type::unit();
            remote.isFoul = true;
        }
        return remote;
    };

    for (const auto& item : def.body) {
        auto add = [&](const std::unique_ptr<ast::TypeAnnotation>& ann) {
            if (!ann) return;
            auto sig = annotationToSignature(*ann, &targetVars);
            if (!sig) return;
            const bool replyResult = [&] {
                auto result = resolve(sig->result);
                auto* named = std::get_if<NamedType>(&result->kind);
                return named && named->name == "Reply" &&
                       named->typeArgs.size() == 1;
            }();
            if (def.isServing && slotNames.count(ann->name) &&
                ann->implicitFrom && !replyResult)
                error(ann->location,
                      "`::>` is not valid on a cast slot — casts have no `from`; use `:>`");
            sig->params.insert(sig->params.begin(), receiver);
            m_annotatedMethods.insert(ann->name);
            m_annotatedReceiverKeys[ann->name].insert(typeToString(receiver));
            // Skip a signature already registered with the same parameters —
            // the same guard checkFunctionDef applies. A prelude type whose
            // method is registered both from the imported interface and from
            // the `:>` annotation in its own source otherwise ends up with two
            // identical entries, and overload resolution reports the call
            // ambiguous against ITSELF: `m.get(1, "")` on a Regex.Match (the
            // documented example) failed with the same signature printed twice.
            auto& existing = m_methodSignatures[ann->name];
            const bool duplicate =
                std::any_of(existing.begin(), existing.end(),
                            [&](const Signature& other) {
                                if (other.params.size() != sig->params.size())
                                    return false;
                                for (size_t i = 0; i < other.params.size(); i++)
                                    if (!typesEqual(other.params[i], sig->params[i]))
                                        return false;
                                return true;
                            });
            sig->makeModule = modulePath;
            if (!duplicate) existing.push_back(*sig);
            if (def.isServing && slotNames.count(ann->name)) {
                auto remote = remoteSlotSignature(*sig, replyResult);
                const bool remoteDuplicate = std::any_of(
                    existing.begin(), existing.end(), [&](const Signature& other) {
                        if (other.params.size() != remote.params.size()) return false;
                        for (size_t i = 0; i < other.params.size(); ++i)
                            if (!typesEqual(other.params[i], remote.params[i]))
                                return false;
                        return true;
                    });
                if (!remoteDuplicate) existing.push_back(std::move(remote));
            }
        };
        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
            add(*ann);
        } else if (auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items)
                if (auto* visibleAnn =
                        std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&visible))
                    add(*visibleAnn);
        }
    }

    // A method written as a plain `let` — no `:>` — was registered only when
    // its make block was CHECKED, which is after every top-level function body.
    // So `let grow(c: Crate) = c.doubled` found no `doubled` at all and typed
    // as unknown, while the identical call inside `main` (checked last) worked.
    // Registering the declared shape here makes the method visible to whatever
    // is checked first; the body still refines it later.
    const auto preRegister = [&](const ast::FunctionDef& method) {
        // Only THIS type's own `:>` contract for `method.name` should defer
        // to it — an unrelated type's same-named annotation (Char's
        // `string :> String` vs. Input's own plain `let string(...)`) must
        // not block this type's method from registering at all (#251).
        if (hasAnnotatedSignatureForReceiver(method.name, receiver)) return;
        if (method.clauses.empty()) return;
        const auto& clause = method.clauses.front();
        // Only plain named parameters. A pattern in first position is the
        // receiver-matching form (`let head(@[x | _])`) whose arity must not
        // gain an implicit receiver, and a type-selector argument
        // (`let to(String)`) is not a parameter type at all.
        for (const auto& param : clause.params)
            if (param.pattern || !param.name) return;
        Signature signature;
        signature.name = method.name;
        signature.isFoul = method.isFoul;
        signature.params.push_back(receiver);
        for (const auto& param : clause.params)
            signature.params.push_back(
                param.type ? resolveTypeExpr(**param.type, targetVars)
                           : freshTypeVar());
        signature.result = clause.returnAnnotation
            ? resolveTypeExpr(**clause.returnAnnotation, targetVars)
            : freshTypeVar();
        signature.makeModule = modulePath;
        auto& existing = m_methodSignatures[method.name];
        const bool duplicate =
            std::any_of(existing.begin(), existing.end(),
                        [&](const Signature& other) {
                            if (other.params.size() != signature.params.size())
                                return false;
                            for (size_t i = 0; i < other.params.size(); i++)
                                if (!typesEqual(other.params[i], signature.params[i]))
                                    return false;
                            return true;
                        });
        if (!duplicate) existing.push_back(signature);
        if (def.isServing && method.isSlot) {
            bool hasFrom = false;
            if (auto* named = std::get_if<NamedType>(&signature.result->kind))
                hasFrom = named->name == "Reply" &&
                          named->typeArgs.size() == 1;
            auto remote = remoteSlotSignature(signature, hasFrom);
            const bool remoteDuplicate = std::any_of(
                existing.begin(), existing.end(), [&](const Signature& other) {
                    if (other.params.size() != remote.params.size()) return false;
                    for (size_t i = 0; i < other.params.size(); ++i)
                        if (!typesEqual(other.params[i], remote.params[i]))
                            return false;
                    return true;
                });
            if (!remoteDuplicate) existing.push_back(std::move(remote));
        }
    };
    for (const auto& item : def.body) {
        if (const auto* function =
                std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
            if (*function) preRegister(**function);
        } else if (const auto* visibility =
                       std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
                   visibility && *visibility) {
            for (const auto& visible : (*visibility)->items)
                if (const auto* function =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&visible))
                    if (*function) preRegister(**function);
        }
    }
}

auto TypeChecker::registerMakeSignaturesInModule(const ast::ModuleDef& mod,
                                                const std::string& parentPath)
    -> void {
    const auto path = parentPath.empty() ||
            mod.name.starts_with(parentPath + ".")
        ? mod.name : parentPath + "." + mod.name;
    for (const auto& item : mod.body) {
        std::visit([this, &path](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) registerMakeSignature(*node, path);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerMakeSignaturesInModule(*node, path);
            }
        }, item);
    }
}

auto TypeChecker::registerMakeSignatures(const ast::Program& program) -> void {
    for (const auto& item : program.items) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                if (node) registerMakeSignature(*node, "");
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerMakeSignaturesInModule(*node, "");
            }
        }, item);
    }
}

auto TypeChecker::preRegisterFunctionSigs(const ast::Program& program) -> void {
    std::function<void(const ast::ModuleDef&)> registerModule;
    registerModule = [&](const ast::ModuleDef& module) {
        const auto previousModule = m_currentModulePath;
        m_currentModulePath = previousModule.empty() ||
                                      module.name.rfind(previousModule + ".", 0) == 0
            ? module.name
            : previousModule + "." + module.name;
        for (const auto& item : module.body) {
            std::visit([&](const auto& member) {
                using M = std::decay_t<decltype(member)>;
                if constexpr (std::is_same_v<
                                  M, std::unique_ptr<ast::FunctionDef>>) {
                    if (member) preRegisterFunctionDef(*member);
                } else if constexpr (std::is_same_v<
                                         M, std::unique_ptr<ast::ModuleDef>>) {
                    if (member) registerModule(*member);
                } else if constexpr (std::is_same_v<
                                         M, std::unique_ptr<ast::VisibilityBlock>>) {
                    if (!member) return;
                    for (const auto& visible : member->items)
                        if (auto* def = std::get_if<
                                std::unique_ptr<ast::FunctionDef>>(&visible);
                            def && *def)
                            preRegisterFunctionDef(**def);
                }
            }, item);
        }
        m_currentModulePath = previousModule;
    };
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) preRegisterFunctionDef(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) registerModule(*node);
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
    const auto previousImports = m_declarationImports;
    if (auto imports = m_functionImports.find(&def);
        imports != m_functionImports.end())
        m_declarationImports = imports->second;
    // A name may be pre-registered more than once: clauses of one definition
    // share a def.name, but so do separate definitions that overload by arity
    // (common for module functions, e.g. `let f(a)` and `let f(a, b)`).
    // Skipping the name outright would leave every arity but the first
    // invisible to calls checked before the definitions themselves are, so
    // only arities already present are skipped.
    auto existing = m_userSignatures.find(def.name);
    const bool alreadyRegistered = existing != m_userSignatures.end();
    auto hasArity = [&](size_t arity) {
        if (!alreadyRegistered) return false;
        return std::any_of(existing->second.begin(), existing->second.end(),
                           [&](const Signature& signature) {
                               return signature.params.size() == arity;
                           });
    };

    std::vector<Signature> provisional;
    for (const auto& clause : def.clauses) {
        std::unordered_map<std::string, TypePtr> genericVars;
        if (hasArity(clause.params.size())) {
            // Retain an annotated sibling alongside an earlier untyped
            // provisional clause. Do not mutate the earlier signature: these
            // may be genuine same-arity overloads. Literal clauses consult
            // this candidate narrowly while they are checked below.
            const bool annotated = std::any_of(
                clause.params.begin(), clause.params.end(),
                [](const auto& param) { return bool(param.type); });
            if (annotated) {
                std::vector<TypePtr> paramTypes;
                for (const auto& param : clause.params)
                    paramTypes.push_back(param.type
                        ? resolveTypeExpr(**param.type, genericVars)
                        : freshTypeVar());
                provisional.push_back(Signature{
                    def.name, std::move(paramTypes), freshTypeVar(), false,
                    clause.params.size()});
            }
            continue;
        }
        std::vector<TypePtr> paramTypes;
        for (const auto& param : clause.params) {
            paramTypes.push_back(
                param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar());
        }
        std::size_t required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue) required--;
        provisional.push_back(Signature{def.name, std::move(paramTypes),
                                        freshTypeVar(), false, required});
    }
    if (provisional.empty()) {
        m_declarationImports = previousImports;
        return;
    }
    if (alreadyRegistered)
        for (auto& signature : provisional)
            existing->second.push_back(std::move(signature));
    else
        m_userSignatures[def.name] = std::move(provisional);
    m_declarationImports = previousImports;
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

auto TypeChecker::expandTypeAliases(const TypePtr& type, int depth) const
    -> TypePtr {
    if (!type || depth > 8) return type;
    auto recur = [&](const TypePtr& inner) {
        return expandTypeAliases(inner, depth + 1);
    };
    if (auto* named = std::get_if<NamedType>(&type->kind)) {
        std::vector<TypePtr> args;
        for (const auto& arg : named->typeArgs) args.push_back(recur(arg));
        const auto dot = named->name.rfind('.');
        const auto last = dot == std::string::npos ? named->name
                                                   : named->name.substr(dot + 1);
        // Distinct types are nominal: `distinct type Meters = Integer` must
        // NOT collapse to Integer, so mirror resolveTypeExpr's precedence.
        if (!m_distinctTypes.count(resolveDistinctName(last)) &&
            !m_recordFields.count(named->name)) {
            if (auto alias = m_typeAliases.find(last);
                alias != m_typeAliases.end() && args.empty())
                return expandTypeAliases(alias->second, depth + 1);
        }
        if (args.empty()) return type;
        return Type::named(named->name, std::move(args));
    }
    if (auto* list = std::get_if<ListType>(&type->kind))
        return Type::list(recur(list->element));
    if (auto* optional = std::get_if<OptionalType>(&type->kind))
        return Type::optional(recur(optional->inner));
    if (auto* map = std::get_if<MapType>(&type->kind))
        return Type::map(recur(map->key), recur(map->value));
    if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
        std::vector<TypePtr> elements;
        for (const auto& element : tuple->elements)
            elements.push_back(recur(element));
        return Type::tuple(std::move(elements));
    }
    if (auto* fn = std::get_if<FuncType>(&type->kind)) {
        std::vector<TypePtr> params;
        for (const auto& param : fn->params) params.push_back(recur(param));
        return Type::func(std::move(params), recur(fn->result));
    }
    if (auto* intersection = std::get_if<IntersectionType>(&type->kind)) {
        std::vector<TypePtr> members;
        for (const auto& member : intersection->members)
            members.push_back(recur(member));
        return Type::intersection(std::move(members));
    }
    if (auto* record = std::get_if<RecordType>(&type->kind)) {
        std::vector<std::pair<std::string, TypePtr>> fields;
        for (const auto& [name, fieldType] : record->fields)
            fields.emplace_back(name, recur(fieldType));
        return Type::record(std::move(fields));
    }
    return type;
}

auto TypeChecker::normalizeIntersection(TypePtr type) const -> TypePtr {
    auto* meet = std::get_if<IntersectionType>(&type->kind);
    if (!meet) return type;

    TypePtr concreteRecord;
    for (const auto& member : meet->members) {
        auto* named = std::get_if<NamedType>(&member->kind);
        if (!named ||
            !m_recordFields.count(resolveRecordName(named->name)))
            continue;
        if (concreteRecord && !typesEqual(concreteRecord, member)) {
            const auto& left = std::get<NamedType>(concreteRecord->kind).name;
            const auto& right = named->name;
            return Type::voidType(
                "incompatible nominal record tags `" + left + "` and `" +
                right + "`");
        }
        concreteRecord = member;
    }
    if (!concreteRecord) return type;

    for (const auto& member : meet->members) {
        if (typesEqual(member, concreteRecord)) continue;
        if (!argMatchesParam(concreteRecord, member))
            return Type::voidType();
    }
    return concreteRecord;
}

auto TypeChecker::resolveTypeExpr(const ast::TypeExpr& typeExpr,
                                   std::unordered_map<std::string, TypePtr>& genericVars) -> TypePtr {
    return std::visit([this, &genericVars](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::TypeQuery>) {
            return resolveTypeQuery(node);
        }
        else if constexpr (std::is_same_v<T, ast::TypeName>) {
            if (node.parts.empty()) return Type::unknown();
            const std::string& last = node.parts.back();
            std::string qualified;
            for (std::size_t i = 0; i < node.parts.size(); ++i) {
                if (i) qualified += ".";
                qualified += node.parts[i];
            }
            // `This` inside a make block refers to the implementing type.
            if (last == "This" && node.parts.size() == 1 && m_inMakeBlock && m_currentMakeType)
                return m_currentMakeType;
            // `Never` — the bottom/uninhabited type for never-returning functions.
            // `Void` is an alias for the unit type `()` (Swift-style naming).
            if (last == "Never" && node.parts.size() == 1)
                return Type::voidType();
            if (last == "Void" && node.parts.size() == 1)
                return Type::unit();
            // `Any` — escape hatch: unifies with everything (unknown message type
            // for Process<Any>, etc.). Treated as UnknownType at the type level.
            if (last == "Any" && node.parts.size() == 1)
                return Type::unknown();
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
            if (node.parts.size() == 1) {
                const auto recordName = resolveRecordName(last);
                if (recordName != last) return Type::named(recordName);
            }
            if (auto known = m_globals.get(last)) return known;
            // Trait-only names (Float, Number, Comparable, ...) have no
            // concrete Type — m_globals deliberately has no entry for them
            // (see check()'s comment) — so a param annotated `Float` means
            // "any T satisfying Float", same as an explicit constraint.
            if (m_traits.get(last)) return Type::constrained(last, last);
            if (auto distinct = resolveDistinctName(last);
                m_distinctTypes.count(distinct))
                return Type::named(distinct);
            // User type alias (e.g. `type Level = :debug | :info | ...`)
            auto aliasIt = m_typeAliases.find(last);
            if (aliasIt != m_typeAliases.end()) return aliasIt->second;
            if (node.parts.size() > 1) {
                m_referencedModules.insert(qualified);
                const bool isRecord = m_recordFields.count(qualified) ||
                    (m_importedInterfaces &&
                     m_importedInterfaces->recordArities.count(qualified));
                return Type::named(isRecord ? qualified : last);
            }
            return Type::named(resolveRecordName(last));
        }
        else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::vector<TypePtr> args;
            for (const auto& arg : node.args) {
                args.push_back(arg ? resolveTypeExpr(*arg, genericVars) : Type::unknown());
            }
            std::string name = node.name.parts.empty() ? "" : node.name.parts.back();
            if (name == "Optional" && args.size() == 1)
                return Type::optional(args[0]);
            if (name == "Map" && args.size() == 2)
                return Type::map(args[0], args[1]);
            if (auto distinct = resolveDistinctName(name);
                m_distinctTypes.count(distinct))
                return Type::named(distinct, std::move(args));
            return Type::named(name, std::move(args));
        }
        else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            std::vector<TypePtr> params;
            params.push_back(node.param
                ? resolveTypeExpr(*node.param, genericVars)
                : Type::unknown());
            const ast::TypeExpr* resultExpr = node.result.get();
            while (resultExpr) {
                const auto* next =
                    std::get_if<ast::FunctionType>(&resultExpr->kind);
                if (!next) break;
                params.push_back(next->param
                    ? resolveTypeExpr(*next->param, genericVars)
                    : Type::unknown());
                resultExpr = next->result.get();
            }
            auto result = resultExpr
                ? resolveTypeExpr(*resultExpr, genericVars)
                : Type::unknown();
            return Type::func(std::move(params), std::move(result));
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
        else if constexpr (std::is_same_v<T, ast::IntersectionType>) {
            std::vector<TypePtr> members;
            members.push_back(node.left
                ? resolveTypeExpr(*node.left, genericVars) : Type::unknown());
            members.push_back(node.right
                ? resolveTypeExpr(*node.right, genericVars) : Type::unknown());
            return normalizeIntersection(
                Type::intersection(std::move(members)));
        }
        else if constexpr (std::is_same_v<T, ast::RecordType>) {
            std::vector<std::pair<std::string, TypePtr>> fields;
            for (const auto& [name, fieldType] : node.fields)
                fields.emplace_back(
                    name, fieldType
                        ? resolveTypeExpr(*fieldType, genericVars)
                        : Type::unknown());
            return Type::record(std::move(fields));
        }
        else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return Type::atom(node.name);
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

// Rejects a constructor pattern that destructures the wrong number of values,
// recursively. Without this, `let Just(a, b) = Just(1)` reached the backends:
// the interpreter failed at runtime with "pattern mismatch — expected Just",
// and BEAM emitted Core Erlang that erlc rejected outright. A constructor's
// arity is known statically, so this belongs at compile time.
auto TypeChecker::alwaysReturns(const ast::Expr& expr) -> bool {
    if (std::holds_alternative<ast::ReturnExpr>(expr.kind)) return true;
    // A block exits iff its last expression does.
    if (const auto* block = std::get_if<ast::BlockExpr>(&expr.kind))
        return alwaysReturns(block->body);
    return false;
}

auto TypeChecker::alwaysReturns(const std::vector<ast::ExprPtr>& body) -> bool {
    return !body.empty() && body.back() && alwaysReturns(*body.back());
}

auto TypeChecker::adtNameOfType(const TypePtr& type) const -> std::string {
    if (!type) return "";
    // `X?` has its own type kind rather than a NamedType, but the arms it can
    // be matched against are the prelude's `Optional` declaration.
    if (std::holds_alternative<OptionalType>(type->kind))
        return m_adtVariants.count("Optional") ? "Optional" : "";
    if (const auto* named = std::get_if<NamedType>(&type->kind))
        if (m_adtVariants.count(named->name)) return named->name;
    return "";
}

auto TypeChecker::checkPatternConstructorOwner(const ast::Pattern& pattern,
                                               const TypePtr& expected)
    -> void {
    const auto adt = adtNameOfType(expected);
    if (adt.empty()) return;

    std::string ctorName;
    if (const auto* ctor = std::get_if<ast::ConstructorPattern>(&pattern.kind)) {
        ctorName = ctor->name;
    } else if (const auto* lit = std::get_if<ast::LiteralPattern>(&pattern.kind);
               lit && lit->literal.type == TokenType::None) {
        // `None` lexes as its own token, so it arrives as a LiteralPattern
        // rather than a nullary ConstructorPattern (same quirk
        // checkMatchExhaustiveness works around).
        ctorName = "None";
    } else {
        return;
    }

    auto owner = m_adtOfConstructor.find(ctorName);
    // An unregistered constructor (imported or opaque) proves nothing.
    if (owner == m_adtOfConstructor.end() || owner->second == adt) return;

    error(pattern.location,
          "`" + ctorName + "` is a constructor of `" + owner->second +
              "`, but the value being matched has type `" + adt +
              "` — this pattern can never match");
}

auto TypeChecker::checkPatternArity(const ast::Pattern& pattern) -> void {
    std::visit([this, &pattern](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            std::optional<size_t> expected;
            std::string kind = "constructor";
            if (auto it = m_constructorArity.find(node.name);
                it != m_constructorArity.end()) {
                expected = it->second;
            } else if (auto record = m_recordFields.find(node.name);
                       record != m_recordFields.end()) {
                expected = record->second.size();
                kind = "record";
            } else if (m_importedInterfaces) {
                auto imported = m_importedInterfaces->recordArities.find(node.name);
                if (imported != m_importedInterfaces->recordArities.end()) {
                    expected = imported->second;
                    kind = "record";
                }
            }
            // An unregistered name (imported/opaque) carries no arity to check.
            if (expected && *expected != node.args.size())
                error(pattern.location,
                      "`" + node.name + "` " + kind + " takes " +
                          std::to_string(*expected) + " value(s), but the "
                          "pattern destructures " +
                          std::to_string(node.args.size()));
            for (const auto& arg : node.args)
                if (arg) checkPatternArity(*arg);
        } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) checkPatternArity(*node.inner);
        } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            // A named record pattern asserts a concrete record type. Verify the
            // name is a known record and that every listed field belongs to it
            // (catches typos like `ParseError { valu, rest }`). Imported records
            // only expose an arity, so field names can't be checked there.
            if (!node.typeName.empty()) {
                const auto recordName = resolveRecordName(node.typeName);
                auto local = m_recordFields.find(recordName);
                bool known = local != m_recordFields.end();
                if (!known && m_importedInterfaces &&
                    m_importedInterfaces->recordArities.count(recordName))
                    known = true;
                if (!known)
                    error(pattern.location,
                          "`" + node.typeName + "` is not a record type");
                else if (local != m_recordFields.end())
                    for (const auto& field : node.fields)
                        if (!field.isStringKey &&
                            !local->second.count(field.name))
                            error(pattern.location,
                                  "record `" + node.typeName + "` has no field `" +
                                  field.name + "`");
            }
            for (const auto& field : node.fields)
                if (field.pattern) checkPatternArity(**field.pattern);
        } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            for (const auto& element : node.elements)
                if (element) checkPatternArity(*element);
            if (node.rest) checkPatternArity(**node.rest);
        } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            for (const auto& element : node.elements)
                if (element) checkPatternArity(*element);
        }
    }, pattern.kind);
}

auto TypeChecker::bindPatternVars(
    const ast::Pattern& pat, TypePtr expected) -> void {
    expected = expected ? resolve(expected) : nullptr;
    checkPatternConstructorOwner(pat, expected);
    std::visit([this, &expected, &pat](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::VarPattern>) {
            if (node.name != "_") {
                auto bound = expected ? expected : freshTypeVar();
                defineVar(node.name, bound);
                // Kept so tooling can answer for the binding itself. Types are
                // resolved once at the end of check(), because a fresh
                // variable here may only learn what it is from a later use.
                m_patternBindings.push_back({node.name, pat.location, bound});
            }
        }
        else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) bindPatternVars(*node.inner, expected);
        }
        else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            // Give each payload binding the type the constructor declares, the
            // way a record pattern below takes field types from the record.
            // Without it a binding had no type until some later use forced one,
            // so `Boxed(b) -> b.size` worked only because `.size` constrained
            // `b`, and the editor could say nothing about `b` itself.
            const ConstructorResult* declaration = nullptr;
            if (auto found = m_constructorResult.find(node.name);
                found != m_constructorResult.end())
                declaration = &found->second;
            for (size_t i = 0; i < node.args.size(); ++i) {
                if (!node.args[i]) continue;
                TypePtr payload;
                const int slot = declaration && i < declaration->slots.size()
                    ? declaration->slots[i] : -1;
                if (slot >= 0 && expected) {
                    // A payload that IS a type parameter takes its type from
                    // the scrutinee. The scrutinee's shape varies: `Result<A,
                    // B>` carries type arguments, but `A?` is an OptionalType
                    // and `[A]` a ListType, neither of which has any. The
                    // declared parameter name must NOT be used as a fallback
                    // here — doing that bound `Just(c)`'s `c` to the literal
                    // `A` and broke the prelude (`parsing.kex` charWhen).
                    const auto scrutinee = resolve(expected);
                    const auto index = static_cast<size_t>(slot);
                    if (const auto* named = std::get_if<NamedType>(&scrutinee->kind);
                        named && index < named->typeArgs.size())
                        payload = named->typeArgs[index];
                    else if (const auto* optional =
                                 std::get_if<OptionalType>(&scrutinee->kind);
                             optional && index == 0)
                        payload = optional->inner;
                    else if (const auto* list =
                                 std::get_if<ListType>(&scrutinee->kind);
                             list && index == 0)
                        payload = list->element;
                } else if (declaration && i < declaration->payloadTypes.size()) {
                    payload = declaration->payloadTypes[i];
                }
                bindPatternVars(*node.args[i], payload);
            }
        }
        else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            const std::unordered_map<std::string, TypePtr>* fields = nullptr;
            // A named record pattern (`Foo { x }`) pins the record type itself,
            // so resolve field types from the declared type regardless of what
            // the scrutinee was inferred to be. Fall back to `expected` for the
            // anonymous `{ x }` form.
            if (!node.typeName.empty()) {
                if (auto found = m_recordFields.find(
                        resolveRecordName(node.typeName));
                    found != m_recordFields.end())
                    fields = &found->second;
            } else if (expected)
                if (auto* named = std::get_if<NamedType>(&expected->kind))
                    if (auto found = m_recordFields.find(
                            resolveRecordName(named->name));
                        found != m_recordFields.end())
                        fields = &found->second;
            for (const auto& field : node.fields) {
                TypePtr fieldType;
                if (fields)
                    if (auto found = fields->find(field.name);
                        found != fields->end())
                        fieldType = found->second;
                if (field.pattern)
                    bindPatternVars(**field.pattern, fieldType);
                else if (!field.isStringKey)
                    defineVar(field.name,
                              fieldType ? fieldType : freshTypeVar());
            }
        }
        else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            TypePtr elementType;
            if (expected)
                if (auto* list = std::get_if<ListType>(&expected->kind))
                    elementType = list->element;
            for (const auto& elem : node.elements) {
                if (elem) bindPatternVars(*elem, elementType);
            }
            if (node.rest) bindPatternVars(**node.rest, expected);
        }
        else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            const TupleType* tuple = expected
                ? std::get_if<TupleType>(&expected->kind) : nullptr;
            for (size_t i = 0; i < node.elements.size(); ++i)
                if (node.elements[i])
                    bindPatternVars(
                        *node.elements[i],
                        tuple && i < tuple->elements.size()
                            ? tuple->elements[i] : nullptr);
        }
        else if constexpr (std::is_same_v<T, ast::RangePattern>) {
            // (x..y) in a match clause binds x and y as variables.
            if (node.start) bindPatternVars(*node.start);
            if (node.end) bindPatternVars(*node.end);
        }
        // LiteralPattern, WildcardPattern introduce nothing.
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
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
            checkUsingBlock(*node);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
            checkRecordDef(*node);
        }
    }, item);
}

auto TypeChecker::checkModule(const ast::ModuleDef& mod) -> void {
    auto previousModulePath = m_currentModulePath;
    m_currentModulePath = previousModulePath.empty() ||
                                  mod.name.rfind(previousModulePath + ".", 0) == 0
        ? mod.name
        : previousModulePath + "." + mod.name;
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
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                checkUsingBlock(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                checkRecordDef(*node);
            } else if constexpr (std::is_same_v<
                                     T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (!node) return;
                for (const auto& visible : node->items) {
                    std::visit([this](const auto& member) {
                        using M = std::decay_t<decltype(member)>;
                        if constexpr (std::is_same_v<
                                          M, std::unique_ptr<ast::FunctionDef>>) {
                            if (member) checkFunctionDef(*member);
                        } else if constexpr (std::is_same_v<
                                                 M, std::unique_ptr<ast::MakeDef>>) {
                            if (member) checkMakeDef(*member);
                        } else if constexpr (std::is_same_v<
                                                 M, std::unique_ptr<ast::UsingBlock>>) {
                            if (member) checkUsingBlock(*member);
                        } else if constexpr (std::is_same_v<
                                                 M, std::unique_ptr<ast::RecordDef>>) {
                            if (member) checkRecordDef(*member);
                        }
                    }, visible);
                }
            }
        }, item);
    }
    popScope();
    m_currentModulePath = std::move(previousModulePath);
}

auto TypeChecker::checkRecordDef(const ast::RecordDef& def) -> void {
    std::unordered_map<std::string, TypePtr> generics;
    for (const auto& name : def.typeParams)
        generics.emplace(name, freshTypeVar());
    for (const auto& field : def.fields) {
        if (!field.type || !field.defaultValue || !*field.defaultValue) continue;
        auto declared = resolveTypeExpr(*field.type, generics);
        auto actual = resolve(inferExpr(**field.defaultValue));
        if (!std::holds_alternative<UnknownType>(actual->kind) &&
            !std::holds_alternative<TypeVar>(actual->kind) &&
            !argMatchesParam(actual, declared))
            typeMismatch((*field.defaultValue)->location, declared, actual);
    }
}

auto TypeChecker::checkUsingBlock(const ast::UsingBlock& block) -> void {
    ImportSelection selection;
    for (size_t i = 0; i < block.module.parts.size(); ++i) {
        if (i) selection.module += ".";
        selection.module += block.module.parts[i];
    }
    selection.onlyNames = block.onlyNames;
    selection.exceptNames = block.exceptNames;
    if (block.body.empty()) {
        if (!m_importScopeStack.empty())
            m_importScopeStack.back().push_back(std::move(selection));
        return;
    }
    pushScope();
    m_importScopeStack.back().push_back(std::move(selection));
    inferBody(block.body);
    popScope();
}

auto TypeChecker::checkFunctionDef(const ast::FunctionDef& def) -> void {
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_functionImports.find(&def);
        imports != m_functionImports.end())
        m_declarationImports = imports->second;
    // Purity is scoped to the RECEIVER as well as the module: two types'
    // methods are not overloads of each other, so `FileHandle`'s pure `read`
    // and a `make Mock.Files` block's `foul read` are unrelated definitions
    // that merely share a name (kexhq/kex#143).
    //
    // `declarationKey` stays module-scoped: it looks up standalone
    // annotations, which are declared at module level and matched by name.
    const auto declarationKey = m_currentModulePath + "\n" + def.name;
    const auto purityKey =
        m_currentModulePath + "\n" +
        (m_inMakeBlock && m_currentMakeType
             ? typeToString(m_currentMakeType) + "."
             : std::string()) +
        def.name;
    if (auto [purity, inserted] =
            m_overloadPurity.emplace(purityKey, def.isFoul);
        !inserted && purity->second != def.isFoul) {
        auto location = def.location;
        if (def.isFoul)
            location.column += 5; // point after `foul `, at the function name
        error(location,
              "Overloads of `" + def.name +
              "` must all have the same purity");
    }
    const bool receiverIsFirstParam = [&] {
        if (!m_inMakeBlock || def.clauses.empty() ||
            def.clauses[0].params.empty()) return false;
        const auto& first = def.clauses[0].params[0];
        if (!first.pattern) return false;
        const auto& kind = (*first.pattern)->kind;
        if (std::holds_alternative<ast::ListPattern>(kind) ||
            std::holds_alternative<ast::TuplePattern>(kind))
            return false;
        return !std::holds_alternative<ast::VarPattern>(kind) &&
               !std::holds_alternative<ast::ConstructorPattern>(kind);
    }();
    // A "declared" signature is one from a standalone TypeAnnotation
    // (`fact : Integer -> Integer`) — tracked in m_annotationDeclared.
    // Pre-registered provisional sigs (for forward-reference/recursion) are
    // in m_userSignatures but NOT in m_annotationDeclared, so they don't
    // affect param-type selection or return-type verification here.
    const auto scopedDeclared = m_scopedDeclaredSignatures.find(declarationKey);
    auto* declared = [&]() -> const Signature* {
        if (scopedDeclared == m_scopedDeclaredSignatures.end()) return nullptr;
        // A lone declaration is still the contract even when its arity is
        // wrong; checkFunctionDef then emits the established annotation/body
        // diagnostics instead of silently treating it as unrelated.
        if (scopedDeclared->second.size() == 1)
            return &scopedDeclared->second.front();
        const auto arity = def.clauses.empty()
            ? size_t(0) : def.clauses.front().params.size();
        const Signature* match = nullptr;
        for (const auto& signature : scopedDeclared->second) {
            if (signature.params.size() != arity) continue;
            // Several same-arity contracts describe a runtime overload set
            // implemented by one permissive body (for example FS.File.open
            // by mode). No single contract may type that body; preserve all
            // call-site signatures without choosing one arbitrarily.
            if (match) return nullptr;
            match = &signature;
        }
        return match;
    }();

    // A `:>` declaration inside `make` is the method's contract just as a
    // standalone annotation is for a top-level function. The registered
    // receiver signature includes `this`; FunctionDef clauses do not, so use
    // a receiver-less copy while checking their parameters and return bodies.
    // Without this, clause-style implementations were inferred from scratch:
    // a generic callback parameter could collapse to Void and poison calls in
    // a later method even though the declared interface returned `[X]`.
    std::optional<Signature> makeDeclared;
    if (!declared && m_inMakeBlock && m_currentMakeType &&
        m_annotatedMethods.count(def.name)) {
        if (auto methods = m_methodSignatures.find(def.name);
            methods != m_methodSignatures.end()) {
            const auto arity = def.clauses.empty()
                ? size_t(0) : def.clauses.front().params.size();
            const auto receiverOffset = receiverIsFirstParam ? 0u : 1u;
            for (const auto& signature : methods->second) {
                if (signature.params.size() != arity + receiverOffset ||
                    !argMatchesParam(m_currentMakeType, signature.params.front()))
                    continue;
                if (makeDeclared) {
                    makeDeclared.reset();
                    break;
                }
                makeDeclared = signature;
                if (!receiverIsFirstParam)
                    makeDeclared->params.erase(makeDeclared->params.begin());
            }
        }
        if (makeDeclared) declared = &*makeDeclared;
    }
    const bool hasDeclaredContracts =
        scopedDeclared != m_scopedDeclaredSignatures.end() || makeDeclared.has_value();

    // Resolve inline return type annotation (-> Type on the function def line),
    // present when no separate TypeAnnotation declaration exists.
    TypePtr inlineReturnType;
    if (!def.clauses.empty() && def.clauses[0].returnAnnotation) {
        std::unordered_map<std::string, TypePtr> gvars;
        inlineReturnType = resolveTypeExpr(**def.clauses[0].returnAnnotation, gvars);
        if (declared && !containsOpenType(declared->result) &&
            !containsOpenType(inlineReturnType) &&
            !typesEqual(resolve(declared->result), resolve(inlineReturnType)))
            error((*def.clauses[0].returnAnnotation)->location,
                  "inline return type for `" + def.name + "` disagrees with its separate annotation: " +
                  typeToString(resolve(inlineReturnType)) + " versus " +
                  typeToString(resolve(declared->result)));
    }

    auto returnType = declared    ? declared->result
                    : inlineReturnType ? inlineReturnType
                    : freshTypeVar();
    defineVar(def.name, returnType);

    // A concrete annotation on one clause is the public parameter contract
    // for sibling pattern clauses too (`factorial(0)` beside
    // `factorial(n: Int)`). A literal by itself is only a match condition,
    // however: numeric patterns deliberately match across the Integer/Float
    // tower, so `classify(0)` beside an unannotated catch-all must not narrow
    // the whole function to Integer.
    std::vector<TypePtr> siblingParamContracts;
    for (const auto& clause : def.clauses) {
        if (siblingParamContracts.size() < clause.params.size())
            siblingParamContracts.resize(clause.params.size());
        std::unordered_map<std::string, TypePtr> clauseGenerics;
        for (size_t pi = 0; pi < clause.params.size(); ++pi)
            if (!siblingParamContracts[pi] && clause.params[pi].type)
                siblingParamContracts[pi] = resolveTypeExpr(
                    **clause.params[pi].type, clauseGenerics);
    }
    if (auto provisional = m_userSignatures.find(def.name);
        provisional != m_userSignatures.end())
        for (const auto& signature : provisional->second) {
            if (signature.params.size() != siblingParamContracts.size())
                continue;
            for (size_t pi = 0; pi < signature.params.size(); ++pi)
                if (!siblingParamContracts[pi] &&
                    std::any_of(def.clauses.begin(), def.clauses.end(),
                        [&](const auto& clause) {
                            return pi < clause.params.size() &&
                                clause.params[pi].pattern &&
                                std::holds_alternative<ast::LiteralPattern>(
                                    (*clause.params[pi].pattern)->kind);
                        }) &&
                    !std::holds_alternative<TypeVar>(signature.params[pi]->kind) &&
                    !std::holds_alternative<UnknownType>(signature.params[pi]->kind))
                    siblingParamContracts[pi] = signature.params[pi];
        }

    std::vector<Signature> signatures;
    for (size_t ci = 0; ci < def.clauses.size(); ci++) {
        const auto& clause = def.clauses[ci];
        pushScope();
        if (m_inMakeBlock && m_currentMakeType) {
            auto receiver = resolve(m_currentMakeType);
            if (auto* named = std::get_if<NamedType>(&receiver->kind);
                named && m_recordFields.count(resolveRecordName(named->name)))
                defineVar("new", m_currentMakeType);
        }
        const auto slotResult = resolve(returnType);
        const auto* slotReply = def.isSlot
            ? std::get_if<NamedType>(&slotResult->kind) : nullptr;
        const bool callSlot = slotReply && slotReply->name == "Reply" &&
                              slotReply->typeArgs.size() == 1;
        if (callSlot)
            defineVar("from", Type::named("From", {slotReply->typeArgs.front()}));
        const bool previousCastSlot = m_inCastSlot;
        m_inCastSlot = def.isSlot && !callSlot;
        std::unordered_map<std::string, TypePtr> genericVars;
        std::vector<TypePtr> paramTypes;
        for (size_t pi = 0; pi < clause.params.size(); pi++) {
            const auto& param = clause.params[pi];
            // Use declared param type if available and the annotation covers
            // this position; fall back to inline annotation or fresh TypeVar.
            TypePtr paramType;
            if (declared && pi < declared->params.size() && !param.type) {
                paramType = declared->params[pi];
            } else if (!param.type && pi < siblingParamContracts.size() &&
                       siblingParamContracts[pi]) {
                paramType = siblingParamContracts[pi];
            } else {
                paramType = param.type ? resolveTypeExpr(**param.type, genericVars) : freshTypeVar();
            }
            paramTypes.push_back(paramType);
            if (param.name.has_value() && *param.name != "_") {
                defineVar(*param.name, paramType);
            }
            if (param.pattern) {
                bindPatternVars(**param.pattern, paramType);
            }
            if (param.defaultValue && *param.defaultValue) {
                auto actual = resolve(inferExpr(**param.defaultValue));
                if (!std::holds_alternative<UnknownType>(actual->kind) &&
                    !std::holds_alternative<TypeVar>(actual->kind) &&
                    !argMatchesParam(actual, paramType))
                    typeMismatch((*param.defaultValue)->location, paramType,
                                 actual);
            }
        }
        if (declared && declared->params.size() != clause.params.size()) {
            bool hasMatchingAnnotation = false;
            auto it = m_annotationArities.find(declarationKey);
            if (it != m_annotationArities.end()) {
                hasMatchingAnnotation = it->second.count(clause.params.size()) > 0;
            }
            if (!hasMatchingAnnotation && m_diagnostics) {
                m_diagnostics->push_back({Diagnostic::Level::Warning, def.location,
                    "type annotation for '" + def.name + "' declares " +
                    std::to_string(declared->params.size()) + " parameter(s) but definition has " +
                    std::to_string(clause.params.size())});
            }
        }
        auto bodyType = inferBody(clause.body);
        m_inCastSlot = previousCastSlot;
        // Verify body matches declared return type (if declared and concrete).
        // Use argMatchesParam (not typesEqual) to apply the same trait-family
        // relaxations that call-site checking uses — e.g. Int and Integer are
        // compatible, so `add : Int -> Int -> Int` with a body returning
        // Integer (inferred from literal arithmetic) isn't a mismatch.
        auto effectiveReturnType = declared ? declared->result : inlineReturnType;
        const auto servingTransitionMatches = [&]() {
            if (!def.isSlot || !effectiveReturnType || clause.body.empty())
                return false;
            auto expected = resolve(effectiveReturnType);
            auto* replyType = std::get_if<NamedType>(&expected->kind);
            if (!replyType || replyType->name != "Reply" ||
                replyType->typeArgs.size() != 1)
                return false;
            const ast::Expr* result = clause.body.back().get();
            if (auto* returned = std::get_if<ast::ReturnExpr>(&result->kind))
                result = returned->value.get();
            auto* map = result ? std::get_if<ast::MapExpr>(&result->kind) : nullptr;
            if (!map) return false;
            bool hasReply = false;
            bool hasState = false;
            for (const auto& entry : map->entries) {
                if (!entry.key || !entry.value) continue;
                auto* key = std::get_if<ast::AtomLiteral>(&entry.key->kind);
                if (!key || key->name != "reply") continue;
                hasReply = true;
                auto actual = resolve(inferExpr(*entry.value));
                if (!containsOpenType(actual) &&
                    !containsOpenType(replyType->typeArgs.front()) &&
                    !argMatchesParam(actual, replyType->typeArgs.front()))
                    typeMismatch(entry.value->location,
                                 replyType->typeArgs.front(), actual);
            }
            for (const auto& entry : map->entries) {
                if (!entry.key) continue;
                auto* key = std::get_if<ast::AtomLiteral>(&entry.key->kind);
                if (key && (key->name == "new" || key->name == "state"))
                    hasState = true;
            }
            if (hasReply) return true;
            if (!hasState) return false;

            std::function<bool(const ast::Expr&)> mentionsFrom;
            mentionsFrom = [&](const ast::Expr& expression) -> bool {
                return std::visit([&](const auto& node) -> bool {
                    using E = std::decay_t<decltype(node)>;
                    auto one = [&](const ast::ExprPtr& value) {
                        return value && mentionsFrom(*value);
                    };
                    if constexpr (std::is_same_v<E, ast::Identifier>)
                        return node.name == "from";
                    else if constexpr (std::is_same_v<E, ast::MethodCall>) {
                        if (one(node.receiver)) return true;
                        for (const auto& arg : node.args) if (one(arg)) return true;
                        for (const auto& [_, value] : node.namedArgs) if (one(value)) return true;
                        return node.block && one(*node.block);
                    } else if constexpr (std::is_same_v<E, ast::FunctionCall>) {
                        for (const auto& arg : node.args) if (one(arg)) return true;
                        for (const auto& [_, value] : node.namedArgs) if (one(value)) return true;
                        return node.block && one(*node.block);
                    } else if constexpr (std::is_same_v<E, ast::Lambda> ||
                                         std::is_same_v<E, ast::BlockExpr>) {
                        for (const auto& item : node.body) if (one(item)) return true;
                    } else if constexpr (std::is_same_v<E, ast::ReturnExpr>)
                        return one(node.value);
                    else if constexpr (std::is_same_v<E, ast::MapExpr>) {
                        for (const auto& entry : node.entries)
                            if (one(entry.key) || one(entry.value)) return true;
                    } else if constexpr (std::is_same_v<E, ast::LetExpr> ||
                                         std::is_same_v<E, ast::VarExpr> ||
                                         std::is_same_v<E, ast::AssignExpr>)
                        return one(node.value);
                    else if constexpr (std::is_same_v<E, ast::IfExpr>) {
                        if (one(node.condition)) return true;
                        for (const auto& item : node.thenBody) if (one(item)) return true;
                        for (const auto& [condition, body] : node.elifs) {
                            if (one(condition)) return true;
                            for (const auto& item : body) if (one(item)) return true;
                        }
                        if (node.elseBody)
                            for (const auto& item : *node.elseBody) if (one(item)) return true;
                    }
                    return false;
                }, expression.kind);
            };
            const bool deferred = std::any_of(
                clause.body.begin(), clause.body.end(),
                [&](const ast::ExprPtr& item) { return item && mentionsFrom(*item); });
            if (!deferred)
                error(def.location,
                      "call slot returns without `reply:` but never uses `from` for a deferred reply");
            return true;
        }();
        const auto servingCastTransitionMatches = [&]() {
            if (!def.isSlot || !effectiveReturnType || clause.body.empty() ||
                !typesEqual(resolve(effectiveReturnType), Type::unit()))
                return false;
            if (m_currentMakeType &&
                argMatchesParam(resolve(bodyType), resolve(m_currentMakeType)))
                return true;
            const ast::Expr* result = clause.body.back().get();
            if (auto* returned = std::get_if<ast::ReturnExpr>(&result->kind))
                result = returned->value.get();
            auto* map = result ? std::get_if<ast::MapExpr>(&result->kind) : nullptr;
            if (!map) return false;
            return std::any_of(map->entries.begin(), map->entries.end(),
                [](const ast::MapEntry& entry) {
                    if (!entry.key) return false;
                    auto* key = std::get_if<ast::AtomLiteral>(&entry.key->kind);
                    return key && (key->name == "new" || key->name == "state" ||
                                   key->name == "stop");
                });
        }();
        if (effectiveReturnType &&
            !std::holds_alternative<TypeVar>(effectiveReturnType->kind) &&
            !std::holds_alternative<UnknownType>(effectiveReturnType->kind) &&
            !std::holds_alternative<UnknownType>(bodyType->kind) &&
            !std::holds_alternative<TypeVar>(bodyType->kind) &&
            !argMatchesParam(bodyType, effectiveReturnType) &&
            !servingTransitionMatches && !servingCastTransitionMatches) {
            error(def.location,
                  "`" + def.name + "` declared to return " + typeToString(effectiveReturnType) +
                  " but body returns " + typeToString(bodyType));
        }
        // Resolve TypeVars — body inference may have constrained unannotated
        // params via unifyVar (e.g. `n * 2` → n : Number).
        for (auto& pt : paramTypes) pt = resolve(pt);
        popScope();
        // Prefer the declared/annotated return type for annotated functions —
        // using the inferred body type would expose internal TypeVars to call
        // sites, which can be incorrectly constrained by the first call.
        auto concrete = [](const TypePtr& t) {
            return !std::holds_alternative<TypeVar>(t->kind);
        };
        auto resultType = (effectiveReturnType && concrete(effectiveReturnType))
                          ? effectiveReturnType : resolve(bodyType);
        // Trailing parameters with a default are optional at the call site.
        std::size_t required = clause.params.size();
        while (required > 0 && clause.params[required - 1].defaultValue) required--;
        signatures.push_back(
            Signature{def.name, std::move(paramTypes), resultType, def.isFoul,
                      required});
    }

    // Preserve the checked declaration interface before the signatures are
    // moved into the unqualified call-resolution table below.
    m_functionSignatures[&def] = signatures;

    // `:>` signatures are registered before bodies are checked, so defaults
    // are not known yet. Copy the checked required arity back now. UFCS counts
    // an implicit receiver as an argument; an explicit @/range receiver
    // pattern already occupies that slot in the clause.
    if (m_inMakeBlock && m_currentMakeType &&
        m_annotatedMethods.count(def.name)) {
        auto methods = m_methodSignatures.find(def.name);
        if (methods != m_methodSignatures.end())
            for (auto& method : methods->second)
                for (const auto& checked : signatures) {
                    const auto receiverOffset = receiverIsFirstParam ? 0u : 1u;
                    if (method.params.size() !=
                        checked.params.size() + receiverOffset)
                        continue;
                    method.requiredParams =
                        checked.requiredParams.value_or(checked.params.size()) +
                        receiverOffset;
                }
    }

    // make-block methods have an implicit `this` receiver, not a regular
    // param — checkCall's UFCS desugaring (receiver as argument 0) would
    // mis-count their arity, so they're checked (body inference still
    // runs above) but not registered for call-site checking.
    // A make-block method defined with `let` (no `:>` annotation) still has to
    // be visible as a local method: m_methodSignatures was populated ONLY from
    // `:>` declarations, so `make Vec2 do let to(target) -> String? ... end`
    // registered nothing and every `v.to(String)` call resolved to the prelude's
    // generic `to` instead — on BEAM that lowered to `kex_prelude:to`, ignoring
    // the module's own `to__Vec2`.
    if (m_inMakeBlock && m_currentMakeType &&
        !hasAnnotatedSignatureForReceiver(def.name, m_currentMakeType)) {
        // A method reaches its receiver implicitly through `this`, or as a
        // first parameter that MATCHES the receiver: `let head(@[x | _])`,
        // `let rangeStart(x.._)`. Only the implicit form needs a receiver
        // prepended — doing it for the pattern form invents a wrong arity and
        // makes `list.head` look like a 1-of-2 argument call. A plain named
        // parameter (`let to(target)`) is an ordinary argument.
        // A constructor pattern in first position is the type-selector idiom
        // (`let to(String) -> String?`), an ARGUMENT naming the conversion
        // target — not a receiver match like `@[x | _]` or `x.._`.
        auto& existing = m_methodSignatures[def.name];
        for (const auto& signature : signatures) {
            Signature withReceiver = signature;
            withReceiver.makeModule = m_currentMakeModule;
            if (!receiverIsFirstParam)
                withReceiver.params.insert(withReceiver.params.begin(),
                                           m_currentMakeType);
            if (!receiverIsFirstParam)
                withReceiver.requiredParams =
                    withReceiver.requiredParams.value_or(
                        signature.params.size()) + 1;
            const bool duplicate =
                std::any_of(existing.begin(), existing.end(),
                            [&](const Signature& other) {
                                if (other.params.size() != withReceiver.params.size())
                                    return false;
                                for (size_t i = 0; i < other.params.size(); i++)
                                    if (!typesEqual(other.params[i],
                                                    withReceiver.params[i]))
                                        return false;
                                return true;
                            });
            if (!duplicate) existing.push_back(std::move(withReceiver));
        }
    }

    if (!m_inMakeBlock) {
        // What this definition contributes, for the qualified publication
        // below — NOT the accumulated bare-name set, which holds every
        // same-named function of every module checked so far.
        std::vector<Signature> publishable;
        // If a declared signature already exists, update its result type with
        // the inferred one (keeping declared params) and keep one entry.
        if (hasDeclaredContracts) {
            auto& sigs = m_userSignatures[def.name];
            // Replace the placeholder declared sig with the fully-checked one.
            if (declared && !signatures.empty()) {
                auto placeholder = std::find_if(
                    sigs.begin(), sigs.end(), [&](const Signature& candidate) {
                        if (candidate.params.size() != declared->params.size())
                            return false;
                        for (size_t i = 0; i < candidate.params.size(); ++i)
                            if (!typesEqual(candidate.params[i],
                                            declared->params[i]))
                                return false;
                        return true;
                    });
                auto checkedInterface = signatures[0];
                const bool hasInlineParamContract = !def.clauses.empty() &&
                    std::any_of(
                        def.clauses[0].params.begin(),
                        def.clauses[0].params.end(),
                        [](const auto& param) {
                            return param.type && *param.type;
                        });
                if (hasInlineParamContract) {
                    // The standalone annotation is the public relationship
                    // (notably A -> A for IO.inspect), while an inline trait
                    // annotation supplies the implementation's dictionary
                    // constraint. Do not erase the declared relationship with
                    // the narrower body-checking parameter type.
                    checkedInterface = *declared;
                    checkedInterface.requiredParams =
                        signatures[0].requiredParams;
                    checkedInterface.isFoul = signatures[0].isFoul;
                }
                publishable.push_back(checkedInterface);
                if (placeholder != sigs.end())
                    *placeholder = checkedInterface;
                else
                    sigs.push_back(std::move(checkedInterface));
            }
        } else {
            publishable = signatures;
            if (m_checkedFunctions.count(def.name)) {
                // Additional `let f(...)` with the same name: append to the
                // overload set rather than replacing (typed overloads).
                for (auto& sig : signatures)
                    m_userSignatures[def.name].push_back(std::move(sig));
            } else {
                // First real check for this name: replace the provisional
                // pre-registered signatures with the inferred ones.
                m_userSignatures[def.name] = std::move(signatures);
                m_checkedFunctions.insert(def.name);
            }
        }
        publishQualifiedSignatures(def.name, publishable);
    }
    m_declarationImports = std::move(savedDeclarationImports);
}

// A module function is also reachable QUALIFIED (`M.lines("x")`), which checks
// as the call name `M::lines` — but its signatures were only ever filed under
// the bare `lines`, so every qualified call came back Unknown. A declared
// result type then bought nothing: `let n: Integer = M.lines("x")` passed with
// `lines : String -> [String]?` in plain sight.
//
// It is not a lookup fallback to the bare name: two modules may each define
// `lines`, and only this module's belongs under this key.
auto TypeChecker::publishQualifiedSignatures(
    const std::string& name, const std::vector<Signature>& signatures) -> void {
    if (m_currentModulePath.empty() || signatures.empty()) return;
    const auto key = m_currentModulePath + "::" + name;
    // Only THIS definition's signatures: `Date.parse` and `Time.parse` share
    // the bare name `parse`, and publishing the accumulated set gave Date's
    // qualified key Time's result type.
    if (m_qualifiedPublished.insert(key).second)
        m_userSignatures[key] = signatures;
    else
        for (const auto& signature : signatures)
            m_userSignatures[key].push_back(signature);
}

// A module-scoped `make` is visible only where its module is: inside the module
// itself (or one nested in it), or under a `using` that names it — top-level or
// the lexical `using M do ... end` form, both of which land in
// m_declarationImports for the enclosing declaration. A top-level `make` (empty
// module) stays global, and so does everything reached through an imported
// INTERFACE: the prelude is always imported, and its interface signatures never
// carry a makeModule.
//
// Before this, a `make` inside a module was visible everywhere with no `using`
// at all — the one module member that was not import-gated
// (docs/ufcs-dispatch-plan.md "Follow-up", case 1).
auto TypeChecker::makeModuleVisible(const std::string& module) const -> bool {
    if (module.empty()) return true;
    // Inside the defining module, or one nested within it.
    if (m_currentModulePath == module ||
        m_currentModulePath.rfind(module + ".", 0) == 0)
        return true;
    for (const auto& import : m_declarationImports) {
        if (import.module == module) return true;
        // `using A` also brings `A.B`'s members into scope, matching how a
        // qualified member of a nested module resolves.
        if (import.module.rfind(module + ".", 0) == 0) return true;
        if (module.rfind(import.module + ".", 0) == 0) return true;
    }
    return false;
}

auto TypeChecker::checkMakeDef(const ast::MakeDef& def) -> void {
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_makeImports.find(&def); imports != m_makeImports.end())
        m_declarationImports = imports->second;
    pushScope();
    bool wasInMakeBlock = m_inMakeBlock;
    auto prevMakeType = m_currentMakeType;
    auto prevMakeModule = m_currentMakeModule;
    m_inMakeBlock = true;
    // A `make` inside a module belongs to it, and is import-gated with it.
    m_currentMakeModule = m_currentModulePath;
    // Preserve the complete receiver type. This keeps primitive targets
    // canonical (Bool is PrimitiveType::Bool), retains Map/List structure,
    // and carries generic arguments for ADT/record receiver functions.
    m_currentMakeType.reset();
    if (def.target) {
        std::unordered_map<std::string, TypePtr> genericVars;
        m_currentMakeType = resolveTypeExpr(*def.target, genericVars);
    }
    for (const auto& item : def.body) {
        std::visit([this](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                checkFunctionDef(*node);
            }
            // NOTE: `private do … end` methods are deliberately NOT checked
            // here. They should be — visibility is not a type-system concept —
            // but registering them exposes an ordering bug: a make block's
            // methods are registered as they are CHECKED, so a call above a
            // private definition (`this.timeMeasure(…)` in units.kex's
            // `make Float`) resolves against the OTHER type's copy and reports
            // a bogus mismatch. Fixing that needs make-block signatures
            // pre-registered before any body is checked. Their names are
            // collected below regardless, so the unknown-method report does not
            // flag calls to them.
            //
            // Checking the block's own private methods FIRST does fix
            // units.kex, but is not enough on its own: the signature a private
            // method registers has the receiver prepended, so `si.kex`'s
            // `productKind` then reads as arity 2 against 3-argument calls,
            // and json_parser.spec fails on BEAM at RUNTIME with "Undefined
            // method: advance for Parser" — registration changes dispatch, not
            // just what tooling can see. Pre-registration is the real fix.
        }, item);
    }
    m_inMakeBlock = wasInMakeBlock;
    m_currentMakeType = prevMakeType;
    m_currentMakeModule = std::move(prevMakeModule);
    popScope();

    checkTraitImplementation(def);
    m_declarationImports = std::move(savedDeclarationImports);
}

auto TypeChecker::checkTraitImplementation(const ast::MakeDef& def) -> void {
    if (def.implements.empty()) return;

    // Extract the type name from the make target.
    std::string typeName;
    if (def.target) {
        if (auto* tn = std::get_if<ast::TypeName>(&def.target->kind))
            if (tn->parts.size() == 1) typeName = tn->parts[0];
        if (auto* gt = std::get_if<ast::GenericType>(&def.target->kind))
            if (gt->name.parts.size() == 1) typeName = gt->name.parts[0];
    }

    // Collect method names and their foul status from this make block.
    std::unordered_map<std::string, bool> provided; // name -> isFoul
    for (const auto& item : def.body) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) provided[node->name] = node->isFoul;
            }
        }, item);
    }

    for (const auto& traitName : def.implements) {
        const TraitDef* trait = m_traits.get(traitName);
        if (!trait) {
            error(def.location, "Unknown trait: " + traitName);
            continue;
        }
        std::string prefix = "make " + (typeName.empty() ? "?" : typeName) +
                             ", implement: " + traitName + " — ";
        for (const auto& req : trait->requiredMethods) {
            auto it = provided.find(req.name);
            if (it == provided.end()) {
                error(def.location, prefix + "missing required method '" + req.name + "'");
            } else if (req.isFoul && !it->second) {
                error(def.location, prefix + "method '" + req.name +
                      "' must be declared foul (trait requires it)");
            }
        }
        // Register so satisfies() / argMatchesParam can verify trait usage.
        if (!typeName.empty()) {
            m_traits.registerImplementation(typeName, traitName);
        }
    }
}

auto TypeChecker::checkMainBlock(const ast::MainBlock& block) -> void {
    const bool wasInSyntheticMain = m_inSyntheticMain;
    m_inSyntheticMain = block.synthetic;
    struct Restore {
        bool& flag;
        bool saved;
        ~Restore() { flag = saved; }
    } restore{m_inSyntheticMain, wasInSyntheticMain};
    auto savedDeclarationImports = m_declarationImports;
    if (auto imports = m_mainImports.find(&block); imports != m_mainImports.end())
        m_declarationImports = imports->second;
    if (!block.synthetic) pushScope();
    for (size_t i = 0; i < block.params.size(); i++) {
        const auto& param = block.params[i];
        if (param.name.has_value() && *param.name != "_") {
            TypePtr type = (i == 0) ? Type::list(Type::string())
                         : (i == 1) ? Type::map(Type::string(), Type::string())
                                    : freshTypeVar();
            defineVar(*param.name, std::move(type));
        }
        if (param.pattern) {
            bindPatternVars(**param.pattern);
        }
    }
    inferBody(block.body);
    if (!block.synthetic) popScope();
    m_declarationImports = std::move(savedDeclarationImports);
}

auto TypeChecker::inferBody(const std::vector<ast::ExprPtr>& body) -> TypePtr {
    TypePtr lastType = Type::unit();
    for (const auto& expr : body) {
        if (expr) lastType = inferExpr(*expr);
    }
    return lastType;
}

auto TypeChecker::importedCandidateSignatures(const std::string& name) const
    -> std::vector<Signature> {
    std::vector<Signature> result;
    if (!m_importedInterfaces) return result;
    if (auto separator = name.find("::"); separator != std::string::npos) {
        auto module = m_importedInterfaces->modules.find(name.substr(0, separator));
        if (module != m_importedInterfaces->modules.end())
            if (auto functions = module->second.exports.find(name.substr(separator + 2));
                functions != module->second.exports.end())
                for (const auto& function : functions->second)
                    result.push_back(function.signature);
        return result;
    }
    if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
        functions != m_importedInterfaces->receiverFunctions.end())
        for (const auto& function : functions->second)
            if (importedFunctionVisible(function))
                result.push_back(function.signature);
    for (const auto& [_, module] : m_importedInterfaces->modules) {
        if (!module.automaticImport) continue;
        if (auto functions = module.exports.find(name);
            functions != module.exports.end())
            for (const auto& function : functions->second)
                result.push_back(function.signature);
    }
    return result;
}

auto TypeChecker::importedFunctionVisible(
    const ImportedFunction& function) const -> bool {
    if (function.sourceModule.empty()) return true;
    if (m_importedInterfaces) {
        if (auto module = m_importedInterfaces->modules.find(function.sourceModule);
            module != m_importedInterfaces->modules.end() &&
            module->second.automaticImport)
            return true;
    }
    return moduleMemberImported(function.sourceModule, function.sourceName);
}

auto TypeChecker::moduleMemberImported(const std::string& module,
                                       const std::string& member) const -> bool {
    auto selected = [&](const ImportSelection& import) {
        const bool prefixMatch = module == import.module ||
            module.rfind(import.module + ".", 0) == 0;
        if (!prefixMatch) {
            // A `make X<A> do ... end` sitting directly in a file-header
            // module (`module Data`) attributes its receivers to the
            // record's own qualified name (`Data.UnorderedSet`) — a SIBLING
            // of another type declared in the same file (`Data.Set`), not
            // its ancestor or descendant. `using Data.Set` is meant to bring
            // every flavour the file declares along with it (the header
            // comment says so), so a shared immediate parent is visibility
            // too, not just a literal prefix match (kexhq/kex#229).
            const auto parentOf = [](const std::string& qualified) {
                const auto dot = qualified.rfind('.');
                return dot == std::string::npos ? std::string()
                                                 : qualified.substr(0, dot);
            };
            const auto importParent = parentOf(import.module);
            if (importParent.empty() || importParent != parentOf(module))
                return false;
        }
        auto selectedMember = member;
        if (prefixMatch && module.size() > import.module.size()) {
            const auto rest = module.substr(import.module.size() + 1);
            const auto dot = rest.find('.');
            selectedMember = rest.substr(0, dot);
        }
        if (!import.onlyNames.empty() &&
            std::find(import.onlyNames.begin(), import.onlyNames.end(),
                      selectedMember) == import.onlyNames.end())
            return false;
        return std::find(import.exceptNames.begin(), import.exceptNames.end(),
                         selectedMember) == import.exceptNames.end();
    };
    if (std::any_of(m_declarationImports.begin(), m_declarationImports.end(),
                    selected))
        return true;
    for (const auto& scope : m_importScopeStack)
        if (std::any_of(scope.begin(), scope.end(), selected))
            return true;
    return false;
}

auto TypeChecker::resolveBlockHints(const std::string& name,
                                     const std::vector<TypePtr>& nonBlockArgTypes,
                                     bool isMethodCall) -> std::vector<TypePtr> {
    auto imported = importedCandidateSignatures(name);
    const std::vector<Signature>* sigs = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() &&
        scoped != m_scopedDeclaredSignatures.end())
        sigs = &scoped->second;
    else if (auto user = m_userSignatures.find(name);
             user != m_userSignatures.end())
        sigs = &user->second;
    auto methodIt = isMethodCall ? m_methodSignatures.find(name)
                                 : m_methodSignatures.end();
    if (!sigs && imported.empty() && methodIt == m_methodSignatures.end())
        return {};

    auto hintsFrom = [&](const Signature& sig) -> std::vector<TypePtr> {
        if (sig.params.size() != nonBlockArgTypes.size() + 1) return {};
        auto* blockParam = std::get_if<FuncType>(&sig.params.back()->kind);
        if (!blockParam) return {};

        for (size_t i = 0; i < nonBlockArgTypes.size(); i++) {
            if (!argMatchesParam(nonBlockArgTypes[i], sig.params[i])) return {};
        }

        // Map negative-ID generic placeholders to concrete types from the actual args.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < nonBlockArgTypes.size(); i++) {
            const auto& sigP = sig.params[i];
            const auto& argT = resolve(nonBlockArgTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&sigP->kind); tv && tv->id < 0)
                sub.emplace(tv->id, argT);
            else if (auto* lt = std::get_if<ListType>(&sigP->kind))
                if (auto* tv2 = std::get_if<TypeVar>(&lt->element->kind); tv2 && tv2->id < 0)
                    if (auto* argLt = std::get_if<ListType>(&argT->kind))
                        sub.emplace(tv2->id, resolve(argLt->element));
        }

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) return it2->second;
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : blockParam->params) hints.push_back(applySubst(p));
        return hints;
    };

    if (methodIt != m_methodSignatures.end())
        for (const auto& sig : methodIt->second) {
            auto hints = hintsFrom(sig);
            if (!hints.empty()) return hints;
        }
    for (const auto& sig : imported) {
        auto hints = hintsFrom(sig);
        if (!hints.empty()) return hints;
    }
    if (sigs)
        for (const auto& sig : *sigs) {
            auto hints = hintsFrom(sig);
            if (!hints.empty()) return hints;
        }
    return {};
}

auto TypeChecker::resolveArgHints(const std::string& name,
                                   const std::vector<TypePtr>& argTypes,
                                   size_t slArgIdx,
                                   bool isMethodCall) -> std::vector<TypePtr> {
    auto imported = importedCandidateSignatures(name);
    const std::vector<Signature>* sigs = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() &&
        scoped != m_scopedDeclaredSignatures.end())
        sigs = &scoped->second;
    else if (auto user = m_userSignatures.find(name);
             user != m_userSignatures.end())
        sigs = &user->second;
    auto methodIt = isMethodCall ? m_methodSignatures.find(name)
                                 : m_methodSignatures.end();
    if (!sigs && imported.empty() && methodIt == m_methodSignatures.end())
        return {};

    auto hintsFrom = [&](const Signature& sig) -> std::vector<TypePtr> {
        if (sig.params.size() != argTypes.size()) return {};
        if (slArgIdx >= sig.params.size()) return {};
        auto* funcParam = std::get_if<FuncType>(&sig.params[slArgIdx]->kind);
        if (!funcParam) return {};

        // All non-SL positions must match.
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (i == slArgIdx) continue;
            if (!argMatchesParam(argTypes[i], sig.params[i])) return {};
        }

        // Build generic substitution from the concrete args.
        std::unordered_map<int, TypePtr> sub;
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (i == slArgIdx) continue;
            const auto& sigP = sig.params[i];
            const auto& argT = resolve(argTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&sigP->kind); tv && tv->id < 0)
                sub.emplace(tv->id, argT);
            else if (auto* lt = std::get_if<ListType>(&sigP->kind))
                if (auto* tv2 = std::get_if<TypeVar>(&lt->element->kind); tv2 && tv2->id < 0)
                    if (auto* argLt = std::get_if<ListType>(&argT->kind))
                        sub.emplace(tv2->id, resolve(argLt->element));
        }

        auto applySubst = [&](const TypePtr& t) -> TypePtr {
            if (auto* tv = std::get_if<TypeVar>(&t->kind)) {
                auto it2 = sub.find(tv->id);
                if (it2 != sub.end()) return it2->second;
            }
            return t;
        };

        std::vector<TypePtr> hints;
        for (const auto& p : funcParam->params) hints.push_back(applySubst(p));
        return hints;
    };

    if (methodIt != m_methodSignatures.end())
        for (const auto& sig : methodIt->second) {
            auto hints = hintsFrom(sig);
            if (!hints.empty()) return hints;
        }
    for (const auto& sig : imported) {
        auto hints = hintsFrom(sig);
        if (!hints.empty()) return hints;
    }
    if (sigs)
        for (const auto& sig : *sigs) {
            auto hints = hintsFrom(sig);
            if (!hints.empty()) return hints;
        }
    return {};
}

// A bare `~f` capture — no argument groups, no operator, unqualified. Like a
// shorthand lambda, it needs the callee's signature to know what it's being
// checked against, so it's deferred to the contextual (hinted) pass.
static auto isBareCapture(const ast::Expr* e) -> bool {
    if (!e) return false;
    const auto* ce = std::get_if<ast::CurryExpr>(&e->kind);
    return ce && ce->argGroups.empty() && !ce->isOperator && ce->module.empty();
}

auto TypeChecker::inferBlock(const ast::Expr& blockExpr,
                             const std::vector<TypePtr>& hintParams) -> TypePtr {
    if (auto* lam = std::get_if<ast::Lambda>(&blockExpr.kind)) {
        if (lam->params.empty()) {
            // Zero-param lambda `{ }` / `do end` — infer body but stay permissive
            // so it matches any FuncType param (the block ignores the passed arg).
            inferExpr(blockExpr);
            return Type::unknown();
        }
        pushScope();
        std::vector<TypePtr> paramTypes;
        std::unordered_map<std::string, TypePtr> genericVars;
        for (size_t i = 0; i < lam->params.size(); i++) {
            TypePtr pt;
            if (lam->params[i].type) {
                pt = resolveTypeExpr(**lam->params[i].type, genericVars);
            } else if (i < hintParams.size()) {
                auto hint = resolve(hintParams[i]);
                if (!std::holds_alternative<TypeVar>(hint->kind) &&
                    !std::holds_alternative<UnknownType>(hint->kind)) {
                    pt = hint;
                } else {
                    pt = freshTypeVar();
                }
            } else {
                pt = freshTypeVar();
            }
            paramTypes.push_back(pt);
            if (lam->params[i].name != "_") {
                defineVar(lam->params[i].name, pt);
                // Recorded for tooling like any other binding. A LambdaParam
                // carries no location of its own, so the block's own position
                // is the anchor and the name is found from there — without
                // this, hovering `x` in `{ |x| … }` answered nothing while a
                // use of it answered fine.
                m_patternBindings.push_back(
                    {lam->params[i].name, blockExpr.location, pt});
            }
        }
        m_blockDepth++;
        auto bodyType = inferBody(lam->body);
        m_blockDepth--;
        auto resultType = resolve(bodyType);
        if (lam->returnAnnotation) {
            auto declared = resolveTypeExpr(**lam->returnAnnotation, genericVars);
            if (!std::holds_alternative<UnknownType>(resultType->kind) &&
                !std::holds_alternative<TypeVar>(resultType->kind) &&
                !argMatchesParam(resultType, declared))
                typeMismatch(blockExpr.location, declared, resultType);
            resultType = declared;
        }
        popScope();
        return Type::func(std::move(paramTypes), resultType);
    }
    // A bare `~f` passed where a function is expected: check it against the
    // hinted param types, so `words.filter(~even?)` reports the element-type
    // mismatch rather than inferring fresh vars that unify with anything.
    // Only plain unqualified captures — an operator or a `~Mod.fn` name isn't
    // resolvable through checkCall here, and partial applications already
    // carry their own arg types.
    if (auto* ce = std::get_if<ast::CurryExpr>(&blockExpr.kind);
        ce && ce->argGroups.empty() && !ce->isOperator && ce->module.empty()
        && !hintParams.empty()) {
        std::vector<TypePtr> paramTypes;
        for (const auto& h : hintParams) {
            auto pt = resolve(h);
            paramTypes.push_back(
                (std::holds_alternative<TypeVar>(pt->kind) ||
                 std::holds_alternative<UnknownType>(pt->kind))
                    ? freshTypeVar() : pt);
        }
        auto resultType = checkCall(ce->name, paramTypes, blockExpr.location,
                                    /*isMethodCall=*/false);
        return Type::func(std::move(paramTypes), resolve(resultType));
    }
    if (auto* sl = std::get_if<ast::ShorthandLambda>(&blockExpr.kind)) {
        TypePtr paramType = (!hintParams.empty()) ? resolve(hintParams[0]) : freshTypeVar();
        if (std::holds_alternative<TypeVar>(paramType->kind) ||
            std::holds_alternative<UnknownType>(paramType->kind))
            paramType = freshTypeVar();
        // &.method or &.method(args): UFCS → checkCall(name, [receiver, ...args])
        std::vector<TypePtr> callArgs = {paramType};
        for (const auto& arg : sl->args) {
            if (arg) callArgs.push_back(inferExpr(*arg));
        }
        auto resultType = checkCall(sl->name, callArgs, blockExpr.location, true);
        return Type::func({paramType}, resolve(resultType));
    }
    // BlockExpr (no param list) — infer body for side effects, stay permissive.
    inferExpr(blockExpr);
    return Type::unknown();
}

auto TypeChecker::typeOf(const ast::Expr* expr) const -> TypePtr {
    auto it = m_typeMap.find(expr);
    return it != m_typeMap.end() ? it->second : nullptr;
}

auto TypeChecker::typeMap() const -> const std::unordered_map<const ast::Expr*, TypePtr>& {
    return m_typeMap;
}

auto TypeChecker::assignmentTargetTypeOf(const ast::Expr* expr) const -> TypePtr {
    auto it = m_assignmentTargetTypes.find(expr);
    return it != m_assignmentTargetTypes.end() ? it->second : nullptr;
}

auto TypeChecker::functionSignatures(const ast::FunctionDef* function) const
    -> const std::vector<Signature>* {
    auto it = m_functionSignatures.find(function);
    return it == m_functionSignatures.end() ? nullptr : &it->second;
}

auto TypeChecker::resolvedCalls() const
    -> const std::unordered_map<const ast::MethodCall*, ResolvedCallTarget>& {
    return m_resolvedCalls;
}

auto TypeChecker::inferExpr(const ast::Expr& expr) -> TypePtr {
    auto result = std::visit([this, &expr](const auto& node) -> TypePtr {
        using T = std::decay_t<decltype(node)>;

        if constexpr (std::is_same_v<T, ast::IntLiteral>) {
            return Type::integer();
        }
        else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
            return Type::float64();
        }
        else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            for (const auto& value : node.values)
                if (value) inferExpr(*value);
            return Type::string();
        }
        else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
            return Type::boolean();
        }
        else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
            return Type::charT();
        }
        else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
            if (auto constructed = constructorResultType("None", {}))
                return constructed;
            return Type::named("None");
        }
        else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
            return Type::atom();
        }
        else if constexpr (std::is_same_v<T, ast::Identifier>) {
            if (node.name == "from" && m_inCastSlot) {
                error(expr.location,
                      "`from` is not available in a cast — no caller is waiting for a reply");
                return Type::unknown();
            }
            auto type = lookupVar(node.name);
            if (!type) {
                if (node.name == "new" && m_inMakeBlock && m_currentMakeType)
                    error(expr.location, "`new` requires a record receiver");
                return Type::unknown();
            }
            return type;
        }
        else if constexpr (std::is_same_v<T, ast::UpperIdentifier>) {
            if (auto constructed = constructorResultType(node.name, {});
                constructed && m_nullaryConstructors.contains(node.name))
                return constructed;
            if (m_nullaryConstructors.contains(node.name))
                return Type::named(node.name);
            for (const auto& [module, constructors] : m_moduleConstructors) {
                auto constructor = constructors.find(node.name);
                if (constructor != constructors.end() &&
                    constructor->second.arity == 0 &&
                    moduleMemberImported(module, node.name))
                    return Type::named(node.name);
            }
            if (m_importedInterfaces)
                for (const auto& [_, module] :
                     m_importedInterfaces->modules)
                    if (auto exports = module.exports.find(node.name);
                        exports != module.exports.end())
                        for (const auto& function : exports->second)
                            if (function.isConstructor &&
                                function.signature.params.empty() &&
                                importedFunctionVisible(function))
                                return function.signature.result;
            auto type = lookupVar(node.name);
            if (type) return type;
            // A bare TYPE NAME used as a value — `x.to(Integer)`, the argument
            // of the conversion protocol. Typing it as the type it names is
            // what makes the protocol dispatchable: left Unknown, every
            // `to` overload matched every call, so `"42".to(Integer)` was typed
            // by whichever came first (`make Showable do let to(String)`) and
            // came out `String?` no matter the target.
            if (isPrimitiveTypeName(node.name) ||
                m_recordFields.count(resolveRecordName(node.name)) ||
                m_adtVariants.count(node.name) || m_typeAliases.count(node.name))
                return Type::named(node.name);
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::WithExpr>) {
            std::string capability;
            for (size_t i = 0; i < node.capability.parts.size(); ++i) {
                if (i) capability += ".";
                capability += node.capability.parts[i];
            }
            // Only a declared capability may be replaced. Without this, any
            // module is substitutable and the keyword means nothing, and a
            // typo binds nothing while the body still runs against the real
            // implementation — the silent-mock failure the feature exists to
            // remove (kexhq/kex#180).
            auto actual = node.value ? resolve(inferExpr(*node.value))
                                     : Type::unknown();
            if (!m_capabilities.count(capability)) {
                error(expr.location,
                      "'" + capability + "' is not a capability, so it cannot "
                      "be replaced by 'with' — declare it as "
                      "`capability " + capability + " do ... end`");
            } else if (!std::holds_alternative<UnknownType>(actual->kind) &&
                       !std::holds_alternative<TypeVar>(actual->kind) &&
                       !satisfiesTrait(actual, capability)) {
                // The replacement has to provide what the capability declares.
                error(expr.location,
                      "replacement for capability '" + capability +
                      "' does not implement it: " + typeToString(actual));
            }
            pushScope();
            auto bodyType = inferBody(node.body);
            popScope();
            return bodyType;
        }
        else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
            ImportSelection selection;
            for (size_t i = 0; i < node.module.parts.size(); ++i) {
                if (i) selection.module += ".";
                selection.module += node.module.parts[i];
            }
            selection.onlyNames = node.onlyNames;
            selection.exceptNames = node.exceptNames;
            if (node.body.empty()) {
                if (!m_importScopeStack.empty())
                    m_importScopeStack.back().push_back(std::move(selection));
                return Type::unit();
            }
            pushScope();
            m_importScopeStack.back().push_back(std::move(selection));
            auto bodyType = inferBody(node.body);
            popScope();
            return bodyType;
        }
        else if constexpr (std::is_same_v<T, ast::TryExpr>) {
            if (m_tryingBlockDepth >= 0 && m_blockDepth > m_tryingBlockDepth)
                m_tryBelowBlock++;
            auto operand = resolve(
                node.operand ? inferExpr(*node.operand) : Type::unknown());
            if (auto* optional = std::get_if<OptionalType>(&operand->kind))
                return optional->inner;
            if (auto* named = std::get_if<NamedType>(&operand->kind);
                named && !named->typeArgs.empty()) {
                if (named->name == "Optional" || named->name == "Result")
                    return named->typeArgs[0];
            }
            if (std::holds_alternative<UnknownType>(operand->kind) ||
                std::holds_alternative<TypeVar>(operand->kind))
                return Type::unknown();
            error(expr.location, "`.try` expects Result or Optional, got " +
                  typeToString(operand));
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
            auto outerTryingDepth = m_tryingBlockDepth;
            auto outerTryBelowBlock = m_tryBelowBlock;
            m_tryingBlockDepth = m_blockDepth;
            m_tryBelowBlock = 0;
            auto bodyType = inferBody(node.body);
            // A body that exits the function hands nothing to the `trying`
            // expression, so it must not be compared against the rescue arms.
            auto result = resolve(
                alwaysReturns(node.body) ? Type::voidType() : bodyType);
            // A block is a function boundary: a `.try` inside one becomes the
            // BLOCK's result and never reaches this rescue, so the body's type
            // and the rescue's have no reason to agree — the rescue is dead
            // for that failure. `.try` is typed optimistically (payload only),
            // which is what makes the disagreement look real.
            const bool rescueUnreachable = m_tryBelowBlock > 0;
            m_tryingBlockDepth = outerTryingDepth;
            m_tryBelowBlock = outerTryBelowBlock;
            auto merge = [&](TypePtr candidate) {
                candidate = resolve(candidate);
                bool resultPermissive =
                    std::holds_alternative<UnknownType>(result->kind) ||
                    std::holds_alternative<TypeVar>(result->kind) ||
                    std::holds_alternative<VoidType>(result->kind);
                bool candidatePermissive =
                    std::holds_alternative<UnknownType>(candidate->kind) ||
                    std::holds_alternative<TypeVar>(candidate->kind) ||
                    std::holds_alternative<VoidType>(candidate->kind);
                if (resultPermissive && !candidatePermissive) {
                    result = candidate;
                } else if (!resultPermissive && !candidatePermissive &&
                           !rescueUnreachable &&
                           !argMatchesParam(candidate, result) &&
                           !argMatchesParam(result, candidate)) {
                    error(expr.location,
                          "`trying` body returns " + typeToString(result) +
                          " but rescue returns " + typeToString(candidate));
                }
            };
            // A rescue clause binds names exactly like a match clause: its
            // patterns introduce locals its guard and body can see, and
            // `rescue |e|` binds the caught error.
            for (const auto& clause : node.rescue.clauses) {
                pushScope();
                for (const auto& pat : clause.patterns)
                    if (pat) {
                        checkPatternArity(*pat);
                        bindPatternVars(*pat);
                    }
                if (clause.guard && *clause.guard) inferExpr(**clause.guard);
                if (clause.body) {
                    auto armType = inferExpr(*clause.body);
                    if (!alwaysReturns(*clause.body)) merge(armType);
                }
                popScope();
            }
            if (!node.rescue.catchAllBody.empty()) {
                pushScope();
                if (!node.rescue.catchAllParam.empty())
                    defineVar(node.rescue.catchAllParam, Type::unknown());
                auto catchAllType = inferBody(node.rescue.catchAllBody);
                if (!alwaysReturns(node.rescue.catchAllBody))
                    merge(catchAllType);
                popScope();
            }
            // `rescue return` exits the enclosing function, so its expression
            // does not participate in the trying-expression result type.
            if (node.rescue.inlineReturnExpr)
                inferExpr(*node.rescue.inlineReturnExpr);
            return result;
        }
        else if constexpr (std::is_same_v<T, ast::LetExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            // `let xs: [Colour] = []` — the annotation is what the binding IS;
            // the initializer only has to be compatible with it. Binding the
            // inferred type instead would defeat the reason for writing one.
            auto declared = declaredBindingType(node.type, valueType, expr.location);
            if (declared && node.value) {
                auto byteType = [](const TypePtr& type) {
                    auto* sized = type ? std::get_if<SizedIntType>(&type->kind) : nullptr;
                    return sized && sized->bits == 8 && !sized->isSigned;
                };
                auto invalidByteLiteral = [](const std::string& text) {
                    if (text.empty() || text.front() == '-') return true;
                    try {
                        size_t parsed = 0;
                        const auto value = std::stoull(text, &parsed, 0);
                        return parsed != text.size() || value > 255;
                    } catch (...) { return true; }
                };
                auto expected = resolve(declared);
                if (byteType(expected)) {
                    if (auto* literal = std::get_if<ast::IntLiteral>(&node.value->kind)) {
                        if (invalidByteLiteral(literal->value))
                            error(node.value->location, "Byte literal must be in 0..255");
                    }
                    if (auto* unary = std::get_if<ast::UnaryOp>(&node.value->kind);
                        unary && unary->op == TokenType::Minus && unary->operand &&
                        std::holds_alternative<ast::IntLiteral>(unary->operand->kind))
                        error(node.value->location, "Byte literal must be in 0..255");
                } else if (auto* listType = std::get_if<ListType>(&expected->kind);
                           listType && byteType(listType->element)) {
                    if (auto* list = std::get_if<ast::ListExpr>(&node.value->kind))
                        for (const auto& element : list->elements)
                            if (element)
                                if (auto* literal = std::get_if<ast::IntLiteral>(&element->kind)) {
                                    if (invalidByteLiteral(literal->value))
                                        error(element->location, "Byte literal must be in 0..255");
                                }
                }
            }
            if (auto* varPat = std::get_if<ast::VarPattern>(&node.pattern->kind)) {
                // `x : Type.of(y)` on the line above `let x = 34`: a top-level
                // binding is a synthetic-main LetExpr, not a 0-arg function, so
                // its declaration has to be matched here. Scoped to the
                // synthetic main, or a local `let` would pick up a global
                // annotation that merely shares its name.
                if (!declared && m_inSyntheticMain)
                    declared = declaredConstantType(varPat->name, valueType,
                                                    expr.location);
                defineVar(varPat->name, declared ? declared : valueType);
            } else if (node.pattern) {
                checkPatternArity(*node.pattern);
                // Reject constructor mismatches early, e.g.
                //   let Ok(v) = parsePrefix(...)   when parsePrefix returns Optional
                //   let Just(v) = parse(...)       when parse returns Result
                if (auto* cp = std::get_if<ast::ConstructorPattern>(&node.pattern->kind)) {
                    auto resolved = resolve(valueType);
                    if (std::holds_alternative<UnknownType>(resolved->kind) ||
                        std::holds_alternative<TypeVar>(resolved->kind))
                        ; // permissive — can't determine the type at compile time
                    else if (cp->name == "Just" && !std::holds_alternative<OptionalType>(resolved->kind))
                        error(node.pattern->location, "cannot match `Just` — expected Optional, got " + typeToString(resolved));
                    else if (cp->name == "Ok" || cp->name == "Error") {
                        if (auto* nt = std::get_if<NamedType>(&resolved->kind)) {
                            if (nt->name != "Result")
                                error(node.pattern->location, "cannot match `" + cp->name + "` — expected Result, got " + typeToString(resolved));
                        } else {
                            error(node.pattern->location, "cannot match `" + cp->name + "` — expected Result, got " + typeToString(resolved));
                        }
                    }
                }
                bindPatternVars(*node.pattern);
            }
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::VarExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            auto declared = declaredBindingType(node.type, valueType, expr.location);
            defineVar(node.name, declared ? declared : valueType);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::AssignExpr>) {
            auto valueType = node.value ? inferExpr(*node.value) : Type::unknown();
            auto varType = lookupVar(node.name);
            if (varType) m_assignmentTargetTypes[&expr] = varType;
            if (!node.path.empty()) {
                if (node.path.size() != 1) {
                    error(expr.location,
                          "nested record-field assignment is not supported yet");
                    return Type::unit();
                }
                auto resolvedVar = varType ? resolve(varType) : Type::unknown();
                auto* named = std::get_if<NamedType>(&resolvedVar->kind);
                auto record = named
                    ? m_recordFields.find(resolveRecordName(named->name))
                    : m_recordFields.end();
                if (record == m_recordFields.end()) {
                    error(expr.location, "field assignment requires a record binding");
                    return Type::unit();
                }
                auto field = record->second.find(node.path.front());
                if (field == record->second.end()) {
                    error(expr.location, "record `" + named->name +
                                             "` has no field `" +
                                             node.path.front() + "`");
                } else if (!containsOpenType(valueType) &&
                           !containsOpenType(field->second) &&
                           !argMatchesParam(valueType, field->second)) {
                    typeMismatch(expr.location, field->second, valueType);
                }
                return Type::unit();
            }
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
            const auto operatorName = [&]() -> std::string {
                switch (node.op) {
                    case TokenType::Plus: return "+";
                    case TokenType::Minus: return "-";
                    case TokenType::Star: return "*";
                    case TokenType::Slash: return "/";
                    case TokenType::Percent: return "%";
                    case TokenType::Caret: return "^";
                    case TokenType::EqEq: return "==";
                    case TokenType::NotEq: return "!=";
                    case TokenType::LessThan: return "<";
                    case TokenType::GreaterThan: return ">";
                    case TokenType::LessEq: return "<=";
                    case TokenType::GreaterEq: return ">=";
                    default: return "";
                }
            }();
            // Operators share make-method signatures with UFCS. Prefer a
            // matching receiver/RHS overload, but leave ordinary operators on
            // the builtin inference path when no local signature applies.
            if (!operatorName.empty()) {
                // Numeric primitives always use the built-in operator. A
                // trait-shaped unit overload such as Watt * Hour must not
                // capture Float * Float merely because both types also occur
                // in generic receiver tables loaded from the prelude.
                if (m_traits.satisfies(leftType, "Number") &&
                    m_traits.satisfies(rightType, "Number"))
                    return inferBinaryOp(node.op, leftType, rightType,
                                         expr.location);
                auto receiver = resolve(leftType);
                // A trait-bounded variable is still a variable. `n - 1`
                // constrains `n` to `N: Number` before `n * f(...)` is
                // checked, and treating that as a concrete receiver let it
                // match any operator overload whose parameter a constrained
                // type satisfies: `let fact(n) = n * fact(n - 1)` picked up
                // `make Period`'s `*(Integer) -> Period` and inferred
                // `fact : Integer -> Period`.
                const bool concreteReceiver =
                    !std::holds_alternative<TypeVar>(receiver->kind) &&
                    !std::holds_alternative<UnknownType>(receiver->kind) &&
                    !std::holds_alternative<ConstrainedType>(receiver->kind);
                // An operator defined in a make block is visible either as a
                // local declaration or — for the prelude's own `Date + Duration`
                // and friends — only through the imported interface. Consulting
                // just the local table sent every prelude operator down the
                // builtin numeric/string path below, which rejects any pair of
                // differing types outright.
                if (concreteReceiver) {
                    auto applies = [&](const Signature& signature) {
                        return signature.params.size() == size_t(2) &&
                               argMatchesParam(leftType, signature.params[0]) &&
                               argMatchesParam(rightType, signature.params[1]);
                    };
                    bool overloaded = false;
                    if (auto local = m_methodSignatures.find(operatorName);
                        local != m_methodSignatures.end())
                        overloaded = std::any_of(local->second.begin(),
                                                 local->second.end(), applies);
                    if (!overloaded)
                        for (const auto& signature :
                             importedCandidateSignatures(operatorName))
                            if (applies(signature)) {
                                overloaded = true;
                                break;
                            }
                    if (overloaded)
                        return checkCall(operatorName, {leftType, rightType},
                                         expr.location, /*isMethodCall=*/true);
                }
            }
            return inferBinaryOp(node.op, leftType, rightType, expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            auto operandType = node.operand ? inferExpr(*node.operand) : Type::unknown();
            auto resolved = resolve(operandType);
            if (node.op == TokenType::Bang) {
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "Logical not '!' requires Bool, got " +
                          typeToString(resolved));
                }
                return Type::boolean();
            }
            if (node.op == TokenType::Minus) {
                // Unary negation requires a numeric operand.
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::constrained("N", "Number"));
                }
                return resolved;
            }
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            std::vector<TypePtr> argTypes;
            // First pass: infer concrete args; defer lambdas with parameters
            // until the selected signature can provide contextual types.
            std::vector<std::pair<size_t, size_t>> contextualLambdas;
            for (size_t i = 0; i < node.args.size(); i++) {
                const auto* lambda = node.args[i]
                    ? std::get_if<ast::Lambda>(&node.args[i]->kind) : nullptr;
                if (node.args[i] &&
                    (std::holds_alternative<ast::ShorthandLambda>(
                         node.args[i]->kind) ||
                     isBareCapture(node.args[i].get()) ||
                     (lambda && !lambda->params.empty()))) {
                    argTypes.push_back(Type::unknown()); // placeholder
                    contextualLambdas.emplace_back(i, i);
                } else {
                    argTypes.push_back(node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                }
            }
            for (const auto& [_, arg] : node.namedArgs)
                argTypes.push_back(arg ? inferExpr(*arg) : Type::unknown());
            // Second pass: infer lambdas with parameter hints from the signature.
            for (const auto& [argIdx, rawIdx] : contextualLambdas) {
                auto hints = resolveArgHints(node.name, argTypes, argIdx);
                argTypes[argIdx] = inferBlock(*node.args[rawIdx], hints);
            }
            if (node.block) {
                auto hints = resolveBlockHints(node.name, argTypes);
                argTypes.push_back(inferBlock(**node.block, hints));
            }
            // send(pid, msg) — check msg type against Process<Msg> if pid type is known.
            if ((node.name == "send" || node.name == "sendFrom") &&
                argTypes.size() == 2) {
                auto pidType = resolve(argTypes[0]);
                auto msgType = resolve(argTypes[1]);
                if (auto* nt = std::get_if<NamedType>(&pidType->kind)) {
                    if (nt->name == "Process" && nt->typeArgs.size() == 1) {
                        auto declared = resolve(nt->typeArgs[0]);
                        // Process<Any> (unknown msg type) skips the check.
                        if (!std::holds_alternative<UnknownType>(declared->kind)) {
                            if (auto* tv = std::get_if<TypeVar>(&declared->kind)) {
                                unifyVar(tv->id, msgType);
                            } else if (!std::holds_alternative<UnknownType>(msgType->kind) &&
                                       !std::holds_alternative<TypeVar>(msgType->kind) &&
                                       !argMatchesParam(msgType, declared)) {
                                error(expr.location,
                                      "send: message type " + typeToString(msgType) +
                                      " does not match Process<" + typeToString(declared) + ">");
                            }
                        }
                    }
                }
                return msgType;
            }
            if (auto constructed =
                    constructorResultType(node.name, argTypes))
                return constructed;
            return checkCall(node.name, argTypes, expr.location,
                             false, nullptr, &expr);
        }
        else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
            for (const auto& value : node.values)
                if (value) inferExpr(*value);
            return checkCall(
                node.tag,
                {Type::list(Type::string()), Type::list(Type::unknown())},
                expr.location);
        }
        else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            // Local module constructors use the same namespace syntax as
            // module constants. Imported constructors are ordinary typed
            // module exports and continue through checkCall below.
            if (node.args.empty() && node.namedArgs.empty() && !node.block &&
                node.receiver) {
                std::function<std::optional<std::string>(const ast::Expr&)>
                    localModulePath;
                localModulePath = [&](const ast::Expr& receiver)
                    -> std::optional<std::string> {
                    if (const auto* root =
                            std::get_if<ast::UpperIdentifier>(
                                &receiver.kind))
                        return root->name;
                    const auto* segment =
                        std::get_if<ast::MethodCall>(&receiver.kind);
                    if (!segment || !segment->receiver ||
                        !segment->args.empty() ||
                        !segment->namedArgs.empty() || segment->block)
                        return std::nullopt;
                    auto parent = localModulePath(*segment->receiver);
                    return parent
                        ? std::optional<std::string>{
                              *parent + "." + segment->method}
                        : std::nullopt;
                };
                if (auto path = localModulePath(*node.receiver)) {
                    if (auto module = m_moduleConstructors.find(*path);
                        module != m_moduleConstructors.end())
                        if (auto constructor =
                                module->second.find(node.method);
                            constructor != module->second.end()) {
                            if (!constructor->second.isPublic &&
                                m_currentModulePath != *path)
                                error(expr.location,
                                      "cannot access private name `" +
                                          node.method + "` from " + *path);
                            return Type::named(node.method);
                        }
                }
            }

            // Backend interop and the private intrinsic ABI are untyped until
            // the intrinsic declaration interface supplies their signatures.
            // Never resolve these against same-named public stdlib functions.
            {
                const ast::Expr* r = node.receiver ? &*node.receiver : nullptr;
                bool intrinsicSegment = false;
                while (r) {
                    if (auto* mc = std::get_if<ast::MethodCall>(&r->kind)) {
                        intrinsicSegment = intrinsicSegment || mc->method == "Intrinsic";
                        r = mc->receiver ? &*mc->receiver : nullptr;
                    } else break;
                }
                if (r) {
                    if (auto* uid = std::get_if<ast::UpperIdentifier>(&r->kind)) {
                        if (uid->name == "Erlang" || uid->name == "Elixir" ||
                            uid->name == "Gleam" ||
                            (uid->name == "Kex" && intrinsicSegment)) {
                            for (const auto& a : node.args)
                                if (a) inferExpr(*a);
                            for (const auto& [_, a] : node.namedArgs)
                                if (a) inferExpr(*a);
                            if (node.block) inferExpr(**node.block);
                            return Type::unknown();
                        }
                    }
                }
            }
            // Namespace call: `Integer.parse(s)` or `Web.Response.text(s)`.
            // A chain made entirely of uppercase segments is a qualified
            // namespace, not a UFCS receiver value, so don't include it as
            // argTypes[0].
            std::string callName = node.method;
            // Only a direct uppercase receiver is unambiguously a namespace.
            // For a qualified chain (`Web.Response.text`) infer `Web.Response`
            // as an Unknown receiver and let checkCall's existing namespace
            // heuristic drop it when the target arity requires that. This also
            // preserves value chains through static constructors/constants,
            // such as `Temperature.Fahrenheit(212).to(String)` and
            // `Temperature.Freezing.to(String)`.
            auto isNamespaceReceiver = [](const ast::Expr& receiver) {
                return std::holds_alternative<ast::UpperIdentifier>(receiver.kind);
            };
            std::function<std::optional<std::string>(const ast::Expr&)>
                importedModulePath;
            importedModulePath = [&](const ast::Expr& receiver)
                -> std::optional<std::string> {
                if (auto* root = std::get_if<ast::UpperIdentifier>(&receiver.kind))
                    return root->name;
                auto* segment = std::get_if<ast::MethodCall>(&receiver.kind);
                if (!segment || !segment->receiver || !segment->args.empty() ||
                    !segment->namedArgs.empty() || segment->block)
                    return std::nullopt;
                auto parent = importedModulePath(*segment->receiver);
                return parent ? std::optional<std::string>{*parent + "." + segment->method}
                              : std::nullopt;
            };
            auto importedPath = node.receiver
                ? importedModulePath(*node.receiver) : std::nullopt;
            // A bare receiver segment (`Set.from(…)`) may name a module only
            // under its qualified identity (`Data.Set`) once a `using`
            // brought it into scope unqualified — expand it before the
            // dependency/namespace checks below, which key everything off
            // `*importedPath` (kexhq/kex#229).
            if (importedPath)
                importedPath = resolveModulePath(*importedPath, node.method);
            // Keep the syntactic qualified module path even when it is not in
            // the prebuilt interface registry. Source modules are discovered
            // from these references before their interfaces exist (for
            // example, `Tey.Git.execute()` must cause tey/git.kex to load).
            // The module resolver later discards paths that do not map to a
            // source module, so ordinary static/type namespaces remain safe.
            if (importedPath) {
                auto dependencyPath = *importedPath;
                // A receiver chain can continue past the module into a
                // constant or record field (`Kex.Kernel.VERSION.number`). If
                // an interface identifies a module prefix, record that prefix
                // rather than asking source discovery to interpret VERSION as
                // another module segment and recompiling the prelude source.
                if (m_importedInterfaces) {
                    auto candidate = dependencyPath;
                    std::string interfaceModule;
                    while (!candidate.empty()) {
                        if (m_importedInterfaces->modules.count(candidate)) {
                            interfaceModule = candidate;
                            break;
                        }
                        // KexI source-module identities currently retain the
                        // backend's leading `Kex.` (logical Kex.Kernel is
                        // stored as Kex.Kex.Kernel). Accept that identity at
                        // this boundary so an automatic prelude module is
                        // still recognized as already compiled.
                        const auto backendPrefixed = "Kex." + candidate;
                        if (m_importedInterfaces->modules.count(backendPrefixed)) {
                            interfaceModule = backendPrefixed;
                            break;
                        }
                        const auto dot = candidate.rfind('.');
                        if (dot == std::string::npos) break;
                        candidate.resize(dot);
                    }
                    if (!interfaceModule.empty())
                        dependencyPath = std::move(interfaceModule);
                }
                m_referencedModules.insert(std::move(dependencyPath));
            }
            bool isImportedNamespace = importedPath && m_importedInterfaces &&
                m_importedInterfaces->modules.count(*importedPath) > 0;
            bool isNamespaceCall = node.receiver &&
                (isNamespaceReceiver(*node.receiver) || isImportedNamespace);
            // A LOCAL nested module is qualified the same way: without this
            // `CollisionWeb.Server.build(7000)` checked as the bare name
            // `build` and picked the prelude's `Web.Server.build` — the two
            // are indistinguishable there, both `Integer -> Server`, and the
            // imported one is listed first. Only when signatures really were
            // published under that key, so value chains through a static
            // constructor (`Temperature.Fahrenheit(212)`) stay untouched.
            const bool isLocalNamespace = importedPath &&
                m_localModules.contains(*importedPath) &&
                m_userSignatures.count(*importedPath + "::" + node.method) > 0;
            if (isImportedNamespace || isLocalNamespace) {
                callName = *importedPath + "::" + node.method;
            } else if (isNamespaceCall &&
                std::holds_alternative<ast::UpperIdentifier>(node.receiver->kind)) {
                callName = std::get<ast::UpperIdentifier>(node.receiver->kind).name +
                           "::" + node.method;
            }
            // `Type.of(Hello)` where `Hello` NAMES a type answers with that
            // type. A bare type name is already a value elsewhere (`x.to(String)`),
            // and it is not a runtime value at all — the walker died on
            // "Undefined identifier: Hello" while BEAM lowered it to an atom
            // and reported `Atom`.
            if (callName == "Type::of" && node.args.size() == 1 && node.args[0]) {
                if (auto referenced = typeNameReference(*node.args[0])) {
                    if (auto structured = structuredTypeOf(referenced)) {
                        m_staticTypeOfCalls[&node] =
                            {std::move(*structured), /*evaluateArgument=*/false};
                        return Type::named("Type");
                    }
                }
                if (auto signature = namedFunctionSignature(*node.args[0])) {
                    StructuredType function;
                    function.name = "Function";
                    function.pure = !signature->isFoul;
                    bool complete = true;
                    for (const auto& param : signature->params) {
                        auto structured = structuredTypeOf(param);
                        if (!structured) { complete = false; break; }
                        function.args.push_back(std::move(*structured));
                    }
                    if (complete)
                        if (auto result = structuredTypeOf(signature->result)) {
                            function.args.push_back(std::move(*result));
                            m_staticTypeOfCalls[&node] =
                                {std::move(function), /*evaluateArgument=*/false};
                            return Type::named("Type");
                        }
                }
            }
            // `value.as(Target)` is a total, compile-time checked conversion.
            // Distinct types erase to their backing representation, so a
            // compatible retag changes only the static type. String is the
            // universal display target and is lowered to the existing total
            // display conversion by both backends.
            if (node.method == "as" && node.args.size() == 1 && node.args[0] &&
                node.namedArgs.empty() && !node.block) {
                std::unordered_map<std::string, TypePtr> targetGenerics;
                auto target = node.targetType
                    ? resolveTypeExpr(*node.targetType, targetGenerics)
                    : typeNameReference(*node.args[0]);
                if (!target) {
                    error(expr.location,
                          "`.as` needs a type name as its target");
                    return Type::unknown();
                }
                auto source = node.receiver ? inferExpr(*node.receiver)
                                            : Type::unknown();
                const auto targetIsString = [](const TypePtr& type) {
                    const auto* primitive =
                        std::get_if<PrimitiveType>(&type->kind);
                    return primitive &&
                           primitive->kind == PrimitiveType::String;
                };
                if (!targetIsString(target) &&
                    !representationsCompatible(source, target)) {
                    error(expr.location,
                          "Cannot convert " + typeToString(source) + " to " +
                              typeToString(target) + " with `.as`");
                }
                if (auto* sized = std::get_if<SizedIntType>(&target->kind);
                    sized && sized->bits == 8 && !sized->isSigned &&
                    !typesEqual(resolve(source), target)) {
                    error(expr.location,
                          "Unchecked narrowing to Byte is not allowed; use `.to(Byte)`");
                }
                return target;
            }
            // `x.to(T)` answers `T?`. The clauses implementing the protocol
            // take their target as an ordinary VALUE parameter
            // (`let to(value, String) = ...`), so every one of them has an
            // `unknown` second parameter, overload resolution cannot tell them
            // apart, and the first one wins: `"34".to(Integer)` and
            // `(1..3).to(List)` both typed as `String?` while evaluating to an
            // Integer and a list. The target NAMES a type, so read it.
            //
            // A String target is left alone: `String?` is already the right
            // answer for it, and a receiver that declares its own total
            // conversion (`Measure.to(String) -> String`) must keep it.
            // Only the bare `to(T)` form. `to(T, radix: n)` carries named
            // arguments, and answering here would skip the call resolution
            // lowering needs to place them — it reported `radix:` as an
            // unknown named argument on BEAM.
            if (node.method == "to" && node.args.size() == 1 && node.args[0] &&
                node.namedArgs.empty() && !node.block) {
                const auto targetIsString = [](const TypePtr& t) {
                    auto* prim = std::get_if<PrimitiveType>(&t->kind);
                    return prim && prim->kind == PrimitiveType::String;
                };
                std::unordered_map<std::string, TypePtr> targetGenerics;
                auto target = node.targetType
                    ? resolveTypeExpr(*node.targetType, targetGenerics)
                    : typeNameReference(*node.args[0]);
                if (target && !targetIsString(target)) {
                    for (const auto& argument : node.args) inferExpr(*argument);
                    if (node.receiver) inferExpr(*node.receiver);
                    return Type::optional(target);
                }
            }
            // `Type.returnedBy(f)`: resolved entirely here — a function value
            // carries no signature at runtime. The argument must NAME a
            // function; an overloaded name has no single answer.
            if (callName == "Type::returnedBy" && node.args.size() == 1 &&
                node.args[0]) {
                const auto* identifier =
                    std::get_if<ast::Identifier>(&node.args[0]->kind);
                std::string functionName;
                if (identifier) {
                    functionName = identifier->name;
                } else if (const auto* qualified =
                               std::get_if<ast::MethodCall>(&node.args[0]->kind);
                           qualified && qualified->args.empty() &&
                           qualified->receiver) {
                    if (auto path = importedModulePath(*qualified->receiver))
                        functionName = *path + "::" + qualified->method;
                }
                std::vector<Signature> candidates;
                if (!functionName.empty()) {
                    if (auto user = m_userSignatures.find(functionName);
                        user != m_userSignatures.end())
                        candidates = user->second;
                    if (candidates.empty())
                        candidates = importedCandidateSignatures(functionName);
                }
                if (candidates.empty()) {
                    error(expr.location,
                          "`Type.returnedBy` needs the NAME of a function; "
                          "a lambda or a function value carries no signature");
                } else if (candidates.size() > 1) {
                    std::string message =
                        "`Type.returnedBy` cannot choose between the overloads of `" +
                        functionName + "`";
                    for (const auto& candidate : candidates)
                        message += "\n\n" + displaySignature(functionName, candidate);
                    error(expr.location, message);
                } else if (auto structured =
                               structuredTypeOf(candidates.front().result)) {
                    m_staticTypeOfCalls[&node] =
                        {std::move(*structured), /*evaluateArgument=*/false};
                } else {
                    error(expr.location,
                          "`Type.returnedBy` cannot answer for `" + functionName +
                          "`: its return type is not concrete");
                }
                return Type::named("Type");
            }

            std::vector<TypePtr> argTypes;
            if (!isNamespaceCall)
                argTypes.push_back(node.receiver ? inferExpr(*node.receiver) : Type::unknown());
            // First pass: infer concrete args; defer lambdas with parameters
            // until the selected receiver signature can provide context.
            std::vector<std::pair<size_t, size_t>> contextualLambdas;
            for (size_t i = 0; i < node.args.size(); i++) {
                const auto* lambda = node.args[i]
                    ? std::get_if<ast::Lambda>(&node.args[i]->kind) : nullptr;
                if (node.args[i] &&
                    (std::holds_alternative<ast::ShorthandLambda>(
                         node.args[i]->kind) ||
                     isBareCapture(node.args[i].get()) ||
                     (lambda && !lambda->params.empty()))) {
                    argTypes.push_back(Type::unknown()); // placeholder
                    contextualLambdas.emplace_back(
                        isNamespaceCall ? i : 1 + i, i);
                } else {
                    argTypes.push_back(node.args[i] ? inferExpr(*node.args[i]) : Type::unknown());
                }
            }
            const bool serverSlotCall = [&] {
                if (isNamespaceCall || argTypes.empty() ||
                    !m_slotMethodNames.count(node.method))
                    return false;
                auto receiver = resolve(argTypes.front());
                auto* named = std::get_if<NamedType>(&receiver->kind);
                return named && named->name == "Server" &&
                       named->typeArgs.size() == 1;
            }();
            if (!isNamespaceCall && node.method == "reply" &&
                argTypes.size() >= 2) {
                auto receiver = resolve(argTypes.front());
                if (auto* named = std::get_if<NamedType>(&receiver->kind);
                    named && named->name == "From" &&
                    named->typeArgs.size() == 1) {
                    auto actual = resolve(argTypes[1]);
                    auto expected = resolve(named->typeArgs.front());
                    if (!containsOpenType(actual) && !containsOpenType(expected) &&
                        !argMatchesParam(actual, expected))
                        typeMismatch(node.args.front()->location, expected, actual);
                }
            }
            for (const auto& [name, arg] : node.namedArgs) {
                auto type = arg ? inferExpr(*arg) : Type::unknown();
                if (serverSlotCall && name == "within") {
                    auto resolved = resolve(type);
                    const bool infinity = [&] {
                        auto* atom = std::get_if<PrimitiveType>(&resolved->kind);
                        return atom && atom->kind == PrimitiveType::Atom &&
                               atom->atomName == "infinity";
                    }();
                    if (!infinity && !containsOpenType(resolved) &&
                        !typesEqual(resolved, Type::integer()))
                        error(arg ? arg->location : expr.location,
                              "`within:` expects Integer milliseconds or :infinity, got " +
                                  typeToString(resolved));
                    continue;
                }
                argTypes.push_back(std::move(type));
            }
            // Second pass: infer lambdas with parameter hints from the signature.
            for (const auto& [argIdx, rawIdx] : contextualLambdas) {
                auto hints = resolveArgHints(
                    callName, argTypes, argIdx, /*isMethodCall=*/!isNamespaceCall);
                argTypes[argIdx] = inferBlock(*node.args[rawIdx], hints);
            }
            if (node.block) {
                auto hints = resolveBlockHints(
                    callName, argTypes, /*isMethodCall=*/!isNamespaceCall);
                argTypes.push_back(inferBlock(**node.block, hints));
            }
            // `Type.of(x)`: record what the CHECKER knows about the argument.
            // A checked expression knows things the value cannot carry — the
            // unused half of a Result, an empty list's element type — and both
            // backends prefer this recording over asking the value. A nullary
            // constructor is widened to its ADT (`Red` is a `Colour`).
            if (callName == "Type::of" && argTypes.size() == 1) {
                // Widen constructor names to their ADT, through containers:
                // `[Red, Blue(2)]` is a `[Colour]`, not a `[Red]`. Type
                // ARGUMENTS are left alone — a phantom typestate parameter is
                // spelled with constructors too, and widening those erases the
                // distinction they exist to make.
                std::function<TypePtr(const TypePtr&)> widen =
                    [&](const TypePtr& type) -> TypePtr {
                    auto resolved = resolve(type);
                    if (auto* named = std::get_if<NamedType>(&resolved->kind)) {
                        if (auto owner = m_adtOfConstructor.find(named->name);
                            owner != m_adtOfConstructor.end() &&
                            owner->second != named->name)
                            return Type::named(owner->second);
                        return resolved;
                    }
                    if (auto* list = std::get_if<ListType>(&resolved->kind))
                        return Type::list(widen(list->element));
                    if (auto* optional = std::get_if<OptionalType>(&resolved->kind))
                        return Type::optional(widen(optional->inner));
                    if (auto* map = std::get_if<MapType>(&resolved->kind))
                        return Type::map(widen(map->key), widen(map->value));
                    if (auto* tuple = std::get_if<TupleType>(&resolved->kind)) {
                        std::vector<TypePtr> elements;
                        for (const auto& element : tuple->elements)
                            elements.push_back(widen(element));
                        return Type::tuple(std::move(elements));
                    }
                    return resolved;
                };
                if (auto structured = structuredTypeOf(widen(argTypes[0])))
                    m_staticTypeOfCalls[&node] = {std::move(*structured), true};
            }

            // pid.send(msg) UFCS — check msg type against Process<Msg>.
            // argTypes[0] = pid type, argTypes[1] = msg type.
            if ((node.method == "send" || node.method == "sendFrom") &&
                argTypes.size() == 2) {
                auto pidType = resolve(argTypes[0]);
                auto msgType = resolve(argTypes[1]);
                if (auto* nt = std::get_if<NamedType>(&pidType->kind)) {
                    if (nt->name == "Process" && nt->typeArgs.size() == 1) {
                        auto declared = resolve(nt->typeArgs[0]);
                        if (!std::holds_alternative<UnknownType>(declared->kind)) {
                            if (auto* tv = std::get_if<TypeVar>(&declared->kind)) {
                                unifyVar(tv->id, msgType);
                            } else if (!std::holds_alternative<UnknownType>(msgType->kind) &&
                                       !std::holds_alternative<TypeVar>(msgType->kind) &&
                                       !argMatchesParam(msgType, declared)) {
                                error(expr.location,
                                      "send: message type " + typeToString(msgType) +
                                      " does not match Process<" + typeToString(declared) + ">");
                            }
                        }
                    }
                }
                return msgType;
            }
            if ((callName == "Binary::fromBytes" || callName == "String::fromBytes") &&
                node.args.size() == 1 && node.args[0]) {
                if (auto* list = std::get_if<ast::ListExpr>(&node.args[0]->kind))
                    for (const auto& element : list->elements) if (element) {
                        bool invalid = false;
                        if (auto* literal = std::get_if<ast::IntLiteral>(&element->kind)) {
                            try {
                                size_t parsed = 0;
                                const auto value = std::stoull(literal->value, &parsed, 0);
                                invalid = parsed != literal->value.size() || value > 255;
                            } catch (...) { invalid = true; }
                        } else if (auto* unary = std::get_if<ast::UnaryOp>(&element->kind);
                                   unary && unary->op == TokenType::Minus && unary->operand &&
                                   std::holds_alternative<ast::IntLiteral>(unary->operand->kind)) {
                            invalid = true;
                        }
                        if (invalid)
                            error(element->location, "Byte literal must be in 0..255");
                    }
            }
            return checkCall(callName, argTypes, expr.location,
                             /*isMethodCall=*/true, &node, &expr);
        }
        else if constexpr (std::is_same_v<T, ast::ListExpr>) {
            TypePtr elemType = Type::unknown();
            for (const auto& elem : node.elements) {
                if (elem) {
                    auto t = inferExpr(*elem);
                    bool elemPermissive = std::holds_alternative<UnknownType>(elemType->kind) ||
                                         std::holds_alternative<TypeVar>(elemType->kind);
                    bool tPermissive = std::holds_alternative<UnknownType>(t->kind) ||
                                       std::holds_alternative<TypeVar>(t->kind) ||
                                       std::holds_alternative<FuncType>(t->kind);
                    bool elemFuncPermissive = elemPermissive ||
                                              std::holds_alternative<FuncType>(elemType->kind);
                    if (elemPermissive) {
                        elemType = t; // adopt the concrete type if we have one
                    } else if (!tPermissive && !elemFuncPermissive &&
                               !argMatchesParam(t, elemType) && !argMatchesParam(elemType, t)) {
                        // Before erroring, check if both types share a common trait.
                        // If so, widen the element type to that trait.
                        std::string common = m_traits.commonTrait(elemType, t);
                        if (!common.empty()) {
                            elemType = Type::named(common);
                        } else {
                            // Sibling nullary constructors share their parent
                            // ADT as a natural list element type. Preserve
                            // constructor refinements for overload selection,
                            // but widen `[Less, Equal, Greater]` to
                            // `[Comparison]` instead of rejecting it.
                            auto* lhs = std::get_if<NamedType>(&elemType->kind);
                            auto* rhs = std::get_if<NamedType>(&t->kind);
                            auto lhsOwner = lhs
                                ? m_adtOfConstructor.find(lhs->name)
                                : m_adtOfConstructor.end();
                            auto rhsOwner = rhs
                                ? m_adtOfConstructor.find(rhs->name)
                                : m_adtOfConstructor.end();
                            if (lhsOwner != m_adtOfConstructor.end() &&
                                rhsOwner != m_adtOfConstructor.end() &&
                                lhsOwner->second == rhsOwner->second) {
                                elemType = Type::named(lhsOwner->second);
                            } else {
                                error(expr.location,
                                      "List elements must be the same type. Expected " +
                                      typeToString(elemType) + ", got " + typeToString(t));
                            }
                        }
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
            auto startType = node.start ? inferExpr(*node.start) : Type::unknown();
            if (node.end) inferExpr(*node.end);
            // Infer element type from the start bound: Char ranges → Range<Char>,
            // everything else → Range<Integer> (the common case).
            auto* prim = std::get_if<PrimitiveType>(&startType->kind);
            auto elemType = (prim && prim->kind == PrimitiveType::Char)
                ? Type::charT() : Type::integer();
            return Type::named("Range", {elemType});
        }
        else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            if (node.letPattern) {
                // `if let Pattern = expr` — infer scrutinee, bind pattern vars
                // in a scope covering only the then-body (already scoped in
                // resolve pass). Skip Bool check — it's a pattern match.
                auto scrutinee = node.condition
                    ? inferExpr(*node.condition) : nullptr;
                pushScope();
                if (node.letPattern)
                    bindPatternVars(*node.letPattern, scrutinee);
            } else if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "If condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            // A branch that exits the function produces nothing for the
            // conditional to agree with, so it must not drive the result type.
            auto branchBodyType = [this](const std::vector<ast::ExprPtr>& body) {
                auto inferred = inferBody(body);
                return alwaysReturns(body) ? Type::voidType() : inferred;
            };
            auto thenType = branchBodyType(node.thenBody);
            if (node.letPattern) popScope();
            TypePtr branchType = resolve(thenType);  // tracks the first concrete non-Never branch
            for (const auto& [cond, body] : node.elifs) {
                if (cond) inferExpr(*cond);
                auto elifType = resolve(branchBodyType(body));
                auto rt = resolve(branchType);
                bool rtPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                    std::holds_alternative<UnknownType>(rt->kind) ||
                                    std::holds_alternative<VoidType>(rt->kind);
                bool rePermissive = std::holds_alternative<TypeVar>(elifType->kind) ||
                                    std::holds_alternative<UnknownType>(elifType->kind) ||
                                    std::holds_alternative<VoidType>(elifType->kind);
                if (!rtPermissive && !rePermissive &&
                    !argMatchesParam(elifType, rt) && !argMatchesParam(rt, elifType)) {
                    error(expr.location, "Branch type mismatch: 'if' returns " +
                          typeToString(rt) + " but 'elif' returns " + typeToString(elifType));
                }
                if (rtPermissive && !rePermissive) branchType = elifType;
            }
            if (node.elseBody) {
                auto elseType = resolve(branchBodyType(*node.elseBody));
                auto rt = resolve(branchType);
                bool thenPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                      std::holds_alternative<UnknownType>(rt->kind) ||
                                      std::holds_alternative<VoidType>(rt->kind);
                bool elsePermissive = std::holds_alternative<TypeVar>(elseType->kind) ||
                                      std::holds_alternative<UnknownType>(elseType->kind) ||
                                      std::holds_alternative<VoidType>(elseType->kind);
                if (!thenPermissive && !elsePermissive &&
                    !argMatchesParam(elseType, rt) && !argMatchesParam(rt, elseType)) {
                    error(expr.location, "Branch type mismatch: 'if' returns " +
                          typeToString(rt) + " but 'else' returns " + typeToString(elseType));
                }
                if (thenPermissive && !elsePermissive) branchType = elseType;
            }
            // With no `else` the conditional may produce nothing at all, so it
            // cannot hand its then-branch's type to the block it closes —
            // `if done? ... return Error(e) ... end` is not a Result-valued
            // expression.
            if (!node.elseBody) return Type::voidType();
            return resolve(branchType);
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
                    if (pat) {
                        checkPatternArity(*pat);
                        bindPatternVars(*pat, subjectType);
                    }
                }
                if (clause.guard && *clause.guard) inferExpr(**clause.guard);
                if (clause.body && alwaysReturns(*clause.body)) {
                    // The arm exits the function; it produces nothing for the
                    // match to agree with.
                    inferExpr(*clause.body);
                } else if (clause.body) {
                    auto t = resolve(inferExpr(*clause.body));
                    auto rt = resolve(resultType);
                    if (std::holds_alternative<UnknownType>(rt->kind) ||
                        std::holds_alternative<TypeVar>(rt->kind)) {
                        resultType = t;  // adopt first concrete arm type
                    } else {
                        // Check subsequent arms match the first concrete arm.
                        bool armPermissive = std::holds_alternative<TypeVar>(t->kind) ||
                                            std::holds_alternative<UnknownType>(t->kind);
                        if (!armPermissive && !argMatchesParam(t, rt) && !argMatchesParam(rt, t)) {
                            error(expr.location, "Match arm type mismatch: expected " +
                                  typeToString(rt) + " but arm returns " + typeToString(t));
                        }
                    }
                }
                popScope();
            }
            checkMatchExhaustiveness(node, expr.location);
            return resultType;
        }
        else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            TypePtr resultType = Type::unknown();
            for (const auto& clause : node.clauses) {
                pushScope();
                if (node.senderBinding)
                    defineVar(*node.senderBinding,
                              Type::named("Process", {Type::unknown()}));
                for (const auto& pattern : clause.patterns)
                    if (pattern) {
                        checkPatternArity(*pattern);
                        bindPatternVars(*pattern);
                    }
                if (clause.guard && *clause.guard)
                    inferExpr(**clause.guard);
                if (clause.body) {
                    auto arm = resolve(inferExpr(*clause.body));
                    if (std::holds_alternative<UnknownType>(
                            resolve(resultType)->kind))
                        resultType = arm;
                }
                popScope();
            }
            if (node.timeout && *node.timeout) inferExpr(**node.timeout);
            if (node.afterBody && *node.afterBody) {
                auto after = resolve(inferExpr(**node.afterBody));
                if (std::holds_alternative<UnknownType>(
                        resolve(resultType)->kind))
                    resultType = after;
            }
            return resultType;
        }
        else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            return node.value ? inferExpr(*node.value) : Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::Lambda>) {
            // Lambda used as a value (not a trailing block — that path goes
            // through inferBlock). Infer param types and body, return a proper
            // FuncType so call-site checking can validate the argument types.
            if (node.params.empty()) {
                m_blockDepth++;
                auto bodyType = inferBody(node.body);
                m_blockDepth--;
                return Type::func({}, resolve(bodyType));
            }
            pushScope();
            std::vector<TypePtr> paramTypes;
            std::unordered_map<std::string, TypePtr> genericVars;
            for (const auto& param : node.params) {
                auto pt = param.type
                    ? resolveTypeExpr(**param.type, genericVars)
                    : freshTypeVar();
                paramTypes.push_back(pt);
                if (param.name != "_") {
                    defineVar(param.name, pt);
                    // Value-position lambdas need the same tooling binding as
                    // contextual/trailing blocks. Without it `{ |name| ... }`
                    // had no local hover entry and the LSP fell through to an
                    // unrelated exported symbol with the same name.
                    m_patternBindings.push_back(
                        {param.name, expr.location, pt});
                }
            }
            m_blockDepth++;
            auto bodyType = inferBody(node.body);
            m_blockDepth--;
            auto resultType = resolve(bodyType);
            if (node.returnAnnotation) {
                auto declared =
                    resolveTypeExpr(**node.returnAnnotation, genericVars);
                if (!std::holds_alternative<UnknownType>(resultType->kind) &&
                    !std::holds_alternative<TypeVar>(resultType->kind) &&
                    !argMatchesParam(resultType, declared))
                    typeMismatch(expr.location, declared, resultType);
                resultType = declared;
            }
            popScope();
            // Resolve param types after body inference — body may have constrained them.
            for (auto& pt : paramTypes) pt = resolve(pt);
            return Type::func(std::move(paramTypes), resultType);
        }
        else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
            pushScope();
            inferBody(node.body);
            popScope();
            // Process<Msg> — Msg is a fresh TypeVar that unification resolves
            // against the declared return-type annotation (e.g. -> Process<Counter>).
            return Type::named("Process", {freshTypeVar()});
        }
        else if constexpr (std::is_same_v<T, ast::LoopExpr>) {
            if (node.counter) {
                pushScope();
                if (*node.counter != "_") defineVar(*node.counter, Type::integer());
                inferBody(node.body);
                popScope();
            } else {
                inferBody(node.body);
            }
            return Type::voidType();  // infinite loop never returns
        }
        else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "While condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            inferBody(node.body);
            return Type::unit();
        }
        else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            // Check each value against the field's DECLARED type. Without
            // this, `User { name: 42 }` on `name : String` was accepted and
            // only surfaced later — or not at all.
            std::string sourceName = node.typeName;
            TypePtr contextualRecordType;
            if (sourceName == "This" || sourceName == "New") {
                if (!m_inMakeBlock || !m_currentMakeType) {
                    error(expr.location, "`" + sourceName +
                                             "` construction requires a make block receiver");
                    return Type::unknown();
                }
                auto receiver = resolve(m_currentMakeType);
                auto* named = std::get_if<NamedType>(&receiver->kind);
                if (!named || !m_recordFields.count(resolveRecordName(named->name))) {
                    error(expr.location, "`" + sourceName +
                                             "` requires a record receiver");
                    return Type::unknown();
                }
                sourceName = named->name;
                contextualRecordType = receiver;
            }
            const auto recordName = resolveRecordName(sourceName);
            // Naming a qualified record is a reference to its MODULE: the
            // record's `make` block and field defaults live in that source,
            // and on BEAM the module has to be in the build at all
            // (kexhq/kex#143). The module is the qualifier, not the record —
            // inserting `Mock.Files` here put a type name into a list of
            // modules to compile.
            if (auto dot = recordName.rfind('.'); dot != std::string::npos)
                m_referencedModules.insert(recordName.substr(0, dot));
            auto record = m_recordFields.find(recordName);
            bool hasSpread = node.typeName == "New";
            std::unordered_set<std::string> supplied;
            // Type arguments recovered from the values, slot by slot.
            std::unordered_map<int, TypePtr> typeParamSlots;
            for (const auto& entry : node.fields) {
                const auto& fieldName = entry.name;
                const auto& val = entry.value;
                if (!val) continue;
                auto valueType = resolve(inferExpr(*val));
                if (entry.spread) {
                    hasSpread = true;
                    // `Box { ...other, items: … }` keeps whatever `other`
                    // already pinned for the slots the new fields don't.
                    if (auto* spreadNamed =
                            std::get_if<NamedType>(&valueType->kind))
                        for (size_t i = 0; i < spreadNamed->typeArgs.size(); ++i)
                            bindRecordTypeParams(
                                Type::typeVar(-static_cast<int>(i + 1)),
                                spreadNamed->typeArgs[i], typeParamSlots);
                    if (!containsOpenType(valueType) &&
                        !argMatchesParam(valueType, Type::named(recordName)))
                        error(val->location, "record spread expects `" +
                                                 sourceName + "`, but got " +
                                                 typeToString(valueType));
                    continue;
                }
                supplied.insert(fieldName);
                if (record == m_recordFields.end()) continue;
                auto declared = record->second.find(fieldName);
                // An unknown field name used to be skipped, which made a
                // renamed field silently absorb the old spelling: the
                // interpreter stored `zebra:` on a record with no such field
                // and `.zebra` read it straight back, while BEAM only failed at
                // runtime with "Undefined method: zebra". Renaming a field
                // therefore broke callers with no diagnostic at all. The
                // pattern side has always checked this (see checkPatternArity);
                // construction just never did.
                if (declared == record->second.end()) {
                    // List the layout: on a rename or a typo the useful
                    // question is immediately "then what ARE the fields?".
                    // Sorted so the message is stable — m_recordFields is a
                    // hash map.
                    std::vector<std::string> available;
                    available.reserve(record->second.size());
                    for (const auto& [known, _] : record->second)
                        available.push_back(known);
                    std::sort(available.begin(), available.end());
                    std::string layout;
                    for (std::size_t i = 0; i < available.size(); i++) {
                        if (i) layout += ", ";
                        layout += available[i];
                    }
                    error(val->location,
                          "record `" + sourceName + "` has no field `" +
                          fieldName + "` — it has " +
                          (layout.empty() ? "no fields" : layout));
                    continue;
                }
                auto expected = resolve(declared->second);
                bindRecordTypeParams(expected, valueType, typeParamSlots);
                // Stay quiet where either side is open ANYWHERE inside it: a
                // type variable, a type the checker could not pin down, or a
                // trait constraint, which names a set rather than a concrete
                // type. Two cases this must not reject: `make Integer do …
                // Duration { seconds: this * 1.0 }` infers `Number` for the
                // value, and a `Map<Any, String>` field takes `{"k": "v"}`
                // — there the openness is nested, not at the top.
                if (containsOpenType(valueType) || containsOpenType(expected))
                    continue;
                if (!argMatchesParam(valueType, expected))
                    error(val->location,
                          "`" + sourceName + "." + fieldName +
                          "` expects " + typeToString(expected) + ", but got " +
                          typeToString(valueType));
            }
            if (!hasSpread) {
                std::vector<std::string> missing;
                for (const auto& required : m_requiredRecordFields[recordName])
                    if (!supplied.count(required)) missing.push_back(required);
                std::sort(missing.begin(), missing.end());
                if (!missing.empty()) {
                    std::string names;
                    for (std::size_t i = 0; i < missing.size(); ++i) {
                        if (i) names += ", ";
                        names += "`" + missing[i] + "`";
                    }
                    error(expr.location, "missing mandatory field" +
                                             std::string(missing.size() == 1 ? " " : "s ") +
                                             names + " constructing `" + sourceName + "`");
                }
            }
            // `recordName` is `node.typeName` itself when no declaration was
            // found, so the resolved identity is always the right answer here:
            // a literal must have the same type name that annotations
            // mentioning the same record resolve to.
            if (contextualRecordType) return contextualRecordType;
            // A generic record's literal has to carry its arguments, or the
            // value can never flow back into anything that names them:
            // `Box { items: [1, 2, 3] }` is a `Box<Integer>`, and only that
            // spelling matches a `Box<A>` receiver or reads `.items` back as
            // `[Integer]` (kexhq/kex#203). Slots no field pinned stay open,
            // which is what `Box { items: [] }` deserves.
            const size_t paramCount = record == m_recordFields.end()
                ? 0 : recordTypeParamCount(record->second);
            if (paramCount == 0) return Type::named(recordName);
            std::vector<TypePtr> typeArgs(paramCount, Type::unknown());
            for (const auto& [slot, bound] : typeParamSlots) {
                const auto index = static_cast<size_t>(-slot - 1);
                if (index < typeArgs.size()) typeArgs[index] = bound;
            }
            return Type::named(recordName, std::move(typeArgs));
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
            // `expr if cond` is a guarded STATEMENT: when the guard is false
            // nothing is produced, so it cannot hand its expression's type to
            // the block it closes. `return Ok(x) if done?` is the shape that
            // matters — it must not make its branch look like it yields a
            // Result.
            if (node.expr) {
                inferExpr(*node.expr);
                return Type::voidType();
            }
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::BlockExpr>) {
            return inferBody(node.body);
        }
        else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            if (node.condition) {
                auto condType = inferExpr(*node.condition);
                auto resolved = resolve(condType);
                if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                    unifyVar(tv->id, Type::boolean());
                } else if (!std::holds_alternative<UnknownType>(resolved->kind) &&
                           !typesEqual(resolved, Type::boolean())) {
                    error(expr.location, "then/else condition must be Bool, got " +
                          typeToString(resolved));
                }
            }
            auto thenType = node.thenExpr ? inferExpr(*node.thenExpr) : Type::unknown();
            if (node.elseExpr) {
                auto elseType = inferExpr(*node.elseExpr);
                auto rt = resolve(thenType);
                auto re = resolve(elseType);
                bool thenPermissive = std::holds_alternative<TypeVar>(rt->kind) ||
                                      std::holds_alternative<UnknownType>(rt->kind);
                bool elsePermissive = std::holds_alternative<TypeVar>(re->kind) ||
                                      std::holds_alternative<UnknownType>(re->kind);
                if (!thenPermissive && !elsePermissive &&
                    !argMatchesParam(re, rt) && !argMatchesParam(rt, re)) {
                    error(expr.location, "Branch type mismatch: 'then' returns " +
                          typeToString(rt) + " but 'else' returns " + typeToString(re));
                }
            }
            return resolve(thenType);
        }
        else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
            // &.method → (T) -> result; T unknown until used in context.
            auto paramType = freshTypeVar();
            std::vector<TypePtr> callArgs = {paramType};
            for (const auto& arg : node.args) {
                if (arg) callArgs.push_back(inferExpr(*arg));
            }
            auto resultType = checkCall(node.name, callArgs, expr.location);
            return Type::func({paramType}, resolve(resultType));
        }
        else if constexpr (std::is_same_v<T, ast::CurryPlaceholder>) {
            return Type::unknown();
        }
        else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
            std::vector<Signature> candidates;
            if (node.isOperator) {
                Signature signature;
                signature.name = node.name;
                if (node.name == "!") {
                    signature.params = {Type::boolean()};
                    signature.result = Type::boolean();
                } else if (node.name == "&&" || node.name == "||") {
                    signature.params = {Type::boolean(), Type::boolean()};
                    signature.result = Type::boolean();
                } else {
                    auto operand = freshTypeVar();
                    signature.params = {operand, operand};
                    if (node.name == "==" || node.name == "!=" ||
                        node.name == "<" || node.name == ">" ||
                        node.name == "<=" || node.name == ">=")
                        signature.result = Type::boolean();
                    else
                        signature.result = operand;
                }
                candidates.push_back(std::move(signature));
            } else {
                const auto key = node.module.empty()
                    ? node.name : node.module + "::" + node.name;
                if (auto user = m_userSignatures.find(key);
                    user != m_userSignatures.end())
                    candidates = user->second;
                if (candidates.empty())
                    candidates = importedCandidateSignatures(key);
            }

            if (candidates.empty()) {
                // Preserve gradual recovery for an unresolved capture while
                // still visiting every bound argument.
                int open = 0;
                for (const auto& group : node.argGroups)
                    for (const auto& arg : group)
                        if (std::holds_alternative<ast::CurryPlaceholder>(arg->kind))
                            ++open;
                        else
                            inferExpr(*arg);
                std::vector<TypePtr> params(open, Type::unknown());
                return params.empty() ? Type::unknown()
                                      : Type::func(std::move(params), Type::unknown());
            }

            if (node.argGroups.empty()) {
                std::vector<const Signature*> distinct;
                for (const auto& signature : candidates) {
                    const bool duplicate = std::any_of(
                        distinct.begin(), distinct.end(), [&](const Signature* other) {
                            if (other->params.size() != signature.params.size()) return false;
                            for (size_t i = 0; i < signature.params.size(); ++i)
                                if (!typesEqual(other->params[i], signature.params[i]))
                                    return false;
                            return true;
                        });
                    if (!duplicate) distinct.push_back(&signature);
                }
                const bool sameArityAmbiguity = std::any_of(
                    distinct.begin(), distinct.end(), [&](const Signature* left) {
                        return std::any_of(
                            distinct.begin(), distinct.end(),
                            [&](const Signature* right) {
                                return left != right &&
                                    left->params.size() == right->params.size();
                            });
                    });
                if (sameArityAmbiguity) {
                    std::string message = "Cannot reference overloaded function `" +
                        node.name + "` without disambiguating arguments";
                    for (const auto* signature : distinct)
                        message += "\n\n" + displaySignature(node.name, *signature);
                    error(expr.location, message);
                }
            }

            // A capture is an application of a fresh INSTANCE of the public
            // signature. This retains relationships such as A -> A -> A,
            // while binding `~add(1)` specializes the open argument/result to
            // Integer without mutating the function's declared signature.
            std::unordered_map<int, TypePtr> instantiatedVars;
            std::function<TypePtr(const TypePtr&)> instantiate =
                [&](const TypePtr& type) -> TypePtr {
                    if (!type) return type;
                    return std::visit([&](const auto& part) -> TypePtr {
                        using Part = std::decay_t<decltype(part)>;
                        if constexpr (std::is_same_v<Part, TypeVar>) {
                            auto [it, inserted] = instantiatedVars.emplace(part.id, nullptr);
                            if (inserted) it->second = freshTypeVar();
                            return it->second;
                        } else if constexpr (std::is_same_v<Part, ListType>) {
                            return Type::list(instantiate(part.element));
                        } else if constexpr (std::is_same_v<Part, MapType>) {
                            return Type::map(instantiate(part.key), instantiate(part.value));
                        } else if constexpr (std::is_same_v<Part, FuncType>) {
                            std::vector<TypePtr> params;
                            for (const auto& param : part.params)
                                params.push_back(instantiate(param));
                            return Type::func(std::move(params), instantiate(part.result));
                        } else if constexpr (std::is_same_v<Part, TupleType>) {
                            std::vector<TypePtr> elements;
                            for (const auto& element : part.elements)
                                elements.push_back(instantiate(element));
                            return Type::tuple(std::move(elements));
                        } else if constexpr (std::is_same_v<Part, OptionalType>) {
                            return Type::optional(instantiate(part.inner));
                        } else if constexpr (std::is_same_v<Part, IntersectionType>) {
                            std::vector<TypePtr> members;
                            for (const auto& member : part.members)
                                members.push_back(instantiate(member));
                            return Type::intersection(std::move(members));
                        } else if constexpr (std::is_same_v<Part, RecordType>) {
                            std::vector<std::pair<std::string, TypePtr>> fields;
                            for (const auto& [name, fieldType] : part.fields)
                                fields.emplace_back(name, instantiate(fieldType));
                            return Type::record(std::move(fields));
                        } else if constexpr (std::is_same_v<Part, NamedType>) {
                            std::vector<TypePtr> args;
                            for (const auto& arg : part.typeArgs)
                                args.push_back(instantiate(arg));
                            return Type::named(part.name, std::move(args));
                        } else {
                            return type;
                        }
                    }, type->kind);
                };

            const auto& selected = candidates.front();
            std::vector<TypePtr> params;
            for (const auto& param : selected.params)
                params.push_back(instantiate(param));
            auto resultType = instantiate(selected.result);
            std::vector<TypePtr> remaining;
            size_t position = 0;
            for (const auto& group : node.argGroups) {
                for (const auto& arg : group) {
                    if (position >= params.size()) {
                        if (!std::holds_alternative<ast::CurryPlaceholder>(arg->kind))
                            inferExpr(*arg);
                        ++position;
                        continue;
                    }
                    if (std::holds_alternative<ast::CurryPlaceholder>(arg->kind)) {
                        remaining.push_back(params[position++]);
                        continue;
                    }
                    auto actual = resolve(inferExpr(*arg));
                    auto expected = resolve(params[position++]);
                    if (auto* variable = std::get_if<TypeVar>(&expected->kind))
                        unifyVar(variable->id, actual);
                    else if (auto* variable = std::get_if<TypeVar>(&actual->kind))
                        unifyVar(variable->id, expected);
                    else if (!argMatchesParam(actual, expected))
                        typeMismatch(arg->location, expected, actual);
                }
            }
            while (position < params.size())
                remaining.push_back(params[position++]);
            for (auto& param : remaining) param = resolve(param);
            resultType = resolve(resultType);

            if (remaining.empty()) return resultType;
            return Type::func(std::move(remaining), resultType);
        }
        else if constexpr (std::is_same_v<T, ast::ThisExpr>) {
            // Inside a make block, `this` / `@field` has the record type.
            if (m_currentMakeType) return m_currentMakeType;
            return Type::unknown();
        }
        else {
            return Type::unknown();
        }
    }, expr.kind);
    m_typeMap[&expr] = result;
    return result;
}

auto TypeChecker::inferBinaryOp(TokenType op, const TypePtr& left, const TypePtr& right,
                                SourceLocation loc) -> TypePtr {
    // Type predicates used both in the TypeVar bail-out and the concrete section.
    auto isString = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::String;
    };
    auto isChar = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Char;
    };
    auto isNumeric = [](const TypePtr& t) -> bool {
        if (auto* prim = std::get_if<PrimitiveType>(&t->kind))
            return prim->kind == PrimitiveType::Integer;
        return std::holds_alternative<SizedIntType>(t->kind) ||
               std::holds_alternative<SizedFloatType>(t->kind);
    };

    // Resolve TypeVars through the substitution map — a prior operation in the
    // same body may have already constrained them (e.g. `n - 1` constrains n
    // to Number before `n == 0` is checked).
    auto lhs = resolve(left);
    auto rhs = resolve(right);

    {
        auto lhsIsVar = std::holds_alternative<TypeVar>(lhs->kind);
        auto rhsIsVar = std::holds_alternative<TypeVar>(rhs->kind);
        auto lhsIsUnk = std::holds_alternative<UnknownType>(lhs->kind);
        auto rhsIsUnk = std::holds_alternative<UnknownType>(rhs->kind);

        if (lhsIsVar || rhsIsVar || lhsIsUnk || rhsIsUnk) {
            auto varId = [](const TypePtr& t) -> int {
                if (auto* tv = std::get_if<TypeVar>(&t->kind)) return tv->id;
                return -1;
            };
            auto isConcrete = [](const TypePtr& t) {
                return !std::holds_alternative<TypeVar>(t->kind) &&
                       !std::holds_alternative<UnknownType>(t->kind);
            };

            switch (op) {
                // -, *, /, %, ^: both operands must be Number.
                case TokenType::Minus: case TokenType::Star:
                case TokenType::Slash: case TokenType::Percent: case TokenType::Caret: {
                    auto nc = Type::constrained("N", "Number");
                    if (int id = varId(lhs); id >= 0) unifyVar(id, nc);
                    if (int id = varId(rhs); id >= 0) unifyVar(id, nc);
                    if (isConcrete(lhs)) return lhs;
                    // A left operand that is UNKNOWN (as opposed to a type
                    // variable the substitution can still learn about) makes
                    // the right one no evidence at all: `3.meter ^ 2` is a
                    // Measure, and typing it as the exponent's Integer sent
                    // the following `.value` to Integer, where it became a
                    // bogus arity error against an unrelated prelude method.
                    if (lhsIsUnk) return Type::unknown();
                    if (isConcrete(rhs)) return rhs;
                    return resolve(lhs);
                }
                // +: String/Char on one side → constrain TypeVar to String;
                //    numeric on one side → constrain TypeVar to Number.
                case TokenType::Plus: {
                    if (isConcrete(lhs) && (isString(lhs) || isChar(lhs))) {
                        if (int id = varId(rhs); id >= 0) unifyVar(id, Type::string());
                        return Type::string();
                    }
                    if (isConcrete(rhs) && (isString(rhs) || isChar(rhs))) {
                        if (int id = varId(lhs); id >= 0) unifyVar(id, Type::string());
                        return Type::string();
                    }
                    if (isConcrete(lhs) && isNumeric(lhs)) {
                        if (int id = varId(rhs); id >= 0) unifyVar(id, Type::constrained("N", "Number"));
                        return lhs;
                    }
                    if (isConcrete(rhs) && isNumeric(rhs)) {
                        if (int id = varId(lhs); id >= 0) unifyVar(id, Type::constrained("N", "Number"));
                        return rhs;
                    }
                    // With two unconstrained operands, `a + b` still proves
                    // that both operands and the result have one shared type.
                    // Keeping them independent leaked signatures such as
                    // `T7 -> T8 -> T7` for the ordinary generic `add`.
                    if (int lhsId = varId(lhs); lhsId >= 0) {
                        if (varId(rhs) >= 0) unifyVar(varId(rhs), lhs);
                        return resolve(Type::typeVar(lhsId));
                    }
                    return lhs;
                }
                // Ordered comparisons: constrain TypeVar to the concrete side.
                case TokenType::LessThan: case TokenType::GreaterThan:
                case TokenType::LessEq: case TokenType::GreaterEq: {
                    if (int id = varId(lhs); id >= 0 && isConcrete(rhs)) unifyVar(id, rhs);
                    if (int id = varId(rhs); id >= 0 && isConcrete(lhs)) unifyVar(id, lhs);
                    return Type::boolean();
                }
                case TokenType::EqEq: case TokenType::NotEq:
                    return Type::boolean();
                case TokenType::AmpAmp: case TokenType::PipePipe: {
                    if (int id = varId(lhs); id >= 0) unifyVar(id, Type::boolean());
                    if (int id = varId(rhs); id >= 0) unifyVar(id, Type::boolean());
                    return Type::boolean();
                }
                default:
                    return lhs;
            }
        }
    }

    auto isFloat = [](const TypePtr& t) { return std::holds_alternative<SizedFloatType>(t->kind); };
    auto isIntegerLike = [](const TypePtr& t) {
        if (auto* prim = std::get_if<PrimitiveType>(&t->kind)) return prim->kind == PrimitiveType::Integer;
        return std::holds_alternative<SizedIntType>(t->kind);
    };
    auto isBool = [](const TypePtr& t) {
        auto* prim = std::get_if<PrimitiveType>(&t->kind);
        return prim && prim->kind == PrimitiveType::Bool;
    };

    bool leftIsString = isString(lhs), rightIsString = isString(rhs);
    bool leftIsFloat = isFloat(lhs), rightIsFloat = isFloat(rhs);
    bool leftIsInt = isIntegerLike(lhs), rightIsInt = isIntegerLike(rhs);

    switch (op) {
        case TokenType::Plus:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString && rightIsString)
                return Type::string();
            // String + Char, Char + String, Char + Char — all produce String
            // (String = [Char], so appending a Char or two Chars is valid concatenation).
            if ((leftIsString || isChar(lhs)) && (rightIsString || isChar(rhs)))
                return Type::string();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot add " + typeToString(lhs) + " and " + typeToString(rhs));
                return Type::string();
            }
            if (!argMatchesParam(lhs, rhs) && !argMatchesParam(rhs, lhs)) {
                error(loc, "Operator '+' requires matching types, got " +
                      typeToString(lhs) + " and " + typeToString(rhs));
            }
            return lhs;

        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
        case TokenType::Caret:
            if (leftIsInt && rightIsInt)
                return Type::integer();
            if ((leftIsFloat || rightIsFloat) && (leftIsFloat || leftIsInt) && (rightIsFloat || rightIsInt))
                return Type::float64();
            if (leftIsString || rightIsString) {
                error(loc, "Cannot use arithmetic operator on String");
                return Type::integer();
            }
            return lhs;

        case TokenType::EqEq:
        case TokenType::NotEq:
            return Type::boolean();

        case TokenType::LessThan:
        case TokenType::GreaterThan:
        case TokenType::LessEq:
        case TokenType::GreaterEq:
            if (isBool(lhs)) {
                error(loc, "Cannot compare Bool values with '<', '>', '<=', '>='");
            }
            return Type::boolean();

        case TokenType::AmpAmp:
        case TokenType::PipePipe:
            if (!isBool(lhs)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(lhs));
            }
            if (!isBool(rhs)) {
                error(loc, "Logical operator requires Bool, got " + typeToString(rhs));
            }
            return Type::boolean();

        default:
            return Type::unknown();
    }
}

// `Type.of(expr)` / `Type.returnedBy(fn)` written where a TYPE goes. The
// value-level forms answer at runtime from a recording made here; this one is
// resolved entirely at check time, because there is no value to fall back on.
// The type an expression NAMES, or null when it does not name one. A bare
// `Hello` is a type reference, not a value: it is exactly what `x.to(String)`
// already relies on. A CONSTRUCTOR is deliberately excluded — `World` is a
// value whose type is `Hello`.
auto TypeChecker::typeNameReference(const ast::Expr& expr) -> TypePtr {
    const auto* named = std::get_if<ast::UpperIdentifier>(&expr.kind);
    if (!named) return nullptr;
    if (m_nullaryConstructors.count(named->name) ||
        m_adtOfConstructor.count(named->name))
        return nullptr;
    ast::TypeName typeName;
    typeName.parts = {named->name};
    ast::TypeExpr asType;
    asType.location = expr.location;
    asType.kind = std::move(typeName);
    std::unordered_map<std::string, TypePtr> generics;
    return resolveTypeExpr(asType, generics);
}

// The single signature of the function an expression NAMES, or null when the
// expression is not a function name (or names an overload set).
auto TypeChecker::namedFunctionSignature(const ast::Expr& expr)
    -> const Signature* {
    std::string functionName;
    if (const auto* identifier = std::get_if<ast::Identifier>(&expr.kind)) {
        functionName = identifier->name;
    } else if (const auto* qualified = std::get_if<ast::MethodCall>(&expr.kind);
               qualified && qualified->args.empty() && qualified->receiver &&
               !qualified->parenthesized) {
        std::function<std::optional<std::string>(const ast::Expr&)> path;
        path = [&](const ast::Expr& receiver) -> std::optional<std::string> {
            if (auto* root = std::get_if<ast::UpperIdentifier>(&receiver.kind))
                return root->name;
            auto* segment = std::get_if<ast::MethodCall>(&receiver.kind);
            if (!segment || !segment->receiver || !segment->args.empty())
                return std::nullopt;
            auto parent = path(*segment->receiver);
            return parent ? std::optional<std::string>{*parent + "." + segment->method}
                          : std::nullopt;
        };
        if (auto module = path(*qualified->receiver))
            functionName = *module + "::" + qualified->method;
    }
    if (functionName.empty()) return nullptr;
    if (auto user = m_userSignatures.find(functionName);
        user != m_userSignatures.end() && user->second.size() == 1)
        return &user->second.front();
    m_importedSignatureCache = importedCandidateSignatures(functionName);
    if (m_importedSignatureCache.size() == 1)
        return &m_importedSignatureCache.front();
    return nullptr;
}

auto TypeChecker::resolveTypeQuery(const ast::TypeQuery& query) -> TypePtr {
    if (!query.argument) return Type::unknown();
    if (query.query == "of") {
        if (auto referenced = typeNameReference(*query.argument)) return referenced;
        return resolve(inferExpr(*query.argument));
    }

    // returnedBy: the argument NAMES a function.
    std::string functionName;
    if (const auto* identifier = std::get_if<ast::Identifier>(&query.argument->kind)) {
        functionName = identifier->name;
    } else if (const auto* qualified =
                   std::get_if<ast::MethodCall>(&query.argument->kind);
               qualified && qualified->args.empty() && qualified->receiver) {
        std::function<std::optional<std::string>(const ast::Expr&)> path;
        path = [&](const ast::Expr& receiver) -> std::optional<std::string> {
            if (auto* root = std::get_if<ast::UpperIdentifier>(&receiver.kind))
                return root->name;
            auto* segment = std::get_if<ast::MethodCall>(&receiver.kind);
            if (!segment || !segment->receiver || !segment->args.empty())
                return std::nullopt;
            auto parent = path(*segment->receiver);
            return parent ? std::optional<std::string>{*parent + "." + segment->method}
                          : std::nullopt;
        };
        if (auto module = path(*qualified->receiver))
            functionName = *module + "::" + qualified->method;
    }

    std::vector<Signature> candidates;
    if (!functionName.empty()) {
        if (auto user = m_userSignatures.find(functionName);
            user != m_userSignatures.end())
            candidates = user->second;
        if (candidates.empty())
            candidates = importedCandidateSignatures(functionName);
    }
    if (candidates.empty()) {
        // A computed ALIAS resolves before signatures are registered (the
        // signatures may themselves name the alias), so fall back to the
        // declaration's own return annotation.
        if (m_functionDeclarations.count(functionName) > 1) {
            error(query.argument->location,
                  "`Type.returnedBy` cannot choose between the overloads of `" +
                  functionName + "`");
            return Type::unknown();
        }
        if (auto declaration = m_functionDeclarations.find(functionName);
            declaration != m_functionDeclarations.end()) {
            const auto* function = declaration->second;
            if (!function->clauses.empty() &&
                function->clauses.front().returnAnnotation) {
                std::unordered_map<std::string, TypePtr> generics;
                return resolveTypeExpr(**function->clauses.front().returnAnnotation,
                                       generics);
            }
            error(query.argument->location,
                  "`Type.returnedBy` needs `" + functionName +
                  "` to declare its return type");
            return Type::unknown();
        }
        error(query.argument->location,
              "`Type.returnedBy` needs the NAME of a function; a lambda or a "
              "function value carries no signature");
        return Type::unknown();
    }
    if (candidates.size() > 1) {
        std::string message =
            "`Type.returnedBy` cannot choose between the overloads of `" +
            functionName + "`";
        for (const auto& candidate : candidates)
            message += "\n\n" + displaySignature(functionName, candidate);
        error(query.argument->location, message);
        return Type::unknown();
    }
    return candidates.front().result;
}

// The type a `let`/`var` annotation declares, after checking the initializer
// against it. Null when there is no annotation.
auto TypeChecker::declaredBindingType(const std::optional<ast::TypeExprPtr>& annotation,
                                      const TypePtr& valueType,
                                      SourceLocation loc) -> TypePtr {
    if (!annotation || !*annotation) return nullptr;
    std::unordered_map<std::string, TypePtr> generics;
    auto declared = resolveTypeExpr(**annotation, generics);
    if (!declared) return nullptr;
    auto actual = resolve(valueType);
    // A gradual initializer (`?`, a type variable) is accepted: pinning it
    // down is exactly what the annotation is for. `This` — the trait
    // placeholder — has no substitution mechanism yet and leaks out of trait
    // default methods as a literal type name, so it is treated the same way
    // (checkCall bails on it for the same reason).
    const bool unsubstitutedThis =
        std::holds_alternative<NamedType>(actual->kind) &&
        std::get<NamedType>(actual->kind).name == "This";
    if (!std::holds_alternative<UnknownType>(actual->kind) &&
        !std::holds_alternative<TypeVar>(actual->kind) && !unsubstitutedThis &&
        !argMatchesParam(actual, declared))
        typeMismatch(loc, declared, actual);
    return declared;
}

// The type a standalone `name : T` declaration gives a top-level constant,
// after checking its definition against it.
auto TypeChecker::declaredConstantType(const std::string& name,
                                       const TypePtr& valueType,
                                       SourceLocation loc) -> TypePtr {
    auto declared = m_userSignatures.find(name);
    if (declared == m_userSignatures.end() || declared->second.size() != 1)
        return nullptr;
    const auto& signature = declared->second.front();
    if (!signature.params.empty() || !signature.result) return nullptr;
    auto actual = resolve(valueType);
    const bool unsubstitutedThis =
        std::holds_alternative<NamedType>(actual->kind) &&
        std::get<NamedType>(actual->kind).name == "This";
    if (!std::holds_alternative<UnknownType>(actual->kind) &&
        !std::holds_alternative<TypeVar>(actual->kind) && !unsubstitutedThis &&
        !argMatchesParam(actual, signature.result))
        typeMismatch(loc, signature.result, actual);
    return signature.result;
}

auto TypeChecker::displayTypeOf(const ast::Expr* expr) const -> TypePtr {
    std::function<TypePtr(const TypePtr&)> widen =
        [&](const TypePtr& type) -> TypePtr {
        if (!type) return type;
        auto widenAll = [&](std::vector<TypePtr> types) {
            for (auto& element : types) element = widen(element);
            return types;
        };
        if (auto* named = std::get_if<NamedType>(&type->kind)) {
            // A parameterized name is a type application, not a nullary ADT
            // constructor occurrence.  In particular, Process<Message> must
            // not widen through the unrelated prelude constructor `Process`
            // to its owning `Feature` ADT in editor-facing types.
            if (auto owner = m_adtOfConstructor.find(named->name);
                named->typeArgs.empty() && owner != m_adtOfConstructor.end() &&
                owner->second != named->name)
                return Type::named(owner->second);
            // Type ARGUMENTS are left alone: a phantom typestate parameter is
            // spelled with constructors too (`FileHandle<Write>`), and
            // widening those to their ADT (`FileHandle<WritePermission>`)
            // erases exactly the distinction the parameter exists to make.
            return type;
        }
        if (auto* list = std::get_if<ListType>(&type->kind))
            return Type::list(widen(list->element));
        if (auto* optional = std::get_if<OptionalType>(&type->kind))
            return Type::optional(widen(optional->inner));
        if (auto* map = std::get_if<MapType>(&type->kind))
            return Type::map(widen(map->key), widen(map->value));
        if (auto* tuple = std::get_if<TupleType>(&type->kind))
            return Type::tuple(widenAll(tuple->elements));
        return type;
    };
    return widen(typeOf(expr));
}

auto TypeChecker::satisfiesTrait(const TypePtr& type,
                                 const std::string& traitName) const -> bool {
    if (m_traits.satisfies(type, traitName)) return true;
    // A nullary constructor's value type is the constructor name (`Dog`,
    // `Meter`), but the conformance is declared on the ADT that owns it
    // (`make Animal, implement: Speaker`). Lift to the owner and retry —
    // otherwise `describe(Dog)` rejects a value the trait plainly covers.
    auto* named = std::get_if<NamedType>(&type->kind);
    if (!named) return false;
    auto ownerSatisfies = [&](const std::string& ownerName) {
        return ownerName != named->name &&
               m_traits.satisfies(Type::named(ownerName), traitName);
    };
    if (auto owner = m_adtOfConstructor.find(named->name);
        owner != m_adtOfConstructor.end() && ownerSatisfies(owner->second))
        return true;
    // Constructors of a `using`-imported module are registered per module
    // rather than in the file-local ADT map.
    for (const auto& [module, constructors] : m_moduleConstructors) {
        auto constructor = constructors.find(named->name);
        if (constructor != constructors.end() &&
            moduleMemberImported(module, named->name) &&
            ownerSatisfies(constructor->second.typeName))
            return true;
    }
    return false;
}

auto TypeChecker::argMatchesParam(const TypePtr& argType, const TypePtr& paramType) const -> bool {
    auto isPermissive = [](const TypePtr& t) {
        return std::holds_alternative<UnknownType>(t->kind) || std::holds_alternative<TypeVar>(t->kind);
    };
    if (isPermissive(argType) || isPermissive(paramType)) return true;
    // Never is the bottom type — a Never-typed expression (never returns) is
    // compatible with any expected type.
    if (std::holds_alternative<VoidType>(argType->kind)) return true;
    // An intersection in expected position is a conjunction: the same actual
    // value must satisfy every member.
    if (auto* intersection =
            std::get_if<IntersectionType>(&paramType->kind)) {
        for (const auto& member : intersection->members)
            if (!argMatchesParam(argType, member)) return false;
        return true;
    }
    // An intersection value may be forgotten to any one of its components.
    if (auto* intersection = std::get_if<IntersectionType>(&argType->kind)) {
        for (const auto& member : intersection->members)
            if (argMatchesParam(member, paramType)) return true;
        return false;
    }
    // A union can flow to an expected type only when every possible member
    // can. This is the directional dual of the expected-union rule below.
    if (auto* unionType = std::get_if<UnionType>(&argType->kind)) {
        for (const auto& member : unionType->members)
            if (!argMatchesParam(member, paramType)) return false;
        return true;
    }
    // Open structural records are record-only and use width compatibility.
    // Field types are deliberately invariant in the first slice.
    if (auto* required = std::get_if<RecordType>(&paramType->kind)) {
        auto fieldMatches = [&](const auto& available) {
            for (const auto& [name, expected] : required->fields) {
                auto found = available.find(name);
                if (found == available.end() ||
                    !typesEqual(found->second, expected))
                    return false;
            }
            return true;
        };
        if (auto* named = std::get_if<NamedType>(&argType->kind)) {
            auto record =
                m_recordFields.find(resolveRecordName(named->name));
            if (record == m_recordFields.end()) return false;
            std::unordered_map<std::string, TypePtr> fields;
            for (const auto& [name, fieldType] : record->second)
                fields[name] = substituteInterfaceGenerics(
                    fieldType, named->typeArgs);
            return fieldMatches(fields);
        }
        if (auto* actual = std::get_if<RecordType>(&argType->kind)) {
            std::unordered_map<std::string, TypePtr> fields;
            for (const auto& [name, fieldType] : actual->fields)
                fields[name] = fieldType;
            return fieldMatches(fields);
        }
        return false;
    }
    if (auto* constrained = std::get_if<ConstrainedType>(&paramType->kind)) {
        return satisfiesTrait(argType, constrained->traitName);
    }
    // NamedType param that is itself a trait name: `Shape`, `Comparable`, etc.
    // Occurs when a heterogeneous list was widened to a trait element type and
    // then each element is checked against it, or when a trait-typed value is
    // passed to a ConstrainedType param that got resolved to NamedType.
    // argType matches if it implements that trait, or if argType IS that trait.
    // `make Tuple` names the whole family: a tuple's arity is part of its
    // type, so there is no single structural spelling for "any tuple" and the
    // receiver has to be matched by name. Without this a `Tuple` receiver
    // method never applied to an actual `(Integer, String)`.
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind);
        paramNamed && paramNamed->name == "Tuple" && paramNamed->typeArgs.empty())
        return std::holds_alternative<TupleType>(argType->kind);
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        if (m_traits.get(paramNamed->name)) {
            if (auto* argNamed = std::get_if<NamedType>(&argType->kind);
                argNamed && argNamed->name == paramNamed->name)
                return true; // trait-typed value matches trait param
            return satisfiesTrait(argType, paramNamed->name);
        }
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
        // `None` has no payload from which to infer an inner type, but it is
        // valid for every optional return/parameter type.
        if (auto* argNamed = std::get_if<NamedType>(&argType->kind);
            argNamed && argNamed->typeArgs.empty() && argNamed->name == "None")
            return true;
        auto* argOpt = std::get_if<OptionalType>(&argType->kind);
        return argOpt && argMatchesParam(argOpt->inner, paramOpt->inner);
    }
    if (auto* argOpt = std::get_if<OptionalType>(&argType->kind)) {
        return argMatchesParam(argOpt->inner, paramType);
    }
    auto isUnitLike = [](const TypePtr& t) -> bool {
        if (auto* p = std::get_if<PrimitiveType>(&t->kind))
            return p->kind == PrimitiveType::Unit;
        if (auto* tup = std::get_if<TupleType>(&t->kind))
            return tup->elements.empty();
        if (auto* n = std::get_if<NamedType>(&t->kind))
            return n->typeArgs.empty() && n->name == "Void";
        return false;
    };
    auto isStringType = [](const TypePtr& t) -> bool {
        if (auto* n = std::get_if<NamedType>(&t->kind))
            return n->typeArgs.empty() &&
                   (n->name == "String" || n->name == "FilePath");
        if (auto* p = std::get_if<PrimitiveType>(&t->kind))
            return p->kind == PrimitiveType::String;
        return false;
    };
    // FunctionType param — e.g. `(T-1) -> Bool` vs `(T81) -> Bool`. Without
    // this branch both sides fall to typesEqual which fails on mismatched
    // TypeVar ids even though both are permissive. Recurse into params and
    // result so lambda arguments to map/filter/each pass the checker.
    if (std::holds_alternative<FuncType>(paramType->kind)) {
        auto* argFn = std::get_if<FuncType>(&argType->kind);
        if (!argFn) return isPermissive(argType);
        // `(A) -> (B) -> C` and `(A, B) -> C` are the same callable shape
        // in Kex. Normalize both sides before comparing so a multi-parameter
        // lambda matches a curried source annotation (and vice versa).
        auto flatten = [](const TypePtr& type) {
            std::pair<std::vector<TypePtr>, TypePtr> result{{}, type};
            auto current = type;
            while (auto* fn = std::get_if<FuncType>(&current->kind)) {
                result.first.insert(
                    result.first.end(), fn->params.begin(), fn->params.end());
                current = fn->result;
            }
            result.second = current;
            return result;
        };
        auto [argParams, argResult] = flatten(argType);
        auto [paramParams, paramResult] = flatten(paramType);
        // A block may DESTRUCTURE a tuple element instead of naming it:
        // `pairs.each do |k, v|` over `[(String, Int)]` binds one element
        // across two parameters, and both backends spread it. The checker was
        // the only thing that disagreed.
        //
        // Decidable from the ARGUMENT alone, which is what makes it cheap:
        // expected-type propagation has already given the block's first
        // parameter the element type, so a 2-parameter block over
        // `[(String, Int)]` arrives here as `((String, Int), T2) -> Void`.
        // A first parameter that is a tuple of exactly as many elements as the
        // block has parameters is a destructuring block; anything else is a
        // real arity error and still reported as one.
        if (argParams.size() != paramParams.size()) {
            if (paramParams.size() != 1 || argParams.size() < 2) return false;
            if (auto* spread = std::get_if<TupleType>(&argParams[0]->kind)) {
                if (spread->elements.size() != argParams.size()) return false;
            } else if (!isPermissive(paramParams[0]) ||
                       !std::all_of(argParams.begin(), argParams.end(),
                                    [&](const TypePtr& p) {
                                        return isPermissive(p);
                                    })) {
                // Something IS known and it does not line up: a real arity
                // error, still reported. Only the case where neither side
                // knows anything is let through — `@conditions.each do |c, v|`
                // over a record field the checker has not resolved to a tuple.
                // Rejecting there would fail a program both backends run.
                return false;
            }
            if (isUnitLike(paramResult)) return true;
            return argMatchesParam(argResult, paramResult);
        }
        for (size_t i = 0; i < paramParams.size(); i++)
            // Function parameters are contravariant: a callback offered to a
            // context must accept everything the context may pass.
            if (!argMatchesParam(paramParams[i], argParams[i])) return false;
        // Void as the expected return means "result discarded" — accept any body type.
        if (isUnitLike(paramResult)) return true;
        return argMatchesParam(argResult, paramResult);
    }
    // NamedType with type args — e.g. `Range<Number>` param vs `Range<Integer>` arg.
    // Recurse into type arguments structurally so the inner types get the same
    // trait-relaxation treatment (argMatchesParam, not typesEqual).
    if (auto* paramNamed = std::get_if<NamedType>(&paramType->kind)) {
        auto* argNamed = std::get_if<NamedType>(&argType->kind);
        if (!argNamed || !namedTypesMatch(argNamed->name, paramNamed->name)) {
            // A nullary ADT constructor is a refined value of its parent
            // type. This relationship comes from the ADT registry rather
            // than from any constructor spelling.
            if (argNamed && argNamed->typeArgs.empty() &&
                paramNamed->typeArgs.empty()) {
                auto owner = m_adtOfConstructor.find(argNamed->name);
                if (owner != m_adtOfConstructor.end() &&
                    owner->second == paramNamed->name)
                    return true;
                // The same constructor/ADT relationship must survive a
                // `using` boundary. Imported nullary variants are recorded
                // per provider module rather than in m_adtOfConstructor.
                for (const auto& [module, constructors] : m_moduleConstructors) {
                    auto imported = constructors.find(argNamed->name);
                    if (imported != constructors.end() &&
                        imported->second.typeName == paramNamed->name &&
                        moduleMemberImported(module, argNamed->name))
                        return true;
                }
            }
            if (isStringType(argType) && isStringType(paramType)) return true;
            return false;
        }
        // An unparameterized receiver declaration is the erased/wildcard
        // form of the named type. For example, `make Range` methods apply to
        // inferred `Range<Integer>` and `Range<Char>` values.
        if (paramNamed->typeArgs.empty()) return true;
        if (argNamed->typeArgs.size() != paramNamed->typeArgs.size()) return false;
        for (size_t i = 0; i < paramNamed->typeArgs.size(); i++) {
            if (!argMatchesParam(argNamed->typeArgs[i], paramNamed->typeArgs[i])) return false;
        }
        return true;
    }
    // UnionType param (e.g. type alias `Level = :a | :b | :c`) — arg
    // matches if it matches any branch of the union.
    if (auto* paramUnion = std::get_if<UnionType>(&paramType->kind)) {
        for (const auto& member : paramUnion->members) {
            if (member && argMatchesParam(argType, member)) return true;
        }
        return false;
    }

    if (isUnitLike(argType) && isUnitLike(paramType)) return true;

    if (isStringType(argType) && isStringType(paramType)) return true;

    return typesEqual(argType, paramType);
}

auto TypeChecker::hasAnnotatedSignatureForReceiver(
    const std::string& name, const TypePtr& receiverType) const -> bool {
    auto it = m_annotatedReceiverKeys.find(name);
    if (it == m_annotatedReceiverKeys.end()) return false;
    return it->second.count(typeToString(receiverType)) > 0;
}

auto TypeChecker::displaySignature(const std::string& name, const Signature& sig) const -> std::string {
    // Collect TypeVar IDs in order of first appearance, then remap to
    // sequential negative IDs so they display as A, B, C, ... regardless
    // of the internal counter state when the signature was built.
    std::unordered_map<int, int> varRemap;
    int nextNeg = -1;
    std::function<void(const TypePtr&)> collectVars = [&](const TypePtr& t) {
        if (!t) return;
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeVar>) {
                if (!varRemap.count(node.id)) { varRemap[node.id] = nextNeg--; }
            } else if constexpr (std::is_same_v<T, ListType>) {
                collectVars(node.element);
            } else if constexpr (std::is_same_v<T, MapType>) {
                collectVars(node.key); collectVars(node.value);
            } else if constexpr (std::is_same_v<T, FuncType>) {
                for (const auto& p : node.params) collectVars(p);
                collectVars(node.result);
            } else if constexpr (std::is_same_v<T, TupleType>) {
                for (const auto& e : node.elements) collectVars(e);
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                collectVars(node.inner);
            } else if constexpr (std::is_same_v<T, NamedType>) {
                for (const auto& a : node.typeArgs) collectVars(a);
            }
        }, t->kind);
    };
    for (const auto& p : sig.params) collectVars(p);
    collectVars(sig.result);

    std::function<TypePtr(const TypePtr&)> remap = [&](const TypePtr& t) -> TypePtr {
        if (!t) return t;
        return std::visit([&](const auto& node) -> TypePtr {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeVar>) {
                auto it = varRemap.find(node.id);
                return it != varRemap.end() ? Type::typeVar(it->second) : t;
            } else if constexpr (std::is_same_v<T, ListType>) {
                return Type::list(remap(node.element));
            } else if constexpr (std::is_same_v<T, MapType>) {
                return Type::map(remap(node.key), remap(node.value));
            } else if constexpr (std::is_same_v<T, FuncType>) {
                std::vector<TypePtr> ps;
                for (const auto& p : node.params) ps.push_back(remap(p));
                return std::make_shared<Type>(Type{FuncType{std::move(ps), remap(node.result)}});
            } else if constexpr (std::is_same_v<T, TupleType>) {
                std::vector<TypePtr> es;
                for (const auto& e : node.elements) es.push_back(remap(e));
                return std::make_shared<Type>(Type{TupleType{std::move(es)}});
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                return Type::optional(remap(node.inner));
            } else if constexpr (std::is_same_v<T, IntersectionType>) {
                std::vector<TypePtr> members;
                for (const auto& member : node.members)
                    members.push_back(remap(member));
                return Type::intersection(std::move(members));
            } else if constexpr (std::is_same_v<T, RecordType>) {
                std::vector<std::pair<std::string, TypePtr>> fields;
                for (const auto& [name, fieldType] : node.fields)
                    fields.emplace_back(name, remap(fieldType));
                return Type::record(std::move(fields));
            } else if constexpr (std::is_same_v<T, NamedType>) {
                std::vector<TypePtr> args;
                for (const auto& a : node.typeArgs) args.push_back(remap(a));
                return Type::named(node.name, std::move(args));
            } else {
                return t;
            }
        }, t->kind);
    };

    auto displayType = [&](const TypePtr& t) -> std::string {
        // Unknown is the checker's gradual internal representation. In a
        // public callable signature it corresponds to source-level `Any`, not
        // an inference failure named "unknown".
        if (std::holds_alternative<UnknownType>(t->kind)) return "Any";
        if (auto* constrained = std::get_if<ConstrainedType>(&t->kind)) return constrained->traitName;
        return typeToString(remap(t));
    };
    std::string result = name + " : ";
    for (const auto& param : sig.params) {
        auto text = displayType(param);
        // A function-typed parameter needs parentheses of its own, or its
        // arrow merges into the signature's: `filter : [A] -> (A -> Bool) -> [A]`
        // rather than `[A] -> A -> Bool -> [A]`, which reads as three
        // parameters.
        if (param && std::holds_alternative<FuncType>(param->kind))
            text = "(" + text + ")";
        result += text + " -> ";
    }
    result += displayType(sig.result);
    return result;
}

auto TypeChecker::checkCall(const std::string& name, const std::vector<TypePtr>& argTypes,
                            SourceLocation loc, bool isMethodCall,
                            const ast::MethodCall* methodCall,
                            const ast::Expr* callExpr) -> TypePtr {
    // A field promised by an open record is known more specifically than an
    // unrelated global/UFCS method with the same name. Concrete record fields
    // already receive this priority later; structural receivers need it here
    // because they have no nominal registry entry to trigger that path.
    if (isMethodCall && argTypes.size() == 1) {
        auto structuralField = [&](const auto& self, const TypePtr& type)
            -> TypePtr {
            auto resolved = resolve(type);
            if (auto* record = std::get_if<RecordType>(&resolved->kind))
                for (const auto& [fieldName, fieldType] : record->fields)
                    if (fieldName == name) return fieldType;
            if (auto* intersection =
                    std::get_if<IntersectionType>(&resolved->kind))
                for (const auto& member : intersection->members)
                    if (auto found = self(self, member)) return found;
            return nullptr;
        };
        if (auto field = structuralField(structuralField, argTypes.front()))
            return field;
    }
    if (isMethodCall && !argTypes.empty()) {
        auto receiver = resolve(argTypes.front());
        if (auto* intersection =
                std::get_if<IntersectionType>(&receiver->kind)) {
            for (const auto& member : intersection->members) {
                std::string traitName;
                if (auto* named = std::get_if<NamedType>(&member->kind))
                    traitName = named->name;
                else if (auto* constrained =
                             std::get_if<ConstrainedType>(&member->kind))
                    traitName = constrained->traitName;
                if (traitName.empty()) continue;
                if (const TraitDef* trait = m_traits.get(traitName))
                    for (const auto& required : trait->requiredMethods)
                        if (required.name == name &&
                            argTypes.size() == required.params.size() + 1)
                            return required.result;
            }
        }
    }
    // A local function binding (`let f(x) ...` inside an expression block)
    // is represented as a FuncType variable rather than a top-level
    // FunctionDef signature. Resolve it before consulting global overloads.
    if (!isMethodCall && name.find("::") == std::string::npos) {
        auto local = lookupVar(name);
        auto resolvedLocal = local ? resolve(local) : nullptr;
        if (resolvedLocal)
            if (auto* function = std::get_if<FuncType>(&resolvedLocal->kind)) {
                if (function->params.size() != argTypes.size()) {
                    error(loc, "`" + name + "` expects " +
                        std::to_string(function->params.size()) +
                        " argument(s), got " + std::to_string(argTypes.size()));
                    return function->result;
                }
                for (size_t i = 0; i < argTypes.size(); ++i)
                    if (!argMatchesParam(argTypes[i], function->params[i]))
                        typeMismatch(loc, function->params[i], argTypes[i]);
                return function->result;
            }
    }
    const std::vector<Signature>* userSignatures = nullptr;
    if (auto scoped = m_scopedDeclaredSignatures.find(
            m_currentModulePath + "\n" + name);
        !m_currentModulePath.empty() &&
        scoped != m_scopedDeclaredSignatures.end())
        userSignatures = &scoped->second;
    else if (auto user = m_userSignatures.find(name);
             user != m_userSignatures.end())
        userSignatures = &user->second;
    bool hasUser = userSignatures != nullptr;
    auto methodIt = m_methodSignatures.find(name);
    bool hasLocalMethods = isMethodCall && methodIt != m_methodSignatures.end();
    // A known record field is not an unrelated imported zero-argument
    // method with the same spelling (`Version.patch` vs HTTP.patch).
    if (isMethodCall && argTypes.size() == 1) {
        auto receiver = resolve(argTypes.front());
        if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
            if (auto record = m_recordFields.find(
                    resolveRecordName(named->name));
                record != m_recordFields.end())
                if (auto field = record->second.find(name);
                    field != record->second.end()) {
                    // Same substitution the main field-access path does: a
                    // `Box<Integer>` reads `.items` as `[Integer]`, not as
                    // the declaration's bare parameter slot.
                    return substituteInterfaceGenerics(field->second,
                                                       named->typeArgs);
                }
            // The same rule for a record the PRELUDE declares. Interfaces
            // carry field names but not their types, so the read is answered
            // gradually — the point is only that it is a field read at all,
            // and not a call to some same-named method: `moment.time.nanosecond`
            // is the Time record's field, never units.kex's `Integer.nanosecond`.
            if (m_importedInterfaces) {
                auto record =
                    m_importedInterfaces->recordFieldNames.find(named->name);
                if (record != m_importedInterfaces->recordFieldNames.end() &&
                    record->second.count(name))
                    return Type::unknown();
            }
        }
    }
    bool hasReceiverRefinementConflict = false;
    if (hasLocalMethods && !argTypes.empty()) {
        auto actual = resolve(argTypes[0]);
        for (const auto& sig : methodIt->second) {
            if (sig.params.empty()) continue;
            auto expected = resolve(sig.params[0]);
            auto* actualNamed = std::get_if<NamedType>(&actual->kind);
            auto* expectedNamed = std::get_if<NamedType>(&expected->kind);
            if (actualNamed && expectedNamed &&
                actualNamed->name == expectedNamed->name) {
                if (!argMatchesParam(actual, expected))
                    hasReceiverRefinementConflict = true;
            }
        }
    }

    std::vector<const ImportedFunction*> importedFunctions;
    std::string qualifiedModule;
    if (m_importedInterfaces) {
        if (auto separator = name.find("::"); separator != std::string::npos) {
            qualifiedModule = name.substr(0, separator);
            auto functionName = name.substr(separator + 2);
            if (auto module = m_importedInterfaces->modules.find(qualifiedModule);
                module != m_importedInterfaces->modules.end())
                if (auto functions = module->second.exports.find(functionName);
                    functions != module->second.exports.end())
                    for (const auto& function : functions->second)
                        importedFunctions.push_back(&function);
        } else if (isMethodCall) {
            if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
                functions != m_importedInterfaces->receiverFunctions.end())
                for (const auto& function : functions->second)
                    if (importedFunctionVisible(function))
                        importedFunctions.push_back(&function);

        } else {
            for (const auto& [_, module] : m_importedInterfaces->modules) {
                if (!module.automaticImport) continue;
                if (auto functions = module.exports.find(name);
                    functions != module.exports.end())
                    for (const auto& function : functions->second)
                        importedFunctions.push_back(&function);
            }
            // Receiver functions are also callable as bare functions via
            // UFCS (e.g. `even?(x)` instead of `x.even?`).
            if (auto functions = m_importedInterfaces->receiverFunctions.find(name);
                functions != m_importedInterfaces->receiverFunctions.end())
                for (const auto& function : functions->second)
                    if (importedFunctionVisible(function))
                        importedFunctions.push_back(&function);
        }
    }
    // A module declared in the current compilation unit shadows a package
    // module with the same source identity. This matches runtime code-path
    // precedence and prevents imported ownership from hijacking local calls.
    if (!qualifiedModule.empty() && m_localModules.contains(qualifiedModule))
        importedFunctions.clear();
    if (isMethodCall && !argTypes.empty()) {
        auto actual = resolve(argTypes[0]);
        auto* actualNamed = std::get_if<NamedType>(&actual->kind);
        if (actualNamed)
            for (const auto* function : importedFunctions) {
                if (function->signature.params.empty()) continue;
                auto expected = resolve(function->signature.params[0]);
                auto* expectedNamed = std::get_if<NamedType>(&expected->kind);
                if (expectedNamed && expectedNamed->name == actualNamed->name &&
                    !argMatchesParam(actual, expected)) {
                    hasReceiverRefinementConflict = true;
                    break;
                }
            }
    }

    auto sameParams = [](const Signature& left, const Signature& right) {
        if (left.params.size() != right.params.size()) return false;
        for (size_t i = 0; i < left.params.size(); i++)
            if (!typesEqual(left.params[i], right.params[i])) return false;
        return true;
    };
    auto matchesActual = [&](const Signature& signature) {
        if (signature.params.size() != argTypes.size()) return false;
        for (size_t i = 0; i < signature.params.size(); i++)
            if (!argMatchesParam(argTypes[i], signature.params[i])) return false;
        return true;
    };
    for (size_t i = 0; i < importedFunctions.size(); i++)
        for (size_t j = i + 1; j < importedFunctions.size(); j++) {
            const auto& left = *importedFunctions[i];
            const auto& right = *importedFunctions[j];
            if (left.backendModule != right.backendModule &&
                matchesActual(left.signature) && matchesActual(right.signature) &&
                sameParams(left.signature, right.signature)) {
                error(loc, "ambiguous imported " +
                    std::string(isMethodCall && name.find("::") == std::string::npos
                        ? "receiver function '" : "function '") + name +
                    "' is provided by both '" + left.backendModule + "' and '" +
                    right.backendModule + "'");
                return Type::unknown();
            }
        }

    std::vector<Signature> importedSigs;
    for (const auto* function : importedFunctions)
        importedSigs.push_back(function->signature);

    std::vector<Signature> merged;
    const std::vector<Signature>* sigs = nullptr;
    // Calls made by ordinary user code should see the full local receiver
    // interface. While checking a make block's own implementation bodies,
    // retain the existing permissive behavior unless the receiver refinement
    // itself is wrong; generic helper calls in those bodies are not yet
    // sufficiently substituted to validate without false positives.
    const bool hasConcreteReceiver = !argTypes.empty() && [&] {
        auto receiver = resolve(argTypes.front());
        return !std::holds_alternative<TypeVar>(receiver->kind) &&
               !std::holds_alternative<UnknownType>(receiver->kind);
    }();
    bool useLocalMethods = hasLocalMethods &&
        (!m_inMakeBlock || hasReceiverRefinementConflict ||
         hasConcreteReceiver);
    // Local methods come FIRST. When a user type defines a method the prelude
    // also provides and both signatures match, the user's is the more specific
    // one. Ordering imported first let a GENERIC prelude signature win on type
    // variables alone: `Map.get :> K -> V?` matched `someMatch.get(1)` and the
    // call lowered to `kex_prelude:get`, while the 3-argument form (whose
    // prelude counterpart did not match) correctly reached the local method —
    // the same call site resolving two different ways by arity.
    // A local method whose receiver does not match still simply isn't a match.
    size_t localSigCount = 0;
    if (!importedSigs.empty() || useLocalMethods) {
        if (useLocalMethods) {
            // A module-scoped `make` is also reachable through a value whose
            // TYPE names that module: `CollisionWeb.Server` already carries
            // the qualification a `using` would supply, and requiring one
            // there would make a method unreachable from anywhere the type
            // itself is nameable.
            const auto receiverModule = [&]() -> std::string {
                if (!isMethodCall || argTypes.empty()) return {};
                auto* named = std::get_if<NamedType>(&resolve(argTypes[0])->kind);
                if (!named) return {};
                const auto dot = named->name.rfind('.');
                return dot == std::string::npos ? std::string{}
                                                : named->name.substr(0, dot);
            }();
            for (const auto& method : methodIt->second)
                if (makeModuleVisible(method.makeModule) ||
                    (!method.makeModule.empty() &&
                     method.makeModule == receiverModule))
                    merged.push_back(method);
            localSigCount = merged.size();
        }
        merged.insert(merged.end(), importedSigs.begin(), importedSigs.end());
        if (hasUser)
            merged.insert(
                merged.end(), userSignatures->begin(), userSignatures->end());
        // Every candidate may have been filtered out as out-of-scope (a
        // module-scoped `make` with no `using`). That is "no such method", not
        // "a method with no signatures" — fall through to the record-field
        // lookup and the undefined-method report below.
        if (!merged.empty()) sigs = &merged;
    } else if (hasUser) {
        sigs = userSignatures;
    }
    if (!sigs) {
        // Record field access: `user.name` desugars to checkCall("name", [User]).
        // Look up the field type in the record registry before giving up.
        // Resolve TypeVars first — the receiver may have been constrained by
        // a previous inference step.
        if (!argTypes.empty()) {
            auto receiver = resolve(argTypes[0]);
            if (auto* named = std::get_if<NamedType>(&receiver->kind)) {
                auto ri = m_recordFields.find(resolveRecordName(named->name));
                if (ri != m_recordFields.end()) {
                    auto fi = ri->second.find(name);
                    if (fi != ri->second.end()) {
                        // A field holding a function is APPLIED when arguments
                        // follow it: `route.handler(request)` is a call, and
                        // its type is the handler's result, not the handler.
                        auto fieldType = substituteInterfaceGenerics(
                            fi->second, named->typeArgs);
                        for (size_t applied = 1; applied < argTypes.size();) {
                            auto* fn = std::get_if<FuncType>(&fieldType->kind);
                            if (!fn) break;
                            applied += std::max<size_t>(fn->params.size(), 1);
                            fieldType = fn->result;
                        }
                        return fieldType;
                    }
                }
            }
            auto fieldFromStructuralType = [&](const auto& self,
                                               const TypePtr& candidate)
                -> TypePtr {
                if (auto* record = std::get_if<RecordType>(&candidate->kind)) {
                    for (const auto& [fieldName, fieldType] : record->fields)
                        if (fieldName == name) return fieldType;
                }
                if (auto* intersection =
                        std::get_if<IntersectionType>(&candidate->kind)) {
                    for (const auto& member : intersection->members)
                        if (auto found = self(self, member)) return found;
                }
                return nullptr;
            };
            if (auto fieldType =
                    fieldFromStructuralType(fieldFromStructuralType, receiver))
                return fieldType;
            // Trait-bounded receiver: `item.method()` where `item: SomeTrait`.
            // Look up the method in the trait's required methods to get return type.
            if (auto* ct = std::get_if<ConstrainedType>(&receiver->kind)) {
                if (const TraitDef* trait = m_traits.get(ct->traitName)) {
                    for (const auto& req : trait->requiredMethods) {
                        if (req.name == name) return req.result;
                    }
                }
            }
            if (auto* intersection =
                    std::get_if<IntersectionType>(&receiver->kind)) {
                for (const auto& member : intersection->members) {
                    auto* namedTrait = std::get_if<NamedType>(&member->kind);
                    if (!namedTrait) continue;
                    if (const TraitDef* trait = m_traits.get(namedTrait->name))
                        for (const auto& req : trait->requiredMethods)
                            if (req.name == name) return req.result;
                }
            }
            // If the field name is unambiguously defined on exactly one record
            // type, constrain a TypeVar receiver to that record type.
            if (std::holds_alternative<TypeVar>(receiver->kind)) {
                std::string matchedRecord;
                TypePtr matchedFieldType;
                for (const auto& [recName, fields] : m_recordFields) {
                    auto fi = fields.find(name);
                    if (fi != fields.end()) {
                        if (!matchedRecord.empty()) { matchedRecord.clear(); break; }
                        matchedRecord = recName;
                        matchedFieldType = fi->second;
                    }
                }
                if (!matchedRecord.empty()) {
                    if (auto* tv = std::get_if<TypeVar>(&receiver->kind)) {
                        unifyVar(tv->id, Type::named(matchedRecord));
                    }
                    // Applied for the same reason as the named-receiver path
                    // above: arguments after a function-valued field make the
                    // expression a call.
                    for (size_t applied = 1; applied < argTypes.size();) {
                        auto* fn = std::get_if<FuncType>(&matchedFieldType->kind);
                        if (!fn) break;
                        applied += std::max<size_t>(fn->params.size(), 1);
                        matchedFieldType = fn->result;
                    }
                    return matchedFieldType;
                }
            }
        }
        // A method call that resolved to nothing at all. It may still be
        // legitimate — a make block further down the file registers its `let`
        // methods as they are CHECKED, so a call above one is simply early —
        // so record it and decide at the end of the program, when everything
        // is registered (see reportUnknownMethods).
        // Reported only with a CONCRETE receiver type. Without one there is
        // nothing to be sure about: a module-qualified call carries no value
        // receiver at all (`Compiled.Late.answer()`), and an inferred TypeVar
        // means the checker simply does not know yet — a constructor-pattern
        // binding carries no type, and cross-file source modules are invisible
        // here. Uppercase names are namespaced CONSTRUCTORS, not methods.
        const bool concreteReceiver = !argTypes.empty() && [&] {
            const auto receiver = resolve(argTypes[0]);
            return !std::holds_alternative<TypeVar>(receiver->kind) &&
                   !std::holds_alternative<UnknownType>(receiver->kind);
        }();
        if (isMethodCall && concreteReceiver && !name.empty() &&
            name.find("::") == std::string::npos &&
            !std::isupper(static_cast<unsigned char>(name.front())))
            m_unresolvedMethods.push_back(
                {name, loc, typeToString(resolve(argTypes[0]))});
        return Type::unknown();  // unknown name, or not yet registered (forward/recursive ref)
    }

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
    // mismatch against the wrong (imported or unrelated) candidate would be
    // actively misleading, so bail out rather than guess.
    for (const auto& argType : argTypes) {
        if (auto* named = std::get_if<NamedType>(&argType->kind); named && named->name == "This") {
            return Type::unknown();
        }
    }

    // Trait-bounded receiver: `item.describe()` where `item: Describable`.
    // The trait method takes priority over imported overloads of the same name.
    if (!argTypes.empty()) {
        auto receiver = resolve(argTypes[0]);
        if (auto* ct = std::get_if<ConstrainedType>(&receiver->kind)) {
            if (const TraitDef* trait = m_traits.get(ct->traitName)) {
                for (const auto& req : trait->requiredMethods) {
                    if (req.name == name) return req.result;
                }
            }
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
        bool anyFullMatch = false;
        bool anyDroppedMatch = false;
        for (const auto& sig : *sigs) {
            if (sig.params.size() == argTypes.size()) {
                bool matches = true;
                for (size_t i = 0; i < sig.params.size(); i++)
                    if (!argMatchesParam(argTypes[i], sig.params[i])) {
                        matches = false;
                        break;
                    }
                anyFullMatch = anyFullMatch || matches;
            }
            if (sig.params.size() == argTypes.size() - 1) {
                bool matches = true;
                for (size_t i = 0; i < sig.params.size(); i++)
                    if (!argMatchesParam(argTypes[i + 1], sig.params[i])) {
                        matches = false;
                        break;
                    }
                anyDroppedMatch = anyDroppedMatch || matches;
            }
        }
        if (!anyFullMatch && anyDroppedMatch) {
            return checkCall(name, {argTypes.begin() + 1, argTypes.end()}, loc);
        }
    }

    // Name collision guard: a make-block method isn't registered here at
    // all (see checkFunctionDef), so `this.modulo(...)` inside a
    // user-defined `make CustomType do let modulo(...) ... end` would
    // otherwise get checked against an imported `modulo` signature
    // purely because the names match — a different, unrelated function.
    // Applies to any receiver type (NamedType or primitive) — if no
    // overload's first param can accept the receiver, this is a
    // make-block method call, not a known function call. For primitive
    // receivers outside of make blocks (m_currentMakeType is empty),
    // imported interfaces are authoritative and a mismatch is a real
    // type error.
    if (isMethodCall && !argTypes.empty() &&
        (std::holds_alternative<NamedType>(argTypes[0]->kind) ||
                              (!hasUser && m_currentMakeType &&
                               (std::holds_alternative<PrimitiveType>(argTypes[0]->kind) ||
                                std::holds_alternative<SizedIntType>(argTypes[0]->kind))))) {
        bool anyFirstParamPlausible = false;
        bool anyConstrainedFirstParam = false;
        // A candidate carrying an Unknown parameter is one whose real contract
        // the checker never learned — the shape-only entries the source-derived
        // interface path records for un-annotated `make` methods, where the
        // compiled artifacts would have given real types. A method with any
        // such candidate has a candidate list that cannot be treated as the
        // whole truth, so no mismatch is provable from it. TYPE VARIABLES are
        // a different thing entirely and stay provable: `[A]` is a known,
        // genuinely generic contract.
        bool anyUnknownParamCandidate = false;
        for (const auto& sig : *sigs) {
            // A signature this call could not possibly reach says nothing
            // about it. `write` is FileHandle's at 2 parameters and a
            // capability stand-in's at 3, and letting the 3-parameter one
            // count as an "unknown candidate" switched off the mismatch report
            // for the 2-argument call — writing through a read-only handle
            // stopped being an error the moment any such stand-in existed
            // (kexhq/kex#143). Same range the overload match below uses.
            const std::size_t requiredParams =
                sig.requiredParams.value_or(sig.params.size());
            if (argTypes.size() < requiredParams ||
                argTypes.size() > sig.params.size())
                continue;
            if (!sig.params.empty()) {
                if (std::holds_alternative<ConstrainedType>(sig.params[0]->kind))
                    anyConstrainedFirstParam = true;
                for (const auto& param : sig.params)
                    if (mentionsUnknownType(param)) {
                        anyUnknownParamCandidate = true;
                        break;
                    }
                if (argMatchesParam(argTypes[0], sig.params[0])) {
                    anyFirstParamPlausible = true;
                    break;
                }
            }
        }
        if (isMethodCall && anyConstrainedFirstParam && !anyFirstParamPlausible) {
            auto receiverKey = m_traits.implementorKey(argTypes[0]);
            if (!m_traits.hasConformances(receiverKey))
                anyConstrainedFirstParam = false;
        }
        // A receiver no candidate accepts is usually the guard's case above —
        // but not when the receiver type is fully known and we are in
        // ordinary code. `Date.of(...).iso` (a `Date` method on a
        // `Result<Date, TimeError>`) landed here and was waved through, which
        // is why it only failed at runtime, as `if_clause` on BEAM. Fall
        // through to the mismatch report instead.
        //
        // Inside a make block the tables are incomplete on purpose (the type
        // under check has its own methods unregistered — see checkFunctionDef),
        // and a gradual receiver genuinely isn't known, so both stay permissive.
        // `date.day` is a field read, not a call to Integer's `day`. Local
        // record layouts are known by name; an IMPORTED record's are not —
        // `recordArities` counts fields without naming them — so any field
        // access on one has to stay permissive until interfaces carry field
        // names (they exist in KexI metadata; display registration uses them).
        const bool receiverIsField = [&] {
            auto* named = std::get_if<NamedType>(&argTypes[0]->kind);
            if (!named) return false;
            if (auto record = m_recordFields.find(
                    resolveRecordName(named->name));
                record != m_recordFields.end())
                return record->second.count(name) > 0;
            return m_importedInterfaces &&
                   m_importedInterfaces->recordArities.count(named->name) > 0;
        }();
        // The receiver's own shape is what decides this, not its type
        // arguments: `Ok(b)` is `Result<Box, ?>`, and the unknown error side
        // says nothing about whether some `label` accepts a Result. A receiver
        // that is ITSELF unknown or a type variable stays permissive, and the
        // enclosing condition already restricts this to those shapes.
        // A method a trait gives the receiver's type — required or defaulted —
        // is legitimate even though no signature names that type: an
        // inherited default is registered under whichever type overrode it,
        // if any (`shout : Player -> String` while `Bot` inherits it).
        const bool traitProvides = m_traits.declaresMethod(
            m_traits.implementorKey(argTypes[0]), name);
        const bool provableMismatch =
            !m_inMakeBlock && isFullyConcrete(headShapeOf(argTypes[0])) &&
            !receiverIsField && !traitProvides && !anyUnknownParamCandidate;
        // An unreliable candidate list blocks every route to a mismatch
        // report, the trait-bound one included: `"5".to(Integer)` finds only
        // `to : Measure -> ? -> ?` from units.kex when the interfaces come
        // from source, and String is not a Measure — but String's own `to` is
        // a native builtin that never appears in these tables at all. The
        // check is confined to the case where NOTHING matched, so a call that
        // does match still gets its real result type rather than `?`.
        if (!anyFirstParamPlausible &&
            (anyUnknownParamCandidate ||
             (!anyConstrainedFirstParam && !hasReceiverRefinementConflict &&
              !provableMismatch)))
            return Type::unknown();
    }

    std::vector<const Signature*> arityMatches;
    std::vector<const Signature*> fullMatches;
    for (const auto& sig : *sigs) {
        // A trailing parameter with a default may be omitted, so the accepted
        // range is [requiredParams, params.size()] rather than one number.
        const std::size_t required =
            sig.requiredParams.value_or(sig.params.size());
        if (argTypes.size() < required || argTypes.size() > sig.params.size())
            continue;
        arityMatches.push_back(&sig);

        // Only the arguments actually PASSED are checked: an omitted
        // defaulted parameter has no argument to compare against.
        bool allMatch = true;
        for (size_t i = 0; i < argTypes.size(); i++) {
            if (!argMatchesParam(argTypes[i], sig.params[i])) {
                allMatch = false;
                break;
            }
        }
        if (allMatch) fullMatches.push_back(&sig);
    }

    // A trait receiver is a fallback for concrete implementations of the
    // same method.  Once a concrete receiver signature matches, do not let a
    // universal trait implementation win merely because another parameter in
    // its signature is more precise.  In particular, Measure.to(String) must
    // keep precedence over Showable.to(String).
    if (isMethodCall && fullMatches.size() > 1 && !argTypes.empty()) {
        const bool hasConcreteReceiverMatch = std::any_of(
            fullMatches.begin(), fullMatches.end(),
            [](const Signature* signature) {
                return !signature->params.empty() &&
                    !std::holds_alternative<ConstrainedType>(
                        signature->params.front()->kind) &&
                    !std::holds_alternative<TypeVar>(
                        signature->params.front()->kind) &&
                    !std::holds_alternative<UnknownType>(
                        signature->params.front()->kind);
            });
        if (hasConcreteReceiverMatch)
            std::erase_if(fullMatches, [](const Signature* signature) {
                return !signature->params.empty() &&
                    std::holds_alternative<ConstrainedType>(
                        signature->params.front()->kind);
            });
    }

    // 5c: Pick the most-specific full match when there are several.
    // Specificity per param: concrete named/list/func type (2) > trait-constrained (1) > TypeVar/Unknown (0).
    // A signature A dominates B if A >= B at every position and > at least one.
    // Concrete ties between explicitly overlapping union/constrained signatures
    // are ambiguous. Unknown/generic clause sets retain deterministic fallback,
    // and identical signatures remain ordinary multi-clause functions.
    if (fullMatches.size() > 1) {
        auto paramSpec = [](const TypePtr& p) -> int {
            if (std::holds_alternative<TypeVar>(p->kind) ||
                std::holds_alternative<UnknownType>(p->kind)) return 0;
            if (std::holds_alternative<ConstrainedType>(p->kind)) return 1;
            return 2;
        };
        auto dominates = [&](const Signature* a, const Signature* b) {
            bool aWins = false;
            for (size_t i = 0; i < a->params.size(); i++) {
                int sa = paramSpec(a->params[i]);
                int sb = paramSpec(b->params[i]);
                if (sb > sa) return false;
                if (sa > sb) aWins = true;
            }
            return aWins;
        };
        std::vector<const Signature*> undominated;
        for (const auto* cand : fullMatches) {
            bool dominated = false;
            for (const auto* other : fullMatches) {
                if (other == cand) continue;
                if (dominates(other, cand)) { dominated = true; break; }
            }
            if (!dominated) undominated.push_back(cand);
        }
        std::vector<const Signature*> distinct;
        for (const auto* candidate : undominated) {
            bool duplicate = std::any_of(
                distinct.begin(), distinct.end(),
                [&](const Signature* other) {
                    if (other->params.size() != candidate->params.size())
                        return false;
                    for (size_t i = 0; i < candidate->params.size(); ++i)
                        if (!typesEqual(
                                other->params[i], candidate->params[i]))
                            return false;
                    return true;
                });
            if (!duplicate) distinct.push_back(candidate);
        }
        const Signature* best =
            undominated.empty() ? fullMatches.front() : undominated.front();
        // A method call dispatches on its RECEIVER, so a candidate whose first
        // parameter is exactly the receiver's type is the one written for it.
        // Equally specific candidates otherwise fall back to declaration
        // order, which picked `factor : SIUnit -> _` for a `Measure` receiver
        // as soon as the receiver stopped being inferred as Unknown.
        if (isMethodCall && !argTypes.empty()) {
            const auto receiver = resolve(argTypes.front());
            for (const auto* candidate : undominated)
                if (!candidate->params.empty() &&
                    typesEqual(resolve(candidate->params.front()), receiver)) {
                    best = candidate;
                    break;
                }
        }
        const bool concreteArguments = std::all_of(
            argTypes.begin(), argTypes.end(),
            [&](const TypePtr& argument) {
                auto resolved = resolve(argument);
                return !std::holds_alternative<TypeVar>(resolved->kind) &&
                    !std::holds_alternative<UnknownType>(resolved->kind);
            });
        bool typedCandidates = false;
        for (size_t left = 0;
             left < distinct.size() && !typedCandidates; ++left)
            for (size_t right = left + 1;
                 right < distinct.size() && !typedCandidates; ++right)
                for (size_t i = 0;
                     i < distinct[left]->params.size(); ++i) {
                    const auto& a = distinct[left]->params[i];
                    const auto& b = distinct[right]->params[i];
                    if (typesEqual(a, b)) continue;
                    const bool explicitOverlap =
                        (std::holds_alternative<UnionType>(a->kind) &&
                         std::holds_alternative<UnionType>(b->kind)) ||
                        (std::holds_alternative<ConstrainedType>(a->kind) &&
                         std::holds_alternative<ConstrainedType>(b->kind));
                    if (explicitOverlap) {
                        typedCandidates = true;
                        break;
                    }
                }
        if (distinct.size() > 1 && concreteArguments && typedCandidates) {
            std::string message =
                "Ambiguous overload for `" + name + "`; candidates:";
            for (const auto* candidate : distinct)
                message += "\n\n" + displaySignature(name, *candidate);
            error(loc, message);
        }
        // An unconstrained shorthand receiver (for example `&.kilo`) can
        // match several concrete receiver overloads. Picking the first one
        // would permanently specialize the closure to declaration order;
        // keep its intermediate result permissive until the surrounding
        // method chain or a call site supplies a concrete receiver.
        if (distinct.size() > 1 && !concreteArguments)
            return Type::unknown();
        if (best) {
            // Rebuild fullMatches with best first so the code below uses it
            std::vector<const Signature*> reordered = {best};
            for (const auto* s : fullMatches) if (s != best) reordered.push_back(s);
            fullMatches = std::move(reordered);
        }
    }

    // When every full match is vacuous (params all Unknown/TypeVar, or only
    // trait bounds — e.g. a trait default method with no type annotation) but
    // concrete-typed arity matches exist that DIDN'T match while agreeing on
    // the receiver, prefer the concrete sigs for error reporting so real type
    // mismatches aren't masked by the generic overload.
    if (!fullMatches.empty()) {
        auto isVacuous = [](const Signature* sig) {
            for (const auto& p : sig->params)
                if (!std::holds_alternative<TypeVar>(p->kind) &&
                    !std::holds_alternative<UnknownType>(p->kind) &&
                    !std::holds_alternative<ConstrainedType>(p->kind))
                    return false;
            return true;
        };
        bool allVacuous = true;
        for (const auto* fm : fullMatches)
            if (!isVacuous(fm)) { allVacuous = false; break; }
        if (allVacuous) {
            bool hasConcrete = false;
            for (const auto* am : arityMatches) {
                if (isVacuous(am) ||
                    std::find(fullMatches.begin(), fullMatches.end(), am) != fullMatches.end())
                    continue;
                // Only a concrete sig that accepts the FIRST argument speaks
                // for this call. For a receiver call that argument is the
                // receiver, and a concrete sig for some other receiver type
                // must not mask a legitimately matching generic default (a
                // user type's trait default method, say).
                //
                // The same holds for a plain call, where the rule used to be
                // skipped: the prelude's receiver functions are all callable
                // bare through UFCS, so `merge : {A: B} -> {A: B} -> {A: B}`
                // was a "concrete candidate" for a user's own
                // `merge : Number -> Number -> Number`, cleared its full match
                // and reported `merge(4, 2)` as expecting a Map. Any prelude
                // method name a program reused as a free function with
                // trait-bounded parameters was unusable (kexhq/kex#208).
                if (!am->params.empty() && !argTypes.empty() &&
                    !argMatchesParam(argTypes[0], am->params[0]))
                    continue;
                hasConcrete = true;
                break;
            }
            if (hasConcrete) fullMatches.clear();
        }
    }

    // With no evidence about the receiver, prefer an overload whose receiver
    // is itself generic. That one is written to accept anything; an overload
    // naming a concrete receiver is a specialization for a type this call has
    // no reason to believe in. Without the preference the winner was whichever
    // sorted first, so `let show(x) = x.to(String)` answered `String` — the
    // result of units.kex's `to(measure: Measure, String)` — where the general
    // `to(value, String)` in optional.kex says `String?`.
    if (isMethodCall && fullMatches.size() > 1 && !argTypes.empty()) {
        const auto receiver = resolve(argTypes[0]);
        if (std::holds_alternative<TypeVar>(receiver->kind) ||
            std::holds_alternative<UnknownType>(receiver->kind)) {
            // A trait-bounded receiver (`Showable`) outranks a bare variable:
            // it accepts anything that satisfies the bound AND says more about
            // the result. `x.to(String)` picks `to : Showable -> String?` over
            // the untyped `to(value, t)`, whose result is only `unknown?`.
            const auto rank = [&](const Signature* candidate) {
                if (candidate->params.empty()) return 2;
                const auto parameter = resolve(candidate->params[0]);
                if (std::holds_alternative<ConstrainedType>(parameter->kind))
                    return 0;
                // A trait may reach here as a plain named type rather than a
                // constrained one; `Showable` is a bound, not a concrete type.
                if (const auto* named = std::get_if<NamedType>(&parameter->kind);
                    named && isTrait(named->name))
                    return 0;
                if (std::holds_alternative<TypeVar>(parameter->kind) ||
                    std::holds_alternative<UnknownType>(parameter->kind))
                    return 1;
                return 2;
            };
            std::stable_sort(fullMatches.begin(), fullMatches.end(),
                             [&](const Signature* left, const Signature* right) {
                                 return rank(left) < rank(right);
                             });
        }
    }

    if (fullMatches.size() >= 1) {
        const auto& matched = *fullMatches[0];
        if (std::getenv("KEX_DEBUG_SIG") && name == "to") {
            std::fprintf(stderr, "[to] params=");
            for (const auto& p : matched.params)
                std::fprintf(stderr, "%s ", typeToString(resolve(p)).c_str());
            std::fprintf(stderr, "=> %s | matches=%zu args=",
                         typeToString(resolve(matched.result)).c_str(),
                         fullMatches.size());
            for (const auto& a : argTypes)
                std::fprintf(stderr, "%s ", typeToString(resolve(a)).c_str());
            std::fprintf(stderr, "\n");
        }
        // Set when this call's receiver type is unknown AND more than one
        // provider could answer it, so nothing here may be treated as
        // evidence about the receiver — not the backend target, and not the
        // receiver's type.
        bool ambiguousReceiver = false;
        if (methodCall) {
            bool isReceiver = name.find("::") == std::string::npos;
            const ImportedFunction* resolved = nullptr;
            // `merged` is importedSigs ++ local methods ++ user functions, so
            // the winner's index says where it came from. When it came from a
            // LOCAL make-block method, the call must not be bound to an
            // imported target — doing so is how `b.get(0)` on a user record
            // with its own `get` was lowered to `kex_prelude:get/2` and died
            // at runtime, purely because the prelude also has a `get`.
            bool winnerIsLocal = false;
            bool winnerIsMakeMethod = false;
            if (sigs == &merged) {
                auto selected = static_cast<size_t>(fullMatches[0] - merged.data());
                if (selected < localSigCount) {
                    winnerIsLocal = true;
                    winnerIsMakeMethod = true;
                } else if (selected >= localSigCount &&
                    selected - localSigCount < importedFunctions.size())
                    resolved = importedFunctions[selected - localSigCount];
                else
                    winnerIsLocal = true;
            }
            if (!resolved && !winnerIsLocal) {
                int matchedArity = static_cast<int>(matched.params.size());
                for (const auto* candidate : importedFunctions) {
                    if (candidate->backendArity == matchedArity) {
                        resolved = candidate; break;
                    }
                }
            }
            // Pinning a receiver call to one provider is only sound when the
            // receiver's type is actually known. It often is not — a binding
            // from a constructor pattern (`Just(d) ->`) carries no payload
            // type yet — and then every same-arity signature "matches", so the
            // winner is whichever came first. `d.get(:year, "?")` on a Map
            // bound to `Kex.Regex:get/3` (Match's accessor) and died with
            // function_clause on BEAM, while the walker dispatched on the
            // runtime value and read the map (examples/regexes.kex).
            //
            // Leaving the call unpinned costs nothing: it lowers to the same
            // runtime-dispatching path the walker takes.
            const bool receiverUnknown = [&] {
                if (!isReceiver || argTypes.empty()) return false;
                const auto receiver = resolve(argTypes[0]);
                return std::holds_alternative<UnknownType>(receiver->kind) ||
                       std::holds_alternative<TypeVar>(receiver->kind);
            }();
            if (resolved && receiverUnknown)
                for (const auto* candidate : importedFunctions) {
                    if (candidate == resolved ||
                        candidate->backendArity != resolved->backendArity ||
                        candidate->backendModule == resolved->backendModule)
                        continue;
                    resolved = nullptr;
                    ambiguousReceiver = true;
                    break;
                }
            // Same for a LOCAL make-block method: a module flattened into this
            // unit owns the name for its own receiver type, and an unknown
            // receiver is no evidence that this call means that type. Pinning
            // it sent every `get` in a program that says `using Regex` to
            // Match's accessor.
            if (!resolved && winnerIsMakeMethod && receiverUnknown)
                for (const auto* candidate : importedFunctions)
                    if (candidate->backendArity ==
                        static_cast<int>(matched.params.size())) {
                        winnerIsMakeMethod = false;
                        ambiguousReceiver = true;
                        break;
                    }
            // Overloads that DISAGREE about what the receiver is are no
            // evidence about it either, even when they share a backend module
            // — the checks above only notice ambiguity across modules. Without
            // this, `x.to(String)` on an unconstrained `x` bound it to Measure
            // because units.kex's `to` happened to sort first, and
            // `let show(x) = x.to(String)` inferred as `Measure -> String`,
            // rejecting `show(42)`.
            if (!ambiguousReceiver && receiverUnknown && fullMatches.size() > 1) {
                std::string receiverType;
                for (const auto* candidate : fullMatches) {
                    if (candidate->params.empty()) continue;
                    auto current = typeToString(resolve(candidate->params[0]));
                    if (receiverType.empty()) {
                        receiverType = std::move(current);
                    } else if (receiverType != current) {
                        ambiguousReceiver = true;
                        break;
                    }
                }
            }
            if (resolved) {
                // The export's own flag is the whole answer: the module that
                // owns it carries no effect of its own (kexhq/kex#130).
                bool resolvedFoul = resolved->signature.isFoul;
                ResolvedCallTarget target{
                    resolved->sourceModule,
                    resolved->backendModule,
                    resolved->backendFunction,
                    resolved->backendArity,
                    isReceiver,
                    resolvedFoul,
                    false,
                    resolved->paramNames,
                };
                for (std::size_t i = 0; i < matched.params.size(); ++i) {
                    const auto resolvedParam = resolve(matched.params[i]);
                    const auto addRequiredDictionary = [&](const std::string& traitName) {
                        const bool backendExpectsDictionary =
                            resolved->backendArity >
                            static_cast<int>(matched.params.size() +
                                             target.traitDictionaries.size());
                        if (!backendExpectsDictionary) return;
                        const auto* trait = m_traits.get(traitName);
                        const bool methodIsRequired = trait && std::any_of(
                            trait->requiredMethods.begin(),
                            trait->requiredMethods.end(),
                            [&](const auto& required) {
                                return required.name == name;
                            });
                        const bool callsTraitImplementationDirectly =
                            i == 0 && isReceiver &&
                            methodIsRequired &&
                            resolved->backendFunction == name + "/" + traitName;
                        if (trait && !trait->requiredMethods.empty() &&
                            !callsTraitImplementationDirectly)
                            target.traitDictionaries.push_back({i, traitName});
                    };
                    if (const auto* constrained =
                            std::get_if<ConstrainedType>(&resolvedParam->kind))
                        addRequiredDictionary(constrained->traitName);
                    else if (const auto* named =
                                 std::get_if<NamedType>(&resolvedParam->kind))
                        addRequiredDictionary(named->name);
                }
                m_resolvedCalls[methodCall] = std::move(target);
            } else if (winnerIsMakeMethod) {
                ResolvedCallTarget target;
                target.backendFunction = name;
                target.backendArity = static_cast<int>(matched.params.size());
                target.passesReceiver = true;
                target.isFoul = matched.isFoul;
                if (!matched.params.empty()) {
                    auto receiver = resolve(matched.params.front());
                    auto* named = std::get_if<NamedType>(&receiver->kind);
                    target.isServingCast = named && named->name == "Server" &&
                        m_slotMethodNames.count(name) &&
                        typesEqual(resolve(matched.result), Type::unit());
                }
                if (target.isServingCast)
                    for (const auto& [label, value] : methodCall->namedArgs)
                        if (label == "within")
                            error(value ? value->location : methodCall->receiver->location,
                                  "`within:` is not valid on an asynchronous cast slot");
                for (const auto& param : matched.params)
                    target.localDispatchTypes.push_back(
                        typeToString(resolve(param)));
                m_resolvedCalls[methodCall] = std::move(target);
            }
        }
        // Propagate the param types back to any TypeVar arguments so that
        // unannotated params are constrained by the functions they're passed
        // into (e.g. `let f(s) = s.split(",")` constrains `s` to String).
        // Only apply when the param is concrete — skip generic params that
        // themselves contain TypeVars (e.g. `first : [A] -> A?`).
        std::function<bool(const TypePtr&)> typeContainsVar =
            [&](const TypePtr& t) {
            if (std::holds_alternative<TypeVar>(t->kind)) return true;
            if (auto* lt = std::get_if<ListType>(&t->kind))
                return typeContainsVar(lt->element);
            if (auto* ot = std::get_if<OptionalType>(&t->kind))
                return typeContainsVar(ot->inner);
            if (auto* map = std::get_if<MapType>(&t->kind))
                return typeContainsVar(map->key) ||
                       typeContainsVar(map->value);
            if (auto* tuple = std::get_if<TupleType>(&t->kind))
                return std::any_of(
                    tuple->elements.begin(), tuple->elements.end(),
                    typeContainsVar);
            if (auto* fn = std::get_if<FuncType>(&t->kind))
                return std::any_of(fn->params.begin(), fn->params.end(),
                                   typeContainsVar) ||
                       typeContainsVar(fn->result);
            if (auto* named = std::get_if<NamedType>(&t->kind))
                return std::any_of(named->typeArgs.begin(),
                                   named->typeArgs.end(), typeContainsVar);
            if (auto* intersection =
                    std::get_if<IntersectionType>(&t->kind))
                return std::any_of(intersection->members.begin(),
                                   intersection->members.end(),
                                   typeContainsVar);
            if (auto* record = std::get_if<RecordType>(&t->kind))
                return std::any_of(
                    record->fields.begin(), record->fields.end(),
                    [&](const auto& field) {
                        return typeContainsVar(field.second);
                    });
            return false;
        };
        for (size_t i = 0; i < argTypes.size() && i < matched.params.size(); i++) {
            // The receiver of an ambiguous call is exactly the argument this
            // match says nothing about: binding it to the winner's param type
            // would then TYPE it as that overload's receiver (a Map inferred
            // as Regex's `Match`), which is worse than leaving it unknown.
            if (i == 0 && ambiguousReceiver) continue;
            auto resolved = resolve(argTypes[i]);
            if (auto* tv = std::get_if<TypeVar>(&resolved->kind)) {
                const auto& param = matched.params[i];
                // Negative ids are per-signature generic placeholders, not
                // inference variables, and every signature numbers its own
                // from -1. Binding one in the global substitution made an
                // unrelated signature's `A` mean whatever the last such call
                // passed: after `args.at(i).or("")` (String), `(0..3).filter`
                // returned [String] and its block param typed as String.
                if (tv->id >= 0 && !typeContainsVar(param))
                    unifyVar(tv->id, param);
            }
        }
        // Instantiate interface-level generic placeholders in the result from
        // the actual arguments. For example,
        // `Result<X, E>.or : X -> X` called on `Result<Regex, RegexError>`
        // must return Regex; leaking the table placeholder made a following
        // overloaded call silently select a same-arity target for any type.
        std::unordered_map<int, TypePtr> genericSubst;
        std::function<void(const TypePtr&, const TypePtr&)> bindGenerics =
            [&](const TypePtr& pattern, const TypePtr& actual) {
                if (!pattern || !actual) return;
                if (auto* tv = std::get_if<TypeVar>(&pattern->kind);
                    tv && tv->id < 0) {
                    genericSubst.try_emplace(tv->id, resolve(actual));
                    return;
                }
                if (auto* constrained =
                        std::get_if<ConstrainedType>(&pattern->kind);
                    constrained && constrained->genericId < 0) {
                    genericSubst.try_emplace(constrained->genericId,
                                             resolve(actual));
                    return;
                }
                if (auto* pn = std::get_if<NamedType>(&pattern->kind)) {
                    auto* an = std::get_if<NamedType>(&actual->kind);
                    if (!an || pn->name != an->name ||
                        pn->typeArgs.size() != an->typeArgs.size()) return;
                    for (size_t i = 0; i < pn->typeArgs.size(); i++)
                        bindGenerics(pn->typeArgs[i], an->typeArgs[i]);
                } else if (auto* pl = std::get_if<ListType>(&pattern->kind)) {
                    if (auto* al = std::get_if<ListType>(&actual->kind))
                        bindGenerics(pl->element, al->element);
                } else if (auto* pm = std::get_if<MapType>(&pattern->kind)) {
                    if (auto* am = std::get_if<MapType>(&actual->kind)) {
                        bindGenerics(pm->key, am->key);
                        bindGenerics(pm->value, am->value);
                    }
                } else if (auto* po = std::get_if<OptionalType>(&pattern->kind)) {
                    if (auto* ao = std::get_if<OptionalType>(&actual->kind))
                        bindGenerics(po->inner, ao->inner);
                } else if (auto* pt = std::get_if<TupleType>(&pattern->kind)) {
                    auto* at = std::get_if<TupleType>(&actual->kind);
                    if (!at || pt->elements.size() != at->elements.size()) return;
                    for (size_t i = 0; i < pt->elements.size(); i++)
                        bindGenerics(pt->elements[i], at->elements[i]);
                } else if (auto* pf = std::get_if<FuncType>(&pattern->kind)) {
                    auto* af = std::get_if<FuncType>(&actual->kind);
                    if (!af || pf->params.size() != af->params.size()) return;
                    for (size_t i = 0; i < pf->params.size(); i++)
                        bindGenerics(pf->params[i], af->params[i]);
                    bindGenerics(pf->result, af->result);
                } else if (auto* pi =
                               std::get_if<IntersectionType>(&pattern->kind)) {
                    // Each constraint applies to the same actual value. This
                    // is what binds R in `R & { name: String }` to the full
                    // nominal argument rather than to its visible row alone.
                    for (const auto& member : pi->members)
                        bindGenerics(member, actual);
                } else if (auto* pr = std::get_if<RecordType>(&pattern->kind)) {
                    if (auto* ar = std::get_if<RecordType>(&actual->kind)) {
                        for (const auto& [name, fieldPattern] : pr->fields) {
                            auto found = std::find_if(
                                ar->fields.begin(), ar->fields.end(),
                                [&](const auto& field) {
                                    return field.first == name;
                                });
                            if (found != ar->fields.end())
                                bindGenerics(fieldPattern, found->second);
                        }
                    } else if (auto* named =
                                   std::get_if<NamedType>(&actual->kind)) {
                        auto record = m_recordFields.find(
                            resolveRecordName(named->name));
                        if (record != m_recordFields.end())
                            for (const auto& [name, fieldPattern] : pr->fields) {
                                auto found = record->second.find(name);
                                if (found != record->second.end())
                                    bindGenerics(fieldPattern, found->second);
                            }
                    }
                }
            };
        for (size_t i = 0; i < argTypes.size() && i < matched.params.size(); i++)
            bindGenerics(matched.params[i], resolve(argTypes[i]));

        // Substitute INSIDE the result too, not only when the whole result is
        // a placeholder: `[X].dropWhile : ((X) -> Bool) -> [X]` on a [String]
        // receiver returns [String]. Leaking `[X]` instead was not merely
        // vague — String IS [Char] here, so the leaked `[A]` went on to match
        // String's `at : String -> Integer -> Char?` and a list of strings
        // indexed as a list of characters. Placeholders the arguments do not
        // bind (`map : (X -> B) -> [B]`) are left exactly as they were.
        std::function<TypePtr(const TypePtr&)> applyGenerics =
            [&](const TypePtr& type) -> TypePtr {
                if (!type) return type;
                if (auto* var = std::get_if<TypeVar>(&type->kind)) {
                    if (var->id >= 0) return type;
                    auto found = genericSubst.find(var->id);
                    return found == genericSubst.end() ? type : found->second;
                }
                if (auto* list = std::get_if<ListType>(&type->kind))
                    return Type::list(applyGenerics(list->element));
                if (auto* optional = std::get_if<OptionalType>(&type->kind))
                    return Type::optional(applyGenerics(optional->inner));
                if (auto* map = std::get_if<MapType>(&type->kind))
                    return Type::map(applyGenerics(map->key),
                                     applyGenerics(map->value));
                if (auto* tuple = std::get_if<TupleType>(&type->kind)) {
                    std::vector<TypePtr> elements;
                    for (const auto& element : tuple->elements)
                        elements.push_back(applyGenerics(element));
                    return Type::tuple(std::move(elements));
                }
                if (auto* named = std::get_if<NamedType>(&type->kind)) {
                    if (named->typeArgs.empty()) return type;
                    std::vector<TypePtr> args;
                    for (const auto& arg : named->typeArgs)
                        args.push_back(applyGenerics(arg));
                    return Type::named(named->name, std::move(args));
                }
                // A function-typed parameter is where a BLOCK is passed, and
                // without this its variables were the only ones left standing:
                // `nums.map { |x| x * 2 }` reported the selected overload as
                // `[Integer] -> (A -> B) -> [Integer]`, substituting the
                // receiver and result but not the block it was actually given.
                if (auto* function = std::get_if<FuncType>(&type->kind)) {
                    std::vector<TypePtr> params;
                    for (const auto& param : function->params)
                        params.push_back(applyGenerics(param));
                    return Type::func(std::move(params),
                                      applyGenerics(function->result));
                }
                if (auto* intersection =
                        std::get_if<IntersectionType>(&type->kind)) {
                    std::vector<TypePtr> members;
                    for (const auto& member : intersection->members)
                        members.push_back(applyGenerics(member));
                    return Type::intersection(std::move(members));
                }
                if (auto* record = std::get_if<RecordType>(&type->kind)) {
                    std::vector<std::pair<std::string, TypePtr>> fields;
                    for (const auto& [name, fieldType] : record->fields)
                        fields.emplace_back(name, applyGenerics(fieldType));
                    return Type::record(std::move(fields));
                }
                return type;
            };
        auto selectedResult =
            normalizeIntersection(applyGenerics(matched.result));
        if (callExpr) {
            Signature selected = matched;
            selected.name = name;
            if (methodCall)
                if (auto target = m_resolvedCalls.find(methodCall);
                    target != m_resolvedCalls.end())
                    selected.isFoul = selected.isFoul || target->second.isFoul;
            for (auto& param : selected.params)
                param = applyGenerics(resolve(param));
            selected.result = selectedResult;
            m_selectedCallSignatures[callExpr] = std::move(selected);
        }
        return selectedResult;
    }

    if (arityMatches.empty()) {
        // A receiver whose type is not known proves nothing about arity. The
        // NamedType permissive guard above never sees these, so without this
        // an unrelated same-named method anywhere in the prelude turns every
        // gradual field read into an error: `e.value` on the `Error(e)` of
        // `Integer.parse` (a ParseError field) was reported as a bad call to
        // OptionParser's `value/2` the moment OptionParser joined the prelude.
        if (isMethodCall && !argTypes.empty()) {
            auto receiver = resolve(argTypes.front());
            if (std::holds_alternative<UnknownType>(receiver->kind) ||
                std::holds_alternative<TypeVar>(receiver->kind))
                return Type::unknown();
        }
        error(loc, "`" + name + "` expects " + std::to_string((*sigs)[0].params.size()) +
              " argument(s), got " + std::to_string(argTypes.size()));
        return (*sigs)[0].result;
    }

    // Zero matches with at least one arity match: find the first
    // mismatching argument against the first arity-matching candidate for
    // the headline message, then list every candidate signature tried,
    // Elm-style. Prefer a concrete sig that agrees on the receiver over a
    // vacuous or foreign-receiver one for error reporting.
    auto isVacuousSig = [](const Signature* sig) {
        for (const auto& p : sig->params)
            if (!std::holds_alternative<TypeVar>(p->kind) &&
                !std::holds_alternative<UnknownType>(p->kind) &&
                !std::holds_alternative<ConstrainedType>(p->kind))
                return false;
        return true;
    };
    // When NO candidate agrees on the receiver — `Result<Date, _>.iso`, where
    // every `iso` belongs to some other type — the loop below never fires and
    // the headline names whatever was registered first. That is registration
    // order, so adding an unrelated overload anywhere in the prelude silently
    // rewrote the message. Fall back to the same ordering the listing uses.
    auto signatureOrder = [](const Signature* a, const Signature* b) {
        if (a->params.size() != b->params.size())
            return a->params.size() < b->params.size();
        for (size_t i = 0; i < a->params.size(); ++i) {
            auto left = typeToString(a->params[i]);
            auto right = typeToString(b->params[i]);
            if (left != right) return left < right;
        }
        return typeToString(a->result) < typeToString(b->result);
    };
    // Among the candidates that could plausibly have been meant, headline the
    // one the call comes CLOSEST to: fewest arguments that do not fit, ties
    // broken by the listing's own order. Taking the first acceptable candidate
    // in registration order — which this used to do — put the message back at
    // the mercy of what the prelude happens to declare: adding `Set.add` made
    // `add("oops", 1)` on a user's own `add : Integer -> Integer -> Integer`
    // report "expects argument 1 to be Set<A>".
    auto mismatchCount = [&](const Signature* sig) {
        size_t count = 0;
        for (size_t i = 0; i < sig->params.size() && i < argTypes.size(); ++i)
            if (!argMatchesParam(argTypes[i], sig->params[i])) ++count;
        return count;
    };
    const Signature* firstPtr = nullptr;
    size_t fewestMismatches = 0;
    for (const auto* am : arityMatches) {
        if (isVacuousSig(am)) continue;
        if (isMethodCall && !am->params.empty() &&
            !argMatchesParam(argTypes[0], am->params[0])) continue;
        const auto mismatches = mismatchCount(am);
        if (!firstPtr || mismatches < fewestMismatches ||
            (mismatches == fewestMismatches && signatureOrder(am, firstPtr))) {
            firstPtr = am;
            fewestMismatches = mismatches;
        }
    }
    // When NO candidate qualifies, the listing's order is the only principled
    // answer left — see the comment above `signatureOrder`.
    if (!firstPtr)
        firstPtr = *std::min_element(arityMatches.begin(), arityMatches.end(),
                                     signatureOrder);
    // Same rule as the receiver-mismatch guard above, for the path a
    // PRIMITIVE receiver takes: when every candidate carries an Unknown
    // parameter, this candidate list is not the whole truth and no mismatch
    // follows from it. `"42".to(Integer)` reaches here seeing only Measure's
    // `to` from units.kex, because String's `to` is a native builtin that
    // appears in no signature table. The runtime still catches a real mistake.
    const bool unreliableCandidates =
        std::all_of(arityMatches.begin(), arityMatches.end(),
                    [](const Signature* sig) {
                        return std::any_of(sig->params.begin(), sig->params.end(),
                                           mentionsUnknownType);
                    });
    if (unreliableCandidates) return arityMatches[0]->result;

    const Signature& first = *firstPtr;
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
    // Vacuous candidates (params all TypeVar/Unknown/trait bounds — e.g. an
    // unannotated trait default method) add noise to the listing when
    // concrete candidates exist; show them only when they are all we have.
    auto sortedSigs = *sigs;
    std::stable_sort(sortedSigs.begin(), sortedSigs.end(),
        [](const Signature& a, const Signature& b) {
            if (a.params.size() != b.params.size()) return a.params.size() < b.params.size();
            for (size_t i = 0; i < a.params.size(); ++i) {
                auto as = typeToString(a.params[i]), bs = typeToString(b.params[i]);
                if (as != bs) return as < bs;
            }
            return typeToString(a.result) < typeToString(b.result);
        });
    bool anyConcreteSig = false;
    for (const auto& sig : sortedSigs)
        if (!isVacuousSig(&sig)) { anyConcreteSig = true; break; }
    // One line per DISTINCT signature. The same overload can be registered
    // more than once — from the prelude interface, from the stdlib source, and
    // again from a merged module — and listing `split : String -> [String]`
    // four times says nothing the first line did not.
    std::unordered_set<std::string> shown;
    for (const auto& sig : sortedSigs) {
        if (anyConcreteSig && isVacuousSig(&sig)) continue;
        // An un-annotated signature this call could not reach is noise, not a
        // suggestion. The stdlib ships capability stand-ins, so `write` also
        // names `Mock.Files -> Any -> Any -> Bool`, and printing that under a
        // 2-argument error told the reader to consider a method taking three
        // (kexhq/kex#143). A CONCRETE overload at another arity still earns
        // its line — `split : String -> [String]` is worth seeing even when
        // the call passed two arguments.
        const std::size_t requiredParams =
            sig.requiredParams.value_or(sig.params.size());
        const bool arityCannotMatch = argTypes.size() < requiredParams ||
                                      argTypes.size() > sig.params.size();
        if (arityCannotMatch &&
            std::any_of(sig.params.begin(), sig.params.end(),
                        mentionsUnknownType))
            continue;
        auto rendered = displaySignature(name, sig);
        if (!shown.insert(rendered).second) continue;
        message += "\n\n" + rendered;
    }
    error(loc, message);
    return arityMatches[0]->result;
}

auto TypeChecker::pushScope() -> void {
    m_scopeStack.emplace_back();
    m_importScopeStack.emplace_back();
}

auto TypeChecker::popScope() -> void {
    if (!m_scopeStack.empty()) {
        m_scopeStack.pop_back();
        m_importScopeStack.pop_back();
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

// Whether a type has any component the checker has not pinned down — a type
// variable, an unknown, or a trait constraint. Checks that would otherwise
// report a mismatch use this to stay quiet rather than invent an error from
// incomplete information.
auto containsOpenType(const TypePtr& type) -> bool {
    if (!type) return true;
    return std::visit(
        [](const auto& node) -> bool {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeVar> ||
                          std::is_same_v<T, UnknownType> ||
                          std::is_same_v<T, ConstrainedType>) {
                return true;
            } else if constexpr (std::is_same_v<T, NamedType>) {
                for (const auto& arg : node.typeArgs)
                    if (containsOpenType(arg)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, FuncType>) {
                for (const auto& param : node.params)
                    if (containsOpenType(param)) return true;
                return containsOpenType(node.result);
            } else if constexpr (std::is_same_v<T, TupleType>) {
                for (const auto& element : node.elements)
                    if (containsOpenType(element)) return true;
                return false;
            } else if constexpr (std::is_same_v<T, ListType>) {
                return containsOpenType(node.element);
            } else if constexpr (std::is_same_v<T, MapType>) {
                return containsOpenType(node.key) || containsOpenType(node.value);
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                return containsOpenType(node.inner);
            } else if constexpr (std::is_same_v<T, UnionType>) {
                for (const auto& member : node.members)
                    if (containsOpenType(member)) return true;
                return false;
            } else {
                return false;
            }
        },
        type->kind);
}

auto TypeChecker::typeMismatch(SourceLocation loc, const TypePtr& expected,
                               const TypePtr& actual) -> void {
    std::string message = "Type mismatch: expected " + typeToString(expected) +
        ", got " + typeToString(actual);
    if (const auto* never = std::get_if<VoidType>(&expected->kind);
        never && !never->reason.empty())
        message += " (" + never->reason + ")";
    error(loc, std::move(message));
}

auto TypeChecker::freshTypeVar() -> TypePtr {
    return Type::typeVar(m_nextTypeVar++);
}

auto TypeChecker::resolve(TypePtr t) const -> TypePtr {
    while (t) {
        auto* tv = std::get_if<TypeVar>(&t->kind);
        if (!tv) break;
        auto it = m_subst.find(tv->id);
        if (it == m_subst.end()) break;
        t = it->second;
    }
    return t;
}

auto TypeChecker::unifyVar(int id, TypePtr concrete) -> void {
    if (!m_subst.count(id)) m_subst[id] = std::move(concrete);
}

} // namespace kex::semantic
