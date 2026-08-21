// Builds a Tey home out of stub compilers.
//
// The layout is the one `tey/src/tey/toolchain.kex` writes and
// `src/toolchain.ts` reads: `toolchains/<version>/bin/kex`, plus a `current`
// file naming the selected version. Tests point `TEY_HOME` at one of these
// and get a machine with exactly the toolchains they asked for.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
export const STUB = path.join(here, 'kex-stub.mjs');

/** A shell wrapper, because `bin/kex` has to be something the OS can exec. */
export function writeStubBinary(binary, { broken = false } = {}) {
  fs.mkdirSync(path.dirname(binary), { recursive: true });
  const broke = broken ? 'KEX_STUB_BROKEN=1 ' : '';
  fs.writeFileSync(binary,
    `#!/bin/sh\n${broke}KEX_STUB_PATH="$0" exec ${process.execPath} ${STUB} "$@"\n`);
  fs.chmodSync(binary, 0o755);
  return binary;
}

/**
 * @param versions installed toolchain versions, e.g. ['0.9.0', '0.8.1']
 * @param selected what `current` names; the newest by default
 */
export function makeTeyHome(versions, selected = versions[0]) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'kex-tey-home-'));
  for (const version of versions) {
    writeStubBinary(path.join(home, 'toolchains', version, 'bin', 'kex'));
  }
  // Staging leftovers, which the extension must not offer as installations.
  fs.mkdirSync(path.join(home, 'toolchains', '9.9.9.partial'), { recursive: true });
  fs.mkdirSync(path.join(home, 'toolchains', '9.9.8.previous'), { recursive: true });
  if (selected !== undefined) fs.writeFileSync(path.join(home, 'current'), `${selected}\n`);
  return home;
}

/** A Tey package: what makes the extension follow a lock file. */
export function makePackage(root, { kexVersion, name = 'probe' } = {}) {
  fs.mkdirSync(path.join(root, 'src'), { recursive: true });
  fs.writeFileSync(path.join(root, 'package.kex'),
    `bundle "${name}" do\n  version("0.1.0")\n  kex(">= ${kexVersion}")\nend\n`);
  fs.writeFileSync(path.join(root, 'tey.lock'), JSON.stringify({
    version: 1,
    kex: { requirement: `>= ${kexVersion}`, version: kexVersion },
    otp: { requirement: '', release: 29 },
    deps: {},
  }, null, 2) + '\n');
  fs.writeFileSync(path.join(root, 'src', 'probe.kex'),
    'module Probe\n\nlet greeting(name: String) = "Hello, ${name}!"\n');
  return root;
}
