#include "declaration_validator.hxx"

#include "../common/type_def_utils.hxx"
#include "analyzer.hxx"
#include "imported_interfaces.hxx"
#include "traits.hxx"
#include "types.hxx"
#include <algorithm>
#include <unordered_set>

namespace kex::semantic {
namespace {

class DeclarationValidator {
public:
    DeclarationValidator(const ImportedInterfaces* imported,
                         const TraitRegistry& traits,
                         std::vector<Diagnostic>& diagnostics)
        : imported(imported), traits(traits), diagnostics(diagnostics) {}

    auto validate(const ast::Program& program) -> void {
        collect(program.items, "");
        walk(program.items, false);
    }

private:
    const ImportedInterfaces* imported;
    const TraitRegistry& traits;
    std::vector<Diagnostic>& diagnostics;
    std::unordered_set<std::string> declaredTypes;

    template <typename Items>
    auto collect(const Items& items, const std::string& owner) -> void {
        for (const auto& item : items)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if (!node) return;
                if constexpr (std::is_same_v<T,
                                             std::unique_ptr<ast::RecordDef>> ||
                              std::is_same_v<T,
                                             std::unique_ptr<ast::TypeDef>>) {
                    declaredTypes.insert(node->name);
                    if (!owner.empty())
                        declaredTypes.insert(owner + "." + node->name);
                    if constexpr (std::is_same_v<
                                      T, std::unique_ptr<ast::TypeDef>>) {
                        if (const auto constructors = kex::typeConstructors(*node))
                            for (const auto& constructor : *constructors) {
                                declaredTypes.insert(constructor.name);
                                if (!owner.empty())
                                    declaredTypes.insert(owner + "." +
                                                         constructor.name);
                            }
                    }
                } else if constexpr (std::is_same_v<
                                         T, std::unique_ptr<ast::ModuleDef>>) {
                    const auto nested = owner.empty() ||
                                                node->name.rfind(owner + ".", 0) == 0
                        ? node->name
                        : owner + "." + node->name;
                    collect(node->body, nested);
                } else if constexpr (std::is_same_v<
                                         T, std::unique_ptr<ast::MakeDef>>) {
                    for (const auto& name : kex::makeTargetNames(node->target)) {
                        declaredTypes.insert(name);
                        if (!owner.empty())
                            declaredTypes.insert(owner + "." + name);
                    }
                    collect(node->body, owner);
                } else if constexpr (std::is_same_v<
                                         T, std::unique_ptr<ast::VisibilityBlock>> ||
                                     std::is_same_v<
                                         T, std::unique_ptr<ast::CompiledBlock>>) {
                    collect(node->items, owner);
                }
            }, item);
    }

    auto known(const ast::TypeName& name, const auto& generics,
               bool allowThis) const -> bool {
        if (name.parts.empty()) return true;
        const auto& last = name.parts.back();
        std::string qualified;
        for (const auto& part : name.parts) {
            if (!qualified.empty()) qualified += ".";
            qualified += part;
        }
        if ((allowThis && last == "This") || last == "Never" ||
            last == "Void" || last == "Any" || generics.count(last) ||
            (name.parts.size() == 1 && last.size() == 1) ||
            isPrimitiveTypeName(last) || traits.get(last) ||
            declaredTypes.count(qualified) ||
            (name.parts.size() == 1 && declaredTypes.count(last)))
            return true;
        if (!imported) return false;
        if (imported->typeNames.count(qualified) ||
            imported->typeNames.count(last))
            return true;
        return std::any_of(imported->adts.begin(), imported->adts.end(),
                           [&](const auto& adt) {
                               return std::find(adt.constructors.begin(),
                                                adt.constructors.end(), last) !=
                                      adt.constructors.end();
                           });
    }

    auto type(const ast::TypeExpr& expr,
              const std::unordered_set<std::string>& generics = {},
              bool allowThis = false) -> void {
        auto name = [&](const ast::TypeName& value) {
            if (known(value, generics, allowThis)) return;
            std::string spelling;
            for (const auto& part : value.parts) {
                if (!spelling.empty()) spelling += ".";
                spelling += part;
            }
            diagnostics.push_back({Diagnostic::Level::Error, expr.location,
                                   "Unknown type `" + spelling + "`"});
        };
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, ast::TypeName>) {
                name(node);
            } else if constexpr (std::is_same_v<T, ast::GenericType>) {
                name(node.name);
                for (const auto& arg : node.args)
                    if (arg) type(*arg, generics, allowThis);
            } else if constexpr (std::is_same_v<T, ast::FunctionType>) {
                if (node.param) type(*node.param, generics, allowThis);
                if (node.result) type(*node.result, generics, allowThis);
            } else if constexpr (std::is_same_v<T, ast::TupleType>) {
                for (const auto& element : node.elements)
                    if (element) type(*element, generics, allowThis);
            } else if constexpr (std::is_same_v<T, ast::ListType>) {
                if (node.element) type(*node.element, generics, allowThis);
            } else if constexpr (std::is_same_v<T, ast::MapType>) {
                if (node.key) type(*node.key, generics, allowThis);
                if (node.value) type(*node.value, generics, allowThis);
            } else if constexpr (std::is_same_v<T, ast::OptionalType> ||
                                 std::is_same_v<T, ast::BlockType>) {
                if (node.inner) type(*node.inner, generics, allowThis);
            } else if constexpr (std::is_same_v<T, ast::UnionType>) {
                if (node.left) type(*node.left, generics, allowThis);
                if (node.right) type(*node.right, generics, allowThis);
            } else if constexpr (std::is_same_v<T, ast::IntersectionType>) {
                if (node.left) type(*node.left, generics, allowThis);
                if (node.right) type(*node.right, generics, allowThis);
            } else if constexpr (std::is_same_v<T, ast::RecordType>) {
                for (const auto& [_, fieldType] : node.fields)
                    if (fieldType) type(*fieldType, generics, allowThis);
            }
            // AtomType and GenericVar are self-validating. TypeQuery names an
            // expression query, not a source type spelling.
        }, expr.kind);
    }

    auto function(const ast::FunctionDef& def, bool allowThis) -> void {
        if (def.name == "new" && !def.isFoul) {
            diagnostics.push_back({
                Diagnostic::Level::Error, def.location,
                "`new` is reserved for the next-state binding; use a name "
                "such as `build` for constructor functions"
            });
        }
        for (const auto& clause : def.clauses) {
            for (const auto& param : clause.params)
                if (param.type && *param.type)
                    type(**param.type, {}, allowThis);
            if (clause.returnAnnotation && *clause.returnAnnotation)
                type(**clause.returnAnnotation, {}, allowThis);
        }
    }

    auto annotation(const ast::TypeAnnotation& ann, bool allowThis,
                    const std::unordered_set<std::string>& generics = {})
        -> void {
        if (ann.type) type(*ann.type, generics, allowThis);
    }

    auto typeDef(const ast::TypeDef& def) -> void {
        const std::unordered_set<std::string> generics(def.typeParams.begin(),
                                                        def.typeParams.end());
        for (const auto& parent : def.parents) {
            ast::TypeExpr expr;
            expr.location = def.location;
            expr.kind = parent;
            type(expr, generics);
        }
        if (def.variants) {
            const auto constructors = kex::typeConstructors(def);
            for (const auto& variant : *def.variants) {
                if (!variant) continue;
                if (constructors && !constructors->empty()) {
                    if (const auto* ctor =
                            std::get_if<ast::GenericType>(&variant->kind))
                        for (const auto& payload : ctor->args)
                            if (payload) type(*payload, generics);
                } else {
                    type(*variant, generics);
                }
            }
        }
        if (def.abstractFunctions)
            for (const auto& fn : *def.abstractFunctions)
                if (fn.type) type(*fn.type, generics, true);
    }

    auto record(const ast::RecordDef& def) -> void {
        const std::unordered_set<std::string> generics(def.typeParams.begin(),
                                                        def.typeParams.end());
        for (const auto& field : def.fields)
            if (field.type) type(*field.type, generics);
    }

    auto trait(const ast::TraitDef& def) -> void {
        const std::unordered_set<std::string> generics(def.typeParams.begin(),
                                                        def.typeParams.end());
        for (const auto& member : def.body)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if (!node) return;
                if constexpr (std::is_same_v<T,
                                             std::unique_ptr<ast::TypeAnnotation>>)
                    annotation(*node, true, generics);
                else if constexpr (std::is_same_v<
                                       T, std::unique_ptr<ast::FunctionDef>>)
                    function(*node, true);
            }, member);
    }

    template <typename Items>
    auto walk(const Items& items, bool inMake) -> void {
        for (const auto& item : items)
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if (!node) return;
                if constexpr (std::is_same_v<T,
                                             std::unique_ptr<ast::FunctionDef>>)
                    function(*node, inMake);
                else if constexpr (std::is_same_v<
                                       T, std::unique_ptr<ast::TypeAnnotation>>)
                    annotation(*node, inMake);
                else if constexpr (std::is_same_v<
                                       T, std::unique_ptr<ast::RecordDef>>)
                    record(*node);
                else if constexpr (std::is_same_v<
                                       T, std::unique_ptr<ast::TypeDef>>)
                    typeDef(*node);
                else if constexpr (std::is_same_v<
                                       T, std::unique_ptr<ast::TraitDef>>)
                    trait(*node);
                else if constexpr (std::is_same_v<
                                       T, std::unique_ptr<ast::MakeDef>>) {
                    if (node->target) type(*node->target);
                    walk(node->body, true);
                } else if constexpr (std::is_same_v<
                                       T, std::unique_ptr<ast::ModuleDef>>)
                    walk(node->body, inMake);
                else if constexpr (std::is_same_v<
                                       T, std::unique_ptr<ast::VisibilityBlock>> ||
                                   std::is_same_v<
                                       T, std::unique_ptr<ast::CompiledBlock>>)
                    walk(node->items, inMake);
            }, item);
    }
};

} // namespace

auto validateDeclarations(const ast::Program& program,
                          const ImportedInterfaces* imported,
                          const TraitRegistry& traits,
                          std::vector<Diagnostic>& diagnostics) -> void {
    DeclarationValidator(imported, traits, diagnostics).validate(program);
}

} // namespace kex::semantic
