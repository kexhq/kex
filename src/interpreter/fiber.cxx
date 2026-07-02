#include "fiber.hxx"

#ifdef __EMSCRIPTEN__
#include <emscripten/fiber.h>
#include <vector>
#else
// Not ucontext.h: on Apple Silicon macOS, ucontext's swapcontext does not
// reliably save/restore FP/NEON register state across a context switch —
// confirmed empirically (heap corruption from garbage freed as a pointer
// whose bytes were exactly the bit pattern of 1.0f) before switching to
// this. Boost.Context's fcontext backend is the well-tested primitive
// purpose-built for this; see docs/fiber-process-plan.md's fiber section
// for the fuller writeup of why plain ucontext was rejected.
#include <boost/context/fiber.hpp>
#endif

namespace kex::interpreter {

namespace {
// Not actually per-OS-thread state (there's exactly one OS thread in this
// design) — named thread_local defensively, in case this ever runs on a
// worker-thread-per-tab wasm setup later.
thread_local Fiber* g_currentFiber = nullptr;
}

#ifndef __EMSCRIPTEN__

namespace ctx = boost::context;

struct Fiber::Impl {
    // The continuation used to enter/re-enter this fiber. Holds the
    // not-yet-started entry before the first resume(); after that, holds
    // whatever continuation the fiber's own yieldToScheduler() call left
    // behind for "the next time the scheduler wants to resume me".
    ctx::fiber intoFiber;
};

namespace {
// A pointer to the currently-running fiber's own `callerSlot` local (see
// the constructor below) — set for the duration that fiber's code is
// executing, read by yieldToScheduler() to know which continuation to
// resume next.
auto g_yieldSlot() -> ctx::fiber*& {
    thread_local ctx::fiber* slot = nullptr;
    return slot;
}
}

Fiber::Fiber(EntryFn entry, std::size_t stackSize)
    : m_impl(std::make_unique<Impl>()), m_entry(std::move(entry)) {
    m_impl->intoFiber = ctx::fiber(
        std::allocator_arg, ctx::fixedsize_stack(stackSize),
        [this](ctx::fiber&& caller) -> ctx::fiber {
            // `caller` is the continuation representing "the scheduler,
            // ready to be resumed" — it needs to live for this fiber's
            // entire lifetime (every yieldToScheduler() call, however deep
            // in the call stack, needs to reach it and update it with the
            // fresh continuation each resume/yield cycle produces), so it's
            // a local of this outer frame, addressed via a thread_local
            // slot rather than passed explicitly through every call.
            ctx::fiber callerSlot = std::move(caller);
            ctx::fiber* prevSlot = g_yieldSlot();
            g_yieldSlot() = &callerSlot;
            trampolineBody();
            g_yieldSlot() = prevSlot;
            return std::move(callerSlot);
        });
}

Fiber::~Fiber() = default;

auto Fiber::resume() -> void {
    if (m_finished) return;

    Fiber* prevFiber = g_currentFiber;
    g_currentFiber = this;
    m_started = true;

    m_impl->intoFiber = std::move(m_impl->intoFiber).resume();

    g_currentFiber = prevFiber;
}

auto Fiber::yieldToScheduler() -> void {
    Fiber* self = g_currentFiber;
    if (!self) return; // called outside any fiber — harmless no-op
    ctx::fiber* slot = g_yieldSlot();
    if (!slot) return;
    *slot = std::move(*slot).resume();
}

auto Fiber::trampolineBody() -> void {
    m_entry();
    m_finished = true;
}

#else // __EMSCRIPTEN__

namespace {
thread_local Fiber* g_startingFiber = nullptr;
}

struct Fiber::Impl {
    emscripten_fiber_t ctx{};
    emscripten_fiber_t callerCtx{};
    std::vector<char> stack;
    std::vector<char> asyncifyStack;
};

namespace {
auto fiberTrampoline(void* arg) -> void {
    static_cast<Fiber*>(arg)->trampolineBody();
}
}

Fiber::Fiber(EntryFn entry, std::size_t stackSize)
    : m_impl(std::make_unique<Impl>()), m_entry(std::move(entry)) {
    m_impl->stack.resize(stackSize);
    // Emscripten's own asyncify scratch buffer, separate from the C stack —
    // see emscripten_fiber_init's signature.
    m_impl->asyncifyStack.resize(stackSize);
    emscripten_fiber_init(&m_impl->ctx, &fiberTrampoline, this,
                           m_impl->stack.data(), m_impl->stack.size(),
                           m_impl->asyncifyStack.data(), m_impl->asyncifyStack.size());
}

Fiber::~Fiber() = default;

auto Fiber::resume() -> void {
    if (m_finished) return;

    Fiber* prevFiber = g_currentFiber;
    g_currentFiber = this;
    m_started = true;

    emscripten_fiber_swap(&m_impl->callerCtx, &m_impl->ctx);
    g_currentFiber = prevFiber;
}

auto Fiber::yieldToScheduler() -> void {
    Fiber* self = g_currentFiber;
    if (!self) return;
    emscripten_fiber_swap(&self->m_impl->ctx, &self->m_impl->callerCtx);
}

auto Fiber::trampolineBody() -> void {
    m_entry();
    m_finished = true;
    emscripten_fiber_swap(&m_impl->ctx, &m_impl->callerCtx);
}

#endif

} // namespace kex::interpreter
