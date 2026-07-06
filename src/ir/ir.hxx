#pragma once
// Kex in-memory lowering IR.
//
// This is the "desugar target" from docs/beam-codegen-plan.md: a small,
// normalized core calculus that sits between the (name-resolved, typed) AST
// and the Core Erlang emitter. The point is to move ALL the desugaring —
// UFCS resolution, operator lowering, loops→tail-recursion, mutable-variable
// SSA, early-return lowering — OUT of the string emitter and into explicit
// IR→IR passes, so the final IR→Core-Erlang step is mechanical.
//
// Node vocabulary intentionally mirrors docs/ir-format.md so the eventual
// `.kexo` binary writer/reader can serialize this same in-memory shape. The
// binary format itself is deliberately NOT built yet (a distribution concern,
// separate from codegen quality).
//
// Invariants the lowering pass establishes (an ANF-ish normal form):
//  - Call/Intrinsic arguments are ATOMIC (Lit or Var) — every compound
//    subexpression is Let-bound to a fresh name first. This is what makes
//    SSA construction and the emitter trivial.
//  - Every mutable `var` reassignment is a fresh Let binding (SSA); control-
//    flow joins carry the merged bindings explicitly (no hidden threading).
//  - `return` is either in tail position (the value) or an explicit Return
//    node that a later pass lowers to the target's early-exit mechanism.
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kex::ir {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

// ---- Literals -------------------------------------------------------------
enum class LitKind { Int, Float, String, Char, Bool, None, Atom };
struct Lit {
    LitKind kind;
    std::string text; // canonical source text of the value (e.g. "42", "hi")
    bool boolValue = false;
};

// ---- Atomic reference -----------------------------------------------------
struct Var {
    std::string name; // an SSA name in the emitted sense; unique within scope
};

// ---- Primitive operations (operators) ------------------------------------
enum class Op {
    Add, Sub, Mul, Div, Mod, Neg,
    Eq, Neq, Lt, Gt, Lte, Gte,
    And, Or, Not,
    Concat,
};
struct Intrinsic {
    Op op;
    std::vector<ExprPtr> args; // atomic
};

// ---- Calls ----------------------------------------------------------------
// A resolved call. `module` empty => a local function in this module
// (`apply 'name'/arity`). Non-empty => cross-module (`call 'module':'name'`),
// used for runtime/stdlib targets like kex_io:print_line.
struct Call {
    std::string module; // "" for local
    std::string name;
    int arity = 0;
    std::vector<ExprPtr> args; // atomic
    bool tail = false;         // eligible for TailCall (musttail-style)
};

// Apply a first-class function value (a Var holding a fun) to args.
struct CallIndirect {
    ExprPtr callee;            // atomic (Var)
    std::vector<ExprPtr> args; // atomic
    bool tail = false;
};

// ---- Binding / sequencing -------------------------------------------------
// let <name> = value in body   (body is the value of the whole Let)
struct Let {
    std::string name;
    ExprPtr value;
    ExprPtr body;
};

// A sequence of effectful expressions; the value is the last one. Lowering
// prefers Let (with fresh throwaway names) but Seq is kept for clarity where
// intermediate results are genuinely unused.
struct Seq {
    std::vector<ExprPtr> exprs;
};

// ---- Pattern matching -----------------------------------------------------
struct Pattern;
using PatternPtr = std::unique_ptr<Pattern>;

enum class PatKind { Lit, Var, Wild, Construct, Tuple, List };
struct Pattern {
    PatKind kind;
    // Lit
    LitKind litKind{};
    std::string litText;
    bool litBool = false;
    // Var
    std::string name;
    // Construct: tag + args ; Tuple/List: args
    std::string tag;
    std::vector<PatternPtr> args;
    // List: the `| rest` tail pattern, if any (else a proper list).
    PatternPtr rest;
};

struct MatchClause {
    std::vector<PatternPtr> patterns; // one per subject value (usually 1)
    std::optional<ExprPtr> guard;
    ExprPtr body;
};
struct Match {
    std::vector<ExprPtr> subjects; // usually 1; >1 for multi-value dispatch
    std::vector<MatchClause> clauses;
};

// ---- Data -----------------------------------------------------------------
// Tagged tuple: {'Tag', args...}. Records and ADT variants share this.
struct Construct {
    std::string tag;
    std::vector<ExprPtr> args; // atomic
};
// Untagged tuple: {a, b, ...}.
struct MakeTuple {
    std::vector<ExprPtr> elements; // atomic
};
// List: [a, b | rest].  rest empty => proper list [a, b].
struct MakeList {
    std::vector<ExprPtr> elements;  // atomic
    std::optional<ExprPtr> rest;    // atomic
};
// Read field at a fixed 1-based tuple position (resolved from the record type).
struct FieldGet {
    ExprPtr record; // atomic
    int position;   // 1-based erlang element index (tag is 1)
    std::string fieldName; // for debugging/dump only
};

// ---- Functions ------------------------------------------------------------
struct Lambda {
    std::vector<std::string> params;
    ExprPtr body;
};

// ---- Control --------------------------------------------------------------
// Early return. A later pass lowers this to throw/try-catch on BEAM.
struct Return {
    ExprPtr value;
};

// A selective `receive`. Each message is the wire tuple {'kex_msg', Payload,
// Sender}; a clause matches Payload with `pattern` (and optional guard), with
// `senderVar` bound to the sender pid. Emits Core Erlang's native `receive`.
struct ReceiveClause {
    PatternPtr pattern; // matched against the payload
    std::optional<ExprPtr> guard;
    ExprPtr body;
};
struct Receive {
    std::vector<ReceiveClause> clauses;
    std::string senderVar;              // sender pid binding (fresh if unused)
    std::optional<ExprPtr> timeout;     // `after <timeout> -> afterBody`
    std::optional<ExprPtr> afterBody;
};

// A local tail-recursive function (loops lower to this). `name(params) =
// funBody` is in scope for both funBody (recursion) and contBody (the entry
// call + continuation). Emits Core Erlang `letrec 'name'/N = fun(...) ->
// funBody in contBody`.
struct LetRec {
    std::string name;
    std::vector<std::string> params;
    ExprPtr funBody;
    ExprPtr contBody;
};

// ---- Expr variant ---------------------------------------------------------
struct Expr {
    std::variant<
        Lit, Var, Intrinsic, Call, CallIndirect,
        Let, Seq, Match, Construct, MakeTuple, MakeList, FieldGet, Lambda, Return, LetRec, Receive
    > node;
};

// ---- Top level ------------------------------------------------------------
struct FunClause {
    std::vector<PatternPtr> params;
    std::optional<ExprPtr> guard;
    ExprPtr body;
};
struct FunDef {
    std::string name;
    int arity = 0;
    std::vector<FunClause> clauses;
    bool exported = true;
};

struct Module {
    std::string name; // erlang module name, e.g. "kex_simple_fact"
    std::vector<FunDef> functions;
    int mainArity = 0; // 0 = none / main/0, 1 = main/1
    bool hasMain = false;
};

// Small constructor helpers (keep lowering code readable).
inline auto lit(LitKind k, std::string text) -> ExprPtr {
    auto e = std::make_unique<Expr>();
    e->node = Lit{k, std::move(text), false};
    return e;
}
inline auto litBool(bool b) -> ExprPtr {
    auto e = std::make_unique<Expr>();
    e->node = Lit{LitKind::Bool, b ? "true" : "false", b};
    return e;
}
inline auto var(std::string name) -> ExprPtr {
    auto e = std::make_unique<Expr>();
    e->node = Var{std::move(name)};
    return e;
}

} // namespace kex::ir
