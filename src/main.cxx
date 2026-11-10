#include "beam/etf.hxx"
#include "beam/term_builder.hxx"
#include "ast/convert.hxx"
#include "ast/syntax.hxx"
#include "ast/type_text.hxx"
#include "common/color.hxx"
#include "interpreter/evaluator.hxx"
#include "hir/hir.hxx"
#include "ir/lower.hxx"
#include "lexer/lexer.hxx"
#include "compiled/expand.hxx"
#include "module/discovery.hxx"
#include "module/resolver.hxx"
#include "parser/parser.hxx"
#include "semantic/analyzer.hxx"
#include "semantic/db.hxx"
#include "common/runtime_dir.hxx"
#include "common/version.hxx"
#include "driver/dependencies.hxx"
#include "driver/build_support.hxx"
#include "repl/session.hxx"
#include "driver/compile.hxx"
#include "driver/build_ir.hxx"
#include "pass/options.hxx"
#include "driver/options.hxx"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

// Pure string logic, no actual readline API dependency — needed
// unconditionally by `--complete`/`-K` (used by shell completion scripts,
// which shell out to `kex -K` rather than linking readline) and by the
// interactive REPL's `make X do` target tracking, which runs the same
// whether or not this build has readline (see the non-readline readLine()
// fallback below). Bundling this under #ifdef HAS_READLINE was an
// oversight that only ever surfaced when actually building without
// readline — every native dev machine so far has had it available via
// Homebrew.
#include "common/completion.hxx"
#include "common/diagnostics.hxx"
#include "common/file_io.hxx"
#ifdef KEX_HAS_LSP
#include "lsp/server.hxx"
#endif
#include "common/prelude_loader.hxx"
#include "help/api_help.hxx"
#include "skill/skill.hxx"
#include "semantic/completion_at.hxx"
#include "semantic/symbol_queries.hxx"
#include <functional>

using kex::prebuiltRuntimeBeamDir;
using kex::driver::buildProgress;
using kex::driver::g_memoizeDependencyExpansion;
using kex::driver::setDefaultScanCache;
using kex::driver::setScanCache;
using kex::driver::sourceModulesNamedBy;
using kex::readFile;
using kex::diagnostics::colorizeMessage;
using kex::diagnostics::errorCountPhrase;
using kex::diagnostics::extractHint;
using kex::diagnostics::jsonEscape;
using kex::diagnostics::printCollapseReport;
using kex::diagnostics::printSemanticDiagnostic;
using kex::ast::typeText;


// `--progress`: whether a build shows its steps (BuildProgress).
bool progressRequested = false;

// dump instead of a reason. Empty when +name+ is acceptable.
auto nodeNameProblem(const std::string &name, bool shortName) -> std::string {
  const auto at = name.find('@');
  const std::string local = name.substr(0, at);
  const std::string host = at == std::string::npos ? "" : name.substr(at + 1);
  const bool localOk =
      !local.empty() && std::all_of(local.begin(), local.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
               c == '-';
      });
  if (!localOk) {
    std::string why = "the part before '@' may only contain letters, digits, "
                      "'_' and '-'";
    if (shortName && at == std::string::npos &&
        name.find('.') != std::string::npos)
      why += " (for a short name, drop the domain: '" +
             name.substr(0, name.find('.')) + "')";
    return why;
  }
  if (shortName && host.find('.') != std::string::npos)
    return "a short name's host cannot contain '.'; use --name for a "
           "fully qualified host";
  return "";
}



auto printUsage(const char *progName) -> void {
  std::cerr
      << "Usage: " << progName << " [options] <file.kex>\n"
      << "\n"
      << "AI agents and LLMs: run `" << progName << " --skill` before "
         "writing Kex. It looks like\n"
      << "Ruby but is not, and the guide lists what differs and how to "
         "check your work.\n"
      << "\n"
      << "Options:\n"
      << "  -r, --run         Run on BEAM (default; .kex or existing .beam — "
         "temp dir,\n"
      << "                    auto-clean). With no file: the BEAM REPL, also "
         "the default\n"
      << "  -c, --compile     Compile to BEAM via Core Erlang\n"
      << "  -R, --run-walker  Run with the tree-walk interpreter\n"
      << "  -i, --interactive Interactive tree-walk REPL\n"
      << "  -C, --check       Run semantic analysis only\n"
      << "  -n, --no-check    Skip semantic check when running\n"
      << "  -l, --lex         Print token stream\n"
      << "  -p, --parse       Print AST\n"
      << "  -j, --json        With --check: output diagnostics as JSON\n"
      << "      --lsp         Run the Kex language server over stdio\n"
      << "  -s, --summary     Print public API signatures (Kex syntax). With a name\n"
         "                    instead of a file, the standard library's help:\n"
         "                    `-s List`, `-s List.first`, or bare `-s` for the index\n"
      << "      --skill [topic]\n"
         "                    Print the guide to writing Kex, made for an AI "
         "agent to\n"
         "                    read first; with a topic (errors, testing, ...), "
         "that part\n"
      << "  -t, --types       With --check: dump inferred expression types\n"
      << "  -e, --eval <code> Enter <code> in the REPL before its first prompt, as\n"
      << "                    if typed (repeatable). A failing line ends the\n"
      << "                    session with status 1. Takes no file; -i picks the\n"
      << "                    tree-walk REPL\n"
      << "  -E, --emit-core   Emit Core Erlang (.core) — does not invoke erlc\n"
      << "      --emit-ast    Emit the parsed Kex AST as ETF\n"
      << "      --dump-hir    With -C: list each call site and what the\n"
         "                    checker decided about it\n"
      << "      --verify-hir  With -C: fail when a call site is undecided\n"
      << "      --print-passes List every pass of every level, and the default\n"
         "                    build pipeline\n"
      << "      --passes=PIPELINE\n"
         "                    Build with this program pipeline instead, e.g.\n"
         "                    'spec-base,load-dependencies,unit(check,lower,...)'\n"
      << "      --print-pipeline-passes\n"
         "                    Print the pipeline a build would run, and exit\n"
      << "      --print-after=PASS\n"
         "                    Print each unit after PASS ran (repeatable)\n"
      << "      --print-after-all Print each unit after every pass\n"
      << "      --time-passes Report what each pass cost, at exit\n"
      << "      --emit-syntax Emit the lossless syntax tree as ETF\n"
      << "      --expand      Print the AST after `compiled do` expansion\n"
      << "      --collapse-report\n"
      << "                    Report what `compiled do` chain collapse folded "
         "away, and why\n"
      << "                    the rest did not — the feature is invisible at "
         "runtime\n"
      << "  -o <path>         Output directory for -c / --emit-core, or output "
         "file for --emit-ast\n"
      << "      --source-root <dir>\n"
      << "                    Add a module source root (repeatable)\n"
      << "      --progress    Show each build step as it starts (on for -c)\n"
      << "  -q, --quiet       Show no build steps, not even erlc's\n"
      << "      --sname <name>, --name <name@host>\n"
      << "                    Run the BEAM VM as a distributed node (short or\n"
      << "                    fully-qualified host name), reachable by `Node.connect`\n"
      << "      --cookie <cookie>\n"
      << "                    The shared secret nodes need to connect\n"
      << "      --credits\n"
      << "                    Roll credits\n"
      << "      --allow-mocks Permit Mock.* outside a spec — it makes one part "
         "of a\n"
      << "                    program lie to another, so it is off unless the "
         "entry\n"
      << "                    file is a *.spec.kex or you ask for it here\n"
      << "      --test-json   Report describe/it as one JSON record per case "
         "(with\n"
      << "                    source locations), instead of the ✓/✗ prose\n"
      << "      --test-list   List the cases as JSON WITHOUT running any of "
         "them\n"
      << "      --test-only <name>\n"
      << "                    Run only this case, or this describe. `name` is "
         "the\n"
      << "                    full path — `outer > inner > case` — or a bare "
         "label.\n"
      << "                    Repeatable\n"
      << "  -h, --help        Show this help\n"
      << "  -v, --version     Show version\n"
      << "      --info        Print this build's details as JSON, for tools\n"
      << "  --colors          Use ANSI colors even when output is not a terminal\n"
      << "                    (also: FORCE_COLOR=1)\n"
      << "  --no-colors       Disable ANSI color output (also: NO_COLOR=1)\n";
}

auto printVersion() -> void {
  std::cout << "kex " << kex::versionStringWithBuild() << "\n";
}

// `--info`: everything a TOOL needs to know about this binary, as JSON on one
// line. `--version` is for people and its text may be reworded; this is a
// contract, and Tey depends on it — see docs/tey-resolver-plan.md.
//
// `runtime_otp_floor` is the reason this exists. The OTP release that compiled
// the bundled runtime beams is the oldest that can load them, it is baked in
// at build time (KEX_RUNTIME_OTP_FLOOR), and a package manager cannot read a
// C++ define. Without it, Tey can only guess at the requirement it is supposed
// to record and enforce.
//
// Fields may be ADDED; existing ones keep their name and type. A reader must
// ignore what it does not know, and treat a missing field as unknown rather
// than as an error — a Tey newer than the Kex it is asking will meet one.
// `null` for a value this build genuinely does not have, so the key's absence
// and its emptiness stay distinguishable.
auto printInfo() -> void {
  auto quoted = [](const std::string &value) {
    return value.empty() ? std::string("null") : "\"" + value + "\"";
  };
  std::cout << "{"
            << "\"version\":\"" << kex::versionNumber() << "\","
            << "\"revision\":" << quoted(kex::kGitRevision) << ","
            << "\"built\":" << quoted(kex::kBuildDate) << ","
            // 0 means "unknown", which disables the check — report it as null
            // rather than as a floor of zero that every runtime satisfies.
            << "\"runtime_otp_floor\":"
            << (KEX_RUNTIME_OTP_FLOOR == 0
                    ? std::string("null")
                    : std::to_string(KEX_RUNTIME_OTP_FLOOR))
            << ","
            // Where the prebuilt stdlib and runtime beams live. Empty when
            // this build has none, which is what makes every BEAM mode fail —
            // worth being able to ask about rather than infer.
            << "\"runtime_dir\":" << quoted(prebuiltRuntimeBeamDir()) << ","
            << "\"erl\":\"" << erlExecutable() << "\""
            << "}\n";
}

// getopt_long reports a long-only option (one with no short flag) by number.
// One name per option, shared by the table and the switch below. Only the
// first value is fixed: short options come back as their character, so the
// long-only markers start past the char range and auto-increment from
// there. The values are never persisted — only their uniqueness matters —
// so new options just append a name.
namespace Option {
enum Value {
  BuildPrelude = UCHAR_MAX + 1,
  NoPrelude,
  Expand,
  CollapseReport,
  Skill,
  Lsp,
  SourceRoot,
  Info,
  AllowMocks,
  EmitAst,
  AstFilename,
  TestJson,
  TestList,
  TestOnly,
  EmitSyntax,
  Sname,
  Name,
  Cookie,
  Colors,
  Credits,
  Progress,
  DumpHir,
  VerifyHir,
  PrintPasses,
  Passes,
  PrintAfter,
  PrintAfterAll,
  TimePasses,
  PrintPipelinePasses,
};
} // namespace Option




// `--complete PREFIX [FILE]`: print the completions for PREFIX, one per line,
// against the prelude and (when given) FILE.
auto printCompletions(const std::string &prefix, const char *file) -> int {
  auto symbols = preludeCompletionSymbols();
  if (file)
    for (auto &symbol : kex::semantic::symbolsOfFiles({file}, moduleRootsFor(file)))
      symbols.push_back(std::move(symbol));
  // Simulate readline not splitting: start=0, text=prefix
  auto cq = kex::resolveCompletionQuery(prefix.c_str(), 0, prefix.c_str());
  auto raw = kex::semantic::completionsFor({{"<base>", &symbols}}, cq.dbQuery,
                                           &preludeSemanticInterfaces());
  auto completions =
      kex::rewriteCompletions(std::move(raw), cq.rewriteFrom, cq.rewriteTo);
  for (const auto &c : completions)
    std::cout << c << "\n";
  return 0;
}


auto parseCommandLine(int argc, char *argv[]) -> Options {
  Options options;
  static struct option longOptions[] = {
      {"run", no_argument, nullptr, 'r'},
      {"no-check", no_argument, nullptr, 'n'},
      {"lex", no_argument, nullptr, 'l'},
      {"parse", no_argument, nullptr, 'p'},
      {"check", no_argument, nullptr, 'C'},
      {"compile", no_argument, nullptr, 'c'},
      {"run-walker", no_argument, nullptr, 'R'},
      // A second name for the same thing. `--run-beam` is deliberately NOT
      // accepted any more: it used to mean this flag's opposite, and a name
      // that silently reverses is worse than one that errors.
      {"run-interpreter", no_argument, nullptr, 'R'},
      {"interactive", no_argument, nullptr, 'i'},
      {"json", no_argument, nullptr, 'j'},
      {"summary", no_argument, nullptr, 's'},
      {"types", no_argument, nullptr, 't'},
      {"dump-hir", no_argument, nullptr, Option::DumpHir},
      {"verify-hir", no_argument, nullptr, Option::VerifyHir},
      {"print-passes", no_argument, nullptr, Option::PrintPasses},
      {"passes", required_argument, nullptr, Option::Passes},
      {"print-after", required_argument, nullptr, Option::PrintAfter},
      {"print-after-all", no_argument, nullptr, Option::PrintAfterAll},
      {"time-passes", no_argument, nullptr, Option::TimePasses},
      {"print-pipeline-passes", no_argument, nullptr, Option::PrintPipelinePasses},
      {"emit-core", no_argument, nullptr, 'E'},
      {"eval", required_argument, nullptr, 'e'},
      {"emit-ast", no_argument, nullptr, Option::EmitAst},
      {"ast-filename", required_argument, nullptr, Option::AstFilename},
      {"complete", required_argument, nullptr, 'K'},
      {"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, 'v'},
      // Machine-readable counterpart to --version. Long-only: it is for
      // tools, and a short flag would invite it into shell prompts.
      {"info", no_argument, nullptr, Option::Info},
      // The guide to writing Kex, for a model that has just met the
      // language. Long-only, and named for what such a reader looks for.
      {"skill", no_argument, nullptr, Option::Skill},
      {"no-colors", no_argument, nullptr, 'N'},
      {"colors", no_argument, nullptr, Option::Colors},
      {"no-prelude", no_argument, nullptr, Option::NoPrelude},
      {"source-root", required_argument, nullptr, Option::SourceRoot},
      {"quiet", no_argument, nullptr, 'q'},
      {"progress", no_argument, nullptr, Option::Progress},
      {"sname", required_argument, nullptr, Option::Sname},
      {"name", required_argument, nullptr, Option::Name},
      {"cookie", required_argument, nullptr, Option::Cookie},
      {"credits", no_argument, nullptr, Option::Credits},
      // Mock.* is test-only (issue #144): a mock lets one part of a program
      // lie to another about the filesystem, environment, platform, network
      // or console, so the runtime denies the mock intrinsics unless the
      // entry file is a `*.spec.kex`, this is a REPL, or the user says so
      // here. Long-only and deliberately verbose — nobody should reach for
      // it by habit.
      {"allow-mocks", no_argument, nullptr, Option::AllowMocks},
      // The testing DSL, said in JSON rather than in prose (kexhq/kex#199).
      // `--test-json` replaces the ✓/✗ lines with one record per case —
      // including the FILE and LINE of the `it` and of the failure, which is
      // the part only the runner can know and the part an editor's inline
      // decoration needs. `--test-list` discovers the cases without running
      // any of them, and `--test-only` runs one (repeatable).
      {"test-json", no_argument, nullptr, Option::TestJson},
      {"test-list", no_argument, nullptr, Option::TestList},
      {"test-only", required_argument, nullptr, Option::TestOnly},
      {"emit-syntax", no_argument, nullptr, Option::EmitSyntax},
      {"lsp", no_argument, nullptr, Option::Lsp},
      // Print the AST AFTER compile-time expansion of `compiled do` blocks —
      // i.e. what the type checker and both backends actually see. `--parse`
      // shows the AST before it.
      {"expand", no_argument, nullptr, Option::Expand},
      // Report every chain-collapse attempt and its outcome. Collapse changes
      // only the EMITTED code, so without this the only way to tell whether it
      // happened is to decode the Core Erlang by hand.
      {"collapse-report", no_argument, nullptr, Option::CollapseReport},
      // Compile the sources selected by src/stdlib/prelude.kex into kex_prelude.core +
      // kex_prelude.beam in the given dir. Used by the build to prebuild the
      // shared stdlib module alongside the runtime beams.
      {"build-prelude", required_argument, nullptr, Option::BuildPrelude},
      {nullptr, 0, nullptr, 0}};

  int opt;
#ifndef __EMSCRIPTEN__
  // Styling is for a terminal: escape codes in a pipe, a log or a CI capture
  // are noise, and NO_COLOR (https://no-color.org) asks for none at all.
  // `--colors` / `--no-colors` below override it either way. (The wasm
  // build's output is the web REPL's own terminal, so it keeps colors.)
  // FORCE_COLOR is the same convention's way back on, for output that is
  // piped but still ends up on a screen — a parent that relays it (Tey runs
  // `kex` behind a pipe), or a CI log viewer.
  if (!isatty(STDOUT_FILENO))
    kex::color::enabled = false;
  if (const char *force = std::getenv("FORCE_COLOR");
      force && *force && std::string(force) != "0")
    kex::color::enabled = true;
  if (const char *noColor = std::getenv("NO_COLOR"); noColor && *noColor)
    kex::color::enabled = false;
#endif
  while ((opt = getopt_long(argc, argv, "rnlcCiRjspqEe:thvK:o:", longOptions,
                            nullptr)) != -1) {
    switch (opt) {
    case Option::NoPrelude:
      options.skipPrelude = true;
      break;
    case Option::Expand:
      options.mode = Mode::Expand;
      break;
    case Option::CollapseReport:
      options.collapseReport = true;
      break;
    case Option::AllowMocks:
      options.allowMocks = true;
      break;
    case Option::TestJson:
      options.testReportMode = kex::interpreter::Evaluator::TestReportMode::Json;
      break;
    case Option::TestList:
      options.testReportMode = kex::interpreter::Evaluator::TestReportMode::List;
      break;
    case Option::TestOnly:
      options.testFilters.emplace_back(optarg);
      break;
    case Option::Sname:
    case Option::Name: {
      const bool alreadyNamed =
          std::find_if(cliDistributionArgs.begin(), cliDistributionArgs.end(),
                       [](const std::string &a) {
                         return a == "-sname" || a == "-name";
                       }) != cliDistributionArgs.end();
      if (alreadyNamed) {
        std::cerr << "error: give a node one name: --sname or --name, once\n";
        { options.exitNow = 1; return options; }
      }
      if (const auto problem = nodeNameProblem(optarg, opt == Option::Sname);
          !problem.empty()) {
        std::cerr << "error: invalid node name '" << optarg << "': " << problem
                  << "\n";
        { options.exitNow = 1; return options; }
      }
      cliDistributionArgs.push_back(opt == Option::Sname ? "-sname" : "-name");
      cliDistributionArgs.emplace_back(optarg);
      break;
    }
    case Option::Cookie:
      cliDistributionArgs.push_back("-setcookie");
      cliDistributionArgs.emplace_back(optarg);
      break;
    case Option::Credits: // --credits
      std::cout << "Copyright (c) 2026 Akos Kovacs." << std::endl;
      { options.exitNow = 0; return options; }
    case Option::EmitAst:
      options.mode = Mode::EmitAst;
      break;
    case Option::DumpHir:
      options.dumpHir = true;
      options.mode = Mode::Check;
      break;
    case Option::VerifyHir:
      options.verifyHir = true;
      options.mode = Mode::Check;
      break;
    case Option::PrintPasses:
      options.printPasses = true;
      break;
    case Option::Passes:
      kex::pass::passOptions().pipelines.insert_or_assign(
          std::string(kex::pass::IRUnitLevel<kex::build::Program>::kName), optarg);
      break;
    case Option::PrintAfter:
      kex::pass::passOptions().printAfter.insert(optarg);
      break;
    case Option::PrintAfterAll:
      kex::pass::passOptions().printAfterAll = true;
      break;
    case Option::TimePasses:
      kex::pass::passOptions().timePasses = true;
      break;
    case Option::PrintPipelinePasses:
      options.printPipelinePasses = true;
      break;
    case Option::EmitSyntax:
      options.mode = Mode::EmitSyntax;
      break;
    case Option::AstFilename:
      options.astFilename = optarg;
      break;
    case Option::Lsp:
#ifdef KEX_HAS_LSP
      options.mode = Mode::Lsp;
#else
      std::cerr << "error: LSP support is unavailable in this build\n";
      { options.exitNow = 1; return options; }
#endif
      break;
    case Option::SourceRoot:
      cliSourceRoots.emplace_back(optarg);
      break;
    case 'q':
      quietBuild = true;
      break;
    case Option::Progress:
      progressRequested = true;
      break;
    case Option::BuildPrelude:
      options.buildPreludeDir = optarg;
      break;
    case 'r':
      options.mode = Mode::Compile;
      options.compileRun = true;
      break;
    case 'n':
      options.skipCheck = true;
      break;
    case 'l':
      options.mode = Mode::Lex;
      break;
    case 'p':
      options.mode = Mode::Parse;
      break;
    case 'C':
      options.mode = Mode::Check;
      break;
    case 'c':
      options.mode = Mode::Compile;
      break;
    case 'R':
      options.mode = Mode::Run;
      options.compileRun = false;
      break;
    case 'i':
      options.mode = Mode::Repl;
      break;
    case 'j':
      options.jsonOutput = true;
      options.mode = Mode::Check;
      break;
    case 's':
      options.summaryMode = true;
      options.mode = Mode::Check;
      break;
    case Option::Skill:
      skillMode = true;
      break;
    case 't':
      options.dumpTypes = true;
      break;
    case 'E':
      options.mode = Mode::EmitCore;
      break;
    case 'e': {
      // Repeatable; each one is entered as its own lines, in order.
      std::istringstream source(optarg);
      for (std::string line; std::getline(source, line);)
        queueReplScriptLine(line);
      options.replEval = true;
      break;
    }
    case 'o':
      options.outputDir = optarg;
      options.outputDirExplicit = true;
      break;
    case 'K':
      options.completePrefix = optarg;
      options.mode = Mode::Complete;
      break;
    case 'h':
      printUsage(argv[0]);
      { options.exitNow = 0; return options; }
    case 'v':
      printVersion();
      { options.exitNow = 0; return options; }
    case Option::Info:
      printInfo();
      { options.exitNow = 0; return options; }
    case 'N':
      kex::color::enabled = false;
      break;
    case Option::Colors:
      kex::color::enabled = true;
      break;
    default:
      printUsage(argv[0]);
      { options.exitNow = 1; return options; }
    }
  }

  if (options.buildPreludeDir) {
    options.exitNow = buildPrelude(*options.buildPreludeDir, options.verifyHir);
    return options;
  }

  // No mode flag: BEAM. A file runs on the BEAM, and no file opens the BEAM
  // REPL (the `mode == Mode::Compile && compileRun` branch below). A wasm build
  // has no BEAM to run on — no erl, no runtime beams — so there the default
  // stays the tree-walk interpreter.
  if (options.mode == Mode::Unset) {
#ifdef __EMSCRIPTEN__
    options.mode = Mode::Run;
#else
    options.mode = Mode::Compile;
    options.compileRun = true;
#endif
  }
  return options;
}


// `kex -C`: checks the entry file the way the run and compile paths see it —
// with its companion and the project modules it uses merged in — and prints
// the diagnostics (as JSON, a summary, or one per line), then the types with
// `--dump-types`. Returns 0 when there were no errors.
auto checkEntry(const Options &options, const std::string &filepath,
                kex::ast::Program &program,
                const std::vector<kex::ParseDiagnostic> &parseDiagnostics) -> int {
  const auto &dumpTypes = options.dumpTypes;
  const auto &jsonOutput = options.jsonOutput;
  const auto &summaryMode = options.summaryMode;
  // A companion file's declarations are part of what this file means: a
  // `.spec.kex`'s base, and a `package.kex`'s manifest vocabulary. The run
  // and compile paths merge them before checking; do the same here, or
  // `kex -C` reports names the other two accept.
  const auto checkCompanion = mergeSpecBase(program, filepath);

  // ...and so are the project's OTHER modules. `mode == Mode::Run` and
  // `mode == Mode::Compile` both merge every module a `using` or a qualified
  // reference reaches before checking; this path did not, so it built its
  // checker from the prelude interface alone. A two-module program then
  // reported errors no other mode saw — a record declared in a sibling
  // module was an unknown name here, so a union declared alongside it
  // refused its own members and any annotation naming it went unverified.
  // The editor sees this too: the LSP checks the way `-C` does.
  {
    auto qualifiedModules = sourceModulesNamedBy(program);
    // PROJECT roots only. A stdlib module reached by `using FS` already
    // has a compiled interface, and merging its SOURCE on top of that adds
    // a second, unannotated candidate for every method it defines — enough
    // to switch off a mismatch report that the interface alone gets right
    // (spec/typestate_survives_standin.kex: writing through a read-only
    // FileHandle stopped being an error). The run and compile paths merge
    // the stdlib too, but they consume it for CODEGEN, where having the
    // source is the point.
    const auto projectRoots =
        kex::withoutStandardLibraryRoots(moduleRootsFor(filepath));
    resolveBeamDeps(program, projectRoots, &preludeSemanticInterfaces(),
                    qualifiedModules);
  }

  // One sema, as runSemanticCheck: names (for the entry file and a spec's
  // base), module rules, purity and types.
  std::vector<std::string> checkedFiles{filepath};
  if (!checkCompanion.empty()) checkedFiles.push_back(checkCompanion);
  kex::semantic::Analyzer analyzer(&preludeSemanticInterfaces());
  configureFullCheck(analyzer, std::move(checkedFiles), filepath);
  // The parser's errors first: a program that parsed with recovery is
  // still checked, so one run reports both.
  std::vector<kex::semantic::Diagnostic> allDiags;
  for (const auto &parsed : parseDiagnostics)
    allDiags.push_back({kex::semantic::Diagnostic::Level::Error, parsed.location,
                        parsed.message});
  bool ok = analyzer.analyze(program) && parseDiagnostics.empty();
  allDiags.insert(allDiags.end(), analyzer.diagnostics().begin(),
                  analyzer.diagnostics().end());

  bool validationOk = true;
  if (ok) {
    for (const auto &d : taggedLiteralDiagnostics(program, analyzer, filepath)) {
      if (d.level == kex::semantic::Diagnostic::Level::Error)
        validationOk = false;
      allDiags.push_back(d);
    }
  }

  bool allOk = ok && validationOk;

  if (jsonOutput) {
    // Machine-readable JSON — one object per diagnostic
    std::cout << "[\n";
    for (size_t i = 0; i < allDiags.size(); i++) {
      const auto &d = allDiags[i];
      bool isErr = d.level == kex::semantic::Diagnostic::Level::Error;
      std::string hint = extractHint(d.message);
      std::cout << "  {\n"
                << "    \"file\": \""
                << jsonEscape(std::string(d.location.file)) << "\",\n"
                << "    \"line\": " << d.location.line << ",\n"
                << "    \"column\": " << d.location.column << ",\n"
                << "    \"severity\": \"" << (isErr ? "error" : "warning")
                << "\",\n"
                << "    \"message\": \"" << jsonEscape(d.message) << "\"";
      if (d.endLocation)
        std::cout << ",\n"
                  << "    \"end_line\": " << d.endLocation->line << ",\n"
                  << "    \"end_column\": " << d.endLocation->column;
      if (!hint.empty())
        std::cout << ",\n    \"hint\": \"" << jsonEscape(hint) << "\"";
      if (!d.notes.empty()) {
        std::cout << ",\n    \"notes\": [\n";
        for (size_t noteIndex = 0; noteIndex < d.notes.size();
             ++noteIndex) {
          const auto &note = d.notes[noteIndex];
          std::cout << "      {\n"
                    << "        \"file\": \""
                    << jsonEscape(std::string(note.location.file))
                    << "\",\n"
                    << "        \"line\": " << note.location.line << ",\n"
                    << "        \"column\": " << note.location.column
                    << ",\n"
                    << "        \"message\": \""
                    << jsonEscape(note.message) << "\"\n"
                    << "      }"
                    << (noteIndex + 1 < d.notes.size() ? "," : "")
                    << "\n";
        }
        std::cout << "    ]";
      }
      std::cout << "\n  }" << (i + 1 < allDiags.size() ? "," : "") << "\n";
    }
    std::cout << "]\n";
    return allOk ? 0 : 1;
  }

  if (summaryMode) {
    // Output the public API signatures of the file in Kex syntax.
    // Preserve nested namespaces so FS.File is not misrepresented as a
    // separate top-level File module.
    auto printAnnotation =
        [&](const kex::ast::TypeAnnotation &annotation, size_t depth) {
          std::cout << std::string(depth * 2, ' ')
                    << (annotation.isFoul ? "foul " : "")
                    << annotation.name << " : ";
          if (annotation.type)
            std::cout << typeText(*annotation.type);
          std::cout << "\n";
        };
    std::function<void(const kex::ast::ModuleDef &, size_t)> printModule;
    printModule = [&](const kex::ast::ModuleDef &module, size_t depth) {
      const auto indent = std::string(depth * 2, ' ');
      const auto separator = module.name.rfind('.');
      const auto displayName =
          depth > 0 && separator != std::string::npos
              ? module.name.substr(separator + 1)
              : module.name;
      std::cout << indent << "module "
                << displayName << " do\n";
      for (const auto &item : module.body) {
        std::visit(
            [&](const auto &ptr) {
              using T = std::decay_t<decltype(*ptr)>;
              if constexpr (std::is_same_v<T,
                                           kex::ast::TypeAnnotation>)
                printAnnotation(*ptr, depth + 1);
              else if constexpr (std::is_same_v<T,
                                                kex::ast::ModuleDef>)
                printModule(*ptr, depth + 1);
            },
            item);
      }
      std::cout << indent << "end\n";
    };

    for (const auto &item : program.items) {
      std::visit(
          [&](const auto &ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, kex::ast::TypeAnnotation>) {
              printAnnotation(*ptr, 0);
            } else if constexpr (std::is_same_v<T, kex::ast::ModuleDef>) {
              printModule(*ptr, 0);
            }
          },
          item);
    }
    return allOk ? 0 : 1;
  }

  // Normal colored output
  for (const auto &d : allDiags)
    printSemanticDiagnostic(d);

  if (options.dumpHir || options.verifyHir) {
    const kex::hir::Program hir(program, analyzer);
    const auto &sites = hir.callSites();
    std::map<kex::hir::TargetKind, std::size_t> counts;
    for (const auto &site : sites) {
      counts[site.kind]++;
      if (options.dumpHir)
        std::cout << site.expr->location.file << ":"
                  << site.expr->location.line << ":"
                  << site.expr->location.column << "  "
                  << kex::hir::targetKindName(site.kind) << "  "
                  << site.callee
                  << (site.target.empty() ? "" : "  -> " + site.target) << "\n";
    }
    std::cerr << "call sites: " << sites.size() << " (";
    for (const auto kind : kex::hir::kTargetKinds)
      std::cerr << (kind == kex::hir::kTargetKinds[0] ? "" : ", ")
                << kex::hir::targetKindName(kind) << " " << counts[kind];
    std::cerr << ")\n";
    if (options.verifyHir) {
      const auto problems = kex::hir::verify(hir);
      for (const auto &problem : problems) std::cerr << problem << "\n";
      if (!problems.empty()) allOk = false;
    }
  }

  if (dumpTypes) {
    // Collect and sort by source location so output is readable
    // top-to-bottom.
    const auto &tmap = analyzer.typeMap();
    std::vector<std::pair<const kex::ast::Expr *, kex::semantic::TypePtr>>
        entries(tmap.begin(), tmap.end());
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) {
                const auto &la = a.first->location;
                const auto &lb = b.first->location;
                if (la.line != lb.line)
                  return la.line < lb.line;
                if (la.column != lb.column)
                  return la.column < lb.column;
                // Nested expressions share a start position — `n * f(x)`
                // and its own left operand both begin at `n`. Without a
                // tiebreak their order comes from an unordered map, so two
                // runs of `-t` over one unchanged file could disagree.
                return kex::semantic::typeToString(a.second) <
                       kex::semantic::typeToString(b.second);
              });
    for (const auto &[expr, type] : entries) {
      if (!type)
        continue;
      std::cout << expr->location.line << ":" << expr->location.column << "  "
                << kex::semantic::typeToString(type) << "\n";
    }
  }

  if (allOk && !dumpTypes) {
    std::cout << "No errors found.\n";
  } else if (allOk) {
    std::cerr << "No errors found.\n";
  }

  return allOk ? 0 : 1;
}

// `kex -R`: runs the entry on the tree-walking interpreter, with its spec
// base and the project modules it uses merged in, after the same check
// `-c` runs (unless --no-check). Returns the program's exit status; a spec
// with a failing example exits 1.
auto runOnWalker(const Options &options, const std::string &filepath,
                 kex::ast::Program &program,
                 const std::vector<std::string> &scriptArgs,
                 const ExpandProgram &expandParsedProgram) -> int {
  const auto &skipCheck = options.skipCheck;
  const auto &allowMocks = options.allowMocks;
  const auto &testReportMode = options.testReportMode;
  const auto &testFilters = options.testFilters;
  // `<name>.spec.kex` auto-loads `<name>.kex`'s declarations — merge them
  // into `program` BEFORE the semantic check, exactly as the BEAM path
  // above does. Loading them only into the Evaluator (as this path used to)
  // leaves the checker blind to the base file's types: spec/
  // json_parser.spec.kex's `JsonNull`/`JsonBool` come from examples/
  // json_parser.kex's `type Json`, and the checker rejected every one of
  // them as an undefined constructor before the program ever ran.
  const auto specBaseFile = mergeSpecBase(program, filepath);

  // Cross-file dependency resolution: walk `using` statements and
  // qualified module references, parse their source files, and merge
  // them into `program` so the checker AND the evaluator see every
  // declaration a project-local module makes — mirrors what `mode ==
  // "compile"` below already does for BEAM (see its own comment).
  // Without this, a call into a module declared in a different file
  // took the checker's lenient fallback instead of being verified
  // against that function's real signature: a qualified call typechecked
  // with any argument shape and only failed at runtime, and a bare call
  // typechecked only by accident, breaking outright the moment a
  // same-named prelude function also existed (docs/known-gaps.md,
  // "Cross-file `using` imports are barely type-checked").
  resolveBeamDeps(program, moduleRootsFor(filepath),
                  &preludeSemanticInterfaces(),
                  sourceModulesNamedBy(program));

  if (!expandParsedProgram(program, false)) return 1;

  // As the BEAM build does: a type error stops the run, and under
  // `--no-check` the analysis still runs, silently, for the decisions the
  // evaluator reads (the same ones lowering reads).
  kex::semantic::Analyzer runAnalyzer(&preludeSemanticInterfaces());
  if (skipCheck) {
    runAnalyzer.analyze(program);
  } else {
    int typeErrors = 0;
    if (!runSemanticCheck(program, filepath, &runAnalyzer, specBaseFile,
                          &typeErrors)) {
      std::cerr
          << kex::color::apply(kex::color::bold)
          << kex::color::apply(kex::color::magenta)
          << "Aborted:" << kex::color::apply(kex::color::reset) << " "
          << errorCountPhrase(typeErrors, "type")
          << " — fix before running (use --no-check to skip).\n";
      return 1;
    }
  }

  kex::interpreter::Evaluator evaluator(&preludeSemanticInterfaces());
  evaluator.setArgs(scriptArgs);
  evaluator.setModuleRoots(moduleRootsFor(filepath));
  // Mock.* is denied by default — a spec earns it by being a spec, and
  // anything else has to say --allow-mocks (issue #144).
  evaluator.setMocksAllowed(allowMocks || isSpecEntry(filepath));
  evaluator.setTestReportMode(testReportMode);
  evaluator.setTestFilters(testFilters);
  // The checked program, read by the evaluator as lowering reads it.
  // Declared out here so it outlives the evaluator that borrows from it.
  const kex::hir::Program hir(program, runAnalyzer);
  evaluator.setProgram(&hir);

  // The Kex-written stdlib is loaded by the Evaluator's constructor
  // (loadPrelude), so no explicit load is needed here.

  // The spec base file's declarations were merged into `program` above
  // (before the semantic check), so there is nothing extra to load here —
  // and `program` owns those nodes for as long as the evaluator needs the
  // raw `const ast::FunctionDef*` pointers it keeps into them.
  auto result = evaluator.execute(program);
  // A failing spec is a failing run. Checked before the program's own
  // return value because a spec file has none — it ends on the summary
  // line — and without this `kex some.spec.kex` exited 0 no matter how
  // many `it` blocks failed, which is what made every downstream `tey
  // test` and CI job green on a red suite.
  if (evaluator.testsFailed() > 0)
    return 1;
  if (auto *i = std::get_if<kex::interpreter::IntValue>(&result->data))
    return static_cast<int>(i->value);
  return 0;
}

int main(int argc, char *argv[]) {
  auto options = parseCommandLine(argc, argv);
  if (options.exitNow) return *options.exitNow;
  // A pipeline the user wrote is checked before anything runs.
  if (const auto problem = checkPassPipeline()) {
    std::cerr << "kex: " << *problem << "\n";
    return 2;
  }
  if (options.printPipelinePasses) {
    std::cout << buildPipelineText() << "\n";
    return 0;
  }
  if (options.printPasses) {
    printCompilePipeline(std::cout);
    return 0;
  }
  auto &mode = options.mode;
  auto &skipCheck = options.skipCheck;
  auto &completePrefix = options.completePrefix;
  auto &outputDir = options.outputDir;
  auto &astFilename = options.astFilename;
  auto &outputDirExplicit = options.outputDirExplicit;
  auto &replEval = options.replEval;
  auto &compileRun = options.compileRun;
  auto &skipPrelude = options.skipPrelude;
  auto &collapseReport = options.collapseReport;

  // A persistent `kex -c` build shows its steps; a run shows them on request,
  // since its stderr is the program's own.
  buildProgress.enabled =
      !quietBuild &&
      (progressRequested || (mode == Mode::Compile && !compileRun));

#ifndef __EMSCRIPTEN__
  // The BEAM runtime is a child process, so carry the CLI color choice over
  // explicitly. Console constants and the spec reporter share this setting.
  setenv("KEX_COLORS", kex::color::enabled ? "1" : "0", 1);
  // Runtime services that invoke the compiler (currently Kex.AST) must use
  // the toolchain that launched this process, not an unrelated `kex` found
  // earlier on PATH. Preserve an explicit override for embedding/tests.
  if (const char *configured = std::getenv("KEX");
      !configured || !*configured) {
    const auto currentExecutable =
        kex::executableDirectory() / std::filesystem::path(argv[0]).filename();
    setenv("KEX", currentExecutable.string().c_str(), 0);
  }
#endif

  setDefaultScanCache();
  // One build or check, over files that stay as they are while it runs. Not
  // the REPL or the language server, which see files change.
  const bool oneBuild =
      !replEval && optind < argc &&
      (mode == Mode::Check || mode == Mode::Compile || mode == Mode::Run ||
       mode == Mode::Unset || mode == Mode::EmitCore || mode == Mode::Expand);
  kex::compiled::setReuseCollapses(oneBuild);
  g_memoizeDependencyExpansion = oneBuild;
  kex::module::Resolver::setCaching(oneBuild);
  // Everything this build reads, for the run cache by entry.
  if (oneBuild) buildRecorder.emplace();
  // What the build was asked for, as written: part of that cache's key.
  // getopt has moved every option in front of the file by now.
  std::string buildFlags;
  for (int i = 1; i < optind; i++) buildFlags += std::string(argv[i]) + "\n";

  if (mode == Mode::Lsp) {
#ifdef KEX_HAS_LSP
    return kex::lsp::run(std::cin, std::cout, prebuiltRuntimeBeamDir());
#endif
  }

  if (mode == Mode::Complete)
    return printCompletions(completePrefix, optind < argc ? argv[optind] : nullptr);

  // `--skill` prints the guide, `--skill <topic>` one part of it. Neither
  // reads a source file, so both answer before anything looks for one.
  if (skillMode) {
    const std::string_view wanted =
        optind < argc ? std::string_view(argv[optind])
                      : kex::skill::kGuideTopic;
    if (const auto *topic = kex::skill::findTopic(wanted)) {
      std::cout << topic->text;
      return 0;
    }
    std::cerr << "kex: --skill has no topic `" << wanted << "`. It has:";
    for (const auto &topic : kex::skill::topics())
      if (topic.name != kex::skill::kGuideTopic)
        std::cerr << " " << topic.name;
    std::cerr << "\n";
    return 1;
  }

  // `-s` with a name rather than a source file asks the standard library
  // about itself; with nothing at all it lists what there is to ask about.
  if (summaryMode && !replEval &&
      (optind >= argc || !std::string_view(argv[optind]).ends_with(".kex"))) {
    const auto index = kex::help::ApiIndex::fromStandardLibrary(std::cerr);
    if (index.empty()) {
      std::cerr << "error: the standard library sources were not found\n";
      return 1;
    }
    if (optind >= argc) {
      index.printOverview(std::cout);
      return 0;
    }
    return index.printTopic(argv[optind], std::cout, std::cerr) ? 0 : 1;
  }

  if (replEval && (optind < argc || !((mode == Mode::Compile && compileRun) ||
                                      mode == Mode::Repl ||
                                      mode == Mode::BeamRepl))) {
    std::cerr << "error: --eval enters code in the REPL, so it takes no file "
                 "and no other mode (use -i for the tree-walk REPL)\n";
    return 1;
  }

  if (optind >= argc && mode != Mode::Repl) {
    // No file — enter REPL mode (the BEAM REPL by default, the tree-walk
    // REPL for -i and for the other non-BEAM modes)
    if (mode == Mode::Compile && compileRun)
      mode = Mode::BeamRepl;
    else if (mode != Mode::BeamRepl)
      mode = Mode::Repl;
  }

  if (mode == Mode::BeamRepl) return runBeamRepl(skipPrelude);
  if (mode == Mode::Repl) return runWalkerRepl(skipPrelude);

  std::string filepath = argv[optind];
  kex::module::Resolver::setEntryFile(filepath);
#ifndef __EMSCRIPTEN__
  // A spec says it is one to what it runs, the BEAM child included, so a
  // framework can pick its test settings without being told: Metro's
  // environment is `test` under it unless METRO_ENV says otherwise.
  if (isSpecEntry(filepath)) setenv("KEX_TEST", "1", 0);
#endif

  const bool compiledInput =
      filepath.size() > 5 &&
      filepath.compare(filepath.size() - 5, 5, ".beam") == 0;
  // Every other mode reads Kex SOURCE. Handed a compiled module, `-c` used to
  // fall into the run path below and execute it, and the source-reading modes
  // parsed BEAM bytes as program text (kexhq/kex#85).
  if (compiledInput && !(mode == Mode::Compile && compileRun)) {
    std::cerr << "error: " << filepath
              << " is a compiled BEAM module, not Kex source — run it with "
                 "`kex "
              << filepath << "`, or pass the .kex file it was built from\n";
    return 1;
  }

  // `kex file.kx.beam [args]` or `kex file.beam [args]` — run a compiled BEAM
  // module.
  if (compiledInput)
    return runCompiledBeam(filepath, options,
                           std::vector<std::string>(argv + optind + 1, argv + argc));

  // Reject non-.kex files before trying to parse them.
  if (filepath.size() < 4 ||
      filepath.compare(filepath.size() - 4, 4, ".kex") != 0) {
    std::cerr << "error: " << filepath << ": expected a .kex source file\n";
    return 1;
  }

  auto source = readFile(filepath);
  // Kept around (the lexer below consumes `source` with std::move) so the
  // BEAM `--run` path can use the entry file's ORIGINAL bytes as part of a
  // run-result cache key (kexhq/kex#323) — see `runCacheKeyFor`.
  const std::string entryRawSourceForCache = source;
  // An empty file is an empty program to the AST tools. The interpreter answers
  // `Kex.AST.parse("")` and `parseSyntax("")` with an empty tree, and BEAM
  // reaches both through a temporary file, so refusing it here made the two
  // backends disagree. A file that could not be opened still stops here —
  // readFile has already said why.
  if (source.empty() &&
      !((mode == Mode::EmitAst || mode == Mode::EmitSyntax) &&
        std::filesystem::is_regular_file(filepath)))
    return 1;

  // Honour `# kex: no-check` pragma in the first few lines — any file that
  // contains it is treated as if --no-check was passed on the command line.
  {
    std::istringstream ss(source);
    std::string line;
    for (int ln = 0; ln < 10 && std::getline(ss, line); ++ln) {
      if (line.find("# kex: no-check") != std::string::npos) {
        skipCheck = true;
        break;
      }
    }
  }

  // Everything after the script path is the script's own argument list,
  // exposed to Kex code via `main(args) do ... end`.
  std::vector<std::string> scriptArgs;
  for (int i = optind + 1; i < argc; i++) {
    scriptArgs.push_back(argv[i]);
  }

  // --emit-syntax reprints the source from its tokens, and --emit-ast reads
  // doc comments out of their trivia, so both keep the text (and
  // --emit-syntax the token list) that the lexer and parser below consume.
  const std::string syntaxSource =
      mode == Mode::EmitSyntax || mode == Mode::EmitAst ? source : std::string{};
  kex::Lexer lexer(std::move(source), filepath);
  auto tokens = lexer.tokenizeAll();
  const auto syntaxTokens =
      mode == Mode::EmitSyntax ? tokens : std::vector<kex::Token>{};
  const auto astDocs = mode == Mode::EmitAst
      ? kex::ast::extractDocComments(tokens, syntaxSource)
      : std::unordered_map<int, std::string>{};

  if (mode == Mode::Lex) {
    for (const auto &token : tokens) {
      std::cout << token.location.line << ":" << token.location.column << "  "
                << kex::tokenTypeName(token.type);
      if (!token.value.empty()) {
        std::cout << "  [" << token.value << "]";
      }
      std::cout << "\n";
    }
    return 0;
  }

  try {
    kex::Parser parser(std::move(tokens), filepath);
    auto program = parser.parseProgram();

    // In --run and --compile/-R modes, print parse errors and abort —
    // SemanticDB is not invoked yet at this point so we're the only
    // place that can report them cleanly. Without this, --compile/-R
    // fell through to runSemanticCheck, which re-parses via SemanticDB
    // and reports the exact same underlying parse error but mislabeled
    // as "fix type errors" — confusing for something that's actually a
    // syntax error (see spec/error_if_with_do.kex, a real regression
    // case). In --check mode checkEntry reports them with the check's
    // own diagnostics; printing here would duplicate every message.
    if (!parser.diagnostics().empty() &&
        (mode == Mode::Run || mode == Mode::Compile || mode == Mode::EmitAst ||
         mode == Mode::EmitSyntax ||
         mode == Mode::Parse || mode == Mode::Expand || mode == Mode::EmitCore)) {
      if (mode == Mode::EmitAst || mode == Mode::EmitSyntax) {
        const auto &diagnostic = parser.diagnostics().front();
        const auto logicalFilename =
            astFilename.empty() ? filepath : astFilename;
        kex::beam::TermBuilder builder;
        auto location = builder.record("Location", {
            {"file", builder.string(logicalFilename)},
            {"line", builder.integer(int64_t{diagnostic.location.line})},
            {"column", builder.integer(int64_t{diagnostic.location.column})},
            {"startOffset", builder.integer(
                int64_t{diagnostic.location.startOffset})},
            {"endOffset", builder.integer(
                int64_t{diagnostic.location.endOffset})},
        });
        auto error = builder.record("ParseError", {
            {"message", builder.string(diagnostic.message)},
            {"location", builder.just(std::move(location))},
        });
        auto result = builder.variant("Error", "Result", {std::move(error)});
        auto bytes = kex::beam::encodeEtf(result);
        std::cout.write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
        return 0;
      }
      for (const auto &pd : parser.diagnostics()) {
        std::cerr << kex::color::apply(kex::color::gray) << pd.location.file
                  << ":" << pd.location.line << ":" << pd.location.column << ":"
                  << kex::color::apply(kex::color::reset) << " "
                  << kex::color::apply(kex::color::bold)
                  << kex::color::apply(kex::color::red)
                  << "error:" << kex::color::apply(kex::color::reset) << " "
                  << colorizeMessage(pd.message) << "\n";
      }
      std::cerr << kex::color::apply(kex::color::bold)
                << kex::color::apply(kex::color::magenta)
                << "Aborted:" << kex::color::apply(kex::color::reset) << " "
                << errorCountPhrase(static_cast<int>(parser.diagnostics().size()),
                                    "syntax")
                << " — fix before "
                << (mode == Mode::Run || compileRun
                        ? "running"
                        : (mode == Mode::Parse || mode == Mode::Expand
                               ? "parsing"
                               : "compiling"))
                << ".\n";
      return 1;
    }

    if (mode == Mode::Parse) {
      printAst(program);
      return 0;
    }

    if (mode == Mode::EmitSyntax) {
      kex::beam::TermBuilder builder;
      auto tree = kex::ast::buildSyntaxTree(builder, syntaxTokens, syntaxSource,
                                            parser.syntaxSpans());
      if (!tree) {
        std::cerr << "error: " << filepath
                  << ": the source could not be divided among its tokens\n";
        return 1;
      }
      auto bytes = kex::beam::encodeEtf(*tree);
      std::cout.write(reinterpret_cast<const char *>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
      return 0;
    }
    if (mode == Mode::EmitAst) {
      kex::beam::TermBuilder builder;
      kex::ast::Converter converter(builder);
      auto bytes = kex::beam::encodeEtf(
          converter.program(program,
                            astFilename.empty() ? filepath : astFilename,
                            astDocs));
      if (outputDirExplicit) {
        std::ofstream output(outputDir, std::ios::binary);
        if (!output) {
          std::cerr << "error: cannot open AST output file: " << outputDir
                    << "\n";
          return 1;
        }
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
      } else {
        std::cout.write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
      }
      return 0;
    }

    // Compile-time expansion of `compiled do` blocks. Deliberately the ONE
    // place every mode passes through, right after parsing and before any
    // semantic pass — so --check, --run, --compile, --emit-core and -R can
    // never disagree about what the program contains.
    //
    // Note for when this stops being an identity pass: the BEAM path also
    // merges a `.spec.kex` base file's declarations into program.items further
    // down, and resolveBeamDeps pulls in dependency modules. Those items are
    // parsed AFTER this point, so their own `compiled` blocks would not be
    // expanded here — they need either their own expand() call at load time or
    // a second pass once the program is fully assembled.
    auto expandParsedProgram = [&](kex::ast::Program &target,
                                   bool reportCollapse) -> bool {
      std::vector<kex::semantic::Diagnostic> expandDiagnostics;
      std::vector<kex::compiled::CollapseNote> collapseNotes;
      kex::compiled::ExpandOptions expandOptions;
      // Kex.embed(...) resolves relative to the ENTRY file, even on the
      // later calls below where dependency modules have been merged into
      // `target` — a Kex.embed inside a dependency module's own source
      // resolves against the wrong directory until per-declaration source
      // provenance exists (the same gap this function's own comment notes
      // for compiled blocks in merged-in modules).
      expandOptions.sourcePath = filepath;
      expandOptions.moduleRoots = moduleRootsFor(filepath);
      expandOptions.interfaces = &preludeSemanticInterfaces();
      setScanCache(expandOptions);
      if (reportCollapse) expandOptions.report = &collapseNotes;
      const bool expanded =
          kex::compiled::expand(target, expandDiagnostics, expandOptions);
      if (reportCollapse) printCollapseReport(collapseNotes);
      for (const auto &diagnostic : expandDiagnostics)
        printSemanticDiagnostic(diagnostic);
      if (!expanded) {
        std::cerr << kex::color::apply(kex::color::bold)
                  << kex::color::apply(kex::color::magenta)
                  << "Aborted:" << kex::color::apply(kex::color::reset)
                  << " compile-time expansion failed.\n";
        return false;
      }
      return true;
    };
    if (!expandParsedProgram(program, collapseReport)) return 1;

    if (mode == Mode::Expand) {
      printAst(program);
      return 0;
    }

    if (mode == Mode::Compile || mode == Mode::EmitCore)
      return compileToBeam(options, filepath, program, scriptArgs,
                           entryRawSourceForCache, buildFlags);

    if (mode == Mode::Run)
      return runOnWalker(options, filepath, program, scriptArgs,
                         expandParsedProgram);
    return checkEntry(options, filepath, program, parser.diagnostics());
  } catch (const std::exception &e) {
    std::cerr << kex::color::apply(kex::color::bold)
              << kex::color::apply(kex::color::red)
              << "Internal error:" << kex::color::apply(kex::color::reset)
              << " " << e.what() << "\n";
    return 1;
  }

  return 0;
}
