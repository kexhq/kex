#include "declaration_validator.hxx"

#include "../common/runtime_dir.hxx"
#include "../common/type_def_utils.hxx"
#include "../common/well_known.hxx"
#include "analyzer.hxx"
#include "imported_interfaces.hxx"
#include "traits.hxx"
#include "types.hxx"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace kex::semantic {
namespace {

class DeclarationValidator {
public:
    DeclarationValidator(const TraitRegistry& traits, std::vector<Diagnostic>& diagnostics)
        : traits(traits), diagnostics(diagnostics) {}

    auto validate(const ast::Program& program) -> void {
        collect(program.items);
        walk(program.items);
        reportUndefinedMakeContracts();
    }

private:
    const TraitRegistry& traits;
    std::vector<Diagnostic>& diagnostics;
    // Every method name DEFINED for a `make` target, gathered across the whole
    // program: one type's methods may be split over several `make` blocks, so
    // a contract is only unimplemented if no block for that target defines it.
    std::unordered_map<std::string, std::unordered_set<std::string>> makeDefinitions;
    // Fields the target's `record` declares, keyed the same way.
    std::unordered_map<std::string, std::unordered_set<std::string>> recordFieldNames;
    struct MakeContract {
        std::string target;
        std::string name;
        SourceLocation location;
        // `a : String` rather than `a :> String` — no receiver arrow. The
        // spelling someone reaches for when they mean a record FIELD.
        bool plainColon;
        // …and its type is not a function either, so a field is exactly what
        // it looks like.
        bool fieldTyped;
        std::vector<std::string> implementedTraits;
    };
    std::vector<MakeContract> makeContracts;

    template <typename Items>
    auto collect(const Items& items) -> void {
        for (const auto& item : items) {
            std::visit(
                [&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if (!node) {
                        return;
                    }
                    if constexpr (
                        std::is_same_v<T, std::unique_ptr<ast::RecordDef>> ||
                        std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
                        if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
                            for (const auto& field : node->fields) {
                                recordFieldNames[node->name].insert(field.name);
                            }
                        }
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        collect(node->body);
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
                        gatherMakeContracts(*node);
                        collect(node->body);
                    } else if constexpr (
                        std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>> ||
                        std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
                        collect(node->items);
                    }
                },
                item);
        }
    }

    // A `make` body holds METHODS and their contracts — `record` is where
    // fields are declared (grammar.ebnf's `make_item`). A contract nothing
    // defines is therefore a promise the type does not keep, and the
    // field-shaped spelling is the mistake that produces one by accident:
    // `make T do a: String end` ran on the walker (which answered the read
    // from the record literal) and emitted a tuple with no layout on BEAM,
    // where dispatch then failed with `undef`. Neither backend said anything.
    auto gatherMakeContracts(const ast::MakeDef& def) -> void {
        const auto targets = kex::makeTargetNames(def.target);
        if (targets.empty()) {
            return;
        }
        // A `make A | B` block defines its methods for every target it names.
        auto define = [&](const std::string& method) {
            for (const auto& target : targets) {
                makeDefinitions[target].insert(method);
            }
        };
        for (const auto& item : def.body) {
            std::visit(
                [&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if (!node) {
                        return;
                    }
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        define(node->name);
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>>) {
                        for (const auto& inner : node->items) {
                            if (const auto* fn =
                                    std::get_if<std::unique_ptr<ast::FunctionDef>>(&inner);
                                fn && *fn) {
                                define((*fn)->name);
                            }
                        }
                    }
                },
                item);
        }
        for (const auto& item : def.body) {
            const auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item);
            if (!ann || !*ann) {
                continue;
            }
            const bool plainColon = !(*ann)->implicitThis && !(*ann)->implicitFrom;
            const bool fieldTyped =
                (*ann)->type && !std::holds_alternative<ast::FunctionType>((*ann)->type->kind);
            makeContracts.push_back(
                {targets.front(), (*ann)->name, (*ann)->location, plainColon, fieldTyped,
                 def.implements});
        }
    }

    auto reportUndefinedMakeContracts() -> void {
        for (const auto& contract : makeContracts) {
            auto defined = makeDefinitions.find(contract.target);
            if (defined != makeDefinitions.end() && defined->second.count(contract.name)) {
                continue;
            }
            // An implemented trait may supply the body, in which case the
            // block declares what it inherits rather than what it owes.
            const bool fromTrait = std::any_of(
                contract.implementedTraits.begin(), contract.implementedTraits.end(),
                [&](const std::string& name) {
                    const auto* definition = traits.lookup(name);
                    return definition &&
                           std::any_of(
                               definition->requiredMethods.begin(),
                               definition->requiredMethods.end(), [&](const auto& required) {
                                   return required.name == contract.name;
                               });
                });
            if (fromTrait) {
                continue;
            }
            // A `:>` contract with no body in sight is the stdlib's normal
            // way of declaring a method the RUNTIME provides — `collect :>
            // (X -> Y?) -> [Y]` on `make List` has no Kex definition and
            // never will. Nothing provides one anywhere else: `shout :>
            // String -> String` in a program's own `make User` checked clean
            // and was undefined at run time (kexhq/kex#795).
            if (!contract.plainColon) {
                if (!isStandardLibrarySource(contract.location.file)) {
                    diagnostics.push_back(makeDiagnostic(
                        Diagnostic::Level::Error, diag::Code::MakeSignatureUndefined,
                        contract.location, {{"name", contract.name}}));
                }
                continue;
            }
            auto fields = recordFieldNames.find(contract.target);
            const auto code =
                !contract.fieldTyped ? diag::Code::MakeContractUndefined
                : fields != recordFieldNames.end() && fields->second.count(contract.name)
                    ? diag::Code::MakeContractRepeatsField
                    : diag::Code::MakeContractFieldTyped;
            diag::Args args = {{"name", contract.name}};
            if (code != diag::Code::MakeContractUndefined) {
                args.emplace_back("record", contract.target);
            }
            diagnostics.push_back(
                makeDiagnostic(Diagnostic::Level::Error, code, contract.location, std::move(args)));
        }
    }

    // `new` is the next-state binding, at any depth: a function, a `make`
    // method or a trait's default.
    auto function(const ast::FunctionDef& def) -> void {
        if (def.name == "new" && !def.isFoul) {
            diagnostics.push_back(
                makeDiagnostic(Diagnostic::Level::Error, diag::Code::NewIsReserved, def.location));
        }
    }

    template <typename Items>
    auto walk(const Items& items) -> void {
        for (const auto& item : items) {
            std::visit(
                [&](const auto& node) {
                    using T = std::decay_t<decltype(node)>;
                    if (!node) {
                        return;
                    }
                    if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                        function(*node);
                    } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
                        walk(node->body);
                    } else if constexpr (
                        std::is_same_v<T, std::unique_ptr<ast::MakeDef>> ||
                        std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
                        walk(node->body);
                    } else if constexpr (
                        std::is_same_v<T, std::unique_ptr<ast::VisibilityBlock>> ||
                        std::is_same_v<T, std::unique_ptr<ast::CompiledBlock>>) {
                        walk(node->items);
                    }
                },
                item);
        }
    }
};

} // namespace

auto validateDeclarations(
    const ast::Program& program, const TraitRegistry& traits, std::vector<Diagnostic>& diagnostics)
    -> void {
    DeclarationValidator(traits, diagnostics).validate(program);
}

} // namespace kex::semantic
