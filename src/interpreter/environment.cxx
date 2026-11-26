#include "environment.hxx"

namespace kex::interpreter {

Environment::Environment(std::shared_ptr<Environment> parent) : m_parent(std::move(parent)) {}

auto Environment::define(const std::string& name, ValuePtr value, bool isMutable) -> void {
    m_bindings[name] = Binding{std::move(value), isMutable};
}

auto Environment::erase(const std::string& name) -> void {
    m_bindings.erase(name);
}

// The lookups walk the scope chain in a loop: it can grow with the calls that
// are still running, and recursing up it overflowed the stack wherever the
// compiler did not turn the tail call into a jump (an ASan build).
auto Environment::set(const std::string& name, ValuePtr value) -> bool {
    for (auto* scope = this; scope; scope = scope->m_parent.get()) {
        auto it = scope->m_bindings.find(name);
        if (it != scope->m_bindings.end()) {
            it->second.value = std::move(value);
            return true;
        }
    }
    return false;
}

auto Environment::get(const std::string& name) const -> ValuePtr {
    for (const auto* scope = this; scope; scope = scope->m_parent.get()) {
        auto it = scope->m_bindings.find(name);
        if (it != scope->m_bindings.end()) {
            return it->second.value;
        }
    }
    return nullptr;
}

auto Environment::has(const std::string& name) const -> bool {
    for (const auto* scope = this; scope; scope = scope->m_parent.get()) {
        if (scope->m_bindings.count(name)) {
            return true;
        }
    }
    return false;
}

auto Environment::isMutable(const std::string& name) const -> bool {
    for (const auto* scope = this; scope; scope = scope->m_parent.get()) {
        auto it = scope->m_bindings.find(name);
        if (it != scope->m_bindings.end()) {
            return it->second.isMutable;
        }
    }
    return false;
}

auto Environment::parent() const -> std::shared_ptr<Environment> {
    return m_parent;
}

auto Environment::setReturnFrame(std::shared_ptr<bool> frame) -> void {
    m_returnFrame = std::move(frame);
}

auto Environment::returnFrame() const -> std::shared_ptr<bool> {
    for (const auto* scope = this; scope; scope = scope->m_parent.get()) {
        if (scope->m_returnFrame) {
            return scope->m_returnFrame;
        }
    }
    return nullptr;
}

auto Environment::names() const -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(m_bindings.size());
    for (const auto& [name, b] : m_bindings) {
        out.push_back(name);
    }
    return out;
}

} // namespace kex::interpreter
