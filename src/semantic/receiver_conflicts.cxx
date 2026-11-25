#include "../ast/function_groups.hxx"
#include "../ast/type_text.hxx"
#include "receiver_conflicts.hxx"

#include "../common/type_def_utils.hxx"

#include <map>
#include <set>
#include <variant>
#include <string>
#include <vector>

namespace kex::semantic {

namespace {

struct Site {
    std::string group; // definitions sharing this are clauses of one method
    SourceLocation location;
    std::string shape = {}; // overloadShape: "" competes with every definition
};

// key: receiver type, method name, UFCS arity (receiver included)
using Key = std::tuple<std::string, std::string, size_t>;

// What tells two definitions of one name on one receiver apart: the make
// target's type arguments, when every one is a concrete type, and the
// parameter types, when every parameter writes one. `*` on `Measure<Mass>`
// and `*` on `Measure<Acceleration>`, or `*(other: Measure<Time>)` beside
// `*(other: Measure<Voltage>)`, are overloads of one name, each answering
// its own types (docs/typechecker-hm-plan.md §8.1, fix 1). A generic target
// (`Measure<D>`) or an untyped parameter may take what another definition
// takes, so it gives no shape and still competes as before.
auto overloadShape(const ast::TypeExpr* target, const ast::FunctionClause& clause) -> std::string {
    std::string shape;
    if (target) {
        if (const auto* generic = std::get_if<ast::GenericType>(&target->kind)) {
            for (const auto& arg : generic->args) {
                const auto* name = arg ? std::get_if<ast::TypeName>(&arg->kind) : nullptr;
                if (!name || (name->parts.size() == 1 && name->parts[0].size() == 1)) {
                    return {}; // a type variable: competes with every instantiation
                }
            }
            shape = ast::typeText(*target);
        }
    }
    std::string params;
    for (const auto& param : clause.params) {
        if (!param.type || !*param.type) {
            return shape; // untyped: takes what another overload may take
        }
        params += (params.empty() ? "" : ", ") + ast::typeText(**param.type);
    }
    return shape + "(" + params + ")";
}

// The type this expression names, or "" — only a plain name counts, since a
// structural type cannot be the receiver of a `make` block either.
auto typeNameOf(const ast::TypeExprPtr& type) -> std::string {
    if (!type) {
        return {};
    }
    if (const auto* named = std::get_if<ast::TypeName>(&type->kind)) {
        return named->parts.empty() ? std::string{} : named->parts.back();
    }
    return {};
}

// A make-block method reaches its receiver implicitly through `this`, or as a
// first parameter that MATCHES it (`let head(@[x | _])`, `let rangeStart(x.._)`).
// Only the implicit form needs the receiver added to the arity — the same rule
// the type checker applies (receiverIsFirstParam).
auto receiverIsFirstParam(const ast::FunctionDef& def) -> bool {
    if (def.clauses.empty() || def.clauses[0].params.empty()) {
        return false;
    }
    const auto& first = def.clauses[0].params[0];
    if (!first.pattern) {
        return false;
    }
    const auto& kind = (*first.pattern)->kind;
    if (std::holds_alternative<ast::ListPattern>(kind) ||
        std::holds_alternative<ast::TuplePattern>(kind)) {
        return false;
    }
    return !std::holds_alternative<ast::VarPattern>(kind) &&
           !std::holds_alternative<ast::ConstructorPattern>(kind);
}

auto paramCount(const ast::FunctionDef& def) -> size_t {
    return def.clauses.empty() ? 0 : def.clauses[0].params.size();
}

class Collector {
public:
    explicit Collector(const TypeChecker& checker) : m_checker(checker) {}
    auto sites() -> std::map<Key, std::vector<Site>>& {
        return m_sites;
    }

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
        for (const auto& item : items) {
            if (const auto* annotation = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
                if (*annotation && (*annotation)->type) {
                    if (const auto* fn =
                            std::get_if<ast::FunctionType>(&(*annotation)->type->kind)) {
                        annotatedReceiver[(*annotation)->name] = typeNameOf(fn->param);
                    }
                }
            }
        }

        for (const auto& item : items) {
            if (const auto* make = std::get_if<std::unique_ptr<ast::MakeDef>>(&item)) {
                if (*make) {
                    collectMake(**make);
                }
            } else if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                if (*fn && topLevel) {
                    collectFreeFunction(**fn, annotatedReceiver);
                }
            } else if (const auto* module = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                if (*module) {
                    walk((*module)->body, false);
                }
            }
        }
    }

private:
    void collectMake(const ast::MakeDef& make) {
        const auto group = "make:" + std::to_string(reinterpret_cast<std::uintptr_t>(&make));
        const auto found = m_checker.makeTargetIdentities().find(&make);
        if (found == m_checker.makeTargetIdentities().end()) {
            return;
        }
        for (const auto& target : found->second) {
            auto method = [&](const ast::FunctionDef& def) {
                const auto arity = paramCount(def) + (receiverIsFirstParam(def) ? 0u : 1u);
                const auto shape = def.clauses.empty()
                                       ? std::string{}
                                       : overloadShape(make.target.get(), def.clauses.front());
                m_sites[{target, def.name, arity}].push_back({group, def.location, shape});
            };
            for (const auto& item : make.body) {
                if (const auto* fn = std::get_if<std::unique_ptr<ast::FunctionDef>>(&item)) {
                    if (*fn) {
                        method(**fn);
                    }
                } else if (
                    const auto* visibility =
                        std::get_if<std::unique_ptr<ast::VisibilityBlock>>(&item)) {
                    if (!*visibility) {
                        continue;
                    }
                    for (const auto& inner : (*visibility)->items) {
                        if (const auto* fn =
                                std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner)) {
                            if (*fn) {
                                method(**fn);
                            }
                        }
                    }
                }
            }
        }
    }

    void collectFreeFunction(
        const ast::FunctionDef& def, const std::map<std::string, std::string>& annotatedReceiver) {
        // Each typed overload folded into the function names its own
        // receiver (`close(connection: TCPConnection)`, `close(listener:
        // TCPListener)`): a declared type by the identity the checker gave
        // its parameter, anything else as written.
        const auto* signatures = m_checker.functionSignatures(&def);
        for (const auto* clause : ast::signatureClauses(def)) {
            if (clause->params.empty()) {
                continue;
            }
            std::string receiver;
            if (signatures) {
                for (const auto& signature : *signatures) {
                    if (signature.clause == clause && !signature.params.empty()) {
                        if (const auto* named =
                                std::get_if<NamedType>(&signature.params[0]->kind)) {
                            receiver = named->name;
                        }
                    }
                }
            }
            const auto& declared = clause->params[0].type;
            if (receiver.empty() && declared) {
                receiver = typeNameOf(*declared);
            }
            if (receiver.empty()) {
                if (auto annotated = annotatedReceiver.find(def.name);
                    annotated != annotatedReceiver.end()) {
                    receiver = annotated->second;
                }
            }
            if (receiver.empty()) {
                continue;
            }
            m_sites[{receiver, def.name, clause->params.size()}].push_back(
                {"free:" + def.name, clause->location, overloadShape(nullptr, *clause)});
        }
    }

    const TypeChecker& m_checker;
    std::map<Key, std::vector<Site>> m_sites;
};

} // namespace

auto findReceiverConflicts(const ast::Program& program, const TypeChecker& checker)
    -> std::vector<Diagnostic> {
    Collector collector(checker);
    collector.walk(program.items);

    std::vector<Diagnostic> diagnostics;
    for (const auto& [key, sites] : collector.sites()) {
        const auto& [type, method, arity] = key;
        (void)arity;
        // Any two definitions that could answer the same call: not clauses of
        // one method, in one file, and not told apart by their shapes.
        const auto compete = [](const Site& a, const Site& b) {
            return a.group != b.group && a.location.file == b.location.file &&
                   (a.shape.empty() || b.shape.empty() || a.shape == b.shape);
        };
        bool reported = false;
        for (size_t i = 1; i < sites.size() && !reported; i++) {
            for (size_t j = 0; j < i && !reported; j++) {
                if (!compete(sites[j], sites[i])) {
                    continue;
                }
                auto diagnostic = makeDiagnostic(
                    Diagnostic::Level::Error, diag::Code::AmbiguousReceiverDefinition,
                    sites[i].location, {{"method", method}, {"type", type}});
                diagnostic.notes.push_back({sites[j].location, "the other definition is here"});
                diagnostics.push_back(std::move(diagnostic));
                reported = true; // one report per (type, method, arity)
            }
        }
    }
    return diagnostics;
}

} // namespace kex::semantic
