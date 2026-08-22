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
#include <unordered_set>
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
    struct TraitMethod {
        std::string name;
        int arity = 1; // receiver plus explicit source parameters
    };
    struct ReceiverFunction {
        std::string moduleAtom;
        std::string beamFunction;
        int beamArity = 0;
        // Source parameter names excluding the receiver. Used to order named
        // arguments before lowering to the positional BEAM call convention.
        std::vector<std::string> paramNames;
        // Concrete receiver name when the provider is type-specific. Empty
        // for generic trait fallbacks and structural receiver families.
        std::string receiverType;
    };

    std::unordered_map<std::string, std::string> nameToAtom;
    std::unordered_map<std::string, std::string> exportToBeamFn;
    std::unordered_map<std::string, int> exportArity;
    std::unordered_map<std::string, std::vector<std::string>> exportParamNames;
    // Imported exports that are `foul`. A foul function takes the capability
    // context, so a call into one has to append it — without this a consumer
    // cannot know that the prelude's `Task.start` is start/2 rather than
    // start/1 (kexhq/kex#181).
    std::unordered_set<std::string> foulExports;
    // Receiver functions are separate from ordinary module exports and are
    // populated only from package-declared provider modules.
    std::unordered_map<std::string, std::vector<ReceiverFunction>> receiverFunctions;
    // Trait name -> required method names in declaration order. Dictionaries
    // use this stable order for their hidden function slots.
    std::unordered_map<std::string, std::vector<TraitMethod>> traitMethods;
};

// A variant tag declared outside the module being compiled (the prelude, or
// a loaded unit). Only display registration needs these: without them a
// compiled program printed prelude ADTs as raw tuples/atoms.
// `Type.of(x)` sites the checker typed concretely — see
// TypeChecker::staticTypeOfCalls. Lowering emits the recorded shape as a
// literal `Type` record; without an entry the call reaches the runtime
// fallback, which asks the value.
using StaticTypeOfCalls =
    std::unordered_map<const ast::MethodCall*, semantic::StaticTypeAnswer>;
using ExpressionTypes =
    std::unordered_map<const ast::Expr*, semantic::TypePtr>;

struct ExternalVariantTag {
    std::string tag;
    int arity = 0;
    std::string owner;
};

struct ExternalRecordLayout {
    std::string name;
    std::vector<std::string> fields;
    // BEAM module that owns the record's field accessors.
    std::string moduleAtom;
};

auto lowerProgram(const ast::Program& prog, const std::string& fileStem,
                  const std::string& sourcePath = "",
                  const ExternalModules* externals = nullptr,
                  const std::vector<ExternalRecordLayout>* externalRecords = nullptr,
                  const std::unordered_map<const ast::MethodCall*,
                      semantic::ResolvedCallTarget>* resolvedCalls = nullptr,
                  bool preferExternalReceivers = false,
                  const std::vector<ExternalVariantTag>* externalVariants = nullptr,
                  const StaticTypeOfCalls* staticTypeOfCalls = nullptr,
                  const ExpressionTypes* expressionTypes = nullptr)
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
                  bool preferExternalReceivers = false,
                  const std::vector<ExternalVariantTag>* externalVariants = nullptr,
                  const StaticTypeOfCalls* staticTypeOfCalls = nullptr,
                  const ExpressionTypes* expressionTypes = nullptr)
    -> std::vector<Module>;

// Lower the prelude with per-tier awareness. The full AST is used for the
// pre-pass (records, types, method owners); items are logically partitioned
// into tiers by tierBounds. Currently produces one merged module; future
// versions will produce separate modules per tier.
auto lowerProgramTiered(
    const ast::Program& prog,
    const std::array<size_t, 5>& tierBounds,
    const std::string& fileStem,
    const std::string& sourcePath = "",
    const ExternalModules* externals = nullptr,
    const std::vector<ExternalRecordLayout>* externalRecords = nullptr,
    const std::unordered_map<const ast::MethodCall*,
        semantic::ResolvedCallTarget>* resolvedCalls = nullptr,
    bool preferExternalReceivers = false) -> Module;

} // namespace kex::ir
