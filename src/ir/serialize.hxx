#pragma once
// Lowering IR as bytes (ETF terms), for IR that must outlive the process that
// lowered it: the standard library's functions a caller may copy with its
// block in place (passes/specialize.hxx). The bytes are read back by the same
// toolchain that wrote them; any other format throws.
#include "ir.hxx"

#include <cstdint>
#include <vector>

namespace kex::ir {

auto encodeFunctions(const std::vector<FunDef>& functions) -> std::vector<uint8_t>;
auto decodeFunctions(const std::vector<uint8_t>& bytes) -> std::vector<FunDef>;

} // namespace kex::ir
