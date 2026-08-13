import * as vscode from 'vscode';
import * as path from 'node:path';
import { LanguageClient, LanguageClientOptions, ServerOptions } from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

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
  void client.start();
}

export function deactivate(): Thenable<void> | undefined {
  return client?.stop();
}
