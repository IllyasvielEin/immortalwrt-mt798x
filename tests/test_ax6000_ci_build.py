"""Run tiny child processes to verify result, cancellation and I/O isolation."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/ax6000-ci-build.py"
SPEC = importlib.util.spec_from_file_location("ci_build", SCRIPT)
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class BuildTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / "actions-output"
        self.env = {**os.environ, "GITHUB_OUTPUT": str(self.output)}

    def invoke(self, codes):
        commands = [[sys.executable, "-c", code] for code in codes]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = BUILD.build(self.root, commands, self.env)
        timings = json.loads((self.root / "ax6000-build-timings.json").read_text())
        return result, timings

    def test_success_records_one_attempt_and_real_started_output(self):
        result, data = self.invoke(["print('built')", "raise SystemExit(3)"])
        self.assertEqual(result, 0)
        self.assertEqual(len(data["attempts"]), 1)
        self.assertEqual(self.output.read_text(), "started=true\n")
        self.assertIn("built", (self.root / "build.log").read_text())

    def test_failure_can_retry_and_two_failures_remain_failure(self):
        result, data = self.invoke(["raise SystemExit(2)", "print('serial success')"])
        self.assertEqual(result, 0)
        self.assertEqual([record["exit_code"] for record in data["attempts"]], [2, 0])
        result, data = self.invoke(["raise SystemExit(2)", "raise SystemExit(7)"])
        self.assertEqual(result, 7)
        self.assertEqual(len(data["attempts"]), 2)

    def test_child_signal_does_not_retry(self):
        result, data = self.invoke(["import os,signal; os.kill(os.getpid(),signal.SIGTERM)", "print('wrong retry')"])
        self.assertEqual(result, 143)
        self.assertEqual(len(data["attempts"]), 1)
        self.assertEqual(data["attempts"][0]["exit_code"], -signal.SIGTERM)

    def test_spawn_failure_never_publishes_started(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = BUILD.build(self.root, [["/missing/ax6000-test-command"]], self.env)
        self.assertEqual(result, 127)
        self.assertEqual(self.output.read_text(), "")
        self.assertFalse((self.root / "ax6000-build-started").exists())

    def test_log_open_failure_does_not_change_success(self):
        (self.root / "build.log").mkdir()
        result, data = self.invoke(["print('still drained')"])
        self.assertEqual(result, 0)
        self.assertEqual(data["attempts"][0]["exit_code"], 0)

    def test_log_write_failure_does_not_change_success(self):
        class BrokenLog:
            def write(self, _value):
                raise OSError("disk full")
            def flush(self):
                pass
            def close(self):
                pass
        original = Path.open
        def fault(path, *args, **kwargs):
            return BrokenLog() if path.name == "build.log" else original(path, *args, **kwargs)
        with mock.patch.object(Path, "open", fault):
            result, data = self.invoke(["print('first'); print('second')"])
        self.assertEqual(result, 0)
        self.assertEqual(data["exit_code"], 0)

    def test_timing_write_failure_preserves_child_result(self):
        (self.root / "ax6000-build-timings.json").mkdir()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = BUILD.build(self.root, [[sys.executable, "-c", "raise SystemExit(9)"]], self.env)
        self.assertEqual(result, 9)

    def test_control_output_failure_blocks_build_without_started(self):
        self.output.mkdir()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = BUILD.build(self.root, [[sys.executable, "-c", "print('must not start')"]], self.env)
        self.assertNotEqual(result, 0)
        self.assertFalse((self.root / "ax6000-build-started").exists())
        data = json.loads((self.root / "ax6000-build-timings.json").read_text())
        self.assertFalse(data["build_started"])

    def test_control_write_failure_after_spawn_fails_without_retry(self):
        class BrokenControl:
            def write(self, _value):
                raise OSError("control output failed")
            def close(self):
                pass
        original = Path.open
        def fault(path, *args, **kwargs):
            return BrokenControl() if path == self.output else original(path, *args, **kwargs)
        retry_file = self.root / "wrong-retry"
        with mock.patch.object(Path, "open", fault):
            result, data = self.invoke(["import time; time.sleep(1)",
                                        f"from pathlib import Path; Path({str(retry_file)!r}).touch()"])
        self.assertNotEqual(result, 0)
        self.assertTrue(data["build_started"])
        self.assertEqual(len(data["attempts"]), 1)
        self.assertIn("control output failed", data["attempts"][0]["error"])
        self.assertFalse(retry_file.exists())

    def test_wrapper_sigterm_forwards_to_child_and_skips_retry(self):
        binary = self.root / "bin"
        binary.mkdir()
        make = binary / "make"
        make.write_text(f"#!{sys.executable}\nimport os,time\nfrom pathlib import Path\nPath('make-ready').write_text(str(os.getpid()))\ntime.sleep(60)\n")
        make.chmod(0o755)
        env = {**self.env, "PATH": str(binary) + os.pathsep + os.environ.get("PATH", "")}
        process = subprocess.Popen([sys.executable, str(SCRIPT)], cwd=self.root, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while not (self.root / "make-ready").exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue((self.root / "make-ready").exists())
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=12)
            self.assertEqual(process.returncode, 143)
            data = json.loads((self.root / "ax6000-build-timings.json").read_text())
            self.assertEqual(len(data["attempts"]), 1)
            self.assertEqual(data["wrapper_signal"], signal.SIGTERM)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    unittest.main()
