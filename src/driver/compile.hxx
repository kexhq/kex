#pragma once

// `kex -c`, `--emit-core` and `-c --run` (driver/compile.cxx): the driver
// around the build's program pipeline (build_ir.hxx).

#include "../ast/ast.hxx"
#include "options.hxx"

#include <optional>
#include <ostream>
#include <string>
#include <vector>

// Compiles `program` (the entry at `filepath`) as `options` ask, and runs it
// for `-c --run`; the process exit status.
auto compileToBeam(Options &options, const std::string &filepath, kex::ast::Program &program,
                   const std::vector<std::string> &scriptArgs,
                   const std::string &entryRawSourceForCache, const std::string &buildFlags)
    -> int;
// `--print-passes`: every pass of every level, and the default pipeline.
auto printCompilePipeline(std::ostream &out) -> void;
// What is wrong with the pipeline the user wrote (`--passes`), if anything.
auto checkPassPipeline() -> std::optional<std::string>;
// `--print-pipeline-passes`: the program pipeline a build runs, as text.
auto buildPipelineText() -> std::string;
// Runs a compiled entry's `.beam` modules on a fresh VM, `beamArgs` its
// arguments; the program's exit status.
auto runCompiledBeam(const std::string &filepath, const Options &options,
                     const std::vector<std::string> &beamArgs) -> int;
