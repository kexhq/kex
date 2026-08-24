// What to ask a spec file's runner for, as a command line.
//
// There are two runners, and which one a workspace gets is not a preference:
//
//   tey test <spec> --json|--list [--only <name>]      in a Tey package
//   kex --test-json|--test-list [--test-only <name>]…  everywhere else
//
// A package's specs need its source roots — its own `src/`, and every locked
// dependency's — and Tey is the only thing that knows them: `tey test` is how
// a package's suite is run, so it is how the editor runs it too. The same
// reasoning as the task provider, which asks `tey help --json` rather than
// reading package.kex itself. Outside a package there is nothing for Tey to
// know, and `kex` is one process fewer.
//
// Both spellings reach the SAME compiler flags — Tey passes `--test-json` and
// friends straight through (tey/src/tey/commands.kex) — so the records an
// editor reads are identical either way. What differs is the flag names, the
// backend default, and how many filters one invocation can carry.
//
// Nothing here imports vscode: deciding WHICH runner needs the filesystem and
// lives in `test-explorer.ts`; building the line does not, and is tested
// without an editor (test/unit/test-runner.test.ts).

import { TeyCommand } from './tey-commands';

/**
 * Which backend runs the cases.
 *
 * `auto` follows the runner's own convention — the BEAM under Tey, because
 * that is what `tey test` does and what a package ships as; the interpreter
 * under bare `kex`, because it starts in milliseconds and there is no `tey
 * test` for it to match. `walker`/`beam` say so outright.
 *
 * Both backends emit identical records for the same file — the compiler's own
 * suite pins that (`make spec-test-json`) — so this changes what the cases run
 * on, never what the tree is told about them.
 */
export type TestBackend = 'auto' | 'walker' | 'beam';

/** A run reporting each case, or a discovery pass that runs none of them. */
export type TestMode = 'json' | 'list';

/**
 * `--only` takes ONE name, so a request naming several cases becomes several
 * invocations. `kex --test-only` is repeatable and needs only one.
 */
export const TEY_MAX_FILTERS = 1;
export const KEX_MAX_FILTERS = Number.POSITIVE_INFINITY;

/**
 * Whether this Tey's `test` command takes the machine-readable flags, read off
 * `tey help --json` — the same answer the task provider already believes about
 * commands, from the same document.
 *
 * Asking rather than assuming, because the flags landed in Tey after several
 * releases were already installed on people's machines: an older `tey test`
 * meets `--list` with its help text and exit 1, and a test tree built on that
 * is silently, permanently empty. `usage` is where a command declares what it
 * accepts, so it is what says whether this one can.
 */
export function teySupportsTestFlags(commands: readonly TeyCommand[] | undefined): boolean {
  const test = commands?.find(command => command.name === 'test');
  return test !== undefined && test.usage.includes('--list');
}

/**
 * Splits the filters a run asked for into one group per invocation.
 *
 * No filters is ONE invocation, not none: "run everything" is a run, and it is
 * the case that matters most. A runner that takes every filter at once gets a
 * single group however many there are.
 */
export function batches(filters: readonly string[], maxFilters: number): string[][] {
  if (filters.length === 0) return [[]];
  const size = Math.max(1, maxFilters);
  if (!Number.isFinite(size) || size >= filters.length) return [[...filters]];
  const groups: string[][] = [];
  for (let start = 0; start < filters.length; start += size) {
    groups.push(filters.slice(start, start + size));
  }
  return groups;
}

/** `tey test <spec> …`, run from the package root. */
export function teyTestArgs(
  spec: string,
  mode: TestMode,
  filters: readonly string[],
  backend: TestBackend,
): string[] {
  const args = ['test', spec];
  // Tey runs on the BEAM unless told otherwise, which is the convention `auto`
  // follows here — an editor that quietly ran a package's suite on the other
  // backend would disagree with `tey test` in the terminal beside it.
  if (backend === 'walker') args.push('--interpret');
  args.push(mode === 'list' ? '--list' : '--json');
  for (const filter of filters.slice(0, TEY_MAX_FILTERS)) args.push('--only', filter);
  return args;
}

/** `kex …`, for a workspace with no package for Tey to read. */
export function kexTestArgs(
  spec: string,
  mode: TestMode,
  filters: readonly string[],
  backend: TestBackend,
): string[] {
  // The compiler tree-walks unless asked for the BEAM, which is what `auto`
  // takes here: nothing in a bare directory says the suite must run as it
  // ships, and the walker starts in milliseconds where a BEAM node does not.
  const args = backend === 'beam' ? ['-R'] : [];
  args.push('--no-colors', mode === 'list' ? '--test-list' : '--test-json');
  for (const filter of filters) args.push('--test-only', filter);
  args.push(spec);
  return args;
}
