#include "../evaluator.hxx"
#include <algorithm>

namespace kex::interpreter {

namespace {
// Maps are unordered; keys/values/entries expose a deterministic CANONICAL
// order (sorted by key) so the tree-walker and both BEAM backends agree — plain
// Erlang map order is non-deterministic across VM invocations. Matches Erlang's
// lists:sort term order for the common homogeneous key types.
auto mapKeyLess(const ValuePtr& a, const ValuePtr& b) -> bool {
    if (auto* ai = std::get_if<IntValue>(&a->data)) if (auto* bi = std::get_if<IntValue>(&b->data)) return ai->value < bi->value;
    if (auto* af = std::get_if<FloatValue>(&a->data)) if (auto* bf = std::get_if<FloatValue>(&b->data)) return af->value < bf->value;
    if (auto* as = std::get_if<StringValue>(&a->data)) if (auto* bs = std::get_if<StringValue>(&b->data)) return as->value < bs->value;
    if (auto* ac = std::get_if<CharValue>(&a->data)) if (auto* bc = std::get_if<CharValue>(&b->data)) return ac->value < bc->value;
    if (auto* aa = std::get_if<AtomValue>(&a->data)) if (auto* ba = std::get_if<AtomValue>(&b->data)) return aa->name < ba->name;
    return false; // unknown/mixed key types — keep stable order
}
auto sortedEntries(const MapValue& m) -> std::vector<std::pair<ValuePtr, ValuePtr>> {
    auto es = m.entries;
    std::stable_sort(es.begin(), es.end(),
        [](const auto& x, const auto& y) { return mapKeyLess(x.first, y.first); });
    return es;
}
} // namespace

// Map operations (UFCS: first arg is the map). Maps are immutable —
// put/delete return a new MapValue rather than mutating in place (the
// existing `!`-suffix mutating-call machinery in eval()'s MethodCall
// handling reassigns the receiver variable to the result automatically,
// e.g. `obj.put!(key, value)`).
auto Evaluator::registerMapBuiltins() -> void {
    defineModule("Map");

    auto reg = [this](const std::string& name, NativeFunc fn) {
        defineIntrinsic("Map::" + name, std::move(fn));
    };

    // get(map, key) -> V? (Just(value) / None)
    // get(map, key, default) -> V (the default itself if key is missing)
    //
    // Also doubles as list indexing — `list[i]` desugars to `list.get(i)`
    // (see parsePostfix's bracket-access handling) — returning the raw
    // element directly (or None/the default if out of range), the same
    // convention as String.at(i), not Map's Just(value)-wrapping.
    reg("get", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            auto* idx = std::get_if<IntValue>(&args[1]->data);
            if (!idx || idx->value < 0 || static_cast<size_t>(idx->value) >= list->elements.size()) {
                return args.size() >= 3 ? args[2] : Value::none();
            }
            return list->elements[static_cast<size_t>(idx->value)];
        }
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return args.size() >= 3 ? args[2] : Value::none();
        for (const auto& [k, v] : sortedEntries(*map)) {
            if (valuesEqual(k, args[1])) {
                return args.size() >= 3 ? v : Value::just(v);
            }
        }
        return args.size() >= 3 ? args[2] : Value::none();
    });

    // Kex.Intrinsic.Map.getWithDefault(map, key, default) → raw value
    // or the default. Backs the prelude's `get(key, default)` wrapper.
    reg("getWithDefault", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 3) return args.size() >= 2 ? args[1] : Value::none();
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return args[2];
        for (const auto& [k, v] : sortedEntries(*map))
            if (valuesEqual(k, args[1])) return v;
        return args[2];
    });

    // put(map, key, value) -> Map — replaces the value if key already
    // exists, otherwise appends a new entry.
    reg("put", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 3) return args.empty() ? Value::list({}) : args[0];
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return args[0];
        auto entries = sortedEntries(*map);
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
        for (const auto& [k, v] : sortedEntries(*map)) {
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
        for (const auto& [k, v] : sortedEntries(*map)) {
            if (valuesEqual(k, args[1])) return Value::boolean(true);
        }
        return Value::boolean(false);
    });

    reg("keys", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& [k, v] : sortedEntries(*map)) result.push_back(k);
        return Value::list(std::move(result));
    });

    reg("values", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& [k, v] : sortedEntries(*map)) result.push_back(v);
        return Value::list(std::move(result));
    });

    // Map HOFs (empty?, each, map, filter, reject, any?, all?, find,
    // mapValues, mapKeys) are provided by the Kex prelude (Enumerable trait
    // + Map overrides in src/prelude/map.kex) built on entries/fromEntries/
    // reduce — no native versions needed. `count` stays below: the prelude
    // calls it as Kex.Intrinsic.Map.count.

    // merge(map, other) -> Map<K, V>  — other's values win on conflict
    reg("merge", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return args.empty() ? Value::list({}) : args[0];
        auto* base  = std::get_if<MapValue>(&args[0]->data);
        auto* other = std::get_if<MapValue>(&args[1]->data);
        if (!base)  return args[1];
        if (!other) return args[0];
        auto entries = base->entries;
        for (const auto& [ok, ov] : other->entries) {
            bool found = false;
            for (auto& [ek, ev] : entries) {
                if (valuesEqual(ek, ok)) { ev = ov; found = true; break; }
            }
            if (!found) entries.push_back({ok, ov});
        }
        auto result = std::make_shared<Value>();
        result->data = MapValue{std::move(entries)};
        return result;
    });

    // count(map, fn(k, v) -> Bool) -> Int
    reg("count", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) {
            if (args.empty()) return Value::integer(0);
            auto* map = std::get_if<MapValue>(&args[0]->data);
            return Value::integer(map ? static_cast<int64_t>(map->entries.size()) : 0);
        }
        auto* map = std::get_if<MapValue>(&args[0]->data);
        auto* fn  = std::get_if<FunctionValue>(&args[1]->data);
        if (!map || !fn || !fn->native) return Value::integer(0);
        int64_t n = 0;
        for (const auto& [k, v] : sortedEntries(*map)) {
            auto r = fn->native({k, v});
            if (auto* b = std::get_if<BoolValue>(&r->data); b && b->value) ++n;
        }
        return Value::integer(n);
    });

    // entries(map) -> [(K, V)]
    reg("entries", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* map = std::get_if<MapValue>(&args[0]->data);
        if (!map) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& [k, v] : sortedEntries(*map)) {
            auto tuple = std::make_shared<Value>();
            tuple->data = TupleValue{{k, v}};
            result.push_back(tuple);
        }
        return Value::list(std::move(result));
    });

    reg("fromEntries", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) {
            auto r = std::make_shared<Value>();
            r->data = MapValue{};
            return r;
        }
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) {
            auto r = std::make_shared<Value>();
            r->data = MapValue{};
            return r;
        }
        std::vector<std::pair<ValuePtr, ValuePtr>> entries;
        for (const auto& elem : list->elements) {
            auto* tv = std::get_if<TupleValue>(&elem->data);
            if (tv && tv->elements.size() >= 2)
                entries.push_back({tv->elements[0], tv->elements[1]});
        }
        auto result = std::make_shared<Value>();
        result->data = MapValue{std::move(entries)};
        return result;
    });

}

} // namespace kex::interpreter
