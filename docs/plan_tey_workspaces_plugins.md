# Tey workspaces, package sources, plugins, and generators

## Context

Tey currently assumes one package per repository, Git-only dependencies, and
the Kex version embedded in the compiler that built Tey. The redesign adds
workspaces and monorepos, reproducible path sources, local development
overrides, workspace-aware command execution, and resource-scoped plugins and
generators.

This is a pre-stability format break. Manifests are migrated manually from
`bundle` to `package`, and existing lockfiles are deleted and regenerated.
There is no compatibility reader or automatic migration.

The work proceeds in dependency order: selected-toolchain correctness first,
then manifests/workspaces and source resolution, then install and command
semantics, and finally plugins and generators.

## 1. Fix selected-toolchain correctness

Resolve the Rodolfo blocker first. `Tey.Resolver` must validate requirements
and write the lockfile using the Kex selected for the project, not
`Kex.Kernel.VERSION`, which describes the compiler that built Tey.

Add a `Tey.Toolchain.ToolchainInfo` record containing the selected compiler
version and optional runtime OTP floor. Read both in one invocation of:

```sh
<Tey.Toolchain.kex> --info
```

For a workspace, collect every member's `kex(...)` requirement, validate the
selected compiler against all of them, report incompatible members together,
and record the selected release version in `tey.lock`. Keep Tey's own build
version entirely separate.

This is an independently shippable milestone and needs a Rodolfo regression
test proving that Tey built with one Kex can install a project requiring a
newer selected Kex.

## 2. Rename manifest roots and add workspaces

Replace:

```kex
bundle "rodolfo" do
end
```

with:

```kex
package "rodolfo" do
  version("0.4.0")

  workspace do
    members(["packages/*", "examples/*"])
  end
end
```

A virtual workspace has no package root:

```kex
workspace do
  members(["packages/*"])
end
```

Reject `bundle` rather than supporting both spellings. Update scaffolding,
documentation, examples, specs, and Tey's own manifests. `bundle` remains an
ordinary Kex identifier; only Tey's manifest DSL changes.

Add `Tey.Workspace`, responsible for:

- Walking upward through `package.kex` files and finding the outermost active
  workspace.
- Expanding explicitly declared member patterns and recursively joining
  nested workspaces.
- Selecting the deepest member containing the current directory.
- Enforcing globally unique package names.
- Warning about unmatched member patterns and directories without manifests.
- Detecting and warning about workspace dependency cycles.

Commands consume a `WorkspaceContext` rather than assuming that the current
directory is the package root.

## 3. Model dependencies as source variants

Replace parallel Git-specific manifest fields with a tagged source model:

```text
ManifestDependency
├── WorkspaceSource
├── GitSource
├── PathSource
└── unresolved external source
```

Manifest forms:

```kex
tey("parser", workspace: true)

tey(
  "parser",
  git: "https://github.com/acme/compiler",
  tag: "~> 1.2",
  subdir: "packages/parser"
)

tey("parser", path: "../../parser")
```

Rules:

- `workspace: true`, `git:`, and `path:` are mutually exclusive.
- Workspace dependencies have no version constraint.
- Git dependencies require an explicit tag/version, branch, or ref.
- Independent-package tags use the package prefix, such as `parser-v*`.
- Git monorepo members are found by package name; optional `subdir:` bypasses
  discovery.
- A workspace dependency fetched externally resolves against another member
  from the same repository commit.
- One Git repository and commit is checked out only once.

## 4. Add immutable path snapshots

`path:` is reproducible rather than live:

1. Resolve it relative to the declaring manifest.
2. Validate the target package name and manifest.
3. Apply mandatory output, VCS, metadata, and cache exclusions.
4. Apply `.teyignore`.
5. Hash the remaining package contents deterministically.
6. Copy them transactionally into Tey's global cache.
7. Record the relative source path and content hash.

Changes become visible only after `tey update <package>`. Preserve symlinks
whose resolved targets remain within the package directory; reject escaping
symlinks.

## 5. Replace lockfile schema version 1 in place

Keep `"version": 1`. Existing lockfiles are manually deleted and regenerated
with `tey install`; do not implement migration, legacy-schema compatibility,
or automatic regeneration.

The replacement shape includes:

```json
{
  "version": 1,
  "workspace": {
    "members": {},
    "manifest_fingerprint": "..."
  },
  "toolchain": {
    "kex": "0.4.0-beta",
    "otp": 28
  },
  "packages": {},
  "plugins": {
    "approvals": {}
  }
}
```

Package entries record identity and declared version, source type and source
identity, dependency edges, groups, workspace membership, and plugin
declarations/fingerprints. Fingerprints cover normalized member manifests,
`.teyignore`, source declarations, dependency edges, and plugin interfaces.

The lockfile belongs to the outermost active workspace. Nested workspace
lockfiles are ignored while included by a parent.

## 6. Merge locking into `tey install`

Remove the public `tey lock` command. `tey install` becomes transactional:

1. Discover the workspace.
2. Read all member manifests.
3. Validate the selected toolchain.
4. Validate or resolve `tey.lock`.
5. Fetch or snapshot the complete workspace graph.
6. Inspect direct plugin declarations and request missing approvals.
7. Atomically publish the new lockfile.
8. Install declared targets.

If the lock remains valid, preserve resolutions and only fetch or verify
packages. Stage cache and lockfile changes before publishing; interrupted
downloads may leave reusable cache entries, but the old lockfile stays intact.

`tey update` changes resolution. At the workspace root it updates the whole
workspace; within a member it updates only that member's dependency closure.
A declined plugin approval aborts the entire update.

The Git merge driver calls an internal `tey install --merge-driver` mode.

Target installation semantics:

- Workspace-root `tey install` installs targets from every member.
- Member-local `tey install` ensures the shared graph exists but installs only
  that member's targets.
- `--package <name>` installs only the selected member's targets while still
  ensuring the shared graph exists.

## 7. Add `package.local.kex`

Developer-only live overrides live beside manifests:

```kex
local("parser", path: "../../parser")
```

Commands:

```sh
tey local parser ../../parser
tey local --list
tey local --remove parser
```

Tey adds `package.local.kex` to the workspace `.gitignore` and rejects a
tracked local manifest. Root/member overrides of the same dependency conflict.
Live sources bypass the cache and never alter the reproducible graph. Package
identity, dependency declarations, and plugin fingerprints must match the
lockfile.

## 8. Add workspace command scope and scheduling

Command scope is determined as follows:

- Workspace root or a non-member directory: whole workspace.
- Member directory: deepest containing member.
- `--package <name>`: explicitly selected member.
- `--workspace`: broaden a member-local operation where applicable.
- `tey install`: always ensures the complete workspace graph.

Workspace-wide build and test commands construct the member dependency graph
and schedule dependency-ready members in parallel. Default concurrency is the
available CPU count, with `--jobs N` as an override. Continue unaffected work
after failures and aggregate failures at the end. Cycles warn rather than
reject.

## 9. Parse plugin declarations statically

Example provider manifest:

```kex
package "kex-github" do
  version("1.0.0")

  plugin namespace: "github", tey: ">= 0.6, < 0.8" do
    capabilities([
      FS.Read.package,
      FS.Write.workspace,
      Process.Run(["git"]),
      Net.Connect(["api.github.com"])
    ])

    command("setup", entrypoint: "src/github/setup.kex") do
      option("branch", type: String)
    end

    generator("workflow", entrypoint: "src/github/workflow.kex") do
      option("otp", type: Integer)
    end
  end
end
```

The manifest reader parses declarations as data and never executes plugin
code. Only direct dependencies can provide active plugins. Root dependencies
provide workspace-global plugins; member dependencies provide member-local
plugins. Member-local plugins require current-member context or `--package`.
Namespace collisions in visible scope are errors.

Approval is independent per namespace, inherited through `tey.lock`, and
invalidated only when the declared interface or requested resources change.

## 10. Add a supervised plugin host

Run plugins as child processes using the selected Kex toolchain. Use a
separately versioned structured protocol over stdin/stdout. The host supplies
workspace context and typed options, receives operation requests, enforces
approved resources, performs or reports operations, and handles crashes.

Context includes workspace root, current member, all members, the resolved
graph, lock metadata, selected toolchain, parsed arguments/options, and the
inherited environment.

Kex capability substitution alone is insufficient for resource-scoped
containment. The host must provide brokered implementations and prevent plugin
children from bypassing them through default intrinsic capabilities.

## 11. Implement generators through the plugin host

Invocation:

```sh
tey generate github:workflow
```

Every filesystem operation is reported. Operations may not escape the
workspace through absolute paths, `..`, or symlinks. New files may be written
immediately. Existing-file changes and deletions require confirmation;
non-interactive execution fails when confirmation is required, while `--force`
explicitly authorizes them. Generated-file ownership is not tracked. Approved
network and subprocess operations are reported without per-operation prompts.

## Verification

Add pure specs for workspace parsing/discovery, member-pattern warnings, scope
selection, source variants, lockfile round trips, plugin fingerprints and
approval invalidation, static command schemas, and `.teyignore` matching.

Add temporary-repository integration tests for shared lock behavior, Git
monorepo discovery and `subdir:`, one checkout serving multiple packages, path
snapshots and symlink rejection, `package.local.kex` Git protection, nested
workspaces, parallel scheduling and aggregated failures, generator path
escapes, and atomic rejection of plugin updates.

## Deliberately deferred

- `tey release`
- Registry sources
- Shared external dependency catalogs
- Generated-file ownership
- Automatic manifest or lockfile migration
- Automatic selection of the newest package tag

