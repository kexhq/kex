#include "../evaluator.hxx"
#include "../../common/color.hxx"
#include <fstream>
#include <iostream>
#include <sstream>

namespace kex::interpreter {

// Everything IO-shaped is namespaced under IO — there is no bare
// print()/println() anymore. Console: IO.print/IO.printLine (write,
// with/without trailing newline; IO.put/IO.putLine are aliases) and
// IO.get/IO.getLine (read one character / one line from stdin). Whole-
// file read/write also live here (IO.read/IO.write); the remaining
// filesystem-specific operations (append, exists?, delete, lines, feed)
// stay under File (see file.cxx).
auto Evaluator::registerIOBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    // Pre-register the namespace placeholder so `IO.printLine(...)` resolves
    // via the empty-RecordValue namespace-dispatch branch in eval()
    // (ast::MethodCall) and gets the mangled "IO::" dispatch.
    m_globalEnv->define("IO", Value::record("IO", {}));

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

    // die(msg) — print msg to stderr and terminate the process.
    // Typed as String -> Void (never returns).
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

    // IO.read(path) -> String | None
    reg("IO::read", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::none();
        std::ifstream file(pathStr->value, std::ios::binary);
        if (!file.is_open()) return Value::none();
        std::ostringstream buffer;
        buffer << file.rdbuf();
        if (file.bad()) return Value::none();
        return Value::string(buffer.str());
    });

    // IO.write(path, content) -> Bool
    reg("IO::write", [](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.size() < 2) return Value::boolean(false);
        auto* pathStr = std::get_if<StringValue>(&args[0]->data);
        if (!pathStr) return Value::boolean(false);
        std::string content = args[1]->toString();
        std::ofstream file(pathStr->value, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return Value::boolean(false);
        file << content;
        return Value::boolean(static_cast<bool>(file));
    });
}

} // namespace kex::interpreter
