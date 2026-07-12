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
    // emitNameOverride, when non-empty, is used as the emitted Erlang
    // function's name instead of group[0]->name — see emitProgram's
    // cross-type method-name-collision mangling (`add` -> `add__Vec2`).
    auto emitFunctionGroup(const std::vector<const ast::FunctionDef*>& group,
                           bool hasImplicitThis = false,
                           const std::string& emitNameOverride = "") -> std::string;
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

    // Identifies an enclosing loop's own letrec function + threaded state,
    // for when a loop is nested inside another loop's body (see
    // emitLoopExpr's `outerLoop` parameter).
    struct LoopContext {
        std::string loopFn;
        int loopArity;
        std::vector<std::string> mutParams;
        std::string fallthrough;
    };

    // Emit a loop body (loop/while) as a letrec tail-recursive function.
    // loopBody: the loop's statement list.
    // condition: non-null for while loops, null for infinite loop.
    // outerBody/outerStart: the enclosing body slice after the loop statement.
    // outerLoop: non-null when this loop is itself nested inside another
    // loop's body — "falling off the end of outerBody" must then continue
    // via the OUTER loop's own emitLoopBodyFrom (tail-call/fallthrough),
    // not emitBodyFrom's plain "return the final value" behavior, which
    // only makes sense at true top level. A real, reproduced bug otherwise:
    // `loop \n loop \n ... \n end \n end` (or `while` nested either way)
    // hit codegen's "unimplemented AST node" catch-all, since
    // emitLoopBodyFrom had no dispatch case for a nested LoopExpr/WhileExpr
    // at all.
    auto emitLoopExpr(const std::vector<ast::ExprPtr>& loopBody,
                      const ast::ExprPtr* condition,
                      const std::vector<ast::ExprPtr>& outerBody, int outerStart,
                      const LoopContext* outerLoop = nullptr) -> std::string;

    // Emit loop body statements forward in a loop context.
    // mutParams: kex names of loop-threaded variables (in order).
    // loopFn: the letrec function name.  loopArity: its arity.
    // `fallthrough`, when non-empty, is the Core Erlang expression to splice
    // in once `body[start:]` is exhausted, instead of tail-calling the loop
    // function again — needed when `body` is actually an `if`'s own
    // then/else statement list (a different vector from the enclosing loop
    // body): falling off the end of an if-branch must continue with
    // whatever follows the `if` in the loop body, not immediately restart
    // the loop (see emitLoopExpr's IfExpr handling — this was a real,
    // reproduced bug: `loop \n if true \n ... \n end \n break \n end`
    // never reached `break` at all, looping forever, since the `if`'s own
    // then-body falling through was wrongly treated as "next iteration").
    auto emitLoopBodyFrom(const std::vector<ast::ExprPtr>& body, int start,
                          const std::string& loopFn, int loopArity,
                          const std::vector<std::string>& mutParams,
                          const std::string& fallthrough = "") -> std::string;

    // Build the tail-call string using current m_varSubst values.
    auto makeTailCall(const std::string& loopFn, int loopArity,
                      const std::vector<std::string>& mutParams) -> std::string;

    // Collect all reassigned var names in a body — plain `x = ...` and
    // mutating `x.push!(..)` calls alike (not crossing lambda/function
    // boundaries). collectAssignedExpr handles a single expression.
    static void collectAssigned(const std::vector<ast::ExprPtr>& body,
                                 std::unordered_set<std::string>& out);
    static void collectAssignedExpr(const ast::ExprPtr& e,
                                    std::unordered_set<std::string>& out);

    // Generate let-bindings that destructure a pattern against a source expr.
    // For a VarPattern: `let <Var> = src in`
    // For a RecordPattern { age }: `let <Age> = apply 'age'/1(src) in` (or element() if position known)
    // For a RecordPattern { address: { city } }: extract address, then city from that
    auto bindPatternLets(const ast::PatternPtr& pat, const std::string& src) -> std::string;

    // Helpers
    auto freshVar(const std::string& hint = "V") -> std::string;
    // Wrap s in single quotes if needed for a Core Erlang atom.
    static auto erlAtom(const std::string& s) -> std::string;
    // Turn a Kex lower-case name into a Core Erlang variable (uppercase first).
    static auto erlVar(const std::string& s) -> std::string;
    // Escape a string for Core Erlang string literal syntax.
    static auto erlString(const std::string& s) -> std::string;

    // True while emitting the body of a function that contains an early
    // `return` (see bodyHasReturn). When set, a `return X` compiles to
    // `throw({'kex_return', X})` and the whole body is wrapped in a
    // try/catch that turns that throw back into the function's result —
    // the same mechanism the tree-walker's ReturnException uses, and the
    // only way to make a `return` buried inside a match/if arm (whose
    // value is consumed by an enclosing `let`) actually exit the function
    // rather than fall through as that arm's value (examples/json_parser.kex).
    bool m_returnThrows = false;

    // Emit `v` as a return: a throw when m_returnThrows, else the bare value
    // (the legacy tail-position behavior for functions with no early return).
    auto emitReturnValue(const ast::ExprPtr& v) -> std::string;

    // Does this body / expression contain an early `return` (not crossing
    // into a nested lambda, which has its own return scope)?
    static auto bodyHasReturn(const std::vector<ast::ExprPtr>& body) -> bool;
    static auto exprHasReturn(const ast::ExprPtr& e) -> bool;

    // Wrap a fully-emitted function body in the kex_return try/catch (only
    // used when m_returnThrows was set for it).
    auto wrapReturnCatch(const std::string& body) -> std::string;

    // Emit `stmts[idx..]` as a let-chain ending in a value that carries the
    // final SSA names of `mergeVars` (a bare var when there's one, else a
    // tuple). Threads assignments/mutations into m_varSubst as it goes;
    // the caller snapshots/restores. Used by emitBodyFrom's statement-level
    // conditional-assignment merge, so a `var` reassigned inside an `if`
    // branch is visible to code after the `if` (examples/json_parser.kex's
    // parseNumber consumes a leading `-` in an `if` before its digit loop).
    auto emitBranchResult(const std::vector<ast::ExprPtr>& stmts, int idx,
                          const std::vector<std::string>& mergeVars) -> std::string;

    // True while emitting a match-clause `when` guard. Core Erlang guards
    // may only use guard-safe operations — no arbitrary function calls —
    // so predicates that normally emit a `call 'kex_io':...` (digit?/
    // alpha?/space?) must instead expand to an inline guard-safe boolean
    // expression when this is set (see emitExpr's MethodCall handling).
    bool m_inGuard = false;

    std::string m_moduleName;
    int m_varCounter = 0;
    int m_loopCounter = 0;
    std::vector<FuncExport> m_exports;
    // kex name → current Core Erlang variable name (for mutable var SSA).
    std::unordered_map<std::string, std::string> m_varSubst;
    // name → arity for all top-level functions defined in this module.
    std::unordered_map<std::string, int> m_topLevelFns;
    // name → ordered parameter names (first clause) for top-level free
    // functions, so a call with named args (`f(b: 2, a: 1)`) or a trailing
    // do-block can reorder those args into positional slots by param name
    // (see emitExpr's FunctionCall reordering). "" for an unnamed/pattern
    // param — such a slot can only ever be filled positionally.
    std::unordered_map<std::string, std::vector<std::string>> m_topLevelParams;
    // "TypeName::ConstName" → mangled function name for 0-arity static constants.
    std::unordered_map<std::string, std::string> m_staticCtors;
    // field_name → [(record_name, 1-based tuple position)]
    // Used to generate direct element() calls during field destructuring.
    std::unordered_map<std::string, std::vector<std::pair<std::string,int>>> m_fieldAccessors;
    // record_name → its RecordDef, so RecordConstruction can emit fields in
    // DECLARED order and fill in defaults for omitted fields (the tuple
    // layout must match the fixed positions the field accessors assume).
    std::unordered_map<std::string, const ast::RecordDef*> m_records;
};

} // namespace kex::codegen
