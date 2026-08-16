#pragma once

#include <iosfwd>
#include <string>

namespace kex::lsp {

// Runs a Language Server Protocol session using Content-Length framed JSON-RPC
// messages. Diagnostics and completion are backed by the compiler's live
// SemanticDB, so open (unsaved) buffers never need temporary files.
//
// `runtimeBeamDir` is the compiled standard library the CLI already locates.
// Passing it matters: without it the checker falls back to reading interfaces
// out of .kex SOURCE, where inferred result types are absent — so every
// method call on a call result typed as `unknown` and a builder chain
// (`Web.Server.new(0).post(…)`) offered no completions at all, while the same
// file checked correctly through `kex -C`.
auto run(std::istream& input, std::ostream& output,
         const std::string& runtimeBeamDir) -> int;

} // namespace kex::lsp
