# `ai-review`

A free AI review of every pull request, posted back to it: one sticky comment
per pull request, `✅ Ready to merge` or `🚩 Not ready to merge`, rewritten in
place on every push, plus one inline comment per finding, anchored to the diff.

```yaml
- uses: actions/checkout@v4
  with:
    fetch-depth: 0
- uses: ./.github/actions/ai-review
```

`.github/workflows/ai-review.yml` wires it to `pull_request`.

## Free

The review runs on `opencode/glm-5.3-flash-free`, one of OpenCode's free
models, through the OpenCode CLI. The CLI runs inside the OpenCode identity,
which is what makes the free tier usable from a runner, so the job needs no
provider credentials. To review with another provider, point `model` and
`fast-model` at it and give the job that provider's credentials.

## How a run goes

1. **Review** (`model`, the effort variant after `#`): the agent follows
   `SKILL.md`. It diffs the PR against `GITHUB_BASE_REF`, reads `CLAUDE.md`
   and `STYLE.md`, and ends with a `kex-review` JSON block of findings rated
   `low` → `critical`. Findings below `min-severity` are dropped. A `high` or
   `critical` that survives flips the verdict to 🚩.
2. **Reconcile** (`fast-model`): when an earlier round left comments, a
   second agent sees them next to the new findings and decides which still
   stand (`keep`) and which are addressed (`close`). An id it does not decide
   on is kept: a comment is never destroyed on a guess.
3. **Publish** (`review.kex`, deterministic): closed comments are resolved
   through GraphQL, or deleted when the token cannot resolve a conversation
   (the workflow's own token cannot; pass a personal access token as `token`
   to resolve instead). Repeated findings update their earlier comment, new
   ones post inline, and the sticky comment is rewritten. Nothing either
   agent writes is posted verbatim: they decide, the code renders.

Each inline comment carries a hidden `file|line|severity` marker, so the next
run updates it rather than posting a duplicate. A finding outside the diff's
hunks falls back to a file-level comment, and one on a file the PR did not
touch is spelled out in the sticky comment instead.

A run that produces no review fails the step. A model that cannot be routed,
or a provider outage, turns the check red instead of reading like a clean
review.

## The reviewer is Kex

`review.kex` runs on a **released** Kex (`kex-version`, installed by
`setup-tey`), not on the compiler the pull request builds, so a PR that breaks
the compiler cannot break its own review. `review.spec.kex` runs first, as
the action's own self-test. GitHub is reached through `gh api` and the
agent's NDJSON stream is flattened with `jq`. The runner image provides both.

Run it locally, without posting anything:

```sh
INPUT_COMMENT=false GITHUB_BASE_REF=main kex .github/actions/ai-review/review.kex
```

With `GITHUB_REPOSITORY` and `GITHUB_PR_NUMBER` set as well, the dry run also
reads the PR's earlier round and runs the reconciliation, and still posts
nothing. It needs the OpenCode CLI the action pins (2.0.31): 2.0 takes the
effort variant as `provider/model#variant`, and 1.x rejects that spelling.
A locally stored OpenCode credential, if it is stale, makes the free
models fail with `Invalid credential`; CI has none.

## Not covered

Pull requests from forks are skipped. GitHub gives `pull_request` runs from a
fork a read-only token, so nothing could be posted, and the agent runs with
`--auto` tool access over the checked-out code. Reviewing forks would need
`pull_request_target` or a `workflow_run` follow-up, with care for both.
