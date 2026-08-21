#pragma once

#include "ast.hxx"
#include <string>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kex::ast {

inline auto extractDocComments(const std::string& source)
    -> std::unordered_map<int, std::string> {
    std::unordered_map<int, std::string> result;
    std::istringstream stream(source);
    std::string line;
    std::string accumulated;
    int lineNumber = 0;
    bool inBlock = false;

    while (std::getline(stream, line)) {
        ++lineNumber;
        auto first = line.find_first_not_of(" \t");
        auto trimmed = first == std::string::npos ? "" : line.substr(first);
        if (!trimmed.empty() && trimmed[0] == '#') {
            auto content = trimmed.size() > 1 && trimmed[1] == ' '
                ? trimmed.substr(2) : trimmed.substr(1);
            if (inBlock)
                accumulated += "\n";
            accumulated += content;
            inBlock = true;
        } else if (trimmed.empty() && inBlock) {
            accumulated += "\n";
        } else {
            if (inBlock && !accumulated.empty()) {
                while (!accumulated.empty() && accumulated.back() == '\n')
                    accumulated.pop_back();
                result[lineNumber] = accumulated;
                accumulated.clear();
            }
            inBlock = false;
        }
    }
    return result;
}

// Schema conversion is parameterized by a small value builder so the AST is
// walked once regardless of whether its destination is an interpreter value
// or an Erlang term. Keep policy (tags, record names, field order) here; sinks
// only decide how those schema operations are represented.
template<typename Builder>
class Converter {
public:
    using Value = typename Builder::Value;
    using Docs = std::unordered_map<int, std::string>;

    explicit Converter(Builder& builder) : m_builder(builder) {}

    auto location(SourceLocation loc, const std::string& filename) -> Value {
        return m_builder.record("Location", {
            {"file", m_builder.string(filename)},
            {"line", m_builder.integer(loc.line)},
            {"column", m_builder.integer(loc.column)},
            {"startOffset", m_builder.integer(loc.startOffset)},
            {"endOffset", m_builder.integer(loc.endOffset)},
        });
    }

    auto param(const Param& param) -> Value {
        return m_builder.record("ParamInfo", {
            {"name", param.name ? m_builder.just(m_builder.string(*param.name))
                                : m_builder.none()},
            {"pattern", param.pattern
                ? m_builder.just(patternRef(**param.pattern))
                : m_builder.none()},
            {"type", param.type ? m_builder.just(typeRef(**param.type))
                                : m_builder.none()},
            {"hasDefault", m_builder.boolean(param.defaultValue.has_value())},
        });
    }

    auto clause(const FunctionClause& clause) -> Value {
        std::vector<Value> params;
        for (const auto& param : clause.params)
            params.push_back(this->param(param));
        return m_builder.record("ClauseInfo", {
            {"params", m_builder.list(std::move(params))},
            {"body", expressionList(clause.body)},
            {"returnType", clause.returnAnnotation
                ? m_builder.just(typeRef(**clause.returnAnnotation))
                : m_builder.none()},
        });
    }

    auto functionDef(const FunctionDef& def, const std::string& doc,
                     const std::string& filename) -> Value {
        std::vector<Value> clauses;
        for (const auto& clause : def.clauses)
            clauses.push_back(this->clause(clause));
        auto info = m_builder.record("FunctionInfo", {
            {"name", m_builder.string(def.name)},
            {"doc", optionalString(doc)},
            {"isFoul", m_builder.boolean(def.isFoul)},
            {"predicate", m_builder.boolean(def.isPredicate)},
            {"clauses", m_builder.list(std::move(clauses))},
            {"location", location(def.location, filename)},
        });
        return nodeVariant("FunctionDef", {std::move(info)});
    }

    auto typeAnnotation(const TypeAnnotation& annotation,
                        const std::string& doc) -> Value {
        auto info = m_builder.record("AnnotationInfo", {
            {"name", m_builder.string(annotation.name)},
            {"type", typeRef(*annotation.type)},
            {"doc", optionalString(doc)},
            {"implicitThis", m_builder.boolean(annotation.implicitThis)},
        });
        return nodeVariant("TypeAnnotation", {std::move(info)});
    }

    auto typeDef(const TypeDef& def, const std::string& doc,
                 const std::string& filename) -> Value {
        std::vector<Value> typeParams;
        for (const auto& typeParam : def.typeParams)
            typeParams.push_back(m_builder.string(typeParam));

        std::vector<Value> parents;
        for (const auto& parent : def.parents)
            parents.push_back(namedType(joinName(parent), {}));

        Value variants = m_builder.none();
        if (def.variants) {
            std::vector<Value> converted;
            for (const auto& variant : *def.variants)
                converted.push_back(typeVariant(*variant));
            variants = m_builder.just(m_builder.list(std::move(converted)));
        }

        auto info = m_builder.record("TypeInfo", {
            {"name", m_builder.string(def.name)},
            {"doc", optionalString(doc)},
            {"typeParams", m_builder.list(std::move(typeParams))},
            {"parents", m_builder.list(std::move(parents))},
            {"variants", std::move(variants)},
            {"location", location(def.location, filename)},
        });
        return nodeVariant("TypeDef", {std::move(info)});
    }

    auto recordDef(const RecordDef& def, const std::string& doc,
                   const std::string& filename) -> Value {
        std::vector<Value> typeParams;
        for (const auto& typeParam : def.typeParams)
            typeParams.push_back(m_builder.string(typeParam));

        std::vector<Value> fields;
        for (const auto& field : def.fields) {
            fields.push_back(m_builder.record("FieldInfo", {
                {"name", m_builder.string(field.name)},
                {"type", typeRef(*field.type)},
                {"hasDefault",
                 m_builder.boolean(field.defaultValue.has_value())},
            }));
        }

        auto info = m_builder.record("RecordInfo", {
            {"name", m_builder.string(def.name)},
            {"doc", optionalString(doc)},
            {"typeParams", m_builder.list(std::move(typeParams))},
            {"fields", m_builder.list(std::move(fields))},
            {"location", location(def.location, filename)},
        });
        return nodeVariant("RecordDef", {std::move(info)});
    }

    auto traitDef(const TraitDef& def, const std::string& doc,
                  const std::string& filename) -> Value {
        std::vector<Value> typeParams;
        for (const auto& typeParam : def.typeParams)
            typeParams.push_back(m_builder.string(typeParam));

        std::vector<Value> body;
        for (const auto& item : def.body) {
            std::visit([&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T,
                              std::unique_ptr<TypeAnnotation>>) {
                    body.push_back(typeAnnotation(*node, ""));
                } else {
                    body.push_back(functionDef(*node, "", filename));
                }
            }, item);
        }

        auto info = m_builder.record("TraitInfo", {
            {"name", m_builder.string(def.name)},
            {"doc", optionalString(doc)},
            {"typeParams", m_builder.list(std::move(typeParams))},
            {"body", m_builder.list(std::move(body))},
            {"location", location(def.location, filename)},
        });
        return nodeVariant("TraitDef", {std::move(info)});
    }

    auto makeDef(const MakeDef& def, const std::string& filename,
                 const Docs& docs) -> Value {
        std::vector<Value> implements;
        for (const auto& name : def.implements)
            implements.push_back(namedType(name, {}));

        std::vector<Value> body;
        for (const auto& item : def.body)
            appendMakeItem(body, item, filename, docs);

        auto info = m_builder.record("MakeInfo", {
            {"target", typeRef(*def.target)},
            {"isFinal", m_builder.boolean(def.isFinal)},
            {"implements", m_builder.list(std::move(implements))},
            {"body", m_builder.list(std::move(body))},
            {"location", location(def.location, filename)},
        });
        return nodeVariant("MakeDef", {std::move(info)});
    }

    auto pragma(const Pragma& pragma, const std::string& filename) -> Value {
        auto info = m_builder.record("PragmaInfo", {
            {"name", m_builder.string(
                pragma.requirements.empty() ? "" : pragma.requirements[0])},
            {"value", pragma.requirements.size() > 1
                ? m_builder.just(m_builder.string(pragma.requirements[1]))
                : m_builder.none()},
            {"location", location(pragma.location, filename)},
        });
        return nodeVariant("PragmaDef", {std::move(info)});
    }

    auto moduleDef(const ModuleDef& module, const std::string& doc,
                   const std::string& filename, const Docs& docs) -> Value {
        std::vector<Value> items;
        for (const auto& item : module.body) {
            auto converted = moduleItem(item, filename, docs);
            if (converted)
                items.push_back(std::move(converted));
        }
        auto info = m_builder.record("ModuleInfo", {
            {"name", m_builder.string(module.name)},
            {"doc", optionalString(doc)},
            {"items", m_builder.list(std::move(items))},
            {"location", location(module.location, filename)},
        });
        return nodeVariant("ModuleDef", {std::move(info)});
    }

    auto usingBlock(const UsingBlock& block, const std::string& filename)
        -> Value {
        auto info = m_builder.record("UsingInfo", {
            {"moduleName", m_builder.string(joinName(block.module))},
            {"alias", block.alias
                ? m_builder.just(m_builder.string(*block.alias))
                : m_builder.none()},
            {"onlyNames", stringList(block.onlyNames)},
            {"exceptNames", stringList(block.exceptNames)},
            {"body", expressionList(block.body)},
            {"location", location(block.location, filename)},
        });
        return nodeVariant("UsingDef", {std::move(info)});
    }

    auto exportDef(const ExportDecl& decl, const std::string& filename)
        -> Value {
        auto info = m_builder.record("ExportInfo", {
            {"moduleName", m_builder.string(joinName(decl.module))},
            {"alias", decl.alias
                ? m_builder.just(m_builder.string(*decl.alias))
                : m_builder.none()},
            {"onlyNames", stringList(decl.onlyNames)},
            {"exceptNames", stringList(decl.exceptNames)},
            {"location", location(decl.location, filename)},
        });
        return nodeVariant("ExportDef", {std::move(info)});
    }

    auto visibilityBlock(const VisibilityBlock& block,
                         const std::string& filename, const Docs& docs)
        -> Value {
        std::vector<Value> items;
        for (const auto& item : block.items) {
            auto converted = std::visit([&](const auto& node) -> Value {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T,
                                             std::unique_ptr<FunctionDef>>)
                    return functionDef(*node, docAt(docs, node->location.line),
                                       filename);
                else if constexpr (std::is_same_v<T,
                                                  std::unique_ptr<TypeAnnotation>>)
                    return typeAnnotation(*node, "");
                else if constexpr (std::is_same_v<T,
                                                  std::unique_ptr<MakeDef>>)
                    return makeDef(*node, filename, docs);
                else if constexpr (std::is_same_v<T,
                                                  std::unique_ptr<TypeDef>>)
                    return typeDef(*node, docAt(docs, node->location.line),
                                   filename);
                else if constexpr (std::is_same_v<T,
                                                  std::unique_ptr<RecordDef>>)
                    return recordDef(*node, docAt(docs, node->location.line),
                                     filename);
                else
                    return usingBlock(*node, filename);
            }, item);
            if (converted)
                items.push_back(std::move(converted));
        }
        auto info = m_builder.record("VisibilityInfo", {
            {"isPublic", m_builder.boolean(block.isPublic)},
            {"items", m_builder.list(std::move(items))},
            {"location", location(block.location, filename)},
        });
        return nodeVariant("Visibility", {std::move(info)});
    }

    auto compiledBlock(const CompiledBlock& block, const std::string& filename,
                       const Docs& docs) -> Value {
        std::vector<Value> items;
        for (const auto& item : block.items) {
            items.push_back(std::visit([&](const auto& node) -> Value {
                using T = std::decay_t<decltype(node)>;
                Value converted;
                if constexpr (std::is_same_v<T,
                                             std::unique_ptr<FunctionDef>>)
                    converted = functionDef(
                        *node, docAt(docs, node->location.line), filename);
                else if constexpr (std::is_same_v<T,
                                                  std::unique_ptr<MakeDef>>)
                    converted = makeDef(*node, filename, docs);
                else if constexpr (std::is_same_v<T,
                                                  std::unique_ptr<RecordDef>>)
                    converted = recordDef(
                        *node, docAt(docs, node->location.line), filename);
                else if constexpr (std::is_same_v<T,
                                                  std::unique_ptr<TypeDef>>)
                    converted = typeDef(
                        *node, docAt(docs, node->location.line), filename);
                else
                    return m_builder.variant(
                        "CompiledExpression", "Kex.AST.CompiledItem",
                        {expressionPtr(node)});
                return m_builder.variant(
                    "CompiledNode", "Kex.AST.CompiledItem",
                    {std::move(converted)});
            }, item));
        }
        auto info = m_builder.record("CompiledInfo", {
            {"items", m_builder.list(std::move(items))},
            {"location", location(block.location, filename)},
        });
        return nodeVariant("Compiled", {std::move(info)});
    }

    auto moduleItem(const ModuleItem& item, const std::string& filename,
                    const Docs& docs) -> Value {
        return std::visit([&](const auto& node) -> Value {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ModuleDef>>) {
                return moduleDef(*node, docAt(docs, node->location.line),
                                 filename, docs);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<FunctionDef>>) {
                return functionDef(*node, docAt(docs, node->location.line),
                                   filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<TypeDef>>) {
                return typeDef(*node, docAt(docs, node->location.line),
                               filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<RecordDef>>) {
                return recordDef(*node, docAt(docs, node->location.line),
                                 filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<TraitDef>>) {
                return traitDef(*node, docAt(docs, node->location.line),
                                filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<MakeDef>>) {
                return makeDef(*node, filename, docs);
            } else if constexpr (std::is_same_v<
                                     T, std::unique_ptr<TypeAnnotation>>) {
                return typeAnnotation(*node, "");
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<CompiledBlock>>) {
                return compiledBlock(*node, filename, docs);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<VisibilityBlock>>) {
                return visibilityBlock(*node, filename, docs);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<UsingBlock>>) {
                return usingBlock(*node, filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<ExportDecl>>) {
                return exportDef(*node, filename);
            } else {
                return {};
            }
        }, item);
    }

    auto topLevelItem(const TopLevelItem& item, const std::string& filename,
                      const Docs& docs) -> Value {
        return std::visit([&](const auto& node) -> Value {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ModuleDef>>) {
                return moduleDef(*node, docAt(docs, node->location.line),
                                 filename, docs);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<FunctionDef>>) {
                return functionDef(*node, docAt(docs, node->location.line),
                                   filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<TypeDef>>) {
                return typeDef(*node, docAt(docs, node->location.line),
                               filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<RecordDef>>) {
                return recordDef(*node, docAt(docs, node->location.line),
                                 filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<TraitDef>>) {
                return traitDef(*node, docAt(docs, node->location.line),
                                filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<MakeDef>>) {
                return makeDef(*node, filename, docs);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<Pragma>>) {
                return pragma(*node, filename);
            } else if constexpr (std::is_same_v<
                                     T, std::unique_ptr<TypeAnnotation>>) {
                return typeAnnotation(*node, "");
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<CompiledBlock>>) {
                return compiledBlock(*node, filename, docs);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<UsingBlock>>) {
                return usingBlock(*node, filename);
            } else if constexpr (std::is_same_v<T,
                                                 std::unique_ptr<MainBlock>>) {
                return mainBlock(*node, filename, docs);
            } else {
                return {};
            }
        }, item);
    }

    auto program(const Program& program, const std::string& filename,
                 const Docs& docs) -> Value {
        std::vector<Value> items;
        for (const auto& item : program.items) {
            auto converted = topLevelItem(item, filename, docs);
            if (converted)
                items.push_back(std::move(converted));
        }
        return m_builder.record("Program", {
            {"schemaVersion", m_builder.integer(int64_t{1})},
            {"items", m_builder.list(std::move(items))},
        });
    }

    auto typeRef(const TypeExpr& expr) -> Value {
        return std::visit([&](const auto& node) -> Value {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeName>) {
                return namedType(joinName(node), {});
            } else if constexpr (std::is_same_v<T, GenericType>) {
                std::vector<Value> args;
                for (const auto& arg : node.args)
                    args.push_back(typeRef(*arg));
                return namedType(joinName(node.name), std::move(args));
            } else if constexpr (std::is_same_v<T, FunctionType>) {
                return variant("FunctionType", {m_builder.list({typeRef(*node.param)}),
                                                typeRef(*node.result)});
            } else if constexpr (std::is_same_v<T, TupleType>) {
                return variant("TupleType", {typeList(node.elements)});
            } else if constexpr (std::is_same_v<T, ListType>) {
                return variant("ListType", {typeRef(*node.element)});
            } else if constexpr (std::is_same_v<T, MapType>) {
                return variant("MapType", {typeRef(*node.key), typeRef(*node.value)});
            } else if constexpr (std::is_same_v<T, UnionType>) {
                return variant("UnionType", {m_builder.list(
                    {typeRef(*node.left), typeRef(*node.right)})});
            } else if constexpr (std::is_same_v<T, OptionalType>) {
                return variant("NullableType", {typeRef(*node.inner)});
            } else if constexpr (std::is_same_v<T, BlockType>) {
                return variant("BlockType", {typeRef(*node.inner)});
            } else if constexpr (std::is_same_v<T, AtomType>) {
                return variant("AtomType", {m_builder.string(node.name)});
            } else if constexpr (std::is_same_v<T, GenericVar>) {
                return variant("TypeVar", {m_builder.string(node.name)});
            } else if constexpr (std::is_same_v<T, TypeQuery>) {
                return variant("TypeQuery", {
                    m_builder.string(node.query), expressionPtr(node.argument),
                });
            }
        }, expr.kind);
    }

    auto patternRef(const Pattern& pattern) -> Value {
        return std::visit([&](const auto& node) -> Value {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, VarPattern>) {
                return patternVariant("BindPattern", {m_builder.string(node.name)});
            } else if constexpr (std::is_same_v<T, LiteralPattern>) {
                return patternVariant("LiteralPattern",
                                      {m_builder.string(node.literal.value)});
            } else if constexpr (std::is_same_v<T, ConstructorPattern>) {
                return patternVariant("ConstructorPattern",
                    {m_builder.string(node.name), patternList(node.args)});
            } else if constexpr (std::is_same_v<T, TuplePattern>) {
                return patternVariant("TuplePattern", {patternList(node.elements)});
            } else if constexpr (std::is_same_v<T, ListPattern>) {
                return patternVariant("ListPattern", {
                    patternList(node.elements), optionalPattern(node.rest),
                });
            } else if constexpr (std::is_same_v<T, WildcardPattern>) {
                return patternVariant("WildcardPattern", {});
            } else if constexpr (std::is_same_v<T, RecordPattern>) {
                std::vector<Value> fields;
                for (const auto& field : node.fields) {
                    fields.push_back(m_builder.record("PatternField", {
                        {"name", m_builder.string(field.name)},
                        {"pattern", optionalPattern(field.pattern)},
                        {"stringKey", m_builder.boolean(field.isStringKey)},
                    }));
                }
                return patternVariant("RecordPattern", {
                    node.typeName.empty()
                        ? m_builder.none()
                        : m_builder.just(m_builder.string(node.typeName)),
                    m_builder.list(std::move(fields)),
                });
            } else if constexpr (std::is_same_v<T, RangePattern>) {
                return patternVariant("RangePattern", {
                    patternRef(*node.start), patternRef(*node.end),
                });
            } else if constexpr (std::is_same_v<T, ThisPattern>) {
                return patternVariant("ThisPattern", {patternRef(*node.inner)});
            } else {
                return patternVariant("WildcardPattern", {});
            }
        }, pattern.kind);
    }

    auto expression(const Expr& expr) -> Value {
        return std::visit([&](const auto& node) -> Value {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, IntLiteral>) {
                return expressionVariant(
                    "LitInt", {m_builder.integer(mpz_class(node.value))});
            } else if constexpr (std::is_same_v<T, FloatLiteral>) {
                return expressionVariant(
                    "LitFloat", {m_builder.floating(std::stod(node.value))});
            } else if constexpr (std::is_same_v<T, StringLiteral>) {
                if (node.values.empty()) {
                    const auto& value = node.parts.empty()
                        ? node.value : node.parts.front();
                    return expressionVariant(
                        "LitString", {m_builder.string(value)});
                }
                std::vector<Value> parts;
                for (const auto& part : node.parts)
                    parts.push_back(m_builder.string(part));
                return expressionVariant("InterpolatedString", {
                    m_builder.list(std::move(parts)), expressionList(node.values),
                });
            } else if constexpr (std::is_same_v<T, CharLiteral>) {
                return expressionVariant(
                    "LitChar", {m_builder.integer(int64_t{node.value})});
            } else if constexpr (std::is_same_v<T, BoolLiteral>) {
                return expressionVariant(
                    "LitBool", {m_builder.boolean(node.value)});
            } else if constexpr (std::is_same_v<T, AtomLiteral>) {
                return expressionVariant(
                    "LitAtom", {m_builder.string(node.name)});
            } else if constexpr (std::is_same_v<T, NoneLiteral>) {
                return expressionVariant("LitNone", {});
            } else if constexpr (std::is_same_v<T, Identifier>) {
                return expressionVariant(
                    "Identifier", {m_builder.string(node.name)});
            } else if constexpr (std::is_same_v<T, UpperIdentifier>) {
                return expressionVariant(
                    "Identifier", {m_builder.string(node.name)});
            } else if constexpr (std::is_same_v<T, ThisExpr>) {
                return expressionVariant("This", {});
            } else if constexpr (std::is_same_v<T, FunctionCall>) {
                auto target = expressionVariant(
                    "Identifier", {m_builder.string(node.name)});
                return expressionVariant("Call", {
                    std::move(target), expressionList(node.args),
                    namedArguments(node.namedArgs), optionalExpression(node.block),
                });
            } else if constexpr (std::is_same_v<T, MethodCall>) {
                return expressionVariant("MethodCall", {
                    expressionPtr(node.receiver), m_builder.string(node.method),
                    expressionList(node.args), namedArguments(node.namedArgs),
                    optionalExpression(node.block),
                    m_builder.boolean(node.mutating),
                    m_builder.boolean(node.parenthesized),
                    node.targetType
                        ? m_builder.just(typeRef(*node.targetType))
                        : m_builder.none(),
                });
            } else if constexpr (std::is_same_v<T, TaggedLiteral>) {
                std::vector<Value> parts;
                for (const auto& part : node.parts)
                    parts.push_back(m_builder.string(part));
                return expressionVariant("TaggedLiteral", {
                    m_builder.string(node.tag), m_builder.list(std::move(parts)),
                    expressionList(node.values),
                });
            } else if constexpr (std::is_same_v<T, BinaryOp>) {
                return expressionVariant("BinaryOp", {
                    expressionPtr(node.left),
                    m_builder.string(std::string(tokenTypeName(node.op))),
                    expressionPtr(node.right),
                });
            } else if constexpr (std::is_same_v<T, UnaryOp>) {
                return expressionVariant("UnaryOp", {
                    m_builder.string(std::string(tokenTypeName(node.op))),
                    expressionPtr(node.operand),
                });
            } else if constexpr (std::is_same_v<T, BlockExpr>) {
                return expressionVariant("Block", {expressionList(node.body)});
            } else if constexpr (std::is_same_v<T, Lambda>) {
                std::vector<Value> params;
                for (const auto& param : node.params) {
                    params.push_back(m_builder.record("LambdaParam", {
                        {"name", m_builder.string(param.name)},
                        {"type", optionalType(param.type)},
                    }));
                }
                return expressionVariant("Lambda", {
                    m_builder.list(std::move(params)), expressionList(node.body),
                    optionalType(node.returnAnnotation),
                    node.rescue
                        ? m_builder.just(rescueInfo(*node.rescue))
                        : m_builder.none(),
                });
            } else if constexpr (std::is_same_v<T, ListExpr>) {
                return expressionVariant("ListLit", {
                    expressionList(node.elements), optionalExpression(node.rest),
                });
            } else if constexpr (std::is_same_v<T, TupleExpr>) {
                return expressionVariant("TupleLit", {
                    expressionList(node.elements),
                });
            } else if constexpr (std::is_same_v<T, RangeExpr>) {
                return expressionVariant("RangeLit", {
                    expressionPtr(node.start), expressionPtr(node.end),
                });
            } else if constexpr (std::is_same_v<T, VarExpr>) {
                return expressionVariant("Var", {
                    m_builder.string(node.name), optionalType(node.type),
                    expressionPtr(node.value),
                });
            } else if constexpr (std::is_same_v<T, LetExpr>) {
                return expressionVariant("Let", {
                    patternRef(*node.pattern), optionalType(node.type),
                    expressionPtr(node.value),
                });
            } else if constexpr (std::is_same_v<T, AssignExpr>) {
                return expressionVariant("Assign", {
                    m_builder.string(node.name), expressionPtr(node.value),
                });
            } else if constexpr (std::is_same_v<T, ReturnExpr>) {
                return expressionVariant("Return", {expressionPtr(node.value)});
            } else if constexpr (std::is_same_v<T, BreakExpr>) {
                return expressionVariant("Break", {});
            } else if constexpr (std::is_same_v<T, NextExpr>) {
                return expressionVariant("Next", {});
            } else if constexpr (std::is_same_v<T, SpawnExpr>) {
                return expressionVariant("Spawn", {expressionList(node.body)});
            } else if constexpr (std::is_same_v<T, TryExpr>) {
                return expressionVariant("Try", {expressionPtr(node.operand)});
            } else if constexpr (std::is_same_v<T, SpreadExpr>) {
                return expressionVariant("Spread", {expressionPtr(node.inner)});
            } else if constexpr (std::is_same_v<T, TrailingIf>) {
                return expressionVariant("TrailingIf", {
                    expressionPtr(node.expr), expressionPtr(node.condition),
                });
            } else if constexpr (std::is_same_v<T, ThenElseExpr>) {
                return expressionVariant("ThenElse", {
                    expressionPtr(node.condition), expressionPtr(node.thenExpr),
                    expressionPtr(node.elseExpr),
                });
            } else if constexpr (std::is_same_v<T, ShorthandLambda>) {
                return expressionVariant("ShorthandLambda", {
                    m_builder.string(node.name), expressionList(node.args),
                    m_builder.boolean(
                        node.kind == ShorthandLambda::Kind::MethodWithArgs),
                });
            } else if constexpr (std::is_same_v<T, CurryPlaceholder>) {
                return expressionVariant("CurryPlaceholder", {});
            } else if constexpr (std::is_same_v<T, CurryExpr>) {
                std::vector<Value> groups;
                for (const auto& group : node.argGroups)
                    groups.push_back(expressionList(group));
                return expressionVariant("Curry", {
                    m_builder.string(node.name),
                    node.module.empty()
                        ? m_builder.none()
                        : m_builder.just(m_builder.string(node.module)),
                    m_builder.boolean(node.isOperator),
                    m_builder.list(std::move(groups)),
                });
            } else if constexpr (std::is_same_v<T, UsingExpr>) {
                std::vector<Value> onlyNames;
                for (const auto& name : node.onlyNames)
                    onlyNames.push_back(m_builder.string(name));
                std::vector<Value> exceptNames;
                for (const auto& name : node.exceptNames)
                    exceptNames.push_back(m_builder.string(name));
                return expressionVariant("Using", {
                    m_builder.string(joinName(node.module)),
                    node.alias
                        ? m_builder.just(m_builder.string(*node.alias))
                        : m_builder.none(),
                    m_builder.list(std::move(onlyNames)),
                    m_builder.list(std::move(exceptNames)),
                    expressionList(node.body),
                });
            } else if constexpr (std::is_same_v<T, ErrorNode>) {
                return expressionVariant("ErrorExpression", {
                    m_builder.string(node.message),
                });
            } else if constexpr (std::is_same_v<T, TryingExpr>) {
                return expressionVariant("Trying", {
                    expressionList(node.body), rescueInfo(node.rescue),
                });
            } else if constexpr (std::is_same_v<T, WhileExpr>) {
                return expressionVariant("While", {
                    expressionPtr(node.condition), expressionList(node.body),
                });
            } else if constexpr (std::is_same_v<T, LoopExpr>) {
                std::vector<Value> counters;
                if (node.counter)
                    counters.push_back(m_builder.string(*node.counter));
                return expressionVariant("Loop", {
                    m_builder.list(std::move(counters)), expressionList(node.body),
                });
            } else if constexpr (std::is_same_v<T, MatchExpr>) {
                std::vector<Value> clauses;
                for (const auto& clause : node.clauses)
                    clauses.push_back(matchArm(clause));
                return expressionVariant("Match", {
                    expressionPtr(node.subject),
                    node.subjectBinding
                        ? m_builder.just(m_builder.string(*node.subjectBinding))
                        : m_builder.none(),
                    m_builder.list(std::move(clauses)),
                });
            } else if constexpr (std::is_same_v<T, ReceiveExpr>) {
                std::vector<Value> clauses;
                for (const auto& clause : node.clauses)
                    clauses.push_back(matchArm(clause));
                return expressionVariant("Receive", {
                    node.senderBinding
                        ? m_builder.just(m_builder.string(*node.senderBinding))
                        : m_builder.none(),
                    m_builder.list(std::move(clauses)),
                    optionalExpression(node.timeout),
                    optionalExpression(node.afterBody),
                });
            } else if constexpr (std::is_same_v<T, MapExpr>) {
                std::vector<Value> entries;
                for (const auto& entry : node.entries) {
                    if (entry.spread) {
                        entries.push_back(m_builder.variant(
                            "MapSpread", "Kex.AST.MapItem",
                            {expressionPtr(entry.value)}));
                    } else {
                        entries.push_back(m_builder.variant(
                            "MapEntry", "Kex.AST.MapItem",
                            {expressionPtr(entry.key), expressionPtr(entry.value)}));
                    }
                }
                return expressionVariant("MapLit", {
                    m_builder.list(std::move(entries)),
                });
            } else if constexpr (std::is_same_v<T, RecordConstruction>) {
                std::vector<Value> fields;
                for (const auto& [name, value] : node.fields) {
                    fields.push_back(m_builder.record("RecordField", {
                        {"name", m_builder.string(name)},
                        {"value", expressionPtr(value)},
                    }));
                }
                return expressionVariant("RecordLit", {
                    m_builder.string(node.typeName),
                    m_builder.list(std::move(fields)),
                });
            } else if constexpr (std::is_same_v<T, IfExpr>) {
                std::vector<Value> elifs;
                for (const auto& [condition, body] : node.elifs) {
                    elifs.push_back(m_builder.record("ElseIf", {
                        {"condition", expressionPtr(condition)},
                        {"body", expressionList(body)},
                    }));
                }
                return expressionVariant("If", {
                    expressionPtr(node.condition),
                    node.letPattern
                        ? m_builder.just(patternRef(*node.letPattern))
                        : m_builder.none(),
                    expressionList(node.thenBody),
                    m_builder.list(std::move(elifs)),
                    optionalExpressionList(node.elseBody),
                });
            } else {
                return expressionVariant("UnsupportedExpression", {
                    m_builder.string(expressionKindName<T>()),
                });
            }
        }, expr.kind);
    }

private:
    auto mainBlock(const MainBlock& block, const std::string& filename,
                   const Docs& docs) -> Value {
        if (block.synthetic) {
            if (block.body.empty())
                return {};
            auto* let = std::get_if<LetExpr>(&block.body[0]->kind);
            if (!let)
                return {};
            auto* variable = std::get_if<VarPattern>(&let->pattern->kind);
            if (!variable)
                return {};
            auto info = m_builder.record("ConstantInfo", {
                {"name", m_builder.string(variable->name)},
                {"doc", optionalString(docAt(docs, block.location.line))},
                {"type", m_builder.none()},
                {"location", location(block.location, filename)},
            });
            return nodeVariant("ConstantDef", {std::move(info)});
        }

        std::vector<Value> params;
        for (const auto& param : block.params)
            params.push_back(this->param(param));
        auto info = m_builder.record("MainInfo", {
            {"doc", optionalString(docAt(docs, block.location.line))},
            {"params", m_builder.list(std::move(params))},
            {"body", expressionList(block.body)},
            {"location", location(block.location, filename)},
        });
        return nodeVariant("MainDef", {std::move(info)});
    }

    template<typename Item>
    auto appendMakeItem(std::vector<Value>& body, const Item& item,
                        const std::string& filename, const Docs& docs) -> void {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<FunctionDef>>) {
                body.push_back(functionDef(
                    *node, docAt(docs, node->location.line), filename));
            } else if constexpr (std::is_same_v<
                                     T, std::unique_ptr<TypeAnnotation>>) {
                body.push_back(typeAnnotation(*node, ""));
            } else if constexpr (std::is_same_v<
                                     T, std::unique_ptr<VisibilityBlock>>) {
                for (const auto& nested : node->items)
                    appendMakeItem(body, nested, filename, docs);
            }
        }, item);
    }

    static auto docAt(const Docs& docs, int line) -> std::string {
        auto it = docs.find(line);
        return it == docs.end() ? "" : it->second;
    }

    auto typeVariant(const TypeExpr& expr) -> Value {
        std::string name;
        std::vector<Value> fields;
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, TypeName>) {
                name = node.parts.empty() ? "" : node.parts.front();
            } else if constexpr (std::is_same_v<T, GenericType>) {
                name = node.name.parts.empty() ? "" : node.name.parts.front();
                for (const auto& arg : node.args)
                    fields.push_back(typeRef(*arg));
            }
        }, expr.kind);
        return m_builder.record("VariantInfo", {
            {"name", m_builder.string(std::move(name))},
            {"fields", m_builder.list(std::move(fields))},
        });
    }

    auto optionalString(const std::string& value) -> Value {
        return value.empty() ? m_builder.none()
                             : m_builder.just(m_builder.string(value));
    }

    auto expressionVariant(std::string tag, std::vector<Value> args) -> Value {
        return m_builder.variant(std::move(tag), "Kex.AST.Expression",
                                 std::move(args));
    }

    auto expressionPtr(const ExprPtr& expr) -> Value {
        return expr ? expression(*expr) : m_builder.none();
    }

    auto expressionList(const std::vector<ExprPtr>& expressions) -> Value {
        std::vector<Value> values;
        for (const auto& expression : expressions)
            values.push_back(expressionPtr(expression));
        return m_builder.list(std::move(values));
    }

    auto namedArguments(
        const std::vector<std::pair<std::string, ExprPtr>>& arguments) -> Value {
        std::vector<Value> values;
        for (const auto& [name, expression] : arguments) {
            values.push_back(m_builder.record("NamedArgument", {
                {"name", m_builder.string(name)},
                {"value", expressionPtr(expression)},
            }));
        }
        return m_builder.list(std::move(values));
    }

    auto optionalExpression(const std::optional<ExprPtr>& expression) -> Value {
        return expression ? m_builder.just(expressionPtr(*expression))
                          : m_builder.none();
    }

    auto optionalExpressionList(
        const std::optional<std::vector<ExprPtr>>& expressions) -> Value {
        return expressions ? m_builder.just(expressionList(*expressions))
                           : m_builder.none();
    }

    auto optionalType(const std::optional<TypeExprPtr>& type) -> Value {
        return type ? m_builder.just(typeRef(**type)) : m_builder.none();
    }

    template<typename T>
    static constexpr auto expressionKindName() -> const char* {
#define KEX_AST_EXPRESSION_NAME(Type) \
        if constexpr (std::is_same_v<T, Type>) return #Type; else
        KEX_AST_EXPRESSION_NAME(GeneratedDecl)
        return "Unknown";
#undef KEX_AST_EXPRESSION_NAME
    }

    auto nodeVariant(std::string tag, std::vector<Value> args) -> Value {
        return m_builder.variant(std::move(tag), "Node", std::move(args));
    }

    auto variant(std::string tag, std::vector<Value> args) -> Value {
        return m_builder.variant(std::move(tag), "Kex.AST.TypeRef",
                                 std::move(args));
    }

    auto namedType(std::string name, std::vector<Value> args) -> Value {
        return variant("NamedType", {m_builder.string(std::move(name)),
                                     m_builder.list(std::move(args))});
    }

    auto patternVariant(std::string tag, std::vector<Value> args) -> Value {
        return m_builder.variant(std::move(tag), "Kex.AST.PatternRef",
                                 std::move(args));
    }

    auto patternList(const std::vector<PatternPtr>& patterns) -> Value {
        std::vector<Value> values;
        for (const auto& pattern : patterns)
            values.push_back(patternRef(*pattern));
        return m_builder.list(std::move(values));
    }

    auto matchArm(const MatchClause& clause) -> Value {
        return m_builder.record("MatchArm", {
            {"patterns", patternList(clause.patterns)},
            {"guard", optionalExpression(clause.guard)},
            {"body", expressionPtr(clause.body)},
        });
    }

    auto rescueInfo(const RescueBlock& rescue) -> Value {
        std::vector<Value> arms;
        for (const auto& clause : rescue.clauses)
            arms.push_back(matchArm(clause));
        return m_builder.record("RescueInfo", {
            {"arms", m_builder.list(std::move(arms))},
            {"catchAllName", rescue.isCatchAll
                ? m_builder.just(m_builder.string(rescue.catchAllParam))
                : m_builder.none()},
            {"catchAllBody", expressionList(rescue.catchAllBody)},
            {"inlineReturn", rescue.isInlineReturn && rescue.inlineReturnExpr
                ? m_builder.just(expressionPtr(rescue.inlineReturnExpr))
                : m_builder.none()},
        });
    }

    auto optionalPattern(const std::optional<PatternPtr>& pattern) -> Value {
        return pattern && *pattern
            ? m_builder.just(patternRef(**pattern))
            : m_builder.none();
    }

    auto typeList(const std::vector<TypeExprPtr>& types) -> Value {
        std::vector<Value> values;
        for (const auto& type : types)
            values.push_back(typeRef(*type));
        return m_builder.list(std::move(values));
    }

    auto stringList(const std::vector<std::string>& strings) -> Value {
        std::vector<Value> values;
        for (const auto& value : strings)
            values.push_back(m_builder.string(value));
        return m_builder.list(std::move(values));
    }

    static auto joinName(const TypeName& name) -> std::string {
        std::string result;
        for (const auto& part : name.parts) {
            if (!result.empty())
                result += ".";
            result += part;
        }
        return result;
    }

    Builder& m_builder;
};

} // namespace kex::ast
