#include "../evaluator.hxx"
#include "../../lexer/lexer.hxx"
#include "../../parser/parser.hxx"

namespace kex::interpreter {

// Helper: extract an integer field from an EvaluatorOptions record
static auto getIntField(const RecordValue& rec, const std::string& name, int64_t def) -> int64_t {
    auto it = rec.fields.find(name);
    if (it == rec.fields.end()) return def;
    if (auto* iv = std::get_if<IntValue>(&it->second->data)) return iv->value;
    return def;
}

// Helper: extract the allow list from EvaluatorOptions
static auto getAllowList(const RecordValue& rec) -> std::vector<std::string> {
    auto it = rec.fields.find("allow");
    if (it == rec.fields.end()) return defaultEvalAllowList();
    auto* list = std::get_if<ListValue>(&it->second->data);
    if (!list) return defaultEvalAllowList();
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

// The limits an `Evaluator.run` call asked for: its EvaluatorOptions when it
// passed one, the record's defaults otherwise.
static auto limitsFrom(const std::vector<ValuePtr>& args) -> SandboxLimits {
    SandboxLimits limits;
    limits.allow = defaultEvalAllowList();
    if (args.size() > 1)
        if (auto* rec = std::get_if<RecordValue>(&args[1]->data)) {
            limits.maxSteps = getIntField(*rec, "maxSteps", limits.maxSteps);
            limits.maxDepth = getIntField(*rec, "maxDepth", limits.maxDepth);
            limits.allow = getAllowList(*rec);
        }
    return limits;
}

// Sandboxed eval: a fresh Evaluator, put under `limits` once its prelude is
// loaded, runs `source` — a whole program, or with `exprOnly` one expression.
static auto sandboxedEval(const std::string& source, bool exprOnly,
                          SandboxLimits limits) -> ValuePtr {
    try {
        const std::string filename = "<eval>";
        std::string toParse = exprOnly ? ("main do\n" + source + "\nend") : source;
        Lexer lexer(toParse, filename);
        Parser parser(lexer.tokenizeAll(), filename);
        auto program = parser.parseProgram();

        Evaluator eval;
        eval.setSandbox(std::move(limits));
        auto result = eval.execute(program);
        return Value::ok(result ? result : Value::none());
    } catch (const std::exception& e) {
        return Value::error(Value::string(std::string("Runtime error: ") + e.what()));
    }
}

// `Evaluator.run(source[, opts])`, or with `exprOnly` `runExpression`.
static auto evaluatorEntry(const std::string& function, bool exprOnly) -> NativeFunc {
    return [function, exprOnly](std::vector<ValuePtr> args) -> ValuePtr {
        auto* source = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!source)
            return Value::error(Value::string(function + " requires a source string"));
        return sandboxedEval(source->value, exprOnly, limitsFrom(args));
    };
}

auto Evaluator::registerEvalBuiltins() -> void {
    defineModule("Evaluator");
    defineIntrinsic("Evaluator::run", evaluatorEntry("Evaluator.run", false));
    defineIntrinsic("Evaluator::runExpression",
                    evaluatorEntry("Evaluator.runExpression", true));
}

} // namespace kex::interpreter
