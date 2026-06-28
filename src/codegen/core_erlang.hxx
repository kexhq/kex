#pragma once
#include "../ast/ast.hxx"
#include <string>
#include <vector>

namespace kex::codegen {

class CoreErlangEmitter {
public:
    struct FuncExport {
        std::string name;
        int arity;
    };

    struct EmitResult {
        std::string source;      // Core Erlang text (.core file content)
        std::string moduleName;  // e.g. "kex_hello"
    };

    // fileName should be the stem (e.g. "hello" from "hello.kex").
    // Produces a complete Core Erlang module.
    auto emitProgram(const ast::Program& prog, const std::string& fileStem) -> EmitResult;

private:
    // Top-level emitters
    auto emitFunctionDef(const ast::FunctionDef& fn) -> std::string;
    auto emitMainBlock(const ast::MainBlock& main) -> std::string;

    // Expression emitter — returns a Core Erlang expression string.
    auto emitExpr(const ast::ExprPtr& expr) -> std::string;

    // Emit a body (sequence of exprs) as a right-nested let chain.
    // The last expr is the "return value"; earlier exprs are bound to
    // fresh throwaway variables if they aren't LetExpr/VarExpr.
    auto emitBody(const std::vector<ast::ExprPtr>& body) -> std::string;

    // Pattern emission for function parameters and match clauses.
    auto emitPattern(const ast::PatternPtr& pat) -> std::string;

    // Map a Kex stdlib call to (beam_module, beam_function).
    // Returns {"", ""} if not a known stdlib target.
    auto resolveStdlib(const std::string& kexModule,
                       const std::string& kexFn) -> std::pair<std::string, std::string>;

    // Parse and emit a string literal that contains ${...} interpolation.
    auto emitInterpolatedString(const std::string& raw) -> std::string;

    // Helpers
    auto freshVar(const std::string& hint = "V") -> std::string;
    // Wrap s in single quotes if needed for a Core Erlang atom.
    static auto erlAtom(const std::string& s) -> std::string;
    // Turn a Kex lower-case name into a Core Erlang variable (uppercase first).
    static auto erlVar(const std::string& s) -> std::string;
    // Escape a string for Core Erlang string literal syntax.
    static auto erlString(const std::string& s) -> std::string;

    std::string m_moduleName;
    int m_varCounter = 0;
    std::vector<FuncExport> m_exports;
};

} // namespace kex::codegen
