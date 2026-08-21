// Finding a Kex compiler, and saying which one it is.
//
// Tey owns Kex installation, so the extension asks Tey rather than guessing.
// It reads the Tey home directly instead of shelling out to `tey kex list`:
// that command reaches the network for released versions and prints prose
// meant for a human, neither of which a picker wants. The layout it reads —
// `toolchains/<version>/bin/kex` and a `current` file naming the selection —
// is the same one `tey/src/tey/toolchain.kex` writes.

import * as vscode from 'vscode';
import * as cp from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';

/** Where a resolved compiler came from, which is what the label says. */
export type ToolchainOrigin =
  | { kind: 'setting' }            // kex.executablePath, set by hand
  | { kind: 'custom' }             // a binary picked from disk
  | { kind: 'toolchain' }          // a version pinned by kex.toolchain
  | { kind: 'lock' }               // the version this package's tey.lock names
  | { kind: 'tey' }                // whatever Tey has selected
  | { kind: 'path' };              // a bare `kex`, found (or not) on PATH

export interface ResolvedToolchain {
  /** What to spawn. Absolute wherever we could make it absolute. */
  command: string;
  origin: ToolchainOrigin;
}

export interface InstalledToolchain {
  version: string;
  binary: string;
  /** True for the version Tey itself has selected in its `current` file. */
  selected: boolean;
}

export interface KexInfo {
  version: string;
  revision?: string;
}

export const TEY_FOLLOW = 'tey';

export function teyHome(): string {
  const explicit = process.env.TEY_HOME;
  if (explicit && explicit.trim() !== '') return explicit;
  return path.join(os.homedir(), '.local', 'share', 'tey');
}

/**
 * Versions installed under this Tey home, newest first. Staging leftovers
 * (`.partial`, `.previous`) are not installations, and a directory without a
 * `bin/kex` is a half-removed one.
 */
export function installedToolchains(): InstalledToolchain[] {
  const root = path.join(teyHome(), 'toolchains');
  let entries: fs.Dirent[];
  try {
    entries = fs.readdirSync(root, { withFileTypes: true });
  } catch {
    return [];
  }
  const selected = teySelectedVersion();
  const found: InstalledToolchain[] = [];
  for (const entry of entries) {
    if (!entry.isDirectory() && !entry.isSymbolicLink()) continue;
    const name = entry.name;
    if (name.endsWith('.partial') || name.endsWith('.previous')) continue;
    const binary = path.join(root, name, 'bin', 'kex');
    if (!isExecutableFile(binary)) continue;
    found.push({ version: name, binary, selected: name === selected });
  }
  return found.sort((a, b) => compareVersions(b.version, a.version));
}

/** The version Tey's `current` file names, if any. */
export function teySelectedVersion(): string | undefined {
  try {
    const name = fs.readFileSync(path.join(teyHome(), 'current'), 'utf8').trim();
    return name === '' ? undefined : name;
  } catch {
    return undefined;
  }
}

export function toolchainBinary(version: string): string {
  return path.join(teyHome(), 'toolchains', version, 'bin', 'kex');
}

/**
 * The Kex version this workspace's `tey.lock` resolved to, if it is a Tey
 * package.
 *
 * A package says which compiler it is built against — `kex(">= 0.3.4")` in
 * `package.kex`, resolved to an exact version in the lock file — and that is
 * a better answer for this checkout than whatever version the machine happens
 * to have selected. The LOCK is read rather than the manifest's requirement:
 * the lock holds one exact version, where the requirement is a range this
 * would have to resolve itself and could resolve differently from Tey.
 */
export function lockedVersion(folder?: vscode.Uri): string | undefined {
  if (folder?.scheme !== 'file') return undefined;
  let text: string;
  try {
    text = fs.readFileSync(path.join(folder.fsPath, 'tey.lock'), 'utf8');
  } catch {
    return undefined;
  }
  try {
    const parsed = JSON.parse(text) as { kex?: { version?: unknown } };
    const version = parsed.kex?.version;
    return typeof version === 'string' && version.trim() !== '' ? version.trim() : undefined;
  } catch {
    // A lock file mid-merge is not something to complain about here.
    return undefined;
  }
}

/**
 * The compiler a bare `kex` runs, asked of Tey.
 *
 * `TEY_KEX` and the `current` file are read directly — the same order
 * `Tey.Toolchain.resolved` uses — and `tey kex which` is the last word,
 * because it also knows about a Kex bundled beside Tey that is on the disk
 * but not in the toolchains directory.
 */
export function teyResolvedKex(): string | undefined {
  const override = process.env.TEY_KEX;
  if (override && isExecutableFile(override)) return override;

  const selected = teySelectedVersion();
  if (selected) {
    const binary = toolchainBinary(selected);
    if (isExecutableFile(binary)) return binary;
  }
  return teyKexWhich();
}

/**
 * `tey kex which` prints the absolute path of the compiler `kex` resolves to,
 * and exits 1 when none is installed. It touches no network.
 */
function teyKexWhich(): string | undefined {
  try {
    const result = cp.spawnSync('tey', ['kex', 'which'], {
      encoding: 'utf8',
      timeout: 5000,
    });
    if (result.status !== 0) return undefined;
    const line = (result.stdout ?? '').trim().split('\n')[0]?.trim();
    return line && isExecutableFile(line) ? line : undefined;
  } catch {
    return undefined;
  }
}

/**
 * Resolves the compiler to spawn.
 *
 * In order: an explicitly set `kex.executablePath`, the toolchain pinned by
 * `kex.toolchain`, the version this package's `tey.lock` names, Tey's own
 * selection, and only then a bare `kex` from PATH.
 * The bare name stays the fallback rather than the first choice — it is what
 * works before Tey is installed, not what should win once it is.
 */
export function resolveToolchain(folder?: vscode.Uri): ResolvedToolchain {
  const config = vscode.workspace.getConfiguration('kex');

  // A setting whose value is the bare default is not an override worth
  // honouring: `kex` is Tey's own dispatcher, so obeying it as an override
  // would resolve to the same compiler while silently switching off the lock
  // file and the picker. Someone who has that value has not chosen anything.
  const configured = explicitSetting(config, 'executablePath');
  if (configured && configured.trim() !== 'kex') {
    return { command: expandPath(configured, folder), origin: { kind: 'setting' } };
  }

  const pinned = (explicitSetting(config, 'toolchain') ?? '').trim();
  if (pinned !== '' && pinned !== TEY_FOLLOW) {
    if (looksLikePath(pinned)) {
      return { command: expandPath(pinned, folder), origin: { kind: 'custom' } };
    }
    const binary = toolchainBinary(pinned);
    if (isExecutableFile(binary)) {
      return { command: binary, origin: { kind: 'toolchain' } };
    }
    // A pinned version somebody has since uninstalled. Falling through to
    // Tey's selection keeps the editor working; the status bar shows which
    // version actually started, so the drift is visible rather than silent.
    void vscode.window.showWarningMessage(
      `Kex ${pinned} is not installed; using Tey's selection instead. ` +
      `Pick another with 'Kex: Select Toolchain'.`);
  }

  // An unset setting follows this package's lock file first. Writing `tey`
  // explicitly is how to say "the machine's selection, whatever this checkout
  // asks for".
  if (pinned === '') {
    const locked = lockedVersion(folder);
    if (locked !== undefined) {
      const binary = toolchainBinary(locked);
      // Not installed is not an error: Tey installs it on the next build, and
      // until then the selected toolchain is a working editor rather than none.
      if (isExecutableFile(binary)) return { command: binary, origin: { kind: 'lock' } };
    }
  }

  const tey = teyResolvedKex();
  if (tey) return { command: tey, origin: { kind: 'tey' } };

  return { command: onPath('kex') ?? 'kex', origin: { kind: 'path' } };
}

/**
 * A setting the user actually set, at any scope. `get` cannot tell a default
 * apart from someone typing the default in, and the difference decides
 * whether `kex.executablePath` outranks Tey.
 */
function explicitSetting(
  config: vscode.WorkspaceConfiguration, key: string,
): string | undefined {
  const values = config.inspect<string>(key);
  const set = values?.workspaceFolderValue ?? values?.workspaceValue ??
    values?.globalValue;
  return set !== undefined && set.trim() !== '' ? set : undefined;
}

function looksLikePath(value: string): boolean {
  return value.includes('/') || value.includes('\\') || value.startsWith('~') ||
    value.includes('${workspaceFolder}');
}

/** Makes a configured path absolute: `${workspaceFolder}`, `~`, then relative. */
export function expandPath(value: string, folder?: vscode.Uri): string {
  let expanded = value;
  if (folder?.scheme === 'file') {
    expanded = expanded.replaceAll('${workspaceFolder}', folder.fsPath);
  }
  if (expanded === '~' || expanded.startsWith('~/')) {
    expanded = path.join(os.homedir(), expanded.slice(1));
  }
  if (path.isAbsolute(expanded)) return expanded;
  if (!looksLikePath(expanded)) {
    // A bare command name, not a path. Resolving it against the workspace
    // would turn `kex` into a file that is not there.
    return onPath(expanded) ?? expanded;
  }
  return folder?.scheme === 'file' ? path.resolve(folder.fsPath, expanded) : expanded;
}

/** The first match for a bare command name on PATH, so it can be watched. */
export function onPath(command: string): string | undefined {
  const search = process.env.PATH ?? '';
  const extensions = process.platform === 'win32'
    ? (process.env.PATHEXT ?? '.EXE;.CMD;.BAT').split(';')
    : [''];
  for (const directory of search.split(path.delimiter)) {
    if (directory === '') continue;
    for (const extension of extensions) {
      const candidate = path.join(directory, command + extension);
      if (isExecutableFile(candidate)) return candidate;
    }
  }
  return undefined;
}

export function isExecutableFile(candidate: string): boolean {
  try {
    if (!fs.statSync(candidate).isFile()) return false;
  } catch {
    return false;
  }
  if (process.platform === 'win32') return true;
  try {
    fs.accessSync(candidate, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

/**
 * What a compiler says it is.
 *
 * `--info` is the machine-readable contract; `--version` is prose and is only
 * read when `--info` is missing, which means a toolchain older than it.
 */
export async function kexInfo(binary: string): Promise<KexInfo | undefined> {
  const info = await run(binary, ['--info']);
  if (info !== undefined) {
    try {
      const parsed = JSON.parse(info) as { version?: unknown; revision?: unknown };
      if (typeof parsed.version === 'string') {
        return {
          version: parsed.version,
          revision: typeof parsed.revision === 'string' ? parsed.revision : undefined,
        };
      }
    } catch {
      // Fall through to --version.
    }
  }
  // "kex 0.4.0-alpha (abc1234, built 2026-08-21)" — the version is the
  // second word.
  const version = (await run(binary, ['--version']))?.trim().split(/\s+/)[1];
  return version ? { version } : undefined;
}

// Asked of the compiler off the extension host thread: a toolchain on a cold
// network share can take a moment to start, and blocking the UI to print a
// version number in the status bar is not a trade worth making.
function run(binary: string, args: string[]): Promise<string | undefined> {
  return new Promise(resolve => {
    try {
      cp.execFile(binary, args, { timeout: 5000, encoding: 'utf8' },
        (error, stdout) => resolve(error ? undefined : stdout));
    } catch {
      resolve(undefined);
    }
  });
}

/**
 * Semver ordering, newest last — the same rules `Tey.Semver` sorts by, so the
 * picker lists versions in the order `tey kex list` does. A release outranks
 * its own pre-releases, and a version this cannot parse sorts oldest rather
 * than throwing.
 */
export function compareVersions(a: string, b: string): number {
  const left = parseVersion(a);
  const right = parseVersion(b);
  if (!left || !right) {
    if (!left && !right) return a.localeCompare(b);
    return left ? 1 : -1;
  }
  for (let index = 0; index < 3; index += 1) {
    if (left.numbers[index] !== right.numbers[index]) {
      return left.numbers[index] - right.numbers[index];
    }
  }
  if (left.pre.length === 0 && right.pre.length === 0) return 0;
  if (left.pre.length === 0) return 1;
  if (right.pre.length === 0) return -1;
  const shared = Math.min(left.pre.length, right.pre.length);
  for (let index = 0; index < shared; index += 1) {
    const order = comparePreRelease(left.pre[index], right.pre[index]);
    if (order !== 0) return order;
  }
  return left.pre.length - right.pre.length;
}

function comparePreRelease(a: string, b: string): number {
  const left = /^\d+$/.test(a);
  const right = /^\d+$/.test(b);
  if (left && right) return Number(a) - Number(b);
  // Numeric identifiers rank below alphanumeric ones.
  if (left !== right) return left ? -1 : 1;
  return a < b ? -1 : a > b ? 1 : 0;
}

function parseVersion(version: string): { numbers: number[]; pre: string[] } | undefined {
  const match = /^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/
    .exec(version.trim());
  if (!match) return undefined;
  return {
    numbers: [Number(match[1]), Number(match[2]), Number(match[3])],
    pre: match[4] ? match[4].split('.') : [],
  };
}
