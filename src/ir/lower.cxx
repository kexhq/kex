#include "lower.hxx"
#include "../lexer/token.hxx"

namespace kex::ir {

namespace {

// A pending `let name = value in ...` binding, accumulated while lowering a
// compound expression's operands into ANF, then wrapped (outermost-first)
// around the consuming expression.
struct Binding {
    std::string name;
    ExprPtr value;
};

struct Lowering {
    int counter = 0;

    auto fresh(const std::string& hint = "T") -> std::string {
        return "_ir_" + hint + std::to_string(counter++);
    }

    // Wrap `body` in the accumulated let-bindings, evaluated left-to-right
    // (so binds[0] is the outermost let).
    auto wrapLets(std::vector<Binding>& binds, ExprPtr body) -> ExprPtr {
        for (auto it = binds.rbegin(); it != binds.rend(); ++it) {
            auto let = std::make_unique<Expr>();
            let->node = Let{std::move(it->name), std::move(it->value), std::move(body)};
            body = std::move(let);
        }
        return body;
    }

    static auto isAtomic(const Expr& e) -> bool {
        return std::holds_alternative<Lit>(e.node) || std::holds_alternative<Var>(e.node);
    }

    // Lower an AST expr and, if the result is compound, bind it to a fresh
    // Let (recorded in `binds`) returning an atomic Var. Literals/vars pass
    // through untouched. This is the ANF normalization step.
    auto atomize(const ast::ExprPtr& e, std::vector<Binding>& binds) -> ExprPtr {
        auto ir = lower(e);
        if (isAtomic(*ir)) return ir;
        auto name = fresh();
        binds.push_back({name, std::move(ir)});
        return var(name);
    }

    auto opOf(TokenType t) -> Op {
        switch (t) {
            case TokenType::Plus: return Op::Add;
            case TokenType::Minus: return Op::Sub;
            case TokenType::Star: return Op::Mul;
            case TokenType::Slash: return Op::Div;
            case TokenType::Percent: return Op::Mod;
            case TokenType::EqEq: return Op::Eq;
            case TokenType::NotEq: return Op::Neq;
            case TokenType::LessThan: return Op::Lt;
            case TokenType::GreaterThan: return Op::Gt;
            case TokenType::LessEq: return Op::Lte;
            case TokenType::GreaterEq: return Op::Gte;
            case TokenType::AmpAmp: return Op::And;
            case TokenType::PipePipe: return Op::Or;
            default:
                throw LowerError("IR lower: unsupported binary operator");
        }
    }

    // ---- Expression lowering ---------------------------------------------
    auto lower(const ast::ExprPtr& e) -> ExprPtr {
        if (!e) return litBool(false);
        return std::visit([&](const auto& n) -> ExprPtr {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, ast::IntLiteral>) {
                return lit(LitKind::Int, n.value);
            } else if constexpr (std::is_same_v<T, ast::FloatLiteral>) {
                return lit(LitKind::Float, n.value);
            } else if constexpr (std::is_same_v<T, ast::StringLiteral>) {
                if (n.value.find("${") != std::string::npos)
                    throw LowerError("IR lower: string interpolation not yet ported");
                return lit(LitKind::String, n.value);
            } else if constexpr (std::is_same_v<T, ast::BoolLiteral>) {
                return litBool(n.value);
            } else if constexpr (std::is_same_v<T, ast::CharLiteral>) {
                return lit(LitKind::Char, std::string(1, n.value));
            } else if constexpr (std::is_same_v<T, ast::AtomLiteral>) {
                return lit(LitKind::Atom, n.name);
            } else if constexpr (std::is_same_v<T, ast::NoneLiteral>) {
                return lit(LitKind::None, "none");
            } else if constexpr (std::is_same_v<T, ast::Identifier>) {
                return var(n.name);
            } else if constexpr (std::is_same_v<T, ast::BinaryOp>) {
                std::vector<Binding> binds;
                auto l = atomize(n.left, binds);
                auto r = atomize(n.right, binds);
                auto ex = std::make_unique<Expr>();
                std::vector<ExprPtr> args;
                args.push_back(std::move(l));
                args.push_back(std::move(r));
                ex->node = Intrinsic{opOf(n.op), std::move(args)};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::UnaryOp>) {
                std::vector<Binding> binds;
                auto a = atomize(n.operand, binds);
                Op op = (n.op == TokenType::Minus) ? Op::Neg
                       : (n.op == TokenType::Bang) ? Op::Not
                       : throw LowerError("IR lower: unsupported unary operator");
                auto ex = std::make_unique<Expr>();
                std::vector<ExprPtr> args;
                args.push_back(std::move(a));
                ex->node = Intrinsic{op, std::move(args)};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::RangeExpr>) {
                // a..b  →  lists:seq(a, b)  (materialized ascending list)
                std::vector<Binding> binds;
                auto s = atomize(n.start, binds);
                auto en = atomize(n.end, binds);
                auto ex = std::make_unique<Expr>();
                std::vector<ExprPtr> args;
                args.push_back(std::move(s));
                args.push_back(std::move(en));
                ex->node = Call{"lists", "seq", 2, std::move(args), false};
                return wrapLets(binds, std::move(ex));
            } else if constexpr (std::is_same_v<T, ast::FunctionCall>) {
                return lowerFunctionCall(n);
            } else if constexpr (std::is_same_v<T, ast::MethodCall>) {
                return lowerMethodCall(n);
            } else {
                throw LowerError(std::string("IR lower: unimplemented expr node ")
                                 + typeid(T).name());
            }
        }, e->kind);
    }

    auto lowerFunctionCall(const ast::FunctionCall& n) -> ExprPtr {
        if (!n.namedArgs.empty() || n.block)
            throw LowerError("IR lower: named args / block calls not yet ported");
        std::vector<Binding> binds;
        std::vector<ExprPtr> args;
        for (const auto& a : n.args) args.push_back(atomize(a, binds));
        auto ex = std::make_unique<Expr>();
        ex->node = Call{"", n.name, static_cast<int>(n.args.size()), std::move(args), false};
        return wrapLets(binds, std::move(ex));
    }

    // Minimal UFCS/stdlib resolution for the walking skeleton. Only the
    // handful of forms an early target program needs; everything else errors
    // (to be widened as constructs are ported from core_erlang.cxx).
    auto lowerMethodCall(const ast::MethodCall& n) -> ExprPtr {
        // Namespace calls on an UpperIdentifier receiver, e.g. IO.printLine.
        if (auto* uid = std::get_if<ast::UpperIdentifier>(&n.receiver->kind)) {
            std::vector<Binding> binds;
            std::vector<ExprPtr> args;
            for (const auto& a : n.args) args.push_back(atomize(a, binds));
            if (uid->name == "IO") {
                std::string fn = n.method == "printLine" ? "print_line"
                               : n.method == "print"     ? "print"
                               : throw LowerError("IR lower: IO." + n.method + " not yet ported");
                auto ex = std::make_unique<Expr>();
                ex->node = Call{"kex_io", fn, static_cast<int>(args.size()), std::move(args), false};
                return wrapLets(binds, std::move(ex));
            }
            throw LowerError("IR lower: namespace call " + uid->name + "." + n.method
                             + " not yet ported");
        }
        // UFCS method on a value receiver.
        std::vector<Binding> binds;
        auto recv = atomize(n.receiver, binds);
        // Builtin methods with a direct runtime target (receiver-first).
        struct B { const char* method; const char* mod; const char* fn; };
        static const B builtins[] = {
            {"product", "kex_io", "list_product"},
        };
        for (const auto& b : builtins) {
            if (n.method == b.method && n.args.empty()) {
                std::vector<ExprPtr> args;
                args.push_back(std::move(recv));
                auto ex = std::make_unique<Expr>();
                ex->node = Call{b.mod, b.fn, 1, std::move(args), false};
                return wrapLets(binds, std::move(ex));
            }
        }
        throw LowerError("IR lower: UFCS method ." + n.method + " not yet ported");
    }

    // ---- Body lowering ----------------------------------------------------
    // A statement sequence: earlier statements bind throwaway names, the last
    // is the value.
    auto lowerBody(const std::vector<ast::ExprPtr>& body) -> ExprPtr {
        if (body.empty()) return lit(LitKind::Atom, "ok");
        return lowerBodyFrom(body, 0);
    }
    auto lowerBodyFrom(const std::vector<ast::ExprPtr>& body, size_t i) -> ExprPtr {
        if (i + 1 == body.size()) return lower(body[i]);
        auto val = lower(body[i]);
        auto rest = lowerBodyFrom(body, i + 1);
        auto let = std::make_unique<Expr>();
        let->node = Let{fresh("S"), std::move(val), std::move(rest)};
        return let;
    }

    // ---- Function / program ----------------------------------------------
    auto lowerFunction(const ast::FunctionDef& fn) -> FunDef {
        if (fn.clauses.size() != 1)
            throw LowerError("IR lower: multi-clause functions not yet ported");
        const auto& clause = fn.clauses[0];
        FunDef def;
        def.name = fn.name;
        def.arity = static_cast<int>(clause.params.size());
        FunClause fc;
        for (const auto& p : clause.params) {
            if (!p.name)
                throw LowerError("IR lower: non-simple parameter not yet ported");
            auto pat = std::make_unique<Pattern>();
            pat->kind = PatKind::Var;
            pat->name = *p.name;
            fc.params.push_back(std::move(pat));
        }
        fc.body = lowerBody(clause.body);
        def.clauses.push_back(std::move(fc));
        return def;
    }
};

} // namespace

auto lowerProgram(const ast::Program& prog, const std::string& fileStem) -> Module {
    Lowering L;
    Module mod;
    mod.name = "kex_" + fileStem;

    for (const auto& item : prog.items) {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<ast::FunctionDef>>) {
                if (node) mod.functions.push_back(L.lowerFunction(*node));
            } else if constexpr (std::is_same_v<T, std::unique_ptr<ast::MainBlock>>) {
                if (node && (node->isExplicitMain || !node->synthetic)) {
                    if (!node->params.empty())
                        throw LowerError("IR lower: main(args) not yet ported");
                    FunDef def;
                    def.name = "main";
                    def.arity = 0;
                    FunClause fc;
                    fc.body = L.lowerBody(node->body);
                    def.clauses.push_back(std::move(fc));
                    mod.functions.push_back(std::move(def));
                    mod.hasMain = true;
                    mod.mainArity = 0;
                }
            } else {
                throw LowerError(std::string("IR lower: unimplemented top-level item ")
                                 + typeid(T).name());
            }
        }, item);
    }
    return mod;
}

} // namespace kex::ir
