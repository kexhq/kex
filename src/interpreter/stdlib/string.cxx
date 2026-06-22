#include "../evaluator.hxx"
#include <cctype>

namespace kex::interpreter {

auto Evaluator::registerStringBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

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
