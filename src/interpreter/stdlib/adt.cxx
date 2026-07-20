#include "../evaluator.hxx"

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

}

} // namespace kex::interpreter
