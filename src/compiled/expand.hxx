#pragma once

#include "../ast/ast.hxx"
#include "../semantic/analyzer.hxx"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

// Compile-time expansion of `compiled do ... end` blocks.
//
// This runs as an AST->AST pass BEFORE semantic analysis: it replaces
// CompiledBlock nodes with the ordinary declarations they generate, so
// everything downstream — SemanticDB, the type checker, the tree-walk
// interpreter, IR lowering, the Core Erlang emitter, --summary — sees only
// plain FunctionDef/TypeDef/RecordDef/MakeDef and needs no changes. It is also
// what keeps the interpreter and the BEAM backend from drifting: neither knows
// the feature exists.
//
// Deliberately NOT under src/semantic/: this pass MUTATES the AST, which the
// semantic passes are careful never to do.
//
// Currently an identity pass — `compiled` blocks are still unwrapped verbatim
// by both backends, exactly as before. See docs/compiled-meta-plan.md for the
// staged plan; this is the plumbing step that lets the real work land behind a
// stable interface.
namespace kex::compiled {

// What became of one expression chain collapse tried to fold away.
//
// The feature's entire value is invisible at runtime — a program that fails to
// collapse still prints the right answer — so without a report the only way to
// know is to decode the emitted Core Erlang. This is that report.
struct CollapseNote {
    SourceLocation location;
    bool collapsed = false;
    // Why not, when it did not: "the value of `userId` is not known at compile
    // time, and converting it to text needs it". Empty when it collapsed.
    std::string reason;
};

struct ExpandOptions {
    // Wall-clock budget for all compile-time evaluation in one program. The
    // backstop for pathological cases; the step limit is what gives
    // reproducible errors across machines.
    //
    // 2000ms (kexhq/kex#379) turned out too tight for the sandboxed tree-walk
    // Evaluator's own throughput: an ordinary, non-pathological template a
    // few KB in size — no exotic content, just realistic HTML/CSS/JS —
    // legitimately needs several seconds of real interpreter work to scan,
    // even with `Template.scan`'s own O(n^2) hot spots fixed (repeated
    // UTF-8 re-decoding per character, and building up plain-text runs one
    // `Char` at a time via `push!`, each of which copies the whole backing
    // list). Fixing those made the scan linear in input size rather than
    // quadratic, but linear-and-slow still needs a budget that fits it.
    std::chrono::milliseconds timeout{10000};
    std::size_t stepLimit = 5'000'000;
    // Report errors inside a template's own body as well as at the use site.
    bool verbose = false;
    // When non-null, receives one note per collapse attempt. Costs nothing to
    // leave null, which every path but `--collapse-report` does.
    std::vector<CollapseNote>* report = nullptr;
    // The path of the file being compiled. `Kex.embed("relative/path")`
    // (kexhq/kex#171) resolves against this file's directory, the way
    // `Kex.embed` is Rust's `include_str!`: the path is relative to the
    // source that wrote it, not to the process's working directory. Left
    // empty by a caller that has no file (a REPL line, an in-memory eval)
    // makes every `Kex.embed` there fail to resolve rather than guessing a
    // directory.
    std::string sourcePath;
    // Where the program's modules are found — the roots the rest of the
    // build resolves `using` against. A `compiled do` block runs its module in
    // a sandbox of its own, and one given no roots knew only the standard
    // library: a library's `using Harmat.Schema` beside a `compiled do` then
    // failed with "Unknown module" from every package but the library's own
    // (kexhq/kex#444). The standard library is always searched as well.
    std::vector<std::string> moduleRoots;
    // Where template scans are kept between builds (kexhq/kex#323), and the
    // toolchain that made them. A scan is a pure function of a template's
    // source and the toolchain, and running it costs the sandboxed Evaluator
    // its prelude and the whole of `Template.scan` — a fifth of a second a
    // template, paid by every build of every module that embeds one, and
    // twice by a `--run` (its gating pre-check expands the dependencies
    // too). Empty: scan every time, keeping only this process's own scans.
    std::filesystem::path scanCache;
    std::string toolchainKey;
};

// Expands every `compiled` block in `program` in place, and folds every
// `Kex.embed("path")` call into the literal contents of that file.
//
// The embed fold runs unconditionally — it is not gated behind the program
// having any `compiled do` block, since embedding a template's source has
// nothing to do with that feature; it just happens to share this pass
// because both are AST rewrites that must run before semantic analysis sees
// the program.
//
// Returns false and appends to `diagnostics` if expansion failed; the program
// is left in an unspecified state in that case and must not be compiled.
// Diagnostics are anchored at the USER's source location, not inside whatever
// template generated the code.
auto expand(ast::Program& program,
            std::vector<semantic::Diagnostic>& diagnostics,
            const ExpandOptions& options = {}) -> bool;

// Where compile-time evaluation spends its time, for KEX_TIMINGS: how many
// sandboxed evaluators were started, how long starting them took (an empty
// evaluator with the prelude loaded) and how long they then ran. A build
// that expands for minutes needs to say which of the two it was.
struct SandboxTimings {
    std::size_t started = 0;
    std::chrono::steady_clock::duration startup{};
    std::chrono::steady_clock::duration evaluation{};
};
auto sandboxTimings() -> SandboxTimings&;

// The template-scan cache an expansion uses when its options name none: a
// module the semantic pass or a compile-time sandbox expands builds options
// of its own, and the compiler sets these once for every one of them.
auto setDefaultScanCache(std::filesystem::path root, std::string toolchainKey) -> void;

// Keep what each builder chain collapsed to, by file and place, and reuse it
// whenever the same file is expanded again in this process. A build expands
// every dependency for its gating pre-check, again for the build itself, and
// again inside each compile-time sandbox that loads it — and each collapse is
// a sandbox run of its own. Only for a process whose files cannot change
// while it runs (one build or check), never the REPL or the language server.
auto setReuseCollapses(bool reuse) -> void;

// The files a compile-time result was made from, so that a build may keep it
// for the next one and know when it no longer holds (setReuseCollapses).
// Whatever reads a source file, a template or a directory for an expansion
// says so (noteDependency), and every recorder alive at that moment keeps
// it: a module a sandbox loads is a dependency of what the sandbox
// computed, and so of the expansion that ran the sandbox. A cache that
// answers from memory repeats the files its answer came from
// (noteDependencies), as reading them would have. Recorders exist only
// while collapses are reused; without one, noting costs nothing.
class DependencyRecorder {
public:
    DependencyRecorder();
    ~DependencyRecorder();
    DependencyRecorder(const DependencyRecorder&) = delete;
    auto operator=(const DependencyRecorder&) -> DependencyRecorder& = delete;
    auto files() const -> const std::set<std::string>& { return m_files; }
    // The innermost live recorder, or null.
    static auto current() -> DependencyRecorder*;

private:
    friend auto noteDependency(const std::string& path) -> void;
    friend auto noteDependencies(const std::set<std::string>& paths) -> void;
    std::set<std::string> m_files;
    DependencyRecorder* m_outer;
};
auto noteDependency(const std::string& path) -> void;
auto noteDependencies(const std::set<std::string>& paths) -> void;
// Whether a recorder is live: whether reading a file needs noting.
auto recordingDependencies() -> bool;

// What a dependency looked like: its size and modification time, its
// modification time for a directory (an entry added or removed changes
// it), or "-" while it does not exist. Equal stamps, same file.
auto fileStamp(const std::string& path) -> std::string;

// Whether any compile-time evaluation in this process reached outside the
// program (interpreter::Evaluator::touchedOutside): what this build made
// then depends on more than its files, and is not kept for the next one.
// Where a new file could take the name of a module read from one of
// `files`: the same path under every other root, and the file of each module
// it is nested in. A cache listing these sees a module taken over, as it
// sees one changed.
auto shadowsOf(const std::set<std::string>& files, const std::vector<std::string>& roots)
    -> std::set<std::string>;

auto noteReachedOutside() -> void;
auto reachedOutside() -> bool;

} // namespace kex::compiled
