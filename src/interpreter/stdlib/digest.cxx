#include "../../common/digest.hxx"
#include "../../common/file_io.hxx"
#include "../evaluator.hxx"

namespace kex::interpreter {

auto Evaluator::registerDigestBuiltins() -> void {
    defineModule("Digest");
    defineIntrinsic("Digest.sha256", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string(sha256Hex(std::string_view{}));
        if (const auto* binary = std::get_if<BinaryValue>(&args[0]->data)) {
            const auto digest = sha256(binary->bytes);
            return Value::binary({digest.begin(), digest.end()});
        }
        const auto* text = std::get_if<StringValue>(&args[0]->data);
        if (!text) return Value::string(sha256Hex(std::string_view{}));
        return Value::string(sha256Hex(text->value));
    });
    defineIntrinsic("Digest.fileSha256", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        const auto* path = std::get_if<StringValue>(&args[0]->data);
        if (!path) return Value::none();
        const auto bytes = readFileBytes(path->value);
        if (!bytes) return Value::none();
        return Value::just(Value::string(sha256Hex(*bytes)));
    });
}

} // namespace kex::interpreter
