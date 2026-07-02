#include "../evaluator.hxx"

namespace kex::interpreter {

// spawn/receive themselves are handled directly in Evaluator::eval (they
// need direct access to the Scheduler, not just argument values — see the
// SpawnExpr/ReceiveExpr branches). This file only covers the ordinary
// builtins: Process.self and pid.send(msg).
auto Evaluator::registerProcessBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // Pre-register the namespace placeholder so `Process.self` resolves via
    // the ModuleValue namespace-dispatch branch in eval() (ast::MethodCall),
    // the same convention IO/Math use.
    m_globalEnv->define("Process", Value::module("Process"));

    reg("Process::self", [this](std::vector<ValuePtr>) -> ValuePtr {
        return Value::process(m_scheduler->currentProcessId(), m_scheduler.get());
    });

    // pid.send(msg) — UFCS, so `send` is registered plain-named (receiver
    // becomes args[0]), not namespaced, matching how list/string methods
    // are registered.
    reg("send", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::unit();
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::unit();
        p->scheduler->send(p->pid, args[1]);
        return Value::unit();
    });
}

} // namespace kex::interpreter
