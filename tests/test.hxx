#pragma once

#include "../src/common/color.hxx"
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

struct TestSuite {
    std::string name;
    std::vector<TestCase> cases;
};

inline std::vector<TestSuite>& suites() {
    static std::vector<TestSuite> s;
    return s;
}

inline int failures = 0;
inline int passes = 0;
inline std::string currentTest;

inline auto describe(const std::string& name, std::function<void()> fn) -> void {
    suites().push_back({name, {}});
    fn();
}

inline auto it(const std::string& name, std::function<void()> fn) -> void {
    suites().back().cases.push_back({name, fn});
}

inline auto runAll() -> int {
    for (auto& suite : suites()) {
        std::cout << "\n  " << suite.name << "\n";
        for (auto& tc : suite.cases) {
            currentTest = suite.name + " > " + tc.name;
            try {
                tc.fn();
                passes++;
                std::cout << "    " << kex::color::apply(kex::color::green) << "✓" << kex::color::apply(kex::color::reset) << " " << tc.name << "\n";
            } catch (const std::exception& e) {
                failures++;
                std::cout << "    " << kex::color::apply(kex::color::red) << "✗" << kex::color::apply(kex::color::reset) << " " << tc.name << "\n";
                std::cout << "      " << e.what() << "\n";
            }
        }
    }

    std::cout << "\n  " << passes << " passing, " << failures << " failing\n\n";
    return failures > 0 ? 1 : 0;
}

class AssertionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

template<typename T>
auto toString(const T& val) -> std::string {
    if constexpr (std::is_same_v<T, std::string>) {
        return "\"" + val + "\"";
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<int>(val));
    } else {
        std::ostringstream oss;
        oss << val;
        return oss.str();
    }
}

inline auto assertEqual(const auto& actual, const auto& expected,
                        const std::string& msg = "") -> void {
    if (actual != expected) {
        std::string message = "Expected: " + toString(expected) + ", got: " + toString(actual);
        if (!msg.empty()) message += " (" + msg + ")";
        throw AssertionError(message);
    }
}

inline auto assertTrue(bool condition, const std::string& msg = "") -> void {
    if (!condition) {
        throw AssertionError(msg.empty() ? "Expected true" : msg);
    }
}

inline auto assertFalse(bool condition, const std::string& msg = "") -> void {
    if (condition) {
        throw AssertionError(msg.empty() ? "Expected false" : msg);
    }
}

} // namespace test
