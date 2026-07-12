#include "../evaluator.hxx"
#include "../../common/color.hxx"

namespace kex::interpreter {

auto Evaluator::registerConsoleBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        auto val = std::make_shared<Value>();
        val->data = FunctionValue{name, std::move(fn)};
        m_globalEnv->define(name, val);
    };

    m_globalEnv->define("Console", Value::module("Console"));

    const auto constant = [&](const char* name, const char* ansi) {
        reg(std::string("Console::") + name, [ansi](std::vector<ValuePtr>) {
            return Value::string(color::apply(ansi));
        });
    };
    constant("Reset", color::reset);
    constant("Bold", color::bold);
    constant("Dim", color::dim);
    constant("Italic", color::italic);
    constant("Underline", color::underline);
    constant("Blink", color::blink);
    constant("Reverse", color::reverse);
    constant("Hidden", color::hidden);
    constant("Strikethrough", color::strikethrough);
    constant("Red", color::red);
    constant("Green", color::green);
    constant("Yellow", color::yellow);
    constant("Blue", color::blue);
    constant("Magenta", color::magenta);
    constant("Cyan", color::cyan);
    constant("White", color::white);
    constant("Gray", color::gray);
    constant("Purple", color::purple);

    reg("Console::enabled?", [](std::vector<ValuePtr>) {
        return Value::boolean(color::enabled);
    });
    reg("Console::colorize", [](std::vector<ValuePtr> args) {
        if (args.size() < 2) return Value::string("");
        if (!color::enabled) return Value::string(args[0]->toString());
        return Value::string(args[1]->toString() + args[0]->toString() + color::reset);
    });
}

} // namespace kex::interpreter
