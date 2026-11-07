# SPDX-License-Identifier: GPL-2.0-only
"""Exercise bootstrap verification and the pinned Make calls without downloads."""

import hashlib
import importlib.util
import io
import os
from contextlib import redirect_stdout
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/ax6000-go-bootstrap.py"
spec = importlib.util.spec_from_file_location("ax6000_go_bootstrap", SCRIPT)
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)

# This executable computes its own root and records the environment actually
# received by subprocesses; no mock stands in for the Go command invocation.
GO_FIXTURE = r'''#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)"
printf '%s|%s|%s|%s\n' "$GOTOOLCHAIN" "$GOENV" "${GOROOT-unset}" "$*" >> "$AX6000_GO_TEST_LOG"
case "$*" in
    version) printf '%s\n' "${AX6000_TEST_VERSION:-go version goVERSION linux/amd64}" ;;
    'env GOROOT GOHOSTOS GOHOSTARCH')
        printf '%s\n' "${AX6000_TEST_REPORTED_ROOT:-$root}" "${AX6000_TEST_HOST_OS:-linux}" "${AX6000_TEST_HOST_ARCH:-amd64}" ;;
    *) exit 2 ;;
esac
'''.replace("goVERSION", "go" + bootstrap.VERSION).encode()


def archive_bytes(omit=(), extra=(), executable=True):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        for name in ("go", "go/bin", "go/pkg", "go/src"):
            if name in omit:
                continue
            member = tarfile.TarInfo(name)
            member.type = tarfile.DIRTYPE
            member.mode = 0o755
            archive.addfile(member)
        for name, content in (("go/bin/go", GO_FIXTURE), ("go/LICENSE", b"fixture license\n")):
            if name in omit:
                continue
            member = tarfile.TarInfo(name)
            member.mode = 0o755 if name.endswith("/go") and executable else 0o644
            member.size = len(content)
            archive.addfile(member, io.BytesIO(content))
        for member in extra:
            archive.addfile(member)
    return output.getvalue()


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ax6000-go-test-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.log = self.work / "go-invocations"
        self.environment = mock.patch.dict(os.environ, {
            "AX6000_GO_TEST_LOG": str(self.log), "GOTOOLCHAIN": "auto",
            "GOENV": "/untrusted/goenv", "GOROOT": "/untrusted/root",
        })
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def install_fixture(self, data, name="install", sha=None, size=None):
        with mock.patch.object(bootstrap, "ARCHIVE_SHA256", sha or hashlib.sha256(data).hexdigest()), \
                mock.patch.object(bootstrap, "ARCHIVE_SIZE", len(data) if size is None else size), \
                mock.patch.object(bootstrap.urllib.request, "urlopen", return_value=io.BytesIO(data)) as request:
            result = bootstrap.install(self.work / name)
            request.assert_called_once_with(bootstrap.ARCHIVE_URL, timeout=60)
            return result

    def make_root(self):
        destination = self.work / "tree"
        destination.mkdir()
        archive = self.work / "fixture.tar.gz"
        archive.write_bytes(archive_bytes())
        bootstrap.extract(archive, destination)
        return destination / "go"

    def test_verified_install_allows_internal_links_and_cli_checks_actual_environment(self):
        symbolic = tarfile.TarInfo("go/src/license")
        symbolic.type, symbolic.linkname = tarfile.SYMTYPE, "../LICENSE"
        hard = tarfile.TarInfo("go/LICENSE.copy")
        hard.type, hard.linkname = tarfile.LNKTYPE, "go/LICENSE"
        root = self.install_fixture(archive_bytes(extra=(symbolic, hard)))
        self.assertEqual(root, self.work / "install/go")
        self.assertEqual((root / "src/license").read_bytes(), b"fixture license\n")
        self.assertEqual((root / "LICENSE.copy").read_bytes(), b"fixture license\n")
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "check", str(root)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, str(root) + "\n")
        self.assertEqual(self.log.read_text().splitlines(), [
            "local|off|unset|version", "local|off|unset|env GOROOT GOHOSTOS GOHOSTARCH",
        ] * 2)

    def test_install_cli_prints_only_the_installed_root(self):
        data = archive_bytes()
        destination = self.work / "cli-install"
        output = io.StringIO()
        with mock.patch.object(sys, "argv", [str(SCRIPT), "install", str(destination)]), \
                mock.patch.object(bootstrap, "ARCHIVE_SHA256", hashlib.sha256(data).hexdigest()), \
                mock.patch.object(bootstrap, "ARCHIVE_SIZE", len(data)), \
                mock.patch.object(bootstrap.urllib.request, "urlopen", return_value=io.BytesIO(data)), \
                redirect_stdout(output):
            self.assertEqual(bootstrap.main(), 0)
        self.assertEqual(output.getvalue(), str(destination / "go") + "\n")

    def test_bad_sha_or_size_never_opens_archive_or_executes_go(self):
        data = archive_bytes()
        for index, arguments in enumerate(({"sha": "0" * 64}, {"size": len(data) + 1}, {"size": len(data) - 1})):
            with self.subTest(arguments=arguments), \
                    mock.patch.object(bootstrap.tarfile, "open", side_effect=AssertionError("must not extract")):
                with self.assertRaises(bootstrap.BootstrapError):
                    self.install_fixture(data, name=f"bad-{index}", **arguments)
                self.assertEqual(list((self.work / f"bad-{index}").iterdir()), [])
                self.assertFalse(self.log.exists())

    def test_missing_structure_and_nonexecutable_go_fail_without_execution(self):
        variants = [{"omit": (name,)} for name in ("go/bin/go", "go/pkg", "go/src")]
        variants.append({"executable": False})
        for index, arguments in enumerate(variants):
            with self.subTest(arguments=arguments), self.assertRaises(bootstrap.BootstrapError):
                self.install_fixture(archive_bytes(**arguments), name=f"missing-{index}")
        self.assertFalse(self.log.exists())

    def test_archive_escape_and_special_members_are_rejected_before_execution(self):
        members = [tarfile.TarInfo(name) for name in ("../escaped", "/escaped", "go/../../escaped", "other/file")]
        for kind, target in ((tarfile.SYMTYPE, "../../escaped"), (tarfile.LNKTYPE, "../escaped")):
            member = tarfile.TarInfo("go/escape")
            member.type, member.linkname = kind, target
            members.append(member)
        device = tarfile.TarInfo("go/device")
        device.type = tarfile.CHRTYPE
        members.append(device)
        for index, member in enumerate(members):
            with self.subTest(name=member.name, kind=member.type):
                with self.assertRaises((bootstrap.BootstrapError, tarfile.TarError)):
                    self.install_fixture(archive_bytes(extra=(member,)), name=f"escape-{index}")
                self.assertEqual(list((self.work / f"escape-{index}").iterdir()), [])
        self.assertFalse((self.work / "escaped").exists())
        self.assertFalse(self.log.exists())

    def test_link_chain_cannot_escape_go_root_within_staging(self):
        first = tarfile.TarInfo("go/a")
        first.type, first.linkname = tarfile.SYMTYPE, "."
        second = tarfile.TarInfo("go/b")
        second.type, second.linkname = tarfile.SYMTYPE, "a/../escaped"
        with self.assertRaises(bootstrap.BootstrapError):
            self.install_fixture(archive_bytes(extra=(first, second)))
        self.assertFalse(self.log.exists())

    def test_wrong_version_platform_and_reported_root_are_rejected(self):
        root = self.make_root()
        variants = (
            {"AX6000_TEST_VERSION": "go version go1.24.12 linux/amd64"},
            {"AX6000_TEST_VERSION": f"go version go{bootstrap.VERSION} linux/arm64"},
            {"AX6000_TEST_HOST_OS": "darwin"}, {"AX6000_TEST_HOST_ARCH": "arm64"},
            {"AX6000_TEST_REPORTED_ROOT": str(self.work)},
        )
        for environment in variants:
            with self.subTest(environment=environment), mock.patch.dict(os.environ, environment):
                with self.assertRaises(bootstrap.BootstrapError):
                    bootstrap.check(root)
        self.assertTrue(self.log.exists())

    def test_nonempty_destination_and_unsafe_paths_never_download(self):
        destination = self.work / "occupied"
        destination.mkdir()
        (destination / "keep").write_text("preserve")
        alias = self.work / "alias"
        alias.symlink_to(destination, target_is_directory=True)
        with mock.patch.object(bootstrap.urllib.request, "urlopen") as request:
            for value in (str(destination), str(alias), "/", "relative/path", str(self.work) + "/../unsafe",
                          str(self.work) + "/a b", str(self.work) + "/$(command)"):
                with self.subTest(path=value), self.assertRaises(bootstrap.BootstrapError):
                    bootstrap.install(value)
            request.assert_not_called()
        self.assertEqual((destination / "keep").read_text(), "preserve")

    def test_configuration_switches_external_to_source_and_checks_normalized_result(self):
        root = self.make_root()
        config = self.work / ".config"
        config.write_text('CONFIG_UNRELATED=y\nCONFIG_GOLANG_BUILD_BOOTSTRAP=y\n'
                          'CONFIG_GOLANG_EXTERNAL_BOOTSTRAP_ROOT="/stale"\n')
        bootstrap.configure(config, str(root))
        self.assertEqual(config.read_text(), 'CONFIG_UNRELATED=y\n'
                         '# CONFIG_GOLANG_BUILD_BOOTSTRAP is not set\n'
                         f'CONFIG_GOLANG_EXTERNAL_BOOTSTRAP_ROOT="{root}"\n')
        bootstrap.validate_config(config, str(root))
        with self.assertRaises(bootstrap.BootstrapError):
            bootstrap.validate_config(config, "")
        with mock.patch.object(bootstrap, "check", side_effect=AssertionError("source must not probe Go")):
            bootstrap.configure(config, "")
            bootstrap.validate_config(config, "")
        self.assertEqual(config.read_text(), 'CONFIG_UNRELATED=y\nCONFIG_GOLANG_BUILD_BOOTSTRAP=y\n'
                         'CONFIG_GOLANG_EXTERNAL_BOOTSTRAP_ROOT=""\n')

    def test_invalid_external_configuration_cannot_fall_back_or_modify_config(self):
        root = self.make_root()
        config = self.work / ".config"
        original = 'CONFIG_GOLANG_BUILD_BOOTSTRAP=y\nCONFIG_GOLANG_EXTERNAL_BOOTSTRAP_ROOT=""\n'
        config.write_text(original)
        with mock.patch.dict(os.environ, {"AX6000_TEST_VERSION": "wrong"}):
            with self.assertRaises(bootstrap.BootstrapError):
                bootstrap.configure(config, str(root))
        self.assertEqual(config.read_text(), original)
        bootstrap.configure(config, str(root))
        expected = config.read_text()
        for invalid in (expected + 'CONFIG_GOLANG_BUILD_BOOTSTRAP=y\n',
                        expected.replace('# CONFIG_GOLANG_BUILD_BOOTSTRAP is not set', 'CONFIG_GOLANG_BUILD_BOOTSTRAP=y'),
                        expected.replace(str(root), str(self.work / "elsewhere"))):
            config.write_text(invalid)
            with self.assertRaises(bootstrap.BootstrapError):
                bootstrap.validate_config(config, str(root))
        config.write_text(expected)
        shutil.rmtree(root / "src")
        with self.assertRaises(bootstrap.BootstrapError):
            bootstrap.validate_config(config, str(root))

    def test_wrong_build_host_is_rejected_before_download(self):
        for host_os, host_arch in (("Darwin", "x86_64"), ("Linux", "aarch64")):
            with self.subTest(host_os=host_os, host_arch=host_arch), \
                    mock.patch.object(bootstrap.platform, "system", return_value=host_os), \
                    mock.patch.object(bootstrap.platform, "machine", return_value=host_arch), \
                    mock.patch.object(bootstrap.urllib.request, "urlopen") as request:
                with self.assertRaises(bootstrap.BootstrapError):
                    bootstrap.install(self.work / "wrong-host")
                request.assert_not_called()

    def test_failed_identity_after_install_leaves_empty_destination(self):
        with mock.patch.dict(os.environ, {"AX6000_TEST_VERSION": "wrong"}):
            with self.assertRaises(bootstrap.BootstrapError):
                self.install_fixture(archive_bytes(), name="bad-identity")
        self.assertEqual(list((self.work / "bad-identity").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
