#include "../evaluator.hxx"

namespace kex::interpreter {

auto Evaluator::registerIntegerBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    reg("modulo", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::integer(0);
        auto* a = std::get_if<IntValue>(&args[0]->data);
        auto* b = std::get_if<IntValue>(&args[1]->data);
        if (a && b && b->value != 0) return Value::integer(a->value % b->value);
        return Value::integer(0);
    });

    reg("abs", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::integer(0);
        if (auto* i = std::get_if<IntValue>(&args[0]->data))
            return Value::integer(i->value < 0 ? -i->value : i->value);
        if (auto* f = std::get_if<FloatValue>(&args[0]->data))
            return Value::floating(f->value < 0 ? -f->value : f->value);
        return args[0];
    });
}

} // namespace kex::interpreter
