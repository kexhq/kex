#pragma once

#include <iosfwd>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// `kex -s <Name>`: the standard library's own documentation, read out of the
// stdlib sources this compiler loads. Nothing here is written by hand or
// generated ahead of time, so what it prints cannot describe a different
// version of the library than the one a program will be checked against.
namespace kex::help {

enum class OwnerKind { Module, Type, Record, Trait };

// One thing a caller can write: a function of a module, or a method a `make`
// or `trait` block gives a type.
struct ApiMember {
    std::string name;
    std::string signature; // as declared: `first :> X?`
    // The block that declares it, as written: `make [Number]`. Empty for a
    // module's own functions and a trait's own methods.
    std::string group;
    std::string doc;
};

// A name `kex -s` can be asked about: a module, a type, a record or a trait.
struct ApiOwner {
    std::string name; // qualified: `FS.File`
    OwnerKind kind = OwnerKind::Type;
    std::string declaration; // `type Optional<X> = Just(X) | None`
    std::string doc;
    // The `using` a program needs before it can say this name; empty when the
    // prelude already provides it.
    std::string requiresUsing;
    std::vector<std::string> traits;
    std::vector<ApiMember> members;
};

class ApiIndex {
public:
    // Every source under the standard-library root. `warnings` receives one
    // line per file that could not be read, so a broken stdlib file costs its
    // own entries rather than the whole lookup.
    static auto fromStandardLibrary(std::ostream& warnings) -> ApiIndex;

    // `usingName` is the module a program must import to see this source,
    // spelled from its path; empty for a prelude source. Throws what the
    // lexer and parser throw.
    auto addSource(const std::string& path, std::string source,
                   const std::string& usingName) -> void;

    auto empty() const -> bool { return m_owners.empty(); }

    // Every owner, one line each.
    auto printOverview(std::ostream& out) const -> void;

    // An owner (`List`), one member (`List.first`), or a member by its bare
    // name (`first`). False when nothing matched; the nearest names have then
    // been printed to `err`.
    auto printTopic(std::string_view query, std::ostream& out,
                    std::ostream& err) const -> bool;

private:
    auto findOwners(std::string_view name) const
        -> std::vector<const ApiOwner*>;
    auto traitOwner(const std::string& name) const -> const ApiOwner*;
    auto printOwner(const ApiOwner& owner, std::ostream& out) const -> void;

    std::map<std::string, ApiOwner> m_owners;
};

} // namespace kex::help
