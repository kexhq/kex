#include "../evaluator.hxx"
#include "../../common/color.hxx"
#include <chrono>
#include <cstdio>
#include <exception>
#include <iostream>
#include <sstream>

namespace kex::interpreter {

namespace {

// The separator that joins a case's enclosing `describe` labels with its own
// into the one string `--test-only` matches against. The JSON records carry
// the path as an ARRAY as well, so a tool never has to split this back apart —
// it is only the command line that needs a single token.
constexpr const char* kTestPathSeparator = " > ";

auto joinTestPath(const std::vector<std::string>& path) -> std::string {
    std::string out;
    for (size_t i = 0; i < path.size(); i++) {
        if (i) out += kTestPathSeparator;
        out += path[i];
    }
    return out;
}

auto jsonString(const std::string& s) -> std::string {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            // Control characters must be escaped; everything else — UTF-8
            // continuation bytes included — passes through untouched, since the
            // records are UTF-8 and JSON says that is fine.
            if (c < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

auto jsonPath(const std::vector<std::string>& path) -> std::string {
    std::string out = "[";
    for (size_t i = 0; i < path.size(); i++) {
        if (i) out += ",";
        out += jsonString(path[i]);
    }
    return out + "]";
}

// `,"file":…,"line":…,"column":…` — omitted entirely when the location is
// unknown, so a consumer can tell "no location" from "line 0".
auto jsonLocation(const SourceLocation& loc) -> std::string {
    if (loc.line <= 0) return "";
    return ",\"file\":" + jsonString(std::string(loc.file)) +
           ",\"line\":" + std::to_string(loc.line) +
           ",\"column\":" + std::to_string(loc.column);
}

auto jsonNumber(double value) -> std::string {
    std::ostringstream out;
    out.precision(3);
    out << std::fixed << value;
    return out.str();
}

} // namespace

// One record per line on stdout. A line, not a document: a spec prints its own
// output too, and a consumer that reads records as they arrive can show a case
// the moment it finishes rather than after the suite ends.
auto Evaluator::emitTestRecord(const std::string& json) -> void {
    auto line = json + "\n";
    m_output += line;
    std::cout << line << std::flush;
}

auto Evaluator::reportTestCase(const std::vector<std::string>& path, const char* status,
                               double durationMs, const std::string& message,
                               SourceLocation caseLoc, SourceLocation failLoc)
    -> void {
    std::string json = "{\"kexTest\":\"case\",\"path\":" + jsonPath(path) +
                       ",\"name\":" + jsonString(path.empty() ? "" : path.back()) +
                       ",\"status\":" + jsonString(status) +
                       ",\"durationMs\":" + jsonNumber(durationMs) +
                       jsonLocation(caseLoc);
    // The failure's own location is nested, because it is a DIFFERENT place
    // from the case's: the assertion that blew up, not the `it` that holds it.
    if (!message.empty()) {
        json += ",\"failure\":{\"message\":" + jsonString(message) +
                jsonLocation(failLoc) + "}";
    }
    emitTestRecord(json + "}");
}

auto Evaluator::reportTestDiscovery(const std::vector<std::string>& path,
                                    const char* kind, SourceLocation loc)
    -> void {
    emitTestRecord("{\"kexTest\":\"item\",\"kind\":" + jsonString(kind) +
                   ",\"path\":" + jsonPath(path) +
                   ",\"name\":" + jsonString(path.empty() ? "" : path.back()) +
                   jsonLocation(loc) + "}");
}

auto Evaluator::reportTestSummary() -> void {
    emitTestRecord("{\"kexTest\":\"summary\",\"passed\":" +
                   std::to_string(m_testsPassed) + ",\"failed\":" +
                   std::to_string(m_testsFailed) + "}");
}

// A filter names a case by its full path, an ancestor `describe` by its own
// path, or any case by its bare label. The first two are what an editor's
// per-case ▶ sends; the last is for typing one by hand.
//
// `isGroup` distinguishes a `describe` from an `it`, and the two answer
// differently: a describe also runs when a filter names something INSIDE it —
// including a bare label, which could belong to any case at any depth, so a
// describe cannot rule it out without running.
auto Evaluator::testCaseSelected(const std::vector<std::string>& path,
                                 bool isGroup) const -> bool {
    if (m_testFilters.empty()) return true;
    auto joined = joinTestPath(path);
    auto prefix = joined + kTestPathSeparator;
    for (const auto& filter : m_testFilters) {
        if (filter == joined) return true;
        // An ancestor describe was named, so everything under it runs.
        if (joined.rfind(filter + kTestPathSeparator, 0) == 0) return true;
        if (isGroup) {
            if (filter.rfind(prefix, 0) == 0) return true;
            if (filter.find(kTestPathSeparator) == std::string::npos) return true;
        } else if (!path.empty() && filter == path.back()) {
            return true;
        }
    }
    return false;
}

// Minimal RSpec-style framework: `describe`/`it`/`before`/`after`/`assert`.
// `Mock.*`/`using Test`/the `kex test` CLI subcommand are separate concerns —
// `using` blocks are currently a no-op (see execTopLevel), so these are
// always-in-scope globals rather than something you import.
auto Evaluator::registerTestBuiltins() -> void {
    auto reg = [this](const std::string& name, NativeFunc fn) {
        definePublic(name, fn);
        defineIntrinsic("Test::" + name, std::move(fn));
    };

    // describe(name) do ... end — purely organizational: prints a header
    // and runs its block (nested describe/it calls included). Indentation
    // tracks nesting depth.
    reg("describe", [this](std::vector<ValuePtr> args) -> ValuePtr {
        if (args.empty()) return Value::none();
        auto groupLoc = m_lastCallLocation;
        auto label = args[0]->toString();
        m_testPath.push_back(label);
        // A `describe` runs whenever anything under it might: it is what
        // REGISTERS the cases, so skipping it would skip them too.
        if (!testCaseSelected(m_testPath, true)) {
            m_testPath.pop_back();
            return Value::none();
        }
        if (m_testReportMode == TestReportMode::Pretty) {
            auto line = std::string(m_testDepth * 2, ' ') + label + "\n";
            m_output += line;
            std::cout << line;
        } else if (m_testReportMode == TestReportMode::List) {
            reportTestDiscovery(m_testPath, "describe", groupLoc);
        }

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
            m_testPath.pop_back();
            if (failure) std::rethrow_exception(failure);
            return Value::none();
        }
        m_testPath.pop_back();
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
        auto caseLoc = m_lastCallLocation;
        auto label = args[0]->toString();
        std::string indent(m_testDepth * 2, ' ');
        auto* fn = args.size() > 1 ? std::get_if<FunctionValue>(&args[1]->data) : nullptr;

        auto path = m_testPath;
        path.push_back(label);
        // Discovery runs no bodies: the tree an editor draws must be cheap and
        // must not have side effects (kexhq/kex#199).
        if (m_testReportMode == TestReportMode::List) {
            reportTestDiscovery(path, "it", caseLoc);
            return Value::none();
        }
        if (!testCaseSelected(path, false)) return Value::none();

        std::string line;
        SourceLocation failLoc{};
        std::string failMessage;
        // An `it` without a block is neither passed nor failed — it is a case
        // that was never written. It counts towards neither tally, so it gets
        // its own status rather than being flattened into one of the two.
        const char* status = "skipped";
        auto started = std::chrono::steady_clock::now();
        if (!fn || !fn->native) {
            line = indent + "? " + label + " (no block)\n";
        } else {
            // Mock state set during this test — by a `before` hook or by the
            // body — is discarded when the test ends, so a forgotten
            // `Mock.FS.clear()` cannot leak into the next one. Captured BEFORE
            // the `before` hooks so per-test setup is undone too, while
            // anything a `before(:all)` established outside this block
            // survives (kexhq/kex#143).
            auto savedMocks = captureMocks();
            std::exception_ptr failure;
            // Where the last `assert` was written — see Evaluator::eval. Saved
            // and restored so a nested run (a spec that evaluates Kex source of
            // its own) cannot leave the outer case pointing into another file.
            auto savedInTestCase = m_inTestCase;
            auto savedTestLocation = m_lastTestFileLocation;
            m_inTestCase = true;
            m_lastTestFileLocation = caseLoc;
            auto run = [&](const ValuePtr& hook) {
                auto* hookFn = std::get_if<FunctionValue>(&hook->data);
                if (!hookFn || !hookFn->native)
                    throw std::runtime_error("test hook requires a block");
                hookFn->native({});
            };
            try {
                for (const auto& scope : m_testHookScopes)
                    for (const auto& hook : scope.before) run(hook);
                auto outcome = fn->native({});
                // An unrescued `.try` failure inside a block does not unwind
                // — a lambda IS a function, so it RETURNS `Error(e)` (see the
                // LambdaExpr TryException handler, which HOFs depend on). A
                // test body is a block, so `assert(f().try == x)` on a failing
                // `f()` left the assert unevaluated and the case reported
                // green. Every spec assertion whose expression propagated was
                // silently passing on both backends.
                if (outcome) {
                    if (const auto* variant =
                            std::get_if<VariantValue>(&outcome->data);
                        variant && variant->tag == "Error" &&
                        variant->parentType == "Result")
                        throw std::runtime_error(
                            ".try propagated out of the test body: " +
                            (variant->args.empty()
                                 ? std::string("Error")
                                 : variant->args[0]->toString()));
                }
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
            restoreMocks(std::move(savedMocks));
            // An assert in another file — the stdlib's, inside an `Assert.*`
            // helper — is no use to a reader of this spec, so the case's own
            // line stands in for it.
            failLoc = m_lastTestFileLocation.file == caseLoc.file
                          ? m_lastTestFileLocation
                          : caseLoc;
            m_inTestCase = savedInTestCase;
            m_lastTestFileLocation = savedTestLocation;
            if (!failure) {
                m_testsPassed++;
                status = "passed";
                line = indent + color::apply(color::green) + "\xE2\x9C\x93" +
                       color::apply(color::reset) + " " + label + "\n"; // ✓
            } else {
                m_testsFailed++;
                status = "failed";
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception& e) {
                    failMessage = e.what();
                } catch (...) {
                    failMessage = "unknown error";
                }
                line = indent + color::apply(color::red) + "\xE2\x9C\x97" +
                       color::apply(color::reset) + " " + label + ": " +
                       failMessage + "\n"; // ✗
            }
        }
        if (m_testReportMode == TestReportMode::Json) {
            auto elapsed = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - started)
                               .count();
            reportTestCase(path, status, elapsed, failMessage,
                           caseLoc, failLoc);
            return Value::none();
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
