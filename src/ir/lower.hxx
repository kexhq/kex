#pragma once
// AST → IR lowering (desugaring). Consumes the (parsed, name-resolvable) AST
// and produces the normalized IR of ir.hxx. This is where UFCS resolution,
// operator lowering, and ANF normalization happen.
//
// Unsupported constructs throw ir::LowerError with a precise message rather
// than silently generating invalid Core Erlang.
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
    struct ReceiverFunction {
        std::string moduleAtom;
        std::string beamFunction;
        int beamArity = 0;
    };

    std::unordered_map<std::string, std::string> nameToAtom;
    std::unordered_map<std::string, std::string> exportToBeamFn;
    std::unordered_map<std::string, int> exportArity;
    // Receiver functions are separate from ordinary module exports and are
    // populated only from package-declared provider modules.
    std::unordered_map<std::string, std::vector<ReceiverFunction>> receiverFunctions;
};

struct ExternalRecordLayout {
    std::string name;
    std::vector<std::string> fields;
};

auto lowerProgram(const ast::Program& prog, const std::string& fileStem,
                  const std::unordered_set<std::string>& preludeFns = {},
                  const std::string& sourcePath = "",
                  const ExternalModules* externals = nullptr,
                  const std::vector<ExternalRecordLayout>* externalRecords = nullptr)
    -> Module;

// Lower a compilation unit using the module-system BEAM mapping. The first
// result is the file-local Kex.Global module; every explicit Kex module is a
// separate `Kex.<Name>` module.
auto lowerModules(const ast::Program& prog, const std::string& fileStem,
                  const std::unordered_set<std::string>& preludeFns = {},
                  const std::string& sourcePath = "",
                  const std::vector<ExternalRecordLayout>* externalRecords = nullptr,
                  const ExternalModules* externals = nullptr)
    -> std::vector<Module>;

} // namespace kex::ir
