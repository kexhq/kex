#include "function_groups.hxx"
#include "type_text.hxx"

#include <algorithm>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace kex::ast {

auto methodDispatchTypes(const FunctionClause& clause,
                         const std::string& receiverType)
    -> std::vector<std::string> {
    std::vector<std::string> types{receiverType};
    for (const auto& param : clause.params)
        types.push_back(param.type && *param.type
            ? typeText(**param.type, TypeSpelling::Dispatch)
            : "Any");
    return types;
}

auto methodDispatchTypes(const FunctionDef& function,
                         const std::string& receiverType)
    -> std::vector<std::string> {
    if (function.clauses.empty()) return {receiverType};
    return methodDispatchTypes(function.clauses.front(), receiverType);
}

// Splitting same-name methods into per-signature functions is only sound when
// the signatures actually SEPARATE the values they accept — that is what lets
// a call site pick one statically (`Date + Duration` vs `Date + Period`) and
// what makes the guard-based dispatchers exhaustive.
//
// A position left untyped renders as `Any` and claims every value there, so a
// set mixing `Any` with a concrete type in one position is not an overload set
// at all: it is ordinary clause dispatch that happens to be spelled as several
// `let`s, as in `make Ordering`'s
//
//   let combine(@Equal, other: Ordering) = other
//   let combine(@Less, _)                = Less
//
// Those must stay ONE function so the clauses keep their source order; split
// apart, the receiver dispatcher sees two impls for the same type and neither
// is reachable. Differing param counts are equally inseparable.
auto separableOverloadSignatures(
    const std::map<std::string, std::vector<std::string>>& signatures) -> bool {
    const auto width = signatures.begin()->second.size();
    for (const auto& [signature, types] : signatures) {
        (void)signature;
        if (types.size() != width) return false;
    }
    for (size_t i = 0; i < width; i++) {
        bool any = false, concrete = false;
        for (const auto& [signature, types] : signatures) {
            (void)signature;
            if (types[i] == "Any") any = true; else concrete = true;
        }
        if (any && concrete) return false;
    }
    return true;
}

auto takesReceiverPattern(const FunctionClause& clause) -> bool {
    if (clause.params.empty()) return false;
    const auto& p0 = clause.params[0];
    return !p0.name && p0.pattern &&
        (std::holds_alternative<ThisPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<RecordPattern>((*p0.pattern)->kind) ||
         std::holds_alternative<RangePattern>((*p0.pattern)->kind));
}

auto methodArity(const FunctionClause& clause) -> size_t {
    // Implicit `this` only (`let reverse = …`), or before the parameters.
    return takesReceiverPattern(clause) ? clause.params.size() : clause.params.size() + 1;
}

auto methodArity(const FunctionDef& function) -> size_t {
    if (function.clauses.empty()) return 1;
    return methodArity(function.clauses.front());
}

// A free function never gets an implicit receiver slot, so `methodArity`'s
// "an unnamed first pattern is the receiver" reading is wrong for it: it made
// `respond(reply: String, route: String)` and `respond(JSON { status, body },
// route: String)` look like functions of different arity (kexhq/kex#347).
auto freeFunctionArity(const FunctionDef& function) -> size_t {
    if (function.clauses.empty()) return 0;
    return function.clauses.front().params.size();
}

auto signatureClauses(const FunctionDef& function)
    -> std::vector<const FunctionClause*> {
    const auto declaresTypes = [](const FunctionClause& clause) {
        return (clause.returnAnnotation && *clause.returnAnnotation) ||
               std::ranges::any_of(clause.params, [](const Param& param) {
                   return (param.type && *param.type) || param.signatureType;
               });
    };
    std::vector<const FunctionClause*> out;
    std::set<std::string> seen;
    for (const auto& clause : function.clauses) {
        // A clause that declares no types (`repeat(0)` beside `repeat(n:
        // Integer) -> This`) is a case of a typed sibling's signature, not a
        // signature of its own: counted apart, the two read as an overload
        // no call could choose between.
        if (!declaresTypes(clause) &&
            std::ranges::any_of(function.clauses, [&](const FunctionClause& sibling) {
                return &sibling != &clause && declaresTypes(sibling) &&
                       sibling.params.size() == clause.params.size();
            }))
            continue;
        std::string key;
        for (const auto& param : clause.params) {
            if (param.type && *param.type)
                key += typeText(**param.type, TypeSpelling::Dispatch);
            else if (param.signatureType)
                key += typeText(*param.signatureType, TypeSpelling::Dispatch);
            else
                key += "_";
            key += ",";
        }
        key += "->";
        if (clause.returnAnnotation && *clause.returnAnnotation)
            key += typeText(**clause.returnAnnotation, TypeSpelling::Dispatch);
        if (seen.insert(std::move(key)).second) out.push_back(&clause);
    }
    return out;
}

auto parameterNames(const FunctionDef& function) -> std::vector<std::string> {
    std::vector<std::string> names;
    for (const auto& clause : function.clauses) {
        if (names.size() < clause.params.size())
            names.resize(clause.params.size());
        for (size_t i = 0; i < clause.params.size(); ++i)
            if (names[i].empty() && clause.params[i].name)
                names[i] = *clause.params[i].name;
    }
    return names;
}

auto parameterDefaults(const FunctionDef& function)
    -> std::vector<const ExprPtr*> {
    std::vector<const ExprPtr*> defaults;
    for (const auto& clause : function.clauses) {
        if (defaults.size() < clause.params.size())
            defaults.resize(clause.params.size(), nullptr);
        for (size_t i = 0; i < clause.params.size(); ++i)
            if (!defaults[i] && clause.params[i].defaultValue)
                defaults[i] = &*clause.params[i].defaultValue;
    }
    return defaults;
}

namespace {

using OverloadKey = std::pair<std::string, size_t>;

// Where a container sits decides how its declarations' arity is counted and
// whether argument-type overloads can split a name.
struct GroupContext {
    bool methods = false;
    // For a make block: the (name, arity) pairs that are argument-type
    // overload sets, whose declarations group by dispatch types.
    std::set<OverloadKey> overloaded;
};

auto arityIn(const GroupContext& context, const FunctionDef& function)
    -> size_t {
    return context.methods ? methodArity(function) : freeFunctionArity(function);
}

auto continuesGroup(const GroupContext& context, const FunctionDef& open,
                    const FunctionDef& next) -> bool {
    if (open.name != next.name) return false;
    // `let %{expr}(...)` computes its name at compile time; two of these are
    // never the same declaration even when they happen to share `name == "%"`.
    if (open.computedName || next.computedName) return false;
    // Left apart so the checker can say the purity disagrees, rather than
    // the whole function silently taking the first declaration's.
    if (open.isFoul != next.isFoul || open.isSlot != next.isSlot ||
        open.isPredicate != next.isPredicate)
        return false;
    if (open.clauses.empty() || next.clauses.empty()) return false;
    const auto arity = arityIn(context, open);
    // The same parameter count also continues a method: in `factor(@Gram)`,
    // `factor(@Liter)`, `factor(_)` the `_` is the receiver too, although
    // only an `@`/record/range pattern says so on its own.
    const bool sameParamCount = context.methods &&
        open.clauses.front().params.size() ==
            next.clauses.front().params.size();
    if (arity != arityIn(context, next) && !sameParamCount) return false;
    if (context.methods && context.overloaded.count({open.name, arity}))
        return methodDispatchTypes(open, "") == methodDispatchTypes(next, "");
    return true;
}

template <typename Item>
auto functionIn(Item& item) -> FunctionDef* {
    if (auto* function = std::get_if<std::unique_ptr<FunctionDef>>(&item))
        return function->get();
    return nullptr;
}

// Items that sit between two clauses without separating them: a signature
// line written above the next clause, or a `using` in a module body.
template <typename Item>
auto transparentBetweenClauses(const Item& item) -> bool {
    if constexpr (std::is_constructible_v<Item, std::unique_ptr<TypeAnnotation>>)
        if (std::holds_alternative<std::unique_ptr<TypeAnnotation>>(item))
            return true;
    if constexpr (std::is_constructible_v<Item, std::unique_ptr<UsingBlock>>)
        if (std::holds_alternative<std::unique_ptr<UsingBlock>>(item))
            return true;
    return false;
}

template <typename Item>
auto collectOverloads(const std::vector<Item>& items,
                      std::unordered_map<std::string,
                          std::map<std::string, std::vector<std::string>>>&
                          signatures) -> void;

template <typename Item>
auto foldItems(std::vector<Item>& items, const GroupContext& context) -> void;

auto groupMake(MakeDef& make) -> void {
    // The overload decision looks at the whole block, the methods in its
    // `private do` / `public do` sections included.
    std::unordered_map<std::string,
        std::map<std::string, std::vector<std::string>>> signatures;
    collectOverloads(make.body, signatures);
    GroupContext context;
    context.methods = true;
    for (const auto& [key, set] : signatures) {
        if (set.size() < 2 || !separableOverloadSignatures(set)) continue;
        const auto slash = key.rfind('/');
        context.overloaded.insert(
            {key.substr(0, slash), std::stoul(key.substr(slash + 1))});
    }
    foldItems(make.body, context);
}

auto groupTrait(TraitDef& trait) -> void {
    GroupContext context;
    context.methods = true;
    foldItems(trait.body, context);
}

auto groupModule(ModuleDef& module) -> void {
    foldItems(module.body, GroupContext{});
}

auto groupCompiled(CompiledBlock& block) -> void {
    foldItems(block.items, GroupContext{});
}

template <typename Item>
auto collectOverloads(const std::vector<Item>& items,
                      std::unordered_map<std::string,
                          std::map<std::string, std::vector<std::string>>>&
                          signatures) -> void {
    for (const auto& item : items) {
        if (const auto* function =
                std::get_if<std::unique_ptr<FunctionDef>>(&item)) {
            if (!*function) continue;
            const auto key = (*function)->name + "/" +
                std::to_string(methodArity(**function));
            for (const auto& clause : (*function)->clauses) {
                auto types = methodDispatchTypes(clause, "");
                std::string signature;
                for (const auto& type : types) signature += type + ",";
                signatures[key].emplace(std::move(signature), std::move(types));
            }
        }
        if constexpr (std::is_constructible_v<Item, std::unique_ptr<VisibilityBlock>>)
            if (const auto* block =
                    std::get_if<std::unique_ptr<VisibilityBlock>>(&item);
                block && *block)
                collectOverloads((*block)->items, signatures);
    }
}

template <typename Item>
auto foldItems(std::vector<Item>& items, const GroupContext& context) -> void {
    std::vector<Item> kept;
    kept.reserve(items.size());
    FunctionDef* open = nullptr;
    for (auto& item : items) {
        // Containers nested here group their own items first.
        std::visit([&](auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ModuleDef>>) {
                if (node) groupModule(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<MakeDef>>) {
                if (node) groupMake(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<TraitDef>>) {
                if (node) groupTrait(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<CompiledBlock>>) {
                if (node) groupCompiled(*node);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<VisibilityBlock>>) {
                if (node) foldItems(node->items, context);
            }
        }, item);

        auto* function = functionIn(item);
        if (function && open && continuesGroup(context, *open, *function)) {
            for (auto& clause : function->clauses)
                open->clauses.push_back(std::move(clause));
            continue;
        }
        if (function) open = function;
        else if (!transparentBetweenClauses(item)) open = nullptr;
        kept.push_back(std::move(item));
    }
    items = std::move(kept);
}

} // namespace

auto groupFunctionClauses(Program& program) -> void {
    foldItems(program.items, GroupContext{});
}

} // namespace kex::ast
