#pragma once

// The compile pipeline (driver/compile.cxx): `kex -c`, `--emit-core` and
// `-c --run` as a list of named passes over the entry program.

#include "../ast/ast.hxx"
#include "options.hxx"

#include <ostream>
#include <string>
#include <vector>

// Compiles `program` (the entry at `filepath`) as `options` ask, and runs it
// for `-c --run`; the process exit status.
auto compileToBeam(Options &options, const std::string &filepath, kex::ast::Program &program,
                   const std::vector<std::string> &scriptArgs,
                   const std::string &entryRawSourceForCache, const std::string &buildFlags)
    -> int;
// `--print-passes`: the compile pipeline's passes, in order.
auto printCompilePipeline(std::ostream &out) -> void;
// Runs a compiled entry's `.beam` modules on a fresh VM, `beamArgs` its
// arguments; the program's exit status.
auto runCompiledBeam(const std::string &filepath, const Options &options,
                     const std::vector<std::string> &beamArgs) -> int;
