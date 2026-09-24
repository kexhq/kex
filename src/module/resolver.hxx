#pragma once

#include <optional>
#include <string>
#include <vector>

namespace kex::module {

struct Resolution {
    std::string moduleName;
    std::string path;
    // Later source-root matches hidden by `path`, in source-root order.
    std::vector<std::string> shadowedPaths;
};

class Resolver {
public:
    explicit Resolver(std::vector<std::string> roots = {"lib", "src"});
    auto resolve(const std::string& moduleName,
                 const std::string& currentModule = "") const -> std::optional<Resolution>;
    static auto isForeignNamespace(const std::string& moduleName) -> bool;
    // The program's entry file, which no `using` may resolve to: its own
    // directory is a source root, so a script named after the module it
    // imports (`dimensions.kex` saying `using Dimensions`) would otherwise
    // import itself and shadow the real module. Process-wide, set once by the
    // CLI; empty means no file is excluded.
    static auto setEntryFile(const std::string& path) -> void;

private:
    std::vector<std::string> m_roots;
};

} // namespace kex::module
