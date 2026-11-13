#pragma once

namespace kex::semantic {

// What a declaration tooling lists is (symbol_info.hxx).
enum class SymbolKind {
    Variable,
    Function,
    Type,
    Trait,
    Record,
    Module,
    TypeParam,
};

} // namespace kex::semantic
