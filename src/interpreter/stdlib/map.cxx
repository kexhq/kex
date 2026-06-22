#include "../evaluator.hxx"

namespace kex::interpreter {

// Map operations (UFCS: first arg is the map). Maps are immutable —
// put/delete return a new MapValue rather than mutating in place (the
// existing `!`-suffix mutating-call machinery in eval()'s MethodCall
// handling reassigns the receiver variable to the result automatically,
// e.g. `obj.put!(key, value)`).
auto Evaluator::registerMapBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // get(map, key) -> V? (Just(value) / None)
    // get(map, key, default) -> V (the default itself if key is missing)
    reg("get", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return args.size() >= 3 ? args[2] : Value::none();
        for (const auto& [k, v] : map->entries) {
            if (valuesEqual(k, args[1])) {
                return args.size() >= 3 ? v : Value::record("Just", {{"0", v}});
            }
        }
        return args.size() >= 3 ? args[2] : Value::none();
    });

    // put(map, key, value) -> Map — replaces the value if key already
    // exists, otherwise appends a new entry.
    reg("put", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 3) return args.empty() ? Value::list({}) : args[0];
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return args[0];
        auto entries = map->entries;
        for (auto& [k, v] : entries) {
            if (valuesEqual(k, args[1])) {
                v = args[2];
                auto result = std::make_shared<Value>();
                result->data = MapValue{std::move(entries)};
                return result;
            }
        }
        entries.push_back({args[1], args[2]});
        auto result = std::make_shared<Value>();
        result->data = MapValue{std::move(entries)};
        return result;
    });

    // delete(map, key) -> Map — without that key (no-op if absent).
    reg("delete", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return args.empty() ? Value::list({}) : args[0];
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return args[0];
        std::vector<std::pair<ValuePtr, ValuePtr>> entries;
        for (const auto& [k, v] : map->entries) {
            if (!valuesEqual(k, args[1])) entries.push_back({k, v});
        }
        auto result = std::make_shared<Value>();
        result->data = MapValue{std::move(entries)};
        return result;
    });

    reg("has?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return Value::boolean(false);
        for (const auto& [k, v] : map->entries) {
            if (valuesEqual(k, args[1])) return Value::boolean(true);
        }
        return Value::boolean(false);
    });

    reg("keys", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& [k, v] : map->entries) result.push_back(k);
        return Value::list(std::move(result));
    });

    reg("values", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& [k, v] : map->entries) result.push_back(v);
        return Value::list(std::move(result));
    });
}

} // namespace kex::interpreter
