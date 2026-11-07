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
    return std::visit([&](const auto& node) -> std::string {
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
}

} // namespace kex::ast
