#include "parser.hxx"
#include <stdexcept>

namespace kex {

Parser::Parser(std::vector<Token> tokens, std::string_view filename)
    : m_tokens(std::move(tokens)), m_filename(filename) {}

// ===== Token Navigation =====

auto Parser::peek() const -> const Token& {
    return m_tokens[m_pos];
}

auto Parser::peekNext() const -> const Token& {
    if (m_pos + 1 >= static_cast<int>(m_tokens.size())) {
        return m_tokens.back();
    }
    return m_tokens[m_pos + 1];
}

auto Parser::advance() -> const Token& {
    const auto& token = m_tokens[m_pos];
    if (!atEnd()) m_pos++;
    return token;
}

auto Parser::atEnd() const -> bool {
    return peek().type == TokenType::Eof;
}

auto Parser::check(TokenType type) const -> bool {
    return peek().type == type;
}

auto Parser::match(TokenType type) -> bool {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

auto Parser::expect(TokenType type, const std::string& message) -> const Token& {
    if (check(type)) {
        return advance();
    }
    error(message + ", got " + std::string(tokenTypeName(peek().type))
          + (peek().value.empty() ? "" : " [" + peek().value + "]"));
}

auto Parser::skipNewlines() -> void {
    while (check(TokenType::Newline)) {
        advance();
    }
}

auto Parser::currentLocation() const -> SourceLocation {
    return peek().location;
}

auto Parser::error(const std::string& message) -> void {
    auto loc = currentLocation();
    throw std::runtime_error(
        std::string(loc.file) + ":" + std::to_string(loc.line) + ":"
        + std::to_string(loc.column) + ": " + message);
}

// ===== Program =====

auto Parser::parseProgram() -> ast::Program {
    ast::Program program;
    skipNewlines();

    while (!atEnd()) {
        program.items.push_back(parseTopLevelItem());
        skipNewlines();
    }

    return program;
}

auto Parser::parseTopLevelItem() -> ast::TopLevelItem {
    if (check(TokenType::Module) ||
        (check(TokenType::Foul) && peekNext().type == TokenType::Module)) {
        return parseModuleDef();
    }
    if (check(TokenType::Type)) return parseTypeDef();
    if (check(TokenType::Record)) return parseRecordDef();
    if (check(TokenType::Make)) return parseMakeDef();
    if (check(TokenType::Compiled)) return parseCompiledBlock();
    if (check(TokenType::Using)) return parseUsingBlock();
    if (check(TokenType::Main)) return parseMainBlock();
    if (check(TokenType::HashLBracket)) return parsePragma();

    if (check(TokenType::Foul) && peekNext().type == TokenType::Let) {
        advance(); // consume foul
        return parseFunctionDef(true);
    }
    if (check(TokenType::Let)) {
        // let { ... } = ... or let ( ... ) = ... is a destructuring binding, not function def
        auto nextTok = peekNext().type;
        if (nextTok == TokenType::LBrace || nextTok == TokenType::LParen ||
            nextTok == TokenType::LBracket) {
            // Parse as top-level expression (let binding)
            auto mainBlock = std::make_unique<ast::MainBlock>();
            mainBlock->location = currentLocation();
            mainBlock->body.push_back(parseExpr());
            return mainBlock;
        }
        return parseFunctionDef();
    }

    // Top-level expression (e.g. describe blocks in tests, bare function calls)
    if (isAtExprStart()) {
        auto mainBlock = std::make_unique<ast::MainBlock>();
        mainBlock->location = currentLocation();
        mainBlock->body.push_back(parseExpr());
        return mainBlock;
    }

    error("Unexpected token at top level: " + std::string(tokenTypeName(peek().type)));
}

// ===== Module =====

auto Parser::parseModuleDef() -> std::unique_ptr<ast::ModuleDef> {
    auto mod = std::make_unique<ast::ModuleDef>();
    mod->location = currentLocation();

    if (match(TokenType::Foul)) {
        mod->isFoul = true;
    }

    expect(TokenType::Module, "Expected 'module'");
    mod->name = expect(TokenType::UpperIdent, "Expected module name").value;
    expect(TokenType::Do, "Expected 'do' after module name");
    skipNewlines();

    while (!check(TokenType::End) && !atEnd()) {
        if (check(TokenType::Module) ||
            (check(TokenType::Foul) && peekNext().type == TokenType::Module)) {
            mod->body.push_back(parseModuleDef());
        } else if (check(TokenType::Type)) {
            mod->body.push_back(parseTypeDef());
        } else if (check(TokenType::Record)) {
            mod->body.push_back(parseRecordDef());
        } else if (check(TokenType::Make)) {
            mod->body.push_back(parseMakeDef());
        } else if (check(TokenType::Compiled)) {
            mod->body.push_back(parseCompiledBlock());
        } else if (check(TokenType::Using)) {
            mod->body.push_back(parseUsingBlock());
        } else if (check(TokenType::Public) || check(TokenType::Private)) {
            mod->body.push_back(parseVisibilityBlock());
        } else if (check(TokenType::Foul) && peekNext().type == TokenType::Let) {
            advance();
            mod->body.push_back(parseFunctionDef(true));
        } else if (check(TokenType::Let)) {
            mod->body.push_back(parseFunctionDef());
        } else if (check(TokenType::LowerIdent)) {
            mod->body.push_back(parseTypeAnnotation());
        } else {
            error("Unexpected token in module body: " + std::string(tokenTypeName(peek().type)));
        }
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close module");
    return mod;
}

// ===== Type Def =====

auto Parser::parseTypeDef() -> std::unique_ptr<ast::TypeDef> {
    auto def = std::make_unique<ast::TypeDef>();
    def->location = currentLocation();
    expect(TokenType::Type, "Expected 'type'");

    def->name = expect(TokenType::UpperIdent, "Expected type name").value;

    // Type params: <A, B> — only if followed by single-letter uppercase params and >
    // Inheritance: < Parent1, Parent2 — followed by multi-letter names
    if (check(TokenType::LessThan)) {
        // Look ahead: if the next token after < is a single-letter UpperIdent
        // and eventually we find >, it's type params. Otherwise it's inheritance.
        auto savedPos = m_pos;
        advance(); // consume <

        bool isTypeParams = false;
        if (check(TokenType::UpperIdent) && peek().value.size() == 1) {
            // Likely type params
            isTypeParams = true;
        }

        m_pos = savedPos; // restore

        if (isTypeParams) {
            advance(); // <
            do {
                def->typeParams.push_back(
                    expect(TokenType::UpperIdent, "Expected type parameter").value);
            } while (match(TokenType::Comma));
            expect(TokenType::GreaterThan, "Expected '>' after type parameters");

            // Could still have inheritance after type params
            if (match(TokenType::LessThan)) {
                do {
                    def->parents.push_back(parseTypeName());
                } while (match(TokenType::Comma));
            }
        } else {
            // Inheritance
            advance(); // <
            do {
                def->parents.push_back(parseTypeName());
            } while (match(TokenType::Comma));
        }
    }

    skipNewlines();

    // Sum type: = Variant1(A) | Variant2 | Variant3(B, C)
    if (match(TokenType::Equals)) {
        def->variants = std::vector<ast::TypeExprPtr>{};
        auto parseVariant = [this]() -> ast::TypeExprPtr {
            auto variant = std::make_unique<ast::TypeExpr>();
            variant->location = currentLocation();

            if (check(TokenType::UpperIdent)) {
                auto name = parseTypeName();
                // Constructor with args: Variant(Type1, Type2)
                if (match(TokenType::LParen)) {
                    std::vector<ast::TypeExprPtr> args;
                    if (!check(TokenType::RParen)) {
                        args.push_back(parseTypeExpr());
                        while (match(TokenType::Comma)) {
                            args.push_back(parseTypeExpr());
                        }
                    }
                    expect(TokenType::RParen, "Expected ')' after variant args");
                    variant->kind = ast::GenericType{std::move(name), std::move(args)};
                } else if (match(TokenType::LessThan)) {
                    std::vector<ast::TypeExprPtr> args;
                    args.push_back(parseTypeExpr());
                    while (match(TokenType::Comma)) {
                        args.push_back(parseTypeExpr());
                    }
                    expect(TokenType::GreaterThan, "Expected '>'");
                    variant->kind = ast::GenericType{std::move(name), std::move(args)};
                } else {
                    variant->kind = std::move(name);
                }

                // Optional suffix: Name?
                if (match(TokenType::Question)) {
                    auto opt = std::make_unique<ast::TypeExpr>();
                    opt->location = currentLocation();
                    opt->kind = ast::OptionalType{std::move(variant)};
                    variant = std::move(opt);
                }

                // Check for function type: Name -> Type (type alias)
                if (check(TokenType::Arrow)) {
                    advance(); // ->
                    auto right = parseTypeFunction();
                    auto funcType = std::make_unique<ast::TypeExpr>();
                    funcType->location = currentLocation();
                    funcType->kind = ast::FunctionType{std::move(variant), std::move(right)};
                    return funcType;
                }
            } else if (check(TokenType::Atom)) {
                variant->kind = ast::AtomType{advance().value};
            } else if (check(TokenType::LParen)) {
                // Tuple type or grouped: (A, B) or (A -> B)
                variant = parseTypePrimary();
                if (check(TokenType::Arrow)) {
                    advance();
                    auto right = parseTypeFunction();
                    auto funcType = std::make_unique<ast::TypeExpr>();
                    funcType->location = currentLocation();
                    funcType->kind = ast::FunctionType{std::move(variant), std::move(right)};
                    return funcType;
                }
            } else {
                variant = parseTypeExpr();
            }
            return variant;
        };

        // Parse using variant parser that handles constructors with args
        // First variant, then check for | (with newline support)
        def->variants->push_back(parseVariant());
        skipNewlines();
        while (match(TokenType::Pipe)) {
            skipNewlines();
            def->variants->push_back(parseVariant());
            skipNewlines();
        }

        // If only one variant and it's a function/complex type, it's a type alias
        // Reparse as full type expr if no pipes were found
        if (def->variants->size() == 1) {
            // Check if next meaningful token after the type is -> (function type)
            // Already handled since parseVariant falls through to parseTypeExpr for non-constructors
        }
    }
    // Abstract type with required functions and/or static block
    else if (match(TokenType::Do)) {
        skipNewlines();
        def->abstractFunctions = std::vector<ast::AbstractFunction>{};
        while (!check(TokenType::End) && !atEnd()) {
            // Static block inside type
            if (check(TokenType::Static)) {
                advance();
                expect(TokenType::Do, "Expected 'do' after 'static'");
                skipNewlines();
                def->staticBlock = ast::StaticBlock{};
                while (!check(TokenType::End) && !atEnd()) {
                    if (check(TokenType::Let) || check(TokenType::Foul)) {
                        bool isFoul = match(TokenType::Foul);
                        def->staticBlock->functions.push_back(parseFunctionDef(isFoul));
                    } else {
                        error("Expected function definition in static block");
                    }
                    skipNewlines();
                }
                expect(TokenType::End, "Expected 'end' to close static block");
                skipNewlines();
                continue;
            }

            ast::AbstractFunction func;
            func.name = expect(TokenType::LowerIdent, "Expected function name").value;
            if (match(TokenType::TypeAnnotation)) {
                func.implicitThis = true;
            } else {
                expect(TokenType::Colon, "Expected ':' or ':>' after function name");
                func.implicitThis = false;
            }
            func.type = parseTypeExpr();
            def->abstractFunctions->push_back(std::move(func));
            skipNewlines();
        }
        expect(TokenType::End, "Expected 'end' to close type");
    }

    return def;
}

// ===== Record =====

auto Parser::parseRecordDef() -> std::unique_ptr<ast::RecordDef> {
    auto def = std::make_unique<ast::RecordDef>();
    def->location = currentLocation();
    expect(TokenType::Record, "Expected 'record'");

    def->name = expect(TokenType::UpperIdent, "Expected record name").value;

    if (match(TokenType::LessThan)) {
        do {
            def->typeParams.push_back(
                expect(TokenType::UpperIdent, "Expected type parameter").value);
        } while (match(TokenType::Comma));
        expect(TokenType::GreaterThan, "Expected '>' after type parameters");
    }

    expect(TokenType::Do, "Expected 'do' after record name");
    skipNewlines();

    while (!atEnd()) {
        // End of record — 'end' not followed by ':' means it's the closing keyword
        if (check(TokenType::End) && peekNext().type != TokenType::Colon) break;

        // Static block
        if (check(TokenType::Static)) {
            advance(); // static
            expect(TokenType::Do, "Expected 'do' after 'static'");
            skipNewlines();
            def->staticBlock = ast::StaticBlock{};
            while (!check(TokenType::End) && !atEnd()) {
                if (check(TokenType::Let) || check(TokenType::Foul)) {
                    bool isFoul = match(TokenType::Foul);
                    def->staticBlock->functions.push_back(parseFunctionDef(isFoul));
                } else {
                    error("Expected function definition in static block");
                }
                skipNewlines();
            }
            expect(TokenType::End, "Expected 'end' to close static block");
            skipNewlines();
            continue;
        }

        ast::RecordField field;
        // Field names can be keywords (e.g. 'end', 'type')
        if (check(TokenType::LowerIdent) || check(TokenType::End) ||
            check(TokenType::Type) || check(TokenType::Match) ||
            check(TokenType::Loop)) {
            field.name = advance().value;
        } else {
            error("Expected field name");
        }
        expect(TokenType::Colon, "Expected ':' after field name");
        field.type = parseTypeExpr();
        if (match(TokenType::Equals)) {
            field.defaultValue = parseExpr();
        }
        def->fields.push_back(std::move(field));
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close record");
    return def;
}

// ===== Make =====

auto Parser::parseMakeDef() -> std::unique_ptr<ast::MakeDef> {
    auto def = std::make_unique<ast::MakeDef>();
    def->location = currentLocation();
    expect(TokenType::Make, "Expected 'make'");

    if (match(TokenType::Final)) {
        expect(TokenType::Colon, "Expected ':' after 'final'");
        def->isFinal = true;
    }

    def->target = parseTypeExpr();
    expect(TokenType::Do, "Expected 'do' after make target");
    skipNewlines();

    while (!check(TokenType::End) && !atEnd()) {
        if (check(TokenType::Public) || check(TokenType::Private)) {
            def->body.push_back(parseVisibilityBlock());
        } else if (check(TokenType::Foul) && peekNext().type == TokenType::Let) {
            advance();
            def->body.push_back(parseFunctionDef(true));
        } else if (check(TokenType::Let)) {
            def->body.push_back(parseFunctionDef());
        } else if (check(TokenType::LowerIdent)) {
            def->body.push_back(parseTypeAnnotation());
        } else {
            error("Unexpected token in make body: " + std::string(tokenTypeName(peek().type)));
        }
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close make");
    return def;
}

// ===== Functions =====

auto Parser::parseFunctionDef(bool isFoul) -> std::unique_ptr<ast::FunctionDef> {
    auto def = std::make_unique<ast::FunctionDef>();
    def->location = currentLocation();
    def->isFoul = isFoul;

    expect(TokenType::Let, "Expected 'let'");

    // Function name can be a lower ident, upper ident (static constructors),
    // keyword-as-name, splice ident, or operator
    if (check(TokenType::LowerIdent) || check(TokenType::UpperIdent) ||
        check(TokenType::Loop) ||
        check(TokenType::Match) || check(TokenType::SpliceIdent)) {
        def->name = advance().value;
    } else if (check(TokenType::Plus) || check(TokenType::Minus) ||
               check(TokenType::Star) || check(TokenType::Slash) ||
               check(TokenType::Percent) || check(TokenType::EqEq) ||
               check(TokenType::NotEq) || check(TokenType::LessThan) ||
               check(TokenType::GreaterThan) || check(TokenType::LessEq) ||
               check(TokenType::GreaterEq)) {
        def->name = std::string(tokenTypeName(advance().type));
    } else {
        error("Expected function name");
    }

    if (match(TokenType::Question)) {
        def->isPredicate = true;
    }

    // Check if function name ends with ?
    if (!def->isPredicate && !def->name.empty() && def->name.back() == '?') {
        def->isPredicate = true;
    }

    def->clauses.push_back(parseFunctionClause());
    return def;
}

auto Parser::parseFunctionClause() -> ast::FunctionClause {
    ast::FunctionClause clause;

    // Parameters
    if (match(TokenType::LParen)) {
        if (!check(TokenType::RParen)) {
            clause.params = parseParams();
        }
        expect(TokenType::RParen, "Expected ')' after parameters");
    }

    // Return type annotation: -> Type
    if (match(TokenType::Arrow)) {
        parseTypeExpr(); // consume return type (stored later when we need it)
    }

    // = expr (single expression body)
    if (match(TokenType::Equals)) {
        clause.body.push_back(parseExpr());
    }
    // do...end block body
    else if (match(TokenType::Do)) {
        skipNewlines();
        while (!check(TokenType::End) && !atEnd()) {
            clause.body.push_back(parseExpr());
            skipNewlines();
        }

        expect(TokenType::End, "Expected 'end' to close function");
    }

    return clause;
}

auto Parser::parseParams() -> std::vector<ast::Param> {
    std::vector<ast::Param> params;
    params.push_back(parseParam());
    while (match(TokenType::Comma)) {
        params.push_back(parseParam());
    }
    return params;
}

auto Parser::parseParam() -> ast::Param {
    ast::Param param;

    // Try pattern first for complex matches
    if (check(TokenType::At) || check(TokenType::LBrace) || check(TokenType::LBracket)) {
        param.pattern = parsePattern();
        return param;
    }

    // Literal pattern
    if (check(TokenType::Integer) || check(TokenType::Float) ||
        check(TokenType::String) || check(TokenType::True) ||
        check(TokenType::False) || check(TokenType::None) ||
        check(TokenType::Atom)) {
        param.pattern = parsePattern();
        return param;
    }

    // Underscore — might be typed: _: Type
    if (check(TokenType::Underscore)) {
        if (peekNext().type == TokenType::Colon) {
            advance(); // _
            advance(); // :
            param.name = "_";
            param.type = parseTypeExpr();
            if (match(TokenType::Equals)) {
                param.defaultValue = parseExpr();
            }
            return param;
        }
        param.pattern = parsePattern();
        return param;
    }

    // Constructor pattern
    if (check(TokenType::UpperIdent)) {
        param.pattern = parsePattern();
        return param;
    }

    // name: Type = default  or  name = default  or  just name
    if (check(TokenType::LowerIdent)) {
        auto& nameToken = advance();
        param.name = nameToken.value;

        if (match(TokenType::Colon)) {
            param.type = parseTypeExpr();
            if (match(TokenType::Equals)) {
                param.defaultValue = parseExpr();
            }
        } else if (match(TokenType::Equals)) {
            param.defaultValue = parseExpr();
        }

        return param;
    }

    error("Expected parameter");
}

auto Parser::parseTypeAnnotation() -> std::unique_ptr<ast::TypeAnnotation> {
    auto ann = std::make_unique<ast::TypeAnnotation>();
    ann->name = expect(TokenType::LowerIdent, "Expected name").value;

    if (match(TokenType::TypeAnnotation)) {
        ann->implicitThis = true;
    } else {
        expect(TokenType::Colon, "Expected ':' or ':>'");
        ann->implicitThis = false;
    }

    ann->type = parseTypeExpr();
    return ann;
}

// ===== Type Expressions =====

auto Parser::parseTypeExpr() -> ast::TypeExprPtr {
    return parseTypeUnion();
}

auto Parser::parseTypeUnion() -> ast::TypeExprPtr {
    auto left = parseTypeFunction();

    while (match(TokenType::Pipe)) {
        auto unionType = std::make_unique<ast::TypeExpr>();
        unionType->location = currentLocation();
        auto right = parseTypeFunction();
        unionType->kind = ast::UnionType{std::move(left), std::move(right)};
        left = std::move(unionType);
    }

    return left;
}

auto Parser::parseTypeFunction() -> ast::TypeExprPtr {
    auto left = parseTypePostfix();

    if (match(TokenType::Arrow)) {
        auto funcType = std::make_unique<ast::TypeExpr>();
        funcType->location = currentLocation();
        auto right = parseTypeFunction(); // right-associative
        funcType->kind = ast::FunctionType{std::move(left), std::move(right)};
        return funcType;
    }

    return left;
}

auto Parser::parseTypePostfix() -> ast::TypeExprPtr {
    auto type = parseTypePrimary();

    if (match(TokenType::Question)) {
        auto opt = std::make_unique<ast::TypeExpr>();
        opt->location = currentLocation();
        opt->kind = ast::OptionalType{std::move(type)};
        return opt;
    }

    return type;
}

auto Parser::parseTypePrimary() -> ast::TypeExprPtr {
    auto type = std::make_unique<ast::TypeExpr>();
    type->location = currentLocation();

    // List type: [A]
    if (match(TokenType::LBracket)) {
        auto elem = parseTypeExpr();
        expect(TokenType::RBracket, "Expected ']' in list type");
        type->kind = ast::ListType{std::move(elem)};
        return type;
    }

    // Map type: { K: V }
    if (match(TokenType::LBrace)) {
        auto key = parseTypeExpr();
        expect(TokenType::Colon, "Expected ':' in map type");
        auto val = parseTypeExpr();
        expect(TokenType::RBrace, "Expected '}' in map type");
        type->kind = ast::MapType{std::move(key), std::move(val)};
        return type;
    }

    // Tuple type: (A, B, C) or unit type ()
    if (match(TokenType::LParen)) {
        if (match(TokenType::RParen)) {
            // Unit type ()
            type->kind = ast::TupleType{{}};
            return type;
        }
        std::vector<ast::TypeExprPtr> elements;
        elements.push_back(parseTypeExpr());
        while (match(TokenType::Comma)) {
            elements.push_back(parseTypeExpr());
        }
        expect(TokenType::RParen, "Expected ')' in tuple type");
        if (elements.size() == 1) {
            return std::move(elements[0]); // just grouping
        }
        type->kind = ast::TupleType{std::move(elements)};
        return type;
    }

    // Block type
    if (check(TokenType::UpperIdent) && peek().value == "Block") {
        advance();
        expect(TokenType::LessThan, "Expected '<' after Block");
        auto inner = parseTypeExpr();
        expect(TokenType::GreaterThan, "Expected '>' to close Block type");
        type->kind = ast::BlockType{std::move(inner)};
        return type;
    }

    // Atom type
    if (check(TokenType::Atom)) {
        type->kind = ast::AtomType{advance().value};
        return type;
    }

    // Generic var (single letter lowercase)
    if (check(TokenType::LowerIdent) && peek().value.size() == 1) {
        type->kind = ast::GenericVar{advance().value};
        return type;
    }

    // Named type (possibly with generics)
    if (check(TokenType::UpperIdent)) {
        auto name = parseTypeName();

        if (match(TokenType::LessThan)) {
            std::vector<ast::TypeExprPtr> args;
            args.push_back(parseTypeExpr());
            while (match(TokenType::Comma)) {
                args.push_back(parseTypeExpr());
            }
            expect(TokenType::GreaterThan, "Expected '>' after type arguments");
            type->kind = ast::GenericType{std::move(name), std::move(args)};
        } else {
            type->kind = std::move(name);
        }
        return type;
    }

    error("Expected type expression");
}

auto Parser::parseTypeName() -> ast::TypeName {
    ast::TypeName name;
    name.parts.push_back(expect(TokenType::UpperIdent, "Expected type name").value);
    while (check(TokenType::Dot) && peekNext().type == TokenType::UpperIdent) {
        advance(); // consume .
        name.parts.push_back(advance().value);
    }
    return name;
}

// ===== Expressions =====

auto Parser::parseExpr() -> ast::ExprPtr {
    // Trailing if handled after parsing the main expr
    auto expr = parseAssignment();

    if (match(TokenType::If)) {
        auto trailing = std::make_unique<ast::Expr>();
        trailing->location = expr->location;
        auto condition = parseExpr();
        trailing->kind = ast::TrailingIf{std::move(expr), std::move(condition)};
        return trailing;
    }

    return expr;
}

auto Parser::parseAssignment() -> ast::ExprPtr {
    if (check(TokenType::LowerIdent) && peekNext().type == TokenType::Equals) {
        auto loc = currentLocation();
        auto name = advance().value;
        advance(); // =
        auto value = parseExpr();

        auto expr = std::make_unique<ast::Expr>();
        expr->location = loc;
        expr->kind = ast::AssignExpr{std::move(name), std::move(value)};
        return expr;
    }
    return parseOr();
}

auto Parser::parseOr() -> ast::ExprPtr {
    auto left = parseAnd();

    // `or` is a word-form alias for `||`.
    while (check(TokenType::PipePipe) || check(TokenType::Or)) {
        advance();
        auto op = std::make_unique<ast::Expr>();
        op->location = currentLocation();
        auto right = parseAnd();
        op->kind = ast::BinaryOp{std::move(left), TokenType::PipePipe, std::move(right)};
        left = std::move(op);
    }

    return left;
}

auto Parser::parseAnd() -> ast::ExprPtr {
    auto left = parseNot();

    // `and` is a word-form alias for `&&`.
    while (check(TokenType::AmpAmp) || check(TokenType::And)) {
        advance();
        auto op = std::make_unique<ast::Expr>();
        op->location = currentLocation();
        auto right = parseNot();
        op->kind = ast::BinaryOp{std::move(left), TokenType::AmpAmp, std::move(right)};
        left = std::move(op);
    }

    return left;
}

// `not` is a word-form alias for `!`, but — unlike `!`, which binds tightly
// at the unary level (`!x == y` means `(!x) == y`) — `not` binds loosely,
// Python-style: `not x == y` means `not (x == y)`. This is what makes
// `return if not ENV.get(key) == "true"` read the way it looks: the whole
// comparison is negated, not just the call.
auto Parser::parseNot() -> ast::ExprPtr {
    if (check(TokenType::Not)) {
        auto loc = currentLocation();
        advance();
        auto operand = parseNot(); // allows chaining: not not x
        auto expr = std::make_unique<ast::Expr>();
        expr->location = loc;
        expr->kind = ast::UnaryOp{TokenType::Bang, std::move(operand)};
        return expr;
    }
    return parseEquality();
}

auto Parser::parseEquality() -> ast::ExprPtr {
    auto left = parseComparison();

    while (check(TokenType::EqEq) || check(TokenType::NotEq)) {
        auto opType = advance().type;
        auto op = std::make_unique<ast::Expr>();
        op->location = currentLocation();
        auto right = parseComparison();
        op->kind = ast::BinaryOp{std::move(left), opType, std::move(right)};
        left = std::move(op);
    }

    return left;
}

auto Parser::parseComparison() -> ast::ExprPtr {
    auto left = parseAddition();

    while (check(TokenType::LessThan) || check(TokenType::GreaterThan) ||
           check(TokenType::LessEq) || check(TokenType::GreaterEq)) {
        auto opType = advance().type;
        auto op = std::make_unique<ast::Expr>();
        op->location = currentLocation();
        auto right = parseAddition();
        op->kind = ast::BinaryOp{std::move(left), opType, std::move(right)};
        left = std::move(op);
    }

    return left;
}

auto Parser::parseAddition() -> ast::ExprPtr {
    auto left = parseMultiplication();

    while (check(TokenType::Plus) || check(TokenType::Minus)) {
        auto opType = advance().type;
        auto op = std::make_unique<ast::Expr>();
        op->location = currentLocation();
        auto right = parseMultiplication();
        op->kind = ast::BinaryOp{std::move(left), opType, std::move(right)};
        left = std::move(op);
    }

    return left;
}

auto Parser::parseMultiplication() -> ast::ExprPtr {
    auto left = parseUnary();

    while (check(TokenType::Star) || check(TokenType::Slash) || check(TokenType::Percent)) {
        auto opType = advance().type;
        auto op = std::make_unique<ast::Expr>();
        op->location = currentLocation();
        auto right = parseUnary();
        op->kind = ast::BinaryOp{std::move(left), opType, std::move(right)};
        left = std::move(op);
    }

    return left;
}

auto Parser::parseUnary() -> ast::ExprPtr {
    // Note: `not` is handled at parseNot() (lower precedence, Python-style)
    // rather than here — see parseNot() for why.
    if (check(TokenType::Minus) || check(TokenType::Bang)) {
        auto loc = currentLocation();
        auto opType = advance().type;
        auto operand = parseUnary();

        auto expr = std::make_unique<ast::Expr>();
        expr->location = loc;
        expr->kind = ast::UnaryOp{opType, std::move(operand)};
        return expr;
    }

    if (check(TokenType::DotDotDot)) {
        auto loc = currentLocation();
        advance();
        auto inner = parseUnary();

        auto expr = std::make_unique<ast::Expr>();
        expr->location = loc;
        expr->kind = ast::SpreadExpr{std::move(inner)};
        return expr;
    }

    return parsePostfix();
}

auto Parser::parsePostfix() -> ast::ExprPtr {
    auto expr = parsePrimary();

    while (true) {
        // Skip newlines if followed by . (method chaining across lines)
        if (check(TokenType::Newline)) {
            auto savedPos = m_pos;
            skipNewlines();
            if (!check(TokenType::Dot)) {
                m_pos = savedPos;
            }
        }

        // Method call: expr.method or expr.method(args) or expr.method!
        // Also handles module access: Mod.Sub.func
        if (match(TokenType::Dot)) {
            if (!check(TokenType::LowerIdent) && !check(TokenType::UpperIdent)) {
                error("Expected method or module name after '.'");
            }
            auto method = advance().value;
            bool mutating = match(TokenType::Bang);

            auto call = std::make_unique<ast::Expr>();
            call->location = currentLocation();

            std::vector<ast::ExprPtr> args;
            std::vector<std::pair<std::string, ast::ExprPtr>> namedArgs;
            std::optional<ast::ExprPtr> block;

            bool hasParens = false;
            if (match(TokenType::LParen)) {
                hasParens = true;
                if (!check(TokenType::RParen)) {
                    // Parse arguments
                    do {
                        if ((check(TokenType::LowerIdent) || check(TokenType::Timeout) || check(TokenType::Type) || check(TokenType::Match) || check(TokenType::Loop)) && peekNext().type == TokenType::Colon) {
                            auto name = advance().value;
                            advance(); // :
                            namedArgs.push_back({name, parseExpr()});
                        } else {
                            args.push_back(parseExpr());
                        }
                    } while (match(TokenType::Comma));
                }
                expect(TokenType::RParen, "Expected ')' after arguments");
            }

            // Block argument
            if (check(TokenType::LBrace)) {
                block = parsePrimary();
            } else if (check(TokenType::Do) && !m_noDoBlocks) {
                block = parsePrimary();
            }

            call->kind = ast::MethodCall{
                std::move(expr), method, std::move(args),
                std::move(namedArgs), std::move(block), mutating};
            expr = std::move(call);
            continue;
        }

        // Bracket access: expr[key]
        if (match(TokenType::LBracket)) {
            auto key = parseExpr();
            expect(TokenType::RBracket, "Expected ']' after index");

            auto call = std::make_unique<ast::Expr>();
            call->location = currentLocation();
            // Desugar to method call: expr.get(key)
            std::vector<ast::ExprPtr> args;
            args.push_back(std::move(key));
            call->kind = ast::MethodCall{
                std::move(expr), "get", std::move(args), {}, std::nullopt, false};
            expr = std::move(call);
            continue;
        }

        // Error propagation: expr?
        if (match(TokenType::Question)) {
            auto prop = std::make_unique<ast::Expr>();
            prop->location = currentLocation();
            prop->kind = ast::ErrorPropagate{std::move(expr)};
            expr = std::move(prop);
            continue;
        }

        // Range: expr..expr
        if (match(TokenType::DotDot)) {
            auto range = std::make_unique<ast::Expr>();
            range->location = currentLocation();
            auto end = parseAddition();
            range->kind = ast::RangeExpr{std::move(expr), std::move(end)};
            expr = std::move(range);
            continue;
        }

        break;
    }

    return expr;
}

auto Parser::parsePrimary() -> ast::ExprPtr {
    auto loc = currentLocation();
    auto expr = std::make_unique<ast::Expr>();
    expr->location = loc;

    // Literals
    if (check(TokenType::Integer)) {
        expr->kind = ast::IntLiteral{advance().value};
        return expr;
    }
    if (check(TokenType::Float)) {
        expr->kind = ast::FloatLiteral{advance().value};
        return expr;
    }
    if (check(TokenType::String)) {
        expr->kind = ast::StringLiteral{advance().value};
        return expr;
    }
    if (match(TokenType::True)) {
        expr->kind = ast::BoolLiteral{true};
        return expr;
    }
    if (match(TokenType::False)) {
        expr->kind = ast::BoolLiteral{false};
        return expr;
    }
    if (match(TokenType::None)) {
        expr->kind = ast::NoneLiteral{};
        return expr;
    }
    if (check(TokenType::Atom)) {
        expr->kind = ast::AtomLiteral{advance().value};
        return expr;
    }

    // this
    if (match(TokenType::This)) {
        expr->kind = ast::ThisExpr{};
        return expr;
    }

    // Shorthand lambda: &.method or &function
    if (check(TokenType::Amp)) {
        return parseShorthandLambda();
    }

    // List
    if (check(TokenType::LBracket)) {
        return parseListExpr();
    }

    // Map or brace lambda
    if (check(TokenType::LBrace)) {
        return parseMapOrBlock();
    }

    // Tuple or grouped expression
    if (check(TokenType::LParen)) {
        return parseTupleOrGrouped();
    }

    // Control flow
    if (check(TokenType::If)) return parseIfExpr();
    if (check(TokenType::Match)) return parseMatchExpr();
    if (check(TokenType::Receive)) return parseReceiveExpr();
    if (check(TokenType::Loop) && peekNext().type == TokenType::Do) return parseLoopExpr();
    if (check(TokenType::Let)) return parseLetExpr();
    if (check(TokenType::Var)) return parseVarExpr();
    if (check(TokenType::Return)) return parseReturnExpr();
    if (check(TokenType::Spawn)) return parseSpawnExpr();

    // Do block (standalone lambda)
    if (check(TokenType::Do)) {
        return parseLambda();
    }

    // Splice ident in expression context (compiled blocks): %name :> type or %name(...)
    if (check(TokenType::SpliceIdent)) {
        auto name = "%" + advance().value;
        // Type annotation: %name :> Type or %name : Type
        if (match(TokenType::TypeAnnotation) || match(TokenType::Colon)) {
            parseTypeExpr(); // consume and discard
            expr->kind = ast::Identifier{name};
            return expr;
        }
        expr->kind = ast::Identifier{name};
        return expr;
    }

    // Upper ident (type constructor or record creation)
    if (check(TokenType::UpperIdent)) {
        auto name = advance().value;

        // Record construction: Type { field: value }
        if (check(TokenType::LBrace)) {
            advance(); // {
            std::vector<std::pair<std::string, ast::ExprPtr>> fields;

            if (!check(TokenType::RBrace)) {
                do {
                    skipNewlines();
                    auto fieldName = expect(TokenType::LowerIdent, "Expected field name").value;
                    expect(TokenType::Colon, "Expected ':' after field name");
                    auto value = parseExpr();
                    fields.push_back({fieldName, std::move(value)});
                    skipNewlines();
                } while (match(TokenType::Comma));
            }
            skipNewlines();
            expect(TokenType::RBrace, "Expected '}' to close record");

            expr->kind = ast::RecordConstruction{name, std::move(fields)};
            return expr;
        }

        // Constructor/function with args: Just(x) or Vector2D.Polar(angle: 1.0)
        if (match(TokenType::LParen)) {
            std::vector<ast::ExprPtr> args;
            std::vector<std::pair<std::string, ast::ExprPtr>> namedArgs;
            if (!check(TokenType::RParen)) {
                do {
                    if ((check(TokenType::LowerIdent) || check(TokenType::Timeout) || check(TokenType::Type) || check(TokenType::Match) || check(TokenType::Loop)) && peekNext().type == TokenType::Colon) {
                        auto argName = advance().value;
                        advance(); // :
                        namedArgs.push_back({argName, parseExpr()});
                    } else {
                        args.push_back(parseExpr());
                    }
                } while (match(TokenType::Comma));
            }
            expect(TokenType::RParen, "Expected ')'");

            // Trailing block as last arg
            std::optional<ast::ExprPtr> block;
            if (check(TokenType::LBrace) ||
                (check(TokenType::Do) && !m_noDoBlocks)) {
                block = parsePrimary();
            }

            auto call = std::make_unique<ast::Expr>();
            call->location = loc;
            call->kind = ast::FunctionCall{name, std::move(args), std::move(namedArgs), std::move(block)};
            return call;
        }

        expr->kind = ast::UpperIdentifier{name};
        return expr;
    }

    // Keywords that can be used as identifiers in expression context
    if ((check(TokenType::Loop) || check(TokenType::Match)) &&
        peekNext().type != TokenType::Do) {
        auto name = advance().value;
        if (match(TokenType::LParen)) {
            std::vector<ast::ExprPtr> args;
            std::vector<std::pair<std::string, ast::ExprPtr>> namedArgs;
            if (!check(TokenType::RParen)) {
                do {
                    if ((check(TokenType::LowerIdent) || check(TokenType::Timeout) || check(TokenType::Type) || check(TokenType::Match) || check(TokenType::Loop)) && peekNext().type == TokenType::Colon) {
                        auto argName = advance().value;
                        advance();
                        namedArgs.push_back({argName, parseExpr()});
                    } else {
                        args.push_back(parseExpr());
                    }
                } while (match(TokenType::Comma));
            }
            expect(TokenType::RParen, "Expected ')'");
            expr->kind = ast::FunctionCall{name, std::move(args), std::move(namedArgs), std::nullopt};
            return expr;
        }
        expr->kind = ast::Identifier{name};
        return expr;
    }

    // Lower ident (variable or function call)
    if (check(TokenType::LowerIdent)) {
        auto name = advance().value;

        // Function call with parens
        if (match(TokenType::LParen)) {
            std::vector<ast::ExprPtr> args;
            std::vector<std::pair<std::string, ast::ExprPtr>> namedArgs;

            if (!check(TokenType::RParen)) {
                do {
                    if ((check(TokenType::LowerIdent) || check(TokenType::Timeout) || check(TokenType::Type) || check(TokenType::Match) || check(TokenType::Loop)) && peekNext().type == TokenType::Colon) {
                        auto argName = advance().value;
                        advance(); // :
                        namedArgs.push_back({argName, parseExpr()});
                    } else {
                        args.push_back(parseExpr());
                    }
                } while (match(TokenType::Comma));
            }
            expect(TokenType::RParen, "Expected ')'");

            std::optional<ast::ExprPtr> block;
            if (check(TokenType::LBrace) ||
                (check(TokenType::Do) && !m_noDoBlocks)) {
                block = parsePrimary();
            }

            expr->kind = ast::FunctionCall{name, std::move(args), std::move(namedArgs), std::move(block)};
            return expr;
        }

        // Function call with named args (no parens): name key: val, ... do...end
        if ((check(TokenType::LowerIdent) || check(TokenType::Timeout) ||
             check(TokenType::Type) || check(TokenType::Match)) &&
            peekNext().type == TokenType::Colon) {
            std::vector<ast::ExprPtr> args;
            std::vector<std::pair<std::string, ast::ExprPtr>> namedArgs;
            do {
                auto argName = advance().value;
                advance(); // :
                namedArgs.push_back({argName, parseExpr()});
            } while (match(TokenType::Comma) &&
                     (check(TokenType::LowerIdent) || check(TokenType::Timeout)) &&
                     peekNext().type == TokenType::Colon);

            std::optional<ast::ExprPtr> block;
            if (check(TokenType::LBrace) ||
                (check(TokenType::Do) && !m_noDoBlocks)) {
                block = parsePrimary();
            }

            expr->kind = ast::FunctionCall{name, std::move(args), std::move(namedArgs), std::move(block)};
            return expr;
        }

        // Function call with block (no parens): name do...end or name { }
        if (check(TokenType::LBrace) ||
            (check(TokenType::Do) && !m_noDoBlocks)) {
            auto block = parsePrimary();
            expr->kind = ast::FunctionCall{name, {}, {}, std::move(block)};
            return expr;
        }

        expr->kind = ast::Identifier{name};
        return expr;
    }

    // Type/Make keywords in expression context (compiled blocks)
    if (check(TokenType::Type) || check(TokenType::Make)) {
        // Skip until matching 'end' for do blocks or newline
        auto kw = advance();
        int depth = 0;
        while (!atEnd()) {
            if (check(TokenType::Do)) depth++;
            if (check(TokenType::End)) {
                if (depth == 0) break;
                depth--;
            }
            if (depth == 0 && check(TokenType::Newline)) break;
            advance();
        }
        if (depth >= 0 && check(TokenType::End)) advance();
        expr->kind = ast::Identifier{"__compiled_" + kw.value + "__"};
        return expr;
    }

    error("Unexpected token: " + std::string(tokenTypeName(peek().type)));
}

// ===== Specific Expression Parsers =====

auto Parser::parseIfExpr() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();
    expect(TokenType::If, "Expected 'if'");

    m_noDoBlocks = true;
    auto condition = parseExpr();
    m_noDoBlocks = false;
    skipNewlines();
    expect(TokenType::Do, "Expected 'do' after if condition");
    skipNewlines();

    std::vector<ast::ExprPtr> thenBody;
    while (!check(TokenType::End) && !check(TokenType::Else) &&
           !check(TokenType::Elif) && !atEnd()) {
        thenBody.push_back(parseExpr());
        skipNewlines();
    }

    std::vector<std::pair<ast::ExprPtr, std::vector<ast::ExprPtr>>> elifs;
    while (match(TokenType::Elif)) {
        auto elifCond = parseExpr();
        expect(TokenType::Do, "Expected 'do' after elif condition");
        skipNewlines();
        std::vector<ast::ExprPtr> elifBody;
        while (!check(TokenType::End) && !check(TokenType::Else) &&
               !check(TokenType::Elif) && !atEnd()) {
            elifBody.push_back(parseExpr());
            skipNewlines();
        }
        elifs.push_back({std::move(elifCond), std::move(elifBody)});
    }

    std::optional<std::vector<ast::ExprPtr>> elseBody;
    if (match(TokenType::Else)) {
        expect(TokenType::Do, "Expected 'do' after else");
        skipNewlines();
        elseBody = std::vector<ast::ExprPtr>{};
        while (!check(TokenType::End) && !atEnd()) {
            elseBody->push_back(parseExpr());
            skipNewlines();
        }
    }

    expect(TokenType::End, "Expected 'end' to close if");

    expr->kind = ast::IfExpr{
        std::move(condition), std::move(thenBody),
        std::move(elifs), std::move(elseBody)};
    return expr;
}

auto Parser::parseMatchExpr() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();
    expect(TokenType::Match, "Expected 'match'");

    m_noDoBlocks = true;
    auto subject = parseExpr();
    m_noDoBlocks = false;
    expect(TokenType::Do, "Expected 'do' after match subject");
    skipNewlines();

    std::vector<ast::MatchClause> clauses;
    while (!check(TokenType::End) && !atEnd()) {
        clauses.push_back(parseMatchClause());
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close match");

    expr->kind = ast::MatchExpr{std::move(subject), std::move(clauses)};
    return expr;
}

auto Parser::parseMatchClause() -> ast::MatchClause {
    ast::MatchClause clause;

    clause.patterns.push_back(parsePattern());
    while (match(TokenType::Comma)) {
        clause.patterns.push_back(parsePattern());
    }

    // Guard
    if (match(TokenType::If)) {
        clause.guard = parseExpr();
    }

    expect(TokenType::Arrow, "Expected '->' in match clause");

    // Body can be single expr or a do...end block
    if (match(TokenType::Do)) {
        skipNewlines();
        std::vector<ast::ExprPtr> body;
        while (!check(TokenType::End) && !atEnd()) {
            body.push_back(parseExpr());
            skipNewlines();
        }
        expect(TokenType::End, "Expected 'end' to close match clause body");

        auto blockExpr = std::make_unique<ast::Expr>();
        blockExpr->location = currentLocation();
        blockExpr->kind = ast::BlockExpr{std::move(body)};
        clause.body = std::move(blockExpr);
    } else {
        clause.body = parseExpr();
    }

    return clause;
}

auto Parser::parseReceiveExpr() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();
    expect(TokenType::Receive, "Expected 'receive'");

    std::optional<ast::ExprPtr> timeout;
    if (match(TokenType::Timeout)) {
        expect(TokenType::Colon, "Expected ':' after 'timeout'");
        timeout = parseExpr();
    }

    expect(TokenType::Do, "Expected 'do' after receive");
    skipNewlines();

    std::vector<ast::MatchClause> clauses;
    while (!check(TokenType::End) && !check(TokenType::After) && !atEnd()) {
        clauses.push_back(parseMatchClause());
        skipNewlines();
    }

    std::optional<ast::ExprPtr> afterBody;
    if (match(TokenType::After)) {
        expect(TokenType::Arrow, "Expected '->' after 'after'");
        afterBody = parseExpr();
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close receive");

    expr->kind = ast::ReceiveExpr{std::move(clauses), std::move(timeout), std::move(afterBody)};
    return expr;
}

auto Parser::parseLoopExpr() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();
    expect(TokenType::Loop, "Expected 'loop'");
    expect(TokenType::Do, "Expected 'do' after 'loop'");
    skipNewlines();

    std::vector<ast::ExprPtr> body;
    while (!check(TokenType::End) && !atEnd()) {
        body.push_back(parseExpr());
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close loop");

    expr->kind = ast::LoopExpr{std::move(body)};
    return expr;
}

auto Parser::parseLetExpr() -> ast::ExprPtr {
    auto loc = currentLocation();

    // Check if this is actually a function def: let name(...) or let name = ...
    // Note: some keywords can be used as function names (e.g. 'loop', 'where')
    auto nextType = peekNext().type;
    bool isNameToken = nextType == TokenType::LowerIdent ||
                       nextType == TokenType::Loop ||
                       nextType == TokenType::Match || nextType == TokenType::SpliceIdent ||
                       nextType == TokenType::Plus || nextType == TokenType::Minus ||
                       nextType == TokenType::Star || nextType == TokenType::EqEq;
    if (isNameToken) {
        // Look further: is there a ( after the name? Or is it let name = expr (simple binding)?
        auto savedPos = m_pos;
        advance(); // let
        advance(); // name
        // It's a function def if followed by ( or -> (return type) or do or ? (predicate)
        // It's a simple let binding if followed by = directly
        bool isFuncDef = check(TokenType::LParen) || check(TokenType::Do) ||
                         check(TokenType::Arrow) || check(TokenType::Question);
        m_pos = savedPos;

        if (isFuncDef) {
            // Parse as inline function def, wrap in a dummy expr
            auto funcDef = parseFunctionDef();
            auto expr = std::make_unique<ast::Expr>();
            expr->location = loc;
            // Represent as a let binding of a lambda with the function name
            ast::PatternPtr pat = std::make_unique<ast::Pattern>();
            pat->location = loc;
            pat->kind = ast::VarPattern{funcDef->name};

            auto lambda = std::make_unique<ast::Expr>();
            lambda->location = loc;
            if (!funcDef->clauses.empty()) {
                lambda->kind = ast::Lambda{
                    {}, std::move(funcDef->clauses[0].body)};
            } else {
                lambda->kind = ast::Lambda{{}, {}};
            }

            expr->kind = ast::LetExpr{std::move(pat), std::move(lambda)};
            return expr;
        }
    }

    auto expr = std::make_unique<ast::Expr>();
    expr->location = loc;
    expect(TokenType::Let, "Expected 'let'");

    auto pattern = parsePattern();
    expect(TokenType::Equals, "Expected '=' in let binding");
    auto value = parseExpr();

    expr->kind = ast::LetExpr{std::move(pattern), std::move(value)};
    return expr;
}

auto Parser::parseVarExpr() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();
    expect(TokenType::Var, "Expected 'var'");

    auto name = expect(TokenType::LowerIdent, "Expected variable name").value;

    // Optional type annotation: var x: Type = value
    if (match(TokenType::Colon)) {
        parseTypeExpr(); // consume type (stored later if needed)
    }

    expect(TokenType::Equals, "Expected '=' in var binding");
    auto value = parseExpr();

    expr->kind = ast::VarExpr{std::move(name), std::move(value)};
    return expr;
}

auto Parser::parseReturnExpr() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();
    expect(TokenType::Return, "Expected 'return'");

    // `return if COND` — value-less guard-clause early return (returns
    // None when COND is true, does nothing when false). Must be handled
    // here, before falling into parseExpr(): `if` is itself a primary
    // expression (`if COND do ... end`), so without this check the `if`
    // immediately after `return` would be parsed as that instead, and
    // fail since there's no `do` block to go with it.
    //
    // `return EXPR if COND` (a value, then a trailing guard) doesn't need
    // special handling here — parseExpr() already produces
    // TrailingIf{EXPR, COND} for that on its own.
    if (check(TokenType::If)) {
        advance(); // if
        auto trailing = std::make_unique<ast::Expr>();
        trailing->location = currentLocation();
        auto condition = parseExpr();
        trailing->kind = ast::TrailingIf{nullptr, std::move(condition)};
        expr->kind = ast::ReturnExpr{std::move(trailing)};
        return expr;
    }

    auto value = parseExpr();
    expr->kind = ast::ReturnExpr{std::move(value)};
    return expr;
}

auto Parser::parseSpawnExpr() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();
    expect(TokenType::Spawn, "Expected 'spawn'");
    expect(TokenType::Do, "Expected 'do' after 'spawn'");
    skipNewlines();

    std::vector<ast::ExprPtr> body;
    while (!check(TokenType::End) && !atEnd()) {
        body.push_back(parseExpr());
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close spawn");

    expr->kind = ast::SpawnExpr{std::move(body)};
    return expr;
}

auto Parser::parseLambda() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();

    // { |params| body } or { expr }
    if (match(TokenType::LBrace)) {
        std::vector<ast::LambdaParam> params;

        if (match(TokenType::Pipe)) {
            while (!check(TokenType::Pipe) && !atEnd()) {
                ast::LambdaParam param;
                if (check(TokenType::Underscore)) {
                    param.name = "_";
                    advance();
                } else if (check(TokenType::LParen)) {
                    // Destructured tuple param: |(a, b)|
                    advance(); // (
                    std::string combined;
                    while (!check(TokenType::RParen) && !atEnd()) {
                        if (!combined.empty() && check(TokenType::Comma)) {
                            combined += ",";
                            advance();
                        } else {
                            combined += advance().value;
                        }
                    }
                    expect(TokenType::RParen, "Expected ')' in tuple param");
                    param.name = "(" + combined + ")";
                } else {
                    param.name = expect(TokenType::LowerIdent, "Expected param name").value;
                }
                params.push_back(std::move(param));
                match(TokenType::Comma);
            }
            expect(TokenType::Pipe, "Expected '|' to close params");
        }

        std::vector<ast::ExprPtr> body;
        skipNewlines();
        while (!check(TokenType::RBrace) && !atEnd()) {
            body.push_back(parseExpr());
            skipNewlines();
        }
        expect(TokenType::RBrace, "Expected '}' to close lambda");

        expr->kind = ast::Lambda{std::move(params), std::move(body)};
        return expr;
    }

    // do |params| body end
    if (match(TokenType::Do)) {
        std::vector<ast::LambdaParam> params;
        skipNewlines();

        if (match(TokenType::Pipe)) {
            while (!check(TokenType::Pipe) && !atEnd()) {
                ast::LambdaParam param;
                if (check(TokenType::Underscore)) {
                    param.name = "_";
                    advance();
                } else {
                    param.name = expect(TokenType::LowerIdent, "Expected param name").value;
                }
                params.push_back(std::move(param));
                match(TokenType::Comma);
            }
            expect(TokenType::Pipe, "Expected '|' to close params");
        }

        skipNewlines();
        std::vector<ast::ExprPtr> body;
        while (!check(TokenType::End) && !atEnd()) {
            body.push_back(parseExpr());
            skipNewlines();
        }
        expect(TokenType::End, "Expected 'end' to close do block");

        expr->kind = ast::Lambda{std::move(params), std::move(body)};
        return expr;
    }

    error("Expected lambda");
}

auto Parser::parseShorthandLambda() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();
    expect(TokenType::Amp, "Expected '&'");

    if (match(TokenType::Dot)) {
        std::string name;
        if (check(TokenType::LowerIdent)) {
            name = advance().value;
        } else if (check(TokenType::Plus) || check(TokenType::Minus) ||
                   check(TokenType::Star) || check(TokenType::Slash) ||
                   check(TokenType::Percent) || check(TokenType::EqEq) ||
                   check(TokenType::NotEq) || check(TokenType::LessThan) ||
                   check(TokenType::GreaterThan)) {
            name = std::string(tokenTypeName(advance().type));
        } else {
            error("Expected method name or operator after '&.'");
        }
        std::vector<ast::ExprPtr> args;

        if (match(TokenType::LParen)) {
            if (!check(TokenType::RParen)) {
                do {
                    args.push_back(parseExpr());
                } while (match(TokenType::Comma));
            }
            expect(TokenType::RParen, "Expected ')'");
            expr->kind = ast::ShorthandLambda{
                ast::ShorthandLambda::Kind::MethodWithArgs, name, std::move(args)};
        } else if (isAtExprStart()) {
            // Operator with inline arg: &.+ 1 or &.+ "hello"
            args.push_back(parsePrimary());
            expr->kind = ast::ShorthandLambda{
                ast::ShorthandLambda::Kind::MethodWithArgs, name, std::move(args)};
        } else {
            expr->kind = ast::ShorthandLambda{
                ast::ShorthandLambda::Kind::Method, name, {}};
        }
        return expr;
    }

    // &operator (e.g. &.+ 1) or &function_name
    if (check(TokenType::Plus) || check(TokenType::Minus) || check(TokenType::Star) ||
        check(TokenType::Slash) || check(TokenType::Percent)) {
        auto opName = std::string(tokenTypeName(advance().type));
        expr->kind = ast::ShorthandLambda{
            ast::ShorthandLambda::Kind::Function, opName, {}};
        return expr;
    }

    auto name = expect(TokenType::LowerIdent, "Expected function name after '&'").value;
    expr->kind = ast::ShorthandLambda{
        ast::ShorthandLambda::Kind::Function, name, {}};
    return expr;
}

auto Parser::parseListExpr() -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->location = currentLocation();
    expect(TokenType::LBracket, "Expected '['");

    std::vector<ast::ExprPtr> elements;
    std::optional<ast::ExprPtr> rest;

    skipNewlines();
    if (!check(TokenType::RBracket)) {
        elements.push_back(parseExpr());
        skipNewlines();
        while (match(TokenType::Comma)) {
            skipNewlines();
            elements.push_back(parseExpr());
            skipNewlines();
        }
        if (match(TokenType::Pipe)) {
            rest = parseExpr();
        }
    }
    skipNewlines();
    expect(TokenType::RBracket, "Expected ']'");

    expr->kind = ast::ListExpr{std::move(elements), std::move(rest)};
    return expr;
}

auto Parser::parseMapOrBlock() -> ast::ExprPtr {
    auto loc = currentLocation();
    advance(); // consume {
    skipNewlines();

    // Empty map: {}
    if (check(TokenType::RBrace)) {
        advance();
        auto expr = std::make_unique<ast::Expr>();
        expr->location = loc;
        expr->kind = ast::MapExpr{{}};
        return expr;
    }

    // Lambda: { |params| body }
    if (check(TokenType::Pipe)) {
        auto expr = std::make_unique<ast::Expr>();
        expr->location = loc;

        std::vector<ast::LambdaParam> params;
        advance(); // |
        while (!check(TokenType::Pipe) && !atEnd()) {
            ast::LambdaParam param;
            if (check(TokenType::Underscore)) {
                param.name = "_";
                advance();
            } else if (check(TokenType::LParen)) {
                advance(); // (
                std::string combined;
                while (!check(TokenType::RParen) && !atEnd()) {
                    if (!combined.empty() && check(TokenType::Comma)) {
                        combined += ",";
                        advance();
                    } else {
                        combined += advance().value;
                    }
                }
                expect(TokenType::RParen, "Expected ')' in tuple param");
                param.name = "(" + combined + ")";
            } else {
                param.name = expect(TokenType::LowerIdent, "Expected param name").value;
            }
            params.push_back(std::move(param));
            match(TokenType::Comma);
        }
        expect(TokenType::Pipe, "Expected '|'");

        std::vector<ast::ExprPtr> body;
        skipNewlines();
        while (!check(TokenType::RBrace) && !atEnd()) {
            body.push_back(parseExpr());
            skipNewlines();
        }
        expect(TokenType::RBrace, "Expected '}'");

        expr->kind = ast::Lambda{std::move(params), std::move(body)};
        return expr;
    }

    // Try to detect map vs single-expression lambda
    // Map: { key: value, ... }
    // Lambda: { expr }
    // Heuristic: if first token is string/ident followed by colon, it's a map
    if ((check(TokenType::String) || check(TokenType::LowerIdent)) &&
        peekNext().type == TokenType::Colon) {
        auto expr = std::make_unique<ast::Expr>();
        expr->location = loc;

        std::vector<ast::MapEntry> entries;
        do {
            skipNewlines();
            auto key = parseExpr();
            expect(TokenType::Colon, "Expected ':' in map entry");
            auto value = parseExpr();
            entries.push_back(ast::MapEntry{std::move(key), std::move(value)});
            skipNewlines();
        } while (match(TokenType::Comma));

        skipNewlines();
        expect(TokenType::RBrace, "Expected '}' to close map");

        expr->kind = ast::MapExpr{std::move(entries)};
        return expr;
    }

    // Single-expression lambda: { expr }
    auto expr = std::make_unique<ast::Expr>();
    expr->location = loc;

    std::vector<ast::ExprPtr> body;
    skipNewlines();
    while (!check(TokenType::RBrace) && !atEnd()) {
        body.push_back(parseExpr());
        skipNewlines();
    }
    expect(TokenType::RBrace, "Expected '}'");

    expr->kind = ast::Lambda{{}, std::move(body)};
    return expr;
}

auto Parser::parseTupleOrGrouped() -> ast::ExprPtr {
    auto loc = currentLocation();
    expect(TokenType::LParen, "Expected '('");

    // Unit value: ()
    if (match(TokenType::RParen)) {
        auto expr = std::make_unique<ast::Expr>();
        expr->location = loc;
        expr->kind = ast::TupleExpr{{}};
        return expr;
    }

    auto first = parseExpr();

    // Tuple: (a, b, ...)
    if (match(TokenType::Comma)) {
        std::vector<ast::ExprPtr> elements;
        elements.push_back(std::move(first));
        elements.push_back(parseExpr());
        while (match(TokenType::Comma)) {
            elements.push_back(parseExpr());
        }
        expect(TokenType::RParen, "Expected ')'");

        auto expr = std::make_unique<ast::Expr>();
        expr->location = loc;
        expr->kind = ast::TupleExpr{std::move(elements)};
        return expr;
    }

    // Grouped expression: (expr)
    expect(TokenType::RParen, "Expected ')'");
    return first;
}

// ===== Patterns =====

auto Parser::parsePattern() -> ast::PatternPtr {
    auto loc = currentLocation();
    auto pattern = std::make_unique<ast::Pattern>();
    pattern->location = loc;

    // @ pattern (match on this)
    if (match(TokenType::At)) {
        auto inner = parsePatternPrimary();
        pattern->kind = ast::ThisPattern{std::move(inner)};
        return pattern;
    }

    return parsePatternPrimary();
}

auto Parser::parsePatternPrimary() -> ast::PatternPtr {
    auto loc = currentLocation();
    auto pattern = std::make_unique<ast::Pattern>();
    pattern->location = loc;

    // Wildcard
    if (match(TokenType::Underscore)) {
        pattern->kind = ast::WildcardPattern{};
        return pattern;
    }

    // Literals
    if (check(TokenType::Integer) || check(TokenType::Float) ||
        check(TokenType::String) || check(TokenType::True) ||
        check(TokenType::False) || check(TokenType::None) ||
        check(TokenType::Atom)) {
        pattern->kind = ast::LiteralPattern{advance()};
        return pattern;
    }

    // Record/map destructuring: { ... }
    if (match(TokenType::LBrace)) {
        std::vector<ast::FieldPattern> fields;

        if (!check(TokenType::RBrace)) {
            do {
                skipNewlines();
                ast::FieldPattern field;

                if (check(TokenType::String)) {
                    field.isStringKey = true;
                    field.name = advance().value;
                } else if (check(TokenType::LowerIdent) || check(TokenType::End) ||
                           check(TokenType::Type) ||
                           check(TokenType::Loop) || check(TokenType::Match) ||
                           check(TokenType::Timeout)) {
                    field.name = advance().value;
                } else {
                    error("Expected field name");
                }

                if (match(TokenType::Colon)) {
                    field.pattern = parsePattern();
                }

                fields.push_back(std::move(field));
                skipNewlines();
            } while (match(TokenType::Comma));
        }

        expect(TokenType::RBrace, "Expected '}'");
        pattern->kind = ast::RecordPattern{std::move(fields)};
        return pattern;
    }

    // List pattern: [...]
    if (match(TokenType::LBracket)) {
        std::vector<ast::PatternPtr> elements;
        std::optional<ast::PatternPtr> rest;

        if (!check(TokenType::RBracket)) {
            elements.push_back(parsePattern());
            while (match(TokenType::Comma)) {
                elements.push_back(parsePattern());
            }
            if (match(TokenType::Pipe)) {
                rest = parsePattern();
            }
        }

        expect(TokenType::RBracket, "Expected ']'");
        pattern->kind = ast::ListPattern{std::move(elements), std::move(rest)};
        return pattern;
    }

    // Tuple pattern: (a, b)
    if (match(TokenType::LParen)) {
        std::vector<ast::PatternPtr> elements;
        if (!check(TokenType::RParen)) {
            elements.push_back(parsePattern());
            while (match(TokenType::Comma)) {
                elements.push_back(parsePattern());
            }
        }
        expect(TokenType::RParen, "Expected ')'");
        pattern->kind = ast::TuplePattern{std::move(elements)};
        return pattern;
    }

    // Constructor pattern: Name(args) or just Name
    if (check(TokenType::UpperIdent)) {
        auto name = advance().value;
        if (match(TokenType::LParen)) {
            std::vector<ast::PatternPtr> args;
            if (!check(TokenType::RParen)) {
                args.push_back(parsePattern());
                while (match(TokenType::Comma)) {
                    args.push_back(parsePattern());
                }
            }
            expect(TokenType::RParen, "Expected ')'");
            pattern->kind = ast::ConstructorPattern{name, std::move(args)};
        } else {
            pattern->kind = ast::ConstructorPattern{name, {}};
        }
        return pattern;
    }

    // Variable binding
    if (check(TokenType::LowerIdent)) {
        pattern->kind = ast::VarPattern{advance().value};
        return pattern;
    }

    error("Expected pattern");
}

// ===== Remaining Top-Level =====

auto Parser::parseCompiledBlock() -> std::unique_ptr<ast::CompiledBlock> {
    auto block = std::make_unique<ast::CompiledBlock>();
    block->location = currentLocation();
    expect(TokenType::Compiled, "Expected 'compiled'");
    expect(TokenType::Do, "Expected 'do' after 'compiled'");
    skipNewlines();

    while (!check(TokenType::End) && !atEnd()) {
        // UPPER = expr (compile-time constant assignment)
        if (check(TokenType::UpperIdent) && peekNext().type == TokenType::Equals) {
            auto loc = currentLocation();
            auto name = advance().value;
            advance(); // =
            auto value = parseExpr();

            auto expr = std::make_unique<ast::Expr>();
            expr->location = loc;
            expr->kind = ast::AssignExpr{std::move(name), std::move(value)};
            block->body.push_back(std::move(expr));
        }
        // type definition inside compiled
        else if (check(TokenType::Type)) {
            // Skip type defs in compiled blocks for now (consume until end of line)
            advance(); // type
            while (!check(TokenType::Newline) && !check(TokenType::End) && !atEnd()) {
                advance();
            }
        }
        // %name :> TypeAnnotation or %name : TypeAnnotation
        else if (check(TokenType::SpliceIdent) &&
                 (peekNext().type == TokenType::TypeAnnotation || peekNext().type == TokenType::Colon)) {
            advance(); // %name
            advance(); // :> or :
            parseTypeExpr(); // consume type
        }
        // let %name(...) or regular let
        else if (check(TokenType::Let) || check(TokenType::Foul)) {
            block->body.push_back(parseExpr());
        }
        else {
            block->body.push_back(parseExpr());
        }
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close compiled block");
    return block;
}

auto Parser::parseUsingBlock() -> std::unique_ptr<ast::UsingBlock> {
    auto block = std::make_unique<ast::UsingBlock>();
    block->location = currentLocation();
    expect(TokenType::Using, "Expected 'using'");

    block->module = parseTypeName();
    skipNewlines();

    // using Module do...end (scoped) or using Module (bare import, rest of scope)
    if (match(TokenType::Do)) {
        skipNewlines();
        while (!check(TokenType::End) && !atEnd()) {
            block->body.push_back(parseExpr());
            skipNewlines();
        }
        expect(TokenType::End, "Expected 'end' to close using block");
    }
    // Bare using — no body (applies to enclosing scope)

    return block;
}

auto Parser::parseMainBlock() -> std::unique_ptr<ast::MainBlock> {
    auto block = std::make_unique<ast::MainBlock>();
    block->location = currentLocation();
    expect(TokenType::Main, "Expected 'main'");

    // Optional params: main(args) do ... end — bound to the script's
    // command-line arguments ([String]) when the body runs.
    if (match(TokenType::LParen)) {
        if (!check(TokenType::RParen)) {
            block->params = parseParams();
        }
        expect(TokenType::RParen, "Expected ')' after main parameters");
    }

    expect(TokenType::Do, "Expected 'do' after 'main'");
    skipNewlines();

    while (!check(TokenType::End) && !atEnd()) {
        block->body.push_back(parseExpr());
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close main block");
    return block;
}

auto Parser::parsePragma() -> std::unique_ptr<ast::Pragma> {
    auto pragma = std::make_unique<ast::Pragma>();
    pragma->location = currentLocation();
    expect(TokenType::HashLBracket, "Expected '#['");

    while (check(TokenType::UpperIdent)) {
        pragma->requirements.push_back(advance().value);
        match(TokenType::Comma); // optional comma
    }

    expect(TokenType::RBracket, "Expected ']' to close pragma");
    return pragma;
}

auto Parser::parseVisibilityBlock() -> std::unique_ptr<ast::VisibilityBlock> {
    auto block = std::make_unique<ast::VisibilityBlock>();
    block->isPublic = check(TokenType::Public);
    advance(); // consume public/private

    expect(TokenType::Do, "Expected 'do' after visibility keyword");
    skipNewlines();

    while (!check(TokenType::End) && !atEnd()) {
        if (check(TokenType::Foul) && peekNext().type == TokenType::Let) {
            advance();
            block->items.push_back(parseFunctionDef(true));
        } else if (check(TokenType::Let)) {
            block->items.push_back(parseFunctionDef());
        } else if (check(TokenType::LowerIdent)) {
            block->items.push_back(parseTypeAnnotation());
        } else if (check(TokenType::Make)) {
            block->items.push_back(parseMakeDef());
        } else if (check(TokenType::Type)) {
            block->items.push_back(parseTypeDef());
        } else if (check(TokenType::Record)) {
            block->items.push_back(parseRecordDef());
        } else {
            error("Unexpected token in visibility block: " +
                  std::string(tokenTypeName(peek().type)));
        }
        skipNewlines();
    }

    expect(TokenType::End, "Expected 'end' to close visibility block");
    return block;
}

auto Parser::parseBody() -> std::vector<ast::ExprPtr> {
    std::vector<ast::ExprPtr> body;
    while (!check(TokenType::End) && !atEnd()) {
        body.push_back(parseExpr());
        skipNewlines();
    }
    return body;
}

auto Parser::isAtExprStart() const -> bool {
    switch (peek().type) {
        case TokenType::Integer:
        case TokenType::Float:
        case TokenType::String:
        case TokenType::True:
        case TokenType::False:
        case TokenType::None:
        case TokenType::Atom:
        case TokenType::LowerIdent:
        case TokenType::UpperIdent:
        case TokenType::This:
        case TokenType::LParen:
        case TokenType::LBracket:
        case TokenType::LBrace:
        case TokenType::If:
        case TokenType::Match:
        case TokenType::Receive:
        case TokenType::Loop:
        case TokenType::Let:
        case TokenType::Var:
        case TokenType::Return:
        case TokenType::Spawn:
        case TokenType::Do:
        case TokenType::Amp:
        case TokenType::Minus:
        case TokenType::Bang:
        case TokenType::DotDotDot:
            return true;
        default:
            return false;
    }
}

} // namespace kex
