#include "../evaluator.hxx"
#include <cmath>
#include <stdexcept>

namespace kex::interpreter {

namespace {

auto toDouble(const ValuePtr& val) -> double {
    if (auto* i = std::get_if<IntValue>(&val->data)) return static_cast<double>(i->value);
    if (auto* f = std::get_if<FloatValue>(&val->data)) return f->value;
    return 0.0;
}

// Domain errors (Math.sqrt(-1.0)) and overflow (Math.exp(1000.0)) raise
// rather than yielding NaN/Infinity — see nonFiniteFloatError. Checking the
// *result* rather than each function's domain is exactly what BEAM does, and
// it covers every function here with one rule. std::runtime_error, not
// RuntimeError, because an intrinsic has no source location to attach.
auto finite(double v, const char* what) -> ValuePtr {
    if (auto error = nonFiniteFloatError(v, what)) throw std::runtime_error(*error);
    return Value::floating(v);
}

} // namespace

// Math's trig/log/power primitives. Public PI and E constants are literals in
// math.kex and require no runtime ABI entry.
auto Evaluator::registerMathBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        defineIntrinsic(name, std::move(fn));
    };

    reg("Math::sqrt", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::sqrt(args.empty() ? 0.0 : toDouble(args[0])), "Math.sqrt");
    });
    reg("Math::cbrt", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::cbrt(args.empty() ? 0.0 : toDouble(args[0])), "Math.cbrt");
    });
    reg("Math::sin", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::sin(args.empty() ? 0.0 : toDouble(args[0])), "Math.sin");
    });
    reg("Math::cos", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::cos(args.empty() ? 0.0 : toDouble(args[0])), "Math.cos");
    });
    reg("Math::tan", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::tan(args.empty() ? 0.0 : toDouble(args[0])), "Math.tan");
    });
    reg("Math::asin", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::asin(args.empty() ? 0.0 : toDouble(args[0])), "Math.asin");
    });
    reg("Math::acos", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::acos(args.empty() ? 0.0 : toDouble(args[0])), "Math.acos");
    });
    reg("Math::atan", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::atan(args.empty() ? 0.0 : toDouble(args[0])), "Math.atan");
    });
    reg("Math::atan2", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::floating(0.0);
        return finite(std::atan2(toDouble(args[0]), toDouble(args[1])), "Math.atan2");
    });
    reg("Math::sinh", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::sinh(args.empty() ? 0.0 : toDouble(args[0])), "Math.sinh");
    });
    reg("Math::cosh", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::cosh(args.empty() ? 0.0 : toDouble(args[0])), "Math.cosh");
    });
    reg("Math::tanh", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::tanh(args.empty() ? 0.0 : toDouble(args[0])), "Math.tanh");
    });
    reg("Math::exp", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::exp(args.empty() ? 0.0 : toDouble(args[0])), "Math.exp");
    });
    // Math.log(x) is natural log; Math.log(x, base) is log base `base` —
    // same two-arity shape as Ruby's Math.log.
    reg("Math::log", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::floating(0.0);
        auto x = std::log(toDouble(args[0]));
        if (args.size() < 2) return finite(x, "Math.log");
        return finite(x / std::log(toDouble(args[1])), "Math.log");
    });
    reg("Math::log2", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::log2(args.empty() ? 0.0 : toDouble(args[0])), "Math.log2");
    });
    reg("Math::log10", [](std::vector<ValuePtr> args) -> ValuePtr {
        return finite(std::log10(args.empty() ? 0.0 : toDouble(args[0])), "Math.log10");
    });
    reg("Math::hypot", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::floating(0.0);
        return finite(std::hypot(toDouble(args[0]), toDouble(args[1])), "Math.hypot");
    });
    reg("Math::pow", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::floating(0.0);
        return finite(std::pow(toDouble(args[0]), toDouble(args[1])), "Math.pow");
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
