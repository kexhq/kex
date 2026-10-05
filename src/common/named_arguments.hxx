#pragma once

#include "../ast/ast.hxx"
#include <algorithm>
#include <string>
#include <vector>

namespace kex {

// Labels reserve their declared slots; positional arguments fill the rest.
// An Optional parameter is still required unless it has a default value.
inline auto namedArgumentsFit(const std::vector<std::string>& names,
                              const std::vector<bool>& defaulted,
                              const std::vector<std::string>& labels,
                              std::size_t positionalCount) -> bool {
    std::vector<bool> supplied(names.size(), false);
    for (const auto& label : labels) {
        const auto found = std::find(names.begin(), names.end(), label);
        if (found == names.end()) return false;
        const auto slot = static_cast<std::size_t>(found - names.begin());
        if (supplied[slot]) return false;
        supplied[slot] = true;
    }
    for (std::size_t slot = 0; positionalCount && slot < supplied.size(); ++slot) {
        if (supplied[slot]) continue;
        supplied[slot] = true;
        --positionalCount;
    }
    if (positionalCount) return false;
    for (std::size_t slot = 0; slot < supplied.size(); ++slot)
        if (!supplied[slot] && (slot >= defaulted.size() || !defaulted[slot]))
            return false;
    return true;
}

inline auto namedArgumentsFit(const ast::FunctionClause& clause,
                              const std::vector<std::string>& labels,
                              std::size_t positionalCount) -> bool {
    std::vector<std::string> names;
    std::vector<bool> defaulted;
    for (const auto& param : clause.params) {
        names.push_back(param.name.value_or(""));
        defaulted.push_back(param.defaultValue.has_value());
    }
    return namedArgumentsFit(names, defaulted, labels, positionalCount);
}

} // namespace kex
