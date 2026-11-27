#pragma once

// The two REPLs: `kex -i` (inputs compiled and run on the BEAM) and `kex -R -i`
// (the tree-walker). Each reads inputs until EOF or `/quit`.

#include <string>

auto runBeamRepl(bool skipPrelude, bool inspect = true) -> int;
auto runWalkerRepl(bool skipPrelude, bool inspect = true) -> int;

// A line `--init`, `--eval` or `--using` queues, read by the REPL before the
// terminal.
auto queueReplScriptLine(const std::string& line) -> void;
// `--eval`: the queued lines are the whole session. No banner and no prompt,
// the terminal is never read, and the session ends when the lines run out.
auto setReplScriptOnly(bool on) -> void;
auto replScriptOnly() -> bool;
