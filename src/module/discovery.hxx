#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace kex::ast {
struct Program;
}

namespace kex::module {

// A file's module header: the name its first significant line declares, and
// that line's number. Nothing when that line is anything else — only one line
// is read from a file that is not a module.
struct ModuleHeader {
  std::string name;
  int line;
};

auto moduleHeaderOf(const std::filesystem::path &file)
    -> std::optional<ModuleHeader>;

// The directories a walk for modules under a source root does not enter.
auto skippedByModuleWalk(const std::string &directory) -> bool;

// The standard library's roots, canonical: a walk for the project's own
// modules leaves them to the prelude.
auto standardModuleRoots() -> std::unordered_set<std::string>;

// Every module a `using` names across a program, at file scope and inside
// nested modules, in source order. Foreign namespaces (`BEAM.`) are left out:
// they have no Kex source.
auto collectUsingModules(const kex::ast::Program &program)
    -> std::vector<std::string>;

// Every module a program declares, a nested one by its full name and by the
// name it is written with.
auto declaredModuleNames(const kex::ast::Program &program)
    -> std::unordered_set<std::string>;

// The module name a file's place under `root` gives it:
// `app/services/authority.kex` is `App.Services.Authority`.
auto moduleNameForPath(const std::filesystem::path &relative) -> std::string;

// A path as a diagnostic shows it: relative to the working directory.
auto shownPath(const std::filesystem::path &path) -> std::string;

struct DeclaringFile {
  std::filesystem::path root;
  std::filesystem::path file;
  int line;
};

// The file under one of the program's own source roots (not the standard
// library's) that declares `module <name>` as its header, with the root it
// was found under.
auto fileDeclaringModule(const std::string &name,
                         const std::vector<std::string> &roots)
    -> std::optional<DeclaringFile>;

} // namespace kex::module
