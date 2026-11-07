#!/usr/bin/env python3
"""Reject the opt-in roaming diagnostics from production sources and modules."""

import argparse
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
MARKERS = (b"ROAMDIAG", b"roam_diag_", b"mt_roam_diag")


def check(patch_dir: Path, symbols: Path | None = None) -> None:
    patches = sorted(patch_dir.rglob("*.patch"))
    if not patches:
        raise ValueError(f"Missing MTK patch series: {patch_dir}")
    for patch in patches:
        name = patch.name.lower()
        if "roam" in name and "diag" in name:
            raise ValueError(f"Production contains a roaming diagnostic patch: {patch}")
        if any(marker in patch.read_bytes() for marker in MARKERS):
            raise ValueError(f"Production contains roaming diagnostic code: {patch}")
    if symbols is not None:
        if any(marker in symbols.read_bytes() for marker in MARKERS):
            raise ValueError(f"Built mt_wifi contains roaming diagnostic symbols: {symbols}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--patch-dir", type=Path,
        default=ROOT / "package/mtk/drivers/mt_wifi/patches-7673",
    )
    parser.add_argument("--symbols", type=Path)
    args = parser.parse_args()
    try:
        check(args.patch_dir, args.symbols)
    except (OSError, ValueError) as error:
        print(f"AX6000 production check failed: {error}", file=sys.stderr)
        return 1
    print("AX6000 production check passed: roaming diagnostics excluded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
