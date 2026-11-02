#include "discovery.hxx"

#include "resolver.hxx"

#include "../ast/ast.hxx"
#include "../common/prelude_loader.hxx"
#include "../common/string_utils.hxx"

#include <cctype>
#include <fstream>
#include <system_error>

namespace kex::module {

auto moduleHeaderOf(const std::filesystem::path &file)
    -> std::optional<ModuleHeader> {
  std::ifstream input(file);
  std::string line;
  for (int number = 1; std::getline(input, line); ++number) {
    const auto start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line[start] == '#') continue;
    if (line.compare(start, 7, "module ") != 0) return std::nullopt;
    const auto rest = line.substr(start + 7);
    return ModuleHeader{rest.substr(0, rest.find_first_of(" \t\r")), number};
  }
  return std::nullopt;
}

auto skippedByModuleWalk(const std::string &directory) -> bool {
  return directory.starts_with(".") || directory == "ebin" ||
         directory == "build" || directory == "_build" ||
         directory == "node_modules";
}

auto standardModuleRoots() -> std::unordered_set<std::string> {
  std::unordered_set<std::string> standard;
  for (const auto &root : kex::standardLibraryModuleRoots()) {
    std::error_code ec;
    standard.insert(std::filesystem::weakly_canonical(root, ec).string());
  }
  return standard;
}

auto collectUsingModules(const kex::ast::Program &program) -> std::vector<std::string> {
  std::vector<std::string> result;
  auto addFrom = [&](const kex::ast::TypeName &tn) {
    std::string name;
    for (size_t i = 0; i < tn.parts.size(); i++) {
      if (i) name += ".";
      name += tn.parts[i];
    }
    if (!Resolver::isForeignNamespace(name))
      result.push_back(std::move(name));
  };
  auto scanModuleBody = [&](auto &self,
                            const std::vector<kex::ast::ModuleItem> &body) -> void {
    for (const auto &item : body) {
      if (auto *ub = std::get_if<std::unique_ptr<kex::ast::UsingBlock>>(&item))
        if (*ub) addFrom((*ub)->module);
      if (auto *md = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
        if (*md) self(self, (*md)->body);
    }
  };
  for (const auto &item : program.items) {
    if (auto *ub = std::get_if<std::unique_ptr<kex::ast::UsingBlock>>(&item))
      if (*ub) addFrom((*ub)->module);
    if (auto *md = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
      if (*md) scanModuleBody(scanModuleBody, (*md)->body);
  }
  return result;
}

auto declaredModuleNames(const kex::ast::Program &program)
    -> std::unordered_set<std::string> {
  std::unordered_set<std::string> names;
  const auto add = [&](auto &self, const kex::ast::ModuleDef &module,
                       const std::string &prefix) -> void {
    const auto full = prefix.empty() ? module.name : prefix + "." + module.name;
    names.insert(module.name);
    names.insert(full);
    for (const auto &item : module.body)
      if (auto *nested = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
        if (*nested) self(self, **nested, full);
  };
  for (const auto &item : program.items)
    if (auto *module = std::get_if<std::unique_ptr<kex::ast::ModuleDef>>(&item))
      if (*module) add(add, **module, "");
  return names;
}

auto moduleNameForPath(const std::filesystem::path &relative) -> std::string {
  std::string name;
  for (auto part : relative) {
    auto text = part.replace_extension().string();
    if (!name.empty()) name += ".";
    bool upper = true;
    for (const auto c : text) {
      if (c == '_') { upper = true; continue; }
      name += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
      upper = false;
    }
  }
  return name;
}

auto shownPath(const std::filesystem::path &path) -> std::string {
  std::error_code ec;
  auto relative = std::filesystem::relative(path, ec);
  return ec || relative.empty() ? path.string() : relative.string();
}

auto fileDeclaringModule(const std::string &name,
                         const std::vector<std::string> &roots)
    -> std::optional<DeclaringFile> {
  namespace fs = std::filesystem;
  const auto standard = standardModuleRoots();
  for (const auto &root : roots) {
    std::error_code ec;
    if (standard.contains(fs::weakly_canonical(root, ec).string())) continue;
    fs::recursive_directory_iterator walk(
        root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && walk != fs::recursive_directory_iterator(); walk.increment(ec)) {
      const auto &entry = *walk;
      const auto base = entry.path().filename().string();
      if (entry.is_directory(ec)) {
        if (skippedByModuleWalk(base)) walk.disable_recursion_pending();
        continue;
      }
      if (entry.path().extension() != ".kex") continue;
      if (const auto header = moduleHeaderOf(entry.path());
          header && header->name == name)
        return DeclaringFile{fs::path(root), entry.path(), header->line};
    }
  }
  return std::nullopt;
}

} // namespace kex::module
