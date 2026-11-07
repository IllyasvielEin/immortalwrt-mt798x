#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify locked AX6000 feeds and apply reviewed source patches offline.

Every touched file must match its complete reviewed preimage or postimage.
All patches are first applied to verified Git baselines in a temporary tree;
no feed file is written until both manifests and all outputs are checked.
Each file is replaced atomically, but this is not a cross-feed transaction.
Use --check after preparation to require the complete patched state.
Feed updates and preparation must not run concurrently.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FEEDS = {"packages", "luci", "routing", "telephony", "video", "nikki"}
MANIFESTS = (("manifest.json", "nikki"), ("golang-manifest.json", "packages"))


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
    if (parts.is_absolute() or not parts.parts or str(parts) != relative
            or any(p in (".", "..") for p in parts.parts)
            or any(c in relative for c in ("\\", "\x00", "\n", "\r"))):
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
        directory = safe_path(root, f"feeds/{name}")
        if directory.is_symlink() or not directory.is_dir():
            raise PreparationError(f"Missing feed directory or unexpected symlink: {directory}")
        top = Path(git(directory, "rev-parse", "--show-toplevel").decode().strip()).resolve()
        actual = git(directory, "rev-parse", "HEAD").decode().strip()
        if top != directory.resolve() or actual != revision:
            raise PreparationError(f"Feed {name} revision differs from feeds.conf.default")
        directories[name] = directory
    return revisions, directories


def stage_manifest(patch_directory, manifest_name, feed_name, revision, feed, staged, check_only):
    """Build one verified output tree, without modifying the live feed."""
    manifest = json.loads(safe_path(patch_directory, manifest_name).read_text())
    if manifest.get("schema_version") != 1 or manifest.get("feed") != feed_name:
        raise PreparationError(f"Unsupported AX6000 patch manifest: {manifest_name}")
    if revision != manifest["feed_revision"]:
        raise PreparationError(f"{feed_name} revision changed; review source patches before building")
    recipes = manifest["patches"]
    if not isinstance(recipes, list) or not recipes:
        raise PreparationError(f"Empty patch manifest: {manifest_name}")
    staged.mkdir()
    plans, seen = [], set()
    for recipe in recipes:
        patch = safe_path(patch_directory, recipe["patch"])
        if sha256(patch.read_bytes()) != recipe["sha256"]:
            raise PreparationError(f"Patch checksum mismatch: {patch.name}")
        if not isinstance(recipe["files"], list) or not recipe["files"]:
            raise PreparationError(f"Empty file list: {patch.name}")
        for record in recipe["files"]:
            relative = record["path"]
            key = (feed_name, relative)
            if key in seen:
                raise PreparationError(f"Duplicate patched file: {feed_name}/{relative}")
            seen.add(key)
            for digest_name in ("preimage_sha256", "postimage_sha256"):
                if not re.fullmatch(r"[0-9a-f]{64}", record[digest_name]):
                    raise PreparationError(f"Invalid file checksum: {feed_name}/{relative}")
            path = safe_path(feed, relative)
            metadata = path.stat()
            if not stat.S_ISREG(metadata.st_mode):
                raise PreparationError(f"Reviewed input is not a regular file: {path}")
            mode = stat.S_IMODE(metadata.st_mode)
            before = path.read_bytes()
            digest = sha256(before)
            if digest not in (record["preimage_sha256"], record["postimage_sha256"]):
                raise PreparationError(f"Unreviewed local changes: {feed_name}/{relative}")
            if check_only and digest != record["postimage_sha256"]:
                raise PreparationError(f"Reviewed patch is not applied: {feed_name}/{relative}")
            baseline = git(feed, "show", f"{revision}:{relative}")
            if sha256(baseline) != record["preimage_sha256"]:
                raise PreparationError(f"Git baseline checksum mismatch: {feed_name}/{relative}")
            output = safe_path(staged, relative)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(baseline)
            plans.append({"feed": feed_name, "relative": relative, "path": path,
                          "before": before, "mode": mode, "output": output,
                          "expected": record["postimage_sha256"]})
        git(staged, "apply", "--check", "--whitespace=error", str(patch))
        git(staged, "apply", "--whitespace=error", str(patch))

    actual_paths = set()
    for path in staged.rglob("*"):
        if path.is_symlink():
            raise PreparationError(f"Patch produced an unreviewed symlink: {path}")
        if path.is_file():
            actual_paths.add((feed_name, path.relative_to(staged).as_posix()))
    if actual_paths != seen:
        raise PreparationError(f"Patches changed files outside the {feed_name} manifest")
    for plan in plans:
        plan["after"] = plan["output"].read_bytes()
        if sha256(plan["after"]) != plan["expected"]:
            raise PreparationError(f"Patched output checksum mismatch: {plan['path']}")
    return plans


def recheck_inputs(root, revisions, directories, plans):
    """Recheck both HEADs and every live file before the first write."""
    current_revisions, current_directories = locked_feeds(root)
    if current_revisions != revisions or current_directories != directories:
        raise PreparationError("Feed revisions changed during preparation")
    for plan in plans:
        path = safe_path(directories[plan["feed"]], plan["relative"])
        if (path != plan["path"] or path.read_bytes() != plan["before"]
                or stat.S_IMODE(path.stat().st_mode) != plan["mode"]):
            raise PreparationError(f"Feed changed during preparation: {path}")


def replace_file(plan, feed):
    """Replace a single file atomically; no cross-file rollback is promised."""
    path = safe_path(feed, plan["relative"])
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".ax6000-feed-", dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(plan["after"])
            output.flush()
            os.fchmod(output.fileno(), plan["mode"])
            os.fsync(output.fileno())
        # Preparation and feed update may not run concurrently. Still reject
        # a changed target/path rather than silently replacing another input.
        path = safe_path(feed, plan["relative"])
        if (path.read_bytes() != plan["before"]
                or stat.S_IMODE(path.stat().st_mode) != plan["mode"]):
            raise PreparationError(f"Feed changed before replacement: {path}")
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def prepare(root, check_only=False):
    root = Path(root).resolve()
    revisions, directories = locked_feeds(root)
    patch_directory = safe_path(root, "config/ax6000/feeds")
    with tempfile.TemporaryDirectory(prefix="ax6000-feed-patches-") as temporary:
        staged = Path(temporary)
        plans, seen = [], set()
        for manifest_name, feed_name in MANIFESTS:
            additions = stage_manifest(patch_directory, manifest_name, feed_name,
                                       revisions[feed_name], directories[feed_name],
                                       staged / feed_name, check_only)
            for plan in additions:
                key = (plan["feed"], plan["relative"])
                if key in seen:
                    raise PreparationError(f"Duplicate patched file: {key}")
                seen.add(key)
            plans.extend(additions)
        recheck_inputs(root, revisions, directories, plans)
        changed = 0
        if not check_only:
            for plan in plans:
                if plan["before"] != plan["after"]:
                    replace_file(plan, directories[plan["feed"]])
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
