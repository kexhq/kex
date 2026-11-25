#pragma once

#include <cstddef>
#include <cstdint>

// How close the running code is to the end of its stack. The parser, the
// checker, lowering and the walker each recurse once per level of what they
// read; a program nested past what the stack holds is refused with a
// diagnostic, never a segmentation fault (kexhq/kex#709). A fixed count
// (the parser's nesting limit) is a rule of the language; this is the
// backstop that holds whatever the build's frame sizes and the thread's
// stack: a debug or sanitizer build overflowed where a release one did not.
namespace kex::stack {

// Whether the stack has less than its margin left: the caller stops
// recursing and reports. +slack+ lets a check that runs more often than
// another go that much deeper, so the more telling one reports first: the
// walker's per-call check names the function, its per-expression one
// cannot.
auto nearEnd(std::size_t slack = 0) -> bool;

// The lowest address the current code may reach before nearEnd() says so.
// A fiber running on a stack of its own sets its floor while it runs, and
// whoever resumed it restores theirs; 0 means the thread's own stack.
auto floor() -> std::uintptr_t;
auto setFloor(std::uintptr_t lowest) -> void;

} // namespace kex::stack
