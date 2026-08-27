#include "../evaluator.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace kex::interpreter {
namespace {

auto binaryArg(const ValuePtr& value) -> const BinaryValue* {
    return value ? std::get_if<BinaryValue>(&value->data) : nullptr;
}

auto integerArg(const ValuePtr& value) -> std::optional<int64_t> {
    if (!value) return std::nullopt;
    if (auto* i = std::get_if<IntValue>(&value->data)) return i->value;
    return std::nullopt;
}

constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

auto encodeBase64(const std::vector<uint8_t>& input) -> std::string {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    for (size_t i = 0; i < input.size(); i += 3) {
        const uint32_t a = input[i];
        const uint32_t b = i + 1 < input.size() ? input[i + 1] : 0;
        const uint32_t c = i + 2 < input.size() ? input[i + 2] : 0;
        const uint32_t n = (a << 16) | (b << 8) | c;
        out += kBase64[(n >> 18) & 63];
        out += kBase64[(n >> 12) & 63];
        out += i + 1 < input.size() ? kBase64[(n >> 6) & 63] : '=';
        out += i + 2 < input.size() ? kBase64[n & 63] : '=';
    }
    return out;
}

auto decodeBase64(const std::string& text) -> std::optional<std::vector<uint8_t>> {
    if (text.size() % 4 != 0) return std::nullopt;
    std::array<int, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i)
        table[static_cast<unsigned char>(kBase64[i])] = i;
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const char c2 = text[i + 2], c3 = text[i + 3];
        if (text[i] == '=' || text[i + 1] == '=' || (!last && (c2 == '=' || c3 == '=')))
            return std::nullopt;
        const int a = table[static_cast<unsigned char>(text[i])];
        const int b = table[static_cast<unsigned char>(text[i + 1])];
        const int c = c2 == '=' ? 0 : table[static_cast<unsigned char>(c2)];
        const int d = c3 == '=' ? 0 : table[static_cast<unsigned char>(c3)];
        if (a < 0 || b < 0 || c < 0 || d < 0) return std::nullopt;
        if (c2 == '=' && c3 != '=') return std::nullopt;
        const uint32_t n = (a << 18) | (b << 12) | (c << 6) | d;
        out.push_back(static_cast<uint8_t>(n >> 16));
        if (c2 != '=') out.push_back(static_cast<uint8_t>(n >> 8));
        if (c3 != '=') out.push_back(static_cast<uint8_t>(n));
        if ((c2 == '=' && (b & 15) != 0) || (c3 == '=' && c2 != '=' && (c & 3) != 0))
            return std::nullopt;
    }
    if (encodeBase64(out) != text) return std::nullopt;
    return out;
}

} // namespace

auto Evaluator::registerBinaryBuiltins() -> void {
    defineModule("Binary");
    defineModule("Byte");
    defineIntrinsic("Binary::fromBytes", [](std::vector<ValuePtr> args) -> ValuePtr {
        std::vector<uint8_t> bytes;
        if (args.empty()) return Value::binary({});
        const auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) throw std::runtime_error("Binary.fromBytes expected [Byte]");
        bytes.reserve(list->elements.size());
        for (const auto& value : list->elements) {
            const auto byte = integerArg(value);
            if (!byte || *byte < 0 || *byte > 255)
                throw std::runtime_error("internal Byte invariant violation");
            bytes.push_back(static_cast<uint8_t>(*byte));
        }
        return Value::binary(std::move(bytes));
    });
    defineIntrinsic("Binary::bytes", [](std::vector<ValuePtr> args) -> ValuePtr {
        std::vector<ValuePtr> values;
        if (!args.empty()) if (const auto* value = binaryArg(args[0])) {
            values.reserve(value->bytes.size());
            for (uint8_t byte : value->bytes) values.push_back(Value::integer(byte));
        }
        return Value::list(std::move(values));
    });
    defineIntrinsic("Binary::length", [](std::vector<ValuePtr> args) -> ValuePtr {
        const auto* value = args.empty() ? nullptr : binaryArg(args[0]);
        return Value::integer(value ? static_cast<int64_t>(value->bytes.size()) : 0);
    });
    defineIntrinsic("Binary::at", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        const auto* value = binaryArg(args[0]);
        const auto index = integerArg(args[1]);
        if (!value || !index || *index < 0 || static_cast<uint64_t>(*index) >= value->bytes.size())
            return Value::none();
        return Value::just(Value::integer(value->bytes[static_cast<size_t>(*index)]));
    });
    auto slice = [](bool take, std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::binary({});
        const auto* value = binaryArg(args[0]);
        const auto count = integerArg(args[1]);
        if (!value || !count) return Value::binary({});
        const size_t n = *count <= 0 ? 0 :
            std::min<size_t>(static_cast<uint64_t>(*count), value->bytes.size());
        if (take) return Value::binary({value->bytes.begin(), value->bytes.begin() + n});
        return Value::binary({value->bytes.begin() + n, value->bytes.end()});
    };
    defineIntrinsic("Binary::take", [slice](std::vector<ValuePtr> args) { return slice(true, std::move(args)); });
    defineIntrinsic("Binary::drop", [slice](std::vector<ValuePtr> args) { return slice(false, std::move(args)); });
    defineIntrinsic("Binary::concat", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::binary({});
        const auto* left = binaryArg(args[0]);
        const auto* right = binaryArg(args[1]);
        if (!left || !right) return Value::binary({});
        auto bytes = left->bytes;
        bytes.insert(bytes.end(), right->bytes.begin(), right->bytes.end());
        return Value::binary(std::move(bytes));
    });
    defineIntrinsic("Binary::hex", [](std::vector<ValuePtr> args) -> ValuePtr {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        const auto* value = args.empty() ? nullptr : binaryArg(args[0]);
        if (value) for (uint8_t byte : value->bytes) {
            out += digits[byte >> 4]; out += digits[byte & 15];
        }
        return Value::string(std::move(out));
    });
    defineIntrinsic("Binary::fromHex", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        const auto* text = std::get_if<StringValue>(&args[0]->data);
        if (!text || text->value.size() % 2) return Value::none();
        std::vector<uint8_t> out;
        for (size_t i = 0; i < text->value.size(); i += 2) {
            auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
            const int hi = digit(text->value[i]), lo = digit(text->value[i + 1]);
            if (hi < 0 || lo < 0) return Value::none();
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return Value::just(Value::binary(std::move(out)));
    });
    defineIntrinsic("Binary::base64", [](std::vector<ValuePtr> args) -> ValuePtr {
        const auto* value = args.empty() ? nullptr : binaryArg(args[0]);
        return Value::string(value ? encodeBase64(value->bytes) : "");
    });
    defineIntrinsic("Binary::fromBase64", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        const auto* text = std::get_if<StringValue>(&args[0]->data);
        if (!text) return Value::none();
        auto bytes = decodeBase64(text->value);
        return bytes ? Value::just(Value::binary(std::move(*bytes))) : Value::none();
    });
    defineIntrinsic("Binary::render", [](std::vector<ValuePtr> args) -> ValuePtr {
        const auto* value = args.empty() ? nullptr : binaryArg(args[0]);
        return Value::string("#Binary<" + std::to_string(value ? value->bytes.size() : 0) + " bytes>");
    });
}

} // namespace kex::interpreter
