#pragma once

#include "../ast/ast.hxx"
#include "db.hxx"
#include <string>

namespace kex::semantic {

// Pass 1: collect all top-level and module-level symbol names into FileState
// before any function body is checked. This ensures forward references and
// recursive calls are always resolvable by pass 2.
class CollectPass {
public:
    auto run(SemanticDB& db, const std::string& file) -> void;

private:
    auto collectTopLevel(const ast::TopLevelItem& item) -> void;
    auto collectModule(const ast::ModuleDef& mod) -> void;
    auto collectFunction(const ast::FunctionDef& def, const std::string& module) -> void;
    auto collectType(const ast::TypeDef& def, const std::string& module) -> void;
    auto collectRecord(const ast::RecordDef& def, const std::string& module) -> void;
    auto collectMake(const ast::MakeDef& def, const std::string& module) -> void;

    FileState* m_state = nullptr;
    std::string m_currentModule;
};

} // namespace kex::semantic
