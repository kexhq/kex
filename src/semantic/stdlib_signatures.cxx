#include "stdlib_signatures.hxx"

namespace kex::semantic {

namespace {

// Table-level generic placeholders. Negative ids so they never collide
// with TypeChecker::freshTypeVar()'s per-call-site fresh variables (which
// start at 0 and increment) — these just describe the *shape* of a
// signature, not a live inference variable.
auto genA() -> TypePtr { return Type::typeVar(-1); }
auto genE() -> TypePtr { return Type::typeVar(-2); }

auto integerLike() -> TypePtr { return Type::constrained("T", "Integer"); }
auto numberLike() -> TypePtr { return Type::constrained("T", "Number"); }
auto resultable() -> TypePtr { return Type::constrained("T", "Resultable"); }
auto optionable() -> TypePtr { return Type::constrained("T", "Optionable"); }

} // namespace

auto SignatureTable::define(Signature sig) -> void {
    m_signatures[sig.name].push_back(std::move(sig));
}

auto SignatureTable::lookup(const std::string& name) const -> const std::vector<Signature>* {
    auto it = m_signatures.find(name);
    return it == m_signatures.end() ? nullptr : &it->second;
}

auto SignatureTable::withStdlib() -> SignatureTable {
    SignatureTable table;
    auto sig = [&table](std::string name, std::vector<TypePtr> params, TypePtr result) {
        table.define(Signature{std::move(name), std::move(params), std::move(result)});
    };

    // src/interpreter/stdlib/integer.cxx
    sig("even?", {integerLike()}, Type::boolean());
    sig("odd?", {integerLike()}, Type::boolean());
    sig("abs", {numberLike()}, numberLike());
    sig("modulo", {integerLike(), integerLike()}, Type::integer());
    sig("in?", {numberLike(), Type::named("Range", {numberLike()})}, Type::boolean());
    sig("times", {integerLike(), Type::func({integerLike()}, Type::unit())}, Type::unit());
    sig("Integer::parse", {Type::string()},
        Type::named("Result", {Type::integer(), Type::string()}));
    sig("Float::parse", {Type::string()},
        Type::named("Result", {Type::float64(), Type::string()}));

    // src/interpreter/stdlib/adt.cxx
    sig("ok?", {resultable()}, Type::boolean());
    sig("error?", {resultable()}, Type::boolean());
    sig("some?", {optionable()}, Type::boolean());
    sig("none?", {optionable()}, Type::boolean());
    // `or` works on either prelude ADT family (unlike ok?/error?/some?/
    // none?), so it's two overloads rather than one constrained param.
    sig("or", {resultable(), genA()}, genA());
    sig("or", {optionable(), genA()}, genA());

    // src/interpreter/stdlib/math.cxx
    sig("Math::sqrt", {numberLike()}, Type::float64());
    sig("Math::cbrt", {numberLike()}, Type::float64());
    sig("Math::sin", {numberLike()}, Type::float64());
    sig("Math::cos", {numberLike()}, Type::float64());
    sig("Math::tan", {numberLike()}, Type::float64());
    sig("Math::log", {numberLike()}, Type::float64());
    sig("Math::log2", {numberLike()}, Type::float64());
    sig("Math::log10", {numberLike()}, Type::float64());
    sig("Math::pow", {numberLike(), numberLike()}, Type::float64());
    sig("Math::hypot", {numberLike(), numberLike()}, Type::float64());
    sig("Math::atan2", {numberLike(), numberLike()}, Type::float64());

    // src/interpreter/stdlib/string.cxx, list.cxx, map.cxx
    sig("digit?", {Type::charT()}, Type::boolean());
    sig("contains?", {Type::string(), Type::string()}, Type::boolean());
    sig("empty?", {Type::list(genA())}, Type::boolean());
    sig("has?", {Type::map(genA(), genE()), genA()}, Type::boolean());

    return table;
}

} // namespace kex::semantic
