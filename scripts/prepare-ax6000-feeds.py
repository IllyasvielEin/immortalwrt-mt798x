#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify locked AX6000 feeds and apply reviewed Nikki source patches offline.

Every touched file must match its complete reviewed preimage or postimage.
All patches are first applied to verified Git baselines in a temporary tree;
no feed file is written until every revision, input and output is checked.
Use --check after preparation to require the complete patched state.
Feed updates and preparation must not run concurrently.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FEEDS = {"packages", "luci", "routing", "telephony", "video", "nikki"}


class PreparationError(ValueError):
    pass


def git(directory, *arguments):
    return subprocess.run(
        ["git", "-C", str(directory), *arguments],
        check=True, capture_output=True,
    ).stdout


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(directory, relative):
    parts = PurePosixPath(relative)
    if parts.is_absolute() or not parts.parts or any(p in (".", "..") for p in parts.parts):
        raise PreparationError(f"Unsafe relative path: {relative}")
    path = directory.joinpath(*parts.parts)
    current = directory
    for part in parts.parts:
        current = current / part
        if current.is_symlink():
            raise PreparationError(f"Symlink is not a reviewed input: {current}")
    if not path.resolve().is_relative_to(directory.resolve()):
        raise PreparationError(f"Path escapes its reviewed directory: {path}")
    return path


def locked_feeds(root):
    revisions = {}
    for line in (root / "feeds.conf.default").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 3 or fields[0] != "src-git":
            raise PreparationError(f"Unsupported feed definition: {line}")
        _, name, source = fields
        if name not in FEEDS or name in revisions:
            raise PreparationError(f"Unexpected or duplicate feed: {name}")
        url, separator, revision = source.rpartition("^")
        if not separator or not url.startswith("https://") or not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise PreparationError(f"Feed {name} must use a full caret-pinned Git revision")
        revisions[name] = revision
    if set(revisions) != FEEDS:
        raise PreparationError("Expected exactly six reviewed AX6000 feeds")

    directories = {}
    for name, revision in revisions.items():
        directory = root / "feeds" / name
        if directory.is_symlink() or not directory.is_dir():
            raise PreparationError(f"Missing feed directory or unexpected symlink: {directory}")
        top = Path(git(directory, "rev-parse", "--show-toplevel").decode().strip()).resolve()
        actual = git(directory, "rev-parse", "HEAD").decode().strip()
        if top != directory.resolve() or actual != revision:
            raise PreparationError(f"Feed {name} revision differs from feeds.conf.default")
        directories[name] = directory
    return revisions, directories


def prepare(root, check_only=False):
    root = Path(root).resolve()
    revisions, directories = locked_feeds(root)
    patch_directory = root / "config" / "ax6000" / "feeds"
    manifest = json.loads((patch_directory / "manifest.json").read_text())
    if manifest.get("schema_version") != 1 or manifest.get("feed") != "nikki":
        raise PreparationError("Unsupported AX6000 patch manifest")
    if revisions["nikki"] != manifest["feed_revision"]:
        raise PreparationError("Nikki revision changed; review the source patches before building")
    feed = directories["nikki"]
    plans = []
    seen = set()

    with tempfile.TemporaryDirectory(prefix="ax6000-feed-patches-") as temporary:
        staged = Path(temporary)
        for recipe in manifest["patches"]:
            patch = safe_path(patch_directory, recipe["patch"])
            if sha256(patch.read_bytes()) != recipe["sha256"]:
                raise PreparationError(f"Patch checksum mismatch: {patch.name}")
            for record in recipe["files"]:
                relative = record["path"]
                if relative in seen:
                    raise PreparationError(f"Duplicate patched file: {relative}")
                seen.add(relative)
                path = safe_path(feed, relative)
                before = path.read_bytes()
                digest = sha256(before)
                if digest not in (record["preimage_sha256"], record["postimage_sha256"]):
                    raise PreparationError(f"Unreviewed local changes: {relative}")
                if check_only and digest != record["postimage_sha256"]:
                    raise PreparationError(f"Reviewed patch is not applied: {relative}")
                baseline = git(feed, "show", f"{revisions['nikki']}:{relative}")
                if sha256(baseline) != record["preimage_sha256"]:
                    raise PreparationError(f"Git baseline checksum mismatch: {relative}")
                output = safe_path(staged, relative)
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(baseline)
                plans.append((path, before, output, record["postimage_sha256"]))
            git(staged, "apply", "--check", "--whitespace=error", str(patch))
            git(staged, "apply", "--whitespace=error", str(patch))

        actual_paths = {p.relative_to(staged).as_posix() for p in staged.rglob("*") if p.is_file()}
        if actual_paths != seen:
            raise PreparationError("Patches changed files outside their reviewed manifest")
        updates = []
        for path, before, output, expected in plans:
            after = output.read_bytes()
            if sha256(after) != expected:
                raise PreparationError(f"Patched output checksum mismatch: {path}")
            updates.append((path, before, after))
        # Check every live preimage again before writing any of them.
        for path, before, _ in updates:
            if path.is_symlink() or path.read_bytes() != before:
                raise PreparationError(f"Feed changed during preparation: {path}")
        changed = 0
        if not check_only:
            for path, before, after in updates:
                if before != after:
                    path.write_bytes(after)
                    changed += 1
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="require the complete reviewed patched state")
    args = parser.parse_args()
    try:
        changed = prepare(ROOT, check_only=args.check)
    except (OSError, PreparationError, KeyError, TypeError, json.JSONDecodeError,
            subprocess.CalledProcessError) as error:
        print(f"AX6000 feed preparation failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode(errors="replace"), end="", file=sys.stderr)
        return 1
    print(f"AX6000 feeds verified; reviewed file updates applied: {changed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
