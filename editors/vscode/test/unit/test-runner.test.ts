// The command line the Testing view runs a spec with.
//
// Worth testing without an editor because a wrong line here is SILENT: a
// filter that goes missing runs the whole suite and reports it as one case, a
// mode that goes missing floods a JSON reader with ✓/✗ prose, and a backend
// that goes missing runs a package's suite on the one it does not ship as.

import { describe, expect, test } from 'bun:test';
import {
  KEX_MAX_FILTERS,
  TEY_MAX_FILTERS,
  batches,
  kexTestArgs,
  teySupportsTestFlags,
  teyTestArgs,
} from '../../src/test-runner';

const SPEC = '/w/pkg/spec/greet.spec.kex';

describe('teyTestArgs', () => {
  test('runs one named spec, reporting as JSON', () => {
    expect(teyTestArgs(SPEC, 'json', [], 'auto')).toEqual(['test', SPEC, '--json']);
  });

  test('discovers without running under list', () => {
    expect(teyTestArgs(SPEC, 'list', [], 'auto')).toEqual(['test', SPEC, '--list']);
  });

  test('names one case with --only', () => {
    expect(teyTestArgs(SPEC, 'json', ['outer > inner'], 'auto')).toEqual(
      ['test', SPEC, '--json', '--only', 'outer > inner']);
  });

  // Tey runs on the BEAM, which is what a package ships as — so `auto` and an
  // explicit `beam` are the same line, and only `walker` asks for the other.
  test('follows Tey onto the BEAM unless the walker was asked for', () => {
    expect(teyTestArgs(SPEC, 'json', [], 'beam')).not.toContain('--interpret');
    expect(teyTestArgs(SPEC, 'json', [], 'auto')).not.toContain('--interpret');
    expect(teyTestArgs(SPEC, 'json', [], 'walker')).toContain('--interpret');
  });

  test('carries only as many filters as --only can take', () => {
    // Tey's --only names ONE case. Passing two would silently keep the last,
    // running one case while the tree waited for two.
    expect(teyTestArgs(SPEC, 'json', ['a', 'b'], 'auto').filter(a => a === '--only'))
      .toHaveLength(TEY_MAX_FILTERS);
  });
});

describe('kexTestArgs', () => {
  test('reports as JSON, with the file last', () => {
    expect(kexTestArgs(SPEC, 'json', [], 'auto')).toEqual(
      ['--no-colors', '--test-json', SPEC]);
  });

  test('discovers without running under list', () => {
    expect(kexTestArgs(SPEC, 'list', [], 'auto')).toEqual(
      ['--no-colors', '--test-list', SPEC]);
  });

  // The compiler tree-walks by default, so here `auto` is the walker — the
  // opposite of Tey's default, and the reason `auto` is not a single flag.
  test('tree-walks unless the BEAM was asked for', () => {
    expect(kexTestArgs(SPEC, 'json', [], 'auto')).not.toContain('-R');
    expect(kexTestArgs(SPEC, 'json', [], 'walker')).not.toContain('-R');
    expect(kexTestArgs(SPEC, 'json', [], 'beam')[0]).toBe('-R');
  });

  test('repeats --test-only, which takes as many as asked', () => {
    expect(kexTestArgs(SPEC, 'json', ['a > b', 'c'], 'auto')).toEqual(
      ['--no-colors', '--test-json', '--test-only', 'a > b', '--test-only', 'c', SPEC]);
  });
});

// The flags reached Tey after several releases were already on people's
// machines, and an older `tey test` answers `--list` with its help text and
// exit 1 — which a test tree can only render as "no tests here, ever".
describe('teySupportsTestFlags', () => {
  const command = (usage: string) =>
    [{ name: 'test', usage, description: 'run spec/*.spec.kex', section: '' }];

  test('a test command that declares --list takes the flags', () => {
    expect(teySupportsTestFlags(
      command('[<spec>...] [--json] [--list] [--only <name>]'))).toBe(true);
  });

  test('a test command from before the flags does not', () => {
    expect(teySupportsTestFlags(command(''))).toBe(false);
  });

  test('a Tey whose answer could not be read does not', () => {
    // `parseTeyCommands` answers undefined for output it does not understand,
    // and an unreadable answer is not evidence of support.
    expect(teySupportsTestFlags(undefined)).toBe(false);
    expect(teySupportsTestFlags([])).toBe(false);
  });

  test('another command declaring --list is not the test command', () => {
    expect(teySupportsTestFlags(
      [{ name: 'kex list', usage: '[--list]', description: '', section: '' }]))
      .toBe(false);
  });
});

describe('batches', () => {
  test('no filters is one invocation, not none', () => {
    // "Run everything" is a run — the one that matters most.
    expect(batches([], TEY_MAX_FILTERS)).toEqual([[]]);
  });

  test('one invocation per filter when only one fits', () => {
    expect(batches(['a', 'b', 'c'], TEY_MAX_FILTERS)).toEqual([['a'], ['b'], ['c']]);
  });

  test('one invocation when every filter fits', () => {
    expect(batches(['a', 'b', 'c'], KEX_MAX_FILTERS)).toEqual([['a', 'b', 'c']]);
  });

  test('groups of the size given', () => {
    expect(batches(['a', 'b', 'c'], 2)).toEqual([['a', 'b'], ['c']]);
  });
});
