#pragma once

#include "../ast/ast.hxx"
#include <algorithm>
#include <optional>
#include <set>
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
inline auto isTransparentTypeAlias(const ast::TypeDef& def) -> bool {
    return !def.leadingPipe && def.variants && def.variants->size() == 1 &&
           (*def.variants)[0] &&
           std::holds_alternative<ast::TypeName>(
               (*def.variants)[0]->kind);
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

inline auto makeTargetNames(const ast::TypeExprPtr& target)
    -> std::vector<std::string> {
    std::vector<std::string> names;
    auto add = [&names](std::string n) {
        if (n.empty()) return;
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
            if (!tn->parts.empty()) add(tn->parts.back());
            return;
        }
        if (const auto* gt = std::get_if<ast::GenericType>(&t->kind)) {
            if (!gt->name.parts.empty()) add(gt->name.parts.back());
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
    if (!def.variants || isTransparentTypeAlias(def))
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
