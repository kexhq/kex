#include "common/stack_guard.hxx"

#include <algorithm>
#include <cstddef>

#if defined(__EMSCRIPTEN__)
#include <emscripten/stack.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace kex::stack {

namespace {

// Kept back for what runs between two checks (a deep native function, the
// diagnostic's own rendering, unwinding): a sanitizer build's frames are
// several times a release build's.
constexpr std::size_t kMargin = 1024 * 1024;

// The thread's own stack bottom plus the margin; 0 where it cannot be found,
// which turns the guard off rather than tripping it.
auto threadFloor() -> std::uintptr_t {
    std::uintptr_t lowest = 0;
    std::size_t size = 0;
#if defined(__EMSCRIPTEN__)
    lowest = emscripten_stack_get_end();
    size = emscripten_stack_get_base() - lowest;
#elif defined(__APPLE__)
    const auto self = pthread_self();
    const auto top = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
    size = pthread_get_stacksize_np(self);
    lowest = top - size;
#elif defined(__linux__)
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
        void* address = nullptr;
        if (pthread_attr_getstack(&attributes, &address, &size) == 0) {
            lowest = reinterpret_cast<std::uintptr_t>(address);
        }
        pthread_attr_destroy(&attributes);
    }
    // The main thread's stack grows on demand up to RLIMIT_STACK. glibc reports
    // that whole extent; musl (Alpine) only what is mapped so far, a few pages
    // early on, which put the floor just below where the program started and
    // refused the prelude as nested too deeply. Measured from the top, both
    // agree. Other threads have a fixed stack, reported the same by both.
    if (lowest != 0 && getpid() == static_cast<pid_t>(syscall(SYS_gettid))) {
        struct rlimit limit{};
        // Unlimited: at least the usual 8 MB, which a stack that may grow
        // without end certainly has.
        constexpr std::size_t kUnlimited = 8 * 1024 * 1024;
        if (getrlimit(RLIMIT_STACK, &limit) == 0) {
            const auto extent = limit.rlim_cur == RLIM_INFINITY
                                    ? kUnlimited
                                    : static_cast<std::size_t>(limit.rlim_cur);
            if (extent > size) {
                const auto top = lowest + size;
                size = extent;
                lowest = top - size;
            }
        }
    }
#endif
    if (lowest == 0 || size == 0) {
        return 0;
    }
    return lowest + std::min(kMargin, size / 4);
}

thread_local std::uintptr_t t_fiberFloor = 0;

} // namespace

auto floor() -> std::uintptr_t {
    return t_fiberFloor;
}

auto setFloor(std::uintptr_t lowest) -> void {
    t_fiberFloor = lowest;
}

// The frame's own address, not a local's: under ASan's
// detect_stack_use_after_return (on by default on Linux) locals live on a
// heap-allocated fake stack, far from the real one. A check probing a local
// said the stack was spent; a fiber marking its top by one put its floor
// somewhere in the heap.
// Emscripten's floor is its shadow stack's, where locals live, and it has no
// ASan.
__attribute__((noinline)) auto here() -> std::uintptr_t {
#if defined(__EMSCRIPTEN__)
    char probe = 0;
    return reinterpret_cast<std::uintptr_t>(&probe);
#else
    return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
}

auto nearEnd(std::size_t slack) -> bool {
    thread_local const std::uintptr_t ownFloor = threadFloor();
    const auto limit = t_fiberFloor != 0 ? t_fiberFloor : ownFloor;
    return limit > slack && here() < limit - slack;
}

} // namespace kex::stack
