#include "../evaluator.hxx"
#include "../../common/color.hxx"
#include <iostream>

namespace kex::interpreter {

auto Evaluator::registerIOBuiltins() -> void {
    // Renders a value through a presentation protocol. Which implementation
    // that is — a concrete one, the owning ADT's, or the trait's generic make
    // block — is ordinary trait dispatch, so this asks the resolver rather
    // than naming any trait itself.
    auto protocolText = [this](const ValuePtr& value,
                               const std::string& method,
                               std::vector<ValuePtr> extra = {}) {
        std::vector<ValuePtr> args{value};
        args.insert(args.end(), extra.begin(), extra.end());
        auto target = resolveMethodName(value, method, nullptr);
        if (m_functionValues.count(target)) {
            auto rendered = callFunction(target, std::move(args), {}, {});
            if (auto* text = std::get_if<StringValue>(&rendered->data))
                return text->value;
        }
        return method == "inspectValue" ? value->inspect() : value->toString();
    };
    auto aliasDual = [this](const std::string& alias, const std::string& target) {
        auto value = m_globalEnv->get(target);
        m_globalEnv->define(alias, value);
        defineIntrinsic(alias, value);
    };

    defineModule("IO");

    // IO.printLine(msg...) — stringify args, write to stdout, trailing newline.
    defineDual("IO::printLine", [this, protocolText](std::vector<ValuePtr> args) -> ValuePtr {
        std::string out;
        for (const auto& arg : args) out += protocolText(arg, "showValue");
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
    defineDual("IO::print", [this, protocolText](std::vector<ValuePtr> args) -> ValuePtr {
        std::string out;
        for (const auto& arg : args) out += protocolText(arg, "showValue");
        m_output += out;
        if (m_mockIO) {
            m_mockIOOutput += out;
        } else {
            std::cout << out;
        }
        return Value::unit();
    });

    // Registered under BOTH `IO.inspect` and the bare `inspect` UFCS name, so
    // the two spellings are one operation — printing the value and returning
    // it unchanged. They used to be different functions, and which one a call
    // reached depended on how it was written (kexhq/kex#143). Mock.IO capture
    // lives here, so it covers both.
    auto inspectValue = [this, protocolText](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        const auto& val = args[0];
        const auto rendered = protocolText(
            val, "inspectValue", {Value::boolean(kex::color::enabled)});
        if (m_mockIO) {
            m_mockIOOutput += rendered + " : " + val->typeName() + "\n";
        } else {
            std::cerr << rendered << " "
                      << kex::color::apply(kex::color::gray) << ":"
                      << kex::color::apply(kex::color::reset) << " "
                      << kex::color::apply(kex::color::cyan) << val->typeName()
                      << kex::color::apply(kex::color::reset) << "\n";
        }
        return val;
    };
    defineDual("IO::inspect", inspectValue);
    definePublic("inspect", inspectValue);

    // The rendering half, for the Kex-level `inspect` in kex.kex: the caller
    // has already produced the text through the Inspectable protocol, so this
    // only writes it. BEAM has had `kex_intrinsic_io:inspectRendered/2` all
    // along; the walker never did, because its `IO.inspect` is native and the
    // Kex body was dead code there (kexhq/kex#143). Mock.IO capture is applied
    // here too, so a mocked run sees `.inspect` output like any other.
    defineIntrinsic("IO::inspectRendered", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::unit();
        const auto& val = args[0];
        const std::string rendered =
            args.size() > 1 && args[1] ? args[1]->toString() : val->inspect();
        if (m_mockIO) {
            m_mockIOOutput += rendered + " : " + val->typeName() + "\n";
        } else {
            std::cerr << rendered << " "
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
    defineDual("IO::printError", [this, protocolText](std::vector<ValuePtr> args) -> ValuePtr {
        std::string out;
        for (const auto& a : args) out += protocolText(a, "showValue");
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

    // System.os() — the operating system family, as one of the atoms in the
    // OS union (system.kex). Both backends answer with the same atom for the
    // same machine, and anything unmodelled is `:unknown` rather than a name
    // the union does not cover.
    defineIntrinsic("System::os", [this](std::vector<ValuePtr>) -> ValuePtr {
#if defined(_WIN32)
        return Value::atom("windows");
#elif defined(__EMSCRIPTEN__)
        return Value::atom("wasm");
#elif defined(__APPLE__)
        return Value::atom("macos");
#elif defined(__linux__)
        return Value::atom("linux");
#elif defined(__FreeBSD__)
        return Value::atom("freebsd");
#elif defined(__OpenBSD__)
        return Value::atom("openbsd");
#elif defined(__NetBSD__)
        return Value::atom("netbsd");
#else
        return Value::atom("unknown");
#endif
    });

    // System.bitWidth() — the machine's pointer width in bits. The BEAM
    // answers from its word size; here it IS the pointer size.
    defineIntrinsic("System::bitWidth", [this](std::vector<ValuePtr>) -> ValuePtr {
        return Value::integer(static_cast<int64_t>(sizeof(void*) * 8));
    });

    // Mock.System — makes the machine claim to be something else, so a test
    // for Windows behaviour can run on this one. Same shape as Mock.FS: a
    // setter per fact, and one `clear` that hands everything back. Test-only
    // like every Mock.* intrinsic (issue #144).

    // die(msg) — print msg to stderr and terminate the process.
    {
        auto fn = [](std::vector<ValuePtr> args) -> ValuePtr {
            std::string msg = args.empty() ? "program terminated" : args[0]->toString();
            std::cerr << "fatal: " << msg << "\n";
            std::exit(1);
        };
        definePublic("die", fn);
        defineIntrinsic("System::die", std::move(fn));
    }

    // IO.getLine() — reads one line from stdin (or mock input).
    //
    // `String?`, as io.kex declares: `Just(line)` or `None` at EOF. The line
    // used to come back UNWRAPPED, so the type was really `String | None` and
    // `match ... Just(x)` matched nothing on either backend — only `.or(...)`
    // worked, because `Optional.or`'s catch-all returns whatever it is given
    // (kexhq/kex#197).
    defineDual("IO::getLine", [this](std::vector<ValuePtr>) -> ValuePtr {
        if (m_mockIO) {
            if (m_mockIOInputLines.empty()) return Value::none();
            auto line = m_mockIOInputLines.front();
            m_mockIOInputLines.pop_front();
            return Value::just(Value::string(line));
        }
        std::string line;
        if (!std::getline(std::cin, line)) {
            return Value::none();
        }
        return Value::just(Value::string(line));
    });

    // IO.get() — reads a single character from stdin (or mock input).
    // `String?` on the same terms as `getLine` above (kexhq/kex#197).
    defineDual("IO::get", [this](std::vector<ValuePtr>) -> ValuePtr {
        if (m_mockIO) {
            if (m_mockIOInputLines.empty()) return Value::none();
            auto& front = m_mockIOInputLines.front();
            if (front.empty()) {
                m_mockIOInputLines.pop_front();
                return Value::just(Value::string("\n"));
            }
            char c = front[0];
            front.erase(0, 1);
            return Value::just(Value::string(std::string(1, c)));
        }
        int c = std::cin.get();
        if (c == EOF) return Value::none();
        return Value::just(Value::string(std::string(1, static_cast<char>(c))));
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
    //
    // Capturing the console is as much a lie as faking a file, so the whole
    // family is test-only (issue #144).

    auto mockStart = [this](std::vector<ValuePtr>) -> ValuePtr {
        requireMocksAllowed("Mock.IO.start");
        m_mockIO = true;
        m_mockIOOutput.clear();
        m_mockIOInputLines.clear();
        return Value::unit();
    };

    auto mockInput = [this](std::vector<ValuePtr> args) -> ValuePtr {
        requireMocksAllowed("Mock.IO.input");
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
        requireMocksAllowed("Mock.IO.output");
        return Value::string(m_mockIOOutput);
    };

    auto mockClear = [this](std::vector<ValuePtr>) -> ValuePtr {
        requireMocksAllowed("Mock.IO.clear");
        m_mockIOOutput.clear();
        m_mockIOInputLines.clear();
        return Value::unit();
    };

    auto mockStop = [this](std::vector<ValuePtr>) -> ValuePtr {
        requireMocksAllowed("Mock.IO.stop");
        m_mockIO = false;
        m_mockIOOutput.clear();
        m_mockIOInputLines.clear();
        return Value::unit();
    };

    // The fixed-arity Mock.IO controls are source-owned. `input` alone stays
    // public-native because its existing API is variadic while Kex function
    // declarations currently have fixed arity.
    defineDual("Mock::IO::input", mockInput);
    defineIntrinsic("IO::ioMockStart", std::move(mockStart));
    defineIntrinsic("IO::ioMockInput", std::move(mockInput));
    defineIntrinsic("IO::ioMockOutput", std::move(mockOutput));
    defineIntrinsic("IO::ioMockClear", std::move(mockClear));
    defineIntrinsic("IO::ioMockStop", std::move(mockStop));
}

} // namespace kex::interpreter
