#include "resolver.hxx"

#include <filesystem>
#include <sstream>

namespace kex::module {

Resolver::Resolver(std::vector<std::string> roots) : m_roots(std::move(roots)) {}

auto Resolver::isForeignNamespace(const std::string& name) -> bool {
    for (const auto& prefix : {"Erlang.", "Elixir.", "Gleam."})
        if (name.rfind(prefix, 0) == 0) return true;
    return false;
}

auto Resolver::resolve(const std::string& moduleName) const -> std::optional<std::string> {
    if (isForeignNamespace(moduleName)) return std::nullopt;
    std::stringstream parts(moduleName);
    std::vector<std::string> path;
    std::string part;
    while (std::getline(parts, part, '.')) {
        for (auto& c : part) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        path.push_back(std::move(part));
    }
    for (const auto& root : m_roots) {
        std::filesystem::path candidate(root);
        for (const auto& segment : path) candidate /= segment;
        candidate += ".kex";
        if (std::filesystem::is_regular_file(candidate)) return candidate.string();
    }
    return std::nullopt;
}

} // namespace kex::module
