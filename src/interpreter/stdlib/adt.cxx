#include "../evaluator.hxx"

namespace kex::interpreter {

// Just(x), Ok(x), Error(e) are used throughout examples/docs without a
// local `type Option<A> = Just(A) | None` / `type Result<A,E> = Ok(A) |
// Error(E)` declaration, i.e. they're meant to be prelude builtins, not
// user-declared. (None/Nothing need no registration — bare NoneValue
// already works, and zero-arg variants work via the UpperIdentifier
// atom fallback.) A user `type Result<A,E> = Ok(A) | Error(E)` etc. that
// re-declares these is harmless — execTopLevel's TypeDef handling just
// redefines them identically.
auto Evaluator::registerAdtConstructors() -> void {
    auto regCtor1 = [this](const std::string& name) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, [name](std::vector<ValuePtr> args) -> ValuePtr {
            return Value::record(name, {{"0", args.empty() ? Value::none() : args[0]}});
        }};
        m_globalEnv->define(name, val);
    };
    regCtor1("Just");
    regCtor1("Ok");
    regCtor1("Error");
}

} // namespace kex::interpreter
