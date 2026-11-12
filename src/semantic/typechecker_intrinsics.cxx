// Compile-time intrinsics (intrinsics.hxx): the registry, and the checker's
// handler for each entry.
#include "typechecker.hxx"
#include "../common/well_known.hxx"
#include "../ast/walk.hxx"
#include "../common/mangle.hxx"
#include "../common/source_name.hxx"

#include <functional>

namespace kex::semantic {

auto findCompileTimeIntrinsic(std::string_view name, std::size_t arity)
    -> const CompileTimeIntrinsic* {
    static const CompileTimeIntrinsic intrinsics[] = {
        {"Type.returnedBy", 1, &TypeChecker::checkTypeReturnedBy, nullptr},
        {"Type.of", 1, &TypeChecker::checkTypeOf, &TypeChecker::foldTypeOf},
        {"Process.spawn", 1, &TypeChecker::checkProcessSpawn, &TypeChecker::foldProcessSpawn},
        {"as", 1, &TypeChecker::checkAs, nullptr},
    };
    for (const auto& intrinsic : intrinsics)
        if (intrinsic.name == name && intrinsic.arity == arity) return &intrinsic;
    return nullptr;
}

// `Type.returnedBy(f)`: resolved entirely here — a function value carries no
// signature at runtime. The argument must NAME a function; an overloaded
// name has no single answer.
auto TypeChecker::checkTypeReturnedBy(const ast::Expr& expr,
                                      const ast::MethodCall& call) -> TypePtr {
    const auto& node = call;
    const auto* identifier =
        std::get_if<ast::Identifier>(&node.args[0]->kind);
    std::string functionName;
    if (identifier) {
        functionName = identifier->name;
    } else if (const auto* qualified =
                   std::get_if<ast::MethodCall>(&node.args[0]->kind);
               qualified && qualified->args.empty() &&
               qualified->receiver) {
        if (auto path = ast::writtenModulePath(*qualified->receiver))
            functionName = kex::mangle::qualify(*path, qualified->method);
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
            sourceName(functionName) + "`";
        for (const auto& candidate : candidates)
            message += "\n\n" + displaySignature(functionName, candidate);
        error(expr.location, message);
    } else if (auto structured =
                   structuredTypeOf(candidates.front().result)) {
        if (std::holds_alternative<ast::MethodCall>(node.args[0]->kind))
            m_decidedCalls[node.args[0].get()] = {CallKind::FunctionReference, functionName};
        m_staticTypeOfCalls[&node] =
            {std::move(*structured), /*evaluateArgument=*/false};
    } else {
        error(expr.location,
              "`Type.returnedBy` cannot answer for `" + sourceName(functionName) +
              "`: its return type is not concrete");
    }
    return Type::named(kex::names::kTypeRecord);
}

// `Type.of(Hello)` where `Hello` NAMES a type answers with that type, and
// `Type.of(f)` naming a function answers with its signature. Neither is a
// runtime value at all — the walker died on "Undefined identifier: Hello"
// while BEAM lowered it to an atom and reported `Atom`. Anything else is an
// ordinary value: null, and `foldTypeOf` records its checked type.
auto TypeChecker::checkTypeOf(const ast::Expr& expr, const ast::MethodCall& call)
    -> TypePtr {
    (void)expr;
    const auto& node = call;
    if (auto referenced = typeNameReference(*node.args[0])) {
        if (auto structured = structuredTypeOf(referenced)) {
            m_staticTypeOfCalls[&node] =
                {std::move(*structured), /*evaluateArgument=*/false};
            return Type::named(kex::names::kTypeRecord);
        }
    }
    // An imported nullary constructor is also represented by a
    // companion function. `Type.of(Kinds.Red)` asks for the
    // constructor VALUE's ADT, not that function's signature.
    const auto* constructor =
        std::get_if<ast::MethodCall>(&node.args[0]->kind);
    const bool constructorValue = constructor && !constructor->method.empty() &&
        std::isupper(static_cast<unsigned char>(constructor->method.front()));
    if (auto signature = constructorValue
                             ? nullptr
                             : namedFunctionSignature(*node.args[0])) {
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
                return Type::named(kex::names::kTypeRecord);
            }
    }
    return nullptr;
}

// `Type.of(x)`: record what the CHECKER knows about the argument. A checked
// expression knows things the value cannot carry — the unused half of a
// Result, an empty list's element type — and both backends prefer this
// recording over asking the value. A nullary constructor is widened to its
// ADT (`Red` is a `Colour`).
auto TypeChecker::foldTypeOf(const ast::Expr& expr, const ast::MethodCall& call,
                             const std::vector<TypePtr>& argTypes) -> void {
    (void)expr;
    const auto& node = call;
    // Widen constructor names to their ADT, through containers:
    // `[Red, Blue(2)]` is a `[Colour]`, not a `[Red]`. Type
    // ARGUMENTS are left alone — a phantom typestate parameter is
    // spelled with constructors too, and widening those erases the
    // distinction they exist to make.
    std::function<TypePtr(const TypePtr&)> widen =
        [&](const TypePtr& type) -> TypePtr {
        auto resolved = resolve(type);
        if (auto* named = std::get_if<NamedType>(&resolved->kind)) {
            if (const auto* owner = constructorOwner(*named);
                owner && *owner != named->name)
                return Type::named(*owner);
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
    // A type value describes itself: `Type.of(as)` for `as: Type<User>` is
    // User's type, as it was when a type name was a bare token.
    auto subject = resolve(argTypes[0]);
    if (const auto* named = std::get_if<NamedType>(&subject->kind);
        named && named->name == kex::names::kTypeRecord && named->typeArgs.size() == 1)
        subject = named->typeArgs.front();
    if (auto structured = structuredTypeOf(widen(subject)))
        m_staticTypeOfCalls[&node] = {std::move(*structured), true};
}

} // namespace kex::semantic

namespace kex::semantic {

// `Process.spawn(Entries)`: the record's NAME, not a value of it. A bare type
// name is typed as the type it names (that is what lets `x.to(Integer)`
// dispatch), so this checked as a clean `Server<Entries>` and the server then
// started with the atom `Entries` as its state, dying on the first `@field`.
// Spawning takes the initial state, so say how to write one; the call is then
// typed by its signature as any other.
auto TypeChecker::checkProcessSpawn(const ast::Expr& expr, const ast::MethodCall& call)
    -> TypePtr {
    (void)expr;
    const auto* arg = call.args.front()
        ? std::get_if<ast::UpperIdentifier>(&call.args.front()->kind) : nullptr;
    if (arg && !lookupVar(arg->name) && !m_nullaryConstructors.contains(arg->name) &&
        m_recordFields.count(resolveRecordName(arg->name)))
        error(call.args.front()->location,
              "`Process.spawn(" + arg->name + ")` passes the type, not a value; spawn a "
              "server with its initial state: `Process.spawn(" + arg->name + " {})`");
    return nullptr;
}

// `Process.spawn(state)`: the server's slots live with the `serving` block
// of the state's type, so lowering needs that type to start it. Recorded as
// the call's decision, the owner empty while the state's type is not known
// (a server with no slots).
auto TypeChecker::foldProcessSpawn(const ast::Expr& expr,
                                   const ast::MethodCall& call,
                                   const std::vector<TypePtr>& argTypes) -> void {
    (void)call;
    if (argTypes.size() != 1) return;
    const auto state = resolve(argTypes.front());
    m_decidedCalls[&expr] = {CallKind::Builtin,
                             isUndetermined(state) ? std::string{} : typeToString(state)};
}

} // namespace kex::semantic

namespace kex::semantic {

// `value.as(Target)` is a total, compile-time checked conversion.
// Distinct types erase to their backing representation, so a
// compatible retag changes only the static type. String is the
// universal display target and is lowered to the existing total
// display conversion by both backends.
// A compile-time intrinsic (intrinsics.hxx): an ordinary signature could
// type it, but not refuse an incompatible representation.
auto TypeChecker::checkAs(const ast::Expr& expr, const ast::MethodCall& node) -> TypePtr {
    if (!node.args[0] || !node.namedArgs.empty() || node.block) return nullptr;
    std::unordered_map<std::string, TypePtr> targetGenerics;
    auto target = node.targetType
        ? resolveTypeExpr(*node.targetType, targetGenerics)
        : typeNameReference(*node.args[0]);
    if (!target) {
        error(expr.location,
              "`.as` needs a type name as its target");
        return Type::unknown();
    }
    m_decidedCalls[&expr] = {CallKind::Conversion,
                             typeToString(target)};
    // `"hello".as(Atom)` is the atom `:hello`, made at compile time —
    // so only from text the compiler can see. Atoms are never freed;
    // run-time text goes through `Atom.from` (makes one) or
    // `.to(Atom)` (finds an existing one), both explicit about it.
    if (typeToString(target) == "Atom") {
        const auto* literal = node.receiver
            ? std::get_if<ast::StringLiteral>(&node.receiver->kind)
            : nullptr;
        if (!literal || !literal->values.empty())
            error(expr.location,
                  "`.as(Atom)` needs a string literal; for text known "
                  "only at run time use `Atom.from(text)`, or "
                  "`text.to(Atom)` to find an existing atom");
        return target;
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

} // namespace kex::semantic
