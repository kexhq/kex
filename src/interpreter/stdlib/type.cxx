#include "../evaluator.hxx"

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
        auto record = m_recordDefs.find(stringArg(args));
        if (record != m_recordDefs.end() && record->second)
            for (const auto& field : record->second->fields)
                fields.push_back(Value::string(field.name));
        return Value::list(std::move(fields));
    });

    defineIntrinsic("Type::constructorsOf",
                    [this](std::vector<ValuePtr> args) -> ValuePtr {
        // m_variantParent maps constructor -> owning ADT; this is its
        // inverse, which is only needed here.
        const auto owner = stringArg(args);
        std::vector<ValuePtr> constructors;
        for (const auto& [constructor, parent] : m_variantParent)
            if (parent == owner) constructors.push_back(Value::string(constructor));
        return Value::list(std::move(constructors));
    });
}

} // namespace kex::interpreter
