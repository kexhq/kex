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

    // Float.parse(s) / Integer.parse(s) -> Result<Float|Int, String> —
    // namespaced under the primitive type, same convention as Math.sqrt
    // etc. Built directly as RecordValue{"Ok"/"Error", ...} rather than
    // calling the registered Ok/Error constructors, since those aren't
    // wired up for native-to-native calls (only Kex-level calls).
    m_globalEnv->define("Float", Value::module("Float"));
    reg("Float::parse", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s) return Value::error(Value::string("Float.parse expects a String"));
        try {
            size_t consumed = 0;
            double v = std::stod(s->value, &consumed);
            if (consumed != s->value.size()) throw std::invalid_argument("trailing characters");
            return Value::ok(Value::floating(v));
        } catch (const std::exception&) {
            return Value::error(Value::string("invalid float: " + s->value));
        }
    });

    m_globalEnv->define("Integer", Value::module("Integer"));
    reg("Integer::parse", [](std::vector<ValuePtr> args) -> ValuePtr {
        auto* s = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!s) return Value::error(Value::string("Integer.parse expects a String"));
        try {
            size_t consumed = 0;
            int64_t v = std::stoll(s->value, &consumed);
            if (consumed != s->value.size()) throw std::invalid_argument("trailing characters");
            return Value::ok(Value::integer(v));
        } catch (const std::out_of_range&) {
            // Too big for int64_t doesn't mean invalid — Integer is
            // arbitrary precision; mpz_class's string constructor throws
            // std::invalid_argument itself if the string isn't a valid
            // integer (it requires a full match, not a prefix parse).
            try {
                return Value::ok(integerResult(mpz_class(s->value)));
            } catch (const std::exception&) {
                return Value::error(Value::string("invalid integer: " + s->value));
            }
        } catch (const std::exception&) {
            return Value::error(Value::string("invalid integer: " + s->value));
        }
    });
}

} // namespace kex::interpreter
