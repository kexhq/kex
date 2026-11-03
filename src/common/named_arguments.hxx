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

// First clause the labels + positional count fit, or nullptr. The lowerer's
// clause-selection loops (named-arg dispatch over `fnNamedClauses`) share
// this; miss policy (throw, flags, diagnostics) stays per call site.
inline auto selectClause(const std::vector<const ast::FunctionClause*>& clauses,
                         const std::vector<std::string>& labels,
                         std::size_t positionalCount)
    -> const ast::FunctionClause* {
    for (const auto* clause : clauses)
        if (namedArgumentsFit(*clause, labels, positionalCount)) return clause;
    return nullptr;
}

struct PlacedArguments {
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    // Slot each label lands in, in label order; `npos` when no parameter
    // carries it. A repeated label lands where the first one did (the
    // caller's fit check rejects duplicates upstream).
    std::vector<std::size_t> labelSlots;
    // Slot each positional lands in, in order; `npos` when no free slot
    // is left. Positionals fill the slots labels left, front to back.
    std::vector<std::size_t> positionalSlots;
};

// Where a call's arguments land in a callee's parameter list: labels by
// name, then positionals (the trailing block counts as one) in the slots
// left. `firstSlot` reserves the front slots, which stay `npos`-free for
// the caller to fill (a pre-placed receiver). Pure index mapping —
// lowering, miss policy, and default-filling stay per call site.
inline auto placeArguments(const std::vector<std::string>& paramNames,
                           const std::vector<std::string>& labels,
                           std::size_t positionalCount,
                           std::size_t firstSlot = 0) -> PlacedArguments {
    PlacedArguments placed;
    std::vector<bool> taken(paramNames.size(), false);
    for (std::size_t slot = 0; slot < firstSlot && slot < taken.size(); ++slot)
        taken[slot] = true;
    for (const auto& label : labels) {
        std::size_t slot = PlacedArguments::npos;
        for (std::size_t i = firstSlot; i < paramNames.size(); ++i)
            if (paramNames[i] == label) {
                slot = i;
                break;
            }
        placed.labelSlots.push_back(slot);
        if (slot != PlacedArguments::npos) taken[slot] = true;
    }
    std::size_t next = firstSlot;
    for (std::size_t k = 0; k < positionalCount; ++k) {
        while (next < taken.size() && taken[next]) ++next;
        if (next >= taken.size()) {
            placed.positionalSlots.push_back(PlacedArguments::npos);
            continue;
        }
        placed.positionalSlots.push_back(next);
        taken[next++] = true;
    }
    return placed;
}

} // namespace kex
