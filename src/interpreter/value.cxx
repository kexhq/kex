#include "value.hxx"

namespace kex::interpreter {

auto Value::none() -> ValuePtr {
    return std::make_shared<Value>(Value{NoneValue{}});
}

auto Value::integer(int64_t v) -> ValuePtr {
    return std::make_shared<Value>(Value{IntValue{v}});
}

auto Value::floating(double v) -> ValuePtr {
    return std::make_shared<Value>(Value{FloatValue{v}});
}

auto Value::string(std::string v) -> ValuePtr {
    return std::make_shared<Value>(Value{StringValue{std::move(v)}});
}

auto Value::boolean(bool v) -> ValuePtr {
    return std::make_shared<Value>(Value{BoolValue{v}});
}

auto Value::atom(std::string name) -> ValuePtr {
    return std::make_shared<Value>(Value{AtomValue{std::move(name)}});
}

auto Value::list(std::vector<ValuePtr> elems) -> ValuePtr {
    return std::make_shared<Value>(Value{ListValue{std::move(elems)}});
}

auto Value::tuple(std::vector<ValuePtr> elems) -> ValuePtr {
    return std::make_shared<Value>(Value{TupleValue{std::move(elems)}});
}

auto Value::record(std::string type, std::unordered_map<std::string, ValuePtr> fields) -> ValuePtr {
    return std::make_shared<Value>(Value{RecordValue{std::move(type), std::move(fields)}});
}

auto Value::isTrue() const -> bool {
    return std::visit([](const auto& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, BoolValue>) return v.value;
        if constexpr (std::is_same_v<T, NoneValue>) return false;
        return true;
    }, data);
}

auto Value::toString() const -> std::string {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, NoneValue>) return "None";
        else if constexpr (std::is_same_v<T, IntValue>) return std::to_string(v.value);
        else if constexpr (std::is_same_v<T, FloatValue>) {
            auto s = std::to_string(v.value);
            // Remove trailing zeros but keep at least one decimal
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                auto last = s.find_last_not_of('0');
                if (last == dot) last++;
                s = s.substr(0, last + 1);
            }
            return s;
        }
        else if constexpr (std::is_same_v<T, StringValue>) return v.value;
        else if constexpr (std::is_same_v<T, BoolValue>) return v.value ? "true" : "false";
        else if constexpr (std::is_same_v<T, AtomValue>) return ":" + v.name;
        else if constexpr (std::is_same_v<T, ListValue>) {
            std::string result = "[";
            for (size_t i = 0; i < v.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += v.elements[i]->toString();
            }
            return result + "]";
        }
        else if constexpr (std::is_same_v<T, TupleValue>) {
            std::string result = "(";
            for (size_t i = 0; i < v.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += v.elements[i]->toString();
            }
            return result + ")";
        }
        else if constexpr (std::is_same_v<T, MapValue>) {
            std::string result = "{ ";
            for (size_t i = 0; i < v.entries.size(); i++) {
                if (i > 0) result += ", ";
                result += v.entries[i].first->toString() + ": " + v.entries[i].second->toString();
            }
            return result + " }";
        }
        else if constexpr (std::is_same_v<T, RangeValue>) {
            return std::to_string(v.start) + ".." + std::to_string(v.end);
        }
        else if constexpr (std::is_same_v<T, StreamValue>) {
            return "<Stream>";
        }
        else if constexpr (std::is_same_v<T, RecordValue>) {
            // Positional constructor (Just(x), Ok(x), Number(n), ...): fields
            // keyed "0", "1", ... — print as Name(v0, v1, ...) in index order
            // rather than the unordered_map's unspecified iteration order.
            bool positional = !v.fields.empty();
            for (size_t i = 0; positional && i < v.fields.size(); i++) {
                if (v.fields.find(std::to_string(i)) == v.fields.end()) positional = false;
            }
            if (positional) {
                std::string result = v.typeName + "(";
                for (size_t i = 0; i < v.fields.size(); i++) {
                    if (i > 0) result += ", ";
                    result += v.fields.at(std::to_string(i))->toString();
                }
                return result + ")";
            }
            std::string result = v.typeName + " { ";
            bool first = true;
            for (const auto& [key, val] : v.fields) {
                if (!first) result += ", ";
                result += key + ": " + val->toString();
                first = false;
            }
            return result + " }";
        }
        else if constexpr (std::is_same_v<T, FunctionValue>) return "<function:" + v.name + ">";
        else if constexpr (std::is_same_v<T, LambdaValue>) return "<lambda>";
        else return "?";
    }, data);
}

auto Value::toRepr() const -> std::string {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, StringValue>) {
            return "\"" + v.value + "\"";
        }
        else if constexpr (std::is_same_v<T, ListValue>) {
            std::string result = "[";
            for (size_t i = 0; i < v.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += v.elements[i]->toRepr();
            }
            return result + "]";
        }
        else if constexpr (std::is_same_v<T, TupleValue>) {
            std::string result = "(";
            for (size_t i = 0; i < v.elements.size(); i++) {
                if (i > 0) result += ", ";
                result += v.elements[i]->toRepr();
            }
            return result + ")";
        }
        else if constexpr (std::is_same_v<T, MapValue>) {
            std::string result = "{ ";
            for (size_t i = 0; i < v.entries.size(); i++) {
                if (i > 0) result += ", ";
                result += v.entries[i].first->toRepr() + ": " + v.entries[i].second->toRepr();
            }
            return result + " }";
        }
        else if constexpr (std::is_same_v<T, RangeValue>) {
            return std::to_string(v.start) + ".." + std::to_string(v.end);
        }
        else if constexpr (std::is_same_v<T, RecordValue>) {
            bool positional = !v.fields.empty();
            for (size_t i = 0; positional && i < v.fields.size(); i++) {
                if (v.fields.find(std::to_string(i)) == v.fields.end()) positional = false;
            }
            if (positional) {
                std::string result = v.typeName + "(";
                for (size_t i = 0; i < v.fields.size(); i++) {
                    if (i > 0) result += ", ";
                    result += v.fields.at(std::to_string(i))->toRepr();
                }
                return result + ")";
            }
            std::string result = v.typeName + " { ";
            bool first = true;
            for (const auto& [key, val] : v.fields) {
                if (!first) result += ", ";
                result += key + ": " + val->toRepr();
                first = false;
            }
            return result + " }";
        }
        else {
            // For non-string types, toRepr == toString
            Value tmp;
            tmp.data = v;
            return tmp.toString();
        }
    }, data);
}

auto Value::typeName() const -> std::string {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, NoneValue>) return "None";
        else if constexpr (std::is_same_v<T, IntValue>) return "Int";
        else if constexpr (std::is_same_v<T, FloatValue>) return "Float";
        else if constexpr (std::is_same_v<T, StringValue>) return "String";
        else if constexpr (std::is_same_v<T, BoolValue>) return "Bool";
        else if constexpr (std::is_same_v<T, AtomValue>) return "Atom";
        else if constexpr (std::is_same_v<T, ListValue>) return "List";
        else if constexpr (std::is_same_v<T, TupleValue>) return "Tuple";
        else if constexpr (std::is_same_v<T, MapValue>) return "Map";
        else if constexpr (std::is_same_v<T, RangeValue>) return "Range";
        else if constexpr (std::is_same_v<T, StreamValue>) return "Stream";
        else if constexpr (std::is_same_v<T, RecordValue>) return v.typeName;
        else if constexpr (std::is_same_v<T, FunctionValue>) return "Function";
        else if constexpr (std::is_same_v<T, LambdaValue>) return "Lambda";
        else return "Unknown";
    }, data);
}

auto valuesEqual(const ValuePtr& a, const ValuePtr& b) -> bool {
    if (!a && !b) return true;
    if (!a || !b) return false;

    return std::visit([&b](const auto& av) -> bool {
        using AT = std::decay_t<decltype(av)>;
        auto* bv = std::get_if<AT>(&b->data);
        if (!bv) return false;

        if constexpr (std::is_same_v<AT, NoneValue>) return true;
        else if constexpr (std::is_same_v<AT, IntValue>) return av.value == bv->value;
        else if constexpr (std::is_same_v<AT, FloatValue>) return av.value == bv->value;
        else if constexpr (std::is_same_v<AT, StringValue>) return av.value == bv->value;
        else if constexpr (std::is_same_v<AT, BoolValue>) return av.value == bv->value;
        else if constexpr (std::is_same_v<AT, AtomValue>) return av.name == bv->name;
        else if constexpr (std::is_same_v<AT, ListValue>) {
            if (av.elements.size() != bv->elements.size()) return false;
            for (size_t i = 0; i < av.elements.size(); i++) {
                if (!valuesEqual(av.elements[i], bv->elements[i])) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<AT, TupleValue>) {
            if (av.elements.size() != bv->elements.size()) return false;
            for (size_t i = 0; i < av.elements.size(); i++) {
                if (!valuesEqual(av.elements[i], bv->elements[i])) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<AT, RangeValue>) {
            return av.start == bv->start && av.end == bv->end;
        }
        else return false;
    }, a->data);
}

} // namespace kex::interpreter
