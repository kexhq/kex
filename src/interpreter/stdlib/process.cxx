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

    // pid.link()/pid.unlink() — links the CALLING process (whichever
    // process is currently running when this is invoked, not necessarily
    // the receiver's spawner) to the receiver pid. Passive bookkeeping
    // only — see Scheduler::link's doc comment for why this deliberately
    // doesn't carry BEAM's signal-propagation semantics.
    reg("link", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::unit();
        p->scheduler->link(p->pid);
        return Value::unit();
    });
    reg("unlink", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::unit();
        p->scheduler->unlink(p->pid);
        return Value::unit();
    });

    // pid.alive?() — true until that process's fiber has finished (whether
    // by a normal return or an uncaught exception caught by its own
    // fiber's outer handler — see Scheduler::spawn/runToCompletion).
    reg("alive?", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::boolean(false);
        return Value::boolean(p->scheduler->isAlive(p->pid));
    });
}

} // namespace kex::interpreter
