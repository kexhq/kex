#pragma once
// What tooling reads of a checked program's names: its symbols with the
// references to them, and the names in scope at each point. Definitions,
// references and completions are answered from it (symbol_queries.hxx).
//
// It is built from the checker's own record (TypeChecker::nameUses,
// scopeNames): which expressions name something no scope binds, the module
// each is written in and the `using`s in force there. No name is resolved
// here a second time; a use the checker bound to a module's function or
// constant is recorded on that declaration.
#include "symbol_info.hxx"
#include "imported_interfaces.hxx"

#include <string>
#include <vector>

namespace kex::semantic {

class ProgramModules;
class TypeChecker;

struct NameIndex {
    std::vector<SymbolInfo> symbols;
    std::vector<ScopeSnapshot> completionScopes;
};

// Records on `modules`' symbols the references `program`, checked by
// `checker` with its names recorded, makes, and returns the names in scope
// at each point of it. `path` is the file preferred for a name several
// declare. With `fileLocalTopLevel`, `modules` holds a whole workspace's
// files, not one program's: a file's top-level declarations are its own.
auto indexNames(const ast::Program& program, const std::string& path,
                const TypeChecker& checker, const ImportedInterfaces* imports,
                ProgramModules& modules, bool fileLocalTopLevel = false)
    -> std::vector<ScopeSnapshot>;

// The index of one program: its own symbols (collectSymbols), indexed.
auto buildNameIndex(const ast::Program& program, const std::string& path,
                    const TypeChecker& checker, const ImportedInterfaces* imports) -> NameIndex;

} // namespace kex::semantic
