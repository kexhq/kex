#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace kex {

// Read a whole file as bytes with one block read; nullopt on failure.
// Byte-by-byte `istreambuf_iterator` reads cost ~90 ms on the 904 KB
// stdlib rescan that keys the prelude interface cache (3c), so nothing on a
// warm path may use them.
inline auto readFileBytes(const std::filesystem::path &path)
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

// The same, as text; nullopt on failure.
inline auto readFileText(const std::filesystem::path &path)
    -> std::optional<std::string> {
  auto bytes = readFileBytes(path);
  if (!bytes) return std::nullopt;
  return std::string(bytes->begin(), bytes->end());
}

// Read a whole file into a string; "" and an error line on failure.
inline auto readFile(const std::string &path) -> std::string {
  if (auto text = readFileText(path)) return std::move(*text);
  std::cerr << "Error: could not open file: " << path << "\n";
  return "";
}

} // namespace kex
