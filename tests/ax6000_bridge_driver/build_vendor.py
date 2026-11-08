#!/usr/bin/env python3
"""Reproduce the package's PKG_KCONFIG mapping for the prepared vendor source.

Print only unless --run is explicit: the shared kernel build must be idle first.
The saved full OpenWrt config supplies selected defaults; the current device
defconfig overlays its explicit values. This does not run OpenWrt defconfig.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--config", type=Path,
                    default=root.parents[1] / "sources/immortalwrt-mt798x/.config")
parser.add_argument("--device", type=Path, default=root / "defconfig/mt7986-ax6000.config")
parser.add_argument("--kernel", type=Path, default=root / ".analysis/kernel-compile")
parser.add_argument("--driver", type=Path, default=root / ".analysis/driver-next")
parser.add_argument("--target", default="mt_wifi.o")
parser.add_argument("--jobs", type=int, default=4)
parser.add_argument("--run", action="store_true")
args = parser.parse_args()


def configuration(path):
    values = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r"(CONFIG_\w+)=(.*)", line)
        if match:
            values[match[1]] = match[2]
        match = re.fullmatch(r"# (CONFIG_\w+) is not set", line)
        if match:
            values[match[1]] = "n"
    return values


values = configuration(args.config)
values.update(configuration(args.device))
recipe = (root / "package/mtk/drivers/mt_wifi/Makefile").read_text()
keys = recipe.split("PKG_KCONFIG:=", 1)[1].split("PKG_CONFIG_DEPENDS:=", 1)[0]
keys = re.findall(r"^\s*(\w+)\s*(?:\\)?\s*$", keys, re.M)
flags = []
for key in dict.fromkeys(keys):
    value = values.get("CONFIG_MTK_" + key)
    if value and value != "n":
        flags.append("CONFIG_" + key + "=" + value)
cross = (root / ".analysis/toolchain/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/aarch64-openwrt-linux-musl-")
host = root / ".analysis/host-tools/usr/bin"
env = {**os.environ, "PATH": str(host) + ":" + os.environ["PATH"],
       "BISON_PKGDATADIR": str(root / ".analysis/host-tools/usr/share/bison"),
       "M4": str(host / "m4"), "STAGING_DIR": str(root / ".analysis/toolchain")}
command = ["make", "-C", str(args.kernel.resolve()), "ARCH=arm64",
           "CROSS_COMPILE=" + str(cross), "M=" + str((args.driver / "mt_wifi_ap").resolve()),
           "LINUX_DIR=" + str(args.kernel.parent.resolve()),
           "CONFIG_MTK_AP_BRIDGE=y", "KCFLAGS=-I" + str(root / ".analysis/warp-next"),
           *flags, "-j" + str(args.jobs), args.target]
print(shlex.join(command), flush=True)
if args.run:
    subprocess.run(command, env=env, check=True)
