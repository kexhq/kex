#pragma once
// Collects KexI metadata from the analyzed AST. Runs after semantic
// checking, before or after IR lowering — it reads the AST, not the
// emitted Core Erlang text.
#include "kexi.hxx"
#include "../ast/ast.hxx"
#include "../semantic/analyzer.hxx"
#include <functional>
#include <set>
#include <string>

namespace kex::beam {

struct CollectOptions {
    std::string unitId;
    std::string moduleAtom;
    std::string fileStem;
    bool noCheck = false;
    KexiModuleRole role = KexiModuleRole::Entry;
    std::string entryBackPointer;
    // Public source-module identity stored in KexI. Unless collectTopLevel is
    // set, a non-empty value also selects that ModuleDef from the source AST.
    std::string moduleName;
    // Label a top-level entry without treating moduleName as an AST selector.
    bool collectTopLevel = false;
    // Used when several source modules are intentionally emitted as one BEAM
    // module, such as the current prelude bootstrap artifact.
    bool flattenModules = false;
    // With `moduleName`, for a unit compiled apart from its importers, which
    // has to describe itself: also the top-level `make` blocks for the
    // module's own types (`make Type.Field` after `module Type`), and a
    // capability's own members (`capabilityMembers`).
    bool unitCompiledApart = false;
    const kex::semantic::Analyzer* analysis = nullptr;
};

auto collectMetadata(const kex::ast::Program& program,
                     const CollectOptions& opts) -> KexiChunk;

// Every bare type name in `chunk` — in its signatures, constants, types,
// methods and record fields — replaced by what `qualify` makes of it. A
// signature written `deps : [Dependency]` names the module's own
// `Dependency` or the one a `using` brought in, but read on its own, by
// another unit, a bare name says neither (docs/plan_separate_compilation.md).
// `method`'s per-type implementation among `emitted`: `name/Head` (the head
// its `make` target is written as, qualified or as its module wrote it) at
// the method's arity, or "".
auto emittedImplementationOf(
    const KexiMethod& method,
    const std::function<bool(const std::string& function, int arity)>& emitted)
    -> std::string;
// Records each receiver method's implementation a value of its type reaches
// through the dispatcher (`routed`: the emitted module's
// routedImplementations). One source of truth, the emitted module, for every
// consumer that devirtualizes.
auto recordImplementations(
    KexiChunk& chunk,
    const std::function<bool(const std::string& function, int arity)>& routed)
    -> void;
// The functions a Core Erlang module defines, as `emitted` above asks; it
// reads `coreSource`, which must outlive it.
auto coreExports(const std::string& coreSource)
    -> std::function<bool(const std::string& function, int arity)>;

auto qualifyTypeNames(KexiChunk& chunk,
                      const std::function<std::string(const std::string&)>& qualify)
    -> void;
// Qualifies the bare type names in `chunk`'s signatures as module `modulePath`
// of `program` reads them: its own types, then those of the modules it uses
// (`Match` in `Regex` is `Regex.Match`). `knownTypes` are the qualified types
// declared outside `program`; its own modules' types are added.
auto qualifyModuleTypeNames(KexiChunk& chunk, const ast::Program& program,
                            const std::string& modulePath,
                            std::set<std::string> knownTypes) -> void;

} // namespace kex::beam
