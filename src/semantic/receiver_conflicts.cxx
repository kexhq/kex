#include "receiver_conflicts.hxx"

#include "../common/type_def_utils.hxx"

#include <map>
#include <type_traits>
#include <variant>
#include <string>
#include <vector>

namespace kex::semantic {

namespace {

struct Site {
    std::string group;      // definitions sharing this are clauses of one method
    SourceLocation location;
};

// key: receiver type, method name, UFCS arity (receiver included)
using Key = std::tuple<std::string, std::string, size_t>;

// The type this expression names, or "" — only a plain name counts, since a
// structural type cannot be the receiver of a `make` block either.
auto typeNameOf(const ast::TypeExprPtr& type) -> std::string {
    if (!type) return {};
    if (const auto* named = std::get_if<ast::TypeName>(&type->kind))
        return named->parts.empty() ? std::string{} : named->parts.back();
    return {};
}

// A make-block method reaches its receiver implicitly through `this`, or as a
// first parameter that MATCHES it (`let head(@[x | _])`, `let rangeStart(x.._)`).
// Only the implicit form needs the receiver added to the arity — the same rule
// the type checker applies (receiverIsFirstParam).
auto receiverIsFirstParam(const ast::FunctionDef& def) -> bool {
    if (def.clauses.empty() || def.clauses[0].params.empty()) return false;
    const auto& first = def.clauses[0].params[0];
    if (!first.pattern) return false;
    const auto& kind = (*first.pattern)->kind;
    if (std::holds_alternative<ast::ListPattern>(kind) ||
        std::holds_alternative<ast::TuplePattern>(kind))
        return false;
    return !std::holds_alternative<ast::VarPattern>(kind) &&
           !std::holds_alternative<ast::ConstructorPattern>(kind);
}

auto paramCount(const ast::FunctionDef& def) -> size_t {
    return def.clauses.empty() ? 0 : def.clauses[0].params.size();
}

class Collector {
public:
    auto sites() -> std::map<Key, std::vector<Site>>& { return m_sites; }

    // Templated over the item container: a module body is a different variant
    // type from the program's, with the same members that matter here.
    // `topLevel` gates FREE functions only: one inside a `module` block is
    // namespaced (`Period.years(3)`), so it does not compete with a `make`
    // method for the bare `3.years` call the way a top-level function does.
    // `make` blocks are collected at any nesting.
    template <typename Items>
    void walk(const Items& items, bool topLevel = true) {
        // A free function's receiver type can come from its own parameter
        // annotation or from a standalone `name : String -> String` line, so
        // the annotations at this level are collected first.
        std::map<std::string, std::string> annotatedReceiver;
        for (const auto& item : items)
            if (const auto* annotation =
                    std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                if (*annotation && (*annotation)->type)
                    if (const auto* fn = std::get_if<ast::FunctionType>(
                            &(*annotation)->type->kind))
                        annotatedReceiver[(*annotation)->name] =
                            typeNameOf(fn->param);

        for (const auto& item : items) {
            if (const auto* make =
                    std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                if (*make) collectMake(**make);
            } else if (const auto* fn =
                           std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn && topLevel) collectFreeFunction(**fn, annotatedReceiver);
            } else if (const auto* module =
                           std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*module) walk((*module)->body, false);
            }
        }
    }

private:
    void collectMake(const ast::MakeDef& make) {
        const auto group = "make:" + std::to_string(
            reinterpret_cast<std::uintptr_t>(&make));
        for (const auto& target : makeTargetNames(make.target)) {
            auto method = [&](const ast::FunctionDef& def) {
                const auto arity = paramCount(def) +
                    (receiverIsFirstParam(def) ? 0u : 1u);
                m_sites[{target, def.name, arity}].push_back({group, def.location});
            };
            for (const auto& item : make.body) {
                if (const auto* fn =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                    if (*fn) method(**fn);
                } else if (const auto* visibility =
                               std::get_if<std::unique_ptr<ast::VisibilityBlock>>(
                                   &item)) {
                    if (!*visibility) continue;
                    for (const auto& inner : (*visibility)->items)
                        if (const auto* fn =
                                std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner))
                            if (*fn) method(**fn);
                }
            }
        }
    }

    void collectFreeFunction(
        const ast::FunctionDef& def,
        const std::map<std::string, std::string>& annotatedReceiver) {
        if (def.clauses.empty() || def.clauses[0].params.empty()) return;
        const auto& declared = def.clauses[0].params[0].type;
        auto receiver = declared ? typeNameOf(*declared) : std::string{};
        if (receiver.empty())
            if (auto annotated = annotatedReceiver.find(def.name);
                annotated != annotatedReceiver.end())
                receiver = annotated->second;
        if (receiver.empty()) return;
        m_sites[{receiver, def.name, paramCount(def)}].push_back(
            {"free:" + def.name, def.location});
    }

    std::map<Key, std::vector<Site>> m_sites;
};

} // namespace

auto findReceiverConflicts(const ast::Program& program)
    -> std::vector<Diagnostic> {
    Collector collector;
    collector.walk(program.items);

    std::vector<Diagnostic> diagnostics;
    for (const auto& [key, sites] : collector.sites()) {
        const auto& [type, method, arity] = key;
        (void)arity;
        for (size_t i = 1; i < sites.size(); i++) {
            const auto& first = sites[0];
            const auto& other = sites[i];
            if (other.group == first.group) continue;      // clauses of one method
            if (other.location.file != first.location.file) continue;
            Diagnostic diagnostic;
            diagnostic.level = Diagnostic::Level::Error;
            diagnostic.location = other.location;
            diagnostic.message = "`" + method + "` is defined twice for receiver `" +
                type + "` — declaration form carries no dispatch priority, so " +
                "neither definition can claim the call";
            diagnostic.notes.push_back(
                {first.location, "the other definition is here"});
            diagnostics.push_back(std::move(diagnostic));
            break;  // one report per (type, method, arity)
        }
    }
    return diagnostics;
}

namespace {

// Top-level items and module-body items are different variants: only the
// latter can hold a `private do` block. Asking the top-level variant for one
// is a compile error, not a `nullptr`, so the alternative is checked first.
template <typename T, typename V>
struct HasAlternative : std::false_type {};
template <typename T, typename... Ts>
struct HasAlternative<T, std::variant<Ts...>>
    : std::disjunction<std::is_same<T, Ts>...> {};

template <typename Item>
auto asVisibilityBlock(const Item& item)
    -> const std::unique_ptr<ast::VisibilityBlock>* {
    if constexpr (HasAlternative<std::unique_ptr<ast::VisibilityBlock>,
                                 Item>::value)
        return std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item);
    else
        return nullptr;
}

// One scope's declarations, keyed by the BEAM symbol they end up sharing.
struct EmittedSymbol {
    std::string receiver;         // the `make` target; "" for a plain function
    SourceLocation location;
    std::string claimedReceiver;  // a plain function's first-parameter type
};

class SymbolCollector {
public:
    template <typename Items>
    void walk(const Items& items) {
        std::map<std::pair<std::string, size_t>, std::vector<EmittedSymbol>> scope;
        // `name : String -> String` states a receiver the definition below it
        // leaves unannotated.
        std::map<std::string, std::string> annotatedReceiver;
        for (const auto& item : items)
            if (const auto* annotation =
                    std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item))
                if (*annotation && (*annotation)->type)
                    if (const auto* fn = std::get_if<ast::FunctionType>(
                            &(*annotation)->type->kind))
                        annotatedReceiver[(*annotation)->name] =
                            typeNameOf(fn->param);
        for (const auto& item : items) {
            if (const auto* make =
                    std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                if (*make) collectMake(**make, scope);
            } else if (const auto* fn =
                           std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn) collectFunction(**fn, scope, annotatedReceiver);
            } else if (const auto* visibility = asVisibilityBlock(item)) {
                // `private do ... end` is a visibility marker, not a scope of
                // its own: its functions are emitted into the same module.
                if (!*visibility) continue;
                for (const auto& inner : (*visibility)->items) {
                    if (const auto* fn =
                            std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner)) {
                        if (*fn) collectFunction(**fn, scope, annotatedReceiver);
                    } else if (const auto* make =
                                   std::get_if<std::unique_ptr<ast::MakeDef>>(&inner)) {
                        if (*make) collectMake(**make, scope);
                    }
                }
            } else if (const auto* module =
                           std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                // A nested module is its own BEAM module, so its symbols
                // never meet this scope's.
                if (*module) walk((*module)->body);
            }
        }
        m_scopes.push_back(std::move(scope));
    }

    auto scopes() const
        -> const std::vector<
            std::map<std::pair<std::string, size_t>, std::vector<EmittedSymbol>>>& {
        return m_scopes;
    }

private:
    using Scope = std::map<std::pair<std::string, size_t>, std::vector<EmittedSymbol>>;

    void collectMake(const ast::MakeDef& make, Scope& scope) {
        // A `serving` slot is reached through the gen_server plumbing under a
        // name of its own, so it never competes for the bare symbol.
        if (make.isServing) return;
        for (const auto& target : makeTargetNames(make.target)) {
            auto method = [&](const ast::FunctionDef& def) {
                // Only the implicit-receiver form. A method that matches its
                // receiver in a pattern (`let or(@Just(x), _)`) is emitted as
                // further CLAUSES of the shared symbol, which is how
                // optional.kex's `or` on Optional, on Result and as a plain
                // catch-all all live at `or/2`. The implicit form gets a
                // function of its own, and two of those cannot share a name.
                if (receiverIsFirstParam(def)) return;
                scope[{def.name, paramCount(def) + 1}].push_back(
                    {target, def.location, ""});
            };
            for (const auto& item : make.body) {
                if (const auto* fn =
                        std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                    if (*fn) method(**fn);
                } else if (const auto* visibility =
                               std::get_if<std::unique_ptr<ast::VisibilityBlock>>(
                                   &item)) {
                    if (!*visibility) continue;
                    for (const auto& inner : (*visibility)->items)
                        if (const auto* fn =
                                std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner))
                            if (*fn) method(**fn);
                }
            }
        }
    }

    void collectFunction(const ast::FunctionDef& def, Scope& scope,
                         const std::map<std::string, std::string>& annotated) {
        // The receiver this function would claim, if any: findReceiverConflicts
        // already reports a function and a method that name the SAME one, with
        // a message about dispatch rather than about symbols.
        std::string receiver;
        if (!def.clauses.empty() && !def.clauses[0].params.empty()) {
            const auto& declared = def.clauses[0].params[0].type;
            receiver = declared ? typeNameOf(*declared) : std::string{};
        }
        if (receiver.empty())
            if (auto found = annotated.find(def.name); found != annotated.end())
                receiver = found->second;
        scope[{def.name, paramCount(def)}].push_back(
            {"", def.location, receiver});
    }

    std::vector<Scope> m_scopes;
};

} // namespace

auto findMethodFunctionCollisions(const ast::Program& program)
    -> std::vector<Diagnostic> {
    SymbolCollector collector;
    collector.walk(program.items);

    std::vector<Diagnostic> diagnostics;
    for (const auto& scope : collector.scopes())
        for (const auto& [key, symbols] : scope) {
            const auto& [name, arity] = key;
            const EmittedSymbol* function = nullptr;
            const EmittedSymbol* method = nullptr;
            for (const auto& symbol : symbols) {
                if (symbol.receiver.empty()) {
                    if (!function) function = &symbol;
                } else if (!method) {
                    method = &symbol;
                }
            }
            if (!function || !method) continue;
            if (function->location.file != method->location.file) continue;
            // Same receiver on both sides is a dispatch tie, and
            // findReceiverConflicts has already said so.
            if (function->claimedReceiver == method->receiver) continue;

            Diagnostic diagnostic;
            diagnostic.level = Diagnostic::Level::Error;
            diagnostic.location = method->location;
            diagnostic.message = "`" + name + "` is both a method on `" +
                method->receiver + "` and a function of " +
                std::to_string(arity) + " argument(s) here — a method takes " +
                "its receiver as an argument, so the two compile to the same " +
                "`" + name + "/" + std::to_string(arity) + "`; rename one";
            diagnostic.notes.push_back(
                {function->location, "the function is here"});
            diagnostics.push_back(std::move(diagnostic));
        }
    return diagnostics;
}

} // namespace kex::semantic
