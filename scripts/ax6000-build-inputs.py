#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Record mandatory build inputs and verify the built host Go compilers."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FEEDS = {"packages", "luci", "routing", "telephony", "video", "nikki"}
EXPECTED_GO = {"1.26": "1.26.10", "1.27": "1.27.3"}
BOOTSTRAP_VERSION = "1.24.13"
BOOTSTRAP_SHA256 = "1fc94b57134d51669c72173ad5d49fd62afb0f1db9bf3f798fd98ee423f8d730"
BOOTSTRAP_SIZE = 78709986
SDK_BASE = "9f397975c7b47ce6986a6e39529d2f72fd5c729d"


class InputError(ValueError):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(root, args, env=None):
    return subprocess.run(args, cwd=root, env=env, check=True, capture_output=True,
                          text=True, errors="replace", timeout=60).stdout.strip()


def git(root, *args):
    return run(root, ["git", *args])


def safe_path(root, value):
    path = Path(value)
    if not path.is_absolute():
        path = root / path
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise InputError(f"Path must remain inside the build tree: {value}") from error
    if ".." in relative.parts:
        raise InputError(f"Unsafe input path: {value}")
    current = root
    for part in relative.parts:
        current /= part
        if current.is_symlink():
            raise InputError(f"Symlink is not a build input/output: {current}")
    return path


def read_file(root, relative):
    path = safe_path(root, relative)
    if not path.is_file():
        raise InputError(f"Missing regular build input: {relative}")
    return path.read_bytes()


def config_lines(text, symbol):
    return [line for line in text.splitlines()
            if line.startswith(symbol + "=") or line == f"# {symbol} is not set"]


def bootstrap(root, config, env):
    mode = env.get("AX6000_GO_BOOTSTRAP_MODE", "source")
    requested = env.get("AX6000_GO_BOOTSTRAP_ROOT", "")
    if mode not in ("source", "external"):
        raise InputError("Unknown Go bootstrap mode")
    build = "CONFIG_GOLANG_BUILD_BOOTSTRAP"
    external = "CONFIG_GOLANG_EXTERNAL_BOOTSTRAP_ROOT"
    expected_build = f"{build}=y" if mode == "source" else f"# {build} is not set"
    expected_root = f'{external}="{requested}"'
    if ((mode == "source" and requested) or (mode == "external" and not requested)
            or config_lines(config, build) != [expected_build]
            or config_lines(config, external) != [expected_root]):
        raise InputError("Normalized Go bootstrap configuration differs from the requested mode")
    result = {"mode": mode, "version": BOOTSTRAP_VERSION,
              "archive_sha256": None, "archive_bytes": None, "root": None}
    if mode == "external":
        helper = safe_path(root, "scripts/ax6000-go-bootstrap.py")
        actual = run(root, [sys.executable, "-B", str(helper), "check", requested], env=env)
        if actual != requested:
            raise InputError("Bootstrap verifier returned another root")
        executable = Path(requested) / "bin/go"
        result.update(archive_sha256=BOOTSTRAP_SHA256, archive_bytes=BOOTSTRAP_SIZE,
                      root=requested, binary_sha256=digest(executable.read_bytes()))
    return result


def go_versions(root):
    files = {}
    for series, expected in EXPECTED_GO.items():
        relative = f"feeds/packages/lang/golang/golang{series}/Makefile"
        data = read_file(root, relative)
        values = {}
        for name in ("GO_VERSION_MAJOR_MINOR", "GO_VERSION_PATCH", "GO_VERSION_RC"):
            found = re.findall(rf"^{name}:=(.*)$", data.decode(), re.MULTILINE)
            if len(found) != 1:
                raise InputError(f"Missing or duplicate {name} in {relative}")
            values[name] = found[0].strip()
        if (values["GO_VERSION_RC"] or values["GO_VERSION_MAJOR_MINOR"] != series
                or f'{series}.{values["GO_VERSION_PATCH"]}' != expected):
            raise InputError(f"Unreviewed final Go version in {relative}")
        files[relative] = digest(data)
    return files


def snapshot(root, env):
    root = Path(root).resolve()
    if Path(git(root, "rev-parse", "--show-toplevel")).resolve() != root:
        raise InputError("Build root is not its own Git worktree")
    commit = git(root, "rev-parse", "HEAD")
    if env.get("GITHUB_SHA") and env["GITHUB_SHA"] != commit:
        raise InputError("Build HEAD differs from GITHUB_SHA")
    if git(root, "status", "--porcelain", "--untracked-files=no"):
        raise InputError("Tracked firmware source differs from the recorded HEAD")
    epoch = git(root, "show", "-s", "--format=%ct", "HEAD")
    if env.get("SOURCE_DATE_EPOCH") and env["SOURCE_DATE_EPOCH"] != epoch:
        raise InputError("SOURCE_DATE_EPOCH differs from the recorded source time")
    # This validator includes the production source guard and feed --check.
    run(root, ["sh", "scripts/validate-ax6000-config.sh", ".config"], env=env)
    feeds = {}
    for line in read_file(root, "feeds.conf.default").decode().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 3 or fields[0] != "src-git" or fields[1] not in FEEDS:
            raise InputError("Unexpected feed specification")
        name = fields[1]
        revision = fields[2].rpartition("^")[2]
        if name in feeds or not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise InputError("Feeds must have unique full locked revisions")
        directory = safe_path(root, f"feeds/{name}")
        if (Path(git(directory, "rev-parse", "--show-toplevel")).resolve() != directory
                or git(directory, "rev-parse", "HEAD") != revision):
            raise InputError(f"Feed HEAD differs from its lock: {name}")
        feeds[name] = revision
    if set(feeds) != FEEDS:
        raise InputError("Expected exactly six feeds")
    required = [".config", "ax6000.diffconfig", "ax6000-cache-inputs.json",
                "ax6000-cache-state.json", "feeds.conf.default"]
    patch_dir = safe_path(root, "config/ax6000/feeds")
    if not (patch_dir / "manifest.json").is_file():
        raise InputError("Missing reviewed Nikki manifest")
    # A source-only commit can precede B's second manifest/helper.
    required.extend(path.relative_to(root).as_posix() for path in sorted(patch_dir.iterdir())
                    if path.suffix in (".json", ".patch"))
    files = {name: digest(read_file(root, name)) for name in required}
    cache_inputs = json.loads(read_file(root, "ax6000-cache-inputs.json"))
    if (not isinstance(cache_inputs, dict)
            or cache_inputs.get("bootstrap_mode") != env.get("AX6000_GO_BOOTSTRAP_MODE", "source")
            or cache_inputs.get("bootstrap_version") != BOOTSTRAP_VERSION):
        raise InputError("Cache input identity differs from the Go bootstrap selection")
    json.loads(read_file(root, "ax6000-cache-state.json"))
    files.update(go_versions(root))
    return {"schema_version": 1, "repository": env.get("GITHUB_REPOSITORY", "local"),
            "workflow_commit": commit, "source_tree": git(root, "rev-parse", "HEAD^{tree}"),
            "source_date_epoch": epoch,
            "feeds": feeds, "files_sha256": files,
            "go_bootstrap": bootstrap(root, read_file(root, ".config").decode(), env),
            "expected_go_compilers": EXPECTED_GO,
            "runner": {"image_os": env.get("ImageOS"), "image_version": env.get("ImageVersion")}}


def write_json(path, data):
    encoded = (json.dumps(data, indent=2, sort_keys=True) + "\n").encode()
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".ax6000-input-", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def prepare(root, output_dir, env=None):
    root = Path(root).resolve()
    env = dict(os.environ if env is None else env)
    data = snapshot(root, env)
    output = safe_path(root, output_dir)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise InputError("Build input destination must be empty")
    for name in (".config", "ax6000.diffconfig", "ax6000-cache-inputs.json", "ax6000-cache-state.json"):
        shutil.copyfile(safe_path(root, name), output / ("ax6000.config" if name == ".config" else name))
    record = {"prepared_at": datetime.now(timezone.utc).isoformat(), "inputs": data}
    # The record is the last write: failed preparation cannot leave a valid marker.
    write_json(output / "inputs.json", record)
    return record


def go_identity(root, series, expected, env):
    path = safe_path(root, f"staging_dir/hostpkg/lib/go-{series}/bin/go")
    before = read_file(root, path)
    clean = {name: value for name, value in env.items() if not name.startswith("GO")}
    clean.update(GOTOOLCHAIN="local", GOENV="off", GOWORK="off")
    version = run(root, [str(path), "version"], env=clean)
    if version != f"go version go{expected} linux/amd64":
        raise InputError(f"Built Go {series} identity differs from its locked version: {version}")
    if read_file(root, path) != before:
        raise InputError(f"Go binary changed during identity check: {series}")
    return {"path": path.relative_to(root).as_posix(), "version": version, "sha256": digest(before)}


def complete(root, output_dir, target_dir, env=None):
    root = Path(root).resolve()
    env = dict(os.environ if env is None else env)
    output = safe_path(root, output_dir)
    record = json.loads(read_file(root, output / "inputs.json"))
    current = snapshot(root, env)
    if not isinstance(record, dict) or record.get("inputs") != current:
        raise InputError("Verified build inputs changed after preparation")
    identities = {series: go_identity(root, series, expected, env)
                  for series, expected in EXPECTED_GO.items()}
    result = dict(record, completed_at=datetime.now(timezone.utc).isoformat(),
                  actual_go_compilers=identities)
    target = safe_path(root, target_dir)
    if not target.is_dir():
        raise InputError("Firmware target directory does not exist")
    write_json(target / "ax6000-build-inputs.json", result)
    bootstrap_data = current["go_bootstrap"]
    lines = [f"repository={current['repository']}", f"workflow_commit={current['workflow_commit']}",
             f"source_tree={current['source_tree']}", "roaming_diagnostics=excluded",
             f"sdk_upstream_base={SDK_BASE}", f"build_time={result['completed_at']}",
             f"source_date_epoch={current['source_date_epoch']}",
             f"go_bootstrap_mode={bootstrap_data['mode']}",
             f"go_bootstrap_version={bootstrap_data['version']}",
             f"go_bootstrap_archive_sha256={bootstrap_data['archive_sha256'] or 'not-used-source-mode'}"]
    clang = shutil.which("clang", path=env.get("PATH"))
    lines.append("clang=" + (run(root, [clang, "--version"], env=env).splitlines()[0] if clang else "unavailable"))
    lines.extend(f"feed_{name}={revision}" for name, revision in sorted(current["feeds"].items()))
    lines.extend(f"go_{series.replace('.', '_')}={identity['version']}" for series, identity in identities.items())
    lines.extend(f"{sha}  {name}" for name, sha in sorted(current["files_sha256"].items())
                 if name.startswith("config/ax6000/feeds/"))
    (target / "build-metadata.txt").write_text("\n".join(lines) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "complete"))
    parser.add_argument("--output-dir", default="ax6000-build-inputs")
    parser.add_argument("--target-dir", default="bin/targets/mediatek/filogic")
    args = parser.parse_args()
    try:
        if args.phase == "prepare":
            prepare(ROOT, args.output_dir)
        else:
            complete(ROOT, args.output_dir, args.target_dir)
    except (OSError, InputError, json.JSONDecodeError, subprocess.SubprocessError) as error:
        print(f"AX6000 mandatory build input verification failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print((error.stdout or "") + (error.stderr or ""), file=sys.stderr)
        return 1
    print(f"AX6000 mandatory build inputs: {args.phase} verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
