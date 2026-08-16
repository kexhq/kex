#include <cstdlib>
#include "test.hxx"
#include "../src/lsp/server.hxx"

#include <sstream>
#include <string>
#include <filesystem>
#include <fstream>

using namespace test;

namespace {
// The compiled standard library, as the CLI locates it. Passed explicitly
// rather than defaulted: reading interfaces from .kex SOURCE instead loses
// inferred result types, which is what once left every method call on a call
// result typed `unknown` in the editor.
auto testRuntimeBeamDir() -> std::string {
    if (const char* configured = std::getenv("KEX_RUNTIME_DIR");
        configured && *configured)
        return configured;
    return {};
}



auto frame(const std::string& json) -> std::string {
    return "Content-Length: " + std::to_string(json.size()) +
           "\r\n\r\n" + json;
}

auto responseForId(const std::string& output, int id) -> std::string {
    const auto needle = "\"id\":" + std::to_string(id);
    size_t cursor = 0;
    while ((cursor = output.find("\r\n\r\n", cursor)) != std::string::npos) {
        const auto body = cursor + 4;
        const auto next = output.find("Content-Length:", body);
        auto message = output.substr(body, next == std::string::npos
            ? std::string::npos : next - body);
        if (message.find(needle) != std::string::npos) return message;
        cursor = body;
    }
    return {};
}

auto occurrences(const std::string& text, std::string_view needle) -> size_t {
    size_t count = 0;
    size_t cursor = 0;
    while ((cursor = text.find(needle, cursor)) != std::string::npos) {
        ++count;
        cursor += needle.size();
    }
    return count;
}

} // namespace

int main() {
    describe("Kex LSP", []() {
        it("serves diagnostics and completion for unsaved buffers", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-unsaved.kex","languageId":"kex","version":1,"text":"main do\n  \"😀\" + missingName\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-unsaved.kex","version":2},"contentChanges":[{"text":"main do\n  \"😀\" List.ma\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-unsaved.kex"},"position":{"line":1,"character":14}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-unsaved.kex","version":3},"contentChanges":[{"text":"main do\n  [1, 2].co\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-unsaved.kex"},"position":{"line":1,"character":11}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-unsaved.kex"},"position":{"line":1,"character":3}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record.kex","languageId":"kex","version":1,"text":"# A Cartesian point.\nrecord Point do\n  x : Int\n  label : String\nend\nlet format(value: Integer) -> String = \"\"\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record.kex"},"position":{"line":1,"character":8}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":7,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record.kex"},"position":{"line":3,"character":11}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":8,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record.kex"},"position":{"line":5,"character":20}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-curry.kex","languageId":"kex","version":1,"text":"let add(a, b) = a + b\nmain do\n  let inc = ~add(1)\n  let present = Just(1)\n  let absent = None\n  let typed: Optional<Integer> = present\n  inc(2)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":9,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-curry.kex"},"position":{"line":0,"character":5}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":10,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-curry.kex"},"position":{"line":2,"character":14}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":11,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-curry.kex"},"position":{"line":3,"character":17}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":12,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-curry.kex"},"position":{"line":4,"character":16}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":16,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-curry.kex"},"position":{"line":5,"character":15}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-fs.kex","languageId":"kex","version":1,"text":"using FS\nmain do\n  FS.\n  FS.File.\n  FS.File.read(\"x\")\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":13,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-fs.kex"},"position":{"line":2,"character":5}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":14,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-fs.kex"},"position":{"line":3,"character":10}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":15,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-fs.kex"},"position":{"line":4,"character":11}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("publishDiagnostics") != std::string::npos,
                       "missing diagnostics notification");
            assertTrue(result.find("Undefined name") != std::string::npos,
                       "missing compiler diagnostic");
            assertTrue(result.find(R"("character":9)") != std::string::npos,
                       "compiler byte column was not converted to LSP UTF-16");
            assertTrue(result.find(R"("label":"map")") != std::string::npos &&
                       result.find("map :") != std::string::npos,
                       "missing compiler-backed completion");
            assertTrue(result.find(R"("label":"count")") != std::string::npos,
                       "missing type-aware list-literal completion");
            assertTrue(result.find("completionProvider") != std::string::npos,
                       "initialize response did not advertise completion");
            assertTrue(result.find(R"("hoverProvider":true)") != std::string::npos,
                       "initialize response did not advertise hover");
            assertTrue(result.find("1 : Integer") != std::string::npos,
                       "hover did not expose an inferred expression type");
            assertTrue(result.find(R"(record Point do\n  x : Int\n  label : String\nend)") !=
                           std::string::npos,
                       "record hover did not expose its fields");
            assertTrue(result.find("A Cartesian point.") != std::string::npos,
                       "record hover did not expose its documentation comment");
            assertTrue(result.find("type String") != std::string::npos,
                       "primitive annotation hover was mistaken for a module");
            assertTrue(result.find("type Integer") != std::string::npos,
                       "parameter type hover was mistaken for a module");
            assertTrue(result.find("add : A -> A -> A") != std::string::npos,
                       "function hover leaked internal type variables");
            assertTrue(result.find("~add : Integer -> Integer") != std::string::npos,
                       "curried function hover lost its specialized signature");
            assertTrue(result.find("Just : Integer?") != std::string::npos,
                       "Just constructor hover did not show Optional payload type");
            assertTrue(result.find("None : A?") != std::string::npos,
                       "None constructor hover did not show generic Optional type");
            assertTrue(result.find(R"("label":"File")") != std::string::npos,
                       "FS completion did not include its nested File module");
            assertTrue(result.find(R"("label":"foul read")") != std::string::npos &&
                       result.find("read : String -> String?") != std::string::npos,
                       "FS.File completion did not include read");
            assertTrue(result.find("foul read : String -> String?") !=
                           std::string::npos,
                       "completion did not expose imported module foulness");
            assertTrue(result.find("(path, content)") != std::string::npos,
                       "completion did not expose function parameter names");
            assertTrue(result.find(R"("label":"foul write")") != std::string::npos &&
                       result.find("(path, content) : String -> String -> Bool") !=
                           std::string::npos,
                       "foul completion did not use declaration order");
            assertTrue(result.find("read : String -> String?") != std::string::npos,
                       "imported built-in function hover omitted its signature");
            assertTrue(result.find("Reads the KexI interface chunk") ==
                           std::string::npos,
                       "FS.File.read used Kex.Interface.read documentation");
            assertTrue(result.find(
                           R"(type Optional<X> =\n  Just(X)\n| None)") !=
                           std::string::npos,
                       "imported ADT hover omitted constructors or payload types");
        });
        it("shows a primitive passed to to(...) as a type value", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-type-value.kex","languageId":"kex","version":1,"text":"main do\n  42.to(String)\n  Type.of(42)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-type-value.kex"},"position":{"line":1,"character":9}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-type-value.kex"},"position":{"line":2,"character":3}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-type-value.kex"},"position":{"line":2,"character":8}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("type String") != std::string::npos,
                       "to(String) hover did not identify String as a type value");
            assertTrue(result.find("module String") == std::string::npos,
                       "to(String) hover incorrectly identified String as a module");
            assertTrue(result.find("record Type do") != std::string::npos,
                       "Type.of receiver hover did not identify the Type record");
            assertTrue(result.find("of : Any -> Type") != std::string::npos,
                       "Type.of hover leaked the checker's internal unknown type");
        });
        it("keeps Type.of result types on local bindings", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hello.kex","languageId":"kex","version":1,"text":"type Hello = You(String) | World\nmain do\n  let at = Type.of(Hello)\n  IO.inspect(at)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hello.kex"},"position":{"line":2,"character":7}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hello.kex"},"position":{"line":3,"character":14}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hello.kex"},"position":{"line":0,"character":6}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hello.kex"},"position":{"line":0,"character":13}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("at : Type") != std::string::npos,
                       "Type.of result was not retained on its local binding");
            assertTrue(result.find("function at") == std::string::npos,
                       "local binding hover fell through to an unrelated global function");
            assertTrue(result.find(
                           R"(type Hello =\n  You(String)\n| World)") !=
                           std::string::npos,
                       "local ADT hover omitted its constructors");
            assertTrue(result.find("You : String -> Hello") != std::string::npos,
                       "ADT constructor hover omitted its full type");
        });
        it("does not attach same-named stdlib docs to a local binding", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-wc.kex","languageId":"kex","version":1,"text":"main do\n  let lines = [\"one\", \"two\"]\n  let words = lines.map { |line| line }\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-wc.kex"},"position":{"line":2,"character":16}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto hover = responseForId(output.str(), 2);
            assertTrue(hover.find("lines : [String]") != std::string::npos,
                       "local lines hover omitted its inferred list type");
            assertTrue(hover.find("Splits into lines") == std::string::npos &&
                       hover.find("String.lines") == std::string::npos,
                       "local lines hover included String.lines documentation");
        });
        it("uses imported signatures for standard-library method hovers", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/stream.spec.kex","languageId":"kex","version":1,"text":"# kex: no-check\nmain do\n  let s = Stream.Sequence(from: 0) { |n| n + 1 }\n  s.drop(3).map { |n| n }.filter { |n| n.even? }\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/stream.spec.kex"},"position":{"line":3,"character":5}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/stream.spec.kex"},"position":{"line":3,"character":12}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/stream.spec.kex"},"position":{"line":3,"character":28}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("drop :") != std::string::npos,
                       "Stream.drop hover omitted its imported signature");
            assertTrue(result.find("map :") != std::string::npos,
                       "Stream.map hover omitted its imported signature");
            assertTrue(result.find("filter :") != std::string::npos,
                       "Stream.filter hover omitted its imported signature");
            assertTrue(result.find("function drop") == std::string::npos,
                       "Stream.drop fell back to a generic function hover");
        });
        it("renders standard-library and user RDoc comments", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-docs.kex","languageId":"kex","version":1,"text":"# A documented module.\nmodule Example do\nend\n\n# Returns +value+ unchanged.\n#\n# @param value [Integer]\n# @return [Integer]\n#\n# @example\n#   identity(3) # => 3\nlet identity(value: Integer) -> Integer = value\n\nmain do\n  # A documented local value.\n  let local = [1, 2, 3].sum\n  IO.inspect(local)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-docs.kex"},"position":{"line":1,"character":8}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-docs.kex"},"position":{"line":11,"character":5}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-docs.kex"},"position":{"line":15,"character":27}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-docs.kex"},"position":{"line":16,"character":14}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":6,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("A documented module.") != std::string::npos,
                       "module hover omitted user documentation");
            assertTrue(result.find("Returns `value` unchanged.") != std::string::npos,
                       "RDoc inline code was not converted to Markdown");
            assertTrue(result.find("**Parameter `value`:** `Integer`") != std::string::npos,
                       "RDoc parameter was not formatted");
            assertTrue(result.find("**Returns:** `Integer`") != std::string::npos,
                       "RDoc return type was not formatted");
            assertTrue(result.find("**Example:**") != std::string::npos &&
                       result.find("identity(3) # => 3") != std::string::npos,
                       "RDoc example was not rendered");
            assertTrue(result.find("Sums all elements. Returns `0`") != std::string::npos &&
                       result.find("[1, 2, 3].sum") != std::string::npos,
                       "List.sum hover omitted standard-library documentation");
            assertTrue(result.find("A documented local value.") != std::string::npos,
                       "local binding documentation was not available at a reference");
        });
        it("identifies traits as traits and type constraints", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-traits.kex","languageId":"kex","version":1,"text":"# A local capability.\ntrait LocalCapability do\nend\nlet accept(value: Enumerable) = value\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-traits.kex"},"position":{"line":1,"character":8}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-traits.kex"},"position":{"line":3,"character":22}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find(R"(type LocalCapability\ntrait LocalCapability)") !=
                           std::string::npos,
                       "local trait hover did not expose its dual trait/type role");
            assertTrue(result.find("A local capability.") != std::string::npos,
                       "local trait hover omitted documentation");
            assertTrue(result.find(R"(type Enumerable\ntrait Enumerable)") !=
                           std::string::npos,
                       "imported trait hover was shown only as a type");
        });
        it("does not confuse Result.Error with JSON.Error", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-error-constructor.kex","languageId":"kex","version":1,"text":"using JSON\nmain do\n  let failed = Error(\"bad\")\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-error-constructor.kex"},"position":{"line":2,"character":17}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("Error : Result<") != std::string::npos,
                       "Error constructor hover omitted its inferred Result type");
            assertTrue(result.find("type Error =") == std::string::npos,
                       "Error constructor hover was confused with JSON.Error");
        });
        it("puts the selected overload first", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-selected-overload.kex","languageId":"kex","version":1,"text":"main do\n  [1, 2].at(0)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-selected-overload.kex"},"position":{"line":1,"character":10}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            const auto selected = result.find(
                "at : [Integer] -> Integer -> Integer?");
            const auto generic = result.find("at : [A] -> Integer -> A?");
            assertTrue(selected != std::string::npos,
                       "hover omitted the selected concrete overload");
            assertTrue(generic == std::string::npos || selected < generic,
                       "hover did not put the selected overload first");
            assertTrue(result.find("Selected overload") != std::string::npos,
                       "selected overload was not visually separated");
        });
        it("uses the selected receiver overload documentation", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/file_modes.kex","languageId":"kex","version":1,"text":"foul readAll(file: FileHandle<CanRead, W>) -> String do\n  file.read().or(\"\")\nend\nmain do\n  let maybe: String? = None\n  IO.printLine(\"value: ${maybe.or(\"\")}\")\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/file_modes.kex"},"position":{"line":1,"character":15}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/file_modes.kex"},"position":{"line":5,"character":31}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("Unwraps the value") != std::string::npos,
                       "Optional.or hover omitted the selected fallback documentation");
            assertEqual(occurrences(result, "Selected overload"), size_t{2},
                        "interpolated Optional.or lost its selected overload");
            assertTrue(result.find("Bitwise OR") == std::string::npos &&
                       result.find("Bits.or") == std::string::npos,
                       "Optional.or hover used Bits.or documentation");
        });
        it("infers literal function clause parameters", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-functions.kex","languageId":"kex","version":1,"text":"let factorial(0) = 1\nlet factorial(n: Int) = n * factorial(n - 1)\nlet fib(0) = 0\nlet fib(1) = 1\nlet fib(n: Int) = fib(n - 1) + fib(n - 2)\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-functions.kex"},"position":{"line":0,"character":6}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-functions.kex"},"position":{"line":2,"character":6}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("factorial : Integer -> Integer") !=
                           std::string::npos &&
                       result.find("factorial : A -> Integer") ==
                           std::string::npos,
                       "factorial literal clause remained unconstrained");
            assertTrue(result.find("fib : Integer -> Integer") !=
                           std::string::npos &&
                       result.find("fib : A -> Integer") == std::string::npos,
                       "fib literal clauses remained unconstrained");
        });
        it("shows record fields on record construction hovers", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record-construction.kex","languageId":"kex","version":1,"text":"record Point do\n  x : Float\n  y : Float\nend\nmain do\n  Point { x: 3.0, y: 4.0 }\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record-construction.kex"},"position":{"line":5,"character":4}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find(
                           R"(record Point do\n  x : Float\n  y : Float\nend)") !=
                           std::string::npos,
                       "record construction hover omitted record fields");
            assertTrue(result.find("Point : Point") == std::string::npos,
                       "record construction hover used a redundant nominal type");
        });
        it("selects the qualified foul filesystem overload and navigates", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-filesystem.kex","languageId":"kex","version":1,"text":"using FS\nmain do\n  FS.File.copy(\"src\", \"utils.kex\")\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-filesystem.kex"},"position":{"line":2,"character":11}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-filesystem.kex"},"position":{"line":2,"character":11}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            const auto selected = result.find(
                "Selected overload**\\n\\n```kex\\nfoul copy : String -> String -> Bool");
            assertTrue(selected != std::string::npos,
                       "FS.File.copy did not select its foul filesystem overload");
            assertTrue(result.find("definitionProvider") != std::string::npos,
                       "server did not advertise definition navigation");
            assertTrue(result.find("fs.kex") != std::string::npos,
                       "FS.File.copy navigation did not resolve to its source");
        });
        it("reads a package.kex manifest instead of flagging its vocabulary", []() {
            // `bundle`, `version` and `tey` are declared by the stdlib but
            // imported by nobody, so a manifest used to open with one
            // "undefined function" per line. The declarations now arrive with
            // the file, which is what makes hover work here too.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-pkg/package.kex","languageId":"kex","version":1,"text":"bundle \"demo\" do\n  version(\"0.1.0\")\n  kex(\">= 0.3.0\")\n  entrypoint(\"src/main.kex\")\n  tey(\"greet\", git: \"https://example.com/g.git\", tag: \"v0.1.0\")\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-pkg/package.kex"},"position":{"line":1,"character":4}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("Undefined function") == std::string::npos,
                       "the manifest vocabulary was reported as undefined");
            assertTrue(result.find("version : String -> Void") != std::string::npos,
                       "hovering a manifest declaration showed no signature");
        });

        it("prefers an at-field over an unrelated receiver method", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-at-field.kex","languageId":"kex","version":1,"text":"record User do\n  name : String\nend\nmake User do\n  let greet -> String = \"Hi, ${@name}!\"\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-at-field.kex"},"position":{"line":4,"character":33}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find("@name : String") != std::string::npos,
                       "@name did not retain the User.name field type: " + result);
            assertTrue(result.find("Weekday -> Any") == std::string::npos,
                       "@name resolved to the unrelated Weekday method");
        });
        it("completes members after an inferred local receiver dot", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-local-dot.kex","languageId":"kex","version":1,"text":"main do\n  let lines = [\"one\", \"two\"]\n  lines.\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-local-dot.kex"},"position":{"line":2,"character":8}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find(R"("label":"map")") != std::string::npos &&
                       result.find(R"("label":"count")") != std::string::npos,
                       "inferred [String] local produced no dot completions: " +
                           result);
        });
        it("keeps local navigation and pattern receiver completion semantic", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-pattern-dot.kex","languageId":"kex","version":1,"text":"main do\n  let lines = [\"one\", \"two\"]\n  lines.count\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-pattern-dot.kex"},"position":{"line":2,"character":4}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-pattern-dot.kex","version":2},"contentChanges":[{"text":"main do\n  let lines = [\"one\", \"two\"]\n  lines.\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-pattern-dot.kex"},"position":{"line":2,"character":8}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-parameter-dot.kex","languageId":"kex","version":1,"text":"let size(lines: [String]) do\n  lines.\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-parameter-dot.kex"},"position":{"line":1,"character":8}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":6,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-parameter-dot.kex"},"position":{"line":1,"character":4}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find(
                           R"("start":{"character":6,"line":1})") !=
                           std::string::npos &&
                       result.find(
                           R"("uri":"file:///tmp/kex-lsp-pattern-dot.kex")") !=
                           std::string::npos,
                       "local navigation fell through to an imported symbol");
            assertTrue(result.find(R"("label":"count")") != std::string::npos,
                       "member completion lost the last valid local type");
            assertTrue(result.find(R"("id":5)") != std::string::npos &&
                       result.find(R"("label":"map")") != std::string::npos,
                       "a parameter dot in the initially opened buffer had no members");
            assertTrue(result.find(
                           R"("start":{"character":9,"line":0})") !=
                           std::string::npos &&
                       result.find(
                           R"("uri":"file:///tmp/kex-lsp-parameter-dot.kex")") !=
                           std::string::npos,
                       "parameter navigation did not resolve to its declaration");
        });
        it("finds semantic and shadow-safe local references", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-references.kex","languageId":"kex","version":1,"text":"let double(value: Integer) = value + value\nmain do\n  let value = 2\n  IO.inspect(value)\n  IO.inspect(double(value))\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-references.kex"},"position":{"line":3,"character":14},"context":{"includeDeclaration":true}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-references.kex"},"position":{"line":4,"character":14},"context":{"includeDeclaration":true}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-references.kex"},"position":{"line":4,"character":14},"context":{"includeDeclaration":false}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(result.find(R"("referencesProvider":true)") !=
                           std::string::npos,
                       "server did not advertise find-references support");
            const auto local = responseForId(result, 2);
            assertEqual(occurrences(local, R"("uri":)"), size_t{3},
                        "local references crossed into the shadowed parameter");
            assertTrue(local.find(
                           R"("start":{"character":6,"line":2})") !=
                           std::string::npos,
                       "local references omitted the declaration");
            const auto globalWithDeclaration = responseForId(result, 3);
            assertEqual(occurrences(globalWithDeclaration, R"("uri":)"),
                        size_t{2},
                        "function references omitted its call or declaration");
            const auto globalWithoutDeclaration = responseForId(result, 4);
            assertEqual(occurrences(globalWithoutDeclaration, R"("uri":)"),
                        size_t{1},
                        "includeDeclaration=false still returned the definition");
        });
        it("keeps receiver overload references separate", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-receiver-references.kex","languageId":"kex","version":1,"text":"main do\n  let first: String? = None\n  let second: String? = None\n  first.or(\"\")\n  second.or(\"\")\n  Bits.or(1, 2)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-receiver-references.kex"},"position":{"line":3,"character":9},"context":{"includeDeclaration":false}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto references = responseForId(output.str(), 2);
            assertEqual(occurrences(references, R"("uri":)"), size_t{2},
                        "Optional.or references included Bits.or");
            assertTrue(references.find(R"("line":5)") == std::string::npos,
                       "receiver references included a different overload owner");
        });
        it("finds references in unopened workspace files", []() {
            namespace fs = std::filesystem;
            const fs::path root = "/tmp/kex-lsp-reference-workspace";
            fs::create_directories(root);
            {
                std::ofstream tools(root / "tools.kex");
                tools << "module Tools do\n"
                         "  let double(value: Integer) = value * 2\n"
                         "end\n";
                std::ofstream consumer(root / "z_consumer.kex");
                consumer << "using Tools\n"
                            "main do\n"
                            "  IO.inspect(double(21))\n"
                            "end\n";
            }
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///tmp/kex-lsp-reference-workspace","capabilities":{}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-reference-workspace/tools.kex","languageId":"kex","version":1,"text":"module Tools do\n  let double(value: Integer) = value * 2\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-reference-workspace/tools.kex"},"position":{"line":1,"character":7},"context":{"includeDeclaration":true}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto references = responseForId(output.str(), 2);
            assertEqual(occurrences(references, R"("uri":)"), size_t{2},
                        "unopened workspace reference was not indexed cleanly");
            assertTrue(references.find("z_consumer.kex") != std::string::npos,
                       "find references omitted the unopened consumer file");
            fs::remove(root / "tools.kex");
            fs::remove(root / "z_consumer.kex");
            fs::remove(root);
        });
    });
    return runAll();
}
