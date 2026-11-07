#pragma once
// IR → Core Erlang text. Because the IR is already normalized (UFCS resolved,
// operators lowered, ANF with atomic call args, calls carrying their target
// module/arity), this step is mechanical — no desugaring decisions, no
// variable threading. The result drives the downstream erlc/erl path.
#include "ir.hxx"
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace kex::ir {

struct EmitResult {
    std::string source;
    std::string moduleName;
    int mainArity = 0;
    // Each per-type implementation (`count/List`, its arity) a value of that
    // type reaches through its dispatcher, as emitted: the module's record of
    // where a statically typed receiver may call directly.
    std::set<std::pair<std::string, int>> routedImplementations;
};

auto emitCore(const Module& mod) -> EmitResult;

// Runs the standard IR passes over each lowered module (pass_manager.hxx)
// and emits its Core Erlang, in order.
auto emitModules(std::vector<Module>& modules) -> std::vector<EmitResult>;

} // namespace kex::ir
