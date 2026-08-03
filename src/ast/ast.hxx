#pragma once

#include "../lexer/token.hxx"
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace kex::ast {

struct Expr;
struct Pattern;
struct TypeExpr;

using ExprPtr = std::unique_ptr<Expr>;
using PatternPtr = std::unique_ptr<Pattern>;
using TypeExprPtr = std::unique_ptr<TypeExpr>;

// ===== Type Expressions =====

struct TypeName {
    std::vector<std::string> parts; // e.g. ["Html", "Element"]
};

struct GenericType {
    TypeName name;
    std::vector<TypeExprPtr> args;
};

struct FunctionType {
    TypeExprPtr param;
    TypeExprPtr result;
};

struct TupleType {
    std::vector<TypeExprPtr> elements;
};

struct ListType {
    TypeExprPtr element;
};

struct MapType {
    TypeExprPtr key;
    TypeExprPtr value;
};

struct UnionType {
    TypeExprPtr left;
    TypeExprPtr right;
};

struct OptionalType {
    TypeExprPtr inner;
};

struct BlockType {
    TypeExprPtr inner;
};

struct AtomType {
    std::string name;
};

struct GenericVar {
    std::string name; // single letter
};

// A type the COMPILER computes: `type Row = Type.returnedBy(parseRow)`,
// `type Item = Type.of(defaultItem)`. The argument is an ordinary expression —
// a function name for `returnedBy`, any expression for `of` — resolved during
// checking and erased afterwards, like every other type.
struct TypeQuery {
    std::string query;   // "of" | "returnedBy"
    ExprPtr argument;
};

struct TypeExpr {
    SourceLocation location;
    std::variant<
        TypeName,
        GenericType,
        FunctionType,
        TupleType,
        ListType,
        MapType,
        UnionType,
        OptionalType,
        BlockType,
        AtomType,
        GenericVar,
        TypeQuery
    > kind;
};

// ===== Patterns =====

struct LiteralPattern {
    Token literal;
};

struct VarPattern {
    std::string name;
};

struct WildcardPattern {};

struct ThisPattern {
    PatternPtr inner;
};

struct ConstructorPattern {
    std::string name;
    std::vector<PatternPtr> args;
};

struct FieldPattern {
    std::string name;
    std::optional<PatternPtr> pattern; // None = shorthand binding
    bool isStringKey = false;
};

struct RecordPattern {
    // Optional type name for a *named* record pattern (`Foo { x, y }`). When
    // absent, the pattern is purely structural — it matches any record (or map)
    // carrying the listed fields. When present, the match also asserts the
    // value's record type, so `ParseError { value, rest }` won't match any
    // other record that happens to share those field names.
    std::string typeName;
    std::vector<FieldPattern> fields;
};

struct ListPattern {
    std::vector<PatternPtr> elements;
    std::optional<PatternPtr> rest; // the | tail part
};

struct TuplePattern {
    std::vector<PatternPtr> elements;
};

struct RangePattern {
    PatternPtr start;
    PatternPtr end;
};

struct Pattern {
    SourceLocation location;
    std::variant<
        LiteralPattern,
        VarPattern,
        WildcardPattern,
        ThisPattern,
        ConstructorPattern,
        RecordPattern,
        ListPattern,
        TuplePattern,
        RangePattern
    > kind;
};

// ===== Expressions =====

struct IntLiteral { std::string value; };
struct FloatLiteral { std::string value; };
struct StringLiteral {
    std::string value;
    bool interpolating = true;
    std::vector<std::string> parts;
    std::vector<ExprPtr> values;
};
struct CharLiteral { char value; };
struct BoolLiteral { bool value; };
struct NoneLiteral {};
struct AtomLiteral { std::string name; };

struct Identifier { std::string name; };
struct UpperIdentifier { std::string name; };
struct ThisExpr {};

struct MethodCall {
    ExprPtr receiver;
    std::string method;
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> namedArgs;
    std::optional<ExprPtr> block;
    bool mutating = false; // the ! suffix
    // Whether the source wrote `()`. `Time.midnight` and `Time.midnight()`
    // are otherwise the same node — a zero-argument MethodCall — and
    // `Type.of` has to tell a function REFERENCE from a call to it.
    bool parenthesized = false;
};

struct FunctionCall {
    std::string name;
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> namedArgs;
    std::optional<ExprPtr> block;
};

// Kept distinct from FunctionCall until semantic analysis has resolved and
// checked the tag ABI. Backends lower it to tag(parts, values) afterward.
struct TaggedLiteral {
    std::string tag;
    std::vector<std::string> parts;
    std::vector<ExprPtr> values;
    bool interpolating = false;
    int bodyStartOffset = -1;
    int bodyEndOffset = -1;
};

struct RecordConstruction {
    std::string typeName;
    std::vector<std::pair<std::string, ExprPtr>> fields;
};

struct BinaryOp {
    ExprPtr left;
    TokenType op;
    ExprPtr right;
};

struct UnaryOp {
    TokenType op;
    ExprPtr operand;
};

struct TupleExpr {
    std::vector<ExprPtr> elements;
};

struct ListExpr {
    std::vector<ExprPtr> elements;
    std::optional<ExprPtr> rest; // [x | rest] construction
};

struct MapEntry {
    ExprPtr key;
    ExprPtr value;
    // `{ ...other, "k": 1 }` — `value` is the map being spread and `key` is
    // null. Later entries win, so a spread can be overridden by what follows.
    bool spread = false;
};

struct MapExpr {
    std::vector<MapEntry> entries;
};

struct RangeExpr {
    ExprPtr start;
    ExprPtr end;
};

struct IfExpr {
    ExprPtr condition;
    std::vector<ExprPtr> thenBody;
    std::vector<std::pair<ExprPtr, std::vector<ExprPtr>>> elifs;
    std::optional<std::vector<ExprPtr>> elseBody;
    // `if let Pattern = expr` — set when the condition is a pattern match.
    // When set, `condition` holds the scrutinee expression and `letPattern`
    // holds the pattern to match against it.
    PatternPtr letPattern;
};

struct MatchClause {
    std::vector<PatternPtr> patterns;
    std::optional<ExprPtr> guard;
    ExprPtr body;
};

struct MatchExpr {
    ExprPtr subject;
    std::optional<std::string> subjectBinding; // |n|
    std::vector<MatchClause> clauses;
};

struct ReceiveExpr {
    std::vector<MatchClause> clauses;
    std::optional<ExprPtr> timeout;
    std::optional<ExprPtr> afterBody;
    // When present, every message is expected to be a 2-tuple {Payload, Sender}
    // and this name is bound to the Sender pid for each clause. Patterns in
    // `clauses` match against the Payload only.
    std::optional<std::string> senderBinding;
};

struct LoopExpr {
    std::vector<ExprPtr> body;
};

struct WhileExpr {
    ExprPtr condition;
    std::vector<ExprPtr> body;
};

struct BreakExpr {};
struct NextExpr {};

struct LetExpr {
    PatternPtr pattern;
    ExprPtr value;
    // `let x: String? = None` — the declared type, when written. The checker
    // binds THIS rather than what the initializer happens to infer to, which
    // is the point of writing it (`let xs: [Colour] = []`).
    std::optional<TypeExprPtr> type;
};

struct VarExpr {
    std::string name;
    ExprPtr value;
    std::optional<TypeExprPtr> type;
};

struct AssignExpr {
    std::string name;
    ExprPtr value;
};

struct ReturnExpr {
    ExprPtr value;
};

struct SpawnExpr {
    std::vector<ExprPtr> body;
};

struct TryExpr {
    ExprPtr operand;
};

struct RescueBlock {
    std::vector<MatchClause> clauses;
    bool isCatchAll = false;
    std::string catchAllParam;
    std::vector<ExprPtr> catchAllBody;
    bool isInlineReturn = false;
    ExprPtr inlineReturnExpr;
};

struct LambdaParam {
    std::string name;
    std::optional<TypeExprPtr> type;
};

struct Lambda {
    std::vector<LambdaParam> params;
    std::vector<ExprPtr> body;
    std::optional<TypeExprPtr> returnAnnotation;
    std::optional<RescueBlock> rescue;
};

// `&.method` / `&.method(args)` — receiver shorthand. Capturing a named
// function or an operator is spelled with `~` (CurryExpr), never `&`.
struct ShorthandLambda {
    enum class Kind { Method, MethodWithArgs };
    Kind kind;
    std::string name;
    std::vector<ExprPtr> args;
};

struct SpreadExpr {
    ExprPtr inner;
};

struct TrailingIf {
    ExprPtr expr;
    ExprPtr condition;
};

struct ThenElseExpr {
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
};

struct CurryPlaceholder {};

struct CurryExpr {
    std::string name;       // function or operator name ("add", "+", etc.)
    bool isOperator;        // true for ~(+), ~(*), etc.
    std::vector<std::vector<ExprPtr>> argGroups; // each (args) paren group
    std::string module;     // namespace for ~Mod.fn; empty when unqualified
};

struct BlockExpr {
    std::vector<ExprPtr> body;
};

struct TryingExpr {
    std::vector<ExprPtr> body;
    RescueBlock rescue;
};

// Placeholder inserted by the parser at a recovery point. Carries the
// original error message so downstream passes can skip this node without
// re-emitting the diagnostic.
struct ErrorNode {
    std::string message;
};

// `using Module` or `using Module do ... end` inside a function/main body.
// Brings the module's compiled-block definitions into the current scope.
// `let %name(...) -> T do ... end` inside a `compiled do` block: a declaration
// whose NAME is computed at compile time.
//
// It is an EXPRESSION rather than a declaration form so that generation can be
// driven by ordinary code — `NAMES.eachIndexed do |n, i| let %n(...) ... end`
// needs no new statement grammar, and loops, nesting and conditionals all work
// because the compile-time sandbox simply runs them. Evaluating one produces no
// value; it RECORDS a declaration for the expansion pass to splice in.
// The declaration a splice generates. Exactly one alternative is set; the
// variant is what keeps that an invariant rather than a convention.
// shared_ptr because one template yields N declarations, each an independent
// ast::clone of it.
using GeneratedTemplate = std::variant<std::shared_ptr<struct FunctionDef>,
                                       std::shared_ptr<struct TypeDef>,
                                       std::shared_ptr<struct MakeDef>>;

struct GeneratedDecl {
    ExprPtr name;                           // `%n` -> Identifier{"n"}
    GeneratedTemplate function;
};

struct UsingExpr {
    TypeName module;
    std::optional<std::string> alias;
    std::vector<std::string> onlyNames;
    std::vector<std::string> exceptNames;
    std::vector<ExprPtr> body; // empty for bare `using Module`
};

struct Expr {
    SourceLocation location;
    std::variant<
        IntLiteral,
        FloatLiteral,
        StringLiteral,
        CharLiteral,
        BoolLiteral,
        NoneLiteral,
        AtomLiteral,
        Identifier,
        UpperIdentifier,
        ThisExpr,
        MethodCall,
        FunctionCall,
        TaggedLiteral,
        RecordConstruction,
        BinaryOp,
        UnaryOp,
        TupleExpr,
        ListExpr,
        MapExpr,
        RangeExpr,
        IfExpr,
        MatchExpr,
        ReceiveExpr,
        LoopExpr,
        WhileExpr,
        BreakExpr,
        NextExpr,
        LetExpr,
        VarExpr,
        AssignExpr,
        ReturnExpr,
        SpawnExpr,
        Lambda,
        ShorthandLambda,
        SpreadExpr,
        TrailingIf,
        ThenElseExpr,
        BlockExpr,
        CurryPlaceholder,
        CurryExpr,
        TryExpr,
        TryingExpr,
        ErrorNode,
        GeneratedDecl,
        UsingExpr
    > kind;
};

// ===== Top-Level Declarations =====

struct TypeAnnotation {
    std::string name;
    TypeExprPtr type;
    bool implicitThis; // :> vs :
    bool isFoul = false;
};

struct Param {
    std::optional<PatternPtr> pattern;
    std::optional<std::string> name;
    std::optional<TypeExprPtr> type;
    std::optional<ExprPtr> defaultValue;
};

struct FunctionClause {
    std::vector<Param> params;
    std::vector<ExprPtr> body;
    std::optional<TypeExprPtr> returnAnnotation;
    std::optional<RescueBlock> rescue;
    // Whether the source wrote a parameter list at all. `let x = 3` and
    // `let x() = 3` are otherwise the same node — an empty `params` — and they
    // are NOT the same thing: the first is a value binding, the second a
    // nullary function. `compiled do` has to tell them apart, since it
    // evaluates bindings at compile time and leaves functions alone. Same
    // distinction MethodCall::parenthesized draws on the call side.
    bool hasParamList = false;
};

struct FunctionDef {
    SourceLocation location;
    std::string name;
    // Set only while parsing `let %{expr}(...)`: the compile-time expression
    // that computes this declaration's name. Transient — parseLetExpr moves it
    // into a GeneratedDecl and clears it, so it never reaches a real
    // declaration or any backend.
    ExprPtr computedName;
    bool isFoul = false;
    bool isPredicate = false; // ends with ?
    std::vector<FunctionClause> clauses;
};

struct AbstractFunction {
    std::string name;
    TypeExprPtr type;
    bool implicitThis;
};

struct StaticBlock {
    std::vector<std::unique_ptr<FunctionDef>> functions;
};

struct TypeDef {
    SourceLocation location;
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<TypeName> parents;
    std::optional<std::vector<TypeExprPtr>> variants; // sum type
    std::optional<std::vector<AbstractFunction>> abstractFunctions;
    std::optional<StaticBlock> staticBlock;
};

struct RecordField {
    std::string name;
    TypeExprPtr type;
    std::optional<ExprPtr> defaultValue;
};

struct RecordDef {
    SourceLocation location;
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<RecordField> fields;
    std::optional<StaticBlock> staticBlock;
};

struct UsingBlock;

struct VisibilityBlock {
    bool isPublic;
    std::vector<std::variant<
        std::unique_ptr<FunctionDef>,
        std::unique_ptr<TypeAnnotation>,
        std::unique_ptr<struct MakeDef>,
        std::unique_ptr<TypeDef>,
        std::unique_ptr<RecordDef>,
        std::unique_ptr<UsingBlock>
    >> items;
};

struct TraitDef {
    SourceLocation location;
    std::string name;
    std::vector<std::string> typeParams;
    // required method signatures (TypeAnnotation) and optional default
    // implementations (FunctionDef) — mixed order is allowed.
    std::vector<std::variant<
        std::unique_ptr<TypeAnnotation>,
        std::unique_ptr<FunctionDef>
    >> body;
};

struct MakeDef {
    SourceLocation location;
    TypeExprPtr target;
    bool isFinal = false;
    std::vector<std::string> implements;  // trait names claimed by this block
    // ExprPtr is only ever produced for a GENERATED `make %name do ... end`,
    // where the body may hold a driver loop declaring methods
    // (`OPS.each do |op| let %op(...) ... end end`). Expansion evaluates those
    // and replaces them with the methods they declare, so no `make` that
    // reaches a backend ever contains one — the ordinary `make Foo do ... end`
    // parser never produces one at all.
    std::vector<std::variant<
        std::unique_ptr<FunctionDef>,
        std::unique_ptr<TypeAnnotation>,
        std::unique_ptr<VisibilityBlock>,
        ExprPtr
    >> body;
};

// Items that can appear inside a compiled do...end block.
// Mirrors the subset of module items that carry runtime-evaluable semantics.
using CompiledItem = std::variant<
    std::unique_ptr<FunctionDef>,
    std::unique_ptr<MakeDef>,
    std::unique_ptr<RecordDef>,
    std::unique_ptr<TypeDef>,
    ExprPtr  // constant assignments (NAME = value) and other expr stmts
>;

struct CompiledBlock {
    SourceLocation location;
    std::vector<CompiledItem> items;
};

struct UsingBlock {
    SourceLocation location;
    TypeName module;
    std::optional<std::string> alias;
    std::vector<std::string> onlyNames;
    std::vector<std::string> exceptNames;
    std::vector<ExprPtr> body;
};

struct ExportDecl {
    SourceLocation location;
    TypeName module;
    std::optional<std::string> alias;
    std::vector<std::string> onlyNames;
    std::vector<std::string> exceptNames;
};

struct MainBlock {
    SourceLocation location;
    std::vector<Param> params;
    std::vector<ExprPtr> body;
    // True for synthetic wrappers created by parseTopLevelItem around plain
    // `let x = expr` bindings — these should NOT push a new env scope so
    // that bound names remain visible to subsequent top-level items.
    bool synthetic = false;
    // True only for explicit `main do ... end` blocks (parsed by parseMainBlock).
    // False for synthetic let-wrappers and bare top-level expression wrappers.
    bool isExplicitMain = false;
    std::optional<RescueBlock> rescue;
};

struct Pragma {
    SourceLocation location;
    std::vector<std::string> requirements;
};

struct ModuleDef;

using ModuleItem = std::variant<
    std::unique_ptr<ModuleDef>,
    std::unique_ptr<TypeDef>,
    std::unique_ptr<RecordDef>,
    std::unique_ptr<TraitDef>,
    std::unique_ptr<MakeDef>,
    std::unique_ptr<FunctionDef>,
    std::unique_ptr<CompiledBlock>,
    std::unique_ptr<VisibilityBlock>,
    std::unique_ptr<UsingBlock>,
    std::unique_ptr<ExportDecl>,
    std::unique_ptr<TypeAnnotation>
>;

struct ModuleDef {
    SourceLocation location;
    std::string name;
    bool isFoul = false;
    std::vector<ModuleItem> body;
};

using TopLevelItem = std::variant<
    std::unique_ptr<ModuleDef>,
    std::unique_ptr<TypeDef>,
    std::unique_ptr<RecordDef>,
    std::unique_ptr<TraitDef>,
    std::unique_ptr<MakeDef>,
    std::unique_ptr<FunctionDef>,
    std::unique_ptr<CompiledBlock>,
    std::unique_ptr<UsingBlock>,
    std::unique_ptr<MainBlock>,
    std::unique_ptr<Pragma>,
    std::unique_ptr<TypeAnnotation>
>;

struct Program {
    std::vector<TopLevelItem> items;
};

} // namespace kex::ast
