#include "../evaluator.hxx"
#include <stdexcept>

namespace kex::interpreter {

// Optional/Result constructors retain specialized native factories solely for
// their generic runtime type metadata. Either is an ordinary source-owned ADT
// in optional.kex, so Left/Right are intentionally not registered here.
auto Evaluator::registerAdtConstructors() -> void {
    // Value::just/ok/error preserve the generic metadata needed for typeName()
    // to render "Optional<Integer>"/"Result<String, ...>" rather than a bare
    // ADT name (see value.hxx's comment on those helpers).
    auto regGenericCtor1 = [this](const std::string& name, ValuePtr (*make)(ValuePtr)) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, [make](std::vector<ValuePtr> args) -> ValuePtr {
            return make(args.empty() ? Value::none() : args[0]);
        }};
        m_globalEnv->define(name, val);
    };
    regGenericCtor1("Just", &Value::just);
    regGenericCtor1("Ok", &Value::ok);
    regGenericCtor1("Error", &Value::error);

    // Comparison — the result type of Comparable.compare.
    // Less/Equal/Greater are zero-arg variant constructors.
    m_globalEnv->define("Less",    Value::variant("Less",    "Ordering"));
    m_globalEnv->define("Equal",   Value::variant("Equal",   "Ordering"));
    m_globalEnv->define("Greater", Value::variant("Greater", "Ordering"));

    // `or` — fallback extraction, shared by both prelude ADTs: unwraps
    // Ok(x)/Just(x) to x, or returns the given default for Error(_)/None.
    // It also preserves the historical universal behavior for raw successful
    // values returned by APIs such as File.read, which the typed Kex methods
    // on Optional and Result intentionally do not cover.
    {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{"or", [](std::vector<ValuePtr> args) -> ValuePtr {
            if (args.size() < 2) {
                throw std::runtime_error("or expects a receiver and a default value");
            }
            const auto& receiver = args[0];
            const auto& fallback = args[1];
            if (receiver->isNone()) return fallback;
            if (auto* var = std::get_if<VariantValue>(&receiver->data)) {
                if (var->tag == "Ok" || var->tag == "Just") {
                    return var->args.empty() ? Value::none() : var->args[0];
                }
                if (var->tag == "Error") return fallback;
            }
            // Raw value (not an ADT wrapper, not None) — already the
            // successful result; return it directly. This handles stdlib
            // functions that return a raw value on success and None on failure
            // (e.g. File.read, File.lines) without requiring Just wrapping.
            return receiver;
        }};
        m_globalEnv->define("or", val);
    }
}

} // namespace kex::interpreter
