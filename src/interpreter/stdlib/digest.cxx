#include "../../beam/kexi.hxx"
#include "../evaluator.hxx"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace kex::interpreter {

namespace {

auto sha256Hex(const std::vector<uint8_t>& bytes) -> std::string {
    const auto digest = beam::computeSha256(bytes);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest) out << std::setw(2) << static_cast<int>(byte);
    return out.str();
}

} // namespace

auto Evaluator::registerDigestBuiltins() -> void {
    defineModule("Digest");
    defineIntrinsic("Digest::sha256", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string(sha256Hex({}));
        const auto* text = std::get_if<StringValue>(&args[0]->data);
        if (!text) return Value::string(sha256Hex({}));
        return Value::string(sha256Hex(
            std::vector<uint8_t>(text->value.begin(), text->value.end())));
    });
    defineIntrinsic("Digest::fileSha256", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        const auto* path = std::get_if<StringValue>(&args[0]->data);
        if (!path) return Value::none();
        std::ifstream input(path->value, std::ios::binary);
        if (!input) return Value::none();
        std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
        return Value::just(Value::string(sha256Hex(bytes)));
    });
}

} // namespace kex::interpreter
