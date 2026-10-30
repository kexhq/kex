#pragma once

#include "../ast/ast.hxx"
#include <algorithm>
#include <optional>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

namespace kex {

struct TypeConstructorInfo {
    std::string name;
    size_t arity = 0;
};

// A single bare type name is the language's existing transparent-alias form
// (`type FilePath = String`). Keep this classification in one place so module
// exports, semantic interfaces, the interpreter, and BEAM lowering agree.
//
// A leading `|` opts out: `type SafeDivError = | DivideByZero` declares a
// one-variant ADT with a nullary constructor. That marker is the only thing
// separating the two readings, since both are a single capitalized name.
// An applied generic is one too: `type Incoming = Request<Binary>` renames a
// type, where `type Wrapped = Wrap(Binary)` declares a constructor.
inline auto isTransparentTypeAlias(const ast::TypeDef& def) -> bool {
    if (def.isDistinct || def.leadingPipe || !def.variants ||
        def.variants->size() != 1 || !(*def.variants)[0])
        return false;
    const auto& only = (*def.variants)[0]->kind;
    if (std::holds_alternative<ast::TypeName>(only)) return true;
    const auto* generic = std::get_if<ast::GenericType>(&only);
    return generic && generic->applied;
}

// What a non-generic alias stands for: a transparent alias's named type, a
// structural right-hand side (`type Nick = String?`, `type Handler = Env ->
// String`) no value carries as a tag, or the backing type of a distinct type
// (`distinct type UserId = String`), which is erased at run time. Null for a
// sum type and a generic alias or distinct type. `Type.fields` reads a field
// declared with one as what it stands for (kexhq/kex#530), on either backend.
inline auto aliasTarget(const ast::TypeDef& def) -> const ast::TypeExpr* {
    if (def.leadingPipe || !def.variants ||
        def.variants->size() != 1 || !(*def.variants)[0] || !def.typeParams.empty())
        return nullptr;
    const auto* only = (*def.variants)[0].get();
    if (def.isDistinct || isTransparentTypeAlias(def)) return only;
    if (std::holds_alternative<ast::TypeName>(only->kind) ||
        std::holds_alternative<ast::GenericType>(only->kind))
        return nullptr;
    return only;
}

// A trait's identity, the rule records follow: its own name at the top level,
// `Shop.Rules.Pricing` when `module Shop.Rules` declares it. Two modules may
// then each declare a `Shape` without one replacing the other.
inline auto traitIdentity(const std::string& modulePath, const std::string& name)
    -> std::string {
    return modulePath.empty() ? name : modulePath + "." + name;
}

// The identity a trait reference written in `scope` (a module path, "" at
// the top level) names, given the identities `known` accepts and `eachKnown`
// lists. In order: a trait of the innermost enclosing module outward
// (`Shape` inside `module Geo` is `Geo.Shape`; `Inner.Shape` inside `module
// Outer` may be `Outer.Inner.Shape`), one a `using` brought in, one declared
// as written, and last a trait exactly one module declares under that
// name — the way an unimported record is found. When several modules do,
// the reference is ambiguous: it comes back as written, unknown, and their
// identities land in `ambiguous` for the error to list.
template <typename Known, typename EachKnown>
auto resolveTraitName(const std::string& written, const std::string& scope,
                      const std::vector<std::string>& imported,
                      const Known& known, const EachKnown& eachKnown,
                      std::vector<std::string>* ambiguous = nullptr)
    -> std::string {
    for (auto enclosing = scope; !enclosing.empty();) {
        if (const auto candidate = enclosing + "." + written; known(candidate))
            return candidate;
        const auto dot = enclosing.rfind('.');
        if (dot == std::string::npos) break;
        enclosing.resize(dot);
    }
    for (const auto& module : imported)
        if (const auto candidate = module + "." + written; known(candidate))
            return candidate;
    if (known(written)) return written;
    std::vector<std::string> matches;
    const auto suffix = "." + written;
    eachKnown([&](const std::string& identity) {
        if (identity.size() > suffix.size() && identity.ends_with(suffix))
            matches.push_back(identity);
    });
    if (matches.size() == 1) return matches.front();
    if (ambiguous) {
        std::sort(matches.begin(), matches.end());
        *ambiguous = std::move(matches);
    }
    return written;
}

// The modules each scope's own `using` lines bring in, by module path (""
// for the file). Read from the declarations rather than tracked during a walk,
// for a phase that resolves names before it has walked to them.
template <typename Items>
auto collectScopeUsings(const Items& items,
                        std::unordered_map<std::string, std::set<std::string>>& out,
                        const std::string& modulePath = "") -> void {
    for (const auto& item : items) {
        if (auto* block = std::get_if<std::unique_ptr<ast::UsingBlock>>(&item);
            block && *block && (*block)->body.empty()) {
            std::string module;
            for (const auto& part : (*block)->module.parts)
                module += (module.empty() ? "" : ".") + part;
            out[modulePath].insert(std::move(module));
        } else if (auto* nested = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
                   nested && *nested) {
            const auto& name = (*nested)->name;
            const bool qualified =
                !modulePath.empty() && name.rfind(modulePath + ".", 0) == 0;
            collectScopeUsings((*nested)->body, out,
                               modulePath.empty() || qualified
                                   ? name : modulePath + "." + name);
        }
    }
}

// What `using`s are in effect in `scope`: the file's, and every enclosing
// module's own.
inline auto usingsVisibleIn(
    const std::unordered_map<std::string, std::set<std::string>>& scopeUsings,
    const std::string& scope) -> std::set<std::string> {
    std::set<std::string> visible;
    auto add = [&](const std::string& path) {
        if (auto found = scopeUsings.find(path); found != scopeUsings.end())
            visible.insert(found->second.begin(), found->second.end());
    };
    add("");
    for (auto enclosing = scope; !enclosing.empty();) {
        add(enclosing);
        const auto dot = enclosing.rfind('.');
        if (dot == std::string::npos) break;
        enclosing.resize(dot);
    }
    return visible;
}

// Every `trait` a program declares, at the top level or inside any module,
// with the path of the module declaring it ("" at the top level). Every phase
// that keeps a trait table walks it this way, so one declared inside a module
// is not known to the checker yet missing from the runtime.
template <typename Items, typename Visit>
auto forEachTraitDef(const Items& items, const Visit& visit,
                     const std::string& modulePath = "") -> void {
    for (const auto& item : items) {
        if (auto* trait = std::get_if<std::unique_ptr<ast::TraitDef>>(&item);
            trait && *trait)
            visit(**trait, modulePath);
        else if (auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item);
                 module && *module) {
            // A nested module may be written already qualified by its parent.
            const auto& name = (*module)->name;
            const bool qualified =
                !modulePath.empty() && name.rfind(modulePath + ".", 0) == 0;
            forEachTraitDef((*module)->body, visit,
                            modulePath.empty() || qualified
                                ? name : modulePath + "." + name);
        }
    }
}

// The dispatch names a `make` target covers. Normally one, but a union target
// (`make Float | Integer do ... end`) applies the block to every member, so
// each gets its own registration under the same method names. Nested unions
// flatten. Returns names in source order, deduplicated; an unnameable member
// (a bare generic, say) is skipped rather than poisoning the whole list.
// The type names a module DECLARES (records, ADTs/aliases), directly or in a
// nested module. A `make` targeting one of these is part of that type's own
// interface — it travels with any value of the type, so it is not import-gated.
// A `make` targeting anything else is patching a FOREIGN type, which is the
// action-at-a-distance `using` has to gate (docs/ufcs-dispatch-plan.md
// "Follow-up", case 1).
template <typename Items>
inline auto collectDeclaredTypeNames(const Items& items,
                                     std::set<std::string>& out) -> void {
    for (const auto& item : items) {
        std::visit([&out](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (node) out.insert(node->name);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                if (node) out.insert(node->name);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                if (node) collectDeclaredTypeNames(node->body, out);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
                // `compiled do … end` declares into the surrounding scope.
                if (node) collectDeclaredTypeNames(node->items, out);
            } else if constexpr (std::is_same_v<T,
                                                std::unique_ptr<ast::VisibilityBlock>>) {
                if (node) collectDeclaredTypeNames(node->items, out);
            }
        }, item);
    }
}

template <typename Items>
inline auto collectDeclaredRecordNames(const Items& items,
                                       std::set<std::string>& out) -> void {
    for (const auto& item : items) {
        std::visit([&out](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                if (node) out.insert(node->name);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
                if (node) collectDeclaredRecordNames(node->items, out);
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                if (node) collectDeclaredRecordNames(node->items, out);
            }
        }, item);
    }
}

inline auto makeTargetNames(const ast::TypeExprPtr& target,
                            const std::string& module = "",
                            const std::set<std::string>& records = {})
    -> std::vector<std::string> {
    std::vector<std::string> names;
    auto add = [&names, &module, &records](std::string n) {
        if (n.empty()) return;
        if (!module.empty() && records.count(n)) n = module + "." + n;
        if (std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(std::move(n));
    };
    auto walk = [&](auto&& self, const ast::TypeExprPtr& t) -> void {
        if (!t) return;
        if (const auto* un = std::get_if<ast::UnionType>(&t->kind)) {
            self(self, un->left);
            self(self, un->right);
            return;
        }
        if (const auto* tn = std::get_if<ast::TypeName>(&t->kind)) {
            std::string name;
            for (const auto& part : tn->parts) {
                if (!name.empty()) name += ".";
                name += part;
            }
            add(std::move(name));
            return;
        }
        if (const auto* gt = std::get_if<ast::GenericType>(&t->kind)) {
            std::string name;
            for (const auto& part : gt->name.parts) {
                if (!name.empty()) name += ".";
                name += part;
            }
            add(std::move(name));
            return;
        }
        if (std::holds_alternative<ast::ListType>(t->kind)) { add("List"); return; }
        if (std::holds_alternative<ast::MapType>(t->kind)) { add("Map"); return; }
    };
    walk(walk, target);
    return names;
}

inline auto typeConstructors(const ast::TypeDef& def)
    -> std::optional<std::vector<TypeConstructorInfo>> {
    if (!def.variants || def.isDistinct || isTransparentTypeAlias(def))
        return std::vector<TypeConstructorInfo>{};

    std::vector<TypeConstructorInfo> result;
    for (const auto& variant : *def.variants) {
        if (!variant) return std::nullopt;
        if (const auto* plain =
                std::get_if<ast::TypeName>(&variant->kind);
            plain && plain->parts.size() == 1) {
            result.push_back({plain->parts.front(), 0});
            continue;
        }
        if (const auto* generic =
                std::get_if<ast::GenericType>(&variant->kind);
            generic && generic->name.parts.size() == 1) {
            result.push_back(
                {generic->name.parts.front(), generic->args.size()});
            continue;
        }
        return std::nullopt;
    }
    return result;
}

} // namespace kex
