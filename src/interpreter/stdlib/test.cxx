#include "../evaluator.hxx"
#include <iostream>

namespace kex::interpreter {

// Minimal real subset of the RSpec-style framework sketched in
// docs/testing.md: `describe`/`it`/`assert`. `before`/`Mock.*`/`using Test`/
// the `kex test` CLI subcommand described there are not implemented yet —
// `using` blocks are currently a no-op (see execTopLevel), so these are
// always-in-scope globals rather than something you import.
auto Evaluator::registerTestBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // describe(name) do ... end — purely organizational: prints a header
    // and runs its block (nested describe/it calls included). Indentation
    // tracks nesting depth.
    reg("describe", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto line = std::string(m_testDepth * 2, ' ') + args[0]->toString() + "\n";
        m_output += line;
        std::cout << line;

        auto* fn = args.size() > 1 ? std::get_if<FunctionValue>(&args[1]->data) : nullptr;
        if (fn && fn->native) {
            m_testDepth++;
            try {
                fn->native({});
            } catch (...) {
                m_testDepth--;
                throw;
            }
            m_testDepth--;
        }
        return Value::none();
    });

    // it(name) do ... end — runs a test case. Any exception escaping the
    // block (typically a failed `assert`, but also an ordinary bug in the
    // code under test) marks it failed and the message is shown, without
    // aborting the rest of the suite.
    reg("it", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto label = args[0]->toString();
        std::string indent(m_testDepth * 2, ' ');
        auto* fn = args.size() > 1 ? std::get_if<FunctionValue>(&args[1]->data) : nullptr;

        std::string line;
        if (!fn || !fn->native) {
            line = indent + "? " + label + " (no block)\n";
        } else {
            try {
                fn->native({});
                m_testsPassed++;
                line = indent + "\xE2\x9C\x93 " + label + "\n"; // ✓
            } catch (const std::exception& e) {
                m_testsFailed++;
                line = indent + "\xE2\x9C\x97 " + label + ": " + e.what() + "\n"; // ✗
            }
        }
        m_output += line;
        std::cout << line;
        return Value::none();
    });

    // assert(value) / assert(value, message) — throws (caught by the
    // enclosing `it`) if value is falsy. Outside of `it`, the failure
    // propagates as an ordinary uncaught runtime error.
    reg("assert", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (!args.empty() && args[0]->isTrue()) return Value::boolean(true);
        std::string msg = "assertion failed";
        if (args.size() > 1) msg += ": " + args[1]->toString();
        throw std::runtime_error(msg);
    });
}

} // namespace kex::interpreter
