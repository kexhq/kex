#include "../evaluator.hxx"
#include "../../lexer/lexer.hxx"
#include "../../parser/parser.hxx"
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace kex::interpreter {

// --- Doc-comment extraction ---

static auto extractDocComments(const std::string& source) -> std::unordered_map<int, std::string> {
    std::unordered_map<int, std::string> result;
    std::istringstream stream(source);
    std::string line;
    std::string accum;
    int lineNum = 0;
    bool inBlock = false;

    while (std::getline(stream, line)) {
        ++lineNum;
        // Trim leading whitespace
        auto trimmed = line;
        auto pos = trimmed.find_first_not_of(" \t");
        if (pos != std::string::npos) trimmed = trimmed.substr(pos);
        else trimmed = "";

        if (!trimmed.empty() && trimmed[0] == '#') {
            // Strip "# " or "#" prefix
            std::string content;
            if (trimmed.size() > 1 && trimmed[1] == ' ')
                content = trimmed.substr(2);
            else
                content = trimmed.substr(1);
            if (inBlock) accum += "\n";
            accum += content;
            inBlock = true;
        } else if (trimmed.empty() && inBlock) {
            accum += "\n";
        } else {
            if (inBlock && !accum.empty()) {
                // Trim trailing newlines
                while (!accum.empty() && accum.back() == '\n') accum.pop_back();
                result[lineNum] = accum;
                accum.clear();
            }
            inBlock = false;
        }
    }
    return result;
}

// --- TypeRef builders ---

static auto makeTypeRef(const std::string& tag, std::vector<ValuePtr> args = {}) -> ValuePtr {
    return Value::variant(tag, "TypeRef", std::move(args));
}

static auto typeExprToTypeRef(const ast::TypeExpr& expr) -> ValuePtr {
    return std::visit([](const auto& node) -> ValuePtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::TypeName>) {
            std::string name;
            for (size_t i = 0; i < node.parts.size(); ++i) {
                if (i > 0) name += ".";
                name += node.parts[i];
            }
            return makeTypeRef("NamedType", {Value::string(name), Value::list({})});
        } else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::string name;
            for (size_t i = 0; i < node.name.parts.size(); ++i) {
                if (i > 0) name += ".";
                name += node.name.parts[i];
            }
            std::vector<ValuePtr> typeArgs;
            for (const auto& arg : node.args) {
                typeArgs.push_back(typeExprToTypeRef(*arg));
            }
            return makeTypeRef("NamedType", {Value::string(name), Value::list(std::move(typeArgs))});
        } else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            std::vector<ValuePtr> paramTypes;
            // FunctionType has param and result
            paramTypes.push_back(typeExprToTypeRef(*node.param));
            return makeTypeRef("FunctionType", {Value::list(std::move(paramTypes)), typeExprToTypeRef(*node.result)});
        } else if constexpr (std::is_same_v<T, ast::TupleType>) {
            std::vector<ValuePtr> elems;
            for (const auto& e : node.elements) {
                elems.push_back(typeExprToTypeRef(*e));
            }
            return makeTypeRef("TupleType", {Value::list(std::move(elems))});
        } else if constexpr (std::is_same_v<T, ast::ListType>) {
            return makeTypeRef("ListType", {typeExprToTypeRef(*node.element)});
        } else if constexpr (std::is_same_v<T, ast::MapType>) {
            return makeTypeRef("MapType", {typeExprToTypeRef(*node.key), typeExprToTypeRef(*node.value)});
        } else if constexpr (std::is_same_v<T, ast::UnionType>) {
            std::vector<ValuePtr> members;
            members.push_back(typeExprToTypeRef(*node.left));
            members.push_back(typeExprToTypeRef(*node.right));
            return makeTypeRef("UnionType", {Value::list(std::move(members))});
        } else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return makeTypeRef("NullableType", {typeExprToTypeRef(*node.inner)});
        } else if constexpr (std::is_same_v<T, ast::BlockType>) {
            return typeExprToTypeRef(*node.inner);
        } else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return makeTypeRef("NamedType", {Value::string(node.name), Value::list({})});
        } else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            return makeTypeRef("TypeVar", {Value::string(node.name)});
        } else {
            return makeTypeRef("AnyType");
        }
    }, expr.kind);
}

// --- PatternRef builders ---

static auto makePatternRef(const std::string& tag, std::vector<ValuePtr> args = {}) -> ValuePtr {
    return Value::variant(tag, "PatternRef", std::move(args));
}

static auto patternToPatternRef(const ast::Pattern& pat) -> ValuePtr {
    return std::visit([](const auto& node) -> ValuePtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::VarPattern>) {
            return makePatternRef("BindPattern", {Value::string(node.name)});
        } else if constexpr (std::is_same_v<T, ast::LiteralPattern>) {
            return makePatternRef("LiteralPattern", {Value::string(node.literal.value)});
        } else if constexpr (std::is_same_v<T, ast::ConstructorPattern>) {
            std::vector<ValuePtr> args;
            for (const auto& a : node.args) {
                args.push_back(patternToPatternRef(*a));
            }
            return makePatternRef("ConstructorPattern", {Value::string(node.name), Value::list(std::move(args))});
        } else if constexpr (std::is_same_v<T, ast::TuplePattern>) {
            std::vector<ValuePtr> elems;
            for (const auto& e : node.elements) {
                elems.push_back(patternToPatternRef(*e));
            }
            return makePatternRef("TuplePattern", {Value::list(std::move(elems))});
        } else if constexpr (std::is_same_v<T, ast::ListPattern>) {
            std::vector<ValuePtr> elems;
            for (const auto& e : node.elements) {
                elems.push_back(patternToPatternRef(*e));
            }
            return makePatternRef("ListPattern", {Value::list(std::move(elems))});
        } else if constexpr (std::is_same_v<T, ast::WildcardPattern>) {
            return makePatternRef("WildcardPattern");
        } else if constexpr (std::is_same_v<T, ast::RecordPattern>) {
            // Represent as ConstructorPattern("{}", fields as bind patterns)
            std::vector<ValuePtr> fields;
            for (const auto& f : node.fields) {
                fields.push_back(makePatternRef("BindPattern", {Value::string(f.name)}));
            }
            return makePatternRef("ConstructorPattern", {Value::string("{}"), Value::list(std::move(fields))});
        } else if constexpr (std::is_same_v<T, ast::RangePattern>) {
            return makePatternRef("LiteralPattern", {Value::string("<range>")});
        } else if constexpr (std::is_same_v<T, ast::ThisPattern>) {
            if (node.inner) return patternToPatternRef(*node.inner);
            return makePatternRef("WildcardPattern");
        } else {
            return makePatternRef("WildcardPattern");
        }
    }, pat.kind);
}

// --- Location builder ---

static auto makeLocation(SourceLocation loc, const std::string& filename) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["file"] = Value::string(filename);
    fields["line"] = Value::integer(loc.line);
    fields["column"] = Value::integer(loc.column);
    return Value::record("Location", std::move(fields));
}

// --- TypeRef.toString ---

static auto typeRefToString(const ValuePtr& ref) -> std::string {
    auto* var = std::get_if<VariantValue>(&ref->data);
    if (!var) return "Any";

    if (var->tag == "NamedType") {
        auto name = std::get<StringValue>(var->args[0]->data).value;
        auto* list = std::get_if<ListValue>(&var->args[1]->data);
        if (!list || list->elements.empty()) return name;
        std::string result = name + "<";
        for (size_t i = 0; i < list->elements.size(); ++i) {
            if (i > 0) result += ", ";
            result += typeRefToString(list->elements[i]);
        }
        return result + ">";
    } else if (var->tag == "FunctionType") {
        auto* params = std::get_if<ListValue>(&var->args[0]->data);
        std::string result = "(";
        if (params) {
            for (size_t i = 0; i < params->elements.size(); ++i) {
                if (i > 0) result += ", ";
                result += typeRefToString(params->elements[i]);
            }
        }
        result += ") -> " + typeRefToString(var->args[1]);
        return result;
    } else if (var->tag == "TupleType") {
        auto* elems = std::get_if<ListValue>(&var->args[0]->data);
        std::string result = "(";
        if (elems) {
            for (size_t i = 0; i < elems->elements.size(); ++i) {
                if (i > 0) result += ", ";
                result += typeRefToString(elems->elements[i]);
            }
        }
        return result + ")";
    } else if (var->tag == "ListType") {
        return "[" + typeRefToString(var->args[0]) + "]";
    } else if (var->tag == "MapType") {
        return "{" + typeRefToString(var->args[0]) + " => " + typeRefToString(var->args[1]) + "}";
    } else if (var->tag == "UnionType") {
        auto* members = std::get_if<ListValue>(&var->args[0]->data);
        std::string result;
        if (members) {
            for (size_t i = 0; i < members->elements.size(); ++i) {
                if (i > 0) result += " | ";
                result += typeRefToString(members->elements[i]);
            }
        }
        return result;
    } else if (var->tag == "NullableType") {
        return typeRefToString(var->args[0]) + "?";
    } else if (var->tag == "TypeVar") {
        return std::get<StringValue>(var->args[0]->data).value;
    } else if (var->tag == "AnyType") {
        return "Any";
    } else if (var->tag == "NoneType") {
        return "None";
    }
    return "?";
}

// --- AST conversion ---

static auto convertParam(const ast::Param& param) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["name"] = param.name ? Value::string(*param.name) : Value::none();
    fields["type"] = param.type ? typeExprToTypeRef(**param.type) : Value::none();
    fields["hasDefault"] = Value::boolean(param.defaultValue.has_value());
    return Value::record("ParamInfo", std::move(fields));
}

static auto convertClause(const ast::FunctionClause& clause) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    std::vector<ValuePtr> params;
    for (const auto& p : clause.params) {
        params.push_back(convertParam(p));
    }
    fields["params"] = Value::list(std::move(params));
    fields["returnType"] = clause.returnAnnotation ? typeExprToTypeRef(**clause.returnAnnotation) : Value::none();
    return Value::record("ClauseInfo", std::move(fields));
}

static auto convertFunctionDef(const ast::FunctionDef& def, const std::string& doc, const std::string& filename) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["name"] = Value::string(def.name);
    fields["doc"] = doc.empty() ? Value::none() : Value::string(doc);
    fields["foul"] = Value::boolean(def.isFoul);
    fields["predicate"] = Value::boolean(def.isPredicate);
    std::vector<ValuePtr> clauses;
    for (const auto& c : def.clauses) {
        clauses.push_back(convertClause(c));
    }
    fields["clauses"] = Value::list(std::move(clauses));
    fields["location"] = makeLocation(def.location, filename);
    return Value::variant("FunctionDef", "Node", {Value::record("FunctionInfo", std::move(fields))});
}

static auto convertTypeAnnotation(const ast::TypeAnnotation& ann, const std::string& doc) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["name"] = Value::string(ann.name);
    fields["type"] = typeExprToTypeRef(*ann.type);
    fields["doc"] = doc.empty() ? Value::none() : Value::string(doc);
    fields["implicitThis"] = Value::boolean(ann.implicitThis);
    return Value::variant("TypeAnnotation", "Node", {Value::record("AnnotationInfo", std::move(fields))});
}

static auto convertTypeDef(const ast::TypeDef& def, const std::string& doc, const std::string& filename) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["name"] = Value::string(def.name);
    fields["doc"] = doc.empty() ? Value::none() : Value::string(doc);
    std::vector<ValuePtr> typeParams;
    for (const auto& tp : def.typeParams) {
        typeParams.push_back(Value::string(tp));
    }
    fields["typeParams"] = Value::list(std::move(typeParams));
    std::vector<ValuePtr> parents;
    for (const auto& p : def.parents) {
        std::string name;
        for (size_t i = 0; i < p.parts.size(); ++i) {
            if (i > 0) name += ".";
            name += p.parts[i];
        }
        parents.push_back(makeTypeRef("NamedType", {Value::string(name), Value::list({})}));
    }
    fields["parents"] = Value::list(std::move(parents));
    if (def.variants) {
        std::vector<ValuePtr> variants;
        for (const auto& v : *def.variants) {
            // Each variant is a TypeExprPtr; extract name from it
            std::unordered_map<std::string, ValuePtr> vf;
            // Variants in the AST are TypeExprPtrs — usually TypeName or GenericType
            std::string vname;
            std::vector<ValuePtr> vfields;
            std::visit([&](const auto& node) {
                using VT = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<VT, ast::TypeName>) {
                    vname = node.parts.empty() ? "" : node.parts[0];
                } else if constexpr (std::is_same_v<VT, ast::GenericType>) {
                    vname = node.name.parts.empty() ? "" : node.name.parts[0];
                    for (const auto& arg : node.args) {
                        vfields.push_back(typeExprToTypeRef(*arg));
                    }
                }
            }, v->kind);
            vf["name"] = Value::string(vname);
            vf["fields"] = Value::list(std::move(vfields));
            variants.push_back(Value::record("VariantInfo", std::move(vf)));
        }
        fields["variants"] = Value::list(std::move(variants));
    } else {
        fields["variants"] = Value::none();
    }
    fields["location"] = makeLocation(def.location, filename);
    return Value::variant("TypeDef", "Node", {Value::record("TypeInfo", std::move(fields))});
}

static auto convertRecordDef(const ast::RecordDef& def, const std::string& doc, const std::string& filename) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["name"] = Value::string(def.name);
    fields["doc"] = doc.empty() ? Value::none() : Value::string(doc);
    std::vector<ValuePtr> typeParams;
    for (const auto& tp : def.typeParams) {
        typeParams.push_back(Value::string(tp));
    }
    fields["typeParams"] = Value::list(std::move(typeParams));
    std::vector<ValuePtr> recordFields;
    for (const auto& f : def.fields) {
        std::unordered_map<std::string, ValuePtr> ff;
        ff["name"] = Value::string(f.name);
        ff["type"] = typeExprToTypeRef(*f.type);
        ff["hasDefault"] = Value::boolean(f.defaultValue.has_value());
        recordFields.push_back(Value::record("FieldInfo", std::move(ff)));
    }
    fields["fields"] = Value::list(std::move(recordFields));
    fields["location"] = makeLocation(def.location, filename);
    return Value::variant("RecordDef", "Node", {Value::record("RecordInfo", std::move(fields))});
}

static auto convertTraitDef(const ast::TraitDef& def, const std::string& doc, const std::string& filename) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["name"] = Value::string(def.name);
    fields["doc"] = doc.empty() ? Value::none() : Value::string(doc);
    std::vector<ValuePtr> typeParams;
    for (const auto& tp : def.typeParams) {
        typeParams.push_back(Value::string(tp));
    }
    fields["typeParams"] = Value::list(std::move(typeParams));
    std::vector<ValuePtr> body;
    for (const auto& item : def.body) {
        std::visit([&](const auto& node) {
            using IT = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<IT, std::unique_ptr<ast::TypeAnnotation>>) {
                body.push_back(convertTypeAnnotation(*node, ""));
            } else if constexpr (std::is_same_v<IT, std::unique_ptr<ast::FunctionDef>>) {
                body.push_back(convertFunctionDef(*node, "", filename));
            }
        }, item);
    }
    fields["body"] = Value::list(std::move(body));
    fields["location"] = makeLocation(def.location, filename);
    return Value::variant("TraitDef", "Node", {Value::record("TraitInfo", std::move(fields))});
}

static auto convertMakeDef(const ast::MakeDef& def, const std::string& filename,
                            const std::unordered_map<int, std::string>& docs) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["target"] = typeExprToTypeRef(*def.target);
    fields["final"] = Value::boolean(def.isFinal);
    std::vector<ValuePtr> impls;
    for (const auto& i : def.implements) {
        impls.push_back(makeTypeRef("NamedType", {Value::string(i), Value::list({})}));
    }
    fields["implements"] = Value::list(std::move(impls));
    std::vector<ValuePtr> body;
    for (const auto& item : def.body) {
        std::visit([&](const auto& node) {
            using IT = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<IT, std::unique_ptr<ast::FunctionDef>>) {
                auto it = docs.find(node->location.line);
                std::string doc = (it != docs.end()) ? it->second : "";
                body.push_back(convertFunctionDef(*node, doc, filename));
            } else if constexpr (std::is_same_v<IT, std::unique_ptr<ast::TypeAnnotation>>) {
                auto it = docs.find(node->name.size()); // approximation
                body.push_back(convertTypeAnnotation(*node, ""));
            } else if constexpr (std::is_same_v<IT, std::unique_ptr<ast::VisibilityBlock>>) {
                // Flatten visibility block items
                for (const auto& vi : node->items) {
                    std::visit([&](const auto& vnode) {
                        using VIT = std::decay_t<decltype(vnode)>;
                        if constexpr (std::is_same_v<VIT, std::unique_ptr<ast::FunctionDef>>) {
                            auto vit = docs.find(vnode->location.line);
                            std::string vdoc = (vit != docs.end()) ? vit->second : "";
                            body.push_back(convertFunctionDef(*vnode, vdoc, filename));
                        } else if constexpr (std::is_same_v<VIT, std::unique_ptr<ast::TypeAnnotation>>) {
                            body.push_back(convertTypeAnnotation(*vnode, ""));
                        }
                    }, vi);
                }
            }
        }, item);
    }
    fields["body"] = Value::list(std::move(body));
    fields["location"] = makeLocation(def.location, filename);
    return Value::variant("MakeDef", "Node", {Value::record("MakeInfo", std::move(fields))});
}

static auto convertPragma(const ast::Pragma& pragma, const std::string& filename) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["name"] = Value::string(pragma.requirements.empty() ? "" : pragma.requirements[0]);
    fields["value"] = pragma.requirements.size() > 1 ? Value::string(pragma.requirements[1]) : Value::none();
    fields["location"] = makeLocation(pragma.location, filename);
    return Value::variant("PragmaDef", "Node", {Value::record("PragmaInfo", std::move(fields))});
}

// Forward declaration for module items
static auto convertModuleItem(const ast::ModuleItem& item, const std::string& filename,
                               const std::unordered_map<int, std::string>& docs) -> ValuePtr;

static auto convertModuleDef(const ast::ModuleDef& mod, const std::string& doc, const std::string& filename,
                              const std::unordered_map<int, std::string>& docs) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["name"] = Value::string(mod.name);
    fields["doc"] = doc.empty() ? Value::none() : Value::string(doc);
    fields["foul"] = Value::boolean(mod.isFoul);
    std::vector<ValuePtr> items;
    for (const auto& item : mod.body) {
        auto converted = convertModuleItem(item, filename, docs);
        if (converted) items.push_back(std::move(converted));
    }
    fields["items"] = Value::list(std::move(items));
    fields["location"] = makeLocation(mod.location, filename);
    return Value::variant("ModuleDef", "Node", {Value::record("ModuleInfo", std::move(fields))});
}

static auto convertModuleItem(const ast::ModuleItem& item, const std::string& filename,
                               const std::unordered_map<int, std::string>& docs) -> ValuePtr {
    return std::visit([&](const auto& node) -> ValuePtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertModuleDef(*node, doc, filename, docs);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertFunctionDef(*node, doc, filename);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertTypeDef(*node, doc, filename);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertRecordDef(*node, doc, filename);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertTraitDef(*node, doc, filename);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
            return convertMakeDef(*node, filename, docs);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
            auto it = docs.find(0); // type annotations don't have location easily
            return convertTypeAnnotation(*node, "");
        } else {
            return nullptr;
        }
    }, item);
}

static auto convertTopLevelItem(const ast::TopLevelItem& item, const std::string& filename,
                                 const std::unordered_map<int, std::string>& docs) -> ValuePtr {
    return std::visit([&](const auto& node) -> ValuePtr {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, std::unique_ptr<ast::ModuleDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertModuleDef(*node, doc, filename, docs);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertFunctionDef(*node, doc, filename);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertTypeDef(*node, doc, filename);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::RecordDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertRecordDef(*node, doc, filename);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TraitDef>>) {
            auto it = docs.find(node->location.line);
            std::string doc = (it != docs.end()) ? it->second : "";
            return convertTraitDef(*node, doc, filename);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MakeDef>>) {
            return convertMakeDef(*node, filename, docs);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::Pragma>>) {
            return convertPragma(*node, filename);
        } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::TypeAnnotation>>) {
            return convertTypeAnnotation(*node, "");
        } else {
            return nullptr;
        }
    }, item);
}

static auto convertProgram(const ast::Program& program, const std::string& source,
                            const std::string& filename) -> ValuePtr {
    auto docs = extractDocComments(source);
    std::vector<ValuePtr> items;
    for (const auto& item : program.items) {
        auto converted = convertTopLevelItem(item, filename, docs);
        if (converted) items.push_back(std::move(converted));
    }
    std::unordered_map<std::string, ValuePtr> fields;
    fields["items"] = Value::list(std::move(items));
    return Value::record("Program", std::move(fields));
}

static auto makeParseError(const std::string& message, std::optional<SourceLocation> loc,
                            const std::string& filename) -> ValuePtr {
    std::unordered_map<std::string, ValuePtr> fields;
    fields["message"] = Value::string(message);
    if (loc) {
        fields["location"] = makeLocation(*loc, filename);
    } else {
        fields["location"] = Value::none();
    }
    return Value::record("ParseError", std::move(fields));
}

// --- Registration ---

auto Evaluator::registerParserBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    m_globalEnv->define("Parser", Value::module("Parser"));

    // Register TypeRef/PatternRef variant->parent mappings so UFCS dispatch works
    for (const auto& tag : {"NamedType", "FunctionType", "TupleType", "ListType",
                            "MapType", "UnionType", "NullableType", "TypeVar",
                            "AnyType", "NoneType"}) {
        m_variantParent[tag] = "TypeRef";
    }
    for (const auto& tag : {"BindPattern", "LiteralPattern", "ConstructorPattern",
                            "TuplePattern", "ListPattern", "WildcardPattern",
                            "GuardedPattern"}) {
        m_variantParent[tag] = "PatternRef";
    }

    // Parser.parse(source) or Parser.parse(source, filename)
    reg("Parser::parse", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(makeParseError("parse requires a source string", std::nullopt, ""));
        auto* srcVal = std::get_if<StringValue>(&args[0]->data);
        if (!srcVal) return Value::error(makeParseError("parse requires a source string", std::nullopt, ""));

        std::string source = srcVal->value;
        std::string filename = "<string>";
        if (args.size() > 1) {
            if (auto* fn = std::get_if<StringValue>(&args[1]->data)) {
                filename = fn->value;
            }
        }

        try {
            auto filenamePtr = std::make_shared<std::string>(filename);
            Lexer lexer(source, *filenamePtr);
            auto tokens = lexer.tokenizeAll();
            Parser parser(tokens, *filenamePtr);
            auto program = parser.parseProgram();
            return Value::ok(convertProgram(program, source, filename));
        } catch (const std::exception& e) {
            return Value::error(makeParseError(e.what(), std::nullopt, filename));
        }
    });

    // Parser.parseFile(path)
    reg("Parser::parseFile", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(makeParseError("parseFile requires a file path", std::nullopt, ""));
        auto* pathVal = std::get_if<StringValue>(&args[0]->data);
        if (!pathVal) return Value::error(makeParseError("parseFile requires a file path", std::nullopt, ""));

        std::string path = pathVal->value;
        std::ifstream file(path);
        if (!file.is_open()) {
            return Value::error(makeParseError("File not found: " + path, std::nullopt, path));
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string source = ss.str();

        try {
            auto filenamePtr = std::make_shared<std::string>(path);
            Lexer lexer(source, *filenamePtr);
            auto tokens = lexer.tokenizeAll();
            Parser parser(tokens, *filenamePtr);
            auto program = parser.parseProgram();
            return Value::ok(convertProgram(program, source, path));
        } catch (const std::exception& e) {
            return Value::error(makeParseError(e.what(), std::nullopt, path));
        }
    });

    // Parser.parseType(typeStr)
    reg("Parser::parseType", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(makeParseError("parseType requires a type string", std::nullopt, ""));
        auto* srcVal = std::get_if<StringValue>(&args[0]->data);
        if (!srcVal) return Value::error(makeParseError("parseType requires a type string", std::nullopt, ""));

        // Wrap in a type annotation to get a valid parse
        std::string source = "x : " + srcVal->value + "\nlet x = 0";
        std::string filename = "<type>";

        try {
            auto filenamePtr = std::make_shared<std::string>(filename);
            Lexer lexer(source, *filenamePtr);
            auto tokens = lexer.tokenizeAll();
            Parser parser(tokens, *filenamePtr);
            auto program = parser.parseProgram();
            // Find the type annotation in the parsed result
            for (const auto& item : program.items) {
                if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&item)) {
                    return Value::ok(typeExprToTypeRef(*(*ann)->type));
                }
            }
            return Value::error(makeParseError("Failed to parse type expression", std::nullopt, filename));
        } catch (const std::exception& e) {
            return Value::error(makeParseError(e.what(), std::nullopt, filename));
        }
    });

    // TypeRef.toString / PatternRef.toString — registered as methods
    reg("TypeRef::toString", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("?");
        return Value::string(typeRefToString(args[0]));
    });

    reg("PatternRef::toString", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::string("?");
        auto* var = std::get_if<VariantValue>(&args[0]->data);
        if (!var) return Value::string("?");
        if (var->tag == "BindPattern") return Value::string(std::get<StringValue>(var->args[0]->data).value);
        if (var->tag == "LiteralPattern") return Value::string(std::get<StringValue>(var->args[0]->data).value);
        if (var->tag == "WildcardPattern") return Value::string("_");
        if (var->tag == "ConstructorPattern") {
            auto name = std::get<StringValue>(var->args[0]->data).value;
            auto* pats = std::get_if<ListValue>(&var->args[1]->data);
            if (!pats || pats->elements.empty()) return Value::string(name);
            std::string result = name + "(";
            for (size_t i = 0; i < pats->elements.size(); ++i) {
                if (i > 0) result += ", ";
                // Recursive toString
                auto* inner = std::get_if<VariantValue>(&pats->elements[i]->data);
                if (inner && inner->tag == "BindPattern")
                    result += std::get<StringValue>(inner->args[0]->data).value;
                else
                    result += "_";
            }
            return Value::string(result + ")");
        }
        if (var->tag == "TuplePattern") {
            auto* elems = std::get_if<ListValue>(&var->args[0]->data);
            std::string result = "(";
            if (elems) {
                for (size_t i = 0; i < elems->elements.size(); ++i) {
                    if (i > 0) result += ", ";
                    result += "_";
                }
            }
            return Value::string(result + ")");
        }
        if (var->tag == "ListPattern") {
            return Value::string("[...]");
        }
        return Value::string("?");
    });
}

} // namespace kex::interpreter
