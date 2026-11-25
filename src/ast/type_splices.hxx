#pragma once

#include "ast.hxx"

#include <type_traits>
#include <variant>

namespace kex::ast {

// Every type splice (`-> Measure<%{dimension}>`) in a type, outermost first
// and left to right. The order is the contract between the evaluator, which
// records each splice's value when it generates a declaration, and expansion,
// which replaces the splices of the clone it makes in the same order.
template <typename Type, typename Visit>
auto forEachTypeSplice(Type& type, Visit&& visit) -> void {
    auto inner = [&](auto& child) {
        if (child) {
            forEachTypeSplice(*child, visit);
        }
    };
    std::visit(
        [&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeSplice>) {
                visit(type);
            } else if constexpr (std::is_same_v<T, GenericType>) {
                for (auto& argument : node.args) {
                    inner(argument);
                }
            } else if constexpr (std::is_same_v<T, FunctionType>) {
                inner(node.param);
                inner(node.result);
            } else if constexpr (std::is_same_v<T, TupleType>) {
                for (auto& element : node.elements) {
                    inner(element);
                }
            } else if constexpr (std::is_same_v<T, ListType>) {
                inner(node.element);
            } else if constexpr (std::is_same_v<T, MapType>) {
                inner(node.key);
                inner(node.value);
            } else if constexpr (
                std::is_same_v<T, UnionType> || std::is_same_v<T, IntersectionType>) {
                inner(node.left);
                inner(node.right);
            } else if constexpr (std::is_same_v<T, RecordType>) {
                for (auto& field : node.fields) {
                    inner(field.second);
                }
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                inner(node.inner);
            } else if constexpr (std::is_same_v<T, BlockType>) {
                for (auto& parameter : node.params) {
                    inner(parameter);
                }
                inner(node.inner);
            }
        },
        type.kind);
}

// The splices of a function's signature: each clause's parameter types, then
// its result.
template <typename Function, typename Visit>
auto forEachSignatureTypeSplice(Function& function, Visit&& visit) -> void {
    for (auto& clause : function.clauses) {
        for (auto& param : clause.params) {
            if (param.type && *param.type) {
                forEachTypeSplice(**param.type, visit);
            }
        }
        if (clause.returnAnnotation && *clause.returnAnnotation) {
            forEachTypeSplice(**clause.returnAnnotation, visit);
        }
    }
}

} // namespace kex::ast
