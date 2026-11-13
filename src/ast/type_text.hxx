#pragma once

#include "ast.hxx"
#include <string>
#include <variant>

// A type expression written back out as the Kex source that declares it. One
// printer for every reader that shows a declared type to a person — the
// collector's hover text, `--summary`, and the API help — so the three cannot
// drift into spelling the same type three ways.
namespace kex::ast {

inline auto typeExprText(const TypeExpr& type) -> std::string;

inline auto typeNameText(const TypeName& name) -> std::string {
    std::string result;
    for (const auto& part : name.parts) {
        if (!result.empty()) result += ".";
        result += part;
    }
    return result;
}

inline auto typeExprText(const TypeExpr& type) -> std::string {
    auto text = std::visit([](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, TypeName>) {
            return typeNameText(node);
        } else if constexpr (std::is_same_v<T, GenericType>) {
            std::string result = typeNameText(node.name) + "<";
            for (size_t i = 0; i < node.args.size(); ++i) {
                if (i) result += ", ";
                result += node.args[i] ? typeExprText(*node.args[i]) : "?";
            }
            return result + ">";
        } else if constexpr (std::is_same_v<T, FunctionType>) {
            return (node.param ? typeExprText(*node.param) : "?") + " -> " +
                   (node.result ? typeExprText(*node.result) : "?");
        } else if constexpr (std::is_same_v<T, TupleType>) {
            std::string result = "(";
            for (size_t i = 0; i < node.elements.size(); ++i) {
                if (i) result += ", ";
                result += node.elements[i] ? typeExprText(*node.elements[i]) : "?";
            }
            return result + ")";
        } else if constexpr (std::is_same_v<T, ListType>) {
            return "[" + (node.element ? typeExprText(*node.element) : "?") + "]";
        } else if constexpr (std::is_same_v<T, MapType>) {
            return "Map<" + (node.key ? typeExprText(*node.key) : "?") + ", " +
                   (node.value ? typeExprText(*node.value) : "?") + ">";
        } else if constexpr (std::is_same_v<T, UnionType>) {
            return (node.left ? typeExprText(*node.left) : "?") + " | " +
                   (node.right ? typeExprText(*node.right) : "?");
        } else if constexpr (std::is_same_v<T, IntersectionType>) {
            return (node.left ? typeExprText(*node.left) : "?") + " & " +
                   (node.right ? typeExprText(*node.right) : "?");
        } else if constexpr (std::is_same_v<T, RecordType>) {
            std::string result = "{";
            for (size_t i = 0; i < node.fields.size(); i++) {
                if (i) result += ", ";
                result += node.fields[i].first + ": " +
                    (node.fields[i].second
                         ? typeExprText(*node.fields[i].second) : "?");
            }
            return result + "}";
        } else if constexpr (std::is_same_v<T, OptionalType>) {
            return (node.inner ? typeExprText(*node.inner) : "?") + "?";
        } else if constexpr (std::is_same_v<T, BlockType>) {
            std::string result = "Block<";
            for (const auto& param : node.params)
                result += (param ? typeExprText(*param) : "?") + ", ";
            return result + (node.inner ? typeExprText(*node.inner) : "?") + ">";
        } else if constexpr (std::is_same_v<T, AtomType>) {
            return ":" + node.name;
        } else if constexpr (std::is_same_v<T, GenericVar>) {
            return node.name;
        }
        return "?";
    }, type.kind);
    // Grouping parens change what a type means wherever an operator binds
    // looser than the one around it: `(X -> Bool) -> X?` takes a predicate
    // where `X -> Bool -> X?` takes two arguments, and `(A | B) -> C` takes
    // either where `A | B -> C` is an `A` or a function. Around a name, a list
    // or anything else that cannot come apart they were decoration, and are
    // not written back.
    const bool grouped = type.parenthesized &&
                         (std::holds_alternative<FunctionType>(type.kind) ||
                          std::holds_alternative<UnionType>(type.kind) ||
                          std::holds_alternative<IntersectionType>(type.kind));
    return grouped ? "(" + text + ")" : text;
}

// One variant of a sum type: `Just(X)`, where `typeExprText` alone would
// print the applied-generic spelling `Just<X>`.
inline auto variantText(const TypeExpr& variant) -> std::string {
    if (const auto* generic = std::get_if<GenericType>(&variant.kind)) {
        std::string result = typeNameText(generic->name) + "(";
        for (size_t i = 0; i < generic->args.size(); ++i) {
            if (i) result += ", ";
            result += generic->args[i] ? typeExprText(*generic->args[i]) : "?";
        }
        return result + ")";
    }
    return typeExprText(variant);
}

} // namespace kex::ast
