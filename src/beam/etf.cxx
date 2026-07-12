#include "etf.hxx"
#include <cstring>

namespace kex::beam {

// ── Term constructors ──────────────────────────────────────────────────

auto Term::atom(std::string name) -> TermPtr {
    auto t = std::make_shared<Term>();
    t->value = Atom{std::move(name)};
    return t;
}
auto Term::integer(int64_t v) -> TermPtr {
    auto t = std::make_shared<Term>();
    t->value = Int{v};
    return t;
}
auto Term::binary(std::vector<uint8_t> data) -> TermPtr {
    auto t = std::make_shared<Term>();
    t->value = Bin{std::move(data)};
    return t;
}
auto Term::binary(const std::string& s) -> TermPtr {
    return binary(std::vector<uint8_t>(s.begin(), s.end()));
}
auto Term::tuple(std::vector<TermPtr> elems) -> TermPtr {
    auto t = std::make_shared<Term>();
    t->value = Tuple{std::move(elems)};
    return t;
}
auto Term::list(std::vector<TermPtr> elems) -> TermPtr {
    auto t = std::make_shared<Term>();
    t->value = List{std::move(elems)};
    return t;
}
auto Term::map(std::vector<std::pair<TermPtr, TermPtr>> pairs) -> TermPtr {
    auto t = std::make_shared<Term>();
    t->value = Map{std::move(pairs)};
    return t;
}
auto Term::nil() -> TermPtr {
    auto t = std::make_shared<Term>();
    t->value = Nil{};
    return t;
}

// ── Accessors ──────────────────────────────────────────────────────────

auto Term::isAtom() const -> bool {
    return std::holds_alternative<Atom>(value);
}
auto Term::isAtom(const std::string& name) const -> bool {
    auto* a = std::get_if<Atom>(&value);
    return a && a->name == name;
}
auto Term::asAtom() const -> const std::string& {
    return std::get<Atom>(value).name;
}
auto Term::asInt() const -> int64_t {
    return std::get<Int>(value).value;
}
auto Term::asBinary() const -> const std::vector<uint8_t>& {
    return std::get<Bin>(value).data;
}
auto Term::asBinaryStr() const -> std::string {
    auto& d = asBinary();
    return {d.begin(), d.end()};
}
auto Term::asTuple() const -> const std::vector<TermPtr>& {
    return std::get<Tuple>(value).elements;
}
auto Term::asList() const -> const std::vector<TermPtr>& {
    if (std::holds_alternative<Nil>(value)) {
        static const std::vector<TermPtr> empty;
        return empty;
    }
    return std::get<List>(value).elements;
}
auto Term::asMap() const -> const std::vector<std::pair<TermPtr, TermPtr>>& {
    if (std::holds_alternative<Nil>(value)) {
        static const std::vector<std::pair<TermPtr, TermPtr>> empty;
        return empty;
    }
    return std::get<Map>(value).pairs;
}

auto Term::mapGet(const std::string& key) const -> TermPtr {
    if (std::holds_alternative<Nil>(value)) return nullptr;
    if (!std::holds_alternative<Map>(value)) return nullptr;
    for (auto& [k, v] : std::get<Map>(value).pairs)
        if (k->isAtom(key)) return v;
    return nullptr;
}

// ── ETF encoding ───────────────────────────────────────────────────────

namespace {

constexpr uint8_t ETF_VERSION = 131;

// Tag bytes
constexpr uint8_t SMALL_INTEGER_EXT  = 97;
constexpr uint8_t INTEGER_EXT        = 98;
constexpr uint8_t ATOM_EXT           = 100;
constexpr uint8_t SMALL_TUPLE_EXT    = 104;
constexpr uint8_t LARGE_TUPLE_EXT    = 105;
constexpr uint8_t NIL_EXT            = 106;
constexpr uint8_t LIST_EXT           = 108;
constexpr uint8_t BINARY_EXT         = 109;
constexpr uint8_t SMALL_ATOM_EXT     = 115;
constexpr uint8_t MAP_EXT            = 116;
constexpr uint8_t ATOM_UTF8_EXT      = 118;
constexpr uint8_t SMALL_ATOM_UTF8_EXT = 119;
constexpr uint8_t SMALL_BIG_EXT      = 110;

void put8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void encodeTerm(std::vector<uint8_t>& out, const TermPtr& term);

void encodeTerm(std::vector<uint8_t>& out, const TermPtr& term) {
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, Term::Atom>) {
            if (node.name.size() <= 255) {
                put8(out, SMALL_ATOM_UTF8_EXT);
                put8(out, static_cast<uint8_t>(node.name.size()));
            } else {
                put8(out, ATOM_UTF8_EXT);
                put16(out, static_cast<uint16_t>(node.name.size()));
            }
            out.insert(out.end(), node.name.begin(), node.name.end());
        } else if constexpr (std::is_same_v<T, Term::Int>) {
            if (node.value >= 0 && node.value <= 255) {
                put8(out, SMALL_INTEGER_EXT);
                put8(out, static_cast<uint8_t>(node.value));
            } else if (node.value >= INT32_MIN && node.value <= INT32_MAX) {
                put8(out, INTEGER_EXT);
                put32(out, static_cast<uint32_t>(static_cast<int32_t>(node.value)));
            } else {
                // Use SMALL_BIG_EXT for values outside int32 range
                put8(out, SMALL_BIG_EXT);
                uint64_t abs = node.value < 0
                    ? static_cast<uint64_t>(-node.value)
                    : static_cast<uint64_t>(node.value);
                uint8_t sign = node.value < 0 ? 1 : 0;
                uint8_t bytes[8];
                int n = 0;
                do {
                    bytes[n++] = abs & 0xFF;
                    abs >>= 8;
                } while (abs > 0);
                put8(out, static_cast<uint8_t>(n));
                put8(out, sign);
                out.insert(out.end(), bytes, bytes + n);
            }
        } else if constexpr (std::is_same_v<T, Term::Bin>) {
            put8(out, BINARY_EXT);
            put32(out, static_cast<uint32_t>(node.data.size()));
            out.insert(out.end(), node.data.begin(), node.data.end());
        } else if constexpr (std::is_same_v<T, Term::Tuple>) {
            if (node.elements.size() <= 255) {
                put8(out, SMALL_TUPLE_EXT);
                put8(out, static_cast<uint8_t>(node.elements.size()));
            } else {
                put8(out, LARGE_TUPLE_EXT);
                put32(out, static_cast<uint32_t>(node.elements.size()));
            }
            for (const auto& e : node.elements)
                encodeTerm(out, e);
        } else if constexpr (std::is_same_v<T, Term::List>) {
            if (node.elements.empty()) {
                put8(out, NIL_EXT);
            } else {
                put8(out, LIST_EXT);
                put32(out, static_cast<uint32_t>(node.elements.size()));
                for (const auto& e : node.elements)
                    encodeTerm(out, e);
                put8(out, NIL_EXT); // proper list tail
            }
        } else if constexpr (std::is_same_v<T, Term::Map>) {
            put8(out, MAP_EXT);
            put32(out, static_cast<uint32_t>(node.pairs.size()));
            for (const auto& [k, v] : node.pairs) {
                encodeTerm(out, k);
                encodeTerm(out, v);
            }
        } else if constexpr (std::is_same_v<T, Term::Nil>) {
            put8(out, NIL_EXT);
        }
    }, term->value);
}

struct Decoder {
    const uint8_t* data;
    size_t len;
    size_t pos = 0;

    auto remaining() const -> size_t { return len - pos; }

    auto read8() -> uint8_t {
        if (pos >= len) throw EtfError("unexpected end of ETF data");
        return data[pos++];
    }
    auto read16() -> uint16_t {
        if (remaining() < 2) throw EtfError("unexpected end of ETF data");
        uint16_t v = (uint16_t(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        return v;
    }
    auto read32() -> uint32_t {
        if (remaining() < 4) throw EtfError("unexpected end of ETF data");
        uint32_t v = (uint32_t(data[pos]) << 24) | (uint32_t(data[pos+1]) << 16) |
                     (uint32_t(data[pos+2]) << 8) | data[pos+3];
        pos += 4;
        return v;
    }
    auto readBytes(size_t n) -> std::vector<uint8_t> {
        if (remaining() < n) throw EtfError("unexpected end of ETF data");
        std::vector<uint8_t> v(data + pos, data + pos + n);
        pos += n;
        return v;
    }
    auto readString(size_t n) -> std::string {
        if (remaining() < n) throw EtfError("unexpected end of ETF data");
        std::string s(reinterpret_cast<const char*>(data + pos), n);
        pos += n;
        return s;
    }

    auto decodeTerm() -> TermPtr {
        uint8_t tag = read8();
        switch (tag) {
        case SMALL_INTEGER_EXT:
            return Term::integer(read8());
        case INTEGER_EXT: {
            auto raw = read32();
            return Term::integer(static_cast<int32_t>(raw));
        }
        case SMALL_BIG_EXT: {
            uint8_t n = read8();
            uint8_t sign = read8();
            uint64_t val = 0;
            for (int i = 0; i < n; i++)
                val |= uint64_t(read8()) << (8 * i);
            int64_t result = static_cast<int64_t>(val);
            if (sign) result = -result;
            return Term::integer(result);
        }
        case ATOM_EXT: {
            auto len = read16();
            return Term::atom(readString(len));
        }
        case SMALL_ATOM_EXT: {
            auto len = read8();
            return Term::atom(readString(len));
        }
        case ATOM_UTF8_EXT: {
            auto len = read16();
            return Term::atom(readString(len));
        }
        case SMALL_ATOM_UTF8_EXT: {
            auto len = read8();
            return Term::atom(readString(len));
        }
        case SMALL_TUPLE_EXT: {
            uint8_t arity = read8();
            std::vector<TermPtr> elems;
            elems.reserve(arity);
            for (int i = 0; i < arity; i++)
                elems.push_back(decodeTerm());
            return Term::tuple(std::move(elems));
        }
        case LARGE_TUPLE_EXT: {
            uint32_t arity = read32();
            std::vector<TermPtr> elems;
            elems.reserve(arity);
            for (uint32_t i = 0; i < arity; i++)
                elems.push_back(decodeTerm());
            return Term::tuple(std::move(elems));
        }
        case NIL_EXT:
            return Term::nil();
        case LIST_EXT: {
            uint32_t count = read32();
            std::vector<TermPtr> elems;
            elems.reserve(count);
            for (uint32_t i = 0; i < count; i++)
                elems.push_back(decodeTerm());
            decodeTerm(); // tail (should be NIL for proper lists)
            return Term::list(std::move(elems));
        }
        case BINARY_EXT: {
            uint32_t len = read32();
            return Term::binary(readBytes(len));
        }
        case MAP_EXT: {
            uint32_t count = read32();
            std::vector<std::pair<TermPtr, TermPtr>> pairs;
            pairs.reserve(count);
            for (uint32_t i = 0; i < count; i++) {
                auto k = decodeTerm();
                auto v = decodeTerm();
                pairs.emplace_back(std::move(k), std::move(v));
            }
            return Term::map(std::move(pairs));
        }
        default:
            throw EtfError("unsupported ETF tag: " + std::to_string(tag));
        }
    }
};

} // namespace

auto encodeEtf(const TermPtr& term) -> std::vector<uint8_t> {
    std::vector<uint8_t> out;
    out.push_back(ETF_VERSION);
    encodeTerm(out, term);
    return out;
}

auto decodeEtf(const uint8_t* data, size_t len) -> TermPtr {
    if (len < 2) throw EtfError("ETF data too short");
    if (data[0] != ETF_VERSION)
        throw EtfError("unsupported ETF version: " + std::to_string(data[0]));
    Decoder dec{data, len, 1};
    return dec.decodeTerm();
}

auto decodeEtf(const std::vector<uint8_t>& bytes) -> TermPtr {
    return decodeEtf(bytes.data(), bytes.size());
}

} // namespace kex::beam
