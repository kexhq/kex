#include "resolver.hxx"

#include <cctype>
#include <filesystem>
#include <sstream>
#include <utility>

namespace kex::module {

Resolver::Resolver(std::vector<std::string> roots) : m_roots(std::move(roots)) {}

auto Resolver::isForeignNamespace(const std::string& name) -> bool {
    for (const auto& prefix : {"Erlang.", "Elixir.", "Gleam."})
        if (name.rfind(prefix, 0) == 0) return true;
    return false;
}

namespace {

auto sourcePath(const std::string& moduleName) -> std::filesystem::path {
    std::stringstream parts(moduleName);
    std::filesystem::path path;
    std::string part;
    while (std::getline(parts, part, '.')) {
        for (auto& c : part)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        path /= part;
    }
    path += ".kex";
    return path;
}

auto candidates(const std::string& moduleName, std::string currentModule)
    -> std::vector<std::string> {
    std::vector<std::string> result;
    while (!currentModule.empty()) {
        result.push_back(currentModule + "." + moduleName);
        const auto dot = currentModule.rfind('.');
        if (dot == std::string::npos) break;
        currentModule.resize(dot);
    }
    result.push_back(moduleName);
    return result;
}

} // namespace

auto Resolver::resolve(const std::string& moduleName,
                       const std::string& currentModule) const -> std::optional<Resolution> {
    if (isForeignNamespace(moduleName)) return std::nullopt;
    // Relative module identities take precedence over absolute ones. Once an
    // identity has a winning source-root match, record any later matches so
    // callers can diagnose the shadowing without changing resolution order.
    for (const auto& candidateName : candidates(moduleName, currentModule)) {
        std::optional<Resolution> result;
        for (const auto& root : m_roots) {
            auto direct = std::filesystem::path(root) / sourcePath(candidateName);
            std::optional<std::string> matchedPath;
            if (std::filesystem::is_regular_file(direct)) {
                matchedPath = direct.string();
            }

            const auto dot = candidateName.find('.');
            if (!matchedPath && dot != std::string::npos) {
                auto container = std::filesystem::path(root)
                    / sourcePath(candidateName.substr(0, dot));
                if (std::filesystem::is_regular_file(container))
                    matchedPath = container.string();
            }

            if (!matchedPath) continue;
            if (!result)
                result = Resolution{candidateName, *matchedPath, {}};
            else if (*matchedPath != result->path)
                result->shadowedPaths.push_back(*matchedPath);
        }
        if (result) return result;
    }
    return std::nullopt;
}

} // namespace kex::module
