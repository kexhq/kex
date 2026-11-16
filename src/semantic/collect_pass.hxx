#pragma once

#include "../ast/ast.hxx"
#include "symbol_info.hxx"
#include <string>

namespace kex::semantic {

// Pass 1: collect all top-level and module-level symbol names into FileState
// before any function body is checked. This ensures forward references and
// recursive calls are always resolvable by pass 2.
class CollectPass {
public:
    // The symbols `program` declares (collectSymbols).
    auto collect(const ast::Program& program, const std::string& path) -> std::vector<SymbolInfo>;

private:
    auto collectTopLevel(const ast::TopLevelItem& item) -> void;
    auto collectModule(const ast::ModuleDef& mod) -> void;
    // `makeTarget`: the type whose `make` block declares it, "" for a free
    // function. Two types' same-named functions are two symbols.
    auto collectFunction(
        const ast::FunctionDef& def,
        const std::string& module,
        const std::string& makeTarget = std::string()) -> void;
    auto collectType(const ast::TypeDef& def, const std::string& module) -> void;
    auto collectRecord(const ast::RecordDef& def, const std::string& module) -> void;
    auto collectMake(const ast::MakeDef& def, const std::string& module) -> void;

    std::vector<SymbolInfo>* m_symbols = nullptr;
    std::string m_path;
    std::string m_currentModule;
};

// The one rule for what a program declares, and where: every function, type,
// record, trait, `make` method and annotation, with its module and whether it
// is exported. The language server indexes files with it; the checker validates
// `using` and `export` against it.
auto collectSymbols(const ast::Program& program, const std::string& path = {})
    -> std::vector<SymbolInfo>;

} // namespace kex::semantic
