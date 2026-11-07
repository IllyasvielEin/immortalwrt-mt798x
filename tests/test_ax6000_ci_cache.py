"""Exercise cache compatibility inputs and restore failure boundaries."""

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ci_cache", ROOT / "scripts/ax6000-ci-cache.py")
CACHE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CACHE)


class CacheStateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.path = self.root / "cache"
        self.path.mkdir()
        (self.path / "partial").write_text("partial data")

    def test_blank_success_is_unclassified_and_can_seed_only_from_empty(self):
        save, state = CACHE.prepare_cache(self.path, False, "success", "", "skipped", "")
        self.assertTrue(save)
        self.assertEqual(state, "miss-or-unavailable")
        self.assertFalse(self.path.exists())

    def test_explicit_lookup_failure_discards_and_cannot_seed(self):
        save, _ = CACHE.prepare_cache(self.path, False, "failure", "", "skipped", "")
        self.assertFalse(save)
        self.assertFalse(self.path.exists())

    def test_confirmed_restore_retains_content(self):
        save, state = CACHE.prepare_cache(self.path, False, "success", "exact", "success", "exact")
        self.assertTrue(save)
        self.assertEqual(state, "restored-expected-key")
        self.assertTrue((self.path / "partial").exists())

    def test_restore_failure_eviction_and_key_mismatch_discard_contents(self):
        for outcome, returned in (("failure", ""), ("success", ""), ("success", "wrong")):
            with self.subTest(outcome=outcome, returned=returned):
                self.path.mkdir(exist_ok=True)
                save, _ = CACHE.prepare_cache(self.path, False, "success", "exact", outcome, returned)
                self.assertFalse(save)
                self.assertFalse(self.path.exists())

    def test_cleanup_failure_is_hard_error(self):
        with mock.patch.object(CACHE.shutil, "rmtree", side_effect=PermissionError("cannot clear")):
            with self.assertRaises(PermissionError):
                CACHE.prepare_cache(self.path, False, "success", "", "skipped", "")

    def test_leaf_symlink_is_unlinked_without_traversal(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "keep").write_text("keep")
        link = self.root / "link"
        link.symlink_to(outside, target_is_directory=True)
        save, _ = CACHE.prepare_cache(link, False, "success", "exact", "success", "exact")
        self.assertFalse(save)
        self.assertFalse(link.exists())
        self.assertEqual((outside / "keep").read_text(), "keep")

    def test_two_cache_decisions_are_independent(self):
        (self.root / "dl").mkdir()
        (self.root / ".ccache").mkdir()
        env = {"AX6000_DOWNLOADS_LOOKUP_OUTCOME": "success",
               "AX6000_DOWNLOADS_EXPECTED_KEY": "dl-key",
               "AX6000_DOWNLOADS_RESTORE_OUTCOME": "success",
               "AX6000_DOWNLOADS_RESTORED_KEY": "dl-key",
               "AX6000_COMPILER_LOOKUP_OUTCOME": "success",
               "AX6000_COMPILER_EXPECTED_KEY": "cc-key",
               "AX6000_COMPILER_RESTORE_OUTCOME": "success",
               "AX6000_COMPILER_RESTORED_KEY": "other"}
        data = CACHE.prepare(self.root, env)
        self.assertTrue(data["caches"]["downloads"]["save"])
        self.assertFalse(data["caches"]["compiler"]["save"])

    def test_cold_clears_all_three_caches_and_never_saves(self):
        for relative in ("dl", ".ccache", "tmp/go-build"):
            (self.root / relative).mkdir(parents=True)
        data = CACHE.prepare(self.root, {"AX6000_COLD_CACHE": "true"})
        self.assertFalse(any(record["save"] for record in data["caches"].values()))
        self.assertFalse((self.root / "tmp/go-build").exists())

    def test_cold_rejects_symlinked_go_cache_parent(self):
        (self.root / "tmp").symlink_to(self.path, target_is_directory=True)
        with self.assertRaises(CACHE.CacheError):
            CACHE.prepare(self.root, {"AX6000_COLD_CACHE": "true"})


class CacheInputTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        for pattern in set(CACHE.TOOLCHAIN_INPUTS + CACHE.DOWNLOAD_INPUTS):
            relative = pattern[:-2] + "fixture" if pattern.endswith("**") else pattern
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(relative + "\n")
        subprocess.run(["git", "-C", str(self.root), "add", "."], check=True)
        self.env = {"RUNNER_OS": "Linux", "RUNNER_ARCH": "X64",
                    "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2"}

    def test_mode_changes_config_identity_but_not_random_runner_paths(self):
        first = CACHE.cache_inputs(self.root, "source", self.env)
        second = CACHE.cache_inputs(self.root, "source", {**self.env, "RUNNER_TEMP": "/different/path"})
        external = CACHE.cache_inputs(self.root, "external", self.env)
        self.assertEqual(first, second)
        self.assertNotEqual(first["fingerprints"]["config"]["sha256"],
                            external["fingerprints"]["config"]["sha256"])
        self.assertTrue(first["outputs"]["compiler_key"].endswith("-123-2"))
        self.assertNotIn("ax6000-ccache-Linux-", first["outputs"]["compiler_restore"])
        self.assertEqual(len(first["outputs"]["compiler_restore"].splitlines()), 2)

    def test_toolchain_content_change_invalidates_compatibility_prefix(self):
        before = CACHE.cache_inputs(self.root, "source", self.env)
        path = self.root / "toolchain/fixture"
        path.write_text("reviewed musl fix\n")
        after = CACHE.cache_inputs(self.root, "source", self.env)
        self.assertNotEqual(before["outputs"]["compiler_restore"], after["outputs"]["compiler_restore"])

    def test_missing_tracked_input_and_empty_group_fail(self):
        (self.root / "rules.mk").unlink()
        with self.assertRaises(CACHE.CacheError):
            CACHE.cache_inputs(self.root, "source", self.env)
        (self.root / "rules.mk").write_text("restored")
        subprocess.run(["git", "-C", str(self.root), "rm", "--cached", "-q", "tools/fixture"], check=True)
        with self.assertRaises(CACHE.CacheError):
            CACHE.cache_inputs(self.root, "source", self.env)

    def test_actions_multiline_outputs_roundtrip(self):
        output = self.root / "actions-output"
        CACHE.actions_outputs({"compiler_restore": "one\ntwo", "compiler_key": "key"},
                              {"GITHUB_OUTPUT": str(output)})
        self.assertIn("compiler_restore<<AX6000_CACHE_OUTPUT_END\none\ntwo\n", output.read_text())


if __name__ == "__main__":
    unittest.main()
