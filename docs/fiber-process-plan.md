# Fiber-based process model for the tree-walking interpreter

## Context

Kex's process syntax (`spawn`, `receive`, `send`, `link`, `Task`, `Supervisor` — see `docs/concurrency.md` for the language-level view) today only works through the separate Core Erlang codegen backend (`src/codegen/core_erlang.cxx`), which compiles to real BEAM bytecode and runs it via `erlc`/`erl`. The tree-walking interpreter (`src/interpreter/evaluator.cxx`) has **no implementation of this at all** — `ast::SpawnExpr` and `ast::ReceiveExpr` are parsed but unhandled in `Evaluator::eval`'s dispatch chain, silently falling through to `return Value::none();`. A prior OS-thread-based attempt at this was deliberately removed because it duplicated what BEAM already does well on native, where real BEAM is one `std::system()` call away.

The motivating case now is narrower and has a real reason to exist: embedding a Kex REPL directly in a web page. In a browser there is no subprocess execution, so the real-BEAM path is unreachable, and running a full BEAM/OTP runtime compiled to WebAssembly was considered and rejected as too heavy. Implementing process semantics directly in the interpreter is the only way `spawn`/`receive` work at all when embedded. Since the interpreter is one C++ codebase compiled for both native and wasm (Emscripten) targets, the same implementation runs on both — this isn't the same duplication problem as before: on native it coexists with (and doesn't replace) the compile-to-BEAM path for anything heavier, and it's what makes the language usable at all where BEAM isn't an option.

**Fibers are not threads.** This distinction is the whole point of the design: a thread is OS-scheduled, preemptible, and needs locks because the OS can interrupt it mid-mutation at any point. A fiber is just a saved call stack that the program itself switches to explicitly — everything runs on one OS thread, nothing is ever preempted, only one fiber's code is ever executing at any instant. No OS thread pool, no mutexes, no condition variables — it's closer to manually saving/restoring a function's call stack than to real threading. That's what makes this the right replacement for the removed thread-based model rather than a repeat of it: no thread management, no synchronization primitives, no real parallelism (concurrency only, which is all this needs — BEAM remains the answer for anything requiring actual multicore parallelism).

## Design

### 1. Fiber abstraction — `src/interpreter/fiber.hxx` / `fiber.cxx`

A small platform-neutral stack-switching primitive:

```cpp
class Fiber {
public:
    using EntryFn = std::function<void()>;
    explicit Fiber(EntryFn entry, size_t stackSize = 256 * 1024);
    ~Fiber();
    auto resume() -> void;                  // scheduler -> this fiber
    auto finished() const -> bool;
    static auto yieldToScheduler() -> void;  // this fiber -> scheduler
};
```

Two backends behind `#ifdef __EMSCRIPTEN__`, both living only in `fiber.cxx` (the header stays platform-neutral):
- **Native**: **Boost.Context** (`boost::context::fiber`, its `fcontext`-based assembly backend), not `ucontext.h` as originally planned. `ucontext` was tried first and rejected after hitting a real, reproducible bug: on Apple Silicon macOS, `swapcontext` doesn't reliably save/restore FP/NEON register state across a context switch, causing heap corruption (confirmed empirically — a freed "pointer" whose bytes were exactly the bit pattern of `1.0f`, and a REPL rendering bug that turned out to be memory corruption, not a logic error). Boost.Context's `fcontext` backend is hand-written assembly per architecture, specifically built to handle this correctly, and is the well-established primitive other coroutine/fiber libraries reach for on this exact platform combination. New dependency: `find_package(Boost REQUIRED COMPONENTS context)` in CMakeLists.txt (Homebrew's `boost` package ships the CMake config needed).
  - One sharp edge worth documenting since it caused a real crash during implementation: destroying a still-suspended (not finished) `boost::context::fiber` makes Boost.Context resume it *one more time*, specifically to throw an internal `boost::context::detail::forced_unwind` exception at its suspension point, so the fiber's own C++ stack unwinds normally (destructors run) before the stack memory is freed. That exception **must** propagate all the way back out of the entry function uncaught. A generic `catch (...)` around the entry function's body (needed anyway to turn an uncaught Kex-level error into that process's exit value) will otherwise swallow it, corrupting Boost.Context's internal state and crashing later, unrelated fiber operations. Every such `catch (...)` must check for and re-throw `forced_unwind` first. This matters in practice specifically because processes are *expected* to be destroyed while still suspended — e.g. `examples/beam/proc_ping.kex`'s server loop, which never explicitly terminates.
- **Wasm**: `emscripten/fiber.h`, unchanged from the original plan and still untested (no wasm toolchain used yet). Confirmed against Emscripten's own docs: fibers are built *on top of* Asyncify, not an alternative to it — `-sASYNCIFY` is a hard requirement for using this API at all, so the wasm build pays Asyncify's binary-size/perf cost regardless (an earlier draft of this plan asserted otherwise; that was wrong).

A single process-wide "current fiber" pointer (not actually thread-local in any meaningful sense — there is exactly one OS thread — just named that way defensively) tracks who `yieldToScheduler()` swaps back to.

Exceptions (`RuntimeError`, `ReturnException`) must not cross a stack switch — each fiber's entry closure catches everything at its outermost frame and stores it as that process's exit state (see §8).

### 2. Scheduler — `src/interpreter/scheduler.hxx` / `scheduler.cxx`

```cpp
using ProcessId = uint64_t;

struct Process {
    ProcessId id;
    std::unique_ptr<Fiber> fiber;
    std::deque<ValuePtr> mailbox;     // each entry a (Payload, SenderPid) tuple — see §4
    bool alive = true;
    std::vector<ProcessId> links;
    std::shared_ptr<Environment> env; // this process's own root scope, see §7
    ValuePtr exitValue;               // set once Done/Exited
};

class Scheduler {
public:
    auto spawn(const std::vector<ast::ExprPtr>& body,
               std::shared_ptr<Environment> closureEnv) -> ProcessId;
    auto send(ProcessId target, ValuePtr message) -> bool;

    // Called from inside the currently-running process's fiber. Blocks
    // (yields) until a matching message arrives or the timeout elapses.
    auto blockingReceive(const ast::ReceiveExpr& expr) -> ValuePtr;

    auto currentProcessId() const -> ProcessId;
    auto isAlive(ProcessId id) const -> bool;
    auto link(ProcessId a, ProcessId b) -> void;

    auto runToCompletion() -> void;  // native script/REPL mode: blocks until idle
    auto step() -> bool;             // wasm mode: one quantum, never blocks
};
```

State: a process table keyed by `ProcessId`, a ready queue, and a `std::multimap<time_point, ProcessId>` for `receive timeout:` deadlines (adequate at this scale — a real binary heap is a drop-in upgrade later if ever needed, not worth building up front).

**Do what BEAM does: there is no non-process execution mode.** On real BEAM, even the shell is just a process — nothing ever runs outside of a process context. Mirror that exactly rather than trying to make concurrency "opt-in": `Evaluator::execute()` unconditionally creates the `Scheduler` and wraps the top-level program in a root `Fiber` (process 0) before evaluating a single expression, the same way `spawn` creates any child. `Process.self`, `spawn`, and `receive` at top level (which `examples/processes.kex` does immediately: `counter.send(Get(Process.self)); receive do n -> ... end`) go through the exact same code path as inside a spawned process, with zero special-casing — because there's no "not yet in a process" state to special-case out of. This also removes a real inconsistency an earlier draft of this plan had: lazily creating the Scheduler only on first `spawn`/`receive` use is impossible to reconcile with "top-level `receive` needs no special path," since by the time a lazy scheduler is first constructed, top-level execution is already running on the plain C++ stack, not on a fiber, and can't be retroactively converted into one. Unconditional process-0-from-the-start avoids that problem entirely, at the cost of one small fixed-size fiber allocation per run — negligible, and exactly what BEAM itself pays for every process, including trivial ones.

Two run modes:
- `runToCompletion()` — resumes the root fiber, then whichever process is next runnable or next-timeout-due, repeating until the ready queue and timeout heap are both empty. Used for native script mode and REPL (each line/script runs to completion before returning — matches current REPL semantics).
- `step()` — one quantum, never blocks. Needed because a browser tab can't let the C++ side spin — it must hand control back to the JS event loop between quanta so messages/timers arriving from JS can be delivered. The wasm-side driver calls `step()` in a loop (`setTimeout`/`requestIdleCallback`), scheduling its next call based on the next timeout deadline or external input.

### 3. Value representation — `src/interpreter/value.hxx`

One new variant, following the existing `ModuleValue`/`FileHandleValue` convention:

```cpp
struct ProcessValue { ProcessId id; Scheduler* scheduler; }; // non-owning back-pointer
```

Deliberately thin — the real `Fiber`, mailbox, and links live in the `Scheduler`'s process table, not on the `Value`, so copying a pid around never risks lifetime issues. `Value::process(id, scheduler)` factory alongside the others; `typeName()` returns `"Process"`; `toString()`/`inspect()` render `#Process<3>`.

**Task**: a separate `TaskValue { ProcessId id; Scheduler* scheduler; }` variant rather than reusing `ProcessValue` — a Task *is* spawn+monitor underneath (same as `runtime/src/kex_task.erl`), but keeping it distinct lets `typeName()` say `"Task"` and lets `.await` dispatch unambiguously without a tag check.

### 4. Mailbox and selective receive

No locking anywhere — the scheduler guarantees exactly one fiber's call stack is ever live, so `send()` (a synchronous push into the target's mailbox from whichever fiber is currently running) can never race.

Every mailbox entry is a `(Payload, SenderPid)` tuple, always — matching the codegen backend's `{'kex_msg', Payload, Sender}` wire format, so the two backends describe the same process-model semantics regardless of which one runs a given program. `receive` is BEAM-style selective receive, not FIFO pop: scan the mailbox in arrival order, evaluate each clause's pattern against the first message that matches *any* clause, remove exactly that message, bind (and bind `senderBinding` to the sender, if present), evaluate the clause body. If nothing in the current mailbox matches, register a timeout deadline if `expr.timeout` is set, then `Fiber::yieldToScheduler()`; on resume, re-scan from scratch (a new message may not be the only unmatched one). On timeout expiry rather than a new message, evaluate `afterBody` (or return `None` if absent).

This selective-scan behavior — not naive FIFO — is worth a dedicated test early (send `[A, B, C]` to a process whose `receive` only matches `B`-shaped messages, assert `B` is processed before `A`/`C` despite arriving second), since it's easy to accidentally implement as a plain queue pop.

### 5. Fairness — reduction-counted auto-yield

As designed so far, a process only yields inside `blockingReceive` when it has no matching message. That's a starvation hazard: a process running a tight CPU loop that never calls `receive` would monopolize the scheduler indefinitely, since nothing forces it to give up its fiber. Real BEAM avoids this despite also being cooperative (single native scheduler thread, no OS preemption) by counting "reductions" (roughly, function calls) per process and automatically switching after a budget is exhausted, at safe call boundaries — invisible to the Erlang programmer, who never writes an explicit yield.

Same mechanism here, placed the same way BEAM places it: give `Process` a `reductions` counter, increment it at *function-call* boundaries specifically (i.e. in `callFunction`, not on every single `eval()` node) — matching BEAM's actual safe-point placement rather than interrupting arbitrarily mid-expression. When it crosses a fixed budget (start with something like BEAM's default of ~2000 and tune from measurement), call `Fiber::yieldToScheduler()` and reset the counter, re-enqueueing the process at the back of the ready queue. This is one more call site for the same yield primitive already needed for `receive` — not a new mechanism — and it means no Kex program ever needs, or can write, an explicit yield: fairness is automatic for both message-driven and pure-compute processes. Restricting yield points to `receive`-blocked and function-call boundaries (rather than "anywhere in `eval()`") also keeps the set of places where `Evaluator`'s shared mutable state needs to be provably yield-safe small and enumerable — see §7's `m_env`-swap requirement, which needs auditing at exactly these same two kinds of sites and no others.

### 6. Evaluator integration — `src/interpreter/evaluator.cxx`

`SpawnExpr` and `ReceiveExpr` become new `if constexpr` branches (they can't be plain stdlib builtins — both need direct access to the current fiber/scheduler):

```cpp
else if constexpr (std::is_same_v<T, ast::SpawnExpr>) {
    ensureScheduler();
    auto pid = m_scheduler->spawn(node.body, m_env);
    return Value::process(pid, m_scheduler.get());
}
else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
    ensureScheduler();
    return m_scheduler->blockingReceive(node);
}
```

`send`/`link`/`unlink`/`alive?`/`Process.self` are ordinary builtins in a new `src/interpreter/stdlib/process.cxx`, mirroring `stdlib/io.cxx`'s `reg()` pattern and wired into `registerBuiltins()`. `pid.send(m)` is UFCS (receiver becomes first arg), so `send` is registered plain-named like list methods, not namespaced; `Process.self` needs `Value::module("Process")` plus `"Process::self"`, matching the `IO`/`Math` module pattern.

### 7. Environment correctness across processes

`environment.cxx` needs no new locking: no `Environment` method itself calls back into `eval()`, so no yield can happen mid-mutation of a single binding — cooperative scheduling guarantees that. Two processes racing to mutate a variable they both closed over is expected concurrent-semantics behavior (same hazard as two BEAM processes sharing an ETS table), not a bug to fix.

One real correctness requirement, not just a data race: `Evaluator::m_env` is a single field shared by the one `Evaluator` instance every process's `eval()` runs through. Before resuming any process's fiber, the scheduler must point `m_env` at *that* process's own root environment (`Scheduler::resumeProcess` calls a small internal `Evaluator::setCurrentEnv` right before `fiber->resume()`). Forgetting this swap silently corrupts variable scoping across processes — flag it with a clear comment at the swap site and cover it with a targeted test (two processes with same-named, differently-valued top-level `let`s in their spawn bodies, asserting no cross-contamination).

`spawn`'s new process gets its own root `Environment` as a genuine *child* of the closure environment (`std::make_shared<Environment>(closureEnv)`, exactly the `LambdaValue` closure-capture convention already in use) — never a copy — so bindings made inside the new process never leak into the parent or siblings.

**Audit of `Evaluator`'s other mutable members** (checked against the actual field list in `evaluator.hxx`): `m_env` is the *only* one that needs the per-resume swap. Everything else is either write-once at startup then read-only for the rest of execution (`m_globalEnv`, `m_functionDefs`, `m_variantParent`, `m_recordDefs`, `m_scriptArgs`, `m_replMode`), or intentionally global shared state by design — `m_output` (all processes' prints should interleave into one stream, same as real stdout), `m_mockFiles`/`m_mockDirs` (a shared mock filesystem), `m_testDepth`/test counters (the `describe`/`it` DSL isn't expected to run under `spawn`). Those fall under the same "expected concurrent-mutation hazard, not a bug" reasoning as `Environment` above, not a new correctness requirement.

### 8. link / alive? — passive bookkeeping, no signal propagation

Deliberately **not** BEAM's link model. Real BEAM links carry active signal propagation (cascading kill on abnormal exit, `trap_exit` converting signals to messages, exit-reason semantics) — that's real machinery this project isn't reimplementing; BEAM itself remains the path for anything that actually needs it.

Here, `link`/`unlink` just record a bidirectional edge (`Process::links: std::vector<ProcessId>`) and `alive?` reports whether a process's fiber has finished. Nothing external ever force-terminates a process — a process only ever dies by finishing its own body, whether that's a normal return or an exception caught by *that same fiber's own* outer handler. Because nothing ever kills a fiber from outside, every fiber's C++ stack always unwinds through its own ordinary exception handling, so `shared_ptr`-held resources are always cleaned up normally — no abandoned-mid-execution-stack case exists in this design, and no special "force-unwind" mechanism is needed. `link`/`unlink`/`alive?` are builtins in `process.cxx` following the same shape as `send`.

### 9. Task and Supervisor — later, explicitly best-effort

- **`Task.start { block }`**: spawn a process wrapping `block`; on completion send `(:task_done, result)` back to the *spawning* process (captured at spawn time, not `self()` inside the body); on an escaping exception, send `(:task_failed, error)` instead — there's no OS monitor/DOWN signal to lean on, so wrapping the body's eval in try/catch replaces it directly. `.await(timeout:)` reuses the same wait/wake/timeout core as `receive` — worth factoring `blockingReceive` into a thin AST-pattern layer over a lower-level `Scheduler::receiveOne(matcher, timeout)` once Task is built, so both share one implementation of "wait, retry on wake, handle timeout" instead of duplicating it.
- **`Supervisor.start(restart: :only_crashed) do ... end`** — scoped to exactly this one strategy (the only one `examples/proc_supervisor.kex` actually uses), equivalent to BEAM's `one_for_one`: when a specific child's fiber finishes, respawn just that child from its original start body. Never touches siblings, so it never needs to force-kill another process — sidesteps needing any of BEAM's link-signal machinery entirely. `restart: :all`/`:crashed_and_newer` (`one_for_all`/`rest_for_one`, which require killing siblings) are cut from scope outright, not deferred — nothing in the current examples needs them, and building them would mean reimplementing the exit-signal propagation this design specifically avoids. If that ever becomes necessary, it's a BEAM-shaped problem and the compile-to-BEAM backend is the better answer anyway.

### 10. CMakeLists.txt

```cmake
add_library(kex_lib
    ...
    src/interpreter/fiber.cxx
    src/interpreter/scheduler.cxx
    src/interpreter/stdlib/process.cxx
    ...
)
```

No new `find_library` for native (`ucontext.h` is always present on macOS/Linux). For an Emscripten build (`CMAKE_SYSTEM_NAME STREQUAL "Emscripten"`, the standard variable `emcmake` sets), the fiber backend needs `-sASYNCIFY -sFIBER=1` link flags — confirm exact flag names against whichever Emscripten SDK version this project ends up pinning, since `emscripten/fiber.h`'s requirements have shifted across SDK versions. Standing up the actual wasm build target (toolchain file, JS bindings exposing `step()`/REPL entry points to the browser) is a distinct follow-up effort, out of scope for landing the native core.

### 11. Testing

- New `tests/fiber_test.cxx`: spawn N fibers incrementing a shared counter and yielding between increments; assert correct interleaving and stack isolation.
- New cases in `tests/interpreter_test.cxx`: spawn/send/receive round trip (`proc_ping.kex`-shaped), the selective-receive-ordering test from §4, `senderBinding` (mirroring `examples/processes.kex`'s counter pattern), `receive timeout:`/`after` (keep the timeout short, ~10-50ms, so the suite stays fast), `link`/`alive?` including crash-propagation, and the two-process environment-isolation test from §7.
- `tests/examples_test.cxx` currently documents that `examples/processes.kex` is parse-only for the interpreter ("uses spawn/receive/send which target BEAM, not the tree-walker"). Since that file also exercises `Task`, it can only be meaningfully un-skipped once Task lands (§9) — until then, update the comment to name specifically what's still missing rather than leaving it stale.

## Phasing

1. **Done.** Fiber abstraction (native on Boost.Context, wasm untested) + Scheduler skeleton + `ProcessValue` + `spawn`/`send`/`receive` (no timeout). Root-process-as-fiber lands here since top-level `receive` needs it immediately. `examples/beam/proc_ping.kex` now runs correctly through the tree-walking interpreter.
2. **Done.** `receive timeout:` / `after` — deadline evaluated once per receive call (not reset per non-matching message, matching BEAM), a `waitGeneration` token on `Process` guards against a stale timeout from an earlier `receive` firing against a later, unrelated one.
3. `link`/`unlink`/`alive?` + exit propagation (next).
4. `Task.start`/`.await(timeout:)`/`.await_all`, including the `blockingReceive` → `receiveOne` refactor.
5. `Supervisor.start(restart:)`, best-effort scope per §9.
6. (Separate follow-up, not gating 1-5) wasm/Emscripten build target + JS bindings + `step()`-driven browser REPL.

## Verification

- Native: `cmake -B build && cmake --build build`, `ctest` (new fiber/interpreter test cases), and manually run a `spawn`/`receive` snippet through `./build/kex` in both script and `-i` REPL mode.
- Wasm (once phase 6 lands): `emcmake cmake -B build-wasm && cmake --build build-wasm`, smoke-test the fiber backend under Node before building any browser UI, to isolate fiber-correctness issues from JS-glue issues.
