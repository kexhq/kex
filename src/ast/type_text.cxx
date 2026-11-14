#include "type_text.hxx"

#include <type_traits>
#include <variant>

namespace kex::ast {

auto typeText(const TypeExpr& type, TypeSpelling spelling) -> std::string {
    const bool dispatch = spelling == TypeSpelling::Dispatch;
    const std::string missing = dispatch ? "Any" : "?";
    const auto text = [&](const TypeExprPtr& inner) {
        return inner ? typeText(*inner, spelling) : missing;
    };
    const auto name = [&](const TypeName& written) {
        if (!dispatch) return written.dotted();
        return written.parts.empty() ? std::string("Any") : written.parts.back();
    };
    const auto joined = [&](const std::vector<TypeExprPtr>& types) {
        std::string out;
        for (std::size_t i = 0; i < types.size(); i++)
            out += (i ? ", " : "") + text(types[i]);
        return out;
    };
    const auto written = std::visit([&](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, TypeName>) {
            return name(node);
        } else if constexpr (std::is_same_v<T, GenericType>) {
            return name(node.name) + "<" + joined(node.args) + ">";
        } else if constexpr (std::is_same_v<T, FunctionType>) {
            return dispatch ? "(" + text(node.param) + ") -> " + text(node.result)
                            : text(node.param) + " -> " + text(node.result);
        } else if constexpr (std::is_same_v<T, TupleType>) {
            return "(" + joined(node.elements) + ")";
        } else if constexpr (std::is_same_v<T, ListType>) {
            const auto element = text(node.element);
            return dispatch && element == "Char" ? "String" : "[" + element + "]";
        } else if constexpr (std::is_same_v<T, MapType>) {
            return dispatch ? "{" + text(node.key) + ": " + text(node.value) + "}"
                            : "Map<" + text(node.key) + ", " + text(node.value) + ">";
        } else if constexpr (std::is_same_v<T, UnionType>) {
            return text(node.left) + " | " + text(node.right);
        } else if constexpr (std::is_same_v<T, IntersectionType>) {
            return text(node.left) + " & " + text(node.right);
        } else if constexpr (std::is_same_v<T, RecordType>) {
            std::string out = "{";
            for (std::size_t i = 0; i < node.fields.size(); i++)
                out += (i ? ", " : "") + node.fields[i].first + ": " +
                       text(node.fields[i].second);
            return out + "}";
        } else if constexpr (std::is_same_v<T, OptionalType>) {
            return text(node.inner) + "?";
        } else if constexpr (std::is_same_v<T, BlockType>) {
            std::string out = "Block<";
            for (const auto& param : node.params) out += text(param) + ", ";
            return out + text(node.inner) + ">";
        } else if constexpr (std::is_same_v<T, AtomType>) {
            return dispatch ? "Atom" : ":" + node.name;
        } else if constexpr (std::is_same_v<T, GenericVar>) {
            return node.name;
        }
        return missing;
    }, type.kind);
    // Grouping parens change what a type means wherever an operator binds
    // looser than the one around it: `(X -> Bool) -> X?` takes a predicate
    // where `X -> Bool -> X?` takes two arguments, and `(A | B) -> C` takes
    // either where `A | B -> C` is an `A` or a function. Around a name, a list
    // or anything else that cannot come apart they were decoration, and are
    // not written back.
    const bool grouped = !dispatch && type.parenthesized &&
                         (std::holds_alternative<FunctionType>(type.kind) ||
                          std::holds_alternative<UnionType>(type.kind) ||
                          std::holds_alternative<IntersectionType>(type.kind));
    return grouped ? "(" + written + ")" : written;
}

} // namespace kex::ast

namespace kex::ast {

auto dispatchNameOf(const TypeExprPtr& type) -> std::string {
    if (!type) return "";
    if (auto* name = std::get_if<TypeName>(&type->kind))
        if (!name->parts.empty()) return name->parts.back();
    if (auto* generic = std::get_if<GenericType>(&type->kind))
        if (!generic->name.parts.empty()) return generic->name.parts.back();
    // List/Map types get canonical names so list-vs-map methods (the
    // polymorphic HOFs) collide and dispatch by runtime type (is_list/
    // is_map) instead of falling into separate un-mangled `map/2`s.
    if (std::holds_alternative<ListType>(type->kind)) return "List";
    if (std::holds_alternative<MapType>(type->kind)) return "Map";
    return "";
}

} // namespace kex::ast
