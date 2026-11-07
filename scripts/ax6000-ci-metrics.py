#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Collect optional, bounded per-build observations without changing results."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def warning(message):
    try:
        print(f"Warning: AX6000 observations unavailable: {message}", file=sys.stderr)
    except (OSError, ValueError):
        pass


def run(args, root, environment):
    try:
        result = subprocess.run(args, cwd=root, env=dict(environment), capture_output=True,
                                text=True, errors="replace", timeout=60)
        return {"exit_code": result.returncode, "stdout": result.stdout.strip()[:32768],
                "stderr": result.stderr.strip()[:32768]}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"error": str(error)}


def identity(command, root, environment):
    resolved = shutil.which(command, path=environment.get("PATH")) if command else None
    if not resolved:
        return {"command": command, "status": "not-built-or-not-found"}
    try:
        executable = Path(resolved).resolve()
        digest = hashlib.sha256()
        with executable.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        return {"command": command, "resolved": str(executable), "sha256": digest.hexdigest(),
                "version": run([str(executable), "--version"], root, environment)}
    except OSError as error:
        return {"command": command, "error": str(error)}


def read_json(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError) as error:
        return {"status": "unavailable", "error": str(error)}


def snapshot(root, environment):
    try:
        memory = [line for line in Path("/proc/meminfo").read_text().splitlines()
                  if line.startswith(("MemTotal:", "MemFree:", "MemAvailable:"))]
    except OSError:
        memory = None
    return {"image_os": environment.get("ImageOS"),
            "image_version": environment.get("ImageVersion"), "cpu_count": os.cpu_count(),
            "memory": memory, "disk": run(["df", "-h", "."], root, environment),
            "peak_memory_bytes": None}


def collect(phase, destination, root, environment):
    destination = destination.resolve()
    data = {"phase": phase, "recorded_at_unix": int(time.time()),
            "ccache_compiler_check": environment.get("CCACHE_COMPILERCHECK"),
            "ccache_max_size": environment.get("CCACHE_MAXSIZE"),
            "runner": snapshot(root, environment),
            "cache_transfer_seconds": {"restore_downloads": None, "restore_compiler": None,
                                       "save_downloads": None, "save_compiler": None},
            "cache_transfer_bytes": {"downloads": None, "compiler": None},
            "cache_restore": {name: environment.get(name) for name in (
                "AX6000_COLD_CACHE", "AX6000_DOWNLOADS_MATCHED_KEY", "AX6000_COMPILER_MATCHED_KEY",
                "AX6000_DOWNLOADS_CACHE_OUTCOME", "AX6000_COMPILER_CACHE_OUTCOME")}}
    stats = destination / "ccache-stats.log"
    if phase == "prepare":
        # Publish only a verified writable file. A failed setup cannot leave an
        # invalid CCACHE_STATSLOG value for the subsequent real build.
        destination.mkdir(parents=True, exist_ok=True)
        with stats.open("w") as output:
            output.write("")
            output.flush()
        if environment.get("GITHUB_ENV"):
            with Path(environment["GITHUB_ENV"]).open("a") as output:
                output.write(f"CCACHE_STATSLOG={stats}\n")
                output.flush()
        data["statslog"] = str(stats)
    else:
        ccache = root / "staging_dir/host/bin/ccache"
        if ccache.is_file():
            data["ccache"] = {
                "version": run([str(ccache), "--version"], root, environment),
                "configuration": run([str(ccache), "--show-config"], root, environment),
                "statistics": (run([str(ccache), "--show-log-stats", "--verbose"], root,
                                   {**environment, "CCACHE_STATSLOG": str(stats)})
                               if stats.is_file() else {"status": "per-build-statslog-unavailable"}),
            }
        else:
            data["ccache"] = {"status": "build-did-not-reach-ccache"}
        resolution = run(["make", "-s", "--no-print-directory", "-f",
                          "scripts/ax6000-ci-toolchain.mk", "ax6000-ci-toolchain"], root, environment)
        data["toolchain_resolution"] = resolution
        data["tools"] = {}
        if resolution.get("exit_code") == 0:
            for line in resolution["stdout"].splitlines():
                name, separator, command = line.partition("=")
                if separator and name in ("host_cc", "target_cc", "bpf_clang", "bpf_llvm_strip"):
                    data["tools"][name] = identity(command, root, environment)
        data["cache_state"] = read_json(root / "ax6000-cache-state.json")
        inputs = read_json(root / "ax6000-cache-inputs.json")
        data["cache_inputs"] = {"algorithm": inputs.get("algorithm"),
                                "bootstrap_mode": inputs.get("bootstrap_mode"),
                                "fingerprints": {name: value.get("sha256") for name, value in
                                                 inputs.get("fingerprints", {}).items()}}
        destination.mkdir(parents=True, exist_ok=True)
    (destination / f"{phase}.json").write_text(json.dumps(data, indent=2) + "\n")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("prepare", "report"))
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        data = collect(args.phase, args.directory, Path.cwd(), os.environ)
        print(json.dumps(data, indent=2))
    except Exception as error:
        # These observations are optional. Real make/guard failures are handled
        # by their own steps; do not turn missing metrics into a build failure.
        warning(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
