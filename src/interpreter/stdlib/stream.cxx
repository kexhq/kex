#include "../evaluator.hxx"
#include <algorithm>

namespace kex::interpreter {

// Stream operations. Must run after registerListBuiltins() — the `take`
// override below wraps the plain List `take` (captured via `origTake`)
// rather than reimplementing it, so list registration must already exist
// in the global environment by the time this runs (enforced by call order
// in Evaluator::registerBuiltins()).
//
// Namespace placeholders — currently static functions are resolved globally.
// Stream.Sequence → just use Sequence directly for now.
auto Evaluator::registerStreamBuiltins() -> void {
    m_globalEnv->define("Stream", Value::module("Stream"));

    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // Stream.Sequence(from: start, step_fn) — creates infinite stream
    reg("Sequence", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        // Find start value and step function regardless of order
        ValuePtr startVal;
        const FunctionValue* stepFn = nullptr;
        for (const auto& arg : args) {
            if (std::get_if<FunctionValue>(&arg->data)) {
                stepFn = std::get_if<FunctionValue>(&arg->data);
            } else if (!startVal) {
                startVal = arg;
            }
        }
        if (!startVal || !stepFn) return Value::none();
        if (!stepFn || !stepFn->native) return Value::none();

        auto start = startVal;
        auto step = stepFn->native;
        auto stream = std::make_shared<Value>();
        stream->data = StreamValue{[start, step](int64_t index) -> ValuePtr {
            auto current = start;
            for (int64_t i = 0; i < index; i++) {
                current = step({current});
            }
            return current;
        }, 0};
        return stream;
    });

    // Stream.Iterate(seed, fn) — like Generate but explicit naming
    reg("Iterate", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto seed = args[0];
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!fn || !fn->native) return Value::none();

        auto step = fn->native;
        auto stream = std::make_shared<Value>();
        stream->data = StreamValue{[seed, step](int64_t index) -> ValuePtr {
            auto current = seed;
            for (int64_t i = 0; i < index; i++) {
                current = step({current});
            }
            return current;
        }, 0};
        return stream;
    });

    // Update take to handle streams
    auto origTake = m_globalEnv->get("take");
    reg("take", [origTake](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        // Stream.take(n)
        if (auto* stream = std::get_if<StreamValue>(&args[0]->data)) {
            auto* n = std::get_if<IntValue>(&args[1]->data);
            if (!n) return Value::list({});
            std::vector<ValuePtr> result;
            for (int64_t i = 0; i < n->value; i++) {
                result.push_back(stream->generator(stream->offset + i));
            }
            return Value::list(std::move(result));
        }
        // List.take(n) — delegate to original
        if (auto* fn = std::get_if<FunctionValue>(&origTake->data)) {
            return fn->native(std::move(args));
        }
        return Value::list({});
    });

    // Stream.drop(n) — returns a new stream with offset
    reg("drop", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        if (auto* stream = std::get_if<StreamValue>(&args[0]->data)) {
            auto* n = std::get_if<IntValue>(&args[1]->data);
            if (!n) return args[0];
            auto newStream = std::make_shared<Value>();
            newStream->data = StreamValue{stream->generator, stream->offset + n->value};
            return newStream;
        }
        auto* n = std::get_if<IntValue>(&args[1]->data);
        if (!n) return Value::list({});
        // String.drop — treat as char sequence
        if (auto* str = std::get_if<StringValue>(&args[0]->data)) {
            auto skip = std::min(static_cast<size_t>(n->value), str->value.size());
            return Value::string(str->value.substr(skip));
        }
        // List.drop
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) return Value::list({});
        auto skip = std::min(static_cast<size_t>(n->value), list->elements.size());
        std::vector<ValuePtr> result(list->elements.begin() + skip, list->elements.end());
        return Value::list(std::move(result));
    });
}

} // namespace kex::interpreter
