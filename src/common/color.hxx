#pragma once

namespace kex::color {

constexpr auto reset   = "\033[0m";
constexpr auto bold    = "\033[1m";
constexpr auto dim     = "\033[2m";
constexpr auto italic  = "\033[3m";
constexpr auto underline = "\033[4m";
constexpr auto blink   = "\033[5m";
constexpr auto reverse = "\033[7m";
constexpr auto hidden  = "\033[8m";
constexpr auto strikethrough = "\033[9m";
constexpr auto red     = "\033[31m";
constexpr auto green   = "\033[32m";
constexpr auto yellow  = "\033[33m";
constexpr auto blue    = "\033[34m";
constexpr auto magenta = "\033[35m";
constexpr auto cyan    = "\033[36m";
constexpr auto white   = "\033[37m";
constexpr auto gray    = "\033[90m";
constexpr auto purple  = "\033[95m";

// Cursor/screen control. `clear` erases the screen AND homes the cursor, which
// is what "clear the display" means to a caller; `home` alone redraws over the
// previous frame without the blank flash in between.
constexpr auto clearScreen = "\033[2J\033[H";
constexpr auto home        = "\033[H";
constexpr auto clearLine   = "\033[2K\r";

inline bool enabled = true;

inline auto apply(const char* code) -> const char* {
    return enabled ? code : "";
}

} // namespace kex::color
