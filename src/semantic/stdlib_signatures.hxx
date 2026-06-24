#pragma once

#include "traits.hxx"
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

// Stdlib function signatures, declared once centrally so the checker,
// lint, and a future LSP completion provider all read the same table
// instead of each re-deriving "what does `even?` accept" independently.
// Mirrors the structure of src/interpreter/stdlib/*.cxx so it's obvious
// which native function a signature belongs to.
class SignatureTable {
public:
    auto define(Signature sig) -> void;

    // Returns the overload set for `name`, or nullptr if unknown to the
    // table (e.g. not yet covered, or a user-defined function).
    auto lookup(const std::string& name) const -> const std::vector<Signature>*;

    static auto withStdlib() -> SignatureTable;

private:
    std::unordered_map<std::string, std::vector<Signature>> m_signatures;
};

} // namespace kex::semantic
