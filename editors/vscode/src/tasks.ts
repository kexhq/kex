// The commands a Kex package declares, offered as VS Code tasks.
//
// A `package.kex` may declare `command("docs", run: "src/main.kex")`, and
// until now the only way to discover one was to type `tey help` in a
// terminal. They are the project-specific half of the tool — the half no
// static list in this extension could know — so they are exactly what a task
// list should carry.
//
// Reading and believing Tey's answer lives in `tey-commands.ts`; this file is
// the VS Code half.

import * as vscode from 'vscode';
import * as cp from 'node:child_process';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { TeyCommand, parseTeyCommands, taskable } from './tey-commands';

interface TeyTaskDefinition extends vscode.TaskDefinition {
  type: 'tey';
  command: string;
}

/**
 * `tey help --json` in `cwd`, or an empty list when there is no tey to ask,
 * it fails, or its answer is not one this extension understands. Never an
 * error: a workspace with no Tey is a normal workspace, and a task list is
 * not the place to learn about it.
 */
function teyCommands(cwd: string): TeyCommand[] {
  try {
    const result = cp.spawnSync('tey', ['help', '--json'], {
      cwd,
      encoding: 'utf8',
      timeout: 5000,
    });
    if (result.status !== 0) return [];
    return parseTeyCommands(result.stdout ?? '') ?? [];
  } catch {
    return [];
  }
}

function groupFor(name: string): vscode.TaskGroup | undefined {
  if (name === 'build') return vscode.TaskGroup.Build;
  if (name === 'test') return vscode.TaskGroup.Test;
  return undefined;
}

function makeTask(command: TeyCommand, folder: vscode.WorkspaceFolder): vscode.Task {
  const definition: TeyTaskDefinition = { type: 'tey', command: command.name };
  const task = new vscode.Task(
    definition,
    folder,
    command.name,
    'tey',
    // Shell rather than process: a command's `run:` is a command line, and
    // the terminal it lands in is the one the user would have typed it into.
    new vscode.ShellExecution('tey', [command.name]),
  );
  task.detail = command.description;
  const group = groupFor(command.name);
  if (group) task.group = group;
  return task;
}

class TeyTaskProvider implements vscode.TaskProvider {
  provideTasks(): vscode.Task[] {
    const folders = vscode.workspace.workspaceFolders ?? [];
    const tasks: vscode.Task[] = [];
    for (const folder of folders) {
      if (folder.uri.scheme !== 'file') continue;
      // No manifest, no package commands — and no reason to spawn tey at all.
      if (!fs.existsSync(path.join(folder.uri.fsPath, 'package.kex'))) continue;
      for (const command of taskable(teyCommands(folder.uri.fsPath))) {
        tasks.push(makeTask(command, folder));
      }
    }
    return tasks;
  }

  // Called for a task the user wrote in tasks.json by hand. The definition
  // carries the command name; everything else is rebuilt the same way.
  resolveTask(task: vscode.Task): vscode.Task | undefined {
    const definition = task.definition as TeyTaskDefinition;
    if (typeof definition.command !== 'string' || definition.command === '') {
      return undefined;
    }
    const resolved = new vscode.Task(
      definition,
      task.scope ?? vscode.TaskScope.Workspace,
      definition.command,
      'tey',
      new vscode.ShellExecution('tey', [definition.command]),
    );
    const group = groupFor(definition.command);
    if (group) resolved.group = group;
    return resolved;
  }
}

export function registerTaskProvider(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.tasks.registerTaskProvider('tey', new TeyTaskProvider()));
}
