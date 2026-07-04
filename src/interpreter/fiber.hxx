#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace kex::interpreter {

// A stack-switching primitive — not a thread. Everything runs on one OS
// thread; a Fiber only stops running when its own code calls
// yieldToScheduler(), and only one fiber's code is ever executing at a
// time. No preemption, no locks needed anywhere that touches fiber state.
//
// Two backends live in fiber.cxx behind #ifdef __EMSCRIPTEN__ (native:
// ucontext.h; wasm: emscripten/fiber.h, which itself requires linking with
// -sASYNCIFY) — this header stays platform-neutral.
class Fiber {
public:
    using EntryFn = std::function<void()>;

    // Default matches typical OS thread stack sizes (e.g. macOS pthread's
    // 8MB default), not an arbitrarily "lightweight" size — this tree-
    // walking interpreter's eval() recursion is many C++ frames deep per
    // one level of Kex-level recursion (nested eval/callFunction/std::visit
    // dispatch), and every process (including process 0, the top-level
    // program) now runs inside a fiber's own stack instead of the main
    // thread's. A smaller default silently regressed recursion-heavy
    // examples (e.g. fizzbuzz_recursive.kex) that ran fine unwrapped.
    explicit Fiber(EntryFn entry, std::size_t stackSize = 8 * 1024 * 1024);
    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    // Switches from the caller into this fiber. Returns once this fiber
    // calls yieldToScheduler() or its entry function returns. A no-op if
    // the fiber has already finished.
    auto resume() -> void;

    auto finished() const -> bool { return m_finished; }

    // Called from inside the currently-running fiber to switch back to
    // whichever context called resume() on it.
    static auto yieldToScheduler() -> void;

    // Public only so the free-function trampoline shim in fiber.cxx (which
    // makecontext/emscripten_fiber_init require as a plain function
    // pointer, so it can't be a capturing lambda or member function
    // pointer) can call back into the entry closure. Not part of the
    // public API otherwise.
    auto trampolineBody() -> void;

private:
    struct Impl;
    // Declaration order matters here — members are destroyed in reverse
    // order, and m_impl MUST be destroyed before m_entry. Destroying m_impl
    // (on the native/Boost.Context backend) can resume a still-suspended
    // fiber one last time to deliver forced_unwind (see scheduler.cxx's
    // rethrowIfForcedUnwind comment) — that resume re-enters code still
    // holding onto m_entry's captured closure state (e.g. Scheduler::
    // startTask's captured Task block). If m_entry were destroyed first (as
    // it was when declared before m_impl), that resume would run against an
    // already-freed closure — a real, reproduced use-after-free (confirmed
    // via Valgrind: a Task that never replies, awaited with a timeout that
    // expires, destroys the still-blocked-in-receive Task fiber at scope
    // exit and corrupts memory).
    EntryFn m_entry;
    bool m_started = false;
    bool m_finished = false;
    std::unique_ptr<Impl> m_impl;
};

} // namespace kex::interpreter
