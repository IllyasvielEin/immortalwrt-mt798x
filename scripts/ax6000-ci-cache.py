#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fingerprint reviewed cache inputs and discard unconfirmed cache restores."""

import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
ALGORITHM = "ax6000-cache-inputs-v1"
BOOTSTRAP_VERSION = "1.24.13"
CONFIG = "defconfig/mt7986-ax6000.config"
TOOLCHAIN_INPUTS = (
    "Makefile", "rules.mk", "toolchain/**", "tools/**", "include/**",
    "config/**", "scripts/**", "target/linux/mediatek/Makefile",
    "target/linux/mediatek/filogic/**", "target/linux/mediatek/patches-6.12/**",
    "target/linux/generic/kernel-6.12", "target/linux/generic/config-6.12",
    "target/linux/generic/backport-6.12/**", "target/linux/generic/hack-6.12/**",
    "target/linux/generic/pending-6.12/**",
)
DOWNLOAD_INPUTS = ("feeds.conf.default", CONFIG, "config/ax6000/feeds/**")


class CacheError(ValueError):
    pass


def actions_outputs(values, environment):
    """Write only fixed output names and generated, single-line-safe values."""
    destination = environment.get("GITHUB_OUTPUT")
    if not destination:
        return
    with Path(destination).open("a") as output:
        for name, value in values.items():
            value = str(value)
            if "\n" in value:
                delimiter = "AX6000_CACHE_OUTPUT_END"
                if delimiter in value.splitlines():
                    raise CacheError("Unexpected cache output delimiter")
                output.write(f"{name}<<{delimiter}\n{value}\n{delimiter}\n")
            else:
                output.write(f"{name}={value}\n")


def tracked_files(root):
    top = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "--show-toplevel"], text=True
    ).strip()
    if Path(top).resolve() != root.resolve():
        raise CacheError("Cache root must be the repository root")
    result = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"])
    return {os.fsdecode(item) for item in result.split(b"\0") if item}


def fingerprint(root, patterns, tracked, parameters=None):
    selected = set()
    for pattern in patterns:
        matches = {name for name in tracked if fnmatch.fnmatchcase(name, pattern)}
        if not matches:
            raise CacheError(f"Required cache input is empty: {pattern}")
        selected.update(matches)
    records = []
    for relative in sorted(selected, key=os.fsencode):
        path = root / relative
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
            raise CacheError(f"Missing or unsafe cache input: {relative}")
        records.append({"path": relative, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    for name, value in sorted((parameters or {}).items()):
        records.append({"path": "@" + name, "sha256": hashlib.sha256(value.encode()).hexdigest(),
                        "value": value})
    records.sort(key=lambda record: os.fsencode(record["path"]))
    digest = hashlib.sha256((ALGORITHM + "\n").encode())
    for record in records:
        digest.update(os.fsencode(record["path"]) + b"\0" + record["sha256"].encode() + b"\n")
    return {"sha256": digest.hexdigest(), "patterns": list(patterns), "files": records}


def cache_inputs(root, bootstrap_mode, environment):
    if bootstrap_mode not in ("source", "external"):
        raise CacheError("Unsupported bootstrap mode")
    tracked = tracked_files(root)
    fingerprints = {
        "toolchain": fingerprint(root, TOOLCHAIN_INPUTS, tracked),
        "config": fingerprint(root, (CONFIG,), tracked, {
            "bootstrap-mode": bootstrap_mode, "bootstrap-version": BOOTSTRAP_VERSION,
        }),
        "downloads": fingerprint(root, DOWNLOAD_INPUTS, tracked),
    }
    values = {}
    for name in ("RUNNER_OS", "RUNNER_ARCH", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT"):
        value = environment.get(name, "")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", value):
            raise CacheError(f"Missing or invalid cache identity: {name}")
        values[name] = value
    os_name, architecture = values["RUNNER_OS"], values["RUNNER_ARCH"]
    suffix = values["GITHUB_RUN_ID"] + "-" + values["GITHUB_RUN_ATTEMPT"]
    downloads_prefix = f"ax6000-dl-v2-{os_name}-"
    download_exact = downloads_prefix + fingerprints["downloads"]["sha256"] + "-"
    compiler_prefix = (f"ax6000-ccache-v2-content-{os_name}-{architecture}-mediatek-filogic-"
                       + fingerprints["toolchain"]["sha256"] + "-")
    compiler_exact = compiler_prefix + fingerprints["config"]["sha256"] + "-"
    outputs = {
        "downloads_key": download_exact + suffix,
        "downloads_restore": download_exact + "\n" + downloads_prefix,
        "compiler_key": compiler_exact + suffix,
        "compiler_restore": compiler_exact + "\n" + compiler_prefix,
    }
    return {"algorithm": ALGORITHM, "bootstrap_mode": bootstrap_mode,
            "bootstrap_version": BOOTSTRAP_VERSION, "identity": values,
            "fingerprints": fingerprints, "outputs": outputs}


def clear(path):
    # Never traverse a cache symlink when discarding an incomplete restore.
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return
    if stat.S_ISDIR(mode):
        shutil.rmtree(path)
    else:
        path.unlink()
    try:
        path.lstat()
    except FileNotFoundError:
        return
    raise CacheError(f"Could not clear cache: {path}")


def prepare_cache(path, cold, lookup_outcome, expected_key, restore_outcome, restored_key):
    if cold:
        clear(path)
        return False, "explicit-cold"
    if lookup_outcome != "success":
        clear(path)
        return False, "lookup-failed-or-unavailable"
    if not expected_key:
        # The standard action can swallow service errors. A blank success is
        # deliberately unclassified, but a fresh build may seed a new cache.
        clear(path)
        return True, "miss-or-unavailable"
    if restore_outcome == "success" and restored_key == expected_key:
        if path.is_symlink() or not path.is_dir():
            clear(path)
            return False, "restored-directory-missing-or-unsafe"
        return True, "restored-expected-key"
    clear(path)
    return False, "incomplete-restore-discarded"


def prepare(root, environment):
    cold_value = environment.get("AX6000_COLD_CACHE", "false")
    if cold_value not in ("true", "false"):
        raise CacheError("AX6000_COLD_CACHE must be true or false")
    cold = cold_value == "true"
    states = {}
    for name, directory in (("downloads", "dl"), ("compiler", ".ccache")):
        prefix = "AX6000_" + name.upper()
        observed = {field.lower(): environment.get(prefix + "_" + field, "") for field in (
            "LOOKUP_OUTCOME", "EXPECTED_KEY", "RESTORE_OUTCOME", "RESTORED_KEY")}
        save, state = prepare_cache(root / directory, cold, **observed)
        states[name] = {"state": state, "save": save, **observed}
    if cold:
        if (root / "tmp").is_symlink():
            raise CacheError("Refusing to clear Go cache through a symlinked tmp directory")
        clear(root / "tmp/go-build")
    return {"cold": cold, "caches": states,
            "fresh_workspace_required": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inputs = commands.add_parser("inputs")
    inputs.add_argument("--bootstrap-mode", choices=("source", "external"), required=True)
    inputs.add_argument("--output", type=Path, default=Path("ax6000-cache-inputs.json"))
    inputs.add_argument("--root", type=Path, default=ROOT)
    state = commands.add_parser("prepare")
    state.add_argument("--output", type=Path, default=Path("ax6000-cache-state.json"))
    state.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        if args.command == "inputs":
            data = cache_inputs(args.root.resolve(), args.bootstrap_mode, os.environ)
            outputs = data["outputs"]
        else:
            data = prepare(args.root.resolve(), os.environ)
            outputs = {name + "_save": str(record["save"]).lower()
                       for name, record in data["caches"].items()}
        args.output.write_text(json.dumps(data, indent=2) + "\n")
        actions_outputs(outputs, os.environ)
        print(json.dumps({"command": args.command, "output": str(args.output),
                          "outputs": outputs}, indent=2))
    except (OSError, CacheError, subprocess.SubprocessError) as error:
        print(f"AX6000 cache preparation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
