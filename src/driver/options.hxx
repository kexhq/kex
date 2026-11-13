#pragma once

// What the command line asked for (main.cxx's parseCommandLine fills it), as
// the compile pipeline and the walker commands read it.

#include "../interpreter/evaluator.hxx"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// What the CLI was asked to do. One enumerator per mode, chosen by the
// flags below and defaulted after option parsing; `compileRun` refines
// `Compile` (emit files vs. compile-and-run on the BEAM).
enum class Mode {
  Unset,
  Lex,
  Parse,
  Check,
  Compile,
  Run,
  Repl,
  BeamRepl,
  EmitCore,
  EmitAst,
  EmitSyntax,
  Expand,
  Complete,
  Lsp,
};

// The command line, read once by parseCommandLine and handed to the mode
// that runs. `exitNow` is set when an option has already finished the run
// (`--help`, `--version`, `--build-prelude`, a bad flag) with that status.
struct Options {
  // Unset until a flag picks a mode; whatever is still unset after option
  // parsing gets the default, which is BEAM (Compile + compileRun). The
  // tree-walk interpreter is opt-in: -R for a script, -i for a REPL.
  Mode mode = Mode::Unset;
  bool skipCheck = false;
  bool dumpTypes = false;
  bool jsonOutput = false;
  bool summaryMode = false;
  // `--skill [topic]`: print the guide to writing Kex, or one part of it.
  bool skillMode = false;
  // `--dump-hir` / `--verify-hir`: list every call site with what the
  // checker decided about it / fail when any is undecided (src/hir/).
  bool dumpHir = false;
  bool verifyHir = false;
  // `--build-prelude DIR`: run after every option is read, so `--verify-hir`
  // applies to the prelude wherever it stands.
  std::optional<std::string> buildPreludeDir;
  // `--print-passes`: list the compile pipeline and the IR passes, then exit.
  bool printPasses = false;
  // `--print-pipeline-passes`: the pipeline a build would run, as text.
  bool printPipelinePasses = false;
  std::string completePrefix;
  std::string outputDir = ".";
  std::string astFilename;
  bool outputDirExplicit = false;
  // `--eval` was given: its lines are queued in g_replScript.
  bool replEval = false;
  bool compileRun = false;
  bool skipPrelude = false;
  bool collapseReport = false;
  bool allowMocks = false;
  kex::interpreter::Evaluator::TestReportMode testReportMode =
      kex::interpreter::Evaluator::TestReportMode::Pretty;
  std::vector<std::string> testFilters;
  std::optional<int> exitNow;
};

// Expands an entry program's `compiled do` code as main sets it up; `true`
// asks for the collapse report.
using ExpandProgram = std::function<bool(kex::ast::Program &, bool)>;
