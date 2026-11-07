#pragma once
// The one printer for a written type (`TypeExpr`).
#include "ast.hxx"

#include <string>

namespace kex::ast {

// How a written type is spelled.
enum class TypeSpelling {
    // As the source reads: dotted names, `Map<K, V>`, `:ok`, `?` where
    // nothing is written. Diagnostics and the language server.
    Source,
    // As the backends' dispatch tests name it: a name's last segment,
    // `{K: V}`, `Atom`, `[Char]` as `String`, `Any` where nothing is
    // declared.
    Dispatch,
};

auto typeText(const TypeExpr& type, TypeSpelling spelling = TypeSpelling::Source)
    -> std::string;

} // namespace kex::ast
