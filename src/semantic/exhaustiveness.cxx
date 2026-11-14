#include "exhaustiveness.hxx"

#include <algorithm>
#include <set>

namespace kex::semantic::exhaustiveness {

namespace {

using Row = std::vector<Space>;
using Matrix = std::vector<Row>;

auto anything() -> Space { return {}; }

// The rows that can match `constructor` (of `arity`) first: its arguments in
// place of it. A row starting with anything matches it with anything.
auto rowsTaking(const Matrix& rows, const std::string& constructor, std::size_t arity) -> Matrix {
    Matrix specialized;
    for (const auto& row : rows) {
        const auto& head = row.front();
        Row next;
        if (head.constructor.empty())
            next.assign(arity, anything());
        else if (head.constructor == constructor)
            next = head.arguments;
        else
            continue;
        next.insert(next.end(), row.begin() + 1, row.end());
        specialized.push_back(std::move(next));
    }
    return specialized;
}

// The rows that match whatever comes first.
auto rest(const Matrix& rows) -> Matrix {
    Matrix remaining;
    for (const auto& row : rows)
        if (row.front().constructor.empty()) remaining.emplace_back(row.begin() + 1, row.end());
    return remaining;
}

auto search(const Matrix& rows, std::size_t width, const Siblings& siblings, std::size_t limit)
    -> std::vector<Row> {
    if (width == 0) return rows.empty() ? std::vector<Row>{Row{}} : std::vector<Row>{};
    if (rows.empty()) return {Row(width, anything())};

    std::set<std::string> heads;
    for (const auto& row : rows)
        if (!row.front().constructor.empty()) heads.insert(row.front().constructor);
    const auto all = heads.empty() ? std::nullopt : siblings(*heads.begin());
    const bool complete =
        all && std::all_of(all->begin(), all->end(),
                           [&](const auto& sibling) { return heads.contains(sibling.first); });

    std::vector<Row> found;
    if (complete) {
        // Each constructor in turn, with what its arguments leave open.
        for (const auto& [constructor, arity] : *all) {
            for (auto& below : search(rowsTaking(rows, constructor, arity), arity + width - 1,
                                      siblings, limit - found.size())) {
                Space head{constructor, {below.begin(), below.begin() + static_cast<std::ptrdiff_t>(arity)}};
                Row row{std::move(head)};
                row.insert(row.end(), below.begin() + static_cast<std::ptrdiff_t>(arity), below.end());
                found.push_back(std::move(row));
                if (found.size() >= limit) return found;
            }
        }
        return found;
    }
    // Some value of the first column no head names: whatever the rows that
    // take anything there leave open past it.
    for (auto& below : search(rest(rows), width - 1, siblings, limit)) {
        std::vector<Space> missing;
        if (all)
            for (const auto& [constructor, arity] : *all)
                if (!heads.contains(constructor))
                    missing.push_back({constructor, std::vector<Space>(arity, anything())});
        if (missing.empty()) missing.push_back(anything());
        for (auto& head : missing) {
            Row row{std::move(head)};
            row.insert(row.end(), below.begin(), below.end());
            found.push_back(std::move(row));
            if (found.size() >= limit) return found;
        }
    }
    return found;
}

} // namespace

auto uncovered(const std::vector<std::vector<Space>>& rows, std::size_t width,
               const Siblings& siblings, std::size_t limit) -> std::vector<std::vector<Space>> {
    return search(rows, width, siblings, std::max<std::size_t>(limit, 1));
}

} // namespace kex::semantic::exhaustiveness
