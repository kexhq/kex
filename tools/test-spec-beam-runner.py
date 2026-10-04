#!/usr/bin/env python3
"""Check the Makefile's BEAM runner against controlled compiler outcomes."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


@unittest.skipUnless(shutil.which('timeout') or shutil.which('gtimeout'),
                     'GNU timeout is required')
class BeamSpecRunner(unittest.TestCase):
    def run_case(self, body, expected='ok'):
        with tempfile.TemporaryDirectory(prefix='kex-beam-runner-') as directory:
            root = pathlib.Path(directory)
            spec = root / 'sample.kex'
            spec.write_text('')
            spec.with_suffix('.expected').write_text(expected + '\n')
            compiler = root / 'compiler'
            count = root / 'count'
            compiler.write_text(
                '#!/bin/bash\n'
                f'count_file="{count}"\n'
                'count=0\n'
                'if [ -f "$count_file" ]; then read -r count < "$count_file"; fi\n'
                'count=$((count + 1))\n'
                'echo "$count" > "$count_file"\n'
                + body + '\n'
            )
            compiler.chmod(0o755)
            result = subprocess.run(
                ['make', '--no-print-directory', '-o', 'build', 'spec-beam',
                 f'KEX={compiler}', f'BEAM_SPEC_FILES={spec}',
                 'BEAM_SPEC_TIMEOUT=0.2'],
                cwd=ROOT, capture_output=True, text=True, timeout=20,
            )
            return result, int(count.read_text())

    def test_correct_output_runs_once(self):
        result, count = self.run_case('echo ok')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(count, 1)

    def test_output_mismatch_is_not_retried(self):
        result, count = self.run_case('echo wrong')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(count, 1)

    def test_expected_error_can_exit_nonzero(self):
        result, count = self.run_case('echo rejected >&2; exit 1', 'rejected')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(count, 1)

    def test_transient_timeout_is_retried_with_fresh_output(self):
        result, count = self.run_case(
            'if [ "$count" -eq 1 ]; then echo partial; sleep 10; fi\necho ok'
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(count, 2)
        self.assertIn('Retrying', result.stdout)

    def test_repeated_timeout_fails_even_with_expected_output(self):
        result, count = self.run_case('echo ok; sleep 10')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(count, 2)
        self.assertIn('timed out twice', result.stdout)

    def test_process_ignoring_term_is_killed_and_fails(self):
        result, count = self.run_case('trap "" TERM\necho ok\nwhile :; do sleep 1; done')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(count, 2)
        self.assertIn('timed out twice', result.stdout)


if __name__ == '__main__':
    unittest.main()
