#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace kex {

// A block-shaped backtick literal (one whose opening delimiter is followed
// immediately by a newline) is dedented by the indentation its own content
// shares, so an indented literal still spells a file's exact bytes. This is
// the amount to strip: the longest leading-whitespace run that every nonblank
// line starts with.
//
// Blank lines never participate — they would otherwise force the margin to
// nothing. Comparison is by literal characters, so a body mixing tabs and
// spaces simply shares a shorter prefix instead of being an error.
//
// The caller is expected to have already removed the opening newline and the
// closing delimiter's own whitespace-only line; neither is content.
inline auto rawStringMargin(std::string_view body) -> std::string {
    std::string margin;
    bool seenLine = false;

    size_t lineStart = 0;
    while (lineStart <= body.size()) {
        auto lineEnd = body.find('\n', lineStart);
        bool hasNewline = lineEnd != std::string_view::npos;
        if (!hasNewline) lineEnd = body.size();

        auto contentEnd = lineEnd;
        if (contentEnd > lineStart && body[contentEnd - 1] == '\r')
            contentEnd--;

        size_t indent = lineStart;
        while (indent < contentEnd &&
               (body[indent] == ' ' || body[indent] == '\t'))
            indent++;

        if (indent < contentEnd) {  // nonblank
            auto leading = body.substr(lineStart, indent - lineStart);
            if (!seenLine) {
                margin = std::string(leading);
                seenLine = true;
            } else {
                auto shared = std::min(margin.size(), leading.size());
                size_t common = 0;
                while (common < shared && margin[common] == leading[common])
                    common++;
                margin.resize(common);
            }
            if (margin.empty()) return margin;
        }

        if (!hasNewline) break;
        lineStart = lineEnd + 1;
    }

    return margin;
}

// The closing delimiter of a block-shaped literal is not content, and
// neither is the whitespace before it when it sits alone on its line.
// Returns where that line begins, so the caller can drop it while
// keeping the newline that precedes it — block literals end in \n.
inline auto rawStringClosingLineStart(std::string_view body)
    -> std::optional<size_t> {
    auto lastNewline = body.rfind('\n');
    size_t lineStart = lastNewline == std::string_view::npos
        ? 0
        : lastNewline + 1;
    auto tail = body.substr(lineStart);
    bool onlyWhitespace = std::all_of(
        tail.begin(), tail.end(),
        [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
    if (!onlyWhitespace) return std::nullopt;
    return lineStart;
}

}  // namespace kex
