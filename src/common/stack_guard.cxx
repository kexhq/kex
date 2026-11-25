#include "common/stack_guard.hxx"

#include <algorithm>
#include <cstddef>

#if defined(__EMSCRIPTEN__)
#include <emscripten/stack.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
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

auto nearEnd(std::size_t slack) -> bool {
    thread_local const std::uintptr_t ownFloor = threadFloor();
    const auto limit = t_fiberFloor != 0 ? t_fiberFloor : ownFloor;
    char probe = 0;
    return limit > slack && reinterpret_cast<std::uintptr_t>(&probe) < limit - slack;
}

} // namespace kex::stack
