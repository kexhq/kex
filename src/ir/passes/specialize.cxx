#include "specialize.hxx"

#include "../clone.hxx"
#include "../walk.hxx"

#include <algorithm>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
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

// The function a call names, as seen from `home` (the module the calling
// code was written in, "" for the one being rewritten); null when unknown.
using Resolve = std::function<const FunDef*(const std::string& home, const Call& call)>;

// How deep a block may be handed on from one function to the next.
constexpr int kMaxHandOn = 3;

// Whether a call of a block of `blockArity` parameters with `args` arguments
// is one a copy puts the block's body in place of: at its own arity, or one
// value that a block of several parameters takes apart (a Map's entries), or
// any for a block of none, which ignores them (`3.times { … }`).
auto appliesAtArity(std::size_t args, std::size_t blockArity) -> bool {
    return args == blockArity || (args == 1 && blockArity > 1) || blockArity == 0;
}

// The position `call` passes `param` at, when it passes it exactly once.
auto passedAt(const Call& call, const std::string& param) -> std::optional<std::size_t> {
    std::optional<std::size_t> at;
    for (std::size_t k = 0; k < call.args.size(); ++k)
        if (isVarNamed(call.args[k], param)) {
            if (at) return std::nullopt;
            at = k;
        }
    return at;
}

auto blockParameters(const FunDef& callee, std::size_t index, std::size_t blockArity,
                     const std::string& home = {}, const Resolve* resolve = nullptr, int depth = 0)
    -> std::optional<std::vector<std::string>>;

// Whether `call`, which passes the block `param`, hands it to a function a
// copy can be made of in turn.
auto handsOn(const Call& call, const std::string& param, const FunDef& from, std::size_t blockArity,
             const std::string& home, const Resolve* resolve, int depth) -> bool {
    if (!resolve || depth >= kMaxHandOn) return false;
    const auto at = passedAt(call, param);
    if (!at) return false;
    const auto* target = (*resolve)(home, call);
    if (!target || target == &from) return false;
    const auto targetHome = call.module.empty() ? home : call.module;
    return blockParameters(*target, *at, blockArity, targetHome, resolve, depth + 1).has_value();
}

// The reads of a block parameter in a callee's body, and how many of them a
// copy replaces: the block applied (appliesAtArity), passed unchanged as the
// same argument of a call to the callee itself, or handed on (handsOn).
struct Uses {
    std::size_t all = 0;
    std::size_t replaceable = 0;
    bool rebound = false;
};

auto countUses(const Expr& e, const std::string& param, const FunDef& callee, std::size_t index,
               std::size_t blockArity, const std::string& home, const Resolve* resolve, int depth,
               Uses& uses) -> void {
    std::visit([&](const auto& n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, Var>) {
            if (n.name == param) ++uses.all;
        } else if constexpr (std::is_same_v<T, CallIndirect>) {
            if (isVarNamed(n.callee, param) && appliesAtArity(n.args.size(), blockArity))
                ++uses.replaceable;
        } else if constexpr (std::is_same_v<T, Call>) {
            if (n.module.empty() && n.name == callee.name && n.arity == callee.arity &&
                index < n.args.size() && isVarNamed(n.args[index], param))
                ++uses.replaceable;
            else if (handsOn(n, param, callee, blockArity, home, resolve, depth))
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
        if (c) countUses(*c, param, callee, index, blockArity, home, resolve, depth, uses);
    });
}

// The name each clause of `callee` binds its block parameter `index` to (""
// for `_`), when every use of it is one a copy replaces; nullopt otherwise.
auto blockParameters(const FunDef& callee, std::size_t index, std::size_t blockArity,
                     const std::string& home, const Resolve* resolve, int depth)
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
            countUses(*clause.body, name, callee, index, blockArity, home, resolve, depth, uses);
            if (uses.rebound || uses.all != uses.replaceable) return std::nullopt;
        }
        names.push_back(name);
    }
    return names;
}

// A `match` on a constant atom, decided: the first clause that takes it, its
// pattern's name bound to the atom. `List.map` branches on the backend this
// way, and a copy must see only the branch the BEAM runs.
auto foldConstantMatches(ExprPtr& e) -> void {
    if (!e) return;
    forEachChild(*e, [](ExprPtr& c) { foldConstantMatches(c); });
    auto* match = std::get_if<Match>(&e->node);
    if (!match || match->subjects.size() != 1 || !match->subjects[0]) return;
    const auto* subject = std::get_if<Lit>(&match->subjects[0]->node);
    if (!subject || subject->kind != LitKind::Atom) return;
    const auto atom = subject->text;
    for (auto& clause : match->clauses) {
        if (clause.guard && *clause.guard) return;
        const Pattern* p = clause.patterns.size() == 1 ? clause.patterns[0].get() : nullptr;
        if (!p) return;
        bool takes = false;
        if (p->kind == PatKind::Wild || p->kind == PatKind::Var)
            takes = true;
        else if (p->kind == PatKind::Lit && p->litKind == LitKind::Atom)
            takes = p->litText == atom;
        else if (p->kind == PatKind::Construct && p->args.empty())
            takes = p->tag == atom;
        else
            return;
        if (!takes) continue;
        auto body = std::move(clause.body);
        if (p->kind == PatKind::Var) {
            auto let = std::make_unique<Expr>();
            let->node = Let{p->name, lit(LitKind::Atom, atom), std::move(body)};
            body = std::move(let);
        }
        e = std::move(body);
        return;
    }
}

// Whether every call `fn` makes is its own recursion or one of Erlang's
// BIFs: with a pure block in place, a copy of it has no effect at all.
auto callsOnlyItselfAndErlang(const FunDef& fn) -> bool {
    std::function<bool(const Expr&)> pure = [&](const Expr& e) -> bool {
        if (const auto* call = std::get_if<Call>(&e.node)) {
            // A raise ends the loop whatever it says (the no-match message).
            if (call->module == "erlang" && call->name == "error") return true;
            const bool itself = call->module.empty() && call->name == fn.name && call->arity == fn.arity;
            if (!itself && call->module != "erlang") return false;
        }
        return !anyChild(e, [&](const ExprPtr& c) { return !pure(*c); });
    };
    for (const auto& clause : fn.clauses)
        if (!clause.body || !pure(*clause.body) || (clause.guard && *clause.guard && !pure(**clause.guard)))
            return false;
    return true;
}

auto readsVar(const Expr& e, const std::string& name) -> bool {
    if (const auto* v = std::get_if<Var>(&e.node); v && v->name == name) return true;
    return anyChild(e, [&](const ExprPtr& c) { return readsVar(*c, name); });
}

// Whether every function `e` calls in its own module is one of `exported`:
// a copy elsewhere calls them there.
auto callsOnly(const Expr& e, const std::set<std::pair<std::string, int>>& exported) -> bool {
    if (const auto* call = std::get_if<Call>(&e.node);
        call && call->module.empty() && !exported.contains({call->name, call->arity}))
        return false;
    return !anyChild(e, [&](const ExprPtr& c) { return !callsOnly(*c, exported); });
}

auto cloneFunction(const FunDef& fn) -> FunDef {
    FunDef copy;
    copy.name = fn.name;
    copy.arity = fn.arity;
    copy.exported = fn.exported;
    copy.dictionaryTraits = fn.dictionaryTraits;
    copy.hasCapabilityContext = fn.hasCapabilityContext;
    copy.isFreeFunction = fn.isFreeFunction;
    copy.declaringModule = fn.declaringModule;
    copy.isCollisionDispatcher = fn.isCollisionDispatcher;
    copy.isFieldAccessor = fn.isFieldAccessor;
    for (const auto& clause : fn.clauses) {
        FunClause out;
        for (const auto& p : clause.params) out.params.push_back(p ? clonePattern(*p) : nullptr);
        if (clause.guard && *clause.guard) out.guard = cloneExpr(**clause.guard);
        if (clause.body) out.body = cloneExpr(*clause.body);
        out.routes = clause.routes;
        copy.clauses.push_back(std::move(out));
    }
    return copy;
}

// Whether some parameter of `fn` is a block a copy could put in place: one
// it applies, or hands on to a function that does.
auto takesBlock(const FunDef& fn, const Resolve& resolve) -> bool {
    constexpr std::size_t kMaxBlockArity = 6;
    for (std::size_t k = 0; k < static_cast<std::size_t>(fn.arity); ++k)
        for (std::size_t blockArity = 0; blockArity <= kMaxBlockArity; ++blockArity) {
            const auto names = blockParameters(fn, k, blockArity, {}, &resolve);
            if (!names) continue;
            for (std::size_t c = 0; c < names->size(); ++c)
                if (!(*names)[c].empty() && readsVar(*fn.clauses[c].body, (*names)[c])) return true;
        }
    return false;
}

struct ForeignRegistry {
    std::mutex mutex;
    bool loaded = false;
    std::function<ForeignFunctions()> load;
    ForeignFunctions functions;
    std::map<std::tuple<std::string, std::string, int>, const FunDef*> index;
};

auto foreignRegistry() -> ForeignRegistry& {
    static ForeignRegistry registry;
    return registry;
}

// The function `module:name/arity` the standard library's build recorded, or
// null. The copies made from it outlive no `setForeignFunctions`.
auto foreignCallee(const std::string& module, const std::string& name, int arity) -> const FunDef* {
    auto& registry = foreignRegistry();
    const std::lock_guard lock(registry.mutex);
    if (!registry.load) return nullptr;
    if (!registry.loaded) {
        registry.loaded = true;
        // A runtime without the record (an older build, a broken file) only
        // loses the copies.
        try {
            registry.functions = registry.load();
        } catch (const std::exception&) {
            registry.functions.clear();
        }
        for (const auto& [owner, functions] : registry.functions)
            for (const auto& fn : functions) registry.index[{owner, fn.name, fn.arity}] = &fn;
    }
    const auto found = registry.index.find({module, name, arity});
    return found == registry.index.end() ? nullptr : found->second;
}

// In a copy of `callee` made in another module: every call `callee` makes in
// its own module goes to `module`, but the recursion that passes the block
// (`param`, at `index`), which becomes the copy's own.
auto redirectCalls(Expr& e, const FunDef& callee, std::size_t index, const std::string& param,
                   const std::string& module) -> void {
    forEachChild(e, [&](ExprPtr& c) {
        if (c) redirectCalls(*c, callee, index, param, module);
    });
    auto* call = std::get_if<Call>(&e.node);
    if (!call || !call->module.empty()) return;
    const bool recursion = !param.empty() && call->name == callee.name && call->arity == callee.arity &&
                           index < call->args.size() && isVarNamed(call->args[index], param);
    if (!recursion) call->module = module;
}

// ---- State threading ------------------------------------------------------
//
// A block that reassigns variables of its caller takes their values after
// its own arguments and answers {Value, State}; lowering passes it through
// `kex_intrinsic_fun:threadState(Block, Initial, Statement)`, which keeps the
// state in the process dictionary. Where the callee can be copied, the copy
// carries the state instead: every expression that applies the block, or
// recurses with it, answers {Value, State}, and the state flows on in
// evaluation order.

auto isThreadState(const Call& call) -> bool {
    return call.module == "kex_intrinsic_fun" && call.name == "threadState" && call.arity == 3;
}

struct Threading {
    // An expression that answers {Value, State} once rewritten by `effect`.
    std::function<bool(const Expr&)> isEffect;
    std::function<ExprPtr(Expr&, const std::string& state)> effect;
    int* counter = nullptr;
    auto fresh(const char* stem) const -> std::string {
        return std::string("_ir_") + stem + std::to_string((*counter)++);
    }
};

auto effectful(const Expr& e, const Threading& t) -> bool {
    return t.isEffect(e) || anyChild(e, [&](const ExprPtr& c) { return effectful(*c, t); });
}

auto pairWith(ExprPtr value, const std::string& state, const Threading& t) -> ExprPtr {
    const auto name = t.fresh("StV");
    std::vector<ExprPtr> elements;
    elements.push_back(var(name));
    elements.push_back(var(state));
    auto tuple = std::make_unique<Expr>();
    tuple->node = MakeTuple{std::move(elements)};
    auto let = std::make_unique<Expr>();
    let->node = Let{name, std::move(value), std::move(tuple)};
    return let;
}

// `e` answering {Value, State}, from `state`; null where an effect sits
// somewhere this cannot follow (a nested function, a match subject).
auto thread(ExprPtr e, const std::string& state, const Threading& t) -> ExprPtr {
    if (!e) return nullptr;
    if (!effectful(*e, t)) return pairWith(std::move(e), state, t);
    if (t.isEffect(*e)) return t.effect(*e, state);
    if (auto* let = std::get_if<Let>(&e->node)) {
        if (!effectful(*let->value, t)) {
            auto body = thread(std::move(let->body), state, t);
            if (!body) return nullptr;
            let->body = std::move(body);
            return e;
        }
        auto value = thread(std::move(let->value), state, t);
        if (!value) return nullptr;
        const auto next = t.fresh("St");
        auto body = thread(std::move(let->body), next, t);
        if (!body) return nullptr;
        auto pattern = std::make_unique<Pattern>();
        pattern->kind = PatKind::Tuple;
        for (const auto& name : {let->name, next}) {
            auto p = std::make_unique<Pattern>();
            p->kind = PatKind::Var;
            p->name = name;
            pattern->args.push_back(std::move(p));
        }
        Match match;
        match.subjects.push_back(std::move(value));
        MatchClause clause;
        clause.patterns.push_back(std::move(pattern));
        clause.body = std::move(body);
        match.clauses.push_back(std::move(clause));
        auto out = std::make_unique<Expr>();
        out->node = std::move(match);
        return out;
    }
    if (auto* seq = std::get_if<Seq>(&e->node)) {
        if (seq->exprs.empty()) return nullptr;
        auto chain = std::move(seq->exprs.back());
        for (std::size_t i = seq->exprs.size() - 1; i-- > 0;) {
            auto let = std::make_unique<Expr>();
            let->node = Let{t.fresh("Seq"), std::move(seq->exprs[i]), std::move(chain)};
            chain = std::move(let);
        }
        return thread(std::move(chain), state, t);
    }
    if (auto* match = std::get_if<Match>(&e->node)) {
        for (const auto& s : match->subjects)
            if (s && effectful(*s, t)) return nullptr;
        for (auto& clause : match->clauses) {
            if (clause.guard && *clause.guard && effectful(**clause.guard, t)) return nullptr;
            auto body = thread(std::move(clause.body), state, t);
            if (!body) return nullptr;
            clause.body = std::move(body);
        }
        return e;
    }
    return nullptr;
}

auto containsReturn(const Expr& e) -> bool {
    if (std::holds_alternative<Return>(e.node)) return true;
    return anyChild(e, [](const ExprPtr& c) { return containsReturn(*c); });
}

// The one call in `e` that passes `name`, when `name` is read nowhere else.
auto soleCallPassing(Expr& e, const std::string& name, Call*& found, std::size_t& index, int& reads)
    -> void {
    if (const auto* v = std::get_if<Var>(&e.node); v && v->name == name) ++reads;
    if (auto* call = std::get_if<Call>(&e.node))
        for (std::size_t k = 0; k < call->args.size(); ++k)
            if (isVarNamed(call->args[k], name)) {
                found = call;
                index = k;
            }
    forEachChild(e, [&](ExprPtr& c) {
        if (c) soleCallPassing(*c, name, found, index, reads);
    });
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
        if (auto* call = std::get_if<Call>(&e->node)) {
            if (isThreadState(*call))
                threadCall(e, blocks);
            else
                specialize(*call, blocks);
        }
    }

    // Sends `call` to a copy of its callee with the block it passes in place,
    // where it passes one the callee only applies or hands on.
    auto specialize(Call& call, const Blocks& blocks) -> void {
        const bool passesBlock = std::any_of(call.args.begin(), call.args.end(), [&](const ExprPtr& a) {
            const auto* v = a ? std::get_if<Var>(&a->node) : nullptr;
            return v && blocks.contains(v->name);
        });
        if (!passesBlock) return;
        const auto* callee = m_resolve("", call);
        if (!callee) return;
        for (std::size_t k = 0; k < call.args.size(); ++k) {
            const auto* arg = call.args[k] ? std::get_if<Var>(&call.args[k]->node) : nullptr;
            const auto block = arg ? blocks.find(arg->name) : blocks.end();
            if (block == blocks.end()) continue;
            const auto& lambda = *block->second.lambda;
            if (!lambda.body || returnsEarly(*lambda.body) || sizeOf(*lambda.body) > kMaxBlockSize)
                continue;
            const auto copy = copyOf(*callee, k, block->second, call.module, false);
            if (!copy) continue;
            const auto& [name, arity] = *copy;
            call.args.erase(call.args.begin() + static_cast<std::ptrdiff_t>(k));
            for (const auto& captured : block->second.captured) call.args.push_back(var(captured));
            call.module.clear();
            call.name = name;
            call.arity = arity;
            return;
        }
    }

    // A copy of `callee` (written in `home`, "" for this module) with
    // `block` in place of its parameter `index`. It takes the block's
    // captured variables after its own parameters. `threaded`: the block
    // takes the caller's state last and answers {Value, State}; so does the
    // copy, which takes the state last. Nullopt where no copy can be made.
    auto copyOf(const FunDef& callee, std::size_t index, const Block& block, const std::string& home,
                bool threaded) -> std::optional<std::pair<std::string, int>> {
        const auto key = std::make_tuple(&callee, index, block.lambda);
        if (const auto made = m_made.find(key); made != m_made.end()) return made->second;
        if (m_copies.size() >= kMaxCopies) return std::nullopt;
        const auto& lambda = *block.lambda;
        if (threaded && lambda.params.empty()) return std::nullopt;
        const auto blockArity = lambda.params.size() - (threaded ? 1 : 0);
        const auto names = blockParameters(callee, index, blockArity, home, &m_resolve);
        if (!names) return std::nullopt;
        if (threaded)
            for (const auto& clause : callee.clauses)
                if (containsReturn(*clause.body)) return std::nullopt;
        std::string name;
        do {
            name = callee.name + (threaded ? "__state" : "__block") + std::to_string(m_counter++);
        } while (m_names.contains(name));
        m_names.insert(name);
        const auto ordinal = m_counter;
        const auto capturedParam = [ordinal](const std::string& captured) {
            return "_ir_Blk" + std::to_string(ordinal) + "_" + captured;
        };
        const int arity = callee.arity - 1 + static_cast<int>(block.captured.size()) + (threaded ? 1 : 0);
        // Registered before its body is made: a copy may reach itself
        // through a function it hands the block on to.
        m_made.emplace(key, std::make_pair(name, arity));
        FunDef copy;
        copy.name = name;
        copy.arity = arity;
        copy.exported = false;
        copy.isFreeFunction = callee.isFreeFunction;
        copy.declaringModule = home.empty() ? callee.declaringModule : std::string{};
        copy.pureCopy = lambda.pure && callsOnlyItselfAndErlang(callee);
        for (std::size_t c = 0; c < callee.clauses.size(); ++c) {
            const auto& clause = callee.clauses[c];
            const auto& param = (*names)[c];
            FunClause out;
            for (std::size_t i = 0; i < clause.params.size(); ++i)
                if (i != index) out.params.push_back(clonePattern(*clause.params[i]));
            for (const auto& captured : block.captured) {
                auto p = std::make_unique<Pattern>();
                p->kind = PatKind::Var;
                p->name = capturedParam(captured);
                out.params.push_back(std::move(p));
            }
            const auto state = "_ir_St" + std::to_string(m_inlined++);
            if (threaded) {
                auto p = std::make_unique<Pattern>();
                p->kind = PatKind::Var;
                p->name = state;
                out.params.push_back(std::move(p));
            }
            if (clause.guard && *clause.guard) out.guard = cloneExpr(**clause.guard);
            auto body = cloneExpr(*clause.body);
            if (!home.empty()) {
                if (out.guard && *out.guard) redirectCalls(**out.guard, callee, index, param, home);
                redirectCalls(*body, callee, index, param, home);
            }
            const Site site{callee, index, param, home, block, blockArity, name, arity, capturedParam};
            if (threaded) {
                Threading threading;
                threading.counter = &m_inlined;
                threading.isEffect = [&](const Expr& x) { return usesBlock(x, site); };
                threading.effect = [&](Expr& x, const std::string& current) -> ExprPtr {
                    return replaceUse(x, site, current);
                };
                body = thread(std::move(body), state, threading);
            } else if (!param.empty() && !inlineUses(body, site)) {
                body = nullptr;
            }
            if (!body) {
                m_made.erase(key);
                return std::nullopt;
            }
            out.body = std::move(body);
            copy.clauses.push_back(std::move(out));
        }
        m_copies.push_back(std::move(copy));
        return std::make_pair(name, arity);
    }

    // What a copy's body is rewritten against: the callee, its block
    // parameter as this clause names it, and the copy being made.
    struct Site {
        const FunDef& callee;
        std::size_t index;
        const std::string& param;
        const std::string& home;
        const Block& block;
        std::size_t blockArity;
        const std::string& copyName;
        int copyArity;
        std::function<std::string(const std::string&)> capturedParam;
    };

    // Whether `x` is a use of the block a copy replaces: an application, the
    // recursion passing it, or a call handing it on.
    static auto usesBlock(const Expr& x, const Site& site) -> bool {
        if (site.param.empty()) return false;
        if (const auto* apply = std::get_if<CallIndirect>(&x.node))
            return isVarNamed(apply->callee, site.param) && appliesAtArity(apply->args.size(), site.blockArity);
        const auto* call = std::get_if<Call>(&x.node);
        return call && passedAt(*call, site.param).has_value();
    }

    // The use `x` rewritten (usesBlock): the block's body in place of an
    // application, a call of the copy for the recursion, a call of a copy of
    // the function it hands the block on to. `state`: the copy is threaded,
    // and this is the state the use starts from; it answers {Value, State}.
    // Null where the function handed on cannot be copied.
    auto replaceUse(Expr& x, const Site& site, std::optional<std::string> state) -> ExprPtr {
        const auto& lambda = *site.block.lambda;
        if (auto* apply = std::get_if<CallIndirect>(&x.node)) {
            const auto instance = "_ir_B" + std::to_string(m_inlined++);
            const Rename rename = [&](const std::string& n) {
                return site.block.captured.contains(n) ? site.capturedParam(n) : n + instance;
            };
            auto body = cloneExpr(*lambda.body, rename);
            if (state) {
                auto let = std::make_unique<Expr>();
                let->node = Let{rename(lambda.params.back()), var(*state), std::move(body)};
                body = std::move(let);
            }
            if (site.blockArity == 0) return body;
            if (apply->args.size() == site.blockArity) {
                for (std::size_t i = site.blockArity; i-- > 0;) {
                    auto let = std::make_unique<Expr>();
                    let->node = Let{rename(lambda.params[i]), std::move(apply->args[i]), std::move(body)};
                    body = std::move(let);
                }
                return body;
            }
            // One value for a block of several parameters: taken apart.
            auto pattern = std::make_unique<Pattern>();
            pattern->kind = PatKind::Tuple;
            for (std::size_t i = 0; i < site.blockArity; ++i) {
                auto p = std::make_unique<Pattern>();
                p->kind = PatKind::Var;
                p->name = rename(lambda.params[i]);
                pattern->args.push_back(std::move(p));
            }
            Match match;
            match.subjects.push_back(std::move(apply->args[0]));
            MatchClause clause;
            clause.patterns.push_back(std::move(pattern));
            clause.body = std::move(body);
            match.clauses.push_back(std::move(clause));
            auto out = std::make_unique<Expr>();
            out->node = std::move(match);
            return out;
        }
        auto& call = std::get<Call>(x.node);
        const auto at = *passedAt(call, site.param);
        const bool recursion = call.module.empty() && call.name == site.callee.name &&
                               call.arity == site.callee.arity && at == site.index;
        std::string name = site.copyName;
        int arity = site.copyArity;
        if (!recursion) {
            const auto* target = m_resolve(site.home, call);
            if (!target) return nullptr;
            const auto targetHome = call.module.empty() ? site.home : call.module;
            const auto copy = copyOf(*target, at, site.block, targetHome, state.has_value());
            if (!copy) return nullptr;
            name = copy->first;
            arity = copy->second;
        }
        Call out{"", name, arity, {}, false};
        for (std::size_t k = 0; k < call.args.size(); ++k)
            if (k != at) out.args.push_back(std::move(call.args[k]));
        for (const auto& captured : site.block.captured) out.args.push_back(var(site.capturedParam(captured)));
        if (state) out.args.push_back(var(*state));
        auto expr = std::make_unique<Expr>();
        expr->node = std::move(out);
        return expr;
    }

    // In a plain copy's body: every use of the block replaced (replaceUse);
    // false where one cannot be.
    auto inlineUses(ExprPtr& e, const Site& site) -> bool {
        bool ok = true;
        forEachChild(*e, [&](ExprPtr& c) {
            if (c && ok) ok = inlineUses(c, site);
        });
        if (!ok) return false;
        if (!usesBlock(*e, site)) return true;
        auto replaced = replaceUse(*e, site, std::nullopt);
        if (!replaced) return false;
        e = std::move(replaced);
        return true;
    }

    // `threadState(Block, Initial, Statement)`, both functions bound in
    // scope, where the statement's call can go to a copy of its callee
    // carrying the state: the statement in place, the state threaded.
    auto threadCall(ExprPtr& e, const Blocks& blocks) -> void {
        auto& call = std::get<Call>(e->node);
        const auto* blockVar = std::get_if<Var>(&call.args[0]->node);
        const auto* initial = std::get_if<Var>(&call.args[1]->node);
        const auto* statementVar = std::get_if<Var>(&call.args[2]->node);
        if (!blockVar || !initial || !statementVar) return;
        const auto blockAt = blocks.find(blockVar->name);
        const auto statementAt = blocks.find(statementVar->name);
        if (blockAt == blocks.end() || statementAt == blocks.end()) return;
        const auto& block = blockAt->second;
        const auto& statement = *statementAt->second.lambda;
        if (block.lambda->params.empty() || !block.lambda->body || statement.params.size() != 1 ||
            !statement.body || returnsEarly(*block.lambda->body) ||
            sizeOf(*block.lambda->body) > kMaxBlockSize)
            return;
        const auto& placeholder = statement.params[0];
        auto body = cloneExpr(*statement.body);
        Call* target = nullptr;
        std::size_t index = 0;
        int reads = 0;
        soleCallPassing(*body, placeholder, target, index, reads);
        if (!target || reads != 1) return;
        const auto* callee = m_resolve("", *target);
        if (!callee) return;
        const auto copy = copyOf(*callee, index, block, target->module, true);
        if (!copy) return;
        const auto [copyName, copyArity] = *copy;
        Threading threading;
        threading.counter = &m_inlined;
        Call* const goal = target;
        threading.isEffect = [goal](const Expr& x) {
            const auto* c = std::get_if<Call>(&x.node);
            return c && c == goal;
        };
        threading.effect = [&, index](Expr& x, const std::string& state) -> ExprPtr {
            auto& c = std::get<Call>(x.node);
            Call out{"", copyName, copyArity, {}, false};
            for (std::size_t k = 0; k < c.args.size(); ++k)
                if (k != index) out.args.push_back(std::move(c.args[k]));
            for (const auto& captured : block.captured) out.args.push_back(var(captured));
            out.args.push_back(var(state));
            auto expr = std::make_unique<Expr>();
            expr->node = std::move(out);
            return expr;
        };
        const auto start = threading.fresh("St");
        auto threaded = thread(std::move(body), start, threading);
        if (!threaded) return;
        auto let = std::make_unique<Expr>();
        let->node = Let{start, var(initial->name), std::move(threaded)};
        e = std::move(let);
    }

    Module& m_mod;
    std::map<std::pair<std::string, int>, std::size_t> m_index;
    std::set<std::string> m_names;
    std::map<std::tuple<const FunDef*, std::size_t, const Lambda*>, std::pair<std::string, int>> m_made;
    std::vector<FunDef> m_copies;
    int m_counter = 0;
    int m_inlined = 0;
    // A call's callee: another module's through the standard library's
    // record, this one's by name where it is unambiguous.
    Resolve m_resolve = [this](const std::string& home, const Call& call) -> const FunDef* {
        if (!call.module.empty()) return foreignCallee(call.module, call.name, call.arity);
        if (!home.empty()) return foreignCallee(home, call.name, call.arity);
        const auto local = m_index.find({call.name, call.arity});
        if (local == m_index.end() || local->second == kAmbiguous) return nullptr;
        return &m_mod.functions[local->second];
    };
};

} // namespace

auto specializeBlocks(Module& mod) -> void { Specializer(mod).run(); }

auto specializableFunctions(const Module& mod) -> std::vector<FunDef> {
    std::set<std::pair<std::string, int>> exported;
    std::map<std::pair<std::string, int>, int> defined;
    for (const auto& fn : mod.functions) {
        ++defined[{fn.name, fn.arity}];
        if (fn.exported) exported.insert({fn.name, fn.arity});
    }
    // The exported functions as a copy would see them, first: one may hand
    // its block on to another.
    std::vector<FunDef> folded;
    for (const auto& fn : mod.functions) {
        if (!fn.exported || defined[{fn.name, fn.arity}] != 1) continue;
        auto copy = cloneFunction(fn);
        for (auto& clause : copy.clauses) {
            if (clause.guard) foldConstantMatches(*clause.guard);
            foldConstantMatches(clause.body);
        }
        folded.push_back(std::move(copy));
    }
    std::map<std::pair<std::string, int>, const FunDef*> byName;
    for (const auto& fn : folded) byName[{fn.name, fn.arity}] = &fn;
    const Resolve resolve = [&](const std::string& home, const Call& call) -> const FunDef* {
        if (!home.empty() || !call.module.empty()) return nullptr;
        const auto found = byName.find({call.name, call.arity});
        return found == byName.end() ? nullptr : found->second;
    };
    std::vector<bool> keep;
    for (const auto& copy : folded) {
        if (!takesBlock(copy, resolve)) {
            keep.push_back(false);
            continue;
        }
        bool callable = true;
        for (const auto& clause : copy.clauses)
            callable = callable && clause.body && callsOnly(*clause.body, exported) &&
                       (!clause.guard || !*clause.guard || callsOnly(**clause.guard, exported));
        keep.push_back(callable);
    }
    std::vector<FunDef> out;
    for (std::size_t i = 0; i < folded.size(); ++i)
        if (keep[i]) out.push_back(std::move(folded[i]));
    return out;
}

auto setForeignFunctions(std::function<ForeignFunctions()> load) -> void {
    auto& registry = foreignRegistry();
    const std::lock_guard lock(registry.mutex);
    registry.load = std::move(load);
    registry.loaded = false;
    registry.index.clear();
    registry.functions.clear();
}

} // namespace kex::ir
