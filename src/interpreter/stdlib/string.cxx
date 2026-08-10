#include "../evaluator.hxx"
#include "../../common/utf8.hxx"
#include "regex_support.hxx"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace kex::interpreter {

auto Evaluator::registerStringBuiltins() -> void {
    defineModule("String");
    defineModule("Char");
    defineModule("Bool");
    defineModule("Atom");

    auto regCharPredicate = [this](const std::string& publicName,
                                   const std::string& intrinsicName,
                                   NativeFunc fn) {
        definePublic(publicName, fn);
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
        auto cps = utf8::decode(str->value);
        return i < cps.size() ? Value::character(cps[i]) : Value::none();
    };
    defineIntrinsic("List::at", std::move(at));

    // Kex.Intrinsic.String.chars — the string's characters as a [Char].
    // Backs the prelude's String `chars` (src/stdlib/string.kex).
    defineIntrinsic("String::chars", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::list({});
        auto cps = utf8::decode(str->value);
        std::vector<ValuePtr> elems;
        elems.reserve(cps.size());
        for (auto cp : cps) elems.push_back(Value::character(cp));
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

    regCharPredicate("letter?", "is_letter", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("letter? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("letter? expects a Char, got " + args[0]->typeName());
        return Value::boolean(std::isalpha(static_cast<unsigned char>(c->value)) != 0);
    });

    regCharPredicate("upper?", "is_upper", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("upper? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("upper? expects a Char, got " + args[0]->typeName());
        return Value::boolean(std::isupper(static_cast<unsigned char>(c->value)) != 0);
    });

    regCharPredicate("lower?", "is_lower", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("lower? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("lower? expects a Char, got " + args[0]->typeName());
        return Value::boolean(std::islower(static_cast<unsigned char>(c->value)) != 0);
    });

    regCharPredicate("space?", "is_space", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) throw std::runtime_error("space? expects a Char, got no argument");
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) throw std::runtime_error("space? expects a Char, got " + args[0]->typeName());
        return Value::boolean(std::isspace(static_cast<unsigned char>(c->value)) != 0);
    });

    defineIntrinsic("Char::codepoint", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(0);
        auto* c = std::get_if<CharValue>(&args[0]->data);
        if (!c) return Value::integer(0);
        return Value::integer(static_cast<unsigned char>(c->value));
    });

    defineIntrinsic("String::fromCodepoint", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* value = std::get_if<IntValue>(&args[0]->data);
        if (!value) return Value::none();
        const auto cp = value->value;
        if (cp < 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return Value::none();

        std::string result;
        if (cp <= 0x7f) {
            result.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7ff) {
            result.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            result.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
        return Value::just(Value::string(std::move(result)));
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

        // A Regex separator dispatches to the regex engine, so `str.split(re)`
        // and `str.split(",")` share this one UFCS name. Constructing a Regex
        // requires `using Regex`, so this path is unreachable without it.
        if (args.size() >= 2 && regexsupport::isRegex(args[1])) {
            int64_t limit = 0;
            if (args.size() >= 3)
                if (auto* i = std::get_if<IntValue>(&args[2]->data)) limit = i->value;
            if (auto fields = regexsupport::split(str->value, args[1], limit))
                return Value::list(std::move(*fields));
            return Value::list({});
        }

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

    defineIntrinsic("String::replace", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 3) return Value::string("");
        auto* source = std::get_if<StringValue>(&args[0]->data);
        auto* pattern = std::get_if<StringValue>(&args[1]->data);
        auto* replacement = std::get_if<StringValue>(&args[2]->data);
        if (!source || !pattern || !replacement) return Value::string("");

        std::string result;
        if (pattern->value.empty()) {
            // Ruby-style empty-pattern replacement: insert at every Unicode
            // scalar boundary, including before the first and after the last.
            result.reserve(source->value.size() +
                           replacement->value.size() * (source->value.size() + 1));
            result += replacement->value;
            for (size_t offset = 0; offset < source->value.size();) {
                const auto lead =
                    static_cast<unsigned char>(source->value[offset]);
                size_t width = lead < 0x80 ? 1
                             : (lead & 0xE0) == 0xC0 ? 2
                             : (lead & 0xF0) == 0xE0 ? 3
                             : (lead & 0xF8) == 0xF0 ? 4
                             : 1;
                width = std::min(width, source->value.size() - offset);
                result.append(source->value, offset, width);
                result += replacement->value;
                offset += width;
            }
            return Value::string(std::move(result));
        }

        size_t start = 0;
        while (true) {
            auto match = source->value.find(pattern->value, start);
            if (match == std::string::npos) {
                result.append(source->value, start);
                break;
            }
            result.append(source->value, start, match - start);
            result += replacement->value;
            start = match + pattern->value.size();
        }
        return Value::string(std::move(result));
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
            return Value::character(utf8::toUpper(cv->value));
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::string("");
        return Value::string(utf8::toUpper(str->value));
    });

    defineIntrinsic("String::lowerCase", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("");
        // Char -> Char (so map(&.lowerCase) on a String round-trips back to String)
        if (auto* cv = std::get_if<CharValue>(&args[0]->data))
            return Value::character(utf8::toLower(cv->value));
        auto* str = std::get_if<StringValue>(&args[0]->data);
        if (!str) return Value::string("");
        return Value::string(utf8::toLower(str->value));
    });

    // Also handles List (reversing elements), since the same `reverse`
    // name is used UFCS-style on both strings and lists.
    defineIntrinsic("List::reverse", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
            // By codepoint — reversing bytes would shred multi-byte characters.
            auto cps = utf8::decode(str->value);
            std::reverse(cps.begin(), cps.end());
            return Value::string(utf8::encodeAll(cps));
        }
        if (auto* list = std::get_if<ListValue>(&args[0]->data)) {
            std::vector<ValuePtr> result(list->elements.rbegin(), list->elements.rend());
            return Value::list(std::move(result));
        }
        return args[0];
    });
}

} // namespace kex::interpreter
