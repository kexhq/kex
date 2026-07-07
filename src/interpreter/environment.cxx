#include "environment.hxx"

namespace kex::interpreter {

Environment::Environment(std::shared_ptr<Environment> parent)
    : m_parent(std::move(parent)) {}

auto Environment::define(const std::string& name, ValuePtr value, bool isMutable) -> void {
    m_bindings[name] = Binding{std::move(value), isMutable};
}

auto Environment::set(const std::string& name, ValuePtr value) -> bool {
    auto it = m_bindings.find(name);
    if (it != m_bindings.end()) {
        it->second.value = std::move(value);
        return true;
    }
    if (m_parent) {
        return m_parent->set(name, std::move(value));
    }
    return false;
}

auto Environment::get(const std::string& name) const -> ValuePtr {
    auto it = m_bindings.find(name);
    if (it != m_bindings.end()) return it->second.value;
    if (m_parent) return m_parent->get(name);
    return nullptr;
}

auto Environment::has(const std::string& name) const -> bool {
    if (m_bindings.count(name)) return true;
    if (m_parent) return m_parent->has(name);
    return false;
}

auto Environment::isMutable(const std::string& name) const -> bool {
    auto it = m_bindings.find(name);
    if (it != m_bindings.end()) return it->second.isMutable;
    if (m_parent) return m_parent->isMutable(name);
    return false;
}

auto Environment::parent() const -> std::shared_ptr<Environment> {
    return m_parent;
}

auto Environment::importAll(const Environment& other) -> void {
    for (const auto& [name, b] : other.m_bindings)
        m_bindings[name] = b;
}

} // namespace kex::interpreter
