// Downloads (or reuses) a VS Code, lays out a machine for the extension to
// find, and runs test/integration/suite.js inside it.
//
// Everything the extension resolves through is built here from stubs: a Tey
// home with two toolchains, and a Tey package whose lock file names the older
// one. Nothing reads the developer's own ~/.local/share/tey, and nothing needs
// a Kex compiler to exist.

import { runTests } from '@vscode/test-electron';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { makePackage, makeTeyHome } from '../fixtures/tey-home.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
// Normally this extension. Overridable so the same suite can be pointed at
// another build of it — an older checkout, to confirm a test fails on the code
// it was written against.
const extensionDevelopmentPath = process.env.KEX_TEST_EXTENSION
  ? path.resolve(process.env.KEX_TEST_EXTENSION)
  : path.resolve(here, '..', '..');

// macOS caps a unix socket path at 103 characters and VS Code puts one inside
// the user data directory, so this stays short rather than living beside the
// other fixtures.
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'kx-'));
const userData = path.join(scratch, 'u');
const workspace = path.join(scratch, 'workspace');
const stubLog = path.join(scratch, 'starts.log');

// Tey has 0.9.0 selected; the package's lock file names 0.8.0. Which one wins
// is the whole question the resolution order answers.
const teyHome = makeTeyHome(['0.9.0', '0.8.0'], '0.9.0');
makePackage(workspace, { kexVersion: '0.8.0' });
fs.writeFileSync(stubLog, '');

try {
  const code = await runTests({
    extensionDevelopmentPath,
    extensionTestsPath: path.join(here, 'suite.js'),
    launchArgs: [
      workspace,
      '--disable-extensions',
      '--disable-gpu',
      '--user-data-dir', userData,
      '--extensions-dir', path.join(scratch, 'e'),
    ],
    extensionTestsEnv: {
      TEY_HOME: teyHome,
      // A `tey` on PATH would be consulted when nothing else resolves; tests
      // must not depend on whether the machine has one.
      TEY_KEX: '',
      KEX_TEST_WORKSPACE: workspace,
      KEX_TEST_USER_DATA: userData,
      KEX_STUB_LOG: stubLog,
    },
  });
  process.exit(code);
} catch (error) {
  console.error('integration tests failed:', error?.message ?? error);
  process.exit(1);
} finally {
  fs.rmSync(teyHome, { recursive: true, force: true });
  // The scratch directory is left behind on failure: its logs are the only
  // record of what the client did.
  if (process.exitCode === 0) fs.rmSync(scratch, { recursive: true, force: true });
}
