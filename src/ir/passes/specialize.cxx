#include "specialize.hxx"

#include "../clone.hxx"
#include "../walk.hxx"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace kex::ir {

namespace {

// Past these sizes (IR nodes) a copy costs more code than the calls it saves.
constexpr std::size_t kMaxCalleeSize = 600;
constexpr std::size_t kMaxBlockSize = 200;
constexpr std::size_t kMaxCopies = 64;

auto sizeOf(const Expr& e) -> std::size_t {
    std::size_t n = 1;
    forEachChild(e, [&](const ExprPtr& c) {
        if (c) n += sizeOf(*c);
    });
    return n;
}

auto isVarNamed(const ExprPtr& e, const std::string& name) -> bool {
    const auto* v = e ? std::get_if<Var>(&e->node) : nullptr;
    return v && v->name == name;
}

auto patternBinds(const PatternPtr& p, const std::string& name) -> bool {
    if (!p) return false;
    std::set<std::string> bound;
    bindPattern(*p, bound);
    return bound.contains(name);
}

// The reads of a block parameter in a callee's body, and how many of them a
// copy replaces: the block applied at its own arity, or passed unchanged as
// the same argument of a call to the callee itself.
struct Uses {
    std::size_t all = 0;
    std::size_t replaceable = 0;
    bool rebound = false;
};

auto countUses(const Expr& e, const std::string& param, const FunDef& callee,
               std::size_t index, std::size_t blockArity, Uses& uses) -> void {
    std::visit([&](const auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, Var>) {
            if (n.name == param) ++uses.all;
        } else if constexpr (std::is_same_v<T, CallIndirect>) {
            if (isVarNamed(n.callee, param) && n.args.size() == blockArity) ++uses.replaceable;
        } else if constexpr (std::is_same_v<T, Call>) {
            if (n.module.empty() && n.name == callee.name && n.arity == callee.arity &&
                index < n.args.size() && isVarNamed(n.args[index], param))
                ++uses.replaceable;
        } else if constexpr (std::is_same_v<T, Let>) {
            uses.rebound = uses.rebound || n.name == param;
        } else if constexpr (std::is_same_v<T, Lambda> || std::is_same_v<T, LetRec>) {
            for (const auto& p : n.params) uses.rebound = uses.rebound || p == param;
        } else if constexpr (std::is_same_v<T, Match> || std::is_same_v<T, TryCatch>) {
            for (const auto& c : n.clauses)
                for (const auto& p : c.patterns) uses.rebound = uses.rebound || patternBinds(p, param);
        } else if constexpr (std::is_same_v<T, Receive>) {
            uses.rebound = uses.rebound || n.senderVar == param;
            for (const auto& c : n.clauses) uses.rebound = uses.rebound || patternBinds(c.pattern, param);
        }
    }, e.node);
    forEachChild(e, [&](const ExprPtr& c) {
        if (c) countUses(*c, param, callee, index, blockArity, uses);
    });
}

// The name each clause of `callee` binds its block parameter `index` to (""
// for `_`), when every use of it is one a copy replaces; nullopt otherwise.
auto blockParameters(const FunDef& callee, std::size_t index, std::size_t blockArity)
    -> std::optional<std::vector<std::string>> {
    if (!callee.dictionaryTraits.empty() || callee.hasCapabilityContext ||
        callee.isCollisionDispatcher || callee.isFieldAccessor || callee.clauses.empty())
        return std::nullopt;
    std::vector<std::string> names;
    for (const auto& clause : callee.clauses) {
        if (clause.params.size() != static_cast<std::size_t>(callee.arity) || !clause.body ||
            !clause.params[index] || sizeOf(*clause.body) > kMaxCalleeSize)
            return std::nullopt;
        const auto& p = *clause.params[index];
        if (p.kind != PatKind::Var && p.kind != PatKind::Wild) return std::nullopt;
        const auto name = p.kind == PatKind::Var ? p.name : std::string{};
        if (!name.empty()) {
            if (clause.guard && *clause.guard) {
                std::set<std::string> read;
                collectFree(**clause.guard, {}, read);
                if (read.contains(name)) return std::nullopt;
            }
            Uses uses;
            countUses(*clause.body, name, callee, index, blockArity, uses);
            if (uses.rebound || uses.all != uses.replaceable) return std::nullopt;
        }
        names.push_back(name);
    }
    return names;
}

class Specializer {
public:
    explicit Specializer(Module& mod) : m_mod(mod) {}

    auto run() -> void {
        for (std::size_t i = 0; i < m_mod.functions.size(); ++i) {
            const auto& fn = m_mod.functions[i];
            m_names.insert(fn.name);
            const auto [at, fresh] = m_index.try_emplace({fn.name, fn.arity}, i);
            if (!fresh) at->second = kAmbiguous;
        }
        // Copies are added after the walk: the walk holds references into the
        // functions it is rewriting.
        for (auto& fn : m_mod.functions)
            for (auto& clause : fn.clauses)
                if (clause.body) findCalls(clause.body, {});
        for (auto& copy : m_copies) m_mod.functions.push_back(std::move(copy));
    }

private:
    static constexpr std::size_t kAmbiguous = static_cast<std::size_t>(-1);

    // A block bound by a `let` in scope, and the variables it captures.
    struct Block {
        const Lambda* lambda;
        std::set<std::string> captured;
    };
    using Blocks = std::map<std::string, Block>;

    // `blocks` past a new binding of `name`: a block that captures a variable
    // of that name would read the new one at a call below.
    static auto shadowed(Blocks blocks, const std::string& name) -> Blocks {
        std::erase_if(blocks, [&](const auto& entry) {
            return entry.first == name || entry.second.captured.contains(name);
        });
        return blocks;
    }
    static auto shadowedBy(Blocks blocks, const PatternPtr& pattern) -> Blocks {
        if (!pattern) return blocks;
        std::set<std::string> bound;
        bindPattern(*pattern, bound);
        for (const auto& name : bound) blocks = shadowed(std::move(blocks), name);
        return blocks;
    }

    auto findCalls(ExprPtr& e, const Blocks& blocks) -> void {
        if (!e) return;
        auto clauses = [&](std::vector<MatchClause>& cs) {
            for (auto& c : cs) {
                auto inner = blocks;
                for (const auto& p : c.patterns) inner = shadowedBy(std::move(inner), p);
                if (c.guard) findCalls(*c.guard, inner);
                findCalls(c.body, inner);
            }
        };
        if (auto* let = std::get_if<Let>(&e->node)) {
            findCalls(let->value, blocks);
            auto inner = shadowed(blocks, let->name);
            if (const auto* lambda = let->value ? std::get_if<Lambda>(&let->value->node) : nullptr) {
                Block block{lambda, {}};
                collectFree(*let->value, {}, block.captured);
                inner[let->name] = std::move(block);
            }
            findCalls(let->body, inner);
            return;
        }
        if (auto* match = std::get_if<Match>(&e->node)) {
            for (auto& s : match->subjects) findCalls(s, blocks);
            clauses(match->clauses);
            return;
        }
        if (auto* tryCatch = std::get_if<TryCatch>(&e->node)) {
            findCalls(tryCatch->body, blocks);
            clauses(tryCatch->clauses);
            return;
        }
        if (auto* lambda = std::get_if<Lambda>(&e->node)) {
            auto inner = blocks;
            for (const auto& p : lambda->params) inner = shadowed(std::move(inner), p);
            findCalls(lambda->body, inner);
            return;
        }
        if (auto* rec = std::get_if<LetRec>(&e->node)) {
            auto inner = blocks;
            for (const auto& p : rec->params) inner = shadowed(std::move(inner), p);
            findCalls(rec->funBody, inner);
            findCalls(rec->contBody, blocks);
            return;
        }
        if (auto* receive = std::get_if<Receive>(&e->node)) {
            for (auto& c : receive->clauses) {
                auto inner = receive->senderVar ? shadowed(blocks, *receive->senderVar) : blocks;
                findCalls(c.body, shadowedBy(std::move(inner), c.pattern));
            }
            if (receive->timeout) findCalls(*receive->timeout, blocks);
            if (receive->afterBody) findCalls(*receive->afterBody, blocks);
            return;
        }
        forEachChild(*e, [&](ExprPtr& c) { findCalls(c, blocks); });
        if (auto* call = std::get_if<Call>(&e->node)) specialize(*call, blocks);
    }

    // Sends `call` to a copy of its callee with the block it passes in place,
    // where it passes one the callee only applies.
    auto specialize(Call& call, const Blocks& blocks) -> void {
        if (!call.module.empty()) return;
        const auto found = m_index.find({call.name, call.arity});
        if (found == m_index.end() || found->second == kAmbiguous) return;
        const auto& callee = m_mod.functions[found->second];
        for (std::size_t k = 0; k < call.args.size(); ++k) {
            const auto* arg = call.args[k] ? std::get_if<Var>(&call.args[k]->node) : nullptr;
            const auto block = arg ? blocks.find(arg->name) : blocks.end();
            if (block == blocks.end()) continue;
            const auto& lambda = *block->second.lambda;
            if (!lambda.body || returnsEarly(*lambda.body) || sizeOf(*lambda.body) > kMaxBlockSize)
                continue;
            const auto key = std::make_tuple(found->second, k, &lambda);
            auto copy = m_made.find(key);
            if (copy == m_made.end()) {
                if (m_copies.size() >= kMaxCopies) return;
                const auto names = blockParameters(callee, k, lambda.params.size());
                if (!names) continue;
                copy = m_made.emplace(key, makeCopy(callee, k, *names, block->second)).first;
            }
            const auto& [name, arity] = copy->second;
            call.args.erase(call.args.begin() + static_cast<std::ptrdiff_t>(k));
            for (const auto& captured : block->second.captured) call.args.push_back(var(captured));
            call.name = name;
            call.arity = arity;
            return;
        }
    }

    auto makeCopy(const FunDef& callee, std::size_t index, const std::vector<std::string>& names,
                  const Block& block) -> std::pair<std::string, int> {
        std::string name;
        do {
            name = callee.name + "__block" + std::to_string(m_counter++);
        } while (!m_names.insert(name).second);
        const auto ordinal = m_counter;
        const auto capturedParam = [&](const std::string& captured) {
            return "_ir_Blk" + std::to_string(ordinal) + "_" + captured;
        };
        FunDef copy;
        copy.name = name;
        copy.arity = callee.arity - 1 + static_cast<int>(block.captured.size());
        copy.exported = false;
        copy.isFreeFunction = callee.isFreeFunction;
        copy.declaringModule = callee.declaringModule;
        for (std::size_t c = 0; c < callee.clauses.size(); ++c) {
            const auto& clause = callee.clauses[c];
            FunClause out;
            for (std::size_t i = 0; i < clause.params.size(); ++i)
                if (i != index) out.params.push_back(clonePattern(*clause.params[i]));
            for (const auto& captured : block.captured) {
                auto p = std::make_unique<Pattern>();
                p->kind = PatKind::Var;
                p->name = capturedParam(captured);
                out.params.push_back(std::move(p));
            }
            if (clause.guard && *clause.guard) out.guard = cloneExpr(**clause.guard);
            out.body = cloneExpr(*clause.body);
            if (!names[c].empty())
                inlineBlock(out.body, names[c], callee, index, name, copy.arity, block,
                           capturedParam);
            copy.clauses.push_back(std::move(out));
        }
        m_copies.push_back(std::move(copy));
        return {name, m_copies.back().arity};
    }

    // In a copy's body: each application of the block parameter becomes the
    // block's body, and each recursive call the call of the copy.
    template <typename CapturedParam>
    auto inlineBlock(ExprPtr& e, const std::string& param, const FunDef& callee,
                    std::size_t index, const std::string& copyName, int copyArity,
                    const Block& block, const CapturedParam& capturedParam) -> void {
        forEachChild(*e, [&](ExprPtr& c) {
            if (c) inlineBlock(c, param, callee, index, copyName, copyArity, block, capturedParam);
        });
        if (auto* apply = std::get_if<CallIndirect>(&e->node); apply && isVarNamed(apply->callee, param)) {
            const auto instance = "_ir_B" + std::to_string(m_inlined++);
            const Rename rename = [&](const std::string& n) {
                return block.captured.contains(n) ? capturedParam(n) : n + instance;
            };
            const auto& lambda = *block.lambda;
            auto body = cloneExpr(*lambda.body, rename);
            for (std::size_t i = lambda.params.size(); i-- > 0;) {
                auto let = std::make_unique<Expr>();
                let->node = Let{rename(lambda.params[i]), std::move(apply->args[i]), std::move(body)};
                body = std::move(let);
            }
            e = std::move(body);
            return;
        }
        if (auto* call = std::get_if<Call>(&e->node);
            call && call->module.empty() && call->name == callee.name &&
            call->arity == callee.arity && isVarNamed(call->args[index], param)) {
            call->args.erase(call->args.begin() + static_cast<std::ptrdiff_t>(index));
            for (const auto& captured : block.captured) call->args.push_back(var(capturedParam(captured)));
            call->name = copyName;
            call->arity = copyArity;
        }
    }

    Module& m_mod;
    std::map<std::pair<std::string, int>, std::size_t> m_index;
    std::set<std::string> m_names;
    std::map<std::tuple<std::size_t, std::size_t, const Lambda*>, std::pair<std::string, int>> m_made;
    std::vector<FunDef> m_copies;
    int m_counter = 0;
    int m_inlined = 0;
};

} // namespace

auto specializeBlocks(Module& mod) -> void { Specializer(mod).run(); }

} // namespace kex::ir
