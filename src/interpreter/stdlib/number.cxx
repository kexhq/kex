#include "../evaluator.hxx"
#include <cmath>
#include <stdexcept>

namespace kex::interpreter {

auto Evaluator::registerNumberBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // asInteger/integerResult (value.hxx) treat IntValue and BigIntValue
    // uniformly, so modulo/abs/even?/odd? work the same on a value that's
    // overflowed into the bignum representation as on a plain Int.
    reg("modulo", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::integer(0);
        auto a = asInteger(args[0]);
        auto b = asInteger(args[1]);
        if (a && b && *b != 0) return integerResult(*a % *b);
        return Value::integer(0);
    });

    reg("abs", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(0);
        if (auto i = asInteger(args[0])) return integerResult(abs(*i));
        if (auto* f = std::get_if<FloatValue>(&args[0]->data))
            return Value::floating(f->value < 0 ? -f->value : f->value);
        return args[0];
    });

    reg("sqrt", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::floating(0.0);
        if (auto* i = std::get_if<IntValue>(&args[0]->data)) return Value::floating(std::sqrt(static_cast<double>(i->value)));
        if (auto* f = std::get_if<FloatValue>(&args[0]->data))
            return Value::floating(std::sqrt(f->value));
        return Value::floating(0.0);
    });

    // n.times { |i| ... } — runs the block n times, passing the index 0..n-1.
    reg("times", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return args.empty() ? Value::none() : args[0];
        auto* n = std::get_if<IntValue>(&args[0]->data);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!n || !fn || !fn->native) return args[0];
        for (int64_t i = 0; i < n->value; i++) {
            fn->native({Value::integer(i)});
        }
        return args[0];
    });

    // n.in?(range) / c.in?(range) — inclusive range membership for an Int
    // or Char receiver against an Int or Char range.
    reg("in?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* range = std::get_if<RangeValue>(&args[1]->data);
        if (!range) return Value::boolean(false);
        if (auto* i = std::get_if<IntValue>(&args[0]->data)) {
            return Value::boolean(i->value >= range->start && i->value <= range->end);
        }
        if (auto* c = std::get_if<CharValue>(&args[0]->data)) {
            auto code = static_cast<int64_t>(static_cast<unsigned char>(c->value));
            return Value::boolean(code >= range->start && code <= range->end);
        }
        return Value::boolean(false);
    });

    reg("even?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto i = asInteger(args[0]);
        return Value::boolean(i.has_value() && mpz_even_p(i->get_mpz_t()) != 0);
    });

    reg("odd?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto i = asInteger(args[0]);
        return Value::boolean(i.has_value() && mpz_odd_p(i->get_mpz_t()) != 0);
    });

    // Float.parse(s) / Integer.parse(s) -> Result<Float|Int, ParseError> —
    // namespaced under the primitive type, same convention as Math.sqrt
    // etc. The Error payload is a typed ParseError record {input, position,
    // value, message, rest} — on a partial match (trailing characters) the
    // `value` field carries what was parsed so the caller can inspect it
    // without re-parsing. Complete failure stores Value::none() for value.
    // Built directly as RecordValue rather than calling the registered
    // constructors (those aren't wired for native-to-native calls).
    auto parseError = [](const std::string& input, size_t position,
                         ValuePtr value,
                         const std::string& message,
                         const std::string& rest) -> ValuePtr {
        return Value::record("ParseError", {
            {"input",    Value::string(input)},
            {"position", Value::integer(static_cast<int64_t>(position))},
            {"value",    value},
            {"message",  Value::string(message)},
            {"rest",     Value::string(rest)},
        });
    };
    auto noVal = []() { return Value::none(); };  // no value parsed

    m_globalEnv->define("Float", Value::module("Float"));
    reg("Float::parse", [parseError, noVal](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s) return Value::error(parseError("", 0, noVal(), "Float.parse expects a String", ""));
        size_t consumed = 0;
        try {
            double v = std::stod(s->value, &consumed);
            if (consumed != s->value.size())
                return Value::error(parseError(s->value, consumed, Value::floating(v),
                                               "trailing characters after float",
                                               s->value.substr(consumed)));
            return Value::ok(Value::floating(v));
        } catch (const std::exception&) {
            return Value::error(parseError(s->value, consumed, noVal(), "invalid float",
                                           s->value.substr(consumed)));
        }
    });

    // Float.parsePrefix(s) -> Just((Float, String)) | None
    reg("Float::parsePrefix", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s) return Value::none();
        size_t consumed = 0;
        try {
            double v = std::stod(s->value, &consumed);
            return Value::just(Value::tuple({Value::floating(v), Value::string(s->value.substr(consumed))}));
        } catch (const std::exception&) {
            return Value::none();
        }
    });

    m_globalEnv->define("Integer", Value::module("Integer"));
    reg("Integer::parse", [parseError, noVal](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s) return Value::error(parseError("", 0, noVal(), "Integer.parse expects a String", ""));
        size_t consumed = 0;
        try {
            int64_t v = std::stoll(s->value, &consumed);
            if (consumed != s->value.size())
                return Value::error(parseError(s->value, consumed, Value::integer(v),
                                               "trailing characters after integer",
                                               s->value.substr(consumed)));
            return Value::ok(Value::integer(v));
        } catch (const std::out_of_range&) {
            ValuePtr prefixVal = noVal();
            if (consumed > 0) {
                try { prefixVal = integerResult(mpz_class(s->value.substr(0, consumed))); }
                catch (...) {}
            }
            try {
                return Value::ok(integerResult(mpz_class(s->value)));
            } catch (const std::exception&) {
                return Value::error(parseError(s->value, consumed, prefixVal, "invalid integer",
                                               s->value.substr(consumed)));
            }
        } catch (const std::exception&) {
            return Value::error(parseError(s->value, consumed, noVal(), "invalid integer",
                                           s->value.substr(consumed)));
        }
    });

    // Integer.parsePrefix(s) -> Just((Integer, String)) | None
    reg("Integer::parsePrefix", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s) return Value::none();
        size_t consumed = 0;
        try {
            int64_t v = std::stoll(s->value, &consumed);
            return Value::just(Value::tuple({Value::integer(v), Value::string(s->value.substr(consumed))}));
        } catch (const std::out_of_range&) {
            size_t i = 0;
            if (i < s->value.size() && (s->value[i] == '+' || s->value[i] == '-')) i++;
            size_t d = i;
            while (d < s->value.size() && s->value[d] >= '0' && s->value[d] <= '9') d++;
            if (d == i) return Value::none();
            try {
                auto big = mpz_class(s->value.substr(0, d));
                return Value::just(Value::tuple({integerResult(big), Value::string(s->value.substr(d))}));
            } catch (const std::exception&) { return Value::none(); }
        } catch (const std::exception&) { return Value::none(); }
    });

    m_globalEnv->define("Number", Value::module("Number"));
    // Number.parse(s) -> Result<Number, ParseError> — tries an Integer full
    // match first (arbitrary precision), then falls back to Float. "42" -> Int,
    // "3.14" -> Float, "1e3" -> Float, "abc" -> Error.
    reg("Number::parse", [parseError, noVal](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s) return Value::error(parseError("", 0, noVal(), "Number.parse expects a String", ""));
        size_t consumed = 0;
        try {
            int64_t v = std::stoll(s->value, &consumed);
            if (consumed == s->value.size()) return Value::ok(Value::integer(v));
        } catch (const std::out_of_range&) {
            try { return Value::ok(integerResult(mpz_class(s->value))); } catch (...) {}
        } catch (...) {}
        try {
            double v = std::stod(s->value, &consumed);
            if (consumed == s->value.size()) return Value::ok(Value::floating(v));
        } catch (...) {}
        return Value::error(parseError(s->value, 0, noVal(), "invalid number", s->value));
    });
}

} // namespace kex::interpreter
