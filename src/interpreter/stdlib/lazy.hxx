#pragma once

// Combinators shared by the two lazy sequence types.
//
// `Stream<A>` is pure: a chain of memoising cells, walkable any number of
// times, and infinite whenever its source is. `Feed<A>` is one-shot: pulling
// consumes, nothing is retained, and it can front a source larger than memory.
// The two differ in exactly that — retention — so the combinators below come
// in matched pairs rather than sharing one implementation.

#include "../value.hxx"

namespace kex::interpreter::lazy {

// ---------------------------------------------------------------- streams

// The cell for the element after `previous`, computing it on force.
inline auto sequenceAfter(ValuePtr previous, NativeFunc step) -> StreamCellPtr {
    auto cell = std::make_shared<StreamCell>();
    cell->thunk = [previous = std::move(previous), step]()
        -> std::pair<ValuePtr, StreamCellPtr> {
        auto value = step({previous});
        auto tail = sequenceAfter(value, step);
        return {std::move(value), std::move(tail)};
    };
    return cell;
}

// `seed`, `step(seed)`, `step(step(seed))`, ... Each `step` runs once, when
// the element it produces is first asked for.
inline auto sequenceFrom(ValuePtr seed, NativeFunc step) -> StreamCellPtr {
    auto cell = std::make_shared<StreamCell>();
    cell->thunk = [seed, step]() -> std::pair<ValuePtr, StreamCellPtr> {
        return {seed, sequenceAfter(seed, step)};
    };
    return cell;
}

// A stream over an already-materialised sequence.
inline auto streamOfList(std::shared_ptr<std::vector<ValuePtr>> elements,
                         size_t index = 0) -> StreamCellPtr {
    if (!elements || index >= elements->size()) return nullptr;
    auto cell = std::make_shared<StreamCell>();
    cell->thunk = [elements, index]() -> std::pair<ValuePtr, StreamCellPtr> {
        return {(*elements)[index], streamOfList(elements, index + 1)};
    };
    return cell;
}

inline auto mapStream(StreamCellPtr source, NativeFunc f) -> StreamCellPtr {
    if (!source) return nullptr;
    auto cell = std::make_shared<StreamCell>();
    cell->thunk = [source = std::move(source), f]()
        -> std::pair<ValuePtr, StreamCellPtr> {
        auto* at = forceStream(source);
        if (!at) return {nullptr, nullptr};   // the source ended; so does this
        return {f({at->head}), mapStream(at->tail, f)};
    };
    return cell;
}

inline auto filterStream(StreamCellPtr source, NativeFunc pred) -> StreamCellPtr {
    if (!source) return nullptr;
    auto cell = std::make_shared<StreamCell>();
    cell->thunk = [source = std::move(source), pred]()
        -> std::pair<ValuePtr, StreamCellPtr> {
        // Walk until the predicate holds. There is deliberately no search cap
        // here: the index-addressed filter this replaced had to bound its
        // rescan, and so reported a stream whose matches were merely sparse as
        // ENDED. A finite source now ends when it actually ends, and an
        // infinite one whose predicate never holds runs forever — which is
        // what `Stream.filter`'s documentation has always promised.
        for (auto at = source; auto* forced = forceStream(at); at = forced->tail) {
            if (pred({forced->head})->isTrue())
                return {forced->head, filterStream(forced->tail, pred)};
        }
        return {nullptr, nullptr};
    };
    return cell;
}

inline auto streamValue(StreamCellPtr cell) -> ValuePtr {
    auto value = std::make_shared<Value>();
    value->data = StreamValue{std::move(cell)};
    return value;
}

// ------------------------------------------------------------------ feeds

inline auto feedValue(std::function<ValuePtr()> pull) -> ValuePtr {
    auto value = std::make_shared<Value>();
    value->data = FeedValue{std::make_shared<FeedState>(FeedState{std::move(pull), false})};
    return value;
}

// A feed whose elements are `f` applied to another's. Shares the source's
// state, so pulling through the map consumes the source — feeds are one-shot
// all the way down, and a `map` that left the original drawable would be a
// second cursor over a source that has only one.
inline auto mapFeed(std::shared_ptr<FeedState> source, NativeFunc f) -> ValuePtr {
    return feedValue([source = std::move(source), f]() -> ValuePtr {
        auto value = pullFeed(source);
        return value ? f({value}) : nullptr;
    });
}

// A memoising chain drawn from a feed: each cell pulls one element and leaves
// a cell that will pull the next. Forcing the chain in order is the only way
// the elements come out, which is what keeps a one-shot source coherent behind
// a type that promises replay — the replay comes from the memoised cells, not
// from the source.
inline auto streamOfFeed(std::shared_ptr<FeedState> source) -> StreamCellPtr {
    auto cell = std::make_shared<StreamCell>();
    cell->thunk = [source = std::move(source)]()
        -> std::pair<ValuePtr, StreamCellPtr> {
        auto value = pullFeed(source);
        if (!value) return {nullptr, nullptr};
        return {std::move(value), streamOfFeed(source)};
    };
    return cell;
}

inline auto filterFeed(std::shared_ptr<FeedState> source, NativeFunc pred) -> ValuePtr {
    return feedValue([source = std::move(source), pred]() -> ValuePtr {
        while (auto value = pullFeed(source)) {
            if (pred({value})->isTrue()) return value;
        }
        return nullptr;
    });
}

}  // namespace kex::interpreter::lazy
