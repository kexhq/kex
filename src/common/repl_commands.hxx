#pragma once

#include <iomanip>
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
        {"exit",      "",  "",      "Exit the REPL"},
        {"/help",     "/h", "",     "Show this help"},
        {"/set",      "",  "<opt>", "Enable a feature"},
        {"/unset",    "",  "<opt>", "Disable a feature"},
        {"/complete", "",  "<p>",   "Show completions for prefix p"},
        {"/load",     "",  "<file>","Load a module from file"},
        {"/reload",   "",  "",      "Reload all loaded modules"},
        {"/reset",    "",  "",      "Clear all bindings"},
    };
    return cmds;
}

inline auto printReplHelp(std::ostream &out,
                          const std::string &setOptions = "") -> void {
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

} // namespace kex
