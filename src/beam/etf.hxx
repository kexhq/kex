#pragma once
// ETF (External Term Format) encoder/decoder — the subset needed for
// KexI chunk payloads. Represents Erlang terms as a C++ variant tree.
#include <cstdint>
#include <gmpxx.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace kex::beam {

struct EtfError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Term;
using TermPtr = std::shared_ptr<Term>;

struct Term {
    struct Atom   { std::string name; };
    struct Int    { int64_t value; };
    struct BigInt { mpz_class value; };
    struct Float  { double value; };
    struct Bin    { std::vector<uint8_t> data; };
    struct Tuple  { std::vector<TermPtr> elements; };
    struct List   { std::vector<TermPtr> elements; }; // proper list (nil tail)
    struct Map    { std::vector<std::pair<TermPtr, TermPtr>> pairs; };
    struct Nil    {};

    std::variant<Atom, Int, BigInt, Float, Bin, Tuple, List, Map, Nil> value;

    static auto atom(std::string name) -> TermPtr;
    static auto integer(int64_t v) -> TermPtr;
    static auto integer(mpz_class v) -> TermPtr;
    static auto floating(double v) -> TermPtr;
    static auto binary(std::vector<uint8_t> data) -> TermPtr;
    static auto binary(const std::string& s) -> TermPtr;
    static auto tuple(std::vector<TermPtr> elems) -> TermPtr;
    static auto list(std::vector<TermPtr> elems) -> TermPtr;
    static auto map(std::vector<std::pair<TermPtr, TermPtr>> pairs) -> TermPtr;
    static auto nil() -> TermPtr;

    auto isAtom() const -> bool;
    auto isAtom(const std::string& name) const -> bool;
    auto asAtom() const -> const std::string&;
    auto asInt() const -> int64_t;
    auto asBigInt() const -> const mpz_class&;
    auto asFloat() const -> double;
    auto asBinary() const -> const std::vector<uint8_t>&;
    auto asBinaryStr() const -> std::string;
    auto asTuple() const -> const std::vector<TermPtr>&;
    auto asList() const -> const std::vector<TermPtr>&;
    auto asMap() const -> const std::vector<std::pair<TermPtr, TermPtr>>&;

    auto mapGet(const std::string& key) const -> TermPtr;
};

auto encodeEtf(const TermPtr& term) -> std::vector<uint8_t>;
auto decodeEtf(const std::vector<uint8_t>& bytes) -> TermPtr;
auto decodeEtf(const uint8_t* data, size_t len) -> TermPtr;

} // namespace kex::beam
