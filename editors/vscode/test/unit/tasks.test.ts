// Reading what `tey help --json` says a workspace can run, without VS Code.
//
// The task list is built from another program's output, so the part worth
// testing is the part that decides what to believe: a Tey that answers with a
// shape this extension does not understand must produce NO tasks rather than
// tasks built from a guess, and a Tey that answers well must not lose the
// package's own commands among the built-ins.

import { describe, expect, test } from 'bun:test';
import { parseTeyCommands, taskable } from '../../src/tey-commands';

const document = (commands: unknown) => JSON.stringify({ commands });

describe('parseTeyCommands', () => {
  test('reads the commands tey lists, keeping their section', () => {
    const parsed = parseTeyCommands(document([
      { name: 'build', usage: '', description: 'compile the package', section: '' },
      { name: 'docs', usage: '[args...]', description: 'Generate docs', section: 'Package commands' },
    ]));
    expect(parsed).toEqual([
      { name: 'build', usage: '', description: 'compile the package', section: '' },
      { name: 'docs', usage: '[args...]', description: 'Generate docs', section: 'Package commands' },
    ]);
  });

  test('an empty list is an answer, not a failure', () => {
    expect(parseTeyCommands(document([]))).toEqual([]);
  });

  // Everything below is a Tey this extension does not understand. Undefined
  // is the only safe answer: a task built from a half-read entry would run
  // the wrong command line.
  test('undefined for output that is not JSON', () => {
    expect(parseTeyCommands('tey: unknown flag --json')).toBeUndefined();
  });

  test('undefined when the document carries no commands array', () => {
    expect(parseTeyCommands(JSON.stringify({ toolchains: [] }))).toBeUndefined();
    expect(parseTeyCommands(JSON.stringify([]))).toBeUndefined();
  });

  test('undefined when an entry has no usable name', () => {
    expect(parseTeyCommands(document([{ usage: '', description: 'nameless' }]))).toBeUndefined();
    expect(parseTeyCommands(document([{ name: '' }]))).toBeUndefined();
    expect(parseTeyCommands(document([{ name: 42 }]))).toBeUndefined();
  });

  // A named command with the rest missing is still runnable — `tey docs`
  // works whether or not the manifest described it — so the optional fields
  // default instead of rejecting the entry.
  test('a command with only a name keeps its defaults', () => {
    expect(parseTeyCommands(document([{ name: 'docs' }]))).toEqual([
      { name: 'docs', usage: '', description: '', section: '' },
    ]);
  });
});

describe('taskable', () => {
  const commands = [
    { name: 'new', usage: '<name>', description: 'create a package', section: '' },
    { name: 'build', usage: '', description: 'compile the package', section: '' },
    { name: 'test', usage: '', description: 'run the specs', section: '' },
    { name: 'kex which', usage: '', description: 'print the compiler', section: '' },
    { name: 'docs', usage: '[args...]', description: 'Generate docs', section: 'Package commands' },
  ];

  test('keeps every command the package declares', () => {
    expect(taskable(commands).map(command => command.name)).toContain('docs');
  });

  test('keeps build and test, which map to VS Code task groups', () => {
    const names = taskable(commands).map(command => command.name);
    expect(names).toContain('build');
    expect(names).toContain('test');
  });

  // Otherwise every workspace's task list carries Tey's whole vocabulary,
  // which says nothing about THIS project.
  test('drops the built-ins that are the same everywhere', () => {
    const names = taskable(commands).map(command => command.name);
    expect(names).not.toContain('new');
    expect(names).not.toContain('kex which');
  });
});
