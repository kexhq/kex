#pragma once

#include "../lexer/token.hxx"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::semantic {

enum class SymbolKind {
    Variable,
    Function,
    Type,
    Trait,
    Record,
    Module,
    TypeParam,
};

struct Symbol {
    std::string name;
    SymbolKind kind;
    bool isFoul = false;
    bool isMutable = false;     // var vs let
    bool isPublic = true;
    SourceLocation location;

    // For functions
    int clauseCount = 0;

    // For types
    std::vector<std::string> typeParams;
    std::vector<std::string> parents;
};

class Scope {
public:
    explicit Scope(Scope* parent = nullptr, bool isFoul = false);

    auto define(Symbol symbol) -> bool;
    auto lookup(const std::string& name) const -> const Symbol*;
    auto lookupLocal(const std::string& name) const -> const Symbol*;
    auto parent() const -> Scope*;
    auto isFoul() const -> bool;

private:
    std::unordered_map<std::string, Symbol> m_symbols;
    Scope* m_parent;
    bool m_isFoul;
};

class SymbolTable {
public:
    SymbolTable();

    auto pushScope(bool isFoul = false) -> Scope&;
    auto popScope() -> void;
    auto currentScope() -> Scope&;
    auto define(Symbol symbol) -> bool;
    auto lookup(const std::string& name) const -> const Symbol*;

private:
    std::vector<std::unique_ptr<Scope>> m_scopes;
    Scope* m_current;
};

} // namespace kex::semantic
