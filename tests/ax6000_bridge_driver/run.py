#!/usr/bin/env python3
"""Exercise production parser, authorization and token reservation with host shims."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
driver = Path(os.environ.get("MTK_AP_BRIDGE_DRIVER_TREE", root / ".analysis/driver-next"))
tests = Path(__file__).parent


def function(source, name):
    match = re.search(r"^(?:bool|int|UINT32)\s+" + re.escape(name) + r"\(", source, re.M)
    assert match is not None, name
    opening = source.index("{", match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


with tempfile.TemporaryDirectory(prefix="ax6000-bridge-driver-") as directory:
    build = Path(directory)
    source = (driver / "mt_wifi/embedded/common/ap_bridge.c").read_text()
    (build / "authorization.inc").write_text("".join(function(source, name) for name in (
        "mtk_ap_plain_ap", "mtk_ap_rx_authorize", "mtk_ap_rx_fragment_prepare")))
    source = (driver / "mt_wifi/embedded/common/token.c").read_text()
    (build / "token.inc").write_text(function(source, "token_rx_dmad_init"))
    flags = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
             "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", "-pthread", "-DCONFIG_MTK_AP_BRIDGE=1",
             "-I" + str(build), "-I" + str(driver / "mt_wifi/embedded/include")]
    env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "halt_on_error=1"}
    for name in ("wire", "authorization", "token"):
        binary = build / ("test-" + name)
        subprocess.run([*flags, str(tests / ("test_" + name + ".c")), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=env)
