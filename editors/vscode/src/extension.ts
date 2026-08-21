import * as vscode from 'vscode';
import * as fs from 'node:fs';
import * as path from 'node:path';
import {
  CloseAction, CloseHandlerResult, ErrorAction, ErrorHandlerResult,
  LanguageClient, LanguageClientOptions, Message, ServerOptions,
} from 'vscode-languageclient/node';
import {
  InstalledToolchain, ResolvedToolchain, TEY_FOLLOW, installedToolchains,
  isExecutableFile, kexInfo, lockedVersion, resolveToolchain, teySelectedVersion,
} from './toolchain';

let client: LanguageClient | undefined;
let restartTimer: NodeJS.Timeout | undefined;
let executableWatcher: fs.FSWatcher | undefined;
let status: vscode.StatusBarItem;
let currentBinary: string | undefined;

// A compiler under development is rebuilt constantly, and every rebuild is a
// server that goes away mid-request. vscode-languageclient's default budget —
// five crashes in three minutes, then silence until the window is reloaded —
// is spent by an afternoon of `cmake --build`. This one is far larger and
// backs off instead of surrendering, and a healthy run clears it.
const MAX_AUTOMATIC_RESTARTS = 12;
const HEALTHY_AFTER_MS = 60_000;
const BACKOFF_MS = [500, 1000, 2000, 4000, 8000, 15_000];

let automaticRestarts = 0;
let startedAt = 0;
// Bumped on every start. Anything that finishes asynchronously carries the
// generation it was started for and stays quiet once a newer one has begun:
// `kex --info` takes long enough that its answer can land after the server it
// describes has already died, and "running" written over an error is worse
// than no status at all.
let generation = 0;

function backoffFor(attempt: number): number {
  return BACKOFF_MS[Math.min(attempt, BACKOFF_MS.length - 1)];
}

/** The folder settings and relative paths are resolved against. */
function workspaceFolder(): vscode.Uri | undefined {
  const folder = vscode.workspace.workspaceFolders?.[0]?.uri;
  return folder?.scheme === 'file' ? folder : undefined;
}

// ---------------------------------------------------------------- lifecycle

/**
 * Stops whatever is running and starts a NEW client.
 *
 * Not `client.restart()`: that rejects once the client has reached `Stopped`
 * or `StartFailed`, which is exactly the state a rebuild leaves it in, so the
 * one command meant to recover from a dead server was the one thing that
 * could not. A fresh `LanguageClient` always can, and re-resolving on the way
 * means a toolchain switch takes effect here too.
 */
async function startLanguageServer(context: vscode.ExtensionContext): Promise<void> {
  const previous = client;
  client = undefined;
  if (previous) {
    // Best effort: a server that already died cannot be asked politely.
    await previous.stop(2000).catch(() => undefined);
    await previous.dispose(2000).catch(() => undefined);
  }

  const resolved = resolveToolchain(workspaceFolder());
  currentBinary = resolved.command;
  watchExecutable(resolved.command, context);
  showStarting();

  const serverOptions: ServerOptions = {
    run: { command: resolved.command, args: ['--lsp'] },
    debug: { command: resolved.command, args: ['--lsp'] },
  };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'kex' }],
    synchronize: { fileEvents: vscode.workspace.createFileSystemWatcher('**/*.kex') },
    errorHandler: {
      error(_error: Error, _message: Message | undefined, count: number | undefined): ErrorHandlerResult {
        // Protocol errors are not a dead server. Only give up once they are
        // relentless, and let the close handler deal with an actual exit.
        if ((count ?? 0) <= 10) return { action: ErrorAction.Continue, handled: true };
        // Shutting down this client stops it from the inside, which means the
        // close handler below will not see it — so the restart is queued from
        // here instead, or the extension goes quiet with nobody to notice.
        scheduleRestart(context);
        return { action: ErrorAction.Shutdown, handled: true };
      },
      closed(): CloseHandlerResult {
        // The client's own restart is declined so the backoff and the fresh
        // client below are the single path back — `Restart` here would loop
        // immediately on a binary that is halfway through being written.
        scheduleRestart(context);
        return { action: CloseAction.DoNotRestart, handled: true };
      },
    },
  };

  const started = new LanguageClient('kex', 'Kex Language Server', serverOptions, clientOptions);
  const mine = (generation += 1);
  client = started;
  startedAt = Date.now();
  try {
    await started.start();
    if (mine === generation) void showRunning(resolved, started, mine);
  } catch (error) {
    if (mine === generation) showFailed(resolved, error);
  }
}

/**
 * Queues a restart, debounced.
 *
 * `manual` is a user asking, which always runs and clears the budget: they
 * can see the server is down, and refusing them because an automatic budget
 * is spent is how the extension ends up inert with a working compiler on
 * disk.
 */
function scheduleRestart(context: vscode.ExtensionContext, manual = false): void {
  if (manual) automaticRestarts = 0;

  if (!manual) {
    // A run that lasted means the previous trouble is over; anything after
    // it is new trouble and gets the full budget again.
    if (startedAt !== 0 && Date.now() - startedAt > HEALTHY_AFTER_MS) automaticRestarts = 0;
    if (automaticRestarts >= MAX_AUTOMATIC_RESTARTS) {
      showStopped('the language server keeps stopping');
      return;
    }
  }

  const delay = manual ? 300 : backoffFor(automaticRestarts);
  if (!manual) automaticRestarts += 1;

  if (restartTimer) clearTimeout(restartTimer);
  restartTimer = setTimeout(() => {
    restartTimer = undefined;
    void startLanguageServer(context);
  }, delay);
}

/**
 * Watches the resolved binary for replacement.
 *
 * `fs.watch` on its DIRECTORY rather than a workspace file watcher: the
 * compiler usually lives outside the workspace (a Tey toolchain, or `kex` on
 * PATH), where `createFileSystemWatcher` does not look, and a rebuild
 * unlinks and recreates the file, which a watch on the file itself stops
 * following. The old watcher only ever existed for an absolute path inside
 * the workspace, so the common case — the default bare `kex` — had none.
 */
function watchExecutable(binary: string, context: vscode.ExtensionContext): void {
  executableWatcher?.close();
  executableWatcher = undefined;
  if (!path.isAbsolute(binary)) return;

  const directory = path.dirname(binary);
  const name = path.basename(binary);
  let debounce: NodeJS.Timeout | undefined;
  try {
    executableWatcher = fs.watch(directory, (_event, changed) => {
      if (changed !== null && changed !== undefined && changed !== name) return;
      if (debounce) clearTimeout(debounce);
      // A build writes the file in pieces; restarting on the first byte
      // starts a server against a truncated binary.
      debounce = setTimeout(() => {
        debounce = undefined;
        if (isExecutableFile(binary)) scheduleRestart(context, true);
      }, 500);
    });
  } catch {
    // An unwatchable directory (it does not exist yet, or the platform is out
    // of watches) is not worth a message: the manual restart command and the
    // status bar still work.
    return;
  }
}

// --------------------------------------------------------------- status bar

function showStarting(): void {
  status.text = '$(sync~spin) Kex';
  status.tooltip = 'Starting the Kex language server…';
  status.backgroundColor = undefined;
  status.command = 'kex.selectToolchain';
  status.show();
}

async function showRunning(
  resolved: ResolvedToolchain, started: LanguageClient, mine: number,
): Promise<void> {
  const binary = resolved.command;
  const info = await kexInfo(binary);
  // The status bar names the compiler that is ACTUALLY running, not the
  // setting that chose it: with a version pinned per workspace and another
  // selected machine-wide, the setting is the one thing that cannot answer
  // "which Kex am I getting here".
  //
  // Only if that server is still the current one AND still up, though: this
  // resumes after an await, by which time it may have been replaced, or have
  // exited on its own — a compiler that refuses to start still answers
  // `--info` perfectly well, which is how a dead server came to be labelled
  // as running.
  if (mine !== generation || client !== started || !started.isRunning()) return;

  const version = info ? `Kex ${info.version}` : 'Kex';
  const label = resolved.origin.kind === 'setting' || resolved.origin.kind === 'custom'
    ? `${version} (${displayPath(binary)})`
    : version;
  status.text = label;
  status.tooltip = new vscode.MarkdownString(
    `Kex language server running\n\n` +
    `- Compiler: \`${binary}\`\n` +
    `- Source: ${describeOrigin(resolved)}\n\n` +
    `Click to select a toolchain.`);
  status.backgroundColor = undefined;
  status.command = 'kex.selectToolchain';
  status.show();
}

function showStopped(reason: string): void {
  status.text = '$(error) Kex — server stopped';
  status.tooltip = `${reason}. Click to restart it.`;
  status.backgroundColor = new vscode.ThemeColor('statusBarItem.errorBackground');
  status.command = 'kex.restartLanguageServer';
  status.show();
}

function showFailed(resolved: ResolvedToolchain, error: unknown): void {
  showStopped(`Kex language server failed to start (${resolved.command})`);
  // Spawn failures land here as ENOENT on macOS/Linux. Getting the user from
  // "installed the extension" to "installed a compiler" is the one actionable
  // message, so say it plainly instead of leaving it in the output channel.
  const message = String((error as Error | undefined)?.message ?? error);
  void vscode.window.showErrorMessage(
    `Kex language server failed to start: ${message}. Install a Kex with ` +
    `'brew install kexhq/tey/tey', pick one with 'Kex: Select Toolchain', or ` +
    `set kex.executablePath.`,
    'Show Output', 'Select Toolchain', 'Restart Language Server',
  ).then(choice => {
    // The compiler's own reason for refusing — a stale prebuilt stdlib, a
    // runtime it cannot find — is printed there and nowhere else.
    if (choice === 'Show Output') client?.outputChannel.show(true);
    if (choice === 'Select Toolchain') void vscode.commands.executeCommand('kex.selectToolchain');
    if (choice === 'Restart Language Server') void vscode.commands.executeCommand('kex.restartLanguageServer');
  });
}

function describeOrigin(resolved: ResolvedToolchain): string {
  switch (resolved.origin.kind) {
    case 'setting': return '`kex.executablePath`';
    case 'custom': return '`kex.toolchain`, a binary of your own';
    case 'toolchain': return '`kex.toolchain`, a Tey toolchain';
    case 'lock': return "`tey.lock`, this package's Kex";
    case 'tey': return "Tey's selection";
    case 'path': return '`kex` on PATH';
  }
}

/** A path short enough for the status bar: relative to the workspace if inside it. */
function displayPath(binary: string): string {
  const folder = workspaceFolder();
  if (folder) {
    const relative = path.relative(folder.fsPath, binary);
    if (relative && !relative.startsWith('..') && !path.isAbsolute(relative)) return relative;
  }
  return path.basename(binary);
}

// ------------------------------------------------------------------- picker

interface ToolchainPick extends vscode.QuickPickItem {
  /** What to write into `kex.toolchain`; undefined for a cancelled pick. */
  value?: string;
  browse?: boolean;
}

/** A tick against the entry currently in force, so the list says where you are. */
function mark(active: boolean): string {
  return active ? '$(check) ' : '$(blank) ';
}

/**
 * Lists what Tey has installed and lets the user pick one, plus the entries
 * that are not versions: follow the package, follow Tey, and a binary of your
 * own.
 *
 * The choice is written to the WORKSPACE, so a Kex checkout can run its own
 * `build/kex` without changing which compiler the rest of the machine uses.
 */
async function selectToolchain(context: vscode.ExtensionContext): Promise<void> {
  const config = vscode.workspace.getConfiguration('kex');
  const pinned = (config.get<string>('toolchain') ?? '').trim();
  const installed = installedToolchains();
  const teySelection = teySelectedVersion();

  const locked = lockedVersion(workspaceFolder());

  const items: ToolchainPick[] = [];
  // The default entry. With a `tey.lock` in the workspace it means this
  // package's Kex, which is the one the package declares it builds against;
  // without one it is Tey's selection. Both are "whatever is right here",
  // which is why they are one choice rather than two.
  items.push({
    label: `${mark(pinned === '')}Automatic`,
    description: locked
      ? `Kex ${locked} · tey.lock`
      : (teySelection ? `Kex ${teySelection} · Tey` : 'no Kex found'),
    detail: locked ? "This package's tey.lock, then Tey" : "Follows tey kex use",
    value: '',
  });
  // Only worth offering when it differs from Automatic: a way to ignore a
  // lock file that pins a Kex you do not want to edit against.
  if (locked) {
    items.push({
      label: `${mark(pinned === TEY_FOLLOW)}Use Tey's selection`,
      description: teySelection ? `Kex ${teySelection}` : 'nothing selected',
      detail: 'Ignores tey.lock',
      value: TEY_FOLLOW,
    });
  }

  if (installed.length > 0) {
    items.push({ label: 'Installed toolchains', kind: vscode.QuickPickItemKind.Separator });
    for (const toolchain of installed) {
      items.push(toolchainItem(toolchain, pinned));
    }
  }

  items.push({ label: '', kind: vscode.QuickPickItemKind.Separator });
  items.push({
    label: '$(folder-opened) Choose a Kex binary…',
    detail: 'Your own build, e.g. build/kex',
    browse: true,
  });
  if (pinned !== '' && pinned !== TEY_FOLLOW && !installed.some(t => t.version === pinned)) {
    items.push({
      label: `$(warning) Kex ${pinned}`,
      description: 'pinned, not installed',
      value: pinned,
    });
  }

  const choice = await vscode.window.showQuickPick(items, {
    title: 'Select the Kex toolchain for this workspace',
    placeHolder: 'Which Kex should the language server run?',
  });
  if (!choice) return;

  let value = choice.value;
  if (choice.browse) {
    const picked = await vscode.window.showOpenDialog({
      title: 'Select a Kex compiler',
      openLabel: 'Use this Kex',
      canSelectMany: false,
      defaultUri: workspaceFolder(),
    });
    const file = picked?.[0];
    if (!file || file.scheme !== 'file') return;
    if (!isExecutableFile(file.fsPath)) {
      void vscode.window.showErrorMessage(`${file.fsPath} is not an executable file.`);
      return;
    }
    value = file.fsPath;
  }
  if (value === undefined) return;

  const target = vscode.workspace.workspaceFolders?.length
    ? vscode.ConfigurationTarget.Workspace
    : vscode.ConfigurationTarget.Global;
  // Unset rather than an empty string, so the setting reads as "not chosen"
  // in the settings UI and a workspace value stops shadowing a global one.
  await config.update('toolchain', value === '' ? undefined : value, target);

  // `kex.executablePath` outranks the picker, so a stale one set long ago
  // would silently win over the version just chosen.
  const override = config.inspect<string>('executablePath');
  const overridden = override?.workspaceFolderValue ?? override?.workspaceValue ?? override?.globalValue;
  if (overridden !== undefined && overridden.trim() !== '') {
    // Telling someone to clear a setting the extension is already holding is
    // a chore handed back to them, and the notification gear cannot be relied
    // on to find this extension's settings — so the buttons do both jobs.
    void vscode.window.showWarningMessage(
      `kex.executablePath ('${overridden}') overrides the toolchain you picked.`,
      'Clear it', 'Open Setting',
    ).then(async choice => {
      if (choice === 'Open Setting') {
        void vscode.commands.executeCommand('workbench.action.openSettings', 'kex.executablePath');
        return;
      }
      if (choice !== 'Clear it') return;
      // Cleared where it was set: clearing the global value would leave a
      // workspace one still winning, and look like the button did nothing.
      const scope = override?.workspaceFolderValue !== undefined
        ? vscode.ConfigurationTarget.WorkspaceFolder
        : override?.workspaceValue !== undefined
          ? vscode.ConfigurationTarget.Workspace
          : vscode.ConfigurationTarget.Global;
      await config.update('executablePath', undefined, scope);
      scheduleRestart(context, true);
    });
  }

  scheduleRestart(context, true);
}

function toolchainItem(toolchain: InstalledToolchain, pinned: string): ToolchainPick {
  const notes: string[] = [];
  if (toolchain.selected) notes.push("Tey's");
  if (toolchain.version === pinned) notes.push('pinned');
  return {
    label: `${mark(toolchain.version === pinned)}Kex ${toolchain.version}`,
    description: notes.join(', '),
    detail: toolchain.binary,
    value: toolchain.version,
  };
}

// --------------------------------------------------------------- activation

export function activate(context: vscode.ExtensionContext): void {
  status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
  status.name = 'Kex';
  context.subscriptions.push(status);
  // One disposable for the watcher rather than one per restart: the binary
  // being watched changes, the obligation to close it does not.
  context.subscriptions.push({ dispose: () => executableWatcher?.close() });

  context.subscriptions.push(vscode.commands.registerCommand(
    'kex.restartLanguageServer', () => scheduleRestart(context, true)));
  context.subscriptions.push(vscode.commands.registerCommand(
    'kex.selectToolchain', () => selectToolchain(context)));

  // Changing which compiler to run is a request to run it.
  context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(event => {
    if (event.affectsConfiguration('kex.executablePath') ||
        event.affectsConfiguration('kex.toolchain')) {
      const resolved = resolveToolchain(workspaceFolder());
      if (resolved.command !== currentBinary) scheduleRestart(context, true);
    }
  }));

  void startLanguageServer(context);
}

export function deactivate(): Thenable<void> | undefined {
  if (restartTimer) clearTimeout(restartTimer);
  executableWatcher?.close();
  // A client that already died rejects rather than stopping, and an unhandled
  // rejection on the way out is noise about a server that is going away
  // anyway.
  return client?.stop().catch(() => undefined);
}
