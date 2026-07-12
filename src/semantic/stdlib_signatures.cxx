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

    // src/interpreter/stdlib/number.cxx
    sig("even?", {integerLike()}, Type::boolean());
    sig("odd?", {integerLike()}, Type::boolean());
    sig("abs", {numberLike()}, numberLike());
    sig("modulo", {integerLike(), integerLike()}, Type::integer());
    sig("in?", {numberLike(), Type::named("Range", {numberLike()})}, Type::boolean());
    sig("in?", {Type::charT(), Type::named("Range", {Type::charT()})}, Type::boolean());
    sig("times", {integerLike(), Type::func({integerLike()}, Type::unit())}, Type::unit());
    sig("Integer::parse", {Type::string()},
        Type::named("Result", {Type::integer(), Type::named("ParseError")}));
    sig("Integer::parsePrefix", {Type::string()},
        Type::optional(Type::tuple({Type::integer(), Type::string()})));
    sig("Float::parse", {Type::string()},
        Type::named("Result", {Type::float64(), Type::named("ParseError")}));
    sig("Float::parsePrefix", {Type::string()},
        Type::optional(Type::tuple({Type::float64(), Type::string()})));
    sig("Number::parse", {Type::string()},
        Type::named("Result", {numberLike(), Type::named("ParseError")}));

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
    sig("Math::log", {numberLike(), numberLike()}, Type::float64());
    sig("Math::log2", {numberLike()}, Type::float64());
    sig("Math::log10", {numberLike()}, Type::float64());
    sig("Math::pow", {numberLike(), numberLike()}, Type::float64());
    sig("Math::hypot", {numberLike(), numberLike()}, Type::float64());
    sig("Math::atan2", {numberLike(), numberLike()}, Type::float64());

    // src/interpreter/stdlib/list.cxx
    sig("first",  {Type::list(genA())}, Type::optional(genA()));
    sig("first",  {Type::string()},     Type::optional(Type::charT()));
    sig("second", {Type::list(genA())}, Type::optional(genA()));
    sig("second", {Type::string()},     Type::optional(Type::charT()));
    sig("third",  {Type::list(genA())}, Type::optional(genA()));
    sig("third",  {Type::string()},     Type::optional(Type::charT()));
    sig("last",   {Type::list(genA())}, Type::optional(genA()));
    sig("last",   {Type::string()},     Type::optional(Type::charT()));
    sig("rest",  {Type::list(genA())}, Type::list(genA()));
    sig("rest",  {Type::string()},     Type::string());
    sig("take",  {Type::list(genA()), Type::integer()}, Type::list(genA()));
    sig("take",  {Type::string(),      Type::integer()}, Type::string());
    sig("drop",  {Type::list(genA()), Type::integer()}, Type::list(genA()));
    sig("drop",  {Type::string(),      Type::integer()}, Type::string());
    sig("push",  {Type::list(genA()), genA()}, Type::list(genA()));
    sig("map",   {Type::list(genA()), Type::func({genA()}, genE())}, Type::list(genE()));
    sig("map",   {Type::string(),     Type::func({Type::charT()}, Type::charT())}, Type::string());
    sig("filter",{Type::list(genA()), Type::func({genA()}, Type::boolean())}, Type::list(genA()));
    sig("filter",{Type::string(),     Type::func({Type::charT()}, Type::boolean())}, Type::string());
    sig("reject",{Type::list(genA()), Type::func({genA()}, Type::boolean())}, Type::list(genA()));
    sig("reject",{Type::string(),     Type::func({Type::charT()}, Type::boolean())}, Type::string());
    sig("each",  {Type::list(genA()), Type::func({genA()}, Type::unit())}, Type::unit());
    sig("each",  {Type::string(),     Type::func({Type::charT()}, Type::unit())}, Type::unit());
    sig("each",  {Type::map(genA(), genE()), Type::func({genA(), genE()}, Type::unit())}, Type::unit());
    sig("reduce",{Type::list(genA()), genE(), Type::func({genE(), genA()}, genE())}, genE());
    sig("any?",  {Type::list(genA()), Type::func({genA()}, Type::boolean())}, Type::boolean());
    sig("all?",  {Type::list(genA()), Type::func({genA()}, Type::boolean())}, Type::boolean());
    sig("count", {Type::list(genA())}, Type::integer());
    sig("count", {Type::string()},     Type::integer());
    sig("count", {Type::map(genA(), genE())}, Type::integer());
    sig("count", {Type::list(genA()), Type::func({genA()}, Type::boolean())}, Type::integer());
    sig("sort",  {Type::list(genA())}, Type::list(genA()));
    sig("sort",  {Type::list(genA()), Type::func({genA(), genA()}, Type::boolean())}, Type::list(genA()));
    sig("min",   {Type::list(genA())}, Type::optional(genA()));
    sig("min",   {Type::list(genA()), Type::func({genA()}, Type::typeVar(-3))}, Type::optional(genA()));
    sig("max",   {Type::list(genA())}, Type::optional(genA()));
    sig("max",   {Type::list(genA()), Type::func({genA()}, Type::typeVar(-3))}, Type::optional(genA()));
    sig("sum",   {Type::list(numberLike())}, numberLike());
    sig("sum",   {Type::list(genA()), Type::func({genA()}, genE())}, genE());
    sig("product", {Type::list(numberLike())}, numberLike());
    sig("product", {Type::list(genA()), Type::func({genA()}, genE())}, genE());
    sig("flatMap", {Type::list(genA()), Type::func({genA()}, Type::list(genE()))}, Type::list(genE()));
    sig("flatMap", {Type::optional(genA()), Type::func({genA()}, Type::optional(genE()))}, Type::optional(genE()));
    sig("collect", {Type::list(genA()), Type::func({genA()}, Type::optional(genE()))}, Type::list(genE()));
    sig("join",  {Type::list(Type::string()), Type::string()}, Type::string());
    sig("join",  {Type::list(Type::string())}, Type::string());
    sig("join",  {Type::string(), Type::string()}, Type::string());
    sig("join",  {Type::string()}, Type::string());
    sig("flatten",  {Type::list(Type::list(genA()))}, Type::list(genA()));
    sig("uniq",     {Type::list(genA())}, Type::list(genA()));
    sig("uniq",     {Type::string()}, Type::string());
    sig("partition",{Type::list(genA()), Type::func({genA()}, Type::boolean())},
                    Type::tuple({Type::list(genA()), Type::list(genA())}));
    sig("indexOf",  {Type::list(genA()), genA()}, Type::optional(Type::integer()));
    sig("zip",   {Type::list(genA()), Type::list(genE())},
                 Type::list(Type::tuple({genA(), genE()})));
    sig("to",    {genA(), genE()}, Type::optional(genE()));

    // src/interpreter/stdlib/file.cxx — File module
    sig("File::read",      {Type::string()}, Type::optional(Type::string()));
    sig("File::write",     {Type::string(), Type::string()}, Type::boolean());
    sig("File::append",    {Type::string(), Type::string()}, Type::boolean());
    sig("File::exists?",   {Type::string()}, Type::boolean());
    sig("File::file?",     {Type::string()}, Type::boolean());
    sig("File::directory?",      {Type::string()}, Type::boolean());
    sig("File::delete",    {Type::string()}, Type::boolean());
    sig("File::copy",      {Type::string(), Type::string()}, Type::boolean());
    sig("File::rename",    {Type::string(), Type::string()}, Type::boolean());
    sig("File::lines",     {Type::string()}, Type::optional(Type::list(Type::string())));
    sig("File::size",      {Type::string()}, Type::optional(Type::integer()));
    sig("File::basename",  {Type::string()}, Type::string());
    sig("File::dirname",   {Type::string()}, Type::string());
    sig("File::extension", {Type::string()}, Type::string());
    sig("File::join",      {Type::string(), Type::string()}, Type::string());
    sig("File::absolute",  {Type::string()}, Type::optional(Type::string()));

    // src/interpreter/stdlib/file.cxx — Directory module
    sig("Directory::exists?",   {Type::string()}, Type::boolean());
    sig("Directory::directory?",      {Type::string()}, Type::boolean());
    sig("Directory::file?",     {Type::string()}, Type::boolean());
    sig("Directory::create",    {Type::string()}, Type::boolean());
    sig("Directory::delete",    {Type::string()}, Type::boolean());
    sig("Directory::deleteAll", {Type::string()}, Type::boolean());
    sig("Directory::list",      {Type::string()}, Type::optional(Type::list(Type::string())));
    sig("Directory::files",     {Type::string()}, Type::optional(Type::list(Type::string())));
    sig("Directory::directories",      {Type::string()}, Type::optional(Type::list(Type::string())));

    // src/interpreter/stdlib/string.cxx, list.cxx, map.cxx
    sig("digit?",  {Type::charT()}, Type::boolean());
    sig("alpha?",  {Type::charT()}, Type::boolean());
    sig("space?",  {Type::charT()}, Type::boolean());
    sig("contains?", {Type::string(), Type::string()}, Type::boolean());
    sig("contains?", {Type::list(genA()), genA()}, Type::boolean());
    sig("contains?", {Type::named("Range", {genA()}), genA()}, Type::boolean());
    sig("empty?", {Type::list(genA())}, Type::boolean());
    sig("empty?", {Type::string()}, Type::boolean());
    sig("has?",   {Type::map(genA(), genE()), genA()}, Type::boolean());
    // at — element access by index
    sig("at",     {Type::string(), Type::integer()}, Type::optional(Type::charT()));
    sig("at",     {Type::list(genA()), Type::integer()}, Type::optional(genA()));
    // find — first element matching predicate
    sig("find",   {Type::list(genA()), Type::func({genA()}, Type::boolean())}, Type::optional(genA()));
    // map operations
    sig("get",    {Type::map(genA(), genE()), genA()}, Type::optional(genE()));
    sig("get",    {Type::map(genA(), genE()), genA(), genE()}, genE());
    sig("get",    {Type::list(genA()), Type::integer()}, Type::optional(genA()));
    sig("get",    {Type::list(genA()), Type::integer(), genA()}, genA());
    sig("put",    {Type::map(genA(), genE()), genA(), genE()}, Type::map(genA(), genE()));
    sig("delete", {Type::map(genA(), genE()), genA()}, Type::map(genA(), genE()));
    sig("keys",   {Type::map(genA(), genE())}, Type::list(genA()));
    sig("values", {Type::map(genA(), genE())}, Type::list(genE()));
    sig("each",   {Type::map(genA(), genE()), Type::func({genA(), genE()}, Type::unit())}, Type::unit());
    sig("map",    {Type::map(genA(), genE()), Type::func({genA(), genE()}, Type::typeVar(-3))}, Type::list(Type::typeVar(-3)));
    sig("mapValues", {Type::map(genA(), genE()), Type::func({genE()}, Type::typeVar(-3))}, Type::map(genA(), Type::typeVar(-3)));
    sig("mapKeys",   {Type::map(genA(), genE()), Type::func({genA()}, Type::typeVar(-3))}, Type::map(Type::typeVar(-3), genE()));
    sig("filter", {Type::map(genA(), genE()), Type::func({genA(), genE()}, Type::boolean())}, Type::map(genA(), genE()));
    sig("reject", {Type::map(genA(), genE()), Type::func({genA(), genE()}, Type::boolean())}, Type::map(genA(), genE()));
    sig("merge",  {Type::map(genA(), genE()), Type::map(genA(), genE())}, Type::map(genA(), genE()));
    sig("any?",   {Type::map(genA(), genE()), Type::func({genA(), genE()}, Type::boolean())}, Type::boolean());
    sig("all?",   {Type::map(genA(), genE()), Type::func({genA(), genE()}, Type::boolean())}, Type::boolean());
    sig("count",  {Type::map(genA(), genE()), Type::func({genA(), genE()}, Type::boolean())}, Type::integer());
    sig("find",   {Type::map(genA(), genE()), Type::func({genA(), genE()}, Type::boolean())}, Type::optional(Type::tuple({genA(), genE()})));
    sig("entries",{Type::map(genA(), genE())}, Type::list(Type::tuple({genA(), genE()})));

    // src/interpreter/stdlib/string.cxx
    sig("split",      {Type::string(), Type::string()}, Type::list(Type::string()));
    sig("trim",       {Type::string()}, Type::string());
    sig("upperCase",   {Type::string()}, Type::string());
    sig("upperCase",   {Type::charT()},  Type::charT());
    sig("lowerCase",   {Type::string()}, Type::string());
    sig("lowerCase",   {Type::charT()},  Type::charT());
    sig("reverse",    {Type::string()}, Type::string());
    sig("reverse",    {Type::list(genA())}, Type::list(genA()));
    sig("startsWith?", {Type::string(), Type::string()}, Type::boolean());
    sig("endsWith?",   {Type::string(), Type::string()}, Type::boolean());
    // IO — accept any value; result is Unit (used for side-effect checking)
    sig("printLine",   {genA()}, Type::unit());
    sig("print",       {genA()}, Type::unit());
    sig("readLine",    {}, Type::string());
    sig("inspect",     {genA()}, genA());  // passes value through, prints to stderr
    sig("IO::printError", {genA()}, Type::unit());
    sig("IO::warn",       {genA()}, Type::unit());
    sig("IO::warning",    {genA()}, Type::unit());
    sig("System::exit",   {Type::integer()}, Type::voidType());
    // Testing DSL: describe/it/assert
    sig("describe",     {Type::string(), Type::func({}, Type::unit())}, Type::unit());
    sig("it",           {Type::string(), Type::func({}, Type::unit())}, Type::unit());
    sig("assert",       {Type::boolean()}, Type::unit());
    sig("assert",       {Type::boolean(), Type::string()}, Type::unit());
    // die — never returns (diverges), so typed as Never (bottom type)
    sig("die",          {Type::string()}, Type::voidType());
    // Process primitives — args are opaque; typed permissively
    sig("send",    {genA(), genA()}, Type::unit());
    sig("worker",  {genA()}, Type::unit());
    sig("worker",  {genA(), genA()}, Type::unit());
    sig("supervisor", {}, Type::unit());
    sig("supervisor", {genA()}, Type::unit());
    sig("supervisor", {genA(), Type::func({}, Type::unit())}, Type::unit());

    return table;
}

} // namespace kex::semantic
