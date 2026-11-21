#pragma once

#include <exception>
#include <string>
#include <vector>

namespace kex::ast {
struct TypeExpr;
struct TypeName;
} // namespace kex::ast
namespace kex::compiled {
struct CollapseNote;
}
namespace kex::semantic {
struct Diagnostic;
}
namespace kex {
struct ParseDiagnostic;
}

namespace kex::diagnostics {

// Whether a diagnostic shows its source with the span underlined
// (kexhq/kex#657). On by default; `--no-snippets` and `--json` turn it off.
inline bool snippets = true;

// JSON string escaping (hand-rolled — no external dep).
auto jsonEscape(const std::string& s) -> std::string;

// Extract the "did you mean `X`?" portion from a diagnostic message, if any.
auto extractHint(const std::string& msg) -> std::string;

// Syntax-highlight a diagnostic message body.
auto colorizeMessage(const std::string& msg) -> std::string;

// `--collapse-report`: what `compiled do` chain collapse folded away.
auto printCollapseReport(const std::vector<kex::compiled::CollapseNote>& notes) -> void;

// One diagnostic as printed: `file:line:col: [KEXET-0001]: Error, message`,
// then the source underlined when it has an end, then its notes. Colored when
// kex::color is enabled.
auto formatDiagnostic(const kex::semantic::Diagnostic& diag) -> std::string;

// Print one semantic diagnostic with source caret.
auto printSemanticDiagnostic(const kex::semantic::Diagnostic& diag) -> void;

// A parse diagnostic as a semantic one (always an error), and printed as one.
auto fromParseDiagnostic(const kex::ParseDiagnostic& diag) -> kex::semantic::Diagnostic;
auto printParseDiagnostic(const kex::ParseDiagnostic& diag) -> void;

// `path: [KEXEB-0001]: Error, message`, for a diagnostic that names a file but no
// place in it (a lowering error). Colored like formatDiagnostic.
auto formatFileDiagnostic(const std::string& path, const kex::semantic::Diagnostic& diag)
    -> std::string;

// `[KEXET-0001]: Error, message` with no location, for the REPL, whose input
// is wrapped in source the user never wrote. Colored like formatDiagnostic.
auto formatBriefDiagnostic(const kex::semantic::Diagnostic& diag) -> std::string;
// An exception as the REPL prints it: each diagnostic of a
// semantic::DiagnosticError on a line of its own, `indent` before every
// line after the first; otherwise the exception's message.
auto formatBriefException(const std::exception& e, const std::string& indent) -> std::string;
// The same for an error with no diagnostic behind it (a runtime error).
auto formatBriefError(const std::string& message) -> std::string;

// `Aborted: <what>`, the line that ends a failed check or build.
auto printAborted(const std::string& what) -> void;

// "3 type errors" / "1 type error".
auto errorCountPhrase(int count, const char* kind) -> std::string;

} // namespace kex::diagnostics
