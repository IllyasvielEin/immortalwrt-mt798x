#!/usr/bin/env python3
"""Compile production WARP control/stop helpers with instrumented MMIO/OS edges."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
WARP = Path(os.environ.get('MTK_AP_BRIDGE_WARP_TREE', ROOT / '.analysis/warp-next'))
KERNEL = Path(os.environ.get('MTK_AP_BRIDGE_KERNEL_TREE', ROOT / '.analysis/kernel-next'))


def function(source, name):
    match = re.search(r'(?m)^(?:static\s+)?(?:void|bool|int|u32)\s*\n?' + name + r'\s*\([^;]*?\)\s*\{', source)
    if not match:
        raise RuntimeError(f'Production function missing: {name}')
    brace = source.index('{', match.start())
    masked = re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', lambda m: ' ' * len(m[0]), source[brace:])
    depth = 0
    for offset, char in enumerate(masked):
        depth += (char == '{') - (char == '}')
        if depth == 0:
            return source[match.start():brace + offset + 1] + '\n'
    raise RuntimeError(name)


with tempfile.TemporaryDirectory(prefix='ax6000-warp-') as directory:
    temp = Path(directory)
    include = temp / 'linux'
    include.mkdir()
    (include / 'types.h').write_text('#include <stdint.h>\ntypedef uint64_t u64; typedef uint32_t u32;\n')
    state = (WARP / 'warp_checked.h').read_text()
    state = re.search(r'struct warp_checked_state \{[\s\S]*?\n\};', state)[0]
    enums = (WARP / 'mcu/warp_woif.h').read_text()
    enums = '\n'.join(re.search(r'enum ' + name + r' \{[\s\S]*?\n\};', enums)[0] for name in ('wo_cmd_id', 'wo_state'))
    api = (KERNEL / 'include/linux/mtk_warp_ser.h').read_text()
    (temp / 'actual-state.inc').write_text(api + '\n' + enums + '\n' + state + '\n' + re.search(r'^#define WARP_CONTROL_IRQ[^\n]*', (WARP / 'warp_checked.h').read_text(), re.M)[0] + '\n')
    production = re.sub(r'^#include[^\n]*\n', '', (WARP / 'warp_checked.c').read_text(), flags=re.M)
    hardware = (WARP / 'regs/reg_v2/warp_hw_v2.c').read_text()
    names = ('checked_read', 'checked_write', 'warp_checked_control_irq', 'checked_poll', 'checked_disable',
             'warp_checked_hw_stop', 'checked_pulse', 'warp_checked_hw_reset',
             'checked_enable', 'warp_checked_hw_enable')
    production += '\n'.join(function(hardware, name) for name in names)
    production += function((WARP / 'mcu/warp_woctrl.c').read_text(), 'warp_checked_wo')
    main = (WARP / 'warp_main.c').read_text()
    production += function(main, 'warp_checked_prepare') + function(main, 'warp_checked_resume')
    (temp / 'actual-control.inc').write_text(production)
    command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
               '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               '-pthread', '-I' + str(temp), '-I' + str(WARP),
               str(Path(__file__).with_name('test.c')), '-o', str(temp / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(temp / 'test')], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'UBSAN_OPTIONS': 'halt_on_error=1'})

subprocess.run([sys.executable, str(Path(__file__).with_name('test_proxy.py'))], check=True)

# Whole production message queue and deterministic timeout/response lifetimes.
subprocess.run([__import__("sys").executable, str(Path(__file__).with_name("test_msg.py"))], check=True)
