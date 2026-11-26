#include "describe.hxx"

#include "api_help.hxx"

#include "../ast/ast.hxx"
#include "../ast/walk.hxx"
#include "../common/diagnostics.hxx"
#include "../common/mangle.hxx"
#include "../common/name_case.hxx"
#include "../semantic/analyzer.hxx"
#include "../semantic/completion_at.hxx"
#include "../semantic/typechecker.hxx"
#include "../semantic/types.hxx"

#include <algorithm>
#include <ostream>
#include <sstream>

namespace kex::help {

auto writtenName(const ast::Expr& expr) -> std::string {
    if (const auto* identifier = std::get_if<ast::Identifier>(&expr.kind)) {
        return identifier->name;
    }
    if (const auto* upper = std::get_if<ast::UpperIdentifier>(&expr.kind)) {
        return upper->name;
    }
    if (const auto* call = std::get_if<ast::MethodCall>(&expr.kind);
        call && call->receiver && call->args.empty() && call->namedArgs.empty() && !call->block &&
        !call->parenthesized && !call->mutating) {
        if (const auto module = ast::writtenModulePath(*call->receiver)) {
            return kex::mangle::qualify(*module, call->method);
        }
    }
    return {};
}

auto describe(const semantic::Analyzer& analyzer, const ast::Expr& expr) -> Description {
    const auto& checker = analyzer.checker();
    Description description;
    const auto document = [&](std::string owner, std::string member) {
        std::pair entry{std::move(owner), std::move(member)};
        if (std::find(description.documented.begin(), description.documented.end(), entry) ==
            description.documented.end()) {
            description.documented.push_back(std::move(entry));
        }
    };
    const auto name = writtenName(expr);

    // A name some scope binds: the checker records as a use only a name no
    // scope binds (nameUses), so one it did not record is the session's.
    if (std::holds_alternative<ast::Identifier>(expr.kind) && !checker.nameUses().contains(&expr)) {
        description.binding = true;
        if (const auto type = analyzer.displayTypeOf(&expr)) {
            description.typeLines.push_back(name + " : " + semantic::typeToString(type));
        }
        return description;
    }

    if (!name.empty()) {
        if (const auto functions = checker.functionsNamed(name); !functions.empty()) {
            description.function = true;
            for (const auto& function : functions) {
                description.typeLines.push_back(
                    checker.displaySignature(function.name, function.signature));
                document(function.owner, kex::mangle::memberOf(function.name));
            }
            return description;
        }
        if (kex::startsUppercase(kex::mangle::memberOf(name))) {
            description.namesType = true;
            // The module or type as the checker resolved the name here.
            std::string owner = checker.typeIdentity("", name);
            if (const auto decided = checker.decidedCalls().find(&expr);
                decided != checker.decidedCalls().end() &&
                (decided->second.kind == semantic::CallKind::ModuleReference ||
                 decided->second.kind == semantic::CallKind::TypeReference) &&
                !decided->second.owner.empty()) {
                owner = decided->second.owner;
            }
            document(owner, "");
            if (owner != name) {
                document(name, "");
            }
            return description;
        }
    }

    if (const auto type = analyzer.displayTypeOf(&expr)) {
        description.typeLines.push_back(semantic::typeToString(type));
    }
    // A call documents the function the checker decided it runs.
    if (const auto* call = std::get_if<ast::MethodCall>(&expr.kind)) {
        // A function made for the receiver's type (`[1, 2].first`) is
        // documented with that type, as the checker typed the receiver.
        if (call->receiver && !ast::writtenModulePath(*call->receiver)) {
            if (const auto receiver = semantic::completionQualifierForType(
                    analyzer.displayTypeOf(call->receiver.get()));
                !receiver.empty()) {
                document(receiver, call->method);
            }
        }
        if (const auto resolved = checker.resolvedCalls().find(call);
            resolved != checker.resolvedCalls().end() && !resolved->second.sourceModule.empty()) {
            document(resolved->second.sourceModule, call->method);
        } else if (const auto decided = checker.decidedCalls().find(&expr);
                   decided != checker.decidedCalls().end()) {
            if (decided->second.kind == semantic::CallKind::OwnMethod ||
                decided->second.kind == semantic::CallKind::TraitMethod) {
                document(decided->second.owner, call->method);
            } else if (decided->second.kind == semantic::CallKind::ModuleFunction) {
                document(
                    kex::mangle::scopeOf(decided->second.owner),
                    kex::mangle::memberOf(decided->second.owner));
            }
        }
    }
    return description;
}

namespace {

auto firstError(const semantic::Analyzer& analyzer) -> const semantic::Diagnostic* {
    for (const auto& diagnostic : analyzer.diagnostics()) {
        if (diagnostic.level == semantic::Diagnostic::Level::Error) {
            return &diagnostic;
        }
    }
    return nullptr;
}

// `/type`'s lines, unless the checker refused the input: a function name
// written alone, which is not a call, is refused for its missing arguments.
auto typeLinesOf(const Description& description, const semantic::Analyzer& analyzer)
    -> const std::vector<std::string>* {
    const bool resolved = description.function || description.binding;
    if (description.typeLines.empty() || (!resolved && firstError(analyzer))) {
        return nullptr;
    }
    return &description.typeLines;
}

} // namespace

auto printReplType(
    const std::string& input,
    const semantic::Analyzer& analyzer,
    const ast::Expr& expr,
    std::ostream& out,
    std::ostream& err) -> bool {
    const auto description = describe(analyzer, expr);
    if (description.namesType) {
        out << "  " << diagnostics::highlightCodeSpans("`" + input + "`")
            << " is a module or a type, not a value: "
            << diagnostics::highlightCodeSpans("`/help " + input + "`") << " describes it\n";
        return true;
    }
    if (const auto* lines = typeLinesOf(description, analyzer)) {
        for (const auto& line : *lines) {
            out << diagnostics::highlightKex(line) << "\n";
        }
        return true;
    }
    if (const auto* error = firstError(analyzer)) {
        err << "  " << diagnostics::formatBriefDiagnostic(*error) << "\n";
        return false;
    }
    out << "  (the checker knows no type for this)\n";
    return true;
}

auto printReplNameHelp(
    const std::string& input,
    const semantic::Analyzer& analyzer,
    const ast::Expr& expr,
    const std::vector<const ApiIndex*>& indexes,
    std::ostream& out,
    std::ostream& err) -> bool {
    const auto description = describe(analyzer, expr);
    bool said = false;
    if (const auto* lines = typeLinesOf(description, analyzer)) {
        for (const auto& line : *lines) {
            out << diagnostics::highlightKex(line) << "\n";
        }
        said = true;
    }
    for (const auto& [owner, member] : description.documented) {
        for (const auto* index : indexes) {
            std::ostringstream entry;
            if (index->printEntity(owner, member, entry)) {
                out << (said ? "\n" : "") << entry.str();
                said = true;
                break;
            }
        }
    }
    if (said) {
        return true;
    }
    err << "  no help for " << diagnostics::highlightCodeSpans("`" + input + "`");
    if (const auto* error = firstError(analyzer)) {
        err << ": " << diagnostics::formatBriefDiagnostic(*error);
    }
    err << "\n";
    return false;
}

auto standardLibraryIndex() -> const ApiIndex& {
    static const auto index = [] {
        std::ostringstream warnings;
        return ApiIndex::fromStandardLibrary(warnings);
    }();
    return index;
}

} // namespace kex::help
