#include "../evaluator.hxx"
#include "../../common/color.hxx"
#include <exception>
#include <iostream>

namespace kex::interpreter {

// Minimal RSpec-style framework: `describe`/`it`/`before`/`after`/`assert`.
// `Mock.*`/`using Test`/the `kex test` CLI subcommand are separate concerns —
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
            m_testHookScopes.emplace_back();
            std::exception_ptr failure;
            try {
                fn->native({});
            } catch (...) {
                failure = std::current_exception();
            }
            for (auto hook = m_testHookScopes.back().afterAll.rbegin();
                 hook != m_testHookScopes.back().afterAll.rend(); ++hook) {
                try {
                    auto* hookFn = std::get_if<FunctionValue>(&(*hook)->data);
                    if (!hookFn || !hookFn->native)
                        throw std::runtime_error("after(:all) requires a block");
                    hookFn->native({});
                } catch (...) {
                    if (!failure) failure = std::current_exception();
                }
            }
            m_testHookScopes.pop_back();
            m_testDepth--;
            if (failure) std::rethrow_exception(failure);
        }
        return Value::none();
    });

    auto registerHook = [this](std::vector<ValuePtr> args, bool isAfter) -> ValuePtr {
        if (m_testHookScopes.empty())
            throw std::runtime_error(std::string(isAfter ? "after" : "before") +
                                     " must be declared inside describe");
        auto scope = std::string("each");
        size_t blockIndex = 0;
        if (args.size() == 2) {
            auto* atom = std::get_if<AtomValue>(&args[0]->data);
            if (!atom || (atom->name != "each" && atom->name != "all"))
                throw std::runtime_error("test hook scope must be :each or :all");
            scope = atom->name;
            blockIndex = 1;
        }
        if (args.size() <= blockIndex ||
            !std::holds_alternative<FunctionValue>(args[blockIndex]->data))
            throw std::runtime_error(std::string(isAfter ? "after" : "before") +
                                     " requires a block");
        if (scope == "all" && !isAfter) {
            auto* fn = std::get_if<FunctionValue>(&args[blockIndex]->data);
            if (!fn || !fn->native) throw std::runtime_error("before(:all) requires a block");
            fn->native({});
            return Value::unit();
        }
        if (scope == "all") {
            m_testHookScopes.back().afterAll.push_back(args[blockIndex]);
            return Value::unit();
        }
        auto& hooks = isAfter ? m_testHookScopes.back().after
                              : m_testHookScopes.back().before;
        hooks.push_back(args[blockIndex]);
        return Value::unit();
    };
    reg("before", [registerHook](std::vector<ValuePtr> args) mutable -> ValuePtr {
        return registerHook(std::move(args), false);
    });
    reg("after", [registerHook](std::vector<ValuePtr> args) mutable -> ValuePtr {
        return registerHook(std::move(args), true);
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
            std::exception_ptr failure;
            auto run = [&](const ValuePtr& hook) {
                auto* hookFn = std::get_if<FunctionValue>(&hook->data);
                if (!hookFn || !hookFn->native)
                    throw std::runtime_error("test hook requires a block");
                hookFn->native({});
            };
            try {
                for (const auto& scope : m_testHookScopes)
                    for (const auto& hook : scope.before) run(hook);
                fn->native({});
            } catch (...) {
                failure = std::current_exception();
            }
            // Teardown is unconditional. Run inner scopes first and reverse
            // declaration order within each scope, preserving the first error.
            for (auto scope = m_testHookScopes.rbegin();
                 scope != m_testHookScopes.rend(); ++scope) {
                for (auto hook = scope->after.rbegin(); hook != scope->after.rend(); ++hook) {
                    try {
                        run(*hook);
                    } catch (...) {
                        if (!failure) failure = std::current_exception();
                    }
                }
            }
            if (!failure) {
                m_testsPassed++;
                line = indent + color::apply(color::green) + "\xE2\x9C\x93" +
                       color::apply(color::reset) + " " + label + "\n"; // ✓
            } else {
                m_testsFailed++;
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception& e) {
                    line = indent + color::apply(color::red) + "\xE2\x9C\x97" +
                           color::apply(color::reset) + " " + label + ": " + e.what() + "\n"; // ✗
                } catch (...) {
                    line = indent + color::apply(color::red) + "\xE2\x9C\x97" +
                           color::apply(color::reset) + " " + label + ": unknown error\n";
                }
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
