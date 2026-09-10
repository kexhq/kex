#include "expand.hxx"

#include "reify.hxx"
#include "../ast/clone.hxx"
#include "../interpreter/evaluator.hxx"
#include "../interpreter/value.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kex::compiled {

namespace {

// Is this `let` a VALUE BINDING rather than a function definition?
//
// Not named "isConstant": Kex is immutable by default, so every `let` value
// binding is already constant — and arguably a function definition is too.
// Constancy is not the distinction. What matters here is whether the source
// wrote a PARAMETER LIST: a binding denotes a value we can compute now, whereas
// a `let` with a parameter list is a function the program calls at runtime,
// with arguments we cannot know, so it is left alone.
//
// It is a binding, not a function; the parser merely encodes a parameterless
// binding as a FunctionDef with one clause and no parameters (grammar.ebnf
// calls that clause form a "parameterless binding"), so that is the shape we
// match on. Being evaluated at compile time comes from the enclosing
// `compiled do` block, not from anything about the binding itself.
//
// `hasParamList` is what separates `let x = 3` from `let x() = 3` — both have
// empty `params`, and treating the second as a binding evaluated it at compile
// time, failed to substitute its (method-call) uses, and then pruned the
// definition. That is what `Undefined function: Css.stylesheet` was.
auto isValueBinding(const ast::FunctionDef& fn) -> bool {
    return fn.clauses.size() == 1 && !fn.clauses[0].hasParamList &&
           fn.clauses[0].params.empty() && fn.clauses[0].body.size() == 1 &&
           fn.clauses[0].body[0] != nullptr;
}

auto addError(std::vector<semantic::Diagnostic>& diagnostics,
              const SourceLocation& location, std::string message) -> void {
    diagnostics.push_back({semantic::Diagnostic::Level::Error, location,
                           std::move(message)});
}

using Constants = std::unordered_map<std::string, interpreter::ValuePtr>;
using ScopedConstants = std::unordered_map<std::string, Constants>;

auto qualifiedName(const std::string& scope, const std::string& name)
    -> std::string {
    return scope.empty() ? name : scope + "::" + name;
}

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
    // Consulted for every expression slot before anything else. Returning true
    // CLAIMS the slot: the walk neither substitutes nor descends into it.
    // Chain collapse uses this — it replaces a whole call chain, so rewriting
    // the parts underneath would be wasted work at best.
    std::function<bool(ast::ExprPtr&)> onSlot;
    // Runtime bindings with the same spelling as a compiled constant hide it
    // in their lexical scope. Substitution must follow the language's scope,
    // not merely perform a global textual replacement.
    std::unordered_set<std::string> shadowed;
    // Declared record field order, so a reified record is written in the order
    // its declaration gives — which on BEAM is its tuple layout. Empty until
    // the sandbox has run and can report it.
    ReifyContext reify;
    // Constants owned by modules, used to replace `A.X` as a whole. Bare-name
    // substitution alone cannot see that `X` is encoded as a MethodCall name.
    const ScopedConstants* scoped = nullptr;
    // A second reason to pre-split an interpolated string, beyond "it mentions
    // a constant". Chain collapse needs one: a chain written straight inside
    // `"${...}"` is raw text here, so the walk would never reach it.
    std::function<bool(const std::string&)> alsoSplit;

    auto nameOf(const ast::Expr& expr) -> const std::string* {
        if (const auto* upper = std::get_if<ast::UpperIdentifier>(&expr.kind))
            return &upper->name;
        if (const auto* lower = std::get_if<ast::Identifier>(&expr.kind))
            return &lower->name;
        return nullptr;
    }

    auto qualifierPath(const ast::Expr& expr, std::string& out) const -> bool {
        if (const auto* upper =
                std::get_if<ast::UpperIdentifier>(&expr.kind)) {
            out = upper->name;
            return true;
        }
        const auto* call = std::get_if<ast::MethodCall>(&expr.kind);
        if (!call || !call->receiver || !call->args.empty() ||
            !call->namedArgs.empty() || call->block || call->parenthesized)
            return false;
        if (!qualifierPath(*call->receiver, out)) return false;
        out += "." + call->method;
        return true;
    }

    auto substitute(ast::ExprPtr& slot) -> void {
        if (!slot) return;
        if (onSlot && onSlot(slot)) return;
        if (scoped) {
            if (const auto* call = std::get_if<ast::MethodCall>(&slot->kind);
                call && call->receiver && call->args.empty() &&
                call->namedArgs.empty() && !call->block) {
                std::string owner;
                if (qualifierPath(*call->receiver, owner)) {
                    auto scope = scoped->find(owner);
                    if (scope != scoped->end()) {
                        auto found = scope->second.find(call->method);
                        if (found != scope->second.end()) {
                            std::string why;
                            auto literal = valueToLiteral(
                                found->second, slot->location, why, reify);
                            if (literal) {
                                slot = std::move(literal);
                                return;
                            }
                            remaining.push_back(
                                qualifiedName(owner, call->method));
                            return;
                        }
                    }
                }
            }
        }
        if (const auto* name = nameOf(*slot)) {
            auto found = constants.find(*name);
            if (found != constants.end() && !shadowed.count(*name)) {
                std::string why;
                auto literal =
                    valueToLiteral(found->second, slot->location, why, reify);
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

    auto patternNames(const ast::Pattern& pattern,
                      std::vector<std::string>& names) -> void {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ast::VarPattern>) {
                if (node.name != "_") names.push_back(node.name);
            } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
                if (node.inner) patternNames(*node.inner, names);
            } else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
                for (const auto& arg : node.args)
                    if (arg) patternNames(*arg, names);
            } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
                for (const auto& field : node.fields) {
                    if (field.pattern) patternNames(**field.pattern, names);
                    else names.push_back(field.name);
                }
            } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
                for (const auto& element : node.elements)
                    if (element) patternNames(*element, names);
                if (node.rest) patternNames(**node.rest, names);
            } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
                for (const auto& element : node.elements)
                    if (element) patternNames(*element, names);
            } else if constexpr (std::is_same_v<T, ast::RangePattern>) {
                if (node.start) patternNames(*node.start, names);
                if (node.end) patternNames(*node.end, names);
            }
        }, pattern.kind);
    }

    template <typename F>
    auto inScope(const std::vector<std::string>& names, F&& fn) -> void {
        std::vector<std::string> added;
        for (const auto& name : names)
            if (shadowed.insert(name).second) added.push_back(name);
        fn();
        for (const auto& name : added) shadowed.erase(name);
    }

    auto body(std::vector<ast::ExprPtr>& expressions) -> void {
        std::vector<std::string> locals;
        inScope(locals, [&] {
            for (auto& expression : expressions) {
                substitute(expression);
                if (!expression) continue;
                if (auto* let = std::get_if<ast::LetExpr>(&expression->kind)) {
                    std::vector<std::string> names;
                    if (let->pattern) patternNames(*let->pattern, names);
                    for (const auto& name : names) {
                        if (shadowed.insert(name).second) locals.push_back(name);
                    }
                } else if (auto* var = std::get_if<ast::VarExpr>(&expression->kind)) {
                    if (shadowed.insert(var->name).second)
                        locals.push_back(var->name);
                }
            }
        });
        for (const auto& name : locals) shadowed.erase(name);
    }
    auto each(std::optional<ast::ExprPtr>& slot) -> void {
        if (slot) substitute(*slot);
    }
    auto each(std::vector<std::pair<std::string, ast::ExprPtr>>& list) -> void {
        for (auto& [_, item] : list) substitute(item);
    }
    auto each(std::vector<ast::RecordEntry>& list) -> void {
        for (auto& entry : list) substitute(entry.value);
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

    // Stamp `at` onto a subtree wholesale, reusing this walk for its
    // traversal — onSlot returns false, so it marks and keeps descending.
    //
    // Column-accurate locations inside an interpolation would need the lexer
    // to track the offset of each `${`, which it does not; the enclosing
    // string's position is both honest and useful, where `<stdin>:1:1` is
    // neither.
    auto reanchor(ast::ExprPtr& root, const SourceLocation& at) -> void {
        Constants none;
        std::vector<semantic::Diagnostic> sink;
        Substituter stamp{none, sink, {}, {}, {}};
        stamp.onSlot = [&at](ast::ExprPtr& slot) {
            if (slot) slot->location = at;
            return false;
        };
        stamp.substitute(root);
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
    auto splitInterpolation(ast::StringLiteral& literal,
                            const SourceLocation& at) -> void {
        if (!literal.interpolating || !literal.parts.empty()) return;
        const std::string& text = literal.value;
        bool mentions = false;
        for (const auto& [name, _] : constants)
            if (text.find(name) != std::string::npos) { mentions = true; break; }
        if (!mentions && !(alsoSplit && alsoSplit(text))) return;

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
                // The mini-parser starts from a fresh Lexer, so everything it
                // produces claims `<stdin>:1:N`. Re-anchor it to where the
                // string actually is, or every diagnostic and every
                // `--collapse-report` line about an interpolated expression
                // points at a file that does not exist.
                reanchor(parsed, at);
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
                    splitInterpolation(node, expr.location);
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
                        body(node.body);
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
                    std::vector<std::string> names;
                    for (const auto& param : node.params)
                        if (param.name != "_") names.push_back(param.name);
                    inScope(names, [&] {
                        body(node.body);
                        if (node.rescue) each(*node.rescue);
                    });
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
            for (auto& param : clause.params)
                if (param.defaultValue) substitute(*param.defaultValue);
            std::vector<std::string> names;
            for (const auto& param : clause.params) {
                if (param.name && *param.name != "_") names.push_back(*param.name);
                if (param.pattern) patternNames(**param.pattern, names);
            }
            inScope(names, [&] {
                body(clause.body);
                if (clause.rescue) each(*clause.rescue);
            });
        }
    }
};

// Turn the driver loops in a generated `make`'s body into the methods they
// declared. The evaluator already ran them (in the scope that had this make's
// own loop variable bound) and recorded the results on `decl.nested`; this
// splices those in and drops the loops, so what reaches a backend is an
// ordinary `make` full of ordinary methods.
//
// Each nested declaration carries its OWN captured bindings — the inner loop's
// variable — which the outer make's bindings do not include, so the
// substitution has to happen here rather than being left to the caller's pass.
auto spliceNestedMethods(ast::MakeDef& make,
                         const interpreter::Evaluator::GeneratedDeclaration& decl,
                         std::vector<semantic::Diagnostic>& diagnostics,
                         bool& ok) -> void {
    decltype(make.body) kept;
    for (auto& item : make.body)
        if (!std::holds_alternative<ast::ExprPtr>(item))
            kept.push_back(std::move(item));
    make.body = std::move(kept);

    for (const auto& nested : decl.nested) {
        const auto* fnTemplate =
            std::get_if<std::shared_ptr<ast::FunctionDef>>(&nested.function);
        if (!fnTemplate || !*fnTemplate) {
            // Not a missing feature: a `make` body holds methods, and
            // `make Foo do type Q = Int end` is "Unexpected token in make
            // body" in ordinary source too. Generation is held to the same
            // grammar as what you could write by hand.
            addError(diagnostics, nested.location,
                     "a `make` body holds methods, so only `let %name(...)` "
                     "can be generated inside one — the same rule an ordinary "
                     "`make` follows");
            ok = false;
            continue;
        }
        if (nested.name.empty()) {
            addError(diagnostics, nested.location,
                     "generated method has an empty name");
            ok = false;
            continue;
        }
        auto method = ast::clone(**fnTemplate);
        method->name = nested.name;
        method->location = nested.location;
        Constants captured;
        for (const auto& [bound, value] : nested.bindings)
            captured.emplace(bound, value);
        for (const auto& clause : method->clauses)
            for (const auto& param : clause.params)
                if (param.name) captured.erase(*param.name);
        Substituter inner{captured, diagnostics, {}, {}, {}, {}, {}};
        inner.functionBody(*method);
        make.body.push_back(std::move(method));
    }
}

// Hygiene substitution over every method of a generated `make`.
//
// Each method's own parameters must WIN over a captured loop variable of the
// same name, and the map is shared across methods, so the erasures have to be
// undone between them — hence the per-method copy rather than one mutated map.
auto substituteMakeBodies(ast::MakeDef& make, Substituter& base,
                          const Constants& captured) -> void {
    auto forFunction = [&](ast::FunctionDef& fn) {
        Constants shadowed = captured;
        for (const auto& clause : fn.clauses)
            for (const auto& param : clause.params)
                if (param.name) shadowed.erase(*param.name);
        Substituter inner{
            shadowed, base.diagnostics, {}, {}, {}, {}, {}, base.reify};
        inner.functionBody(fn);
    };
    for (auto& item : make.body) {
        std::visit(
            [&](auto& node) {
                using Ptr = std::decay_t<decltype(*node)>;
                if (!node) return;
                if constexpr (std::is_same_v<Ptr, ast::FunctionDef>) {
                    forFunction(*node);
                } else if constexpr (std::is_same_v<Ptr, ast::VisibilityBlock>) {
                    for (auto& inner : node->items)
                        if (auto* fn = std::get_if<
                                std::unique_ptr<ast::FunctionDef>>(&inner))
                            if (*fn) forFunction(**fn);
                }
                // TypeAnnotation holds types, not expressions — nothing to do.
            },
            item);
    }
}

// Applies `visitor` to every function/main body reachable in `items`.
//
// `intoCompiled` false stops at a `compiled do` block. Constant substitution
// wants to go in (a constant may be used by another declaration there); chain
// collapse must not, since those bodies ARE the builder being collapsed.
template <typename Items, typename Visit>
auto walkBodies(Items& items, Visit&& visit, bool intoCompiled = true) -> void {
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
                    walkBodies(node->body, visit, intoCompiled);
                } else if constexpr (std::is_same_v<Ptr, ast::ModuleDef>) {
                    walkBodies(node->body, visit, intoCompiled);
                } else if constexpr (std::is_same_v<Ptr, ast::CompiledBlock>) {
                    if (intoCompiled) walkBodies(node->items, visit, intoCompiled);
                } else if constexpr (std::is_same_v<Ptr, ast::VisibilityBlock>) {
                    walkBodies(node->items, visit, intoCompiled);
                }
            },
            item);
    }
}

// Like walkBodies, but deliberately stops at a nested module. Compile-time
// constants are lexical: a bare `X` in module A must not be substituted with
// module B's `X`, so each module gets its own Substituter invocation.
template <typename Items, typename Visit>
auto walkScopeBodies(Items& items, Visit&& visit) -> void {
    for (auto& item : items) {
        std::visit(
            [&](auto& node) {
                using Ptr = std::decay_t<decltype(*node)>;
                if (!node) return;
                if constexpr (std::is_same_v<Ptr, ast::FunctionDef>) {
                    visit(*node);
                } else if constexpr (std::is_same_v<Ptr, ast::MainBlock>) {
                    for (auto& expr : node->body) visit(expr);
                } else if constexpr (std::is_same_v<Ptr, ast::MakeDef>) {
                    walkScopeBodies(node->body, visit);
                } else if constexpr (std::is_same_v<Ptr, ast::VisibilityBlock>) {
                    walkScopeBodies(node->items, visit);
                } else if constexpr (std::is_same_v<Ptr, ast::CompiledBlock>) {
                    walkScopeBodies(node->items, visit);
                }
                // ModuleDef starts a new lexical constant scope and is walked
                // by the caller with a different Substituter.
            },
            item);
    }
}

// ---------------------------------------------------------------------------
// Chain collapse
//
// A method declared inside a `compiled do` block, called as the TERMINAL of a
// chain whose every argument is already known at compile time, is evaluated
// during compilation and the result reified in place. That is what turns a
// builder like `SQL.select(:all).from(:users).emit()` from a sequence of
// record allocations into the string literal it always described.
//
// The rule is deliberately SYNTACTIC — no dataflow analysis. An expression
// qualifies when the walk can see that it is made of literals and compiled
// calls, and nothing else. Anything mentioning a runtime name (a parameter, a
// local, a global) is left to run at runtime, which is also the entire reason
// `q2`/`q3` in examples/compiled_sql.kex still build normally: free-variable
// placeholders are the next slice, not this one.
//
// Evaluation is attempted, not proven: a candidate that throws in the sandbox
// simply keeps its runtime form. Compiled-block code is required to be pure,
// so a failed attempt leaves nothing behind.
// ---------------------------------------------------------------------------

// Names declared inside a `compiled do` block that a use site can call: the
// block's own functions plus the methods of any `make` it holds. A chain is a
// candidate only if its outermost call names one of these, so ordinary runtime
// code is never evaluated at compile time.
struct CompiledNames {
    // Free/top-level compiled functions.
    std::unordered_set<std::string> functions;
    // Functions declared directly in a module's compiled block (`Css::px`).
    std::unordered_set<std::string> qualified;
    // Receiver methods declared by a make inside a compiled block. Their
    // owner is determined by the runtime receiver, not a namespace qualifier.
    std::unordered_set<std::string> methods;

    auto empty() const -> bool {
        return functions.empty() && qualified.empty() && methods.empty();
    }
};

auto collectMakeMethods(const ast::MakeDef& make, CompiledNames& into) -> void {
    auto collect = [&](auto&& self, const auto& items) -> void {
        for (const auto& entry : items) {
            std::visit(
                [&](const auto& node) {
                    using Ptr = std::decay_t<decltype(*node)>;
                    if (!node) return;
                    if constexpr (std::is_same_v<Ptr, ast::FunctionDef>) {
                        into.methods.insert(node->name);
                    } else if constexpr (
                        std::is_same_v<Ptr, ast::VisibilityBlock>) {
                        self(self, node->items);
                    }
                },
                entry);
        }
    };
    collect(collect, make.body);
}

auto collectDeclaredNames(const std::vector<ast::CompiledItem>& items,
                          const std::string& scope,
                          CompiledNames& into) -> void {
    for (const auto& entry : items) {
        if (const auto* fn =
                std::get_if<std::unique_ptr<ast::FunctionDef>>(&entry)) {
            if (!*fn || isValueBinding(**fn)) continue;
            if (scope.empty()) into.functions.insert((*fn)->name);
            else into.qualified.insert(qualifiedName(scope, (*fn)->name));
        } else if (const auto* make =
                       std::get_if<std::unique_ptr<ast::MakeDef>>(&entry)) {
            if (*make) collectMakeMethods(**make, into);
        }
    }
}

// Every `compiled do` block in the program, at top level or inside a module.
auto compiledNames(const ast::Program& program) -> CompiledNames {
    CompiledNames names;
    auto scan = [&](auto&& self, const auto& items,
                    const std::string& scope) -> void {
        for (const auto& item : items) {
            if (const auto* block =
                    std::get_if<std::unique_ptr<ast::CompiledBlock>>(&item)) {
                if (*block) collectDeclaredNames((*block)->items, scope, names);
            } else if (const auto* mod =
                           std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*mod) self(self, (*mod)->body, (*mod)->name);
            }
        }
    };
    scan(scan, program.items, std::string{});
    return names;
}

// Folds `Kex.embed("path")` into the literal text of that file, read at
// compile time (kexhq/kex#171 M2). Unlike chain collapse below, this needs
// no sandbox run: there is nothing to evaluate, only a syntactic shape to
// recognize and a file to read, so `claim` does the whole job in place.
//
// The argument must be a PLAIN string literal — not interpolated, not a
// runtime expression — because the path has to be knowable to the compiler.
// `Template.embed("build/${target}.ket")` cannot mean anything at compile
// time no matter how eagerly this walk runs.
struct EmbedFolder {
    // The directory `Kex.embed`'s relative paths resolve against: the
    // enclosing source file's own directory, exactly like `#include` or
    // Rust's `include_str!` — never the process's current directory, which
    // would make the same program embed a different file depending on where
    // it happened to be invoked from.
    std::filesystem::path sourceDir;
    std::vector<semantic::Diagnostic>& diagnostics;
    bool ok = true;

    static auto isKexReceiver(const ast::Expr& expr) -> bool {
        const auto* upper = std::get_if<ast::UpperIdentifier>(&expr.kind);
        return upper && upper->name == "Kex";
    }

    auto fail(const SourceLocation& location, std::string message) -> bool {
        addError(diagnostics, location, std::move(message));
        ok = false;
        return true; // claimed: still stop the walk here either way.
    }

    auto claim(ast::ExprPtr& slot) -> bool {
        if (!slot) return false;
        const auto* call = std::get_if<ast::MethodCall>(&slot->kind);
        if (!call || call->method != "embed" || !call->receiver ||
            !isKexReceiver(*call->receiver))
            return false;
        if (call->args.size() != 1 || !call->namedArgs.empty() || call->block)
            return fail(slot->location,
                        "Kex.embed takes exactly one argument: the path of "
                        "the file to embed");
        const auto* literal =
            std::get_if<ast::StringLiteral>(&call->args[0]->kind);
        // An ordinary `"..."` literal is ALWAYS parsed with interpolating =
        // true and its text split into `parts`/`values` (parser.cxx), even
        // when it holds no `${...}` at all — interpolating only means "this
        // token could have held one", not "it does". What actually matters
        // here is whether any runtime VALUE was interpolated: `values` is
        // empty for both a raw string and a plain `"data.txt"`.
        std::string path;
        if (!literal) {
            return fail(slot->location,
                        "Kex.embed's argument must be a plain string "
                        "literal naming the file to embed — the path has to "
                        "be knowable to the compiler, not just to the "
                        "running program");
        } else if (!literal->interpolating) {
            path = literal->value;
        } else if (literal->values.empty()) {
            path = literal->parts.empty() ? std::string{} : literal->parts.front();
        } else {
            return fail(slot->location,
                        "Kex.embed's argument must be a plain string "
                        "literal naming the file to embed, not an "
                        "interpolated one — the path has to be knowable to "
                        "the compiler, not just to the running program");
        }
        std::filesystem::path target = sourceDir / path;
        std::ifstream file(target, std::ios::binary);
        if (!file)
            return fail(slot->location, "Kex.embed: cannot read '" +
                                             target.string() + "'");
        std::ostringstream contents;
        contents << file.rdbuf();
        slot->kind = ast::StringLiteral{contents.str(), false};
        return true;
    }
};

// Runs EmbedFolder over the whole program — every function, `make` method
// and `main` body, inside a `compiled do` block or not. Deliberately NOT
// gated behind whether the program has any `compiled` block at all: embed
// folding is unconditional, unlike everything below it in this file.
auto foldEmbeds(ast::Program& program, const std::string& sourcePath,
                 std::vector<semantic::Diagnostic>& diagnostics) -> bool {
    EmbedFolder folder{
        std::filesystem::path(sourcePath).parent_path(), diagnostics};
    Constants none;
    Substituter walk{none, diagnostics, {}, {}, {}};
    walk.onSlot = [&](ast::ExprPtr& slot) { return folder.claim(slot); };
    walkBodies(program.items, [&](auto& target) {
        using T = std::decay_t<decltype(target)>;
        if constexpr (std::is_same_v<T, ast::FunctionDef>)
            walk.functionBody(target);
        else
            walk.substitute(target);
    });
    return folder.ok;
}

// ---------------------------------------------------------------------------
// Template lowering (kexhq/kex#171 M3): `Template.text(source)` /
// `Template.html(source)`, given a compile-time-known SOURCE STRING (almost
// always `Kex.embed(...)`, already folded to a literal by foldEmbeds above),
// become a brand-new top-level function — the holes compiled into real,
// type-checked Kex, exactly as the design insists on ("the mechanism is
// compilation, not interpretation"). The call site is replaced with `~name`,
// a reference to that function, which a caller then invokes with named
// arguments for whatever free names the template's holes turned out to need.
//
// The SCANNING half is not reimplemented here: M1's `Template.scan` (already
// shipped, kexhq/kex#252) is the one source of truth for ERB syntax. This
// runs it for real, through the same sandboxed Evaluator `compiled do`
// blocks use, and reads the result straight out of the returned Value. Only
// the STRUCTURE Template.scan deliberately leaves opaque — control regions,
// nested `if`/`do |x| ... end` — is reconstructed here, and even that is
// built as real AST via the real Parser on each opaque Control node's own
// text, never by re-serializing a whole template back to source and
// re-lexing it (which would need to re-escape every Text run for no reason —
// building the AST directly needs no escaping at all).
//
// Supported control regions: `if`/`elif`/`else`, `let`/`var`, and any
// block-taking call (`<%= expr do |params| %> ... <% end %>` — most
// commonly `.map`/`.mapIndexed`/`.flatMap`), nested to any depth. Anything
// else (`match`, `loop`, `while`, `receive`, `spawn`, `with`, `trying`) is
// not specially recognized, and — because every one of those ALSO ends its
// opening line in `do` — falls into the block-taking-call path, which wraps
// it in `.join("")`. The real type checker then reports whatever error that
// produces (`.join` on a process handle, say). Not a polished message, but a
// safe, loud failure rather than a silently wrong template, and it needs no
// rejection list kept in sync with the grammar.
//
// Free variables become the generated function's (unannotated) parameters,
// found by a syntactic scan of the lowered body — every bare Identifier not
// bound by an enclosing let/var/block-param within the template itself. This
// does not attempt to exclude a bare reference to a prelude or module-scope
// name the way a fully resolution-aware pass would (kexhq/kex#171's own
// design doc flags exactly this nuance) — expand() runs before semantic
// analysis, so no name resolution exists yet to lean on. A template whose
// hole bare-references an unqualified top-level function by name, with no
// arguments, will incorrectly treat it as a required parameter; qualified
// (`Mod.fn`) and UFCS (`x.fn`) references are unaffected, since neither
// parses as a bare Identifier. A `params: [...]` frontmatter key (already
// implemented by `Template.scan`/`Parsed#parameters`) overrides inference
// when present, fixing both the name list and its order — but not yet the
// declared TYPES it may also carry, which are dropped: relying on
// inference for those is the accepted v1 gap.
// ---------------------------------------------------------------------------

// One node of a scanned template, read directly out of the Value
// Template.scan returned.
struct TemplateNode {
    enum class Kind { Text, Interpolate, InterpolateRaw, Control, Comment };
    Kind kind;
    std::string text;
};

auto templateNodeKind(const std::string& tag) -> std::optional<TemplateNode::Kind> {
    if (tag == "Text") return TemplateNode::Kind::Text;
    if (tag == "Interpolate") return TemplateNode::Kind::Interpolate;
    if (tag == "InterpolateRaw") return TemplateNode::Kind::InterpolateRaw;
    if (tag == "Control") return TemplateNode::Kind::Control;
    if (tag == "Comment") return TemplateNode::Kind::Comment;
    return std::nullopt;
}

auto trimmed(std::string text) -> std::string {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

// Runs the REAL `Template.scan` at compile time (via the same sandboxed
// Evaluator `compiled do` blocks use) and reads its `nodes` and frontmatter
// `params` directly out of the returned Value — no scanning logic duplicated
// here. `Template.scan` resolves fully qualified with no `using Template`
// needed, so this asks for it directly against an otherwise empty program.
//
// Returns false and sets `error` on a scan failure or a sandbox crash; both
// are compile errors at the call site, never a silently empty template.
auto scanTemplateSource(const std::string& source, std::chrono::milliseconds timeout,
                         std::vector<TemplateNode>& nodes,
                         std::vector<std::string>& explicitParams,
                         std::string& error) -> bool {
    ast::Expr sourceLit;
    sourceLit.kind = ast::StringLiteral{source, false};
    ast::Expr receiver;
    receiver.kind = ast::UpperIdentifier{"Template"};
    ast::Expr callExpr;
    {
        ast::MethodCall call;
        call.receiver = std::make_unique<ast::Expr>(std::move(receiver));
        call.method = "scan";
        call.args.push_back(std::make_unique<ast::Expr>(std::move(sourceLit)));
        call.parenthesized = true;
        callExpr.kind = std::move(call);
    }

    ast::Program empty;
    std::vector<interpreter::ValuePtr> results;
    std::vector<std::string> reasons;
    try {
        interpreter::Evaluator evaluator;
        evaluator.loadPrelude();
        interpreter::Evaluator::ExpressionRequest request{&callExpr, {}};
        results = evaluator.evaluateExpressions(empty, {request}, timeout, &reasons);
    } catch (const interpreter::EvaluationTimeout&) {
        error = "template scan timed out after " + std::to_string(timeout.count()) + " ms";
        return false;
    } catch (const std::exception& crash) {
        error = std::string("compile-time evaluation failed: ") + crash.what();
        return false;
    }
    if (results.empty() || !results[0]) {
        error = (reasons.empty() || reasons[0].empty())
            ? "Template.scan could not be evaluated at compile time"
            : reasons[0];
        return false;
    }
    auto* outcome = std::get_if<interpreter::VariantValue>(&results[0]->data);
    if (!outcome) { error = "Template.scan did not return a Result"; return false; }
    if (outcome->tag != "Ok") {
        std::string message = (outcome->args.empty() || !outcome->args[0])
            ? "scan failed" : outcome->args[0]->inspect();
        error = "template does not scan: " + message;
        return false;
    }
    if (outcome->args.empty() || !outcome->args[0]) {
        error = "Template.scan returned an unexpected Ok value";
        return false;
    }
    auto* parsed = std::get_if<interpreter::RecordValue>(&outcome->args[0]->data);
    if (!parsed) { error = "Template.scan's Ok value is not a Parsed record"; return false; }

    auto nodesField = parsed->fields.find("nodes");
    if (nodesField == parsed->fields.end() || !nodesField->second) {
        error = "Parsed has no `nodes` field"; return false;
    }
    auto* list = std::get_if<interpreter::ListValue>(&nodesField->second->data);
    if (!list) { error = "Parsed.nodes is not a list"; return false; }
    for (const auto& elem : list->elements) {
        if (!elem) continue;
        auto* variant = std::get_if<interpreter::VariantValue>(&elem->data);
        if (!variant || variant->args.size() != 1 || !variant->args[0]) {
            error = "Parsed.nodes holds something other than a Node";
            return false;
        }
        auto kind = templateNodeKind(variant->tag);
        if (!kind) { error = "unknown template node: " + variant->tag; return false; }
        auto* text = std::get_if<interpreter::StringValue>(&variant->args[0]->data);
        if (!text) { error = "a template Node's payload is not a String"; return false; }
        nodes.push_back({*kind, text->value});
    }

    // Explicit `params: [...]` frontmatter, split the same way
    // `Parsed#parameters` does: each entry on its first `:`. Only the name
    // half is kept — see the file-level comment on the dropped-types gap.
    auto frontField = parsed->fields.find("frontmatter");
    if (frontField != parsed->fields.end() && frontField->second) {
        if (auto* map = std::get_if<interpreter::MapValue>(&frontField->second->data)) {
            for (const auto& [key, value] : map->entries) {
                const auto* keyStr = key ? std::get_if<interpreter::StringValue>(&key->data) : nullptr;
                if (!keyStr || keyStr->value != "params" || !value) continue;
                const auto* tag = std::get_if<interpreter::VariantValue>(&value->data);
                if (!tag || tag->tag != "Tags" || tag->args.empty() || !tag->args[0]) continue;
                const auto* entries = std::get_if<interpreter::ListValue>(&tag->args[0]->data);
                if (!entries) continue;
                for (const auto& entry : entries->elements) {
                    const auto* entryStr = entry ? std::get_if<interpreter::StringValue>(&entry->data) : nullptr;
                    if (!entryStr) continue;
                    const auto colon = entryStr->value.find(':');
                    auto name = trimmed(colon == std::string::npos
                                            ? entryStr->value : entryStr->value.substr(0, colon));
                    if (!name.empty()) explicitParams.push_back(name);
                }
            }
        }
    }
    return true;
}

// What one Control node's own text turns out to be, decided from its OWN
// tokens alone (never from what comes before or after it in the node list —
// that is `Lowerer::lowerScope`'s job).
enum class ControlKind { End, Else, Elif, If, LetVar, BlockOpener, Plain };

struct ControlShape {
    ControlKind kind = ControlKind::Plain;
    std::vector<Token> tokens; // this node's own tokens, EOF excluded
    Token eof{TokenType::Eof, "", SourceLocation{}, -1, -1};
    std::size_t keywordEnd = 0;    // If/Elif: condition tokens start here
    std::size_t blockOpenAt = 0;   // BlockOpener: index of the `do` token
    std::vector<std::string> blockParams; // BlockOpener: `|params|`, if any
};

auto classifyControl(const std::string& text) -> ControlShape {
    ControlShape shape;
    Lexer lexer(text, "<template>");
    for (auto& tok : lexer.tokenizeAll()) {
        if (tok.type == TokenType::Eof) { shape.eof = tok; break; }
        shape.tokens.push_back(std::move(tok));
    }
    if (shape.tokens.empty()) return shape; // Plain, and lowerScope skips it

    const auto& first = shape.tokens.front();
    if (shape.tokens.size() == 1 && first.type == TokenType::End) {
        shape.kind = ControlKind::End;
        return shape;
    }
    if (first.type == TokenType::Else) { shape.kind = ControlKind::Else; return shape; }
    if (first.type == TokenType::Elif) {
        shape.kind = ControlKind::Elif;
        shape.keywordEnd = 1;
        return shape;
    }
    if (first.type == TokenType::If) {
        shape.kind = ControlKind::If;
        shape.keywordEnd = 1;
        return shape;
    }
    if (first.type == TokenType::Let || first.type == TokenType::Var) {
        shape.kind = ControlKind::LetVar;
        return shape;
    }

    // A block opener: the token stream ends in `do`, or `do` followed by a
    // `|param, param|` list and nothing else.
    std::size_t doIdx = shape.tokens.size();
    for (std::size_t i = shape.tokens.size(); i-- > 0;) {
        if (shape.tokens[i].type == TokenType::Do) { doIdx = i; break; }
        if (shape.tokens[i].type != TokenType::Pipe &&
            shape.tokens[i].type != TokenType::LowerIdent &&
            shape.tokens[i].type != TokenType::Underscore &&
            shape.tokens[i].type != TokenType::Comma)
            break;
    }
    if (doIdx < shape.tokens.size()) {
        shape.kind = ControlKind::BlockOpener;
        shape.blockOpenAt = doIdx;
        if (doIdx + 1 < shape.tokens.size() && shape.tokens[doIdx + 1].type == TokenType::Pipe) {
            std::size_t j = doIdx + 2;
            while (j < shape.tokens.size() && shape.tokens[j].type != TokenType::Pipe) {
                if (shape.tokens[j].type == TokenType::LowerIdent)
                    shape.blockParams.push_back(shape.tokens[j].value);
                else if (shape.tokens[j].type == TokenType::Underscore)
                    shape.blockParams.push_back("_");
                j++;
            }
        }
        return shape;
    }
    return shape; // Plain
}

// Lowers a scanned template's flat node list into a Kex function body,
// recursively reconstructing `if`/block-call nesting. See the file-level
// comment above for the algorithm and its known gaps.
struct Lowerer {
    const std::vector<TemplateNode>& nodes;
    bool htmlMode;
    SourceLocation blame;
    std::vector<semantic::Diagnostic>& diagnostics;
    bool ok = true;

    auto parseSlice(const std::vector<Token>& toks, std::size_t from, std::size_t to,
                    const Token& eof, const char* what) -> ast::ExprPtr {
        std::vector<Token> slice(toks.begin() + static_cast<std::ptrdiff_t>(from),
                                 toks.begin() + static_cast<std::ptrdiff_t>(to));
        slice.push_back(eof);
        Parser parser(std::move(slice), "<template>");
        auto expr = parser.parseExpr();
        if (!expr || !parser.diagnostics().empty()) {
            ok = false;
            addError(diagnostics, blame,
                     std::string("in a template ") + what + ": " +
                         (parser.diagnostics().empty() ? std::string("expected an expression")
                                                        : parser.diagnostics().front().message));
            return nullptr;
        }
        return expr;
    }

    auto parseHole(const std::string& text) -> ast::ExprPtr {
        Lexer lexer(text, "<template>");
        Parser parser(lexer.tokenizeAll(), "<template>");
        auto expr = parser.parseExpr();
        if (!expr || !parser.diagnostics().empty()) {
            ok = false;
            addError(diagnostics, blame,
                     "in a template hole `" + text + "`: " +
                         (parser.diagnostics().empty() ? std::string("expected an expression")
                                                        : parser.diagnostics().front().message));
            return nullptr;
        }
        return expr;
    }

    auto stringLiteral(std::string text) const -> ast::ExprPtr {
        auto e = std::make_unique<ast::Expr>();
        e->location = blame;
        e->kind = ast::StringLiteral{std::move(text), false};
        return e;
    }

    // Ordinary Kex string interpolation already show-converts every
    // splice — `wrapInShowCalls` in parser.cxx does exactly this for
    // hand-written `"${expr}"` — so a hole gets the same treatment here.
    auto showWrap(ast::ExprPtr expr) const -> ast::ExprPtr {
        auto call = std::make_unique<ast::Expr>();
        call->location = blame;
        ast::MethodCall mc;
        mc.receiver = std::move(expr);
        mc.method = "showValue";
        call->kind = std::move(mc);
        return call;
    }

    auto htmlEscapeWrap(ast::ExprPtr expr) const -> ast::ExprPtr {
        auto receiver = std::make_unique<ast::Expr>();
        receiver->location = blame;
        receiver->kind = ast::UpperIdentifier{"Template"};
        auto call = std::make_unique<ast::Expr>();
        call->location = blame;
        ast::MethodCall mc;
        mc.receiver = std::move(receiver);
        mc.method = "escapeHtml";
        mc.parenthesized = true;
        mc.args.push_back(std::move(expr));
        call->kind = std::move(mc);
        return call;
    }

    // One fragment: a plain run of Text/Interpolate/InterpolateRaw becomes
    // an interpolated-string AST node, built DIRECTLY — the Text portions
    // are the runtime bytes verbatim, needing no escaping at all, since this
    // never passes through source text.
    auto buildRun(const std::vector<TemplateNode>& run) -> ast::ExprPtr {
        ast::StringLiteral literal;
        literal.interpolating = true;
        std::string current;
        for (const auto& node : run) {
            if (node.kind == TemplateNode::Kind::Text) {
                current += node.text;
                continue;
            }
            literal.parts.push_back(current);
            current.clear();
            auto hole = parseHole(node.text);
            if (!hole) hole = stringLiteral("");
            auto wrapped = showWrap(std::move(hole));
            if (htmlMode && node.kind == TemplateNode::Kind::Interpolate)
                wrapped = htmlEscapeWrap(std::move(wrapped));
            literal.values.push_back(std::move(wrapped));
        }
        literal.parts.push_back(current);
        auto e = std::make_unique<ast::Expr>();
        e->location = blame;
        e->kind = std::move(literal);
        return e;
    }

    // `Substituter::walkChildren` (used below by collectFreeNames) walks an
    // if/lambda BRANCH body with a plain per-element loop, not its
    // shadow-tracking `body()` — a `let` inside a branch never enters
    // `shadowed` there. Naming each fragment (`let __fragN = ...`) hit
    // exactly that gap: the name "used" itself, one line down, inside the
    // very branch that bound it, so it looked free from outside. Rather
    // than change a walk the rest of this file already depends on, nested
    // scopes here never introduce a name at all — fragments are `+`-chained
    // directly as one expression. This does accept one narrow trade: a
    // hole referencing a name BEFORE a same-named `let` rebinds it, later
    // in the SAME nested branch, resolves to the rebound value instead of
    // the original — correct at the top level (summed immediately, before
    // any later `let` in the same scope), and rare enough elsewhere to
    // accept for v1.
    auto sumOf(std::vector<ast::ExprPtr> fragments) const -> ast::ExprPtr {
        if (fragments.empty()) return stringLiteral("");
        ast::ExprPtr acc = std::move(fragments.front());
        for (std::size_t i = 1; i < fragments.size(); i++) {
            auto e = std::make_unique<ast::Expr>();
            e->location = blame;
            e->kind = ast::BinaryOp{std::move(acc), TokenType::Plus, std::move(fragments[i])};
            acc = std::move(e);
        }
        return acc;
    }

    enum class Stop { End, Else, Elif, Eof };
    struct ScopeResult {
        std::vector<ast::ExprPtr> statements; // ends in the scope's summed value
        Stop stop = Stop::Eof;
        ast::ExprPtr elifCondition; // set only when stop == Elif
        std::size_t next = 0;       // index just past what this scope consumed
    };

    // Lowers one scope: everything from `start` up to (not including) the
    // Control node that closes it — its own "end", or an "else"/"elif" the
    // if-branch loop below is watching for. `let`/`var` and other
    // pass-through control regions stay their own statements, in source
    // order; every value-producing piece (a plain run, a nested `if`, a
    // nested block call) is collected and `+`-chained into ONE final
    // statement — the scope's own value, purely, with nothing buffered or
    // mutated.
    auto lowerScope(std::size_t start) -> ScopeResult {
        std::vector<ast::ExprPtr> statements;
        std::vector<ast::ExprPtr> fragments;
        std::vector<TemplateNode> run;

        auto flushRun = [&]() {
            if (run.empty()) return;
            fragments.push_back(buildRun(run));
            run.clear();
        };

        std::size_t i = start;
        while (i < nodes.size()) {
            const auto& node = nodes[i];
            if (node.kind == TemplateNode::Kind::Comment) { i++; continue; }
            if (node.kind != TemplateNode::Kind::Control) {
                run.push_back(node);
                i++;
                continue;
            }

            auto shape = classifyControl(node.text);
            switch (shape.kind) {
            case ControlKind::End:
                flushRun();
                statements.push_back(sumOf(std::move(fragments)));
                return {std::move(statements), Stop::End, nullptr, i + 1};

            case ControlKind::Else:
                flushRun();
                statements.push_back(sumOf(std::move(fragments)));
                return {std::move(statements), Stop::Else, nullptr, i + 1};

            case ControlKind::Elif: {
                flushRun();
                statements.push_back(sumOf(std::move(fragments)));
                auto cond = parseSlice(shape.tokens, shape.keywordEnd, shape.tokens.size(),
                                       shape.eof, "elif condition");
                return {std::move(statements), Stop::Elif, std::move(cond), i + 1};
            }

            case ControlKind::LetVar: {
                flushRun();
                auto stmt = parseSlice(shape.tokens, 0, shape.tokens.size(), shape.eof,
                                       "let/var region");
                if (stmt) statements.push_back(std::move(stmt));
                i++;
                break;
            }

            case ControlKind::If: {
                flushRun();
                auto cond = parseSlice(shape.tokens, shape.keywordEnd, shape.tokens.size(),
                                       shape.eof, "if condition");
                auto thenResult = lowerScope(i + 1);
                ast::IfExpr ifExpr;
                ifExpr.condition = cond ? std::move(cond) : stringLiteral("");
                ifExpr.thenBody = std::move(thenResult.statements);

                std::size_t cursor = thenResult.next;
                Stop stop = thenResult.stop;
                ast::ExprPtr elifCond = std::move(thenResult.elifCondition);
                while (stop == Stop::Elif) {
                    auto branch = lowerScope(cursor);
                    ifExpr.elifs.emplace_back(std::move(elifCond), std::move(branch.statements));
                    cursor = branch.next;
                    stop = branch.stop;
                    elifCond = std::move(branch.elifCondition);
                }
                bool hasElse = false;
                if (stop == Stop::Else) {
                    auto branch = lowerScope(cursor);
                    ifExpr.elseBody = std::move(branch.statements);
                    cursor = branch.next;
                    stop = branch.stop;
                    hasElse = true;
                }
                if (stop != Stop::End) {
                    ok = false;
                    addError(diagnostics, blame, "template `if` has no matching `end`");
                }
                // No else written: an if used as a value needs one on every
                // arm, so an empty string stands in for "nothing here".
                if (!hasElse) {
                    std::vector<ast::ExprPtr> empty;
                    empty.push_back(stringLiteral(""));
                    ifExpr.elseBody = std::move(empty);
                }

                auto e = std::make_unique<ast::Expr>();
                e->location = blame;
                e->kind = std::move(ifExpr);
                fragments.push_back(std::move(e));
                i = cursor;
                break;
            }

            case ControlKind::BlockOpener: {
                flushRun();
                auto prefix = parseSlice(shape.tokens, 0, shape.blockOpenAt, shape.eof,
                                         "block call");
                auto bodyResult = lowerScope(i + 1);
                if (bodyResult.stop != Stop::End) {
                    ok = false;
                    addError(diagnostics, blame, "template block has no matching `end`");
                }
                ast::Lambda lambda;
                for (const auto& p : shape.blockParams)
                    lambda.params.push_back(ast::LambdaParam{p, std::nullopt});
                lambda.body = std::move(bodyResult.statements);
                auto lambdaExpr = std::make_unique<ast::Expr>();
                lambdaExpr->location = blame;
                lambdaExpr->kind = std::move(lambda);

                bool attached = false;
                if (prefix) {
                    if (auto* mc = std::get_if<ast::MethodCall>(&prefix->kind)) {
                        mc->block = std::move(lambdaExpr);
                        attached = true;
                    } else if (auto* fc = std::get_if<ast::FunctionCall>(&prefix->kind)) {
                        fc->block = std::move(lambdaExpr);
                        attached = true;
                    }
                }
                if (!attached) {
                    ok = false;
                    addError(diagnostics, blame,
                             "template block `" + node.text +
                                 "` is not a call that can take a block");
                    i = bodyResult.next;
                    break;
                }

                auto join = std::make_unique<ast::Expr>();
                join->location = blame;
                ast::MethodCall joinCall;
                joinCall.receiver = std::move(prefix);
                joinCall.method = "join";
                joinCall.parenthesized = true;
                joinCall.args.push_back(stringLiteral(""));
                join->kind = std::move(joinCall);

                fragments.push_back(std::move(join));
                i = bodyResult.next;
                break;
            }

            case ControlKind::Plain: {
                flushRun();
                if (!shape.tokens.empty()) {
                    auto stmt = parseSlice(shape.tokens, 0, shape.tokens.size(), shape.eof,
                                           "control region");
                    if (stmt) statements.push_back(std::move(stmt));
                }
                i++;
                break;
            }
            }
        }
        flushRun();
        statements.push_back(sumOf(std::move(fragments)));
        return {std::move(statements), Stop::Eof, nullptr, i};
    }
};

// Every bare Identifier the lowered body references that it does not itself
// bind — see the file-level comment for what this does and does not catch.
// Reuses Substituter purely for its traversal (empty constants map, onSlot
// only records and never claims), so lambda-param and let-shadowing come
// from the same, already-correct logic the rest of this file relies on.
auto collectFreeNames(std::vector<ast::ExprPtr>& body) -> std::vector<std::string> {
    std::vector<std::string> ordered;
    std::unordered_set<std::string> seen;
    Constants none;
    std::vector<semantic::Diagnostic> unused;
    Substituter walker{none, unused, {}, {}, {}};
    walker.onSlot = [&](ast::ExprPtr& slot) -> bool {
        if (slot) {
            if (const auto* id = std::get_if<ast::Identifier>(&slot->kind)) {
                if (!walker.shadowed.count(id->name) && seen.insert(id->name).second)
                    ordered.push_back(id->name);
            }
        }
        return false;
    };
    walker.body(body);
    return ordered;
}

struct LoweredTemplate {
    std::unique_ptr<ast::FunctionDef> function;
    ast::ExprPtr reference;
};

auto lowerTemplateFunction(bool htmlMode, const std::string& source, const SourceLocation& blame,
                           int& counter, std::chrono::milliseconds timeout,
                           std::vector<semantic::Diagnostic>& diagnostics)
    -> std::optional<LoweredTemplate> {
    std::vector<TemplateNode> nodes;
    std::vector<std::string> explicitParams;
    std::string error;
    if (!scanTemplateSource(source, timeout, nodes, explicitParams, error)) {
        addError(diagnostics, blame, error);
        return std::nullopt;
    }

    Lowerer lowerer{nodes, htmlMode, blame, diagnostics};
    auto result = lowerer.lowerScope(0);
    if (!lowerer.ok) return std::nullopt;
    if (result.stop != Lowerer::Stop::Eof) {
        // Template.scan only ever emits a bare Control("end") for one this
        // pass itself opened (an if or a block), both of which consume
        // their own "end" above — so reaching here means a stray
        // "end"/"else"/"elif" the pass never opened, i.e. the template's
        // control regions do not balance.
        addError(diagnostics, blame,
                 "template's control regions do not balance — an `end`, "
                 "`else` or `elif` with no opener");
        return std::nullopt;
    }

    auto paramNames = explicitParams.empty() ? collectFreeNames(result.statements)
                                             : explicitParams;

    auto fn = std::make_unique<ast::FunctionDef>();
    fn->location = blame;
    fn->name = "__kexTemplate" + std::to_string(counter++);
    ast::FunctionClause clause;
    clause.hasParamList = true;
    for (const auto& p : paramNames) {
        ast::Param param;
        param.name = p;
        clause.params.push_back(std::move(param));
    }
    clause.body = std::move(result.statements);
    fn->clauses.push_back(std::move(clause));

    auto reference = std::make_unique<ast::Expr>();
    reference->location = blame;
    reference->kind = ast::CurryExpr{fn->name, false, {}, ""};

    return LoweredTemplate{std::move(fn), std::move(reference)};
}

struct TemplateCallFolder {
    std::vector<semantic::Diagnostic>& diagnostics;
    std::chrono::milliseconds timeout;
    std::vector<std::unique_ptr<ast::FunctionDef>> generated;
    int counter = 0;
    bool ok = true;

    static auto isTemplateReceiver(const ast::Expr& expr) -> bool {
        const auto* upper = std::get_if<ast::UpperIdentifier>(&expr.kind);
        return upper && upper->name == "Template";
    }

    auto claim(ast::ExprPtr& slot) -> bool {
        if (!slot) return false;
        auto* call = std::get_if<ast::MethodCall>(&slot->kind);
        if (!call || !call->receiver || !isTemplateReceiver(*call->receiver)) return false;
        bool htmlMode;
        if (call->method == "text") htmlMode = false;
        else if (call->method == "html") htmlMode = true;
        else return false;

        if (call->args.size() != 1 || !call->namedArgs.empty() || call->block) {
            addError(diagnostics, slot->location,
                     "Template." + call->method +
                         " takes exactly one argument: the template's source text");
            ok = false;
            return true;
        }
        std::string source;
        bool isLiteral = false;
        if (const auto* literal = std::get_if<ast::StringLiteral>(&call->args[0]->kind)) {
            if (!literal->interpolating) {
                source = literal->value;
                isLiteral = true;
            } else if (literal->values.empty()) {
                source = literal->parts.empty() ? std::string{} : literal->parts.front();
                isLiteral = true;
            }
        }
        if (!isLiteral) {
            addError(diagnostics, slot->location,
                     "Template." + call->method +
                         "'s argument must be a compile-time-known string (a plain literal, "
                         "or Kex.embed(...)) — the template's text has to be knowable to the "
                         "compiler, not just to the running program");
            ok = false;
            return true;
        }

        auto lowered =
            lowerTemplateFunction(htmlMode, source, slot->location, counter, timeout, diagnostics);
        if (!lowered) {
            ok = false;
            return true;
        }
        generated.push_back(std::move(lowered->function));
        slot = std::move(lowered->reference);
        return true;
    }
};

// Runs TemplateCallFolder over the whole program, exactly like foldEmbeds:
// every function/`make` method/`main` body, `compiled do` or not,
// unconditional on the program having any `compiled` block at all.
auto foldTemplates(ast::Program& program, std::vector<semantic::Diagnostic>& diagnostics,
                    const ExpandOptions& options) -> bool {
    TemplateCallFolder folder{diagnostics, options.timeout, {}, 0, true};
    Constants none;
    Substituter walk{none, diagnostics, {}, {}, {}};
    walk.onSlot = [&](ast::ExprPtr& slot) { return folder.claim(slot); };
    walkBodies(program.items, [&](auto& target) {
        using T = std::decay_t<decltype(target)>;
        if constexpr (std::is_same_v<T, ast::FunctionDef>)
            walk.functionBody(target);
        else
            walk.substitute(target);
    });
    for (auto& fn : folder.generated) program.items.push_back(std::move(fn));
    return folder.ok;
}

struct ChainCollapser {
    const CompiledNames& compiled;

    // A claimed expression, and the runtime names it is allowed to carry
    // without knowing. Outermost only: claiming stops the walk from
    // descending, so a nested call is never collected separately.
    struct Candidate {
        ast::ExprPtr* slot;
        std::vector<std::string> names;         // index -> name, as written
        std::vector<const ast::Expr*> exprs;    // index -> what it stands for
    };
    std::vector<Candidate> candidates;

    // Free names seen during the current claim attempt. Mutable because
    // `isStatic` is logically a query and is called from const context, but
    // has to report what it let through — a name is only "static" here on the
    // promise that the caller turns it into a placeholder.
    mutable std::vector<std::string> freeNames;
    mutable std::vector<const ast::Expr*> freeExprs;

    // Expressions a previous round claimed and could not evaluate. Skipping
    // them lets the walk descend INTO a failed chain and collapse whatever
    // part of it does work — without this, one unresolvable `.emit()` at the
    // top costs every static call underneath it.
    const std::unordered_set<const ast::Expr*>* rejected = nullptr;

    // A bare `Foo` in receiver position is a MODULE or TYPE qualifier, not a
    // value — `SQL.select(...)`. Accepted there and nowhere else: treating it
    // as a value would let a top-level binding be baked in, which is exactly
    // what must keep happening at runtime.
    static auto qualifierPath(const ast::Expr& expr, std::string& out) -> bool {
        if (const auto* upper =
                std::get_if<ast::UpperIdentifier>(&expr.kind)) {
            out = upper->name;
            return true;
        }
        const auto* call = std::get_if<ast::MethodCall>(&expr.kind);
        if (!call || !call->receiver || !call->args.empty() ||
            !call->namedArgs.empty() || call->block || call->parenthesized)
            return false;
        if (!qualifierPath(*call->receiver, out)) return false;
        out += "." + call->method;
        return true;
    }

    auto allStatic(const std::vector<ast::ExprPtr>& list) const -> bool {
        for (const auto& item : list)
            if (!item || !isStatic(*item)) return false;
        return true;
    }
    auto allStatic(const std::vector<std::pair<std::string, ast::ExprPtr>>& list)
        const -> bool {
        for (const auto& [_, item] : list)
            if (!item || !isStatic(*item)) return false;
        return true;
    }
    auto allStatic(const std::vector<ast::RecordEntry>& list) const -> bool {
        for (const auto& entry : list)
            if (!entry.value || !isStatic(*entry.value)) return false;
        return true;
    }

    // Is this expression's value determined at compile time, treating any
    // free runtime name as a PLACEHOLDER it may carry but not compute with?
    //
    // By the time collapse runs, every compiled constant is already a literal,
    // so a lower-case name left standing is a runtime binding. It does not
    // disqualify the expression — it is recorded, bound to a placeholder in
    // the sandbox, and reified back to itself if it survives into the result.
    // If the builder actually USES it, evaluation throws PlaceholderMisuse and
    // the whole expression falls back to runtime.
    //
    // `UpperIdentifier` is a placeholder too, but only where it cannot be a
    // qualifier — never in receiver position, which `isCollapsible` checks
    // separately. Telling `Foo` the binding from `Foo` the module apart needs
    // name resolution this pass runs before, so the sandbox settles it instead:
    // bind it as a placeholder and let the attempt fail if the builder needed
    // the real thing. A nullary variant like `None` loses a little precision
    // that way — it IS knowable — but a chain taking one did not collapse at
    // all before, so this is never worse.
    auto isStatic(const ast::Expr& expr) const -> bool {
        return std::visit(
            [&](const auto& node) -> bool {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, ast::Identifier> ||
                              std::is_same_v<T, ast::UpperIdentifier>) {
                    for (const auto& seen : freeNames)
                        if (seen == node.name) return true;
                    freeNames.push_back(node.name);
                    freeExprs.push_back(&expr);
                    return true;
                } else if constexpr (std::is_same_v<T, ast::IntLiteral> ||
                              std::is_same_v<T, ast::FloatLiteral> ||
                              std::is_same_v<T, ast::CharLiteral> ||
                              std::is_same_v<T, ast::BoolLiteral> ||
                              std::is_same_v<T, ast::NoneLiteral> ||
                              std::is_same_v<T, ast::AtomLiteral>) {
                    return true;
                } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                    // `interpolating` is set for every double-quoted string,
                    // whether or not it contains a `${...}` — so it is not the
                    // question. What matters is whether anything is embedded,
                    // and whether that is itself known.
                    if (!node.interpolating) return true;
                    // Already pre-split into parts/values.
                    if (!node.parts.empty()) return allStatic(node.values);
                    // Still raw text. No `${` means nothing to look up; with
                    // one, the reference is invisible from here and the string
                    // has to count as unknown.
                    return node.value.find("${") == std::string::npos;
                } else if constexpr (std::is_same_v<T, ast::ListExpr>) {
                    return !node.rest && allStatic(node.elements);
                } else if constexpr (std::is_same_v<T, ast::TupleExpr>) {
                    return allStatic(node.elements);
                } else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
                    return allStatic(node.fields);
                } else if constexpr (std::is_same_v<T, ast::MapExpr>) {
                    for (const auto& entry : node.entries)
                        if (!entry.key || !isStatic(*entry.key) ||
                            !entry.value || !isStatic(*entry.value))
                            return false;
                    return true;
                } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                    return node.left && node.right && isStatic(*node.left) &&
                           isStatic(*node.right);
                } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                    return node.operand && isStatic(*node.operand);
                } else if constexpr (std::is_same_v<T, ast::MethodCall> ||
                                     std::is_same_v<T, ast::FunctionCall>) {
                    // Only a call into compiled code counts: an ordinary
                    // runtime call on literals stays where the author put it.
                    return isCollapsible(expr);
                } else {
                    return false;
                }
            },
            expr.kind);
    }

    auto isCollapsible(const ast::Expr& expr) const -> bool {
        if (const auto* call = std::get_if<ast::MethodCall>(&expr.kind)) {
            if (call->block || !call->receiver) return false;
            std::string owner;
            const bool qualified = qualifierPath(*call->receiver, owner);
            const bool isCompiled = qualified
                ? compiled.qualified.count(
                      qualifiedName(owner, call->method)) > 0
                : compiled.methods.count(call->method) > 0;
            if (!isCompiled) return false;
            const bool receiverKnown = qualified || isStatic(*call->receiver);
            return receiverKnown && allStatic(call->args) &&
                   allStatic(call->namedArgs);
        }
        if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind)) {
            const auto dot = call->name.rfind('.');
            const bool isCompiled = dot == std::string::npos
                ? compiled.functions.count(call->name) > 0
                : compiled.qualified.count(
                      qualifiedName(call->name.substr(0, dot),
                                    call->name.substr(dot + 1))) > 0;
            if (!isCompiled || call->block) return false;
            return allStatic(call->args) && allStatic(call->namedArgs);
        }
        return false;
    }

    // Does this expression call into compiled code ANYWHERE inside it?
    //
    // The gate that stops collapse from claiming expressions it has no
    // business touching: `[1, 2, 3]` is static, but "evaluating" it would just
    // rebuild the literal it already is. Only walks the shapes `isStatic`
    // accepts, since it is never asked about anything else.
    auto callsCompiled(const ast::Expr& expr) const -> bool {
        if (isCollapsible(expr)) return true;
        return std::visit(
            [&](const auto& node) -> bool {
                using T = std::decay_t<decltype(node)>;
                auto anyOf = [&](const auto& list) {
                    for (const auto& item : list)
                        if (item && callsCompiled(*item)) return true;
                    return false;
                };
                if constexpr (std::is_same_v<T, ast::ListExpr> ||
                              std::is_same_v<T, ast::TupleExpr>) {
                    return anyOf(node.elements);
                } else if constexpr (std::is_same_v<T, ast::RecordConstruction>) {
                    for (const auto& field : node.fields)
                        if (field.value && callsCompiled(*field.value)) return true;
                    return false;
                } else if constexpr (std::is_same_v<T, ast::MapExpr>) {
                    for (const auto& entry : node.entries)
                        if ((entry.key && callsCompiled(*entry.key)) ||
                            (entry.value && callsCompiled(*entry.value)))
                            return true;
                    return false;
                } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                    return anyOf(node.values);
                } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                    return (node.left && callsCompiled(*node.left)) ||
                           (node.right && callsCompiled(*node.right));
                } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                    return node.operand && callsCompiled(*node.operand);
                } else {
                    return false;
                }
            },
            expr.kind);
    }

    // What gets collapsed: any expression whose value is fully determined AND
    // which reaches compiled code at least once.
    //
    // Deliberately wider than "the terminal of a call chain", which was the
    // first rule: it left `"0 " + Css.px(24)` building at runtime, both
    // operands literal and the result decidable, purely because the outermost
    // node was a `+` rather than a call. `isStatic` already computes the
    // property that matters; the chain shape never did.
    auto claim(ast::ExprPtr& slot) -> bool {
        if (!slot) return false;
        if (rejected && rejected->count(slot.get())) return false;
        freeNames.clear();
        freeExprs.clear();
        if (!isStatic(*slot) || !callsCompiled(*slot)) return false;
        candidates.push_back({&slot, freeNames, freeExprs});
        return true;
    }
};

// Finds every collapsible expression in `program`, evaluates them in one
// sandbox run, and replaces those that produced a reifiable value.
//
// Iterates, because an expression that fails is not the end of the story: a
// chain whose OUTERMOST call needs a runtime value can still have a perfectly
// collapsible interior. Each round records what failed, and the next round's
// walk descends past it. Without this, one unresolvable `.emit()` at the top of
// `compiled_css.kex` cost all seven `.rule` calls underneath it.
//
// Terminates because a round only repeats when the rejected set GREW, and a
// program has finitely many expressions.
auto collapseChains(ast::Program& program, const ExpandOptions& options) -> void {
    const auto compiled = compiledNames(program);
    if (compiled.empty()) return;

    std::unordered_set<const ast::Expr*> rejected;
    for (;;) {
        ChainCollapser collapser{compiled};
        collapser.rejected = &rejected;
        Constants none;
        // The walk is used purely for its traversal here: onSlot claims every
        // candidate before the constant machinery sees it, so neither the
        // empty constant map nor this sink ever receives anything.
        std::vector<semantic::Diagnostic> unused;
        Substituter walk{none, unused, {}, {}, {}};
        walk.onSlot = [&](ast::ExprPtr& slot) { return collapser.claim(slot); };
        walk.alsoSplit = [&](const std::string& text) {
            for (const auto& name : compiled.functions)
                if (text.find(name) != std::string::npos) return true;
            for (const auto& name : compiled.methods)
                if (text.find(name) != std::string::npos) return true;
            for (const auto& identity : compiled.qualified) {
                const auto split = identity.rfind("::");
                const auto name = split == std::string::npos
                    ? identity : identity.substr(split + 2);
                if (text.find(name) != std::string::npos) return true;
            }
            return false;
        };
        walkBodies(
            program.items,
            [&](auto& target) {
                using T = std::decay_t<decltype(target)>;
                if constexpr (std::is_same_v<T, ast::FunctionDef>)
                    walk.functionBody(target);
                else
                    walk.substitute(target);
            },
            /*intoCompiled=*/false);
        if (collapser.candidates.empty()) return;

        std::vector<interpreter::Evaluator::ExpressionRequest> requests;
        requests.reserve(collapser.candidates.size());
        for (const auto& candidate : collapser.candidates)
            requests.push_back({candidate.slot->get(), candidate.names});

        std::vector<interpreter::ValuePtr> values;
        std::vector<std::string> reasons;
        RecordLayouts layouts;
        try {
            interpreter::Evaluator evaluator;
            evaluator.loadPrelude();
            values = evaluator.evaluateExpressions(
                program, requests, options.timeout,
                options.report ? &reasons : nullptr);
            layouts = evaluator.recordFieldOrder();
        } catch (const std::exception&) {
            // Collapse is an optimization: if the sandbox itself cannot run,
            // the program still compiles and builds its values at runtime.
            return;
        }

        bool grew = false;
        for (std::size_t i = 0;
             i < collapser.candidates.size() && i < values.size(); i++) {
            const auto& candidate = collapser.candidates[i];
            const auto* claimed = candidate.slot->get();
            std::string why;
            // Reify into a separate expression first: a placeholder the
            // reifier cannot resolve fails partway, and overwriting the slot
            // before knowing that would leave a half-built literal.
            const SourceLocation where = (*candidate.slot)->location;
            auto literal =
                values[i] ? valueToLiteral(values[i], where, why,
                                           {&layouts, &candidate.exprs})
                          : nullptr;
            if (literal) {
                *candidate.slot = std::move(literal);
                if (options.report) options.report->push_back({where, true, {}});
                continue;
            }
            if (options.report) {
                // `why` is set when reification failed; otherwise the reason
                // came from evaluation.
                if (why.empty() && i < reasons.size()) why = reasons[i];
                if (why.empty()) why = "it could not be evaluated";
                options.report->push_back({where, false, std::move(why)});
            }
            grew = rejected.insert(claimed).second || grew;
        }
        if (!grew) return;
    }
}
} // namespace

auto expand(ast::Program& program,
            std::vector<semantic::Diagnostic>& diagnostics,
            const ExpandOptions& options) -> bool {
    // --- 0. Fold Kex.embed(...) and Template.text/.html(...) everywhere.
    // Unconditional: a program using either has no `compiled do` block at
    // all, so both must run before the hasBlock check below would otherwise
    // skip the rest of this function. Embeds first, since
    // `Template.text(Kex.embed(...))` needs the embed already folded to a
    // literal before the template call's own argument-shape check runs.
    if (!foldEmbeds(program, options.sourcePath, diagnostics)) return false;
    if (!foldTemplates(program, diagnostics, options)) return false;

    // --- 1. Find the constants, in source order.
    std::vector<std::string> names;
    std::vector<SourceLocation> locations;
    bool hasBlock = false;
    auto scan = [&](auto&& self, auto& items, const std::string& scope) -> void {
        for (auto& item : items) {
            if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*mod) self(self, (*mod)->body, (*mod)->name);
                continue;
            }
            auto* block = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&item);
            if (!block || !*block) continue;
            hasBlock = true;
            for (auto& entry : (*block)->items)
                if (auto* fn =
                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&entry))
                    if (*fn && isValueBinding(**fn)) {
                        names.push_back(qualifiedName(scope, (*fn)->name));
                        locations.push_back((*fn)->location);
                    }
        }
    };
    scan(scan, program.items, "");
    if (!hasBlock) return true;

    // Which scope does each generation template belong to? A `let %name(...)`
    // inside `module M do compiled do ... end end` must produce a method OF M,
    // not a top-level function. The template pointer identifies its block, so
    // map it to the owning module before anything runs. Null means top level.
    std::unordered_map<const void*, ast::ModuleDef*> templateOwner;
    {
        auto record = [&](auto&& self, auto& items,
                          ast::ModuleDef* owner) -> void {
            for (auto& item : items) {
                if (auto* mod =
                        std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                    if (*mod) self(self, (*mod)->body, mod->get());
                    continue;
                }
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
                // Generation effects may live in an ordinary compiled helper
                // function (`let emit(name) do record %name ... end`). Find
                // those templates too, or records emitted by a helper inside
                // a module would incorrectly be appended at top level.
                Constants none;
                Substituter finder{none, diagnostics, {}, {}, {}};
                finder.onGenerated = [&](const ast::GeneratedDecl& decl) {
                    templateOwner[templateId(decl.function)] = owner;
                };
                walkBodies((*block)->items, [&](auto& target) {
                    using T = std::decay_t<decltype(target)>;
                    if constexpr (std::is_same_v<T, ast::FunctionDef>)
                        finder.functionBody(target);
                    else
                        finder.substitute(target);
                });
            }
        };
        record(record, program.items, nullptr);
    }


    // --- 2. Run the block once, against the whole program.
    //
    // This both forces the constants AND executes any generation code: a
    // driver loop is ordinary compile-time code, and `let %name(...)` records
    // a declaration as it runs (Evaluator's GeneratedDecl case). That is why
    // `each`, `eachIndexed`, nesting and conditionals need no support here —
    // they are just code the sandbox runs.
    ScopedConstants constants;
    std::vector<interpreter::Evaluator::GeneratedDeclaration> generated;
    // Declared field order for every record the sandbox knows, the prelude's
    // included — a reified record has to be written in its declared order,
    // which on BEAM is its tuple layout.
    RecordLayouts layouts;
    {
        const SourceLocation blame =
            locations.empty() ? SourceLocation{} : locations.front();
        std::vector<interpreter::ValuePtr> values;
        try {
            interpreter::Evaluator evaluator;
            evaluator.loadPrelude();
            values = evaluator.evaluateConstants(program, names, options.timeout);
            generated = evaluator.generatedDeclarations();
            layouts = evaluator.recordFieldOrder();
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
        for (std::size_t i = 0; i < names.size() && i < values.size(); i++) {
            if (!values[i]) continue;
            const auto split = names[i].rfind("::");
            const auto scope = split == std::string::npos
                ? std::string{} : names[i].substr(0, split);
            const auto bare = split == std::string::npos
                ? names[i] : names[i].substr(split + 2);
            constants[scope].emplace(bare, values[i]);
        }
    }

    // --- 3. Substitute every use with the computed literal.
    std::unordered_set<std::string> keep;
    bool ok = true;
    std::unordered_set<std::string> reportedFailures;
    auto substituteScope =
        [&](auto&& self, auto& items, const std::string& scope) -> void {
            Constants visible = constants[""];
            if (!scope.empty())
                for (const auto& [name, value] : constants[scope])
                    visible[name] = value;
            Substituter substituter{
                visible, diagnostics, {}, {}, {}, {}, {}, {&layouts}};
            substituter.scoped = &constants;
            walkScopeBodies(items, [&](auto& target) {
                using T = std::decay_t<decltype(target)>;
                if constexpr (std::is_same_v<T, ast::FunctionDef>)
                    substituter.functionBody(target);
                else
                    substituter.substitute(target);
            });
            for (const auto& reference : substituter.remaining) {
                const auto split = reference.rfind("::");
                const auto name = split == std::string::npos
                    ? reference : reference.substr(split + 2);
                const auto owner = split != std::string::npos
                    ? reference.substr(0, split)
                    : (constants[scope].count(name) ? scope : std::string{});
                const auto identity = qualifiedName(owner, name);
                keep.insert(identity);
                if (!reportedFailures.insert(identity).second) continue;
                std::string why;
                (void)valueToLiteral(constants[owner].at(name),
                                     SourceLocation{}, why, {&layouts});
                addError(diagnostics, SourceLocation{},
                         "compile-time constant `" + identity +
                             "` cannot be inlined: " + why);
                ok = false;
            }
            for (auto& item : items)
                if (auto* mod =
                        std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
                    if (*mod) self(self, (*mod)->body, (*mod)->name);
        };
    substituteScope(substituteScope, program.items, "");

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
        // A generated name has to be a legal identifier of the right KIND:
        // types are upper-case, functions lower-case. Without this a
        // `type %{name}` over lower-case data silently declared a type called
        // `small`, which nothing can ever reference.
        {
            // A `make`'s name is its TARGET TYPE, so it wants upper-case for
            // the same reason a `type` does.
            const bool wantsUpper =
                std::holds_alternative<std::shared_ptr<ast::TypeDef>>(decl.function) ||
                std::holds_alternative<std::shared_ptr<ast::RecordDef>>(decl.function) ||
                std::holds_alternative<std::shared_ptr<ast::MakeDef>>(decl.function);
            const unsigned char lead =
                static_cast<unsigned char>(decl.name.front());
            const bool isUpper = lead >= 'A' && lead <= 'Z';
            const bool isLower = (lead >= 'a' && lead <= 'z') || lead == '_';
            if (wantsUpper && !isUpper) {
                addError(diagnostics, decl.location,
                         std::string("generated ") +
                             (std::holds_alternative<std::shared_ptr<ast::MakeDef>>(
                                  decl.function)
                                  ? "make target"
                                  : std::holds_alternative<std::shared_ptr<ast::RecordDef>>(
                                        decl.function)
                                      ? "record"
                                      : "type") +
                             " name `" + decl.name +
                             "` must start with an upper-case letter" +
                             (isLower ? " — try `%{name.capitalize}`" : ""));
                ok = false;
                continue;
            }
            if (!wantsUpper && !isLower) {
                addError(diagnostics, decl.location,
                         "generated function name `" + decl.name +
                             "` must start with a lower-case letter");
                ok = false;
                continue;
            }
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
            Substituter inner{
                captured, diagnostics, {}, {}, {}, {}, {}, {&layouts}};
            inner.functionBody(*copy);
            if (into) into->body.push_back(std::move(copy));
            else program.items.push_back(std::move(copy));
            continue;
        }

        if (auto* makeTemplate =
                std::get_if<std::shared_ptr<ast::MakeDef>>(&decl.function)) {
            if (!*makeTemplate) continue;
            auto copy = ast::clone(**makeTemplate);
            copy->location = decl.location;
            // The template's target is null — the name was not known when it
            // was parsed. Build it now from the resolved name.
            copy->target = std::make_unique<ast::TypeExpr>();
            copy->target->location = decl.location;
            copy->target->kind = ast::TypeName{{decl.name}};
            // Same hygiene as a generated function, applied to every method:
            // the loop variables the bodies closed over do not exist at
            // runtime, so they are baked in as literals. Each method's own
            // parameters shadow them.
            Constants captured;
            for (const auto& [bound, value] : decl.bindings)
                captured.emplace(bound, value);
            // Replace the driver loops with the methods they declared, then
            // apply hygiene to every method the block now has.
            spliceNestedMethods(*copy, decl, diagnostics, ok);
            Substituter scope{
                captured, diagnostics, {}, {}, {}, {}, {}, {&layouts}};
            substituteMakeBodies(*copy, scope, captured);
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
            continue;
        }

        if (auto* recordTemplate =
                std::get_if<std::shared_ptr<ast::RecordDef>>(&decl.function)) {
            if (!*recordTemplate) continue;
            auto copy = ast::clone(**recordTemplate);
            copy->name = decl.name;
            copy->location = decl.location;
            Constants captured;
            for (const auto& [bound, value] : decl.bindings)
                captured.emplace(bound, value);
            Substituter scope{
                captured, diagnostics, {}, {}, {}, {}, {}, {&layouts}};
            for (auto& field : copy->fields)
                if (field.defaultValue) scope.substitute(*field.defaultValue);
            if (into) into->body.push_back(std::move(copy));
            else program.items.push_back(std::move(copy));
        }
    }

    // GeneratedDecl is a compile-time EFFECT. Helpers that contain one are
    // still ordinary functions and may be hoisted out of `compiled`, so every
    // executed effect must become Unit before semantic analysis and lowering.
    // Leaving the node behind makes the BEAM path fail with "unimplemented
    // expr node GeneratedDecl" even though its record was emitted correctly.
    {
        Constants none;
        Substituter eraser{none, diagnostics, {}, {}, {}};
        eraser.onSlot = [](ast::ExprPtr& slot) {
            if (!slot ||
                !std::holds_alternative<ast::GeneratedDecl>(slot->kind))
                return false;
            slot->kind = ast::TupleExpr{{}};
            return true;
        };
        walkBodies(program.items, [&](auto& target) {
            using T = std::decay_t<decltype(target)>;
            if constexpr (std::is_same_v<T, ast::FunctionDef>)
                eraser.functionBody(target);
            else
                eraser.substitute(target);
        });
    }

    // --- 4. Collapse compiled-builder chains that are already fully known.
    //
    // After substitution, so the constants are already literals and "mentions a
    // lower-case name" is an exact test for "depends on something that only
    // exists at runtime" — but BEFORE the hoist below, which removes the
    // `compiled` blocks that tell collapse which methods are its own. Getting
    // that order wrong disables the feature silently: `compiledNames` returns
    // an empty set and every chain is left alone.
    if (ok) collapseChains(program, options);

    // --- 5. Drop the definitions. A constant is gone from the emitted module
    // entirely — no nullary function, nothing exported. Only constants whose
    // uses were all substituted are removed; anything left referenced keeps
    // its definition so the program still resolves.
    // Every surviving declaration is HOISTED into the enclosing scope and the
    // block itself removed, so no `CompiledBlock` reaches anything downstream.
    //
    // Two things fall out of that, both of which used to be bugs:
    //  - The type checker walks past a `CompiledBlock`, so declarations inside
    //    one were never checked. Hoisted, they are ordinary declarations and
    //    are checked like any other.
    //  - Top-level lowering has no `CompiledBlock` case, so a block holding a
    //    `make` outside any module died with "unimplemented top-level item".
    //    There is no longer a block to lower.
    auto prune = [&](auto&& self, auto& items, const std::string& scope) -> void {
        using Item = std::decay_t<decltype(items[0])>;
        std::vector<Item> hoisted;
        for (std::size_t i = 0; i < items.size();) {
            if (auto* mod =
                    std::get_if<std::unique_ptr<ast::ModuleDef>>(&items[i])) {
                if (*mod) self(self, (*mod)->body, (*mod)->name);
                i++;
                continue;
            }
            auto* block = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&items[i]);
            if (!block || !*block) { i++; continue; }
            for (auto& entry : (*block)->items) {
                auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&entry);
                const bool inlined = fn && *fn && isValueBinding(**fn) &&
                                     constants[scope].count((*fn)->name) &&
                                     !keep.count(
                                         qualifiedName(scope, (*fn)->name));
                // A bare expression in a compiled block is compile-time work
                // and nothing else — a driver loop, typically. It has already
                // run, and its generated declarations are now real items, so
                // keeping it would re-run the loop at runtime.
                if (inlined || std::holds_alternative<ast::ExprPtr>(entry))
                    continue;
                std::visit(
                    [&](auto& held) {
                        using Held = std::decay_t<decltype(held)>;
                        if constexpr (!std::is_same_v<Held, ast::ExprPtr>)
                            hoisted.push_back(std::move(held));
                    },
                    entry);
            }
            items.erase(items.begin() + static_cast<long>(i));
        }
        for (auto& item : hoisted) items.push_back(std::move(item));
    };
    prune(prune, program.items, "");

    return ok;
}

} // namespace kex::compiled
