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

    m_globalEnv->define("Task", Value::module("Task"));

    // Task.start { block } — `block` arrives here as an already-evaluated
    // zero-arg FunctionValue (the `{ ... }` block, per MethodCall's "block
    // as last positional arg" handling in eval()).
    reg("Task::start", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto pid = m_scheduler->startTask(args[0]);
        return Value::task(pid, m_scheduler.get());
    });

    // task.await(timeout: N) — UFCS plain-named like send/link/alive?.
    // Named args without a matching m_functionDefs entry (true for every
    // native builtin) are appended positionally by callFunction, so
    // `timeout:`'s value simply lands at args[1] here. Returns
    // Ok(result)/Error(reason) — Error(:timeout) specifically on timeout,
    // matching docs/concurrency.md's documented `Result<T, TaskError>`
    // shape (TaskError isn't a distinct type here, just whatever reason
    // atom/value ends up in Error(...) — no separate error ADT to keep
    // this from growing beyond what the interpreter needs).
    reg("await", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::variant("Error", "Result", {Value::string("not a task")});
        auto* t = std::get_if<TaskValue>(&args[0]->data);
        if (!t) return Value::variant("Error", "Result", {Value::string("not a task")});

        std::optional<int64_t> timeoutMs;
        if (args.size() > 1) {
            if (auto* iv = std::get_if<IntValue>(&args[1]->data)) timeoutMs = iv->value;
        }

        auto msg = t->scheduler->awaitTaskMessage(t->pid, timeoutMs);
        if (!msg) return Value::variant("Error", "Result", {Value::atom("timeout")});

        auto* tup = std::get_if<TupleValue>(&(*msg)->data);
        if (!tup || tup->elements.size() != 2) {
            return Value::variant("Error", "Result", {Value::string("malformed task result")});
        }
        auto* tag = std::get_if<AtomValue>(&tup->elements[0]->data);
        if (tag && tag->name == "task_done") {
            return Value::variant("Ok", "Result", {tup->elements[1]});
        }
        return Value::variant("Error", "Result", {tup->elements[1]});
    });

    // Task.awaitAll([tasks]) — awaits each task in order, no timeout
    // (matches Task::start's counterpart having no bulk-timeout variant in
    // docs/concurrency.md; call .await(timeout: N) per-task first via
    // Task.awaitAll([...]) is not a thing — this is the simple sequential
    // form). Returns a list of Ok/Error results, same shape as `await`.
    // Matches the Kex-facing name in the BEAM backend (core_erlang.cxx) —
    // both dispatch to the same surface syntax, camelCase like every other
    // multi-word Kex builtin (was briefly registered as "await_all" here,
    // the one snake_case outlier — fixed to match).
    reg("Task::awaitAll", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto* lst = std::get_if<ListValue>(&args[0]->data);
        if (!lst) return Value::list({});

        std::vector<ValuePtr> results;
        for (const auto& taskVal : lst->elements) {
            auto* t = std::get_if<TaskValue>(&taskVal->data);
            if (!t) {
                results.push_back(Value::variant("Error", "Result", {Value::string("not a task")}));
                continue;
            }
            auto msg = t->scheduler->awaitTaskMessage(t->pid, std::nullopt);
            if (!msg) {
                results.push_back(Value::variant("Error", "Result", {Value::atom("timeout")}));
                continue;
            }
            auto* tup = std::get_if<TupleValue>(&(*msg)->data);
            if (!tup || tup->elements.size() != 2) {
                results.push_back(Value::variant("Error", "Result", {Value::string("malformed task result")}));
                continue;
            }
            auto* tag = std::get_if<AtomValue>(&tup->elements[0]->data);
            if (tag && tag->name == "task_done") {
                results.push_back(Value::variant("Ok", "Result", {tup->elements[1]}));
            } else {
                results.push_back(Value::variant("Error", "Result", {tup->elements[1]}));
            }
        }
        return Value::list(std::move(results));
    });
}

} // namespace kex::interpreter
