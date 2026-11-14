#include "repl_input.hxx"

namespace kex::hir {

namespace {

auto mainOf(ast::Program& program) -> ast::MainBlock* {
    ast::MainBlock* found = nullptr;
    for (auto& item : program.items)
        if (auto* main = std::get_if<std::unique_ptr<ast::MainBlock>>(&item);
            main && *main)
            found = main->get();
    return found;
}

} // namespace

auto checkReplInput(std::unique_ptr<ast::Program> session, ast::Program& input,
                    std::vector<ast::ExprPtr> after,
                    const std::function<void(ast::Program&)>& prepare,
                    const semantic::ImportedInterfaces* interfaces)
    -> CheckedInput {
    CheckedInput checked;
    checked.session = std::move(session);
    auto* sessionMain = mainOf(*checked.session);
    auto* inputMain = mainOf(input);
    // Lend the input's definitions and statements; the guards give them back
    // however the check ends, before `checked` is returned.
    {
        // Found again by the node they hold: `prepare` may merge items in
        // ahead of them.
        struct GiveBackItems {
            ast::Program& session;
            ast::Program& input;
            std::vector<std::pair<std::size_t, const void*>> lent;
            ~GiveBackItems() {
                for (const auto& [slot, node] : lent)
                    for (auto& item : session.items)
                        if (ast::heldNode(item) == node) {
                            input.items[slot] = std::move(item);
                            break;
                        }
                std::erase_if(session.items, [](const ast::TopLevelItem& item) {
                    return ast::heldNode(item) == nullptr;
                });
            }
        } giveBackItems{*checked.session, input, {}};
        for (std::size_t i = 0; i < input.items.size(); ++i)
            if (!std::holds_alternative<std::unique_ptr<ast::MainBlock>>(
                    input.items[i])) {
                giveBackItems.lent.emplace_back(i, ast::heldNode(input.items[i]));
                checked.session->items.push_back(std::move(input.items[i]));
            }
        struct GiveBack {
            ast::MainBlock* session;
            ast::MainBlock* input;
            std::size_t from;
            std::vector<ast::ExprPtr>& kept;
            ~GiveBack() {
                if (!session || !input) return;
                for (std::size_t i = 0; i < input->body.size(); ++i)
                    input->body[i] = std::move(session->body[from + i]);
                for (auto i = from + input->body.size(); i < session->body.size(); ++i)
                    kept.push_back(std::move(session->body[i]));
                session->body.resize(from);
            }
        } giveBack{sessionMain, inputMain, sessionMain ? sessionMain->body.size() : 0,
                   checked.after};
        if (sessionMain && inputMain) {
            for (auto& statement : inputMain->body)
                sessionMain->body.push_back(std::move(statement));
            for (auto& statement : after) sessionMain->body.push_back(std::move(statement));
        }
        if (prepare) prepare(*checked.session);
        checked.analysis = std::make_unique<semantic::Analyzer>(interfaces);
        if (checked.analysis->analyze(*checked.session))
            checked.program =
                std::make_unique<Program>(*checked.session, *checked.analysis);
        else
            for (const auto& diagnostic : checked.analysis->diagnostics())
                if (diagnostic.level == semantic::Diagnostic::Level::Error) {
                    checked.error = diagnostic.message;
                    break;
                }
    }
    return checked;
}

} // namespace kex::hir
