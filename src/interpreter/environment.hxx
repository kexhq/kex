#pragma once

#include "value.hxx"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kex::interpreter {

class Environment {
public:
    explicit Environment(std::shared_ptr<Environment> parent = nullptr);

    auto define(const std::string& name, ValuePtr value, bool isMutable = false) -> void;
    auto erase(const std::string& name) -> void;
    auto set(const std::string& name, ValuePtr value) -> bool;
    auto get(const std::string& name) const -> ValuePtr;
    auto has(const std::string& name) const -> bool;
    // Only meaningful when has(name) is true; absent bindings are reported as
    // immutable so callers don't need a separate existence check.
    auto isMutable(const std::string& name) const -> bool;
    auto parent() const -> std::shared_ptr<Environment>;
    // Copy all bindings from `other` into this environment.
    auto importAll(const Environment& other) -> void;
    // Names bound directly in this environment (not parents). Used to snapshot
    // the registered stdlib for the sealed-method check.
    auto names() const -> std::vector<std::string>;

private:
    struct Binding {
        ValuePtr value;
        bool isMutable;
    };

    std::unordered_map<std::string, Binding> m_bindings;
    std::shared_ptr<Environment> m_parent;
};

} // namespace kex::interpreter
