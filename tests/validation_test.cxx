#include "test.hxx"
#include "../src/lexer/lexer.hxx"
#include "../src/parser/parser.hxx"
#include "../src/semantic/analyzer.hxx"
#include "../src/validation/tag_validator.hxx"
#include <filesystem>
#include <fstream>

using namespace kex;
using namespace test;

namespace {

constexpr auto issueTypes =
    "type ByteSpan = At(Integer) | Between(Integer, Integer)\n"
    "type Issue = Fatal(ByteSpan?, String) | Warn(ByteSpan?, String)\n";

auto validate(
    const std::string& source,
    std::chrono::milliseconds timeout = std::chrono::seconds(1))
    -> std::vector<semantic::Diagnostic> {
    Lexer lexer(std::string{issueTypes} + source, "validation_test.kex");
    Parser parser(lexer.tokenizeAll(), "validation_test.kex");
    auto program = parser.parseProgram();
    semantic::Analyzer analyzer;
    if (!analyzer.analyze(program)) return analyzer.diagnostics();
    return validation::validateTaggedLiterals(program, analyzer, timeout);
}

auto validateFile(const std::string& source)
    -> std::vector<semantic::Diagnostic> {
    const std::string filename = "/tmp/kex_validation_position.kex";
    const auto completeSource = std::string{issueTypes} + source;
    {
        std::ofstream output{filename};
        output << completeSource;
    }
    Lexer lexer(completeSource, filename);
    Parser parser(lexer.tokenizeAll(), filename);
    auto program = parser.parseProgram();
    semantic::Analyzer analyzer;
    analyzer.analyze(program);
    auto diagnostics =
        validation::validateTaggedLiterals(program, analyzer);
    std::filesystem::remove(filename);
    return diagnostics;
}

auto contains(
    const std::vector<semantic::Diagnostic>& diagnostics,
    semantic::Diagnostic::Level level,
    const std::string& text) -> bool {
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.level == level &&
            diagnostic.message.find(text) != std::string::npos)
            return true;
    return false;
}

} // namespace

int main() {
    describe("Compile-time tagged-literal validation", []() {
        it("turns Fatal issues into errors", []() {
            auto diagnostics = validate(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> [Issue] = "
                "[Fatal(None, \"invalid demo literal\")]\n"
                "main do demo`bad` end\n");
            assertTrue(contains(
                diagnostics,
                semantic::Diagnostic::Level::Error,
                "invalid demo literal"));
        });

        it("emits Warn issues without failing validation", []() {
            auto diagnostics = validate(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> [Issue] = "
                "[Warn(At(1), \"suspicious demo literal\")]\n"
                "main do demo`okay` end\n");
            assertTrue(contains(
                diagnostics,
                semantic::Diagnostic::Level::Warning,
                "suspicious demo literal"));
        });

        it("maps cooked byte offsets back to source positions", []() {
            auto diagnostics = validateFile(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> [Issue] = "
                "[Warn(At(1), \"positioned warning\")]\n"
                "main do demo`abc` end\n");
            assertEqual(diagnostics.size(), size_t(1));
            assertEqual(diagnostics[0].location.line, 5);
            assertEqual(diagnostics[0].location.column, 15);
        });

        it("preserves Between as an exclusive source range", []() {
            auto diagnostics = validateFile(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> [Issue] = "
                "[Warn(Between(1, 3), \"ranged warning\")]\n"
                "main do demo`abcde` end\n");
            assertEqual(diagnostics.size(), size_t(1));
            assertEqual(diagnostics[0].location.line, 5);
            assertEqual(diagnostics[0].location.column, 15);
            assertTrue(diagnostics[0].endLocation.has_value());
            assertEqual(diagnostics[0].endLocation->line, 5);
            assertEqual(diagnostics[0].endLocation->column, 17);
        });

        it("rejects reversed Between ranges", []() {
            auto diagnostics = validate(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> [Issue] = "
                "[Warn(Between(3, 1), \"bad range\")]\n"
                "main do demo`abcde` end\n");
            assertTrue(contains(
                diagnostics,
                semantic::Diagnostic::Level::Error,
                "byte span outside the literal"));
        });

        it("rejects Between ranges past the literal", []() {
            auto diagnostics = validate(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> [Issue] = "
                "[Warn(Between(1, 6), \"bad range\")]\n"
                "main do demo`abcde` end\n");
            assertTrue(contains(
                diagnostics,
                semantic::Diagnostic::Level::Error,
                "byte span outside the literal"));
        });

        it("does nothing when the companion is absent", []() {
            auto diagnostics = validate(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "main do demo`anything` end\n");
            assertTrue(diagnostics.empty());
        });

        it("does not invoke companions for interpolating tags", []() {
            auto diagnostics = validate(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> [Issue] = "
                "[Fatal(None, \"must not run\")]\n"
                "main do demo$`value ${42}` end\n");
            assertTrue(diagnostics.empty());
        });

        it("does not validate ordinary function calls", []() {
            auto diagnostics = validate(
                "let demo(source: String) -> String = source\n"
                "let validateDemo(source: String) -> [Issue] = "
                "[Fatal(None, \"must not run\")]\n"
                "main do demo(\"bad\") end\n");
            assertTrue(diagnostics.empty());
        });

        it("finds the companion in the tag's module", []() {
            auto diagnostics = validate(
                "module Demo do\n"
                "  let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "  let validateDemo(source: String) -> [Issue] = "
                "[Fatal(None, \"module validator ran\")]\n"
                "  let build() = demo`bad`\n"
                "end\n");
            assertTrue(contains(
                diagnostics,
                semantic::Diagnostic::Level::Error,
                "module validator ran"));
        });

        it("requires the exact pure companion signature", []() {
            auto diagnostics = validate(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> String = source\n"
                "main do demo`bad` end\n");
            assertTrue(contains(
                diagnostics,
                semantic::Diagnostic::Level::Error,
                "String -> [Issue]"));
        });

        it("bounds non-terminating validators", []() {
            auto diagnostics = validate(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> [Issue] do\n"
                "  loop\n"
                "    next\n"
                "  end\n"
                "end\n"
                "main do demo`bad` end\n",
                std::chrono::milliseconds(5));
            assertTrue(contains(
                diagnostics,
                semantic::Diagnostic::Level::Error,
                "timed out"));
        });

        it("reports validator crashes against the companion", []() {
            auto diagnostics = validate(
                "let demo(parts: [String], values: [Any]) -> String = "
                "parts.first.or(\"\")\n"
                "let validateDemo(source: String) -> [Issue] = "
                "die(\"validator exploded\")\n"
                "main do demo`bad` end\n");
            assertTrue(contains(
                diagnostics,
                semantic::Diagnostic::Level::Error,
                "crashed"));
        });
    });

    return runAll();
}
