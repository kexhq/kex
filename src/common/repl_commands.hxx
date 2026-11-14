#pragma once

#include "color.hxx"
#include "version.hxx"

#include <iostream>
#include <string>
#include <vector>

namespace kex {

// Every REPL command, for dispatch. `None` is ordinary input to evaluate.
enum class ReplCommandKind {
  None, Exit, Clear, Help, Set, Unset, Complete, Load, Unload, Reload, Reset,
  With, Without,
};

struct ReplCommand {
  ReplCommandKind kind;
  std::string name;
  std::string alias;
  std::string arg;
  std::string description;
};

inline auto replCommands() -> const std::vector<ReplCommand> & {
  using K = ReplCommandKind;
  static const std::vector<ReplCommand> cmds = {
      {K::Exit, "exit", "", "", "Exit the REPL"},
      {K::Clear, "/clear", "", "", "Clear the terminal screen"},
      {K::Help, "/help", "/h", "", "Show this help"},
      {K::Set, "/set", "", "<opt>", "Enable a feature"},
      {K::Unset, "/unset", "", "<opt>", "Disable a feature"},
      {K::Complete, "/complete", "", "<p>", "Show completions for prefix p"},
      {K::Load, "/load", "", "<file>", "Load a module from file"},
      {K::Unload, "/unload", "", "<mod>", "Unload a previously loaded module"},
      {K::Reload, "/reload", "", "", "Reload all loaded modules"},
      {K::Reset, "/reset", "", "", "Clear all bindings"},
      {K::With, "/with", "", "<C> = <v>",
       "Replace capability C with v for the session"},
      {K::Without, "/without", "", "<C>", "Stop replacing capability C"},
  };
  return cmds;
}

inline auto printReplHelp(std::ostream &out, const std::string &setOptions = "")
    -> void {
  out << "\n  Commands:\n";
  for (auto &cmd : replCommands()) {
    std::string left = cmd.name;
    if (!cmd.arg.empty())
      left += " " + cmd.arg;
    out << "    ";
    out.width(16);
    out << std::left << left << cmd.description << "\n";
  }
  if (!setOptions.empty())
    out << "\n" << setOptions;
  out << "\n";
}

inline auto replCommandCompletions(const std::string &prefix)
    -> std::vector<std::string> {
  std::vector<std::string> matches;
  for (auto &cmd : replCommands()) {
    if (!cmd.name.empty() && cmd.name[0] == '/' &&
        cmd.name.rfind(prefix, 0) == 0)
      matches.push_back(cmd.name);
    if (!cmd.alias.empty() && cmd.alias[0] == '/' &&
        cmd.alias.rfind(prefix, 0) == 0)
      matches.push_back(cmd.alias);
  }
  return matches;
}

inline auto isReplExit(const std::string &input) -> bool {
  return input == "exit" || input == "/exit" || input == "/quit" ||
         input == "/q";
}

// A line typed at a REPL prompt, read as a command: which one, and the text
// after the command word with its outer whitespace trimmed. Every REPL (the
// BEAM one, the walker's, the browser's) dispatches on this instead of
// matching the spellings itself.
struct ParsedReplCommand {
  ReplCommandKind kind = ReplCommandKind::None;
  std::string argument;
};

inline auto parseReplCommand(const std::string &input) -> ParsedReplCommand {
  if (isReplExit(input)) return {ReplCommandKind::Exit, ""};
  if (input.empty() || input[0] != '/') return {};
  const auto wordEnd = input.find_first_of(" \t");
  const auto word = input.substr(0, wordEnd);
  for (const auto &cmd : replCommands()) {
    if (word != cmd.name && (cmd.alias.empty() || word != cmd.alias)) continue;
    std::string argument;
    if (wordEnd != std::string::npos) {
      const auto first = input.find_first_not_of(" \t", wordEnd);
      const auto last = input.find_last_not_of(" \t\r\n");
      if (first != std::string::npos && last >= first)
        argument = input.substr(first, last - first + 1);
    }
    return {cmd.kind, std::move(argument)};
  }
  return {};
}

// ANSI sequence: clear the visible screen and its scrollback, then move the
// cursor to the top-left. Works on the native REPLs' real terminals and on
// the web REPL's xterm.js, which renders the same escape codes.
inline auto replClearScreenSequence() -> std::string {
  return "\x1b[H\x1b[2J\x1b[3J";
}

inline auto printReplBanner(std::ostream &out, const std::string &backend)
    -> void {
  if (!backend.empty()) out << "\n";
  out << color::apply(color::bold) << color::apply(color::yellow)
      << "Kex Interactive" << color::apply(color::reset) << " "
      << color::apply(color::yellow) << versionString()
      << color::apply(color::reset);
  if (!backend.empty())
    out << " " << color::apply(color::gray) << "(" << backend << ")"
        << color::apply(color::reset);
  out << " " << color::apply(color::gray) << "—" << color::apply(color::reset)
      << " press " << color::apply(color::cyan) << "Ctrl+C"
      << color::apply(color::reset) << " to exit " << color::apply(color::gray)
      << "(type " << color::apply(color::cyan) << "/help"
      << color::apply(color::gray) << " ENTER for commands)"
      << color::apply(color::reset) << "\n\n";
}

} // namespace kex
