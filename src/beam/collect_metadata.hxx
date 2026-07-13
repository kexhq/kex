#pragma once
// Collects KexI metadata from the analyzed AST. Runs after semantic
// checking, before or after IR lowering — it reads the AST, not the
// emitted Core Erlang text.
#include "kexi.hxx"
#include "../ast/ast.hxx"
#include "../semantic/analyzer.hxx"
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
    const kex::semantic::Analyzer* analysis = nullptr;
};

auto collectMetadata(const kex::ast::Program& program,
                     const CollectOptions& opts) -> KexiChunk;

} // namespace kex::beam
