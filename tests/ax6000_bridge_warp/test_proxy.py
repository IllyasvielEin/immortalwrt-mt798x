#!/usr/bin/env python3
"""Test actual WARP proxy/gate and kernel permission functions with mock MMIO."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
WARP = Path(os.environ.get("MTK_AP_BRIDGE_WARP_TREE", ROOT / ".analysis/warp-next"))
KERNEL = Path(os.environ.get("MTK_AP_BRIDGE_KERNEL_TREE", ROOT / ".analysis/kernel-next"))


def function(source, name):
    match = re.search(r"(?m)^(?:static\s+)?(?:void|bool|int|u32)\s*\n?" +
                      re.escape(name) + r"\s*\([^;]*?\)\s*\{", source)
    if match is None:
        raise RuntimeError(f"Production function missing: {name}")
    opening = source.index("{", match.start())
    masked = re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: " " * len(m[0]), source[opening:])
    depth = 0
    for offset, char in enumerate(masked):
        depth += (char == "{") - (char == "}")
        if depth == 0:
            return source[match.start():opening + offset + 1] + "\n"
    raise RuntimeError(name)


with tempfile.TemporaryDirectory(prefix="ax6000-warp-proxy-") as directory:
    temporary = Path(directory)
    header = (WARP / "warp_checked.h").read_text()
    state = re.search(r"struct warp_checked_state \{[\s\S]*?\n\};", header)[0]
    irq = re.search(r"(?m)^#define WARP_CONTROL_IRQ[^\n]*", header)[0]
    (temporary / "proxy-state.inc").write_text(irq + "\n" + state + "\n")
    permission = (KERNEL / "drivers/net/ethernet/mediatek/mtk_eth_reset.c").read_text()
    (temporary / "proxy-permission.inc").write_text("".join(function(permission, name) for name in (
        "mtk_cpu_profile_is_fatal", "mtk_cpu_control_permitted")))
    checked = (WARP / "warp_checked.c").read_text()
    (temporary / "proxy-gate.inc").write_text("".join(function(checked, name) for name in (
        "warp_checked_enter", "warp_checked_io_enter", "warp_checked_exit")))
    main = (WARP / "warp_main.c").read_text()
    hardware = (WARP / "regs/reg_v2/warp_hw_v2.c").read_text()
    (temporary / "proxy-mmio.inc").write_text(function(hardware, "warp_mtable_build_hw") +
        "".join(function(main, name) for name in (
            "warp_cr_search", "warp_proxy_read", "warp_proxy_write")))
    executable = temporary / "proxy"
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
               "-Werror", "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I" + str(temporary), "-I" + str(KERNEL / "include/linux"), "-I" + str(WARP),
               str(Path(__file__).with_suffix(".c")), "-o", str(executable)]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"})
