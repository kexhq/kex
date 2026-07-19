#include "../evaluator.hxx"
#include <cctype>
#include <stdexcept>

namespace kex::interpreter {

auto Evaluator::registerStringBuiltins() -> void {
    m_globalEnv->define("String", Value::module("String"));
    m_globalEnv->define("Char",   Value::module("Char"));
    m_globalEnv->define("Bool",   Value::module("Bool"));
    m_globalEnv->define("Atom",   Value::module("Atom"));

    auto regCharPredicate = [this](const std::string& publicName,
                                   const std::string& intrinsicName,
                                   NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{publicName, fn};
        m_globalEnv->define(publicName, val);
        defineIntrinsic("Char::" + intrinsicName, std::move(fn));
    };

    // String.at(i) -> Char (not a 1-char String) — Char and String are
    // interchangeable for comparisons/concatenation (see valuesEqual/+),
    // so existing code comparing the result against a string literal
    // keeps working either way.
    NativeFunc at = [](std::vector<ValuePtr> args) -> ValuePtr {
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
    };
    defineIntrinsic("List::at", at);
    defineIntrinsic("String::at", std::move(at));

    // Kex.Intrinsic.String.chars — the string's characters as a [Char].
    // Backs the prelude's String `chars` (src/prelude/string.kex).
    defineIntrinsic("String::chars", [](std::vector<ValuePtr> args) -> ValuePtr {
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
    regCharPredicate("digit?", "is_digit", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("digit? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("digit? expects a Char, got " + args[0]->typeName());
        return Value::boolean(c->value >= '0' && c->value <= '9');
    });

    regCharPredicate("alpha?", "is_alpha", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("alpha? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("alpha? expects a Char, got " + args[0]->typeName());
        return Value::boolean(std::isalpha(static_cast<unsigned char>(c->value)) != 0);
    });

    regCharPredicate("space?", "is_space", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("space? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("space? expects a Char, got " + args[0]->typeName());
        return Value::boolean(std::isspace(static_cast<unsigned char>(c->value)) != 0);
    });

    defineIntrinsic("String::startsWith?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* str = std::get_if<StringValue>(&args[0]->data);
        auto* pre = std::get_if<StringValue>(&args[1]->data);
        if (!str || !pre) return Value::boolean(false);
        return Value::boolean(str->value.starts_with(pre->value));
    });

    defineIntrinsic("String::endsWith?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* str = std::get_if<StringValue>(&args[0]->data);
        auto* suf = std::get_if<StringValue>(&args[1]->data);
        if (!str || !suf) return Value::boolean(false);
        return Value::boolean(str->value.ends_with(suf->value));
    });

    defineIntrinsic("String::contains?", [](std::vector<ValuePtr> args) -> ValuePtr {
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

    defineIntrinsic("String::split", [](std::vector<ValuePtr> args) -> ValuePtr {
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

    defineIntrinsic("String::trim", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::string("");
        auto s = str->value;
        auto start = s.find_first_not_of(" \t\n\r");
        auto end = s.find_last_not_of(" \t\n\r");
        if (start == std::string::npos) return Value::string("");
        return Value::string(s.substr(start, end - start + 1));
    });

    defineIntrinsic("String::upperCase", [](std::vector<ValuePtr> args) -> ValuePtr {
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

    defineIntrinsic("String::lowerCase", [](std::vector<ValuePtr> args) -> ValuePtr {
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
    defineIntrinsic("List::reverse", [](std::vector<ValuePtr> args) -> ValuePtr {
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
}

} // namespace kex::interpreter
