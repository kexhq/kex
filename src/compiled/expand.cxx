#include "expand.hxx"

#include "reify.hxx"
#include "../interpreter/evaluator.hxx"

#include <utility>

namespace kex::compiled {

namespace {

// The `NAME = expr` form inside a `compiled do` block. The parser emits it as
// a LetExpr over a VarPattern (see Parser::parseCompiledBlock), so that is what
// identifies a compile-time constant.
auto constantName(const ast::ExprPtr& expr) -> const std::string* {
    if (!expr) return nullptr;
    const auto* let = std::get_if<ast::LetExpr>(&expr->kind);
    if (!let || !let->pattern) return nullptr;
    const auto* var = std::get_if<ast::VarPattern>(&let->pattern->kind);
    return var ? &var->name : nullptr;
}

auto addError(std::vector<semantic::Diagnostic>& diagnostics,
              const SourceLocation& location, std::string message) -> void {
    diagnostics.push_back({semantic::Diagnostic::Level::Error, location,
                           std::move(message)});
}

// Names of every compile-time constant in `items`' compiled blocks, in source
// order, with the location to blame if one cannot be reified.
template <typename Items>
auto collectConstants(const Items& items, std::vector<std::string>& names,
                      std::vector<SourceLocation>& locations) -> void {
    for (const auto& item : items) {
        const auto* block =
            std::get_if<std::unique_ptr<ast::CompiledBlock>>(&item);
        if (!block || !*block) continue;
        for (const auto& entry : (*block)->items)
            if (const auto* expr = std::get_if<ast::ExprPtr>(&entry))
                if (const auto* name = constantName(*expr)) {
                    names.push_back(*name);
                    locations.push_back((*expr)->location);
                }
    }
}

// Hoists each compiled block's CONSTANTS out as ordinary declarations holding
// their already-computed value, and drops the block if nothing else remains.
//
// Only constants are hoisted. The block's function/make/type/record
// declarations stay put, because both backends already unwrap them in place
// and hoisting them would change what the type checker sees: today the
// checker walks past a CompiledBlock entirely, so those bodies have never been
// checked. Lifting them out type-checks them for the first time, which makes
// examples/compiled_sql.kex and compiled_css.kex fail on the pre-existing
// tuple-auto-spread gap (`@conditions.each do |col, val|` over a
// `[(String, Any)]`). That is a real latent bug worth fixing, but it belongs
// with the step that makes those examples' declarations expand — not smuggled
// in behind constants.
template <typename Items>
auto rewriteBlocks(Items& items,
                   const std::vector<interpreter::ValuePtr>& values,
                   std::size_t& next,
                   std::vector<semantic::Diagnostic>& diagnostics) -> bool {
    using Item = typename Items::value_type;
    bool ok = true;
    for (std::size_t i = 0; i < items.size();) {
        auto* slot = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&items[i]);
        if (!slot || !*slot) { i++; continue; }
        auto& block = **slot;

        std::vector<Item> hoisted;
        std::vector<ast::CompiledItem> kept;
        for (auto& entry : block.items) {
            auto* expr = std::get_if<ast::ExprPtr>(&entry);
            const std::string* name = expr ? constantName(*expr) : nullptr;
            if (!name) {
                // Declarations stay in the block. A bare expression does too:
                // it has no declaration form, and both backends already
                // evaluate it where it sits.
                kept.push_back(std::move(entry));
                continue;
            }
            const auto& value =
                next < values.size() ? values[next] : interpreter::ValuePtr{};
            next++;
            std::string why;
            auto literal = valueToLiteral(value, (*expr)->location, why);
            if (!literal) {
                addError(diagnostics, (*expr)->location,
                         "compile-time constant `" + *name +
                             "` has no runtime form: " + why);
                ok = false;
                continue;
            }
            // A named constant at declaration scope is a PARAMETERLESS
            // FunctionDef — what `let TOP = 3` parses to at top level
            // (grammar.ebnf's "parameterless binding" clause). Neither
            // TopLevelItem nor ModuleItem can hold a bare expression.
            auto constant = std::make_unique<ast::FunctionDef>();
            constant->location = (*expr)->location;
            constant->name = *name;
            ast::FunctionClause clause;
            clause.body.push_back(std::move(literal));
            constant->clauses.push_back(std::move(clause));
            hoisted.push_back(Item{std::move(constant)});
        }

        block.items = std::move(kept);
        const bool dropBlock = block.items.empty();
        if (dropBlock) items.erase(items.begin() + static_cast<long>(i));
        if (!hoisted.empty())
            items.insert(items.begin() + static_cast<long>(i),
                         std::make_move_iterator(hoisted.begin()),
                         std::make_move_iterator(hoisted.end()));
        i += hoisted.size() + (dropBlock ? 0 : 1);
    }
    return ok;
}

} // namespace

auto expand(ast::Program& program,
            std::vector<semantic::Diagnostic>& diagnostics,
            const ExpandOptions& options) -> bool {
    std::vector<std::string> names;
    std::vector<SourceLocation> locations;
    collectConstants(program.items, names, locations);
    for (auto& item : program.items)
        if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
            if (*mod) collectConstants((*mod)->body, names, locations);

    // Nothing to do unless the program actually has a compiled block. Checked
    // separately from `names` because a block may hold only declarations.
    bool hasBlock = false;
    auto noteBlocks = [&](const auto& items) {
        for (const auto& item : items)
            if (std::holds_alternative<std::unique_ptr<ast::CompiledBlock>>(item))
                hasBlock = true;
    };
    noteBlocks(program.items);
    for (auto& item : program.items)
        if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
            if (*mod) noteBlocks((*mod)->body);
    if (!hasBlock) return true;

    // One evaluation for the whole program: executing its declarations is what
    // binds the constants, and doing it once lets a later constant see an
    // earlier one (`B = A + 1`).
    std::vector<interpreter::ValuePtr> values;
    if (!names.empty()) {
        const SourceLocation blame =
            locations.empty() ? SourceLocation{} : locations.front();
        try {
            interpreter::Evaluator evaluator;
            evaluator.loadPrelude();
            values = evaluator.evaluateGlobals(program, names, options.timeout);
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
    }

    std::size_t next = 0;
    bool ok = rewriteBlocks(program.items, values, next, diagnostics);
    for (auto& item : program.items)
        if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item))
            if (*mod)
                ok = rewriteBlocks((*mod)->body, values, next, diagnostics) && ok;
    return ok;
}

} // namespace kex::compiled
