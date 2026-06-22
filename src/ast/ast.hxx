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
        GenericVar
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
    std::vector<FieldPattern> fields;
};

struct ListPattern {
    std::vector<PatternPtr> elements;
    std::optional<PatternPtr> rest; // the | tail part
};

struct TuplePattern {
    std::vector<PatternPtr> elements;
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
        TuplePattern
    > kind;
};

// ===== Expressions =====

struct IntLiteral { std::string value; };
struct FloatLiteral { std::string value; };
struct StringLiteral { std::string value; };
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
};

struct FunctionCall {
    std::string name;
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> namedArgs;
    std::optional<ExprPtr> block;
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

struct ErrorPropagate {
    ExprPtr inner;
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
};

struct MatchClause {
    std::vector<PatternPtr> patterns;
    std::optional<ExprPtr> guard;
    ExprPtr body;
};

struct MatchExpr {
    ExprPtr subject;
    std::vector<MatchClause> clauses;
};

struct ReceiveExpr {
    std::vector<MatchClause> clauses;
    std::optional<ExprPtr> timeout;
    std::optional<ExprPtr> afterBody;
};

struct LoopExpr {
    std::vector<ExprPtr> body;
};

struct LetExpr {
    PatternPtr pattern;
    ExprPtr value;
};

struct VarExpr {
    std::string name;
    ExprPtr value;
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

struct LambdaParam {
    std::string name;
    std::optional<TypeExprPtr> type;
};

struct Lambda {
    std::vector<LambdaParam> params;
    std::vector<ExprPtr> body;
};

struct ShorthandLambda {
    enum class Kind { Method, MethodWithArgs, Function };
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

struct BlockExpr {
    std::vector<ExprPtr> body;
};

struct Expr {
    SourceLocation location;
    std::variant<
        IntLiteral,
        FloatLiteral,
        StringLiteral,
        BoolLiteral,
        NoneLiteral,
        AtomLiteral,
        Identifier,
        UpperIdentifier,
        ThisExpr,
        MethodCall,
        FunctionCall,
        RecordConstruction,
        BinaryOp,
        UnaryOp,
        ErrorPropagate,
        TupleExpr,
        ListExpr,
        MapExpr,
        RangeExpr,
        IfExpr,
        MatchExpr,
        ReceiveExpr,
        LoopExpr,
        LetExpr,
        VarExpr,
        AssignExpr,
        ReturnExpr,
        SpawnExpr,
        Lambda,
        ShorthandLambda,
        SpreadExpr,
        TrailingIf,
        BlockExpr
    > kind;
};

// ===== Top-Level Declarations =====

struct TypeAnnotation {
    std::string name;
    TypeExprPtr type;
    bool implicitThis; // :> vs :
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
};

struct FunctionDef {
    SourceLocation location;
    std::string name;
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

struct VisibilityBlock {
    bool isPublic;
    std::vector<std::variant<
        std::unique_ptr<FunctionDef>,
        std::unique_ptr<TypeAnnotation>,
        std::unique_ptr<struct MakeDef>,
        std::unique_ptr<TypeDef>,
        std::unique_ptr<RecordDef>
    >> items;
};

struct MakeDef {
    SourceLocation location;
    TypeExprPtr target;
    bool isFinal = false;
    std::vector<std::variant<
        std::unique_ptr<FunctionDef>,
        std::unique_ptr<TypeAnnotation>,
        std::unique_ptr<VisibilityBlock>
    >> body;
};

struct CompiledBlock {
    SourceLocation location;
    std::vector<ExprPtr> body; // simplified for now
};

struct UsingBlock {
    SourceLocation location;
    TypeName module;
    std::vector<ExprPtr> body;
};

struct MainBlock {
    SourceLocation location;
    std::vector<Param> params;
    std::vector<ExprPtr> body;
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
    std::unique_ptr<MakeDef>,
    std::unique_ptr<FunctionDef>,
    std::unique_ptr<CompiledBlock>,
    std::unique_ptr<VisibilityBlock>,
    std::unique_ptr<UsingBlock>,
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
    std::unique_ptr<MakeDef>,
    std::unique_ptr<FunctionDef>,
    std::unique_ptr<CompiledBlock>,
    std::unique_ptr<UsingBlock>,
    std::unique_ptr<MainBlock>,
    std::unique_ptr<Pragma>
>;

struct Program {
    std::vector<TopLevelItem> items;
};

} // namespace kex::ast
