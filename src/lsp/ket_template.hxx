#pragma once

#include <string>
#include <vector>

namespace kex::lsp::ket {

// One `<% ... %>` (or `<%= %>`/`<%== %>`) hole's byte range in the synthetic
// Kex source we hand to the ordinary parser/analyzer, and the byte range in
// the original `.ket` template that text came from. `<%# %>` comments and
// everything outside a tag (the template's literal text) have no segment:
// they never reach the synthetic source at all.
struct Segment {
    int synStart = 0;
    int synEnd = 0;
    int origStart = 0;
    int origEnd = 0;
};

struct Translation {
    // A Kex function whose body is the template's holes, concatenated in
    // order, wrapped in `let __ket_render(<params>) do ... end`. Parsing and
    // type-checking THIS is what gives a `.ket` file diagnostics and hover:
    // the existing pipeline never has to know it isn't looking at a real
    // `.kex` file.
    std::string source;
    // Sorted by synStart, non-overlapping.
    std::vector<Segment> segments;
};

// Translates `.ket` template source (an optional `---`-delimited frontmatter
// block, `params: [...]` among its keys, followed by the template body) into
// a synthetic Kex source plus the segment map back to the original bytes.
// See kexhq/kex#317: this is the "server treats a .ket file as a template it
// lowers itself" half of that issue — a scanner mirroring
// `src/stdlib/template.kex`'s `Template.scan`, since that scanner runs at
// user-program runtime/compile-time and the C++ LSP server has no way to
// invoke it directly.
auto translate(const std::string& source) -> Translation;

// Maps a byte offset in the synthetic source back to the original template,
// or -1 when it falls in synthetic-only scaffolding (the generated function
// signature, the wrapping parens around an interpolation, `do`/`end`) with
// no corresponding template text.
auto mapOffset(const std::vector<Segment>& segments, int synOffset) -> int;

}  // namespace kex::lsp::ket
