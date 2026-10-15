"""Check that profiling failures cannot publish an apparently optimized binary."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


HELPER = Path(__file__).resolve().parents[1] / 'bolt_optimize.py'


class BoltEvidenceTest(unittest.TestCase):
    def run_case(self, outcome, expected_success):
        root = Path(os.environ['ACTIVEN_TEST_WORK'])
        root.mkdir(parents=True, exist_ok=True)
        case = Path(tempfile.mkdtemp(prefix=f'{outcome} space ', dir=root))
        (case / 'case.json').write_text(json.dumps({'outcome': outcome}) + '\n')
        binary = case / 'sim with spaces'
        binary.write_bytes(b'ORIGINAL EXECUTABLE')
        binary.chmod(0o755)
        tool = case / 'mock llvm-bolt'
        tool.write_text(f'#!{sys.executable}\n' + r'''
import json, pathlib, sys
args = sys.argv[1:]
case = pathlib.Path(__file__).parent
outcome = json.loads((case / 'case.json').read_text())['outcome']
if '--version' in args:
    print('mock LLVM BOLT for publication tests')
    raise SystemExit(0)
output = pathlib.Path(args[args.index('-o') + 1])
if '--instrument' in args:
    fdata = next(a.split('=', 1)[1] for a in args if a.startswith('--instrumentation-file='))
    log = '[System] Loaded 2 memory image(s)\n[System] Result: 0 (0x0)\n[System] Cycles: 100\n'
    status = 0
    if outcome == 'startup_failure':
        log, status = 'Error: MC size is smaller than image size\n', 1
    elif outcome == 'checksum_failure':
        log, status = '[System] Result: 7 (0x7)\n[System] Cycles: 100\n', 1
    elif outcome in {'cycle_limit', 'cycle_limit_before_timer'}:
        log, status = '[System] Loaded 2 memory image(s)\n[HEARTBEAT 500000] mc_reqs=100 periph_reqs=20\n[System] Timed out at cycle 500000\n', 1
        if outcome == 'cycle_limit':
            log += '[System] Timer: 400 cycles\n'
    elif outcome == 'interrupted':
        log, status = '[System] Loaded 2 memory image(s)\n[System] Interrupted at cycle 500000\n', 1
    elif outcome in {'helper_interrupt', 'launch_interrupt'}:
        log = '[System] Loaded 2 memory image(s)\n[System] Running (max 500000 cycles)...\n'
    counts = '' if outcome == 'empty_profile' else '1 example 0 1 example 1 0 3\n'
    program = ('#!' + sys.executable + '\nimport pathlib, sys\n' +
               'pathlib.Path(' + repr(fdata) + ').write_text(' + repr(counts) + ')\n' +
               'sys.stderr.write(' + repr(log) + ')\n')
    if outcome in {'helper_interrupt', 'launch_interrupt'}:
        program += ('pathlib.Path(' + repr(str(case / 'training-started')) +
                    ').write_text("ready")\nimport time\ntime.sleep(60)\n')
    else:
        program += 'sys.exit(' + str(status) + ')\n'
    output.write_text(program)
    output.chmod(0o755)
else:
    if outcome == 'optimizer_failure':
        print('mock rejected profile')
        raise SystemExit(2)
    output.write_bytes(b'OPTIMIZED EXECUTABLE')
    output.chmod(0o755)
''')
        tool.chmod(0o755)
        command = [sys.executable, '-B', str(HELPER), '--llvm-bolt', str(tool),
                   '--binary', str(binary), '--evidence-root', str(case / 'evidence'),
                   '--kind', 'sim_system', '--max-cycles', '500000',
                   '--input', str(case / 'case.json'), '--', '--soft',
                   '--pu', '64', '--mc', '2', '--mc-size', '0x40000000']
        if outcome == 'launch_interrupt':
            wrapper = case / 'interrupt_during_launch.py'
            wrapper.write_text('import importlib.util, os, pathlib, signal, subprocess, sys, time\n' +
                f'spec = importlib.util.spec_from_file_location("bolt_helper", {str(HELPER)!r})\n' +
                'helper = importlib.util.module_from_spec(spec)\nspec.loader.exec_module(helper)\n' +
                'original = subprocess.Popen\n' +
                'def launch(argv, **kwargs):\n' +
                '    child = original(argv, **kwargs)\n' +
                '    if str(argv[0]).endswith("sim.instrumented"):\n' +
                f'        ready = pathlib.Path({str(case / "training-started")!r})\n' +
                '        deadline = time.monotonic() + 5\n' +
                '        while not ready.exists() and time.monotonic() < deadline:\n' +
                '            time.sleep(0.02)\n' +
                '        os.kill(os.getpid(), signal.SIGINT)\n' +
                '    return child\n' +
                'subprocess.Popen = launch\nsys.exit(helper.main())\n')
            command[2] = str(wrapper)
        (case / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        with (case / 'raw.log').open('wb') as log:
            if outcome == 'helper_interrupt':
                result = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 10
                    while not (case / 'training-started').exists() and time.monotonic() < deadline:
                        if result.poll() is not None:
                            break
                        time.sleep(0.02)
                    self.assertTrue((case / 'training-started').exists(), str(case))
                    result.send_signal(signal.SIGINT)
                    result.wait(timeout=15)
                finally:
                    if result.poll() is None:
                        result.terminate()
                        result.wait(timeout=15)
            else:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        manifests = list((case / 'evidence').glob('*/manifest.json'))
        self.assertEqual(len(manifests), 1, str(case))
        manifest = json.loads(manifests[0].read_text())
        self.assertEqual(result.returncode == 0, expected_success,
                         (case / 'raw.log').read_text())
        expected_status = 'completed' if expected_success else 'failed'
        if outcome in {'helper_interrupt', 'launch_interrupt'}:
            expected_status = 'interrupted'
            self.assertEqual(result.returncode, 130)
            self.assertEqual(manifest['commands'][-1]['interrupted_signal'], signal.SIGINT)
            self.assertIsNotNone(manifest['commands'][-1]['exit_status'])
            self.assertNotEqual(manifest['commands'][-1]['exit_status'], 0)
        self.assertEqual(manifest['status'], expected_status)
        self.assertEqual(binary.read_bytes(), b'OPTIMIZED EXECUTABLE' if expected_success
                         else b'ORIGINAL EXECUTABLE')
        self.assertTrue((manifests[0].parent / 'training.log').is_file())
        self.assertTrue((manifests[0].parent / 'profile.fdata').is_file())
        self.assertEqual(manifest['inputs'][0]['sha256'],
                         __import__('hashlib').sha256((case / 'case.json').read_bytes()).hexdigest())
        if outcome == 'cycle_limit':
            self.assertEqual(manifest['training']['outcome'], 'cycle-limit-with-progress')
        print(f'case={outcome} evidence={case}', flush=True)
        (case / 'helper-result.json').write_text(json.dumps({
            'argv': command, 'exit_status': result.returncode,
            'manifest': str(manifests[0]), 'published_bytes': binary.read_bytes().decode(),
        }, indent=2) + '\n')

    def test_completed(self):
        self.run_case('completed', True)

    def test_capped_with_progress(self):
        self.run_case('cycle_limit', True)

    def test_startup_error_with_profile(self):
        self.run_case('startup_failure', False)

    def test_payload_error_with_profile(self):
        self.run_case('checksum_failure', False)

    def test_missing_profile_counts(self):
        self.run_case('empty_profile', False)

    def test_optimizer_error(self):
        self.run_case('optimizer_failure', False)

    def test_interruption_is_not_a_cycle_limit(self):
        self.run_case('interrupted', False)

    def test_cap_before_timed_work_is_rejected(self):
        self.run_case('cycle_limit_before_timer', False)

    def test_helper_interrupt_records_child_exit(self):
        self.run_case('helper_interrupt', False)

    def test_interruption_during_child_launch_is_reaped(self):
        self.run_case('launch_interrupt', False)


if __name__ == '__main__':
    unittest.main()
