#include <cstdlib>
#include "test.hxx"
#include "../src/lsp/server.hxx"
#include "../src/lsp/snippets.hxx"
#include "../src/lsp/tey_roots.hxx"

#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace test;

namespace {
// The compiled standard library, as the CLI locates it. Passed explicitly
// rather than defaulted: reading interfaces from .kex SOURCE instead loses
// inferred result types, which is what once left every method call on a call
// result typed `unknown` in the editor.
auto testRuntimeBeamDir() -> std::string {
    if (const char* configured = std::getenv("KEX_RUNTIME_DIR"); configured && *configured) {
        return configured;
    }
    return {};
}

auto frame(const std::string& json) -> std::string {
    return "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
}

auto responseForId(const std::string& output, int id) -> std::string {
    const auto needle = "\"id\":" + std::to_string(id);
    size_t cursor = 0;
    while ((cursor = output.find("\r\n\r\n", cursor)) != std::string::npos) {
        const auto body = cursor + 4;
        const auto next = output.find("Content-Length:", body);
        auto message =
            output.substr(body, next == std::string::npos ? std::string::npos : next - body);
        if (message.find(needle) != std::string::npos) {
            return message;
        }
        cursor = body;
    }
    return {};
}

// The text as a JSON string's contents.
auto jsonText(const std::string& text) -> std::string {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out;
}

// Opens `text` as `uri` and answers one request (`hover`, `completion`,
// `definition`) at a 0-based line and character: the server's reply to it.
// `capabilities` is the client capabilities object `initialize` sends, and
// `extraParams` more of the request's params (`,"ch":"\\n"`).
auto askAt(
    const std::string& uri,
    const std::string& text,
    const std::string& method,
    int line,
    int character,
    const std::string& capabilities = "{}",
    const std::string& extraParams = "") -> std::string {
    std::string messages;
    messages += frame(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":)" +
        capabilities + "}}");
    messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    messages += frame(
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
        uri + R"(","languageId":"kex","version":1,"text":")" + jsonText(text) + R"("}}})");
    messages += frame(
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/)" + method +
        R"(","params":{"textDocument":{"uri":")" + uri + R"("},"position":{"line":)" +
        std::to_string(line) + R"(,"character":)" + std::to_string(character) + "}" + extraParams +
        "}}");
    messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
    messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");
    std::istringstream input(messages);
    std::ostringstream output;
    kex::lsp::run(input, output, testRuntimeBeamDir());
    return responseForId(output.str(), 2);
}

// The snippet items of a completion reply, for a failure message short
// enough to read: the name items before them run to tens of kilobytes.
auto snippetItems(const std::string& reply) -> std::string {
    std::string items;
    for (auto at = reply.find(R"("kind":15)"); at != std::string::npos;
         at = reply.find(R"("kind":15)", at + 1)) {
        const auto start = reply.rfind("{\"", at);
        const auto end = reply.find("\"label\":", at);
        const auto close = end == std::string::npos ? end : reply.find('"', end + 9);
        items += reply.substr(start, close == std::string::npos ? 200 : close - start + 1) + "\n";
    }
    return items.empty() ? "(no snippets)" : items;
}

// `textDocument/rename` at a position of `uri`, opened with `text`, in the
// workspace `rootUri` ("null" for none).
auto renameAt(
    const std::string& rootUri,
    const std::string& uri,
    const std::string& text,
    int line,
    int character,
    const std::string& newName) -> std::string {
    std::string messages;
    messages += frame(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":)" +
        rootUri + R"(,"capabilities":{}}})");
    messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    messages += frame(
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
        uri + R"(","languageId":"kex","version":1,"text":")" + jsonText(text) + R"("}}})");
    messages += frame(
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/rename","params":{"textDocument":{"uri":")" +
        uri + R"("},"position":{"line":)" + std::to_string(line) + R"(,"character":)" +
        std::to_string(character) + R"(},"newName":")" + newName + R"("}})");
    messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
    messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");
    std::istringstream input(messages);
    std::ostringstream output;
    kex::lsp::run(input, output, testRuntimeBeamDir());
    return responseForId(output.str(), 2);
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
        it("preserves distinct declarations, inferred types, and diagnostics", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-distinct.kex","languageId":"kex","version":1,"text":"distinct type UserId = Integer\nmain do\n  let id = 1.as(UserId)\n  let wrong : UserId = 1 + 1\n  id\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-distinct.kex"},"position":{"line":0,"character":15}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-distinct.kex"},"position":{"line":4,"character":3}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            const auto declarationHover = responseForId(result, 2);
            assertTrue(
                declarationHover.find("distinct type UserId =") != std::string::npos &&
                    declarationHover.find("Integer") != std::string::npos,
                "distinct declaration hover lost its modifier or backing type");
            assertTrue(
                responseForId(result, 3).find("id : UserId") != std::string::npos,
                "distinct local binding hover lost its nominal type");
            assertTrue(
                result.find("type mismatch: expected UserId, got Integer") != std::string::npos,
                "distinct nominal mismatch did not reach LSP diagnostics");
        });

        it("hovers a negated receiver and its field by their own names", []() {
            // kexhq/kex#456: in `!control.hint.empty?` the `!` belongs to no
            // name. It made `control` hover as `!control : Bool`, and `hint`
            // fall through to whatever function shares its name.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-negated.kex","languageId":"kex","version":1,"text":"record FormField do\n  hint : String\nend\n\nlet shown(control: FormField) -> Bool = !control.hint.empty?\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-negated.kex"},"position":{"line":4,"character":43}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-negated.kex"},"position":{"line":4,"character":50}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto receiver = responseForId(output.str(), 2);
            assertTrue(
                receiver.find("control : FormField") != std::string::npos,
                "negated receiver hovered as something else: " + receiver);
            const auto field = responseForId(output.str(), 3);
            assertTrue(
                field.find("hint : String") != std::string::npos,
                "field of a negated receiver lost its type: " + field);
        });

        it("hovers custom DNS resolvers as nominal typed handles", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-dns.kex","languageId":"kex","version":1,"text":"using Net.DNS\nusing Net.IP, only: [Address]\nmain do\n  let server = Nameserver { address: Address.parse(\"127.0.0.1\").try, port: Net.Port.from(53).try }\n  let resolver: Resolver = Resolver.custom(ResolverOptions { nameservers: [server], timeout: 100.millisecond }).try\n  resolver.close\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-dns.kex"},"position":{"line":5,"character":5}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("resolver : Net.DNS.Resolver") != std::string::npos ||
                    result.find("resolver : Resolver") != std::string::npos,
                "custom DNS resolver hover lost its nominal type: " + result);
            assertTrue(
                result.find("undefined") == std::string::npos,
                "typed DNS source produced LSP diagnostics: " + result);
        });

        it("publishes diagnostics for unknown types in bare declarations", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-bare-type.kex","languageId":"kex","version":1,"text":"value : MissingTopLevel\nlet value = 1\nmodule Example do\n  moduleValue : MissingModuleType\n  let moduleValue = 1\nend\ntrait ExampleTrait do\n  traitValue : MissingTraitType\nend\ntype Box\nmake Box do\n  makeValue :> MissingMakeType\n  let makeValue = 1\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-declaration-defaults.kex","languageId":"kex","version":1,"text":"record Settings do\n  retries : Integer = \"many\"\nend\nlet retry(count: Integer = \"many\") = count\n"}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("publishDiagnostics") != std::string::npos,
                "missing diagnostics notification");
            for (const auto* type :
                 {"MissingTopLevel", "MissingModuleType", "MissingTraitType", "MissingMakeType"}) {
                assertTrue(
                    result.find("unknown type `" + std::string(type) + "`") != std::string::npos,
                    std::string("missing LSP diagnostic for ") + type);
            }
            assertTrue(
                occurrences(result, "type mismatch: expected Integer, got String") >= 2,
                "declaration-default diagnostics did not reach the LSP");
        });

        it("publishes diagnostics for a .ket template, mapped back to its own lines", []() {
            // kexhq/kex#317: the server treats a `.ket` file as the template
            // it is, scanning its `<% %>` holes into a synthetic Kex
            // function it can run the ordinary analyzer against, then
            // translates any diagnostic's position back through the holes'
            // byte ranges. `count` comes from the `params:` frontmatter;
            // `missingVariable` does not exist anywhere, so this must
            // surface as a real "Undefined" diagnostic — and specifically on
            // line 3 (0-based), the actual `<%= %>` line, not some line
            // inside the server's own generated wrapper function.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-template.html.ket","languageId":"ket-html","version":1,"text":"---\nparams: [count: Integer]\n---\nTotal: <%= count + missingVariable %>\n"}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("publishDiagnostics") != std::string::npos,
                "missing diagnostics notification for .ket template");
            assertTrue(
                result.find("missingVariable") != std::string::npos,
                "undefined name inside a .ket hole never reached the LSP: " + result);
            assertTrue(
                result.find("\"line\":3") != std::string::npos,
                "diagnostic did not map back to the template's own line: " + result);
            assertTrue(
                result.find("\"line\":0") == std::string::npos &&
                    result.find("\"line\":4") == std::string::npos,
                "diagnostic pointed at the frontmatter or the synthetic wrapper "
                "instead of the hole: " +
                    result);
        });

        it("completes a params: receiver's methods inside a .ket hole", []() {
            // `items` comes from `params: [items: [Integer]]`; completing
            // right after `items.` must offer real List methods, which
            // means the incoming position (line 3, template coordinates)
            // had to translate to where `items.` actually sits in the
            // synthetic function before the ordinary completion logic ran
            // (kexhq/kex#317). No range needs translating back here: a
            // completion item never carries a `.textEdit` in this server.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-completion.ket","languageId":"ket","version":1,"text":"---\nparams: [items: [Integer]]\n---\n<%= items. %>\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-completion.ket"},"position":{"line":3,"character":10}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto completion = responseForId(output.str(), 2);
            assertTrue(
                completion.find(R"("label":"map")") != std::string::npos &&
                    completion.find(R"("label":"count")") != std::string::npos,
                "completion after a params: receiver's dot inside a .ket hole "
                "did not offer List methods: " +
                    completion);
        });

        it("navigates to a local let binding declared in an earlier .ket hole", []() {
            // `doubled` is declared in the control hole on line 3 and used
            // in the interpolation hole on line 4. Go-to-definition from
            // the use must land back on line 3 in THIS SAME template, which
            // needs both directions of translation: the incoming position
            // (line 4) into synthetic coordinates to find the reference,
            // and the found declaration's synthetic position back into the
            // template's own coordinates for the reported range
            // (kexhq/kex#317).
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-definition.ket","languageId":"ket","version":1,"text":"---\nparams: [count: Integer]\n---\n<% let doubled = count * 2 %>\n<%= doubled %>\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-definition.ket"},"position":{"line":4,"character":6}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto definition = responseForId(output.str(), 2);
            assertTrue(
                definition.find("kex-lsp-definition.ket") != std::string::npos,
                "definition did not resolve to the .ket template itself: " + definition);
            assertTrue(
                definition.find("\"line\":3") != std::string::npos,
                "definition range did not map back to the declaring hole's own "
                "line: " +
                    definition);
        });

        it("hovers a let-bound name as its binding, without a same-named function's docs", []() {
            // `let name` and `let list` share their names with stdlib
            // functions (Culture's `name`, FS.Directory's `list`), whose
            // prose was attached to the binding's hover.
            const std::string text =
                "main do\n  let name = \"ada\".upperCase\n  let list = [1, 2].map { |x| x * 2 }\n"
                "  IO.printLine(name)\nend\n";
            const auto name = askAt("file:///tmp/kex-lsp-let-hover.kex", text, "hover", 1, 6);
            assertTrue(name.find("name : String") != std::string::npos, name);
            assertTrue(
                name.find("Display name") == std::string::npos,
                "the binding took Culture's documentation: " + name);
            const auto list = askAt("file:///tmp/kex-lsp-let-hover.kex", text, "hover", 2, 6);
            assertTrue(list.find("list : [Integer]") != std::string::npos, list);
            assertTrue(
                list.find("Returns the names of the files and directories") == std::string::npos,
                "the binding took FS.Directory.list's documentation: " + list);
        });

        it("navigates from a record pattern and a constructor to the checker's declaration", []() {
            // Two modules declare `Point` and `Shade`. Inside B, the pattern
            // `Point { x }` and the constructor `Tint(3)` are B's.
            const std::string text = "module A do\n  record Point do\n    x : Integer\n  end\n"
                                     "  type Shade = Light | Tint(Integer)\nend\n\n"
                                     "module B do\n  record Point do\n    x : Integer\n  end\n"
                                     "  type Shade = Light | Tint(Integer)\n\n"
                                     "  let read(p: Point) -> Integer do\n    match p do\n      "
                                     "Point { x } => x\n    end\n  end\n\n"
                                     "  let shade -> Shade = Tint(3)\nend\n\n"
                                     "main do\n  IO.inspect(B.read(B.Point { x: 1 }))\nend\n";
            const auto pattern =
                askAt("file:///tmp/kex-lsp-pattern-definition.kex", text, "definition", 15, 7);
            assertTrue(
                pattern.find("\"line\":8") != std::string::npos,
                "the pattern did not lead to B's Point: " + pattern);
            const auto constructor =
                askAt("file:///tmp/kex-lsp-pattern-definition.kex", text, "definition", 19, 24);
            assertTrue(
                constructor.find("\"line\":11") != std::string::npos,
                "the constructor did not lead to B's Shade: " + constructor);
            const auto annotation =
                askAt("file:///tmp/kex-lsp-pattern-definition.kex", text, "definition", 19, 17);
            assertTrue(
                annotation.find("\"line\":11") != std::string::npos,
                "the annotation did not lead to B's Shade: " + annotation);
        });

        it("navigates from a constructor's argument to the argument, not the type", []() {
            // `Orders(me)` is a reference to `Msg` where `Orders` is written;
            // the index took it as `Msg`'s name's length from there, which
            // covered `me` (examples/supervision_tree.kex). And `me`, bound by
            // a block passed to a generic parameter, had no binding position.
            const std::string text =
                "type Msg = Ping(String) | Orders(Process<(Atom, [String])>)\n\n"
                "foul ask(name: Atom, build: Process<A> -> X) do\n"
                "  build(Process.self)\nend\n\n"
                "foul status do\n  let a = ask(:x) { |me| Orders(me) }\n  a\nend\n";
            const auto definition =
                askAt("file:///tmp/kex-lsp-constructor-argument.kex", text, "definition", 7, 32);
            assertTrue(
                definition.find("\"line\":7") != std::string::npos &&
                    definition.find("\"character\":21") != std::string::npos,
                "`me` did not lead to its block parameter: " + definition);
            const auto hover =
                askAt("file:///tmp/kex-lsp-constructor-argument.kex", text, "hover", 7, 32);
            assertTrue(
                hover.find("me : ") != std::string::npos,
                "`me` hovered as something else: " + hover);
        });

        it("completes a name the condition of a `then … else` shares a position with", []() {
            // The conditional is recorded at `other` too; the receiver is
            // `other`, whichever the checker's map gives first
            // (examples/more_traits.kex).
            const std::string text =
                "record Version do\n  major : Integer\n  minor : Integer\nend\n\n"
                "let cmp(a: Version, other: Version) -> Integer do\n"
                "  if true\n    a.major < other.major then 1 else 2\n"
                "  else\n    a.minor < other.minor then 1 else 2\n  end\nend\n";
            for (const auto& [line, field] : {std::pair{7u, "major"}, std::pair{9u, "minor"}}) {
                const auto reply =
                    askAt("file:///tmp/kex-lsp-then-receiver.kex", text, "completion", line, 25);
                assertTrue(
                    reply.find(std::string("\"filterText\":\"") + field + "\"") !=
                        std::string::npos,
                    "`other.` did not complete Version's fields: " + reply);
            }
        });

        it("completes a parenthesized receiver as the expression inside", []() {
            // A group is no node: nothing was recorded at its `(`, and an
            // operator is recorded at its right operand.
            const std::string text =
                "let f(a: Integer, b: Integer) -> String do\n"
                "  let xs = (1..10).map { |x| x }\n  (a + b).to(String)\nend\n";
            const auto range = askAt("file:///tmp/kex-lsp-group.kex", text, "completion", 1, 22);
            assertTrue(
                range.find("\"filterText\":\"map\"") != std::string::npos,
                "`(1..10).map` completed nothing: " + range);
            const auto sum = askAt("file:///tmp/kex-lsp-group.kex", text, "completion", 2, 12);
            assertTrue(
                sum.find("\"filterText\":\"to\"") != std::string::npos,
                "`(a + b).to` completed nothing: " + sum);
        });

        it("answers a record literal's key as the field the checker resolved", []() {
            // By name, the record was the capitalised word before the `{`:
            // `Shapes.Box { size: 3 }` and `Label { size: 1.5 }` are two
            // records' `size`, and a qualified one was found by its last segment.
            const std::string text =
                "module Shapes do\n  record Box do\n    size : Integer\n    name : String\n  "
                "end\nend\n\n"
                "record Label do\n  size : Float\nend\n\n"
                "main do\n  let b = Shapes.Box { size: 3, name: \"b\" }\n"
                "  let l = Label { size: 1.5 }\n  IO.printLine(b.size + l.size.round)\nend\n";
            const std::string uri = "file:///tmp/kex-lsp-record-keys.kex";
            const auto boxSize = askAt(uri, text, "hover", 12, 24);
            assertTrue(
                boxSize.find("size : Integer") != std::string::npos, "Box's `size`: " + boxSize);
            const auto labelSize = askAt(uri, text, "hover", 13, 18);
            assertTrue(
                labelSize.find("size : Float") != std::string::npos,
                "Label's `size`: " + labelSize);
            const auto definition = askAt(uri, text, "definition", 13, 18);
            assertTrue(
                definition.find("\"start\":{\"character\":2,\"line\":8}") != std::string::npos,
                "Label's `size` declaration: " + definition);
        });

        it("completes a field shorthand and a `.try` as the value they are", []() {
            // `@timeout.` shares its position with the implicit `this`, which
            // was completed instead (Throttle's members); `.try` is no call, and a
            // chain ending in it completed nothing.
            const std::string text =
                "record Throttle do\n  timeout : Integer\nend\n\n"
                "make Throttle do\n  let capped -> Integer = @timeout.abs\nend\n\n"
                "main do\n  let n = \"4\".to(Integer).try.abs\n"
                "  IO.printLine(Throttle { timeout: n }.capped.to(String).or(\"\"))\nend\n";
            const std::string uri = "file:///tmp/kex-lsp-shorthand.kex";
            const auto field = askAt(uri, text, "completion", 5, 38);
            assertTrue(
                field.find("\"filterText\":\"abs\"") != std::string::npos,
                "`@timeout.abs` completed nothing: " + field);
            const auto tried = askAt(uri, text, "completion", 9, 36);
            assertTrue(
                tried.find("\"filterText\":\"abs\"") != std::string::npos,
                "`.try.abs` completed nothing: " + tried);
        });

        describe("rename", []() {
            // The edits of a rename answer, as `line:character` of each start.
            const auto starts = [](const std::string& answer) {
                std::vector<std::string> found;
                for (size_t at = answer.find("\"start\":{"); at != std::string::npos;
                     at = answer.find("\"start\":{", at + 1)) {
                    const auto character = answer.find("\"character\":", at);
                    const auto line = answer.find("\"line\":", at);
                    found.push_back(
                        std::to_string(std::stoi(answer.substr(line + 7))) + ":" +
                        std::to_string(std::stoi(answer.substr(character + 12))));
                }
                std::sort(found.begin(), found.end());
                return found;
            };
            const std::string shapes =
                "let area(width: Integer, height: Integer) -> Integer = width * height\n"
                "\n"
                "let total(sizes: [Integer]) -> Integer do\n"
                "  let doubled = sizes.map { |size| size * 2 }\n"
                "  let count = doubled.length\n"
                "  count + area(count, 2)\n"
                "end\n";
            const std::string uri = "file:///tmp/kex-lsp-rename.kex";

            it("renames a local where the checker bound it", [=]() {
                const auto answer = renameAt("null", uri, shapes, 4, 7, "n");
                assertTrue(
                    starts(answer) == std::vector<std::string>{"4:6", "5:15", "5:2"},
                    "`count`'s binding and its two uses: " + answer);
            });

            it("renames a parameter from its declaration", [=]() {
                // A function's parameter had no position, so nothing was
                // bound there: no references, no rename, and hover showed
                // the stdlib's `Bits.width`.
                const auto answer = renameAt("null", uri, shapes, 0, 11, "w");
                assertTrue(
                    starts(answer) == std::vector<std::string>{"0:55", "0:9"},
                    "`width`'s declaration and its use: " + answer);
                const auto hover = askAt(uri, shapes, "hover", 0, 11);
                assertTrue(
                    hover.find("width : Integer") != std::string::npos,
                    "the parameter hovered as something else: " + hover);
            });

            it("refuses a name the scope already has", [=]() {
                const auto answer = renameAt("null", uri, shapes, 4, 7, "doubled");
                assertTrue(
                    answer.find("`doubled` already names something") != std::string::npos,
                    "renamed `count` over `doubled`: " + answer);
            });

            it("refuses a keyword, and a function declared outside the workspace", [=]() {
                const auto keyword = renameAt("null", uri, shapes, 4, 7, "end");
                assertTrue(
                    keyword.find("is not a name") != std::string::npos,
                    "renamed to a keyword: " + keyword);
                const auto stdlib = renameAt("null", uri, shapes, 3, 23, "mapped");
                assertTrue(
                    stdlib.find("\"error\"") != std::string::npos &&
                        stdlib.find("\"changes\"") == std::string::npos,
                    "renamed the standard library's `map`: " + stdlib);
            });

            it("renames a function across the workspace, qualified calls too", [=]() {
                namespace fs = std::filesystem;
                const fs::path root = "/tmp/kex-lsp-rename-ws";
                fs::remove_all(root);
                fs::create_directories(root);
                std::ofstream(root / "geo.kex")
                    << "module Geo\n\nlet area(side: Integer) -> Integer = side * side\n";
                const std::string main = "using Geo\n\nmain do\n  IO.printLine(Geo.area(2))\n  "
                                         "IO.printLine(area(3))\nend\n";
                std::ofstream(root / "main.kex") << main;
                const auto answer = renameAt(
                    "\"file://" + root.string() + "\"", "file://" + (root / "main.kex").string(),
                    main, 3, 20, "surface");
                // `Geo.area(2)` was missed: the index checked main.kex without
                // geo.kex, so `Geo` was no module there.
                assertTrue(
                    starts(answer) == std::vector<std::string>{"2:4", "3:19", "4:15"},
                    "the declaration and both calls: " + answer);
                assertTrue(
                    answer.find("geo.kex") != std::string::npos,
                    "the declaration in geo.kex was not renamed: " + answer);
                fs::remove_all(root);
            });
        });

        it("hovers a binding of unknown type as the binding", []() {
            // `json` is a Map of Any, so `name` is of a type nothing knows:
            // it is still the binding, not Weekday's `name`
            // (examples/read_package.kex).
            const std::string text = "let lookup(json: Map<String, Any>) -> String do\n"
                                     "  let name = json[\"name\"].or(\"?\")\n  \"${name}\"\nend\n\n"
                                     "main do\n  IO.printLine(lookup({}))\nend\n";
            const auto binding = askAt("file:///tmp/kex-lsp-any-hover.kex", text, "hover", 1, 6);
            assertTrue(binding.find("name : Any") != std::string::npos, binding);
            assertTrue(
                binding.find("Weekday") == std::string::npos,
                "the binding hovered as Weekday's name: " + binding);
            const auto use = askAt("file:///tmp/kex-lsp-any-hover.kex", text, "hover", 2, 6);
            assertTrue(
                use.find("Weekday") == std::string::npos,
                "the use hovered as Weekday's name: " + use);
        });

        it("completes the members of a value an opt-in module's function returns", []() {
            // `TCP.bind(...).try` is a TCPListener: its own functions, not
            // `close` alone.
            const auto reply = askAt(
                "file:///tmp/kex-lsp-listener.kex",
                "using Net.Socket\n\nmain do\n  let listener = "
                "TCP.bind(\"127.0.0.1:0\").try\n  listener.\nend\n",
                "completion", 4, 11);
            for (const auto* member : {"accept", "localAddress", "closed?", "close"}) {
                assertTrue(
                    reply.find(std::string("\"filterText\":\"") + member + "\"") !=
                        std::string::npos,
                    std::string("missing ") + member + ": " + reply);
            }
        });

        // A record of a module loaded on demand is known from its interface
        // alone, and its fields were offered nowhere (kexhq/kex#700).
        it("completes the fields of a record an opt-in module declares", []() {
            const auto reply = askAt(
                "file:///tmp/kex-lsp-culture.kex",
                "using Culture\n\nmain do\n  let culture = Culture.of(\"hu-HU\").try\n"
                "  culture.\nend\n",
                "completion", 4, 10);
            for (const auto* field : {"name", "numbers", "time"}) {
                assertTrue(
                    reply.find(std::string("\"filterText\":\"") + field + "\"") !=
                        std::string::npos,
                    std::string("missing ") + field + ": " + reply);
            }
        });

        it("completes a range's own functions", []() {
            const auto reply = askAt(
                "file:///tmp/kex-lsp-range.kex", "main do\n  let range = 1..10\n  range.\nend\n",
                "completion", 2, 8);
            for (const auto* member : {"map", "reduce", "first", "last", "contains?"}) {
                assertTrue(
                    reply.find(std::string("\"label\":\"") + member + "\"") != std::string::npos,
                    std::string("missing ") + member + ": " + reply);
            }
        });

        it("completes a chain against its last link, not its root", []() {
            // `layer.biases.` is a [Float]: Layer's fields came back, read
            // from where the chain STARTS, which is `layer`'s position.
            const std::string text =
                "record Layer do\n  weights : [[Float]]\n  biases  : [Float]\nend\n\n"
                "main do\n  let layer = Layer { weights: [[0.1]], biases: [0.5] }\n"
                "  layer.biases.\nend\n";
            const auto reply = askAt("file:///tmp/kex-lsp-chain.kex", text, "completion", 7, 15);
            assertTrue(
                reply.find("\"label\":\"biases\"") == std::string::npos &&
                    reply.find("\"label\":\"weights\"") == std::string::npos,
                "a list offered the record's fields: " + reply);
            for (const auto* member : {"map", "first", "count"}) {
                assertTrue(
                    reply.find(std::string("\"label\":\"") + member + "\"") != std::string::npos,
                    std::string("missing ") + member + ": " + reply);
            }
            // And only what the receiver takes: nothing named `read` or
            // `write` accepts a Layer.
            const auto own = askAt(
                "file:///tmp/kex-lsp-chain.kex",
                "record Layer do\n  biases : [Float]\nend\n\n"
                "main do\n  let layer = Layer { biases: [0.5] }\n  layer.\nend\n",
                "completion", 6, 8);
            assertTrue(
                own.find("\"label\":\"biases\"") != std::string::npos, "missing field: " + own);
            for (const auto* foreign : {"read", "write", "open", "min", "rename"}) {
                assertTrue(
                    own.find(std::string("\"label\":\"") + foreign + "\"") == std::string::npos,
                    std::string("offered ") + foreign + " on a record: " + own);
            }
        });

        it("offers a module's functions only where a `using` brings it in", []() {
            // `"hello".sha256` without `using Digest` is an error; it was
            // offered all the same.
            const auto without = askAt(
                "file:///tmp/kex-lsp-scope.kex", "main do\n  \"hello\".sh\nend\n", "completion", 1,
                12);
            assertTrue(
                without.find("\"label\":\"sha256\"") == std::string::npos,
                "offered Digest's sha256 without `using Digest`: " + without);
            const auto with = askAt(
                "file:///tmp/kex-lsp-scope.kex", "using Digest\n\nmain do\n  \"hello\".sh\nend\n",
                "completion", 3, 12);
            assertTrue(
                with.find("\"label\":\"sha256\"") != std::string::npos,
                "did not offer sha256 under `using Digest`: " + with);
        });

        it("completes a bare name to what a use of it there reaches", []() {
            // Every top-level declaration of every prelude source was
            // offered, private helpers and opt-in modules' functions included.
            const auto reply = askAt(
                "file:///tmp/kex-lsp-bare.kex",
                "main do\n  let rate = 2\n  IO.inspect(ra)\n  IO.inspect(me)\n"
                "  IO.inspect(sh)\nend\n",
                "completion", 2, 15);
            assertTrue(
                reply.find("\"label\":\"rate\"") != std::string::npos,
                "the local was not offered: " + reply);
            const auto helper = askAt(
                "file:///tmp/kex-lsp-bare.kex", "main do\n  IO.inspect(me)\nend\n", "completion", 1,
                15);
            assertTrue(
                helper.find("\"label\":\"measureKind\"") == std::string::npos,
                "offered a name a bare use cannot reach: " + helper);
            const auto optIn = askAt(
                "file:///tmp/kex-lsp-bare.kex", "main do\n  IO.inspect(sh)\nend\n", "completion", 1,
                15);
            assertTrue(
                optIn.find("\"label\":\"sha256\"") == std::string::npos,
                "offered Digest's sha256 without `using Digest`: " + optIn);
        });

        it("goes to a local's binding and a field's declaration, as the checker resolved them",
           []() {
               // By name, `let offset` led to a stdlib record's `offset` field and
               // `p.x` to the `record` line.
               const std::string text = "record Point do\n  x : Float\n  y : Float\nend\n\n"
                                        "main do\n  let offset = Point { x: 1.0, y: 2.0 }\n  "
                                        "IO.inspect(offset.x)\nend\n";
               const auto binding =
                   askAt("file:///tmp/kex-lsp-definition.kex", text, "definition", 6, 7);
               assertTrue(
                   binding.find(R"("start":{"character":6,"line":6})") != std::string::npos,
                   "the binding did not lead to itself: " + binding);
               const auto use =
                   askAt("file:///tmp/kex-lsp-definition.kex", text, "definition", 7, 14);
               assertTrue(
                   use.find(R"("start":{"character":6,"line":6})") != std::string::npos,
                   "the use did not lead to its binding: " + use);
               const auto field =
                   askAt("file:///tmp/kex-lsp-definition.kex", text, "definition", 7, 21);
               assertTrue(
                   field.find(R"("start":{"character":2,"line":1})") != std::string::npos,
                   "the field read did not lead to the field: " + field);
           });

        it("goes to an `if … do |x|` binding at its name", []() {
            // The parser's rewrite to a `match` put the binding at the `if`.
            const std::string text =
                "main do\n  let limit: Integer? = Just(3)\n  if limit do |lim|\n"
                "    IO.inspect(lim)\n  end\nend\n";
            const auto use = askAt("file:///tmp/kex-lsp-if-binding.kex", text, "definition", 3, 15);
            assertTrue(
                use.find(R"("start":{"character":15,"line":2})") != std::string::npos,
                "the use did not lead to `|lim|`: " + use);
        });

        it("hovers `This` as the make block's type, inside a body too", []() {
            // By line, only a signature line had a symbol to name the target.
            const auto reply = askAt(
                "file:///tmp/kex-lsp-this-hover.kex",
                "record Box do\n  size : Integer\nend\n\nmake Box do\n  let grown -> Box do\n"
                "    let copy: This = this\n    copy\n  end\nend\n\nmain do\n"
                "  IO.inspect(Box { size: 2 }.grown)\nend\n",
                "hover", 6, 15);
            assertTrue(
                reply.find("This : Box") != std::string::npos,
                "`This` in a body was not the make target: " + reply);
        });

        it("documents a call by name with the function the checker chose", []() {
            // By name, `sqrt` gathered Integer's and Float's `sqrt` prose too.
            const auto reply = askAt(
                "file:///tmp/kex-lsp-called-docs.kex",
                "using Math\n\nmain do\n  IO.inspect(sqrt(9.0))\nend\n", "hover", 3, 14);
            assertTrue(
                reply.find("Raises for a negative") != std::string::npos,
                "not Math.sqrt's prose: " + reply);
            assertTrue(
                reply.find("16.sqrt") == std::string::npos,
                "a same-named function's prose came along: " + reply);
        });

        it("hovers a name after `implement:` as the trait the checker resolved", []() {
            // By name, `Pricing` was the top-level record of that name.
            const auto reply = askAt(
                "file:///tmp/kex-lsp-implement-hover.kex",
                "record Pricing do\n  amount : Integer\nend\n\nmodule Shop do\n"
                "  trait Pricing do\n    price :> Integer\n  end\nend\n\n"
                "record Box do\n  size : Integer\nend\n\n"
                "make Box, implement: Shop.Pricing do\n  let price = @size\nend\n\n"
                "main do\n  IO.inspect(Box { size: 2 }.price)\nend\n",
                "hover", 14, 28);
            assertTrue(
                reply.find("trait Shop.Pricing") != std::string::npos,
                "not the trait the block implements: " + reply);
        });

        it("hovers a field read with the type the checker gave it", []() {
            const auto reply = askAt(
                "file:///tmp/kex-lsp-field-hover.kex",
                "record Box do\n  size : Integer\nend\n\nmain do\n  let box = Box { size: 2 }\n"
                "  IO.inspect(box.size)\nend\n",
                "hover", 6, 18);
            assertTrue(
                reply.find("size : Integer") != std::string::npos,
                "the field read was not typed: " + reply);
        });

        it("hovers a local `let` as its binding, without a same-named declaration's prose", []() {
            const auto reply = askAt(
                "file:///tmp/kex-lsp-let-name.kex",
                "main do\n  let name = \"x\"\n  IO.printLine(name)\nend\n", "hover", 1, 7);
            assertTrue(
                reply.find("name : String") != std::string::npos, "not the binding: " + reply);
            assertTrue(
                reply.find("ISO tag") == std::string::npos &&
                    reply.find("Display name") == std::string::npos,
                "the binding hovered with Culture's prose: " + reply);
        });

        it("completes after the `&.` shorthand from the block's element type", []() {
            // The shorthand's receiver is the block's parameter, typed by the
            // call the block is passed to.
            const auto reply = askAt(
                "file:///tmp/kex-lsp-shorthand.kex",
                "record Item do\n  text : String\nend\n\nmain do\n  let items "
                "= [Item { text: \"a\" }]\n"
                "  IO.inspect(items.map(&.))\nend\n",
                "completion", 6, 25);
            assertTrue(
                reply.find("\"label\":\"text\"") != std::string::npos,
                "the shorthand did not complete the Item's field: " + reply);
            assertTrue(
                reply.find("\"label\":\"Assert\"") == std::string::npos,
                "the shorthand completed global names: " + reply);
        });

        it("navigates from a method call to the function the checker chose", []() {
            // Two types have a `render`. By name the first came back (Tag's);
            // the checker decided the call runs Box's, and the name index
            // records the call as a reference to that one.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-method-definition.kex","languageId":"kex","version":1,"text":"record Box do\n  value : Integer\nend\n\nrecord Tag do\n  value : Integer\nend\n\nmake Tag do\n  let render = \"tag\"\nend\n\nmake Box do\n  let render = \"box\"\nend\n\nmain do\n  let box = Box { value: 1 }\n  IO.printLine(box.render)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-method-definition.kex"},"position":{"line":18,"character":20}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto definition = responseForId(output.str(), 2);
            assertTrue(
                definition.find("\"line\":13") != std::string::npos,
                "definition did not land on Box's `render`: " + definition);
        });

        it("navigates from a record's name to the record the checker resolved", []() {
            // Two modules declare a `Point`. In B, both the result annotation
            // and the construction name B's; by name A's came first.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record-definition.kex","languageId":"kex","version":1,"text":"module A do\n  record Point do\n    x : Integer\n  end\nend\n\nmodule B do\n  record Point do\n    x : Integer\n  end\n  let origin -> Point = Point { x: 0 }\nend\n\nmain do\n  IO.inspect(B.origin)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record-definition.kex"},"position":{"line":10,"character":17}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record-definition.kex"},"position":{"line":10,"character":25}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto annotation = responseForId(output.str(), 2);
            assertTrue(
                annotation.find("\"line\":7") != std::string::npos,
                "the annotation did not lead to B's Point: " + annotation);
            const auto construction = responseForId(output.str(), 3);
            assertTrue(
                construction.find("\"line\":7") != std::string::npos,
                "the construction did not lead to B's Point: " + construction);
        });

        it("hovers a params: name inside a .ket hole with its declared type", []() {
            // Same translation the diagnostics test exercises, but for the
            // request/response direction: the incoming position (line 3,
            // pointing at "count" inside `<%= count %>`) arrives in the
            // template's own coordinates and must be translated to the
            // synthetic function's before lookup, and the returned hover
            // range translated back before it reaches the client.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hover.ket","languageId":"ket","version":1,"text":"---\nparams: [count: Integer]\n---\nTotal: <%= count %>\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hover.ket"},"position":{"line":3,"character":13}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto hover = responseForId(output.str(), 2);
            assertTrue(
                hover.find("count") != std::string::npos &&
                    hover.find("Integer") != std::string::npos,
                "hover inside a .ket hole lost the params: declared type: " + hover);
            assertTrue(
                hover.find("\"line\":3") != std::string::npos,
                "hover range did not map back to the template's own line: " + hover);
        });

        it("returns no hover for a position in a .ket template's literal text", []() {
            // Line 3, character 0 sits in "Total: " — plain template text,
            // never Kex. A position outside every hole has nothing to map
            // to, and must answer with no hover rather than misattributing
            // the request to whatever the nearest hole happens to be.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hover-text.ket","languageId":"ket","version":1,"text":"---\nparams: [count: Integer]\n---\nTotal: <%= count %>\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hover-text.ket"},"position":{"line":3,"character":0}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto hover = responseForId(output.str(), 2);
            assertTrue(
                hover.find("\"contents\"") == std::string::npos,
                "hover over the template's own literal text should be empty: " + hover);
        });

        it("resolves the wrong number type as a real diagnostic in a .ket hole", []() {
            // The synthetic function's own DECLARED parameter type
            // (`count: Integer`, from `params:`) must be honored by the
            // analyzer, not just parsed — passing a String where the
            // template declared an Integer must fail exactly as it would in
            // an ordinary .kex function.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-template2.ket","languageId":"ket","version":1,"text":"---\nparams: [count: Integer]\n---\n<% let doubled: Integer = count + \"oops\" %>\n"}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("Integer") != std::string::npos &&
                    result.find("String") != std::string::npos,
                "a declared params: type was not honored inside a .ket hole: " + result);
        });

        it("checks a .ket template inside the module that embeds it", []() {
            // `params: [card: Card]` means the `Card` declared beside the
            // `Template.html(Kex.embed(...))` that compiles the template. On
            // its own the template knew no such record, so every `?` field
            // read through it was an "Undefined method".
            //
            // Also: a hole's error used to be dropped when it was mapped back
            // (its offset was the `(` wrapped around the hole), and literal
            // text emitted nothing, so `<% if %>none<% else %><%= s %><%
            // end %>` had a Void branch beside a String one.
            namespace fs = std::filesystem;
            char rootTemplate[] = "/tmp/kex_lsp_ket_host_XXXXXX";
            const auto root = fs::path(mkdtemp(rootTemplate));
            fs::create_directories(root / "app" / "views" / "parts");
            std::ofstream(root / "package.kex")
                << "package \"kp\" do\n  version(\"0.1.0\")\n  sources([\"app\"])\nend\n";
            std::ofstream(root / "app" / "views" / "page.kex")
                << "module App.Views.Page\n\n"
                   "record Card do\n  key : String\n  shown? : Bool\n  note : String = "
                   "\"\"\nend\n\n"
                   "let card = Template.html(Kex.embed(\"parts/_card.html.ket\"))\n";
            const auto templatePath = root / "app" / "views" / "parts" / "_card.html.ket";
            const std::string text =
                "---\\nparams: [card: Card, topic: String]\\n---\\n"
                "<p><%= card.key %><% if card.shown? %> on<% end %><%= card.note %></p>\\n"
                "<% if topic.empty? %>none<% else %><%= topic %><% end %>\\n"
                "<%= card.missing %>\\n";
            std::ofstream(templatePath) << "";

            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file://)" +
                templatePath.string() + R"(","languageId":"ket-html","version":1,"text":")" + text +
                R"("}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            fs::remove_all(root);
            const auto result = output.str();
            assertTrue(
                result.find("shown?") == std::string::npos,
                "the embedding module's record was not in scope: " + result);
            assertTrue(
                result.find("branch type mismatch") == std::string::npos,
                "literal text did not count as a branch's output: " + result);
            assertTrue(
                result.find("undefined function `missing`") != std::string::npos,
                "an error inside a <%= %> hole was dropped: " + result);
            assertTrue(
                result.find("\"line\":5") != std::string::npos,
                "the hole's error did not map back to its own line: " + result);
        });

        it("completes a module the file declares, and a constant's members", []() {
            // examples/city_distance.kex: `Hav` offered no `Haversine`, and
            // `EARTH_RADIUS.` nothing: its Measure<Quantity.Length> was split
            // at the argument's dot.
            const std::string uri = "file:///tmp/kex-lsp-module-constant.kex";
            const std::string text = "using Units.SI\n"
                                     "module Haversine do\n"
                                     "  let EARTH_RADIUS = 6371.kilometer\n"
                                     "  let around() = EARTH_RADIUS.sc\n"
                                     "end\n"
                                     "main do\n"
                                     "  Hav\n"
                                     "  Haversine.EARTH_RADIUS.sca\n"
                                     "end\n";
            const auto module = askAt(uri, text, "completion", 6, 5);
            assertTrue(
                module.find(R"("label":"Haversine")") != std::string::npos,
                "the file's module was not offered: " + module);
            const auto constant = askAt(uri, text, "completion", 3, 32);
            assertTrue(
                constant.find(R"("label":"scale")") != std::string::npos,
                "the constant's Measure members were not offered: " + constant);
            // Reached through its module, the constant is a member read the
            // checker records by the member's name.
            const auto qualified = askAt(uri, text, "completion", 7, 28);
            assertTrue(
                qualified.find(R"("label":"scaled")") != std::string::npos,
                "the module-qualified constant's members were not offered: " + qualified);
        });

        it("serves diagnostics and completion for unsaved buffers", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
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
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("publishDiagnostics") != std::string::npos,
                "missing diagnostics notification");
            assertTrue(
                result.find("undefined name") != std::string::npos, "missing compiler diagnostic");
            assertTrue(
                result.find(R"("character":9)") != std::string::npos,
                "compiler byte column was not converted to LSP UTF-16");
            assertTrue(
                result.find(R"("label":"map")") != std::string::npos &&
                    result.find("map :") != std::string::npos,
                "missing compiler-backed completion");
            assertTrue(
                result.find(R"("label":"count")") != std::string::npos,
                "missing type-aware list-literal completion");
            assertTrue(
                result.find("completionProvider") != std::string::npos,
                "initialize response did not advertise completion");
            assertTrue(
                result.find(R"("hoverProvider":true)") != std::string::npos,
                "initialize response did not advertise hover");
            assertTrue(
                result.find("1 : Integer") != std::string::npos,
                "hover did not expose an inferred expression type");
            assertTrue(
                result.find(R"(record Point do\n  x : Int\n  label : String\nend)") !=
                    std::string::npos,
                "record hover did not expose its fields");
            assertTrue(
                result.find("A Cartesian point.") != std::string::npos,
                "record hover did not expose its documentation comment");
            assertTrue(
                result.find("type String") != std::string::npos,
                "primitive annotation hover was mistaken for a module");
            assertTrue(
                result.find("type Integer") != std::string::npos,
                "parameter type hover was mistaken for a module");
            assertTrue(
                result.find("add : A -> A -> A") != std::string::npos,
                "function hover leaked internal type variables");
            assertTrue(
                result.find("~add : Integer -> Integer") != std::string::npos,
                "curried function hover lost its specialized signature");
            assertTrue(
                result.find("Just : Integer?") != std::string::npos,
                "Just constructor hover did not show Optional payload type");
            assertTrue(
                result.find("None : A?") != std::string::npos,
                "None constructor hover did not show generic Optional type");
            assertTrue(
                result.find(R"("label":"File")") != std::string::npos,
                "FS completion did not include its nested File module");
            assertTrue(
                result.find(R"("label":"foul read")") != std::string::npos &&
                    result.find("read : String -> Result<String, FS.FileError>") !=
                        std::string::npos,
                "FS.File completion did not include read");
            assertTrue(
                result.find("foul read : String -> Result<String, FS.FileError>") !=
                    std::string::npos,
                "completion did not expose imported module foulness");
            assertTrue(
                result.find("(path, content)") != std::string::npos,
                "completion did not expose function parameter names");
            assertTrue(
                result.find(R"("label":"foul write")") != std::string::npos &&
                    result.find(
                        "(path, content) : String -> String -> Result<Void, FS.FileError>") !=
                        std::string::npos,
                "foul completion did not use declaration order");
            assertTrue(
                result.find("read : String -> Result<String, FS.FileError>") != std::string::npos,
                "imported built-in function hover omitted its signature");
            assertTrue(
                result.find("Reads the KexI interface chunk") == std::string::npos,
                "FS.File.read used Kex.Interface.read documentation");
            assertTrue(
                result.find(R"(type Optional<X> =\n  Just(X)\n| None)") != std::string::npos,
                "imported ADT hover omitted constructors or payload types");
        });
        it("shows a primitive passed to to(...) as a type value", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-type-value.kex","languageId":"kex","version":1,"text":"main do\n  42.to(String)\n  Type.of(42)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-type-value.kex"},"position":{"line":1,"character":9}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-type-value.kex"},"position":{"line":2,"character":3}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-type-value.kex"},"position":{"line":2,"character":8}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("type String") != std::string::npos,
                "to(String) hover did not identify String as a type value");
            assertTrue(
                result.find("module String") == std::string::npos,
                "to(String) hover incorrectly identified String as a module");
            assertTrue(
                result.find("record Type<T> do") != std::string::npos,
                "Type.of receiver hover did not identify the Type record");
            assertTrue(
                result.find("of : Integer -> Type<Integer>") != std::string::npos,
                "Type.of hover leaked the checker's internal unknown type");
        });
        it("keeps Type.of result types on local bindings", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
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
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("at : Type") != std::string::npos,
                "Type.of result was not retained on its local binding");
            assertTrue(
                result.find("function at") == std::string::npos,
                "local binding hover fell through to an unrelated global function");
            assertTrue(
                result.find(R"(type Hello =\n  You(String)\n| World)") != std::string::npos,
                "local ADT hover omitted its constructors");
            assertTrue(
                result.find("You : String -> Hello") != std::string::npos,
                "ADT constructor hover omitted its full type");
        });
        it("does not attach same-named stdlib docs to a local binding", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-wc.kex","languageId":"kex","version":1,"text":"main do\n  let lines = [\"one\", \"two\"]\n  let words = lines.map { |line| line }\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-wc.kex"},"position":{"line":2,"character":16}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto hover = responseForId(output.str(), 2);
            assertTrue(
                hover.find("lines : [String]") != std::string::npos,
                "local lines hover omitted its inferred list type");
            assertTrue(
                hover.find("Splits into lines") == std::string::npos &&
                    hover.find("String.lines") == std::string::npos,
                "local lines hover included String.lines documentation");
        });
        it("keeps value-lambda parameters local on hover", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-closure-name.kex","languageId":"kex","version":1,"text":"let makeGreeter(prefix: String) do\n  return { |name| \"${prefix}, ${name}!\" }\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-closure-name.kex"},"position":{"line":1,"character":13}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-closure-name.kex"},"position":{"line":1,"character":32}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            for (const auto id : {2, 3}) {
                const auto response = responseForId(result, id);
                assertTrue(
                    response.find("name : A") != std::string::npos,
                    "lambda parameter hover lost its local generic type: " + response);
                assertTrue(
                    response.find("Weekday") == std::string::npos,
                    "lambda parameter hover leaked Weekday.name: " + response);
            }
        });
        it("uses imported signatures for standard-library method hovers", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/stream.spec.kex","languageId":"kex","version":1,"text":"# kex: no-check\nmain do\n  let s = Stream.from(0) { |n| n + 1 }\n  s.drop(3).map { |n| n }.filter { |n| n.even? }\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/stream.spec.kex"},"position":{"line":3,"character":5}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/stream.spec.kex"},"position":{"line":3,"character":12}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/stream.spec.kex"},"position":{"line":3,"character":28}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":5,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("drop :") != std::string::npos,
                "Stream.drop hover omitted its imported signature");
            assertTrue(
                result.find("map :") != std::string::npos,
                "Stream.map hover omitted its imported signature");
            assertTrue(
                result.find("filter :") != std::string::npos,
                "Stream.filter hover omitted its imported signature");
            assertTrue(
                result.find("function drop") == std::string::npos,
                "Stream.drop fell back to a generic function hover");
        });
        it("renders standard-library and user RDoc comments", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
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
            messages += frame(R"({"jsonrpc":"2.0","id":6,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("A documented module.") != std::string::npos,
                "module hover omitted user documentation");
            assertTrue(
                result.find("Returns `value` unchanged.") != std::string::npos,
                "RDoc inline code was not converted to Markdown");
            assertTrue(
                result.find("**Parameter `value`:** `Integer`") != std::string::npos,
                "RDoc parameter was not formatted");
            assertTrue(
                result.find("**Returns:** `Integer`") != std::string::npos,
                "RDoc return type was not formatted");
            assertTrue(
                result.find("**Example:**") != std::string::npos &&
                    result.find("identity(3) # => 3") != std::string::npos,
                "RDoc example was not rendered");
            assertTrue(
                result.find("Sums all elements. Returns `0`") != std::string::npos &&
                    result.find("[1, 2, 3].sum") != std::string::npos,
                "List.sum hover omitted standard-library documentation");
            assertTrue(
                result.find("A documented local value.") != std::string::npos,
                "local binding documentation was not available at a reference");
        });
        it("shows the capability a function needs and why", []() {
            // Nothing in `deep`'s source mentions Clock — it is inherited two
            // calls down — so hover is the only place a reader can learn it.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-caps.kex","languageId":"kex","version":1,"text":"capability Clock do\n  foul now() -> Integer do\n    return 1000\n  end\nend\nfoul stamp() -> Integer = Clock.now()\nfoul deep() -> Integer = stamp()\nmain do\n  with Clo\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-caps.kex"},"position":{"line":6,"character":6}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-caps.kex"},"position":{"line":8,"character":10}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-caps.kex"},"position":{"line":0,"character":12}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":5,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("requires Clock") != std::string::npos,
                "hover did not report the inherited capability");
            assertTrue(
                result.find("deep calls stamp") != std::string::npos,
                "hover did not explain where the requirement came from");
            assertTrue(
                result.find("stamp uses Clock") != std::string::npos,
                "hover did not reach the end of the chain");
            assertTrue(
                result.find(R"("detail":"capability Clock","kind":22,"label":"Clock")") !=
                    std::string::npos,
                "with completion did not restrict and identify capabilities");
            assertTrue(
                result.find(R"(```kex\ncapability Clock\n```)") != std::string::npos,
                "capability hover was shown as a normal module");
        });
        it("identifies traits as traits and type constraints", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-traits.kex","languageId":"kex","version":1,"text":"# A local capability.\ntrait LocalCapability do\nend\nlet accept(value: Enumerable) = value\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-traits.kex"},"position":{"line":1,"character":8}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-traits.kex"},"position":{"line":3,"character":22}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find(R"(```kex\ntrait LocalCapability\n```)") != std::string::npos,
                "local trait hover was shown as a normal type");
            assertTrue(
                result.find("A local capability.") != std::string::npos,
                "local trait hover omitted documentation");
            assertTrue(
                result.find(R"(```kex\ntrait Enumerable\n```)") != std::string::npos,
                "imported trait hover was shown only as a type");
        });
        it("does not confuse Result.Error with JSON.Error", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-error-constructor.kex","languageId":"kex","version":1,"text":"using JSON\nmain do\n  let failed = Error(\"bad\")\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-error-constructor.kex"},"position":{"line":2,"character":17}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("Error : Result<") != std::string::npos,
                "Error constructor hover omitted its inferred Result type");
            assertTrue(
                result.find("type Error =") == std::string::npos,
                "Error constructor hover was confused with JSON.Error");
        });
        it("puts the selected overload first", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-selected-overload.kex","languageId":"kex","version":1,"text":"main do\n  [1, 2].at(0)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-selected-overload.kex"},"position":{"line":1,"character":10}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            const auto selected = result.find("at : [Integer] -> Integer -> Integer?");
            const auto generic = result.find("at : [A] -> Integer -> A?");
            assertTrue(
                selected != std::string::npos, "hover omitted the selected concrete overload");
            assertTrue(
                generic == std::string::npos || selected < generic,
                "hover did not put the selected overload first");
            assertTrue(
                result.find("Selected overload") != std::string::npos,
                "selected overload was not visually separated");
        });
        it("uses the selected receiver overload documentation", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/file_modes.kex","languageId":"kex","version":1,"text":"foul readAll(file: FileHandle<CanRead, W>) -> String do\n  file.read().or(\"\")\nend\nmain do\n  let maybe: String? = None\n  IO.printLine(\"value: ${maybe.or(\"\")}\")\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/file_modes.kex"},"position":{"line":1,"character":15}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/file_modes.kex"},"position":{"line":5,"character":31}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("Returns the wrapped value") != std::string::npos,
                "Optional.or hover omitted the selected fallback documentation");
            assertEqual(
                occurrences(result, "Selected overload"), size_t{2},
                "interpolated Optional.or lost its selected overload");
            assertTrue(
                result.find("bitwise OR of") == std::string::npos &&
                    result.find("Bits.or") == std::string::npos,
                "Optional.or hover used Bits.or documentation");
        });
        it("infers literal function clause parameters", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-functions.kex","languageId":"kex","version":1,"text":"let factorial(0) = 1\nlet factorial(n: Int) = n * factorial(n - 1)\nlet fib(0) = 0\nlet fib(1) = 1\nlet fib(n: Int) = fib(n - 1) + fib(n - 2)\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-functions.kex"},"position":{"line":0,"character":6}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-functions.kex"},"position":{"line":2,"character":6}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("factorial : Integer -> Integer") != std::string::npos &&
                    result.find("factorial : A -> Integer") == std::string::npos,
                "factorial literal clause remained unconstrained");
            assertTrue(
                result.find("fib : Integer -> Integer") != std::string::npos &&
                    result.find("fib : A -> Integer") == std::string::npos,
                "fib literal clauses remained unconstrained");
        });
        it("shows record fields on record construction hovers", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record-construction.kex","languageId":"kex","version":1,"text":"record Point do\n  x : Float\n  y : Float\nend\nmain do\n  Point { x: 3.0, y: 4.0 }\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-record-construction.kex"},"position":{"line":5,"character":4}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find(R"(record Point do\n  x : Float\n  y : Float\nend)") !=
                    std::string::npos,
                "record construction hover omitted record fields");
            assertTrue(
                result.find("Point : Point") == std::string::npos,
                "record construction hover used a redundant nominal type");
        });
        it("selects the qualified foul filesystem overload and navigates", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-filesystem.kex","languageId":"kex","version":1,"text":"using FS\nmain do\n  FS.File.copy(\"src\", \"utils.kex\")\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-filesystem.kex"},"position":{"line":2,"character":11}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-filesystem.kex"},"position":{"line":2,"character":11}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            const auto selected = result.find(
                "Selected overload**\\n\\n```kex\\nfoul copy : "
                "String -> String -> Result<Void, FS.FileError>");
            assertTrue(
                selected != std::string::npos,
                "FS.File.copy did not select its foul filesystem overload");
            assertTrue(
                result.find("definitionProvider") != std::string::npos,
                "server did not advertise definition navigation");
            assertTrue(
                result.find("fs.kex") != std::string::npos,
                "FS.File.copy navigation did not resolve to its source");
        });
        it("reads a package.kex manifest instead of flagging its vocabulary", []() {
            // `bundle`, `version` and `tey` are declared by the stdlib but
            // imported by nobody, so a manifest used to open with one
            // "undefined function" per line. The declarations now arrive with
            // the file, which is what makes hover work here too.
            //
            // Every declaration the vocabulary defines appears below, and
            // `tey` and `toolchain` are the last two it declares: a
            // vocabulary file that stops parsing part way through loses the
            // declarations after the break and fails this test.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-pkg/package.kex","languageId":"kex","version":1,"text":"bundle \"demo\" do\n  version(\"0.1.0\")\n  description(\"A Kex package\")\n  license(\"MIT\")\n  kex(\">= 0.3.0\")\n  otp(\">= 26\")\n  entrypoint(\"src/main.kex\")\n  target(\"demo\")\n  command(\"check\", run: \"make check\", description: \"Type-check\")\n  toolchain(\"demo\", compiler: \"bin/demo\")\n  sources([\"app\", \"db\"])\n  plugin namespace: \"demo\", tey: \">= 0.2\" do\n    capabilities([FS.Write.workspace, FS.Read.package, Process.Run([\"git\"]), Net.Connect([\"example.com\"])])\n    command(\"docs\", run: \"script/docs.kex\")\n    generator(\"page\", entrypoint: \"generators/page.kex\")\n    generator(\"model\", entrypoint: \"generators/model.kex\") do\n      option(\"name\", type: String)\n    end\n  end\n  tey(\"greet\", git: \"https://example.com/g.git\", tag: \"v0.1.0\")\n  group :dev do\n    tey(\"mock\", git: \"https://example.com/m.git\", branch: \"main\")\n  end\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-pkg/package.kex"},"position":{"line":1,"character":4}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-pkg/package.kex"},"position":{"line":8,"character":4}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("undefined function") == std::string::npos,
                "the manifest vocabulary was reported as undefined");
            // `FS.Write.workspace` is `workspace(FS.Write)` by UFCS, and was
            // once held against the `workspace do` that takes a block.
            assertTrue(
                result.find("\"severity\":1") == std::string::npos,
                "a well-formed manifest was reported with errors: " + result);
            assertTrue(
                result.find("version : String -> Void") != std::string::npos,
                "hovering a manifest declaration showed no signature");
            assertTrue(
                result.find("command : String") != std::string::npos,
                "hovering a manifest command declaration showed no signature");
        });

        it("prefers an at-field over an unrelated receiver method", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-at-field.kex","languageId":"kex","version":1,"text":"record User do\n  name : String\nend\nmake User do\n  let greet -> String = \"Hi, ${@name}!\"\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-at-field.kex"},"position":{"line":4,"character":33}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("@name : String") != std::string::npos,
                "@name did not retain the User.name field type: " + result);
            assertTrue(
                result.find("Weekday -> Any") == std::string::npos,
                "@name resolved to the unrelated Weekday method");
        });
        // Runs one document through the server: open it, then send `requests`
        // (each a complete JSON-RPC request line), then shut down.
        auto session = [](const std::string& uri, const std::string& text,
                          const std::vector<std::string>& requests) {
            auto escaped = std::string{};
            for (char c : text) {
                if (c == '\n') {
                    escaped += "\\n";
                } else if (c == '"' || c == '\\') {
                    escaped += '\\';
                    escaped += c;
                } else {
                    escaped += c;
                }
            }
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
                uri + R"(","languageId":"kex","version":1,"text":")" + escaped + R"("}}})");
            for (const auto& request : requests) {
                messages += frame(request);
            }
            messages += frame(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");
            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            return output.str();
        };

        it("parses quoted and @ atoms, keeping later columns exact", [session]() {
            const auto result = session(
                "file:///tmp/kex-lsp-atoms.kex",
                "main do\n"
                "  let node = :b@localhost\n"
                "  let x: Integer = (:\"a b\", missingName)\n"
                "end\n",
                {});
            assertTrue(
                result.find("unexpected token") == std::string::npos,
                "the new atom spellings must parse: " + result);
            // `missingName` starts at column 28 of its line — right after a
            // quoted atom whose token text is shorter than its source.
            assertTrue(
                result.find(R"("start":{"character":28,"line":2})") != std::string::npos,
                "diagnostic after a quoted atom is misplaced: " + result);
        });

        it("offers no completion inside an atom, even after its @", [session]() {
            const auto result = session(
                "file:///tmp/kex-lsp-atom-completion.kex", "main do\n  let host = :b@\nend\n",
                {R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-atom-completion.kex"},"position":{"line":1,"character":17}}})"});
            const auto completion = responseForId(result, 2);
            assertTrue(
                completion.find(R"("result":[])") != std::string::npos,
                "typing a node name must not pop up completions: " + completion);
        });

        it("completes and documents the Node module", [session]() {
            const auto result = session(
                "file:///tmp/kex-lsp-node.kex",
                "using Node\n\nmain do\n  Node.connect(:b@localhost)\n  Node.\nend\n",
                {R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-node.kex"},"position":{"line":4,"character":7}}})",
                 R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-node.kex"},"position":{"line":3,"character":8}}})"});
            const auto completion = responseForId(result, 2);
            assertTrue(
                completion.find("connect") != std::string::npos &&
                    completion.find("whereIs") != std::string::npos,
                "Node members missing from completion: " + completion);
            const auto hover = responseForId(result, 3);
            assertTrue(
                hover.find("connect : Atom -> Result<Void, String>") != std::string::npos,
                "Node.connect hover lacks its signature: " + hover);
        });

        it("underlines the retired interop root with its replacement", [session]() {
            const auto result = session(
                "file:///tmp/kex-lsp-retired.kex",
                "main do\n  IO.printLine(Erlang.lists.reverse([1]))\nend\n", {});
            assertTrue(
                result.find("interop is now `BEAM.`") != std::string::npos,
                "retired prefix not reported: " + result);
            assertTrue(
                result.find(R"("start":{"character":15,"line":1})") != std::string::npos,
                "the diagnostic must sit on `Erlang`: " + result);
        });

        it("completes members after an inferred local receiver dot", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-local-dot.kex","languageId":"kex","version":1,"text":"main do\n  let lines = [\"one\", \"two\"]\n  lines.\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-local-dot.kex"},"position":{"line":2,"character":8}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find(R"("label":"map")") != std::string::npos &&
                    result.find(R"("label":"count")") != std::string::npos,
                "inferred [String] local produced no dot completions: " + result);
        });
        it("understands functional updates in make blocks", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex","languageId":"kex","version":1,"text":"record User do\n  age : Integer\nend\nmake User do\n  let birthday -> This do\n    new.age = this.age + 1\n    New { age: new.age }\n  end\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex"},"position":{"line":5,"character":5}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex"},"position":{"line":5,"character":8}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex"},"position":{"line":6,"character":5}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex"},"position":{"line":5,"character":16}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":6,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex"},"position":{"line":4,"character":18}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":7,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex"},"position":{"line":5,"character":19}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex","version":2},"contentChanges":[{"text":"record User do\n  age : Integer\nend\nmake User do\n  let birthday -> This do\n    New {\n      \n    }\n  end\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":8,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex"},"position":{"line":6,"character":6}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex","version":3},"contentChanges":[{"text":"record User do\n  age : Integer\nend\nmake User do\n  let birthday -> This do\n    return new { }\n  end\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":9,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex"},"position":{"line":5,"character":17}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex","version":4},"contentChanges":[{"text":"record User do\n  age : Integer\nend\nmake User do\n  let birthday -> This = New { age: @ }\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":10,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-new.kex"},"position":{"line":4,"character":37}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":11,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                responseForId(result, 2).find("new : User") != std::string::npos,
                "implicit new hover omitted its make receiver type: " + result);
            assertTrue(
                responseForId(result, 3).find(R"("label":"age")") != std::string::npos,
                "new receiver produced no record field completions: " + result);
            assertTrue(
                responseForId(result, 4).find("New") != std::string::npos &&
                    responseForId(result, 4).find("User") != std::string::npos,
                "New hover omitted its functional-update type: " + result);
            assertTrue(
                responseForId(result, 5).find("this : User") != std::string::npos,
                "implicit this hover omitted its make receiver type: " + result);
            assertTrue(
                responseForId(result, 6).find("User") != std::string::npos,
                "This hover omitted its concrete make type: " + result);
            assertTrue(
                responseForId(result, 7).find(R"("label":"age")") != std::string::npos,
                "this receiver produced no record field completions: " + result);
            assertTrue(
                responseForId(result, 8).find(R"("label":"age")") != std::string::npos &&
                    responseForId(result, 8).find(R"("label":"assert")") == std::string::npos,
                "New record completion was not restricted to writable fields: " + result);
            assertTrue(
                responseForId(result, 9).find(R"("label":"age")") != std::string::npos &&
                    responseForId(result, 9).find(R"("label":"assert")") == std::string::npos,
                "incomplete new record completion exposed global symbols: " + result);
            assertTrue(
                responseForId(result, 10).find(R"("label":"age")") != std::string::npos &&
                    responseForId(result, 10).find(R"("label":"assert")") == std::string::npos,
                "@ field completion did not use the receiver record: " + result);
            assertTrue(
                result.find("undefined variable: new") == std::string::npos,
                "LSP diagnosed the implicit new binding as undefined");
        });
        it("understands implicit serving bindings and server slots", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex","languageId":"kex","version":1,"text":"record Counter do\n  count : Integer = 0\nend\nserving Counter do\n  slot reset(to: Integer) -> Void = New { count: to }\n  foul slot later(value: Integer) -> Reply<Integer> do\n    new.count = value\n    from.reply(new.count)\n    { new }\n  end\nend\nmain do\n  let counter = Process.spawn(Counter {})\n  counter.later(1)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex"},"position":{"line":6,"character":5}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex"},"position":{"line":7,"character":6}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex"},"position":{"line":5,"character":14}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":5,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex"},"position":{"line":13,"character":11}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":10,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex"},"position":{"line":5,"character":39}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex","version":2},"contentChanges":[{"text":"record Counter do\n  count : Integer = 0\nend\nserving Counter do\n  slot reset(to: Integer) -> Void = New { count: to }\n  foul slot later(value: Integer) -> Reply<Integer> do\n    new.count = value\n    from.reply(new.count)\n    { new }\n  end\nend\nmain do\n  let counter = Process.spawn(Counter {})\n  counter.\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":6,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex"},"position":{"line":13,"character":10}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex","version":3},"contentChanges":[{"text":"record Counter do\n  count : Integer = 0\nend\nserving Counter do\n  slot increment(by: Integer) -> Reply<Integer> do\n    return { new, }\n  end\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":7,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex"},"position":{"line":5,"character":18}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex","version":4},"contentChanges":[{"text":"record Counter do\n  count : Integer = 0\nend\nserving Counter do\n  slot increment(by: Integer) -> Reply<Integer> do\n    return { }\n  end\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":8,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-serving.kex"},"position":{"line":5,"character":13}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":11,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                responseForId(result, 2).find("new : Counter") != std::string::npos,
                "serving new hover omitted its state type: " + result);
            assertTrue(
                responseForId(result, 3).find("From<Integer>") != std::string::npos,
                "serving from hover omitted its reply type: " + result);
            assertTrue(
                responseForId(result, 4).find("foul slot later") != std::string::npos &&
                    responseForId(result, 4).find("Reply<Integer>") != std::string::npos,
                "slot declaration hover omitted its signature: " + result);
            assertTrue(
                responseForId(result, 5).find("later") != std::string::npos &&
                    responseForId(result, 5).find("CallError") != std::string::npos,
                "remote slot hover omitted its Result/CallError projection: " + result);
            assertTrue(
                responseForId(result, 10).find("record Reply<Integer> do") != std::string::npos &&
                    responseForId(result, 10).find("reply : Integer") != std::string::npos &&
                    responseForId(result, 10).find("new : Counter?") != std::string::npos &&
                    responseForId(result, 10).find("stop : ProcessExitReason?") !=
                        std::string::npos &&
                    responseForId(result, 10).find("required") != std::string::npos,
                "Reply hover did not expose its transition contract: " + result);
            assertTrue(
                responseForId(result, 6).find(R"("filterText":"later")") != std::string::npos &&
                    responseForId(result, 6).find(R"("label":"foul slot later")") !=
                        std::string::npos &&
                    responseForId(result, 6).find(R"("label":"slot reset")") != std::string::npos,
                "Server<Counter> produced no slot completion: " + result);
            assertTrue(
                responseForId(result, 7).find(R"("label":"reply")") != std::string::npos &&
                    responseForId(result, 7).find(R"("label":"stop")") != std::string::npos &&
                    responseForId(result, 7).find(R"("label":"assert")") == std::string::npos,
                "serving transition completion exposed global symbols: " + result);
            assertTrue(
                responseForId(result, 8).find(R"("label":"new")") != std::string::npos &&
                    responseForId(result, 8).find(R"("label":"reply")") != std::string::npos &&
                    responseForId(result, 8).find(R"("label":"stop")") != std::string::npos &&
                    responseForId(result, 8).find(R"("label":"assert")") == std::string::npos,
                "empty serving transition exposed global symbols: " + result);
            assertTrue(
                result.find("undefined variable: new") == std::string::npos &&
                    result.find("undefined variable: from") == std::string::npos,
                "LSP diagnosed an implicit serving binding as undefined: " + result);
        });
        it("hovers and completes typed process handles", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-process.kex","languageId":"kex","version":1,"text":"type Message = :hello\nfoul worker -> Process<Message> do\n  spawn do true end\nend\nmain do\n  let process = worker()\n  process.send(:hello)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-process.kex"},"position":{"line":5,"character":7}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-process.kex"},"position":{"line":6,"character":11}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-process.kex","version":2},"contentChanges":[{"text":"type Message = :hello\nfoul worker -> Process<Message> do\n  spawn do true end\nend\nmain do\n  let process = worker()\n  process.\nend\n"}]}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-process.kex"},"position":{"line":6,"character":10}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":5,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                responseForId(result, 2).find("process : Process<:hello>") != std::string::npos,
                "typed process binding hover omitted its message type: " + result);
            assertTrue(
                responseForId(result, 3).find("send") != std::string::npos &&
                    responseForId(result, 3).find("Process<A>") != std::string::npos,
                "typed process method hover omitted its signature: " + result);
            assertTrue(
                responseForId(result, 4).find(R"("filterText":"send")") != std::string::npos &&
                    responseForId(result, 4).find(R"("filterText":"sendFrom")") !=
                        std::string::npos,
                "Process<Message> produced no message-method completions: " + result);
        });
        it("provides signature help for methods and serving slots", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-signature.kex","languageId":"kex","version":1,"text":"record Counter do\n  count : Integer = 0\nend\nserving Counter do\n  slot reset(to: Integer) -> Void = New { count: to }\nend\nmain do\n  let hello = \"hello\"\n  hello.at(0)\n  let counter = Process.spawn(Counter {})\n  counter.reset(1)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-signature.kex"},"position":{"line":8,"character":11}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/signatureHelp","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-signature.kex"},"position":{"line":10,"character":16}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                responseForId(result, 2).find("at(") != std::string::npos &&
                    responseForId(result, 2).find("Integer") != std::string::npos,
                "ordinary method signature help was empty: " + result);
            assertTrue(
                responseForId(result, 3).find("reset(to: Integer") != std::string::npos &&
                    responseForId(result, 3).find("within: Integer?") != std::string::npos,
                "serving slot signature help omitted parameters/options: " + result);
        });
        // A function called on a receiver is the receiver type's, not the
        // first function of that name: `t.text` reached the top-level `text`,
        // and from a dependency nothing at all (kexhq/kex#541).
        it("navigates a block parameter's function to its type's make block", []() {
            const std::string text =
                R"(record TableBuilder do\n  name : String\nend\n\nmake TableBuilder do\n  let text(column: Atom) -> String = \"${@name}.${column}\"\nend\n\nlet text(value: String) -> String = value\n\nlet change(name: Atom, from: Block<TableBuilder, String>) -> String = from(TableBuilder { name: \"t\" })\n\nmain do\n  IO.printLine(change(:c, from: { |t| t.text(:kind) }))\n  IO.printLine(change(:c) { |t| t.text(:kind) })\nend\n)";
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-block-receiver.kex","languageId":"kex","version":1,"text":")" +
                text + R"("}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-block-receiver.kex"},"position":{"line":13,"character":41}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-block-receiver.kex"},"position":{"line":14,"character":36}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            const std::string makeText =
                R"("range":{"end":{"character":10,"line":5},"start":{"character":6,"line":5}})";
            for (const int id : {2, 3}) {
                assertTrue(
                    responseForId(result, id).find(makeText) != std::string::npos,
                    "t.text did not reach TableBuilder's text: " + responseForId(result, id));
            }
        });
        it("keeps local navigation and pattern receiver completion semantic", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
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
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find(R"("start":{"character":6,"line":1})") != std::string::npos &&
                    result.find(R"("uri":"file:///tmp/kex-lsp-pattern-dot.kex")") !=
                        std::string::npos,
                "local navigation fell through to an imported symbol");
            assertTrue(
                result.find(R"("label":"count")") != std::string::npos,
                "member completion lost the last valid local type");
            assertTrue(
                result.find(R"("id":5)") != std::string::npos &&
                    result.find(R"("label":"map")") != std::string::npos,
                "a parameter dot in the initially opened buffer had no members");
            assertTrue(
                result.find(R"("start":{"character":9,"line":0})") != std::string::npos &&
                    result.find(R"("uri":"file:///tmp/kex-lsp-parameter-dot.kex")") !=
                        std::string::npos,
                "parameter navigation did not resolve to its declaration");
        });
        it("finds semantic and shadow-safe local references", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-references.kex","languageId":"kex","version":1,"text":"let double(value: Integer) = value + value\nmain do\n  let value = 2\n  IO.inspect(value)\n  IO.inspect(double(value))\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-references.kex"},"position":{"line":3,"character":14},"context":{"includeDeclaration":true}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-references.kex"},"position":{"line":4,"character":14},"context":{"includeDeclaration":true}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-references.kex"},"position":{"line":4,"character":14},"context":{"includeDeclaration":false}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":5,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find(R"("referencesProvider":true)") != std::string::npos,
                "server did not advertise find-references support");
            const auto local = responseForId(result, 2);
            assertEqual(
                occurrences(local, R"("uri":)"), size_t{3},
                "local references crossed into the shadowed parameter");
            assertTrue(
                local.find(R"("start":{"character":6,"line":2})") != std::string::npos,
                "local references omitted the declaration");
            const auto globalWithDeclaration = responseForId(result, 3);
            assertEqual(
                occurrences(globalWithDeclaration, R"("uri":)"), size_t{2},
                "function references omitted its call or declaration");
            const auto globalWithoutDeclaration = responseForId(result, 4);
            assertEqual(
                occurrences(globalWithoutDeclaration, R"("uri":)"), size_t{1},
                "includeDeclaration=false still returned the definition");
        });
        it("keeps receiver overload references separate", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-receiver-references.kex","languageId":"kex","version":1,"text":"main do\n  let first: String? = None\n  let second: String? = None\n  first.or(\"\")\n  second.or(\"\")\n  Bits.or(1, 2)\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-receiver-references.kex"},"position":{"line":3,"character":9},"context":{"includeDeclaration":false}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto references = responseForId(output.str(), 2);
            assertEqual(
                occurrences(references, R"("uri":)"), size_t{2},
                "Optional.or references included Bits.or");
            assertTrue(
                references.find(R"("line":5)") == std::string::npos,
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
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-reference-workspace/tools.kex","languageId":"kex","version":1,"text":"module Tools do\n  let double(value: Integer) = value * 2\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/references","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-reference-workspace/tools.kex"},"position":{"line":1,"character":7},"context":{"includeDeclaration":true}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto references = responseForId(output.str(), 2);
            assertEqual(
                occurrences(references, R"("uri":)"), size_t{2},
                "unopened workspace reference was not indexed cleanly");
            assertTrue(
                references.find("z_consumer.kex") != std::string::npos,
                "find references omitted the unopened consumer file");
            fs::remove(root / "tools.kex");
            fs::remove(root / "z_consumer.kex");
            fs::remove(root);
        });
        // The editor checks the way `kex -C` does, and it too built its
        // checker from the prelude interface alone: a record declared in a
        // sibling module was an unknown name, so a union declared alongside
        // it refused its own members and the editor reported errors no other
        // mode saw. Member completion on such a receiver offered nothing for
        // the same reason — the type is qualified across a file boundary
        // (`Shapes.Circle`) while the `make` target is written bare.
        it("checks and completes against a sibling module's declarations", []() {
            namespace fs = std::filesystem;
            const fs::path root = "/tmp/kex-lsp-sibling-module";
            fs::remove_all(root);
            fs::create_directories(root / "lib");
            {
                std::ofstream library(root / "lib" / "shapes.kex");
                library << "module Shapes\n"
                           "\n"
                           "record Circle do\n"
                           "  radius : Float\n"
                           "end\n"
                           "\n"
                           "module Kinds do\n"
                           "  record Ellipse do\n"
                           "    major : Float\n"
                           "  end\n"
                           "end\n"
                           "\n"
                           "type Shape = Shapes.Circle | Shapes.Kinds.Ellipse\n"
                           "\n"
                           "make Circle do\n"
                           "  let area() -> Float = 3.0 * this.radius\n"
                           "end\n";
            }
            // Newlines stay ESCAPED here: this text is embedded in the
            // didOpen JSON, not written to disk.
            const char* entry = "using Shapes\\n"
                                "let rounder(radius: Float) -> Shapes.Shape do\\n"
                                "  Kinds.Ellipse { major: radius }\\n"
                                "end\\n"
                                "main do\\n"
                                "  let c = Circle { radius: 2.0 }\\n"
                                "  IO.printLine(c.area().to(String))\\n"
                                "end\\n";
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///tmp/kex-lsp-sibling-module","capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                std::string(
                    R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-sibling-module/main.kex","languageId":"kex","version":1,"text":")") +
                entry + R"("}}})");
            // Just past the `.` of `c.area()`.
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-sibling-module/main.kex"},"position":{"line":6,"character":17}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto text = output.str();
            // A union member declared one module deeper is still a member.
            assertTrue(
                text.find("declared to return") == std::string::npos,
                "sibling module's union refused its own member: " + text.substr(0, 2000));
            const auto completion = responseForId(text, 2);
            assertTrue(
                completion.find(R"("label":"area")") != std::string::npos,
                "member completion missed the sibling module's method: " + completion);
            fs::remove_all(root);
        });
        // A sibling module's `compiled do` declarations exist for the editor
        // too: Metro.Html's element constructors were "has no function" in
        // every file that called them, while the same files built.
        it("sees declarations a sibling module's compiled block generates", []() {
            namespace fs = std::filesystem;
            const fs::path root = "/tmp/kex-lsp-sibling-compiled";
            fs::remove_all(root);
            fs::create_directories(root / "lib");
            {
                std::ofstream library(root / "lib" / "gen.kex");
                library << "module Gen do\n"
                           "  let NAMES = [\"em\"]\n"
                           "  compiled do\n"
                           "    NAMES.each do |name|\n"
                           "      let %{name}(text: String) -> String = text\n"
                           "    end\n"
                           "  end\n"
                           "end\n";
            }
            const char* entry = "using Gen\\n"
                                "main do\\n"
                                "  IO.printLine(Gen.em(\\\"x\\\"))\\n"
                                "end\\n";
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///tmp/kex-lsp-sibling-compiled","capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                std::string(
                    R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-sibling-compiled/main.kex","languageId":"kex","version":1,"text":")") +
                entry + R"("}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto text = output.str();
            assertTrue(
                text.find("publishDiagnostics") != std::string::npos,
                "no diagnostics were published: " + text.substr(0, 2000));
            assertTrue(
                text.find("has no function") == std::string::npos,
                "a generated function was missing: " + text.substr(0, 2000));
            fs::remove_all(root);
        });
        // A package can keep its modules in folders at its top and say which:
        // then its own directory is the root, as it is for `tey build`.
        it("knows a tey package that declares its source folders", []() {
            namespace fs = std::filesystem;
            const fs::path root = "/tmp/kex-lsp-tey-sources";
            fs::remove_all(root);
            fs::create_directories(root / "declared");
            fs::create_directories(root / "usual");
            {
                std::ofstream declared(root / "declared" / "package.kex");
                declared << "package \"chat\" do\n  entrypoint(\"main.kex\")\n"
                            "  sources([\"app\", \"config\", \"db\"])\nend\n";
                std::ofstream usual(root / "usual" / "package.kex");
                usual << "package \"lib\" do\n  # sources([\"app\"]) would move them\nend\n";
            }
            assertTrue(
                kex::lsp::teyPackageDeclaresSources((root / "declared").string()),
                "a `sources([...])` declaration was not seen");
            assertTrue(
                !kex::lsp::teyPackageDeclaresSources((root / "usual").string()),
                "a comment was taken for a `sources` declaration");
            assertTrue(
                !kex::lsp::teyPackageDeclaresSources((root / "missing").string()),
                "a directory with no manifest declared sources");
            fs::remove_all(root);
        });
        // What `tey build` sees and the editor did not: a dependency's
        // modules, reachable through the roots `tey.lock` names.
        it("resolves modules from a tey package's locked dependencies", []() {
            namespace fs = std::filesystem;
            const fs::path root = "/tmp/kex-lsp-tey-package";
            const fs::path cache = root / "cache";
            const std::string git = "https://example.invalid/greeter.git";
            const std::string commit = "0123456789abcdef0123456789abcdef01234567";
            fs::remove_all(root);
            fs::create_directories(root / "app" / "src");
            setenv("TEY_CACHE", cache.string().c_str(), 1);
            const fs::path dependencySource =
                fs::path(kex::lsp::teyCachePackagePath(git, commit)) / "src";
            fs::create_directories(dependencySource);
            {
                std::ofstream greeter(dependencySource / "greeter.kex");
                greeter << "module Greeter\n\n"
                           "let hello(name: String) -> String = \"hello \" + name\n";
                std::ofstream manifest(root / "app" / "package.kex");
                manifest << "bundle \"app\"\n";
                std::ofstream lock(root / "app" / "tey.lock");
                lock << "{\n  \"version\": 1,\n"
                        "  \"kex\": {\"requirement\": \">= 0.1.0\", \"version\": \"0.1.0\"},\n"
                        "  \"deps\": {\n    \"greeter\": {\"git\": \""
                     << git << "\", \"resolved\": \"v1.0.0\", \"commit\": \"" << commit
                     << "\", \"sha256\": \"\", \"groups\": []}\n  }\n}\n";
            }
            const std::string source = "using Greeter, only: [hello]\\nmain do\\n  "
                                       "IO.printLine(hello(\\\"world\\\"))\\nend\\n";
            const std::string uri = "file:///tmp/kex-lsp-tey-package/app/src/main.kex";
            const auto session = [&](const char* extraInitOptions) {
                std::string messages;
                messages += frame(
                    std::string(
                        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///tmp/kex-lsp-tey-package/app",)") +
                    extraInitOptions + R"("capabilities":{}}})");
                messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
                messages += frame(
                    R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
                    uri + R"(","languageId":"kex","version":1,"text":")" + source + R"("}}})");
                messages += frame(
                    R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":")" +
                    uri + R"("},"position":{"line":2,"character":16}}})");
                messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
                messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");
                std::istringstream input(messages);
                std::ostringstream output;
                assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
                return output.str();
            };

            const auto resolved = session("");
            assertTrue(
                resolved.find("module not found in source roots") == std::string::npos,
                "a locked dependency's module was still unreachable");
            assertTrue(
                resolved.find("undefined function: `hello`") == std::string::npos,
                "a locked dependency's function was still undefined");
            assertTrue(
                responseForId(resolved, 2).find("greeter.kex") != std::string::npos,
                "go-to-definition did not reach the dependency source");

            // A dependency the cache does not hold degrades to the old
            // behaviour rather than erroring — `tey install` is the answer,
            // and the resolver's own warning says so.
            fs::remove_all(cache);
            const auto missing = session("");
            assertTrue(
                missing.find("module not found in source roots") != std::string::npos,
                "an unfetched dependency did not fall back to the resolver warning");

            // The escape hatch: roots the editor names directly, for the
            // layouts no lockfile describes.
            fs::create_directories(root / "vendor" / "greeter" / "src");
            fs::rename(root / "app" / "tey.lock", root / "app" / "tey.lock.off");
            {
                std::ofstream greeter(root / "vendor" / "greeter" / "src" / "greeter.kex");
                greeter << "module Greeter\n\n"
                           "let hello(name: String) -> String = \"hello \" + name\n";
            }
            const auto configured =
                session(R"("initializationOptions":{"sourceRoots":["../vendor/greeter/src"]},)");
            assertTrue(
                configured.find("undefined function: `hello`") == std::string::npos,
                "initializationOptions sourceRoots were ignored");

            unsetenv("TEY_CACHE");
            fs::remove_all(root);
        });

        // The lockfile tey writes today (`packages`, with workspace, path and
        // git-with-subdir sources), read from the WORKSPACE root by a member
        // that has no lockfile of its own — the layout of Rodolfo's examples
        // and of any app that vendors a package by path.
        it("resolves workspace, path and git packages from the current tey.lock", [session]() {
            namespace fs = std::filesystem;
            const fs::path root = "/tmp/kex-lsp-tey-workspace";
            const fs::path cache = root / "cache";
            const std::string git = "https://example.invalid/remote.git";
            const std::string commit = "fedcba9876543210fedcba9876543210fedcba98";
            fs::remove_all(root);
            setenv("TEY_CACHE", cache.string().c_str(), 1);
            auto write = [](const fs::path& path, const std::string& text) {
                fs::create_directories(path.parent_path());
                std::ofstream(path) << text;
            };
            write(
                root / "package.kex",
                "package \"ws\" do\n  workspace do\n    members([\"apps/*\"])\n  end\nend\n");
            write(
                root / "apps" / "app" / "package.kex",
                "package \"app\" do\n  tey(\"web\", workspace: true)\n"
                "  tey(\"vend\", path: \"vendor/vend\")\nend\n");
            // A workspace package whose submodule names it back.
            write(
                root / "lib" / "web" / "src" / "web.kex",
                "module Web\n\nusing Web.Markup\n\n"
                "greet : String -> String\n"
                "let greet(name: String) -> String = \"hi \" + name\n");
            write(
                root / "lib" / "web" / "src" / "web" / "markup.kex",
                "module Web.Markup\n\nusing Web\n\n"
                "let broken -> Integer = \"not a number\"\n");
            write(
                root / "vendor" / "vend" / "src" / "vend.kex",
                "module Vend\n\nlet vended -> Integer = 1\n");
            write(
                fs::path(kex::lsp::teyCachePackagePath(git, commit)) / "pkg" / "src" / "remote.kex",
                "module Remote\n\nlet remote -> Integer = 2\n");
            // Locked from git, but overridden locally — and not in the cache.
            write(
                root / "over-local" / "src" / "over.kex",
                "module Over\n\nlet over -> Integer = 3\n");
            write(root / "package.local.kex", "local(\"over\", path: \"over-local\")\n");
            auto entry = [](const std::string& name, const std::string& source,
                            const std::string& git, const std::string& commit,
                            const std::string& subdir, const std::string& path) {
                return "\"" + name + "\": {\"version\": \"0.1.0\", \"source\": \"" + source +
                       "\", \"git\": \"" + git + "\", \"resolved\": \"\", \"commit\": \"" + commit +
                       "\", \"subdir\": \"" + subdir + "\", \"path\": \"" + path +
                       "\", \"sha256\": \"\", \"groups\": [], \"dependencies\": [], \"plugins\": "
                       "{}, \"workspace\": " +
                       (source == "workspace" ? "true" : "false") + "}";
            };
            write(
                root / "tey.lock",
                "{\n  \"version\": 1,\n  \"packages\": {\n    " +
                    entry("app", "workspace", "", "", "", "apps/app") + ",\n    " +
                    entry(
                        "over", "git", "https://example.invalid/over.git",
                        "0000000000000000000000000000000000000000", "", "") +
                    ",\n    " + entry("remote", "git", git, commit, "pkg", "") + ",\n    " +
                    entry("vend", "path", "", "", "", "vendor/vend") + ",\n    " +
                    entry("web", "workspace", "", "", "", "lib/web") + "\n  }\n}\n");

            const auto mainPath = (root / "apps" / "app" / "src" / "main.kex").string();
            const std::string mainText = "using Web\nusing Vend\nusing Remote\nusing Over\n\n"
                                         "main do\n  IO.printLine(greet(\"x\"))\n"
                                         "  IO.printLine(vended + remote + over)\nend\n";
            write(mainPath, mainText);
            const auto app = session(
                "file://" + mainPath, mainText,
                {R"({"jsonrpc":"2.0","id":2,"method":"textDocument/definition","params":{"textDocument":{"uri":"file://)" +
                 mainPath + R"("},"position":{"line":6,"character":16}}})"});
            assertTrue(
                app.find("module not found") == std::string::npos &&
                    app.find("undefined") == std::string::npos,
                "a locked package was unreachable: " + app);
            // Web.Markup's own error belongs to markup.kex, not to main.kex.
            assertTrue(
                app.find("not a number") == std::string::npos &&
                    app.find("expected Integer") == std::string::npos,
                "an imported module's diagnostic leaked into the document: " + app);
            const auto definition = responseForId(app, 2);
            assertTrue(
                definition.find("web.kex") != std::string::npos &&
                    definition.find(R"("start":{"character":0,"line":4})") != std::string::npos,
                "go-to-definition must land on greet's declaration: " + definition);

            // The package itself, open while its submodule names it back.
            const auto webPath = (root / "lib" / "web" / "src" / "web.kex").string();
            std::ifstream webFile(webPath);
            const std::string webText((std::istreambuf_iterator<char>(webFile)), {});
            const auto web = session("file://" + webPath, webText, {});
            assertTrue(
                web.find("defined twice") == std::string::npos,
                "the open module was merged into itself: " + web);

            // The member's manifest, in today's vocabulary.
            const auto manifestPath = (root / "apps" / "app" / "package.kex").string();
            const auto manifest = session(
                "file://" + manifestPath,
                "package \"app\" do\n  tey(\"web\", workspace: true)\n"
                "  tey(\"vend\", path: \"vendor/vend\")\nend\n",
                {});
            assertTrue(
                manifest.find("undefined function") == std::string::npos &&
                    manifest.find("unknown named argument") == std::string::npos,
                "the manifest vocabulary is out of date: " + manifest);

            unsetenv("TEY_CACHE");
            fs::remove_all(root);
        });

        it("resolves a module nested under its package's src", []() {
            namespace fs = std::filesystem;
            // `tey build` passes the package root's `src/` as the source
            // root, so a module file at `src/tey/commands.kex` answers to
            // `Tey.Commands`. A document BESIDE it (`src/tey/cli.kex`) used
            // to fall back to its own directory as the root, where
            // `tey/commands.kex` does not exist — every `using` line warned
            // "module not found in source roots" while the same file built.
            const fs::path root = "/tmp/kex-lsp-tey-nested";
            fs::remove_all(root);
            fs::create_directories(root / "app" / "src" / "tey");
            {
                std::ofstream manifest(root / "app" / "package.kex");
                manifest << "bundle \"app\"\n";
                std::ofstream commands(root / "app" / "src" / "tey" / "commands.kex");
                commands << "module Tey.Commands\n\n"
                            "let greet(name: String) -> String = \"hi \" + name\n";
            }
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///tmp/kex-lsp-tey-nested/app","capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            const std::string source = "using Tey.Commands, only: [greet]\\nmain do\\n  "
                                       "IO.printLine(greet(\\\"world\\\"))\\nend\\n";
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-tey-nested/app/src/tey/cli.kex","languageId":"kex","version":1,"text":")" +
                source + R"("}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");
            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("module not found in source roots") == std::string::npos,
                "a sibling module in the package's src was not found");
            assertTrue(
                result.find("undefined function: `greet`") == std::string::npos,
                "a sibling module's function was still undefined");
            fs::remove_all(root);
        });
    });
    // A describe of its own: one more test in the one above passes the size
    // at which clang-format 20 re-lays out the whole call.
    describe("Kex LSP capability requirements", []() {
        it("hints a function's requirements at the end of its head", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hints.kex","languageId":"kex","version":1,"text":"capability Clock do\n  foul now() -> Integer = 42\nend\nfoul stamp() -> Integer = Clock.now()\nlet pure(x: Integer) -> Integer = x\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/inlayHint","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-hints.kex"},"range":{"start":{"line":0,"character":0},"end":{"line":5,"character":0}}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto hints = responseForId(output.str(), 2);
            // `stamp` on line 3, after its 37 characters; `pure` needs nothing,
            // and `now` is a member of Clock itself, so needs nothing more.
            assertTrue(
                hints.find(R"("label":"requires Clock")") != std::string::npos,
                "the requirement was not hinted");
            assertTrue(
                hints.find(R"("position":{"character":37,"line":3})") != std::string::npos,
                "the hint was not at the end of the function's head");
            assertEqual(occurrences(hints, "requires"), size_t{1});
        });
        it("follows a capability through make functions and imported interfaces", []() {
            // The requirement follows the function each call reaches, as the
            // checker decided it: `stamp` is a `make` function (the old
            // syntactic graph never looked inside a `make`), `FS.File` is a
            // capability nested in a module, and `Time.now` needs
            // `Time.Clock` only as its KexI interface says.
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-caps2.kex","languageId":"kex","version":1,"text":"capability Clock do\n  foul now() -> Integer = 42\nend\nrecord Stamper do\n  prefix : String\nend\nmake Stamper do\n  foul stamp -> String = \"${@prefix} ${Clock.now()}\"\nend\nfoul viaMake -> String = Stamper { prefix: \"at\" }.stamp\nfoul viaFile -> String = FS.File.read(\"x\").or(\"\")\nfoul viaTime -> String = Time.now.to(String).or(\"\")\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-caps2.kex"},"position":{"line":9,"character":7}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-caps2.kex"},"position":{"line":10,"character":7}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-caps2.kex"},"position":{"line":11,"character":7}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":5,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("viaMake calls stamp") != std::string::npos,
                "hover did not follow the call into the make function");
            assertTrue(
                result.find("stamp uses Clock") != std::string::npos,
                "hover did not reach the capability from the make function");
            assertTrue(
                result.find("requires FS.File") != std::string::npos,
                "hover did not name the nested capability module");
            // `Time.now` needs the clock only through what it calls: a
            // compiled interface carries that, the stdlib read from source
            // does not.
            if (!testRuntimeBeamDir().empty()) {
                assertTrue(
                    result.find("requires Time.Clock") != std::string::npos,
                    "hover did not read the requirement from the imported interface");
            }
        });
    });
    describe("Kex LSP block capabilities", []() {
        // A block's function type carries what it requires (docs/typechecker-
        // hm-plan.md §8.3, decided 2026-11-24: inferred, shown only), and hover
        // on what holds it says so.
        it("shows what a block requires on its binding", []() {
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-block-caps.kex","languageId":"kex","version":1,"text":"foul run() -> Void do\n  let task = { FS.File.read(\"x\") }\n  let plain = { 1 + 1 }\n  IO.printLine(task())\n  IO.printLine(plain())\nend\n"}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-block-caps.kex"},"position":{"line":1,"character":7}}})");
            messages += frame(
                R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-block-caps.kex"},"position":{"line":2,"character":7}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto result = output.str();
            assertTrue(
                result.find("requires FS.File") != std::string::npos,
                "hover did not show what the block requires");
            assertEqual(occurrences(result, "requires"), size_t{1});
        });
    });
    describe("Kex LSP type letters", []() {
        // A letter written as a type is the declaration's own: `B` is not
        // Units.Data's byte unit, and `E` is not Math.E.
        const std::string source = "record Tagged<A, E> do\n"
                                   "  item : A\n"
                                   "  tag : E\n"
                                   "end\n"
                                   "\n"
                                   "make Tagged<_, E> do\n"
                                   "  let retag(f) -> Tagged<B, E> = Tagged { item: f(@item), tag: "
                                   "@tag }\n"
                                   "end\n";
        it("hovers a function's own letter as a type parameter", [source]() {
            const auto reply = askAt("file:///tmp/kex-lsp-letter.kex", source, "hover", 6, 25);
            assertTrue(reply.find("DataUnit") == std::string::npos, "B: " + reply);
            assertTrue(reply.find("type parameter") != std::string::npos, "B: " + reply);
        });
        it("hovers a make block's letter as a type parameter", [source]() {
            const auto reply = askAt("file:///tmp/kex-lsp-letter.kex", source, "hover", 6, 28);
            assertTrue(reply.find("logarithm") == std::string::npos, "E: " + reply);
            assertTrue(reply.find("type parameter") != std::string::npos, "E: " + reply);
            const auto declared = askAt("file:///tmp/kex-lsp-letter.kex", source, "hover", 5, 15);
            assertTrue(declared.find("logarithm") == std::string::npos, "E: " + declared);
        });
    });

    describe("Kex LSP algebra traits", []() {
        // Applicable's `with`, Monad's `into` and `wrap`, Foldable<T>'s `find`
        // and `is?`, as an editor sees them.
        const std::string source =
            "let combined(a: Integer?, b: Integer?) -> Integer? = a.with(b) { |x, y| x + y }\n"
            "let firstVowel(word: String) -> Char? = word.find { |c| c == 'e' }\n"
            "let pairs(xs: [Integer], ys: [String]) -> [(Integer, String)] do\n"
            "  xs.into { |x| ys.into { |y| [(x, y)] } }\n"
            "end\n"
            "let halves(xs: [Integer]) -> [Integer] = xs.into { |x| [x / 2] }\n"
            "let half(n: Integer?) -> Integer? = n.into { |x| Just(x / 2) }\n"
            "let discrete(v: Any) -> Bool = v.is?(Discrete)\n"
            "let bumped(M is Monad => xs: M<Integer>) -> M<Integer> = xs.into { |x| M.wrap(x + "
            "1) }\n";
        it("hovers an `into` block's parameters as the elements", [source]() {
            const auto first = askAt("file:///tmp/kex-lsp-into.kex", source, "hover", 3, 13);
            assertTrue(first.find("x : Integer") != std::string::npos, "x: " + first);
            const auto second = askAt("file:///tmp/kex-lsp-into.kex", source, "hover", 3, 27);
            assertTrue(second.find("y : String") != std::string::npos, "y: " + second);
        });
        it("hovers `into` as a function", [source]() {
            const auto reply = askAt("file:///tmp/kex-lsp-into-fn.kex", source, "hover", 3, 6);
            assertTrue(reply.find("into : [Integer]") != std::string::npos, "into: " + reply);
            // The inner call on the same line is its own: `ys` is a [String].
            const auto inner = askAt("file:///tmp/kex-lsp-into-fn.kex", source, "hover", 3, 19);
            assertTrue(inner.find("into : [String]") != std::string::npos, "inner into: " + inner);
        });
        it("types a block through a bounded constructor letter", [source]() {
            // `xs: M<Integer>`: the requirement's `F<A>` makes the block's x an
            // Integer, not whatever `x + 1` alone would.
            const auto reply = askAt("file:///tmp/kex-lsp-monad-block.kex", source, "hover", 8, 79);
            assertTrue(reply.find("x : Integer") != std::string::npos, "x: " + reply);
        });
        it("hovers `with` as the overload the call selected", [source]() {
            const auto reply = askAt("file:///tmp/kex-lsp-with.kex", source, "hover", 0, 56);
            assertTrue(
                reply.find(
                    "with : Integer? -> Integer? -> (Integer -> Integer -> Integer) -> "
                    "Integer?") != std::string::npos,
                "with: " + reply);
        });
        it("hovers a String's `find` as answering a Char", [source]() {
            const auto reply = askAt("file:///tmp/kex-lsp-find.kex", source, "hover", 1, 46);
            assertTrue(reply.find("-> Char?") != std::string::npos, "find: " + reply);
        });
        it("completes `with`, `wrap`, `into` and `is?` on a list", [source]() {
            const auto reply =
                askAt("file:///tmp/kex-lsp-complete.kex", source, "completion", 3, 5);
            for (const auto* label : {"with", "wrap", "into", "is?"}) {
                assertTrue(
                    reply.find(std::string("\"label\":\"") + label + "\"") != std::string::npos,
                    std::string("no ") + label + ": " + reply);
            }
        });
    });
    describe("Kex LSP bounds", []() {
        // A bound's trait (#651) hovers as the trait the checker resolved,
        // as a name after `implement:` does: here not the record `Pricing`.
        const std::string source =
            "module Shop do\n  trait Pricing do\n    price :> Integer\n  end\nend\n\n"
            "record Pricing do\n  amount : Integer\nend\n\n"
            "let cost(A is Shop.Pricing => item: A) -> Integer = item.price\n\n"
            "record Box<T> do\n  item : T\nend\n\n"
            "make T is Shop.Pricing => Box<T> do\n  let total -> Integer = .price\nend\n";
        it("hovers a function bound's trait as the trait it resolved", [source]() {
            const auto reply = askAt("file:///tmp/kex-lsp-bound-fn.kex", source, "hover", 10, 20);
            assertTrue(
                reply.find("trait Shop.Pricing") != std::string::npos,
                "not the trait the bound names: " + reply);
        });
        it("hovers a make bound's trait as the trait it resolved", [source]() {
            const auto reply = askAt("file:///tmp/kex-lsp-bound-make.kex", source, "hover", 16, 16);
            assertTrue(
                reply.find("trait Shop.Pricing") != std::string::npos,
                "not the trait the bound names: " + reply);
        });
    });
    describe("Kex LSP effects", []() {
        it("hovers a function that runs its parameter as foul when it carries an effect", []() {
            // Nothing in `fire`'s signature says it runs `h`'s block (#142).
            const auto reply = askAt(
                "file:///tmp/kex-lsp-transparent.kex",
                "record Hook do\n  run : Block<Bool>\nend\n\n"
                "let fire(h: Hook) -> Bool do\n  let f = h.run\n  f()\nend\n",
                "hover", 4, 5);
            assertTrue(
                reply.find("foul when `h` carries an effect") != std::string::npos,
                "the transparent parameter was not shown: " + reply);
        });
    });
    describe("Kex LSP one main", []() {
        // A spec's base is its companion: its `main` is the program's own, not
        // a used module's, though the spec names the base's module and so
        // reaches the file again (examples/binary_tree.spec.kex).
        it("does not report a spec base's main", []() {
            namespace fs = std::filesystem;
            const fs::path root = "/tmp/kex-lsp-spec-base-main";
            fs::remove_all(root);
            fs::create_directories(root);
            {
                std::ofstream base(root / "counter.kex");
                base << "module Counter do\n"
                        "  let step(n: Integer) -> Integer = n + 1\n"
                        "end\n"
                        "\n"
                        "main do\n"
                        "  IO.printLine(Counter.step(1))\n"
                        "end\n";
            }
            const char* spec = "describe(\\\"counter\\\") do\\n"
                               "  it(\\\"counts\\\") do\\n"
                               "    assert(Counter.step(1) == 2)\\n"
                               "  end\\n"
                               "end\\n";
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///tmp/kex-lsp-spec-base-main","capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                std::string(
                    R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-spec-base-main/counter.spec.kex","languageId":"kex","version":1,"text":")") +
                spec + R"("}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto text = output.str();
            assertTrue(
                text.find("publishDiagnostics") != std::string::npos &&
                    text.find("KEXEM-0012") == std::string::npos &&
                    text.find("KEXET-") == std::string::npos,
                "a spec base's main was reported, or the base was not merged: " +
                    text.substr(0, 3000));
            fs::remove_all(root);
        });
        // A program has one `main`. A used module's own `main` is reported, as
        // the build reports it (KEXEM-0012), in the document at the `using`
        // that reached it, with the module's `main` as related information.
        it("reports a used module's main at the using", []() {
            namespace fs = std::filesystem;
            const fs::path root = "/tmp/kex-lsp-used-main";
            fs::remove_all(root);
            fs::create_directories(root / "lib");
            {
                std::ofstream library(root / "lib" / "greeter.kex");
                library << "module Greeter\n"
                           "\n"
                           "let greet(name: String) -> String = \"hi ${name}\"\n"
                           "\n"
                           "main do\n"
                           "  IO.printLine(greet(\"self\"))\n"
                           "end\n";
            }
            const char* entry = "# the entry\\n"
                                "using Greeter\\n"
                                "main do\\n"
                                "  IO.printLine(greet(\\\"kex\\\"))\\n"
                                "end\\n";
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":"file:///tmp/kex-lsp-used-main","capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                std::string(
                    R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-used-main/main.kex","languageId":"kex","version":1,"text":")") +
                entry + R"("}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto text = output.str();
            const auto at = text.find("KEXEM-0012");
            assertTrue(at != std::string::npos, "no used-module main diagnostic: " + text);
            // On the `using` line (0-based line 1) of this document, not at the
            // module's line: its own range follows its related information.
            const auto published =
                text.find(R"("uri":"file:///tmp/kex-lsp-used-main/main.kex")", at);
            const auto range = text.find(
                R"("range":{"end":{"character":5,"line":1},"start":{"character":0,"line":1}})", at);
            assertTrue(
                published != std::string::npos && range < published,
                "used-module main not at the using: " + text.substr(0, 3000));
            assertTrue(
                text.find("greeter.kex", at) != std::string::npos &&
                    text.find("its `main` is here", at) != std::string::npos,
                "no related information at the module's main: " + text.substr(0, 3000));
            fs::remove_all(root);
        });
        // A second `main` in the document, and a statement beside one, are the
        // parser's errors, published as the build prints them.
        it("reports a second main and a statement beside main", []() {
            const char* entry = "IO.printLine(\\\"before\\\")\\n"
                                "main do\\n"
                                "  IO.printLine(\\\"one\\\")\\n"
                                "end\\n"
                                "main do\\n"
                                "  IO.printLine(\\\"two\\\")\\n"
                                "end\\n";
            std::string messages;
            messages += frame(
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}})");
            messages += frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
            messages += frame(
                std::string(
                    R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/kex-lsp-two-mains.kex","languageId":"kex","version":1,"text":")") +
                entry + R"("}}})");
            messages += frame(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
            messages += frame(R"({"jsonrpc":"2.0","method":"exit"})");

            std::istringstream input(messages);
            std::ostringstream output;
            assertEqual(kex::lsp::run(input, output, testRuntimeBeamDir()), 0);
            const auto text = output.str();
            assertTrue(
                text.find("KEXEP-0163") != std::string::npos,
                "no second-main diagnostic: " + text.substr(0, 3000));
            assertTrue(
                text.find("KEXEP-0164") != std::string::npos,
                "no statement-beside-main diagnostic: " + text.substr(0, 3000));
        });
    });

    describe("Kex LSP snippets", []() {
        const std::string snippets =
            R"({"textDocument":{"completion":{"completionItem":{"snippetSupport":true,"insertTextModeSupport":{"valueSet":[1,2]}}}}})";
        const std::string uri = "file:///tmp/kex-lsp-snippets.kex";
        const auto offers = [](const std::string& reply, const std::string& label) {
            return reply.find("\"label\":\"" + label + "\"") != std::string::npos;
        };
        const auto anySnippet = [](const std::string& reply) {
            return reply.find("\"insertTextFormat\":2") != std::string::npos;
        };

        it("sends no snippets to a client that cannot expand them", [=]() {
            const std::string text = "let f(x: Integer) -> Integer do\n  i\n  x\nend\n";
            const auto reply = askAt(uri, text, "completion", 1, 3);
            assertTrue(!anySnippet(reply), "a snippet went to a plain-text client: " + reply);
        });

        it("offers control structures in a function body, indented for its line", [=]() {
            const std::string text = "let f(x: Integer) -> Integer do\n  i\n  x\nend\n";
            const auto reply = askAt(uri, text, "completion", 1, 3, snippets);
            assertTrue(
                reply.find(R"("insertText":"if ${1:condition}\n\t$0\nend")") != std::string::npos,
                "no `if` snippet in a function body: " + reply);
            assertTrue(offers(reply, "if else"), "no `if else` snippet: " + reply);
            assertTrue(!offers(reply, "main"), "`main` offered in a function body: " + reply);
        });

        it("offers declarations at the top level and not statements", [=]() {
            const std::string text = "re\n";
            const auto reply = askAt(uri, text, "completion", 0, 2, snippets);
            assertTrue(offers(reply, "record"), "no `record` snippet at the top level: " + reply);
            assertTrue(!offers(reply, "receive"), "`receive` offered at the top level: " + reply);
            const auto main = askAt(uri, "ma\n", "completion", 0, 2, snippets);
            assertTrue(offers(main, "main"), "no `main` snippet at the top level: " + main);
            assertTrue(offers(main, "make implement"), "no `make implement` snippet: " + main);
            assertTrue(!offers(main, "match"), "`match` offered at the top level: " + main);
        });

        it("offers only function declarations inside make", [=]() {
            const std::string text =
                "record Box do\n  size: Integer\nend\n\nmake Box do\n  l\nend\n";
            const auto reply = askAt(uri, text, "completion", 5, 3, snippets);
            assertTrue(offers(reply, "let"), "no `let` snippet inside make: " + reply);
            assertTrue(!offers(reply, "loop"), "`loop` offered inside make: " + reply);
        });

        it("offers main, not match, once a function with a trailing if has ended", [=]() {
            const std::string text = "let f(x: Integer) -> Integer do\n"
                                     "  return 0 if x < 0\n"
                                     "  if x > 10 then 10 else x end\n"
                                     "end\n"
                                     "ma\n";
            const auto reply = askAt(uri, text, "completion", 4, 2, snippets);
            assertTrue(offers(reply, "main"), "no `main` after the function ended: " + reply);
            assertTrue(!offers(reply, "match"), "`match` offered at the top level: " + reply);
        });

        it("offers value constructs after `=`", [=]() {
            const std::string text = "main do\n  let y = ma\nend\n";
            const auto reply = askAt(uri, text, "completion", 1, 12, snippets);
            assertTrue(offers(reply, "match"), "no `match` snippet after `=`: " + reply);
        });

        it("offers a do block after a call", [=]() {
            const std::string text = "main do\n  [1, 2].each d\nend\n";
            const auto reply = askAt(uri, text, "completion", 1, 15, snippets);
            assertTrue(
                offers(reply, "do |i|"),
                "no `do |i|` snippet after a call: " + snippetItems(reply));
            assertTrue(
                reply.find(R"("insertText":"do\n\t$0\nend")") != std::string::npos,
                "the `do` snippet is not indented for its line: " + snippetItems(reply));
        });

        it("names a block's parameters for what the call passes it", [=]() {
            const auto strings = askAt(
                uri, "main do\n  [\"a\", \"b\"].each d\nend\n", "completion", 1, 19, snippets);
            assertTrue(
                strings.find(R"("insertText":"do |${1:s}|\n\t$0\nend")") != std::string::npos,
                "no `do |s|` for each over Strings: " + snippetItems(strings));
            const auto pairs =
                askAt(uri, "main do\n  [(\"a\", 1)].each d\nend\n", "completion", 1, 19, snippets);
            assertTrue(
                pairs.find(R"("insertText":"do |${1:s}, ${2:i}|\n\t$0\nend")") != std::string::npos,
                "no `do |s, i|` for each over pairs: " + snippetItems(pairs));
        });

        it("offers nothing in a string, a comment or a record body", [=]() {
            const std::string text = "record Box do\n  i\nend\n\nmain do\n"
                                     "  IO.printLine(\"wh\")\n"
                                     "  # wh\n"
                                     "end\n";
            const auto record = askAt(uri, text, "completion", 1, 3, snippets);
            assertTrue(!anySnippet(record), "a snippet in a record body: " + record);
            const auto string = askAt(uri, text, "completion", 5, 18, snippets);
            assertTrue(!anySnippet(string), "a snippet in a string: " + string);
            const auto comment = askAt(uri, text, "completion", 6, 6, snippets);
            assertTrue(!anySnippet(comment), "a snippet in a comment: " + comment);
        });

        it("leaves the step inward to the editor's indentation setting", [=]() {
            // A tab, which the client expands as its settings say; the
            // cursor line's own indentation it adds itself.
            const std::string text = "let f(x: Integer) -> Integer do\n    x\n    wh\nend\n";
            const auto reply = askAt(uri, text, "completion", 2, 6, snippets);
            assertTrue(
                reply.find(R"("insertText":"while ${1:condition} do\n\t$0\nend")") !=
                    std::string::npos,
                "the `while` body is not one tab in: " + snippetItems(reply));
            assertTrue(
                reply.find(R"("insertTextMode")") == std::string::npos,
                "the snippet asks to be inserted as it is: " + snippetItems(reply));
        });

        it("writes the same snippet for a client that declares no insertTextMode", [=]() {
            // Neovim declares snippets but no insertTextMode.
            const std::string adjusting =
                R"({"textDocument":{"completion":{"completionItem":{"snippetSupport":true}}}})";
            const std::string text = "let f(x: Integer) -> Integer do\n  i\n  x\nend\n";
            const auto reply = askAt(uri, text, "completion", 1, 3, adjusting);
            assertTrue(
                reply.find(R"("insertText":"if ${1:condition}\n\t$0\nend")") != std::string::npos,
                "the `if` body is not relative to its line: " + snippetItems(reply));
            assertTrue(
                reply.find(R"("insertTextMode")") == std::string::npos,
                "insertTextMode sent to a client without it: " + snippetItems(reply));
        });

        it("offers elif and else level with their if", [=]() {
            const std::string text = "let f(x: Integer) -> Integer do\n"
                                     "  if x > 0\n"
                                     "    1\n"
                                     "    el\n"
                                     "  end\n"
                                     "end\n";
            const auto reply = askAt(uri, text, "completion", 3, 6, snippets);
            assertTrue(
                reply.find(R"("newText":"  elif ${1:condition}\n  \t$0")") != std::string::npos,
                "no `elif` level with its `if`: " + reply);
            assertTrue(
                reply.find(R"("newText":"  else\n  \t$0")") != std::string::npos,
                "no `else` level with its `if`: " + reply);
        });

        const std::string enter = R"(,"ch":"\n","options":{"tabSize":2,"insertSpaces":true})";

        it("writes the end of a block Enter opened", [=]() {
            const std::string text = "let f(x: Integer) -> Integer do\n"
                                     "  if x > 0\n"
                                     "  \n"
                                     "end\n";
            const auto reply = askAt(uri, text, "onTypeFormatting", 2, 2, "{}", enter);
            assertTrue(
                reply.find(
                    R"({"newText":"    ","range":{"end":{"character":2,"line":2},"start":{"character":0,"line":2}}})") !=
                    std::string::npos,
                "the new line is not indented into the `if`: " + reply);
            assertTrue(
                reply.find(
                    R"({"newText":"  end\n","range":{"end":{"character":0,"line":3},"start":{"character":0,"line":3}}})") !=
                    std::string::npos,
                "no `end` for the `if`: " + reply);
            const auto block = askAt(
                uri, "main do\n  [1, 2].each do |x|\n\nend\n", "onTypeFormatting", 2, 0, "{}",
                enter);
            assertTrue(
                block.find(R"("newText":"  end\n")") != std::string::npos,
                "no `end` for the `do` block: " + block);
        });

        it("indents the new line by the editor's tab size", [=]() {
            const std::string text = "let f(x: Integer) -> Integer do\n"
                                     "  if x > 0\n"
                                     "  \n"
                                     "end\n";
            const auto four = askAt(
                uri, text, "onTypeFormatting", 2, 2, "{}",
                R"(,"ch":"\n","options":{"tabSize":4,"insertSpaces":true})");
            assertTrue(
                four.find(R"({"newText":"      ","range")") != std::string::npos,
                "the new line is not four spaces past the `if`: " + four);
            const auto tabs = askAt(
                uri, text, "onTypeFormatting", 2, 2, "{}",
                R"(,"ch":"\n","options":{"tabSize":4,"insertSpaces":false})");
            assertTrue(
                tabs.find(R"({"newText":"  \t","range")") != std::string::npos,
                "the new line is not a tab past the `if`: " + tabs);
        });

        it("infers the file's step from block bodies, not continuation lines", [=]() {
            const std::string text = "type Day = Monday\n"
                                     "         | Tuesday\n\n"
                                     "let f(x: Integer) -> Integer do\n"
                                     "    if x > 0\n"
                                     "        x\n"
                                     "    else\n"
                                     "        0\n"
                                     "    end\n"
                                     "end\n";
            assertTrue(
                kex::lsp::indentStep(text) == "    ",
                "not four spaces: [" + kex::lsp::indentStep(text) + "]");
            assertTrue(kex::lsp::indentStep("main do\n  1\nend\n") == "  ", "not two spaces");
            assertTrue(kex::lsp::indentStep("1\n") == "  ", "no default of two spaces");
        });

        it("writes no end for a block that has one", [=]() {
            const std::string text = "let f(x: Integer) -> Integer do\n"
                                     "  if x > 0\n"
                                     "  \n"
                                     "  end\n"
                                     "end\n";
            const auto reply = askAt(uri, text, "onTypeFormatting", 2, 2, "{}", enter);
            assertTrue(
                reply.find("end") == std::string::npos ||
                    reply.find(R"("result":null)") != std::string::npos,
                "an `end` for a closed block: " + reply);
        });

        it("writes an arm per constructor after `match subject`", [=]() {
            const std::string text = "type Shape = Circle(Float) | Rect(Float, Float) | Empty\n\n"
                                     "let area(s: Shape) -> Float do\n"
                                     "  match s d\n"
                                     "end\n";
            const auto reply = askAt(uri, text, "completion", 3, 11, snippets);
            assertTrue(
                reply.find(
                    R"("insertText":"do\n\tCircle(${1:_}) => $2\n\tRect(${3:_}, ${4:_}) => $5\n\tEmpty => $0\nend")") !=
                    std::string::npos,
                "no `do` with every arm after `match s`: " + reply);
        });

        it("offers the arms a match still owes inside its block", [=]() {
            const std::string text = "type Shape = Circle(Float) | Rect(Float, Float) | Empty\n\n"
                                     "let area(s: Shape) -> Float do\n"
                                     "  match s do\n"
                                     "    Circle(r) => r\n"
                                     "    R\n"
                                     "  end\n"
                                     "end\n";
            const auto reply = askAt(uri, text, "completion", 5, 5, snippets);
            assertTrue(
                reply.find(R"("insertText":"Rect(${1:_}, ${2:_}) => $0")") != std::string::npos,
                "no `Rect` arm in the match: " + reply);
            assertTrue(
                reply.find(R"("insertText":"Rect(${1:_}, ${2:_}) => $3\nEmpty => $0")") !=
                    std::string::npos,
                "no arm for every missing case: " + reply);
            assertTrue(
                !offers(reply, "Circle(_) =>"), "the taken `Circle` offered again: " + reply);
        });

        it("writes a type pattern for each member of a union of types", [=]() {
            const std::string text = "record Safe do\n  html: String\nend\n\n"
                                     "type Reply = String | Safe\n\n"
                                     "let show(r: Reply) -> String do\n"
                                     "  match r d\n"
                                     "end\n";
            const auto reply = askAt(uri, text, "completion", 7, 11, snippets);
            assertTrue(
                reply.find(R"(${1:s}: String => $2\n\t${3:s}: Safe => $0)") != std::string::npos,
                "no type patterns for the union's members: " + reply);
        });

        it("offers no arms when an arm already takes everything", [=]() {
            const std::string text = "type Shape = Circle(Float) | Empty\n\n"
                                     "let area(s: Shape) -> Float do\n"
                                     "  match s do\n"
                                     "    _ => 0.0\n"
                                     "    E\n"
                                     "  end\n"
                                     "end\n";
            const auto reply = askAt(uri, text, "completion", 5, 5, snippets);
            assertTrue(!offers(reply, "Empty =>"), "an arm offered after `_`: " + reply);
        });

        it("writes the arms of a standard-library sum type", [=]() {
            const std::string text = "let show(x: Integer?) -> Integer do\n"
                                     "  match x d\n"
                                     "end\n";
            const auto reply = askAt(uri, text, "completion", 1, 11, snippets);
            assertTrue(
                reply.find(R"(Just(${1:_}) => $2\n\tNone => $0)") != std::string::npos,
                "no Just and None arms for an Optional: " + reply);
        });

        const auto actionAt = [](int line, int character) {
            const auto position = R"({"line":)" + std::to_string(line) + R"(,"character":)" +
                                  std::to_string(character) + "}";
            return R"(,"range":{"start":)" + position + R"(,"end":)" + position +
                   R"(},"context":{"diagnostics":[]})";
        };

        it("adds the arms a match is missing as a quick fix", [=]() {
            const std::string text = "type Shape = Circle(Float) | Rect(Float, Float) | Empty\n\n"
                                     "let area(s: Shape) -> Float do\n"
                                     "  match s do\n"
                                     "    Circle(r) => r\n"
                                     "  end\n"
                                     "end\n";
            for (const auto& [line, character] : {std::pair{3, 2}, std::pair{4, 8}}) {
                const auto reply = askAt(
                    uri, text, "codeAction", line, character, "{}", actionAt(line, character));
                assertTrue(
                    reply.find(
                        R"("newText":"    Rect(_, _) => die(\"Rect is not handled yet\")\n    Empty => die(\"Empty is not handled yet\")\n")") !=
                        std::string::npos,
                    "no missing arms inserted: " + reply);
                assertTrue(
                    reply.find(R"("start":{"character":0,"line":5})") != std::string::npos,
                    "the arms are not inserted before the match's `end`: " + reply);
                assertTrue(
                    reply.find(R"("title":"Add the 2 missing match arms")") != std::string::npos,
                    "the quick fix is not titled: " + reply);
            }
        });

        it("offers no quick fix for a match that takes every case", [=]() {
            const std::string text = "type Shape = Circle(Float) | Empty\n\n"
                                     "let area(s: Shape) -> Float do\n"
                                     "  match s do\n"
                                     "    Circle(r) => r\n"
                                     "    Empty => 0.0\n"
                                     "  end\n"
                                     "end\n";
            const auto reply = askAt(uri, text, "codeAction", 3, 2, "{}", actionAt(3, 2));
            assertTrue(
                reply.find("newText") == std::string::npos,
                "a quick fix for a whole match: " + reply);
        });

        it("writes the trait's required functions after `make …, implement:`", [=]() {
            const std::string text = "trait Shape do\n"
                                     "  area :> Float\n"
                                     "  perimeter :> Float\n\n"
                                     "  let describe = \"area\"\n"
                                     "end\n\n"
                                     "record Circle do\n  radius: Float\nend\n\n"
                                     "make Circle, implement: Shape d\n";
            const auto reply = askAt(uri, text, "completion", 11, 31, snippets);
            assertTrue(
                reply.find(R"("insertText":"do\n\tlet area = $1\n\tlet perimeter = $0\nend")") !=
                    std::string::npos,
                "no `do` with the required functions: " + reply);
        });

        it("writes a stub with a parameter after the header", [=]() {
            const std::string text = "trait Ranked do\n"
                                     "  rank : This -> Integer\n"
                                     "end\n\n"
                                     "record Version do\n  major: Integer\nend\n\n"
                                     "make Version, implement: Ranked d\n";
            const auto reply = askAt(uri, text, "completion", 8, 33, snippets);
            assertTrue(
                reply.find(R"(let rank(${1:other}) -> Integer do)") != std::string::npos,
                "no `rank` stub after the header: " + reply);
        });

        it("offers the functions a make block still owes its trait", [=]() {
            const std::string text = "trait Ranked do\n"
                                     "  rank : This -> Integer\n"
                                     "  weight :> Integer\n"
                                     "end\n\n"
                                     "record Version do\n  major: Integer\nend\n\n"
                                     "make Version, implement: Ranked do\n"
                                     "  let weight = @major\n"
                                     "  l\n"
                                     "end\n";
            const auto reply = askAt(uri, text, "completion", 11, 3, snippets);
            assertTrue(
                reply.find(R"("insertText":"let rank(${1:other}) -> Integer do\n\t$0\nend")") !=
                    std::string::npos,
                "no stub for the owed `rank`: " + snippetItems(reply));
            assertTrue(
                !offers(reply, "let weight -> Integer"), "the defined `weight` offered: " + reply);
        });
    });
    return runAll();
}
