#include "../evaluator.hxx"

namespace kex::interpreter {

auto Evaluator::registerWebBuiltins() -> void {
    // Kex.Intrinsic.Web.serve(server). The tree-walking interpreter has no
    // socket reactor, but returning a typed error keeps configuration and
    // unit tests usable outside BEAM.
    auto value = std::make_shared<Value>();
    value->data = FunctionValue{"serve", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::error(Value::string("Web.Server requires BEAM backend"));
    }};
    m_globalEnv->define("serve", std::move(value));
}

} // namespace kex::interpreter
