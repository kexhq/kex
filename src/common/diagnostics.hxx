#pragma once

#include <string>
#include <vector>

namespace kex::ast {
struct TypeExpr;
struct TypeName;
}
namespace kex::compiled {
struct CollapseNote;
}
namespace kex::semantic {
struct Diagnostic;
}

namespace kex::diagnostics {

// JSON string escaping (hand-rolled — no external dep).
auto jsonEscape(const std::string &s) -> std::string;

// Extract the "did you mean `X`?" portion from a diagnostic message, if any.
auto extractHint(const std::string &msg) -> std::string;

// Syntax-highlight a diagnostic message body.
auto colorizeMessage(const std::string &msg) -> std::string;

// `--collapse-report`: what `compiled do` chain collapse folded away.
auto printCollapseReport(const std::vector<kex::compiled::CollapseNote> &notes)
    -> void;

// Print one semantic diagnostic with source caret.
auto printSemanticDiagnostic(const kex::semantic::Diagnostic &diag) -> void;

// "3 type errors" / "1 type error".
auto errorCountPhrase(int count, const char *kind) -> std::string;

} // namespace kex::diagnostics
