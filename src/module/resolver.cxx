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

auto lowered(const std::string& part) -> std::string {
    std::string result = part;
    for (auto& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// `MockData` names `mockdata.kex` and `mock_data.kex` alike: a module name is
// CamelCase and a file name is conventionally snake_case, so the resolver has
// to try both spellings rather than force the run-together one.
auto snakeCased(const std::string& part) -> std::string {
    std::string result;
    for (std::size_t i = 0; i < part.size(); ++i) {
        const auto c = static_cast<unsigned char>(part[i]);
        const bool boundary = i > 0 && std::isupper(c)
            && (std::islower(static_cast<unsigned char>(part[i - 1]))
                || std::isdigit(static_cast<unsigned char>(part[i - 1]))
                || (i + 1 < part.size()
                    && std::islower(static_cast<unsigned char>(part[i + 1]))));
        if (boundary && !result.empty() && result.back() != '_') result += '_';
        result += static_cast<char>(std::tolower(c));
    }
    return result;
}

// Every file spelling a module name can take, most specific first. Each name
// part contributes its own spellings, so `Web.MockData` reaches
// `web/mock_data.kex` as readily as `web/mockdata.kex`.
auto sourcePaths(const std::string& moduleName)
    -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> paths{{}};
    std::stringstream parts(moduleName);
    std::string part;
    while (std::getline(parts, part, '.')) {
        std::vector<std::string> spellings{lowered(part)};
        if (auto snake = snakeCased(part); snake != spellings.front())
            spellings.push_back(std::move(snake));

        std::vector<std::filesystem::path> extended;
        extended.reserve(paths.size() * spellings.size());
        for (const auto& prefix : paths)
            for (const auto& spelling : spellings)
                extended.push_back(prefix / spelling);
        paths = std::move(extended);
    }
    for (auto& path : paths) path += ".kex";
    return paths;
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
            std::optional<std::string> matchedPath;
            for (const auto& relative : sourcePaths(candidateName)) {
                auto direct = std::filesystem::path(root) / relative;
                if (std::filesystem::is_regular_file(direct)) {
                    matchedPath = direct.string();
                    break;
                }
            }

            const auto dot = candidateName.find('.');
            if (!matchedPath && dot != std::string::npos) {
                for (const auto& relative :
                     sourcePaths(candidateName.substr(0, dot))) {
                    auto container = std::filesystem::path(root) / relative;
                    if (std::filesystem::is_regular_file(container)) {
                        matchedPath = container.string();
                        break;
                    }
                }
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
