#include "../evaluator.hxx"

namespace kex::interpreter {

// Stream operations. Must run after registerListBuiltins() — the `take`
// override below wraps the plain List `take` (captured via `origTake`)
// rather than reimplementing it, so list registration must already exist
// in the global environment by the time this runs (enforced by call order
// in Evaluator::registerBuiltins()).
//
auto Evaluator::registerStreamBuiltins() -> void {
    auto streamMake = [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        ValuePtr seed;
        const FunctionValue* stepFn = nullptr;
        for (const auto& arg : args) {
            if (std::get_if<FunctionValue>(&arg->data))
                stepFn = std::get_if<FunctionValue>(&arg->data);
            else if (!seed)
                seed = arg;
        }
        if (!seed || !stepFn || !stepFn->native) return Value::none();

        auto step = stepFn->native;
        auto stream = std::make_shared<Value>();
        stream->data = StreamValue{[seed, step](int64_t index) -> ValuePtr {
            auto current = seed;
            for (int64_t i = 0; i < index; i++) {
                current = step({current});
            }
            return current;
        }, 0};
        return stream;
    };

    defineIntrinsic("Stream::generate", streamMake);

    defineIntrinsic("Stream::take", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        if (auto* stream = std::get_if<StreamValue>(&args[0]->data)) {
            auto* n = std::get_if<IntValue>(&args[1]->data);
            if (!n) return Value::list({});
            std::vector<ValuePtr> result;
            for (int64_t i = 0; i < n->value; i++) {
                result.push_back(stream->generator(stream->offset + i));
            }
            return Value::list(std::move(result));
        }
        return Value::list({});
    });

    // Stream.drop(n) — returns a new stream with offset
    defineIntrinsic("Stream::drop", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        if (auto* stream = std::get_if<StreamValue>(&args[0]->data)) {
            auto* n = std::get_if<IntValue>(&args[1]->data);
            if (!n) return args[0];
            auto newStream = std::make_shared<Value>();
            newStream->data = StreamValue{stream->generator, stream->offset + n->value};
            return newStream;
        }
        return Value::list({});
    });

    // Stream map/filter share the lazy implementation registered by the List
    // runtime domain, but retain distinct private ABI identities.
    for (const char* name : {"map", "filter"}) {
        if (auto value = m_intrinsicEnv->get("List::" + std::string(name)))
            defineIntrinsic("Stream::" + std::string(name), value);
    }
}

} // namespace kex::interpreter
