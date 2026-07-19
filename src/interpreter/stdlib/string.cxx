#include "../evaluator.hxx"
#include <cctype>
#include <stdexcept>

namespace kex::interpreter {

auto Evaluator::registerStringBuiltins() -> void {
    m_globalEnv->define("String", Value::module("String"));
    m_globalEnv->define("Char",   Value::module("Char"));
    m_globalEnv->define("Bool",   Value::module("Bool"));
    m_globalEnv->define("Atom",   Value::module("Atom"));

    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // String.at(i) -> Char (not a 1-char String) — Char and String are
    // interchangeable for comparisons/concatenation (see valuesEqual/+),
    // so existing code comparing the result against a string literal
    // keeps working either way.
    reg("at", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto* idx = std::get_if<IntValue>(&args[1]->data);
        if (!idx || idx->value < 0) return Value::none();
        auto i = static_cast<size_t>(idx->value);
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            return i < list->elements.size() ? list->elements[i] : Value::none();
        }
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::none();
        return i < str->value.size() ? Value::character(str->value[i]) : Value::none();
    });

    // Kex.Intrinsic.String.chars — the string's characters as a [Char].
    // Backs the prelude's String `chars` (src/prelude/string.kex).
    reg("chars", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::list({});
        std::vector<ValuePtr> elems;
        elems.reserve(str->value.size());
        for (char c : str->value) elems.push_back(Value::character(c));
        return Value::list(std::move(elems));
    });

    // c.digit? -> Bool — true for '0'..'9'. UFCS-callable on a Char.
    // Throws for a non-Char receiver rather than silently answering
    // `false` — "hello".digit? or 5.digit? are caller bugs, not "no".
    reg("digit?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("digit? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("digit? expects a Char, got " + args[0]->typeName());
        return Value::boolean(c->value >= '0' && c->value <= '9');
    });

    reg("alpha?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("alpha? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("alpha? expects a Char, got " + args[0]->typeName());
        return Value::boolean(std::isalpha(static_cast<unsigned char>(c->value)) != 0);
    });

    reg("space?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("space? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("space? expects a Char, got " + args[0]->typeName());
        return Value::boolean(std::isspace(static_cast<unsigned char>(c->value)) != 0);
    });

    reg("startsWith?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* str = std::get_if<StringValue>(&args[0]->data);
        auto* pre = std::get_if<StringValue>(&args[1]->data);
        if (!str || !pre) return Value::boolean(false);
        return Value::boolean(str->value.starts_with(pre->value));
    });

    reg("endsWith?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* str = std::get_if<StringValue>(&args[0]->data);
        auto* suf = std::get_if<StringValue>(&args[1]->data);
        if (!str || !suf) return Value::boolean(false);
        return Value::boolean(str->value.ends_with(suf->value));
    });

    reg("contains?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        // String.contains?(substr)
        auto* str = std::get_if<StringValue>(&args[0]->data);
        auto* sub = std::get_if<StringValue>(&args[1]->data);
        if (str && sub) return Value::boolean(str->value.find(sub->value) != std::string::npos);
        // [A].contains?(elem)
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (list) {
            for (const auto& elem : list->elements)
                if (valuesEqual(elem, args[1])) return Value::boolean(true);
            return Value::boolean(false);
        }
        // Range.contains?(n)
        if (auto* range = std::get_if<RangeValue>(&args[0]->data)) {
            if (auto* i = std::get_if<IntValue>(&args[1]->data))
                return Value::boolean(i->value >= range->start && i->value <= range->end);
            if (auto* c = std::get_if<CharValue>(&args[1]->data))
                return Value::boolean(range->isChar &&
                    static_cast<int64_t>(c->value) >= range->start &&
                    static_cast<int64_t>(c->value) <= range->end);
        }
        return Value::boolean(false);
    });

    reg("split", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::list({});

        std::string sep = "";
        if (args.size() >= 2) {
            if (auto* s = std::get_if<StringValue>(&args[1]->data)) sep = s->value;
        }

        std::vector<ValuePtr> parts;
        if (sep.empty()) {
            for (char c : str->value) parts.push_back(Value::string(std::string(1, c)));
        } else {
            size_t start = 0, pos;
            while ((pos = str->value.find(sep, start)) != std::string::npos) {
                parts.push_back(Value::string(str->value.substr(start, pos - start)));
                start = pos + sep.size();
            }
            parts.push_back(Value::string(str->value.substr(start)));
        }
        return Value::list(std::move(parts));
    });

    reg("trim", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::string("");
        auto s = str->value;
        auto start = s.find_first_not_of(" \t\n\r");
        auto end = s.find_last_not_of(" \t\n\r");
        if (start == std::string::npos) return Value::string("");
        return Value::string(s.substr(start, end - start + 1));
    });

    reg("upperCase", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        // Char -> Char (so map(&.upperCase) on a String round-trips back to String)
        if (auto* cv = std::get_if<CharValue>(&args[0]->data))
            return Value::character(static_cast<char>(std::toupper(cv->value)));
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::string("");
        std::string result = str->value;
        for (auto& c : result) c = static_cast<char>(std::toupper(c));
        return Value::string(result);
    });

    reg("lowerCase", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        // Char -> Char (so map(&.lowerCase) on a String round-trips back to String)
        if (auto* cv = std::get_if<CharValue>(&args[0]->data))
            return Value::character(static_cast<char>(std::tolower(cv->value)));
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::string("");
        std::string result = str->value;
        for (auto& c : result) c = static_cast<char>(std::tolower(c));
        return Value::string(result);
    });

    // Also handles List (reversing elements), since the same `reverse`
    // name is used UFCS-style on both strings and lists.
    reg("reverse", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
            std::string result(str->value.rbegin(), str->value.rend());
            return Value::string(result);
        }
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            std::vector<ValuePtr> result(list->elements.rbegin(), list->elements.rend());
            return Value::list(std::move(result));
        }
        return args[0];
    });
    // String owns these private primitive identities. `at` and `reverse`
    // also implement the representation-level List operations for String's
    // [Char] equivalence, so publish those two under List as well.
    for (const char* name : {
             "at", "chars", "contains?", "endsWith?", "lowerCase", "split",
             "startsWith?", "trim", "upperCase"}) {
        if (auto value = m_globalEnv->get(name))
            defineIntrinsic("String::" + std::string(name), value);
    }
    for (const char* name : {"at", "reverse"}) {
        if (auto value = m_globalEnv->get(name))
            defineIntrinsic("List::" + std::string(name), value);
    }
    for (const auto& [intrinsic, native] : {
             std::pair{"is_digit", "digit?"},
             std::pair{"is_alpha", "alpha?"},
             std::pair{"is_space", "space?"}}) {
        if (auto value = m_globalEnv->get(native))
            defineIntrinsic("Char::" + std::string(intrinsic), value);
    }
}

} // namespace kex::interpreter
