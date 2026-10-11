#pragma once

#include <string>

namespace kex {

// A callee as the user writes it. Module and type members are keyed
// `Owner::name` internally; a diagnostic shows `Owner.name`.
inline auto sourceName(const std::string& name) -> std::string {
    std::string result;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name.compare(i, 2, "::") == 0) {
            result += '.';
            ++i;
        } else {
            result += name[i];
        }
    }
    return result;
}

} // namespace kex
