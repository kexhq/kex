#include "reify.hxx"

#include <algorithm>
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
            } else if constexpr (std::is_same_v<T, interpreter::RecordValue>) {
                ast::RecordConstruction record;
                record.typeName = node.typeName;
                // Sorted to keep the emitted AST stable: RecordValue::fields
                // is an unordered_map, and an unstable order would make
                // identical builds emit different code.
                //
                // TODO — emit DECLARATION order instead, and drop the sort.
                // Lowering's main path is name-based (lowerRecordConstruction
                // walks the declared fields and looks each name up), so the
                // written order is normally irrelevant. But its `records.find`
                // MISS path falls back to "fields as written", positionally —
                // and there the order here would BE the tuple layout, so a
                // rename that moves a field alphabetically would silently
                // change the ABI. No reachable case was found (even prelude
                // records like ParseError, whose declared order differs from
                // alphabetical in 4 of 5 slots, are registered and take the
                // safe path), but relying on that fallback being unreachable
                // is the wrong guarantee. Emitting declaration order makes
                // written order and layout coincide, so the fallback stops
                // mattering. Needs record layouts passed in to valueToLiteral.
                std::vector<const std::string*> names;
                names.reserve(node.fields.size());
                for (const auto& [field, _] : node.fields) names.push_back(&field);
                std::sort(names.begin(), names.end(),
                          [](const std::string* a, const std::string* b) {
                              return *a < *b;
                          });
                for (const auto* field : names) {
                    auto lowered =
                        valueToLiteral(node.fields.at(*field), location, error);
                    if (!lowered) return nullptr;
                    record.fields.emplace_back(*field, std::move(lowered));
                }
                out->kind = std::move(record);
            } else if constexpr (std::is_same_v<T, interpreter::VariantValue>) {
                // A nullary variant is just its name (`None`); one with a
                // payload is a constructor call (`Just(3)`).
                if (node.args.empty()) {
                    out->kind = ast::UpperIdentifier{node.tag};
                } else {
                    ast::FunctionCall call;
                    call.name = node.tag;
                    for (const auto& arg : node.args) {
                        auto lowered = valueToLiteral(arg, location, error);
                        if (!lowered) return nullptr;
                        call.args.push_back(std::move(lowered));
                    }
                    out->kind = std::move(call);
                }
            } else {
                error = "a " + value->typeName() + " has no literal form";
                return nullptr;
            }
            return out;
        },
        value->data);
}

} // namespace kex::compiled
