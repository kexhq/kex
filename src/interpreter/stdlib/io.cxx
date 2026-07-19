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

    m_globalEnv->define("IO", Value::module("IO"));
    m_globalEnv->define("System", Value::module("System"));

    // IO.printLine(msg...) — stringify args, write to stdout, trailing newline.
    reg("IO::printLine", [this](std::vector<ValuePtr> args) -> ValuePtr {
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
    reg("IO::print", [this](std::vector<ValuePtr> args) -> ValuePtr {
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
    reg("IO::inspect", [this](std::vector<ValuePtr> args) -> ValuePtr {
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
    m_globalEnv->define("IO::put", m_globalEnv->get("IO::print"));
    m_globalEnv->define("IO::putLine", m_globalEnv->get("IO::printLine"));

    // IO.printError(msg) — write a line to stderr (no exit).
    reg("IO::printError", [this](std::vector<ValuePtr> args) -> ValuePtr {
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
    m_globalEnv->define("IO::warn",    m_globalEnv->get("IO::printError"));
    m_globalEnv->define("IO::warning", m_globalEnv->get("IO::printError"));

    // System.exit(code) — terminate with the given numeric exit code.
    reg("System::exit", [](std::vector<ValuePtr> args) -> ValuePtr {
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
    reg("IO::getLine", [this](std::vector<ValuePtr>) -> ValuePtr {
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
    reg("IO::get", [this](std::vector<ValuePtr>) -> ValuePtr {
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

    // These namespace functions intentionally have both a public native
    // binding and a Kex.Intrinsic identity.
    for (const char* name : {
             "IO::get", "IO::getLine", "IO::inspect", "IO::print",
             "IO::printError", "IO::printLine", "IO::put", "IO::putLine",
             "IO::warn", "IO::warning", "System::exit"}) {
        if (auto value = m_globalEnv->get(name)) defineIntrinsic(name, value);
    }

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

    // Mock namespace is defined in file.cxx; make sure it exists.
    if (!m_globalEnv->has("Mock"))
        m_globalEnv->define("Mock", Value::module("Mock"));

    reg("Mock::IO", [](std::vector<ValuePtr>) -> ValuePtr {
        return Value::module("Mock::IO");
    });

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

    reg("Mock::IO::start", mockStart);
    reg("Mock::IO::input", mockInput);
    reg("Mock::IO::output", mockOutput);
    reg("Mock::IO::clear", mockClear);
    reg("Mock::IO::stop", mockStop);
    defineIntrinsic("IO::ioMockStart", std::move(mockStart));
    defineIntrinsic("IO::ioMockInput", std::move(mockInput));
    defineIntrinsic("IO::ioMockOutput", std::move(mockOutput));
    defineIntrinsic("IO::ioMockClear", std::move(mockClear));
    defineIntrinsic("IO::ioMockStop", std::move(mockStop));
}

} // namespace kex::interpreter
