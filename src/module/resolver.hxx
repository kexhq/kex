#pragma once

#include <optional>
#include <string>
#include <vector>

namespace kex::module {

struct Resolution {
    std::string moduleName;
    std::string path;
};

class Resolver {
public:
    explicit Resolver(std::vector<std::string> roots = {"lib", "src"});
    auto resolve(const std::string& moduleName,
                 const std::string& currentModule = "") const -> std::optional<Resolution>;
    static auto isForeignNamespace(const std::string& moduleName) -> bool;

private:
    std::vector<std::string> m_roots;
};

} // namespace kex::module
