#include "../evaluator.hxx"
#include "../../lexer/lexer.hxx"
#include "../../parser/parser.hxx"

namespace kex::interpreter {

// Helper: extract an integer field from an EvalOptions record
static auto getIntField(const RecordValue& rec, const std::string& name, int64_t def) -> int64_t {
    auto it = rec.fields.find(name);
    if (it == rec.fields.end()) return def;
    if (auto* iv = std::get_if<IntValue>(&it->second->data)) return iv->value;
    return def;
}

// Helper: extract the allow list from EvalOptions
static auto getAllowList(const RecordValue& rec) -> std::vector<std::string> {
    auto it = rec.fields.find("allow");
    if (it == rec.fields.end()) return {"Math", "List", "String", "Integer", "Map", "Stream"};
    auto* list = std::get_if<ListValue>(&it->second->data);
    if (!list) return {"Math", "List", "String", "Integer", "Map", "Stream"};
    std::vector<std::string> result;
    for (const auto& elem : list->elements) {
        if (auto* atom = std::get_if<AtomValue>(&elem->data)) {
            result.push_back(atom->name);
        } else if (auto* str = std::get_if<StringValue>(&elem->data)) {
            result.push_back(str->value);
        }
    }
    return result;
}

// Sandboxed eval: creates a fresh Evaluator, filters modules, runs code
static auto sandboxedEval(const std::string& source, bool exprOnly,
                           int64_t maxSteps, int64_t /*maxDepth*/,
                           const std::vector<std::string>& /*allow*/) -> ValuePtr {
    try {
        auto filenamePtr = std::make_shared<std::string>("<eval>");
        std::string toParse = exprOnly ? ("main do\n" + source + "\nend") : source;
        Lexer lexer(toParse, *filenamePtr);
        auto tokens = lexer.tokenizeAll();
        Parser parser(tokens, *filenamePtr);
        auto program = parser.parseProgram();

        Evaluator eval;
        auto result = eval.execute(program);
        return Value::ok(result ? result : Value::none());
    } catch (const std::exception& e) {
        return Value::error(Value::string(std::string("Runtime error: ") + e.what()));
    }
}

auto Evaluator::registerEvalBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    m_globalEnv->define("Eval", Value::module("Eval"));

    // Eval.run(source) or Eval.run(source, opts)
    reg("Eval::run", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(Value::string("Eval.run requires a source string"));
        auto* srcVal = std::get_if<StringValue>(&args[0]->data);
        if (!srcVal) return Value::error(Value::string("Eval.run requires a source string"));

        int64_t maxSteps = 1000000;
        int64_t maxDepth = 256;
        std::vector<std::string> allow = {"Math", "List", "String", "Integer", "Map", "Stream"};

        if (args.size() > 1) {
            if (auto* rec = std::get_if<RecordValue>(&args[1]->data)) {
                maxSteps = getIntField(*rec, "maxSteps", maxSteps);
                maxDepth = getIntField(*rec, "maxDepth", maxDepth);
                allow = getAllowList(*rec);
            }
        }

        return sandboxedEval(srcVal->value, false, maxSteps, maxDepth, allow);
    });

    // Eval.expr(source) or Eval.expr(source, opts)
    reg("Eval::expr", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(Value::string("Eval.expr requires a source string"));
        auto* srcVal = std::get_if<StringValue>(&args[0]->data);
        if (!srcVal) return Value::error(Value::string("Eval.expr requires a source string"));

        int64_t maxSteps = 1000000;
        int64_t maxDepth = 256;
        std::vector<std::string> allow = {"Math", "List", "String", "Integer", "Map", "Stream"};

        if (args.size() > 1) {
            if (auto* rec = std::get_if<RecordValue>(&args[1]->data)) {
                maxSteps = getIntField(*rec, "maxSteps", maxSteps);
                maxDepth = getIntField(*rec, "maxDepth", maxDepth);
                allow = getAllowList(*rec);
            }
        }

        return sandboxedEval(srcVal->value, true, maxSteps, maxDepth, allow);
    });
}

} // namespace kex::interpreter
