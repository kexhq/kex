#include "expand.hxx"

#include "reify.hxx"
#include "../ast/clone.hxx"
#include "../interpreter/evaluator.hxx"
#include "../lexer/lexer.hxx"
#include "../parser/parser.hxx"

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
    // Declared record field order, so a reified record is written in the order
    // its declaration gives — which on BEAM is its tuple layout. Empty until
    // the sandbox has run and can report it.
    ReifyContext reify;
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

    auto substitute(ast::ExprPtr& slot) -> void {
        if (!slot) return;
        if (onSlot && onSlot(slot)) return;
        if (const auto* name = nameOf(*slot)) {
            auto found = constants.find(*name);
            if (found != constants.end()) {
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
            addError(diagnostics, nested.location,
                     "only `let %name(...)` can be generated inside a "
                     "generated `make` — a nested `type` or `make` cannot");
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
        Substituter inner{shadowed, base.diagnostics, {}, {}, {}, {}, base.reify};
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
using CompiledNames = std::unordered_set<std::string>;

auto collectMakeMethods(const ast::MakeDef& make, CompiledNames& into) -> void;

template <typename Items>
auto collectDeclaredNames(const Items& items, CompiledNames& into) -> void {
    for (const auto& entry : items) {
        std::visit(
            [&](const auto& node) {
                using Ptr = std::decay_t<decltype(*node)>;
                if constexpr (std::is_same_v<Ptr, ast::FunctionDef>) {
                    if (node && !isValueBinding(*node)) into.insert(node->name);
                } else if constexpr (std::is_same_v<Ptr, ast::MakeDef>) {
                    if (node) collectMakeMethods(*node, into);
                } else if constexpr (std::is_same_v<Ptr, ast::VisibilityBlock>) {
                    if (node) collectDeclaredNames(node->items, into);
                }
            },
            entry);
    }
}

// Overload for the CompiledItem variant, which also holds bare ExprPtr.
auto collectDeclaredNames(const std::vector<ast::CompiledItem>& items,
                          CompiledNames& into) -> void {
    for (const auto& entry : items) {
        if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&entry)) {
            if (*fn && !isValueBinding(**fn)) into.insert((*fn)->name);
        } else if (const auto* make =
                       std::get_if<std::unique_ptr<ast::MakeDef>>(&entry)) {
            if (*make) collectMakeMethods(**make, into);
        }
    }
}

auto collectMakeMethods(const ast::MakeDef& make, CompiledNames& into) -> void {
    collectDeclaredNames(make.body, into);
}

// Every `compiled do` block in the program, at top level or inside a module.
auto compiledNames(const ast::Program& program) -> CompiledNames {
    CompiledNames names;
    auto scan = [&](const auto& items) {
        for (const auto& item : items)
            if (const auto* block =
                    std::get_if<std::unique_ptr<ast::CompiledBlock>>(&item))
                if (*block) collectDeclaredNames((*block)->items, names);
    };
    scan(program.items);
    for (const auto& item : program.items)
        if (const auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
            if (*mod) scan((*mod)->body);
    return names;
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
    static auto isCallQualifier(const ast::Expr& expr) -> bool {
        return std::holds_alternative<ast::UpperIdentifier>(expr.kind);
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
    // Note what is still NOT here: `UpperIdentifier`. It is ambiguous with a
    // module or type qualifier, and telling those apart needs name resolution
    // this pass runs before.
    auto isStatic(const ast::Expr& expr) const -> bool {
        return std::visit(
            [&](const auto& node) -> bool {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, ast::Identifier>) {
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
            if (!compiled.count(call->method) || call->block || !call->receiver)
                return false;
            const bool receiverKnown = isCallQualifier(*call->receiver) ||
                                       isStatic(*call->receiver);
            return receiverKnown && allStatic(call->args) &&
                   allStatic(call->namedArgs);
        }
        if (const auto* call = std::get_if<ast::FunctionCall>(&expr.kind)) {
            const auto dot = call->name.rfind('.');
            const std::string bare = dot == std::string::npos
                                         ? call->name
                                         : call->name.substr(dot + 1);
            if (!compiled.count(bare) || call->block) return false;
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
                    for (const auto& [_, value] : node.fields)
                        if (value && callsCompiled(*value)) return true;
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
            for (const auto& name : compiled)
                if (text.find(name) != std::string::npos) return true;
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
        for (std::size_t i = 0; i < names.size() && i < values.size(); i++)
            if (values[i]) constants.emplace(names[i], values[i]);
    }

    // --- 3. Substitute every use with the computed literal.
    Substituter substituter{constants, diagnostics, {}, {}, {}, {}, {&layouts}};
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
        (void)valueToLiteral(constants.at(name), SourceLocation{}, why, {&layouts});
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
        // A generated name has to be a legal identifier of the right KIND:
        // types are upper-case, functions lower-case. Without this a
        // `type %{name}` over lower-case data silently declared a type called
        // `small`, which nothing can ever reference.
        {
            // A `make`'s name is its TARGET TYPE, so it wants upper-case for
            // the same reason a `type` does.
            const bool wantsUpper =
                std::holds_alternative<std::shared_ptr<ast::TypeDef>>(decl.function) ||
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
            Substituter inner{captured, diagnostics, {}, {}, {}, {}, {&layouts}};
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
            Substituter scope{captured, diagnostics, {}, {}, {}, {}, {&layouts}};
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
        }
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
    std::unordered_map<std::string, bool> keep;
    for (const auto& name : substituter.remaining) keep[name] = true;

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
    auto prune = [&](auto& items) {
        using Item = std::decay_t<decltype(items[0])>;
        std::vector<Item> hoisted;
        for (std::size_t i = 0; i < items.size();) {
            auto* block = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&items[i]);
            if (!block || !*block) { i++; continue; }
            for (auto& entry : (*block)->items) {
                auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&entry);
                const bool inlined = fn && *fn && isValueBinding(**fn) &&
                                     constants.count((*fn)->name) &&
                                     !keep.count((*fn)->name);
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
    prune(program.items);
    for (auto& item : program.items)
        if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
            if (*mod) prune((*mod)->body);

    return ok;
}

} // namespace kex::compiled
