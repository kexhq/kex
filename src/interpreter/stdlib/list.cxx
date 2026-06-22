#include "../evaluator.hxx"
#include <algorithm>

namespace kex::interpreter {

// List operations (UFCS: first arg is the list/range).
auto Evaluator::registerListBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // Helper: expand a Range to a list for operations
    auto rangeToList = [](const RangeValue& r) -> std::vector<ValuePtr> {
        std::vector<ValuePtr> elems;
        for (int64_t i = r.start; i <= r.end; i++) {
            elems.push_back(Value::integer(i));
        }
        return elems;
    };

    // Helper: get elements from a list or range
    auto getElements = [&rangeToList](const ValuePtr& val) -> std::vector<ValuePtr> {
        if (auto* list = std::get_if<ListValue>(&val->data)) return list->elements;
        if (auto* range = std::get_if<RangeValue>(&val->data)) return rangeToList(*range);
        return {};
    };

    // to(Type) — universal conversion
    reg("to", [&rangeToList](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        // args[0] = this (value to convert), args[1] = target type identifier
        // For now, check by UpperIdent name passed as a constructor/identifier
        auto* targetId = std::get_if<StringValue>(&args[1]->data);
        // If target is a bare UpperIdent, it gets evaluated as an identifier
        // For now handle common conversions directly

        // Range/Stream/List -> List
        if (auto* range = std::get_if<RangeValue>(&args[0]->data)) {
            return Value::list(rangeToList(*range));
        }
        if (std::holds_alternative<ListValue>(args[0]->data)) return args[0];

        // Anything -> String
        return Value::string(args[0]->toString());
    });

    reg("length", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(0);
        if (auto* list = std::get_if<ListValue>(&args[0]->data))
            return Value::integer(static_cast<int64_t>(list->elements.size()));
        if (auto* str = std::get_if<StringValue>(&args[0]->data))
            return Value::integer(static_cast<int64_t>(str->value.size()));
        if (auto* range = std::get_if<RangeValue>(&args[0]->data))
            return Value::integer(range->end - range->start + 1);
        return Value::integer(0);
    });

    reg("empty?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(true);
        if (auto* list = std::get_if<ListValue>(&args[0]->data))
            return Value::boolean(list->elements.empty());
        if (auto* str = std::get_if<StringValue>(&args[0]->data))
            return Value::boolean(str->value.empty());
        return Value::boolean(true);
    });

    reg("push", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return args.empty() ? Value::list({}) : args[0];
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            auto newList = list->elements;
            newList.push_back(args[1]);
            return Value::list(std::move(newList));
        }
        return args[0];
    });

    reg("map", [this, &getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});

        // Lazy map on streams
        if (auto* stream = std::get_if<StreamValue>(&args[0]->data)) {
            auto* fn = std::get_if<FunctionValue>(&args[1]->data);
            if (!fn || !fn->native) return Value::list({});
            auto gen = stream->generator;
            auto offset = stream->offset;
            auto mapFn = fn->native;
            auto newStream = std::make_shared<Value>();
            newStream->data = StreamValue{[gen, offset, mapFn](int64_t index) -> ValuePtr {
                return mapFn({gen(offset + index)});
            }, 0};
            return newStream;
        }

        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::list({});

        std::vector<ValuePtr> result;
        for (const auto& elem : elems) {
            result.push_back(fn->native({elem}));
        }
        return Value::list(std::move(result));
    });

    reg("filter", [this, &getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});

        // Lazy filter on streams — returns a new stream that skips non-matching
        if (auto* stream = std::get_if<StreamValue>(&args[0]->data)) {
            auto* fn = std::get_if<FunctionValue>(&args[1]->data);
            if (!fn || !fn->native) return Value::list({});
            auto gen = stream->generator;
            auto offset = stream->offset;
            auto filterFn = fn->native;
            auto newStream = std::make_shared<Value>();
            newStream->data = StreamValue{[gen, offset, filterFn](int64_t index) -> ValuePtr {
                // Walk forward through the source stream to find the nth matching element
                int64_t found = 0;
                int64_t i = 0;
                int64_t maxSearch = index * 100 + 1000; // safety limit
                while (found <= index && i < maxSearch) {
                    auto val = gen(offset + i);
                    if (filterFn({val})->isTrue()) {
                        if (found == index) return val;
                        found++;
                    }
                    i++;
                }
                return Value::none();
            }, 0};
            return newStream;
        }

        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::list({});

        std::vector<ValuePtr> result;
        for (const auto& elem : elems) {
            auto val = fn->native({elem});
            if (val->isTrue()) result.push_back(elem);
        }
        return Value::list(std::move(result));
    });

    reg("reject", [this, &getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::list({});

        std::vector<ValuePtr> result;
        for (const auto& elem : elems) {
            auto val = fn->native({elem});
            if (!val->isTrue()) result.push_back(elem);
        }
        return Value::list(std::move(result));
    });

    reg("reduce", [this, &getElements](std::vector<ValuePtr> args) -> ValuePtr {
        // reduce(list, initial, fn)
        if (args.size() < 3) return Value::none();
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[2]->data);
        if (!fn || !fn->native) return args[1];

        auto acc = args[1];
        for (const auto& elem : elems) {
            acc = fn->native({acc, elem});
        }
        return acc;
    });

    reg("each", [this, &getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::none();

        for (const auto& elem : elems) {
            fn->native({elem});
        }
        return Value::none();
    });

    reg("find", [this, &getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::none();

        for (const auto& elem : elems) {
            auto val = fn->native({elem});
            if (val->isTrue()) return elem;
        }
        return Value::none();
    });

    reg("join", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) return Value::string("");

        std::string sep = "";
        if (args.size() >= 2) {
            if (auto* s = std::get_if<StringValue>(&args[1]->data)) sep = s->value;
        }

        std::string result;
        for (size_t i = 0; i < list->elements.size(); i++) {
            if (i > 0) result += sep;
            result += list->elements[i]->toString();
        }
        return Value::string(result);
    });

    reg("first", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            if (list->elements.empty()) return Value::none();
            return list->elements[0];
        }
        return Value::none();
    });

    reg("last", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            if (list->elements.empty()) return Value::none();
            return list->elements.back();
        }
        return Value::none();
    });

    reg("size", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(0);
        if (auto* list = std::get_if<ListValue>(&args[0]->data))
            return Value::integer(static_cast<int64_t>(list->elements.size()));
        if (auto* map = std::get_if<MapValue>(&args[0]->data))
            return Value::integer(static_cast<int64_t>(map->entries.size()));
        return Value::integer(0);
    });

    reg("take", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* list = std::get_if<ListValue>(&args[0]->data);
        auto* n = std::get_if<IntValue>(&args[1]->data);
        if (!list || !n) return Value::list({});
        auto count = std::min(static_cast<size_t>(n->value), list->elements.size());
        std::vector<ValuePtr> result(list->elements.begin(), list->elements.begin() + count);
        return Value::list(std::move(result));
    });

    reg("drop", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* list = std::get_if<ListValue>(&args[0]->data);
        auto* n = std::get_if<IntValue>(&args[1]->data);
        if (!list || !n) return Value::list({});
        auto skip = std::min(static_cast<size_t>(n->value), list->elements.size());
        std::vector<ValuePtr> result(list->elements.begin() + skip, list->elements.end());
        return Value::list(std::move(result));
    });

    reg("zip", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* a = std::get_if<ListValue>(&args[0]->data);
        auto* b = std::get_if<ListValue>(&args[1]->data);
        if (!a || !b) return Value::list({});
        auto len = std::min(a->elements.size(), b->elements.size());
        std::vector<ValuePtr> result;
        for (size_t i = 0; i < len; i++) {
            result.push_back(Value::tuple({a->elements[i], b->elements[i]}));
        }
        return Value::list(std::move(result));
    });

    reg("flatten", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& elem : list->elements) {
            if (auto* inner = std::get_if<ListValue>(&elem->data)) {
                for (const auto& e : inner->elements) result.push_back(e);
            } else {
                result.push_back(elem);
            }
        }
        return Value::list(std::move(result));
    });

    reg("any?", [this, &getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::boolean(false);
        for (const auto& elem : elems) {
            if (fn->native({elem})->isTrue()) return Value::boolean(true);
        }
        return Value::boolean(false);
    });

    reg("all?", [this, &getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(true);
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::boolean(true);
        for (const auto& elem : elems) {
            if (!fn->native({elem})->isTrue()) return Value::boolean(false);
        }
        return Value::boolean(true);
    });

    reg("count", [this, &getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(0);
        auto elems = getElements(args[0]);
        if (args.size() < 2) return Value::integer(static_cast<int64_t>(elems.size()));
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::integer(0);
        int64_t c = 0;
        for (const auto& elem : elems) {
            if (fn->native({elem})->isTrue()) c++;
        }
        return Value::integer(c);
    });

    reg("sort", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) return Value::list({});
        auto sorted = list->elements;
        std::sort(sorted.begin(), sorted.end(), [](const ValuePtr& a, const ValuePtr& b) {
            auto* ai = std::get_if<IntValue>(&a->data);
            auto* bi = std::get_if<IntValue>(&b->data);
            if (ai && bi) return ai->value < bi->value;
            auto* af = std::get_if<FloatValue>(&a->data);
            auto* bf = std::get_if<FloatValue>(&b->data);
            if (af && bf) return af->value < bf->value;
            auto* as = std::get_if<StringValue>(&a->data);
            auto* bs = std::get_if<StringValue>(&b->data);
            if (as && bs) return as->value < bs->value;
            return false;
        });
        return Value::list(std::move(sorted));
    });

    reg("min", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            if (list->elements.empty()) return Value::none();
            auto result = list->elements[0];
            for (size_t i = 1; i < list->elements.size(); i++) {
                auto* ri = std::get_if<IntValue>(&result->data);
                auto* ei = std::get_if<IntValue>(&list->elements[i]->data);
                if (ri && ei && ei->value < ri->value) result = list->elements[i];
            }
            return result;
        }
        return args[0];
    });

    reg("max", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            if (list->elements.empty()) return Value::none();
            auto result = list->elements[0];
            for (size_t i = 1; i < list->elements.size(); i++) {
                auto* ri = std::get_if<IntValue>(&result->data);
                auto* ei = std::get_if<IntValue>(&list->elements[i]->data);
                if (ri && ei && ei->value > ri->value) result = list->elements[i];
            }
            return result;
        }
        return args[0];
    });

    reg("sum", [&getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(0);
        auto elems = getElements(args[0]);
        int64_t total = 0;
        for (const auto& elem : elems) {
            if (auto* i = std::get_if<IntValue>(&elem->data)) total += i->value;
        }
        return Value::integer(total);
    });
}

} // namespace kex::interpreter
