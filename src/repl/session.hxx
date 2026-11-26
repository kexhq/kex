#pragma once

// The two REPLs: `kex -i` (inputs compiled and run on the BEAM) and `kex -R -i`
// (the tree-walker). Each reads inputs until EOF or `/quit`.

#include <string>

auto runBeamRepl(bool skipPrelude, bool inspect = true) -> int;
auto runWalkerRepl(bool skipPrelude, bool inspect = true) -> int;

// A line `--eval` queues, read by the REPL before the terminal.
auto queueReplScriptLine(const std::string& line) -> void;
