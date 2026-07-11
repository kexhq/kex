#pragma once

#include "../ast/ast.hxx"
#include "fiber.hxx"
#include "value.hxx"
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace kex::interpreter {

class Environment;
class Evaluator;

using ProcessId = uint64_t;

struct Process {
    ProcessId id = 0;
    std::unique_ptr<Fiber> fiber;
    // Every entry is a (Payload, SenderPid) TupleValue — see
    // Scheduler::send/blockingReceive. Selective receive scans this in
    // arrival order rather than popping FIFO.
    std::deque<ValuePtr> mailbox;
    bool finished = false;
    ValuePtr exitValue;
    // Only ever set for the root process created by runToCompletion() — an
    // exception can't safely cross the fiber stack-switch boundary, so it's
    // caught inside the fiber and stashed here, then rethrown by
    // runToCompletion() back on the caller's own stack once the process
    // finishes, preserving normal error reporting (Evaluator::execute()'s
    // caller, e.g. main.cxx, still sees the real exception). Processes
    // created by spawn() deliberately do NOT do this: an uncaught error
    // in a spawned process just ends that process, nothing propagates
    // anywhere.
    std::exception_ptr error;
    std::vector<ProcessId> links;
    // This process's own root scope (a child of whatever environment was
    // active at spawn time — see Scheduler::spawn).
    std::shared_ptr<Environment> env;
    // Evaluator::m_env snapshot to restore right before this process's next
    // resume(), and updated right after each resume() call returns — NOT
    // reset to `env` every time. A process can yield mid-nested-scope (via
    // the reduction-counted auto-yield in Scheduler::tickReduction, not
    // just at a receive's top level), so the correct restore point is
    // "wherever this process's own execution last left m_env", not its
    // root scope. Initialized to `env` so the very first resume() (which
    // runs the entry closure calling into evalBody) starts from the right
    // scope.
    std::shared_ptr<Environment> savedEnv;
    // Set by send() only when a message arrives while this process is
    // genuinely blocked in receive with nothing matching — guards against
    // enqueueing the same process into the ready queue more than once.
    bool blockedOnReceive = false;
    // BEAM-style reduction counter — see Scheduler::tickReduction.
    int reductions = 0;
    // Incremented once at the start of every blockingReceive call — a
    // "which wait is this" token. A pending timeout registered for wait
    // generation N must be ignored if it fires after this process has since
    // moved on to a *different* receive (generation N+1, N+2, ...): without
    // this, an early message-wake followed immediately by a fresh `receive
    // timeout:` could let the FIRST call's now-stale timeout fire against
    // the second call, timing it out prematurely. See blockingReceive.
    uint64_t waitGeneration = 0;
    // Set by the scheduler's timeout-firing logic (never by send()) right
    // before waking this process, so blockingReceive can tell "woke because
    // a message arrived" from "woke because the deadline passed" without
    // needing to re-scan the mailbox first.
    bool wokeByTimeout = false;
};

// Cooperative, single-OS-thread scheduler over Fiber-backed processes — in
// short: no OS threads, no locks, exactly one fiber's code ever executing at
// a time, concurrency (not parallelism) only. BEAM remains the answer for
// anything needing real multicore parallelism.
class Scheduler {
public:
    explicit Scheduler(Evaluator& evaluator);

    // Runs `body` as a fresh process to completion — draining the ready
    // queue until that specific process finishes — then returns its result.
    // Any OTHER still-alive processes (e.g. a spawned server loop that
    // never explicitly terminates, matching examples/proc_ping.kex) are
    // left exactly as they are in the process table, so a later call (e.g.
    // the next REPL line) can still `send` to them. This is how
    // Evaluator::execute() runs the top-level program — there is no
    // "outside of a process" execution mode, matching BEAM, where even the
    // shell is just a process.
    auto runToCompletion(std::function<ValuePtr()> body, std::shared_ptr<Environment> env) -> ValuePtr;

    // Registers a new process running `body` (evaluated via Evaluator's
    // ordinary evalBody) in a fresh child Environment of `closureEnv`, and
    // enqueues it as ready. Returns immediately — the child doesn't run
    // until the scheduler gives it a turn.
    auto spawn(const std::vector<ast::ExprPtr>& body, std::shared_ptr<Environment> closureEnv) -> ProcessId;

    // Pushes (payload, sender) onto target's mailbox and wakes it if it was
    // blocked in receive. Returns false if target doesn't exist / already
    // finished (a no-op send, mirroring BEAM silently dropping sends to
    // dead pids).
    auto send(ProcessId target, ValuePtr payload) -> bool;

    // Called from inside the currently-running process's fiber (via
    // Evaluator::eval's ReceiveExpr branch). Blocks (yields) until a
    // matching message arrives, re-scanning the whole mailbox on every
    // wake since an earlier-arrived, still-unmatched message must still be
    // considered. If expr.timeout is set, evaluates it once (milliseconds)
    // and registers a deadline; if nothing matches by then, evaluates
    // expr.afterBody instead of continuing to wait (or returns None if
    // afterBody is absent) — matches BEAM's `after` semantics: the timeout
    // is measured from entering receive, not reset on each non-matching
    // message.
    auto blockingReceive(const ast::ReceiveExpr& expr) -> ValuePtr;

    auto currentProcessId() const -> ProcessId { return m_current; }
    auto isAlive(ProcessId id) const -> bool;

    // Passive bookkeeping only — records/removes a bidirectional edge
    // between the currently-running process and `other`. Deliberately NOT
    // BEAM's link model: no cascading kill on abnormal exit, no
    // `trap_exit`, no exit-reason signal delivery. A process only ever
    // dies by finishing its own body; nothing external ever force-
    // terminates it, so there's no signal-propagation machinery to build
    // here — real BEAM remains the answer for anything that needs actual
    // supervision robustness.
    auto link(ProcessId other) -> void;
    auto unlink(ProcessId other) -> void;

    // Task.start { block }: spawns a process that calls `blockFn` (a
    // zero-arg FunctionValue — the evaluated `{ ... }` block) and, on
    // completion, sends `(:task_done, result)` back to whichever process
    // is currently running (the one calling Task.start) — captured once,
    // at start time, not re-derived via `self()` inside the block, so the
    // reply always goes to the real spawner even if the block itself calls
    // spawn/receive. An escaping exception sends `(:task_failed, message)`
    // instead — there's no OS-level monitor/DOWN signal to lean on, so
    // catching here is what replaces it.
    auto startTask(ValuePtr blockFn) -> ProcessId;

    // Blocks (same yield/timeout mechanics as blockingReceive, but scanning
    // for a specific task's reply rather than arbitrary Kex patterns) until
    // `taskId`'s (:task_done, ...)/(:task_failed, ...) message arrives, or
    // returns nullopt if timeoutMs elapses first. Used by Task::await.
    auto awaitTaskMessage(ProcessId taskId, std::optional<int64_t> timeoutMs) -> std::optional<ValuePtr>;

    // A plain cooperative sleep — registers a timeout for the currently-
    // running process and yields until it fires. Used by the supervisor
    // poll loop below; any message that happens to arrive during the sleep
    // wakes it early too (harmless here — the supervisor process never
    // reads its own mailbox for anything).
    auto sleepFor(int64_t ms) -> void;

    // Supervisor.start(restart: :only_crashed) do [worker { ... }, ...] end
    // — spawns a dedicated long-running process that starts each child (by
    // calling its zero-arg block, which is expected to itself call `spawn`
    // and return the child's pid — see stdlib/process.cxx's `worker`
    // builtin) and then polls their aliveness, respawning (by calling the
    // same block again) any that have died. This is deliberately NOT BEAM's
    // supervisor: no push notification on crash (nothing in this design
    // ever signals process death — see `link`'s doc comment), so polling
    // is the only option; only `:only_crashed` (one_for_one-equivalent) is
    // supported — `:all`/`:crashed_and_newer` are cut from scope entirely,
    // not deferred.
    // `childBlocks` are the already-evaluated zero-arg FunctionValues from
    // each `worker { ... }` spec.
    auto startSupervisor(std::vector<ValuePtr> childBlocks) -> ProcessId;

    // Called once per function call (see Evaluator::callFunction) so a
    // compute-bound process that never calls `receive` still yields
    // periodically — BEAM's reduction-counting preemption, placed at the
    // same kind of safe point (function-call boundaries), not on every
    // single eval() node.
    auto tickReduction() -> void;

private:
    auto resumeProcess(ProcessId id) -> void;
    auto driveUntilFinished(ProcessId target) -> ValuePtr;
    auto fireExpiredTimeouts() -> void;

    Evaluator& m_evaluator;
    ProcessId m_nextId = 0;
    std::unordered_map<ProcessId, std::unique_ptr<Process>> m_processes;
    std::deque<ProcessId> m_ready;
    ProcessId m_current = 0;

    struct TimeoutEntry {
        ProcessId pid;
        uint64_t generation;
    };
    // Ordered by deadline — std::multimap is a plain balanced tree, which
    // is plenty at the process counts this is meant for; a real binary
    // heap is a drop-in upgrade later if that ever stops being true.
    std::multimap<std::chrono::steady_clock::time_point, TimeoutEntry> m_timeouts;

    static constexpr int kReductionBudget = 2000;
};

} // namespace kex::interpreter
