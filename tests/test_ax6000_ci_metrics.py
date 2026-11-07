"""Optional observations must not publish broken paths or leak environment."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/ax6000-ci-metrics.py"
SPEC = importlib.util.spec_from_file_location("ci_metrics", SCRIPT)
METRICS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(METRICS)


class MetricsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.destination = self.root / "observations"
        self.envfile = self.root / "actions-env"
        self.env = {**os.environ, "GITHUB_ENV": str(self.envfile), "GH_TOKEN": "not-an-observation"}

    def test_prepare_publishes_only_absolute_writable_statslog(self):
        data = METRICS.collect("prepare", self.destination, self.root, self.env)
        path = Path(data["statslog"])
        self.assertTrue(path.is_absolute())
        with path.open("a") as output:
            output.write("one build\n")
        self.assertEqual(self.envfile.read_text(), f"CCACHE_STATSLOG={path}\n")
        self.assertNotIn("not-an-observation", json.dumps(data))

    def test_failed_prepare_is_warning_and_does_not_publish_invalid_path(self):
        self.destination.mkdir()
        (self.destination / "ccache-stats.log").mkdir()
        result = subprocess.run([sys.executable, str(SCRIPT), "prepare", str(self.destination)],
                                cwd=self.root, env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("Warning", result.stderr)
        self.assertFalse(self.envfile.exists())

    def test_report_missing_tools_records_unavailable_without_fake_statistics(self):
        with mock.patch.object(METRICS, "run", return_value={"exit_code": 2, "stdout": "", "stderr": "not built"}):
            data = METRICS.collect("report", self.destination, self.root, self.env)
        self.assertEqual(data["ccache"]["status"], "build-did-not-reach-ccache")
        self.assertIsNone(data["cache_transfer_seconds"]["save_compiler"])
        self.assertEqual(data["tools"], {})
        self.assertNotIn("not-an-observation", json.dumps(data))

    def test_json_write_failure_does_not_fail_cli(self):
        self.destination.mkdir()
        (self.destination / "prepare.json").mkdir()
        result = subprocess.run([sys.executable, str(SCRIPT), "prepare", str(self.destination)],
                                cwd=self.root, env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("Warning", result.stderr)


if __name__ == "__main__":
    unittest.main()
