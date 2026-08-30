#include "../evaluator.hxx"
#include "lazy.hxx"

namespace kex::interpreter {

// Feed operations — foul, one-shot, and constant-space.
//
// A feed is the counterpart to `Stream<A>` for a source that can only be read
// once: a file, a socket, a device. Everything here CONSUMES. `map` and
// `filter` answer a feed over the same cursor rather than a second cursor over
// the same source, so a pipeline stays a single pass — which is what lets a
// feed front something larger than memory, where a stream, memoising while
// anything holds its head, cannot.
auto Evaluator::registerFeedBuiltins() -> void {
    auto feedOf = [](const ValuePtr& v) -> std::shared_ptr<FeedState> {
        auto* feed = std::get_if<FeedValue>(&v->data);
        return feed ? feed->state : nullptr;
    };

    // Feed.empty — a feed that is spent from the start.
    defineIntrinsic("Feed::empty", [](std::vector<ValuePtr>) -> ValuePtr {
        return lazy::feedValue(nullptr);
    });

    // Feed.Elements(xs) — a feed over an already-materialised list. For a
    // source that is a list all along (a test fake standing in for a file),
    // so that what it hands back consumes like the real thing.
    defineIntrinsic("Feed::elements", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return lazy::feedValue(nullptr);
        auto* list = std::get_if<ListValue>(&args[0]->data);
        if (!list) return lazy::feedValue(nullptr);
        auto elements = std::make_shared<std::vector<ValuePtr>>(list->elements);
        auto index = std::make_shared<size_t>(0);
        return lazy::feedValue([elements, index]() -> ValuePtr {
            if (*index >= elements->size()) return nullptr;
            return (*elements)[(*index)++];
        });
    });

    // Feed.pull -> A? — the defining operation: answers the next element and
    // consumes it, or None once the source is spent.
    //
    // NOT `next`: that is the loop-continue keyword, and the lexer takes it as
    // one even in member position, so `Kex.Intrinsic.Feed.next(this)` silently
    // swallowed the whole enclosing `make` block.
    defineIntrinsic("Feed::pull", [feedOf](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto state = feedOf(args[0]);
        if (!state) return Value::none();
        auto value = pullFeed(state);
        return value ? Value::just(value) : Value::none();
    });

    // Feed.spent? -> Bool — whether the source has run out. False does not
    // promise another element, only that the feed has not yet been told
    // otherwise; the source is asked, and may end, on the next pull.
    defineIntrinsic("Feed::spent?", [feedOf](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(true);
        auto state = feedOf(args[0]);
        return Value::boolean(!state || state->spent);
    });

    // Feed.take(n) -> [A] — the next n elements, consumed. Unlike the stream
    // operation of the same name, taking twice answers two DIFFERENT windows:
    // the first n elements, then the n after them.
    defineIntrinsic("Feed::take", [feedOf](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::list({});
        auto state = feedOf(args[0]);
        auto* n = std::get_if<IntValue>(&args[1]->data);
        if (!state || !n) return Value::list({});
        std::vector<ValuePtr> result;
        for (int64_t i = 0; i < n->value; i++) {
            auto value = pullFeed(state);
            if (!value) break;
            result.push_back(std::move(value));
        }
        return Value::list(std::move(result));
    });

    // Feed.drop(n) -> Feed<A> — discards n elements and answers the same feed,
    // since there is only ever the one cursor to hand back.
    defineIntrinsic("Feed::drop", [feedOf](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return args.empty() ? Value::none() : args[0];
        auto state = feedOf(args[0]);
        auto* n = std::get_if<IntValue>(&args[1]->data);
        if (!state || !n) return args[0];
        for (int64_t i = 0; i < n->value && pullFeed(state); i++) {}
        return args[0];
    });

    defineIntrinsic("Feed::map", [feedOf](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto state = feedOf(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!state || !fn || !fn->native) return Value::none();
        return lazy::mapFeed(state, fn->native);
    });

    defineIntrinsic("Feed::filter", [feedOf](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::none();
        auto state = feedOf(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!state || !fn || !fn->native) return Value::none();
        return lazy::filterFeed(state, fn->native);
    });

    // Feed.each(f) — drains the feed. Terminates for any real source, since a
    // feed ends when its source does.
    defineIntrinsic("Feed::each", [feedOf](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::unit();
        auto state = feedOf(args[0]);
        auto* fn = std::get_if<FunctionValue>(&args[1]->data);
        if (!state || !fn || !fn->native) return Value::unit();
        while (auto value = pullFeed(state)) fn->native({value});
        return Value::unit();
    });

    // Feed.collect -> [A] — drains the feed into a list. The operation that
    // gives up on constant space, so it is named rather than implicit.
    defineIntrinsic("Feed::collect", [feedOf](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::list({});
        auto state = feedOf(args[0]);
        if (!state) return Value::list({});
        std::vector<ValuePtr> result;
        while (auto value = pullFeed(state)) result.push_back(std::move(value));
        return Value::list(std::move(result));
    });

    // Feed.toStream -> Stream<A> — a memoising chain drawn from the feed, for
    // a source small enough to be worth replaying. This is the one direction
    // that trades space for reusability: the resulting stream retains every
    // element forced through it.
    defineIntrinsic("Feed::toStream", [feedOf](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return lazy::streamValue(nullptr);
        auto state = feedOf(args[0]);
        if (!state) return lazy::streamValue(nullptr);
        return lazy::streamValue(lazy::streamOfFeed(state));
    });
}

} // namespace kex::interpreter
