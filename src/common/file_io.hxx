#pragma once

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace kex {

// Read a whole file into a string; "" and an error line on failure.
inline auto readFile(const std::string &path) -> std::string {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "Error: could not open file: " << path << "\n";
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

} // namespace kex
