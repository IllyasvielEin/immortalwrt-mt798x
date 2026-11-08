#!/usr/bin/env python3
"""Compile the actual kernel lifecycle helper, not a second implementation."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument("--kernel-source", type=Path, default=ROOT / ".analysis/kernel-next")
args = parser.parse_args()
header = args.kernel_source / "include/linux/mtk_cpu_lifecycle.h"
if not header.is_file():
    raise SystemExit(f"Prepared production header required: {header}")
with tempfile.TemporaryDirectory(prefix="ax6000-lifecycle-") as temporary:
    executable = Path(temporary) / "lifecycle"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
         "-I", str(header.parent), str(Path(__file__).with_name("lifecycle.c")),
         "-o", str(executable)], check=True,
    )
    subprocess.run([str(executable)], check=True)

    # Exercise the production noreturn body in two isolated processes sharing
    # only the atomic fault state; no router or real restart operation is used.
    source = (args.kernel_source / 'drivers/net/ethernet/mediatek/mtk_eth_reset.c').read_text()
    helpers = []
    for name in ('mtk_cpu_profile_note_fault', 'mtk_cpu_dma_fatal'):
        match = re.search(r'^void (?:__noreturn )?' + name + r'\(', source, re.M)
        opening = source.index('{', match.start())
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        helpers.append(source[match.start():end])
    (Path(temporary) / 'actual-fatal.inc').write_text('\n'.join(helpers))
    fatal = Path(temporary) / 'fatal'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-I', temporary, str(Path(__file__).with_name('fatal.c')),
                    '-o', str(fatal)], check=True)
    subprocess.run([str(fatal)], check=True, timeout=5)
