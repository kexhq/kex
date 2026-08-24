// The resolution logic, without VS Code.
//
// `src/toolchain.ts` decides which compiler the language server runs, and it
// is the half of the extension worth testing this way: it is nearly pure —
// settings in, a path out — and every rule in it is one somebody can get
// wrong. The VS Code API it touches is small enough to stand in for, and a
// fake `TEY_HOME` of stub compilers stands in for the machine, so these tests
// say the same thing on a laptop with four toolchains and on a CI runner with
// none.

import { afterEach, beforeEach, describe, expect, mock, test } from 'bun:test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { makePackage, makeTeyHome, writeStubBinary } from '../fixtures/tey-home.mjs';

// What the extension reads out of the VS Code configuration, and nothing more.
const settings: Record<string, string | undefined> = {};
const warnings: string[] = [];

mock.module('vscode', () => ({
  workspace: {
    workspaceFolders: undefined,
    // `getConfiguration(section)` scopes the keys, so the stub scopes them
    // too — reading `executablePath` here must find `kex.executablePath`.
    getConfiguration: (section: string) => ({
      get: (key: string, fallback?: string) => settings[`${section}.${key}`] ?? fallback,
      // `inspect` is how the extension tells a value somebody set from a
      // default, so the stub has to distinguish them too.
      inspect: (key: string) => ({ globalValue: settings[`${section}.${key}`] }),
    }),
  },
  window: { showWarningMessage: (message: string) => { warnings.push(message); } },
}));

const {
  compareVersions, expandPath, installedToolchains, kexInfo, lockedVersion,
  parseTeyListing, resolveToolchain, teyHome, teySelectedVersion,
  toolchainBinary,
} = await import('../../src/toolchain');

const folder = (fsPath: string) => ({ scheme: 'file', fsPath }) as never;

let home: string;
let workspace: string;
const originalHome = process.env.TEY_HOME;
const originalKex = process.env.TEY_KEX;

beforeEach(() => {
  for (const key of Object.keys(settings)) delete settings[key];
  warnings.length = 0;
  home = makeTeyHome(['0.9.0', '0.8.1', '0.8.0'], '0.8.1');
  process.env.TEY_HOME = home;
  delete process.env.TEY_KEX;
  workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'kex-workspace-'));
});

afterEach(() => {
  fs.rmSync(home, { recursive: true, force: true });
  fs.rmSync(workspace, { recursive: true, force: true });
  if (originalHome === undefined) delete process.env.TEY_HOME;
  else process.env.TEY_HOME = originalHome;
  if (originalKex === undefined) delete process.env.TEY_KEX;
  else process.env.TEY_KEX = originalKex;
});

describe('reading the Tey home', () => {
  test('honours TEY_HOME', () => {
    expect(teyHome()).toBe(home);
  });

  test('lists installed toolchains newest first', () => {
    expect(installedToolchains().map(t => t.version)).toEqual(['0.9.0', '0.8.1', '0.8.0']);
  });

  test('marks the version Tey has selected', () => {
    expect(installedToolchains().filter(t => t.selected).map(t => t.version)).toEqual(['0.8.1']);
    expect(teySelectedVersion()).toBe('0.8.1');
  });

  test('skips staging leftovers, which are not installations', () => {
    const versions = installedToolchains().map(t => t.version);
    expect(versions).not.toContain('9.9.9.partial');
    expect(versions).not.toContain('9.9.8.previous');
  });

  test('a directory without a bin/kex is not an installation', () => {
    fs.mkdirSync(path.join(home, 'toolchains', '0.7.0'), { recursive: true });
    expect(installedToolchains().map(t => t.version)).not.toContain('0.7.0');
  });

  test('an empty or missing Tey home is not an error', () => {
    process.env.TEY_HOME = path.join(home, 'nope');
    expect(installedToolchains()).toEqual([]);
    expect(teySelectedVersion()).toBeUndefined();
  });
});

describe('resolution order', () => {
  test('falls back to Tey when nothing is set', () => {
    const resolved = resolveToolchain(folder(workspace));
    expect(resolved.command).toBe(toolchainBinary('0.8.1'));
    expect(resolved.origin.kind).toBe('tey');
  });

  test("a Kex source checkout uses its own build", () => {
    fs.writeFileSync(path.join(workspace, 'CMakeLists.txt'), 'project(kex)');
    fs.mkdirSync(path.join(workspace, 'src', 'stdlib'), { recursive: true });
    fs.writeFileSync(path.join(workspace, 'src', 'main.cxx'), 'int main() {}');
    const local = writeStubBinary(path.join(workspace, 'build', 'kex'));
    const resolved = resolveToolchain(folder(workspace));
    expect(resolved.command).toBe(local);
    expect(resolved.origin.kind).toBe('workspace');
  });

  test('an unrelated workspace does not claim a build/kex helper', () => {
    writeStubBinary(path.join(workspace, 'build', 'kex'));
    expect(resolveToolchain(folder(workspace)).origin.kind).toBe('tey');
  });

  test("an explicit toolchain selection overrides a checkout's own build", () => {
    fs.writeFileSync(path.join(workspace, 'CMakeLists.txt'), 'project(kex)');
    fs.mkdirSync(path.join(workspace, 'src', 'stdlib'), { recursive: true });
    fs.writeFileSync(path.join(workspace, 'src', 'main.cxx'), 'int main() {}');
    writeStubBinary(path.join(workspace, 'build', 'kex'));
    settings['kex.toolchain'] = 'tey';
    expect(resolveToolchain(folder(workspace)).origin.kind).toBe('tey');
  });

  test('TEY_KEX wins over the selected version', () => {
    const custom = writeStubBinary(path.join(workspace, 'elsewhere', 'kex'));
    process.env.TEY_KEX = custom;
    expect(resolveToolchain(folder(workspace)).command).toBe(custom);
  });

  test('kex.toolchain pins an installed version', () => {
    settings['kex.toolchain'] = '0.8.0';
    const resolved = resolveToolchain(folder(workspace));
    expect(resolved.command).toBe(toolchainBinary('0.8.0'));
    expect(resolved.origin.kind).toBe('toolchain');
  });

  test('kex.toolchain can name a binary of your own', () => {
    const own = writeStubBinary(path.join(workspace, 'build', 'kex'));
    settings['kex.toolchain'] = own;
    const resolved = resolveToolchain(folder(workspace));
    expect(resolved.command).toBe(own);
    expect(resolved.origin.kind).toBe('custom');
  });

  test('a pinned version that is not installed warns and falls back', () => {
    settings['kex.toolchain'] = '4.5.6';
    const resolved = resolveToolchain(folder(workspace));
    expect(resolved.command).toBe(toolchainBinary('0.8.1'));
    expect(warnings.join(' ')).toContain('4.5.6');
  });

  test('kex.executablePath overrides everything', () => {
    settings['kex.toolchain'] = '0.8.0';
    settings['kex.executablePath'] = writeStubBinary(path.join(workspace, 'mine', 'kex'));
    const resolved = resolveToolchain(folder(workspace));
    expect(resolved.command).toBe(settings['kex.executablePath']);
    expect(resolved.origin.kind).toBe('setting');
  });

  // The bare default is not a choice: `kex` is Tey's own dispatcher, so
  // honouring it as an override would resolve to the same compiler while
  // switching off the lock file and the picker.
  test.each(['kex', '  kex  '])('kex.executablePath of %p is not an override', value => {
    settings['kex.executablePath'] = value;
    settings['kex.toolchain'] = '0.8.0';
    const resolved = resolveToolchain(folder(workspace));
    expect(resolved.command).toBe(toolchainBinary('0.8.0'));
    expect(resolved.origin.kind).toBe('toolchain');
  });

  test('a relative executablePath resolves against the workspace', () => {
    writeStubBinary(path.join(workspace, 'build', 'kex'));
    settings['kex.executablePath'] = 'build/kex';
    expect(resolveToolchain(folder(workspace)).command)
      .toBe(path.join(workspace, 'build', 'kex'));
  });

  test('${workspaceFolder} is expanded', () => {
    writeStubBinary(path.join(workspace, 'build', 'kex'));
    settings['kex.executablePath'] = '${workspaceFolder}/build/kex';
    expect(resolveToolchain(folder(workspace)).command)
      .toBe(path.join(workspace, 'build', 'kex'));
  });

  test('~ is expanded', () => {
    expect(expandPath('~/bin/kex')).toBe(path.join(os.homedir(), 'bin', 'kex'));
  });
});

describe("a package's tey.lock", () => {
  test('is used when the locked version is installed', () => {
    makePackage(workspace, { kexVersion: '0.8.0' });
    expect(lockedVersion(folder(workspace))).toBe('0.8.0');
    const resolved = resolveToolchain(folder(workspace));
    expect(resolved.command).toBe(toolchainBinary('0.8.0'));
    expect(resolved.origin.kind).toBe('lock');
  });

  test('falls back to Tey when the locked version is not installed', () => {
    makePackage(workspace, { kexVersion: '0.1.2' });
    const resolved = resolveToolchain(folder(workspace));
    expect(resolved.command).toBe(toolchainBinary('0.8.1'));
    expect(resolved.origin.kind).toBe('tey');
  });

  test("kex.toolchain: 'tey' ignores the lock file", () => {
    makePackage(workspace, { kexVersion: '0.8.0' });
    settings['kex.toolchain'] = 'tey';
    expect(resolveToolchain(folder(workspace)).origin.kind).toBe('tey');
  });

  test('a lock file mid-merge is not an error', () => {
    makePackage(workspace, { kexVersion: '0.8.0' });
    fs.writeFileSync(path.join(workspace, 'tey.lock'), '<<<<<<< HEAD\n{');
    expect(lockedVersion(folder(workspace))).toBeUndefined();
    expect(resolveToolchain(folder(workspace)).origin.kind).toBe('tey');
  });

  test('no lock file at all is not an error', () => {
    expect(lockedVersion(folder(workspace))).toBeUndefined();
  });
});

describe('asking a compiler what it is', () => {
  test('reads --info', async () => {
    const binary = toolchainBinary('0.9.0');
    expect(await kexInfo(binary)).toEqual({ version: '0.9.0', revision: 'stub123' });
  });

  test('a compiler that cannot run says nothing', async () => {
    const broken = writeStubBinary(path.join(workspace, 'broken', 'kex'), { broken: true });
    expect(await kexInfo(broken)).toBeUndefined();
  });

  test('a path that is not there says nothing', async () => {
    expect(await kexInfo(path.join(workspace, 'absent'))).toBeUndefined();
  });
});

describe('the tey listing command', () => {
  // The shape `tey kex list --installed --json` prints; kept as a literal so
  // a change on tey's side shows up here as a parse failure, not a silence.
  const document = (home: string) => JSON.stringify({
    home,
    selected: '0.8.1',
    toolchains: [
      { version: '0.9.0', binary: toolchainBinary('0.9.0'), selected: false, bundled: false },
      { version: '0.8.1', binary: toolchainBinary('0.8.1'), selected: true, bundled: false },
      // The seed bundled beside Tey: runnable where it lies, newest, and not
      // under the toolchains directory at all.
      { version: '9.8.0', binary: '/opt/homebrew/opt/kex/bin/kex', selected: false, bundled: true },
    ],
  });

  test('parses the document, newest first, with selection and bundling', () => {
    const listed = parseTeyListing(document(teyHome()));
    expect(listed).toEqual([
      { version: '9.8.0', binary: '/opt/homebrew/opt/kex/bin/kex', selected: false, bundled: true },
      { version: '0.9.0', binary: toolchainBinary('0.9.0'), selected: false, bundled: false },
      { version: '0.8.1', binary: toolchainBinary('0.8.1'), selected: true, bundled: false },
    ]);
  });

  test('an empty toolchains array is a valid answer', () => {
    const empty = JSON.stringify({ home: teyHome(), selected: null, toolchains: [] });
    expect(parseTeyListing(empty)).toEqual([]);
  });

  test('a listing for a different Tey home is not this machine\'s answer', () => {
    expect(parseTeyListing(document('/somewhere/else'))).toBeUndefined();
  });

  test.each([
    ['prose, not JSON', 'not json'],
    ['no home', JSON.stringify({ selected: null, toolchains: [] })],
    ['toolchains not an array', JSON.stringify({ home: teyHome(), selected: null, toolchains: {} })],
    ['an entry missing fields', JSON.stringify({
      home: teyHome(), selected: null, toolchains: [{ version: '0.9.0' }],
    })],
    ['an entry with the wrong types', JSON.stringify({
      home: teyHome(), selected: null,
      toolchains: [{ version: '0.9.0', binary: '/x', selected: 'yes', bundled: false }],
    })],
  ])('rejects %s', (_name, text) => {
    expect(parseTeyListing(text as string)).toBeUndefined();
  });
});

describe('version ordering', () => {
  const newestFirst = (versions: string[]) => [...versions].sort((a, b) => compareVersions(b, a));

  test('orders releases numerically, not as strings', () => {
    expect(newestFirst(['0.9.0', '0.10.0', '0.2.0'])).toEqual(['0.10.0', '0.9.0', '0.2.0']);
  });

  test('a release outranks its own pre-releases', () => {
    expect(newestFirst(['0.4.0-rc.1', '0.4.0'])).toEqual(['0.4.0', '0.4.0-rc.1']);
  });

  test('pre-release numbers compare as numbers', () => {
    expect(newestFirst(['0.4.0-rc.2', '0.4.0-rc.10'])).toEqual(['0.4.0-rc.10', '0.4.0-rc.2']);
  });

  test('something that is not a version sorts last rather than throwing', () => {
    expect(newestFirst(['nightly', '0.1.0'])).toEqual(['0.1.0', 'nightly']);
  });
});
