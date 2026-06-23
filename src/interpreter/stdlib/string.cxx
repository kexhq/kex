#include "../evaluator.hxx"
#include <cctype>
#include <stdexcept>

namespace kex::interpreter {

auto Evaluator::registerStringBuiltins() -> void {
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
        auto* str = std::get_if<StringValue>(&args[0]->data);
        auto* idx = std::get_if<IntValue>(&args[1]->data);
        if (!str || !idx) return Value::none();
        if (idx->value < 0 || static_cast<size_t>(idx->value) >= str->value.size()) {
            return Value::none();
        }
        return Value::character(str->value[static_cast<size_t>(idx->value)]);
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

    // String.chars -> [Char]
    reg("chars", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::list({});
        std::vector<ValuePtr> elems;
        elems.reserve(str->value.size());
        for (char c : str->value) elems.push_back(Value::character(c));
        return Value::list(std::move(elems));
    });

    reg("contains?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* str = std::get_if<StringValue>(&args[0]->data);
        auto* sub = std::get_if<StringValue>(&args[1]->data);
        if (str && sub) return Value::boolean(str->value.find(sub->value) != std::string::npos);
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

    reg("upcase", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::string("");
        std::string result = str->value;
        for (auto& c : result) c = static_cast<char>(std::toupper(c));
        return Value::string(result);
    });

    reg("downcase", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
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
}

} // namespace kex::interpreter
