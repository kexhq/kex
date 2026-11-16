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

namespace kex::hir {
class Program;
}

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
        // A foul receiver function takes the capability context, and a
        // receiver call resolves through this table rather than through
        // `exportToBeamFn` — so its foulness has to live here too
        // (kexhq/kex#181).
        bool isFoul = false;
        // Provided by one of this build's own units, lowered apart from the
        // unit calling it: for its own types it comes before
        // the prelude's generic function and its structural `==`.
        bool compiledApart = false;
        // As KexiMethod::foreignTarget: reached only where its module is
        // `using`-imported.
        bool foreignTarget = false;
    };

    std::unordered_map<std::string, std::string> nameToAtom;
    std::unordered_map<std::string, std::string> exportToBeamFn;
    std::unordered_map<std::string, int> exportArity;
    std::unordered_map<std::string, std::vector<std::string>> exportParamNames;
    // Imported exports' defaults as source text (KexiExport::defaultSources),
    // by the same key: which parameters have one, for the sentinel.
    std::unordered_map<std::string, std::vector<std::string>> exportDefaults;
    // Imported exports that are `foul`. A foul function takes the capability
    // context, so a call into one has to append it — without this a consumer
    // cannot know that the prelude's `Task.start` is start/2 rather than
    // start/1 (kexhq/kex#181).
    std::unordered_set<std::string> foulExports;
    // Source module names declared `capability` in another compiled unit. A
    // consumer must recognise an imported capability to emit the substitution
    // dispatch for it — otherwise `with FS.File = ...` silently keeps calling
    // the real implementation (kexhq/kex#143).
    std::unordered_set<std::string> capabilityModules;
    // A capability's own members where its interface lists them
    // (KexiStructuralMetadata::capabilityMembers); otherwise its foul exports
    // stand for them.
    std::unordered_map<std::string, std::unordered_set<std::string>> capabilityMembers;
    // The source modules of this build's own units lowered apart:
    // `using` one makes its functions callable by UFCS and as tags, as a
    // module merged into the program is.
    std::unordered_set<std::string> compiledApartModules;
    // Receiver functions are separate from ordinary module exports and are
    // populated only from package-declared provider modules.
    std::unordered_map<std::string, std::vector<ReceiverFunction>> receiverFunctions;
    // External serving type -> module that owns its slots, and their names.
    std::unordered_map<std::string, std::pair<std::string, std::vector<std::string>>>
        servingSlots;
    // Trait name -> required method names in declaration order. Dictionaries
    // use this stable order for their hidden function slots.
    std::unordered_map<std::string, std::vector<TraitMethod>> traitMethods;
};

// A variant tag declared outside the module being compiled (the prelude, or
// a loaded unit). Only display registration needs these: without them a
// compiled program printed prelude ADTs as raw tuples/atoms.
struct ExternalVariantTag {
    std::string tag;
    int arity = 0;
    std::string owner;
    // The module holding the type's `make` functions, when it is a unit of
    // this build compiled apart; empty leaves it to this module, which
    // forwards what it lacks to the prelude.
    std::string methodOwner;
};

struct ExternalRecordLayout {
    std::string name;
    std::vector<std::string> fields;
    // BEAM module that owns the record's field accessors.
    std::string moduleAtom;
    // As ExternalVariantTag::methodOwner.
    std::string methodOwner;
    // Each field's default as source text, "" for none, and the module the
    // record is declared in, which a default is read in: a record of a unit
    // compiled apart has no AST here to take them from.
    std::vector<std::string> fieldDefaults;
    std::string declaringModule;
};

// Everything lowering is told besides the program itself. One struct, with
// named fields, rather than a run of positional pointers: the entry points
// used to take the same nine in two different orders.
struct LowerContext {
    // The file being lowered, for locations in runtime errors.
    std::string sourcePath;
    // What other modules (the prelude, dependencies compiled apart) declare.
    const ExternalModules* externals = nullptr;
    const std::vector<ExternalRecordLayout>* externalRecords = nullptr;
    const std::vector<ExternalVariantTag>* externalVariants = nullptr;
    // The prelude itself is being lowered (see Lowering::lowersPrelude).
    bool lowersPrelude = false;
    // The checked program: what each call is and what type each expression
    // has (src/hir/), and every other answer of its check. Required.
    const hir::Program* hir = nullptr;
};

// `base` with every entry of `overrides` laid over it: the prelude's
// externals beside those of the units a build links.
auto mergeExternalModules(const ExternalModules& base, const ExternalModules& overrides)
    -> ExternalModules;

auto lowerProgram(const ast::Program& prog, const std::string& fileStem,
                  const LowerContext& context = {}) -> Module;

// Lower a compilation unit using the module-system BEAM mapping. The first
// result is the file-local Kex.Global module; every explicit Kex module is a
// separate `Kex.<Name>` module.
auto lowerModules(const ast::Program& prog, const std::string& fileStem,
                  const LowerContext& context = {}) -> std::vector<Module>;

// Lower the prelude with per-tier awareness. The full AST is used for the
// pre-pass (records, types, method owners); items are logically partitioned
// into tiers by tierBounds. Currently produces one merged module; future
// versions will produce separate modules per tier.
// The prelude's tiers: lowered as one module, with all of `context`,
// the checked program (`hir`) included.
auto lowerProgramTiered(const ast::Program& prog,
                        const std::array<size_t, 5>& tierBounds,
                        const std::string& fileStem,
                        const LowerContext& context = {}) -> Module;

} // namespace kex::ir
