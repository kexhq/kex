#pragma once

#include "../ast/ast.hxx"
#include "types.hxx"

#include <cctype>
#include <string>
#include <variant>

namespace kex::semantic {

// A declared type — a record field's, a constructor argument's — in the shape
// `Type.of` hands back to Kex code, read from the source as written. Both
// backends answer `Type.fields` and `Type.constructors` from it, so it needs
// no checking pass (it holds under `--no-check`, and for a type compiled in
// another unit). Names follow `structuredTypeOf`: `T?` is `Option`, every
// sized integer is `Integer`, every sized float `Float`, and a function is
// `Function` with its parameters followed by its result. A type variable
// keeps its own name.
inline auto structuredTypeOfDeclared(const ast::TypeExpr& type) -> StructuredType {
    const auto of = [](const ast::TypeExprPtr& inner) {
        return inner ? structuredTypeOfDeclared(*inner) : StructuredType{"Any", {}};
    };
    const auto named = [](const ast::TypeName& name) {
        std::string joined;
        for (const auto& part : name.parts) {
            if (!joined.empty()) joined += '.';
            joined += part;
        }
        if (joined == "Int" || joined == "Byte" ||
            (joined.size() > 3 && (joined.rfind("Int", 0) == 0 ||
                                   joined.rfind("UInt", 0) == 0) &&
             std::isdigit(static_cast<unsigned char>(joined.back()))))
            return std::string("Integer");
        if (joined == "Float32" || joined == "Float64") return std::string("Float");
        if (joined == "Optional") return std::string("Option");
        return joined;
    };
    return std::visit([&](const auto& t) -> StructuredType {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, ast::TypeName>) {
            return {named(t), {}};
        } else if constexpr (std::is_same_v<T, ast::GenericType>) {
            StructuredType result{named(t.name), {}};
            for (const auto& arg : t.args) result.args.push_back(of(arg));
            return result;
        } else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            // `A -> B -> C` nests to the right; flatten it as a signature
            // reads, unless the result was written in its own parentheses.
            StructuredType result{"Function", {of(t.param)}};
            const ast::TypeExpr* rest = t.result.get();
            while (rest) {
                const auto* next = std::get_if<ast::FunctionType>(&rest->kind);
                if (!next || rest->parenthesized) break;
                result.args.push_back(of(next->param));
                rest = next->result.get();
            }
            result.args.push_back(rest ? structuredTypeOfDeclared(*rest)
                                       : StructuredType{"Any", {}});
            return result;
        } else if constexpr (std::is_same_v<T, ast::TupleType>) {
            StructuredType result{"Tuple", {}};
            for (const auto& element : t.elements) result.args.push_back(of(element));
            return result;
        } else if constexpr (std::is_same_v<T, ast::ListType>) {
            return {"List", {of(t.element)}};
        } else if constexpr (std::is_same_v<T, ast::MapType>) {
            return {"Map", {of(t.key), of(t.value)}};
        } else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return {"Option", {of(t.inner)}};
        } else if constexpr (std::is_same_v<T, ast::BlockType>) {
            return {"Function", {of(t.inner)}};
        } else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return {"Atom", {}};
        } else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            return {t.name, {}};
        } else if constexpr (std::is_same_v<T, ast::UnionType>) {
            // A union of atom literals (`:low | :high`) is an Atom at runtime;
            // any other union has no single name.
            const auto left = of(t.left);
            const auto right = of(t.right);
            if (left.name == "Atom" && right.name == "Atom") return left;
            return {"Any", {}};
        } else {
            return {"Any", {}};
        }
    }, type.kind);
}

// Whether a declared type admits `None`: written `T?` or `Optional<T>`.
inline auto isOptionalDeclared(const ast::TypeExpr& type) -> bool {
    return structuredTypeOfDeclared(type).name == "Option";
}

} // namespace kex::semantic
