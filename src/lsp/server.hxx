#pragma once

#include <iosfwd>

namespace kex::lsp {

// Runs a Language Server Protocol session using Content-Length framed JSON-RPC
// messages. Diagnostics and completion are backed by the compiler's live
// SemanticDB, so open (unsaved) buffers never need temporary files.
auto run(std::istream& input, std::ostream& output) -> int;

} // namespace kex::lsp
