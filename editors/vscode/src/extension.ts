import * as vscode from 'vscode';
import * as path from 'node:path';
import { LanguageClient, LanguageClientOptions, ServerOptions } from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let restartTimer: NodeJS.Timeout | undefined;

function restartLanguageServer(): void {
  if (restartTimer) clearTimeout(restartTimer);
  restartTimer = setTimeout(() => {
    restartTimer = undefined;
    void client?.restart().catch(error => {
      void vscode.window.showErrorMessage(`Failed to restart Kex language server: ${String(error)}`);
    });
  }, 300);
}

function resolveExecutable(configured: string): string {
  const workspace = vscode.workspace.workspaceFolders?.[0]?.uri;
  if (!workspace || workspace.scheme !== 'file') {
    return configured;
  }

  const expanded = configured.replaceAll('${workspaceFolder}', workspace.fsPath);
  const isPath = expanded !== configured || expanded.startsWith('.') ||
    expanded.includes('/') || expanded.includes('\\');
  return isPath && !path.isAbsolute(expanded)
    ? path.resolve(workspace.fsPath, expanded)
    : expanded;
}

export function activate(context: vscode.ExtensionContext): void {
  const configuredPath = vscode.workspace.getConfiguration('kex').get<string>('executablePath', 'kex');
  const kexPath = resolveExecutable(configuredPath);
  const serverOptions: ServerOptions = {
    run: { command: kexPath, args: ['--lsp'] },
    debug: { command: kexPath, args: ['--lsp'] },
  };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'kex' }],
    synchronize: { fileEvents: vscode.workspace.createFileSystemWatcher('**/*.kex') },
  };
  client = new LanguageClient('kex', 'Kex Language Server', serverOptions, clientOptions);
  context.subscriptions.push(client);
  context.subscriptions.push(vscode.commands.registerCommand(
    'kex.restartLanguageServer', restartLanguageServer));

  const workspace = vscode.workspace.workspaceFolders?.[0]?.uri;
  if (workspace?.scheme === 'file' && path.isAbsolute(kexPath)) {
    const relative = path.relative(workspace.fsPath, kexPath);
    if (relative && !relative.startsWith('..') && !path.isAbsolute(relative)) {
      const executableWatcher = vscode.workspace.createFileSystemWatcher(
        new vscode.RelativePattern(workspace, relative));
      executableWatcher.onDidChange(restartLanguageServer);
      executableWatcher.onDidCreate(restartLanguageServer);
      context.subscriptions.push(executableWatcher);
    }
  }
  void client.start();
}

export function deactivate(): Thenable<void> | undefined {
  if (restartTimer) clearTimeout(restartTimer);
  return client?.stop();
}
