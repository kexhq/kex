// What `tey help --json` says a workspace can run.
//
// Kept apart from `tasks.ts` — and from `vscode` — because this half is pure:
// text in, commands out. It is the half that decides what to believe about
// another program's output, so it is the half worth testing directly.
//
// The commands are read from Tey rather than by parsing `package.kex` here:
// the manifest is a Kex program that Tey interprets, and a second reader
// written in TypeScript would drift from it the first time the vocabulary
// grows. Tey answers with the same list its help text renders, each entry
// carrying the `section` that separates a package's own commands from the
// built-ins.

// The heading Tey files a package's own commands under. Built-ins carry "".
export const PACKAGE_SECTION = 'Package commands';

export interface TeyCommand {
  name: string;
  usage: string;
  description: string;
  section: string;
}

/**
 * The document `tey help --json` prints, as commands. Undefined rather than a
 * best effort for anything off about it: a wrong shape is a Tey this
 * extension does not understand, and offering tasks built from a guess is
 * worse than offering none.
 */
export function parseTeyCommands(text: string): TeyCommand[] | undefined {
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    return undefined;
  }
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    return undefined;
  }
  const listed = (parsed as { commands?: unknown }).commands;
  if (!Array.isArray(listed)) return undefined;
  const commands: TeyCommand[] = [];
  for (const entry of listed) {
    if (typeof entry !== 'object' || entry === null) return undefined;
    const { name, usage, description, section } = entry as Record<string, unknown>;
    if (typeof name !== 'string' || name === '') return undefined;
    commands.push({
      name,
      usage: typeof usage === 'string' ? usage : '',
      description: typeof description === 'string' ? description : '',
      section: typeof section === 'string' ? section : '',
    });
  }
  return commands;
}

/**
 * Which of a workspace's commands become tasks: everything the package
 * declares, plus `build` and `test`, the two built-ins VS Code has dedicated
 * task groups for. The rest of Tey's vocabulary is the same in every
 * workspace and belongs in a terminal, not in a per-project task list.
 */
export function taskable(commands: TeyCommand[]): TeyCommand[] {
  return commands.filter(command =>
    command.section === PACKAGE_SECTION ||
    command.name === 'build' ||
    command.name === 'test');
}
