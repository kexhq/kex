#include "compile_time.hxx"

#include "persist.hxx"
#include <gmpxx.h>
#include "../ast/convert.hxx"
#include "../beam/etf.hxx"
#include "../beam/term_builder.hxx"
#include "../cache/cache.hxx"
#include "../common/digest.hxx"
#include "../common/file_io.hxx"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <vector>

namespace kex::hir {

namespace {

template <typename Node>
constexpr bool isDeclaration =
    std::is_same_v<Node, std::unique_ptr<ast::FunctionDef>> ||
    std::is_same_v<Node, std::unique_ptr<ast::MakeDef>> ||
    std::is_same_v<Node, std::unique_ptr<ast::RecordDef>> ||
    std::is_same_v<Node, std::unique_ptr<ast::TypeDef>>;

// What one block lent the items around it, to be given back by node.
template <typename Item>
struct Loan {
    std::vector<Item>* container = nullptr;
    ast::CompiledBlock* block = nullptr;
    // Each declaration's slot in the block, and the node it holds.
    std::vector<std::pair<std::size_t, const void*>> declarations;
    // Each statement's slot in the block, the wrapper holding it, and its
    // place in the wrapper's body.
    struct Statement {
        std::size_t slot;
        const void* wrapper;
        std::size_t position;
    };
    std::vector<Statement> statements;
};

// The statements of a block at the top level: each a top-level statement,
// as the parser wraps one (a synthetic `main`).
auto wrapStatement(std::vector<ast::TopLevelItem>& items, const ast::CompiledBlock& block,
                   ast::ExprPtr statement) -> const void* {
    auto main = std::make_unique<ast::MainBlock>();
    main->location = block.location;
    main->synthetic = true;
    main->body.push_back(std::move(statement));
    const void* node = main.get();
    items.emplace_back(std::move(main));
    return node;
}

auto statementOf(ast::TopLevelItem& wrapper, std::size_t) -> ast::ExprPtr& {
    return std::get<std::unique_ptr<ast::MainBlock>>(wrapper)->body.front();
}

auto statementOf(ast::ModuleItem& wrapper, std::size_t position) -> ast::ExprPtr& {
    return std::get<std::unique_ptr<ast::FunctionDef>>(wrapper)->clauses.front().body[position];
}

class Lender {
public:
    ~Lender() {
        giveBack(m_topLevel);
        giveBack(m_modules);
    }

    auto lend(std::vector<ast::TopLevelItem>& items) -> void { lendFrom(items, m_topLevel); }

private:
    template <typename Item>
    auto lendFrom(std::vector<Item>& items, std::vector<Loan<Item>>& loans) -> void {
        const auto count = items.size();
        for (std::size_t index = 0; index < count; ++index) {
            if (auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&items[index]);
                module && *module)
                lendFrom((*module)->body, m_modules);
            auto* block = std::get_if<std::unique_ptr<ast::CompiledBlock>>(&items[index]);
            if (!block || !*block) continue;
            Loan<Item> loan{&items, block->get(), {}, {}};
            // A module's statements share one body, in order: a module holds
            // no statements of its own.
            std::unique_ptr<ast::FunctionDef> moduleBody;
            for (std::size_t slot = 0; slot < loan.block->items.size(); ++slot)
                std::visit([&](auto& node) {
                    using Node = std::decay_t<decltype(node)>;
                    if (!node) return;
                    if constexpr (isDeclaration<Node>) {
                        loan.declarations.emplace_back(slot, node.get());
                        items.emplace_back(std::move(node));
                    } else if constexpr (std::is_same_v<Item, ast::TopLevelItem>) {
                        loan.statements.push_back(
                            {slot, wrapStatement(items, *loan.block, std::move(node)), 0});
                    } else {
                        if (!moduleBody) {
                            moduleBody = std::make_unique<ast::FunctionDef>();
                            moduleBody->location = loan.block->location;
                            moduleBody->name = "%compiled";
                            moduleBody->clauses.emplace_back();
                        }
                        auto& body = moduleBody->clauses.front().body;
                        loan.statements.push_back({slot, moduleBody.get(), body.size()});
                        body.push_back(std::move(node));
                    }
                }, loan.block->items[slot]);
            if (moduleBody) items.emplace_back(std::move(moduleBody));
            loans.push_back(std::move(loan));
        }
    }

    template <typename Item>
    static auto giveBack(std::vector<Loan<Item>>& loans) -> void {
        for (auto& loan : loans) {
            auto& items = *loan.container;
            const auto holding = [&](const void* node) -> Item* {
                for (auto& item : items)
                    if (ast::heldNode(item) == node) return &item;
                return nullptr;
            };
            for (const auto& [slot, node] : loan.declarations)
                if (auto* item = holding(node))
                    std::visit([&](auto& lent) {
                        using Node = std::decay_t<decltype(lent)>;
                        if constexpr (isDeclaration<Node>) loan.block->items[slot] = std::move(lent);
                    }, *item);
            std::vector<const void*> wrappers;
            for (const auto& statement : loan.statements)
                if (auto* wrapper = holding(statement.wrapper)) {
                    loan.block->items[statement.slot] =
                        std::move(statementOf(*wrapper, statement.position));
                    wrappers.push_back(statement.wrapper);
                }
            std::erase_if(items, [&](const Item& item) {
                const auto* node = ast::heldNode(item);
                return node == nullptr ||
                       std::find(wrappers.begin(), wrappers.end(), node) != wrappers.end();
            });
        }
        loans.clear();
    }

    std::vector<Loan<ast::TopLevelItem>> m_topLevel;
    std::vector<Loan<ast::ModuleItem>> m_modules;
};

// What a program is, for the cache: its tree, as `--emit-ast` writes it.
auto programDigest(const ast::Program& program) -> std::string {
    beam::TermBuilder builder;
    ast::Converter converter(builder);
    return sha256Hex(beam::encodeEtf(converter.program(program, {}, {})));
}

} // namespace

auto checkCompileTime(ast::Program& program, const semantic::ImportedInterfaces* interfaces)
    -> std::unique_ptr<CheckedProgram> {
    // Keyed by the program as written, before its blocks are lent; answered by
    // the lent program's nodes, which a cached check is read back onto.
    const auto key = "compile-time\n" + programDigest(program);
    Lender lender;
    lender.lend(program.items);
    return cachedCheck(program, interfaces, key);
}

auto checkSilently(const ast::Program& program, const semantic::ImportedInterfaces* interfaces)
    -> std::unique_ptr<CheckedProgram> {
    auto checked = std::make_unique<CheckedProgram>();
    checked->analyzer = std::make_unique<semantic::Analyzer>(interfaces);
    checked->analyzer->analyze(program);
    checked->program = std::make_unique<Program>(program, *checked->analyzer);
    return checked;
}

auto cachedCheck(const ast::Program& program, const semantic::ImportedInterfaces* interfaces,
                 const std::string& key) -> std::unique_ptr<CheckedProgram> {
    const auto order = nodeOrder(program);
    std::optional<std::filesystem::path> path;
    if (const auto dir = cache::directory("walker-modules"))
        path = *dir / (sha256Hex("kex-walker-module-v1\n" + cache::toolchainFingerprint() +
                                 "\n" + key) +
                       ".bin");
    if (path)
        if (const auto bytes = readFileBytes(*path))
            if (auto loaded = load(program, order, *bytes)) {
                auto checked = std::make_unique<CheckedProgram>();
                checked->program = std::move(loaded);
                return checked;
            }
    auto checked = checkSilently(program, interfaces);
    if (path) {
        const auto bytes = save(*checked->program, order);
        cache::atomicWrite(*path, std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                                   bytes.size()));
    }
    return checked;
}

} // namespace kex::hir
