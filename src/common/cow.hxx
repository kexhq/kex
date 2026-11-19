#pragma once

#include <memory>
#include <utility>

namespace kex {

// A container shared between copies until one of them is changed.
//
// The checker reads the prelude's interfaces through every unit of a build, and
// each unit's copy of them was a deep copy of every function signature it
// holds, then freed again (kexhq/kex#672). Copied, a `Cow` shares the one
// container; reading through a const one never copies. A change through a
// non-const one, or a non-const iterator, copies first if the container is
// shared, so no other copy sees it.
//
// It offers the container's own interface for the calls made on these tables.
// `get()` is the container, read-only, and `mut()` the container to change.
template <typename Container>
class Cow {
public:
    using value_type = typename Container::value_type;
    using size_type = typename Container::size_type;
    using iterator = typename Container::iterator;
    using const_iterator = typename Container::const_iterator;

    Cow() : m_shared(std::make_shared<Container>()) {}
    Cow(Container container) : m_shared(std::make_shared<Container>(std::move(container))) {}
    Cow(std::initializer_list<value_type> values) : m_shared(std::make_shared<Container>(values)) {}
    // A moved-from Cow still holds its container, shared: code that moves a
    // table out and goes on reading the source finds what it held.
    Cow(const Cow&) = default;
    Cow(Cow&& other) noexcept : m_shared(other.m_shared) {}
    auto operator=(const Cow&) -> Cow& = default;
    auto operator=(Cow&& other) noexcept -> Cow& {
        m_shared = other.m_shared;
        return *this;
    }
    auto operator=(Container container) -> Cow& {
        m_shared = std::make_shared<Container>(std::move(container));
        return *this;
    }

    auto get() const -> const Container& {
        return *m_shared;
    }
    operator const Container&() const {
        return *m_shared;
    }
    auto mut() -> Container& {
        if (m_shared.use_count() > 1) {
            m_shared = std::make_shared<Container>(*m_shared);
        }
        return *m_shared;
    }

    auto begin() const {
        return m_shared->cbegin();
    }
    auto end() const {
        return m_shared->cend();
    }
    auto cbegin() const {
        return m_shared->cbegin();
    }
    auto cend() const {
        return m_shared->cend();
    }
    auto begin() {
        return mut().begin();
    }
    auto end() {
        return mut().end();
    }
    auto size() const {
        return m_shared->size();
    }
    auto empty() const {
        return m_shared->empty();
    }

    template <typename Key>
    auto find(const Key& key) const {
        return m_shared->find(key);
    }
    template <typename Key>
    auto find(const Key& key) {
        return mut().find(key);
    }
    template <typename Key>
    auto count(const Key& key) const {
        return m_shared->count(key);
    }
    template <typename Key>
    auto contains(const Key& key) const {
        return m_shared->contains(key);
    }
    template <typename Key>
    decltype(auto) at(const Key& key) const {
        return m_shared->at(key);
    }
    template <typename Key>
    decltype(auto) at(const Key& key) {
        return mut().at(key);
    }
    template <typename Key>
    decltype(auto) operator[](Key&& key) {
        return mut()[std::forward<Key>(key)];
    }
    template <typename Key>
    decltype(auto) operator[](const Key& key) const
        requires requires(const Container& c, const Key& k) { c[k]; }
    {
        return (*m_shared)[key];
    }
    decltype(auto) front() const {
        return m_shared->front();
    }
    decltype(auto) back() const {
        return m_shared->back();
    }
    decltype(auto) front() {
        return mut().front();
    }
    decltype(auto) back() {
        return mut().back();
    }

    template <typename... Args>
    decltype(auto) emplace(Args&&... args) {
        return mut().emplace(std::forward<Args>(args)...);
    }
    template <typename... Args>
    decltype(auto) try_emplace(Args&&... args) {
        return mut().try_emplace(std::forward<Args>(args)...);
    }
    template <typename... Args>
    decltype(auto) insert(Args&&... args) {
        return mut().insert(std::forward<Args>(args)...);
    }
    template <typename... Args>
    decltype(auto) insert_or_assign(Args&&... args) {
        return mut().insert_or_assign(std::forward<Args>(args)...);
    }
    template <typename... Args>
    decltype(auto) erase(Args&&... args) {
        return mut().erase(std::forward<Args>(args)...);
    }
    template <typename Value>
    void push_back(Value&& value) {
        mut().push_back(std::forward<Value>(value));
    }
    template <typename... Args>
    decltype(auto) emplace_back(Args&&... args) {
        return mut().emplace_back(std::forward<Args>(args)...);
    }
    void reserve(size_type size) {
        mut().reserve(size);
    }
    void clear() {
        mut().clear();
    }

    friend auto operator==(const Cow& a, const Cow& b) -> bool {
        return a.m_shared == b.m_shared || a.get() == b.get();
    }

private:
    std::shared_ptr<Container> m_shared;
};

// `std::erase_if` on what a Cow holds, copying it first only if something is
// erased.
template <typename Container, typename Predicate>
auto erase_if(Cow<Container>& cow, Predicate predicate) -> typename Container::size_type {
    const auto& shared = cow.get();
    bool any = false;
    for (const auto& value : shared) {
        if (predicate(value)) {
            any = true;
            break;
        }
    }
    return any ? std::erase_if(cow.mut(), predicate) : 0;
}

} // namespace kex
