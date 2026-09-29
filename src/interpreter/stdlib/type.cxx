#include "../evaluator.hxx"
#include "../../semantic/declared_type.hxx"

namespace kex::interpreter {

namespace {

auto namedType(const std::string& name, std::vector<ValuePtr> args = {})
    -> ValuePtr {
    return Value::record("Type", {
        {"name", Value::string(name)},
        {"args", Value::list(std::move(args))},
        {"pure", Value::boolean(true)},
    });
}

auto typeOfValue(const ValuePtr& value) -> ValuePtr;

// A container's element type, when every element agrees on it. `[]` has
// nothing to inspect, which is exactly the erasure the checker covers.
auto elementType(const std::vector<ValuePtr>& elements) -> ValuePtr {
    if (elements.empty()) return namedType("?");
    auto first = typeOfValue(elements.front());
    for (size_t i = 1; i < elements.size(); i++)
        if (!valuesEqual(first, typeOfValue(elements[i])))
            return namedType("Any");
    return first;
}

auto typeOfValue(const ValuePtr& value) -> ValuePtr {
    if (!value) return namedType("Any");
    return std::visit([&](const auto& v) -> ValuePtr {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, UnitValue>) return namedType("Void");
        else if constexpr (std::is_same_v<T, IntValue> ||
                           std::is_same_v<T, BigIntValue>)
            return namedType("Integer");
        else if constexpr (std::is_same_v<T, FloatValue>) return namedType("Float");
        else if constexpr (std::is_same_v<T, StringValue>) return namedType("String");
        else if constexpr (std::is_same_v<T, BinaryValue>) return namedType("Binary");
        else if constexpr (std::is_same_v<T, CharValue>) return namedType("Char");
        else if constexpr (std::is_same_v<T, BoolValue>) return namedType("Bool");
        else if constexpr (std::is_same_v<T, ListValue>)
            return namedType("List", {elementType(v.elements)});
        else if constexpr (std::is_same_v<T, MapValue>) {
            // Same rule as a list: report the key/value types when every
            // entry agrees on them.
            std::vector<ValuePtr> keys;
            std::vector<ValuePtr> values;
            for (const auto& [key, value] : v.entries) {
                keys.push_back(key);
                values.push_back(value);
            }
            if (keys.empty()) return namedType("Map");
            return namedType("Map", {elementType(keys), elementType(values)});
        }
        else if constexpr (std::is_same_v<T, RangeValue>) return namedType("Range");
        else if constexpr (std::is_same_v<T, StreamValue>) return namedType("Stream");
        else if constexpr (std::is_same_v<T, FeedValue>) return namedType("Feed");
        else if constexpr (std::is_same_v<T, RecordValue>) return namedType(v.typeName);
        else if constexpr (std::is_same_v<T, TupleValue>) {
            std::vector<ValuePtr> args;
            for (const auto& element : v.elements) args.push_back(typeOfValue(element));
            return namedType("Tuple", std::move(args));
        }
        else if constexpr (std::is_same_v<T, VariantValue>) {
            // Option/Result carry their payload's type; the half a value does
            // not hold is unknowable from the value alone.
            const std::string owner = v.parentType.empty() ? v.tag : v.parentType;
            if (owner == "Optional" || owner == "Option")
                return namedType("Option", {v.args.empty() ? namedType("?")
                                                           : typeOfValue(v.args[0])});
            if (owner == "Result") {
                if (v.tag == "Ok")
                    return namedType("Result", {v.args.empty() ? namedType("?")
                                                               : typeOfValue(v.args[0]),
                                                namedType("?")});
                return namedType("Result", {namedType("?"),
                                            v.args.empty() ? namedType("?")
                                                           : typeOfValue(v.args[0])});
            }
            return namedType(owner);
        }
        else if constexpr (std::is_same_v<T, FunctionValue> ||
                           std::is_same_v<T, LambdaValue>)
            return namedType("Function");
        else if constexpr (std::is_same_v<T, ProcessValue>) return namedType("Process");
        else if constexpr (std::is_same_v<T, TaskValue>) return namedType("Task");
        else if constexpr (std::is_same_v<T, FileHandleValue>)
            return namedType("FileHandle");
        else if constexpr (std::is_same_v<T, AtomValue>) return namedType("Atom");
        else if constexpr (std::is_same_v<T, ModuleValue>) return namedType("Module");
        else return namedType("Any");
    }, value->data);
}

// A type's entry by the name `Type.of` reports. A type declared inside a
// module is registered under one spelling (`Kex.Version`) while a value may
// report the other (`Version`), so a qualified name also matches its bare
// last segment, and a bare name the one qualified entry ending in it.
template <typename Map>
auto findByTypeName(const Map& map, const std::string& name)
    -> const typename Map::mapped_type* {
    if (auto exact = map.find(name); exact != map.end()) return &exact->second;
    const auto dot = name.rfind('.');
    if (dot != std::string::npos)
        if (auto bare = map.find(name.substr(dot + 1)); bare != map.end())
            return &bare->second;
    const typename Map::mapped_type* found = nullptr;
    for (const auto& [key, value] : map) {
        const auto keyDot = key.rfind('.');
        if (keyDot == std::string::npos || key.compare(keyDot + 1, std::string::npos, name) != 0)
            continue;
        if (found) return nullptr;  // ambiguous: two modules declare it
        found = &value;
    }
    return found;
}

auto stringArg(const std::vector<ValuePtr>& args) -> std::string {
    if (args.empty()) return "";
    auto* s = std::get_if<StringValue>(&args[0]->data);
    return s ? s->value : "";
}

} // namespace

// The runtime half of `Type.of` — what a value IS, structurally. The compiler
// answers first where it can (a checked expression knows the halves of a
// Result and the element type of an empty list); this is the fallback, and
// kex_intrinsic_type.erl mirrors it clause for clause so both backends agree.
auto Evaluator::registerTypeBuiltins() -> void {
    defineIntrinsic("Type::ofValue", [](std::vector<ValuePtr> args) -> ValuePtr {
        return typeOfValue(args.empty() ? nullptr : args[0]);
    });

    defineIntrinsic("Type::fieldsOf", [this](std::vector<ValuePtr> args) -> ValuePtr {
        std::vector<ValuePtr> fields;
        const auto* record = findByTypeName(m_recordDefs, stringArg(args));
        if (record && *record)
            for (const auto& field : (*record)->fields)
                fields.push_back(Value::string(field.name));
        return Value::list(std::move(fields));
    });

    // `Type.fields`: one `(name, type, optional, hasDefault)` tuple per field,
    // in declaration order, the declared type read from the source as written
    // (semantic::structuredTypeOfDeclared) — the same shape the BEAM runtime
    // is handed by the compiler.
    defineIntrinsic("Type::fieldLayoutsOf",
                    [this](std::vector<ValuePtr> args) -> ValuePtr {
        std::vector<ValuePtr> fields;
        const auto* record = findByTypeName(m_recordDefs, stringArg(args));
        if (record && *record)
            for (const auto& field : (*record)->fields) {
                const auto type = field.type
                    ? semantic::structuredTypeOfDeclared(*field.type)
                    : semantic::StructuredType{"Any", {}};
                fields.push_back(Value::tuple({
                    Value::string(field.name),
                    structuredTypeValue(type),
                    Value::boolean(type.name == "Option"),
                    Value::boolean(field.defaultValue.has_value()),
                }));
            }
        return Value::list(std::move(fields));
    });

    // `Type.constructors`: one `(name, [argument type])` tuple per variant,
    // in declaration order.
    defineIntrinsic("Type::constructorLayoutsOf",
                    [this](std::vector<ValuePtr> args) -> ValuePtr {
        std::vector<ValuePtr> constructors;
        const auto* variants = findByTypeName(m_adtVariants, stringArg(args));
        if (variants)
            for (const auto* variant : *variants) {
                std::string name;
                std::vector<ValuePtr> argTypes;
                if (const auto* generic = std::get_if<ast::GenericType>(&variant->kind)) {
                    if (generic->name.parts.empty()) continue;
                    name = generic->name.parts.back();
                    for (const auto& arg : generic->args)
                        argTypes.push_back(structuredTypeValue(
                            arg ? semantic::structuredTypeOfDeclared(*arg)
                                : semantic::StructuredType{"Any", {}}));
                } else if (const auto* plain = std::get_if<ast::TypeName>(&variant->kind)) {
                    if (plain->parts.empty()) continue;
                    name = plain->parts.back();
                } else {
                    continue;
                }
                constructors.push_back(Value::tuple({
                    Value::string(name), Value::list(std::move(argTypes))}));
            }
        return Value::list(std::move(constructors));
    });

    defineIntrinsic("Type::constructorsOf",
                    [this](std::vector<ValuePtr> args) -> ValuePtr {
        // In declaration order, as written: iterating m_variantParent (a
        // hash map) answered in whatever order it happened to hold them.
        std::vector<ValuePtr> constructors;
        if (const auto* variants = findByTypeName(m_adtVariants, stringArg(args)))
            for (const auto* variant : *variants) {
                if (const auto* generic = std::get_if<ast::GenericType>(&variant->kind);
                    generic && !generic->name.parts.empty())
                    constructors.push_back(Value::string(generic->name.parts.back()));
                else if (const auto* plain = std::get_if<ast::TypeName>(&variant->kind);
                         plain && !plain->parts.empty())
                    constructors.push_back(Value::string(plain->parts.back()));
            }
        return Value::list(std::move(constructors));
    });
}

} // namespace kex::interpreter
