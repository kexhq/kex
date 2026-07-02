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
  - One sharp edge worth documenting since it caused a real crash during implementation: destroying a still-suspended (not finished) `boost::context::fiber` makes Boost.Context resume it *one more time*, specifically to throw an internal `boost::context::detail::forced_unwind` exception at its suspension point, so the fiber's own C++ stack unwinds normally (destructors run) before the stack memory is freed. That exception **must** propagate all the way back out of the entry function uncaught. A generic `catch (...)` around the entry function's body (needed anyway to turn an uncaught Kex-level error into that process's exit value) will otherwise swallow it, corrupting Boost.Context's internal state and crashing later, unrelated fiber operations. Every such `catch (...)` must check for and re-throw `forced_unwind` first. This matters in practice specifically because processes are *expected* to be destroyed while still suspended — e.g. `examples/beam/proc_ping.kex`'s server loop, which never explicitly terminates. Regression test: `tests/interpreter_test.cxx`'s "destroying a still-suspended process doesn't corrupt later, unrelated process operations".
  - A second sharp edge, found while building the Supervisor poll loop: the "which continuation does `yieldToScheduler()` resume" pointer is a single `thread_local` slot, and it was only ever pointed at a given fiber's own continuation *once* — inside that fiber's entry lambda, the first time it started. Resuming the *same* fiber a second time, after some *other* fiber had run in between (and repointed that shared slot at itself), left `yieldToScheduler()` reading a stale pointer into a different, currently-suspended, unrelated fiber's own stack — Boost.Context caught it as an assertion failure (`nullptr != fctx_`) rather than silently corrupting memory, which is how this was found instead of shipped. Fix: `Fiber::resume()` now restores the slot to point at *this* fiber's own continuation on every resume (via `Impl::selfSlot`, communicated back from the entry lambda), not just relying on the lambda's one-time setup. Any scenario with two or more processes each yielding more than once hits this — the minimal repro is a ping-pong server handling more than one round trip. Regression test: "a server process handling multiple round trips, interleaved with the caller, doesn't corrupt fiber state".
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
3. **Done.** `link`/`unlink`/`alive?` — passive bookkeeping (`Scheduler::link`/`unlink` record/remove a bidirectional edge on `Process::links`; `alive?` is just `isAlive`), no signal propagation, per §8. Verified a linked partner exiting does not kill the other process.
4. **Done.** `Task.start`/`.await(timeout:)`/`Task.awaitAll`. Took the pragmatic path on the `blockingReceive`/`receiveOne` refactor mentioned in §9: rather than force `receive`'s Kex-AST pattern/guard matching and Task's fixed-shape C++ message check through one shared abstraction, `Scheduler::awaitTaskMessage` is its own small function with the same yield/timeout/generation-token mechanics — duplicated shape, not duplicated complexity, and simpler than bending one to fit the other.
5. **Done.** `Supervisor.start(restart: :only_crashed)` — best-effort scope per §9, exactly as planned. Since nothing in this design ever signals a process's death (no push notification — see `link`'s doc comment), the supervisor is a real long-running process that polls each child's `alive?` every 50ms and respawns (by calling the same `worker { ... }` start block again) any that have died. Any restart strategy other than `:only_crashed` returns a clear `Error(...)` rather than silently doing the wrong thing. Verified against `examples/beam/proc_supervisor.kex` and a dedicated restart-detection test.
6. **Core done, browser UI still open.** wasm/Emscripten build target — see the new section below for the full account. JS bindings + `step()`-driven browser REPL UI is the only remaining work.

## Verification

- Native: `cmake -B build && cmake --build build`, `ctest` (new fiber/interpreter test cases), and manually run a `spawn`/`receive` snippet through `./build/kex` in both script and `-i` REPL mode.
- Wasm (once phase 6 lands): `emcmake cmake -B build-wasm && cmake --build build-wasm`, smoke-test the fiber backend under Node before building any browser UI, to isolate fiber-correctness issues from JS-glue issues.

## Post-phase-5 cleanup: unifying examples/specs and error output across backends

Once the interpreter genuinely supported spawn/receive/timeout/link/Task/Supervisor, `examples/beam/*.kex` (kept separate on the assumption these only ran via BEAM) and `-R`'s error handling (which never ran semantic analysis at all, unlike plain `run` mode) both needed reconciling:

- Moved `examples/beam/{proc_ping,proc_link,proc_supervisor,proc_task,processes}.kex` back into plain `examples/` — all five run cleanly, with full type-checking on, through **both** backends now. `chat_server_genserver.kex` moved to `examples/aspirational/` instead — it's unrelated to the process model (needs `Net`/`serving`/`slot`, none of which exist on either backend).
- `-R`/`--compile` never ran semantic analysis before reaching `erlc`, so a plain undefined-function error showed up as a raw, un-Kex-like Core Erlang compile failure (`unbound variable 'UndefinedFunctionCall' in main/0`) instead of the same clean diagnostic the interpreter gives. Fixed by extracting the `run` mode's semantic-check block into a shared `runSemanticCheck()` (`src/main.cxx`) and calling it from `compile` mode too (not `emit-core`, which is a debug dump you may want even for code that doesn't type-check). Both backends now produce byte-for-byte identical diagnostics for a shared error, differing only in the final "before running" vs "before compiling" phrase.
- Turning semantic checking on for `-R` surfaced a real, previously-unreachable bug: `resolve_pass.cxx`'s `ReceiveExpr` handling never bound `receive do |sender| ... end`'s sender name into scope (unlike `analyzer.cxx`'s equivalent handling, which did), so any use of it was reported as "Undefined name: `sender`". Never caught before because nothing had ever run semantic checking on process code — the interpreter had no process support until this session, and `-R` skipped checking entirely. Fixed, with a dedicated regression spec (`spec/receive_sender_binding.kex`, `-C`/check-only).
- Also surfaced a genuine cross-backend inconsistency in `examples/proc_supervisor.kex`: it matched `Supervisor.start`'s error case as a raw `(:error, reason)` tuple instead of the `Error(reason)` ADT pattern every other Result-returning API in these examples uses (`Task.await`, etc.) — the type checker's exhaustiveness analysis didn't recognize the tuple pattern as covering the `Error` case at all. Fixed to `Error(reason)`, which correctly matches on both backends (Kex's `Ok`/`Error` patterns compile to Erlang's conventional `{ok, X}`/`{error, X}` tuples on the BEAM backend).

## Phase 6: the wasm build, for real

The interpreter core — literals, arithmetic, arbitrary-precision Integer, control flow, and the entire process model (spawn/receive/timeout/link/Task/Supervisor) — is now confirmed correct under wasm: `interpreter_test.cxx`'s full 145-case suite passes under Node with zero special flags. `examples_test.cxx` still fails there, but only because it reads real files from disk and wasm's default virtual filesystem (MEMFS) isn't preloaded with `examples/` — a distinct, already-anticipated follow-up (see the "Not yet attempted" list below), not a regression.

Getting there took several rounds of real, non-obvious debugging — worth a full account since each one is the kind of thing that'll bite again if forgotten:

- **Emscripten toolchain**: installed via Homebrew initially (6.0.2).
- **GMP has no wasm build anywhere** (not Emscripten's port system, not Homebrew) — built from source (`third_party/gmp-wasm/README.md` has the exact recipe: `emconfigure`, `--disable-assembly` since GMP's hand-tuned assembly doesn't exist for wasm, `--build=aarch64-apple-darwin` explicit or GMP's configure misdetects cross-compilation entirely). Statically linked for wasm specifically — the native build's dynamic-linking-only GMP policy (see CMakeLists.txt) doesn't transfer, since wasm has no real dynamic-linking story; this is permissible under LGPLv3's static-linking allowance since the build recipe (and this repo) provides everything needed to rebuild and relink a modified GMP. **Not vendored as binaries** — `.gitignore`d, regenerated locally from the documented recipe, so the repo doesn't carry permanent binary bloat that can silently drift from what the recipe would actually produce.
- **`ucontext` doesn't work on this platform at all** (separate from wasm — see §1) — that's why native uses Boost.Context.
- **`g_yieldSlot` staleness bug** in the wasm fiber backend (§1) — a shared thread-local slot only ever pointed at a fiber's own continuation once, at first start, so resuming a fiber a second time (after another fiber ran in between) read a stale pointer into an unrelated, currently-suspended fiber's stack. Found by comparing against Emscripten's own `test/test_fibers.cpp`.
- **`emscripten_fiber_swap` requires linking with `-sASYNCIFY`** — confirmed against Emscripten's own docs, not optional.
- **Default `ASYNCIFY_STACK_SIZE` (4KB) is nowhere near enough** for this tree-walker's per-Kex-level-recursion C++ call depth — bumped to 16MB in CMakeLists.txt.
- **Default wasm heap (16MB) is too small** for even one fiber's 8MB C-stack + 8MB asyncify-stack allocation — `-sALLOW_MEMORY_GROWTH=1`.
- **The big one — Asyncify + exception catching hangs on Emscripten 6.0.2**, even with zero exceptions ever thrown, on the very first `emscripten_fiber_swap`. Reproduced in a ~20-line standalone repro with no Kex code at all, so not project-specific. Bisected across `emsdk` versions — **5.0.7 doesn't have this problem**; both fibers and exceptions work correctly together there. This project's wasm build is pinned to 5.0.7 accordingly (`third_party/gmp-wasm/README.md`'s "Emscripten version" section has the details and a note to periodically re-test newer releases, since this was a version-specific regression, not a fundamental incompatibility).
- **A second, sneakier bug masquerading as more Asyncify/EH trouble**: even after pinning to 5.0.7, one specific exception (thrown from deep inside the interpreter, not from any of several closely-matching standalone repros) still wasn't being caught. Root cause: a CMake ordering bug, not a toolchain issue — `add_compile_options(-sNO_DISABLE_EXCEPTION_CATCHING)` was called *after* `add_library(kex_lib ...)`, and `add_compile_options`/`add_link_options` only affect targets declared afterward. So `kex_lib` — where the entire interpreter lives — never actually got the flag at compile time; only the final executables got it, at link time, which doesn't retroactively fix exception codegen already baked into `kex_lib`'s object files. Moved the whole Emscripten flags block to before `add_library(kex_lib ...)`. This was the actual root cause of what had looked like a second toolchain-level EH bug.
- **`long` is 32-bit on wasm32**, unlike every native (LP64) target this project has ever built on — `value.cxx`'s `asInteger`/`integerResult` and one spot in `evaluator.cxx` (`-INT64_MIN` promotion) used `static_cast<long>`/GMP's `fits_slong_p()`/`get_si()`, which silently misbehave outside 32-bit range there. Fixed by round-tripping through decimal-string construction instead (portable regardless of `long`'s width; Integer arithmetic isn't a hot path here anyway — GMP-for-wasm is already on the slower portable-C fallback, not hand-tuned assembly, for the same non-goal).
- **A wall-clock-timing-sensitive test failed** (`Supervisor` restart, tuned for a 50ms poll / 500ms test-timeout margin on native) — turned out to be `CMAKE_BUILD_TYPE` defaulting to empty (`-O0`) for the wasm build. An unoptimized Asyncify build isn't just slower, it's *dramatically* slower — the full 145-test suite went from timing out past 280 seconds (and failing that one timing-sensitive test along the way) to completing in well under a second at `-O3`. Fixed by defaulting `CMAKE_BUILD_TYPE` to `Release` specifically for the Emscripten target in CMakeLists.txt, so building the wasm target doesn't silently require knowing to pass that flag. Also means `node --stack-size=...` (needed to work around the earlier unoptimized build's much deeper effective call-stack usage) isn't needed at all anymore — one less thing to configure, which matters since a browser deployment can't pass JS-engine flags like that anyway.

**Not yet attempted at all**: real filesystem access for the wasm CLI/REPL (preloading `examples/`/`src/prelude/` into MEMFS, or `NODERAWFS` for a Node-hosted CLI use case specifically). A browser UI/HTML page now exists — see below.

### JS REPL bindings (`src/wasm_repl.cxx`) — working test page for non-yielding code, still blocked on a real Asyncify/JS-interop bug for yielding code

`src/wasm_repl.cxx` is a small C API (`kex_repl_create`/`kex_repl_eval`/`kex_repl_last_result`/`kex_repl_destroy`) wrapping one persistent `Evaluator` per session — same cross-call state persistence already proven in phase 1 (a `let`/`spawn` on one call stays visible/alive on the next). Built as its own wasm target (`kex_repl_wasm`, `-sMODULARIZE` + `EXPORTED_FUNCTIONS`), deliberately not reusing `main.cxx`'s interactive REPL loop (built around a real blocking stdin, which doesn't exist in a browser).

**`web/index.html`** is a self-contained test page (dark theme, textarea + Run button + scrolling output log, Enter-to-submit) that drives this API from a real `<script type="module">`. It carries a visible warning box calling out the still-unresolved yield-crossing limitation below, so anyone opening the page understands the boundary of what currently works.

Verified end-to-end in a **real headless browser** (Playwright + Chromium — deliberately not just Node, since Node's CJS/ESM interop can mask browser-only module-loading bugs; see below), serving the page over plain HTTP (`python3 -m http.server`, from the repo root, so `../build-wasm/kex_repl_wasm.js` resolves): `1 + 2` → `=> 3 : Int`, `let x = 5` then a later `x + 10` → `=> 15 : Int` (cross-call state persists), `IO.printLine(...)` (output shown correctly, once), and `spawn` returning a real `#Process<N> : Process` value.

Along the way this caught a **real, separate bug**: the page failed to load at all in Chromium with `The requested module '../build-wasm/kex_repl_wasm.js' does not provide an export named 'default'`. Emscripten's default build emits a UMD/AMD-style export (`module.exports = ...; module.exports.default = ...`), which works via Node's automatic CJS-as-default-export interop but has no export at all under a real browser ES module loader (`module`/`exports` don't exist there). Fixed by adding `-sEXPORT_ES6=1` to the `kex_repl_wasm` target's link options in `CMakeLists.txt`, which makes Emscripten emit a real `export default createKexReplModule;`. This is unrelated to the Asyncify return-value/yield-crossing bug below — it's specifically a module-format mismatch, and Node-only testing had missed it entirely.

**Works correctly**, confirmed via Node driving the compiled module through `ccall(..., {async: true})`: simple non-yielding calls (`1 + 2`, `let x = 5` then `x + 10` on a later call, even `spawn` returning a real `Process` value).

**Breaks** for anything that actually causes the fiber machinery to yield (a `receive`, `receive timeout:`, `Task.await`) — two distinct symptoms, found in this order:
1. A direct return value from an Asyncify-instrumented export wasn't reliably reaching the JS caller — even a hardcoded static-string return came back as a null pointer JS-side, ruling out a memory-ownership bug specifically. Worked around by splitting `kex_repl_eval` into a void-returning "do the work" call plus a separate, ordinary synchronous `kex_repl_last_result` fetch (which doesn't touch Asyncify at all) — this fix is in place and is why the non-yielding cases above work.
2. Even with that split, anything that yields shows **side effects replaying** (`IO.printLine` output appearing twice) and **state going missing** (a `spawn`'d variable becoming undefined on a later call) — consistent with Asyncify's unwind/rewind semantics not being driven correctly by however `ccall`'s `{async: true}` is being invoked here.

Ruled out with clean, isolated, working repros (none of these reproduce the bug in isolation — the real bug only shows up with the full `Scheduler`/`Evaluator` machinery involved):
- Plain `std::this_thread::sleep_until` inside a `ccall`'d async export.
- A real `emscripten_fiber_swap` + `sleep_until` combined, in one call.
- `-sASYNCIFY_EXPORTS` (deprecated in favor of `JSPI_EXPORTS`, tried anyway) — no effect.

Next steps for whoever picks this back up: systematic bisection — incrementally adding pieces of the real `Scheduler`/`Evaluator` call chain into a minimal repro until it breaks, since raw fibers alone don't reproduce it. Worth also directly consulting current Emscripten docs/issues on `ccall`'s `{async: true}` semantics specifically (vs. the JSPI-based replacement it's being deprecated in favor of), since this may be a documented limitation or require a different calling convention than what's used here (`Module.ccall(fn, ret, argTypes, args, {async: true})` from a plain ES module import, not Emscripten-generated glue calling into itself).
