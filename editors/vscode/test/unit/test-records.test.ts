// Reading `kex --test-json` / `--test-list` output, without VS Code.
//
// The test tree is built from another program's output on a stream it SHARES
// with the spec under test, so the part worth testing is the part that decides
// what to believe: a line the spec printed must never become a test result, a
// record shape this extension does not understand must be ignored rather than
// half-read, and a record split across two reads must survive.

import { describe, expect, test } from 'bun:test';
import {
  KexTestRecord,
  TestRecordStream,
  joinTestPath,
  parseTestRecord,
  parseTestRecords,
} from '../../src/test-records';

const caseLine = JSON.stringify({
  kexTest: 'case',
  path: ['arithmetic', 'adds'],
  name: 'adds',
  status: 'passed',
  durationMs: 1.5,
  file: '/w/spec/a.spec.kex',
  line: 2,
  column: 3,
});

const failureLine = JSON.stringify({
  kexTest: 'case',
  path: ['arithmetic', 'adds badly'],
  name: 'adds badly',
  status: 'failed',
  durationMs: 0.5,
  file: '/w/spec/a.spec.kex',
  line: 6,
  column: 3,
  failure: {
    message: 'assertion failed: math is broken',
    file: '/w/spec/a.spec.kex',
    line: 7,
    column: 5,
  },
});

describe('parseTestRecord', () => {
  test('reads a case, its path and where it is written', () => {
    expect(parseTestRecord(caseLine)).toEqual({
      kexTest: 'case',
      path: ['arithmetic', 'adds'],
      name: 'adds',
      status: 'passed',
      durationMs: 1.5,
      location: { file: '/w/spec/a.spec.kex', line: 2, column: 3 },
      failure: undefined,
    });
  });

  test('a failure carries its own location, distinct from the case s', () => {
    const record = parseTestRecord(failureLine);
    expect(record?.kexTest).toBe('case');
    if (record?.kexTest !== 'case') return;
    expect(record.location?.line).toBe(6);
    expect(record.failure?.message).toBe('assertion failed: math is broken');
    expect(record.failure?.location).toEqual(
      { file: '/w/spec/a.spec.kex', line: 7, column: 5 });
  });

  test('reads a discovered item and its kind', () => {
    expect(parseTestRecord(JSON.stringify({
      kexTest: 'item', kind: 'describe', path: ['arithmetic'], name: 'arithmetic',
      file: '/w/spec/a.spec.kex', line: 1, column: 1,
    }))).toEqual({
      kexTest: 'item',
      kind: 'describe',
      path: ['arithmetic'],
      name: 'arithmetic',
      location: { file: '/w/spec/a.spec.kex', line: 1, column: 1 },
    });
  });

  test('a record with no location is a record, not a failure to parse', () => {
    const record = parseTestRecord(JSON.stringify({
      kexTest: 'case', path: ['a'], name: 'a', status: 'skipped', durationMs: 0,
    }));
    expect(record?.kexTest).toBe('case');
    if (record?.kexTest !== 'case') return;
    expect(record.location).toBeUndefined();
  });

  // Everything below is not a record. Undefined is the only safe answer: a
  // result built from a half-read line would decorate the wrong line, and the
  // spec's own output shares this stream.
  test('undefined for the spec s own output', () => {
    expect(parseTestRecord('checking 3 files...')).toBeUndefined();
    expect(parseTestRecord('{ not json at all')).toBeUndefined();
    expect(parseTestRecord('')).toBeUndefined();
  });

  test('undefined for JSON that is not a test record', () => {
    expect(parseTestRecord(JSON.stringify({ ok: true }))).toBeUndefined();
    expect(parseTestRecord(JSON.stringify({ kexTest: 'progress' }))).toBeUndefined();
  });

  test('undefined for a record missing what an editor needs', () => {
    // No path: nothing to attach the result to.
    expect(parseTestRecord(JSON.stringify({
      kexTest: 'case', status: 'passed', durationMs: 0 }))).toBeUndefined();
    // A status this extension has no meaning for.
    expect(parseTestRecord(JSON.stringify({
      kexTest: 'case', path: ['a'], status: 'exploded', durationMs: 0 }))).toBeUndefined();
    // An item that is neither a group nor a case.
    expect(parseTestRecord(JSON.stringify({
      kexTest: 'item', kind: 'module', path: ['a'] }))).toBeUndefined();
  });

  test('reads the summary', () => {
    expect(parseTestRecord(JSON.stringify({
      kexTest: 'summary', passed: 2, failed: 1,
    }))).toEqual({ kexTest: 'summary', passed: 2, failed: 1 });
  });
});

describe('parseTestRecords', () => {
  test('keeps the records and drops the noise, in order', () => {
    const output = [
      'a spec may print whatever it likes',
      caseLine,
      '',
      failureLine,
      JSON.stringify({ kexTest: 'summary', passed: 1, failed: 1 }),
    ].join('\n');
    expect(parseTestRecords(output).map(r => r.kexTest)).toEqual(
      ['case', 'case', 'summary']);
  });
});

describe('TestRecordStream', () => {
  test('a record split across two writes still arrives', () => {
    const seen: KexTestRecord[] = [];
    const stream = new TestRecordStream(record => seen.push(record));
    const half = Math.floor(caseLine.length / 2);
    stream.write(caseLine.slice(0, half));
    expect(seen).toHaveLength(0);
    stream.write(`${caseLine.slice(half)}\n`);
    expect(seen).toHaveLength(1);
  });

  test('the last line arrives even with no trailing newline', () => {
    const seen: KexTestRecord[] = [];
    const stream = new TestRecordStream(record => seen.push(record));
    stream.write(caseLine);
    expect(seen).toHaveLength(0);
    stream.end();
    expect(seen).toHaveLength(1);
  });

  test('end() twice does not report the same record twice', () => {
    const seen: KexTestRecord[] = [];
    const stream = new TestRecordStream(record => seen.push(record));
    stream.write(caseLine);
    stream.end();
    stream.end();
    expect(seen).toHaveLength(1);
  });
});

describe('joinTestPath', () => {
  // This is what `--test-only` is given back, so it has to be the separator
  // the runner joined with — a filter built with any other one runs nothing.
  test('joins with the separator the runner uses', () => {
    expect(joinTestPath(['outer', 'inner', 'case'])).toBe('outer > inner > case');
    expect(joinTestPath(['only'])).toBe('only');
    expect(joinTestPath([])).toBe('');
  });
});
