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
    // Address of this fiber's own `callerSlot` local (see the constructor
    // below) — nullptr until the fiber has started running at least once.
    // g_yieldSlot() is a single thread_local shared by every fiber, so by
    // the time the scheduler gets back around to resuming a fiber that
    // *isn't* the one that ran most recently, some other fiber's own
    // resume/yield cycle will have overwritten it — resume() must restore
    // it to point at THIS fiber's slot every time, not just rely on the
    // one-time assignment the entry lambda makes on its first line below.
    // (Found the hard way: without this, a fiber resumed a second time —
    // e.g. a supervisor's poll loop calling sleepFor() repeatedly — reads
    // yieldToScheduler()'s slot as whichever *other* fiber ran last,
    // dereferencing into a currently-suspended, unrelated fiber's own
    // stack. Boost.Context caught it as an assertion failure rather than
    // silently corrupting memory, which is how this got found instead of
    // shipped.)
    ctx::fiber* selfSlot = nullptr;
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
    Impl* implPtr = m_impl.get();
    m_impl->intoFiber = ctx::fiber(
        std::allocator_arg, ctx::fixedsize_stack(stackSize),
        [this, implPtr](ctx::fiber&& caller) -> ctx::fiber {
            // `caller` is the continuation representing "the scheduler,
            // ready to be resumed" — it needs to live for this fiber's
            // entire lifetime (every yieldToScheduler() call, however deep
            // in the call stack, needs to reach it and update it with the
            // fresh continuation each resume/yield cycle produces), so it's
            // a local of this outer frame, addressed via a thread_local
            // slot rather than passed explicitly through every call.
            ctx::fiber callerSlot = std::move(caller);
            implPtr->selfSlot = &callerSlot;
            g_yieldSlot() = &callerSlot;
            trampolineBody();
            implPtr->selfSlot = nullptr;
            return std::move(callerSlot);
        });
}

Fiber::~Fiber() = default;

auto Fiber::resume() -> void {
    if (m_finished) return;

    Fiber* prevFiber = g_currentFiber;
    g_currentFiber = this;
    m_started = true;

    // Restore the shared yield-target slot to THIS fiber's own caller
    // continuation before jumping in — see Impl::selfSlot's comment for
    // why this can't just be left to the entry lambda's one-time setup.
    // nullptr on the very first resume (the lambda hasn't run yet and sets
    // it itself as its first line), harmless to skip in that case.
    if (m_impl->selfSlot) {
        g_yieldSlot() = m_impl->selfSlot;
    }

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

// The "native" context — wherever execution is when it's not inside any
// Fiber (i.e. the scheduler's own flat driving loop). Every Fiber::resume()
// call in this design originates from that same place (never fiber-to-
// fiber — the scheduler never nests), so unlike the native/Boost.Context
// backend (which needs a per-fiber caller slot, since boost's fiber
// objects are consumed/replaced on every swap), a single shared context
// suffices here and is reused across every fiber's resume/yield cycle.
//
// Must be initialized via emscripten_fiber_init_from_current_context, NOT
// left zero-initialized — confirmed against Emscripten's own
// test/test_fibers.cpp after a zero-initialized "caller" context produced
// an opaque "unreachable" trap on the very first swap, reproduced even in
// a minimal standalone repro with no Kex code involved at all. Lazily
// initialized on first use (captures "resume here" at that call site,
// which is safe precisely because every resume() call shares that one
// flat call site).
emscripten_fiber_t g_nativeContext{};
std::vector<char> g_nativeAsyncifyStack(65536);
bool g_nativeContextInitialized = false;

auto ensureNativeContext() -> void {
    if (g_nativeContextInitialized) return;
    emscripten_fiber_init_from_current_context(
        &g_nativeContext, g_nativeAsyncifyStack.data(), g_nativeAsyncifyStack.size());
    g_nativeContextInitialized = true;
}
}

struct Fiber::Impl {
    emscripten_fiber_t ctx{};
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
    ensureNativeContext();

    Fiber* prevFiber = g_currentFiber;
    g_currentFiber = this;
    m_started = true;

    emscripten_fiber_swap(&g_nativeContext, &m_impl->ctx);
    g_currentFiber = prevFiber;
}

auto Fiber::yieldToScheduler() -> void {
    Fiber* self = g_currentFiber;
    if (!self) return;
    emscripten_fiber_swap(&self->m_impl->ctx, &g_nativeContext);
}

auto Fiber::trampolineBody() -> void {
    m_entry();
    m_finished = true;
    emscripten_fiber_swap(&m_impl->ctx, &g_nativeContext);
}

#endif

} // namespace kex::interpreter
