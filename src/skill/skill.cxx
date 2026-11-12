#include "skill.hxx"

#include <algorithm>
#include <cctype>

namespace kex::skill {

auto findTopic(std::string_view name) -> const Topic* {
    const auto sameLetter = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    for (const auto& topic : topics())
        if (std::equal(topic.name.begin(), topic.name.end(), name.begin(),
                       name.end(), sameLetter))
            return &topic;
    return nullptr;
}

} // namespace kex::skill
