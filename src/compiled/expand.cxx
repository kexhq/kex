#include "expand.hxx"

#include "reify.hxx"
#include "../ast/clone.hxx"
#include "../interpreter/evaluator.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"

#include <unordered_map>
#include <utility>

namespace kex::compiled {

namespace {

// Is this `let` a VALUE BINDING rather than a function definition?
//
// Not named "isConstant": Kex is immutable by default, so every `let` value
// binding is already constant — and arguably a function definition is too.
// Constancy is not the distinction. What matters here is arity: a binding with
// no parameters denotes a value we can compute now, whereas a `let` WITH
// parameters is a function the program calls at runtime, with arguments we
// cannot know, so it is left alone.
//
// It is a binding, not a function; the parser merely encodes a parameterless
// binding as a FunctionDef with one clause and no parameters (grammar.ebnf
// calls that clause form a "parameterless binding"), so that is the shape we
// match on. Being evaluated at compile time comes from the enclosing
// `compiled do` block, not from anything about the binding itself.
auto isValueBinding(const ast::FunctionDef& fn) -> bool {
    return fn.clauses.size() == 1 && fn.clauses[0].params.empty() &&
           fn.clauses[0].body.size() == 1 && fn.clauses[0].body[0] != nullptr;
}

auto addError(std::vector<semantic::Diagnostic>& diagnostics,
              const SourceLocation& location, std::string message) -> void {
    diagnostics.push_back({semantic::Diagnostic::Level::Error, location,
                           std::move(message)});
}

using Constants = std::unordered_map<std::string, interpreter::ValuePtr>;

// Stable identity for a generation template, whichever declaration shape it
// holds. Used to map a template back to the scope that declared it.
auto templateId(const ast::GeneratedTemplate& tmpl) -> const void* {
    return std::visit([](const auto& held) -> const void* { return held.get(); },
                      tmpl);
}

// ---------------------------------------------------------------------------
// Use-site substitution
//
// Replaces every reference to a compile-time constant with its value, reified
// as a literal. This is what makes the constant free: without it each use
// compiles to `apply 'NAME'/0()`, a call into a nullary function that also has
// to be exported.
//
// A fresh literal is built per use site (rather than cloning one AST) so each
// carries its own source location, and so no node is ever shared.
// ---------------------------------------------------------------------------

struct Substituter {
    const Constants& constants;
    std::vector<semantic::Diagnostic>& diagnostics;
    // Names that could not be reified: left as references so the program still
    // has something to resolve, and their definitions are kept.
    std::vector<std::string> failed;
    // Names still referenced after the walk. A definition is only dropped when
    // nothing refers to it any more, so a node kind this walk fails to reach
    // degrades to today's behaviour instead of an undefined name.
    std::vector<std::string> remaining;
    // Notified for each `let %name(...)` encountered. Used before evaluation to
    // learn which scope a template belongs to, so its generated declarations
    // land in the module that declared them rather than at top level.
    std::function<void(const ast::GeneratedDecl&)> onGenerated;

    auto nameOf(const ast::Expr& expr) -> const std::string* {
        if (const auto* upper = std::get_if<ast::UpperIdentifier>(&expr.kind))
            return &upper->name;
        if (const auto* lower = std::get_if<ast::Identifier>(&expr.kind))
            return &lower->name;
        return nullptr;
    }

    auto substitute(ast::ExprPtr& slot) -> void {
        if (!slot) return;
        if (const auto* name = nameOf(*slot)) {
            auto found = constants.find(*name);
            if (found != constants.end()) {
                std::string why;
                auto literal =
                    valueToLiteral(found->second, slot->location, why);
                if (literal) {
                    slot = std::move(literal);
                    return;
                }
                remaining.push_back(*name);
                return;
            }
        }
        walkChildren(*slot);
    }

    auto each(std::vector<ast::ExprPtr>& list) -> void {
        for (auto& item : list) substitute(item);
    }
    auto each(std::optional<ast::ExprPtr>& slot) -> void {
        if (slot) substitute(*slot);
    }
    auto each(std::vector<std::pair<std::string, ast::ExprPtr>>& list) -> void {
        for (auto& [_, item] : list) substitute(item);
    }
    auto each(std::vector<ast::MatchClause>& clauses) -> void {
        for (auto& clause : clauses) {
            if (clause.guard) substitute(*clause.guard);
            substitute(clause.body);
        }
    }
    auto each(ast::RescueBlock& rescue) -> void {
        each(rescue.clauses);
        each(rescue.catchAllBody);
        substitute(rescue.inlineReturnExpr);
    }

    // A plain `"...${x}..."` keeps its interpolations as RAW TEXT: the parser
    // leaves parts/values empty and the evaluator re-parses `${...}` at
    // runtime (see the StringLiteral case in Evaluator::eval). Such a string
    // has no child nodes, so a use of a constant inside one is invisible to
    // the walk. Pre-split it into parts/values — the form the evaluator
    // already prefers — so the reference becomes a real node we can replace.
    //
    // Only done for strings that actually mention a constant, so programs
    // without compiled constants keep the existing lazy path untouched.
    auto splitInterpolation(ast::StringLiteral& literal) -> void {
        if (!literal.interpolating || !literal.parts.empty()) return;
        const std::string& text = literal.value;
        bool mentions = false;
        for (const auto& [name, _] : constants)
            if (text.find(name) != std::string::npos) { mentions = true; break; }
        if (!mentions) return;

        std::vector<std::string> parts;
        std::vector<ast::ExprPtr> values;
        std::string current;
        for (std::size_t i = 0; i < text.size();) {
            if (i + 1 < text.size() && text[i] == '$' && text[i + 1] == '{') {
                i += 2;
                std::string inner;
                int depth = 1;
                while (i < text.size() && depth > 0) {
                    if (text[i] == '{') depth++;
                    else if (text[i] == '}') { depth--; if (!depth) break; }
                    inner += text[i++];
                }
                if (i < text.size()) i++; // closing brace
                Lexer lexer(inner);
                Parser parser(lexer.tokenizeAll());
                auto parsed = parser.parseExpr();
                if (!parser.diagnostics().empty() || !parsed) {
                    // Not parseable here — leave the whole literal alone and
                    // let the runtime path report it, exactly as today.
                    return;
                }
                parts.push_back(current);
                current.clear();
                values.push_back(std::move(parsed));
                continue;
            }
            current += text[i++];
        }
        parts.push_back(current);
        literal.parts = std::move(parts);
        literal.values = std::move(values);
    }

    auto walkChildren(ast::Expr& expr) -> void {
        std::visit(
            [&](auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                    splitInterpolation(node);
                    each(node.values);
                } else if constexpr (std::is_same_v<T, ast::TaggedLiteral>) {
                    each(node.values);
                } else if constexpr (std::is_same_v<T, ast::GeneratedDecl>) {
                    if (onGenerated) onGenerated(node);
                    substitute(node.name);
                } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                    substitute(node.receiver);
                    each(node.args);
                    each(node.namedArgs);
                    each(node.block);
                } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                    each(node.args);
                    each(node.namedArgs);
                    each(node.block);
                } else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
                    each(node.fields);
                } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                    substitute(node.left);
                    substitute(node.right);
                } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                    substitute(node.operand);
                } else if constexpr (std::is_same_v<T, ast::TupleExpr> ||
                                     std::is_same_v<T, ast::BlockExpr> ||
                                     std::is_same_v<T, ast::LoopExpr> ||
                                     std::is_same_v<T, ast::SpawnExpr>) {
                    if constexpr (std::is_same_v<T, ast::TupleExpr>)
                        each(node.elements);
                    else
                        each(node.body);
                } else if constexpr (std::is_same_v<T, ast::ListExpr>) {
                    each(node.elements);
                    each(node.rest);
                } else if constexpr (std::is_same_v<T, ast::MapExpr>) {
                    for (auto& entry : node.entries) {
                        substitute(entry.key);
                        substitute(entry.value);
                    }
                } else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
                    substitute(node.start);
                    substitute(node.end);
                } else if constexpr (std::is_same_v<T, ast::IfExpr>) {
                    substitute(node.condition);
                    each(node.thenBody);
                    for (auto& [condition, body] : node.elifs) {
                        substitute(condition);
                        each(body);
                    }
                    if (node.elseBody) each(*node.elseBody);
                } else if constexpr (std::is_same_v<T, ast::MatchExpr>) {
                    substitute(node.subject);
                    each(node.clauses);
                } else if constexpr (std::is_same_v<T, ast::ReceiveExpr>) {
                    each(node.clauses);
                    each(node.timeout);
                    each(node.afterBody);
                } else if constexpr (std::is_same_v<T, ast::WhileExpr>) {
                    substitute(node.condition);
                    each(node.body);
                } else if constexpr (std::is_same_v<T, ast::LetExpr> ||
                                     std::is_same_v<T, ast::VarExpr> ||
                                     std::is_same_v<T, ast::AssignExpr> ||
                                     std::is_same_v<T, ast::ReturnExpr>) {
                    substitute(node.value);
                } else if constexpr (std::is_same_v<T, ast::Lambda>) {
                    each(node.body);
                    if (node.rescue) each(*node.rescue);
                } else if constexpr (std::is_same_v<T, ast::ShorthandLambda>) {
                    each(node.args);
                } else if constexpr (std::is_same_v<T, ast::SpreadExpr>) {
                    substitute(node.inner);
                } else if constexpr (std::is_same_v<T, ast::TrailingIf>) {
                    substitute(node.expr);
                    substitute(node.condition);
                } else if constexpr (std::is_same_v<T, ast::ThenElseExpr>) {
                    substitute(node.condition);
                    substitute(node.thenExpr);
                    substitute(node.elseExpr);
                } else if constexpr (std::is_same_v<T, ast::CurryExpr>) {
                    for (auto& group : node.argGroups) each(group);
                } else if constexpr (std::is_same_v<T, ast::TryExpr>) {
                    substitute(node.operand);
                } else if constexpr (std::is_same_v<T, ast::TryingExpr>) {
                    each(node.body);
                    each(node.rescue);
                }
                // Remaining kinds hold no sub-expressions: the literals,
                // Identifier/UpperIdentifier (handled above), ThisExpr,
                // Break/Next, CurryPlaceholder, ErrorNode.
            },
            expr.kind);
    }

    auto functionBody(ast::FunctionDef& fn) -> void {
        for (auto& clause : fn.clauses) {
            each(clause.body);
            for (auto& param : clause.params)
                if (param.defaultValue) substitute(*param.defaultValue);
            if (clause.rescue) each(*clause.rescue);
        }
    }
};

// Applies `visitor` to every function/main body reachable in `items`.
template <typename Items, typename Visit>
auto walkBodies(Items& items, Visit&& visit) -> void {
    for (auto& item : items) {
        std::visit(
            [&](auto& node) {
                using T = std::decay_t<decltype(node)>;
                using Ptr = std::decay_t<decltype(*node)>;
                if (!node) return;
                if constexpr (std::is_same_v<Ptr, ast::FunctionDef>) {
                    visit(*node);
                } else if constexpr (std::is_same_v<Ptr, ast::MainBlock>) {
                    for (auto& expr : node->body) visit(expr);
                } else if constexpr (std::is_same_v<Ptr, ast::MakeDef>) {
                    walkBodies(node->body, visit);
                } else if constexpr (std::is_same_v<Ptr, ast::ModuleDef>) {
                    walkBodies(node->body, visit);
                } else if constexpr (std::is_same_v<Ptr, ast::CompiledBlock>) {
                    walkBodies(node->items, visit);
                } else if constexpr (std::is_same_v<Ptr, ast::VisibilityBlock>) {
                    walkBodies(node->items, visit);
                }
            },
            item);
    }
}

} // namespace

auto expand(ast::Program& program,
            std::vector<semantic::Diagnostic>& diagnostics,
            const ExpandOptions& options) -> bool {
    // --- 1. Find the constants, in source order.
    std::vector<std::string> names;
    std::vector<SourceLocation> locations;
    bool hasBlock = false;
    auto scan = [&](auto& items) {
        for (auto& item : items) {
            auto* block = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&item);
            if (!block || !*block) continue;
            hasBlock = true;
            for (auto& entry : (*block)->items)
                if (auto* fn =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&entry))
                    if (*fn && isValueBinding(**fn)) {
                        names.push_back((*fn)->name);
                        locations.push_back((*fn)->location);
                    }
        }
    };
    scan(program.items);
    for (auto& item : program.items)
        if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
            if (*mod) scan((*mod)->body);
    if (!hasBlock) return true;

    // Which scope does each generation template belong to? A `let %name(...)`
    // inside `module M do compiled do ... end end` must produce a method OF M,
    // not a top-level function. The template pointer identifies its block, so
    // map it to the owning module before anything runs. Null means top level.
    std::unordered_map<const void*, ast::ModuleDef*> templateOwner;
    {
        auto record = [&](auto& items, ast::ModuleDef* owner) {
            for (auto& item : items) {
                auto* block =
                    std::get_if<std::unique_ptr<ast::CompiledBlock>>(&item);
                if (!block || !*block) continue;
                for (auto& entry : (*block)->items) {
                    auto* expr = std::get_if<ast::ExprPtr>(&entry);
                    if (!expr || !*expr) continue;
                    Constants none;
                    Substituter finder{none, diagnostics, {}, {}, {}};
                    finder.onGenerated = [&](const ast::GeneratedDecl& decl) {
                        templateOwner[templateId(decl.function)] = owner;
                    };
                    finder.substitute(*expr);
                }
            }
        };
        record(program.items, nullptr);
        for (auto& item : program.items)
            if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                if (*mod) record((*mod)->body, mod->get());
    }


    // --- 2. Run the block once, against the whole program.
    //
    // This both forces the constants AND executes any generation code: a
    // driver loop is ordinary compile-time code, and `let %name(...)` records
    // a declaration as it runs (Evaluator's GeneratedDecl case). That is why
    // `each`, `eachIndexed`, nesting and conditionals need no support here —
    // they are just code the sandbox runs.
    Constants constants;
    std::vector<interpreter::Evaluator::GeneratedDeclaration> generated;
    {
        const SourceLocation blame =
            locations.empty() ? SourceLocation{} : locations.front();
        std::vector<interpreter::ValuePtr> values;
        try {
            interpreter::Evaluator evaluator;
            evaluator.loadPrelude();
            values = evaluator.evaluateConstants(program, names, options.timeout);
            generated = evaluator.generatedDeclarations();
        } catch (const interpreter::EvaluationTimeout&) {
            addError(diagnostics, blame,
                     "compile-time evaluation timed out after " +
                         std::to_string(options.timeout.count()) + " ms");
            return false;
        } catch (const std::exception& crash) {
            addError(diagnostics, blame,
                     std::string("compile-time evaluation failed: ") +
                         crash.what());
            return false;
        }
        for (std::size_t i = 0; i < names.size() && i < values.size(); i++)
            if (values[i]) constants.emplace(names[i], values[i]);
    }

    // --- 3. Substitute every use with the computed literal.
    Substituter substituter{constants, diagnostics, {}, {}, {}};
    walkBodies(program.items, [&](auto& target) {
        using T = std::decay_t<decltype(target)>;
        if constexpr (std::is_same_v<T, ast::FunctionDef>)
            substituter.functionBody(target);
        else
            substituter.substitute(target);
    });

    bool ok = true;
    for (const auto& name : substituter.remaining) {
        std::string why;
        (void)valueToLiteral(constants.at(name), SourceLocation{}, why);
        addError(diagnostics, SourceLocation{},
                 "compile-time constant `" + name +
                     "` cannot be inlined: " + why);
        ok = false;
    }

    // --- 3b. Instantiate the generated declarations.
    //
    // Each recorded `let %name(...)` becomes a real FunctionDef: the template
    // is CLONED (one template, N independent bodies), given its resolved name,
    // and has the compile-time bindings it closed over baked in as literals —
    // a loop variable does not exist at runtime, so it cannot survive as a
    // reference. Parameters of the generated function shadow those bindings,
    // which falls out of Substituter skipping any name it does not know.
    // Each entry is appended to the scope that declared its template.
    for (const auto& decl : generated) {
        if (decl.name.empty()) {
            addError(diagnostics, decl.location,
                     "generated declaration has an empty name");
            ok = false;
            continue;
        }
        auto owner = templateOwner.find(templateId(decl.function));
        auto* into = owner != templateOwner.end() ? owner->second : nullptr;

        if (auto* fnTemplate =
                std::get_if<std::shared_ptr<ast::FunctionDef>>(&decl.function)) {
            if (!*fnTemplate) continue;
            auto copy = ast::clone(**fnTemplate);
            copy->name = decl.name;
            copy->location = decl.location;
            // A parameter of the generated function must win over a same-named
            // compile-time binding, so drop those from the substitution map.
            Constants captured;
            for (const auto& [bound, value] : decl.bindings)
                captured.emplace(bound, value);
            for (const auto& clause : copy->clauses)
                for (const auto& param : clause.params)
                    if (param.name) captured.erase(*param.name);
            Substituter inner{captured, diagnostics, {}, {}, {}};
            inner.functionBody(*copy);
            if (into) into->body.push_back(std::move(copy));
            else program.items.push_back(std::move(copy));
            continue;
        }

        if (auto* typeTemplate =
                std::get_if<std::shared_ptr<ast::TypeDef>>(&decl.function)) {
            if (!*typeTemplate) continue;
            auto copy = ast::clone(**typeTemplate);
            copy->name = decl.name;
            copy->location = decl.location;
            // A generated type's body is types, not expressions, so there is
            // nothing for the hygiene substitution to do here.
            if (into) into->body.push_back(std::move(copy));
            else program.items.push_back(std::move(copy));
        }
    }

    // --- 4. Drop the definitions. A constant is gone from the emitted module
    // entirely — no nullary function, nothing exported. Only constants whose
    // uses were all substituted are removed; anything left referenced keeps
    // its definition so the program still resolves.
    std::unordered_map<std::string, bool> keep;
    for (const auto& name : substituter.remaining) keep[name] = true;

    auto prune = [&](auto& items) {
        for (std::size_t i = 0; i < items.size();) {
            auto* block = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&items[i]);
            if (!block || !*block) { i++; continue; }
            auto& entries = (*block)->items;
            std::vector<ast::CompiledItem> kept;
            for (auto& entry : entries) {
                auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&entry);
                const bool inlined = fn && *fn && isValueBinding(**fn) &&
                                     constants.count((*fn)->name) &&
                                     !keep.count((*fn)->name);
                // A bare expression in a compiled block is compile-time work
                // and nothing else — a driver loop, typically. It has already
                // run, and its generated declarations are now real items, so
                // keeping it would re-run the loop at runtime (and leave a
                // CompiledBlock that top-level BEAM lowering cannot handle).
                const bool consumed = std::holds_alternative<ast::ExprPtr>(entry);
                if (!inlined && !consumed) kept.push_back(std::move(entry));
            }
            entries = std::move(kept);
            if (entries.empty()) items.erase(items.begin() + static_cast<long>(i));
            else i++;
        }
    };
    prune(program.items);
    for (auto& item : program.items)
        if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
            if (*mod) prune((*mod)->body);

    return ok;
}

} // namespace kex::compiled
