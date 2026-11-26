#include "../evaluator.hxx"
#include "../../lexer/lexer.hxx"
#include "../../parser/parser.hxx"
#include "../../common/runtime_dir.hxx"
#include "../../driver/dependencies.hxx"
#include "../../beam/etf.hxx"
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>

namespace kex::interpreter {

// Helper: extract an integer field from an EvaluatorOptions record
static auto getIntField(const RecordValue& rec, const std::string& name, int64_t def) -> int64_t {
    auto it = rec.fields.find(name);
    if (it == rec.fields.end()) {
        return def;
    }
    if (auto* iv = std::get_if<IntValue>(&it->second->data)) {
        return iv->value;
    }
    return def;
}

// Helper: extract the allow list from EvaluatorOptions
static auto getAllowList(const RecordValue& rec) -> std::vector<std::string> {
    auto it = rec.fields.find("allow");
    if (it == rec.fields.end()) {
        return defaultEvalAllowList();
    }
    auto* list = std::get_if<ListValue>(&it->second->data);
    if (!list) {
        return defaultEvalAllowList();
    }
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
    if (args.size() > 1) {
        if (auto* rec = std::get_if<RecordValue>(&args[1]->data)) {
            limits.maxSteps = getIntField(*rec, "maxSteps", limits.maxSteps);
            limits.maxDepth = getIntField(*rec, "maxDepth", limits.maxDepth);
            limits.allow = getAllowList(*rec);
        }
    }
    return limits;
}

// Sandboxed eval: a fresh Evaluator, put under `limits` once its prelude is
// loaded, runs `source` — a whole program, or with `exprOnly` one expression.
// `finish`, when given, sees the result while the evaluator that made it is
// still alive (what --eval-etf needs to read its record declarations).
static auto sandboxedEval(
    const std::string& source,
    bool exprOnly,
    SandboxLimits limits,
    const std::function<void(const Evaluator&, const ValuePtr&)>& finish = {}) -> ValuePtr {
    try {
        const std::string filename = "<eval>";
        std::string toParse = exprOnly ? ("main do\n" + source + "\nend") : source;
        Lexer lexer(toParse, filename);
        Parser parser(lexer.tokenizeAll(), filename);
        auto program = parser.parseProgram();
        // The parser recovers past an error, so a program that did not parse
        // still comes back, often empty: running it would answer Ok(None).
        if (!parser.diagnostics().empty()) {
            const auto& diagnostic = parser.diagnostics().front();
            // `runExpression` wraps the source in one leading line.
            const int line = std::max(diagnostic.location.line - (exprOnly ? 1 : 0), 1);
            return Value::error(
                Value::string(
                    "Parse error at " + std::to_string(line) + ":" +
                    std::to_string(diagnostic.location.column) + ": " + diagnostic.message));
        }
        // Checked first, as the walker runs every program.
        const auto checked =
            driver::prepareForWalker(program, {}, &defaultPreludeInterfaces(), filename);

        Evaluator eval;
        eval.setSandbox(std::move(limits));
        eval.setProgram(&*checked->program);
        auto result = eval.execute(program);
        auto answer = Value::ok(result ? result : Value::none());
        if (finish) {
            finish(eval, answer);
        }
        return answer;
    } catch (const std::exception& e) {
        return Value::error(Value::string(std::string("Runtime error: ") + e.what()));
    }
}

// `Evaluator.run(source[, opts])`, or with `exprOnly` `runExpression`.
static auto evaluatorEntry(const std::string& function, bool exprOnly) -> NativeFunc {
    return [function, exprOnly](std::vector<ValuePtr> args) -> ValuePtr {
        auto* source = args.empty() ? nullptr : std::get_if<StringValue>(&args[0]->data);
        if (!source) {
            return Value::error(Value::string(function + " requires a source string"));
        }
        return sandboxedEval(source->value, exprOnly, limitsFrom(args));
    };
}

auto Evaluator::registerEvalBuiltins() -> void {
    defineIntrinsic("Evaluator.run", evaluatorEntry("Evaluator.run", false));
    defineIntrinsic("Evaluator.runExpression", evaluatorEntry("Evaluator.runExpression", true));
}

namespace {

// A walker value as the term a BEAM program holds for it: a record is its
// name's atom then its fields in declaration order, a variant its tag (alone
// when it carries nothing), a Char `{'Char', C}`, a Binary `{'Binary', B}`, a
// range `{'Range', A, B[, Step]}`, Void `'Kex.Unit'`. Functions, processes and
// streams belong to the evaluator that made them and do not cross.
auto valueToTerm(const ValuePtr& value, const Evaluator& owner) -> beam::TermPtr {
    using beam::Term;
    if (!value) {
        return Term::atom("None");
    }
    const auto all = [&](const std::vector<ValuePtr>& values) {
        std::vector<beam::TermPtr> terms;
        terms.reserve(values.size());
        for (const auto& item : values) {
            terms.push_back(valueToTerm(item, owner));
        }
        return terms;
    };
    return std::visit(
        [&](const auto& node) -> beam::TermPtr {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, UnitValue>) {
                return Term::atom("Kex.Unit");
            } else if constexpr (std::is_same_v<T, IntValue>) {
                return Term::integer(node.value);
            } else if constexpr (std::is_same_v<T, BigIntValue>) {
                return Term::integer(node.value);
            } else if constexpr (std::is_same_v<T, FloatValue>) {
                return Term::floating(node.value);
            } else if constexpr (std::is_same_v<T, StringValue>) {
                return Term::binary(node.value);
            } else if constexpr (std::is_same_v<T, BinaryValue>) {
                return Term::tuple({Term::atom("Binary"), Term::binary(node.bytes)});
            } else if constexpr (std::is_same_v<T, CharValue>) {
                return Term::tuple(
                    {Term::atom("Char"), Term::integer(static_cast<int64_t>(node.value))});
            } else if constexpr (std::is_same_v<T, BoolValue>) {
                return Term::boolean(node.value);
            } else if constexpr (std::is_same_v<T, AtomValue>) {
                return Term::atom(node.name);
            } else if constexpr (std::is_same_v<T, VariantValue>) {
                if (node.args.empty()) {
                    return Term::atom(node.tag);
                }
                auto elements = all(node.args);
                elements.insert(elements.begin(), Term::atom(node.tag));
                return Term::tuple(std::move(elements));
            } else if constexpr (std::is_same_v<T, ListValue>) {
                return Term::list(all(node.elements));
            } else if constexpr (std::is_same_v<T, TupleValue>) {
                return Term::tuple(all(node.elements));
            } else if constexpr (std::is_same_v<T, MapValue>) {
                std::vector<std::pair<beam::TermPtr, beam::TermPtr>> pairs;
                pairs.reserve(node.entries.size());
                for (const auto& [key, item] : node.entries) {
                    pairs.emplace_back(valueToTerm(key, owner), valueToTerm(item, owner));
                }
                return Term::map(std::move(pairs));
            } else if constexpr (std::is_same_v<T, RangeValue>) {
                const auto bound = [&](const ValuePtr& exact, int64_t plain) {
                    if (exact) {
                        return valueToTerm(exact, owner);
                    }
                    return node.isChar ? Term::tuple({Term::atom("Char"), Term::integer(plain)})
                                       : Term::integer(plain);
                };
                std::vector<beam::TermPtr> elements{
                    Term::atom("Range"), bound(node.lower, node.start),
                    bound(node.upper, node.end)};
                if (node.step != 1) {
                    elements.push_back(Term::integer(node.step));
                }
                return Term::tuple(std::move(elements));
            } else if constexpr (std::is_same_v<T, RecordValue>) {
                std::vector<beam::TermPtr> elements{Term::atom(node.typeName)};
                for (const auto& name : owner.recordFieldNames(node.typeName)) {
                    const auto field = node.fields.find(name);
                    elements.push_back(
                        field != node.fields.end() ? valueToTerm(field->second, owner)
                                                   : Term::atom("None"));
                }
                return Term::tuple(std::move(elements));
            } else {
                throw std::runtime_error(
                    "a " + value->typeName() + " value cannot leave the evaluator on BEAM");
            }
        },
        value->data);
}

auto readFileBytes(const std::string& path) -> std::optional<std::vector<uint8_t>> {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

} // namespace

auto runEvaluatorRequest(const std::string& requestPath) -> int {
    using beam::Term;
    const auto bytes = readFileBytes(requestPath);
    if (!bytes) {
        std::cerr << "error: cannot read evaluator request: " << requestPath << "\n";
        return 1;
    }
    beam::TermPtr request;
    try {
        request = beam::decodeEtf(*bytes);
    } catch (const std::exception& e) {
        std::cerr << "error: invalid evaluator request: " << e.what() << "\n";
        return 1;
    }
    const auto source = request ? request->mapGet("source") : nullptr;
    const auto output = request ? request->mapGet("output") : nullptr;
    if (!source || !output) {
        std::cerr << "error: an evaluator request needs a source and an output\n";
        return 1;
    }
    const auto expression = request->mapGet("expression");
    SandboxLimits limits;
    limits.allow = defaultEvalAllowList();
    if (const auto allow = request->mapGet("allow")) {
        limits.allow.clear();
        for (const auto& item : allow->asList()) {
            limits.allow.push_back(item->isAtom() ? item->asAtom() : item->asBinaryStr());
        }
    }
    if (const auto steps = request->mapGet("maxSteps")) {
        limits.maxSteps = steps->asInt();
    }
    if (const auto depth = request->mapGet("maxDepth")) {
        limits.maxDepth = depth->asInt();
    }

    beam::TermPtr answer;
    const auto encode = [&](const Evaluator& owner, const ValuePtr& result) {
        try {
            answer = valueToTerm(result, owner);
        } catch (const std::exception& e) {
            answer = Term::tuple(
                {Term::atom("Error"), Term::binary(std::string("Runtime error: ") + e.what())});
        }
    };
    const auto result = sandboxedEval(
        source->asBinaryStr(), expression && expression->isAtom("true"), std::move(limits), encode);
    std::cout.flush();
    if (!answer) {
        // An Error made without an evaluator to encode it: a String inside.
        const auto* variant = std::get_if<VariantValue>(&result->data);
        const auto* message = variant && !variant->args.empty()
                                  ? std::get_if<StringValue>(&variant->args.front()->data)
                                  : nullptr;
        answer = Term::tuple(
            {Term::atom("Error"), Term::binary(message ? message->value : result->toString())});
    }
    const auto encoded = beam::encodeEtf(answer);
    std::ofstream out(output->asBinaryStr(), std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot write evaluator result: " << output->asBinaryStr() << "\n";
        return 1;
    }
    out.write(
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<std::streamsize>(encoded.size()));
    return out ? 0 : 1;
}

} // namespace kex::interpreter
