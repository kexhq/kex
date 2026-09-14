# `.ket` editor support, standalone tag trimming, and a 10× faster BEAM REPL

Three pieces of work that started from one report — "`.ket` files have no
syntax highlighting" — and ended somewhere else entirely.

## `.ket` templates get their host language (kexhq/kex#314)

`syntaxes/ket.tmLanguage.json` scoped only the `<% %>` delimiters and the Kex
inside them, leaving everything outside a tag deliberately unscoped: the host
language is knowable only from the extension in front of `.ket`, and the
grammar refused to guess. In practice that made a real template look
unhighlighted — `examples/templates/profile.html.ket` produced exactly four
colored spans, because the HTML around the tags had no scopes and
`name`/`bio`/`badge` are bare identifiers `source.kex` does not color.

Two host grammars now do what `text.html.erb` does: `text.html.ket` for
`*.html.ket` over `text.html.basic`, `text.markdown.ket` for `*.md.ket` over
`text.html.markdown`. VS Code prefers the longest matching extension, so bare
`.ket` still falls back to today's plain grammar for every other host.

The tag rules are an **`L:` injection**, not a leading entry under `patterns`.
Pattern order only settles ties at the same position: the first attempt used
ordering and `# <%= name %>` lost its tag to the Markdown heading rule, which
owns the line from its `#`. The same would happen to `<a href="<%= url %>">`
inside the HTML attribute rule. An injection outranks host rules at every
position; both cases are pinned by tests.

Also here: a `ket-language-configuration.json` (comment toggle `<%#`/`%>`,
`<%`/`%>` as a bracket pair) for all three ket languages, and 12 unit tests
that tokenize against a stub host, so they do not depend on VS Code's own
grammars being on disk.

## A control tag alone on its line trims itself (kexhq/kex#315)

`Template.scan` only trimmed whitespace when a tag asked for it, so every
control tag standing alone on its line had to be written `<%- ... -%>`:

```
<%- if library -%>
A Kex library.
<%- end -%>
```

The dashes carried no intent — nobody wants the blank lines — and forgetting
one was a silent output bug. Now a tag that emits nothing (`<% %>` control,
`<%# %>` comment) and has only whitespace between the line's start and itself,
and between itself and the newline, takes that line with it. This is what
Jinja's `trim_blocks`+`lstrip_blocks`, Liquid, and Handlebars standalone
helpers all do by default.

`<%= %>` and `<%== %>` are untouched: they produce output, so their
surroundings are content. `<%- -%>` keeps working and stays meaningful for a
tag that is NOT alone on its line, where what to trim is a judgement call.

Breaking, deliberately: `spec/compiled_template_comment_and_trim.expected`
loses the blank first line its `<%# %>` used to leave behind. 11 new scan
specs cover the standalone rule — both hosts of the boundary (first line, last
line), CRLF, stacked tag lines, two tags sharing a line, and the interpolation
that must never trim. The module docs grow a Whitespace section, and
`examples/templates/readme.md.ket` drops the dashes it no longer needs.

## The BEAM REPL stops forking erlc per line (kexhq/kex#318)

Every expression line forked a fresh `erlc +from_core`. A 4-second `sample` of
the REPL working through expression lines put **3270 of 3525 samples (93%) in
`runShellCommand` → `__wait4`** — blocked on the child. Lowering and emitting
were 52 samples. `erlc -v` alone costs 90 ms: a whole BEAM starting up to
compile one small module.

The REPL already keeps a persistent VM alive for hot-loading, and that is the
VM which runs the code anyway, so the fork bought nothing. A new `compile`
driver command does `compile:file/2` with `from_core, binary` and
`code:load_binary` in one round trip — no `erlc`, no intermediate `.beam`.
`return_errors`/`return_warnings` are what make it usable: without them the
compiler prints diagnostics to the same stdout the C++ side reads program
output from, and a warning would arrive in the middle of a value.

| lines | before | after |
|-------|--------|-------|
| 1     | 0.45s  | 0.38s |
| 10    | 1.86s  | 0.51s |
| 30    | 5.09s  | 0.86s |

Per line: **160 ms → ~15 ms**. The tree-walk REPL is 0.26s for the same 30
lines, so the gap is nearly closed.

The batch compile path had the same shape per module: `tey test` and
`make spec` spawned one `erlc` per `.core` file, serially. They now go to a
single invocation, with KexI-chunk attachment moved to its own pass. A
three-module spec file: 1.30s → 0.81s. Single-module files are unchanged —
they only ever paid for one spawn.

Not a regression, for the record: `git log -L` shows that line has been `erlc`
since the BEAM REPL was written. It is an architectural cost that becomes
obvious the moment the REPL is used for real work.

## Verification

`make test-all`: 401 kex specs passing, 0 failing; 21/21 ctest suites,
`repl_cli_test` included. The VS Code extension's own suite is 92 passing.
The REPL was also exercised by hand — bindings, function definitions, blocks,
destructuring `let`, `/load` of a companion module (the reverse-order
compile-and-load), stdlib `using`, and a runtime error.

## Left for later

- **No LSP inside `.ket` holes (kexhq/kex#317).** The client attaches to
  `language: 'kex'` only, so a template's `<% %>` gets highlighting and nothing
  else. Filed with a breakdown of what it needs rather than started here.
- **docgen fences every verbatim block as ```kex** (`tey/src/tey/docgen/rdoc.kex`),
  so the template snippets in the new module docs render with Kex highlighting
  — `## Dependencies` inside an example comes out as a comment. Structurally
  fine, just mis-colored; fixing it means letting a doc comment declare a
  fence language.
- **Vim has no host embedding.** `editors/vim/syntax/ket.vim` still highlights
  tags only; matching #314 there needs its own ftdetect work.
