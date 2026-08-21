// The extension, in a real VS Code.
//
// What is checked here cannot be checked any other way: whether the client
// actually starts, whether it comes back after its compiler is replaced or
// killed, and whether the picker writes what it says it writes. The compiler
// is a stub (test/fixtures/kex-stub.mjs) laid out in a fake Tey home, so this
// depends on nothing installed on the machine.

const vscode = require('vscode');
const fs = require('node:fs');
const path = require('node:path');

const WORKSPACE = process.env.KEX_TEST_WORKSPACE;
const TEY_HOME = process.env.TEY_HOME;
const STUB_LOG = process.env.KEX_STUB_LOG;
const USER_DATA = process.env.KEX_TEST_USER_DATA;

const log = (...parts) => console.log('[kex-test]', ...parts);
const wait = (ms) => new Promise(resolve => setTimeout(resolve, ms));

async function waitFor(what, predicate, timeout = 60_000, step = 250) {
  const deadline = Date.now() + timeout;
  let last;
  while (Date.now() < deadline) {
    try {
      last = await predicate();
      if (last) return last;
    } catch (error) {
      last = error;
    }
    await wait(step);
  }
  throw new Error(`timed out waiting for ${what} (last saw: ${String(last).slice(0, 120)})`);
}

function assert(condition, message) {
  if (!condition) throw new Error(`FAILED: ${message}`);
  log('  ok —', message);
}

const probeUri = () => vscode.Uri.file(path.join(WORKSPACE, 'src', 'probe.kex'));
const toolchainBinary = (version) => path.join(TEY_HOME, 'toolchains', version, 'bin', 'kex');

/** The hover text carries the version of whichever stub answered. */
async function hover() {
  const hovers = await vscode.commands.executeCommand(
    'vscode.executeHoverProvider', probeUri(), new vscode.Position(2, 6));
  if (!hovers || hovers.length === 0) return undefined;
  return hovers
    .map(one => one.contents.map(part => part.value ?? String(part)).join(''))
    .join('')
    // Hover contents come back as rendered markdown, where a run of spaces is
    // non-breaking ones.
    .replace(/&nbsp;/g, ' ')
    .trim();
}

/** How many servers have ever started, from the stub's own record. */
function starts() {
  try {
    return fs.readFileSync(STUB_LOG, 'utf8').split('\n').filter(Boolean);
  } catch {
    return [];
  }
}

/** Every "Kex Language Server" output channel VS Code has persisted. */
function outputChannels() {
  const found = [];
  const walk = (dir) => {
    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) walk(full);
      else if (entry.name.includes('Kex Language Server')) found.push(full);
    }
  };
  walk(path.join(USER_DATA, 'logs'));
  return found;
}

async function setToolchain(value) {
  await vscode.workspace.getConfiguration('kex')
    .update('toolchain', value, vscode.ConfigurationTarget.Workspace);
}

const tests = [];
const it = (name, run) => tests.push({ name, run });

// --------------------------------------------------------------------------

it('activates on a .kex file and registers its commands', async () => {
  const extension = await waitFor('the extension to activate', () => {
    const found = vscode.extensions.getExtension('Kex.kex-language');
    return found && found.isActive ? found : false;
  });
  assert(extension.isActive, 'the extension is active');
  const commands = await vscode.commands.getCommands(true);
  assert(commands.includes('kex.selectToolchain'), 'Kex: Select Toolchain is registered');
  assert(commands.includes('kex.restartLanguageServer'),
    'Kex: Restart Language Server is registered');
});

// The workspace is a Tey package whose tey.lock names 0.8.0, while Tey itself
// has 0.9.0 selected. The package must win: it is the compiler the package
// says it builds against.
it("starts the compiler this package's tey.lock names", async () => {
  const answer = await waitFor('the first hover', hover, 90_000);
  log('  hover says:', answer);
  assert(answer.includes('kex-stub 0.8.0'),
    `the running server is the tey.lock version, not Tey's selection (${answer})`);
});

it('restarts when its compiler is rebuilt underneath it', async () => {
  const before = starts().length;
  const binary = toolchainBinary('0.8.0');
  // What a build does: unlink, then write a new file in its place.
  const contents = fs.readFileSync(binary);
  fs.unlinkSync(binary);
  await wait(300);
  fs.writeFileSync(binary, contents);
  fs.chmodSync(binary, 0o755);
  const after = await waitFor('a new server after the rebuild',
    () => starts().length > before, 60_000);
  assert(after, `the rebuild started a new server (${before} → ${starts().length})`);
  assert((await waitFor('hover after the rebuild', hover, 90_000)).includes('0.8.0'),
    'hover works again after the rebuild');
});

it('comes back after the server dies', async () => {
  const before = starts();
  const pid = Number(before[before.length - 1].split(' ')[2]);
  process.kill(pid, 'SIGKILL');
  log('  killed server pid', pid);
  await waitFor('a new server after the crash', () => starts().length > before.length, 90_000);
  const answer = await waitFor('hover after the crash', hover, 90_000);
  assert(answer.includes('0.8.0'), 'the error handler brought the server back');
});

it('restarts on the toolchain a setting names', async () => {
  await setToolchain('0.9.0');
  const answer = await waitFor('hover from 0.9.0',
    async () => { const value = await hover(); return value?.includes('0.9.0') ? value : false; },
    90_000);
  assert(answer.includes('kex-stub 0.9.0'), 'the server now runs the pinned version');
  await setToolchain(undefined);
  await waitFor('hover from the lock file again',
    async () => (await hover())?.includes('0.8.0'), 90_000);
});

// The picker is UI, so it is driven the way a user drives it: open it, walk
// down, accept. One step past the default entry is "Use Tey's selection",
// which exists only because this workspace has a tey.lock — landing on it is
// itself proof the list was built lock-aware.
it('writes the picked toolchain into the workspace and restarts on it', async () => {
  void vscode.commands.executeCommand('kex.selectToolchain');
  await wait(2000);
  await vscode.commands.executeCommand('workbench.action.quickOpenSelectNext');
  await wait(250);
  await vscode.commands.executeCommand('workbench.action.acceptSelectedQuickOpenItem');

  const value = await waitFor('the setting to be written',
    () => vscode.workspace.getConfiguration('kex').inspect('toolchain').workspaceValue ?? false,
    20_000);
  log('  picked:', JSON.stringify(value));
  assert(fs.existsSync(path.join(WORKSPACE, '.vscode', 'settings.json')),
    `the choice (${value}) landed in .vscode/settings.json`);

  // Which entry the keystrokes land on depends on how the list is laid out,
  // and pinning the test to a position would make it fail every time an entry
  // is added. What must hold is the rule: whatever was written is what now
  // runs — a version runs that version, and "use Tey's selection" runs Tey's.
  const expected = value === 'tey' ? '0.9.0' : value;
  const answer = await waitFor(`the server to be running ${expected}`,
    async () => { const text = await hover(); return text?.includes(expected) ? text : false; },
    90_000);
  assert(answer.includes(`kex-stub ${expected}`),
    `picking '${value}' restarted the server on ${expected}`);
  await setToolchain(undefined);
});

// Every restart above built a fresh LanguageClient. Each one that makes its
// own output channel leaves a dead "Kex Language Server" behind in the Output
// dropdown, so by now there would be a pile of them.
it('keeps a single output channel across every restart', async () => {
  const channels = outputChannels();
  log('  output channels:', channels.length);
  assert(channels.length === 1,
    `exactly one output channel survives every restart (found ${channels.length})`);
});

// --------------------------------------------------------------------------

exports.run = async function run() {
  const document = await vscode.workspace.openTextDocument(probeUri());
  await vscode.window.showTextDocument(document);

  const failures = [];
  for (const { name, run: runTest } of tests) {
    log(name);
    try {
      await runTest();
    } catch (error) {
      failures.push(`${name}: ${error?.message ?? error}`);
      log('  FAILED —', error?.message ?? error);
    }
  }

  log(`${tests.length - failures.length}/${tests.length} passed`);
  if (failures.length > 0) {
    throw new Error(`${failures.length} failing:\n  ${failures.join('\n  ')}`);
  }
};
