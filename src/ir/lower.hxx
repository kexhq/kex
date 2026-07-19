#pragma once
// AST → IR lowering (desugaring). Consumes the (parsed, name-resolvable) AST
// and produces the normalized IR of ir.hxx. This is where UFCS resolution,
// operator lowering, and ANF normalization happen.
//
// Unsupported constructs throw ir::LowerError with a precise message rather
// than silently generating invalid Core Erlang.
#include "ir.hxx"
#include "../ast/ast.hxx"
#include "../semantic/imported_interfaces.hxx"
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::ir {

struct LowerError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// externalModules maps loaded-module short names (e.g. "BinaryTree") to their
// BEAM atoms (e.g. "Kex.BinaryTree"). Used by the REPL to resolve calls into
// modules loaded via /load. Each module's exports/methods are in externalExports
// keyed by "ModuleName.functionName".
struct ExternalModules {
    struct ReceiverFunction {
        std::string moduleAtom;
        std::string beamFunction;
        int beamArity = 0;
        // Source parameter names excluding the receiver. Used to order named
        // arguments before lowering to the positional BEAM call convention.
        std::vector<std::string> paramNames;
    };

    std::unordered_map<std::string, std::string> nameToAtom;
    std::unordered_map<std::string, std::string> exportToBeamFn;
    std::unordered_map<std::string, int> exportArity;
    std::unordered_map<std::string, std::vector<std::string>> exportParamNames;
    // Receiver functions are separate from ordinary module exports and are
    // populated only from package-declared provider modules.
    std::unordered_map<std::string, std::vector<ReceiverFunction>> receiverFunctions;
};

struct ExternalRecordLayout {
    std::string name;
    std::vector<std::string> fields;
};

auto lowerProgram(const ast::Program& prog, const std::string& fileStem,
                  const std::string& sourcePath = "",
                  const ExternalModules* externals = nullptr,
                  const std::vector<ExternalRecordLayout>* externalRecords = nullptr,
                  const std::unordered_map<const ast::MethodCall*,
                      semantic::ResolvedCallTarget>* resolvedCalls = nullptr,
                  bool preferExternalReceivers = false)
    -> Module;

// Lower a compilation unit using the module-system BEAM mapping. The first
// result is the file-local Kex.Global module; every explicit Kex module is a
// separate `Kex.<Name>` module.
auto lowerModules(const ast::Program& prog, const std::string& fileStem,
                  const std::string& sourcePath = "",
                  const std::vector<ExternalRecordLayout>* externalRecords = nullptr,
                  const ExternalModules* externals = nullptr,
                  const std::unordered_map<const ast::MethodCall*,
                      semantic::ResolvedCallTarget>* resolvedCalls = nullptr,
                  bool preferExternalReceivers = false)
    -> std::vector<Module>;

} // namespace kex::ir
