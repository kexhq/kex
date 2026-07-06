#include "../evaluator.hxx"
#include <algorithm>

namespace kex::interpreter {

auto Evaluator::registerListBuiltins() -> void {
    m_globalEnv->define("List",  Value::module("List"));
    m_globalEnv->define("Tuple", Value::module("Tuple"));
    m_globalEnv->define("Range", Value::module("Range"));

    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // Captured by value in every lambda below — rangeToList and getElements
    // are local stack variables that vanish when registerListBuiltins returns.
    // All lambdas that need them must capture by value, not by reference.
    auto rangeToList = [](const RangeValue& r) -> std::vector<ValuePtr> {
        std::vector<ValuePtr> elems;
        for (int64_t i = r.start; i <= r.end; i++)
            elems.push_back(r.isChar ? Value::character(static_cast<char>(i))
                                     : Value::integer(i));
        return elems;
    };

    auto getElements = [rangeToList](const ValuePtr& val) -> std::vector<ValuePtr> {
        if (auto* list = std::get_if<ListValue>(&val->data)) return list->elements;
        if (auto* range = std::get_if<RangeValue>(&val->data)) return rangeToList(*range);
        if (auto* str = std::get_if<StringValue>(&val->data)) {
            std::vector<ValuePtr> chars;
            chars.reserve(str->value.size());
            for (unsigned char c : str->value)
                chars.push_back(Value::character(static_cast<char>(c)));
            return chars;
        }
        return {};
    };

    // If every element is a Char, collapse to String — preserves String identity
    // through map/filter/etc. so `"hello".map(&.upperCase)` returns a String.
    auto repackChars = [](std::vector<ValuePtr> elems) -> ValuePtr {
        std::string s;
        s.reserve(elems.size());
        for (const auto& e : elems) {
            if (auto* cv = std::get_if<CharValue>(&e->data)) { s += cv->value; }
            else return Value::list(std::move(elems));
        }
        return Value::string(std::move(s));
    };

    // to(Type) — universal conversion. Type argument is a ModuleValue (for
    // builtin types like Integer, Float, String) or a RecordValue{} namespace
    // sentinel (for user record types used as namespaces).
    reg("to", [rangeToList](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        std::string targetName;
        if (auto* m = std::get_if<ModuleValue>(&args[1]->data))
            targetName = m->name;
        else if (auto* r = std::get_if<RecordValue>(&args[1]->data))
            targetName = r->typeName;

        if (targetName == "Integer") {
            if (std::holds_alternative<IntValue>(args[0]->data) ||
                std::holds_alternative<BigIntValue>(args[0]->data))
                return args[0];
            if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
                try { return Value::integer(std::stoll(str->value)); } catch (...) {}
            }
            if (auto* f = std::get_if<FloatValue>(&args[0]->data))
                return Value::integer(static_cast<int64_t>(f->value));
            return Value::none();
        }
        if (targetName == "Float") {
            if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
                try { return Value::floating(std::stod(str->value)); } catch (...) {}
            }
            if (auto i = asInteger(args[0])) return Value::floating(i->get_d());
            return Value::none();
        }
        if (targetName == "String") {
            return Value::string(args[0]->toString());
        }
        if (targetName == "List") {
            if (auto* range = std::get_if<RangeValue>(&args[0]->data))
                return Value::list(rangeToList(*range));
            if (std::holds_alternative<ListValue>(args[0]->data)) return args[0];
        }
        if (auto* range = std::get_if<RangeValue>(&args[0]->data))
            return Value::list(rangeToList(*range));
        if (std::holds_alternative<ListValue>(args[0]->data)) return args[0];
        return Value::string(args[0]->toString());
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

    reg("map", [this, getElements, repackChars](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
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
        bool srcIsStr = std::holds_alternative<StringValue>(args[0]->data);
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& elem : elems) result.push_back(fn->native({elem}));
        return srcIsStr ? repackChars(std::move(result)) : Value::list(std::move(result));
    });

    reg("filter", [this, getElements, repackChars](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        if (auto* stream = std::get_if<StreamValue>(&args[0]->data)) {
            auto* fn = std::get_if<FunctionValue>(&args[1]->data);
            if (!fn || !fn->native) return Value::list({});
            auto gen = stream->generator;
            auto offset = stream->offset;
            auto filterFn = fn->native;
            auto newStream = std::make_shared<Value>();
            newStream->data = StreamValue{[gen, offset, filterFn](int64_t index) -> ValuePtr {
                int64_t found = 0, i = 0, maxSearch = index * 100 + 1000;
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
        bool srcIsStr = std::holds_alternative<StringValue>(args[0]->data);
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& elem : elems) {
            if (fn->native({elem})->isTrue()) result.push_back(elem);
        }
        return srcIsStr ? repackChars(std::move(result)) : Value::list(std::move(result));
    });

    reg("reject", [this, getElements, repackChars](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        bool srcIsStr = std::holds_alternative<StringValue>(args[0]->data);
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& elem : elems) {
            if (!fn->native({elem})->isTrue()) result.push_back(elem);
        }
        return srcIsStr ? repackChars(std::move(result)) : Value::list(std::move(result));
    });

    reg("reduce", [this, getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 3) return Value::none();
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[2]->data);
        if (!fn || !fn->native) return args[1];
        auto acc = args[1];
        for (const auto& elem : elems) acc = fn->native({acc, elem});
        return acc;
    });

    // foldl — the intrinsic backing for Enumerable.reduce (Kex.Intrinsic.List.
    // foldl). Same left fold as `reduce` above (acc-first reducer), exposed
    // under the primitive name so the prelude's reduce is a thin intrinsic
    // wrapper on both backends.
    reg("foldl", [this, getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 3) return Value::none();
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[2]->data);
        if (!fn || !fn->native) return args[1];
        auto acc = args[1];
        for (const auto& elem : elems) acc = fn->native({acc, elem});
        return acc;
    });

    reg("each", [this, getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::none();
        if (auto* map = std::get_if<MapValue>(&args[0]->data)) {
            for (const auto& [k, v] : map->entries) fn->native({k, v});
            return Value::none();
        }
        auto elems = getElements(args[0]);
        for (const auto& elem : elems) fn->native({elem});
        return Value::none();
    });

    reg("find", [this, getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::none();
        for (const auto& elem : elems) {
            if (fn->native({elem})->isTrue()) return elem;
        }
        return Value::none();
    });

    reg("join", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) return Value::string("");
        std::string sep;
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
        if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
            if (str->value.empty()) return Value::none();
            return Value::just(Value::character(str->value[0]));
        }
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            if (list->elements.empty()) return Value::none();
            return Value::just(list->elements[0]);
        }
        if (auto* range = std::get_if<RangeValue>(&args[0]->data))
            return Value::just(Value::integer(range->start));
        return Value::none();
    });

    reg("second", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            if (list->elements.size() < 2) return Value::none();
            return Value::just(list->elements[1]);
        }
        if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
            if (str->value.size() < 2) return Value::none();
            return Value::just(Value::character(str->value[1]));
        }
        return Value::none();
    });

    reg("third", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            if (list->elements.size() < 3) return Value::none();
            return Value::just(list->elements[2]);
        }
        if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
            if (str->value.size() < 3) return Value::none();
            return Value::just(Value::character(str->value[2]));
        }
        return Value::none();
    });

    reg("rest", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        if (auto* str = std::get_if<StringValue>(&args[0]->data))
            return Value::string(str->value.size() <= 1 ? "" : str->value.substr(1));
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            if (list->elements.size() <= 1) return Value::list({});
            return Value::list({list->elements.begin() + 1, list->elements.end()});
        }
        return Value::list({});
    });

    reg("last", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
            if (str->value.empty()) return Value::none();
            return Value::just(Value::character(str->value.back()));
        }
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            if (list->elements.empty()) return Value::none();
            return Value::just(list->elements.back());
        }
        if (auto* range = std::get_if<RangeValue>(&args[0]->data))
            return Value::just(Value::integer(range->end));
        return Value::none();
    });

    // count([X]) -> Integer           — element count
    // count(String) -> Integer        — character count
    // count(Map) -> Integer           — entry count
    // count(Range) -> Integer         — range size
    // count([X], X->Bool) -> Integer  — filtered count
    reg("count", [this, getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(0);
        if (auto* str = std::get_if<StringValue>(&args[0]->data))
            return Value::integer(static_cast<int64_t>(str->value.size()));
        if (auto* map = std::get_if<MapValue>(&args[0]->data))
            return Value::integer(static_cast<int64_t>(map->entries.size()));
        if (auto* range = std::get_if<RangeValue>(&args[0]->data))
            return Value::integer(range->end - range->start + 1);
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

    reg("take", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto n = std::get_if<IntValue>(&args[1]->data);
        if (!n) return Value::list({});
        if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
            auto cnt = std::min(static_cast<size_t>(n->value), str->value.size());
            return Value::string(str->value.substr(0, cnt));
        }
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) return Value::list({});
        auto cnt = std::min(static_cast<size_t>(n->value), list->elements.size());
        return Value::list({list->elements.begin(), list->elements.begin() + cnt});
    });

    reg("drop", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto n = std::get_if<IntValue>(&args[1]->data);
        if (!n) return Value::list({});
        if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
            auto skip = std::min(static_cast<size_t>(n->value), str->value.size());
            return Value::string(str->value.substr(skip));
        }
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) return Value::list({});
        auto skip = std::min(static_cast<size_t>(n->value), list->elements.size());
        return Value::list({list->elements.begin() + skip, list->elements.end()});
    });

    reg("zip", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* a = std::get_if<ListValue>(&args[0]->data);
        auto* b = std::get_if<ListValue>(&args[1]->data);
        if (!a || !b) return Value::list({});
        auto len = std::min(a->elements.size(), b->elements.size());
        std::vector<ValuePtr> result;
        for (size_t i = 0; i < len; i++)
            result.push_back(Value::tuple({a->elements[i], b->elements[i]}));
        return Value::list(std::move(result));
    });

    reg("flatten", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& elem : list->elements) {
            if (auto* inner = std::get_if<ListValue>(&elem->data))
                for (const auto& e : inner->elements) result.push_back(e);
            else
                result.push_back(elem);
        }
        return Value::list(std::move(result));
    });

    reg("uniq", [getElements, repackChars](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto elems = getElements(args[0]);
        std::vector<ValuePtr> result;
        for (const auto& elem : elems) {
            bool found = false;
            for (const auto& seen : result)
                if (valuesEqual(seen, elem)) { found = true; break; }
            if (!found) result.push_back(elem);
        }
        if (std::get_if<StringValue>(&args[0]->data)) return repackChars(result);
        return Value::list(std::move(result));
    });

    reg("partition", [getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::tuple({Value::list({}), Value::list({})});
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::tuple({Value::list({}), Value::list({})});
        std::vector<ValuePtr> yes, no;
        for (const auto& elem : elems) {
            if (fn->native({elem})->isTrue()) yes.push_back(elem);
            else no.push_back(elem);
        }
        return Value::tuple({Value::list(std::move(yes)), Value::list(std::move(no))});
    });

    reg("indexOf", [getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto elems = getElements(args[0]);
        for (size_t i = 0; i < elems.size(); i++)
            if (valuesEqual(elems[i], args[1]))
                return Value::just(Value::integer(static_cast<int64_t>(i)));
        return Value::none();
    });

    reg("any?", [this, getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::boolean(false);
        for (const auto& elem : elems) {
            if (fn->native({elem})->isTrue()) return Value::boolean(true);
        }
        return Value::boolean(false);
    });

    reg("all?", [this, getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(true);
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::boolean(true);
        for (const auto& elem : elems) {
            if (!fn->native({elem})->isTrue()) return Value::boolean(false);
        }
        return Value::boolean(true);
    });

    auto compareVia = [this](const ValuePtr& a, const ValuePtr& b) -> std::string {
        if (auto* ai = std::get_if<IntValue>(&a->data))
            if (auto* bi = std::get_if<IntValue>(&b->data))
                return ai->value < bi->value ? "Less" : (ai->value > bi->value ? "Greater" : "Equal");
        if (auto* af = std::get_if<FloatValue>(&a->data))
            if (auto* bf = std::get_if<FloatValue>(&b->data))
                return af->value < bf->value ? "Less" : (af->value > bf->value ? "Greater" : "Equal");
        if (auto* as = std::get_if<StringValue>(&a->data))
            if (auto* bs = std::get_if<StringValue>(&b->data))
                return as->value < bs->value ? "Less" : (as->value > bs->value ? "Greater" : "Equal");
        if (auto* ac = std::get_if<CharValue>(&a->data))
            if (auto* bc = std::get_if<CharValue>(&b->data))
                return ac->value < bc->value ? "Less" : (ac->value > bc->value ? "Greater" : "Equal");
        auto dispatchCompare = [&](const std::string& typeName) -> std::string {
            try {
                auto r = callFunction(typeName + "::compare", {a, b}, {}, {});
                if (auto* var = std::get_if<VariantValue>(&r->data)) return var->tag;
            } catch (...) {}
            return "";
        };
        if (auto* var = std::get_if<VariantValue>(&a->data)) {
            auto r = dispatchCompare(var->tag);
            if (!r.empty()) return r;
        }
        if (auto* rec = std::get_if<RecordValue>(&a->data)) {
            auto r = dispatchCompare(rec->typeName);
            if (!r.empty()) return r;
        }
        return "Equal";
    };

    reg("sort", [this, getElements, compareVia](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto elems = getElements(args[0]);
        if (elems.empty()) return Value::list({});

        // 2-arg form: sort(list, comparator) where comparator(a,b) -> Bool
        if (args.size() >= 2) {
            auto* fn = std::get_if<FunctionValue>(&args[1]->data);
            if (fn && fn->native) {
                auto cmp = fn->native;
                std::stable_sort(elems.begin(), elems.end(),
                    [&cmp](const ValuePtr& a, const ValuePtr& b) {
                        return cmp({a, b})->isTrue();
                    });
                return Value::list(std::move(elems));
            }
        }

        // 1-arg form: natural order for primitives, or Comparable.compare for records
        std::stable_sort(elems.begin(), elems.end(),
            [&compareVia](const ValuePtr& a, const ValuePtr& b) {
                return compareVia(a, b) == "Less";
            });
        return Value::list(std::move(elems));
    });

    reg("min", [this, getElements, compareVia](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto elems = getElements(args[0]);
        if (elems.empty()) return Value::none();
        if (args.size() >= 2) {
            auto* fn = std::get_if<FunctionValue>(&args[1]->data);
            if (fn && fn->native) {
                auto keyOf = fn->native;
                auto result = elems[0];
                auto bestKey = keyOf({result});
                for (size_t i = 1; i < elems.size(); i++) {
                    auto k = keyOf({elems[i]});
                    if (compareVia(k, bestKey) == "Less") { result = elems[i]; bestKey = k; }
                }
                return Value::just(result);
            }
        }
        auto result = elems[0];
        for (size_t i = 1; i < elems.size(); i++)
            if (compareVia(elems[i], result) == "Less") result = elems[i];
        return Value::just(result);
    });

    reg("max", [this, getElements, compareVia](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto elems = getElements(args[0]);
        if (elems.empty()) return Value::none();
        if (args.size() >= 2) {
            auto* fn = std::get_if<FunctionValue>(&args[1]->data);
            if (fn && fn->native) {
                auto keyOf = fn->native;
                auto result = elems[0];
                auto bestKey = keyOf({result});
                for (size_t i = 1; i < elems.size(); i++) {
                    auto k = keyOf({elems[i]});
                    if (compareVia(k, bestKey) == "Greater") { result = elems[i]; bestKey = k; }
                }
                return Value::just(result);
            }
        }
        auto result = elems[0];
        for (size_t i = 1; i < elems.size(); i++)
            if (compareVia(elems[i], result) == "Greater") result = elems[i];
        return Value::just(result);
    });

    auto numericSum = [](const std::vector<ValuePtr>& elems) -> ValuePtr {
        bool hasFloat = false;
        for (const auto& e : elems)
            if (std::holds_alternative<FloatValue>(e->data)) { hasFloat = true; break; }
        if (hasFloat) {
            double total = 0.0;
            for (const auto& e : elems) {
                if (auto* f = std::get_if<FloatValue>(&e->data)) total += f->value;
                else if (auto i = asInteger(e)) total += i->get_d();
            }
            return Value::floating(total);
        }
        mpz_class total = 0;
        for (const auto& e : elems)
            if (auto i = asInteger(e)) total += *i;
        return integerResult(total);
    };

    reg("sum", [getElements, numericSum](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(0);
        auto elems = getElements(args[0]);
        if (args.size() >= 2) {
            auto* fn = std::get_if<FunctionValue>(&args[1]->data);
            if (fn && fn->native) {
                std::vector<ValuePtr> mapped;
                mapped.reserve(elems.size());
                for (const auto& e : elems) mapped.push_back(fn->native({e}));
                return numericSum(mapped);
            }
        }
        return numericSum(elems);
    });

    reg("product", [getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(1);
        auto elems = getElements(args[0]);
        std::vector<ValuePtr> mapped;
        if (args.size() >= 2) {
            auto* fn = std::get_if<FunctionValue>(&args[1]->data);
            if (fn && fn->native) {
                mapped.reserve(elems.size());
                for (const auto& e : elems) mapped.push_back(fn->native({e}));
                elems = mapped;
            }
        }
        bool hasFloat = false;
        for (const auto& e : elems)
            if (std::holds_alternative<FloatValue>(e->data)) { hasFloat = true; break; }
        if (hasFloat) {
            double total = 1.0;
            for (const auto& e : elems) {
                if (auto* f = std::get_if<FloatValue>(&e->data)) total *= f->value;
                else if (auto i = asInteger(e)) total *= i->get_d();
            }
            return Value::floating(total);
        }
        mpz_class total = 1;
        for (const auto& e : elems)
            if (auto i = asInteger(e)) total *= *i;
        return integerResult(total);
    });

    reg("flatMap", [this, getElements](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto elems = getElements(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::list({});
        std::vector<ValuePtr> result;
        for (const auto& elem : elems) {
            auto mapped = fn->native({elem});
            if (auto* inner = std::get_if<ListValue>(&mapped->data))
                for (const auto& e : inner->elements) result.push_back(e);
            else
                result.push_back(mapped);
        }
        return Value::list(std::move(result));
    });

    // inspect() — pretty-printed representation of any value (UFCS on all types)
    reg("inspect", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("()");
        return Value::string(args[0]->inspect());
    });
}

} // namespace kex::interpreter
