#!/usr/bin/env python3
"""Exercise the real fuzzer CLI against controlled backend failures.

Run with: python3 tools/test-fuzz.py ./build/kex
All generated programs, fake compilers and findings live in a temporary directory.
"""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEX = pathlib.Path(sys.argv.pop(1) if len(sys.argv) > 1 else ROOT / 'build/kex').resolve()


class FuzzerCLI(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='kex-fuzz-test-')
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        self.output = self.root / 'findings'
        self.compiler = self.root / 'fake compiler'

    def stub(self, body):
        self.compiler.write_text(
            '#!/bin/sh\n'
            'if [ "$1" = --version ]; then echo "kex test compiler"; exit 0; fi\n'
            + body + '\n'
        )
        self.compiler.chmod(0o755)

    def run_fuzzer(self, *, seed='42', count='1', budget='1000'):
        result = subprocess.run(
            [str(KEX), '--run-walker', '--no-colors', '--', 'tools/fuzz.kex',
             seed, count, str(self.compiler), str(self.output), budget],
            cwd=ROOT, text=True, capture_output=True, timeout=90,
        )
        return result

    def test_backend_outcomes(self):
        cases = [
            ('agreement', 'echo ok', 0),
            ('warnings', 'echo "$1 warning" >&2; echo ok', 0),
            ('stdout mismatch', 'echo "$1"', 1),
            ('matching rejection', 'echo rejected >&2; exit 1', 1),
            ('matching crash', 'kill -KILL $$', 1),
            ('different failures',
             'if [ "$1" = --run-walker ]; then exit 139; else exit 1; fi', 1),
            ('one failure',
             'if [ "$1" = --run-walker ]; then exit 1; else echo ok; fi', 1),
            ('matching timeout', 'sleep 5', 1),
            ('one timeout',
             'if [ "$1" = --run-walker ]; then sleep 5; else echo ok; fi', 1),
        ]
        for name, body, expected in cases:
            with self.subTest(name=name):
                self.output = self.root / name
                self.stub(body)
                result = self.run_fuzzer(budget='1000')
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                if expected:
                    self.assertTrue((self.output / 'fail_42.kex').is_file())
                    self.assertTrue((self.output / 'fail_42.txt').is_file())

    def test_diagnostics_and_replay_metadata(self):
        self.stub('echo "crash details" >&2; exit 1')
        result = self.run_fuzzer()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        report = (self.output / 'fail_42.txt').read_text()
        for expected in ['crash details', 'stderr:', 'stdout:', 'exit=1',
                         'kex test compiler', 'seed=42', 'timeoutMs=1000',
                         str(self.compiler), '--run-walker', '--run', 'fail_42.kex']:
            self.assertIn(expected, report)

    def test_missing_compiler(self):
        result = self.run_fuzzer()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('preflight failed', result.stderr)

    def test_bad_output_directory(self):
        self.stub('echo ok')
        self.output.write_text('not a directory')
        result = self.run_fuzzer()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_write_failures(self):
        self.stub('exit 1')
        for filename in ['run.txt', 'case_0.kex', 'fail_42.kex', 'fail_42.txt']:
            with self.subTest(filename=filename):
                self.output = self.root / filename.replace('.', '-')
                (self.output / filename).mkdir(parents=True)
                result = self.run_fuzzer()
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('cannot write', result.stderr)

    def test_invalid_arguments(self):
        self.stub('echo ok')
        for kwargs in [dict(seed='oops'), dict(seed='0'), dict(count='0'),
                       dict(count='-1'), dict(budget='0'), dict(budget='oops')]:
            with self.subTest(kwargs=kwargs):
                result = self.run_fuzzer(**kwargs)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('usage:', result.stderr)

    def test_generated_match_corpus(self):
        # Auto-load the actual generator beside this temporary spec, then run
        # all generated matches on both backends as one program. This covers
        # empty-list inference and real reads of singleton/head bindings.
        (self.root / 'fuzz.kex').symlink_to(ROOT / 'tools/fuzz.kex')
        corpus = self.root / 'matches.kex'
        spec = self.root / 'fuzz.spec.kex'
        spec.write_text(r'''it("generates the match corpus") do
  let empty = Env { ints: [], floats: [], strings: [], bools: [] }
  var program = "main do\n"
  var seed = 1
  while seed <= 100 do
    let ctx = Ctx { rng: Rng { seed: seed }, counter: 0, env: empty }
    let (source, ignored) = genMatch(ctx, :int, 0)
    program = "${program}let result${seed} = ${source}\nIO.printLine(result${seed})\n"
    seed = seed + 1
  end
  assert(FS.File.write("CORPUS", "${program}end\n"))
end
'''.replace('CORPUS', str(corpus)))
        generated = subprocess.run(
            [str(KEX), '--run-walker', '--no-colors', str(spec)],
            cwd=ROOT, text=True, capture_output=True, timeout=90,
        )
        self.assertEqual(generated.returncode, 0, generated.stdout + generated.stderr)
        answers = []
        for backend in ['--run-walker', '--run']:
            result = subprocess.run(
                [str(KEX), backend, '--no-colors', str(corpus)],
                cwd=ROOT, text=True, capture_output=True, timeout=90,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(len(result.stdout.splitlines()), 100)
            answers.append(result.stdout)
        self.assertEqual(*answers)


if __name__ == '__main__':
    unittest.main()
