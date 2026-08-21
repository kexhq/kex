#include "../evaluator.hxx"
#include "../value_builder.hxx"
#include "../../ast/convert.hxx"
#include "../../lexer/lexer.hxx"
#include "../../parser/parser.hxx"
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace kex::interpreter {

static auto typeExprToTypeRef(const ast::TypeExpr& expr) -> ValuePtr {
    ValueBuilder builder;
    ast::Converter converter(builder);
    return converter.typeRef(expr);
}

// --- Location builder ---

static auto makeLocation(SourceLocation loc, const std::string& filename) -> ValuePtr {
    ValueBuilder builder;
    ast::Converter converter(builder);
    return converter.location(loc, filename);
}

static auto convertProgram(const ast::Program& program, const std::string& source,
                            const std::string& filename) -> ValuePtr {
    auto docs = ast::extractDocComments(source);
    ValueBuilder builder;
    ast::Converter converter(builder);
    return converter.program(program, filename, docs);
}

static auto makeParseError(const std::string& message, std::optional<SourceLocation> loc,
                            const std::string& filename) -> ValuePtr {
    ValueBuilder builder;
    return builder.record("ParseError", {
        {"message", Value::string(message)},
        {"location", loc ? builder.just(makeLocation(*loc, filename))
                         : builder.none()},
    });
}

// --- Registration ---

auto Evaluator::registerParserBuiltins() -> void {

    defineModule("Kex.AST");
    defineModule("Kex.Intrinsic.AST");

    // Register TypeRef/PatternRef/Expression variant->parent mappings so UFCS dispatch works
    for (const auto& tag : {"NamedType", "FunctionType", "TupleType", "ListType",
                            "MapType", "UnionType", "NullableType", "TypeVar",
                            "AnyType", "NoneType"}) {
        m_variantParent[tag] = "Kex.AST.TypeRef";
    }
    for (const auto& tag : {"BindPattern", "LiteralPattern", "ConstructorPattern",
                            "TuplePattern", "ListPattern", "WildcardPattern",
                            "GuardedPattern"}) {
        m_variantParent[tag] = "Kex.AST.PatternRef";
    }
    for (const auto& tag : {"LitInt", "LitFloat", "LitString", "InterpolatedString",
                            "LitBool", "LitAtom",
                            "LitNone", "Identifier", "BinaryOp", "UnaryOp", "Call",
                            "TaggedLiteral",
                            "MethodCall", "If", "Match", "ListLit", "TupleLit", "Block",
                            "Lambda", "Let", "Var", "Assign", "Return", "Spread",
                            "TrailingIf", "While", "Loop", "RangeLit"}) {
        m_variantParent[tag] = "Kex.AST.Expression";
    }

    // Kex.AST.parse(source) or Kex.AST.parse(source, filename)
    defineIntrinsic("AST::parse", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(makeParseError("parse requires a source string", std::nullopt, ""));
        auto* srcVal = std::get_if<StringValue>(&args[0]->data);
        if (!srcVal) return Value::error(makeParseError("parse requires a source string", std::nullopt, ""));

        std::string source = srcVal->value;
        std::string filename = "<string>";
        if (args.size() > 1) {
            if (auto* fn = std::get_if<StringValue>(&args[1]->data)) {
                filename = fn->value;
            }
        }

        try {
            auto filenamePtr = std::make_shared<std::string>(filename);
            Lexer lexer(source, *filenamePtr);
            auto tokens = lexer.tokenizeAll();
            Parser parser(tokens, *filenamePtr);
            auto program = parser.parseProgram();
            if (!parser.diagnostics().empty()) {
                const auto& diagnostic = parser.diagnostics().front();
                return Value::error(makeParseError(
                    diagnostic.message, diagnostic.location, filename));
            }
            return Value::ok(convertProgram(program, source, filename));
        } catch (const std::exception& e) {
            return Value::error(makeParseError(e.what(), std::nullopt, filename));
        }
    });

    // Kex.AST.parseFile(path)
    defineIntrinsic("AST::parseFile", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(makeParseError("parseFile requires a file path", std::nullopt, ""));
        auto* pathVal = std::get_if<StringValue>(&args[0]->data);
        if (!pathVal) return Value::error(makeParseError("parseFile requires a file path", std::nullopt, ""));

        std::string path = pathVal->value;
        std::ifstream file(path);
        if (!file.is_open()) {
            return Value::error(makeParseError("File not found: " + path, std::nullopt, path));
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string source = ss.str();

        try {
            auto filenamePtr = std::make_shared<std::string>(path);
            Lexer lexer(source, *filenamePtr);
            auto tokens = lexer.tokenizeAll();
            Parser parser(tokens, *filenamePtr);
            auto program = parser.parseProgram();
            if (!parser.diagnostics().empty()) {
                const auto& diagnostic = parser.diagnostics().front();
                return Value::error(makeParseError(
                    diagnostic.message, diagnostic.location, path));
            }
            return Value::ok(convertProgram(program, source, path));
        } catch (const std::exception& e) {
            return Value::error(makeParseError(e.what(), std::nullopt, path));
        }
    });

    // Kex.AST.parseType(typeStr)
    defineIntrinsic("AST::parseType", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(makeParseError("parseType requires a type string", std::nullopt, ""));
        auto* srcVal = std::get_if<StringValue>(&args[0]->data);
        if (!srcVal) return Value::error(makeParseError("parseType requires a type string", std::nullopt, ""));

        // Wrap in a module with a type annotation to get unambiguous parsing
        std::string source = "module T do\n  x : " + srcVal->value + "\nend";
        std::string filename = "<type>";

        try {
            auto filenamePtr = std::make_shared<std::string>(filename);
            Lexer lexer(source, *filenamePtr);
            auto tokens = lexer.tokenizeAll();
            Parser parser(tokens, *filenamePtr);
            auto program = parser.parseProgram();
            if (!parser.diagnostics().empty()) {
                const auto& diagnostic = parser.diagnostics().front();
                return Value::error(makeParseError(
                    diagnostic.message, diagnostic.location, filename));
            }
            // Find the type annotation inside the module wrapper
            for (const auto& item : program.items) {
                if (auto* mod = std::get_if<std::unique_ptr<ast::ModuleDef>>(&item)) {
                    for (const auto& mi : (*mod)->body) {
                        if (auto* ann = std::get_if<std::unique_ptr<ast::TypeAnnotation>>(&mi)) {
                            return Value::ok(typeExprToTypeRef(*(*ann)->type));
                        }
                    }
                }
            }
            return Value::error(makeParseError("Failed to parse type expression", std::nullopt, filename));
        } catch (const std::exception& e) {
            return Value::error(makeParseError(e.what(), std::nullopt, filename));
        }
    });

    // Kex.AST.parseExpression(source)
    defineIntrinsic("AST::parseExpression", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::error(makeParseError("parseExpression requires a source string", std::nullopt, ""));
        auto* srcVal = std::get_if<StringValue>(&args[0]->data);
        if (!srcVal) return Value::error(makeParseError("parseExpression requires a source string", std::nullopt, ""));

        std::string source = srcVal->value;
        std::string filename = "<expression>";

        try {
            auto filenamePtr = std::make_shared<std::string>(filename);
            Lexer lexer(source, *filenamePtr);
            auto tokens = lexer.tokenizeAll();
            Parser parser(tokens, *filenamePtr);
            auto expr = parser.parseExpr();
            if (!parser.diagnostics().empty()) {
                const auto& diagnostic = parser.diagnostics().front();
                return Value::error(makeParseError(
                    diagnostic.message, diagnostic.location, filename));
            }
            ValueBuilder builder;
            ast::Converter converter(builder);
            return Value::ok(converter.expression(*expr));
        } catch (const std::exception& e) {
            return Value::error(makeParseError(e.what(), std::nullopt, filename));
        }
    });

}

} // namespace kex::interpreter
