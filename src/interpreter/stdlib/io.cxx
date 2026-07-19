#include "../evaluator.hxx"
#include "../../common/color.hxx"
#include <iostream>

namespace kex::interpreter {

auto Evaluator::registerIOBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };
    auto regDual = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, fn};
        m_globalEnv->define(name, val);
        defineIntrinsic(name, std::move(fn));
    };
    auto aliasDual = [this](const std::string& alias, const std::string& target) {
        auto value = m_globalEnv->get(target);
        m_globalEnv->define(alias, value);
        defineIntrinsic(alias, value);
    };

    m_globalEnv->define("IO", Value::module("IO"));

    // IO.printLine(msg...) — stringify args, write to stdout, trailing newline.
    regDual("IO::printLine", [this](std::vector<ValuePtr> args) -> ValuePtr {
        std::string out;
        for (const auto& arg : args) out += arg->toString();
        out += "\n";
        m_output += out;
        if (m_mockIO) {
            m_mockIOOutput += out;
        } else {
            std::cout << out;
        }
        return Value::unit();
    });

    // IO.print(msg...) — like printLine but without the trailing newline.
    regDual("IO::print", [this](std::vector<ValuePtr> args) -> ValuePtr {
        std::string out;
        for (const auto& arg : args) out += arg->toString();
        m_output += out;
        if (m_mockIO) {
            m_mockIOOutput += out;
        } else {
            std::cout << out;
        }
        return Value::unit();
    });

    // IO.inspect(val) — pretty-prints an inspect representation to stderr
    // then returns the value unchanged so it can be inserted mid-pipeline.
    regDual("IO::inspect", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        const auto& val = args[0];
        if (m_mockIO) {
            m_mockIOOutput += val->inspect() + " : " + val->typeName() + "\n";
        } else {
            std::cerr << val->inspect() << " "
                      << kex::color::apply(kex::color::gray) << ":"
                      << kex::color::apply(kex::color::reset) << " "
                      << kex::color::apply(kex::color::cyan) << val->typeName()
                      << kex::color::apply(kex::color::reset) << "\n";
        }
        return val;
    });

    // IO.put/IO.putLine — aliases of IO.print/IO.printLine.
    aliasDual("IO::put", "IO::print");
    aliasDual("IO::putLine", "IO::printLine");

    // IO.printError(msg) — write a line to stderr (no exit).
    regDual("IO::printError", [this](std::vector<ValuePtr> args) -> ValuePtr {
        std::string out;
        for (const auto& a : args) out += a->toString();
        out += "\n";
        m_output += out;
        if (m_mockIO) {
            m_mockIOOutput += out;
        } else {
            std::cerr << out;
        }
        return Value::unit();
    });

    // IO.warn / IO.warning — aliases for printError.
    aliasDual("IO::warn", "IO::printError");
    aliasDual("IO::warning", "IO::printError");

    // System.exit(code) — terminate with the given numeric exit code.
    defineIntrinsic("System::exit", [](std::vector<ValuePtr> args) -> ValuePtr {
        int code = 0;
        if (!args.empty()) {
            if (auto* i = std::get_if<IntValue>(&args[0]->data)) code = static_cast<int>(i->value);
        }
        std::exit(code);
    });

    // die(msg) — print msg to stderr and terminate the process.
    reg("die", [](std::vector<ValuePtr> args) -> ValuePtr {
        std::string msg = args.empty() ? "program terminated" : args[0]->toString();
        std::cerr << "fatal: " << msg << "\n";
        std::exit(1);
    });

    // IO.getLine() — reads one line from stdin (or mock input). Returns
    // String, or None at EOF / when mock input is exhausted.
    regDual("IO::getLine", [this](std::vector<ValuePtr>) -> ValuePtr {
        if (m_mockIO) {
            if (m_mockIOInputLines.empty()) return Value::none();
            auto line = m_mockIOInputLines.front();
            m_mockIOInputLines.pop_front();
            return Value::string(line);
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            return Value::none();
        }
        return Value::string(line);
    });

    // IO.get() — reads a single character from stdin (or mock input).
    // Returns a one-character String, or None at EOF.
    regDual("IO::get", [this](std::vector<ValuePtr>) -> ValuePtr {
        if (m_mockIO) {
            if (m_mockIOInputLines.empty()) return Value::none();
            auto& front = m_mockIOInputLines.front();
            if (front.empty()) {
                m_mockIOInputLines.pop_front();
                return Value::string("\n");
            }
            char c = front[0];
            front.erase(0, 1);
            return Value::string(std::string(1, c));
        }
        int c = std::cin.get();
        if (c == EOF) return Value::none();
        return Value::string(std::string(1, static_cast<char>(c)));
    });

    // ── Mock.IO ──────────────────────────────────────────────────────
    // When active, IO.print*/printError/warn write to an in-memory
    // buffer instead of stdout/stderr, and IO.getLine/get consume
    // pre-staged lines instead of reading stdin.
    //
    //   Mock.IO.start()              — activate mock mode
    //   Mock.IO.input("line", ...)   — stage input lines for getLine/get
    //   Mock.IO.output()             — return captured output as a String
    //   Mock.IO.clear()              — reset output buffer + input queue
    //   Mock.IO.stop()               — deactivate mock mode and clear

    auto mockStart = [this](std::vector<ValuePtr>) -> ValuePtr {
        m_mockIO = true;
        m_mockIOOutput.clear();
        m_mockIOInputLines.clear();
        return Value::unit();
    };

    auto mockInput = [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() == 1 && std::holds_alternative<ListValue>(args[0]->data)) {
            for (const auto& elem : std::get<ListValue>(args[0]->data).elements)
                m_mockIOInputLines.push_back(elem->toString());
        } else {
            for (const auto& a : args)
                m_mockIOInputLines.push_back(a->toString());
        }
        return Value::unit();
    };

    auto mockOutput = [this](std::vector<ValuePtr>) -> ValuePtr {
        return Value::string(m_mockIOOutput);
    };

    auto mockClear = [this](std::vector<ValuePtr>) -> ValuePtr {
        m_mockIOOutput.clear();
        m_mockIOInputLines.clear();
        return Value::unit();
    };

    auto mockStop = [this](std::vector<ValuePtr>) -> ValuePtr {
        m_mockIO = false;
        m_mockIOOutput.clear();
        m_mockIOInputLines.clear();
        return Value::unit();
    };

    // The fixed-arity Mock.IO controls are source-owned. `input` alone stays
    // public-native because its existing API is variadic while Kex function
    // declarations currently have fixed arity.
    reg("Mock::IO::input", mockInput);
    defineIntrinsic("IO::ioMockStart", std::move(mockStart));
    defineIntrinsic("IO::ioMockInput", std::move(mockInput));
    defineIntrinsic("IO::ioMockOutput", std::move(mockOutput));
    defineIntrinsic("IO::ioMockClear", std::move(mockClear));
    defineIntrinsic("IO::ioMockStop", std::move(mockStop));
}

} // namespace kex::interpreter
