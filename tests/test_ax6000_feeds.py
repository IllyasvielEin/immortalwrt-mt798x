# SPDX-License-Identifier: GPL-2.0-only
"""Cross-feed preparation failures, recovery, and the real Go Make calls."""

import difflib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ax6000_feeds", ROOT / "scripts/prepare-ax6000-feeds.py")
PREP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREP)


def digest(value):
    return hashlib.sha256(value).hexdigest()


def git(directory, *arguments):
    return subprocess.check_output(["git", "-C", str(directory), *arguments], stderr=subprocess.PIPE)


class FeedPreparationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ax6000-feeds-test-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.root = self.work / "root"
        self.patch_dir = self.root / "config/ax6000/feeds"
        self.patch_dir.mkdir(parents=True)
        self.before, self.after, self.paths, self.revisions = {}, {}, {}, {}
        locks = []
        for name in sorted(PREP.FEEDS):
            feed = self.root / "feeds" / name
            feed.mkdir(parents=True)
            git(feed, "init", "--quiet")
            relative = "shared/settings.mk"
            path = feed / relative
            path.parent.mkdir()
            before = f"{name} setting=before\n".encode()
            after = f"{name} setting=after\n".encode()
            path.write_bytes(before)
            path.chmod(0o640 if name == "nikki" else 0o755)
            git(feed, "add", ".")
            git(feed, "-c", "user.name=AX6000 tests", "-c", "user.email=tests@example.invalid",
                "commit", "--quiet", "-m", "fixture")
            revision = git(feed, "rev-parse", "HEAD").decode().strip()
            self.revisions[name] = revision
            locks.append(f"src-git {name} https://example.invalid/{name}.git^{revision}\n")
            if name not in ("nikki", "packages"):
                continue
            self.before[name], self.after[name], self.paths[name] = before, after, path
            patch = "".join(difflib.unified_diff(
                before.decode().splitlines(keepends=True), after.decode().splitlines(keepends=True),
                fromfile="a/" + relative, tofile="b/" + relative,
            )).encode()
            patch_name = f"{name}.patch"
            (self.patch_dir / patch_name).write_bytes(patch)
            manifest = {"schema_version": 1, "feed": name, "feed_revision": revision, "patches": [{
                "patch": patch_name, "sha256": digest(patch), "files": [{"path": relative,
                "preimage_sha256": digest(before), "postimage_sha256": digest(after)}],
            }]}
            self.write_manifest(name, manifest)
        (self.root / "feeds.conf.default").write_text("".join(locks))

    def manifest_path(self, feed):
        return self.patch_dir / ("manifest.json" if feed == "nikki" else "golang-manifest.json")

    def read_manifest(self, feed):
        return json.loads(self.manifest_path(feed).read_text())

    def write_manifest(self, feed, manifest):
        self.manifest_path(feed).write_text(json.dumps(manifest) + "\n")

    def contents(self):
        return {name: path.read_bytes() for name, path in self.paths.items()}

    def test_same_relative_paths_are_isolated_and_modes_preserved(self):
        modes = {name: stat.S_IMODE(path.stat().st_mode) for name, path in self.paths.items()}
        self.assertEqual(PREP.prepare(self.root), 2)
        self.assertEqual(self.contents(), self.after)
        self.assertEqual({name: stat.S_IMODE(path.stat().st_mode) for name, path in self.paths.items()}, modes)
        self.assertEqual(PREP.prepare(self.root), 0)
        with mock.patch.object(PREP, "replace_file", side_effect=AssertionError("--check must not write")):
            self.assertEqual(PREP.prepare(self.root, check_only=True), 0)

    def test_check_rejects_preimages_without_writing(self):
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root, check_only=True)
        self.assertEqual(self.contents(), self.before)

    def test_second_manifest_output_error_leaves_first_feed_untouched(self):
        manifest = self.read_manifest("packages")
        manifest["patches"][0]["files"][0]["postimage_sha256"] = "0" * 64
        self.write_manifest("packages", manifest)
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root)
        self.assertEqual(self.contents(), self.before)

    def test_second_manifest_bad_patch_hash_leaves_all_feeds_untouched(self):
        (self.patch_dir / "packages.patch").write_text("tampered\n")
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root)
        self.assertEqual(self.contents(), self.before)

    def test_wrong_reviewed_baseline_is_rejected(self):
        manifest = self.read_manifest("packages")
        manifest["feed_revision"] = "0" * 40
        self.write_manifest("packages", manifest)
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root)
        self.assertEqual(self.contents(), self.before)

    def test_live_modification_is_not_overwritten(self):
        self.paths["packages"].write_text("unreviewed edit\n")
        expected = self.contents()
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root)
        self.assertEqual(self.contents(), expected)

    def test_patch_cannot_write_an_unlisted_file(self):
        path = self.patch_dir / "packages.patch"
        patch = path.read_bytes() + b"--- /dev/null\n+++ b/unlisted\n@@ -0,0 +1 @@\n+extra\n"
        path.write_bytes(patch)
        manifest = self.read_manifest("packages")
        manifest["patches"][0]["sha256"] = digest(patch)
        self.write_manifest("packages", manifest)
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root)
        self.assertEqual(self.contents(), self.before)
        self.assertFalse((self.root / "feeds/packages/unlisted").exists())

    def test_duplicate_manifest_path_is_rejected(self):
        manifest = self.read_manifest("packages")
        manifest["patches"][0]["files"] *= 2
        self.write_manifest("packages", manifest)
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root)
        self.assertEqual(self.contents(), self.before)

    def test_symlink_input_cannot_escape_feed(self):
        outside = self.work / "outside"
        outside.write_bytes(self.before["packages"])
        self.paths["packages"].unlink()
        self.paths["packages"].symlink_to(outside)
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root)
        self.assertEqual(outside.read_bytes(), self.before["packages"])
        self.assertEqual(self.paths["nikki"].read_bytes(), self.before["nikki"])

    def test_symlink_feed_parent_and_manifest_are_rejected(self):
        real = self.root / "real-feeds"
        (self.root / "feeds").rename(real)
        (self.root / "feeds").symlink_to(real, target_is_directory=True)
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root)
        (self.root / "feeds").unlink()
        real.rename(self.root / "feeds")
        manifest = self.manifest_path("packages")
        other = self.work / "manifest-copy.json"
        manifest.rename(other)
        manifest.symlink_to(other)
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root)
        self.assertEqual(self.contents(), self.before)

    def test_head_change_after_staging_is_rejected_before_any_write(self):
        original = PREP.recheck_inputs

        def changed(*args):
            feed = self.root / "feeds/packages"
            (feed / "new-input").write_text("changed HEAD\n")
            git(feed, "add", "new-input")
            git(feed, "-c", "user.name=AX6000 tests", "-c", "user.email=tests@example.invalid",
                "commit", "--quiet", "-m", "concurrent feed update")
            return original(*args)

        with mock.patch.object(PREP, "recheck_inputs", side_effect=changed):
            with self.assertRaises(PREP.PreparationError):
                PREP.prepare(self.root)
        self.assertEqual(self.contents(), self.before)

    def test_live_content_or_mode_changes_after_staging_are_rejected(self):
        original = PREP.recheck_inputs
        for kind in ("content", "mode"):
            with self.subTest(kind=kind):
                def changed(*args):
                    if kind == "content":
                        self.paths["packages"].write_text("concurrent edit\n")
                    else:
                        self.paths["packages"].chmod(0o600)
                    return original(*args)
                with mock.patch.object(PREP, "recheck_inputs", side_effect=changed):
                    with self.assertRaises(PREP.PreparationError):
                        PREP.prepare(self.root)
                self.assertEqual(self.paths["nikki"].read_bytes(), self.before["nikki"])
                self.paths["packages"].write_bytes(self.before["packages"])
                self.paths["packages"].chmod(0o755)

    def test_failed_atomic_replace_does_not_truncate_target_and_cleans_temp(self):
        with mock.patch.object(PREP.os, "replace", side_effect=OSError("disk failure")):
            with self.assertRaises(OSError):
                PREP.prepare(self.root)
        self.assertEqual(self.contents(), self.before)
        self.assertFalse(list((self.root / "feeds").rglob(".ax6000-feed-*")))

    def test_interrupted_second_replace_leaves_recoverable_complete_images(self):
        replace = os.replace
        calls = []

        def interrupted(source, destination):
            calls.append(destination)
            if len(calls) == 2:
                raise OSError("interrupted second feed write")
            return replace(source, destination)

        with mock.patch.object(PREP.os, "replace", side_effect=interrupted):
            with self.assertRaises(OSError):
                PREP.prepare(self.root)
        self.assertEqual(self.contents(), {"nikki": self.after["nikki"], "packages": self.before["packages"]})
        with self.assertRaises(PREP.PreparationError):
            PREP.prepare(self.root, check_only=True)
        self.assertEqual(PREP.prepare(self.root), 1)
        self.assertEqual(PREP.prepare(self.root, check_only=True), 0)


class ActualGoPatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        manifest = json.loads((ROOT / "config/ax6000/feeds/golang-manifest.json").read_text())
        cls.recipe = manifest["patches"][0]
        cls.patch = ROOT / "config/ax6000/feeds" / cls.recipe["patch"]
        cls.originals = {}
        cls.revision = manifest["feed_revision"]
        for name in ("golang-compiler.mk", "golang-host-build.mk", "golang-values.mk"):
            result = subprocess.run(["git", "-C", str(ROOT / "feeds/packages"), "show",
                                     f"{cls.revision}:lang/golang/{name}"], capture_output=True)
            if result.returncode:
                raise unittest.SkipTest("locked packages feed must be available for actual Make-call checks")
            cls.originals[name] = result.stdout

    def test_reviewed_patch_hashes_and_actual_make_calls_stay_local(self):
        with tempfile.TemporaryDirectory(prefix="ax6000-go-calls-") as temporary:
            work = Path(temporary)
            golang = work / "packages/lang/golang"
            golang.mkdir(parents=True)
            for name, content in self.originals.items():
                (golang / name).write_bytes(content)
            self.assertEqual(digest(self.patch.read_bytes()), self.recipe["sha256"])
            git(work / "packages", "apply", "--check", "--whitespace=error", str(self.patch))
            git(work / "packages", "apply", "--whitespace=error", str(self.patch))
            for record in self.recipe["files"]:
                self.assertEqual(digest(self.originals[Path(record["path"]).name]), record["preimage_sha256"])
                self.assertEqual(digest((work / "packages" / record["path"]).read_bytes()), record["postimage_sha256"])
            self.assertEqual((golang / "golang-values.mk").read_bytes(), self.originals["golang-values.mk"])
            source = work / "compiler/src"
            source.mkdir(parents=True)
            (source / "make.bash").write_text(
                'printf "compiler|%s|%s|%s\\n" "$GOTOOLCHAIN" "$GOPROXY" "$GOSUMDB" >> "$AX6000_TEST_CALLS"\n')
            (golang / "golang-build.sh").write_text(
                'printf "host|%s|%s|%s|%s\\n" "$GOTOOLCHAIN" "$GOENV" "$GOPROXY" "$GOSUMDB" >> "$AX6000_TEST_CALLS"\n')
            makefile = work / "Makefile"
            makefile.write_text(f'''GO_INCLUDE_DIR:={golang}
BASH:=/bin/bash
include $(GO_INCLUDE_DIR)/golang-compiler.mk
include $(GO_INCLUDE_DIR)/golang-host-build.mk
.PHONY: all
all:
\t$(call GoCompiler/Default/Make,{source.parent},GOTOOLCHAIN=auto)
\t$(call GoHost/Host/Compile)
''')
            calls = work / "calls"
            env = os.environ | {"GOTOOLCHAIN": "auto", "GOENV": "/bad/goenv", "AX6000_TEST_CALLS": str(calls),
                                "GOPROXY": "https://proxy.golang.org", "GOSUMDB": "sum.golang.org"}
            result = subprocess.run(["make", "--no-print-directory", "-f", str(makefile)],
                                    cwd=work, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(calls.read_text().splitlines(), [
                "compiler|local|https://proxy.golang.org|sum.golang.org",
                "host|local|off|https://proxy.golang.org|sum.golang.org",
            ])


if __name__ == "__main__":
    unittest.main()
