#include "test.hxx"
#include "../src/lsp/ket_template.hxx"

using namespace kex::lsp::ket;
using namespace test;

// Extracts the substring a segment covers in `source` (its synthetic half)
// or `original` (its template half), for readable assertions.
auto synText(const Translation& t, size_t index) -> std::string {
    const auto& seg = t.segments.at(index);
    return t.source.substr(seg.synStart, seg.synEnd - seg.synStart);
}

auto origText(const std::string& original, const Translation& t, size_t index)
    -> std::string {
    const auto& seg = t.segments.at(index);
    return original.substr(seg.origStart, seg.origEnd - seg.origStart);
}

int main() {
    describe("ket::translate — no frontmatter", []() {
        it("wraps a bodyless template in an empty render function", []() {
            auto t = translate("Hi there");
            assertTrue(t.source.find("let __ket_render() do") !=
                       std::string::npos, t.source);
            assertTrue(t.segments.empty());
        });

        it("extracts an interpolation hole as a parenthesized expression", []() {
            const std::string source = "Hi <%= name %>!";
            auto t = translate(source);
            assertEqual(t.segments.size(), size_t(1));
            assertEqual(synText(t, 0), std::string(" name "));
            assertEqual(origText(source, t, 0), std::string(" name "));
        });

        it("extracts a raw interpolation hole the same as a plain one", []() {
            auto t = translate("<%== rawHtml %>");
            assertEqual(t.segments.size(), size_t(1));
            assertEqual(synText(t, 0), std::string(" rawHtml "));
        });

        it("drops comment holes entirely", []() {
            auto t = translate("<%# not checked %>after");
            assertTrue(t.segments.empty());
            assertTrue(t.source.find("not checked") == std::string::npos);
        });

        it("keeps a <%% escape out of the synthetic source", []() {
            auto t = translate("literal <%% not a tag");
            assertTrue(t.segments.empty());
            assertTrue(t.source.find("not a tag") == std::string::npos);
        });

        it("copies a control tag's body verbatim as a statement", []() {
            const std::string source =
                "<% if admin %>\nWelcome back.\n<% end %>\n";
            auto t = translate(source);
            assertEqual(t.segments.size(), size_t(2));
            assertEqual(synText(t, 0), std::string(" if admin "));
            assertEqual(synText(t, 1), std::string(" end "));
        });

        it("honors -%> as a closer without treating a bare - as one", []() {
            const std::string source = "<% x - y -%>rest";
            auto t = translate(source);
            assertEqual(t.segments.size(), size_t(1));
            assertEqual(synText(t, 0), std::string(" x - y "));
        });

        it("stops scanning at an unterminated tag rather than crashing", []() {
            auto t = translate("Hi <%= name");
            assertTrue(t.segments.empty());
            assertTrue(t.source.find("end") != std::string::npos);
        });
    });

    describe("ket::translate — frontmatter params", []() {
        it("declares typed and untyped parameters on the synthetic function", []() {
            const std::string source =
                "---\nparams: [name, library: Bool]\n---\n<%= name %>";
            auto t = translate(source);
            assertTrue(t.source.find(
                "let __ket_render(name, library: Bool) do") !=
                std::string::npos, t.source);
        });

        it("declares no parameters when params is absent", []() {
            auto t = translate("---\ntitle: Release notes\n---\nHi");
            assertTrue(t.source.find("let __ket_render() do") !=
                       std::string::npos, t.source);
        });

        it("keeps a bracketed type's own comma out of the split", []() {
            const std::string source =
                "---\nparams: [pairs: Map<String, Integer>, other]\n---\n";
            auto t = translate(source);
            assertTrue(t.source.find(
                "let __ket_render(pairs: Map<String, Integer>, other) do") !=
                std::string::npos, t.source);
        });

        it("maps hole positions past the frontmatter block correctly", []() {
            const std::string source =
                "---\nparams: [name]\n---\n<%= name %>";
            auto t = translate(source);
            assertEqual(t.segments.size(), size_t(1));
            assertEqual(origText(source, t, 0), std::string(" name "));
        });
    });

    describe("ket::mapOffset", []() {
        it("resolves an offset inside a hole back to the original byte range", []() {
            const std::string source = "Hi <%= name %>!";
            auto t = translate(source);
            const auto& seg = t.segments.at(0);
            // Point at the 'n' of "name" inside the synthetic "(name)".
            const int synPointAtN = seg.synStart + 1;
            assertEqual(mapOffset(t.segments, synPointAtN), seg.origStart + 1);
        });

        it("returns -1 for a position in synthetic-only scaffolding", []() {
            const std::string source = "<%= name %>";
            auto t = translate(source);
            // Offset 0 sits inside "let __ket_render(...", never in a hole.
            assertEqual(mapOffset(t.segments, 0), -1);
        });

        it("resolves a segment's own exclusive end offset", []() {
            const std::string source = "<% end %>";
            auto t = translate(source);
            const auto& seg = t.segments.at(0);
            assertEqual(mapOffset(t.segments, seg.synEnd), seg.origEnd);
        });
    });

    return runAll();
}
