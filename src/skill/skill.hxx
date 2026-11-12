#pragma once

#include <string_view>
#include <vector>

// `kex --skill`: the guide to writing Kex that the compiler carries for a
// model meeting the language for the first time. The text is the Markdown
// under src/skill/, built into the binary, so the guide a model reads is the
// guide for the compiler it is about to run and needs no file beside it.
namespace kex::skill {

struct Topic {
    std::string_view name; // the file's stem: `SKILL`, `errors`
    std::string_view text;
};

// The topic `--skill` prints when it is given no name. It is a complete
// SKILL.md, front matter included, so its output can be saved as one.
inline constexpr std::string_view kGuideTopic = "SKILL";

// Every topic, the guide first. Defined by the source CMake generates from
// src/skill/*.md (see embed.cmake).
auto topics() -> const std::vector<Topic>&;

// Null when there is no such topic. Names compare without regard to case.
auto findTopic(std::string_view name) -> const Topic*;

} // namespace kex::skill
