#include "collect_pass.hxx"
#include "../common/type_def_utils.hxx"
#include <variant>
#include <cctype>

namespace kex::semantic {
namespace {

auto typeExprText(const ast::TypeExpr& type) -> std::string;

auto typeNameText(const ast::TypeName& name) -> std::string {
    std::string result;
    for (const auto& part : name.parts) {
        if (!result.empty()) result += ".";
        result += part;
    }
    return result;
}

auto typeExprText(const ast::TypeExpr& type) -> std::string {
    return std::visit([](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ast::TypeName>) {
            return typeNameText(node);
        } else if constexpr (std::is_same_v<T, ast::GenericType>) {
            std::string result = typeNameText(node.name) + "<";
            for (size_t i = 0; i < node.args.size(); ++i) {
                if (i) result += ", ";
                result += node.args[i] ? typeExprText(*node.args[i]) : "?";
            }
            return result + ">";
        } else if constexpr (std::is_same_v<T, ast::FunctionType>) {
            return (node.param ? typeExprText(*node.param) : "?") + " -> " +
                   (node.result ? typeExprText(*node.result) : "?");
        } else if constexpr (std::is_same_v<T, ast::TupleType>) {
            std::string result = "(";
            for (size_t i = 0; i < node.elements.size(); ++i) {
                if (i) result += ", ";
                result += node.elements[i] ? typeExprText(*node.elements[i]) : "?";
            }
            return result + ")";
        } else if constexpr (std::is_same_v<T, ast::ListType>) {
            return "[" + (node.element ? typeExprText(*node.element) : "?") + "]";
        } else if constexpr (std::is_same_v<T, ast::MapType>) {
            return "Map<" + (node.key ? typeExprText(*node.key) : "?") + ", " +
                   (node.value ? typeExprText(*node.value) : "?") + ">";
        } else if constexpr (std::is_same_v<T, ast::UnionType>) {
            return (node.left ? typeExprText(*node.left) : "?") + " | " +
                   (node.right ? typeExprText(*node.right) : "?");
        } else if constexpr (std::is_same_v<T, ast::OptionalType>) {
            return (node.inner ? typeExprText(*node.inner) : "?") + "?";
        } else if constexpr (std::is_same_v<T, ast::BlockType>) {
            return "Block<" + (node.inner ? typeExprText(*node.inner) : "?") + ">";
        } else if constexpr (std::is_same_v<T, ast::AtomType>) {
            return ":" + node.name;
        } else if constexpr (std::is_same_v<T, ast::GenericVar>) {
            return node.name;
        }
        return "?";
    }, type.kind);
}

auto variantText(const ast::TypeExpr& variant) -> std::string {
    if (const auto* generic = std::get_if<ast::GenericType>(&variant.kind)) {
        std::string result = typeNameText(generic->name) + "(";
        for (size_t i = 0; i < generic->args.size(); ++i) {
            if (i) result += ", ";
            result += generic->args[i] ? typeExprText(*generic->args[i]) : "?";
        }
        return result + ")";
    }
    return typeExprText(variant);
}

} // namespace

auto CollectPass::run(SemanticDB& db, const std::string& file) -> void {
    m_state = db.fileState(file);
    if (!m_state) return;
    m_state->symbols.clear();
    m_currentModule = "";

    for (const auto& item : m_state->ast.items) {
        collectTopLevel(item);
    }
}

auto CollectPass::collectTopLevel(const ast::TopLevelItem& item) -> void {
    std::visit([this](const auto& ptr) {
        using T = std::decay_t<decltype(*ptr)>;
        if constexpr (std::is_same_v<T, ast::FunctionDef>) {
            collectFunction(*ptr, "");
        } else if constexpr (std::is_same_v<T, ast::TypeDef>) {
            collectType(*ptr, "");
        } else if constexpr (std::is_same_v<T, ast::RecordDef>) {
            collectRecord(*ptr, "");
        } else if constexpr (std::is_same_v<T, ast::MakeDef>) {
            collectMake(*ptr, "");
        } else if constexpr (std::is_same_v<T, ast::ModuleDef>) {
            collectModule(*ptr);
        } else if constexpr (std::is_same_v<T, ast::TraitDef>) {
            SymbolInfo info;
            info.name = ptr->name;
            info.kind = SymbolKind::Trait;
            info.definition = ptr->location;
            info.module = "";
            info.type = Type::unknown();
            m_state->symbols.push_back(std::move(info));
        } else if constexpr (std::is_same_v<T, ast::TypeAnnotation>) {
            // Standalone type signature declaration (e.g. in prelude files):
            // `describe : (String, Block<Void>) -> Void`
            SymbolInfo info;
            info.name = ptr->name;
            info.kind = SymbolKind::Function;
            info.definition = SourceLocation{std::string_view(m_state->path), 0, 0};
            info.module = "";
            info.isFoul = ptr->isFoul;
            info.type = Type::unknown();
            m_state->symbols.push_back(std::move(info));
        }
        // UsingBlock, MainBlock, Pragma, CompiledBlock: skip
    }, item);
}

auto CollectPass::collectModule(const ast::ModuleDef& mod) -> void {
    const std::string parentModule = m_currentModule;
    const bool nameIsQualified =
        !parentModule.empty() &&
        mod.name.rfind(parentModule + ".", 0) == 0;
    const std::string qualifiedModule =
        parentModule.empty() || nameIsQualified
            ? mod.name
            : parentModule + "." + mod.name;

    // Register the module itself
    SymbolInfo moduleInfo;
    const auto separator = mod.name.rfind('.');
    moduleInfo.name =
        parentModule.empty() || separator == std::string::npos
            ? mod.name
            : mod.name.substr(separator + 1);
    moduleInfo.kind = SymbolKind::Module;
    moduleInfo.definition = mod.location;
    moduleInfo.module = parentModule;
    moduleInfo.type = Type::unknown();
    m_state->symbols.push_back(std::move(moduleInfo));

    std::string savedModule = m_currentModule;
    m_currentModule = qualifiedModule;

    auto collectAnnotation = [&](const ast::TypeAnnotation& ann) {
        // If a FunctionDef with the same name already exists in this module,
        // don't create a duplicate — the def wins.
        for (const auto& sym : m_state->symbols) {
            if (sym.name == ann.name && sym.module == m_currentModule
                    && sym.kind == SymbolKind::Function)
                return;
        }
        SymbolInfo info;
        info.name = ann.name;
        info.kind = SymbolKind::Function;
        info.definition = SourceLocation{std::string_view(m_state->path), 0, 0};
        info.module = m_currentModule;
        info.isFoul = ann.isFoul;
        info.type = Type::unknown();
        m_state->symbols.push_back(std::move(info));
    };

    for (const auto& item : mod.body) {
        std::visit([&](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, ast::FunctionDef>) {
                collectFunction(*ptr, m_currentModule);
            } else if constexpr (std::is_same_v<T, ast::TypeAnnotation>) {
                collectAnnotation(*ptr);
            } else if constexpr (std::is_same_v<T, ast::TypeDef>) {
                collectType(*ptr, m_currentModule);
            } else if constexpr (std::is_same_v<T, ast::RecordDef>) {
                collectRecord(*ptr, m_currentModule);
            } else if constexpr (std::is_same_v<T, ast::MakeDef>) {
                collectMake(*ptr, m_currentModule);
            } else if constexpr (std::is_same_v<T, ast::ModuleDef>) {
                collectModule(*ptr);
            } else if constexpr (std::is_same_v<T, ast::TraitDef>) {
                SymbolInfo info;
                info.name = ptr->name;
                info.kind = SymbolKind::Trait;
                info.definition = ptr->location;
                info.module = m_currentModule;
                info.type = Type::unknown();
                m_state->symbols.push_back(std::move(info));
            } else if constexpr (std::is_same_v<T, ast::VisibilityBlock>) {
                for (const auto& vitem : ptr->items) {
                    const auto firstNewSymbol = m_state->symbols.size();
                    std::visit([&](const auto& vptr) {
                        using VT = std::decay_t<decltype(*vptr)>;
                        if constexpr (std::is_same_v<VT, ast::FunctionDef>) {
                            collectFunction(*vptr, m_currentModule);
                        } else if constexpr (std::is_same_v<VT, ast::TypeAnnotation>) {
                            collectAnnotation(*vptr);
                        } else if constexpr (std::is_same_v<VT, ast::TypeDef>) {
                            collectType(*vptr, m_currentModule);
                        } else if constexpr (std::is_same_v<VT, ast::RecordDef>) {
                            collectRecord(*vptr, m_currentModule);
                        } else if constexpr (std::is_same_v<VT, ast::MakeDef>) {
                            collectMake(*vptr, m_currentModule);
                        }
                    }, vitem);
                    for (size_t i = firstNewSymbol; i < m_state->symbols.size(); ++i)
                        m_state->symbols[i].isExported = ptr->isPublic;
                }
            }
            // UsingBlock, CompiledBlock: skip
        }, item);
    }

    m_currentModule = savedModule;
}

auto CollectPass::collectFunction(const ast::FunctionDef& def, const std::string& module) -> void {
    // Check if this function name is already in symbols (multi-clause)
    for (auto& sym : m_state->symbols) {
        if (sym.name == def.name && sym.module == module && sym.kind == SymbolKind::Function) {
            sym.clauseCount++;
            return;
        }
    }

    SymbolInfo info;
    info.name = def.name;
    info.kind = SymbolKind::Function;
    info.definition = def.location;
    info.module = module;
    info.isFoul = def.isFoul;
    info.type = Type::unknown();
    info.clauseCount = 1;

    // Capture param names from the first clause if available
    if (!def.clauses.empty()) {
        for (const auto& param : def.clauses[0].params) {
            std::string pname = param.name.value_or("_");
            info.params.emplace_back(pname, Type::unknown());
        }
    }

    m_state->symbols.push_back(std::move(info));
}

auto CollectPass::collectType(const ast::TypeDef& def, const std::string& module) -> void {
    SymbolInfo info;
    info.name = def.name;
    info.kind = SymbolKind::Type;
    info.definition = def.location;
    info.module = module;
    info.type = Type::unknown();
    info.detail = std::string(def.isDistinct ? "distinct type " : "type ") +
                  def.name;
    if (!def.typeParams.empty()) {
        info.detail += "<";
        for (size_t i = 0; i < def.typeParams.size(); ++i) {
            if (i) info.detail += ", ";
            info.detail += def.typeParams[i];
        }
        info.detail += ">";
    }
    if (def.variants && !def.variants->empty()) {
        info.detail += " =";
        for (size_t i = 0; i < def.variants->size(); ++i) {
            const auto& variant = (*def.variants)[i];
            if (!variant) continue;
            info.detail += i == 0 ? "\n  " : "\n| ";
            info.detail += kex::isTransparentTypeAlias(def)
                ? typeExprText(*variant) : variantText(*variant);
        }
    }
    m_state->symbols.push_back(std::move(info));

    // Variant constructors are also top-level names
    if (def.variants && !def.isDistinct) {
        for (const auto& variant : *def.variants) {
            // variants are TypeExprPtrs; each top-level TypeName is a constructor
            if (!variant) continue;
            if (const auto* tn = std::get_if<ast::TypeName>(&variant->kind)) {
                if (!tn->parts.empty() && std::isupper(tn->parts[0][0])) {
                    SymbolInfo ctor;
                    ctor.name = tn->parts[0];
                    ctor.kind = SymbolKind::Type;
                    ctor.definition = variant->location;
                    ctor.module = module;
                    ctor.type = Type::unknown();
                    ctor.detail = ctor.name + " : " + def.name;
                    if (!def.typeParams.empty()) {
                        ctor.detail += "<";
                        for (size_t i = 0; i < def.typeParams.size(); ++i) {
                            if (i) ctor.detail += ", ";
                            ctor.detail += def.typeParams[i];
                        }
                        ctor.detail += ">";
                    }
                    m_state->symbols.push_back(std::move(ctor));
                }
            } else if (const auto* gt = std::get_if<ast::GenericType>(&variant->kind)) {
                if (!gt->name.parts.empty()) {
                    SymbolInfo ctor;
                    ctor.name = gt->name.parts[0];
                    ctor.kind = SymbolKind::Type;
                    ctor.definition = variant->location;
                    ctor.module = module;
                    ctor.type = Type::unknown();
                    ctor.detail = ctor.name + " : ";
                    for (const auto& arg : gt->args)
                        ctor.detail += (arg ? typeExprText(*arg) : "?") + " -> ";
                    ctor.detail += def.name;
                    if (!def.typeParams.empty()) {
                        ctor.detail += "<";
                        for (size_t i = 0; i < def.typeParams.size(); ++i) {
                            if (i) ctor.detail += ", ";
                            ctor.detail += def.typeParams[i];
                        }
                        ctor.detail += ">";
                    }
                    m_state->symbols.push_back(std::move(ctor));
                }
            }
        }
    }
}

auto CollectPass::collectRecord(const ast::RecordDef& def, const std::string& module) -> void {
    SymbolInfo info;
    info.name = def.name;
    info.kind = SymbolKind::Record;
    info.definition = def.location;
    info.module = module;
    info.type = Type::unknown();
    info.detail = "record " + def.name;
    if (!def.typeParams.empty()) {
        info.detail += "<";
        for (size_t i = 0; i < def.typeParams.size(); ++i) {
            if (i) info.detail += ", ";
            info.detail += def.typeParams[i];
        }
        info.detail += ">";
    }
    info.detail += " do";
    for (const auto& field : def.fields)
        info.detail += "\n  " + field.name + " : " +
                       (field.type ? typeExprText(*field.type) : "?");
    info.detail += "\nend";
    m_state->symbols.push_back(std::move(info));

    // Fields are accessible as instance members (record.fieldName)
    for (const auto& field : def.fields) {
        SymbolInfo fi;
        fi.name = field.name;
        fi.kind = SymbolKind::Function; // treated as accessor for completion
        fi.definition = SourceLocation{std::string_view(m_state->path), 0, 0};
        fi.module = module;
        fi.makeTarget = def.name;
        fi.type = Type::unknown();
        m_state->symbols.push_back(std::move(fi));
    }
}

static auto makeTargetName(const ast::TypeExprPtr& te) -> std::string {
    if (!te) return "";
    return std::visit([](const auto& k) -> std::string {
        using T = std::decay_t<decltype(k)>;
        if constexpr (std::is_same_v<T, ast::TypeName>)
            return k.parts.empty() ? "" : k.parts[0];
        if constexpr (std::is_same_v<T, ast::GenericType>)
            return k.name.parts.empty() ? "" : k.name.parts[0];
        // List literal syntax [X] → "List", map {K:V} would need MapType
        if constexpr (std::is_same_v<T, ast::ListType>)
            return "List";
        return "";
    }, te->kind);
}

auto CollectPass::collectMake(const ast::MakeDef& def, const std::string& module) -> void {
    std::string target = makeTargetName(def.target);

    auto tagTarget = [&](const std::string& funcName) {
        if (target.empty()) return;
        for (auto& sym : m_state->symbols) {
            if (sym.name == funcName && sym.module == module && sym.makeTarget.empty())
                sym.makeTarget = target;
        }
    };

    // Index a TypeAnnotation from a make block as an instance method of `target`.
    auto collectMakeAnnotation = [&](const ast::TypeAnnotation& ann) {
        for (const auto& sym : m_state->symbols) {
            if (sym.name == ann.name && sym.makeTarget == target) return; // dedup
        }
        SymbolInfo info;
        info.name = ann.name;
        info.kind = SymbolKind::Function;
        info.definition = SourceLocation{std::string_view(m_state->path), 0, 0};
        info.module = module;
        info.isFoul = ann.isFoul;
        info.makeTarget = target;
        info.type = Type::unknown();
        m_state->symbols.push_back(std::move(info));
    };

    for (const auto& item : def.body) {
        std::visit([&](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, ast::FunctionDef>) {
                collectFunction(*ptr, module);
                tagTarget(ptr->name);
            } else if constexpr (std::is_same_v<T, ast::TypeAnnotation>) {
                collectMakeAnnotation(*ptr);
            } else if constexpr (std::is_same_v<T, ast::VisibilityBlock>) {
                for (const auto& vitem : ptr->items) {
                    std::visit([&](const auto& vptr) {
                        using VT = std::decay_t<decltype(*vptr)>;
                        if constexpr (std::is_same_v<VT, ast::FunctionDef>) {
                            collectFunction(*vptr, module);
                            tagTarget(vptr->name);
                        } else if constexpr (std::is_same_v<VT, ast::TypeAnnotation>) {
                            collectMakeAnnotation(*vptr);
                        }
                    }, vitem);
                }
            }
        }, item);
    }
}

} // namespace kex::semantic
