#include "../evaluator.hxx"
#include "../../common/color.hxx"
#include <iostream>

namespace kex::interpreter {

// Console I/O only: IO.print/IO.printLine (write, with/without trailing
// newline; IO.put/IO.putLine are aliases), IO.get/IO.getLine (read one
// character / one line from stdin), and IO.inspect. File I/O has moved
// entirely to File (see file.cxx).
auto Evaluator::registerIOBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // Pre-register the namespace placeholder so `IO.printLine(...)` resolves
    // via the empty-RecordValue namespace-dispatch branch in eval()
    // (ast::MethodCall) and gets the mangled "IO::" dispatch.
    m_globalEnv->define("IO", Value::module("IO"));
    m_globalEnv->define("System", Value::module("System"));

    // IO.printLine(msg...) — stringify args, write to stdout, trailing newline.
    reg("IO::printLine", [this](std::vector<ValuePtr> args) -> ValuePtr {
        for (const auto& arg : args) {
            auto str = arg->toString();
            m_output += str;
            std::cout << str;
        }
        m_output += "\n";
        std::cout << "\n";
        return Value::unit();
    });

    // IO.print(msg...) — like printLine but without the trailing newline.
    reg("IO::print", [this](std::vector<ValuePtr> args) -> ValuePtr {
        for (const auto& arg : args) {
            auto str = arg->toString();
            m_output += str;
            std::cout << str;
        }
        return Value::unit();
    });

    // IO.inspect(val) — pretty-prints an inspect representation to stderr
    // (colorful, with type name), then returns the value unchanged so it can
    // be inserted mid-pipeline: `xs.map(&double).inspect.filter(&even?)`.
    // Treated as pure for purity checking (compiler ignores it as a foul call).
    reg("IO::inspect", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        const auto& val = args[0];
        std::cerr << val->inspect() << " "
                  << kex::color::apply(kex::color::gray) << ":"
                  << kex::color::apply(kex::color::reset) << " "
                  << kex::color::apply(kex::color::cyan) << val->typeName()
                  << kex::color::apply(kex::color::reset) << "\n";
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
        std::cerr << out;
        m_output += out;
        return Value::unit();
    });

    // IO.warn / IO.warning — aliases for printError; softer signal names.
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
    // Typed as String -> Never (never returns).
    reg("die", [](std::vector<ValuePtr> args) -> ValuePtr {
        std::string msg = args.empty() ? "program terminated" : args[0]->toString();
        std::cerr << "fatal: " << msg << "\n";
        std::exit(1);
    });

    // IO.getLine() — reads one line from stdin. Returns String, or None at EOF.
    reg("IO::getLine", [](std::vector<ValuePtr>) -> ValuePtr {
        std::string line;
        if (!std::getline(std::cin, line)) {
            return Value::none();
        }
        return Value::string(line);
    });

    // IO.get() — reads a single character from stdin. Returns a one-
    // character String, or None at EOF.
    reg("IO::get", [](std::vector<ValuePtr>) -> ValuePtr {
        int c = std::cin.get();
        if (c == EOF) return Value::none();
        return Value::string(std::string(1, static_cast<char>(c)));
    });

}

} // namespace kex::interpreter
