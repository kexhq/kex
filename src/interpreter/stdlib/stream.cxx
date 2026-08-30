#include "../evaluator.hxx"
#include "lazy.hxx"

namespace kex::interpreter {

// Stream operations — pure, lazy, and reusable.
//
// Every operation here is O(1) in the elements it does not need, and walking a
// stream twice costs one walk: the cells memoise (see StreamCell in value.hxx,
// which also records what the index-addressed generator this replaced got
// wrong). `take` is the only materialiser.
auto Evaluator::registerStreamBuiltins() -> void {
    defineIntrinsic("Stream::generate", [](std::vector<ValuePtr> args) -> ValuePtr {
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
        return lazy::streamValue(lazy::sequenceFrom(seed, stepFn->native));
    });

    // Stream.empty — the stream with no elements, which is just a null chain.
    defineIntrinsic("Stream::empty", [](std::vector<ValuePtr>) -> ValuePtr {
        return lazy::streamValue(nullptr);
    });

    defineIntrinsic("Stream::take", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* stream = std::get_if<StreamValue>(&args[0]->data);
        auto* n = std::get_if<IntValue>(&args[1]->data);
        if (!stream || !n) return Value::list({});
        std::vector<ValuePtr> result;
        auto at = stream->cell;
        for (int64_t i = 0; i < n->value; i++) {
            // A null answer means the stream ended: take what there is rather
            // than padding the list out to `n`.
            auto* forced = forceStream(at);
            if (!forced) break;
            result.push_back(forced->head);
            at = forced->tail;
        }
        return Value::list(std::move(result));
    });

    // Stream.drop(n) — the rest of the chain, still lazy. The receiver is
    // untouched, so a dropped stream and the one it came from share the cells
    // they have in common rather than recomputing them.
    defineIntrinsic("Stream::drop", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* stream = std::get_if<StreamValue>(&args[0]->data);
        if (!stream) return Value::list({});
        auto* n = std::get_if<IntValue>(&args[1]->data);
        if (!n) return args[0];
        auto at = stream->cell;
        for (int64_t i = 0; i < n->value; i++) {
            auto* forced = forceStream(at);
            if (!forced) { at = nullptr; break; }
            at = forced->tail;
        }
        return lazy::streamValue(std::move(at));
    });

    defineIntrinsic("Stream::map", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* stream = std::get_if<StreamValue>(&args[0]->data);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!stream || !fn || !fn->native) return Value::list({});
        return lazy::streamValue(lazy::mapStream(stream->cell, fn->native));
    });

    defineIntrinsic("Stream::filter", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto* stream = std::get_if<StreamValue>(&args[0]->data);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!stream || !fn || !fn->native) return Value::list({});
        return lazy::streamValue(lazy::filterStream(stream->cell, fn->native));
    });

    // Stream.each(f) — forces the whole chain. Only ever finite for a stream
    // that ends (a file's lines); on `Stream.Sequence` it deliberately runs
    // forever, exactly as writing the same loop by hand would.
    defineIntrinsic("Stream::each", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::unit();
        auto* stream = std::get_if<StreamValue>(&args[0]->data);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!stream || !fn || !fn->native) return Value::unit();
        for (auto at = stream->cell; auto* forced = forceStream(at); at = forced->tail)
            fn->native({forced->head});
        return Value::unit();
    });

    // Stream.toFeed — hands the chain to a one-shot cursor. The point is to
    // stop retaining: a feed drawn from a stream walks it without anything
    // holding the head, so the prefix is released as it goes.
    defineIntrinsic("Stream::toFeed", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* stream = std::get_if<StreamValue>(&args[0]->data);
        if (!stream) return Value::none();
        auto at = std::make_shared<StreamCellPtr>(stream->cell);
        return lazy::feedValue([at]() -> ValuePtr {
            auto* forced = forceStream(*at);
            if (!forced) return nullptr;
            // Copy the head out before retargeting `*at` — if `*at` is the
            // only owner of `forced`'s cell (the common case, since nothing
            // else keeps the intermediate stream alive), reassigning it
            // destroys the cell `forced` points into, and reading
            // `forced->head` afterward would be a use-after-free.
            auto head = forced->head;
            *at = forced->tail;
            return head;
        });
    });
}

} // namespace kex::interpreter
