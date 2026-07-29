#pragma once

#include "../ast/ast.hxx"
#include <optional>
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
inline auto isTransparentTypeAlias(const ast::TypeDef& def) -> bool {
    return def.variants && def.variants->size() == 1 &&
           (*def.variants)[0] &&
           std::holds_alternative<ast::TypeName>(
               (*def.variants)[0]->kind);
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
