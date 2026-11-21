#pragma once

#include <string>
#include <string_view>

namespace kex::color {

constexpr auto reset = "\033[0m";
constexpr auto bold = "\033[1m";
constexpr auto dim = "\033[2m";
constexpr auto italic = "\033[3m";
constexpr auto underline = "\033[4m";
constexpr auto blink = "\033[5m";
constexpr auto reverse = "\033[7m";
constexpr auto hidden = "\033[8m";
constexpr auto strikethrough = "\033[9m";
constexpr auto red = "\033[31m";
constexpr auto green = "\033[32m";
constexpr auto yellow = "\033[33m";
constexpr auto blue = "\033[34m";
constexpr auto magenta = "\033[35m";
constexpr auto cyan = "\033[36m";
constexpr auto white = "\033[37m";
constexpr auto gray = "\033[90m";
constexpr auto purple = "\033[95m";

// Cursor/screen control. `clear` erases the screen AND homes the cursor, which
// is what "clear the display" means to a caller; `home` alone redraws over the
// previous frame without the blank flash in between.
constexpr auto clearScreen = "\033[2J\033[H";
constexpr auto home = "\033[H";
constexpr auto clearLine = "\033[2K\r";

inline bool enabled = true;

inline auto apply(const char* code) -> const char* {
    return enabled ? code : "";
}

// OSC 8 terminal hyperlinks (https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda):
// a diagnostic's code links to its page on oops.kex.run. Off unless styling
// is: main() turns it on for a terminal, and KEX_HYPERLINKS overrides that.
inline bool hyperlinks = false;

// +text+ linked to +url+ when hyperlinks are on, and +text+ alone otherwise.
// A terminal that does not know OSC 8 shows the text and ignores the rest.
inline auto link(std::string_view url, std::string_view text) -> std::string {
    if (!enabled || !hyperlinks || url.empty()) {
        return std::string(text);
    }
    std::string out = "\033]8;;";
    out += url;
    out += "\033\\";
    out += text;
    out += "\033]8;;\033\\";
    return out;
}

} // namespace kex::color
