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

    // Private receiver primitives. The public methods are defined by
    // process.kex and call these category-qualified identities.
    defineIntrinsic("Process::send", [this](std::vector<ValuePtr> args) -> ValuePtr {
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
    defineIntrinsic("Process::link", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::unit();
        p->scheduler->link(p->pid);
        return Value::unit();
    });
    defineIntrinsic("Process::unlink", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        auto* p = std::get_if<ProcessValue>(&args[0]->data);
        if (!p) return Value::unit();
        p->scheduler->unlink(p->pid);
        return Value::unit();
    });

    // pid.alive?() — true until that process's fiber has finished (whether
    // by a normal return or an uncaught exception caught by its own
    // fiber's outer handler — see Scheduler::spawn/runToCompletion).
    defineIntrinsic("Process::alive?", [this](std::vector<ValuePtr> args) -> ValuePtr {
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

    // Walker-only public fallback: awaiting yields the scheduler while the
    // evaluator's environment stack is active, so a Kex wrapper cannot safely
    // own this call until evaluator environments are process-local. Returns
    // Ok(result)/Error(reason) — Error(:timeout) specifically on timeout,
    // matching docs/concurrency.md's documented `Result<T, TaskError>`
    // shape (TaskError isn't a distinct type here, just whatever reason
    // atom/value ends up in Error(...) — no separate error ADT to keep
    // this from growing beyond what the interpreter needs).
    reg("await", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(Value::string("not a task"));
        auto* t = std::get_if<TaskValue>(&args[0]->data);
        if (!t) return Value::error(Value::string("not a task"));

        std::optional<int64_t> timeoutMs;
        if (args.size() > 1) {
            if (auto* iv = std::get_if<IntValue>(&args[1]->data)) timeoutMs = iv->value;
        }

        auto msg = t->scheduler->awaitTaskMessage(t->pid, timeoutMs);
        if (!msg) return Value::error(Value::atom("timeout"));

        auto* tup = std::get_if<TupleValue>(&(*msg)->data);
        if (!tup || tup->elements.size() != 2) {
            return Value::error(Value::string("malformed task result"));
        }
        auto* tag = std::get_if<AtomValue>(&tup->elements[0]->data);
        if (tag && tag->name == "task_done") {
            return Value::ok(tup->elements[1]);
        }
        return Value::error(tup->elements[1]);
    });

    // The source declaration still calls the qualified identity (used by
    // direct intrinsic calls and kept ABI-aligned with BEAM), while ordinary
    // walker receiver dispatch intentionally selects the bare fallback above.
    if (auto value = m_globalEnv->get("await"))
        defineIntrinsic("Process::await", value);

    // Task.awaitAll([tasks]) — awaits each task in order, no timeout
    // (matches Task::start's counterpart having no bulk-timeout variant in
    // docs/concurrency.md; call .await(timeout: N) per-task first via
    // Task.awaitAll([...]) is not a thing — this is the simple sequential
    // form). Returns a list of Ok/Error results, same shape as `await`.
    // Matches the Kex-facing name in the BEAM backend —
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
                results.push_back(Value::error(Value::string("not a task")));
                continue;
            }
            auto msg = t->scheduler->awaitTaskMessage(t->pid, std::nullopt);
            if (!msg) {
                results.push_back(Value::error(Value::atom("timeout")));
                continue;
            }
            auto* tup = std::get_if<TupleValue>(&(*msg)->data);
            if (!tup || tup->elements.size() != 2) {
                results.push_back(Value::error(Value::string("malformed task result")));
                continue;
            }
            auto* tag = std::get_if<AtomValue>(&tup->elements[0]->data);
            if (tag && tag->name == "task_done") {
                results.push_back(Value::ok(tup->elements[1]));
            } else {
                results.push_back(Value::error(tup->elements[1]));
            }
        }
        return Value::list(std::move(results));
    });

    for (const char* name : {"Process::self", "Task::start", "Task::awaitAll"}) {
        if (auto value = m_globalEnv->get(name)) defineIntrinsic(name, value);
    }

    m_globalEnv->define("Supervisor", Value::module("Supervisor"));

    // worker { startFn() } — wraps a zero-arg block (expected to call
    // `spawn` and return the child's pid, matching
    // examples/beam/proc_supervisor.kex's `worker { startCounter("A") }`)
    // into a spec Supervisor.start can both call now (to start it) and
    // recall later (to restart it, from the exact same start function).
    reg("worker", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        return Value::tuple({Value::atom("worker"), args[0]});
    });

    // Supervisor.start(restart: :only_crashed) do [worker { ... }, ...] end
    // — see Scheduler::startSupervisor for the actual poll/restart loop.
    // args[0] is the do-block (a deferred zero-arg FunctionValue evaluating
    // to the list of worker specs); args[1], if present, is the `restart:`
    // atom (named args land positionally-appended here — see `await`'s
    // comment on why). Only :only_crashed is supported — anything else is
    // a clear Error(...) pointing at the BEAM backend instead of a silent
    // wrong behavior.
    reg("Supervisor::start", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) {
            return Value::error(Value::string("Supervisor.start requires a do...end block"));
        }
        auto* specsBlockFn = std::get_if<FunctionValue>(&args[0]->data);
        if (!specsBlockFn || !specsBlockFn->native) {
            return Value::error(Value::string("Supervisor.start requires a do...end block"));
        }

        std::string strategy = "only_crashed";
        if (args.size() > 1) {
            if (auto* av = std::get_if<AtomValue>(&args[1]->data)) strategy = av->name;
        }
        if (strategy != "only_crashed") {
            return Value::error(Value::string(
                "Supervisor restart strategy :" + strategy + " isn't supported by the interpreter — "
                "only :only_crashed is; use the BEAM backend (kex -R) for :all/:crashed_and_newer."));
        }

        auto specsVal = specsBlockFn->native({});
        auto* specsList = std::get_if<ListValue>(&specsVal->data);
        if (!specsList) {
            return Value::error(Value::string("Supervisor.start's block must evaluate to a list of worker specs"));
        }

        std::vector<ValuePtr> childBlocks;
        for (const auto& spec : specsList->elements) {
            auto* tup = std::get_if<TupleValue>(&spec->data);
            if (tup && tup->elements.size() == 2) {
                childBlocks.push_back(tup->elements[1]);
            }
        }

        auto supPid = m_scheduler->startSupervisor(std::move(childBlocks));
        return Value::ok(Value::process(supPid, m_scheduler.get()));
    });
}

} // namespace kex::interpreter
