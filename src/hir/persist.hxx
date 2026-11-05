#pragma once
// A checked program saved in one process and read back in another. Within one
// toolchain (the same sources and the same compiler: cache::toolchain
// Fingerprint) a program parses to the same nodes in the same order, so a
// node's identity is its position in `nodeOrder`: the checker's answers are
// written by position and read back onto the new process's nodes. The walker
// keeps its prelude's this way, rather than checking the whole stdlib in every
// process.
#include "hir.hxx"

#include <cstdint>
#include <memory>
#include <vector>

namespace kex::hir {

// Every expression of `program`'s bodies, in a fixed pre-order.
auto nodeOrder(const ast::Program& program) -> std::vector<const ast::Expr*>;

// `program`'s answers for the nodes in `order`.
auto save(const Program& program, const std::vector<const ast::Expr*>& order)
    -> std::vector<uint8_t>;

// The program `bytes` describe, answering for `ast`'s nodes in `order`; null
// when the bytes are malformed, of another format, or of another program
// (another node count).
auto load(const ast::Program& ast, const std::vector<const ast::Expr*>& order,
          const std::vector<uint8_t>& bytes) -> std::unique_ptr<Program>;

} // namespace kex::hir
