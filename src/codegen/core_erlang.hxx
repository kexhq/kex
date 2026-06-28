#pragma once
#include "../ast/ast.hxx"
#include <string>
#include <unordered_map>
#include <unordered_set>
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
        int mainArity = 0;       // 0 = main/0, 1 = main/1 (args)
    };

    // fileName should be the stem (e.g. "hello" from "hello.kex").
    // Produces a complete Core Erlang module.
    auto emitProgram(const ast::Program& prog, const std::string& fileStem) -> EmitResult;

private:
    // Top-level emitters
    auto emitFunctionDef(const ast::FunctionDef& fn) -> std::string;
    // Emit a group of same-name FunctionDef nodes as a single function.
    auto emitFunctionGroup(const std::vector<const ast::FunctionDef*>& group,
                           bool hasImplicitThis = false) -> std::string;
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

    // Forward-recursive body emitter used by emitBody.
    auto emitBodyFrom(const std::vector<ast::ExprPtr>& body, int start) -> std::string;

    // Emit a loop body (loop/while) as a letrec tail-recursive function.
    // loopBody: the loop's statement list.
    // condition: non-null for while loops, null for infinite loop.
    // outerBody/outerStart: the enclosing body slice after the loop statement.
    auto emitLoopExpr(const std::vector<ast::ExprPtr>& loopBody,
                      const ast::ExprPtr* condition,
                      const std::vector<ast::ExprPtr>& outerBody, int outerStart) -> std::string;

    // Emit loop body statements forward in a loop context.
    // mutParams: kex names of loop-threaded variables (in order).
    // loopFn: the letrec function name.  loopArity: its arity.
    auto emitLoopBodyFrom(const std::vector<ast::ExprPtr>& body, int start,
                          const std::string& loopFn, int loopArity,
                          const std::vector<std::string>& mutParams) -> std::string;

    // Build the tail-call string using current m_varSubst values.
    auto makeTailCall(const std::string& loopFn, int loopArity,
                      const std::vector<std::string>& mutParams) -> std::string;

    // Collect all AssignExpr names in a body (not crossing lambda/function boundaries).
    static void collectAssigned(const std::vector<ast::ExprPtr>& body,
                                 std::unordered_set<std::string>& out);

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
    int m_loopCounter = 0;
    std::vector<FuncExport> m_exports;
    // kex name → current Core Erlang variable name (for mutable var SSA).
    std::unordered_map<std::string, std::string> m_varSubst;
    // name → arity for all top-level functions defined in this module.
    std::unordered_map<std::string, int> m_topLevelFns;
    // "TypeName::ConstName" → mangled function name for 0-arity static constants.
    std::unordered_map<std::string, std::string> m_staticCtors;
    // field_name → [(record_name, 1-based tuple position)]
    // Used to generate direct element() calls during field destructuring.
    std::unordered_map<std::string, std::vector<std::pair<std::string,int>>> m_fieldAccessors;
};

} // namespace kex::codegen
