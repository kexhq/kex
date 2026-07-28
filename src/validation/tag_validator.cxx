#include "tag_validator.hxx"

#include "../interpreter/evaluator.hxx"
#include "../lexer/lexer.hxx"
#include "../module/resolver.hxx"
#include "../parser/parser.hxx"
#include "../semantic/types.hxx"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>

namespace kex::validation {
namespace {

struct TagUse {
    const ast::Expr* expr = nullptr;
    const ast::TaggedLiteral* literal = nullptr;
    std::string scope;
};

using Functions =
    std::unordered_map<std::string, std::vector<const ast::FunctionDef*>>;

auto qualified(const std::string& scope, const std::string& name)
    -> std::string {
    return scope.empty() ? name : scope + "::" + name;
}

auto companionName(const std::string& tag) -> std::string {
    if (tag.empty()) return "validate";
    auto result = std::string{"validate"};
    result += static_cast<char>(
        std::toupper(static_cast<unsigned char>(tag.front())));
    result += tag.substr(1);
    return result;
}

auto walkExpr(
    const ast::Expr& expression,
    const std::string& scope,
    std::vector<TagUse>& uses) -> void {
    std::visit([&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        auto walk = [&](const ast::ExprPtr& value) {
            if (value) walkExpr(*value, scope, uses);
        };
        if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
            uses.push_back(TagUse{&expression, &node, scope});
            for (const auto& value : node.values) walk(value);
        } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
            for (const auto& value : node.values) walk(value);
        } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
            for (const auto& value : node.args) walk(value);
            for (const auto& [_, value] : node.namedArgs) walk(value);
            if (node.block) walk(*node.block);
        } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
            walk(node.receiver);
            for (const auto& value : node.args) walk(value);
            for (const auto& [_, value] : node.namedArgs) walk(value);
            if (node.block) walk(*node.block);
        } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
            walk(node.left);
            walk(node.right);
        } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
            walk(node.operand);
        } else if constexpr (std::is_same_v<T, ast::LetExpr> ||
                             std::is_same_v<T, ast::VarExpr> ||
                             std::is_same_v<T, ast::AssignExpr>) {
            walk(node.value);
        } else if constexpr (std::is_same_v<T, ast::ReturnExpr>) {
            walk(node.value);
        } else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
            walk(node.inner);
        } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
            walk(node.expr);
            walk(node.condition);
        } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
            walk(node.condition);
            walk(node.thenExpr);
            walk(node.elseExpr);
        } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
            walk(node.condition);
            for (const auto& value : node.thenBody) walk(value);
            for (const auto& [condition, body] : node.elifs) {
                walk(condition);
                for (const auto& value : body) walk(value);
            }
            if (node.elseBody)
                for (const auto& value : *node.elseBody) walk(value);
        } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
            walk(node.subject);
            for (const auto& clause : node.clauses) {
                if (clause.guard) walk(*clause.guard);
                walk(clause.body);
            }
        } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
            for (const auto& clause : node.clauses) {
                if (clause.guard) walk(*clause.guard);
                walk(clause.body);
            }
            if (node.timeout) walk(*node.timeout);
            if (node.afterBody) walk(*node.afterBody);
        } else if constexpr (std::is_same_v<T, ast::Lambda> ||
                             std::is_same_v<T, ast::BlockExpr> ||
                             std::is_same_v<T, ast::LoopExpr> ||
                             std::is_same_v<T, ast::SpawnExpr>) {
            for (const auto& value : node.body) walk(value);
        } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
            walk(node.condition);
            for (const auto& value : node.body) walk(value);
        } else if constexpr (std::is_same_v<T, ast::ListExpr> ||
                             std::is_same_v<T, ast::TupleExpr>) {
            for (const auto& value : node.elements) walk(value);
        } else if constexpr (std::is_same_v<T, ast::MapExpr>) {
            for (const auto& entry : node.entries) {
                walk(entry.key);
                walk(entry.value);
            }
        } else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
            walk(node.start);
            walk(node.end);
        } else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
            for (const auto& [_, value] : node.fields) walk(value);
        } else if constexpr (std::is_same_v<T, ast::UsingExpr>) {
            for (const auto& value : node.body) walk(value);
        }
    }, expression.kind);
}

auto collectFunction(
    const ast::FunctionDef& function,
    const std::string& scope,
    Functions& functions,
    std::vector<TagUse>& uses) -> void {
    functions[qualified(scope, function.name)].push_back(&function);
    for (const auto& clause : function.clauses)
        for (const auto& expression : clause.body)
            if (expression) walkExpr(*expression, scope, uses);
}

auto collectModule(
    const ast::ModuleDef& module,
    Functions& functions,
    std::vector<TagUse>& uses) -> void {
    const auto scope = module.name;
    for (const auto& item : module.body) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) collectFunction(*node, scope, functions, uses);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectModule(*node, functions, uses);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                if (node)
                    for (const auto& expression : node->body)
                        if (expression) walkExpr(*expression, scope, uses);
            }
        }, item);
    }
}

auto collect(
    const ast::Program& program,
    Functions& functions,
    std::vector<TagUse>& uses) -> void {
    for (const auto& item : program.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) collectFunction(*node, "", functions, uses);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (node)
                    for (const auto& expression : node->body)
                        if (expression) walkExpr(*expression, "", uses);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectModule(*node, functions, uses);
            } else if constexpr (
                std::is_same_v<T, std::unique_ptr<ast::UsingBlock>>) {
                if (node)
                    for (const auto& expression : node->body)
                        if (expression) walkExpr(*expression, "", uses);
            }
        }, item);
    }
}

// A `using`-imported module, parsed and kept alive so the AST pointers in
// `functions` stay valid for as long as validation runs.
struct ImportedModule {
    // unique_ptr, not a bare string: the vector holding these reallocates, and
    // parsed locations reference the buffer.
    std::unique_ptr<std::string> source;
    std::unique_ptr<ast::Program> program;
    Functions functions;
    // The module is analyzed on its own so its companion's signature can be
    // checked — the caller's analyzer only knows the user program.
    std::unique_ptr<semantic::Analyzer> analyzer;
};

// Collects the module names a program imports with `using`, ignoring foreign
// namespaces (Erlang./Elixir./Gleam.), which have no Kex source to parse.
auto usingModuleNames(const ast::Program& program) -> std::vector<std::string> {
    std::vector<std::string> names;
    auto add = [&](const ast::TypeName& typeName) {
        std::string name;
        for (size_t i = 0; i < typeName.parts.size(); i++) {
            if (i) name += ".";
            name += typeName.parts[i];
        }
        if (!module::Resolver::isForeignNamespace(name))
            names.push_back(std::move(name));
    };
    auto scanBody = [&](auto& self, const std::vector<ast::ModuleItem>& body)
        -> void {
        for (const auto& item : body) {
            if (auto* block = std::get_if<std::unique_ptr<ast::UsingBlock>>(&item))
                if (*block) add((*block)->module);
            if (auto* def = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                if (*def) self(self, (*def)->body);
        }
    };
    for (const auto& item : program.items) {
        if (auto* block = std::get_if<std::unique_ptr<ast::UsingBlock>>(&item))
            if (*block) add((*block)->module);
        if (auto* def = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
            if (*def) scanBody(scanBody, (*def)->body);
    }
    return names;
}

auto loadUsingModules(
    const ast::Program& program,
    const std::vector<std::string>& roots) -> std::vector<ImportedModule> {
    std::vector<ImportedModule> modules;
    module::Resolver resolver(roots);
    for (const auto& name : usingModuleNames(program)) {
        auto resolved = resolver.resolve(name);
        if (!resolved) continue;

        std::ifstream input{resolved->path};
        if (!input) continue;
        std::stringstream buffer;
        buffer << input.rdbuf();

        ImportedModule module;
        module.source = std::make_unique<std::string>(buffer.str());
        Lexer lexer(*module.source, resolved->path);
        Parser parser(lexer.tokenizeAll(), resolved->path);
        auto parsed = std::make_unique<ast::Program>(parser.parseProgram());
        // A module that does not parse is reported by the normal pipeline;
        // silently skipping it here avoids duplicate diagnostics.
        if (!parser.diagnostics().empty()) continue;
        module.program = std::move(parsed);
        modules.push_back(std::move(module));
    }
    return modules;
}

auto exactValidatorSignature(
    const std::vector<const ast::FunctionDef*>& definitions,
    const semantic::Analyzer& analyzer) -> bool {
    auto hasQualifiedIssueReturn =
        [](const ast::FunctionDef& definition) {
            for (const auto& clause : definition.clauses) {
                if (!clause.returnAnnotation ||
                    !*clause.returnAnnotation)
                    return false;
                auto* list = std::get_if<ast::ListType>(
                    &(*clause.returnAnnotation)->kind);
                if (!list || !list->element)
                    return false;
                auto* name = std::get_if<ast::TypeName>(
                    &list->element->kind);
                if (!name ||
                    name->parts != std::vector<std::string>{
                        "TaggedValidation", "Issue"})
                    return false;
            }
            return !definition.clauses.empty();
        };

    bool found = false;
    for (const auto* definition : definitions) {
        if (!definition || definition->isFoul ||
            !hasQualifiedIssueReturn(*definition))
            return false;
        auto* signatures = analyzer.functionSignatures(definition);
        if (!signatures || signatures->empty()) return false;
        for (const auto& signature : *signatures) {
            found = true;
            if (signature.isFoul || signature.params.size() != 1 ||
                semantic::typeToString(signature.params[0]) != "String" ||
                semantic::typeToString(signature.result) != "[Issue]")
                return false;
        }
    }
    return found;
}

auto sourceLocationForOffset(
    const TagUse& use,
    std::optional<int64_t> offset) -> SourceLocation {
    auto location = use.expr->location;
    if (!offset || *offset < 0 || use.literal->bodyStartOffset < 0)
        return location;

    std::ifstream input{std::string(location.file), std::ios::binary};
    if (!input) return location;
    std::string source{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    const auto start =
        static_cast<size_t>(use.literal->bodyStartOffset + 1);
    const auto end = use.literal->bodyEndOffset > 0
        ? static_cast<size_t>(use.literal->bodyEndOffset - 1)
        : source.size();
    if (start > end || end > source.size()) return location;

    // Reconstruct the lexer's cooked-byte boundary map for raw literals.
    // Keeping it here makes validation independent of tag identity while
    // still accounting for opening-newline removal, doubled backticks, and
    // the closing-line dedent margin.
    std::string decoded;
    std::vector<size_t> offsets;
    for (size_t cursor = start; cursor < end;) {
        offsets.push_back(cursor);
        if (source[cursor] == '`' && cursor + 1 < end &&
            source[cursor + 1] == '`') {
            decoded += '`';
            cursor += 2;
        } else {
            decoded += source[cursor++];
        }
    }
    offsets.push_back(end);

    auto erasePrefix = [&](size_t count) {
        decoded.erase(0, count);
        offsets.erase(offsets.begin(), offsets.begin() + count);
    };
    if (decoded.starts_with("\r\n")) erasePrefix(2);
    else if (decoded.starts_with("\n")) erasePrefix(1);

    auto lastNewline = decoded.rfind('\n');
    if (lastNewline != std::string::npos) {
        auto margin = decoded.substr(lastNewline + 1);
        bool closingOnOwnLine = std::all_of(
            margin.begin(), margin.end(),
            [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
        if (closingOnOwnLine) {
            if (!margin.empty() && margin.back() == '\r')
                margin.pop_back();
            decoded.erase(lastNewline + 1);
            offsets.erase(
                offsets.begin() + lastNewline + 1, offsets.end() - 1);
            offsets.back() = end;

            if (!margin.empty()) {
                std::string cooked;
                std::vector<size_t> cookedOffsets;
                size_t lineStart = 0;
                while (lineStart < decoded.size()) {
                    auto lineEnd = decoded.find('\n', lineStart);
                    bool hasNewline = lineEnd != std::string::npos;
                    if (!hasNewline) lineEnd = decoded.size();
                    auto contentEnd = lineEnd;
                    if (contentEnd > lineStart &&
                        decoded[contentEnd - 1] == '\r')
                        contentEnd--;
                    bool blank = std::all_of(
                        decoded.begin() + lineStart,
                        decoded.begin() + contentEnd,
                        [](char c) { return c == ' ' || c == '\t'; });
                    auto keepStart =
                        blank ? contentEnd : lineStart + margin.size();
                    for (size_t i = keepStart; i < lineEnd; ++i) {
                        cooked += decoded[i];
                        cookedOffsets.push_back(offsets[i]);
                    }
                    if (hasNewline) {
                        cooked += '\n';
                        cookedOffsets.push_back(offsets[lineEnd]);
                    }
                    lineStart = lineEnd + (hasNewline ? 1 : 0);
                }
                cookedOffsets.push_back(end);
                decoded = std::move(cooked);
                offsets = std::move(cookedOffsets);
            }
        }
    }

    auto cookedOffset = static_cast<size_t>(*offset);
    if (cookedOffset >= offsets.size()) return location;
    auto target = offsets[cookedOffset];
    if (target > source.size()) return location;
    int line = 1;
    int column = 1;
    for (size_t i = 0; i < target; ++i) {
        if (source[i] == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }
    location.line = line;
    location.column = column;
    return location;
}

auto integerValue(const interpreter::ValuePtr& value)
    -> std::optional<int64_t> {
    if (!value) return std::nullopt;
    if (auto* integer = std::get_if<interpreter::IntValue>(&value->data))
        return integer->value;
    return std::nullopt;
}

auto appendIssues(
    const interpreter::ValuePtr& result,
    const TagUse& use,
    const std::string& validator,
    std::vector<semantic::Diagnostic>& diagnostics) -> void {
    auto* list = result
        ? std::get_if<interpreter::ListValue>(&result->data)
        : nullptr;
    if (!list) {
        diagnostics.push_back({
            semantic::Diagnostic::Level::Error,
            use.expr->location,
            "Compile-time validator `" + validator +
                "` must return [TaggedValidation.Issue], got " +
                (result ? result->typeName() : std::string{"nothing"})});
        return;
    }

    struct PositionedDiagnostic {
        semantic::Diagnostic diagnostic;
        int64_t offset = 0;
        bool wholeLiteral = false;
    };
    std::vector<PositionedDiagnostic> positioned;

    for (const auto& issueValue : list->elements) {
        auto* issue = issueValue
            ? std::get_if<interpreter::VariantValue>(&issueValue->data)
            : nullptr;
        if (!issue || (issue->tag != "Fatal" && issue->tag != "Warn") ||
            issue->args.size() != 2) {
            diagnostics.push_back({
                semantic::Diagnostic::Level::Error,
                use.expr->location,
                "Compile-time validator `" + validator +
                    "` returned a malformed Issue"});
            continue;
        }

        std::optional<int64_t> offset;
        std::optional<int64_t> endOffset;
        bool wholeLiteral = issue->args[0]->isNone();
        if (!issue->args[0]->isNone()) {
            auto* span = std::get_if<interpreter::VariantValue>(
                &issue->args[0]->data);
            if (span && span->tag == "At" && span->args.size() == 1) {
                offset = integerValue(span->args[0]);
                if (!offset) {
                    diagnostics.push_back({
                        semantic::Diagnostic::Level::Error,
                        use.expr->location,
                        "Compile-time validator `" + validator +
                            "` returned an invalid Issue byte span"});
                    continue;
                }
            } else if (span && span->tag == "Between" &&
                       span->args.size() == 2) {
                offset = integerValue(span->args[0]);
                endOffset = integerValue(span->args[1]);
                if (!offset || !endOffset) {
                    diagnostics.push_back({
                        semantic::Diagnostic::Level::Error,
                        use.expr->location,
                        "Compile-time validator `" + validator +
                            "` returned an invalid Issue byte span"});
                    continue;
                }
            } else {
                diagnostics.push_back({
                    semantic::Diagnostic::Level::Error,
                    use.expr->location,
                    "Compile-time validator `" + validator +
                        "` returned an invalid Issue byte span"});
                continue;
            }
        }
        const auto bodySize =
            static_cast<int64_t>(use.literal->parts[0].size());
        if (wholeLiteral) {
            offset = 0;
            endOffset = bodySize;
        }
        if ((offset && (*offset < 0 || *offset > bodySize)) ||
            (endOffset &&
             (*endOffset < *offset || *endOffset > bodySize))) {
            diagnostics.push_back({
                semantic::Diagnostic::Level::Error,
                use.expr->location,
                "Compile-time validator `" + validator +
                    "` returned an Issue byte span outside the literal"});
            continue;
        }

        auto* message =
            std::get_if<interpreter::StringValue>(&issue->args[1]->data);
        if (!message) {
            diagnostics.push_back({
                semantic::Diagnostic::Level::Error,
                use.expr->location,
                "Compile-time validator `" + validator +
                    "` returned an Issue with a non-String message"});
            continue;
        }
        semantic::Diagnostic diagnostic{
            issue->tag == "Fatal"
                ? semantic::Diagnostic::Level::Error
                : semantic::Diagnostic::Level::Warning,
            sourceLocationForOffset(use, offset),
            message->value};
        if (endOffset)
            diagnostic.endLocation =
                sourceLocationForOffset(use, endOffset);
        positioned.push_back(PositionedDiagnostic{
            std::move(diagnostic), *offset, wholeLiteral});
    }

    std::stable_sort(
        positioned.begin(), positioned.end(),
        [](const PositionedDiagnostic& left,
           const PositionedDiagnostic& right) {
            if (left.wholeLiteral != right.wholeLiteral)
                return !left.wholeLiteral;
            return left.offset < right.offset;
        });
    for (auto& item : positioned)
        diagnostics.push_back(std::move(item.diagnostic));
}

} // namespace

auto validateTaggedLiterals(
    const ast::Program& program,
    const semantic::Analyzer& analyzer,
    const std::vector<std::string>& moduleRoots,
    std::chrono::milliseconds timeout)
    -> std::vector<semantic::Diagnostic> {
    Functions functions;
    std::vector<TagUse> uses;
    collect(program, functions, uses);

    // Tags defined in a `using`-imported module are validated too. Their
    // definitions live in another file, so each module is parsed here and kept
    // alive for the duration — `functions` holds raw AST pointers into it, and
    // the companion is evaluated against that module's own program (which is
    // self-contained: it can reach the prelude and intrinsics, which is all a
    // validator is allowed to use anyway, being pure).
    std::vector<ImportedModule> imported;
    if (!moduleRoots.empty() && !uses.empty())
        imported = loadUsingModules(program, moduleRoots);
    for (auto& module : imported)
        if (module.program) {
            std::vector<TagUse> ignored;
            collect(*module.program, module.functions, ignored);
            module.analyzer = std::make_unique<semantic::Analyzer>();
            module.analyzer->analyze(*module.program);
        }

    std::vector<semantic::Diagnostic> diagnostics;
    for (const auto& use : uses) {
        if (!use.literal || use.literal->interpolating ||
            use.literal->parts.size() != 1)
            continue;
        const std::string subject = use.literal->parts[0];

        const auto companion = companionName(use.literal->tag);
        const auto companionKey = qualified(use.scope, companion);

        // Prefer a program-local definition; fall back to an imported module's.
        const ast::Program* home = &program;
        const Functions* scope = &functions;
        std::string moduleCompanionKey;
        const ImportedModule* matchedModule = nullptr;
        if (!functions.contains(qualified(use.scope, use.literal->tag))) {
            // A module file collects its functions under its own scope
            // ("Regex::regex"), while the tag at the use site is unqualified
            // ("regex") — `using` brought it into scope. Match on the trailing
            // segment, and remember the qualified key for evaluation.
            const Functions* fromModule = nullptr;
            for (const auto& module : imported) {
                for (const auto& [key, definitions] : module.functions) {
                    const auto bare = key.substr(key.rfind("::") == std::string::npos
                                                     ? 0 : key.rfind("::") + 2);
                    if (bare != use.literal->tag) continue;
                    const auto prefix =
                        key.size() > bare.size() ? key.substr(0, key.size() - bare.size())
                                                 : std::string{};
                    if (!module.functions.contains(prefix + companion)) continue;
                    fromModule = &module.functions;
                    home = module.program.get();
                    moduleCompanionKey = prefix + companion;
                    matchedModule = &module;
                    break;
                }
                if (fromModule) break;
            }
            if (!fromModule)
                continue; // precompiled source, or no companion anywhere
            scope = fromModule;
        }

        auto found = scope->find(
            scope == &functions ? companionKey : moduleCompanionKey);
        if (found == scope->end())
            continue;

        const auto& signatureSource =
            matchedModule ? *matchedModule->analyzer : analyzer;
        if (!exactValidatorSignature(found->second, signatureSource)) {
            diagnostics.push_back({
                semantic::Diagnostic::Level::Error,
                found->second.front()->location,
                "Companion validator `" + companion +
                    "` must be pure with signature "
                    "String -> [TaggedValidation.Issue]"});
            continue;
        }

        try {
            interpreter::Evaluator evaluator;
            evaluator.loadPrelude();
            auto result = evaluator.evaluateFunction(
                *home,
                scope == &functions ? companionKey : moduleCompanionKey,
                {interpreter::Value::string(subject)},
                timeout);
            appendIssues(result, use, companion, diagnostics);
        } catch (const interpreter::EvaluationTimeout&) {
            semantic::Diagnostic diagnostic{
                semantic::Diagnostic::Level::Error,
                found->second.front()->location,
                "Compile-time validator `" + companionKey +
                    "` timed out after " +
                    std::to_string(timeout.count()) + " ms"};
            diagnostic.notes.push_back({
                use.expr->location,
                "triggered by tagged literal `" + use.literal->tag + "`"});
            diagnostics.push_back(std::move(diagnostic));
        } catch (const std::exception& error) {
            semantic::Diagnostic diagnostic{
                semantic::Diagnostic::Level::Error,
                found->second.front()->location,
                "Compile-time validator `" + companionKey +
                    "` crashed: " + error.what()};
            diagnostic.notes.push_back({
                use.expr->location,
                "triggered by tagged literal `" + use.literal->tag + "`"});
            diagnostics.push_back(std::move(diagnostic));
        }
    }
    return diagnostics;
}

} // namespace kex::validation
