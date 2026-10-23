---
name: review
description: Review a Kex pull request and report concrete findings
---

Review the changes in this pull request to the Kex compiler.

Start by determining what changed:

- If `GITHUB_BASE_REF` is set, run `git diff origin/"$GITHUB_BASE_REF"...HEAD`.
- Otherwise, run `git diff HEAD`.

Then read `CLAUDE.md` at the repository root (project layout and C++ conventions) and, if any `.kex` file changed, `STYLE.md` (the rules for Kex source).

Then:

- Read the surrounding code for every changed file before judging it. A change to one backend often has a counterpart that must agree: the tree-walk interpreter (`src/interpreter/`), the IR lowering and Core Erlang emitter (`src/ir/`), the type checker (`src/semantic/`), and the stdlib sources (`src/stdlib/`). Check that the counterparts were updated when they need to be.
- Focus on correctness, edge cases, and consistency with the existing patterns in the repository.
- Hold changes to the repository's own rules, among them:
  - C++20, `.hxx`/`.cxx`, camelCase functions, `m_member` fields, trailing return types (`auto foo() -> T`), namespace `kex`.
  - No Python, JavaScript, or TypeScript files enter the repository.
  - In Kex source: a one-line block is `{ |p| ... }` and a multi-line block is `do |p| ... end`; a one-line conditional is `cond then a else b`; conversion to a string is `to(String)`, never `toString`.
- New behaviour should come with a test: `spec/*.kex` or `tests/`. Flag a behaviour change that has none.
- Skip praise and summaries that restate the diff.

Finish your response with exactly one fenced code block labelled `kex-review`, and nothing outside it. Fence it with exactly three backticks — the example below is nested in four backticks only because it sits inside this document's own fence. The response is read programmatically: every finding becomes a comment on the pull request.

````kex-review
{
  "summary": "Overall judgement in two or three sentences of Markdown.",
  "findings": [
    {
      "file": "src/ir/lower.cxx",
      "startLine": 10,
      "endLine": 12,
      "severity": "high",
      "comment": "What is wrong, why it matters, and what to do instead, in self-contained Markdown."
    }
  ]
}
````

The block is one valid JSON object. JSON strings cannot span lines, so newlines inside a `comment` are `\n`. No trailing commas. `summary` is required.

**`findings`**, most important first, every finding you can stand behind, however small — the action, not you, decides what gets posted and what the verdict is:

- `file` is the path as `git diff` reports it.
- `startLine` and `endLine` are the lines the finding applies to in the new version of the file, omitting `endLine` for a single line. Inline comments anchor to changed hunks of the diff, and the API rejects a line outside them, so prefer the closest line inside a changed hunk when a judgement reaches beyond it.
- `comment` stands alone: a reader sees the code and the comment, not the rest of your review.
- `severity` is one of, ranked:

  | severity | meaning |
  | --- | --- |
  | `critical` | Must not ship: a miscompilation, data loss, a security hole, or the change does not do what it claims to do. |
  | `high` | Blocks the merge: wrong behaviour in a realistic case, the backends disagreeing, or a clear violation of the code's own contract or invariants. |
  | `medium` | Should be addressed: a missed edge case, a fragile pattern, a missing test, or inconsistency with the repository's conventions. |
  | `low` | Polish: naming, clarity, or style. |

Do not write anything after the block. Text outside it is discarded.
