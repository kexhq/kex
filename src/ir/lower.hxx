#pragma once
// AST → IR lowering (desugaring). Consumes the (parsed, name-resolvable) AST
// and produces the normalized IR of ir.hxx. This is where UFCS resolution,
// operator lowering, and ANF normalization happen.
//
// Walking-skeleton scope: handles the subset of the language needed to take a
// simple program end-to-end through the new pipeline. Any construct not yet
// ported throws ir::LowerError with a precise message, so `--ir` fails loudly
// (and the default string-emitter path is entirely unaffected). Coverage is
// widened construct-by-construct until it subsumes core_erlang.cxx.
#include "ir.hxx"
#include "../ast/ast.hxx"
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kex::ir {

struct LowerError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Lower a whole program to an IR module. `fileStem` becomes the module name
// (`kex_<stem>`), matching the existing backend's convention. `preludeFns` is
// the set of stdlib function names provided by the shared kex_prelude module;
// a UFCS call to one (that isn't a local method) routes to `kex_prelude:<fn>`.
// externalModules maps loaded-module short names (e.g. "BinaryTree") to their
// BEAM atoms (e.g. "Kex.BinaryTree"). Used by the REPL to resolve calls into
// modules loaded via /load. Each module's exports/methods are in externalExports
// keyed by "ModuleName.functionName".
struct ExternalModules {
    std::unordered_map<std::string, std::string> nameToAtom;
    std::unordered_map<std::string, std::string> exportToBeamFn;
    std::unordered_map<std::string, int> exportArity;
};

auto lowerProgram(const ast::Program& prog, const std::string& fileStem,
                  const std::unordered_set<std::string>& preludeFns = {},
                  const std::string& sourcePath = "",
                  const ExternalModules* externals = nullptr) -> Module;

// Lower a compilation unit using the module-system BEAM mapping. The first
// result is the file-local Kex.Global module; every explicit Kex module is a
// separate `Kex.<Name>` module.
auto lowerModules(const ast::Program& prog, const std::string& fileStem,
                  const std::unordered_set<std::string>& preludeFns = {},
                  const std::string& sourcePath = "") -> std::vector<Module>;

} // namespace kex::ir
