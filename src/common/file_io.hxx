#pragma once

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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

// Read a whole file as bytes with one block read; nullopt on failure.
// Byte-by-byte `istreambuf_iterator` reads cost ~90 ms on the 904 KB
// stdlib rescan that keys the prelude interface cache (3c), so the
// warm path must not use them.
inline auto readFileBytes(const std::string &path)
    -> std::optional<std::vector<uint8_t>> {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return std::nullopt;
  const auto size = input.tellg();
  if (size < 0) return std::nullopt;
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size())))
    return std::nullopt;
  return bytes;
}

} // namespace kex
