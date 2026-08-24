// Reading what `kex --test-json` / `kex --test-list` say about a spec file.
//
// The runner emits one JSON object per line: `item` records for a case it
// discovered, `case` records for one it ran, and a final `summary`. They share
// stdout with whatever the spec itself prints, so a line is a record only if it
// parses as JSON AND carries the `kexTest` tag — anything else is the program's
// own output and belongs to the terminal, not to the test tree.
//
// Nothing here imports vscode: this is the half that decides what to believe,
// and it is tested without an editor (test/unit/test-records.test.ts).

/** Where something is written. Absent when the runner could not say. */
export interface KexTestLocation {
  file: string;
  line: number;    // 1-based, as the compiler counts
  column: number;  // 1-based
}

export interface KexTestItem {
  kexTest: 'item';
  kind: 'describe' | 'it';
  path: string[];
  name: string;
  location?: KexTestLocation;
}

export interface KexTestCase {
  kexTest: 'case';
  path: string[];
  name: string;
  status: 'passed' | 'failed' | 'skipped';
  durationMs: number;
  location?: KexTestLocation;
  failure?: { message: string; location?: KexTestLocation };
}

export interface KexTestSummary {
  kexTest: 'summary';
  passed: number;
  failed: number;
}

export type KexTestRecord = KexTestItem | KexTestCase | KexTestSummary;

/**
 * The separator the runner joins a case's enclosing `describe` labels with —
 * and the one `--test-only` expects back. Kept in one place on this side too,
 * since a filter built with the wrong separator silently runs nothing.
 */
export const TEST_PATH_SEPARATOR = ' > ';

export function joinTestPath(path: readonly string[]): string {
  return path.join(TEST_PATH_SEPARATOR);
}

function isStringArray(value: unknown): value is string[] {
  return Array.isArray(value) && value.every(item => typeof item === 'string');
}

function locationOf(record: Record<string, unknown>): KexTestLocation | undefined {
  const { file, line, column } = record;
  if (typeof file !== 'string' || typeof line !== 'number') return undefined;
  return { file, line, column: typeof column === 'number' ? column : 1 };
}

/**
 * One record, or undefined for a line that is not one. Undefined covers both
 * "this is the spec's own output" and "this is a record shape we do not
 * understand" — an editor that guessed at a half-read record would decorate
 * the wrong line, which is worse than showing nothing.
 */
export function parseTestRecord(line: string): KexTestRecord | undefined {
  const trimmed = line.trim();
  if (!trimmed.startsWith('{')) return undefined;
  let parsed: unknown;
  try {
    parsed = JSON.parse(trimmed);
  } catch {
    return undefined;
  }
  if (typeof parsed !== 'object' || parsed === null) return undefined;
  const record = parsed as Record<string, unknown>;
  switch (record.kexTest) {
    case 'item': {
      if (!isStringArray(record.path) || record.path.length === 0) return undefined;
      if (record.kind !== 'describe' && record.kind !== 'it') return undefined;
      return {
        kexTest: 'item',
        kind: record.kind,
        path: record.path,
        name: typeof record.name === 'string' ? record.name : record.path[record.path.length - 1],
        location: locationOf(record),
      };
    }
    case 'case': {
      if (!isStringArray(record.path) || record.path.length === 0) return undefined;
      const status = record.status;
      if (status !== 'passed' && status !== 'failed' && status !== 'skipped') return undefined;
      const failure = record.failure;
      return {
        kexTest: 'case',
        path: record.path,
        name: typeof record.name === 'string' ? record.name : record.path[record.path.length - 1],
        status,
        durationMs: typeof record.durationMs === 'number' ? record.durationMs : 0,
        location: locationOf(record),
        failure: typeof failure === 'object' && failure !== null
          ? {
              message: String((failure as Record<string, unknown>).message ?? ''),
              location: locationOf(failure as Record<string, unknown>),
            }
          : undefined,
      };
    }
    case 'summary': {
      if (typeof record.passed !== 'number' || typeof record.failed !== 'number') {
        return undefined;
      }
      return { kexTest: 'summary', passed: record.passed, failed: record.failed };
    }
    default:
      return undefined;
  }
}

/** Every record in a chunk of output, in the order the runner emitted them. */
export function parseTestRecords(text: string): KexTestRecord[] {
  const records: KexTestRecord[] = [];
  for (const line of text.split('\n')) {
    const record = parseTestRecord(line);
    if (record) records.push(record);
  }
  return records;
}

/**
 * Feeds a byte stream one line at a time, holding back the partial last line
 * until the rest of it arrives. A run's records are shown as they land, and a
 * record split across two `data` events must not be dropped for it.
 */
export class TestRecordStream {
  private buffer = '';

  constructor(private readonly onRecord: (record: KexTestRecord) => void) {}

  write(chunk: string): void {
    this.buffer += chunk;
    let newline = this.buffer.indexOf('\n');
    while (newline >= 0) {
      const line = this.buffer.slice(0, newline);
      this.buffer = this.buffer.slice(newline + 1);
      const record = parseTestRecord(line);
      if (record) this.onRecord(record);
      newline = this.buffer.indexOf('\n');
    }
  }

  /** The last line, if the process ended without a newline after it. */
  end(): void {
    const record = parseTestRecord(this.buffer);
    this.buffer = '';
    if (record) this.onRecord(record);
  }
}
