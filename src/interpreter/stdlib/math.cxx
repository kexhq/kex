#include "../evaluator.hxx"
#include <cmath>

namespace kex::interpreter {

namespace {

// Constants are defined directly rather than relying on <cmath>'s
// non-standard M_PI/M_E (not guaranteed available on every platform).
constexpr double kPi = 3.14159265358979323846;
constexpr double kE = 2.71828182845904523536;

auto toDouble(const ValuePtr& val) -> double {
    if (auto* i = std::get_if<IntValue>(&val->data)) return static_cast<double>(i->value);
    if (auto* f = std::get_if<FloatValue>(&val->data)) return f->value;
    return 0.0;
}

} // namespace

// Math.PI, Math.E, and the usual trig/log/power functions — modeled on
// Ruby's Math module. Domain errors (e.g. Math.sqrt(-1.0)) propagate as
// NaN/Infinity rather than raising, matching plain `<cmath>` behavior;
// Kex has no exception type to raise a Math::DomainError equivalent into.
auto Evaluator::registerMathBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // Namespace placeholder so `Math.sqrt(...)` resolves via the
    // empty-RecordValue namespace-dispatch branch in eval() (ast::MethodCall)
    // and gets the mangled "Math::" dispatch — see io.cxx for the same setup.
    m_globalEnv->define("Math", Value::module("Math"));

    reg("Math::PI", [](std::vector<ValuePtr>) -> ValuePtr { return Value::floating(kPi); });
    reg("Math::E", [](std::vector<ValuePtr>) -> ValuePtr { return Value::floating(kE); });

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
}

} // namespace kex::interpreter
