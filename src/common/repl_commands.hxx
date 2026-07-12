#pragma once

#include "color.hxx"
#include "version.hxx"

#include <iostream>
#include <string>
#include <vector>

namespace kex {

struct ReplCommand {
  std::string name;
  std::string alias;
  std::string arg;
  std::string description;
};

inline auto replCommands() -> const std::vector<ReplCommand> & {
  static const std::vector<ReplCommand> cmds = {
      {"exit", "", "", "Exit the REPL"},
      {"/help", "/h", "", "Show this help"},
      {"/set", "", "<opt>", "Enable a feature"},
      {"/unset", "", "<opt>", "Disable a feature"},
      {"/complete", "", "<p>", "Show completions for prefix p"},
      {"/load", "", "<file>", "Load a module from file"},
      {"/unload", "", "<mod>", "Unload a previously loaded module"},
      {"/reload", "", "", "Reload all loaded modules"},
      {"/reset", "", "", "Clear all bindings"},
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

inline auto printReplBanner(std::ostream &out, const std::string &backend)
    -> void {
  out << "\n"
      << color::apply(color::bold) << color::apply(color::yellow)
      << "Kex Interactive" << color::apply(color::reset) << " "
      << color::apply(color::yellow) << kVersion << color::apply(color::reset);
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
