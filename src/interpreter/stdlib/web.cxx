#include "../evaluator.hxx"

namespace kex::interpreter {

auto Evaluator::registerWebBuiltins() -> void {
    // Kex.Intrinsic.Web.serve(server). The tree-walking interpreter has no
    // socket reactor, but returning a typed error keeps configuration and
    // unit tests usable outside BEAM.
    defineIntrinsic("Web::serve", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::error(Value::string("Web.Server requires BEAM backend"));
    });
}

} // namespace kex::interpreter
