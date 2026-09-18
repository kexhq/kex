#!/usr/bin/env python3
"""Regression test for kexhq/kex#279: "tey: every interactive prompt answers
itself -- bin/tey backgrounds erl, so stdin is /dev/null".

`tey/bin/tey` backgrounds `erl` (so a terminal's Ctrl+C can be turned into
the SIGTERM that runs kex_child_guard) -- and per POSIX, an asynchronous
list's stdin is /dev/null unless the command redirects it explicitly. Every
interactive prompt Tey has (askLibrary, tey new's own, Tey.Generator's) then
reads eof at once and silently takes its default, even on a real terminal.

This can ONLY be caught by actually running the launcher SCRIPT attached to
a real pty -- a plain pipe does not reproduce it (a pipe's other end is
real, open, data-bearing stdin; the bug is specifically POSIX's
"backgrounded AND no controlling terminal" substitution), which is also why
it was invisible in CI before: "not visible in CI, where stdin is a pipe or
closed and the default answer is right anyway" (#279).

Deliberately does not drive `tey new` itself: that needs a compiled
tey/ebin, tying this test's ability to catch a stdin regression to whether
Tey's OWN (much larger, independently changing) source happens to compile
that day. Instead it compiles a two-line throwaway Kex program (this file's
own `PROBE_SOURCE`) that reads one line and reports what it got, and runs
THAT under `tey/bin/tey` by pointing TEY_EBIN at it -- exercising exactly
the launcher's stdin plumbing the bug and fix are about, nothing else.

Run with: python3 tools/test-tey-pty-prompt.py [./build/kex] [tey/bin/tey]
"""
import os
import pathlib
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEX = pathlib.Path(sys.argv.pop(1) if len(sys.argv) > 1 else ROOT / 'build/kex').resolve()
TEY_LAUNCHER = pathlib.Path(
    sys.argv.pop(1) if len(sys.argv) > 1 else ROOT / 'tey/bin/tey').resolve()

PROBE_SOURCE = '''main(args) do
  IO.printLine("GOT:${IO.getLine.or("NONE")}")
  System.exit(0)
end
'''


def read_for(fd, timeout_s: float) -> bytes:
    """Reads whatever arrives on the pty master within `timeout_s`, stopping
    early on a clean EOF (the child closed its end)."""
    buffer = b''
    deadline = time.monotonic() + timeout_s
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        ready, _, _ = select.select([fd], [], [], remaining)
        if not ready:
            break
        try:
            chunk = os.read(fd, 512)
        except OSError:
            break
        if not chunk:
            break
        buffer += chunk
    return buffer


class TeyLauncherPty(unittest.TestCase):
    def setUp(self):
        self.assertTrue(KEX.exists(), f'{KEX} not found -- run `make build` first')
        self.assertTrue(TEY_LAUNCHER.exists(), f'{TEY_LAUNCHER} not found')
        self.directory = tempfile.TemporaryDirectory(prefix='kex-tey-pty-test-')
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)

        probe_source = self.root / 'main.kex'
        probe_source.write_text(PROBE_SOURCE)
        self.ebin = self.root / 'ebin'
        self.ebin.mkdir()
        subprocess.run([str(KEX), '--compile', '-o', str(self.ebin), str(probe_source)],
                       check=True, capture_output=True, text=True)

    def test_reads_a_line_typed_at_a_real_terminal_instead_of_racing_to_eof(self):
        env = dict(os.environ)
        env['TEY_EBIN'] = str(self.ebin)

        pid, master_fd = os.forkpty()
        if pid == 0:
            try:
                os.execve(str(TEY_LAUNCHER), ['tey'], env)
            finally:
                os._exit(127)  # execve only returns on failure

        try:
            written = os.write(master_fd, b'hello-from-pty\n')
            self.assertEqual(written, len(b'hello-from-pty\n'),
                              'failed to write to the pty')

            deadline = time.monotonic() + 20.0
            exited = False
            while time.monotonic() < deadline:
                done, _ = os.waitpid(pid, os.WNOHANG)
                if done == pid:
                    exited = True
                    break
                time.sleep(0.05)
            output = read_for(master_fd, 1.0)
            if not exited:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
        finally:
            os.close(master_fd)

        self.assertTrue(exited, 'the launcher did not exit within 20s')
        self.assertIn(b'GOT:hello-from-pty', output,
                      "expected the line written to the pty to be read back; got "
                      + repr(output)
                      + " -- if this instead contains GOT:NONE, the launcher raced "
                      'to end-of-input without waiting (kexhq/kex#279 is back)')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
