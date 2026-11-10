// What every command does with a program before it builds or runs it: the
// entry's companion files merged in, its source roots found, its check run
// and reported. Shared by `kex -c`, `kex -R`, the REPLs and `--check`.

#include "../beam/beam_file.hxx"
#include "../beam/collect_metadata.hxx"
#include "../beam/etf.hxx"
#include "../beam/kexi.hxx"
#include "../beam/kexi_registry.hxx"
#include "../cache/cache.hxx"
#include "../cache/separate_unit.hxx"
#include "../beam/term_builder.hxx"
#include "../ast/clone.hxx"
#include "../ast/convert.hxx"
#include "../ast/syntax.hxx"
#include "../ast/type_text.hxx"
#include "../common/artifact_versions.hxx"
#include "../common/prelude_tiers.hxx"
#include "../common/color.hxx"
#include "../interpreter/evaluator.hxx"
#include "../hir/hir.hxx"
#include "../hir/repl_input.hxx"
#include "../ir/emit_core.hxx"
#include "../ir/lower.hxx"
#include "../lexer/lexer.hxx"
#include "../compiled/expand.hxx"
#include "../module/discovery.hxx"
#include "../module/resolver.hxx"
#include "../parser/parser.hxx"
#include "../semantic/analyzer.hxx"
#include "../semantic/db.hxx"
#include "../validation/tag_validator.hxx"
#include "../common/runtime_dir.hxx"
#include "dependencies.hxx"
#include "build_support.hxx"
#include "../repl/session.hxx"
#include "compile.hxx"
#include "options.hxx"
#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef __EMSCRIPTEN__
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
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
#include "../common/completion.hxx"
#include "../common/diagnostics.hxx"
#include "../common/digest.hxx"
#include "../common/file_io.hxx"
#ifdef KEX_HAS_LSP
#include "../lsp/server.hxx"
#endif
#include "../common/prelude_interfaces.hxx"
#include "../common/prelude_loader.hxx"
#include "../common/process.hxx"
#include "../common/repl_commands.hxx"
#include "../common/string_utils.hxx"
#include "../common/timings.hxx"
#include "../common/well_known.hxx"
#include "../semantic/completion_at.hxx"
#include "../semantic/symbol_queries.hxx"
#include <functional>
#include <thread>

using kex::prebuiltRuntimeBeamDir;
using kex::driver::buildProgress;
using kex::driver::BuildProgress;
using kex::driver::dependencyFailure;
using kex::driver::g_dependencyFailureThrows;
using kex::driver::g_memoizeDependencyExpansion;
using kex::driver::isAutomaticImport;
using kex::driver::LoadedDep;
using kex::driver::moduleCount;
using kex::driver::prependItems;
using kex::driver::reportModuleFileError;
using kex::driver::setDefaultScanCache;
using kex::driver::setScanCache;
using kex::driver::sourceModulesNamedBy;
using kex::driver::UnitGraph;
using kex::driver::unitsOf;
using kex::process::BeamVm;
using kex::process::beamTestGate;
using kex::process::exitStatusOf;
using kex::process::runShellCommand;
using kex::process::runShellCommandsTogether;
using kex::process::shellSingleQuote;
using kex::readFile;
using kex::diagnostics::colorizeMessage;
using kex::diagnostics::errorCountPhrase;
using kex::diagnostics::extractHint;
using kex::diagnostics::jsonEscape;
using kex::diagnostics::printCollapseReport;
using kex::diagnostics::printSemanticDiagnostic;
using kex::ast::typeText;
using kex::module::collectUsingModules;
using kex::module::declaredModuleNames;
using kex::module::fileDeclaringModule;
using kex::module::moduleHeaderOf;
using kex::module::moduleNameForPath;
using kex::module::shownPath;
using kex::module::skippedByModuleWalk;
using kex::module::standardModuleRoots;
using kex::snakeCased;

// Additional project roots supplied by repeatable `--source-root` options.
// Keeping this at the CLI boundary lets every existing consumer of
// moduleRootsFor (semantic checks, discovery, validation and both runtimes)
// observe exactly the same search path.
std::vector<std::string> cliSourceRoots;
// `--quiet` and `--progress`: whether a build shows its steps (BuildProgress).
bool quietBuild = false;

// `--sname` / `--name` / `--cookie`: start the BEAM VM as a distributed node,
// so the program can connect to others (`Node.connect`) and they to it. They
// become erl's own `-sname`/`-name`/`-setcookie` on every VM kex launches.
std::vector<std::string> cliDistributionArgs;


auto distributionArgsForShell() -> std::string {
  std::string out;
  for (const auto &arg : cliDistributionArgs)
    out += " " + shellSingleQuote(arg);
  return out;
}

// What erl would reject as a node name, said before the VM is launched: left
// to erl, a bad name kills the kernel during boot and the user gets a crash

// Is this entry file a spec — the one kind of program allowed to install
// mocks without asking (issue #144)? Deliberately the FILE NAME and not, say,
// "the program called describe somewhere": the answer has to be known before
// a single line runs, and it has to be something a reader of the command line
// can check. Everything else needs --allow-mocks.
auto isSpecEntry(const std::string &filepath) -> bool {
  static const std::string suffix = ".spec.kex";
  return filepath.size() > suffix.size() &&
         filepath.compare(filepath.size() - suffix.size(), suffix.size(),
                          suffix) == 0;
}

// Convention: `<name>.spec.kex` is a spec for `<name>.kex` and doesn't need
// to redeclare its types/records/functions — running the spec auto-loads
// the base file's declarations (skipping its own `main` block(s), so its
// demo output/side effects don't run) into the same scope first. Looked up
// next to the spec file, and — since this project keeps specs in spec/
// alongside examples in a sibling examples/ — also under examples/ if the
// spec's directory is named "spec".
auto specBaseCandidates(const std::string &filepath)
    -> std::vector<std::string> {
  static const std::string suffix = ".spec.kex";
  // A `package.kex` manifest speaks a vocabulary the stdlib describes but no
  // program imports (`bundle`, `version`, `tey`, ...). It comes in the same
  // way a spec's base file does, so checking a manifest reports real
  // mistakes instead of "undefined function" on every declaration.
  if (std::filesystem::path(filepath).filename() == "package.kex") {
    if (auto vocabulary = kex::manifestVocabularyFile(); !vocabulary.empty())
      return {vocabulary};
    return {};
  }
  if (filepath.size() <= suffix.size())
    return {};
  if (filepath.compare(filepath.size() - suffix.size(), suffix.size(),
                       suffix) != 0)
    return {};

  std::string stem = filepath.substr(0, filepath.size() - suffix.size());
  auto slash = stem.find_last_of('/');
  std::string dir = slash == std::string::npos ? "" : stem.substr(0, slash);
  std::string name = slash == std::string::npos ? stem : stem.substr(slash + 1);

  std::vector<std::string> candidates = {stem + ".kex"};
  const std::string specSuffix = "spec";
  if (dir.size() >= specSuffix.size() &&
      dir.compare(dir.size() - specSuffix.size(), specSuffix.size(),
                  specSuffix) == 0) {
    std::string parent = dir.substr(0, dir.size() - specSuffix.size());
    candidates.push_back(parent + "examples/" + name + ".kex");
  }
  return candidates;
}

auto fileExists(const std::string &path) -> bool {
  std::ifstream probe(path);
  return probe.good();
}

// `file`'s declarations as a companion contributes them: everything but its
// own `main` block(s). `text` is the file's source when the caller already
// has it. Parse errors are left to whatever checks `file` itself.
auto declarationsOf(const std::string &file, std::string text)
    -> std::vector<kex::ast::TopLevelItem> {
  kex::Lexer lexer(std::move(text), file);
  kex::Parser parser(lexer.tokenizeAll(), file);
  auto parsed = parser.parseProgram();
  std::vector<kex::ast::TopLevelItem> declarations;
  for (auto &item : parsed.items)
    if (kex::ast::isMergedDeclaration(item))
      declarations.push_back(std::move(item));
  return declarations;
}

auto declarationsOf(const std::string &file)
    -> std::vector<kex::ast::TopLevelItem> {
  return declarationsOf(file, readFile(file));
}


// Prepends each file's declarations, the first file's first.
auto prependDeclarationsOf(kex::ast::Program &program,
                           const std::vector<std::string> &files) -> void {
  std::vector<kex::ast::TopLevelItem> merged;
  for (const auto &file : files)
    for (auto &item : declarationsOf(file)) merged.push_back(std::move(item));
  prependItems(program, std::move(merged));
}

// Merges the entry's companion — a spec's base file, a manifest's vocabulary
// (specBaseCandidates) — into `program`. Returns the file merged, or "" when
// there is none; `rawSource`, when given, receives its text.
auto mergeSpecBase(kex::ast::Program &program, const std::string &entry,
                   std::string *rawSource) -> std::string {
  for (const auto &candidate : specBaseCandidates(entry)) {
    if (!fileExists(candidate)) continue;
    auto text = readFile(candidate);
    if (rawSource) *rawSource = text;
    prependItems(program, declarationsOf(candidate, std::move(text)));
    return candidate;
  }
  return "";
}



auto checkForCompletion(const std::string &path, const std::string &source)
    -> std::optional<kex::semantic::CheckedSource> {
  auto program = std::make_shared<kex::ast::Program>();
  {
    kex::Lexer lexer(source, path);
    kex::Parser parser(lexer.tokenizeAll(), path);
    *program = parser.parseProgram();
  }
  const auto roots =
      moduleRootsFor((std::filesystem::current_path() / "<repl>").string());
  std::vector<kex::semantic::Diagnostic> expansionDiagnostics;
  kex::compiled::ExpandOptions options;
  options.sourcePath = path;
  options.moduleRoots = roots;
  options.interfaces = &preludeSemanticInterfaces();
  kex::compiled::expand(*program, expansionDiagnostics, options);
  // Its `using` modules merged in, as the REPL merges them to run an input; a
  // module that cannot be loaded leaves the rest to complete.
  auto dependencies = std::make_shared<std::vector<LoadedDep>>();
  const bool throws = g_dependencyFailureThrows;
  g_dependencyFailureThrows = true;
  try {
    *dependencies = resolveBeamDeps(*program, roots, &preludeSemanticInterfaces());
  } catch (const std::exception &) {
  }
  g_dependencyFailureThrows = throws;
  auto analyzer = std::make_shared<kex::semantic::Analyzer>(&preludeSemanticInterfaces());
  analyzer->setNameCheckFiles({path});
  analyzer->analyze(*program);
  return kex::semantic::CheckedSource{program, analyzer, dependencies};
}

auto preludeCompletionSymbols() -> const std::vector<kex::semantic::SymbolInfo> & {
  static const auto symbols = kex::semantic::symbolsOfFiles(
      kex::preludeSourceFiles(),
      moduleRootsFor((std::filesystem::current_path() / "<repl>").string()));
  return symbols;
}

auto moduleRootsFor(const std::string &filepath) -> std::vector<std::string> {
  namespace fs = std::filesystem;
  const auto srcDir = fs::weakly_canonical(filepath).parent_path();
  std::vector<std::string> roots;
  for (const auto &configured : cliSourceRoots) {
    std::error_code ec;
    auto root = fs::weakly_canonical(configured, ec);
    const auto normalized = ec ? fs::path(configured).lexically_normal() : root;
    const auto value = normalized.string();
    if (std::find(roots.begin(), roots.end(), value) == roots.end())
      roots.push_back(value);
  }
  for (const auto &relative : {"lib", "src"}) {
    const auto candidate = srcDir / relative;
    std::error_code ec;
    if (fs::is_directory(candidate, ec) && !ec)
      roots.push_back(candidate.string());
  }
  if (roots.empty())
    roots.push_back(srcDir.string());
  for (auto &root : kex::standardLibraryModuleRoots())
    if (std::find(roots.begin(), roots.end(), root) == roots.end())
      roots.push_back(std::move(root));
  return roots;
}

// The compile-time expansion a REPL input needs before it is checked or run:
// a file run goes through `expandParsedProgram`, and without this the REPL
// left `Kex.embed`, `Template.text`/`html` and `compiled do` for run time,
// where none of them exists (kexhq/kex#543). An input has no file of its own,
// so it is expanded as if it were one in the working directory: that is
// where `Kex.embed`'s relative paths and the session's modules are found.
//
// Each call gets a name of its own: generated template functions are named
// after the path, and the walker REPL keeps every input's definitions, so a
// second input reusing `<repl>` redefined the first one's templates.
auto expandReplProgram(kex::ast::Program &program) -> void {
  static int inputs = 0;
  const auto here = (std::filesystem::current_path() /
                     ("<repl-" + std::to_string(++inputs) + ">"))
                        .string();
  std::vector<kex::semantic::Diagnostic> diagnostics;
  kex::compiled::ExpandOptions options;
  options.sourcePath = here;
  options.moduleRoots = moduleRootsFor(here);
  options.interfaces = &preludeSemanticInterfaces();
  setScanCache(options);
  if (kex::compiled::expand(program, diagnostics, options)) return;
  for (const auto &diagnostic : diagnostics)
    if (diagnostic.level == kex::semantic::Diagnostic::Level::Error)
      throw std::runtime_error(diagnostic.message);
  throw std::runtime_error("compile-time expansion failed");
}


// The checked type of a REPL expression, when it is worth showing. Both REPLs
// otherwise derive the type from the VALUE, which cannot recover what runtime
// representation erases (a typestate parameter, an empty list's element type,
// the unused half of a Result). A gradual/unannotated expression checks as
// `?`/`A`/`N`, and there the value is the better source — hence the
// concreteness gate rather than "always prefer static".
auto displayTypeOf(const kex::semantic::Analyzer &analyzer,
                   const kex::ast::Expr *expr)
    -> std::optional<std::string> {
  if (!expr) return std::nullopt;
  auto type = analyzer.displayTypeOf(expr);
  if (!type || !kex::semantic::isFullyConcrete(type)) return std::nullopt;
  return kex::semantic::typeToString(type);
}



// Runs semantic analysis (undefined-name detection + type checking) and
// prints any diagnostics, same as plain `run` mode's pre-execution check.
// Shared by `run` and `compile`/`-R` (BEAM) so both backends catch the same
// errors before doing anything backend-specific — previously `-R` skipped
// this entirely and fell straight through to erlc, which reports things
// like an undefined function as a raw, un-Kex-like Core Erlang compile
// error ("unbound variable 'UndefinedFunctionCall' in main/0") instead of
// this backend-agnostic diagnostic. Returns false if there were any errors.
// `errorCount`, when given, receives the number of error-level diagnostics
// reported — the abort message quotes it so a long compile shows progress as
// the count comes down, rather than just repeating "fix type errors".
// The full check's setup, for the merged program and for each unit checked
// apart alike: names are reported for `files` (the entry and a spec's base),
// and an exported module the program does not merge is looked for where the
// build finds modules.
auto configureFullCheck(kex::semantic::Analyzer &analyzer, std::vector<std::string> files,
                        const std::string &entryPath) -> void {
  analyzer.setModuleLocator(
      [resolver = kex::module::Resolver(moduleRootsFor(entryPath))](
          const std::string &module, const std::string &current) {
        return resolver.resolve(module, current).has_value();
      });
  analyzer.setNameCheckFiles(std::move(files));
}

// What the full check adds after a clean analysis: tagged literals.
auto taggedLiteralDiagnostics(const kex::ast::Program &program,
                              const kex::semantic::Analyzer &analyzer,
                              const std::string &entryPath)
    -> std::vector<kex::semantic::Diagnostic> {
  return kex::validation::validateTaggedLiterals(program, analyzer, moduleRootsFor(entryPath));
}

auto runSemanticCheck(const kex::ast::Program &program,
                      const std::string &filepath,
                      kex::semantic::Analyzer *retainedAnalyzer,
                      const std::string &extraDeclFile,
                      int *errorCount) -> bool {
  int errors = 0;
  auto countError = [&](const kex::semantic::Diagnostic &diag) {
    if (diag.level == kex::semantic::Diagnostic::Level::Error) errors++;
  };
  // KEX_TIMINGS=1 splits the type check into its passes (kexhq/kex#323).
  kex::timings::PhaseTimer passTimer(4, false);
  auto phase = [&](const char *name) { passTimer.mark(name); };
  // One sema: names (the resolver SemanticDB indexes files with, over the
  // checked program, its dependencies and a spec's base merged in; reported
  // for the entry file and the base), module rules, purity and types.
  std::vector<std::string> files{filepath};
  if (!extraDeclFile.empty()) files.push_back(extraDeclFile);
  kex::semantic::Analyzer localAnalyzer(&preludeSemanticInterfaces());
  auto &analyzer = retainedAnalyzer ? *retainedAnalyzer : localAnalyzer;
  configureFullCheck(analyzer, std::move(files), filepath);
  bool ok = analyzer.analyze(program);
  for (const auto &diag : analyzer.diagnostics()) {
    countError(diag);
    printSemanticDiagnostic(diag);
  }
  phase("analyzer (whole program)");

  bool validationOk = true;
  if (ok) {
    for (const auto &diag :
         taggedLiteralDiagnostics(program, analyzer, filepath)) {
      if (diag.level == kex::semantic::Diagnostic::Level::Error)
        validationOk = false;
      countError(diag);
      printSemanticDiagnostic(diag);
    }
    phase("tagged literal validation");
  }

  if (errorCount) *errorCount = errors;
  return ok && validationOk;
}

auto printAst(const kex::ast::Program &program) -> void {
  std::cout << "Program (" << program.items.size() << " items)\n";
  for (const auto &item : program.items) {
    std::visit(
        [](const auto &node) {
          using T = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<T,
                                       std::unique_ptr<kex::ast::ModuleDef>>) {
            std::cout << "  Module: " << node->name << "\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::TypeDef>>) {
            std::cout << "  Type: " << node->name << "\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::RecordDef>>) {
            std::cout << "  Record: " << node->name << " ("
                      << node->fields.size() << " fields)\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::MakeDef>>) {
            std::cout << "  Make" << (node->isFinal ? " [final]" : "") << "\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::FunctionDef>>) {
            std::cout << "  Function: " << node->name
                      << (node->isFoul ? " [foul]" : "")
                      << (node->isPredicate ? " [?]" : "") << "\n";
          } else if constexpr (std::is_same_v<
                                   T,
                                   std::unique_ptr<kex::ast::CompiledBlock>>) {
            std::cout << "  Compiled block\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::UsingBlock>>) {
            std::cout << "  Using block\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::MainBlock>>) {
            std::cout << "  Main (" << node->body.size() << " expressions)\n";
          } else if constexpr (std::is_same_v<
                                   T, std::unique_ptr<kex::ast::Pragma>>) {
            std::cout << "  Pragma: ";
            for (const auto &r : node->requirements)
              std::cout << r << " ";
            std::cout << "\n";
          }
        },
        item);
  }
}

// An emitted module's routed implementations, as recordImplementations asks.
auto routedIn(const std::set<std::pair<std::string, int>> &routed)
    -> std::function<bool(const std::string &, int)> {
  return [&routed](const std::string &function, int arity) {
    return routed.count({function, arity}) > 0;
  };
}

// The modules providing receiver functions in `externals`: what the check
// counts a call's providers among (Analyzer::setLinkedModules).
auto providerModules(const kex::ir::ExternalModules &externals) -> std::set<std::string> {
  std::set<std::string> modules;
  for (const auto &[_, providers] : externals.receiverFunctions)
    for (const auto &provider : providers) modules.insert(provider.moduleAtom);
  return modules;
}

// The modules `program` uses, loaded and merged in (kex::driver). A build
// does this through its pass pipeline (build_ir.hxx), unit by unit.
auto resolveBeamDeps(kex::ast::Program &program, const std::vector<std::string> &roots,
                     const kex::semantic::ImportedInterfaces *interfaces,
                     const std::optional<std::unordered_set<std::string>> &qualifiedModules)
    -> std::vector<LoadedDep> {
  auto dependencies =
      kex::driver::loadDependencies(program, roots, interfaces, qualifiedModules);
  kex::driver::mergeDependencies(program, dependencies, roots, interfaces);
  return std::move(dependencies.files);
}
