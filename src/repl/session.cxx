// The two REPLs (`kex -i`, the BEAM one, and `kex -R -i`, the tree-walker's):
// reading an input, classifying it, checking it against the session, and
// running it. Moved out of main.cxx; the build services they share with the
// compile commands are declared in driver/build_support.hxx.
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
#include "../ir/pass_manager.hxx"
#include "../driver/pipeline.hxx"
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
#include "../driver/dependencies.hxx"
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
#include "../common/completion.hxx"
#include "../common/diagnostics.hxx"
#include "../common/digest.hxx"
#include "../common/file_io.hxx"
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
#include "../driver/build_support.hxx"
#include "input.hxx"
#include "session.hxx"

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
// The BEAM REPL (`kex -i`, and `kex -c --run` without a file): every input is
// lowered and evaluated in a long-lived BEAM VM. One session: that VM, and
// everything defined in it so far.
class BeamReplSession {
public:
  explicit BeamReplSession(bool skipPrelude) : m_skipPrelude(skipPrelude) {}
  auto run() -> int;

private:
  bool m_skipPrelude;
  // Writable directory for each evaluation's beams, and the prebuilt runtime.
  std::string m_beamDir;
  std::string m_rtPaDir;
  // ONE persistent erl VM for the whole session — driven by
  // runtime/src/kex_repl_driver.erl over stdin/stdout. This is what makes
  // spawned processes and registered names survive across inputs, and turns
  // session recompiles into hot-loads instead of cold re-runs.
  BeamVm m_vm;
  // Completion's index (prelude + session) and the one inputs are checked in.
  ReplCompletionBase m_completionBase;
  // Top-level definitions, tracked by name so redefining a function
  // REPLACES its earlier clauses rather than appending duplicates (a
  // stale first clause would otherwise shadow the new one — the
  // "this clause cannot match" BEAM warning). Insertion order kept so
  // independent defs stay in source order.
  std::vector<std::pair<std::string, std::string>> m_topDefs;
  std::string m_localBinds; // let x = ... — re-emitted inside main do each eval
  // Names bound with `var`. They are replayed as `var` (not `let`) so a
  // later line may reassign or `!`-mutate them, and every evaluation stores
  // them back into the process dictionary so those mutations survive.
  std::vector<std::string> m_mutableBinds;
  // Original local binding sources retained for semantic replay. Runtime
  // replay uses process-dictionary lookups, which intentionally erase the
  // source type and therefore cannot enforce typestate by itself.
  std::vector<std::pair<std::string, std::string>> m_beamSemanticBinds;
  std::vector<ReplSessionWith> m_sessionWiths;
  // Each input's session module gets a name of its own. Reloading one stable
  // name on every input meant the BEAM purged the version two inputs back —
  // killing any process spawned there that was still blocked in `receive`,
  // and turning every fun created there into a badfun.
  int m_sessionVersion = 0;
  // The Core Erlang last loaded for each companion module (a `module`
  // defined in the session). One is reloaded only when its code changed,
  // so its processes are purged only by redefinition, as in Erlang.
  std::map<std::string, std::string> m_loadedCompanionCore;
  std::vector<std::string> m_loadedBeamFiles; // .kex paths loaded via /load
  // Where a module's `using`s are found (see `run`), and those roots minus
  // the standard library's.
  std::vector<std::string> m_replRoots;
  std::vector<std::string> m_replProjectRoots;
  kex::beam::KexiRegistry m_kexiRegistry;

  auto isMutableBind(const std::string &name) const -> bool {
    return std::find(m_mutableBinds.begin(), m_mutableBinds.end(), name) !=
           m_mutableBinds.end();
  }
  // Prepends the loaded files' declarations (and `extra`, a file being
  // `/load`-ed) to a checker program, then the project modules they and the
  // input use. Installed stdlib modules stay external: their source would
  // shadow their typed interface (see the expression check in `evaluate`).
  auto mergeLoadedForCheck(kex::ast::Program &program,
                           const std::string &extra = {}) -> void {
    auto files = m_loadedBeamFiles;
    if (!extra.empty()) files.push_back(extra);
    prependDeclarationsOf(program, files);
    resolveBeamDeps(program, m_replProjectRoots, &preludeSemanticInterfaces(),
                    sourceModulesNamedBy(program));
  }
  auto topDefsStr() const -> std::string {
    std::string s;
    for (const auto &[name, source] : m_topDefs) s += source + "\n";
    return s;
  }
  // `/load file`: a compiled `.beam` joins through its KexI, a `.kex` file
  // once it parses and checks with the session.
  auto load(const std::string &filePath) -> ReplStep;
  // Evaluates one complete input (`source`, read as `shape`) in the session,
  // `pendingWith` being a `/with` waiting for its value to evaluate.
  auto evaluate(const std::string &source, const ReplInput &shape,
                const std::optional<ReplSessionWith> &pendingWith) -> ReplStep;
  auto define(const std::string &source) -> void;
  auto loadModules(const std::vector<kex::ir::EmitResult> &results) -> bool;
};

auto runBeamRepl(bool skipPrelude) -> int {
  BeamReplSession session(skipPrelude);
  return session.run();
}

auto BeamReplSession::load(const std::string &filePath) -> ReplStep {
  if (!fileExists(filePath)) {
    std::cerr << "  /load: file not found: " << filePath << "\n";
    return ReplStep::Next;
  }

  // Compiled .beam/.kx.beam: load via KexI registry + hot-load into VM
  bool isBeam = (filePath.size() > 5 &&
                 filePath.substr(filePath.size() - 5) == ".beam");
  if (isBeam) {
    auto absPath = std::filesystem::weakly_canonical(filePath).string();
    auto errors = m_kexiRegistry.loadUnit(absPath);
    if (!errors.empty()) {
      for (const auto& err : errors)
        std::cerr << "  /load: " << err.message << "\n";
      return ReplStep::Next;
    }
    auto* unit = m_kexiRegistry.getUnit(
        m_kexiRegistry.lastLoadedEntryAtom());
    if (!unit) {
      std::cerr << "  /load: internal error\n";
      return ReplStep::Next;
    }
    // Hot-load each module into the running BEAM VM using the
    // VM's "load" protocol command.
    bool loadOk = true;
    for (const auto& mod : unit->modules) {
      if (!vmRequest(m_vm, "load", mod.beamAtom + " " + mod.beamPath).ok()) {
        std::cerr << "  /load: failed to hot-load " << mod.beamAtom
                  << "\n";
        loadOk = false;
        break;
      }
    }
    if (!loadOk) return ReplStep::Next;

    auto displayExpr = m_kexiRegistry.generateDisplayRegistration(*unit);
    if (!displayExpr.empty()) vmRequest(m_vm, "exec", displayExpr);

    auto stubs = m_kexiRegistry.generateCompletionStubs(*unit);
    if (!stubs.empty()) {
      try {
        const auto name = "<kexi:" + m_kexiRegistry.lastLoadedEntryAtom() + ">";
        m_completionBase.loaded[name] = kex::semantic::symbolsOfSource(name, stubs, {});
      } catch (...) {}
    }

    std::cout << "  loaded " << filePath;
    if (unit->modules.size() > 1)
      std::cout << " (" << unit->modules.size() << " modules)";
    std::cout << "\n";
    return ReplStep::Next;
  }

  // Source .kex file: validate syntax and register for session build
  try {
    auto src = readFile(filePath);
    kex::Lexer lex(std::move(src), filePath);
    kex::Parser parser(lex.tokenizeAll(), filePath);
    parser.parseProgram();
    throwOnParseErrors(parser);
  } catch (const std::exception& e) {
    std::cerr << "  /load: " << filePath << " was not loaded: "
              << e.what() << "\n";
    return ReplStep::Next;
  }
  // Checked with the session and the modules it uses before it joins:
  // a file that does not check is refused here, rather than kept and
  // replayed into every later input, each of which then failed with
  // its error.
  try {
    kex::Lexer checkLexer(topDefsStr() + "main do\nend\n", "<repl>");
    kex::Parser checkParser(checkLexer.tokenizeAll(), "<repl>");
    auto checkProgram = checkParser.parseProgram();
    expandReplProgram(checkProgram);
    mergeLoadedForCheck(checkProgram, filePath);
    kex::semantic::Analyzer checkAnalyzer(&preludeSemanticInterfaces());
    if (!checkAnalyzer.analyze(checkProgram)) {
      std::string message = "semantic analysis failed";
      for (const auto &diagnostic : checkAnalyzer.diagnostics())
        if (diagnostic.level == kex::semantic::Diagnostic::Level::Error) {
          message = diagnostic.message;
          break;
        }
      throw std::runtime_error(message);
    }
  } catch (const std::exception& e) {
    std::cerr << "  /load: " << filePath << " was not loaded: "
              << e.what() << "\n";
    if (g_replLineFromScript) {
      g_replScriptFailed = true;
      return ReplStep::Stop;
    }
    return ReplStep::Next;
  }
  m_loadedBeamFiles.push_back(filePath);
  std::cout << "  loaded " << filePath << "\n";
  return ReplStep::Next;
}

// A definition (`let f(...)`, a record, a module, ...): checked against the
// session's definitions, then kept, replacing an earlier one of that name.
auto BeamReplSession::define(const std::string &source) -> void {
  // Validate by parsing against accumulated defs.
  std::string check = topDefsStr() + source + "\n";
  kex::Lexer lexer(check);
  auto tokens = lexer.tokenizeAll();
  kex::Parser parser(std::move(tokens));
  parser.parseProgram();
  throwOnParseErrors(parser);

  std::string fname = replDefinitionName(source);
  const auto previousTopDefs = m_topDefs;

  // A new clause of a function already defined EXTENDS it; anything
  // else replaces what was there. Redefinition still takes effect
  // (last def wins) because a clause whose parameter patterns match
  // an existing one overwrites that clause rather than being added
  // beside it — otherwise the stale clause would shadow the new one
  // and BEAM would warn that it cannot match.
  auto existing =
      std::find_if(m_topDefs.begin(), m_topDefs.end(),
                   [&](const auto &p) { return p.first == fname; });
  const auto head = replClauseHead(source);
  if (existing != m_topDefs.end() && head) {
    // Substituted IN PLACE, never moved to the end: clause order is
    // match order, so hoisting a catch-all above a base case turns
    // `fact` into an infinite recursion.
    std::string rebuilt;
    bool substituted = false;
    for (const auto &clause :
         splitReplClauses(existing->second, replBlockDepth)) {
      if (!rebuilt.empty()) rebuilt += "\n";
      if (replClauseHead(clause) == head) {
        rebuilt += source;
        substituted = true;
      } else {
        rebuilt += clause;
      }
    }
    if (!substituted) {
      if (!rebuilt.empty()) rebuilt += "\n";
      rebuilt += source;
    }
    existing->second = rebuilt;
  } else {
    m_topDefs.erase(
        std::remove_if(m_topDefs.begin(), m_topDefs.end(),
                       [&](const auto &p) { return p.first == fname; }),
        m_topDefs.end());
    m_topDefs.push_back({fname, source});
  }

  // A parse is not enough: a definition the checker rejects (a pure
  // `let` that receives, say) was kept anyway, and every later input
  // — which replays all definitions — then failed with its error.
  // Any failure here restores the earlier definitions, including a
  // dependency that cannot be loaded (thrown from mergeLoadedForCheck).
  try {
    const auto checkSource = topDefsStr() + "main do\nend\n";
    kex::Lexer checkLexer(checkSource, "<repl>");
    kex::Parser checkParser(checkLexer.tokenizeAll(), "<repl>");
    auto checkProgram = checkParser.parseProgram();
    expandReplProgram(checkProgram);
    mergeLoadedForCheck(checkProgram);
    kex::semantic::Analyzer checkAnalyzer(&preludeSemanticInterfaces());
    if (!checkAnalyzer.analyze(checkProgram)) {
      for (const auto &diagnostic : checkAnalyzer.diagnostics())
        if (diagnostic.level == kex::semantic::Diagnostic::Level::Error)
          throw std::runtime_error(diagnostic.message);
      throw std::runtime_error("semantic analysis failed");
    }
  } catch (...) {
    m_topDefs = previousTopDefs;
    throw;
  }

  // `using M` is an import, not a definition — it is kept in m_topDefs
  // so it persists across inputs, but saying "defined M" is wrong.
  const bool isImport = source.rfind("using ", 0) == 0;
  std::cout << kex::color::apply(kex::color::gray) << "=> "
            << kex::color::apply(kex::color::reset)
            << (isImport ? "using " : "defined ") << fname << "\n";
}

// Compiles and hot-loads each module in the persistent VM, which is the VM
// that will run the code anyway. This used to fork an `erlc` per module and
// load the .beam it wrote: a whole BEAM starting up to compile one small
// module, ~150 ms a line, which a sampling profile put at 93% of this REPL's
// wall time (kexhq/kex#318). Companion modules go first, in reverse: the
// session entry can then call their functions the moment it is loaded
// itself. False when one did not build, having said why.
auto BeamReplSession::loadModules(
    const std::vector<kex::ir::EmitResult> &results) -> bool {
  bool loaded = true;
  std::vector<std::pair<std::string, std::string>> newlyLoaded;
  for (auto it = results.rbegin(); it != results.rend(); ++it) {
    if (it != std::prev(results.rend())) {
      auto previous = m_loadedCompanionCore.find(it->moduleName);
      if (previous != m_loadedCompanionCore.end() &&
          previous->second == it->source)
        continue;
    }
    std::string corePath = m_beamDir + "/" + it->moduleName + ".core";
    std::ofstream cf(corePath);
    if (!cf) {
      std::cerr << "  error: cannot write " << corePath << "\n";
      loaded = false;
      break;
    }
    cf << it->source;
    cf.close();


    // Whatever the compiler had to say arrives above the sentinel;
    // it is the only account of WHY a module did not build, so it
    // reaches the terminal rather than being dropped.
    auto compiled = vmRequest(m_vm, "compile", corePath);
    const std::string &diagnostics = compiled.output;
    std::filesystem::remove(corePath);
    // A successful compile prints nothing, so what arrived here is
    // output a spawned process wrote after the previous input's
    // sentinel — it was dropped, losing a child's first printLine.
    if (compiled.ok()) std::cout << diagnostics;
    if (!compiled.ok()) {
      if (!diagnostics.empty()) std::cerr << diagnostics;
      loaded = false;
      break;
    }
    if (it != std::prev(results.rend()))
      newlyLoaded.emplace_back(it->moduleName, it->source);
  }
  for (auto &[name, core] : newlyLoaded)
    m_loadedCompanionCore[name] = std::move(core);
  return loaded;
}

auto BeamReplSession::evaluate(
    const std::string &source, const ReplInput &shape,
    const std::optional<ReplSessionWith> &pendingWith) -> ReplStep {
  const bool isFuncDef = shape.definition;
  const bool isLocalLet = shape.localBinding;
  const bool isMutableLet = shape.mutableBinding;
  const std::string &letVarName = shape.bindingName;
  const auto &patternLetNames = shape.patternNames;
  const std::vector<std::string> lineBoundNames = shape.boundNames();
  if (isFuncDef) {
    define(source);
    return ReplStep::Next;
  }
  // Expression or local let — compile and run on BEAM.
  // For local lets, the current binding is evaluated fresh and
  // stashed in the process dictionary so subsequent evals
  // retrieve it (avoiding re-evaluation of side-effectful
  // expressions like Ets.new).
  throwIfPartialPatternRebind(lineBoundNames, m_beamSemanticBinds);
  std::string semanticBindSource;
  for (const auto &[name, binding] : m_beamSemanticBinds)
    if (std::find(lineBoundNames.begin(), lineBoundNames.end(),
                   name) == lineBoundNames.end())
      semanticBindSource += "  " + binding + "\n";
  const auto withExcept =
      pendingWith ? pendingWith->capability : std::string();
  const auto withOpen = replWithOpen(m_sessionWiths, withExcept);
  const auto withClose = replWithClose(m_sessionWiths, withExcept);
  auto semanticSource = topDefsStr() + "main do\n" +
                        semanticBindSource + withOpen + "  " + source +
                        "\n" + withClose + "end\n";
  kex::Lexer semanticLexer(semanticSource, "<repl>");
  kex::Parser semanticParser(semanticLexer.tokenizeAll(), "<repl>");
  auto semanticProgram = semanticParser.parseProgram();
  expandReplProgram(semanticProgram);
  // Loaded declarations go BEFORE the replayed main block: the
  // checker registers a module's functions as it checks them, so a
  // module appended after the call site is registered too late and
  // `Loaded.values()` came back untyped — the walker REPL, which
  // prepends the same source, reported `[Integer]` for it while this
  // one fell back to the runtime `[Int]`.
  mergeLoadedForCheck(semanticProgram);
  // Installed stdlib modules already have a typed source interface.
  // Keep them external here: merging their source into the replay
  // would make the local module shell shadow that interface and erase
  // refined results such as FileHandle<CannotRead, CanWrite>.
  kex::semantic::Analyzer semanticAnalyzer(
      &preludeSemanticInterfaces());
  if (!semanticAnalyzer.analyze(semanticProgram)) {
    for (const auto &diagnostic : semanticAnalyzer.diagnostics())
      if (diagnostic.level ==
          kex::semantic::Diagnostic::Level::Error)
        throw std::runtime_error(diagnostic.message);
    throw std::runtime_error("semantic analysis failed");
  }
  if (pendingWith) {
    // The value has to be something `with` accepts for the
    // capability; checked here, before it is kept, so a bad one does
    // not break every input after it.
    const auto withCheck =
        topDefsStr() + "main do\n" + semanticBindSource + withOpen +
        "  " + source + "\n  with " + pendingWith->capability + " = " +
        pendingWith->binding + " do\n    0\n  end\n" + withClose +
        "end\n";
    kex::Lexer withLexer(withCheck, "<repl>");
    kex::Parser withParser(withLexer.tokenizeAll(), "<repl>");
    auto withProgram = withParser.parseProgram();
    expandReplProgram(withProgram);
    mergeLoadedForCheck(withProgram);
    kex::semantic::Analyzer withAnalyzer(&preludeSemanticInterfaces());
    if (!withAnalyzer.analyze(withProgram)) {
      for (const auto &diagnostic : withAnalyzer.diagnostics())
        if (diagnostic.level ==
            kex::semantic::Diagnostic::Level::Error)
          throw std::runtime_error(diagnostic.message);
      throw std::runtime_error("semantic analysis failed");
    }
  }
  std::optional<std::string> beamSemanticType;
  for (auto item = semanticProgram.items.rbegin();
       item != semanticProgram.items.rend(); ++item) {
    auto *main =
        std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&*item);
    if (!main || !*main || (*main)->body.empty())
      continue;
    const auto *last = replLastStatement(**main);
    const kex::ast::Expr *typedExpr = last;
    if (const auto *binding =
            std::get_if<kex::ast::LetExpr>(&last->kind);
        binding && binding->value)
      typedExpr = binding->value.get();
    else if (const auto *mutableBinding =
                 std::get_if<kex::ast::VarExpr>(&last->kind);
             mutableBinding && mutableBinding->value)
      typedExpr = mutableBinding->value.get();
    else if (const auto *assigned =
                 std::get_if<kex::ast::AssignExpr>(&last->kind);
             assigned && assigned->value)
      typedExpr = assigned->value.get();
    beamSemanticType = displayTypeOf(semanticAnalyzer, typedExpr);
    break;
  }
  const auto inspectCall = [&](const std::string &expression) {
    if (!beamSemanticType)
      return "Kex.Intrinsic.IO.inspectRepl(" + expression + ")";
    return "Kex.Intrinsic.IO.inspectTyped(" + expression + ", \"" +
           *beamSemanticType + "\")";
  };

  // Validate the input on its own before it is wrapped. Wrapping it as
  // `IO.inspect(<source>)` can turn a syntax error into a well-formed
  // argument list — `1,2` became `IO.inspect(1,2)`, which parses as a
  // two-argument call and only failed later on BEAM as an opaque
  // `undef`. Checking it inside a bare `main` is what the walker REPL
  // effectively does, so both report the same syntax error.
  {
    kex::Lexer checkLexer("main do\n" + source + "\nend\n");
    kex::Parser checkParser(checkLexer.tokenizeAll());
    checkParser.parseProgram();
    throwOnParseErrors(checkParser);
  }

  const auto putBack = [](const std::string &name) {
    return "  BEAM.erlang.put(:kexrepl" + name + ", " + name + ")\n";
  };
  // Any line can mutate a `var` from an earlier line — by reassigning
  // it or through a `!` method — so every tracked mutable name is
  // written back, not only the one this line binds.
  std::string mutablePuts;
  for (const auto &name : m_mutableBinds)
    mutablePuts += putBack(name);

  // `x = v` and `x.foo!(v)` are statements: lowering rejects a `!`
  // receiver that is not a plain binding, and neither node lowers as
  // an expression, so they cannot go inside `IO.inspect(...)`. Run the
  // line as a statement and inspect the variable afterwards.
  std::string mutatedName;
  if (!isLocalLet && !isFuncDef) {
    size_t i = 0;
    while (i < source.size() &&
           (std::isalnum((unsigned char)source[i]) || source[i] == '_'))
      i++;
    const auto head = source.substr(0, i);
    const auto headRest = source.find_first_not_of(" \t", i);
    // `.field` segments after the head: `p.x = v` writes through the
    // head binding, so the operator scan below starts past them.
    // Segments may sit apart (`p . x = 1` lexes the same).
    size_t seg = i;
    while (true) {
      const auto dot = source.find_first_not_of(" \t", seg);
      if (dot == std::string::npos || source[dot] != '.') break;
      auto k = source.find_first_not_of(" \t", dot + 1);
      if (k == std::string::npos ||
          !(std::isalnum((unsigned char)source[k]) ||
            source[k] == '_'))
        break;
      while (k < source.size() &&
             (std::isalnum((unsigned char)source[k]) ||
              source[k] == '_'))
        k++;
      seg = k;
    }
    const auto opPos = source.find_first_not_of(" \t", seg);
    // `n = v` reassigns; so do the compound forms (`n += v`, `n -=
    // v`, `n *= v`, `n /= v`, `n %= v`, `n &&= v`, `n ||= v` — the
    // same set parseAssignment desugars). A compound operator is one
    // token, so its `=` sits directly against it: `n * 2` stays an
    // expression, and `n == v` stays a comparison (kexhq/kex#604).
    const bool reassigns = [&] {
      if (opPos == std::string::npos) return false;
      if (source[opPos] == '=') {
        return opPos + 1 < source.size() &&
               source[opPos + 1] != '=';
      }
      const char op = source[opPos];
      if (op == '+' || op == '-' || op == '*' || op == '/' ||
          op == '%') {
        return opPos + 1 < source.size() &&
               source[opPos + 1] == '=';
      }
      if (op == '&' || op == '|') {
        return opPos + 2 < source.size() &&
               source[opPos + 1] == op && source[opPos + 2] == '=';
      }
      return false;
    }();
    const bool mutates = headRest != std::string::npos &&
                         source[headRest] == '.' &&
                         source.find('!', headRest) != std::string::npos;
    if (!head.empty() && isMutableBind(head) && (reassigns || mutates))
      mutatedName = head;
  }

  std::string kexSource;
  if (isLocalLet) {
    std::string puts;
    std::string shown = letVarName;
    std::string bindLine = "  " + source + "\n";
    if (!patternLetNames.empty()) {
      for (const auto &name : patternLetNames)
        puts += putBack(name);
      // Echo the whole MATCHED value, not the first name bound out of
      // it: the type comes from analysing the binding as a whole, so
      // showing `a` against it printed `1 : (Integer, Integer)`. The
      // right-hand side is bound to a temporary first so it is still
      // evaluated exactly once.
      constexpr const char *matched = "__replMatched";
      auto eq = replBindingEqualsPos(source, 0);
      if (eq != std::string::npos) {
        bindLine = "  let " + std::string(matched) + " =" +
                   source.substr(eq + 1) + "\n" +
                   "  " + source.substr(0, eq + 1) + " " + matched + "\n";
        shown = matched;
      } else {
        shown = patternLetNames.front();
      }
    } else {
      puts = putBack(letVarName);
    }
    // A `/with` value is not echoed: the command answers for itself.
    kexSource = topDefsStr() + "main do\n" + m_localBinds + withOpen +
                bindLine + puts + mutablePuts +
                (pendingWith ? "" : "  " + inspectCall(shown) + "\n") +
                withClose + "end\n";
  } else if (!mutatedName.empty()) {
    kexSource = topDefsStr() + "main do\n" + m_localBinds + withOpen +
                "  " + source + "\n" + mutablePuts +
                "  " + inspectCall(mutatedName) + "\n" + withClose +
                "end\n";
  } else {
    kexSource = topDefsStr() + "main do\n" + m_localBinds + withOpen +
                "  " + inspectCall(source) + "\n" + mutablePuts +
                withClose + "end\n";
  }

  kex::Lexer lexer(kexSource);
  auto tokens = lexer.tokenizeAll();
  kex::Parser parser(std::move(tokens));
  auto program = parser.parseProgram();
  throwOnParseErrors(parser);
  expandReplProgram(program);
  // Merge non-MainBlock items from files loaded via /load so their
  // definitions (function defs, make blocks, records, types) are
  // visible to every subsequent REPL input. Re-read on each session
  // build so the current on-disk version is always used.
  for (const auto& f : m_loadedBeamFiles) {
    try {
      for (auto &item : declarationsOf(f))
        program.items.push_back(std::move(item));
    } catch (...) {
      // Syntax errors in a loaded file were caught by /load; if the
      // file changed underneath us, just skip it this round.
    }
  }
  // Source-module imports in a REPL expression need the same
  // dependency expansion as `kex -R file.kex`, for both `using` and
  // qualified access such as `Units.SI.Meter`.
  resolveBeamDeps(program, m_replRoots, &preludeSemanticInterfaces(),
                  sourceModulesNamedBy(program));
  auto extMods = kex::ir::mergeExternalModules(
      preludeExternalModules(), m_kexiRegistry.buildExternalModules());
  kex::semantic::Analyzer replAnalyzer(&preludeSemanticInterfaces());
  replAnalyzer.setLinkedModules(providerModules(extMods));
  replAnalyzer.analyze(program);
  auto replRecordLayouts = loadPreludeRecordLayouts();
  auto replVariantTags = loadPreludeVariantTags();
  // All of the checker's answers, `Type.of` included: without them
  // `Type.of(someFunction)` fell back to the runtime, which lowered
  // the bare function name to an unbound Core Erlang variable and
  // failed to compile.
  const kex::hir::Program hir(program, replAnalyzer);
  auto irModules = kex::ir::lowerModules(
      program, "kex_repl_session_" + std::to_string(++m_sessionVersion),
      {.externals = extMods.nameToAtom.empty() ? nullptr : &extMods,
       .externalRecords = &replRecordLayouts,
       .externalVariants = &replVariantTags,
       .hir = &hir});
  auto results = kex::ir::emitModules(irModules);
  const auto& result = results.front();

  const bool loaded = loadModules(results);
  if (!loaded) {
    std::cerr << "  " << kex::color::apply(kex::color::red)
              << "error:" << kex::color::apply(kex::color::reset)
              << " failed to load session module\n";
    if (g_replLineFromScript) {
      g_replScriptFailed = true;
      return ReplStep::Stop;
    }
    return ReplStep::Next;
  }
  auto evaluated = vmRequest(m_vm, "eval", result.moduleName);
  const std::string &output = evaluated.output;
  if (evaluated.ok()) {
    std::cout << output;
  } else {
    std::cerr << "  " << kex::color::apply(kex::color::red)
              << "error:" << kex::color::apply(kex::color::reset) << " "
              << output;
    // A boot line that fails at run time must not leave a
    // half-booted session behind its prompt (kexhq/kex#549).
    if (g_replLineFromScript) {
      g_replScriptFailed = true;
      return ReplStep::Stop;
    }
  }

  if (isLocalLet && evaluated.ok()) {
    const auto equals = source.find('=');
    const auto rhs = equals == std::string::npos
        ? std::string::npos
        : source.find_first_not_of(" \t", equals + 1);
    // A lambda literal is replayed from its source rather than read
    // back from the process dictionary. This used to be what kept it
    // from turning into a badfun when the session module reloaded;
    // each input now has a session module of its own, so a stored
    // fun stays valid either way.
    const bool isLambda = rhs != std::string::npos &&
        (source[rhs] == '&' ||
         (source[rhs] == '{' &&
          source.find_first_not_of(" \t", rhs + 1) != std::string::npos &&
          source[source.find_first_not_of(" \t", rhs + 1)] == '|'));
    // A destructuring let binds through its pattern, so every name it
    // introduced has to be replayed, not just a single variable —
    // otherwise `let Just(x) = ...` stored x but later inputs could
    // not see it.
    const std::vector<std::string> &replayNames = lineBoundNames;
    if (isLambda)
      m_localBinds += "  " + source + "\n";
    else
      for (const auto &name : replayNames) {
        // The replayed value comes back from the process dictionary
        // untyped, so lowering could not see what `pa` holds and
        // dispatched `pa.get(k)` dynamically by name — where a slot
        // called `get` lost to every other `get` ("Undefined method:
        // get for Server") while the checker, which replays the
        // original source, had accepted it. Carrying the type the
        // binding was checked at restores the static dispatch a file
        // gets. Only a type that checks as an annotation is carried;
        // anything else (type variables, Unknown) replays untyped.
        std::string annotation;
        if (patternLetNames.empty() && beamSemanticType &&
            replTypeAnnotates(topDefsStr(), *beamSemanticType))
          annotation = " : " + *beamSemanticType;
        // A `var` replays as `var`: replaying it as `let` would make
        // every later line see an immutable binding and reject both
        // `name = v` and `name.foo!(v)`.
        m_localBinds += std::string("  ") +
                      (isMutableLet ? "var " : "let ") + name +
                      annotation +
                      " = BEAM.erlang.get(:kexrepl" + name + ")\n";
        if (isMutableLet && !isMutableBind(name))
          m_mutableBinds.push_back(name);
      }
    m_beamSemanticBinds.erase(
        std::remove_if(
            m_beamSemanticBinds.begin(), m_beamSemanticBinds.end(),
            [&](const auto &binding) {
              return std::find(replayNames.begin(), replayNames.end(),
                               binding.first) != replayNames.end();
            }),
        m_beamSemanticBinds.end());
    m_beamSemanticBinds.emplace_back(replayNames.front(), source);
    if (pendingWith) {
      std::erase_if(m_sessionWiths, [&](const auto &with) {
        return with.capability == pendingWith->capability;
      });
      m_sessionWiths.push_back(*pendingWith);
      std::cout << "  (with " << pendingWith->capability << ")\n";
    }
  }
  return ReplStep::Next;
}

auto BeamReplSession::run() -> int {
  // ── Writable temp dir for per-eval user beams ─────────────────────
  char rtmpl[] = "/tmp/kex_irepl_XXXXXX";
  char *rtd = mkdtemp(rtmpl);
  if (!rtd) {
    std::cerr << "error: mkdtemp failed\n";
    return 1;
  }
  m_beamDir = rtd;

  // Runtime artifacts are produced explicitly by the toolchain build.
  m_rtPaDir = prebuiltRuntimeBeamDir();
  if (m_rtPaDir.empty()) {
    std::cerr << "error: prebuilt runtime artifacts are missing; "
                 "rebuild or reinstall the Kex toolchain\n";
    std::filesystem::remove_all(m_beamDir);
    return 1;
  }

  // Spawn ONE persistent erl VM for the whole session — driven by
  // runtime/src/kex_repl_driver.erl over stdin/stdout. This is what
  // makes spawned processes / registered names survive across REPL
  // inputs and turns session recompiles into hot-loads instead of
  // cold re-runs.
  {
    std::vector<std::string> erlArgs = {erlExecutable(), "-noshell", "-pa",
                                        m_beamDir};
    erlArgs.insert(erlArgs.end(), cliDistributionArgs.begin(),
                   cliDistributionArgs.end());
    if (!m_rtPaDir.empty()) {
      erlArgs.push_back("-pa");
      erlArgs.push_back(m_rtPaDir);
    }
    erlArgs.push_back("-eval");
    // allow_mocks/0 before the loop, in the same process the driver
    // evaluates input in — the BEAM half of "a REPL may mock" (issue #144).
    // The flag lives in the process dictionary, so this grants it for the
    // session and for nothing else.
    erlArgs.push_back(beamOtpFloorGuard() +
                      "kex_test:allow_mocks(), kex_repl_driver:loop()");
    if (!m_vm.start(erlArgs)) {
      std::cerr << "error: could not start erl VM for BEAM REPL\n";
      std::filesystem::remove_all(m_beamDir);
      return 1;
    }
  }

  // readline history — same file as the tree-walker REPL, so a session
  // in one is recalled in the other.
#ifdef HAS_READLINE
  const std::string historyFile = loadReplHistory();
#endif

  {
    auto info = vmRequest(m_vm, "info");
    std::string otpInfo = std::move(info.output);
    // The VM died before answering the first handshake — an OTP too old for
    // the bundled beams (the floor guard has already said so on stderr), a
    // missing runtime module, a broken erl. Without this the REPL prints
    // its banner over the corpse and answers every input with "failed to
    // load session module".
    if (info.status == "eof") {
      m_vm.close();
      std::cerr
          << "error: the BEAM runtime did not start; cannot run the REPL\n";
      std::filesystem::remove_all(m_beamDir);
      return 1;
    }
    while (!otpInfo.empty() && otpInfo.back() == '\n')
      otpInfo.pop_back();
    if (!otpInfo.empty())
      std::cout << kex::color::apply(kex::color::gray) << otpInfo
                << kex::color::apply(kex::color::reset) << "\n";
  }
  kex::printReplBanner(std::cout, "BEAM");

  if (!m_skipPrelude) {
    const auto preludeDisplay = preludeDisplayRegistration();
    if (!preludeDisplay.empty()) vmRequest(m_vm, "exec", preludeDisplay);
  }

  if (!m_skipPrelude) m_completionBase.prelude = preludeCompletionSymbols();
  g_replCompletionBase = &m_completionBase;
  installReplLineEditing();

  // Where a module's `using`s are found: the session's source roots
  // (`--source-root`, then lib/ and src/ under the working directory), as a
  // build of a file there would search them. A `/load`-ed module used to
  // bring in only its own declarations, so `Store`'s `using Wire` left
  // `Statement` unknown, and every later input failed with that error.
  // A dependency that cannot be loaded is a refused `/load` or a failed
  // input here, not the end of the session.
  g_dependencyFailureThrows = true;
  m_replRoots = moduleRootsFor(
      (std::filesystem::current_path() / "repl.kex").string());
  m_replProjectRoots = kex::withoutStandardLibraryRoots(m_replRoots);

  g_replCompletionContext = [&]() {
    ReplCompletionContext context{topDefsStr(), {}};
    for (const auto &[name, binding] : m_beamSemanticBinds)
      context.bindings += "  " + binding + "\n";
    return context;
  };



  while (true) {
    auto [read, ok] = readLine("kex(beam)> ");
    if (!ok)
      break;
    std::string input = replTrimLeadingIndent(std::move(read));
    if (input.empty())
      continue;
    if (kex::isReplExit(input))
      break;
    const auto command = kex::parseReplCommand(input);
    using Command = kex::ReplCommandKind;
    if (command.kind == Command::Help) {
      kex::printReplHelp(std::cout);
      continue;
    }
    if (command.kind == Command::Clear) {
      std::cout << kex::replClearScreenSequence() << std::flush;
      continue;
    }
    if (command.kind == Command::Reset) {
      m_topDefs.clear();
      m_localBinds.clear();
      m_mutableBinds.clear();
      m_beamSemanticBinds.clear();
      m_sessionWiths.clear();
      std::cout << "  (bindings cleared)\n";
      continue;
    }
    std::optional<ReplSessionWith> pendingWith;
    if (command.kind == Command::With || command.kind == Command::Without) {
      try {
        input = handleReplWithCommand(input, m_sessionWiths, pendingWith);
      } catch (const std::exception &e) {
        std::cerr << "  " << kex::color::apply(kex::color::red)
                  << "error:" << kex::color::apply(kex::color::reset) << " "
                  << e.what() << "\n";
        if (g_replLineFromScript) {
          g_replScriptFailed = true;
          break;
        }
        continue;
      }
      if (input.empty()) continue;
    }
    if (command.kind == Command::Set || command.kind == Command::Unset) {
      std::cerr << "  not yet available in the BEAM REPL\n";
      continue;
    }
    if (command.kind == Command::Complete) {
      printReplCompletions(command.argument);
      continue;
    }
    if (command.kind == Command::Load) {
      if (load(command.argument) == ReplStep::Stop) break;
      continue;
    }
    if (command.kind == Command::Unload) {
      std::string modName = command.argument;

      std::string entryAtom;
      if (m_kexiRegistry.isLoaded(modName))
        entryAtom = modName;
      else
        entryAtom = m_kexiRegistry.findEntryByShortName(modName);

      if (entryAtom.empty()) {
        std::cerr << "  /unload: module '" << modName << "' is not loaded\n";
        continue;
      }

      auto* unit = m_kexiRegistry.getUnit(entryAtom);
      if (unit) {
        for (const auto& mod : unit->modules) {
          vmRequest(m_vm, "exec", "code:purge('" + mod.beamAtom +
                                    "'), code:delete('" + mod.beamAtom + "')");
        }
      }

      m_completionBase.loaded.erase("<kexi:" + entryAtom + ">");
      m_kexiRegistry.unloadUnit(entryAtom);
      std::cout << "  unloaded " << modName << "\n";
      continue;
    }
    if (command.kind == Command::Reload) {
      // Loaded files are re-read on every session build — no explicit
      // reload needed. /reset clears m_topDefs + m_localBinds but keeps
      // loaded paths; use /reset then /load each file again to refresh.
      std::cout << "  (loaded files are re-evaluated on each input)\n";
      continue;
    }

    // Accumulate do/end blocks, then chain consecutive clauses of the
    // same function so `let f(0) = ...` / `let f(n) = ...` combine,
    // and a bare `let f(` header keeps reading instead of being
    // accepted as a broken standalone clause. Matches the tree-walker.
    std::string source = input;
    int dc = replBlockDepth(source);
    bool cancelled = false;
    while (dc > 0 || replHasOpenDelimiter(source)) {
      g_replPendingInput = source;
      auto [cont, contOk] = readLine("  ...> ");
      if (takeReplInputCancelled()) {
        cancelled = true;
        break;
      }
      if (!contOk)
        break;
      source += "\n" + cont;
      dc = replBlockDepth(source);
    }
    g_replPendingInput.clear();
    if (cancelled) {
      std::cout << kex::color::apply(kex::color::gray) << "   (cancelled)"
                << kex::color::apply(kex::color::reset) << "\n";
      continue;
    }
    // Clauses of one function are NOT gathered by reading ahead here. That
    // is what made a complete line hang on `...>`: after `let fact(0) = 1`
    // the REPL blocked waiting to see whether the next line was another
    // clause, with nothing on screen to say so. Each line is now accepted
    // on its own and clauses accumulate at definition time instead.

    // Classify: function def vs local let vs expression

    try {
      if (evaluate(source, classifyReplInput(source), pendingWith) ==
          ReplStep::Stop)
        break;
    } catch (const std::exception &e) {
      std::cerr << "  " << kex::color::apply(kex::color::red)
                << "error:" << kex::color::apply(kex::color::reset) << " "
                << e.what() << "\n";
      if (g_replLineFromScript) {
        g_replScriptFailed = true;
        break;
      }
    }
  }

  saveReplHistory(historyFile);

  m_vm.close();
  std::filesystem::remove_all(m_beamDir);
  return g_replScriptFailed ? 1 : 0;
}

// The tree-walking REPL (`kex` with no file, or a non-BEAM mode). One
// session: the evaluator every input runs in, and what has been defined.
class WalkerReplSession {
public:
  explicit WalkerReplSession(bool skipPrelude) : m_skipPrelude(skipPrelude) {}
  auto run() -> int;

private:
  bool m_skipPrelude;
  // Completion's index (prelude loaded once, updated on each input) and the
  // one inputs are checked in.
  ReplCompletionBase m_completionBase;
  // Made in `run`, after the banner: making one loads the prelude.
  std::optional<kex::interpreter::Evaluator> m_evaluator;
  // Accumulated source of all top-level definitions typed in the REPL so
  // far, used to keep the SemanticDB index complete across multiple inputs.
  std::string m_replAccumSource;
  // Local bindings are evaluated one input at a time, but their source is
  // retained here so semantic analysis can infer the type of later inputs.
  std::vector<std::pair<std::string, std::string>> m_replBindings;
  std::vector<ReplSessionWith> m_sessionWiths;
  // Keep parsed programs alive so function closures can reference AST nodes
  std::vector<kex::ast::Program *> m_replPrograms;
  // Every input's checked program, kept for the session: a later input
  // calls an earlier one's definitions, and the evaluator reads them all.
  std::vector<kex::hir::CheckedInput> m_checkedInputs;

  // The program an input is checked in: the session's definitions, then a
  // `main` replaying `bindingSource` (the earlier bindings).
  auto sessionProgram(const std::string &bindingSource)
      -> std::unique_ptr<kex::ast::Program> {
    kex::Lexer lexer(m_replAccumSource + "main do\n" + bindingSource + "end\n",
                     "<repl>");
    kex::Parser parser(lexer.tokenizeAll(), "<repl>");
    auto session = std::make_unique<kex::ast::Program>(parser.parseProgram());
    expandReplProgram(*session);
    return session;
  }
  // What the session reads from disk, merged in before the check.
  auto prepareSession(kex::ast::Program &semantic) -> void {
    prependDeclarationsOf(semantic, m_replLoadedDepFiles);
    // A module the session names (`using Clocks`, `Clocks.stamp()`) is
    // checked from its source, as the BEAM REPL does. Without it the checker
    // knew the module's functions only from what the runtime had loaded, and
    // none of its capabilities: `with Clocks.Ticker = ...` was rejected as
    // "not a capability".
    resolveBeamDeps(semantic, m_walkerProjectRoots, &preludeSemanticInterfaces(),
                    sourceModulesNamedBy(semantic));
  }
  // Keeps `checked` for the session and has the evaluator read it.
  auto keepChecked(kex::hir::CheckedInput checked) -> void {
    if (!checked.program) return;
    m_evaluator->addProgram(checked.program.get());
    m_checkedInputs.push_back(std::move(checked));
  }
  std::vector<LoadedDep> m_replLoadedDeps;
  // Their files, which the input check parses in as declarations: a file
  // with a `module Wire` header cannot be glued onto the session source.
  std::vector<std::string> m_replLoadedDepFiles;
  // Where a `/load`-ed file's `using`s are found: the project's roots.
  std::vector<std::string> m_walkerProjectRoots;
  // `/set` and `/unset` options.
  bool m_showTypes = true;
  bool m_showAst = false;
  bool m_showTokens = false;

  auto handleSet(const std::string &arg, bool enable) -> void {
    if (arg == "types") {
      m_showTypes = enable;
      std::cout << "  types: " << (enable ? "on" : "off") << "\n";
    } else if (arg == "ast") {
      m_showAst = enable;
      std::cout << "  ast: " << (enable ? "on" : "off") << "\n";
    } else if (arg == "tokens") {
      m_showTokens = enable;
      std::cout << "  tokens: " << (enable ? "on" : "off") << "\n";
    } else {
      std::cerr << "  Unknown option: " << arg << "\n";
      std::cerr << "  Available: types, ast, tokens\n";
    }
  }
  auto execProgram(kex::ast::Program *program, bool expanded = false)
      -> kex::interpreter::ValuePtr {
    if (!expanded) expandReplProgram(*program);
    if (m_showAst)
      printAst(*program);
    return m_evaluator->execute(*program);
  }
  auto showResult(const kex::interpreter::ValuePtr &result,
                  const std::optional<std::string> &semanticType = std::nullopt)
      -> void {
    if (result && !std::holds_alternative<kex::interpreter::UnitValue>(
                      result->data)) {
      std::cout << kex::color::apply(kex::color::gray) << "=> "
                << kex::color::apply(kex::color::reset)
                << result->inspect();
      if (m_showTypes) {
        std::cout << " " << kex::color::apply(kex::color::gray) << ":"
                  << kex::color::apply(kex::color::reset) << " "
                  << kex::color::apply(kex::color::cyan)
                  << (semanticType ? *semanticType : result->typeName())
                  << kex::color::apply(kex::color::reset);
      }
      std::cout << "\n";
    }
  }
  // `/load file`: runs a `.kex` file's declarations into the session once it
  // checks with it.
  auto load(const std::string &filePath) -> ReplStep;
  // Evaluates one complete input; `implicitDo` when it is a `make X` whose
  // `do` the user left off, `pendingWith` a `/with` waiting for its value.
  auto evaluate(std::string source, bool implicitDo,
                const std::optional<ReplSessionWith> &pendingWith) -> void;
};

auto runWalkerRepl(bool skipPrelude) -> int {
  WalkerReplSession session(skipPrelude);
  return session.run();
}

auto WalkerReplSession::load(const std::string &filePath) -> ReplStep {
  if (!fileExists(filePath)) {
    std::cerr << "  /load: file not found: " << filePath << "\n";
    return ReplStep::Next;
  }
  try {
    auto src = readFile(filePath);
    kex::Lexer lex(std::move(src), filePath);
    kex::Parser parser(lex.tokenizeAll(), filePath);
    // Loaded definitions retain pointers into their parsed AST (function
    // bodies, make clauses, module members). Keep that Program alive for
    // the rest of the REPL session, just like definitions entered at the
    // prompt; a stack-local Program leaves those pointers dangling as
    // soon as /load returns.
    auto prog = std::make_unique<kex::ast::Program>(parser.parseProgram());
    throwOnParseErrors(parser);
    auto deps = resolveBeamDeps(*prog, m_walkerProjectRoots,
                                &preludeSemanticInterfaces());
    // Checked with the session before anything runs, as the definitions
    // that run (hir::checkReplInput): a file that does not check is refused,
    // rather than kept in the session source that every later input is
    // checked against, each failing with its error.
    expandReplProgram(*prog);
    {
      std::string bindingSource;
      for (const auto &[_, binding] : m_replBindings) bindingSource += binding + "\n";
      auto checked = kex::hir::checkReplInput(
          sessionProgram(bindingSource), *prog, {},
          [this](kex::ast::Program &semantic) { prepareSession(semantic); },
          &preludeSemanticInterfaces());
      if (!checked.program) {
        std::cerr << "  /load: " << filePath << " was not loaded: "
                  << checked.error.value_or("semantic analysis failed") << "\n";
        return ReplStep::Next;
      }
      keepChecked(std::move(checked));
    }
    m_replPrograms.push_back(prog.release());
    m_evaluator->execute(*m_replPrograms.back());
    for (auto &dep : deps) {
      if (dep.path &&
          std::find(m_replLoadedDepFiles.begin(), m_replLoadedDepFiles.end(),
                    *dep.path) == m_replLoadedDepFiles.end())
        m_replLoadedDepFiles.push_back(*dep.path);
      m_replLoadedDeps.push_back(std::move(dep));
    }
    m_replAccumSource += readFile(filePath) + "\n";
    std::cout << "  loaded " << filePath << "\n";
  } catch (const std::exception& e) {
    std::cerr << "  /load: " << filePath << " was not loaded: " << e.what() << "\n";
  }
  return ReplStep::Next;
}

auto WalkerReplSession::evaluate(
    std::string source, bool implicitDo,
    const std::optional<ReplSessionWith> &pendingWith) -> void {
  const bool isFuncDef = classifyReplInput(source).definition;
  if (isFuncDef) {
    // If the user wrote `make TypeName` without `do`, insert it
    // before parsing so the grammar's `expect(Do)` is satisfied.
    if (implicitDo) {
      auto nl = source.find('\n');
      if (nl != std::string::npos)
        source.insert(nl, " do");
      else
        source += " do";
    }
    // Parse as top-level definition
    kex::Lexer lexer(source);
    auto tokens = lexer.tokenizeAll();
    kex::Parser parser(std::move(tokens));
    auto *program = new kex::ast::Program(parser.parseProgram());
    throwOnParseErrors(parser);
    m_replPrograms.push_back(program);
    // Checked as the definitions that run, so a later call into them reads
    // the checker's decisions. Definitions were never refused here, so one
    // the check rejects still runs, on the values alone.
    expandReplProgram(*program);
    try {
      std::string bindingSource;
      for (const auto &[_, binding] : m_replBindings) bindingSource += binding + "\n";
      keepChecked(kex::hir::checkReplInput(
          sessionProgram(bindingSource), *program, {},
          [this](kex::ast::Program &semantic) { prepareSession(semantic); },
          &preludeSemanticInterfaces()));
    } catch (...) {
      // A session history that does not re-parse: run on the values.
    }
    execProgram(program, true);
    // Accumulated, so every prior definition reaches tab completion's
    // check, not just the latest one.
    m_replAccumSource += source + "\n";
    const bool isImport = source.rfind("using ", 0) == 0;
    std::cout << kex::color::apply(kex::color::gray) << "=> "
              << kex::color::apply(kex::color::reset)
              << (isImport ? "using " : "defined ")
              << replDefinitionName(source) << "\n";
  } else {
    // Wrap in main for expression evaluation
    const auto withExcept =
        pendingWith ? pendingWith->capability : std::string();
    const auto withOpen = replWithOpen(m_sessionWiths, withExcept);
    const auto withClose = replWithClose(m_sessionWiths, withExcept);
    auto wrapped =
        "main do\n" + withOpen + source + "\n" + withClose + "end\n";
    kex::Lexer lexer(wrapped);
    auto tokens = lexer.tokenizeAll();
    kex::Parser parser(std::move(tokens));
    auto *program = new kex::ast::Program(parser.parseProgram());
    throwOnParseErrors(parser);
    m_replPrograms.push_back(program);

    // A REPL `let` may intentionally shadow a previous binding. Keep
    // its name so the semantic replay can replace, rather than
    // redeclare, the old source.
    std::optional<std::string> bindingName;
    std::vector<std::string> boundNames;
    for (const auto &item : program->items) {
      const auto *main =
          std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&item);
      if (!main || !*main || (*main)->body.empty())
        continue;
      const auto &lastKind = replLastStatement(**main)->kind;
      // `var x = v` binds just as much as `let` does. The runtime value
      // lives in the m_evaluator's persistent REPL env either way; what
      // this record drives is the semantic replay below, and without it
      // the NEXT line's analysis reports `x` undefined.
      if (const auto *mutableBinding =
              std::get_if<kex::ast::VarExpr>(&lastKind)) {
        if (mutableBinding->pattern) {
          std::vector<std::string> names;
          collectPatternNames(*mutableBinding->pattern, names);
          if (!names.empty()) {
            bindingName = names.front();
            boundNames = std::move(names);
          }
        }
        continue;
      }
      const auto *binding = std::get_if<kex::ast::LetExpr>(&lastKind);
      if (!binding || !binding->pattern)
        continue;
      std::vector<std::string> names;
      collectPatternNames(*binding->pattern, names);
      if (!names.empty()) {
        bindingName = names.front();
        boundNames = std::move(names);
      }
    }

    // Runtime values intentionally erase phantom typestate parameters.
    // Analyze a parallel source containing prior REPL bindings so the
    // displayed type is the source-level type rather than FileHandle.
    std::optional<std::string> semanticType;
    std::optional<std::string> semanticError;
    // Before the try: its catch-all swallows exceptions so a stale
    // semantic history cannot break evaluation — this error must
    // surface instead.
    throwIfPartialPatternRebind(boundNames, m_replBindings);
    // The input is checked as the very statements that run: they are lent
    // to the checked program (the session's definitions and bindings) and
    // returned, so the checked program the evaluator reads (`hir`) answers
    // for the nodes it evaluates, as for a file.
    expandReplProgram(*program);
    kex::ast::MainBlock *inputMain = nullptr;
    for (auto &item : program->items)
      if (auto *main = std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&item);
          main && *main)
        inputMain = main->get();
    // Its last statement (inside any `with` wrappers), found before the
    // statements are lent: the node, not its slot, is what is typed.
    const kex::ast::Expr *inputLast =
        inputMain && !inputMain->body.empty() ? replLastStatement(*inputMain)
                                              : nullptr;
    // The input checked as the statements that run (hir::checkReplInput).
    std::optional<kex::hir::CheckedInput> checked;
    try {
      std::string bindingSource;
      for (const auto &[name, binding] : m_replBindings)
        if (!bindingName ||
            std::find(boundNames.begin(), boundNames.end(), name) ==
                boundNames.end())
          bindingSource += binding + "\n";
      auto session = sessionProgram(bindingSource);
      // A `/with` value is checked against its capability after the input;
      // its type is not shown.
      std::vector<kex::ast::ExprPtr> after;
      if (pendingWith) {
        kex::Lexer withLexer("main do\nwith " + pendingWith->capability +
                                 " = " + pendingWith->binding +
                                 " do\n0\nend\nend\n",
                             "<repl>");
        kex::Parser withParser(withLexer.tokenizeAll(), "<repl>");
        auto withProgram = withParser.parseProgram();
        for (auto &item : withProgram.items)
          if (auto *main = std::get_if<std::unique_ptr<kex::ast::MainBlock>>(&item);
              main && *main)
            for (auto &statement : (*main)->body)
              after.push_back(std::move(statement));
      }
      checked = kex::hir::checkReplInput(
          std::move(session), *program, std::move(after),
          [this](kex::ast::Program &semantic) { prepareSession(semantic); },
          &preludeSemanticInterfaces());
      semanticError = checked->error;
      // The input's last statement, lent with the rest: the same node.
      if (const auto *last = inputLast; last && checked->program) {
        const kex::ast::Expr *typedExpr = last;
        if (const auto *binding = std::get_if<kex::ast::LetExpr>(&last->kind);
            binding && binding->value)
          typedExpr = binding->value.get();
        else if (const auto *mutableBinding =
                     std::get_if<kex::ast::VarExpr>(&last->kind);
                 mutableBinding && mutableBinding->value)
          typedExpr = mutableBinding->value.get();
        else if (const auto *assigned =
                     std::get_if<kex::ast::AssignExpr>(&last->kind);
                 assigned && assigned->value)
          typedExpr = assigned->value.get();
        semanticType = displayTypeOf(*checked->analysis, typedExpr);
      }
    } catch (...) {
      // Evaluation still works if an incomplete semantic history (for
      // example a loaded file with a main block) cannot be re-parsed.
    }

    if (semanticError)
      throw std::runtime_error(*semanticError);

    if (checked) keepChecked(std::move(*checked));
    auto result = execProgram(program, true);
    if (pendingWith) {
      std::erase_if(m_sessionWiths, [&](const auto &with) {
        return with.capability == pendingWith->capability;
      });
      m_sessionWiths.push_back(*pendingWith);
      std::cout << "  (with " << pendingWith->capability << ")\n";
    } else {
      showResult(result, semanticType);
    }
    if (bindingName) {
      m_replBindings.erase(
          std::remove_if(
              m_replBindings.begin(), m_replBindings.end(),
              [&](const auto &binding) {
                return std::find(boundNames.begin(), boundNames.end(),
                                 binding.first) != boundNames.end();
              }),
          m_replBindings.end());
      m_replBindings.emplace_back(*bindingName, source);
    }
  }
}

auto WalkerReplSession::run() -> int {
  kex::printReplBanner(std::cout, "");

#ifdef HAS_READLINE
  const std::string historyFile = loadReplHistory();
#endif

  // Completion's names: the prelude's, read once.
  if (!m_skipPrelude) m_completionBase.prelude = preludeCompletionSymbols();
  g_replCompletionBase = &m_completionBase;
  installReplLineEditing();

  m_evaluator.emplace(&preludeSemanticInterfaces());
  m_evaluator->setReplMode(true);
  // A REPL session is the user experimenting with their own machine, and
  // there is no other code present for a mock to deceive — the reason
  // Mock.* is gated elsewhere (issue #144) does not exist here.
  m_evaluator->setMocksAllowed(true);
  // Without this the m_evaluator keeps its default relative {"lib", "src"}
  // roots, so `using Regex` (or any opt-in stdlib module) failed with
  // "Unknown module" in the interpreter REPL while working both in scripts
  // and in the BEAM REPL. There is no file being run, so resolution is
  // anchored at the working directory plus the standard library roots.
  m_evaluator->setModuleRoots(
      moduleRootsFor((std::filesystem::current_path() / "<repl>").string()));
  std::string line;
  g_replCompletionContext = [&]() {
    ReplCompletionContext context{m_replAccumSource, {}};
    for (const auto &[name, binding] : m_replBindings)
      context.bindings += binding + "\n";
    return context;
  };

  // The project modules a `/load`-ed file uses, found as a build of a file
  // in the working directory would find them, and kept alive for the same
  // reason. Without them `Store`'s `using Wire` left `Statement` unknown.
  g_dependencyFailureThrows = true; // as in the BEAM REPL
  m_walkerProjectRoots = kex::withoutStandardLibraryRoots(
      moduleRootsFor((std::filesystem::current_path() / "repl.kex").string()));


  static const std::string setOptionsHelp =
      "  Options for /set / /unset:\n"
      "    types         Show type of each result\n"
      "    ast           Show AST for each input\n"
      "    tokens        Show token stream for each input\n";
  while (true) {
    auto [read, ok] = readLine("kex> ");
    if (!ok)
      break;
    std::string input = replTrimLeadingIndent(std::move(read));
    line = input;
    if (kex::isReplExit(line))
      break;
    const auto command = kex::parseReplCommand(line);
    using Command = kex::ReplCommandKind;
    if (line.empty())
      continue;

    // REPL commands
    if (command.kind == Command::Help) {
      kex::printReplHelp(std::cout, setOptionsHelp);
      continue;
    }
    if (command.kind == Command::Clear) {
      std::cout << kex::replClearScreenSequence() << std::flush;
      continue;
    }
    if (command.kind == Command::Set && command.argument.empty()) {
      std::cout << "  types:  " << (m_showTypes ? "on" : "off") << "\n"
                << "  ast:    " << (m_showAst ? "on" : "off") << "\n"
                << "  tokens: " << (m_showTokens ? "on" : "off") << "\n";
      continue;
    }
    if (command.kind == Command::Set) {
      handleSet(command.argument, true);
      continue;
    }
    if (command.kind == Command::Unset) {
      handleSet(command.argument, false);
      continue;
    }
    if (command.kind == Command::Complete) {
      printReplCompletions(command.argument);
      continue;
    }
    std::optional<ReplSessionWith> pendingWith;
    if (command.kind == Command::With || command.kind == Command::Without) {
      try {
        line = handleReplWithCommand(line, m_sessionWiths, pendingWith);
      } catch (const std::exception &e) {
        std::cerr << "  " << kex::color::apply(kex::color::red)
                  << "error:" << kex::color::apply(kex::color::reset) << " "
                  << e.what() << "\n";
        if (g_replLineFromScript) {
          g_replScriptFailed = true;
          break;
        }
        continue;
      }
      if (line.empty()) continue;
    }
    if (command.kind == Command::Load) {
      if (load(command.argument) == ReplStep::Stop) break;
      continue;
    }
    if (command.kind == Command::Reload) {
      std::cerr << "  /reload not yet implemented (use /reset then /load to rebuild)\n";
      continue;
    }

    // Multi-line: accumulate if there are unmatched do/end blocks
    std::string source = line;

    // Returns true if `s` starts with `make <TypeName>` but has no
    // `do` keyword on the same line (implicit-do block opener).
    auto isMakeWithoutDo = [](const std::string &s) -> bool {
      if (s.rfind("make ", 0) != 0)
        return false;
      // Check that the rest is a type name (no explicit `do`)
      auto doPos = s.find(" do");
      return doPos == std::string::npos;
    };


    // `make TypeName` (no explicit `do`) implicitly opens a block.
    bool implicitDo = isMakeWithoutDo(source);
    int doCount = implicitDo ? 1 : replBlockDepth(source);

    bool cancelled = false;
    while (doCount > 0 || replHasOpenDelimiter(source)) {
      g_replPendingInput = source;
      auto [contLine, contOk] = readLine("...> ");
      if (takeReplInputCancelled()) {
        cancelled = true;
        break;
      }
      if (!contOk)
        break;
      line = contLine;
      source += "\n" + line;
      doCount = implicitDo ? 1 + replBlockDepth(source) : replBlockDepth(source);
    }
    g_replPendingInput.clear();
    if (cancelled) {
      std::cout << kex::color::apply(kex::color::gray) << "   (cancelled)"
                << kex::color::apply(kex::color::reset) << "\n";
      continue;
    }
    // Clauses are not gathered by reading ahead — see the BEAM REPL above.
    // A complete line is accepted as soon as it is entered, and clauses of
    // one function accumulate where the definition is stored.

    // Show tokens if enabled
    if (m_showTokens) {
      kex::Lexer debugLexer(source);
      auto debugTokens = debugLexer.tokenizeAll();
      std::cout << "  tokens: ";
      for (const auto &t : debugTokens) {
        if (t.type == kex::TokenType::Eof ||
            t.type == kex::TokenType::Newline)
          continue;
        std::cout << kex::tokenTypeName(t.type);
        if (!t.value.empty())
          std::cout << "[" << t.value << "]";
        std::cout << " ";
      }
      std::cout << "\n";
    }
    try {
      evaluate(std::move(source), implicitDo, pendingWith);
    } catch (const std::exception &e) {
      std::cerr << "  " << kex::color::apply(kex::color::red)
                << "error:" << kex::color::apply(kex::color::reset) << " "
                << e.what() << "\n";
      if (g_replLineFromScript) {
        g_replScriptFailed = true;
        break;
      }
    }
  }
  saveReplHistory(historyFile);
  return g_replScriptFailed ? 1 : 0;
}
