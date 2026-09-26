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
    // Names bound directly in this environment (not parents).
    auto names() const -> std::vector<std::string>;
    // The function call a `return` evaluated in this scope leaves: the
    // nearest one set on this environment or a parent. A block's environment
    // chains to the function it was WRITTEN in, which is what makes a
    // `return` in a block leave that function (kexhq/kex#408). The flag is
    // true while the call runs; see ReturnException.
    auto setReturnFrame(std::shared_ptr<bool> frame) -> void;
    auto returnFrame() const -> std::shared_ptr<bool>;

private:
    struct Binding {
        ValuePtr value;
        bool isMutable;
    };

    std::unordered_map<std::string, Binding> m_bindings;
    std::shared_ptr<Environment> m_parent;
    std::shared_ptr<bool> m_returnFrame;
};

} // namespace kex::interpreter
