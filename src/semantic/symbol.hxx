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

    // An implicit clause binding (`new`/`from` in a make method), not a
    // declaration the user wrote: an explicit binding shadows it rather
    // than redeclaring it (kexhq/kex#601).
    bool isImplicit = false;
};

class Scope {
public:
    explicit Scope(Scope* parent = nullptr);

    auto define(Symbol symbol) -> bool;
    // Overwrites any same-scope entry: an explicit binding replacing the
    // implicit `new`/`from` it shadows (kexhq/kex#601).
    auto redefine(Symbol symbol) -> void;
    auto lookup(const std::string& name) const -> const Symbol*;
    auto lookupLocal(const std::string& name) const -> const Symbol*;
    auto parent() const -> Scope*;

private:
    std::unordered_map<std::string, Symbol> m_symbols;
    Scope* m_parent;
};

class SymbolTable {
public:
    SymbolTable();

    auto pushScope() -> Scope&;
    auto popScope() -> void;
    auto currentScope() -> Scope&;
    auto define(Symbol symbol) -> bool;
    auto redefine(Symbol symbol) -> void;
    auto lookup(const std::string& name) const -> const Symbol*;

private:
    std::vector<std::unique_ptr<Scope>> m_scopes;
    Scope* m_current;
};

} // namespace kex::semantic
