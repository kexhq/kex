#include "collect_pass.hxx"
#include <variant>
#include <cctype>

namespace kex::semantic {

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
            info.kind = SymbolKind::Type;
            info.definition = ptr->location;
            info.module = "";
            info.type = Type::unknown();
            m_state->symbols.push_back(std::move(info));
        } else if constexpr (std::is_same_v<T, ast::TypeAnnotation>) {
            // Standalone type signature declaration (e.g. in prelude files):
            // `describe : (String, Block<Unit>) -> Unit`
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
    // Register the module itself
    SymbolInfo moduleInfo;
    moduleInfo.name = mod.name;
    moduleInfo.kind = SymbolKind::Module;
    moduleInfo.definition = mod.location;
    moduleInfo.module = "";
    moduleInfo.isFoul = mod.isFoul;
    moduleInfo.type = Type::unknown();
    m_state->symbols.push_back(std::move(moduleInfo));

    std::string savedModule = m_currentModule;
    m_currentModule = mod.name;

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
                info.kind = SymbolKind::Type;
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
    m_state->symbols.push_back(std::move(info));

    // Variant constructors are also top-level names
    if (def.variants) {
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

    // Static block functions are callable as RecordName.FuncName(...)
    if (def.staticBlock) {
        for (const auto& fn : def.staticBlock->functions) {
            SymbolInfo si;
            si.name = fn->name;
            si.kind = SymbolKind::Function;
            si.definition = fn->location;
            si.module = module;
            si.makeTarget = def.name;
            si.isFoul = fn->isFoul;
            si.type = Type::unknown();
            m_state->symbols.push_back(std::move(si));
        }
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
