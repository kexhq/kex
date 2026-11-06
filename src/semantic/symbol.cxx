#include "symbol.hxx"

namespace kex::semantic {

Scope::Scope(Scope* parent) : m_parent(parent) {}

auto Scope::define(Symbol symbol) -> bool {
    auto [_, inserted] = m_symbols.emplace(symbol.name, std::move(symbol));
    return inserted;
}

auto Scope::redefine(Symbol symbol) -> void {
    m_symbols[symbol.name] = std::move(symbol);
}

auto Scope::lookup(const std::string& name) const -> const Symbol* {
    if (auto it = m_symbols.find(name); it != m_symbols.end()) {
        return &it->second;
    }
    if (m_parent) {
        return m_parent->lookup(name);
    }
    return nullptr;
}

auto Scope::lookupLocal(const std::string& name) const -> const Symbol* {
    if (auto it = m_symbols.find(name); it != m_symbols.end()) {
        return &it->second;
    }
    return nullptr;
}

auto Scope::parent() const -> Scope* {
    return m_parent;
}

SymbolTable::SymbolTable() : m_current(nullptr) {
    pushScope();
}

auto SymbolTable::pushScope() -> Scope& {
    auto scope = std::make_unique<Scope>(m_current);
    m_current = scope.get();
    m_scopes.push_back(std::move(scope));
    return *m_current;
}

auto SymbolTable::popScope() -> void {
    if (m_current) {
        m_current = m_current->parent();
    }
}

auto SymbolTable::currentScope() -> Scope& {
    return *m_current;
}

auto SymbolTable::define(Symbol symbol) -> bool {
    return m_current->define(std::move(symbol));
}

auto SymbolTable::redefine(Symbol symbol) -> void {
    m_current->redefine(std::move(symbol));
}

auto SymbolTable::lookup(const std::string& name) const -> const Symbol* {
    return m_current->lookup(name);
}

} // namespace kex::semantic
