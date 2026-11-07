"""Mandatory provenance checks with isolated Git/feed trees and executable Go fixtures."""

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
SPEC = importlib.util.spec_from_file_location("ax6000_build_inputs", ROOT / "scripts/ax6000-build-inputs.py")
INPUTS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INPUTS)


class BuildInputTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "firmware"
        self.root.mkdir()
        self.env = {"PATH": os.environ["PATH"], "AX6000_GO_BOOTSTRAP_MODE": "source",
                    "AX6000_GO_BOOTSTRAP_ROOT": "", "GITHUB_REPOSITORY": "test/firmware"}
        self.git(self.root, "init", "-q")
        self.write(".gitignore", "feeds/\n.config\nax6000.diffconfig\nax6000-cache-*.json\nrecord/\nstaging_dir/\nbin/\n")
        self.write("README", "reviewed source\n")
        self.write("scripts/validate-ax6000-config.sh", "#!/bin/sh\nset -eu\ntest -f \"$1\"\n")
        self.write("config/ax6000/feeds/manifest.json", '{"feed":"nikki"}\n')
        self.write("config/ax6000/feeds/100-reviewed.patch", "reviewed patch\n")
        self.write_config()
        self.write("ax6000.diffconfig", "CONFIG_PACKAGE_mihomo-meta=y\n")
        self.write_cache()
        self.write("ax6000-cache-state.json", '{"cold":false}\n')
        lines = []
        for name in sorted(INPUTS.FEEDS):
            feed = self.root / "feeds" / name
            feed.mkdir(parents=True)
            self.git(feed, "init", "-q")
            (feed / "README").write_text(name)
            if name == "packages":
                for series, expected in INPUTS.EXPECTED_GO.items():
                    recipe = feed / f"lang/golang/golang{series}/Makefile"
                    recipe.parent.mkdir(parents=True)
                    recipe.write_text(f"GO_VERSION_MAJOR_MINOR:={series}\nGO_VERSION_PATCH:={expected.rsplit('.', 1)[1]}\nGO_VERSION_RC:=\n")
            self.commit(feed)
            revision = self.git(feed, "rev-parse", "HEAD")
            lines.append(f"src-git {name} https://example.invalid/{name}.git^{revision}")
        self.write("feeds.conf.default", "\n".join(lines) + "\n")
        self.commit(self.root)
        self.target = self.root / "bin/targets/mediatek/filogic"
        self.target.mkdir(parents=True)

    def git(self, directory, *arguments):
        return subprocess.run(["git", "-C", str(directory), *arguments], check=True,
                              capture_output=True, text=True).stdout.strip()

    def commit(self, directory):
        self.git(directory, "add", ".")
        self.git(directory, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                 "commit", "-qm", "reviewed fixture")

    def write(self, relative, value):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value)
        return path

    def write_config(self, external=None):
        self.write(".config", ("# CONFIG_GOLANG_BUILD_BOOTSTRAP is not set\n" if external else
                               "CONFIG_GOLANG_BUILD_BOOTSTRAP=y\n") +
                   f'CONFIG_GOLANG_EXTERNAL_BOOTSTRAP_ROOT="{external or ""}"\n')

    def write_cache(self, mode="source"):
        self.write("ax6000-cache-inputs.json", json.dumps({"bootstrap_mode": mode,
                   "bootstrap_version": INPUTS.BOOTSTRAP_VERSION}) + "\n")

    def install_go(self, wrong=None):
        for series, version in INPUTS.EXPECTED_GO.items():
            reported = "0.0.0" if wrong == series else version
            binary = self.write(f"staging_dir/hostpkg/lib/go-{series}/bin/go", f"#!{sys.executable}\n"
                                "import os\n"
                                "assert os.environ.get('GOTOOLCHAIN') == 'local'\n"
                                "assert os.environ.get('GOENV') == 'off'\n"
                                "assert os.environ.get('GOWORK') == 'off'\n"
                                "assert 'GOROOT' not in os.environ and 'GOFLAGS' not in os.environ\n"
                                f"print('go version go{reported} linux/amd64')\n")
            binary.chmod(0o755)

    def prepare(self):
        return INPUTS.prepare(self.root, "record", self.env)

    def complete(self):
        return INPUTS.complete(self.root, "record", self.target, self.env)

    def test_source_prepare_and_complete_without_b_helpers(self):
        self.assertFalse((self.root / "scripts/ax6000-go-bootstrap.py").exists())
        self.assertFalse((self.root / "config/ax6000/feeds/golang-manifest.json").exists())
        record = self.prepare()
        self.assertEqual(record["inputs"]["go_bootstrap"]["mode"], "source")
        self.assertIsNone(record["inputs"]["go_bootstrap"]["archive_sha256"])
        self.assertEqual((self.root / "record/ax6000.config").read_bytes(), (self.root / ".config").read_bytes())
        self.install_go()
        self.env.update(GOROOT="/wrong/root", GOFLAGS="-bad")
        result = self.complete()
        self.assertEqual(set(result["actual_go_compilers"]), {"1.26", "1.27"})
        self.assertIn("go version go1.27.3 linux/amd64", (self.target / "build-metadata.txt").read_text())
        self.assertEqual(json.loads((self.target / "ax6000-build-inputs.json").read_text()), result)

    def test_complete_without_preparation_is_rejected(self):
        with self.assertRaises(INPUTS.InputError):
            self.complete()

    def test_required_configuration_validator_failure_blocks_recording(self):
        self.write("scripts/validate-ax6000-config.sh", "#!/bin/sh\nexit 9\n")
        self.commit(self.root)
        with self.assertRaises(subprocess.CalledProcessError) as failure:
            self.prepare()
        self.assertEqual(failure.exception.returncode, 9)
        self.assertFalse((self.root / "record/inputs.json").exists())

    def test_config_drift_and_cache_identity_drift_are_rejected(self):
        self.prepare()
        self.write(".config", (self.root / ".config").read_text() + "CONFIG_NEW_INPUT=y\n")
        with self.assertRaisesRegex(INPUTS.InputError, "changed after preparation"):
            self.complete()
        self.write_config()
        self.write("ax6000-cache-state.json", '{"cold":true}\n')
        with self.assertRaisesRegex(INPUTS.InputError, "changed after preparation"):
            self.complete()

    def test_feed_head_drift_is_rejected(self):
        self.prepare()
        feed = self.root / "feeds/luci"
        (feed / "README").write_text("unexpected new revision")
        self.commit(feed)
        with self.assertRaisesRegex(INPUTS.InputError, "Feed HEAD differs"):
            self.complete()

    def test_wrong_built_go_identity_never_creates_final_record(self):
        self.prepare()
        self.install_go(wrong="1.27")
        with self.assertRaisesRegex(INPUTS.InputError, "Built Go 1.27 identity differs"):
            self.complete()
        self.assertFalse((self.target / "ax6000-build-inputs.json").exists())

    def test_source_or_external_mode_mismatch_fails_before_recording(self):
        self.write_config(external="/tmp/some-bootstrap")
        with self.assertRaisesRegex(INPUTS.InputError, "configuration differs"):
            self.prepare()
        self.write_config()
        self.write_cache("external")
        self.env.update(AX6000_GO_BOOTSTRAP_MODE="external", AX6000_GO_BOOTSTRAP_ROOT="/tmp/some-bootstrap")
        with self.assertRaisesRegex(INPUTS.InputError, "configuration differs"):
            self.prepare()
        self.assertFalse((self.root / "record/inputs.json").exists())

    def test_external_mode_records_verified_bootstrap_identity(self):
        boot = Path(self.temporary.name) / "verified-bootstrap"
        (boot / "bin").mkdir(parents=True)
        (boot / "bin/go").write_bytes(b"verified official executable fixture")
        self.write("scripts/ax6000-go-bootstrap.py", "import sys\nassert sys.argv[1] == 'check'\nprint(sys.argv[2])\n")
        self.commit(self.root)
        self.write_config(external=str(boot))
        self.write_cache("external")
        self.env.update(AX6000_GO_BOOTSTRAP_MODE="external", AX6000_GO_BOOTSTRAP_ROOT=str(boot))
        data = self.prepare()["inputs"]["go_bootstrap"]
        self.assertEqual(data["archive_sha256"], INPUTS.BOOTSTRAP_SHA256)
        self.assertEqual(data["binary_sha256"], INPUTS.digest((boot / "bin/go").read_bytes()))

    def test_head_source_time_and_tracked_source_mismatches_fail(self):
        self.env["GITHUB_SHA"] = "0" * 40
        with self.assertRaisesRegex(INPUTS.InputError, "GITHUB_SHA"):
            self.prepare()
        self.env.pop("GITHUB_SHA")
        self.env["SOURCE_DATE_EPOCH"] = "0"
        with self.assertRaisesRegex(INPUTS.InputError, "SOURCE_DATE_EPOCH"):
            self.prepare()
        self.env.pop("SOURCE_DATE_EPOCH")
        self.write("README", "unreviewed source")
        with self.assertRaisesRegex(INPUTS.InputError, "Tracked firmware source"):
            self.prepare()

    def test_missing_input_and_write_failure_leave_no_valid_record(self):
        state = self.root / "ax6000-cache-state.json"
        state.unlink()
        with self.assertRaises(INPUTS.InputError):
            self.prepare()
        self.write("ax6000-cache-state.json", "{}\n")
        with mock.patch.object(INPUTS, "write_json", side_effect=OSError("disk full")):
            with self.assertRaisesRegex(OSError, "disk full"):
                self.prepare()
        self.assertFalse((self.root / "record/inputs.json").exists())

    def test_cache_profile_and_final_recipe_versions_must_match(self):
        self.write_cache("external")
        with self.assertRaisesRegex(INPUTS.InputError, "Cache input identity"):
            self.prepare()
        self.write_cache()
        recipe = self.root / "feeds/packages/lang/golang/golang1.26/Makefile"
        recipe.write_text(recipe.read_text().replace("PATCH:=10", "PATCH:=11"))
        with self.assertRaisesRegex(INPUTS.InputError, "Unreviewed final Go version"):
            self.prepare()

    def test_symlinked_output_is_rejected(self):
        (self.root / "record").symlink_to(Path(self.temporary.name))
        with self.assertRaisesRegex(INPUTS.InputError, "Symlink"):
            self.prepare()


if __name__ == "__main__":
    unittest.main()
