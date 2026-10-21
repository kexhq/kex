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

// findByTypeName's key rather than its entry: the name a record value or a
// variant is built under.
template <typename Map>
auto typeNameKey(const Map& map, const std::string& name)
    -> std::optional<std::string> {
    const auto* found = findByTypeName(map, name);
    if (!found) return std::nullopt;
    for (const auto& [key, value] : map)
        if (&value == found) return key;
    return std::nullopt;
}

// `matches` (the evaluator's runtimeTypeMatches) also applied to a list's
// elements and a map's entries, as kex_intrinsic_type.erl's accepts/2 does.
template <typename Matches>
auto matchesDeeply(const ValuePtr& value, const ast::TypeExpr& type,
                   const Matches& matches) -> bool {
    if (!matches(value, type)) return false;
    if (const auto* list = std::get_if<ast::ListType>(&type.kind); list && list->element)
        if (const auto* values = std::get_if<ListValue>(&value->data))
            return std::all_of(values->elements.begin(), values->elements.end(),
                               [&](const ValuePtr& element) {
                                   return matchesDeeply(element, *list->element, matches);
                               });
    if (const auto* map = std::get_if<ast::MapType>(&type.kind); map && map->key && map->value)
        if (const auto* entries = std::get_if<MapValue>(&value->data))
            return std::all_of(entries->entries.begin(), entries->entries.end(),
                               [&](const auto& entry) {
                                   return matchesDeeply(entry.first, *map->key, matches) &&
                                          matchesDeeply(entry.second, *map->value, matches);
                               });
    return true;
}

// A `Type { name, args }` value as `Name<Arg, …>`, the spelling
// kex_intrinsic_type.erl's type_name/1 gives.
auto renderedType(const ValuePtr& type) -> std::string {
    const auto* record = std::get_if<RecordValue>(&type->data);
    if (!record) return "?";
    const auto name = record->fields.find("name");
    const auto args = record->fields.find("args");
    std::string text = name != record->fields.end()
        ? name->second->toString() : "?";
    if (args == record->fields.end()) return text;
    const auto* list = std::get_if<ListValue>(&args->second->data);
    if (!list || list->elements.empty()) return text;
    text += "<";
    for (size_t i = 0; i < list->elements.size(); ++i) {
        if (i) text += ", ";
        text += renderedType(list->elements[i]);
    }
    return text + ">";
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

    // `Type.values`: a record's field values in declaration order. The
    // fields are held by name here, so the order comes from the declaration,
    // as the tuple layout gives it on BEAM.
    defineIntrinsic("Type::recordValues", [this](std::vector<ValuePtr> args) -> ValuePtr {
        const auto* value = args.empty() || !args[0]
            ? nullptr : std::get_if<RecordValue>(&args[0]->data);
        if (!value) return Value::none();
        const auto* record = findByTypeName(m_recordDefs, value->typeName);
        if (!record || !*record) return Value::none();
        std::vector<ValuePtr> values;
        for (const auto& field : (*record)->fields) {
            const auto found = value->fields.find(field.name);
            values.push_back(found != value->fields.end() ? found->second
                                                          : Value::none());
        }
        return Value::just(Value::list(std::move(values)));
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
    // (semantic::structuredTypeOfDeclared), a transparent alias read as what
    // it stands for — the same shape the BEAM runtime is handed by the
    // compiler.
    defineIntrinsic("Type::fieldLayoutsOf",
                    [this](std::vector<ValuePtr> args) -> ValuePtr {
        std::vector<ValuePtr> fields;
        const auto* record = findByTypeName(m_recordDefs, stringArg(args));
        if (record && *record)
            for (const auto& field : (*record)->fields) {
                const auto type = field.type
                    ? semantic::structuredTypeOfDeclared(*field.type, m_declaredAliases)
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
                            arg ? semantic::structuredTypeOfDeclared(*arg, m_declaredAliases)
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

    const auto matches = [this](const ValuePtr& value, const ast::TypeExpr& type) {
        return runtimeTypeMatches(value, type);
    };

    // `Type.construct`: a record of the named type from values by field name
    // (kexhq/kex#487). The same rules and messages as kex_intrinsic_type.erl.
    defineIntrinsic("Type::construct", [this, matches](std::vector<ValuePtr> args) -> ValuePtr {
        const auto name = stringArg(args);
        const auto key = typeNameKey(m_recordDefs, name);
        if (!key) return Value::error(Value::string("Not a record type: " + name));
        const auto* record = m_recordDefs.at(*key);
        const auto* values =
            args.size() > 1 ? std::get_if<MapValue>(&args[1]->data) : nullptr;
        if (!values) return Value::error(Value::string("Expected a map of field values"));
        std::unordered_map<std::string, ValuePtr> given;
        for (const auto& [k, v] : values->entries) {
            const auto* field = std::get_if<StringValue>(&k->data);
            const bool known = field && std::any_of(
                record->fields.begin(), record->fields.end(),
                [&](const auto& declared) { return declared.name == field->value; });
            if (!known)
                return Value::error(Value::string(
                    "Unknown field " + (field ? "\"" + field->value + "\"" : k->toString()) +
                    " for " + name));
            given[field->value] = v;
        }
        std::unordered_map<std::string, ValuePtr> fields;
        for (const auto& field : record->fields) {
            const auto declared = field.type
                ? semantic::structuredTypeOfDeclared(*field.type, m_declaredAliases)
                : semantic::StructuredType{"Any", {}};
            if (auto found = given.find(field.name); found != given.end()) {
                if (field.type && !matchesDeeply(found->second, *field.type, matches))
                    return Value::error(Value::string(
                        "Field \"" + field.name + "\" of " + name + " expects " +
                        renderedType(structuredTypeValue(declared)) + ", got " +
                        renderedType(typeOfValue(found->second))));
                fields[field.name] = found->second;
            } else if (field.defaultValue && *field.defaultValue) {
                fields[field.name] = evalFieldDefault(*key, **field.defaultValue);
            } else if (declared.name == "Option") {
                fields[field.name] = Value::none();
            } else {
                return Value::error(Value::string(
                    "Missing field \"" + field.name + "\" for " + name));
            }
        }
        return Value::ok(Value::record(*key, std::move(fields)));
    });

    // `Type.variant`: the named constructor of a sum type, applied to its
    // arguments (kexhq/kex#509).
    defineIntrinsic("Type::constructVariant",
                    [this, matches](std::vector<ValuePtr> args) -> ValuePtr {
        const auto name = stringArg(args);
        const auto key = typeNameKey(m_adtVariants, name);
        if (!key) return Value::error(Value::string("Not a sum type: " + name));
        const auto* ctor = args.size() > 1 ? std::get_if<StringValue>(&args[1]->data) : nullptr;
        const auto* given = args.size() > 2 ? std::get_if<ListValue>(&args[2]->data) : nullptr;
        if (!ctor || !given)
            return Value::error(Value::string("Expected a constructor name and its arguments"));
        for (const auto* variant : m_adtVariants.at(*key)) {
            std::vector<const ast::TypeExpr*> argTypes;
            std::string tag;
            if (const auto* generic = std::get_if<ast::GenericType>(&variant->kind);
                generic && !generic->name.parts.empty()) {
                tag = generic->name.parts.back();
                for (const auto& arg : generic->args) argTypes.push_back(arg.get());
            } else if (const auto* plain = std::get_if<ast::TypeName>(&variant->kind);
                       plain && !plain->parts.empty()) {
                tag = plain->parts.back();
            }
            if (tag != ctor->value) continue;
            if (argTypes.size() != given->elements.size())
                return Value::error(Value::string(
                    tag + " takes " + std::to_string(argTypes.size()) +
                    " argument(s), got " + std::to_string(given->elements.size())));
            for (size_t i = 0; i < argTypes.size(); ++i)
                if (argTypes[i] && !matchesDeeply(given->elements[i], *argTypes[i], matches))
                    return Value::error(Value::string(
                        tag + " expects " +
                        renderedType(structuredTypeValue(
                            semantic::structuredTypeOfDeclared(*argTypes[i], m_declaredAliases))) +
                        ", got " + renderedType(typeOfValue(given->elements[i]))));
            return Value::ok(Value::variant(tag, *key, given->elements));
        }
        return Value::error(Value::string(name + " has no constructor " + ctor->value));
    });
}

} // namespace kex::interpreter
