#include "reify.hxx"

#include <variant>

namespace kex::compiled {

using interpreter::ValuePtr;

namespace {

auto make(const SourceLocation& location) -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = location;
    return expr;
}

} // namespace

auto valueToLiteral(const ValuePtr& value,
                    const SourceLocation& location,
                    std::string& error) -> ast::ExprPtr {
    if (!value) {
        error = "no value";
        return nullptr;
    }
    return std::visit(
        [&](const auto& node) -> ast::ExprPtr {
            using T = std::decay_t<decltype(node)>;
            auto out = make(location);
            if constexpr (std::is_same_v<T, interpreter::UnitValue>) {
                out->kind = ast::TupleExpr{};  // `()`
            } else if constexpr (std::is_same_v<T, interpreter::IntValue>) {
                out->kind = ast::IntLiteral{std::to_string(node.value)};
            } else if constexpr (std::is_same_v<T, interpreter::BigIntValue>) {
                // Through decimal text, so arbitrary precision survives —
                // never via int64_t.
                out->kind = ast::IntLiteral{node.value.get_str()};
            } else if constexpr (std::is_same_v<T, interpreter::FloatValue>) {
                // to_string would clamp to 6 decimals; round-trip precisely.
                std::string text = std::to_string(node.value);
                {
                    char buffer[40];
                    std::snprintf(buffer, sizeof(buffer), "%.17g", node.value);
                    text = buffer;
                }
                // A Float literal must still LOOK like one after reification.
                if (text.find('.') == std::string::npos &&
                    text.find('e') == std::string::npos &&
                    text.find("inf") == std::string::npos &&
                    text.find("nan") == std::string::npos)
                    text += ".0";
                out->kind = ast::FloatLiteral{text};
            } else if constexpr (std::is_same_v<T, interpreter::StringValue>) {
                // interpolating=false: the text is already the final value, so
                // a `${...}` inside it must NOT be re-expanded at runtime.
                ast::StringLiteral literal;
                literal.value = node.value;
                literal.interpolating = false;
                out->kind = std::move(literal);
            } else if constexpr (std::is_same_v<T, interpreter::CharValue>) {
                out->kind = ast::CharLiteral{node.value};
            } else if constexpr (std::is_same_v<T, interpreter::BoolValue>) {
                out->kind = ast::BoolLiteral{node.value};
            } else if constexpr (std::is_same_v<T, interpreter::AtomValue>) {
                out->kind = ast::AtomLiteral{node.name};
            } else if constexpr (std::is_same_v<T, interpreter::ListValue>) {
                ast::ListExpr list;
                for (const auto& element : node.elements) {
                    auto lowered = valueToLiteral(element, location, error);
                    if (!lowered) return nullptr;
                    list.elements.push_back(std::move(lowered));
                }
                out->kind = std::move(list);
            } else if constexpr (std::is_same_v<T, interpreter::TupleValue>) {
                ast::TupleExpr tuple;
                for (const auto& element : node.elements) {
                    auto lowered = valueToLiteral(element, location, error);
                    if (!lowered) return nullptr;
                    tuple.elements.push_back(std::move(lowered));
                }
                out->kind = std::move(tuple);
            } else if constexpr (std::is_same_v<T, interpreter::MapValue>) {
                ast::MapExpr map;
                for (const auto& [key, mapped] : node.entries) {
                    auto loweredKey = valueToLiteral(key, location, error);
                    if (!loweredKey) return nullptr;
                    auto loweredValue = valueToLiteral(mapped, location, error);
                    if (!loweredValue) return nullptr;
                    map.entries.push_back(
                        ast::MapEntry{std::move(loweredKey), std::move(loweredValue)});
                }
                out->kind = std::move(map);
            } else {
                error = "a " + value->typeName() + " has no literal form";
                return nullptr;
            }
            return out;
        },
        value->data);
}

} // namespace kex::compiled
