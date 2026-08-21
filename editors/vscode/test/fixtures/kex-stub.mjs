// A Kex compiler, for tests only.
//
// The extension's job is to find the right compiler, start it, notice when it
// dies and start it again. None of that needs a real Kex — it needs something
// that answers `--info`, `--version` and `--lsp` the way one does. Using a
// real toolchain would tie these tests to whatever the machine happens to have
// installed, and a real build would tie them to a compiler that takes minutes
// to produce.
//
// The version is read from its own path (`toolchains/<version>/bin/kex`), so
// one script backs as many fake toolchains as a test wants to lay out.

import process from 'node:process';
import path from 'node:path';
import fs from 'node:fs';

function version() {
  if (process.env.KEX_STUB_VERSION) return process.env.KEX_STUB_VERSION;
  // .../toolchains/<version>/bin/kex → <version>
  const parts = (process.env.KEX_STUB_PATH ?? process.argv[1]).split(path.sep);
  const index = parts.lastIndexOf('toolchains');
  return index >= 0 && parts[index + 1] ? parts[index + 1] : '0.0.0-stub';
}

// `exit 1` on demand, so a test can turn a working compiler into a broken one
// the way a bad build does.
if (process.env.KEX_STUB_BROKEN === '1') {
  process.stderr.write('kex: invalid prebuilt standard library (stub)\n');
  process.exit(1);
}

const args = process.argv.slice(2);

if (args.includes('--info')) {
  process.stdout.write(JSON.stringify({
    version: version(),
    revision: 'stub123',
    built: '2026-01-01',
    runtime_otp_floor: 29,
  }) + '\n');
  process.exit(0);
}

if (args.includes('--version')) {
  process.stdout.write(`kex ${version()} (stub123, built 2026-01-01)\n`);
  process.exit(0);
}

if (!args.includes('--lsp')) {
  process.stderr.write(`kex-stub: nothing to do with ${args.join(' ')}\n`);
  process.exit(2);
}

// --- the language server ---------------------------------------------------

// Every start is recorded, so a test can count them: "did the rebuild restart
// the server" is a question about how many servers have run, and the alternative
// — reading the process table — differs per platform and catches other people's
// compilers.
if (process.env.KEX_STUB_LOG) {
  fs.appendFileSync(process.env.KEX_STUB_LOG,
    `start ${version()} ${process.pid} ${Date.now()}\n`);
}

function send(message) {
  const body = JSON.stringify(message);
  process.stdout.write(`Content-Length: ${Buffer.byteLength(body, 'utf8')}\r\n\r\n${body}`);
}

function handle(message) {
  const { id, method } = message;
  switch (method) {
    case 'initialize':
      send({
        jsonrpc: '2.0',
        id,
        result: {
          capabilities: {
            textDocumentSync: 1,
            hoverProvider: true,
            definitionProvider: true,
            referencesProvider: true,
            completionProvider: { triggerCharacters: ['.'] },
          },
          serverInfo: { name: 'kex-stub', version: version() },
        },
      });
      return;
    case 'textDocument/hover':
      // The version goes in the answer, so a test can tell WHICH toolchain
      // replied without hunting through the process table.
      send({
        jsonrpc: '2.0',
        id,
        result: { contents: { kind: 'plaintext', value: `kex-stub ${version()} pid ${process.pid}` } },
      });
      return;
    case 'textDocument/definition':
    case 'textDocument/references':
      send({ jsonrpc: '2.0', id, result: [] });
      return;
    case 'textDocument/completion':
      send({ jsonrpc: '2.0', id, result: { isIncomplete: false, items: [] } });
      return;
    case 'shutdown':
      send({ jsonrpc: '2.0', id, result: null });
      return;
    case 'exit':
      process.exit(0);
      return;
    default:
      // A request must be answered even when it is not understood, or the
      // client waits forever; a notification has no id and needs nothing.
      if (id !== undefined) send({ jsonrpc: '2.0', id, result: null });
  }
}

let buffer = Buffer.alloc(0);
process.stdin.on('data', chunk => {
  buffer = Buffer.concat([buffer, chunk]);
  for (;;) {
    const separator = buffer.indexOf('\r\n\r\n');
    if (separator < 0) return;
    const header = buffer.subarray(0, separator).toString('ascii');
    const length = Number(/Content-Length: (\d+)/i.exec(header)?.[1] ?? 0);
    const start = separator + 4;
    if (buffer.length < start + length) return;
    const body = buffer.subarray(start, start + length).toString('utf8');
    buffer = buffer.subarray(start + length);
    try {
      handle(JSON.parse(body));
    } catch {
      // A frame this stub cannot parse is not what any of these tests are
      // about; dropping it keeps the server alive for the ones that are.
    }
  }
});
process.stdin.on('close', () => process.exit(0));
