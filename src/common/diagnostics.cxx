#include "diagnostics.hxx"

#include "color.hxx"
#include "file_io.hxx"

#include "../compiled/expand.hxx"
#include "../parser/parser.hxx"
#include "../semantic/analyzer.hxx"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

namespace kex::diagnostics {

auto jsonEscape(const std::string& s) -> std::string {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

// Extract the "did you mean `X`?" portion from a diagnostic message, if any.
auto extractHint(const std::string& msg) -> std::string {
    const std::string needle = "did you mean `";
    auto pos = msg.find(needle);
    if (pos == std::string::npos) {
        return "";
    }
    pos += needle.size();
    auto end = msg.find('`', pos);
    if (end == std::string::npos) {
        return "";
    }
    return msg.substr(pos, end - pos);
}

// Syntax-highlights a diagnostic message body using a palette kept distinct
// from Value::inspect's literal coloring: type names / type vars become cyan
// (matching the REPL/IO.inspect type suffix), function names become bold
// (backtick spans and signature heads), arrows (->) become magenta, and
// backtick delimiters become gray. Cyan-vs-bold keeps functions and types
// distinguishable. Honors kex::color::enabled.
auto colorizeMessage(const std::string& msg) -> std::string {
    using namespace kex::color;
    const auto isIdentChar = [](char c) -> bool {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    std::string out;
    out.reserve(msg.size() * 2);
    const auto n = msg.size();
    bool atLineStart = true;
    for (size_t i = 0; i < n;) {
        char c = msg[i];

        if (c == '`') {
            size_t end = msg.find('`', i + 1);
            if (end == std::string::npos) {
                out += c;
                i++;
                continue;
            }
            out += apply(gray);
            out += '`';
            out += apply(reset);
            out += apply(bold);
            out.append(msg, i + 1, end - i - 1);
            out += apply(reset);
            out += apply(gray);
            out += '`';
            out += apply(reset);
            i = end + 1;
            atLineStart = false;
            continue;
        }

        if (atLineStart) {
            bool startsLower = std::isalpha(static_cast<unsigned char>(c)) &&
                               std::islower(static_cast<unsigned char>(c));
            bool identStart = std::isalpha(static_cast<unsigned char>(c)) || c == '_';
            if (startsLower || (c == '_' && identStart)) {
                size_t j = i + 1;
                while (j < n && isIdentChar(msg[j])) {
                    j++;
                }
                if (j < n && (msg[j] == '?' || msg[j] == '!')) {
                    j++;
                }
                size_t k = j;
                while (k < n && msg[k] == ' ') {
                    k++;
                }
                if (k < n && msg[k] == ':') {
                    out += apply(bold);
                    out.append(msg, i, j - i);
                    out += apply(reset);
                    i = j;
                    atLineStart = false;
                    continue;
                }
            }
        }

        if (c == '-' && i + 1 < n && msg[i + 1] == '>') {
            out += apply(magenta);
            out += "->";
            out += apply(reset);
            i += 2;
            atLineStart = false;
            continue;
        }

        if (std::isupper(static_cast<unsigned char>(c)) && (i == 0 || !isIdentChar(msg[i - 1]))) {
            size_t j = i + 1;
            while (j < n && isIdentChar(msg[j])) {
                j++;
            }
            out += apply(cyan);
            out.append(msg, i, j - i);
            out += apply(reset);
            i = j;
            atLineStart = false;
            continue;
        }

        if (c == '\n') {
            out += c;
            i++;
            atLineStart = true;
            continue;
        }
        out += c;
        i++;
        atLineStart = false;
    }
    return out;
}

// `--collapse-report`: what `compiled do` chain collapse folded away, and why
// the rest did not.
//
// Goes to stderr and says nothing at all for a program with no `compiled`
// block, so it composes with every other mode — you can ask about a run, a
// check or an emit without changing what any of them print.
auto printCollapseReport(const std::vector<kex::compiled::CollapseNote>& notes) -> void {
    if (notes.empty()) {
        return;
    }

    // Source order, not the order collapse worked in: a chain that failed and
    // was retried from the inside reports its outer attempt in a later round,
    // and reading the file top to bottom is what a user wants.
    auto ordered = notes;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.location.file != b.location.file) {
            return a.location.file < b.location.file;
        }
        if (a.location.line != b.location.line) {
            return a.location.line < b.location.line;
        }
        return a.location.column < b.location.column;
    });

    // Retrying a failed chain from the inside re-reports the parts it tries
    // again, and everything written inside one `"${...}"` shares that string's
    // position — so the same verdict can arrive several times over. Identical
    // lines say nothing extra. A location with BOTH verdicts keeps both: those
    // are genuinely different sub-expressions that happen to start together.
    ordered.erase(
        std::unique(
            ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) {
                return a.location.file == b.location.file && a.location.line == b.location.line &&
                       a.location.column == b.location.column && a.collapsed == b.collapsed &&
                       a.reason == b.reason;
            }),
        ordered.end());

    std::size_t collapsed = 0;
    for (const auto& note : ordered) {
        if (note.collapsed) {
            collapsed++;
        }
    }

    std::cerr << kex::color::apply(kex::color::bold)
              << "collapse:" << kex::color::apply(kex::color::reset) << " " << collapsed << " of "
              << ordered.size() << " evaluated at compile time\n";
    for (const auto& note : ordered) {
        std::cerr << "  " << kex::color::apply(kex::color::gray) << note.location.file << ":"
                  << note.location.line << ":" << note.location.column
                  << kex::color::apply(kex::color::reset) << " ";
        if (note.collapsed) {
            std::cerr << kex::color::apply(kex::color::green) << "collapsed"
                      << kex::color::apply(kex::color::reset) << "\n";
        } else {
            std::cerr << kex::color::apply(kex::color::magenta) << "kept"
                      << kex::color::apply(kex::color::reset) << " — " << note.reason << "\n";
        }
    }
}

// `[KEXET-0001]: Error,`: the code naming the kind of problem, then whether
// it is an error or a warning, spelled out rather than left to the code's E or
// W. A diagnostic with no code says `error:` or `warning:` instead.
static auto severityLabel(const kex::semantic::Diagnostic& diag) -> std::string {
    using namespace kex::color;
    const bool isError = diag.level == kex::semantic::Diagnostic::Level::Error;
    std::string out = std::string(apply(bold)) + apply(isError ? red : yellow);
    if (const auto id = kex::diag::idOf(diag.code); !id.empty()) {
        out += "[" + std::string(id) + "]: " + (isError ? "Error," : "Warning,");
    } else {
        out += isError ? "error:" : "warning:";
    }
    return out + apply(reset);
}

auto formatDiagnostic(const kex::semantic::Diagnostic& diag) -> std::string {
    using namespace kex::color;
    const bool isError = diag.level == kex::semantic::Diagnostic::Level::Error;
    const auto* tone = isError ? red : yellow;
    std::ostringstream out;
    out << apply(gray) << diag.location.file << ":" << diag.location.line << ":"
        << diag.location.column;
    if (diag.endLocation) {
        out << "-" << diag.endLocation->line << ":" << diag.endLocation->column;
    }
    out << ":" << apply(reset) << " " << severityLabel(diag) << " " << colorizeMessage(diag.message)
        << "\n";

    if (diag.endLocation) {
        auto source = readFile(std::string(diag.location.file));
        if (!source.empty()) {
            std::istringstream lines{source};
            std::string line;
            for (int current = 1; current <= diag.location.line && std::getline(lines, line);
                 ++current) {
                if (current != diag.location.line) {
                    continue;
                }
                const auto start = std::max(1, diag.location.column);
                int finish = static_cast<int>(line.size()) + 1;
                if (diag.endLocation->line == diag.location.line) {
                    finish = std::max(start + 1, diag.endLocation->column);
                }
                const auto width = std::max(1, finish - start);
                out << "  " << current << " | " << line << "\n"
                    << "    | " << std::string(static_cast<size_t>(start - 1), ' ') << apply(tone)
                    << "^" << std::string(static_cast<size_t>(width - 1), '~') << apply(reset)
                    << "\n";
                if (diag.endLocation->line != diag.location.line) {
                    out << "    | ... through line " << diag.endLocation->line << ", column "
                        << diag.endLocation->column << "\n";
                }
            }
        }
    }

    for (const auto& note : diag.notes) {
        out << apply(gray) << note.location.file << ":" << note.location.line << ":"
            << note.location.column << ":" << apply(reset) << " " << apply(bold) << apply(cyan)
            << "note:" << apply(reset) << " " << colorizeMessage(note.message) << "\n";
    }
    return out.str();
}

auto formatBriefDiagnostic(const kex::semantic::Diagnostic& diag) -> std::string {
    return severityLabel(diag) + " " + colorizeMessage(diag.message);
}

auto formatFileDiagnostic(const std::string& path, const kex::semantic::Diagnostic& diag)
    -> std::string {
    using namespace kex::color;
    return std::string(apply(gray)) + path + ":" + apply(reset) + " " +
           formatBriefDiagnostic(diag) + "\n";
}

auto formatBriefException(const std::exception& e, const std::string& indent) -> std::string {
    if (const auto* coded = dynamic_cast<const kex::diag::CodedError*>(&e)) {
        kex::semantic::Diagnostic diag{
            .level = kex::semantic::Diagnostic::Level::Error,
            .message = coded->what(),
            .code = coded->code,
            .args = coded->args,
        };
        return formatBriefDiagnostic(diag);
    }
    const auto* failed = dynamic_cast<const kex::semantic::DiagnosticError*>(&e);
    if (!failed || failed->diagnostics.empty()) {
        return formatBriefError(e.what());
    }
    std::string out;
    for (const auto& diag : failed->diagnostics) {
        if (!out.empty()) {
            out += "\n" + indent;
        }
        out += formatBriefDiagnostic(diag);
    }
    return out;
}

auto formatBriefError(const std::string& message) -> std::string {
    using namespace kex::color;
    return std::string(apply(bold)) + apply(red) + "error:" + apply(reset) + " " +
           colorizeMessage(message);
}

auto printSemanticDiagnostic(const kex::semantic::Diagnostic& diag) -> void {
    std::cerr << formatDiagnostic(diag);
}

auto fromParseDiagnostic(const kex::ParseDiagnostic& diag) -> kex::semantic::Diagnostic {
    return kex::semantic::Diagnostic{
        .level = kex::semantic::Diagnostic::Level::Error,
        .location = diag.location,
        .message = diag.message,
        .code = diag.code,
        .args = diag.args,
    };
}

auto printParseDiagnostic(const kex::ParseDiagnostic& diag) -> void {
    printSemanticDiagnostic(fromParseDiagnostic(diag));
}

auto printAborted(const std::string& what) -> void {
    using namespace kex::color;
    std::cerr << apply(bold) << apply(magenta) << "Aborted:" << apply(reset) << " " << what << "\n";
}

auto errorCountPhrase(int count, const char* kind) -> std::string {
    return std::to_string(count) + " " + kind + (count == 1 ? " error" : " errors");
}

} // namespace kex::diagnostics
