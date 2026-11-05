// Compile-time intrinsics (intrinsics.hxx): the registry, and the checker's
// handler for each entry.
#include "typechecker.hxx"
#include "typechecker_internal.hxx"
#include "../ast/walk.hxx"
#include "../common/mangle.hxx"
#include "../common/source_name.hxx"

#include <functional>

namespace kex::semantic {

auto findCompileTimeIntrinsic(std::string_view name, std::size_t arity)
    -> const CompileTimeIntrinsic* {
    static const CompileTimeIntrinsic intrinsics[] = {
        {"Type::returnedBy", 1, &TypeChecker::checkTypeReturnedBy, nullptr},
        {"Type::of", 1, &TypeChecker::checkTypeOf, &TypeChecker::foldTypeOf},
        {"Process::spawn", 1, nullptr, &TypeChecker::foldProcessSpawn},
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
        m_staticTypeOfCalls[&node] =
            {std::move(*structured), /*evaluateArgument=*/false};
    } else {
        error(expr.location,
              "`Type.returnedBy` cannot answer for `" + sourceName(functionName) +
              "`: its return type is not concrete");
    }
    return Type::named("Type");
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
            return Type::named("Type");
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
                return Type::named("Type");
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
    if (auto structured = structuredTypeOf(widen(argTypes[0])))
        m_staticTypeOfCalls[&node] = {std::move(*structured), true};
}

} // namespace kex::semantic

namespace kex::semantic {

// `Process.spawn(state)`: the server's slots live with the `serving` block
// of the state's type, so lowering needs that type to start it. Recorded as
// the call's decision; lowering used to read it back off the argument.
auto TypeChecker::foldProcessSpawn(const ast::Expr& expr,
                                   const ast::MethodCall& call,
                                   const std::vector<TypePtr>& argTypes) -> void {
    (void)call;
    if (argTypes.size() != 1) return;
    const auto state = resolve(argTypes.front());
    if (std::holds_alternative<UnknownType>(state->kind) ||
        std::holds_alternative<TypeVar>(state->kind))
        return;
    m_decidedCalls[&expr] = {CallKind::Builtin, typeToString(state)};
}

} // namespace kex::semantic
