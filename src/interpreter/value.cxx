#include "value.hxx"
#include "../common/color.hxx"
#include <algorithm>
#include <optional>

namespace kex::interpreter {

auto Value::none() -> ValuePtr {
    return std::make_shared<Value>(Value{VariantValue{"None", "Optional", {}, {"X"}, {}}});
}

auto Value::isNone() const -> bool {
    auto* v = std::get_if<VariantValue>(&data);
    return v && v->tag == "None" && v->args.empty();
}

auto Value::unit() -> ValuePtr {
    return std::make_shared<Value>(Value{UnitValue{}});
}

auto Value::integer(int64_t v) -> ValuePtr {
    return std::make_shared<Value>(Value{IntValue{v}});
}

auto Value::bigInteger(mpz_class v) -> ValuePtr {
    return std::make_shared<Value>(Value{BigIntValue{std::move(v)}});
}

auto asInteger(const ValuePtr& v) -> std::optional<mpz_class> {
    // Deliberately NOT `mpz_class(static_cast<long>(i->value))`: `long` is
    // 64-bit on every native LP64 target this project has ever built on
    // (macOS/Linux), but wasm32 has a 32-bit `long` — that cast would
    // silently truncate any
    // IntValue outside 32-bit range before it ever reached GMP. Round-
    // tripping through decimal string construction is slower but portable
    // regardless of the platform's `long` width, and Integer arithmetic
    // isn't a hot path here (the wasm build's GMP is already the slower
    // portable-C fallback, not hand-tuned assembly, for the same reason).
    if (auto* i = std::get_if<IntValue>(&v->data)) return mpz_class(std::to_string(i->value));
    if (auto* b = std::get_if<BigIntValue>(&v->data)) return b->value;
    return std::nullopt;
}

auto integerResult(mpz_class v) -> ValuePtr {
    // Same reasoning as asInteger: fits_slong_p()/get_si() are tied to the
    // platform's `long` width, not int64_t's — comparing against explicit
    // int64_t bounds (built once, via decimal string, so this doesn't
    // depend on `long` either) is the portable equivalent.
    static const mpz_class kInt64Min(std::to_string(INT64_MIN));
    static const mpz_class kInt64Max(std::to_string(INT64_MAX));
    if (v >= kInt64Min && v <= kInt64Max) {
        return Value::integer(std::stoll(v.get_str()));
    }
    return Value::bigInteger(std::move(v));
}

auto Value::floating(double v) -> ValuePtr {
    return std::make_shared<Value>(Value{FloatValue{v}});
}

auto Value::string(std::string v) -> ValuePtr {
    return std::make_shared<Value>(Value{StringValue{std::move(v)}});
}

auto Value::character(char v) -> ValuePtr {
    return std::make_shared<Value>(Value{CharValue{v}});
}

auto Value::boolean(bool v) -> ValuePtr {
    return std::make_shared<Value>(Value{BoolValue{v}});
}

auto Value::atom(std::string name) -> ValuePtr {
    return std::make_shared<Value>(Value{AtomValue{std::move(name)}});
}

auto Value::variant(std::string tag, std::string parentType, std::vector<ValuePtr> args,
                     std::vector<std::string> typeParams, std::vector<int> argParamIndex) -> ValuePtr {
    return std::make_shared<Value>(Value{VariantValue{std::move(tag), std::move(parentType), std::move(args),
                                                        std::move(typeParams), std::move(argParamIndex)}});
}

auto Value::just(ValuePtr inner) -> ValuePtr {
    return variant("Just", "Option", {std::move(inner)}, {"T"}, {0});
}

auto Value::ok(ValuePtr inner) -> ValuePtr {
    return variant("Ok", "Result", {std::move(inner)}, {"T", "E"}, {0});
}

auto Value::error(ValuePtr inner) -> ValuePtr {
    return variant("Error", "Result", {std::move(inner)}, {"T", "E"}, {1});
}

auto Value::module(std::string name) -> ValuePtr {
    return std::make_shared<Value>(Value{ModuleValue{std::move(name)}});
}

auto Value::process(uint64_t pid, class Scheduler* scheduler) -> ValuePtr {
    return std::make_shared<Value>(Value{ProcessValue{pid, scheduler}});
}

auto Value::task(uint64_t pid, class Scheduler* scheduler) -> ValuePtr {
    return std::make_shared<Value>(Value{TaskValue{pid, scheduler}});
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
    if (isNone()) return false;
    return std::visit([](const auto& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, BoolValue>) return v.value;
        if constexpr (std::is_same_v<T, UnitValue>) return false;
        return true;
    }, data);
}

auto Value::toString() const -> std::string {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, UnitValue>) return "()";
        else if constexpr (std::is_same_v<T, IntValue>) return std::to_string(v.value);
        else if constexpr (std::is_same_v<T, BigIntValue>) return v.value.get_str();
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
        else if constexpr (std::is_same_v<T, CharValue>) return std::string(1, v.value);
        else if constexpr (std::is_same_v<T, BoolValue>) return v.value ? "true" : "false";
        else if constexpr (std::is_same_v<T, AtomValue>) return ":" + v.name;
        else if constexpr (std::is_same_v<T, VariantValue>) {
            if (v.args.empty()) return v.tag;
            std::string result = v.tag + "(";
            for (size_t i = 0; i < v.args.size(); i++) {
                if (i > 0) result += ", ";
                result += v.args[i]->toString();
            }
            return result + ")";
        }
        else if constexpr (std::is_same_v<T, ModuleValue>) return v.name;
        else if constexpr (std::is_same_v<T, ProcessValue>) return "#Process<" + std::to_string(v.pid) + ">";
        else if constexpr (std::is_same_v<T, TaskValue>) return "#Task<" + std::to_string(v.pid) + ">";
        else if constexpr (std::is_same_v<T, ListValue>) {
            // A list of nothing but Chars displays as text, not as a
            // bracketed list — [Char] is meant to look like a String from
            // the language user's standpoint (see also valuesEqual/+).
            bool allChars = !v.elements.empty();
            for (const auto& el : v.elements) {
                if (!std::holds_alternative<CharValue>(el->data)) { allChars = false; break; }
            }
            if (allChars) {
                std::string result;
                for (const auto& el : v.elements) result += std::get<CharValue>(el->data).value;
                return result;
            }
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
            auto entries = v.entries;
            std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return a.first->toString() < b.first->toString();
            });
            std::string result = "{ ";
            for (size_t i = 0; i < entries.size(); i++) {
                if (i > 0) result += ", ";
                result += entries[i].first->toString() + ": " + entries[i].second->toString();
            }
            return result + " }";
        }
        else if constexpr (std::is_same_v<T, RangeValue>) {
            if (v.isChar) {
                return std::string("'") + static_cast<char>(v.start) + "'.." +
                       "'" + static_cast<char>(v.end) + "'";
            }
            return std::to_string(v.start) + ".." + std::to_string(v.end);
        }
        else if constexpr (std::is_same_v<T, StreamValue>) {
            return "<Stream>";
        }
        else if constexpr (std::is_same_v<T, FileHandleValue>) {
            return "<FileHandle: \"" + v.path + "\">";
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
            // Sorted field order: deterministic, and reproducible by the
            // BEAM backend (the unordered_map's hash order isn't).
            std::vector<std::string> keys;
            keys.reserve(v.fields.size());
            for (const auto& [key, _] : v.fields) keys.push_back(key);
            std::sort(keys.begin(), keys.end());
            std::string result = v.typeName + " { ";
            bool first = true;
            for (const auto& key : keys) {
                if (!first) result += ", ";
                result += key + ": " + v.fields.at(key)->toString();
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
        else if constexpr (std::is_same_v<T, CharValue>) {
            return "'" + std::string(1, v.value) + "'";
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
            if (v.isChar) {
                return std::string("'") + static_cast<char>(v.start) + "'.." +
                       "'" + static_cast<char>(v.end) + "'";
            }
            return std::to_string(v.start) + ".." + std::to_string(v.end);
        }
        else if constexpr (std::is_same_v<T, VariantValue>) {
            if (v.args.empty()) return v.tag;
            std::string result = v.tag + "(";
            for (size_t i = 0; i < v.args.size(); i++) {
                if (i > 0) result += ", ";
                result += v.args[i]->toRepr();
            }
            return result + ")";
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
        if constexpr (std::is_same_v<T, UnitValue>) return "Void";
        else if constexpr (std::is_same_v<T, IntValue>) return "Int";
        else if constexpr (std::is_same_v<T, BigIntValue>) return "Integer";
        else if constexpr (std::is_same_v<T, FloatValue>) return "Float";
        else if constexpr (std::is_same_v<T, StringValue>) return "String";
        else if constexpr (std::is_same_v<T, CharValue>) return "Char";
        else if constexpr (std::is_same_v<T, BoolValue>) return "Bool";
        else if constexpr (std::is_same_v<T, AtomValue>) return "Atom";
        else if constexpr (std::is_same_v<T, VariantValue>) {
            std::string base = v.parentType.empty() ? v.tag : v.parentType;
            if (v.typeParams.empty()) return base;
            std::vector<std::string> resolved(v.typeParams.size(), "?");
            bool anyResolved = false;
            for (size_t i = 0; i < v.args.size() && i < v.argParamIndex.size(); i++) {
                int pi = v.argParamIndex[i];
                if (pi >= 0 && static_cast<size_t>(pi) < resolved.size()) {
                    resolved[static_cast<size_t>(pi)] = v.args[i]->typeName();
                    anyResolved = true;
                }
            }
            // Zero-arg variant of a generic type (e.g. Nothing) carries no
            // payload to infer params from — show the bare type name.
            if (!anyResolved) return base;
            std::string result = base + "<";
            for (size_t i = 0; i < resolved.size(); i++) {
                if (i) result += ", ";
                result += resolved[i];
            }
            return result + ">";
        }
        else if constexpr (std::is_same_v<T, ModuleValue>) return "Module";
        else if constexpr (std::is_same_v<T, ProcessValue>) return "Process";
        else if constexpr (std::is_same_v<T, TaskValue>) return "Task";
        else if constexpr (std::is_same_v<T, ListValue>) {
            if (v.elements.empty()) return "List";
            return "[" + v.elements.front()->typeName() + "]";
        }
        else if constexpr (std::is_same_v<T, TupleValue>) return "Tuple";
        else if constexpr (std::is_same_v<T, MapValue>) return "Map";
        else if constexpr (std::is_same_v<T, RangeValue>) return "Range";
        else if constexpr (std::is_same_v<T, StreamValue>) return "Stream";
        else if constexpr (std::is_same_v<T, FileHandleValue>) return "FileHandle";
        else if constexpr (std::is_same_v<T, RecordValue>) return v.typeName;
        else if constexpr (std::is_same_v<T, FunctionValue>) return "Function";
        else if constexpr (std::is_same_v<T, LambdaValue>) return "Lambda";
        else return "Unknown";
    }, data);
}

// String, Char, and [Char] (Char-list) are meant to be interchangeable from
// `[Char]` (a list of Char) *is* String — same type, fully interchangeable
// — but a bare Char is its own distinct type, not a 1-character String.
// Returns text content for StringValue or a ListValue whose elements are
// *all* CharValue; nullopt for anything else, including a bare CharValue
// (so e.g. 'a' == "a" is correctly false — see textContent below for the
// broader version that does include Char, used by + and toString()).
auto stringOrCharListText(const ValuePtr& v) -> std::optional<std::string> {
    if (auto* s = std::get_if<StringValue>(&v->data)) return s->value;
    if (auto* l = std::get_if<ListValue>(&v->data)) {
        std::string out;
        for (const auto& el : l->elements) {
            auto* ec = std::get_if<CharValue>(&el->data);
            if (!ec) return std::nullopt;
            out += ec->value;
        }
        return out;
    }
    return std::nullopt;
}

// Like stringOrCharListText, but also renders a bare Char as text — for
// "what text would concatenating/printing this produce" (+ and toString()),
// not "is this the same type as String" (valuesEqual, pattern matching).
auto textContent(const ValuePtr& v) -> std::optional<std::string> {
    if (auto* c = std::get_if<CharValue>(&v->data)) return std::string(1, c->value);
    return stringOrCharListText(v);
}

auto valuesEqual(const ValuePtr& a, const ValuePtr& b) -> bool {
    if (!a && !b) return true;
    if (!a || !b) return false;

    if (std::holds_alternative<StringValue>(a->data) || std::holds_alternative<StringValue>(b->data)) {
        auto at = stringOrCharListText(a);
        auto bt = stringOrCharListText(b);
        if (at && bt) return *at == *bt;
    }

    // IntValue and BigIntValue are both just `Integer` — compare across
    // representations rather than requiring an exact variant match (the
    // generic std::visit below only compares same-variant).
    if (auto av = asInteger(a)) {
        if (auto bv = asInteger(b)) return *av == *bv;
    }

    return std::visit([&b](const auto& av) -> bool {
        using AT = std::decay_t<decltype(av)>;
        auto* bv = std::get_if<AT>(&b->data);
        if (!bv) return false;

        if constexpr (std::is_same_v<AT, IntValue>) return av.value == bv->value;
        else if constexpr (std::is_same_v<AT, FloatValue>) return av.value == bv->value;
        else if constexpr (std::is_same_v<AT, StringValue>) return av.value == bv->value;
        else if constexpr (std::is_same_v<AT, CharValue>) return av.value == bv->value;
        else if constexpr (std::is_same_v<AT, BoolValue>) return av.value == bv->value;
        else if constexpr (std::is_same_v<AT, AtomValue>) return av.name == bv->name;
        else if constexpr (std::is_same_v<AT, ProcessValue>) return av.pid == bv->pid;
        else if constexpr (std::is_same_v<AT, TaskValue>) return av.pid == bv->pid;
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
        else if constexpr (std::is_same_v<AT, MapValue>) {
            if (av.entries.size() != bv->entries.size()) return false;
            for (const auto& [key, val] : av.entries) {
                bool found = false;
                for (const auto& [bkey, bval] : bv->entries) {
                    if (valuesEqual(key, bkey)) {
                        if (!valuesEqual(val, bval)) return false;
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<AT, VariantValue>) {
            if (av.tag != bv->tag) return false;
            if (av.args.size() != bv->args.size()) return false;
            for (size_t i = 0; i < av.args.size(); i++) {
                if (!valuesEqual(av.args[i], bv->args[i])) return false;
            }
            return true;
        }
        else if constexpr (std::is_same_v<AT, RecordValue>) {
            // Structural equality — same type, same fields. Without this,
            // `==` on any record/ADT value (Just(x) == Just(y), Ok(x) ==
            // Ok(y), two instances of a user record, ...) always falls
            // through to `false` here, silently breaking every comparison
            // that isn't covered by an explicit operator overload.
            if (av.typeName != bv->typeName) return false;
            if (av.fields.size() != bv->fields.size()) return false;
            for (const auto& [key, val] : av.fields) {
                auto it = bv->fields.find(key);
                if (it == bv->fields.end()) return false;
                if (!valuesEqual(val, it->second)) return false;
            }
            return true;
        }
        else return false;
    }, a->data);
}

auto Value::inspect() const -> std::string {
    using namespace kex::color;
    auto c = [](const char* code) -> const char* {
        return apply(code);
    };

    std::function<std::string(const Value&)> rec = [&](const Value& v) -> std::string {
        return std::visit([&](const auto& node) -> std::string {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, UnitValue>)
                return std::string(c(gray)) + "()" + c(reset);
            else if constexpr (std::is_same_v<T, IntValue>)
                return std::string(c(yellow)) + std::to_string(node.value) + c(reset);
            else if constexpr (std::is_same_v<T, BigIntValue>)
                return std::string(c(yellow)) + node.value.get_str() + c(reset);
            else if constexpr (std::is_same_v<T, FloatValue>) {
                auto s = std::to_string(node.value);
                auto dot = s.find('.');
                if (dot != std::string::npos) {
                    auto last = s.find_last_not_of('0');
                    if (last == dot) last++;
                    s = s.substr(0, last + 1);
                }
                return std::string(c(yellow)) + s + c(reset);
            }
            else if constexpr (std::is_same_v<T, StringValue>)
                return std::string(c(green)) + "\"" + node.value + "\"" + c(reset);
            else if constexpr (std::is_same_v<T, CharValue>)
                return std::string(c(green)) + "'" + std::string(1, node.value) + "'" + c(reset);
            else if constexpr (std::is_same_v<T, BoolValue>)
                return std::string(c(magenta)) + (node.value ? "true" : "false") + c(reset);
            else if constexpr (std::is_same_v<T, AtomValue>)
                return std::string(c(purple)) + ":" + node.name + c(reset);
            else if constexpr (std::is_same_v<T, VariantValue>) {
                if (node.args.empty())
                    return std::string(c(cyan)) + node.tag + c(reset);
                std::string result = std::string(c(cyan)) + node.tag + c(reset) + "(";
                for (size_t i = 0; i < node.args.size(); i++) {
                    if (i > 0) result += ", ";
                    result += rec(*node.args[i]);
                }
                return result + ")";
            }
            else if constexpr (std::is_same_v<T, ModuleValue>)
                return std::string(c(cyan)) + node.name + c(reset);
            else if constexpr (std::is_same_v<T, ProcessValue>)
                return std::string(c(gray)) + "#Process<" + std::to_string(node.pid) + ">" + c(reset);
            else if constexpr (std::is_same_v<T, TaskValue>)
                return std::string(c(gray)) + "#Task<" + std::to_string(node.pid) + ">" + c(reset);
            else if constexpr (std::is_same_v<T, ListValue>) {
                bool allChars = !node.elements.empty();
                for (const auto& el : node.elements)
                    if (!std::holds_alternative<CharValue>(el->data)) { allChars = false; break; }
                if (allChars) {
                    std::string s;
                    for (const auto& el : node.elements) s += std::get<CharValue>(el->data).value;
                    return std::string(c(green)) + "\"" + s + "\"" + c(reset);
                }
                std::string result = "[";
                for (size_t i = 0; i < node.elements.size(); i++) {
                    if (i > 0) result += ", ";
                    result += rec(*node.elements[i]);
                }
                return result + "]";
            }
            else if constexpr (std::is_same_v<T, TupleValue>) {
                std::string result = "(";
                for (size_t i = 0; i < node.elements.size(); i++) {
                    if (i > 0) result += ", ";
                    result += rec(*node.elements[i]);
                }
                return result + ")";
            }
            else if constexpr (std::is_same_v<T, MapValue>) {
                std::string result = "{ ";
                for (size_t i = 0; i < node.entries.size(); i++) {
                    if (i > 0) result += ", ";
                    result += rec(*node.entries[i].first) + ": " + rec(*node.entries[i].second);
                }
                return result + " }";
            }
            else if constexpr (std::is_same_v<T, RangeValue>) {
                if (node.isChar) {
                    return std::string(c(green)) + "'" + static_cast<char>(node.start) + "'" + c(reset)
                           + ".." + std::string(c(green)) + "'" + static_cast<char>(node.end) + "'" + c(reset);
                }
                return std::string(c(yellow)) + std::to_string(node.start) + c(reset)
                       + ".." + std::string(c(yellow)) + std::to_string(node.end) + c(reset);
            }
            else if constexpr (std::is_same_v<T, StreamValue>)
                return std::string(c(gray)) + "<Stream>" + c(reset);
            else if constexpr (std::is_same_v<T, FileHandleValue>)
                return std::string(c(gray)) + "<FileHandle: \"" + node.path + "\">" + c(reset);
            else if constexpr (std::is_same_v<T, RecordValue>) {
                bool positional = !node.fields.empty();
                for (size_t i = 0; positional && i < node.fields.size(); i++)
                    if (node.fields.find(std::to_string(i)) == node.fields.end()) positional = false;
                if (positional) {
                    std::string result = std::string(c(cyan)) + node.typeName + c(reset) + "(";
                    for (size_t i = 0; i < node.fields.size(); i++) {
                        if (i > 0) result += ", ";
                        result += rec(*node.fields.at(std::to_string(i)));
                    }
                    return result + ")";
                }
                std::string result = std::string(c(cyan)) + node.typeName + c(reset) + " { ";
                bool first = true;
                for (const auto& [key, val] : node.fields) {
                    if (!first) result += ", ";
                    result += key + ": " + rec(*val);
                    first = false;
                }
                return result + " }";
            }
            else if constexpr (std::is_same_v<T, FunctionValue>)
                return std::string(c(gray)) + "<function:" + node.name + ">" + c(reset);
            else if constexpr (std::is_same_v<T, LambdaValue>)
                return std::string(c(gray)) + "<lambda>" + c(reset);
            else return "?";
        }, v.data);
    };
    return rec(*this);
}

auto dispatchTypeName(const ValuePtr& v) -> std::string {
    return std::visit([](const auto& d) -> std::string {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, RecordValue>) return d.typeName;
        else if constexpr (std::is_same_v<T, VariantValue>) return d.tag;
        else if constexpr (std::is_same_v<T, ListValue>) return "List";
        else if constexpr (std::is_same_v<T, MapValue>) return "Map";
        else if constexpr (std::is_same_v<T, FileHandleValue>) return "FileHandle";
        else if constexpr (std::is_same_v<T, ProcessValue>) return "Pid";
        else if constexpr (std::is_same_v<T, IntValue> || std::is_same_v<T, BigIntValue>) return "Integer";
        else if constexpr (std::is_same_v<T, FloatValue>) return "Float";
        else if constexpr (std::is_same_v<T, BoolValue>) return "Bool";
        else if constexpr (std::is_same_v<T, CharValue>) return "Char";
        else if constexpr (std::is_same_v<T, StringValue>) return "String";
        else if constexpr (std::is_same_v<T, RangeValue>) return "Range";
        else if constexpr (std::is_same_v<T, StreamValue>) return "Stream";
        else return "";
    }, v->data);
}

auto matchesTypeName(const std::string& name, const ValuePtr& v) -> bool {
    return std::visit([&name](const auto& d) -> bool {
        using T = std::decay_t<decltype(d)>;
        if constexpr (std::is_same_v<T, StringValue>) return name == "String";
        else if constexpr (std::is_same_v<T, IntValue> || std::is_same_v<T, BigIntValue>)
            return name == "Int" || name == "Integer";
        else if constexpr (std::is_same_v<T, FloatValue>) return name == "Float";
        else if constexpr (std::is_same_v<T, BoolValue>) return name == "Bool";
        else if constexpr (std::is_same_v<T, AtomValue>) return name == "Atom";
        else if constexpr (std::is_same_v<T, CharValue>) return name == "Char";
        else if constexpr (std::is_same_v<T, ListValue>) return name == "List";
        else if constexpr (std::is_same_v<T, TupleValue>) return name == "Tuple";
        else if constexpr (std::is_same_v<T, MapValue>) return name == "Map";
        else if constexpr (std::is_same_v<T, RangeValue>) return name == "Range";
        else if constexpr (std::is_same_v<T, StreamValue>) return name == "Stream";
        else return false;
    }, v->data);
}

auto builtinTypeNames() -> const std::unordered_set<std::string>& {
    static const std::unordered_set<std::string> names = {
        "Integer", "Float", "Char", "Bool", "Number", "String",
        "List", "Map", "Range", "Optional", "Result"};
    return names;
}

auto defaultEvalAllowList() -> const std::vector<std::string>& {
    static const std::vector<std::string> allow = {
        "Math", "List", "String", "Integer", "Map", "Stream"};
    return allow;
}

} // namespace kex::interpreter
