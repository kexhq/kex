#include "reify.hxx"

#include "../ast/clone.hxx"

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
                    std::string& error,
                    const ReifyContext& context) -> ast::ExprPtr {
    if (!value) {
        error = "no value";
        return nullptr;
    }
    return std::visit(
        [&](const auto& node) -> ast::ExprPtr {
            using T = std::decay_t<decltype(node)>;
            auto out = make(location);
            if constexpr (std::is_same_v<T, interpreter::PlaceholderValue>) {
                // A placeholder reifies to the EXPRESSION it stands for, not
                // to a value — that is what lets a collapsed builder keep a
                // runtime reference in the middle of an otherwise-literal
                // result.
                if (!context.placeholders ||
                    node.index >= context.placeholders->size() ||
                    !(*context.placeholders)[node.index]) {
                    error = "`" + node.name +
                            "` is not known at compile time and has no "
                            "expression to fall back on";
                    return nullptr;
                }
                // `clone` takes the owning pointer; borrow the node into one and
                // release it again so the original AST keeps its owner.
                auto borrowed = ast::ExprPtr(
                    const_cast<ast::Expr*>((*context.placeholders)[node.index]));
                auto copy = ast::clone(borrowed);
                (void)borrowed.release();
                return copy;
            } else
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
                    auto lowered = valueToLiteral(element, location, error, context);
                    if (!lowered) return nullptr;
                    list.elements.push_back(std::move(lowered));
                }
                out->kind = std::move(list);
            } else if constexpr (std::is_same_v<T, interpreter::TupleValue>) {
                ast::TupleExpr tuple;
                for (const auto& element : node.elements) {
                    auto lowered = valueToLiteral(element, location, error, context);
                    if (!lowered) return nullptr;
                    tuple.elements.push_back(std::move(lowered));
                }
                out->kind = std::move(tuple);
            } else if constexpr (std::is_same_v<T, interpreter::MapValue>) {
                ast::MapExpr map;
                for (const auto& [key, mapped] : node.entries) {
                    auto loweredKey = valueToLiteral(key, location, error, context);
                    if (!loweredKey) return nullptr;
                    auto loweredValue = valueToLiteral(mapped, location, error, context);
                    if (!loweredValue) return nullptr;
                    map.entries.push_back(
                        ast::MapEntry{std::move(loweredKey), std::move(loweredValue)});
                }
                out->kind = std::move(map);
            } else if constexpr (std::is_same_v<T, interpreter::RecordValue>) {
                ast::RecordConstruction record;
                // The value carries the module-qualified identity
                // (`Boxes.Box`); the layout table is keyed by the name as
                // declared. Fall back to the last segment so the reified
                // literal gets DECLARATION order rather than the alphabetical
                // unknown-type tail.
                record.typeName = node.typeName;
                if (context.layouts && !context.layouts->count(record.typeName))
                    if (const auto dot = record.typeName.rfind('.');
                        dot != std::string::npos)
                        if (const auto bare = record.typeName.substr(dot + 1);
                            context.layouts->count(bare))
                            record.typeName = bare;
                // DECLARATION order, because a record is a tuple on BEAM and
                // the order here can BE the tuple layout: lowering's main path
                // looks each written field up by name, but its `records.find`
                // MISS path falls back to "fields as written", positionally.
                // Emitting declared order makes written order and layout
                // coincide, so that fallback cannot corrupt anything.
                //
                // Falling back to ALPHABETICAL for a type with no known layout
                // — rather than the map's own order — because RecordValue's
                // fields are an unordered_map, and an unstable order would
                // make identical builds emit different code.
                std::vector<const std::string*> names;
                names.reserve(node.fields.size());
                if (context.layouts) {
                    const auto declared = context.layouts->find(record.typeName);
                    if (declared != context.layouts->end())
                        for (const auto& field : declared->second)
                            if (node.fields.count(field)) names.push_back(&field);
                }
                // Anything the layout did not account for (and every field, if
                // the type is unknown) keeps the reproducible alphabetical
                // tail rather than being dropped.
                std::vector<const std::string*> extra;
                for (const auto& [field, _] : node.fields) {
                    bool listed = false;
                    for (const auto* seen : names)
                        if (*seen == field) { listed = true; break; }
                    if (!listed) extra.push_back(&field);
                }
                std::sort(extra.begin(), extra.end(),
                          [](const std::string* a, const std::string* b) {
                              return *a < *b;
                          });
                names.insert(names.end(), extra.begin(), extra.end());
                for (const auto* field : names) {
                    auto lowered =
                        valueToLiteral(node.fields.at(*field), location, error, context);
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
                        auto lowered = valueToLiteral(arg, location, error, context);
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
