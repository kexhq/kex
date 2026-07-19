#include "../evaluator.hxx"
#include <cmath>

namespace kex::interpreter {

namespace {

auto toDouble(const ValuePtr& val) -> double {
    if (auto* i = std::get_if<IntValue>(&val->data)) return static_cast<double>(i->value);
    if (auto* f = std::get_if<FloatValue>(&val->data)) return f->value;
    return 0.0;
}

} // namespace

// Math's trig/log/power primitives. Public PI and E constants are literals in
// math.kex and require no runtime ABI entry. Domain errors (e.g.
// Math.sqrt(-1.0)) propagate as
// NaN/Infinity rather than raising, matching plain `<cmath>` behavior;
// Kex has no exception type to raise a Math::DomainError equivalent into.
auto Evaluator::registerMathBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        defineIntrinsic(name, std::move(fn));
    };

    reg("Math::sqrt", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::sqrt(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::cbrt", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::cbrt(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::sin", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::sin(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::cos", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::cos(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::tan", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::tan(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::asin", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::asin(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::acos", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::acos(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::atan", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::atan(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::atan2", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::floating(0.0);
        return Value::floating(std::atan2(toDouble(args[0]), toDouble(args[1])));
    });
    reg("Math::sinh", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::sinh(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::cosh", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::cosh(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::tanh", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::tanh(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::exp", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::exp(args.empty() ? 0.0 : toDouble(args[0])));
    });
    // Math.log(x) is natural log; Math.log(x, base) is log base `base` —
    // same two-arity shape as Ruby's Math.log.
    reg("Math::log", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::floating(0.0);
        auto x = std::log(toDouble(args[0]));
        if (args.size() < 2) return Value::floating(x);
        return Value::floating(x / std::log(toDouble(args[1])));
    });
    reg("Math::log2", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::log2(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::log10", [](std::vector<ValuePtr> args) -> ValuePtr {
        return Value::floating(std::log10(args.empty() ? 0.0 : toDouble(args[0])));
    });
    reg("Math::hypot", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::floating(0.0);
        return Value::floating(std::hypot(toDouble(args[0]), toDouble(args[1])));
    });
    reg("Math::pow", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::floating(0.0);
        return Value::floating(std::pow(toDouble(args[0]), toDouble(args[1])));
    });

    // Math shares these implementations with Number receiver methods, but
    // publishes its own private intrinsic identities. Do not reintroduce the
    // old bare-name bridge: Kex.Intrinsic.Math dispatch is category-qualified.
    for (const char* name : {"abs", "ceil", "floor"}) {
        if (auto value = m_intrinsicEnv->get("Number::" + std::string(name)))
            defineIntrinsic("Math::" + std::string(name), value);
    }
}

} // namespace kex::interpreter
