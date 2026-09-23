#include "../evaluator.hxx"

namespace kex::interpreter {

// The tree walker is a single process tree inside one OS process: it has no
// node name and nothing to connect to. These answers are the ones an unnamed
// BEAM node gives, so a program that checks `Node.alive?` first behaves the
// same on both backends.
auto Evaluator::registerNodeBuiltins() -> void {
    defineModule("Node");
    defineIntrinsic("Node::self", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::atom("nonode@nohost");
    });
    defineIntrinsic("Node::alive?", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::boolean(false);
    });
    defineIntrinsic("Node::list", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::list({});
    });
    defineIntrinsic("Node::start", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::error(Value::string(
            "the tree-walk interpreter cannot be a distributed node; run on the BEAM"));
    });
    defineIntrinsic("Node::stop", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::boolean(false);
    });
    defineIntrinsic("Node::connect", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::boolean(false);
    });
    defineIntrinsic("Node::disconnect", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::boolean(false);
    });
    defineIntrinsic("Node::setCookie", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::unit();
    });
    defineIntrinsic("Node::send", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::unit();
    });
    defineIntrinsic("Node::whereIs", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::none();
    });
}

} // namespace kex::interpreter
