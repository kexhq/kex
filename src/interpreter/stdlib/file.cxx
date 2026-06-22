#include "../evaluator.hxx"
#include <filesystem>
#include <fstream>

namespace kex::interpreter {

// Filesystem-specific operations; whole-file read/write live under IO
// (see io.cxx) since those are general-purpose IO, not filesystem-only.
//
// NOTE: these still return None/Bool rather than Result<T, IOError>.
// The ADT runtime gap that originally blocked Result (constructor calls,
// constructor-pattern matching with args, and `?` short-circuiting) is
// now implemented (see the TypeDef variant-constructor registration in
// execTopLevel, the ConstructorPattern branch in matchPattern, and the
// ErrorPropagate branch in eval()) — so File/IO *could* be upgraded to
// return Result<T, IOError> now. Left as None/Bool for this iteration to
// avoid changing already-shipped behavior/tests; revisit deliberately.
//
// Not implemented: File.open(path) do |handle| ... end (resource-block
// pattern from streams.kex) — no file-handle value kind exists yet.
auto Evaluator::registerFileBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // Pre-register the namespace placeholder so `File.append(...)` resolves
    // via the empty-RecordValue namespace-dispatch branch in eval()
    // (ast::MethodCall) and gets the mangled "File::" dispatch.
    m_globalEnv->define("File", Value::record("File", {}));

    reg("File::append", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        std::string content = args[1]->toString();
        std::ofstream file(pathStr->value, std::ios::binary | std::ios::app);
        if (!file.is_open()) return Value::boolean(false);
        file << content;
        return Value::boolean(static_cast<bool>(file));
    });

    reg("File::exists?", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        std::error_code ec;
        return Value::boolean(std::filesystem::exists(pathStr->value, ec));
    });

    reg("File::delete", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        std::error_code ec;
        bool removed = std::filesystem::remove(pathStr->value, ec);
        return Value::boolean(removed && !ec);
    });

    reg("File::lines", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        std::ifstream file(pathStr->value, std::ios::binary);
        if (!file.is_open()) return Value::none();
        std::vector<ValuePtr> result;
        std::string line;
        while (std::getline(file, line)) {
            result.push_back(Value::string(line));
        }
        if (file.bad()) return Value::none();
        return Value::list(std::move(result));
    });

    // File.feed(path) -> Stream<String> | None
    //
    // NOTE: this reads the whole file eagerly into a vector<string> up front,
    // then wraps it in a StreamValue with index-based lookup. It is NOT a true
    // single-consumption Feed<A> — the resulting Stream looks pure/reusable
    // (calling .take(3) twice gives the same result) despite representing a
    // side-effecting read captured at call time. A real Feed<A> should be a
    // distinct Value kind with single-consumption semantics — future work.
    //
    // End-of-stream convention: returns None once `index` is past the
    // captured line count. NOTE: generic stream consumers (e.g. the `take`
    // builtin in stream.cxx) don't check for a None sentinel and will call
    // the generator past the end regardless, so File.feed(path).take(n) with
    // n greater than the line count pads the result with None values rather
    // than stopping early — a pre-existing limitation of the Stream consumer
    // protocol, not specific to File.feed.
    reg("File::feed", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        auto lines = std::make_shared<std::vector<std::string>>();
        {
            std::ifstream file(pathStr->value, std::ios::binary);
            if (!file.is_open()) return Value::none();
            std::string line;
            while (std::getline(file, line)) {
                lines->push_back(line);
            }
            if (file.bad()) return Value::none();
        }
        auto stream = std::make_shared<Value>();
        stream->data = StreamValue{[lines](int64_t index) -> ValuePtr {
            if (index < 0 || static_cast<size_t>(index) >= lines->size()) {
                return Value::none();
            }
            return Value::string((*lines)[index]);
        }, 0};
        return stream;
    });
}

} // namespace kex::interpreter
