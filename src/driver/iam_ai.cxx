#include "iam_ai.hxx"
#include "../common/color.hxx"
#include "../lexer/lexer.hxx"
#include <array>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace kex::driver {

namespace {

namespace c = kex::color;

constexpr std::array thinking = {
    "Pondering",   "Reasoning",  "Ruminating", "Cogitating", "Kexifying",  "Kexinating",
    "Kexinizing",  "Kexafating", "Mulling",    "Percolating", "Noodling",  "Kexploring",
    "Kexpanding",  "Deliberating",
};

constexpr std::array worked = {
    "Cogitated", "Pondered",   "Mulled",     "Ruminated",  "Brewed",     "Percolated",
    "Marinated", "Simmered",   "Noodled",    "Deliberated", "Kexinated", "Kexified",
    "Kexinized", "Kexafated",  "Kexpanded",  "Kexecuted",  "Kexorcised", "Kexcavated",
    "Kexpedited", "Kexplored",
};

constexpr std::array openers = {
    "Let me start by understanding the structure of your project.",
    "I'll take a look at your code and compile it step by step.",
    "Great question! Let me compile this for you.",
    "I'll begin by exploring the codebase.",
};

constexpr std::array wrongCauses = {
    "The root cause is clearly a missing semicolon. Kex has no semicolons, but still.",
    "This looks like a caching issue. Clearing the cache should fix it.",
    "The problem is that your values are immutable. I'd suggest making them mutable.",
    "This is a race condition in the type checker. Running the build again will help.",
    "The error is caused by trailing whitespace somewhere in the standard library.",
};

constexpr std::array spinner = {"·", "✢", "✳", "✶", "✻", "✽"};

// Cost per million tokens, in dollars. Made up.
constexpr double inputRate = 15.0;
constexpr double outputRate = 75.0;
// A model's rule of thumb: four bytes of output to a token.
constexpr std::uintmax_t bytesPerToken = 4;

// FNV-1a: the same entry always gets the same words, on every platform.
auto seedFor(std::string_view text) -> std::uint64_t {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char ch : text) {
        hash = (hash ^ ch) * 1099511628211ULL;
    }
    return hash;
}

constexpr auto hideCursor = "\033[?25l";
constexpr auto showCursor = "\033[?25h";

void showCursorAndStop(int signal) {
    (void)!write(STDERR_FILENO, showCursor, std::strlen(showCursor));
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

auto toTerminal() -> bool {
    return isatty(STDERR_FILENO) != 0;
}

auto tokens(std::uintmax_t count) -> std::string {
    char text[32];
    if (count >= 1'000'000) {
        std::snprintf(text, sizeof text, "%.1fM", static_cast<double>(count) / 1e6);
    } else if (count >= 1'000) {
        std::snprintf(text, sizeof text, "%.1fk", static_cast<double>(count) / 1e3);
    } else {
        std::snprintf(text, sizeof text, "%ju", count);
    }
    return text;
}

auto duration(double seconds) -> std::string {
    char text[32];
    const int minutes = static_cast<int>(seconds) / 60;
    if (minutes > 0) {
        std::snprintf(text, sizeof text, "%dm %.0fs", minutes, seconds - minutes * 60.0);
    } else {
        std::snprintf(text, sizeof text, "%.1fs", seconds);
    }
    return text;
}

auto plural(std::uintmax_t count, const char* noun) -> std::string {
    return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

auto mark(const char* symbol) -> void {
    std::fprintf(stderr, "%s%s%s%s ", c::apply(c::bold), c::apply(c::purple), symbol,
                 c::apply(c::reset));
}

} // namespace

IamAi::IamAi(std::string entry, std::string outputDir)
    : m_entry(std::move(entry)), m_outputDir(std::move(outputDir)), m_seed(seedFor(m_entry)),
      m_start(std::chrono::steady_clock::now()),
      m_fileStart(std::filesystem::file_time_type::clock::now()) {
    // Two different ones after "Thinking".
    const auto first = pick(thinking.size());
    const auto second = (first + 1 + pick(thinking.size() - 1)) % thinking.size();
    const std::array<const char*, 3> verbs = {"Thinking", thinking[first], thinking[second]};
    if (toTerminal()) {
        // The cursor is hidden while the line redraws, and shown again even
        // when ^C ends it there.
        std::fprintf(stderr, "%s", hideCursor);
        const auto previous = std::signal(SIGINT, showCursorAndStop);
        constexpr int framesPerVerb = 8;
        int frame = 0;
        for (const char* verb : verbs) {
            for (int i = 0; i < framesPerVerb; i++, frame++) {
                std::fprintf(stderr, "%s", c::clearLine);
                mark(spinner[frame % spinner.size()]);
                std::fprintf(stderr, "%s…", verb);
                std::fflush(stderr);
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            }
        }
        std::fprintf(stderr, "%s%s", c::clearLine, showCursor);
        std::signal(SIGINT, previous);
    } else {
        for (const char* verb : verbs) {
            mark("✻");
            std::fprintf(stderr, "%s…\n", verb);
        }
    }
    mark("✻");
    std::fprintf(stderr, "%s\n", openers[pick(openers.size())]);
    std::fflush(stderr);
}

IamAi::~IamAi() {
    if (!m_finished) {
        finish(1);
    }
}

auto IamAi::pick(std::size_t count) -> std::size_t {
    // One step of a 64-bit LCG (Knuth's MMIX constants).
    m_seed = m_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::size_t>(m_seed >> 33) % count;
}

auto IamAi::finish(int status) -> void {
    m_finished = true;
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();

    mark("✻");
    std::fprintf(stderr, "%s%s for %s%s\n", c::apply(c::gray), worked[pick(worked.size())],
                 duration(seconds).c_str(), c::apply(c::reset));

    if (status != 0) {
        std::fprintf(stderr, "\nI apologize — the build failed.\n%s\nI'm confident this will "
                             "fix it.\n",
                     wrongCauses[pick(wrongCauses.size())]);
        std::fflush(stderr);
        return;
    }

    // What this build wrote: the modules modified since it started.
    std::uintmax_t written = 0;
    std::uintmax_t writtenBytes = 0;
    std::error_code ec;
    for (const auto& file : std::filesystem::directory_iterator(m_outputDir, ec)) {
        std::error_code fileError;
        if (file.path().extension() == ".beam" &&
            file.last_write_time(fileError) >= m_fileStart && !fileError) {
            written++;
            writtenBytes += file.file_size(fileError);
        }
    }
    const auto read = kex::lexTally();
    const auto outputTokens = writtenBytes / bytesPerToken;
    const double cost =
        (static_cast<double>(read.tokens) * inputRate +
         static_cast<double>(outputTokens) * outputRate) /
        1e6;

    std::fprintf(
        stderr,
        "\nI've successfully compiled your project! Here's what I did:\n"
        "  • Read %s (↑ %s tokens)\n"
        "  • Wrote %s (↓ %s tokens)\n"
        "  • Cost: $%.2f\n"
        "Let me know if you'd like me to make any further changes!\n",
        plural(read.files, "file").c_str(), tokens(read.tokens).c_str(),
        plural(written, "module").c_str(), tokens(outputTokens).c_str(), cost);
    std::fflush(stderr);
}

} // namespace kex::driver
