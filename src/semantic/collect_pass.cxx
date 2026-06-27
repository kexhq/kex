#include "collect_pass.hxx"
#include <variant>

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

    for (const auto& item : mod.body) {
        std::visit([this](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, ast::FunctionDef>) {
                collectFunction(*ptr, m_currentModule);
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
                    std::visit([this](const auto& vptr) {
                        using VT = std::decay_t<decltype(*vptr)>;
                        if constexpr (std::is_same_v<VT, ast::FunctionDef>) {
                            collectFunction(*vptr, m_currentModule);
                        } else if constexpr (std::is_same_v<VT, ast::TypeDef>) {
                            collectType(*vptr, m_currentModule);
                        } else if constexpr (std::is_same_v<VT, ast::RecordDef>) {
                            collectRecord(*vptr, m_currentModule);
                        } else if constexpr (std::is_same_v<VT, ast::MakeDef>) {
                            collectMake(*vptr, m_currentModule);
                        }
                        // TypeAnnotation: skip (just a type sig declaration)
                    }, vitem);
                }
            }
            // UsingBlock, CompiledBlock, TypeAnnotation: skip
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
}

auto CollectPass::collectMake(const ast::MakeDef& def, const std::string& module) -> void {
    // Register functions defined inside make blocks
    for (const auto& item : def.body) {
        std::visit([this, &module](const auto& ptr) {
            using T = std::decay_t<decltype(*ptr)>;
            if constexpr (std::is_same_v<T, ast::FunctionDef>) {
                collectFunction(*ptr, module);
            } else if constexpr (std::is_same_v<T, ast::VisibilityBlock>) {
                for (const auto& vitem : ptr->items) {
                    std::visit([this, &module](const auto& vptr) {
                        using VT = std::decay_t<decltype(*vptr)>;
                        if constexpr (std::is_same_v<VT, ast::FunctionDef>) {
                            collectFunction(*vptr, module);
                        }
                    }, vitem);
                }
            }
        }, item);
    }
}

} // namespace kex::semantic
